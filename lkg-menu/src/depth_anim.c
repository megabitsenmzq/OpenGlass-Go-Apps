#include "depth_anim.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

double depth_anim_monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

int depth_anim_margin(const DepthEffect *effect)
{
    float first = effect ? fabsf(effect->front_disparity) : 0.0f;
    float second = effect ? fabsf(effect->back_disparity) : 0.0f;
    float maximum = first > second ? first : second;
    return (int)ceilf(maximum) + 2;
}

void depth_anim_clip_rect(const Display *display, const DepthEffect *effect,
                          int x, int y, int width, int height,
                          int *cx, int *cy, int *cw, int *ch)
{
    const int screen_w = (int)display->mode.hdisplay;
    const int screen_h = (int)display->mode.vdisplay;
    int margin = depth_anim_margin(effect);
    int rx0 = x - margin;
    int ry0 = y;
    int rx1 = x + width + margin;
    int ry1 = y + height;
    if (rx0 < 0)
        rx0 = 0;
    if (ry0 < 0)
        ry0 = 0;
    if (rx1 > screen_w)
        rx1 = screen_w;
    if (ry1 > screen_h)
        ry1 = screen_h;
    *cx = rx0;
    *cy = ry0;
    *cw = rx1 > rx0 ? rx1 - rx0 : 0;
    *ch = ry1 > ry0 ? ry1 - ry0 : 0;
}

void depth_anim_tight_x(const Display *display, const Buffer *buffer,
                        int y, int height,
                        int fallback_x0, int fallback_x1,
                        int *x0_out, int *x1_out)
{
    int width = display ? (int)display->mode.hdisplay : 0;
    int screen_h = display ? (int)display->mode.vdisplay : 0;
    int min_x = width;
    int max_x = 0;
    int row;
    if (!display || !buffer || !buffer->pixels || width <= 0 || screen_h <= 0 ||
        buffer->stride < (uint32_t)width * 4 || y < 0 || height <= 0 || y >= screen_h) {
        *x0_out = fallback_x0;
        *x1_out = fallback_x1;
        return;
    }
    if (y + height > screen_h)
        height = screen_h - y;
    for (row = 0; row < height; ++row) {
        const uint32_t *line = (const uint32_t *)(buffer->pixels +
                               (size_t)(y + row) * (size_t)buffer->stride);
        int col;
        for (col = 0; col < min_x; ++col) {
            if ((line[col] >> 24) != 0u) {
                min_x = col;
                break;
            }
        }
        for (col = width - 1; col >= max_x; --col) {
            if ((line[col] >> 24) != 0u) {
                max_x = col + 1;
                break;
            }
        }
    }
    if (min_x >= max_x) {
        *x0_out = 0;
        *x1_out = 0;
        return;
    }
    *x0_out = min_x;
    *x1_out = max_x;
}

void depth_anim_set_clips(DepthAnim *anim, const Display *display,
                          const DepthEffect *effect,
                          const DepthAnimRect *a, const DepthAnimRect *b)
{
    int cx0 = 0, cy0 = 0, cw0 = 0, ch0 = 0;
    int cx1 = 0, cy1 = 0, cw1 = 0, ch1 = 0;
    int has0;
    int has1;
    if (a)
        depth_anim_clip_rect(display, effect, a->x0, a->y0,
                             a->x1 - a->x0, a->y1 - a->y0, &cx0, &cy0, &cw0, &ch0);
    if (b)
        depth_anim_clip_rect(display, effect, b->x0, b->y0,
                             b->x1 - b->x0, b->y1 - b->y0, &cx1, &cy1, &cw1, &ch1);
    has0 = cw0 > 0 && ch0 > 0;
    has1 = cw1 > 0 && ch1 > 0;
    if (has0 && has1) {
        if (cx0 <= cx1 + cw1 && cx1 <= cx0 + cw0 &&
            cy0 <= cy1 + ch1 && cy1 <= cy0 + ch0) {
            int rx0 = cx0 < cx1 ? cx0 : cx1;
            int ry0 = cy0 < cy1 ? cy0 : cy1;
            int rx1 = cx0 + cw0 > cx1 + cw1 ? cx0 + cw0 : cx1 + cw1;
            int ry1 = cy0 + ch0 > cy1 + ch1 ? cy0 + ch0 : cy1 + ch1;
            anim->clip_count = 1;
            anim->clip_x[0] = rx0;
            anim->clip_y[0] = ry0;
            anim->clip_w[0] = rx1 - rx0;
            anim->clip_h[0] = ry1 - ry0;
            return;
        }
        anim->clip_count = 2;
        anim->clip_x[0] = cx0;
        anim->clip_y[0] = cy0;
        anim->clip_w[0] = cw0;
        anim->clip_h[0] = ch0;
        anim->clip_x[1] = cx1;
        anim->clip_y[1] = cy1;
        anim->clip_w[1] = cw1;
        anim->clip_h[1] = ch1;
        return;
    }
    if (has0 || has1) {
        anim->clip_count = 1;
        anim->clip_x[0] = has0 ? cx0 : cx1;
        anim->clip_y[0] = has0 ? cy0 : cy1;
        anim->clip_w[0] = has0 ? cw0 : cw1;
        anim->clip_h[0] = has0 ? ch0 : ch1;
        return;
    }
    anim->clip_count = 0;
}

