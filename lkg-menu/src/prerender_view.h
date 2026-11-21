#ifndef LKG_MENU_PRERENDER_VIEW_H
#define LKG_MENU_PRERENDER_VIEW_H

#include "depth.h"

typedef struct {
    size_t done;
    size_t total;
    double item_progress;
    uint64_t frame_current;
    uint64_t frame_total;
    int show_frames;
    int finished;
    int thumbnail_phase;
    const char *current_path;
} PrerenderViewState;

typedef struct {
    int full;
    int overall;
    int current;
    int path;
} PrerenderViewDirty;

PrerenderViewDirty prerender_view_dirty(const PrerenderViewState *previous,
                                        const PrerenderViewState *next, int full);
void prerender_view_draw(Display *display, Font *font, DepthLayer layers[2],
                         const PrerenderViewState *state, PrerenderViewDirty dirty);

#endif
