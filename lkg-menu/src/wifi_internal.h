#ifndef LKG_MENU_WIFI_INTERNAL_H
#define LKG_MENU_WIFI_INTERNAL_H

#include "depth_anim.h"
#include "depth_frame.h"
#include "wifi_control.h"

typedef enum {
    WIFI_PAGE_NETWORKS = 0,
    WIFI_PAGE_PASSWORD,
    WIFI_PAGE_DETAILS
} WifiPage;

typedef struct {
    Display *display;
    Font *font;
    const DepthEffect *depth;
    DepthLayer *layers;
    WifiPage page;
    WifiModel model;
    WifiProcess scan;
    WifiProcess connection;
    WifiProcess update;
    WifiOperation operation;
    double failure_until;
    int selected;
    int scroll;
    WifiPage cached_page;
    int cached_selected;
    int cached_scroll;
    DepthCacheState cache_state;
    DepthAnim anim;
    WifiPage anim_page;
    int keyboard_layout;
    int key_selected;
    int back_selected;
    char password[65];
    size_t password_length;
} WifiMenu;

static inline int wifi_menu_network_is_current(const WifiMenu *menu,
                                               const WifiNetwork *network)
{
    return wifi_model_is_current(&menu->model, network);
}

#endif
