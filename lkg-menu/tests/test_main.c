#include "test.h"

#include <signal.h>

volatile sig_atomic_t running = 1;

int main(void)
{
    int tests_run = 0;
    int tests_failed = 0;
    TEST_RUN(test_serial_parser);
    TEST_RUN(test_depth_cache);
    TEST_RUN(test_wifi_model);
    TEST_RUN(test_password_keyboard);
    TEST_RUN(test_menu_model);
    TEST_RUN(test_media_scan);
    TEST_RUN(test_cache_key);
    if (tests_failed) {
        fprintf(stderr, "%d/%d tests failed\n", tests_failed, tests_run);
        return 1;
    }
    printf("All %d tests passed\n", tests_run);
    return 0;
}
