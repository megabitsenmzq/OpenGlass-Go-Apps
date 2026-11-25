#include "depth.h"
#include "depth_gpu.h"
#include "lkg_app_protocol.h"
#include "menu_model.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_text_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    char *text;
    long length;
    size_t count;
    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    text = malloc((size_t)length + 1);
    if (!text) {
        fclose(file);
        return NULL;
    }
    count = fread(text, 1, (size_t)length, file);
    fclose(file);
    if (count != (size_t)length) {
        free(text);
        return NULL;
    }
    text[length] = '\0';
    return text;
}

static int calibration_value(const char *json, const char *key, double *value)
{
    char needle[64];
    const char *section;
    const char *number;
    char *end;
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) >= (int)sizeof(needle))
        return 0;
    section = strstr(json, needle);
    if (!section)
        return 0;
    number = strstr(section + strlen(needle), "\"value\"");
    if (!number)
        return 0;
    number = strchr(number + 7, ':');
    if (!number)
        return 0;
    errno = 0;
    *value = strtod(number + 1, &end);
    return end != number + 1 && errno != ERANGE && isfinite(*value);
}

int depth_cache_needs_full(const DepthEffect *effect, const DepthCacheState *state,
                           uint64_t content_key)
{
    return depth_cache_needs_full_revision(state, content_key,
                                           effect->gpu ? depth_gpu_cache_revision(effect->gpu) : 0,
                                           effect->gpu != NULL);
}

void depth_cache_commit(const DepthEffect *effect, DepthCacheState *state,
                        uint64_t content_key)
{
    depth_cache_commit_revision(state, content_key,
                                effect->gpu ? depth_gpu_cache_revision(effect->gpu) : 0,
                                effect->gpu != NULL);
}

int depth_effect_open(DepthEffect *effect, Display *display, const char *calibration_path)
{
    char *json;
    double value;
    memset(effect, 0, sizeof(*effect));
    effect->screen_w = 1440;
    effect->screen_h = 2560;
    effect->dpi = 491;
    effect->inv_view = 1;
    effect->front_disparity = LKG_DEPTH_SUBTLE_POP_OUT;
    effect->back_disparity = LKG_DEPTH_SUBTLE_RECESSED;
    json = read_text_file(calibration_path);
    if (!json)
        return -1;
    if (!calibration_value(json, "pitch", &value) || value <= 0)
        goto fail;
    effect->pitch = (float)value;
    if (!calibration_value(json, "slope", &value) || value == 0)
        goto fail;
    effect->slope = (float)value;
    if (!calibration_value(json, "center", &value))
        goto fail;
    effect->center = (float)value;
    if (calibration_value(json, "fringe", &value))
        effect->fringe = (float)value;
    if (calibration_value(json, "DPI", &value)) {
        if (value <= 0)
            goto fail;
        effect->dpi = (int)value;
    }
    if (calibration_value(json, "screenW", &value) && value > 0)
        effect->screen_w = (int)value;
    if (calibration_value(json, "screenH", &value) && value > 0)
        effect->screen_h = (int)value;
    if (calibration_value(json, "invView", &value))
        effect->inv_view = value != 0;
    if (calibration_value(json, "flipImageX", &value))
        effect->flip_image_x = value != 0;
    if (calibration_value(json, "flipImageY", &value))
        effect->flip_image_y = value != 0;
    if (calibration_value(json, "flipSubp", &value))
        effect->flip_subp = value != 0;
    free(json);
    effect->gpu = depth_gpu_create(display);
    return effect->gpu ? 0 : -1;
fail:
    free(json);
    return -1;
}

void depth_effect_apply_mode(DepthEffect *effect, DepthLayer layers[2], int menu_3d)
{
    if (!effect)
        return;
    if (menu_3d == MENU_3D_OFF) {
        effect->stereo = 0;
        effect->front_disparity = LKG_DEPTH_SCREEN;
        effect->back_disparity = LKG_DEPTH_SCREEN;
    } else if (menu_3d == MENU_3D_DEEP) {
        effect->stereo = 1;
        effect->front_disparity = LKG_DEPTH_DEEP_POP_OUT;
        effect->back_disparity = LKG_DEPTH_DEEP_RECESSED;
    } else {
        effect->stereo = 1;
        effect->front_disparity = LKG_DEPTH_SUBTLE_POP_OUT;
        effect->back_disparity = LKG_DEPTH_SUBTLE_RECESSED;
    }
    if (layers) {
        layers[0].disparity = effect->front_disparity;
        layers[1].disparity = effect->back_disparity;
    }
}

void depth_effect_close(DepthEffect *effect)
{
    depth_gpu_destroy(effect->gpu);
    effect->gpu = NULL;
}

int depth_layer_create(Display *display, DepthLayer *layer, float disparity)
{
    size_t size;
    memset(layer, 0, sizeof(*layer));
    layer->disparity = disparity;
    layer->buffer.layered = 1;
    layer->buffer.stride = display->mode.hdisplay * 4;
    size = (size_t)layer->buffer.stride * display->mode.vdisplay;
    layer->buffer.pixels = calloc(1, size);
    if (!layer->buffer.pixels)
        return -1;
    layer->buffer.size = size;
    return 0;
}

void depth_layer_destroy(DepthLayer *layer)
{
    free(layer->buffer.pixels);
    memset(layer, 0, sizeof(*layer));
}

void depth_layer_finish(DepthLayer *layer)
{
    uint32_t *pixels = (uint32_t *)layer->buffer.pixels;
    size_t i;
    for (i = 0; i < layer->buffer.size / 4; ++i) {
        if (pixels[i] && !(pixels[i] & 0xff000000u))
            pixels[i] |= 0xff000000u;
    }
}

