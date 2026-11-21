#define _GNU_SOURCE
#include "gallery.h"
#include "cache_key.h"
#include "thumbnail.h"
#include "serial_parser.h"
#include "gallery_view.h"
#include "gallery_internal.h"
#include "media_scan.h"
#include "depth_frame.h"
#include "prerender_view.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>

extern char **environ;

static dev_t gallery_usb_device(void);

static void gallery_clear(Gallery *gallery)
{
    DepthEffect *depth = gallery->depth;
    DepthLayer *layers = gallery->cache.layers;
    int i;
    media_items_clear(&gallery->items, gallery->count);
    free(gallery->cache.packed[0]);
    free(gallery->cache.packed[1]);
    memset(gallery, 0, sizeof(*gallery));
    gallery->depth = depth;
    gallery->cache.layers = layers;
    if (layers) {
        for (i = 0; i < 2; ++i) {
            if (layers[i].buffer.pixels)
                memset(layers[i].buffer.pixels, 0, layers[i].buffer.size);
        }
    }
}

static void gallery_destroy(Gallery *gallery)
{
    gallery_clear(gallery);
}

void gallery_release_memory(Gallery *gallery)
{
    size_t i;
    for (i = 0; i < gallery->count; ++i) {
        free(gallery->items[i].thumbnail);
        gallery->items[i].thumbnail = NULL;
        gallery->items[i].thumbnail_state = 0;
        gallery->items[i].progress = 0.0f;
    }
    free(gallery->cache.packed[0]);
    free(gallery->cache.packed[1]);
    gallery->cache.packed[0] = NULL;
    gallery->cache.packed[1] = NULL;
    gallery->cache.packed_size = 0;
    gallery->cache.base_valid = 0;
    memset(gallery->cache.valid, 0, sizeof(gallery->cache.valid));
    memset(gallery->cache.cells, 0, sizeof(gallery->cache.cells));
    gallery->cache.bake_active = 0;
    gallery->cache.bake_base = 0;
    gallery->cache.displayed_cached = 0;
}

int gallery_scan(Gallery *gallery, const char *directory)
{
    if (strlen(directory) >= PATH_MAX)
        return -1;
    gallery_clear(gallery);
    strcpy(gallery->directory, directory);
    if (media_scan_directory(&gallery->items, &gallery->count, directory) != 0)
        return -1;
    if (strcmp(directory, MEDIA_ROOT) == 0 && !gallery_usb_device()) {
        size_t i;
        for (i = 0; i < gallery->count; ++i) {
            GalleryItem *item = &gallery->items[i];
            if (strcmp(item->path, MEDIA_ROOT "/USB") != 0)
                continue;
            free(item->name);
            free(item->path);
            free(item->cover_path);
            free(item->thumbnail);
            memmove(item, item + 1, (gallery->count - i - 1) * sizeof(*item));
            --gallery->count;
            break;
        }
    }
    gallery->page = 0;
    gallery->cursor = gallery_default_cursor(gallery->count);
    return 0;
}

static size_t page_start(const Gallery *gallery)
{
    return gallery->page == 0 ? 0 : FIRST_PAGE_ITEMS +
           (size_t)(gallery->page - 1) * PAGE_ITEMS;
}

static size_t page_items(const Gallery *gallery)
{
    size_t start = page_start(gallery);
    size_t capacity = gallery->page == 0 ? FIRST_PAGE_ITEMS : PAGE_ITEMS;
    size_t remaining = start < gallery->count ? gallery->count - start : 0;
    return remaining < capacity ? remaining : capacity;
}

static int last_cursor(const Gallery *gallery)
{
    size_t items = page_items(gallery);
    return gallery->page == 0 ? (int)items + 1 : (int)items - 1;
}

static int page_count(const Gallery *gallery)
{
    size_t remaining;
    if (gallery->count <= FIRST_PAGE_ITEMS)
        return 1;
    remaining = gallery->count - FIRST_PAGE_ITEMS;
    return 1 + (int)((remaining + PAGE_ITEMS - 1) / PAGE_ITEMS);
}

static GalleryItem *selected_item(Gallery *gallery)
{
    size_t index;
    if (gallery->page == 0) {
        if (gallery->cursor < 2)
            return NULL;
        index = gallery->cursor - 2;
    } else {
        index = page_start(gallery) + (size_t)gallery->cursor;
    }
    return index < gallery->count ? &gallery->items[index] : NULL;
}

static int gallery_update_cell(Display *display, Font *font, Gallery *gallery,
                               DepthLayer *layers, int cell, int selected, GalleryItem *item)
{
    gallery_view_draw_cell(display, font, layers, cell, selected, item,
                           gallery->page == 0 && cell == 0, gallery->cursor);
    return 0;
}

static int gallery_update_footer(Display *display, Font *font, Gallery *gallery,
                                 DepthLayer layers[2])
{
    const int y = display->mode.vdisplay - 70;
    char page_text[32];
    Buffer *buffer = &layers[1].buffer;
    int row;
    for (row = y; row < y + 70; ++row)
        memset(buffer->pixels + (size_t)row * buffer->stride, 0,
               (size_t)display->mode.hdisplay * 4);
    draw_rect(display, buffer, 0, y, display->mode.hdisplay, 70, 0x101014);
    snprintf(page_text, sizeof(page_text), "%d/%d", gallery->page + 1, page_count(gallery));
    FT_Set_Pixel_Sizes(font->face, 0, 48);
    gallery_view_draw_text_start(display, buffer, font, gallery->directory, 30,
                    display->mode.vdisplay - 24,
                    display->mode.hdisplay - 240, 0xa0a0a0);
    draw_text(display, buffer, font, page_text, display->mode.hdisplay - 140,
              display->mode.vdisplay - 24, 0xe8e8e8);
    FT_Set_Pixel_Sizes(font->face, 0, 64);
    return 0;
}

static int gallery_pack(Display *display, Gallery *gallery, DepthLayer *layers,
                        int x, int y, int width, int height, const uint8_t **pixels)
{
    size_t needed = (size_t)width * height * 4;
    int layer;
    if (needed > gallery->cache.packed_size) {
        uint8_t *allocation = realloc(gallery->cache.packed[0], needed);
        if (!allocation)
            return -1;
        gallery->cache.packed[0] = allocation;
        allocation = realloc(gallery->cache.packed[1], needed);
        if (!allocation)
            return -1;
        gallery->cache.packed[1] = allocation;
        gallery->cache.packed_size = needed;
    }
    for (layer = 0; layer < 2; ++layer) {
        int row;
        depth_layer_finish_area(&layers[layer], x, y, width, height,
                                display->mode.hdisplay, display->mode.vdisplay);
        for (row = 0; row < height; ++row) {
            memcpy(gallery->cache.packed[layer] + (size_t)row * width * 4,
                   layers[layer].buffer.pixels + (size_t)(y + row) *
                   layers[layer].buffer.stride + (size_t)x * 4,
                   (size_t)width * 4);
        }
        pixels[layer] = gallery->cache.packed[layer];
    }
    return 0;
}

static GalleryItem *gallery_cell_item(Gallery *gallery, int cell)
{
    size_t index;
    if (gallery->page == 0 && cell > 0) {
        index = (size_t)cell - 1;
        return index < gallery->count ? &gallery->items[index] : NULL;
    }
    if (gallery->page > 0) {
        index = page_start(gallery) + (size_t)cell;
        return index < gallery->count ? &gallery->items[index] : NULL;
    }
    return NULL;
}

