#include "gallery_view.h"
#include "depth_frame.h"

#include <stdlib.h>
#include <string.h>

int gallery_default_cursor(size_t item_count)
{
    (void)item_count;
    return 0;
}

static int text_width(Font *font, const char *text)
{
    return font_text_width(font, text);
}

void gallery_view_draw_text_end(Display *display, Buffer *buffer, Font *font,
                                const char *text, int x, int baseline, int max_width,
                                uint32_t color)
{
    size_t length = strlen(text);
    size_t copy;
    int *widths = malloc((length + 1) * sizeof(*widths));
    int ellipsis_width;
    char shortened[512];
    if (!widths)
        return;
    font_text_widths(font, text, widths);
    if (widths[length] <= max_width) {
        free(widths);
        draw_text(display, buffer, font, text, x, baseline, color);
        return;
    }
    ellipsis_width = text_width(font, "...");
    copy = length < sizeof(shortened) - 4 ? length : sizeof(shortened) - 4;
    while (copy > 0 && widths[copy] + ellipsis_width > max_width)
        --copy;
    if (widths[copy] + ellipsis_width <= max_width) {
        memcpy(shortened, text, copy);
        memcpy(shortened + copy, "...", 4);
        draw_text(display, buffer, font, shortened, x, baseline, color);
    }
    free(widths);
}

void gallery_view_draw_text_start(Display *display, Buffer *buffer, Font *font,
                                  const char *text, int x, int baseline, int max_width,
                                  uint32_t color)
{
    size_t length = strlen(text);
    size_t offset = 0;
    int *widths = malloc((length + 1) * sizeof(*widths));
    int ellipsis_width;
    char shortened[512];
    if (!widths)
        return;
    font_text_widths(font, text, widths);
    if (widths[length] <= max_width) {
        free(widths);
        draw_text(display, buffer, font, text, x, baseline, color);
        return;
    }
    ellipsis_width = text_width(font, "...");
    while (offset < length) {
        if (length - offset + 4 <= sizeof(shortened) &&
            ellipsis_width + widths[length] - widths[offset] <= max_width) {
            snprintf(shortened, sizeof(shortened), "...%s", text + offset);
            draw_text(display, buffer, font, shortened, x, baseline, color);
            break;
        }
        ++offset;
    }
    free(widths);
}

static void draw_arrow(Display *display, Buffer *buffer, int x, int y,
                       uint32_t color, int bold)
{
    int pad = bold ? 1 : 0;
    int thick = 10 + pad * 2;
    int i;
    draw_rect(display, buffer, x - pad, y - 5 - pad, 120 + pad * 2, thick, color);
    for (i = 0; i < 50; ++i) {
        draw_rect(display, buffer, x + i - pad, y - i - 5 - pad, thick, thick, color);
        draw_rect(display, buffer, x + i - pad, y + i - 5 - pad, thick, thick, color);
    }
}

static void draw_play(Display *display, Buffer *buffer, int x, int y,
                      uint32_t color, int bold)
{
    int left = bold ? x - 1 : x;
    int width = bold ? 99 : 96;
    int base_height = bold ? 114 : 110;
    int column;
    for (column = 0; column < width; ++column) {
        int height = base_height - column * base_height / width;
        draw_rect(display, buffer, left + column, y - height / 2, 1, height, color);
    }
}

static void draw_border(Display *display, Buffer *buffer, int x, int y, int width,
                        uint32_t color)
{
    draw_rect(display, buffer, x - 8, y - 8, width + 16, 8, color);
    draw_rect(display, buffer, x - 8, y + THUMB_HEIGHT, width + 16, 8, color);
    draw_rect(display, buffer, x - 8, y, 8, THUMB_HEIGHT, color);
    draw_rect(display, buffer, x + width, y, 8, THUMB_HEIGHT, color);
}

static void draw_selection(Display *display, Buffer *buffer, int x, int y, int width)
{
    draw_border(display, buffer, x, y, width, 0xffffff);
}

