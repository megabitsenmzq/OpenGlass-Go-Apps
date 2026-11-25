#include "ui.h"

#include <ft2build.h>
#include FT_SYNTHESIS_H
#include <stdio.h>
#include <string.h>

int font_open(Font *font, const char *path, int pixel_size)
{
    memset(font, 0, sizeof(*font));
    if (FT_Init_FreeType(&font->library) != 0)
        return -1;
    if (FT_New_Face(font->library, path, 0, &font->face) != 0) {
        FT_Done_FreeType(font->library);
        return -1;
    }
    if (FT_Set_Pixel_Sizes(font->face, 0, pixel_size) != 0) {
        FT_Done_Face(font->face);
        FT_Done_FreeType(font->library);
        return -1;
    }
    snprintf(font->path, sizeof(font->path), "%s", path);
    return 0;
}

void font_close(Font *font)
{
    if (font->face)
        FT_Done_Face(font->face);
    if (font->library)
        FT_Done_FreeType(font->library);
}

int font_centered_baseline(const Font *font, int box_y, int box_height)
{
    int pixel_size = font && font->face && font->face->size ?
                     (int)font->face->size->metrics.y_ppem : 64;
    return box_y + box_height / 2 + (pixel_size * 3 + 4) / 8;
}

static unsigned int font_next_character(const Font *font, const char **text)
{
    const unsigned char *input = (const unsigned char *)*text;
    unsigned int value;
    unsigned int minimum;
    int length;
    int i;
    if (!font->utf8 || input[0] < 0x80) {
        *text += 1;
        return input[0];
    }
    if (input[0] >= 0xc2 && input[0] <= 0xdf) {
        value = input[0] & 0x1f;
        length = 2;
        minimum = 0x80;
    } else if (input[0] >= 0xe0 && input[0] <= 0xef) {
        value = input[0] & 0x0f;
        length = 3;
        minimum = 0x800;
    } else if (input[0] >= 0xf0 && input[0] <= 0xf4) {
        value = input[0] & 0x07;
        length = 4;
        minimum = 0x10000;
    } else {
        *text += 1;
        return 0xfffd;
    }
    for (i = 1; i < length; ++i) {
        if ((input[i] & 0xc0) != 0x80) {
            *text += 1;
            return 0xfffd;
        }
        value = (value << 6) | (input[i] & 0x3f);
    }
    if (value < minimum || (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) {
        *text += 1;
        return 0xfffd;
    }
    *text += length;
    return value;
}

static int font_glyph_metrics(Font *font, unsigned int character,
                              FT_UInt *index, int *advance, int *loaded)
{
    FontGlyphMetrics *cached = character < 128 ? &font->ascii[character] : NULL;
    unsigned int size = font->face->size->metrics.y_ppem;
    if (size != font->glyph_size) {
        memset(font->ascii, 0, sizeof(font->ascii));
        font->glyph_size = size;
    }
    if (cached && cached->valid) {
        *index = cached->index;
        *advance = cached->advance;
        if (loaded)
            *loaded = 0;
        return 0;
    }
    *index = FT_Get_Char_Index(font->face, character);
    if (FT_Load_Glyph(font->face, *index, FT_LOAD_DEFAULT) != 0)
        return -1;
    *advance = font->face->glyph->advance.x >> 6;
    if (cached) {
        cached->index = *index;
        cached->advance = *advance;
        cached->valid = 1;
    }
    if (loaded)
        *loaded = 1;
    return 0;
}

int font_text_width(Font *font, const char *text)
{
    int width = 0;
    while (*text) {
        FT_UInt index;
        int advance;
        unsigned int character = font_next_character(font, &text);
        if (font_glyph_metrics(font, character, &index, &advance, NULL) == 0)
            width += advance;
    }
    return width;
}

void font_text_widths(Font *font, const char *text, int *prefix)
{
    int width = 0;
    *prefix = 0;
    while (*text) {
        const char *previous = text;
        FT_UInt index;
        int advance = 0;
        unsigned int character = font_next_character(font, &text);
        (void)font_glyph_metrics(font, character, &index, &advance, NULL);
        while (previous < text) {
            ++previous;
            prefix[1] = previous == text ? width + advance : width;
            ++prefix;
        }
        width += advance;
    }
}

void draw_rect(Display *display, Buffer *buffer, int x, int y,
               int width, int height, uint32_t color)
{
    int row;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + width > (int)display->mode.hdisplay ? display->mode.hdisplay : x + width;
    int y1 = y + height > (int)display->mode.vdisplay ? display->mode.vdisplay : y + height;
    for (row = y0; row < y1; ++row) {
        uint32_t *pixels = (uint32_t *)(buffer->pixels + (size_t)row * buffer->stride);
        int column;
        for (column = x0; column < x1; ++column)
            pixels[column] = color;
    }
}

static void blend_glyph(Display *display, Buffer *buffer, FT_Bitmap *bitmap,
                        int left, int top, uint32_t color)
{
    unsigned int row;
    for (row = 0; row < bitmap->rows; ++row) {
        unsigned int column;
        int y = top + (int)row;
        if (y < 0 || y >= (int)display->mode.vdisplay)
            continue;
        for (column = 0; column < bitmap->width; ++column) {
            int x = left + (int)column;
            unsigned int alpha;
            uint32_t *pixel;
            uint32_t old;
            unsigned int r;
            unsigned int g;
            unsigned int b;
            if (x < 0 || x >= (int)display->mode.hdisplay)
                continue;
            alpha = bitmap->buffer[row * bitmap->pitch + column];
            if (!alpha)
                continue;
            pixel = (uint32_t *)(buffer->pixels + (size_t)y * buffer->stride) + x;
            old = *pixel;
            if ((color & 0xff000000u) || (old & 0xff000000u)) {
                unsigned int source_alpha = ((color >> 24) & 255) * alpha / 255;
                unsigned int old_alpha = old >> 24;
                unsigned int alpha_sum = source_alpha * 255 + old_alpha * (255 - source_alpha);
                unsigned int output_alpha = (alpha_sum + 127) / 255;
                unsigned int remaining = old_alpha * (255 - source_alpha);
                r = (((color >> 16) & 255) * source_alpha * 255 +
                     ((old >> 16) & 255) * remaining + alpha_sum / 2) / alpha_sum;
                g = (((color >> 8) & 255) * source_alpha * 255 +
                     ((old >> 8) & 255) * remaining + alpha_sum / 2) / alpha_sum;
                b = (((color & 255) * source_alpha * 255 +
                     (old & 255) * remaining + alpha_sum / 2) / alpha_sum);
                *pixel = (output_alpha << 24) | (r << 16) | (g << 8) | b;
            } else {
                r = (((color >> 16) & 255) * alpha + ((old >> 16) & 255) * (255 - alpha)) / 255;
                g = (((color >> 8) & 255) * alpha + ((old >> 8) & 255) * (255 - alpha)) / 255;
                b = (((color & 255) * alpha + (old & 255) * (255 - alpha)) / 255);
                *pixel = (r << 16) | (g << 8) | b;
            }
        }
    }
}

static void draw_text_internal(Display *display, Buffer *buffer, Font *font,
                               const char *text, int x, int baseline,
                               uint32_t color, int bold)
{
    FT_Face face = font->face;
    if (buffer->layered)
        color |= 0xff000000u;
    while (*text) {
        FT_UInt glyph;
        int advance;
        int loaded = 0;
        unsigned int character = font_next_character(font, &text);
        if (font_glyph_metrics(font, character, &glyph, &advance, &loaded) != 0 ||
            (!loaded && FT_Load_Glyph(face, glyph, FT_LOAD_DEFAULT) != 0))
            continue;
        if (bold)
            FT_GlyphSlot_Embolden(face->glyph);
        if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0)
            continue;
        blend_glyph(display, buffer, &face->glyph->bitmap,
                    x + face->glyph->bitmap_left,
                    baseline - face->glyph->bitmap_top, color);
        x += advance;
    }
}

void draw_text(Display *display, Buffer *buffer, Font *font,
               const char *text, int x, int baseline, uint32_t color)
{
    draw_text_internal(display, buffer, font, text, x, baseline, color, 0);
}

void draw_text_bold(Display *display, Buffer *buffer, Font *font,
                    const char *text, int x, int baseline, uint32_t color)
{
    draw_text_internal(display, buffer, font, text, x, baseline, color, 1);
}

void draw_back_arrow(Display *display, Buffer *buffer, int x, int center_y,
                     uint32_t color)
{
    int i;
    draw_rect(display, buffer, x, center_y - 3, 64, 6, color);
    for (i = 0; i < 28; ++i) {
        draw_rect(display, buffer, x + i, center_y - i - 3, 6, 6, color);
        draw_rect(display, buffer, x + i, center_y + i - 3, 6, 6, color);
    }
}