static void gallery_selected_slot(const Gallery *gallery, int *cell_out, int *slot_out)
{
    if (gallery->page == 0) {
        if (gallery->cursor < 2) {
            *cell_out = 0;
            *slot_out = gallery->cursor;
        } else {
            *cell_out = gallery->cursor - 1;
            *slot_out = 1;
        }
    } else {
        *cell_out = gallery->cursor;
        *slot_out = 1;
    }
}

static void gallery_cell_patch_rect(Display *display, int cell,
                                    int *patch_x, int *patch_y,
                                    int *patch_w, int *patch_h,
                                    int *src_x, int *src_w)
{
    const int screen_w = display->mode.hdisplay;
    const int screen_h = display->mode.vdisplay;
    const int cell_width = screen_w / 3;
    const int grid_y = 4;
    const int cell_height = (screen_h - grid_y - 50) / 3;
    const int x = (cell % 3) * cell_width + (cell_width - THUMB_WIDTH) / 2;
    const int y = grid_y + (cell / 3) * cell_height + (cell_height - THUMB_HEIGHT) / 2;
    /* Covers thumbnail + 8px white selection border + +/-71px horizontal disparity */
    int px0 = x - 79;
    int px1 = x + THUMB_WIDTH + 79;
    int py0 = y - 8;
    int py1 = y + THUMB_HEIGHT + 8;
    int sx0;
    int sx1;
    if (px0 < 0)
        px0 = 0;
    if (px1 > screen_w)
        px1 = screen_w;
    if (py0 < 0)
        py0 = 0;
    if (py1 > screen_h)
        py1 = screen_h;
    sx0 = px0 - 79;
    sx1 = px1 + 79;
    if (sx0 < 0)
        sx0 = 0;
    if (sx1 > screen_w)
        sx1 = screen_w;
    *patch_x = px0;
    *patch_y = py0;
    *patch_w = px1 - px0;
    *patch_h = py1 - py0;
    *src_x = sx0;
    *src_w = sx1 - sx0;
}

static int gallery_layers_selected_cell(const Gallery *gallery)
{
    int cell;
    for (cell = 0; cell < 9; ++cell) {
        if (gallery->cache.cells[cell].selected)
            return cell;
    }
    return -1;
}

static int gallery_cache_pending(Gallery *gallery)
{
    int cell;
    if (!gallery->cache.base_valid)
        return 1;
    if (gallery->page == 0 && (!gallery->cache.valid[0][0] || !gallery->cache.valid[0][1]))
        return 1;
    for (cell = gallery->page == 0 ? 1 : 0; cell < 9; ++cell) {
        if (!gallery->cache.valid[cell][1])
            return 1;
    }
    return 0;
}

static int gallery_cache_ready(Gallery *gallery)
{
    int cell;
    int slot;
    gallery_selected_slot(gallery, &cell, &slot);
    return gallery->cache.base_valid && gallery->cache.valid[cell][slot];
}

static int gallery_prepare_bake_pixels(Display *display, Gallery *gallery,
                                       int x, int y, int width, int height,
                                       const uint8_t *pixels[2])
{
    int direct_upload = depth_gallery_direct_upload(display, gallery->depth);
    if (direct_upload) {
        int layer;
        for (layer = 0; layer < 2; ++layer)
            depth_layer_finish_area(&gallery->cache.layers[layer],
                                    x, y, width, height,
                                    display->mode.hdisplay, display->mode.vdisplay);
        pixels[0] = NULL;
        pixels[1] = NULL;
        return 0;
    }
    return gallery_pack(display, gallery, gallery->cache.layers,
                        x, y, width, height, pixels);
}