static void draw_thumbnail_rows(Display *display, Buffer *buffer, const GalleryItem *item,
                                int x, int y, int first_row, int row_count)
{
    int row;
    (void)display;
    if (!item->thumbnail)
        return;
    for (row = first_row; row < first_row + row_count; ++row) {
        const uint8_t *source = item->thumbnail + (size_t)row * THUMB_WIDTH * 4;
        uint32_t *target = (uint32_t *)(buffer->pixels + (size_t)(y + row) * buffer->stride) + x;
        memcpy(target, source, THUMB_WIDTH * 4);
    }
}

static void draw_thumbnail(Display *display, Buffer *buffer, const GalleryItem *item, int x, int y)
{
    draw_thumbnail_rows(display, buffer, item, x, y, 0, THUMB_HEIGHT);
}

static void draw_folder(Display *display, Buffer *buffer, const GalleryItem *item, int x, int y)
{
    const int icon_width = 255;
    const int icon_height = 204;
    const int icon_x = x + (THUMB_WIDTH - icon_width) / 2;
    const int icon_y = y + (THUMB_HEIGHT - icon_height) / 2;
    int row;
    if (item->thumbnail)
        draw_thumbnail(display, buffer, item, x, y);
    else
        draw_rect(display, buffer, x, y, THUMB_WIDTH, THUMB_HEIGHT, 0x202028);
    for (row = 0; row < icon_height; ++row) {
        int source_y = row * 2 / 3;
        int width = source_y < 20 ? (80 + source_y) * icon_width / 212 : icon_width;
        draw_rect(display, buffer, icon_x, icon_y + row, width, 1, 0xffffff);
    }
}

static void draw_warning(Display *display, Buffer *buffer,
                         int center_x, int center_y, int scale)
{
    int row;
    for (row = 0; row <= 162; ++row) {
        int half_width = row < 144 ? 11 + 88 * row / 144 :
                                     99 - 11 * (row - 144) / 18;
        draw_rect(display, buffer, center_x - half_width * scale,
                  center_y + (-82 + row) * scale,
                  (half_width * 2 + 1) * scale, scale, 0xffcc00);
    }
    draw_rect(display, buffer, center_x - 12 * scale, center_y - 50 * scale,
              24 * scale, 70 * scale, 0x101014);
    draw_rect(display, buffer, center_x - 12 * scale, center_y + 38 * scale,
              24 * scale, 24 * scale, 0x101014);
}

static void draw_progress(Display *display, Buffer *buffer, const GalleryItem *item, int x, int y)
{
    int bar_width = THUMB_WIDTH - 40;
    int width = (int)(bar_width * item->progress);
    if (item->thumbnail_state < 0) {
        draw_warning(display, buffer, x + THUMB_WIDTH / 2,
                     y + THUMB_HEIGHT / 2, 1);
        return;
    }
    draw_rect(display, buffer, x + 20, y + THUMB_HEIGHT - 70, bar_width, 12, 0x404048);
    if (width > 0)
        draw_rect(display, buffer, x + 20, y + THUMB_HEIGHT - 70, width, 12, 0xe8e8e8);
}

static void draw_title(Display *display, Buffer *buffer, Font *font,
                       const GalleryItem *item, int x, int y)
{
    FT_Set_Pixel_Sizes(font->face, 0, 43);
    draw_rect(display, buffer, x, y + THUMB_HEIGHT - 56, THUMB_WIDTH, 56, 0x101014);
    gallery_view_draw_text_end(display, buffer, font, item->name, x + 12,
                               y + THUMB_HEIGHT - 18, THUMB_WIDTH - 24, 0xffffff);
    FT_Set_Pixel_Sizes(font->face, 0, 64);
}

static void clear_area(DepthLayer *layers, int x, int y, int width, int height)
{
    int layer;
    int row;
    for (layer = 0; layer < 2; ++layer) {
        for (row = y; row < y + height; ++row) {
            memset(layers[layer].buffer.pixels +
                   (size_t)row * layers[layer].buffer.stride + (size_t)x * 4,
                   0, (size_t)width * 4);
        }
    }
}

