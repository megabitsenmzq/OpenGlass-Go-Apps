#include "wifi_view.h"
#include "password_keyboard.h"

#include <math.h>
#include <string.h>

static void draw_checkmark(Display *display, Buffer *buffer, int x, int center_y, uint32_t color)
{
    int i;
    for (i = 0; i <= 20; ++i)
        draw_rect(display, buffer, x + 4 + i, center_y - 4 + i, 6, 6, color);
    for (i = 0; i <= 36; ++i)
        draw_rect(display, buffer, x + 24 + i, center_y + 16 - i * 44 / 36, 6, 6, color);
}

static void draw_signal_icon(Display *display, Buffer *buffer, int center_x, int center_y,
                             int signal, uint32_t color)
{
    int level = signal >= -55 ? 4 : signal >= -65 ? 3 : signal >= -75 ? 2 : 1;
    int arc;
    draw_rect(display, buffer, center_x - 5, center_y - 5, 10, 10, color);
    for (arc = 0; arc < 3; ++arc) {
        int radius = 20 + arc * 12;
        uint32_t arc_color = arc < level - 1 ? color : 0xff505050;
        int angle;
        for (angle = 210; angle <= 330; angle += 2) {
            double radians = angle * 3.14159265358979323846 / 180.0;
            int x = center_x + (int)(cos(radians) * radius) - 3;
            int y = center_y + (int)(sin(radians) * radius) - 3;
            draw_rect(display, buffer, x, y, 6, 6, arc_color);
        }
    }
}

int wifi_view_networks_page_start(int selected)
{
    if (selected <= 1)
        return 0;
    return (selected - 1) / (WIFI_VISIBLE_ROWS - 1) * (WIFI_VISIBLE_ROWS - 1);
}

int wifi_view_networks_start_y(const Display *display, int visible_rows)
{
    if (visible_rows < 0)
        visible_rows = 0;
    if (visible_rows > WIFI_VISIBLE_ROWS)
        visible_rows = WIFI_VISIBLE_ROWS;
    return ((int)display->mode.vdisplay - visible_rows * WIFI_ROW_HEIGHT) / 2;
}

int wifi_view_networks_row_y(const Display *display, int visible_rows, int row, int scroll)
{
    int start_y = wifi_view_networks_start_y(display, visible_rows);
    return start_y + (row == 0 ? 0 : (row - scroll) * WIFI_ROW_HEIGHT);
}

static void draw_network_row_internal(Display *display, Font *font, const DepthEffect *depth,
                                      DepthLayer layers[2], int row, int y, int active,
                                      int scanning, const WifiNetwork *network,
                                      const char *current_ssid, size_t current_ssid_length,
                                      int force_front)
{
    int width = (int)display->mode.hdisplay;
    int margin = width / 12;
    Buffer *buffer = (active || force_front) ? &layers[0].buffer : &layers[1].buffer;
    uint32_t color = active ? 0xff101014 : 0xffd8d8d8;
    int bar_height = WIFI_ROW_HEIGHT - 8;
    int baseline;
    void (*draw_item_text)(Display *, Buffer *, Font *, const char *, int, int,
                           uint32_t) = active && depth->stereo ? draw_text_bold : draw_text;
    FT_Set_Pixel_Sizes(font->face, 0, 64);
    baseline = font_centered_baseline(font, y, bar_height);
    if (row == 0) {
        draw_back_arrow(display, buffer, margin / 3, y + bar_height / 2, color);
        draw_item_text(display, buffer, font, "Back", margin + 24, baseline, color);
    } else if (row == 1) {
        draw_item_text(display, buffer, font, scanning ? "Scanning..." : "Scan again",
                       margin + 24, baseline, color);
    } else {
        int is_current = network->ssid_length == current_ssid_length &&
                         memcmp(network->ssid, current_ssid, network->ssid_length) == 0;
        if (is_current)
            draw_checkmark(display, buffer, margin / 3, y + bar_height / 2, color);
        draw_text_fit(display, buffer, font, network->display_ssid, margin + 24,
                               baseline, color, width - margin * 2 - 190,
                               active && depth->stereo);
        draw_signal_icon(display, buffer, width - margin - 50, y + 90, network->signal, color);
    }
}

