#include "wifi_ui.h"
#include "password_keyboard.h"
#include "wifi_view.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static void clear_layers(WifiMenu *menu)
{
    memset(menu->layers[0].buffer.pixels, 0, menu->layers[0].buffer.size);
    memset(menu->layers[1].buffer.pixels, 0, menu->layers[1].buffer.size);
}

static int present_region(WifiMenu *menu, int x, int y, int width, int height,
                          int bar_top, int bar_bottom)
{
    DepthFrame frame;
    if (depth_frame_begin(&frame, menu->display, menu->depth, menu->layers,
                          &menu->cache_state, 0, 0) != 0)
        return -1;
    depth_frame_update_rect(&frame, x, y, width, height, bar_top, bar_bottom);
    return depth_frame_present(&frame);
}

static int networks_row_y(const WifiMenu *menu, int row)
{
    int visible_rows = menu->model.network_count + 2 - menu->scroll;
    return wifi_view_networks_row_y(menu->display, visible_rows, row, menu->scroll);
}

static void draw_network_row(WifiMenu *menu, int row)
{
    wifi_view_draw_network_row(menu->display, menu->font, menu->depth, menu->layers,
                               row, networks_row_y(menu, row),
                               menu->selected, menu->scan.pid > 0,
                               menu->connection.pid <= 0 && !menu->failure_until,
                               row >= 2 ? &menu->model.networks[row - 2] : NULL,
                               menu->model.current_ssid, menu->model.current_ssid_length);
}

static uint64_t networks_content_key(const WifiMenu *menu)
{
    return wifi_networks_content_key(menu->model.networks, menu->model.network_count,
                                     menu->scan.pid > 0, menu->model.current_ssid,
                                     menu->model.current_ssid_length);
}

static uint64_t details_content_key(const WifiMenu *menu)
{
    const char *values[] = {
        menu->model.ip_address, menu->model.router_address, menu->model.mac_address, menu->model.bssid,
        menu->model.signal, menu->model.frequency, menu->model.link_speed, menu->model.security
    };
    return wifi_details_content_key(&menu->model.selected_network,
                                    wifi_menu_network_is_current(menu, &menu->model.selected_network), values);
}

static int draw_networks(WifiMenu *menu)
{
    Display *display = menu->display;
    int width = (int)display->mode.hdisplay;
    int height = (int)display->mode.vdisplay;
    int count = menu->model.network_count + 2;
    int end = menu->scroll + WIFI_VISIBLE_ROWS;
    uint64_t content_key = networks_content_key(menu);
    DepthFrame frame;
    int full;
    int row;
    int result;
    if (depth_frame_begin(&frame, display, menu->depth, menu->layers,
                          &menu->cache_state, content_key,
                          menu->cached_page != WIFI_PAGE_NETWORKS ||
                          menu->cached_scroll != menu->scroll) != 0)
        return -1;
    full = frame.full;
    if (end > count)
        end = count;
    if (full) {
        clear_layers(menu);
        draw_network_row(menu, 0);
        for (row = menu->scroll + 1; row < end; ++row)
            draw_network_row(menu, row);
        result = depth_frame_update_rect(&frame, 0, 0, width, height,
                                         networks_row_y(menu, menu->selected),
                                         networks_row_y(menu, menu->selected) +
                                         WIFI_ROW_HEIGHT - 8);
    } else {
        int rows[2] = { menu->cached_selected, menu->selected };
        int i;
        result = 0;
        for (i = 0; i < 2; ++i) {
            int y;
            row = rows[i];
            if (i == 1 && row == rows[0])
                continue;
            y = networks_row_y(menu, row);
            draw_rect(display, &menu->layers[0].buffer, 0, y, width, WIFI_ROW_HEIGHT, 0);
            draw_rect(display, &menu->layers[1].buffer, 0, y, width, WIFI_ROW_HEIGHT, 0);
            draw_network_row(menu, row);
            result = depth_frame_update_rect(&frame, 0, y, width, WIFI_ROW_HEIGHT,
                                             row == menu->selected ? y : 0,
                                             row == menu->selected ? y + WIFI_ROW_HEIGHT - 8 : 0);
            if (result != 0)
                break;
        }
    }
    result = depth_frame_present(&frame);
    if (result == 0) {
        menu->cached_page = WIFI_PAGE_NETWORKS;
        menu->cached_selected = menu->selected;
        menu->cached_scroll = menu->scroll;
    }
    return result;
}