static int gallery_bake_next(Display *display, Font *font, Gallery *gallery)
{
    const int screen_w = display->mode.hdisplay;
    const int screen_h = display->mode.vdisplay;
    DepthLayer *layers = gallery->cache.layers;
    const uint8_t *pixels[2] = {NULL, NULL};
    if (!layers || !layers[0].buffer.pixels || !layers[1].buffer.pixels)
        return -1;
    if (!gallery->cache.base_valid) {
        int band_count = (screen_h + GALLERY_BAKE_BAND_HEIGHT - 1) /
                         GALLERY_BAKE_BAND_HEIGHT;
        int band;
        int result;
        if (!gallery->cache.bake_active || !gallery->cache.bake_base) {
            int layers_sel = gallery_layers_selected_cell(gallery);
            band = 0;
            if (layers_sel >= 0) {
                int special = gallery->page == 0 && layers_sel == 0;
                gallery_view_draw_cell(display, font, layers, layers_sel, 0,
                                       gallery_cell_item(gallery, layers_sel),
                                       special, 2);
                if (layers_sel / 3 == 2)
                    gallery_update_footer(display, font, gallery, layers);
            }
            result = gallery_prepare_bake_pixels(display, gallery, 0, 0,
                                                 screen_w, screen_h, pixels);
            if (result == 0)
                result = depth_gallery_bake_base(display, gallery->depth, layers, pixels, 0);
            if (layers_sel >= 0) {
                int special = gallery->page == 0 && layers_sel == 0;
                gallery_view_draw_cell(display, font, layers, layers_sel, 1,
                                       gallery_cell_item(gallery, layers_sel),
                                       special, gallery->cache.layers_cursor);
                if (layers_sel / 3 == 2)
                    gallery_update_footer(display, font, gallery, layers);
            }
            if (result != 0) {
                gallery->cache.bake_active = 0;
                gallery->cache.bake_base = 0;
                return -1;
            }
            gallery->cache.bake_active = 1;
            gallery->cache.bake_base = 1;
            gallery->cache.bake_band = 1;
            if (gallery->cache.bake_band >= band_count) {
                gallery->cache.base_valid = 1;
                gallery->cache.bake_active = 0;
                gallery->cache.bake_base = 0;
            }
            return 0;
        }
        band = gallery->cache.bake_band;
        if (depth_gallery_bake_base(display, gallery->depth, layers, NULL, band) != 0) {
            gallery->cache.bake_active = 0;
            gallery->cache.bake_base = 0;
            return -1;
        }
        ++gallery->cache.bake_band;
        if (gallery->cache.bake_band >= band_count) {
            gallery->cache.base_valid = 1;
            gallery->cache.bake_active = 0;
            gallery->cache.bake_base = 0;
        }
        return 0;
    }
    {
        int cell = -1;
        int slot = 1;
        int band;
        int patch_x;
        int patch_y;
        int patch_w;
        int patch_h;
        int src_x;
        int src_w;
        int band_count;
        if (gallery->cache.bake_active && !gallery->cache.bake_base) {
            cell = gallery->cache.bake_cell;
            slot = gallery->cache.bake_slot;
            band = gallery->cache.bake_band;
        } else {
            int cur_cell;
            int cur_slot;
            int offset;
            gallery_selected_slot(gallery, &cur_cell, &cur_slot);
            for (offset = 0; offset < 9 && cell < 0; ++offset) {
                int candidate = (cur_cell + offset) % 9;
                if (candidate == 0 && gallery->page == 0) {
                    int first_slot = cur_cell == 0 ? cur_slot : 1;
                    int second_slot = 1 - first_slot;
                    if (!gallery->cache.valid[0][first_slot]) {
                        cell = 0;
                        slot = first_slot;
                    } else if (!gallery->cache.valid[0][second_slot]) {
                        cell = 0;
                        slot = second_slot;
                    }
                } else if (!gallery->cache.valid[candidate][1]) {
                    cell = candidate;
                    slot = 1;
                }
            }
            if (cell < 0)
                return 0;
            band = 0;
        }
        gallery_cell_patch_rect(display, cell, &patch_x, &patch_y,
                                &patch_w, &patch_h, &src_x, &src_w);
        band_count = (patch_h + GALLERY_BAKE_BAND_HEIGHT - 1) /
                     GALLERY_BAKE_BAND_HEIGHT;
        if (band == 0) {
            int layers_sel = gallery_layers_selected_cell(gallery);
            int special = gallery->page == 0 && cell == 0;
            int target_cursor = special ? slot : 2;
            int need_unselect_other = layers_sel >= 0 &&
                                      layers_sel / 3 == cell / 3 &&
                                      layers_sel != cell;
            int need_select_cell = layers_sel != cell ||
                                   (special && gallery->cache.layers_cursor != slot);
            int result;
            if (need_unselect_other) {
                int other_special = gallery->page == 0 && layers_sel == 0;
                gallery_view_draw_cell(display, font, layers, layers_sel, 0,
                                       gallery_cell_item(gallery, layers_sel),
                                       other_special, 2);
            }
            if (need_select_cell) {
                gallery_view_draw_cell(display, font, layers, cell, 1,
                                       gallery_cell_item(gallery, cell),
                                       special, target_cursor);
            }
            if ((need_unselect_other || need_select_cell) && cell / 3 == 2)
                gallery_update_footer(display, font, gallery, layers);
            result = gallery_prepare_bake_pixels(display, gallery,
                                                 src_x, patch_y, src_w, patch_h, pixels);
            if (result == 0)
                result = depth_gallery_bake_patch(display, gallery->depth, layers, pixels,
                                                  cell, slot,
                                                  src_x, patch_y, src_w, patch_h,
                                                  patch_x, patch_y, patch_w, patch_h, 0);
            if (need_select_cell && layers_sel != cell) {
                gallery_view_draw_cell(display, font, layers, cell, 0,
                                       gallery_cell_item(gallery, cell),
                                       special, 2);
            }
            if (need_unselect_other || (need_select_cell && layers_sel == cell)) {
                int sel_special = gallery->page == 0 && layers_sel == 0;
                gallery_view_draw_cell(display, font, layers, layers_sel, 1,
                                       gallery_cell_item(gallery, layers_sel),
                                       sel_special, gallery->cache.layers_cursor);
            }
            if ((need_unselect_other || need_select_cell) && cell / 3 == 2)
                gallery_update_footer(display, font, gallery, layers);
            if (result != 0) {
                gallery->cache.bake_active = 0;
                return -1;
            }
            gallery->cache.bake_active = 1;
            gallery->cache.bake_base = 0;
            gallery->cache.bake_cell = cell;
            gallery->cache.bake_slot = slot;
            gallery->cache.bake_band = 1;
            if (gallery->cache.bake_band >= band_count) {
                gallery->cache.valid[cell][slot] = 1;
                gallery->cache.bake_active = 0;
            }
            return 0;
        }
        if (depth_gallery_bake_patch(display, gallery->depth, layers, NULL,
                                     cell, slot,
                                     src_x, patch_y, src_w, patch_h,
                                     patch_x, patch_y, patch_w, patch_h, band) != 0) {
            gallery->cache.bake_active = 0;
            return -1;
        }
        ++gallery->cache.bake_band;
        if (gallery->cache.bake_band >= band_count) {
            gallery->cache.valid[cell][slot] = 1;
            gallery->cache.bake_active = 0;
        }
        return 0;
    }
}

double gallery_monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

