#ifndef LKG_MENU_WIFI_MODEL_H
#define LKG_MENU_WIFI_MODEL_H

#include <stddef.h>
#include <stdint.h>

#define WIFI_MAX_NETWORKS 32
#define WIFI_MAX_SAVED_NETWORKS 32

typedef struct {
    char ssid[33];
    size_t ssid_length;
    char display_ssid[65];
    char flags[160];
    int signal;
    int secured;
    int enterprise;
    int wep;
} WifiNetwork;

typedef struct {
    WifiNetwork networks[WIFI_MAX_NETWORKS];
    int network_count;
    WifiNetwork selected_network;
    char current_ssid[33];
    size_t current_ssid_length;
    char saved_ssids[WIFI_MAX_SAVED_NETWORKS][33];
    size_t saved_ssid_lengths[WIFI_MAX_SAVED_NETWORKS];
    int saved_network_count;
    char ip_address[40];
    char router_address[40];
    char mac_address[24];
    char bssid[24];
    char signal[64];
    char frequency[64];
    char link_speed[64];
    char security[64];
} WifiModel;

size_t wifi_model_decode_ssid(const char *input, char output[33]);
int wifi_model_hex_value(char character);
void wifi_model_parse_scan(WifiModel *model, char *output);
int wifi_model_is_saved(const WifiModel *model, const WifiNetwork *network);
int wifi_model_is_current(const WifiModel *model, const WifiNetwork *network);
uint64_t wifi_networks_content_key(const WifiNetwork *networks, int network_count,
                                   int scanning, const char *current_ssid,
                                   size_t current_ssid_length);
uint64_t wifi_details_content_key(const WifiNetwork *selected_network, int connected,
                                  const char *const values[8]);

#endif