void wifi_view_draw_network_row(Display *display, Font *font, const DepthEffect *depth,
                                DepthLayer layers[2], int row, int y, int selected,
                                int scanning, int interactive, const WifiNetwork *network,
                                const char *current_ssid, size_t current_ssid_length)
{
    draw_network_row_internal(display, font, depth, layers, row, y,
                              row == selected && interactive, scanning,
                              network, current_ssid, current_ssid_length, 0);
}

void wifi_view_draw_network_row_anim(Display *display, Font *font, const DepthEffect *depth,
                                     DepthLayer layers[2], int row, int y, int active,
                                     int scanning, const WifiNetwork *network,
                                     const char *current_ssid, size_t current_ssid_length)
{
    int width = (int)display->mode.hdisplay;
    draw_rect(display, &layers[0].buffer, 0, y, width, WIFI_ROW_HEIGHT, 0);
    draw_rect(display, &layers[1].buffer, 0, y, width, WIFI_ROW_HEIGHT, 0);
    draw_network_row_internal(display, font, depth, layers, row, y, active, scanning,
                              network, current_ssid, current_ssid_length, 1);
}

static void draw_details_row_internal(Display *display, Font *font, const DepthEffect *depth,
                                      DepthLayer layers[2], int row, int y, int active,
                                      int connected, const char *const values[9],
                                      int force_front)
{
    int width = (int)display->mode.hdisplay;
    int margin = width / 12;
    Buffer *buffer = (active || force_front) ? &layers[0].buffer : &layers[1].buffer;
    uint32_t color = active ? 0xff101014 : 0xffd8d8d8;
    void (*draw_item_text)(Display *, Buffer *, Font *, const char *, int, int,
                           uint32_t) = active && depth->stereo ? draw_text_bold : draw_text;
    static const char *const labels[] = {
        "SSID", "IP Address", "Router", "MAC Address", "BSSID",
        "Signal Strength", "Frequency", "Link Speed", "Security"
    };
    if (row < 3) {
        const char *label = row == 0 ? "Back" : row == 1 ?
                            (connected ? "Disconnect" : "Connect") : "Forget";
        int bar_height = WIFI_ROW_HEIGHT - 8;
        FT_Set_Pixel_Sizes(font->face, 0, 64);
        if (row == 0)
            draw_back_arrow(display, buffer, margin / 3, y + bar_height / 2, color);
        draw_item_text(display, buffer, font, label, margin + 24,
                       font_centered_baseline(font, y, bar_height), color);
    } else {
        int info = row - 3;
        FT_Set_Pixel_Sizes(font->face, 0, 32);
        draw_item_text(display, buffer, font, labels[info], margin + 24,
                       y + WIFI_ROW_HEIGHT * 38 / 100, color);
        FT_Set_Pixel_Sizes(font->face, 0, 48);
        draw_text_fit(display, buffer, font, values[info], margin + 24,
                               y + WIFI_ROW_HEIGHT * 76 / 100, color,
                               width - margin * 2 - 24, active && depth->stereo);
    }
}

void wifi_view_draw_details_row(Display *display, Font *font, const DepthEffect *depth,
                                DepthLayer layers[2], int row, int y, int selected,
                                int interactive, int connected, const char *const values[9])
{
    draw_details_row_internal(display, font, depth, layers, row, y,
                              row == selected && interactive, connected, values, 0);
}

void wifi_view_draw_details_row_anim(Display *display, Font *font, const DepthEffect *depth,
                                     DepthLayer layers[2], int row, int y, int active,
                                     int connected, const char *const values[9])
{
    int width = (int)display->mode.hdisplay;
    draw_rect(display, &layers[0].buffer, 0, y, width, WIFI_ROW_HEIGHT, 0);
    draw_rect(display, &layers[1].buffer, 0, y, width, WIFI_ROW_HEIGHT, 0);
    draw_details_row_internal(display, font, depth, layers, row, y, active,
                              connected, values, 1);
}