static void password_key_geometry(WifiMenu *menu, int index,
                                  int *key_x, int *key_y, int *key_width, int *key_height)
{
    wifi_view_key_geometry(menu->display, index, key_x, key_y, key_width, key_height);
}

static void draw_password_key(WifiMenu *menu, int index, int active)
{
    wifi_view_draw_password_key(menu->display, menu->font, menu->depth, menu->layers,
                                index, active, menu->keyboard_layout);
}

double wifi_ui_monotonic_seconds(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec + time.tv_nsec / 1000000000.0;
}

static void draw_password_back(WifiMenu *menu);

int wifi_ui_draw_password_cursor(WifiMenu *menu, int previous, int include_back)
{
    int old_x;
    int old_y;
    int old_width;
    int old_height;
    int new_x;
    int new_y;
    int new_width;
    int new_height;
    double started = wifi_ui_monotonic_seconds();
    DepthFrame frame;
    int result;
    if (depth_frame_begin(&frame, menu->display, menu->depth, menu->layers,
                          &menu->cache_state, 0, 0) != 0)
        return -1;
    if (include_back) {
        int row_height = menu->display->mode.vdisplay / 17;
        int top = row_height / 6;
        draw_password_back(menu);
        depth_frame_update_rect(&frame, 0, top, menu->display->mode.hdisplay,
                                row_height, 0, 0);
    }
    password_key_geometry(menu, previous, &old_x, &old_y, &old_width, &old_height);
    password_key_geometry(menu, menu->key_selected, &new_x, &new_y, &new_width, &new_height);
    draw_rect(menu->display, &menu->layers[0].buffer, old_x, old_y, old_width, old_height, 0);
    draw_rect(menu->display, &menu->layers[1].buffer, new_x, new_y, new_width, new_height, 0);
    draw_password_key(menu, previous, 0);
    draw_password_key(menu, menu->key_selected, !menu->back_selected &&
                      menu->connection.pid <= 0 && !menu->failure_until);
    depth_frame_update_rect(&frame, old_x, old_y, old_width, old_height, 0, 0);
    depth_frame_update_rect(&frame, new_x, new_y, new_width, new_height, 0, 0);
    result = depth_frame_present(&frame);
    if (result == 0) {
        printf("Wi-Fi cursor update: %.1f ms (%d -> %d)\n",
               (wifi_ui_monotonic_seconds() - started) * 1000.0, previous, menu->key_selected);
        fflush(stdout);
    }
    return result;
}

static void draw_password_back(WifiMenu *menu)
{
    wifi_view_draw_password_back(menu->display, menu->font, menu->depth, menu->layers,
                                 menu->back_selected,
                                 menu->connection.pid <= 0 && !menu->failure_until);
}

static void draw_password_field(WifiMenu *menu)
{
    wifi_view_draw_password_field(menu->display, menu->font, menu->layers, menu->password);
}

static void draw_password_keyboard(WifiMenu *menu)
{
    int width = (int)menu->display->mode.hdisplay;
    int top;
    int bottom;
    int key_x;
    int key_width;
    int key_height;
    int i;
    password_key_geometry(menu, 0, &key_x, &top, &key_width, &key_height);
    password_key_geometry(menu, WIFI_KEYBOARD_KEYS - 1, &key_x, &bottom, &key_width, &key_height);
    bottom += key_height;
    draw_rect(menu->display, &menu->layers[0].buffer, 0, top, width, bottom - top, 0);
    draw_rect(menu->display, &menu->layers[1].buffer, 0, top, width, bottom - top, 0);
    for (i = 0; i < WIFI_KEYBOARD_KEYS; ++i)
        draw_password_key(menu, i, i == menu->key_selected && !menu->back_selected &&
                          menu->connection.pid <= 0 && !menu->failure_until);
}

