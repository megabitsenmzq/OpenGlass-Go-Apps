#include "wifi_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t wifi_hash_bytes(uint64_t hash, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    size_t i;
    for (i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t wifi_hash_string(uint64_t hash, const char *text)
{
    return wifi_hash_bytes(hash, text, strlen(text) + 1);
}

uint64_t wifi_networks_content_key(const WifiNetwork *networks, int network_count,
                                   int scanning, const char *current_ssid,
                                   size_t current_ssid_length)
{
    uint64_t hash = 1469598103934665603ULL;
    int i;
    hash = wifi_hash_bytes(hash, &network_count, sizeof(network_count));
    hash = wifi_hash_bytes(hash, &scanning, sizeof(scanning));
    hash = wifi_hash_bytes(hash, &current_ssid_length, sizeof(current_ssid_length));
    hash = wifi_hash_bytes(hash, current_ssid, current_ssid_length);
    for (i = 0; i < network_count; ++i) {
        const WifiNetwork *network = &networks[i];
        hash = wifi_hash_bytes(hash, &network->ssid_length, sizeof(network->ssid_length));
        hash = wifi_hash_bytes(hash, network->ssid, network->ssid_length);
        hash = wifi_hash_string(hash, network->display_ssid);
        hash = wifi_hash_bytes(hash, &network->signal, sizeof(network->signal));
    }
    return hash;
}

uint64_t wifi_details_content_key(const WifiNetwork *selected_network, int connected,
                                  const char *const values[8])
{
    uint64_t hash = 1469598103934665603ULL;
    size_t i;
    hash = wifi_hash_bytes(hash, &connected, sizeof(connected));
    hash = wifi_hash_bytes(hash, selected_network->ssid, selected_network->ssid_length);
    for (i = 0; i < 8; ++i)
        hash = wifi_hash_string(hash, values[i]);
    return hash;
}

int wifi_model_hex_value(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

size_t wifi_model_decode_ssid(const char *input, char output[33])
{
    size_t length = 0;
    while (*input && length < 32) {
        if (input[0] == '\\' && input[1] == 'x' &&
            wifi_model_hex_value(input[2]) >= 0 && wifi_model_hex_value(input[3]) >= 0) {
            output[length++] = (char)((wifi_model_hex_value(input[2]) << 4) |
                                      wifi_model_hex_value(input[3]));
            input += 4;
        } else if (input[0] == '\\' && input[1]) {
            output[length++] = input[1];
            input += 2;
        } else {
            output[length++] = *input++;
        }
    }
    output[length] = '\0';
    return length;
}

static void make_display_ssid(WifiNetwork *network)
{
    size_t i;
    for (i = 0; i < network->ssid_length; ++i) {
        unsigned char character = (unsigned char)network->ssid[i];
        network->display_ssid[i] = character >= 32 && character < 127 ? (char)character : '?';
    }
    network->display_ssid[network->ssid_length] = '\0';
}

static int network_compare(const void *left, const void *right)
{
    const WifiNetwork *a = left;
    const WifiNetwork *b = right;
    if (a->signal != b->signal)
        return a->signal < b->signal ? 1 : -1;
    return strcmp(a->display_ssid, b->display_ssid);
}

void wifi_model_parse_scan(WifiModel *model, char *output)
{
    char *line;
    char *save_line = NULL;
    model->network_count = 0;
    line = strtok_r(output, "\n", &save_line);
    while (line) {
        char *fields[5] = {NULL, NULL, NULL, NULL, NULL};
        char *cursor = line;
        WifiNetwork network;
        int i;
        int existing = -1;
        for (i = 0; i < 5 && cursor; ++i) {
            char *tab = strchr(cursor, '\t');
            fields[i] = cursor;
            if (tab) {
                *tab = '\0';
                cursor = tab + 1;
            } else {
                cursor = NULL;
            }
        }
        line = strtok_r(NULL, "\n", &save_line);
        if (!fields[0] || !fields[1] || !fields[2] || !fields[3] || !fields[4] ||
            !fields[4][0] || strcmp(fields[0], "bssid") == 0)
            continue;
        memset(&network, 0, sizeof(network));
        network.signal = atoi(fields[2]);
        network.ssid_length = wifi_model_decode_ssid(fields[4], network.ssid);
        if (!network.ssid_length)
            continue;
        make_display_ssid(&network);
        snprintf(network.flags, sizeof(network.flags), "%s", fields[3]);
        network.enterprise = strstr(fields[3], "EAP") != NULL;
        network.wep = strstr(fields[3], "WEP") != NULL;
        network.secured = strstr(fields[3], "WPA") != NULL ||
                          strstr(fields[3], "SAE") != NULL || network.wep;
        for (i = 0; i < model->network_count; ++i) {
            if (model->networks[i].ssid_length == network.ssid_length &&
                memcmp(model->networks[i].ssid, network.ssid, network.ssid_length) == 0) {
                existing = i;
                break;
            }
        }
        if (existing >= 0) {
            if (network.signal > model->networks[existing].signal)
                model->networks[existing] = network;
        } else if (model->network_count < WIFI_MAX_NETWORKS) {
            model->networks[model->network_count++] = network;
        }
    }
    qsort(model->networks, (size_t)model->network_count,
          sizeof(model->networks[0]), network_compare);
}

int wifi_model_is_saved(const WifiModel *model, const WifiNetwork *network)
{
    int i;
    for (i = 0; i < model->saved_network_count; ++i) {
        if (model->saved_ssid_lengths[i] == network->ssid_length &&
            memcmp(model->saved_ssids[i], network->ssid, network->ssid_length) == 0)
            return 1;
    }
    return 0;
}

int wifi_model_is_current(const WifiModel *model, const WifiNetwork *network)
{
    return network->ssid_length == model->current_ssid_length &&
           memcmp(network->ssid, model->current_ssid, network->ssid_length) == 0;
}
