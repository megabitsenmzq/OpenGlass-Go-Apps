#ifndef LKG_MENU_WIFI_VIEW_H
#define LKG_MENU_WIFI_VIEW_H

#include "depth.h"
#include "wifi_model.h"

#define WIFI_ROW_HEIGHT 150
#define WIFI_VISIBLE_ROWS 17

int wifi_view_networks_page_start(int selected);
int wifi_view_networks_start_y(const Display *display, int visible_rows);
int wifi_view_networks_row_y(const Display *display, int visible_rows, int row, int scroll);

void wifi_view_draw_network_row(Display *display, Font *font, const DepthEffect *depth,
                                DepthLayer layers[2], int row, int y, int selected,
                                int scanning, int interactive, const WifiNetwork *network,
                                const char *current_ssid, size_t current_ssid_length);
void wifi_view_draw_details_row(Display *display, Font *font, const DepthEffect *depth,
                                DepthLayer layers[2], int row, int y, int selected,
                                int interactive, int connected, const char *const values[9]);
void wifi_view_key_geometry(Display *display, int index, int *key_x, int *key_y,
                            int *key_width, int *key_height);
void wifi_view_draw_password_key(Display *display, Font *font, const DepthEffect *depth,
                                 DepthLayer layers[2], int index, int active, int layout);
void wifi_view_draw_password_back(Display *display, Font *font, const DepthEffect *depth,
                                  DepthLayer layers[2], int back_selected, int interactive);
void wifi_view_draw_password_field(Display *display, Font *font, DepthLayer layers[2],
                                   const char *password);
void wifi_view_draw_operation(Display *display, Font *font, const DepthEffect *depth,
                              DepthLayer layers[2], int failed, const char *operation,
                              const char *reason);

#endif
