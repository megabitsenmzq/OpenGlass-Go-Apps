#ifndef LKG_MENU_DEPTH_H
#define LKG_MENU_DEPTH_H

#include "ui.h"
#include "depth_cache.h"

#define GALLERY_BAKE_BAND_HEIGHT 256

typedef struct {
    float pitch;
    float slope;
    float center;
    float fringe;
    int dpi;
    int screen_w;
    int screen_h;
    int inv_view;
    int flip_image_x;
    int flip_image_y;
    int flip_subp;
    int stereo;
    float front_disparity;
    float back_disparity;
    void *gpu;
} DepthEffect;

typedef struct {
    Buffer buffer;
    float disparity;
} DepthLayer;

typedef struct {
    int x0;
    int y0;
    int x1;
    int y1;
    float disparity;
} DepthAnimRect;

int depth_cache_needs_full(const DepthEffect *effect, const DepthCacheState *state,
                           uint64_t content_key);
void depth_cache_commit(const DepthEffect *effect, DepthCacheState *state,
                        uint64_t content_key);
int depth_effect_open(DepthEffect *effect, Display *display, const char *calibration_path);
void depth_effect_apply_mode(DepthEffect *effect, DepthLayer layers[2], int menu_3d);
void depth_effect_close(DepthEffect *effect);
int depth_layer_create(Display *display, DepthLayer *layer, float disparity);
void depth_layer_destroy(DepthLayer *layer);
void depth_layer_finish(DepthLayer *layer);
void depth_layer_finish_area(DepthLayer *layer, int x, int y, int width, int height,
                             int screen_width, int screen_height);
int depth_gallery_begin(Display *display, const DepthEffect *effect);
int depth_gallery_direct_upload(Display *display, const DepthEffect *effect);
int depth_gallery_upload(Display *display, const DepthEffect *effect,
                         const DepthLayer *layers, int x, int y, int width, int height);
int depth_cached_region(Display *display, const DepthEffect *effect,
                        const DepthLayer *layers, int x, int y, int width, int height,
                        int bar_top, int bar_bottom);
int depth_gallery_region(Display *display, const DepthEffect *effect,
                         const DepthLayer *layers, const uint8_t *const pixels[2],
                         int x, int y, int width, int height,
                         int clip_x, int clip_y, int clip_width, int clip_height, int upload);
int depth_gallery_anim_region(Display *display, const DepthEffect *effect,
                              const DepthLayer *layers,
                              const DepthAnimRect *first, const DepthAnimRect *second,
                              int clip_x, int clip_y, int clip_width, int clip_height,
                              int bar_top, int bar_bottom, int has_back);
int depth_gallery_anim_scroll_region(Display *display, const DepthEffect *effect,
                                     const DepthLayer *layers,
                                     const DepthAnimRect *first, const DepthAnimRect *second,
                                     int clip_x, int clip_y, int clip_width, int clip_height,
                                     int bar_top, int bar_bottom, int has_back,
                                     float scroll_y, int scroll_limit_y, int source_max_y);
int depth_gallery_anim_clear(Display *display, const DepthEffect *effect,
                             int clip_x, int clip_y, int clip_width, int clip_height,
                             int bar_top, int bar_bottom);
int depth_gallery_bake_base(Display *display, const DepthEffect *effect,
                            const DepthLayer *layers, const uint8_t *const pixels[2],
                            int band);
int depth_gallery_bake_patch(Display *display, const DepthEffect *effect,
                             const DepthLayer *layers, const uint8_t *const pixels[2],
                             int cell, int slot,
                             int src_x, int src_y, int src_width, int src_height,
                             int patch_x, int patch_y, int patch_width, int patch_height,
                             int band);
int depth_gallery_compose(Display *display, const DepthEffect *effect,
                          int selected_cell, int slot);
void depth_gallery_release_bake(Display *display, const DepthEffect *effect);
int depth_gallery_present(Display *display, const DepthEffect *effect);

#endif