static void gallery_cell_origin(const Display *display, int cell, int *x, int *y)
{
    const int cell_width = display->mode.hdisplay / 3;
    const int grid_y = 4;
    const int cell_height = (display->mode.vdisplay - grid_y - 50) / 3;
    *x = (cell % 3) * cell_width + (cell_width - THUMB_WIDTH) / 2;
    *y = grid_y + (cell / 3) * cell_height + (cell_height - THUMB_HEIGHT) / 2;
}

static void draw_cell_content(Display *display, Font *font, Buffer *buffer,
                              int selected, GalleryItem *item, int x, int y)
{
    if (item) {
        if (item->is_directory) {
            draw_folder(display, buffer, item, x, y);
            if (item->cover_path && !item->thumbnail)
                draw_progress(display, buffer, item, x, y);
        } else if (item->thumbnail) {
            draw_thumbnail(display, buffer, item, x, y);
        } else {
            draw_rect(display, buffer, x, y, THUMB_WIDTH, THUMB_HEIGHT, 0x202028);
            draw_progress(display, buffer, item, x, y);
        }
    }
    if (selected && item)
        draw_title(display, buffer, font, item, x, y);
    if (selected)
        draw_selection(display, buffer, x, y, THUMB_WIDTH);
}

static void draw_special_part(Display *display, Buffer *buffer, int x, int y,
                              int part, uint32_t color, int bold)
{
    if (part == 0)
        draw_arrow(display, buffer, x + (THUMB_WIDTH / 2 - 120) / 2,
                   y + THUMB_HEIGHT / 2, color, bold);
    else
        draw_play(display, buffer,
                  x + THUMB_WIDTH / 2 + (THUMB_WIDTH / 2 - 96) / 2 + 8,
                  y + THUMB_HEIGHT / 2, color, bold);
}

void gallery_view_draw_cell(Display *display, Font *font, DepthLayer layers[2],
                            int cell, int selected, GalleryItem *item,
                            int special, int cursor)
{
    int x;
    int y;
    gallery_cell_origin(display, cell, &x, &y);
    if (special) {
        int part;
        clear_area(layers, x - 8, y - 8, THUMB_WIDTH + 16, THUMB_HEIGHT + 16);
        draw_rect(display, &layers[1].buffer, x - 8, y - 8,
                  THUMB_WIDTH + 16, THUMB_HEIGHT + 16, 0x101014);
        for (part = 0; part < 2; ++part) {
            int part_x = x + part * THUMB_WIDTH / 2;
            int active = cursor == part;
            Buffer *buffer = active ? &layers[0].buffer : &layers[1].buffer;
            uint32_t icon_color = active ? 0x00000000u : 0xe8e8e8u;
            if (active)
                draw_rect(display, buffer, part_x - 8, y - 8,
                          THUMB_WIDTH / 2 + 16, THUMB_HEIGHT + 16, 0xffe8e8e8u);
            draw_special_part(display, buffer, x, y, part, icon_color, active);
        }
        return;
    }
    {
        Buffer *buffer = selected ? &layers[0].buffer : &layers[1].buffer;
        clear_area(layers, x - 8, y - 8, THUMB_WIDTH + 16, THUMB_HEIGHT + 16);
        draw_rect(display, buffer, x - 8, y - 8, THUMB_WIDTH + 16,
                  THUMB_HEIGHT + 16, 0x101014);
        draw_cell_content(display, font, buffer, selected, item, x, y);
    }
}

void gallery_view_draw_cell_anim(Display *display, Font *font, DepthLayer layers[2],
                                 int cell, int selected, GalleryItem *item)
{
    int x;
    int y;
    gallery_cell_origin(display, cell, &x, &y);
    clear_area(layers, x - 8, y - 8, THUMB_WIDTH + 16, THUMB_HEIGHT + 16);
    draw_rect(display, &layers[1].buffer, x - 8, y - 8,
              THUMB_WIDTH + 16, THUMB_HEIGHT + 16, 0x101014);
    draw_cell_content(display, font, &layers[0].buffer, selected, item, x, y);
}