int gallery_draw(Display *display, Font *font, Gallery *gallery)
{
    static int profile_frames;
    double profile_started = gallery_monotonic_seconds();
    double profile_cpu;
    double profile_pack_ms = 0.0;
    double profile_submit_ms = 0.0;
    size_t profile_pixels = 0;
    int profile_regions = 0;
    DepthFrame frame;
    int full;
    int cached = 0;
    int region_x0[9];
    int region_y0[9];
    int region_x1[9];
    int region_y1[9];
    int region_count = 0;
    int cell;
    if (!gallery->cache.layers || !gallery->cache.layers[0].buffer.pixels ||
        !gallery->cache.layers[1].buffer.pixels)
        return -1;
    if (depth_frame_begin(&frame, display, gallery->depth, gallery->cache.layers,
                          &gallery->cache.cache_state, 0,
                          gallery->cache.page != gallery->page) != 0)
        return -1;
    full = frame.full;
    if (full) {
        region_x0[0] = 0;
        region_y0[0] = 0;
        region_x1[0] = display->mode.hdisplay;
        region_y1[0] = display->mode.vdisplay;
        region_count = 1;
        gallery->cache.base_valid = 0;
        memset(gallery->cache.valid, 0, sizeof(gallery->cache.valid));
        gallery->cache.bake_active = 0;
        gallery->cache.bake_base = 0;
    }
    {
        GalleryItem *items[9] = {0};
        int selected[9];
        int content_dirty[9];
        int dirty[9] = {0};
        int bottom_row_dirty = 0;
        int pass;
        for (cell = 0; cell < 9; ++cell) {
            int special = gallery->page == 0 && cell == 0;
            GalleryCellState *state = &gallery->cache.cells[cell];
            size_t item_index;
            selected[cell] = special ? gallery->cursor < 2 :
                             gallery->cursor == cell + (gallery->page == 0 ? 1 : 0);
            if (gallery->page == 0 && cell > 0) {
                item_index = (size_t)cell - 1;
                if (item_index < gallery->count)
                    items[cell] = &gallery->items[item_index];
            } else if (gallery->page > 0) {
                item_index = page_start(gallery) + (size_t)cell;
                if (item_index < gallery->count)
                    items[cell] = &gallery->items[item_index];
            }
            content_dirty[cell] = full || state->item != items[cell] ||
                    state->thumbnail != (items[cell] ? items[cell]->thumbnail : NULL) ||
                    state->thumbnail_state != (items[cell] ? items[cell]->thumbnail_state : 0) ||
                    state->progress != (items[cell] ? items[cell]->progress : 0.0f);
            dirty[cell] = content_dirty[cell] || state->selected != selected[cell] ||
                    (special && selected[cell] && gallery->cache.layers_cursor != gallery->cursor);
            if (content_dirty[cell]) {
                gallery->cache.base_valid = 0;
                memset(gallery->cache.valid, 0, sizeof(gallery->cache.valid));
                gallery->cache.bake_active = 0;
                gallery->cache.bake_base = 0;
            }
        }
        cached = !full && gallery_cache_ready(gallery);
        if (!cached && gallery->cache.displayed_cached) {
            int previous_cell = gallery->cache.page == 0 ?
                                (gallery->cache.cursor < 2 ? 0 : gallery->cache.cursor - 1) :
                                gallery->cache.cursor;
            if (previous_cell >= 0 && previous_cell < 9)
                dirty[previous_cell] = 1;
        }
        for (pass = 0; pass < (cached ? 0 : 2); ++pass) {
            for (cell = 0; cell < 9; ++cell) {
                const int cell_width = display->mode.hdisplay / 3;
                const int grid_y = 4;
                const int cell_height = (display->mode.vdisplay - grid_y - 50) / 3;
                const int x = (cell % 3) * cell_width + (cell_width - THUMB_WIDTH) / 2;
                const int y = grid_y + (cell / 3) * cell_height +
                              (cell_height - THUMB_HEIGHT) / 2;
                GalleryCellState *state = &gallery->cache.cells[cell];
                if (!dirty[cell] || selected[cell] != pass)
                    continue;
                if (!content_dirty[cell] && state->selected != selected[cell] &&
                    items[cell] && items[cell]->thumbnail) {
                    gallery_view_move_cell(display, font, gallery->cache.layers,
                                           cell, selected[cell], items[cell]);
                } else if (gallery_update_cell(display, font, gallery, gallery->cache.layers,
                                               cell, selected[cell], items[cell]) != 0) {
                    return -1;
                }
                if (cell / 3 == 2)
                    bottom_row_dirty = 1;
                if (!full) {
                    region_x0[region_count] = x - 79;
                    region_y0[region_count] = y - 8;
                    region_x1[region_count] = x + THUMB_WIDTH + 79;
                    region_y1[region_count] = y + THUMB_HEIGHT + 8;
                    ++region_count;
                }
                state->item = items[cell];
                state->thumbnail = items[cell] ? items[cell]->thumbnail : NULL;
                state->thumbnail_state = items[cell] ? items[cell]->thumbnail_state : 0;
                state->progress = items[cell] ? items[cell]->progress : 0.0f;
                state->selected = selected[cell];
            }
        }
        if (!cached) {
            gallery->cache.layers_cursor = gallery->cursor;
            if ((full || bottom_row_dirty) &&
                gallery_update_footer(display, font, gallery, gallery->cache.layers) != 0)
                return -1;
        }
    }
    if (cached) {
        int selected_cell;
        int selected_slot;
        gallery_selected_slot(gallery, &selected_cell, &selected_slot);
        if (depth_gallery_compose(display, gallery->depth, selected_cell, selected_slot) != 0)
            return -1;
    } else if (region_count > 0) {
        gallery->cache.bake_active = 0;
        gallery->cache.bake_base = 0;
    }
    profile_cpu = gallery_monotonic_seconds();
    {
        int direct_upload = depth_gallery_direct_upload(display, gallery->depth);
        int pass;
        int region;
        for (pass = 0; pass < (direct_upload ? 2 : 1); ++pass) {
            for (region = 0; region < region_count; ++region) {
                const uint8_t *pixels[2] = {NULL, NULL};
                double stage_started;
                int x0 = region_x0[region] < 0 ? 0 : region_x0[region];
                int y0 = region_y0[region] < 0 ? 0 : region_y0[region];
                int x1 = region_x1[region] > display->mode.hdisplay ? display->mode.hdisplay : region_x1[region];
                int y1 = region_y1[region] > display->mode.vdisplay ? display->mode.vdisplay : region_y1[region];
                int source_x0 = x0 - 79 > 0 ? x0 - 79 : 0;
                int source_x1 = x1 + 79 < display->mode.hdisplay ? x1 + 79 : display->mode.hdisplay;
                if (x1 <= x0 || y1 <= y0)
                    continue;
                if (direct_upload && pass == 0) {
                    int layer;
                    for (layer = 0; layer < 2; ++layer)
                        depth_layer_finish_area(&gallery->cache.layers[layer],
                                                source_x0, y0, source_x1 - source_x0, y1 - y0,
                                                display->mode.hdisplay, display->mode.vdisplay);
                    stage_started = gallery_monotonic_seconds();
                    profile_pack_ms += (stage_started - profile_cpu) * 1000.0;
                    if (depth_gallery_upload(display, gallery->depth, gallery->cache.layers,
                                             source_x0, y0, source_x1 - source_x0, y1 - y0) != 0)
                        return -1;
                    profile_submit_ms += (gallery_monotonic_seconds() - stage_started) * 1000.0;
                    profile_cpu = gallery_monotonic_seconds();
                    continue;
                }
                if (!direct_upload &&
                    gallery_pack(display, gallery, gallery->cache.layers,
                                 source_x0, y0, source_x1 - source_x0, y1 - y0, pixels) != 0)
                    return -1;
                stage_started = gallery_monotonic_seconds();
                profile_pack_ms += (stage_started - profile_cpu) * 1000.0;
                if (depth_gallery_region(display, gallery->depth, gallery->cache.layers,
                                         pixels, source_x0, y0, source_x1 - source_x0, y1 - y0,
                                         x0, y0, x1 - x0, y1 - y0,
                                         direct_upload ? 0 : 1) != 0)
                    return -1;
                profile_submit_ms += (gallery_monotonic_seconds() - stage_started) * 1000.0;
                profile_cpu = gallery_monotonic_seconds();
                profile_pixels += (size_t)(x1 - x0) * (y1 - y0);
                ++profile_regions;
            }
        }
    }
    gallery->cache.page = gallery->page;
    gallery->cache.cursor = gallery->cursor;
    gallery->cache.displayed_cached = cached;
    {
        int result;
        frame.defer_cache_commit = 1;
        if (cached || profile_regions > 0) {
            result = depth_frame_present(&frame);
        } else {
            result = frame.failed ? -1 : 0;
            depth_frame_close(&frame);
        }
        if (profile_frames < 20) {
            double profile_finished = gallery_monotonic_seconds();
            fprintf(stderr,
                    "Gallery frame: draw+pack+submit %.1f (pack %.1f, submit %.1f), render+present %.1f, total %.1f ms, regions %d, pixels %zu\n",
                    (profile_cpu - profile_started) * 1000.0,
                    profile_pack_ms, profile_submit_ms,
                    (profile_finished - profile_cpu) * 1000.0,
                    (profile_finished - profile_started) * 1000.0,
                    profile_regions, profile_pixels);
            ++profile_frames;
        }
        if (result == 0 && gallery->allow_cache_bake && gallery_cache_pending(gallery))
            result = gallery_bake_next(display, font, gallery);
        gallery->allow_cache_bake = 0;
        if (result == 0) {
            frame.defer_cache_commit = 0;
            depth_frame_sync_cache(&frame);
        }
        return result;
    }
}

int gallery_thumbnail_path(GalleryItem *item, char *path, size_t size)
{
    return thumbnail_cache_path(media_thumbnail_path(item), path, size);
}

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} CacheSet;

static void cache_set_free(CacheSet *set)
{
    size_t i;
    for (i = 0; i < set->count; ++i)
        free(set->items[i]);
    free(set->items);
    memset(set, 0, sizeof(*set));
}

static int cache_set_add(CacheSet *set, const char *name)
{
    char **items;
    char *copy;
    if (set->count == set->capacity) {
        size_t capacity = set->capacity ? set->capacity * 2 : 64;
        items = realloc(set->items, capacity * sizeof(*items));
        if (!items)
            return -1;
        set->items = items;
        set->capacity = capacity;
    }
    copy = strdup(name);
    if (!copy)
        return -1;
    set->items[set->count++] = copy;
    return 0;
}

static int cache_set_add_hash(CacheSet *set, uint64_t hash, const char *extension)
{
    char name[32];
    snprintf(name, sizeof(name), "%016llx%s", (unsigned long long)hash, extension);
    return cache_set_add(set, name);
}

