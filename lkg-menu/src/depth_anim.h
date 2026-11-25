#ifndef LKG_MENU_DEPTH_ANIM_H
#define LKG_MENU_DEPTH_ANIM_H

#include "depth.h"

typedef struct {
    int active;
    double started;
    double duration;
    int old_id;
    int new_id;
    float old_start_disparity;
    float new_start_disparity;
    float cur_old_disparity;
    float cur_new_disparity;
    DepthAnimRect old_rect;
    DepthAnimRect new_rect;
    int clip_count;
    int clip_x[2];
    int clip_y[2];
    int clip_w[2];
    int clip_h[2];
    int bar_top;
    int bar_bottom;
    int has_back;
    uint64_t content_key;
    int frames;
} DepthAnim;

double depth_anim_monotonic_seconds(void);
int depth_anim_margin(const DepthEffect *effect);
void depth_anim_clip_rect(const Display *display, const DepthEffect *effect,
                          int x, int y, int width, int height,
                          int *cx, int *cy, int *cw, int *ch);
void depth_anim_tight_x(const Display *display, const Buffer *buffer,
                        int y, int height,
                        int fallback_x0, int fallback_x1,
                        int *x0_out, int *x1_out);
void depth_anim_set_clips(DepthAnim *anim, const Display *display,
                          const DepthEffect *effect,
                          const DepthAnimRect *a, const DepthAnimRect *b);
int depth_anim_upload_rect(Display *display, const DepthEffect *effect,
                           DepthLayer layers[2], int x, int y, int width, int height);
void depth_anim_start(DepthAnim *anim, const Display *display, const DepthEffect *effect,
                      int old_id, int new_id,
                      const DepthAnimRect *old_rect, const DepthAnimRect *new_rect,
                      int bar_top, int bar_bottom, int has_back, uint64_t content_key,
                      float old_start, float new_start);
int depth_anim_step(DepthAnim *anim, Display *display, const DepthEffect *effect,
                    DepthLayer layers[2], const char *label, int *finished_out);

#endif
