#ifndef LKG_MENU_GALLERY_VIEW_H
#define LKG_MENU_GALLERY_VIEW_H

#include "depth.h"
#include "media_scan.h"

#include <stddef.h>

#define THUMB_WIDTH 450
#define THUMB_HEIGHT 800
#define THUMB_BYTES (THUMB_WIDTH * THUMB_HEIGHT * 4)

int gallery_default_cursor(size_t item_count);
void gallery_view_draw_text_end(Display *display, Buffer *buffer, Font *font,
                                const char *text, int x, int baseline, int max_width,
                                uint32_t color);
void gallery_view_draw_text_start(Display *display, Buffer *buffer, Font *font,
                                  const char *text, int x, int baseline, int max_width,
                                  uint32_t color);
void gallery_view_draw_cell(Display *display, Font *font, DepthLayer layers[2],
                            int cell, int selected, GalleryItem *item,
                            int special, int cursor);
void gallery_view_move_cell(Display *display, Font *font, DepthLayer layers[2],
                            int cell, int selected, GalleryItem *item);
int gallery_view_show_error(Display *display, DepthEffect *depth, DepthLayer layers[2]);

#endif