static int cache_name_compare(const void *left, const void *right)
{
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

static int cache_set_contains(const CacheSet *set, const char *name)
{
    char *const *match;
    if (!set->count)
        return 0;
    match = bsearch(&name, set->items, set->count,
                    sizeof(*set->items), cache_name_compare);
    return match != NULL;
}

static uint8_t *cache_read_contents(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    uint8_t *contents;
    long length;
    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    contents = malloc((size_t)length + 1);
    if (!contents || fread(contents, 1, (size_t)length, file) != (size_t)length) {
        free(contents);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return contents;
}

static int cache_set_add_player(CacheSet *set, const char *media_path, int is_video,
                                const uint8_t *calibration, size_t calibration_size)
{
    char json_path[PATH_MAX];
    uint64_t hash = 0;
    if (snprintf(json_path, sizeof(json_path), "%s.json", media_path) >=
            (int)sizeof(json_path))
        return -1;
    if (lkg_cache_media_hash(is_video ? LKG_CACHE_VIDEO : LKG_CACHE_IMAGE,
                             media_path, json_path, CALIBRATION_PATH,
                             calibration, calibration_size, &hash) != 0)
        return -1;
    if (is_video) {
        uint64_t plain_hash = lkg_cache_plain_video_hash(hash);
        if (cache_set_add_hash(set, hash, ".mp4") != 0 ||
            cache_set_add_hash(set, hash, ".mp4.meta") != 0 ||
            cache_set_add_hash(set, plain_hash, ".mp4") != 0)
            return -1;
        return cache_set_add_hash(set, plain_hash, ".mp4.meta");
    }
    if (cache_set_add_hash(set, hash, ".png") != 0)
        return -1;
    return cache_set_add_hash(set, hash, ".rgb");
}

static int cache_scan_media(CacheSet *set, const char *directory,
                            const uint8_t *calibration, size_t calibration_size)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    int result = 0;
    if (!dir)
        return -1;
    while ((entry = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        struct stat status;
        GalleryItem item;
        if (entry->d_name[0] == '.')
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >=
                (int)sizeof(path) || lstat(path, &status) != 0) {
            result = -1;
            break;
        }
        if (S_ISDIR(status.st_mode)) {
            if (strcmp(path, MEDIA_ROOT "/USB") == 0)
                continue;
            if (cache_scan_media(set, path, calibration, calibration_size) != 0) {
                result = -1;
                break;
            }
            continue;
        }
        if (!S_ISREG(status.st_mode) || !media_extension(entry->d_name))
            continue;
        memset(&item, 0, sizeof(item));
        item.path = path;
        {
            char cache_path[PATH_MAX];
            const char *name;
            if (gallery_thumbnail_path(&item, cache_path, sizeof(cache_path)) != 0) {
                result = -1;
                break;
            }
            name = strrchr(cache_path, '/');
            if (cache_set_add(set, name ? name + 1 : cache_path) != 0) {
                result = -1;
                break;
            }
        }
        if (cache_set_add_player(set, path, video_extension(entry->d_name),
                                 calibration, calibration_size) != 0)
            result = -1;
        if (result != 0)
            break;
    }
    closedir(dir);
    return result;
}

static int cache_process_active(pid_t pid)
{
    if (pid <= 0)
        return 0;
    return kill(pid, 0) == 0 || errno != ESRCH;
}

static int cache_temporary_pid(const char *name, pid_t *pid)
{
    const char *marker = strstr(name, ".tmp.");
    char *end;
    long value;
    if (!marker)
        return 0;
    value = strtol(marker + 5, &end, 10);
    if (end == marker + 5 || *end != '\0' || value <= 0 || value > INT_MAX)
        return 0;
    *pid = (pid_t)value;
    return 1;
}

static int cache_pid_file_active(const char *path)
{
    FILE *file = fopen(path, "r");
    int pid;
    int active;
    if (!file)
        return 1;
    active = fscanf(file, "%d", &pid) == 1 && cache_process_active(pid);
    fclose(file);
    return active;
}

static int cache_prune_directory(const char *directory, const char *extension,
                                 const CacheSet *valid)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    int removed = 0;
    if (!dir)
        return 0;
    while ((entry = readdir(dir)) != NULL) {
        const char *suffix = strrchr(entry->d_name, '.');
        char path[PATH_MAX];
        struct stat status;
        pid_t temporary_pid = 0;
        int temporary = cache_temporary_pid(entry->d_name, &temporary_pid);
        int pid_file = suffix && strcmp(suffix, ".pid") == 0;
        int matches = extension ? suffix && strcmp(suffix, extension) == 0 :
                        suffix && (strcmp(suffix, ".png") == 0 ||
                                   strcmp(suffix, ".rgb") == 0 ||
                                   strcmp(suffix, ".mp4") == 0 ||
                                   strcmp(suffix, ".meta") == 0);
        if ((!matches && !temporary && !pid_file) || cache_set_contains(valid, entry->d_name) ||
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >=
                (int)sizeof(path) || lstat(path, &status) != 0 || !S_ISREG(status.st_mode))
            continue;
        if ((temporary && cache_process_active(temporary_pid)) ||
            (pid_file && cache_pid_file_active(path)))
            continue;
        if (unlink(path) == 0)
            ++removed;
    }
    closedir(dir);
    return removed;
}

static int cache_prune(void)
{
    CacheSet valid;
    uint8_t *calibration;
    size_t calibration_size;
    int removed = 0;
    calibration = cache_read_contents(CALIBRATION_PATH, &calibration_size);
    if (!calibration)
        return -1;
    memset(&valid, 0, sizeof(valid));
    if (cache_scan_media(&valid, MEDIA_ROOT, calibration, calibration_size) != 0) {
        free(calibration);
        cache_set_free(&valid);
        return -1;
    }
    qsort(valid.items, valid.count, sizeof(*valid.items), cache_name_compare);
    removed = cache_prune_directory(PLAYER_CACHE, NULL, &valid);
    removed += cache_prune_directory(THUMBNAIL_ROOT, ".rgba", &valid);
    cache_set_free(&valid);
    if (gallery_usb_device()) {
        memset(&valid, 0, sizeof(valid));
        if (cache_scan_media(&valid, MEDIA_ROOT "/USB", calibration, calibration_size) != 0) {
            free(calibration);
            cache_set_free(&valid);
            return -1;
        }
        qsort(valid.items, valid.count, sizeof(*valid.items), cache_name_compare);
        removed += cache_prune_directory(PLAYER_CACHE "/usb", NULL, &valid);
        removed += cache_prune_directory(PLAYER_CACHE "/usb-thumbnails", ".rgba", &valid);
        cache_set_free(&valid);
    }
    free(calibration);
    return removed;
}

int gallery_cache_cleanup_run(void)
{
    int removed;
    (void)nice(10);
    removed = cache_prune();
    if (removed >= 0)
        dprintf(STDOUT_FILENO, "Cache cleanup removed %d orphaned files\n", removed);
    else
        dprintf(STDERR_FILENO, "Cache cleanup aborted without deleting files\n");
    return removed < 0 ? 1 : 0;
}

static pid_t cache_cleanup_start(void)
{
    pid_t pid = -1;
    char *arguments[] = { "lkg-menu", "--cache-cleanup", NULL };
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error != 0)
        return -1;
    error = posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
    if (error == 0)
        error = posix_spawn(&pid, "/proc/self/exe", &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    return error == 0 ? pid : -1;
}

