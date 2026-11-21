#ifndef LKG_MENU_DEPTH_FRAME_H
#define LKG_MENU_DEPTH_FRAME_H

#include "depth.h"

typedef struct {
    Display *display;
    const DepthEffect *effect;
    DepthLayer *layers;
    DepthCacheState *cache_state;
    uint64_t content_key;
    uint8_t *packed[2];
    size_t packed_size;
    int full;
    int direct_upload;
    int updates;
    int defer_cache_commit;
    int failed;
} DepthFrame;

int depth_frame_begin(DepthFrame *frame, Display *display, const DepthEffect *effect,
                      DepthLayer layers[2], DepthCacheState *cache_state,
                      uint64_t content_key, int force_full);
int depth_frame_update_rect(DepthFrame *frame, int x, int y, int width, int height,
                            int bar_top, int bar_bottom);
int depth_frame_present(DepthFrame *frame);
void depth_frame_sync_cache(DepthFrame *frame);
void depth_frame_close(DepthFrame *frame);

#endif
