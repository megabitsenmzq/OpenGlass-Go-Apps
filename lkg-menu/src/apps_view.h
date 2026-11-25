#ifndef LKG_MENU_APPS_VIEW_H
#define LKG_MENU_APPS_VIEW_H

#include "apps_scan.h"
#include "depth.h"

#define APPS_ROW_HEIGHT 150
#define APPS_VISIBLE_ROWS 17

int apps_view_page_start(int selected);
int apps_view_start_y(const Display *display, int visible_rows);
int apps_view_row_y(const Display *display, int visible_rows, int row, int scroll);

void apps_view_draw_row(Display *display, Font *font, const DepthEffect *depth,
                        DepthLayer layers[2], int row, int y, int selected,
                        int interactive, const ExternalApp *app);
void apps_view_draw_row_anim(Display *display, Font *font, const DepthEffect *depth,
                             DepthLayer layers[2], int row, int y, int active,
                             const ExternalApp *app);

void apps_view_draw_text_input(Display *display, Font *font, const DepthEffect *depth,
                               DepthLayer layers[2], const char *title,
                               const char *text, int layout, int key_selected,
                               int back_selected, int interactive);

void apps_view_draw_status(Display *display, Font *font, const DepthEffect *depth,
                           DepthLayer layers[2], int failed, const char *title,
                           const char *detail);

#endif
