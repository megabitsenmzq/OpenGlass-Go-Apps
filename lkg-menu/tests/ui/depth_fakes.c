#include "depth_fakes.h"

#include <stdlib.h>
#include <string.h>

DepthFake depth_fake;

void depth_fake_reset(void)
{
    memset(&depth_fake, 0, sizeof(depth_fake));
    depth_fake.direct_upload = 1;
    depth_fake.revision = 1;
}

int depth_cache_needs_full(const DepthEffect *effect, const DepthCacheState *state,
                           uint64_t content_key)
{
    (void)effect;
    return depth_fake.force_full || !state->initialized ||
           state->content_key != content_key || state->revision != depth_fake.revision;
}

void depth_cache_commit(const DepthEffect *effect, DepthCacheState *state,
                        uint64_t content_key)
{
    (void)effect;
    state->revision = depth_fake.revision;
    state->content_key = content_key;
    state->initialized = 1;
    ++depth_fake.commit_calls;
    depth_fake.committed_key = content_key;
}

int depth_layer_create(Display *display, DepthLayer *layer, float disparity)
{
    memset(layer, 0, sizeof(*layer));
    layer->disparity = disparity;
    layer->buffer.layered = 1;
    layer->buffer.stride = display->mode.hdisplay * 4;
    layer->buffer.size = (size_t)layer->buffer.stride * display->mode.vdisplay;
    layer->buffer.pixels = calloc(1, layer->buffer.size);
    return layer->buffer.pixels ? 0 : -1;
}

void depth_layer_destroy(DepthLayer *layer)
{
    free(layer->buffer.pixels);
    memset(layer, 0, sizeof(*layer));
}

void depth_layer_finish_area(DepthLayer *layer, int x, int y, int width, int height,
                             int screen_width, int screen_height)
{
    (void)layer;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)screen_width;
    (void)screen_height;
    ++depth_fake.finish_calls;
}

int depth_gallery_direct_upload(Display *display, const DepthEffect *effect)
{
    (void)display;
    (void)effect;
    return depth_fake.direct_upload;
}

int depth_gallery_begin(Display *display, const DepthEffect *effect)
{
    (void)display;
    (void)effect;
    ++depth_fake.begin_calls;
    return depth_fake.fail_begin ? -1 : 0;
}

int depth_gallery_upload(Display *display, const DepthEffect *effect,
                         const DepthLayer *layers, int x, int y, int width, int height)
{
    (void)display;
    (void)effect;
    (void)layers;
    ++depth_fake.upload_calls;
    depth_fake.last_x = x;
    depth_fake.last_y = y;
    depth_fake.last_width = width;
    depth_fake.last_height = height;
    return depth_fake.fail_update ? -1 : 0;
}

int depth_gallery_anim_region(Display *display, const DepthEffect *effect,
                              const DepthLayer *layers,
                              const DepthAnimRect *first, const DepthAnimRect *second,
                              int clip_x, int clip_y, int clip_width, int clip_height,
                              int bar_top, int bar_bottom, int has_back)
{
    (void)display;
    (void)effect;
    (void)layers;
    ++depth_fake.anim_region_calls;
    depth_fake.last_clip_x = clip_x;
    depth_fake.last_clip_y = clip_y;
    depth_fake.last_clip_width = clip_width;
    depth_fake.last_clip_height = clip_height;
    depth_fake.last_bar_top = bar_top;
    depth_fake.last_bar_bottom = bar_bottom;
    depth_fake.last_has_back = has_back;
    depth_fake.last_first_disparity = first ? first->disparity : 0.0f;
    depth_fake.last_second_disparity = second ? second->disparity : 0.0f;
    return depth_fake.fail_update ? -1 : 0;
}

int depth_gallery_anim_scroll_region(Display *display, const DepthEffect *effect,
                                     const DepthLayer *layers,
                                     const DepthAnimRect *first, const DepthAnimRect *second,
                                     int clip_x, int clip_y, int clip_width, int clip_height,
                                     int bar_top, int bar_bottom, int has_back,
                                     float scroll_y, int scroll_limit_y, int source_max_y)
{
    (void)scroll_y;
    (void)scroll_limit_y;
    (void)source_max_y;
    ++depth_fake.anim_scroll_region_calls;
    return depth_gallery_anim_region(display, effect, layers, first, second,
                                     clip_x, clip_y, clip_width, clip_height,
                                     bar_top, bar_bottom, has_back);
}

int depth_gallery_anim_clear(Display *display, const DepthEffect *effect,
                             int clip_x, int clip_y, int clip_width, int clip_height,
                             int bar_top, int bar_bottom)
{
    (void)display;
    (void)effect;
    ++depth_fake.anim_clear_calls;
    depth_fake.last_clip_x = clip_x;
    depth_fake.last_clip_y = clip_y;
    depth_fake.last_clip_width = clip_x >= 0 ? clip_width : 0;
    depth_fake.last_clip_height = clip_height;
    depth_fake.last_bar_top = bar_top;
    depth_fake.last_bar_bottom = bar_bottom;
    return depth_fake.fail_update ? -1 : 0;
}

int depth_cached_region(Display *display, const DepthEffect *effect,
                        const DepthLayer *layers, int x, int y, int width, int height,
                        int bar_top, int bar_bottom)
{
    (void)display;
    (void)effect;
    (void)layers;
    (void)bar_top;
    (void)bar_bottom;
    ++depth_fake.cached_calls;
    depth_fake.last_x = x;
    depth_fake.last_y = y;
    depth_fake.last_width = width;
    depth_fake.last_height = height;
    return depth_fake.fail_update ? -1 : 0;
}

int depth_gallery_region(Display *display, const DepthEffect *effect,
                         const DepthLayer *layers, const uint8_t *const pixels[2],
                         int x, int y, int width, int height,
                         int clip_x, int clip_y, int clip_width, int clip_height, int upload)
{
    (void)display;
    (void)effect;
    (void)layers;
    (void)clip_x;
    (void)clip_y;
    (void)clip_width;
    (void)clip_height;
    (void)upload;
    ++depth_fake.region_calls;
    depth_fake.last_x = x;
    depth_fake.last_y = y;
    depth_fake.last_width = width;
    depth_fake.last_height = height;
    memcpy(&depth_fake.packed_first[0], pixels[0], sizeof(uint32_t));
    memcpy(&depth_fake.packed_first[1], pixels[1], sizeof(uint32_t));
    return depth_fake.fail_update ? -1 : 0;
}

int depth_gallery_present(Display *display, const DepthEffect *effect)
{
    (void)display;
    (void)effect;
    ++depth_fake.present_calls;
    return depth_fake.fail_present ? -1 : 0;
}