static void cache_cleanup_reap(pid_t *pid)
{
    int status;
    pid_t result;
    if (*pid <= 0)
        return;
    result = waitpid(*pid, &status, WNOHANG);
    if (result == *pid || (result < 0 && errno != EINTR))
        *pid = -1;
}

static void cache_cleanup_stop(pid_t *pid)
{
    int status;
    if (*pid <= 0)
        return;
    kill(*pid, SIGTERM);
    while (waitpid(*pid, &status, 0) < 0 && errno == EINTR)
        ;
    *pid = -1;
}

int gallery_load_thumbnail(GalleryItem *item)
{
    char path[PATH_MAX];
    FILE *file;
    uint8_t *pixels;
    size_t count;
    if (gallery_thumbnail_path(item, path, sizeof(path)) != 0)
        return 0;
    file = fopen(path, "rb");
    if (!file)
        return 0;
    pixels = malloc(THUMB_BYTES);
    if (!pixels) {
        fclose(file);
        return 0;
    }
    count = fread(pixels, 1, THUMB_BYTES, file);
    fclose(file);
    if (count != THUMB_BYTES) {
        free(pixels);
        return 0;
    }
    for (count = 0; count < THUMB_BYTES; count += 4) {
        uint8_t red = pixels[count];
        pixels[count] = pixels[count + 2];
        pixels[count + 2] = red;
    }
    item->thumbnail = pixels;
    item->thumbnail_state = 1;
    item->progress = 1.0f;
    return 1;
}

static int quilt_json_int(const char *json, const char *key, int fallback)
{
    const char *position = strstr(json, key);
    if (!position)
        return fallback;
    position = strchr(position, ':');
    if (!position)
        return fallback;
    return atoi(position + 1);
}

int gallery_quilt_dimensions(GalleryItem *item, int *columns, int *rows,
                            double *duration, double *aspect)
{
    char path[PATH_MAX];
    char json[8192];
    FILE *file;
    size_t count;
    const char *position;
    *columns = 1;
    *rows = 1;
    *duration = 0.0;
    *aspect = 1440.0 / 2560.0;
    if (!media_thumbnail_path(item) ||
        snprintf(path, sizeof(path), "%s.json", media_thumbnail_path(item)) >= (int)sizeof(path))
        return -1;
    file = fopen(path, "rb");
    if (!file && errno == ENOENT) {
        *aspect = 0.0;
        return 0;
    }
    if (!file)
        return -1;
    count = fread(json, 1, sizeof(json) - 1, file);
    fclose(file);
    json[count] = '\0';
    *columns = quilt_json_int(json, "\"viewX\"", 1);
    *rows = quilt_json_int(json, "\"viewY\"", 1);
    position = strstr(json, "\"duration\"");
    if (position) {
        position = strchr(position, ':');
        if (position)
            *duration = strtod(position + 1, NULL);
    }
    position = strstr(json, "\"aspect\"");
    if (position) {
        position = strchr(position, ':');
        if (position)
            *aspect = strtod(position + 1, NULL);
    }
    if (*columns < 1 || *rows < 1 || *columns > 64 || *rows > 64 ||
        !isfinite(*aspect) || *aspect < 0.0)
        return -1;
    return 0;
}

void gallery_thumbnail_progress(char *text, double duration, float *progress)
{
    char *line = text;
    while (*line) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        if (strncmp(line, "out_time_us=", 12) == 0 || strncmp(line, "out_time_ms=", 12) == 0) {
            double seconds = strtod(line + 12, NULL) / 1000000.0;
            if (duration > 0.0) {
                *progress = (float)(seconds / duration);
                if (*progress > 0.95f)
                    *progress = 0.95f;
            }
        } else if (strcmp(line, "progress=end") == 0) {
            *progress = 0.98f;
        }
        if (!next)
            break;
        line = next;
    }
}

void gallery_worker_stop(ThumbWorker *worker)
{
    int status;
    if (worker->pid > 0) {
        kill(worker->pid, SIGKILL);
        waitpid(worker->pid, &status, 0);
        unlink(worker->temporary);
    }
    if (worker->fd >= 0)
        close(worker->fd);
    worker->pid = -1;
    worker->fd = -1;
    worker->item = NULL;
}

static int worker_item_visible(const Gallery *gallery, const GalleryItem *item)
{
    size_t index = (size_t)(item - gallery->items);
    size_t start = page_start(gallery);
    return index >= start && index < start + page_items(gallery);
}

int gallery_thumbnail_begin(ThumbWorker *worker, GalleryItem *item)
{
    char filter[512];
    int descriptors[2];
    int columns;
    int rows;
    int view;
    int view_x;
    int view_y;
    int flags;
    const char *media_path = media_thumbnail_path(item);
    int direct_image = media_path && thumbnail_image_supported(media_path);
    double aspect;
    if (!media_path || item->thumbnail || item->thumbnail_state < 0 || gallery_load_thumbnail(item))
        return 0;
    if (gallery_thumbnail_path(item, worker->cache_path, sizeof(worker->cache_path)) != 0 ||
        gallery_quilt_dimensions(item, &columns, &rows, &worker->duration, &aspect) != 0) {
        item->thumbnail_state = -1;
        item->progress = 1.0f;
        return 0;
    }
    view = (columns * rows - 1) / 2;
    view_x = view % columns;
    view_y = view / columns;
    snprintf(worker->temporary, sizeof(worker->temporary), "%s.tmp.%d",
             worker->cache_path, (int)getpid());
    if (aspect > 0.0) {
        snprintf(filter, sizeof(filter),
                 "crop=iw/%d:ih/%d:%d*iw/%d:ih-%d*ih/%d,"
                 "scale=trunc(ih*%.9f):ih,"
                 "scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d",
                 columns, rows, view_x, columns, view_y + 1, rows, aspect,
                 THUMB_WIDTH, THUMB_HEIGHT, THUMB_WIDTH, THUMB_HEIGHT);
    } else {
        snprintf(filter, sizeof(filter),
                 "scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d",
                 THUMB_WIDTH, THUMB_HEIGHT, THUMB_WIDTH, THUMB_HEIGHT);
    }
    if (pipe(descriptors) != 0)
        return 0;
    fcntl(descriptors[0], F_SETFD, FD_CLOEXEC);
    fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
    {
        posix_spawn_file_actions_t actions;
        int spawn_error = posix_spawn_file_actions_init(&actions);
        if (spawn_error != 0) {
            close(descriptors[0]);
            close(descriptors[1]);
            return 0;
        }
        if (direct_image) {
            char columns_arg[16];
            char rows_arg[16];
            char view_arg[16];
            char aspect_arg[32];
            char width_arg[16];
            char height_arg[16];
            char fd_arg[16];
            char *arguments[] = {
                "lkg-menu", "--thumb-worker", (char *)media_path, worker->temporary,
                columns_arg, rows_arg, view_arg, aspect_arg,
                width_arg, height_arg, fd_arg, NULL
            };
            snprintf(columns_arg, sizeof(columns_arg), "%d", columns);
            snprintf(rows_arg, sizeof(rows_arg), "%d", rows);
            snprintf(view_arg, sizeof(view_arg), "%d", view);
            snprintf(aspect_arg, sizeof(aspect_arg), "%.9f", aspect);
            snprintf(width_arg, sizeof(width_arg), "%d", THUMB_WIDTH);
            snprintf(height_arg, sizeof(height_arg), "%d", THUMB_HEIGHT);
            snprintf(fd_arg, sizeof(fd_arg), "%d", STDERR_FILENO + 1);
            spawn_error = posix_spawn_file_actions_adddup2(&actions, descriptors[1],
                                                           STDERR_FILENO + 1);
            if (spawn_error == 0)
                spawn_error = posix_spawn_file_actions_addclosefrom_np(&actions,
                                                                       STDERR_FILENO + 2);
            if (spawn_error == 0)
                spawn_error = posix_spawn(&worker->pid, "/proc/self/exe",
                                          &actions, NULL, arguments, environ);
        } else {
            char *arguments[] = {
                "ffmpeg", "-hide_banner", "-loglevel", "error",
                "-nostdin", "-y", "-i", (char *)media_path, "-map", "0:v:0",
                "-frames:v", "1", "-vf", filter, "-f", "rawvideo", "-pix_fmt", "rgba",
                "-progress", "pipe:1", "-nostats", worker->temporary, NULL
            };
            spawn_error = posix_spawn_file_actions_adddup2(&actions, descriptors[1],
                                                           STDOUT_FILENO);
            if (spawn_error == 0)
                spawn_error = posix_spawn_file_actions_addclosefrom_np(&actions,
                                                                       STDERR_FILENO + 1);
            if (spawn_error == 0)
                spawn_error = posix_spawn(&worker->pid, "/usr/bin/ffmpeg",
                                          &actions, NULL, arguments, environ);
        }
        posix_spawn_file_actions_destroy(&actions);
        close(descriptors[1]);
        if (spawn_error != 0) {
            close(descriptors[0]);
            worker->pid = -1;
            return 0;
        }
    }
    worker->fd = descriptors[0];
    flags = fcntl(worker->fd, F_GETFL, 0);
    fcntl(worker->fd, F_SETFL, flags | O_NONBLOCK);
    worker->item = item;
    if (direct_image)
        worker->duration = 1.0;
    worker->progress = 0.02f;
    worker->ticks = 0;
    item->progress = worker->progress;
    return 1;
}