void wifi_view_key_geometry(Display *display, int index, int *key_x, int *key_y,
                            int *key_width, int *key_height)
{
    int width = (int)display->mode.hdisplay;
    int height = (int)display->mode.vdisplay;
    int margin = width / 24;
    int gap = width / 120;
    int cell_width = (width - margin * 2 - gap * (WIFI_KEYBOARD_COLUMNS - 1)) / WIFI_KEYBOARD_COLUMNS;
    int cell_height = height * 46 / 844;
    int keyboard_height = cell_height * WIFI_KEYBOARD_ROWS + gap * (WIFI_KEYBOARD_ROWS - 1);
    int top = height - height * 20 / 844 - keyboard_height;
    int column = 0;
    int row = 0;
    int i;
    for (i = 0; i <= index; ++i) {
        int span = i == 41 ? 4 : i >= 40 ? 3 : 1;
        if (i == index) {
            *key_x = margin + column * (cell_width + gap);
            *key_y = top + row * (cell_height + gap);
            *key_width = cell_width * span + gap * (span - 1);
            *key_height = cell_height;
            return;
        }
        column += span;
        if (column >= WIFI_KEYBOARD_COLUMNS) {
            column = 0;
            ++row;
        }
    }
}

static void draw_backspace_icon(Display *display, Buffer *buffer, int center_x, int center_y,
                                uint32_t color)
{
    int i;
    draw_rect(display, buffer, center_x - 12, center_y - 27, 44, 6, color);
    draw_rect(display, buffer, center_x - 12, center_y + 21, 44, 6, color);
    draw_rect(display, buffer, center_x + 26, center_y - 24, 6, 48, color);
    for (i = 0; i <= 22; ++i) {
        draw_rect(display, buffer, center_x - 34 + i, center_y - i - 3, 6, 6, color);
        draw_rect(display, buffer, center_x - 34 + i, center_y + i - 3, 6, 6, color);
    }
    for (i = -10; i <= 10; ++i) {
        draw_rect(display, buffer, center_x + i + 5, center_y + i - 2, 5, 5, color);
        draw_rect(display, buffer, center_x + i + 5, center_y - i - 2, 5, 5, color);
    }
}

static void draw_shift_icon(Display *display, Buffer *buffer, int center_x, int center_y,
                            uint32_t color)
{
    int i;
    draw_rect(display, buffer, center_x - 4, center_y - 28, 8, 56, color);
    for (i = 0; i <= 24; ++i) {
        draw_rect(display, buffer, center_x - i - 3, center_y - 28 + i, 6, 6, color);
        draw_rect(display, buffer, center_x + i - 3, center_y - 28 + i, 6, 6, color);
    }
}

static void draw_space_icon(Display *display, Buffer *buffer, int center_x, int center_y,
                            uint32_t color)
{
    draw_rect(display, buffer, center_x - 34, center_y - 12, 6, 24, color);
    draw_rect(display, buffer, center_x + 28, center_y - 12, 6, 24, color);
    draw_rect(display, buffer, center_x - 34, center_y + 6, 68, 6, color);
}

static void draw_connect_icon(Display *display, Buffer *buffer, int center_x, int center_y,
                              uint32_t color)
{
    int i;
    draw_rect(display, buffer, center_x + 26, center_y - 28, 6, 34, color);
    draw_rect(display, buffer, center_x - 26, center_y, 58, 6, color);
    for (i = 0; i <= 22; ++i) {
        draw_rect(display, buffer, center_x - 28 + i, center_y - i - 3, 6, 6, color);
        draw_rect(display, buffer, center_x - 28 + i, center_y + i - 3, 6, 6, color);
    }
}