int wifi_ui_draw_password(WifiMenu *menu)
{
    Display *display = menu->display;
    Font *font = menu->font;
    int width = (int)display->mode.hdisplay;
    int height = (int)display->mode.vdisplay;
    int margin = width / 24;
    DepthFrame frame;
    int result;
    if (depth_frame_begin(&frame, display, menu->depth, menu->layers,
                          &menu->cache_state, 0, 1) != 0)
        return -1;
    clear_layers(menu);
    draw_password_back(menu);
    FT_Set_Pixel_Sizes(font->face, 0, 48);
    draw_text(display, &menu->layers[1].buffer, font, "Network", margin, 270, 0xffffffff);
    FT_Set_Pixel_Sizes(font->face, 0, 64);
    draw_text_fit(display, &menu->layers[1].buffer, font, menu->model.selected_network.display_ssid,
                  margin, 355, 0xffffffff, width - margin * 2, 0);
    FT_Set_Pixel_Sizes(font->face, 0, 48);
    draw_text(display, &menu->layers[1].buffer, font, "Password", margin, 490, 0xffffffff);
    draw_password_field(menu);
    draw_password_keyboard(menu);
    depth_frame_update_rect(&frame, 0, 0, width, height, 0, 0);
    result = depth_frame_present(&frame);
    if (result == 0) {
        menu->cached_page = WIFI_PAGE_PASSWORD;
        menu->cached_selected = menu->key_selected;
        menu->cached_scroll = menu->back_selected;
    }
    return result;
}

int wifi_ui_update_password_field(WifiMenu *menu)
{
    draw_password_field(menu);
    return present_region(menu, 0, 535, menu->display->mode.hdisplay, 140, 0, 0);
}

int wifi_ui_update_password_keyboard(WifiMenu *menu)
{
    int top;
    int bottom;
    int key_x;
    int key_width;
    int key_height;
    password_key_geometry(menu, 0, &key_x, &top, &key_width, &key_height);
    password_key_geometry(menu, WIFI_KEYBOARD_KEYS - 1, &key_x, &bottom, &key_width, &key_height);
    bottom += key_height;
    draw_password_keyboard(menu);
    return present_region(menu, 0, top, menu->display->mode.hdisplay, bottom - top, 0, 0);
}

static void draw_details_row(WifiMenu *menu, int row)
{
    int connected = wifi_menu_network_is_current(menu, &menu->model.selected_network);
    int count = 3 + (connected ? 9 : 1);
    int start_y = ((int)menu->display->mode.vdisplay - count * WIFI_ROW_HEIGHT) / 2;
    const char *values[] = {
        menu->model.selected_network.display_ssid, menu->model.ip_address, menu->model.router_address,
        menu->model.mac_address, menu->model.bssid, menu->model.signal, menu->model.frequency,
        menu->model.link_speed, menu->model.security
    };
    wifi_view_draw_details_row(menu->display, menu->font, menu->depth, menu->layers,
                               row, start_y + row * WIFI_ROW_HEIGHT, menu->selected,
                               menu->connection.pid <= 0 && !menu->failure_until,
                               connected, values);
}

static int draw_details(WifiMenu *menu)
{
    Display *display = menu->display;
    int connected = wifi_menu_network_is_current(menu, &menu->model.selected_network);
    int count = 3 + (connected ? 9 : 1);
    int total_height = count * WIFI_ROW_HEIGHT;
    int start_y = ((int)display->mode.vdisplay - total_height) / 2;
    uint64_t content_key = details_content_key(menu);
    DepthFrame frame;
    int full;
    int row;
    int result;
    if (depth_frame_begin(&frame, display, menu->depth, menu->layers,
                          &menu->cache_state, content_key,
                          menu->cached_page != WIFI_PAGE_DETAILS) != 0)
        return -1;
    full = frame.full;
    if (full) {
        clear_layers(menu);
        for (row = 0; row < count; ++row)
            draw_details_row(menu, row);
        result = depth_frame_update_rect(&frame, 0, start_y, display->mode.hdisplay,
                                         total_height, start_y + menu->selected * WIFI_ROW_HEIGHT,
                                         start_y + (menu->selected + 1) * WIFI_ROW_HEIGHT - 8);
    } else {
        int rows[2] = { menu->cached_selected, menu->selected };
        int i;
        result = 0;
        for (i = 0; i < 2; ++i) {
            int y;
            row = rows[i];
            if (i == 1 && row == rows[0])
                continue;
            y = start_y + row * WIFI_ROW_HEIGHT;
            draw_rect(display, &menu->layers[0].buffer, 0, y,
                      display->mode.hdisplay, WIFI_ROW_HEIGHT, 0);
            draw_rect(display, &menu->layers[1].buffer, 0, y,
                      display->mode.hdisplay, WIFI_ROW_HEIGHT, 0);
            draw_details_row(menu, row);
            result = depth_frame_update_rect(&frame, 0, y, display->mode.hdisplay,
                                             WIFI_ROW_HEIGHT,
                                             row == menu->selected ? y : 0,
                                             row == menu->selected ? y + WIFI_ROW_HEIGHT - 8 : 0);
            if (result != 0)
                break;
        }
    }
    result = depth_frame_present(&frame);
    if (result == 0) {
        menu->cached_page = WIFI_PAGE_DETAILS;
        menu->cached_selected = menu->selected;
        menu->cached_scroll = 0;
    }
    return result;
}

