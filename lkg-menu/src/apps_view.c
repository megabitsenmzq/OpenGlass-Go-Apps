#include "apps_view.h"
#include "password_keyboard.h"
#include "wifi_view.h"

#include <string.h>

int apps_view_page_start(int selected)
{
    if (selected <= 1)
        return 0;
    return (selected - 1) / (APPS_VISIBLE_ROWS - 1) * (APPS_VISIBLE_ROWS - 1);
}

int apps_view_start_y(const Display *display, int visible_rows)
{
    if (visible_rows < 0)
        visible_rows = 0;
    if (visible_rows > APPS_VISIBLE_ROWS)
        visible_rows = APPS_VISIBLE_ROWS;
    return ((int)display->mode.vdisplay - visible_rows * APPS_ROW_HEIGHT) / 2;
}

int apps_view_row_y(const Display *display, int visible_rows, int row, int scroll)
{
    int start_y = apps_view_start_y(display, visible_rows);
    return start_y + (row == 0 ? 0 : (row - scroll) * APPS_ROW_HEIGHT);
}

void apps_view_draw_row(Display *display, Font *font, const DepthEffect *depth,
                        DepthLayer layers[2], int row, int y, int selected,
                        int interactive, const ExternalApp *app)
{
    int width = (int)display->mode.hdisplay;
    int margin = width / 12;
    int active = row == selected && interactive;
    Buffer *buffer = active ? &layers[0].buffer : &layers[1].buffer;
    uint32_t color = active ? 0xff101014 : 0xffd8d8d8;
    int bar_height = APPS_ROW_HEIGHT - 8;
    int baseline;
    void (*draw_item_text)(Display *, Buffer *, Font *, const char *, int, int,
                           uint32_t) = active && depth->stereo ? draw_text_bold : draw_text;
    FT_Set_Pixel_Sizes(font->face, 0, 64);
    baseline = font_centered_baseline(font, y, bar_height);
    if (row == 0) {
        draw_back_arrow(display, buffer, margin / 3, y + bar_height / 2, color);
        draw_item_text(display, buffer, font, "Back", margin + 24, baseline, color);
    } else if (app && app->name) {
        draw_text_fit(display, buffer, font, app->name, margin + 24,
                      baseline, color, width - margin * 2 - 24,
                      active && depth->stereo);
    }
}

void apps_view_draw_text_input(Display *display, Font *font, const DepthEffect *depth,
                               DepthLayer layers[2], const char *title,
                               const char *text, int layout, int key_selected,
                               int back_selected, int interactive)
{
    int width = (int)display->mode.hdisplay;
    int margin = width / 24;
    int i;
    memset(layers[0].buffer.pixels, 0, layers[0].buffer.size);
    memset(layers[1].buffer.pixels, 0, layers[1].buffer.size);
    wifi_view_draw_password_back(display, font, depth, layers, back_selected, interactive);
    FT_Set_Pixel_Sizes(font->face, 0, 48);
    draw_text(display, &layers[1].buffer, font, "Edit", margin, 270, 0xffffffff);
    if (title && title[0]) {
        FT_Set_Pixel_Sizes(font->face, 0, 64);
        draw_text_fit(display, &layers[1].buffer, font, title,
                      margin, 355, 0xffffffff, width - margin * 2, 0);
    }
    FT_Set_Pixel_Sizes(font->face, 0, 48);
    draw_text(display, &layers[1].buffer, font, "Input", margin, 490, 0xffffffff);
    wifi_view_draw_password_field(display, font, layers, text ? text : "");
    for (i = 0; i < WIFI_KEYBOARD_KEYS; ++i)
        wifi_view_draw_password_key(display, font, depth, layers, i,
                                    i == key_selected && !back_selected && interactive,
                                    layout);
    FT_Set_Pixel_Sizes(font->face, 0, 64);
}

void apps_view_draw_status(Display *display, Font *font, const DepthEffect *depth,
                           DepthLayer layers[2], int failed, const char *title,
                           const char *detail)
{
    wifi_view_draw_operation(display, font, depth, layers, failed,
                             title ? title : "", detail);
    FT_Set_Pixel_Sizes(font->face, 0, 64);
}