static int worker_start(Display *display, Font *font, Gallery *gallery, ThumbWorker *worker)
{
    int changed = 0;
    while (worker->order_position < worker->order_count) {
        GalleryItem *item = &gallery->items[worker->order[worker->order_position]];
        int visible = worker_item_visible(gallery, item);
        int started = gallery_thumbnail_begin(worker, item);
        ++worker->order_position;
        if (started) {
            if (visible && gallery_draw(display, font, gallery) != 0)
                return -1;
            return 0;
        }
        changed |= visible;
    }
    if (changed)
        return gallery_draw(display, font, gallery);
    return 0;
}

static int worker_update(Display *display, Font *font, Gallery *gallery, ThumbWorker *worker)
{
    char input[2048];
    ssize_t count;
    int status;
    pid_t result;
    if (worker->pid < 0)
        return 0;
    count = read(worker->fd, input, sizeof(input) - 1);
    if (count > 0) {
        input[count] = '\0';
        gallery_thumbnail_progress(input, worker->duration, &worker->progress);
        worker->item->progress = worker->duration > 0.0 ? worker->progress : 0.5f;
    } else if (count == 0) {
        close(worker->fd);
        worker->fd = -1;
    }
    result = waitpid(worker->pid, &status, WNOHANG);
    if (result == worker->pid) {
        GalleryItem *item = worker->item;
        int visible = worker_item_visible(gallery, item);
        if (worker->fd >= 0)
            close(worker->fd);
        worker->fd = -1;
        worker->pid = -1;
        worker->item = NULL;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
            rename(worker->temporary, worker->cache_path) != 0 || !gallery_load_thumbnail(item)) {
            unlink(worker->temporary);
            item->thumbnail_state = -1;
            item->progress = 1.0f;
        }
        if (visible)
            return gallery_draw(display, font, gallery);
        return 0;
    }
    if (worker_item_visible(gallery, worker->item) && (worker->ticks++ % 4) == 0)
        return gallery_draw(display, font, gallery);
    return 0;
}

static size_t page_start_for(int page)
{
    return page == 0 ? 0 : FIRST_PAGE_ITEMS + (size_t)(page - 1) * PAGE_ITEMS;
}

static size_t page_items_for(const Gallery *gallery, int page)
{
    size_t start = page_start_for(page);
    size_t capacity = page == 0 ? FIRST_PAGE_ITEMS : PAGE_ITEMS;
    size_t remaining = start < gallery->count ? gallery->count - start : 0;
    return remaining < capacity ? remaining : capacity;
}

static void worker_queue_page(Gallery *gallery, ThumbWorker *worker, int page)
{
    size_t start;
    size_t end;
    size_t i;
    if (page < 0 || page >= page_count(gallery))
        return;
    start = page_start_for(page);
    end = start + page_items_for(gallery, page);
    for (i = start; i < end; ++i)
        worker->order[worker->order_count++] = i;
}

static int prepare_page(Display *display, Font *font, Gallery *gallery, ThumbWorker *worker)
{
    size_t i;
    int page;
    gallery_worker_stop(worker);
    for (i = 0; i < gallery->count; ++i) {
        int keep = 0;
        for (page = gallery->page - 1; page <= gallery->page + 1; ++page) {
            size_t start;
            size_t end;
            if (page < 0 || page >= page_count(gallery))
                continue;
            start = page_start_for(page);
            end = start + page_items_for(gallery, page);
            if (i >= start && i < end)
                keep = 1;
        }
        if (!keep) {
            free(gallery->items[i].thumbnail);
            gallery->items[i].thumbnail = NULL;
            gallery->items[i].thumbnail_state = 0;
            gallery->items[i].progress = 0.0f;
        }
    }
    worker->order_count = 0;
    worker->order_position = 0;
    worker_queue_page(gallery, worker, gallery->page);
    worker_queue_page(gallery, worker, gallery->page - 1);
    worker_queue_page(gallery, worker, gallery->page + 1);
    return gallery_draw(display, font, gallery);
}

static dev_t gallery_usb_device(void)
{
    struct stat root_status;
    struct stat usb_status;

    if (stat(MEDIA_ROOT, &root_status) != 0 ||
        stat(MEDIA_ROOT "/USB", &usb_status) != 0 ||
        !S_ISDIR(usb_status.st_mode) || usb_status.st_dev == root_status.st_dev)
        return 0;
    return usb_status.st_dev;
}

static int gallery_sync_usb(Display *display, Font *font, Gallery *gallery,
                            ThumbWorker *worker, dev_t *usb_device)
{
    dev_t current = gallery_usb_device();
    static const char usb_directory[] = MEDIA_ROOT "/USB";
    size_t usb_length = sizeof(usb_directory) - 1;
    int inside_usb = strncmp(gallery->directory, usb_directory, usb_length) == 0 &&
                     (gallery->directory[usb_length] == '\0' ||
                      gallery->directory[usb_length] == '/');

    if (current == *usb_device)
        return 0;
    *usb_device = current;
    if (strcmp(gallery->directory, MEDIA_ROOT) != 0 && (current || !inside_usb))
        return 0;
    gallery_worker_stop(worker);
    if (gallery_scan(gallery, MEDIA_ROOT) == 0) {
        if (prepare_page(display, font, gallery, worker) != 0)
            return -1;
        printf("USB media %s\n", current ? "mounted" : "removed");
    } else {
        *usb_device = (dev_t)-1;
    }
    return 0;
}