static void draw_password_key_internal(Display *display, Font *font, const DepthEffect *depth,
                                       DepthLayer layers[2], int index, int active, int layout,
                                       int force_front)
{
    const char *const *keys = password_keyboard_keys(layout);
    int x;
    int y;
    int key_width;
    int key_height;
    int special = index == 29 || (index == 30 && layout != 2) || index >= 40;
    int highlighted = active || (index == 30 && layout == 1);
    Buffer *buffer;
    uint32_t color;
    uint32_t background;
    const char *label = keys[index];
    wifi_view_key_geometry(display, index, &x, &y, &key_width, &key_height);
    if (force_front) {
        draw_rect(display, &layers[0].buffer, x, y, key_width, key_height, 0);
        draw_rect(display, &layers[1].buffer, x, y, key_width, key_height, 0);
    }
    buffer = (active || force_front) ? &layers[0].buffer : &layers[1].buffer;
    color = highlighted ? 0xff000000 : 0xffffffff;
    background = highlighted ? 0xffffffff : special ? 0xff444444 : 0xff2d2d2d;
    draw_rect(display, buffer, x, y, key_width, key_height, background);
    if (index == 29) {
        draw_backspace_icon(display, buffer, x + key_width / 2, y + key_height / 2, color);
    } else if (index == 30 && layout != 2) {
        draw_shift_icon(display, buffer, x + key_width / 2, y + key_height / 2, color);
    } else if (index == 41) {
        draw_space_icon(display, buffer, x + key_width / 2, y + key_height / 2, color);
    } else if (index == 42) {
        draw_connect_icon(display, buffer, x + key_width / 2, y + key_height / 2, color);
    } else {
        int text_width;
        int baseline;
        if (index == 40)
            label = layout == 2 ? "abc" : "#+=";
        FT_Set_Pixel_Sizes(font->face, 0, special ? 42 : 56);
        text_width = font_text_width(font, label);
        baseline = font_centered_baseline(font, y, key_height);
        if (active && depth->stereo)
            draw_text_bold(display, buffer, font, label, x + (key_width - text_width) / 2,
                           baseline, color);
        else
            draw_text(display, buffer, font, label, x + (key_width - text_width) / 2,
                      baseline, color);
    }
}

void wifi_view_draw_password_key(Display *display, Font *font, const DepthEffect *depth,
                                 DepthLayer layers[2], int index, int active, int layout)
{
    draw_password_key_internal(display, font, depth, layers, index, active, layout, 0);
}

void wifi_view_draw_password_key_anim(Display *display, Font *font, const DepthEffect *depth,
                                      DepthLayer layers[2], int index, int active, int layout)
{
    draw_password_key_internal(display, font, depth, layers, index, active, layout, 1);
}

static void draw_password_back_internal(Display *display, Font *font, const DepthEffect *depth,
                                        DepthLayer layers[2], int active, int force_front)
{
    int width = (int)display->mode.hdisplay;
    int back_margin = width / 12;
    int row_height = display->mode.vdisplay / 17;
    int top = row_height / 6;
    Buffer *buffer = (active || force_front) ? &layers[0].buffer : &layers[1].buffer;
    uint32_t color = active ? 0xff101014 : 0xffd8d8d8;
    int baseline;
    draw_rect(display, &layers[0].buffer, 0, top, width, row_height, 0);
    draw_rect(display, &layers[1].buffer, 0, top, width, row_height, 0);
    if (active)
        draw_rect(display, buffer, 0, top, width, row_height, 0xffe8e8e8);
    draw_back_arrow(display, buffer, back_margin / 3, top + row_height / 2, color);
    FT_Set_Pixel_Sizes(font->face, 0, 64);
    baseline = font_centered_baseline(font, top, row_height);
    if (active && depth->stereo)
        draw_text_bold(display, buffer, font, "Back", back_margin + 24, baseline, color);
    else
        draw_text(display, buffer, font, "Back", back_margin + 24, baseline, color);
}

void wifi_view_draw_password_back(Display *display, Font *font, const DepthEffect *depth,
                                  DepthLayer layers[2], int back_selected, int interactive)
{
    draw_password_back_internal(display, font, depth, layers,
                                back_selected && interactive, 0);
}

