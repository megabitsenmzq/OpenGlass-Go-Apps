#ifndef LKG_MENU_DEPTH_GPU_H
#define LKG_MENU_DEPTH_GPU_H

#include "depth.h"

typedef struct DepthGpu DepthGpu;

DepthGpu *depth_gpu_create(Display *display);
void depth_gpu_destroy(DepthGpu *gpu);
uint64_t depth_gpu_cache_revision(const DepthGpu *gpu);
int depth_gpu_gallery_begin(DepthGpu *gpu, Display *display);
int depth_gpu_direct_upload(DepthGpu *gpu);
int depth_gpu_gallery_upload(DepthGpu *gpu, Display *display, const DepthLayer *layers,
                             int x, int y, int width, int height);
int depth_gpu_cached_region(DepthGpu *gpu, Display *display, const DepthEffect *effect,
                            const DepthLayer *layers, int x, int y, int width, int height,
                            int bar_top, int bar_bottom);
int depth_gpu_gallery_region(DepthGpu *gpu, Display *display, const DepthEffect *effect,
                             const DepthLayer *layers, const uint8_t *const pixels[2],
                             int x, int y, int width, int height,
                             int clip_x, int clip_y, int clip_width, int clip_height, int upload);
int depth_gpu_gallery_anim_region(DepthGpu *gpu, Display *display, const DepthEffect *effect,
                                  const DepthLayer *layers,
                                  const DepthAnimRect *first, const DepthAnimRect *second,
                                  int clip_x, int clip_y, int clip_width, int clip_height,
                                  int bar_top, int bar_bottom);
int depth_gpu_gallery_bake_base(DepthGpu *gpu, Display *display, const DepthEffect *effect,
                                const DepthLayer *layers, const uint8_t *const pixels[2],
                                int band);
int depth_gpu_gallery_bake_patch(DepthGpu *gpu, Display *display, const DepthEffect *effect,
                                 const DepthLayer *layers, const uint8_t *const pixels[2],
                                 int cell, int slot,
                                 int src_x, int src_y, int src_width, int src_height,
                                 int patch_x, int patch_y, int patch_width, int patch_height,
                                 int band);
int depth_gpu_gallery_compose(DepthGpu *gpu, Display *display, int selected_cell, int slot);
void depth_gpu_gallery_release_bake(DepthGpu *gpu);
int depth_gpu_gallery_present(DepthGpu *gpu, Display *display);

#endif
