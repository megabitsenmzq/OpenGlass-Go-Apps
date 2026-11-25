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

static void draw_details_row(WifiMenu *menu, int row);

static int list_row_y(const WifiMenu *menu, WifiPage page, int row)
{
    if (page == WIFI_PAGE_NETWORKS)
        return networks_row_y(menu, row);
    {
        int connected = wifi_menu_network_is_current(menu, &menu->model.selected_network);
        int count = 3 + (connected ? 9 : 1);
        int start_y = ((int)menu->display->mode.vdisplay - count * WIFI_ROW_HEIGHT) / 2;
        return start_y + row * WIFI_ROW_HEIGHT;
    }
}

static uint64_t list_content_key(const WifiMenu *menu, WifiPage page)
{
    return page == WIFI_PAGE_NETWORKS ? networks_content_key(menu) : details_content_key(menu);
}

static void draw_list_row_anim(WifiMenu *menu, WifiPage page, int row, int active)
{
    int y = list_row_y(menu, page, row);
    if (page == WIFI_PAGE_NETWORKS) {
        wifi_view_draw_network_row_anim(menu->display, menu->font, menu->depth,
                                        menu->layers, row, y, active,
                                        menu->scan.pid > 0,
                                        row >= 2 ? &menu->model.networks[row - 2] : NULL,
                                        menu->model.current_ssid,
                                        menu->model.current_ssid_length);
    } else {
        int connected = wifi_menu_network_is_current(menu, &menu->model.selected_network);
        const char *values[] = {
            menu->model.selected_network.display_ssid, menu->model.ip_address,
            menu->model.router_address, menu->model.mac_address, menu->model.bssid,
            menu->model.signal, menu->model.frequency, menu->model.link_speed,
            menu->model.security
        };
        wifi_view_draw_details_row_anim(menu->display, menu->font, menu->depth,
                                        menu->layers, row, y, active, connected, values);
    }
}

static int list_settle_row(WifiMenu *menu, WifiPage page, int row)
{
    int width = (int)menu->display->mode.hdisplay;
    int y = list_row_y(menu, page, row);
    draw_rect(menu->display, &menu->layers[0].buffer, 0, y, width, WIFI_ROW_HEIGHT, 0);
    draw_rect(menu->display, &menu->layers[1].buffer, 0, y, width, WIFI_ROW_HEIGHT, 0);
    if (page == WIFI_PAGE_NETWORKS)
        draw_network_row(menu, row);
    else
        draw_details_row(menu, row);
    return depth_anim_upload_rect(menu->display, menu->depth, menu->layers,
                                  0, y, width, WIFI_ROW_HEIGHT);
}

static int list_anim_finish(WifiMenu *menu)
{
    if (!menu->anim.active)
        return 0;
    if (list_settle_row(menu, menu->anim_page, menu->anim.old_id) != 0)
        return -1;
    depth_cache_commit(menu->depth, &menu->cache_state, menu->anim.content_key);
    menu->anim.active = 0;
    return 0;
}

static void list_anim_cancel(WifiMenu *menu)
{
    WifiPage page = menu->anim_page;
    uint64_t expected_key;
    if (!menu->anim.active)
        return;
    expected_key = list_content_key(menu, menu->page);
    if (menu->page == page && menu->cache_state.initialized &&
        menu->anim.content_key == expected_key &&
        (page != WIFI_PAGE_NETWORKS || menu->cached_scroll == menu->scroll)) {
        int i;
        if (list_settle_row(menu, page, menu->anim.old_id) == 0) {
            menu->anim.new_rect.disparity = menu->depth->front_disparity;
            for (i = 0; i < menu->anim.clip_count; ++i) {
                if (depth_gallery_anim_region(menu->display, menu->depth, menu->layers,
                                              &menu->anim.new_rect, NULL,
                                              menu->anim.clip_x[i], menu->anim.clip_y[i],
                                              menu->anim.clip_w[i], menu->anim.clip_h[i],
                                              menu->anim.bar_top, menu->anim.bar_bottom, 1) != 0) {
                    menu->anim.active = 0;
                    return;
                }
            }
            depth_cache_commit(menu->depth, &menu->cache_state, expected_key);
        }
    }
    menu->anim.active = 0;
}