void depth_layer_finish_area(DepthLayer *layer, int x, int y, int width, int height,
                             int screen_width, int screen_height)
{
    int row;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x + width > screen_width)
        width = screen_width - x;
    if (y + height > screen_height)
        height = screen_height - y;
    for (row = y; row < y + height; ++row) {
        uint32_t *pixels = (uint32_t *)(layer->buffer.pixels +
                                        (size_t)row * layer->buffer.stride) + x;
        int column;
        for (column = 0; column < width; ++column) {
            if (pixels[column] && !(pixels[column] & 0xff000000u))
                pixels[column] |= 0xff000000u;
        }
    }
}

int depth_gallery_begin(Display *display, const DepthEffect *effect)
{
    return effect->gpu ? depth_gpu_gallery_begin(effect->gpu, display) : -1;
}

int depth_gallery_direct_upload(Display *display, const DepthEffect *effect)
{
    (void)display;
    return effect->gpu ? depth_gpu_direct_upload(effect->gpu) : 0;
}

int depth_gallery_upload(Display *display, const DepthEffect *effect,
                         const DepthLayer *layers, int x, int y, int width, int height)
{
    return effect->gpu ? depth_gpu_gallery_upload(effect->gpu, display, layers,
                                                  x, y, width, height) : -1;
}

int depth_cached_region(Display *display, const DepthEffect *effect,
                        const DepthLayer *layers, int x, int y, int width, int height,
                        int bar_top, int bar_bottom)
{
    return effect->gpu ? depth_gpu_cached_region(effect->gpu, display, effect, layers,
                                                 x, y, width, height,
                                                 bar_top, bar_bottom) : -1;
}

int depth_gallery_region(Display *display, const DepthEffect *effect,
                         const DepthLayer *layers, const uint8_t *const pixels[2],
                         int x, int y, int width, int height,
                         int clip_x, int clip_y, int clip_width, int clip_height, int upload)
{
    return effect->gpu ? depth_gpu_gallery_region(effect->gpu, display, effect, layers,
                                                  pixels, x, y, width, height,
                                                  clip_x, clip_y, clip_width, clip_height,
                                                  upload) : -1;
}

int depth_gallery_anim_region(Display *display, const DepthEffect *effect,
                              const DepthLayer *layers,
                              const DepthAnimRect *first, const DepthAnimRect *second,
                              int clip_x, int clip_y, int clip_width, int clip_height,
                              int bar_top, int bar_bottom, int has_back)
{
    return effect->gpu ? depth_gpu_gallery_anim_region(effect->gpu, display, effect, layers,
                                                       first, second,
                                                       clip_x, clip_y, clip_width, clip_height,
                                                       bar_top, bar_bottom, has_back)
                       : -1;
}

int depth_gallery_anim_scroll_region(Display *display, const DepthEffect *effect,
                                     const DepthLayer *layers,
                                     const DepthAnimRect *first, const DepthAnimRect *second,
                                     int clip_x, int clip_y, int clip_width, int clip_height,
                                     int bar_top, int bar_bottom, int has_back,
                                     float scroll_y, int scroll_limit_y, int source_max_y)
{
    return effect->gpu ? depth_gpu_gallery_anim_scroll_region(
                             effect->gpu, display, effect, layers,
                             first, second,
                             clip_x, clip_y, clip_width, clip_height,
                             bar_top, bar_bottom, has_back,
                             scroll_y, scroll_limit_y, source_max_y)
                       : -1;
}

int depth_gallery_anim_clear(Display *display, const DepthEffect *effect,
                             int clip_x, int clip_y, int clip_width, int clip_height,
                             int bar_top, int bar_bottom)
{
    return (effect && effect->gpu)
               ? depth_gpu_gallery_anim_clear(effect->gpu, display,
                                              clip_x, clip_y, clip_width, clip_height,
                                              bar_top, bar_bottom)
               : -1;
}

int depth_gallery_bake_base(Display *display, const DepthEffect *effect,
                            const DepthLayer *layers, const uint8_t *const pixels[2],
                            int band)
{
    return effect->gpu ? depth_gpu_gallery_bake_base(effect->gpu, display, effect, layers,
                                                     pixels, band) : -1;
}

int depth_gallery_bake_patch(Display *display, const DepthEffect *effect,
                             const DepthLayer *layers, const uint8_t *const pixels[2],
                             int cell, int slot,
                             int src_x, int src_y, int src_width, int src_height,
                             int patch_x, int patch_y, int patch_width, int patch_height,
                             int band)
{
    return effect->gpu ? depth_gpu_gallery_bake_patch(effect->gpu, display, effect, layers,
                                                      pixels, cell, slot,
                                                      src_x, src_y, src_width, src_height,
                                                      patch_x, patch_y, patch_width, patch_height,
                                                      band) : -1;
}

int depth_gallery_compose(Display *display, const DepthEffect *effect,
                          int selected_cell, int slot)
{
    return effect->gpu ? depth_gpu_gallery_compose(effect->gpu, display,
                                                   selected_cell, slot) : -1;
}

void depth_gallery_release_bake(Display *display, const DepthEffect *effect)
{
    (void)display;
    if (effect && effect->gpu)
        depth_gpu_gallery_release_bake(effect->gpu);
}

int depth_gallery_present(Display *display, const DepthEffect *effect)
{
    return effect->gpu ? depth_gpu_gallery_present(effect->gpu, display) : -1;
}
