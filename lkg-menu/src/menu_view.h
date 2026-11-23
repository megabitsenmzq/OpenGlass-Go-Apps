#ifndef LKG_MENU_VIEW_H
#define LKG_MENU_VIEW_H

#include "depth.h"
#include "menu_model.h"

typedef struct {
    char serial[128];
    char cpu[128];
    char memory[128];
    char gpu[128];
    char display[128];
    char disk[128];
    char os[128];
    char kernel[128];
    char ssh[128];
    char ip[128];
} SystemInfo;

void menu_view_draw_page(Display *display, Font *font, const DepthEffect *depth,
                         DepthLayer layers[2], int page, int has_apps,
                         int selected, const SystemInfo *info);
void menu_view_draw_row(Display *display, Font *font, const DepthEffect *depth,
                        DepthLayer layers[2], int page, int has_apps, int row,
                        int selected, const SystemInfo *info);

#endif