static int list_anim_begin(WifiMenu *menu, WifiPage page, uint64_t content_key, int same_scroll)
{
    Display *display = menu->display;
    int width = (int)display->mode.hdisplay;
    int interactive = menu->connection.pid <= 0 && !menu->failure_until;
    int old_row = menu->cached_selected;
    int new_row = menu->selected;
    float old_start = menu->depth->front_disparity;
    float new_start = menu->depth->back_disparity;
    int old_y;
    int new_y;
    int bar_top;
    int bar_bottom;
    DepthAnimRect old_rect;
    DepthAnimRect new_rect;
    if (!menu->depth->stereo || !depth_gallery_direct_upload(display, menu->depth) ||
        !interactive || menu->cached_page != page || !same_scroll || old_row < 0 ||
        (!menu->anim.active ? depth_cache_needs_full(menu->depth, &menu->cache_state, content_key)
                            : (!menu->cache_state.initialized || menu->anim_page != page ||
                               menu->anim.content_key != content_key)))
        return 0;
    if (old_row == new_row)
        return menu->anim.active ? 1 : 0;
    if (menu->anim.active) {
        DepthAnim prev = menu->anim;
        if (old_row == prev.new_id)
            old_start = prev.cur_new_disparity;
        if (new_row == prev.old_id)
            new_start = prev.cur_old_disparity;
        if (prev.old_id != old_row && prev.old_id != new_row) {
            int cx, cy, cw, ch;
            if (list_settle_row(menu, page, prev.old_id) != 0)
                return -1;
            depth_anim_clip_rect(display, menu->depth,
                                 prev.old_rect.x0, prev.old_rect.y0,
                                 prev.old_rect.x1 - prev.old_rect.x0,
                                 prev.old_rect.y1 - prev.old_rect.y0,
                                 &cx, &cy, &cw, &ch);
            if (cw > 0 && ch > 0 &&
                depth_gallery_anim_region(display, menu->depth, menu->layers,
                                          NULL, NULL, cx, cy, cw, ch, 0, 0, 1) != 0)
                return -1;
        }
        menu->anim.active = 0;
    }
    old_y = list_row_y(menu, page, old_row);
    new_y = list_row_y(menu, page, new_row);
    bar_top = new_y;
    bar_bottom = new_y + WIFI_ROW_HEIGHT - 8;
    old_rect.y0 = old_y;
    old_rect.y1 = old_y + WIFI_ROW_HEIGHT;
    old_rect.disparity = 0.0f;
    new_rect.y0 = new_y;
    new_rect.y1 = new_y + WIFI_ROW_HEIGHT;
    new_rect.disparity = 0.0f;
    draw_list_row_anim(menu, page, old_row, 0);
    draw_list_row_anim(menu, page, new_row, 1);
    depth_anim_tight_x(display, &menu->layers[0].buffer, old_y, WIFI_ROW_HEIGHT,
                       0, width, &old_rect.x0, &old_rect.x1);
    depth_anim_tight_x(display, &menu->layers[0].buffer, new_y, WIFI_ROW_HEIGHT,
                       0, width, &new_rect.x0, &new_rect.x1);
    if (depth_anim_upload_rect(display, menu->depth, menu->layers,
                               old_rect.x0, old_rect.y0,
                               old_rect.x1 - old_rect.x0,
                               old_rect.y1 - old_rect.y0) != 0 ||
        depth_anim_upload_rect(display, menu->depth, menu->layers,
                               new_rect.x0, new_rect.y0,
                               new_rect.x1 - new_rect.x0,
                               new_rect.y1 - new_rect.y0) != 0 ||
        depth_gallery_anim_clear(display, menu->depth,
                                 0, old_y, width, WIFI_ROW_HEIGHT,
                                 bar_top, bar_bottom) != 0 ||
        depth_gallery_anim_clear(display, menu->depth,
                                 0, new_y, width, WIFI_ROW_HEIGHT,
                                 bar_top, bar_bottom) != 0)
        return -1;
    depth_anim_start(&menu->anim, display, menu->depth, old_row, new_row,
                     &old_rect, &new_rect,
                     bar_top, bar_bottom, 0,
                     content_key, old_start, new_start);
    menu->anim_page = page;
    if (wifi_ui_anim_step(menu) != 0)
        return -1;
    menu->cached_page = page;
    menu->cached_selected = new_row;
    menu->cached_scroll = page == WIFI_PAGE_NETWORKS ? menu->scroll : 0;
    return 1;
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
    int anim_result = list_anim_begin(menu, WIFI_PAGE_NETWORKS, content_key,
                                      menu->cached_scroll == menu->scroll);
    if (anim_result < 0)
        return -1;
    if (anim_result > 0)
        return 0;
    wifi_ui_anim_cancel(menu);
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

static void password_item_bounds(Display *display, int id, DepthAnimRect *rect)
{
    if (id == WIFI_PASSWORD_ANIM_BACK_ID) {
        int row_height = (int)display->mode.vdisplay / 17;
        int top = row_height / 6;
        rect->x0 = 0;
        rect->y0 = top;
        rect->x1 = (int)display->mode.hdisplay;
        rect->y1 = top + row_height;
    } else {
        int kx, ky, kw, kh;
        wifi_view_key_geometry(display, id, &kx, &ky, &kw, &kh);
        rect->x0 = kx;
        rect->y0 = ky;
        rect->x1 = kx + kw;
        rect->y1 = ky + kh;
    }
    rect->disparity = 0.0f;
}

static int password_item_settle(Display *display, Font *font, const DepthEffect *depth,
                                DepthLayer layers[2], int layout, int id, int active)
{
    DepthAnimRect rect;
    password_item_bounds(display, id, &rect);
    if (id == WIFI_PASSWORD_ANIM_BACK_ID) {
        wifi_view_draw_password_back(display, font, depth, layers, active, 1);
    } else {
        draw_rect(display, &layers[0].buffer, rect.x0, rect.y0,
                  rect.x1 - rect.x0, rect.y1 - rect.y0, 0);
        draw_rect(display, &layers[1].buffer, rect.x0, rect.y0,
                  rect.x1 - rect.x0, rect.y1 - rect.y0, 0);
        wifi_view_draw_password_key(display, font, depth, layers, id, active, layout);
    }
    return depth_anim_upload_rect(display, depth, layers, rect.x0, rect.y0,
                                  rect.x1 - rect.x0, rect.y1 - rect.y0);
}

static void password_item_draw_anim(Display *display, Font *font, const DepthEffect *depth,
                                    DepthLayer layers[2], int layout, int id, int active)
{
    if (id == WIFI_PASSWORD_ANIM_BACK_ID)
        wifi_view_draw_password_back_anim(display, font, depth, layers, active);
    else
        wifi_view_draw_password_key_anim(display, font, depth, layers, id, active, layout);
}

static int password_anim_finish(Display *display, Font *font, const DepthEffect *depth,
                                DepthLayer layers[2], DepthCacheState *cache_state,
                                DepthAnim *anim, int layout)
{
    if (!anim->active)
        return 0;
    if (password_item_settle(display, font, depth, layers, layout, anim->old_id, 0) != 0)
        return -1;
    depth_cache_commit(depth, cache_state, anim->content_key);
    anim->active = 0;
    return 0;
}

int wifi_ui_password_anim_step(Display *display, Font *font, const DepthEffect *depth,
                               DepthLayer layers[2], DepthCacheState *cache_state,
                               DepthAnim *anim, int layout)
{
    int finished = 0;
    if (!anim->active)
        return 0;
    if (depth_anim_step(anim, display, depth, layers, "Wi-Fi key", &finished) != 0)
        return -1;
    if (finished)
        return password_anim_finish(display, font, depth, layers, cache_state, anim, layout);
    return 0;
}

void wifi_ui_password_anim_cancel(Display *display, Font *font, const DepthEffect *depth,
                                  DepthLayer layers[2], DepthCacheState *cache_state,
                                  DepthAnim *anim, int layout)
{
    if (!anim->active)
        return;
    if (cache_state->initialized && anim->content_key == 0) {
        int i;
        if (password_item_settle(display, font, depth, layers, layout, anim->old_id, 0) == 0) {
            anim->new_rect.disparity = depth->front_disparity;
            for (i = 0; i < anim->clip_count; ++i) {
                if (depth_gallery_anim_region(display, depth, layers,
                                              &anim->new_rect, NULL,
                                              anim->clip_x[i], anim->clip_y[i],
                                              anim->clip_w[i], anim->clip_h[i],
                                              0, 0, 1) != 0) {
                    anim->active = 0;
                    return;
                }
            }
            depth_cache_commit(depth, cache_state, 0);
        }
    }
    anim->active = 0;
}

int wifi_ui_password_anim_begin(Display *display, Font *font, const DepthEffect *depth,
                                DepthLayer layers[2], DepthCacheState *cache_state,
                                DepthAnim *anim, int layout, int old_id, int new_id)
{
    float old_start = depth->front_disparity;
    float new_start = depth->back_disparity;
    DepthAnimRect old_rect;
    DepthAnimRect new_rect;
    if (!depth->stereo || !depth_gallery_direct_upload(display, depth) ||
        old_id < 0 || new_id < 0 || old_id == new_id ||
        (!anim->active ? depth_cache_needs_full(depth, cache_state, 0)
                       : (!cache_state->initialized || anim->content_key != 0)))
        return 0;
    if (anim->active) {
        DepthAnim prev = *anim;
        if (old_id == prev.new_id)
            old_start = prev.cur_new_disparity;
        if (new_id == prev.old_id)
            new_start = prev.cur_old_disparity;
        if (prev.old_id != old_id && prev.old_id != new_id) {
            int cx, cy, cw, ch;
            if (password_item_settle(display, font, depth, layers, layout,
                                     prev.old_id, 0) != 0)
                return -1;
            depth_anim_clip_rect(display, depth,
                                 prev.old_rect.x0, prev.old_rect.y0,
                                 prev.old_rect.x1 - prev.old_rect.x0,
                                 prev.old_rect.y1 - prev.old_rect.y0,
                                 &cx, &cy, &cw, &ch);
            if (cw > 0 && ch > 0 &&
                depth_gallery_anim_region(display, depth, layers, NULL, NULL,
                                          cx, cy, cw, ch, 0, 0, 1) != 0)
                return -1;
        }
        anim->active = 0;
    }
    password_item_bounds(display, old_id, &old_rect);
    password_item_bounds(display, new_id, &new_rect);
    password_item_draw_anim(display, font, depth, layers, layout, old_id, 0);
    password_item_draw_anim(display, font, depth, layers, layout, new_id, 1);
    if (depth_anim_upload_rect(display, depth, layers, old_rect.x0, old_rect.y0,
                               old_rect.x1 - old_rect.x0,
                               old_rect.y1 - old_rect.y0) != 0 ||
        depth_anim_upload_rect(display, depth, layers, new_rect.x0, new_rect.y0,
                               new_rect.x1 - new_rect.x0,
                               new_rect.y1 - new_rect.y0) != 0)
        return -1;
    depth_anim_start(anim, display, depth, old_id, new_id, &old_rect, &new_rect,
                     0, 0, 1, 0, old_start, new_start);
    if (wifi_ui_password_anim_step(display, font, depth, layers,
                                   cache_state, anim, layout) != 0)
        return -1;
    return 1;
}

static void draw_password_back(WifiMenu *menu);

int wifi_ui_draw_password_cursor(WifiMenu *menu, int previous, int previous_back)
{
    int old_id = previous_back ? WIFI_PASSWORD_ANIM_BACK_ID : previous;
    int new_id = menu->back_selected ? WIFI_PASSWORD_ANIM_BACK_ID : menu->key_selected;
    int interactive = menu->connection.pid <= 0 && !menu->failure_until;
    int include_back = previous_back != menu->back_selected;
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
    if (interactive && menu->cached_page == WIFI_PAGE_PASSWORD) {
        int anim_result = wifi_ui_password_anim_begin(menu->display, menu->font,
                                                      menu->depth, menu->layers,
                                                      &menu->cache_state, &menu->anim,
                                                      menu->keyboard_layout,
                                                      old_id, new_id);
        if (anim_result < 0)
            return -1;
        if (anim_result > 0) {
            menu->anim_page = WIFI_PAGE_PASSWORD;
            menu->cached_selected = menu->key_selected;
            menu->cached_scroll = menu->back_selected;
            return 0;
        }
    }
    wifi_ui_anim_cancel(menu);
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
    draw_password_key(menu, menu->key_selected, !menu->back_selected && interactive);
    depth_frame_update_rect(&frame, old_x, old_y, old_width, old_height, 0, 0);
    depth_frame_update_rect(&frame, new_x, new_y, new_width, new_height, 0, 0);
    result = depth_frame_present(&frame);
    if (result == 0) {
        menu->cached_selected = menu->key_selected;
        menu->cached_scroll = menu->back_selected;
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
    menu->anim.active = 0;
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
    wifi_ui_anim_cancel(menu);
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
    wifi_ui_anim_cancel(menu);
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
    int anim_result = list_anim_begin(menu, WIFI_PAGE_DETAILS, content_key, 1);
    if (anim_result < 0)
        return -1;
    if (anim_result > 0)
        return 0;
    wifi_ui_anim_cancel(menu);
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

int wifi_ui_anim_active(const WifiMenu *menu)
{
    return menu->anim.active;
}

int wifi_ui_anim_step(WifiMenu *menu)
{
    int finished = 0;
    if (!menu->anim.active)
        return 0;
    if (menu->anim_page == WIFI_PAGE_PASSWORD)
        return wifi_ui_password_anim_step(menu->display, menu->font, menu->depth,
                                          menu->layers, &menu->cache_state,
                                          &menu->anim, menu->keyboard_layout);
    if (depth_anim_step(&menu->anim, menu->display, menu->depth, menu->layers,
                        menu->anim_page == WIFI_PAGE_NETWORKS ? "Wi-Fi list" : "Wi-Fi details",
                        &finished) != 0)
        return -1;
    if (finished)
        return list_anim_finish(menu);
    return 0;
}

void wifi_ui_anim_cancel(WifiMenu *menu)
{
    if (!menu->anim.active)
        return;
    if (menu->anim_page == WIFI_PAGE_PASSWORD) {
        if (menu->page == WIFI_PAGE_PASSWORD)
            wifi_ui_password_anim_cancel(menu->display, menu->font, menu->depth,
                                         menu->layers, &menu->cache_state,
                                         &menu->anim, menu->keyboard_layout);
        else
            menu->anim.active = 0;
        return;
    }
    list_anim_cancel(menu);
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
    wifi_ui_anim_cancel(menu);
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
