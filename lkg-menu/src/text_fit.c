#include "ui.h"

#include <string.h>

void draw_text_fit(Display *display, Buffer *buffer, Font *font,
                   const char *text, int x, int baseline, uint32_t color,
                   int max_width, int bold)
{
    char shortened[256];
    int widths[sizeof(shortened)];
    size_t length = strlen(text);
    int ellipsis_width;
    void (*draw_fit_text)(Display *, Buffer *, Font *, const char *, int, int, uint32_t) =
        bold ? draw_text_bold : draw_text;
    if (font_text_width(font, text) <= max_width) {
        draw_fit_text(display, buffer, font, text, x, baseline, color);
        return;
    }
    if (length > sizeof(shortened) - 4)
        length = sizeof(shortened) - 4;
    if (font->utf8) {
        while (length > 0 && ((unsigned char)text[length] & 0xc0) == 0x80)
            --length;
    }
    memcpy(shortened, text, length);
    shortened[length] = '\0';
    font_text_widths(font, shortened, widths);
    ellipsis_width = font_text_width(font, "...");
    while (length > 0) {
        if (font->utf8) {
            while (length > 0 && ((unsigned char)text[length] & 0xc0) == 0x80)
                --length;
        }
        if (widths[length] + ellipsis_width <= max_width) {
            memcpy(shortened + length, "...", 4);
            break;
        }
        --length;
    }
    if (!length)
        memcpy(shortened, "...", 4);
    draw_fit_text(display, buffer, font, shortened, x, baseline, color);
}
