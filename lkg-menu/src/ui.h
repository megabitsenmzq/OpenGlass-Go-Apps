#ifndef LKG_MENU_UI_H
#define LKG_MENU_UI_H

#include <ft2build.h>
#include FT_FREETYPE_H
#include <signal.h>
#include <stdint.h>
#include <xf86drmMode.h>

extern volatile sig_atomic_t running;

typedef struct {
    uint32_t id;
    uint32_t handle;
    uint32_t stride;
    uint64_t size;
    uint8_t *pixels;
    int layered;
} Buffer;

typedef struct {
    int fd;
    uint32_t connector_id;
    uint32_t crtc_id;
    drmModeModeInfo mode;
    drmModeCrtc *saved_crtc;
    uint32_t active_framebuffer;
    int flip_pending;
    void (*flip_complete)(void *data);
    void *flip_data;
} Display;

typedef struct {
    FT_UInt index;
    int advance;
    int valid;
} FontGlyphMetrics;

typedef struct {
    FT_Library library;
    FT_Face face;
    unsigned int glyph_size;
    FontGlyphMetrics ascii[128];
} Font;

int display_wait_for_flip(Display *display);
int font_open(Font *font, const char *path, int pixel_size);
void font_close(Font *font);
int font_text_width(Font *font, const char *text);
void font_text_widths(Font *font, const char *text, int *prefix);
void draw_rect(Display *display, Buffer *buffer, int x, int y,
               int width, int height, uint32_t color);
void draw_text(Display *display, Buffer *buffer, Font *font,
               const char *text, int x, int baseline, uint32_t color);
void draw_text_bold(Display *display, Buffer *buffer, Font *font,
                    const char *text, int x, int baseline, uint32_t color);
void draw_text_fit(Display *display, Buffer *buffer, Font *font,
                   const char *text, int x, int baseline, uint32_t color,
                   int max_width, int bold);
void draw_back_arrow(Display *display, Buffer *buffer, int x, int center_y,
                     uint32_t color);

#endif
