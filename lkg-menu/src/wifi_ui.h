#ifndef LKG_MENU_WIFI_UI_H
#define LKG_MENU_WIFI_UI_H

#include "wifi_internal.h"

int wifi_ui_draw_current_page(WifiMenu *menu);
int wifi_ui_draw_password(WifiMenu *menu);
int wifi_ui_draw_password_cursor(WifiMenu *menu, int previous, int previous_back);
int wifi_ui_update_password_field(WifiMenu *menu);
int wifi_ui_update_password_keyboard(WifiMenu *menu);
int wifi_ui_draw_operation(WifiMenu *menu);
int wifi_ui_anim_active(const WifiMenu *menu);
int wifi_ui_anim_step(WifiMenu *menu);
void wifi_ui_anim_cancel(WifiMenu *menu);
int wifi_ui_password_anim_begin(Display *display, Font *font, const DepthEffect *depth,
                                DepthLayer layers[2], DepthCacheState *cache_state,
                                DepthAnim *anim, int layout, int old_id, int new_id);
int wifi_ui_password_anim_step(Display *display, Font *font, const DepthEffect *depth,
                               DepthLayer layers[2], DepthCacheState *cache_state,
                               DepthAnim *anim, int layout);
void wifi_ui_password_anim_cancel(Display *display, Font *font, const DepthEffect *depth,
                                  DepthLayer layers[2], DepthCacheState *cache_state,
                                  DepthAnim *anim, int layout);
double wifi_ui_monotonic_seconds(void);

#endif
