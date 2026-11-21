#ifndef LKG_MENU_WIFI_H
#define LKG_MENU_WIFI_H

#include "depth.h"
#include "ui.h"
#include "usb_keyboard.h"

int wifi_run(Display *display, Font *font, const DepthEffect *depth,
             DepthLayer layers[2], int serial_fd, UsbKeyboard *keyboard, int button_reversed);

#endif