void wifi_view_draw_password_back_anim(Display *display, Font *font, const DepthEffect *depth,
                                       DepthLayer layers[2], int active)
{
    draw_password_back_internal(display, font, depth, layers, active, 1);
}

void wifi_view_draw_password_field(Display *display, Font *font, DepthLayer layers[2],
                                   const char *password)
{
    int width = (int)display->mode.hdisplay;
    int margin = width / 24;
    draw_rect(display, &layers[0].buffer, margin, 535, width - margin * 2, 140, 0);
    draw_rect(display, &layers[1].buffer, margin, 535, width - margin * 2, 140, 0xff303038);
    FT_Set_Pixel_Sizes(font->face, 0, 64);
    draw_text_fit(display, &layers[1].buffer, font, password,
                  margin + 30, font_centered_baseline(font, 535, 140),
                  0xffffffff, width - margin * 2 - 60, 0);
}

static void draw_clock_icon(Display *display, Buffer *buffer, int x, int y, uint32_t color)
{
    int angle;
    for (angle = 0; angle < 360; angle += 3) {
        double radians = angle * 3.14159265358979323846 / 180.0;
        int px = x + 32 + (int)(cos(radians) * 28) - 3;
        int py = y + 32 + (int)(sin(radians) * 28) - 3;
        draw_rect(display, buffer, px, py, 6, 6, color);
    }
    draw_rect(display, buffer, x + 29, y + 14, 6, 21, color);
    draw_rect(display, buffer, x + 32, y + 29, 18, 6, color);
    draw_rect(display, buffer, x + 28, y + 28, 8, 8, color);
}

static void draw_failure_icon(Display *display, Buffer *buffer, int x, int y, uint32_t color)
{
    int i;
    for (i = 0; i <= 48; ++i) {
        draw_rect(display, buffer, x + 5 + i, y + 5 + i, 6, 6, color);
        draw_rect(display, buffer, x + 53 - i, y + 5 + i, 6, 6, color);
    }
}

void wifi_view_draw_operation(Display *display, Font *font, const DepthEffect *depth,
                              DepthLayer layers[2], int failed, const char *operation,
                              const char *reason)
{
    Buffer *buffer = &layers[0].buffer;
    void (*draw_status_text)(Display *, Buffer *, Font *, const char *, int, int,
                             uint32_t) = depth->stereo ? draw_text_bold : draw_text;
    int width = (int)display->mode.hdisplay;
    int height = (int)display->mode.vdisplay;
    int margin = width / 12;
    int y = (height - WIFI_ROW_HEIGHT) / 2;
    FT_Set_Pixel_Sizes(font->face, 0, 64);
    draw_rect(display, buffer, 0, y, width, WIFI_ROW_HEIGHT, 0);
    draw_rect(display, &layers[1].buffer, 0, y, width, WIFI_ROW_HEIGHT, 0xffffffff);
    if (failed)
        draw_failure_icon(display, buffer, margin / 3,
                          y + (WIFI_ROW_HEIGHT - 64) / 2, 0xff000000);
    else
        draw_clock_icon(display, buffer, margin / 3,
                        y + (WIFI_ROW_HEIGHT - 64) / 2, 0xff000000);
    draw_status_text(display, buffer, font, operation, margin + 24,
                     font_centered_baseline(font, y, WIFI_ROW_HEIGHT), 0xff000000);
    if (failed && reason && reason[0]) {
        const int reason_height = 96;
        draw_rect(display, buffer, 0, y + WIFI_ROW_HEIGHT, width, reason_height, 0);
        draw_rect(display, &layers[1].buffer, 0, y + WIFI_ROW_HEIGHT, width, reason_height, 0xffffffff);
        FT_Set_Pixel_Sizes(font->face, 0, 48);
        draw_text_fit(display, buffer, font, reason, margin + 24,
                      font_centered_baseline(font, y + WIFI_ROW_HEIGHT, reason_height),
                      0xff000000, width - margin * 2, depth->stereo);
    }
}