int wifi_ui_draw_current_page(WifiMenu *menu)
{
    if (menu->page == WIFI_PAGE_NETWORKS)
        return draw_networks(menu);
    if (menu->page == WIFI_PAGE_DETAILS)
        return draw_details(menu);
    return wifi_ui_draw_password(menu);
}

int wifi_ui_draw_operation(WifiMenu *menu)
{
    Display *display = menu->display;
    Font *font = menu->font;
    int width = (int)display->mode.hdisplay;
    int height = (int)display->mode.vdisplay;
    int failed = menu->connection.pid <= 0;
    int y = (height - WIFI_ROW_HEIGHT) / 2;
    int focus_x = 0;
    int focus_y = 0;
    int focus_width = width;
    int focus_height = 0;
    DepthFrame frame;
    const char *operation = failed ? "Connection failed" :
                            menu->operation == WIFI_OPERATION_DISCONNECT ? "Disconnecting..." :
                            menu->operation == WIFI_OPERATION_FORGET ? "Forgetting..." : "Connecting...";
    if (depth_frame_begin(&frame, display, menu->depth, menu->layers, NULL, 0, 0) != 0)
        return -1;
    if (menu->page == WIFI_PAGE_NETWORKS) {
        focus_y = networks_row_y(menu, menu->selected);
        focus_height = WIFI_ROW_HEIGHT;
        draw_rect(display, &menu->layers[0].buffer, 0, focus_y, width, focus_height, 0);
        draw_rect(display, &menu->layers[1].buffer, 0, focus_y, width, focus_height, 0);
        draw_network_row(menu, menu->selected);
    } else if (menu->page == WIFI_PAGE_DETAILS) {
        int connected = wifi_menu_network_is_current(menu, &menu->model.selected_network);
        int count = 3 + (connected ? 9 : 1);
        focus_y = (height - count * WIFI_ROW_HEIGHT) / 2 +
                  menu->selected * WIFI_ROW_HEIGHT;
        focus_height = WIFI_ROW_HEIGHT;
        draw_rect(display, &menu->layers[0].buffer, 0, focus_y, width, focus_height, 0);
        draw_rect(display, &menu->layers[1].buffer, 0, focus_y, width, focus_height, 0);
        draw_details_row(menu, menu->selected);
    } else if (menu->back_selected) {
        focus_height = height / 17;
        focus_y = focus_height / 6;
        draw_password_back(menu);
    } else {
        password_key_geometry(menu, menu->key_selected, &focus_x, &focus_y,
                              &focus_width, &focus_height);
        draw_rect(display, &menu->layers[0].buffer, focus_x, focus_y,
                  focus_width, focus_height, 0);
        draw_rect(display, &menu->layers[1].buffer, focus_x, focus_y,
                  focus_width, focus_height, 0);
        draw_password_key(menu, menu->key_selected, 0);
    }
    if (focus_height > 0)
        depth_frame_update_rect(&frame, focus_x, focus_y, focus_width, focus_height, 0, 0);
    {
        float background_disparity = menu->layers[1].disparity;
        int result;
        menu->cache_state.initialized = 0;
        menu->layers[1].disparity = 0.0f;
        wifi_view_draw_operation(display, font, menu->depth, menu->layers, failed, operation,
                                 failed ? menu->connection.output : NULL);
        depth_frame_update_rect(&frame, 0, y, width, WIFI_ROW_HEIGHT, 0, 0);
        if (failed && menu->connection.output[0])
            depth_frame_update_rect(&frame, 0, y + WIFI_ROW_HEIGHT, width, 96, 0, 0);
        result = depth_frame_present(&frame);
        menu->layers[1].disparity = background_disparity;
        return result;
    }
}
