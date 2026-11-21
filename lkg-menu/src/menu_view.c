#include "menu_view.h"

#include <string.h>

static void draw_menu_item(Display *display, Font *font, const DepthEffect *depth,
                           DepthLayer layers[2], int page, const char *item,
                           const SystemInfo *info, int index, int active, int y)
{
    const int row_height = display->mode.vdisplay / 17;
    const int margin = display->mode.hdisplay / 12;
    const int ascender = font->face->size->metrics.ascender >> 6;
    const int descender = font->face->size->metrics.descender >> 6;
    const int baseline = y + (row_height - (ascender - descender)) / 2 + ascender;
    uint32_t color = (active ? 0x101014 : 0xd8d8d8) | 0xff000000u;
    Buffer *item_buffer = active ? &layers[0].buffer : &layers[1].buffer;
    void (*draw_item_text)(Display *, Buffer *, Font *, const char *, int, int,
                           uint32_t) = active && depth->stereo ? draw_text_bold : draw_text;
    FT_Set_Pixel_Sizes(font->face, 0, 64);
    if (page != PAGE_MAIN && index == 0)
        draw_back_arrow(display, item_buffer, margin / 3, y + row_height / 2, color);
    if (page == PAGE_INFO && index > 0) {
        const char *value = index == 1 ? info->serial :
                            index == 2 ? info->cpu :
                            index == 3 ? info->memory :
                            index == 4 ? info->gpu :
                            index == 5 ? info->display :
                            index == 6 ? info->disk :
                            index == 7 ? info->os :
                            index == 8 ? info->kernel :
                            index == 9 ? info->ssh : info->ip;
        FT_Set_Pixel_Sizes(font->face, 0, 32);
        draw_item_text(display, item_buffer, font, item, margin + 24,
                       y + row_height * 38 / 100, color);
        FT_Set_Pixel_Sizes(font->face, 0, 48);
        draw_item_text(display, item_buffer, font, value, margin + 24,
                       y + row_height * 76 / 100, color);
        FT_Set_Pixel_Sizes(font->face, 0, 64);
    } else {
        draw_item_text(display, item_buffer, font, item, margin + 24, baseline, color);
    }
}

void menu_view_draw_page(Display *display, Font *font, const DepthEffect *depth,
                         DepthLayer layers[2], int page, int selected,
                         const SystemInfo *info)
{
    const char **items;
    int count;
    int row_height = display->mode.vdisplay / 17;
    int start_y;
    int i;
    menu_items(page, &items, &count);
    start_y = ((int)display->mode.vdisplay - row_height * count) / 2;
    memset(layers[0].buffer.pixels, 0, layers[0].buffer.size);
    memset(layers[1].buffer.pixels, 0, layers[1].buffer.size);
    for (i = 0; i < count; ++i)
        draw_menu_item(display, font, depth, layers, page, items[i], info,
                       i, i == selected, start_y + i * row_height);
}

void menu_view_draw_row(Display *display, Font *font, const DepthEffect *depth,
                        DepthLayer layers[2], int page, int row, int selected,
                        const SystemInfo *info)
{
    const char **items;
    int count;
    int row_height = display->mode.vdisplay / 17;
    int start_y;
    int y;
    menu_items(page, &items, &count);
    start_y = ((int)display->mode.vdisplay - row_height * count) / 2;
    y = start_y + row * row_height;
    draw_rect(display, &layers[0].buffer, 0, y, display->mode.hdisplay, row_height, 0);
    draw_rect(display, &layers[1].buffer, 0, y, display->mode.hdisplay, row_height, 0);
    draw_menu_item(display, font, depth, layers, page, items[row], info,
                   row, row == selected, y);
}