void gallery_view_draw_special_anim(Display *display, DepthLayer layers[2],
                                    int active_slot, int sinking_slot)
{
    int x;
    int y;
    int part;
    gallery_cell_origin(display, 0, &x, &y);
    clear_area(layers, x - 8, y - 8, THUMB_WIDTH + 16, THUMB_HEIGHT + 16);
    draw_rect(display, &layers[1].buffer, x - 8, y - 8,
              THUMB_WIDTH + 16, THUMB_HEIGHT + 16, 0x101014);
    for (part = 0; part < 2; ++part) {
        if (part == active_slot) {
            int part_x = x + part * THUMB_WIDTH / 2;
            draw_rect(display, &layers[0].buffer, part_x - 8, y - 8,
                      THUMB_WIDTH / 2 + 16, THUMB_HEIGHT + 16, 0xffe8e8e8u);
            draw_special_part(display, &layers[0].buffer, x, y, part, 0x00000000u, 1);
        } else if (part == sinking_slot) {
            draw_special_part(display, &layers[0].buffer, x, y, part, 0xe8e8e8u, 0);
        } else {
            draw_special_part(display, &layers[1].buffer, x, y, part, 0xe8e8e8u, 0);
        }
    }
}

void gallery_view_move_cell(Display *display, Font *font, DepthLayer layers[2],
                            int cell, int selected, GalleryItem *item)
{
    const int cell_width = display->mode.hdisplay / 3;
    const int grid_y = 4;
    const int cell_height = (display->mode.vdisplay - grid_y - 50) / 3;
    const int x = (cell % 3) * cell_width + (cell_width - THUMB_WIDTH) / 2;
    const int y = grid_y + (cell / 3) * cell_height + (cell_height - THUMB_HEIGHT) / 2;
    Buffer *source = &layers[selected ? 1 : 0].buffer;
    Buffer *target = &layers[selected ? 0 : 1].buffer;
    int row;
    for (row = y - 8; row < y + THUMB_HEIGHT + 8; ++row) {
        size_t offset = (size_t)row * source->stride + (size_t)(x - 8) * 4;
        memcpy(target->pixels + offset, source->pixels + offset, (THUMB_WIDTH + 16) * 4);
        memset(source->pixels + offset, 0, (THUMB_WIDTH + 16) * 4);
    }
    if (selected) {
        if (item)
            draw_title(display, target, font, item, x, y);
        draw_selection(display, target, x, y, THUMB_WIDTH);
    } else {
        draw_border(display, target, x, y, THUMB_WIDTH, 0x101014);
        if (item && item->thumbnail)
            draw_thumbnail_rows(display, target, item, x, y, THUMB_HEIGHT - 56, 56);
    }
}

int gallery_view_show_error(Display *display, DepthEffect *depth, DepthLayer layers[2])
{
    DepthFrame frame;
    float disparity0;
    float disparity1;
    int result = -1;

    if (!layers || !layers[0].buffer.pixels || !layers[1].buffer.pixels)
        return -1;
    disparity0 = layers[0].disparity;
    disparity1 = layers[1].disparity;
    layers[0].disparity = 0.0f;
    layers[1].disparity = 0.0f;
    draw_rect(display, &layers[0].buffer, 0, 0,
              display->mode.hdisplay, display->mode.vdisplay, 0x101014);
    draw_rect(display, &layers[1].buffer, 0, 0,
              display->mode.hdisplay, display->mode.vdisplay, 0x101014);
    draw_warning(display, &layers[0].buffer,
                 display->mode.hdisplay / 2, display->mode.vdisplay / 2, 4);
    draw_warning(display, &layers[1].buffer,
                 display->mode.hdisplay / 2, display->mode.vdisplay / 2, 4);
    if (depth_frame_begin(&frame, display, depth, layers, NULL, 0, 1) == 0) {
        (void)depth_frame_update_rect(&frame, 0, 0,
                                      display->mode.hdisplay, display->mode.vdisplay, 0, 0);
        result = depth_frame_present(&frame);
    }
    layers[0].disparity = disparity0;
    layers[1].disparity = disparity1;
    return result;
}
