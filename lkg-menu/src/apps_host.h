#ifndef LKG_MENU_APPS_HOST_H
#define LKG_MENU_APPS_HOST_H

#include "apps_scan.h"
#include "depth.h"
#include "lkg_app_protocol.h"
#include "usb_keyboard.h"

LkgEvent apps_host_event_from_serial(const char *normalized_cmd);
LkgEvent apps_host_event_from_usb_key(UsbKeyEvent key);

int apps_run(Display *display, Font *font, DepthEffect *depth,
             DepthLayer layers[2], int serial_fd, UsbKeyboard *keyboard,
             int button_reversed, const char *apps_dir);

#endif