static int gallery_up(Gallery *gallery)
{
    char parent[PATH_MAX];
    char *slash;

    if (strcmp(gallery->directory, MEDIA_ROOT) == 0)
        return 1;
    if (strlen(gallery->directory) >= sizeof(parent))
        return -1;
    strcpy(parent, gallery->directory);
    slash = strrchr(parent, '/');
    if (!slash)
        return 1;
    *slash = '\0';
    return gallery_scan(gallery, parent);
}

int gallery_run(Display *display, Font *font, DepthEffect *depth,
                DepthLayer layers[2], int serial_fd, UsbKeyboard *keyboard,
                double image_duration, int button_reversed, int auto_play)
{
    Gallery gallery;
    SerialParser parser;
    ThumbWorker worker;
    pid_t cleanup_pid = -1;
    dev_t usb_device;
    int result = 0;
    memset(&gallery, 0, sizeof(gallery));
    gallery.depth = depth;
    gallery.cache.layers = layers;
    memset(&parser, 0, sizeof(parser));
    memset(&worker, 0, sizeof(worker));
    worker.pid = -1;
    worker.fd = -1;
    if ((mkdir("/home/lkg/data", 0755) != 0 && errno != EEXIST) ||
        (mkdir(PLAYER_CACHE, 0755) != 0 && errno != EEXIST) ||
        (mkdir(PLAYER_CACHE "/usb", 0755) != 0 && errno != EEXIST) ||
        (mkdir(THUMBNAIL_ROOT, 0755) != 0 && errno != EEXIST) ||
        (mkdir(PLAYER_CACHE "/usb-thumbnails", 0755) != 0 && errno != EEXIST))
        return -1;
    if (gallery_scan(&gallery, MEDIA_ROOT) != 0)
        return -1;
    usb_device = gallery_usb_device();
    if (prepare_page(display, font, &gallery, &worker) != 0) {
        gallery_destroy(&gallery);
        return -1;
    }
    if (auto_play && gallery.count) {
        result = gallery_play_media(display, font, &gallery, NULL, 1, image_duration,
                                    serial_fd, keyboard, &parser, button_reversed);
        if (result == 0 && prepare_page(display, font, &gallery, &worker) != 0)
            result = -1;
    }
    cleanup_pid = cache_cleanup_start();
    while (running && result == 0) {
        char command[16];
        int event;
        int timeout;
        cache_cleanup_reap(&cleanup_pid);
        if (gallery_sync_usb(display, font, &gallery, &worker, &usb_device) != 0) {
            result = -1;
            break;
        }
        if (worker.pid < 0 && worker_start(display, font, &gallery, &worker) != 0) {
            result = -1;
            break;
        }
        timeout = worker.pid > 0 || cleanup_pid > 0 || gallery_cache_pending(&gallery) ? 50 : 500;
        event = usb_keyboard_next_command(keyboard, command, sizeof(command));
        if (event > 0)
            usb_keyboard_prepare_command(command, button_reversed);
        if (event == 0)
            event = serial_parser_next_with_fd(serial_fd, &parser, command, sizeof(command),
                                               timeout, keyboard->fd);
        if (worker_update(display, font, &gallery, &worker) != 0) {
            result = -1;
            break;
        }
        if (event < 0)
            break;
        if (event == 0) {
            if (worker.pid <= 0 && gallery_cache_pending(&gallery)) {
                gallery.allow_cache_bake = 1;
                if (gallery_draw(display, font, &gallery) != 0) {
                    result = -1;
                    break;
                }
            }
            continue;
        }
        normalize_button_command(command, button_reversed);
        if (strcmp(command, "SHUT") == 0) {
            result = SERIAL_RESULT_POWER_OFF;
            break;
        }
        if (strcmp(command, "USBD") == 0) {
            result = SERIAL_RESULT_USB_DESKTOP;
            break;
        }
        if (strcmp(command, "LOOP") == 0) {
            gallery_worker_stop(&worker);
            if (gallery_up(&gallery) != 0)
                break;
            if (prepare_page(display, font, &gallery, &worker) != 0) {
                result = -1;
                break;
            }
        } else if (strcmp(command, "PREV") == 0) {
            if (gallery.cursor == last_cursor(&gallery)) {
                if (gallery.page + 1 < page_count(&gallery))
                    ++gallery.page;
                else
                    gallery.page = 0;
                gallery.cursor = 0;
                if (page_count(&gallery) > 1) {
                    if (prepare_page(display, font, &gallery, &worker) != 0) {
                        result = -1;
                        break;
                    }
                }
            } else {
                ++gallery.cursor;
            }
            if (gallery_draw(display, font, &gallery) != 0) {
                result = -1;
                break;
            }
        } else if (strcmp(command, "NEXT") == 0) {
            if (gallery.cursor == 0) {
                if (gallery.page > 0)
                    --gallery.page;
                else
                    gallery.page = page_count(&gallery) - 1;
                gallery.cursor = last_cursor(&gallery);
                if (page_count(&gallery) > 1) {
                    if (prepare_page(display, font, &gallery, &worker) != 0) {
                        result = -1;
                        break;
                    }
                }
            } else {
                --gallery.cursor;
            }
            if (gallery_draw(display, font, &gallery) != 0) {
                result = -1;
                break;
            }
        } else if (strcmp(command, "PLPA") == 0) {
            GalleryItem *item = selected_item(&gallery);
            if (gallery.page == 0 && gallery.cursor == 0) {
                gallery_worker_stop(&worker);
                if (gallery_up(&gallery) != 0)
                    break;
                if (prepare_page(display, font, &gallery, &worker) != 0) {
                    result = -1;
                    break;
                }
            } else if (gallery.page == 0 && gallery.cursor == 1) {
                gallery_worker_stop(&worker);
                cache_cleanup_stop(&cleanup_pid);
                result = gallery_play_media(display, font, &gallery, NULL, 1, image_duration,
                                            serial_fd, keyboard, &parser, button_reversed);
                if (result != 0)
                    break;
                if (prepare_page(display, font, &gallery, &worker) != 0) {
                    result = -1;
                    break;
                }
            } else if (item && item->is_directory) {
                char path[PATH_MAX];
                if (strlen(item->path) < sizeof(path)) {
                    gallery_worker_stop(&worker);
                    strcpy(path, item->path);
                    if (gallery_scan(&gallery, path) == 0) {
                        if (prepare_page(display, font, &gallery, &worker) != 0) {
                            result = -1;
                            break;
                        }
                    }
                }
            } else if (item) {
                gallery_worker_stop(&worker);
                cache_cleanup_stop(&cleanup_pid);
                result = gallery_play_media(display, font, &gallery, item, 0, image_duration,
                                            serial_fd, keyboard, &parser, button_reversed);
                if (result != 0)
                    break;
                if (prepare_page(display, font, &gallery, &worker) != 0) {
                    result = -1;
                    break;
                }
            }
        }
    }
    cache_cleanup_stop(&cleanup_pid);
    gallery_worker_stop(&worker);
    gallery_destroy(&gallery);
    depth_gallery_release_bake(display, depth);
    return result;
}