int depth_anim_upload_rect(Display *display, const DepthEffect *effect,
                           DepthLayer layers[2], int x, int y, int width, int height)
{
    const int screen_w = (int)display->mode.hdisplay;
    const int screen_h = (int)display->mode.vdisplay;
    int rx0 = x < 0 ? 0 : x;
    int ry0 = y < 0 ? 0 : y;
    int rx1 = x + width > screen_w ? screen_w : x + width;
    int ry1 = y + height > screen_h ? screen_h : y + height;
    int layer;
    if (rx1 <= rx0 || ry1 <= ry0)
        return 0;
    for (layer = 0; layer < 2; ++layer)
        depth_layer_finish_area(&layers[layer], rx0, ry0, rx1 - rx0, ry1 - ry0,
                                screen_w, screen_h);
    return depth_gallery_upload(display, effect, layers, rx0, ry0, rx1 - rx0, ry1 - ry0);
}

void depth_anim_start(DepthAnim *anim, const Display *display, const DepthEffect *effect,
                      int old_id, int new_id,
                      const DepthAnimRect *old_rect, const DepthAnimRect *new_rect,
                      int bar_top, int bar_bottom, int has_back, uint64_t content_key,
                      float old_start, float new_start)
{
    memset(anim, 0, sizeof(*anim));
    anim->active = 1;
    anim->duration = 0.22;
    anim->started = depth_anim_monotonic_seconds() - 0.016;
    anim->old_id = old_id;
    anim->new_id = new_id;
    anim->old_start_disparity = old_start;
    anim->new_start_disparity = new_start;
    anim->cur_old_disparity = old_start;
    anim->cur_new_disparity = new_start;
    anim->old_rect = *old_rect;
    anim->new_rect = *new_rect;
    anim->bar_top = bar_top;
    anim->bar_bottom = bar_bottom;
    anim->has_back = has_back ? 1 : 0;
    anim->content_key = content_key;
    depth_anim_set_clips(anim, display, effect, &anim->old_rect, &anim->new_rect);
}

int depth_anim_step(DepthAnim *anim, Display *display, const DepthEffect *effect,
                    DepthLayer layers[2], const char *label, int *finished_out)
{
    double elapsed;
    double t;
    float inv;
    float ease;
    float d_old;
    float d_new;
    int done;
    int i;
    if (finished_out)
        *finished_out = 0;
    if (!anim->active)
        return 0;
    elapsed = depth_anim_monotonic_seconds() - anim->started;
    t = anim->duration > 0.0 ? elapsed / anim->duration : 1.0;
    done = t >= 1.0;
    if (t < 0.0)
        t = 0.0;
    else if (t > 1.0)
        t = 1.0;
    inv = 1.0f - (float)t;
    ease = 1.0f - inv * inv * inv;
    d_old = anim->old_start_disparity +
            (effect->back_disparity - anim->old_start_disparity) * ease;
    d_new = anim->new_start_disparity +
            (effect->front_disparity - anim->new_start_disparity) * ease;
    anim->cur_old_disparity = d_old;
    anim->cur_new_disparity = d_new;
    anim->old_rect.disparity = d_old;
    anim->new_rect.disparity = d_new;
    for (i = 0; i < anim->clip_count; ++i) {
        if (depth_gallery_anim_region(display, effect, layers,
                                      &anim->new_rect, &anim->old_rect,
                                      anim->clip_x[i], anim->clip_y[i],
                                      anim->clip_w[i], anim->clip_h[i],
                                      anim->bar_top, anim->bar_bottom,
                                      anim->has_back) != 0)
            return -1;
    }
    if (depth_gallery_present(display, effect) != 0)
        return -1;
    ++anim->frames;
    if (done) {
        double total_ms = (depth_anim_monotonic_seconds() - anim->started) * 1000.0;
        if (label && total_ms > 0.0) {
            fprintf(stderr, "%s anim: %d frames in %.1f ms (%.1f FPS)\n",
                    label, anim->frames, total_ms, anim->frames * 1000.0 / total_ms);
        }
        if (finished_out)
            *finished_out = 1;
    }
    return 0;
}
