#ifndef LKG_MENU_GALLERY_H
#define LKG_MENU_GALLERY_H

#include "depth.h"
#include "ui.h"
#include "usb_keyboard.h"

int gallery_run(Display *display, Font *font, DepthEffect *depth,
                DepthLayer layers[2], int serial_fd, UsbKeyboard *keyboard,
                double image_duration, int button_reversed, int auto_play);
int gallery_prerender(Display *display, Font *font, const DepthEffect *depth,
                      DepthLayer layers[2], int serial_fd, UsbKeyboard *keyboard);
int gallery_cache_cleanup_run(void);

#endif
