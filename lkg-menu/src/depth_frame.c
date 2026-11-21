#include "depth_frame.h"

#include <stdlib.h>
#include <string.h>

void depth_frame_close(DepthFrame *frame)
{
    free(frame->packed[0]);
    free(frame->packed[1]);
    frame->packed[0] = NULL;
    frame->packed[1] = NULL;
    frame->packed_size = 0;
}

int depth_frame_begin(DepthFrame *frame, Display *display, const DepthEffect *effect,
                      DepthLayer layers[2], DepthCacheState *cache_state,
                      uint64_t content_key, int force_full)
{
    memset(frame, 0, sizeof(*frame));
    frame->display = display;
    frame->effect = effect;
    frame->layers = layers;
    frame->cache_state = cache_state;
    frame->content_key = content_key;
    frame->full = force_full ||
                  (cache_state && depth_cache_needs_full(effect, cache_state, content_key));
    frame->direct_upload = depth_gallery_direct_upload(display, effect);
    if (frame->full && depth_gallery_begin(display, effect) != 0)
        frame->failed = 1;
    return frame->failed ? -1 : 0;
}

static int frame_pack(DepthFrame *frame, int x, int y, int width, int height)
{
    size_t needed = (size_t)width * height * 4;
    int layer;
    if (needed > frame->packed_size) {
        uint8_t *packed[2];
        packed[0] = malloc(needed);
        packed[1] = malloc(needed);
        if (!packed[0] || !packed[1]) {
            free(packed[0]);
            free(packed[1]);
            return -1;
        }
        free(frame->packed[0]);
        free(frame->packed[1]);
        frame->packed[0] = packed[0];
        frame->packed[1] = packed[1];
        frame->packed_size = needed;
    }
    for (layer = 0; layer < 2; ++layer) {
        int row;
        for (row = 0; row < height; ++row) {
            memcpy(frame->packed[layer] + (size_t)row * width * 4,
                   frame->layers[layer].buffer.pixels + (size_t)(y + row) *
                   frame->layers[layer].buffer.stride + (size_t)x * 4,
                   (size_t)width * 4);
        }
    }
    return 0;
}

int depth_frame_update_rect(DepthFrame *frame, int x, int y, int width, int height,
                            int bar_top, int bar_bottom)
{
    int result;
    if (frame->failed)
        return -1;
    if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
        x + width > (int)frame->display->mode.hdisplay ||
        y + height > (int)frame->display->mode.vdisplay) {
        frame->failed = 1;
        return -1;
    }
    {
        float first = frame->layers[0].disparity;
        float second = frame->layers[1].disparity;
        float maximum;
        int margin;
        int right;
        if (first < 0)
            first = -first;
        if (second < 0)
            second = -second;
        maximum = first > second ? first : second;
        margin = (int)maximum;
        if ((float)margin < maximum)
            ++margin;
        right = x + width + margin;
        if (right > (int)frame->display->mode.hdisplay)
            right = frame->display->mode.hdisplay;
        x -= margin;
        if (x < 0)
            x = 0;
        width = right - x;
    }
    depth_layer_finish_area(&frame->layers[0], x, y, width, height,
                            frame->display->mode.hdisplay, frame->display->mode.vdisplay);
    depth_layer_finish_area(&frame->layers[1], x, y, width, height,
                            frame->display->mode.hdisplay, frame->display->mode.vdisplay);
    if (frame->direct_upload) {
        result = depth_cached_region(frame->display, frame->effect, frame->layers,
                                     x, y, width, height, bar_top, bar_bottom);
    } else {
        if (frame_pack(frame, x, y, width, height) != 0) {
            frame->failed = 1;
            return -1;
        }
        result = depth_gallery_region(frame->display, frame->effect, frame->layers,
                                      (const uint8_t *const *)frame->packed,
                                      x, y, width, height, x, y, width, height, 1);
    }
    if (result != 0) {
        frame->failed = 1;
        return -1;
    }
    ++frame->updates;
    return 0;
}

void depth_frame_sync_cache(DepthFrame *frame)
{
    if (!frame->failed && frame->cache_state && !frame->defer_cache_commit)
        depth_cache_commit(frame->effect, frame->cache_state, frame->content_key);
}

int depth_frame_present(DepthFrame *frame)
{
    int result = 0;
    if (!frame->failed && depth_gallery_present(frame->display, frame->effect) != 0) {
        frame->failed = 1;
        result = -1;
    }
    depth_frame_sync_cache(frame);
    if (frame->failed)
        result = -1;
    depth_frame_close(frame);
    return result;
}
