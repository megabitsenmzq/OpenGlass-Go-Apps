#include "test.h"
#include "wifi_model.h"

static void network(WifiNetwork *network, const char *ssid, int signal)
{
    memset(network, 0, sizeof(*network));
    snprintf(network->ssid, sizeof(network->ssid), "%s", ssid);
    network->ssid_length = strlen(ssid);
    snprintf(network->display_ssid, sizeof(network->display_ssid), "%s", ssid);
    network->signal = signal;
}

static uint64_t list_key(WifiNetwork networks[2], int scanning, const char *current)
{
    return wifi_networks_content_key(networks, 2, scanning, current, strlen(current));
}

int test_wifi_model(void)
{
    WifiNetwork networks[2];
    uint64_t baseline;
    network(&networks[0], "Alpha", -50);
    network(&networks[1], "Beta", -70);
    baseline = list_key(networks, 1, "Alpha");
    TEST_ASSERT(list_key(networks, 1, "Alpha") == baseline);
    TEST_ASSERT(list_key(networks, 0, "Alpha") != baseline);
    TEST_ASSERT(list_key(networks, 1, "Beta") != baseline);
    networks[0].signal = -51;
    TEST_ASSERT(list_key(networks, 1, "Alpha") != baseline);
    networks[0].signal = -50;
    networks[0].display_ssid[0] = 'a';
    TEST_ASSERT(list_key(networks, 1, "Alpha") != baseline);
    networks[0].display_ssid[0] = 'A';
    networks[1].ssid[0] = 'C';
    networks[1].ssid_length = strlen(networks[1].ssid);
    TEST_ASSERT(list_key(networks, 1, "Alpha") != baseline);
    network(&networks[1], "Beta", -70);
    TEST_ASSERT(wifi_networks_content_key(networks, 1, 1, "Alpha", 5) != baseline);

    {
        const char *values[8] = {
            "192.168.1.2", "192.168.1.1", "00:11", "22:33",
            "-50 dBm", "5180 MHz", "100 Mb/s", "WPA2"
        };
        uint64_t details = wifi_details_content_key(&networks[0], 1, values);
        int i;
        TEST_ASSERT(wifi_details_content_key(&networks[0], 1, values) == details);
        TEST_ASSERT(wifi_details_content_key(&networks[0], 0, values) != details);
        for (i = 0; i < 8; ++i) {
            char original[64];
            char *copy[8];
            int j;
            for (j = 0; j < 8; ++j)
                copy[j] = (char *)values[j];
            snprintf(original, sizeof(original), "%s", values[i]);
            copy[i] = "changed";
            TEST_ASSERT(wifi_details_content_key(&networks[0], 1, (const char *const *)copy) != details);
            copy[i] = original;
        }
        networks[0].ssid[0] = 'X';
        TEST_ASSERT(wifi_details_content_key(&networks[0], 1, values) != details);
    }
    {
        WifiModel model = {0};
        char decoded[33];
        char output[] =
            "bssid\tfrequency\tsignal\tflags\tssid\n"
            "aa\t2412\t-70\t[WPA2-PSK]\tAlpha\n"
            "bb\t2412\t-40\t[WPA2-PSK]\tAlpha\n"
            "cc\t5180\t-55\t[SAE]\tBeta\n";
        TEST_ASSERT(wifi_model_decode_ssid("Caf\\xC3\\xA9", decoded) == 5);
        TEST_ASSERT((unsigned char)decoded[3] == 0xc3);
        TEST_ASSERT((unsigned char)decoded[4] == 0xa9);
        wifi_model_parse_scan(&model, output);
        TEST_ASSERT(model.network_count == 2);
        TEST_STRING(model.networks[0].display_ssid, "Alpha");
        TEST_ASSERT(model.networks[0].signal == -40);
        TEST_ASSERT(model.networks[0].secured == 1);
        TEST_STRING(model.networks[1].display_ssid, "Beta");
        snprintf(model.current_ssid, sizeof(model.current_ssid), "Alpha");
        model.current_ssid_length = 5;
        TEST_ASSERT(wifi_model_is_current(&model, &model.networks[0]) == 1);
        TEST_ASSERT(wifi_model_is_current(&model, &model.networks[1]) == 0);
        snprintf(model.saved_ssids[0], sizeof(model.saved_ssids[0]), "Beta");
        model.saved_ssid_lengths[0] = 4;
        model.saved_network_count = 1;
        TEST_ASSERT(wifi_model_is_saved(&model, &model.networks[1]) == 1);
        TEST_ASSERT(wifi_model_is_saved(&model, &model.networks[0]) == 0);
    }
    return 0;
}
