#ifndef LKG_APP_H
#define LKG_APP_H

#include "lkg_app_protocol.h"

#include <stddef.h>
#include <stdint.h>

typedef struct LkgApp LkgApp;

/* Connect to the lkg-menu host using LKG_APP_FD (or an explicit fd). */
LkgApp *lkg_app_open(void);
LkgApp *lkg_app_open_fd(int fd);
void lkg_app_close(LkgApp *app);

/* Returns display dimensions, standard font sizes, default depths, and theme colors. */
const LkgAppInfo *lkg_app_info(const LkgApp *app);
int lkg_app_fd(const LkgApp *app);

/*
 * Wait up to timeout_ms (-1 = forever, 0 = non-blocking) for the next input
 * or lifecycle event (physical buttons, USB keyboard, or quit).
 * Returns 1 if an event was read, 0 on timeout/EINTR, -1 on error/disconnect.
 */
int lkg_app_poll_event(LkgApp *app, LkgEvent *event, int timeout_ms);

/*
 * Open the host's 3D on-screen virtual keyboard (with USB keyboard passthrough)
 * to edit a text string up to 64 characters.
 * Returns 1 if confirmed, 0 if cancelled, -1 on error/quit.
 */
int lkg_app_text_input(LkgApp *app, const char *title,
                       const char *initial_text, char *out_text, size_t out_size);

/* Hosted rendering controls (no-op / error in LKG_RENDER_DRM mode). */
int lkg_app_set_depth(LkgApp *app, float front_disparity, float back_disparity);
int lkg_app_set_stereo(LkgApp *app, int stereo_mode);
int lkg_app_clear(LkgApp *app);
int lkg_app_clear_layer(LkgApp *app, int layer);
int lkg_app_draw_rect(LkgApp *app, int layer, int x, int y, int width, int height,
                      uint32_t color);
int lkg_app_draw_text(LkgApp *app, int layer, const char *text, int x, int baseline,
                      int font_size, uint32_t color, int bold);
int lkg_app_draw_text_fit(LkgApp *app, int layer, const char *text, int x, int baseline,
                          int font_size, uint32_t color, int max_width, int bold);
int lkg_app_draw_back_arrow(LkgApp *app, int layer, int x, int center_y, uint32_t color);
int lkg_app_measure_text(LkgApp *app, const char *text, int font_size,
                         LkgTextMetrics *metrics);
int lkg_app_row_baseline(int y, int row_height, const LkgTextMetrics *metrics);

/* Draw a standard lkg-menu list row (active on front layer, inactive on back layer). */
int lkg_app_draw_menu_row(LkgApp *app, const char *label, int y, int active, int is_back);

/*
 * Direct zero-copy ARGB8888 shared memory buffer for LKG_LAYER_FRONT (0) or
 * LKG_LAYER_BACK (1). Call lkg_app_sync() after remote draw_* calls if mixing
 * them with direct pixel writes before presenting.
 */
uint32_t *lkg_app_layer_pixels(LkgApp *app, int layer);
int lkg_app_sync(LkgApp *app);

/*
 * Composite layers through the Looking Glass lenticular shader and flip DRM.
 * bar_top..bar_bottom sets the standard light highlight strip on the background plane
 * (pass 0, 0 for no highlight bar).
 */
int lkg_app_present(LkgApp *app, int bar_top, int bar_bottom);
int lkg_app_present_rect(LkgApp *app, int x, int y, int width, int height,
                         int bar_top, int bar_bottom);
int lkg_app_present_rects(LkgApp *app, const LkgRectUpdate *rects, size_t count);
int lkg_app_upload_rect(LkgApp *app, int x, int y, int width, int height);
int lkg_app_present_anim(LkgApp *app, const LkgAnimRect *rects, size_t rect_count,
                         const LkgClipRect *clips, size_t clip_count,
                         int bar_top, int bar_bottom, int has_back,
                         int present, int commit_cache);
int lkg_app_present_anim_scroll(LkgApp *app, const LkgAnimRect *rects, size_t rect_count,
                                const LkgClipRect *clips, size_t clip_count,
                                int bar_top, int bar_bottom, int has_back,
                                float scroll_y, int scroll_limit_y, int source_max_y,
                                int clear_cache, int present, int commit_cache);

#endif
