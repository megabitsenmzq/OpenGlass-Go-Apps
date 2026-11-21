#ifndef LKG_MENU_FILE_MANAGER_VIEW_H
#define LKG_MENU_FILE_MANAGER_VIEW_H

#include "depth.h"

void file_manager_view_draw(Display *display, Font *font, const DepthEffect *depth,
                            DepthLayer layers[2], const char *ip_address);

#endif
