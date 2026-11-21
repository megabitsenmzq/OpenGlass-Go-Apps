#ifndef LKG_MENU_WIFI_UI_H
#define LKG_MENU_WIFI_UI_H

#include "wifi_internal.h"

int wifi_ui_draw_current_page(WifiMenu *menu);
int wifi_ui_draw_password(WifiMenu *menu);
int wifi_ui_draw_password_cursor(WifiMenu *menu, int previous, int include_back);
int wifi_ui_update_password_field(WifiMenu *menu);
int wifi_ui_update_password_keyboard(WifiMenu *menu);
int wifi_ui_draw_operation(WifiMenu *menu);
double wifi_ui_monotonic_seconds(void);

#endif
