#ifndef LKG_MENU_TEST_H
#define LKG_MENU_TEST_H

#include <stdio.h>
#include <string.h>

#define TEST_ASSERT(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define TEST_STRING(actual, expected) \
    TEST_ASSERT(strcmp((actual), (expected)) == 0)

#define TEST_RUN(test) do { \
    ++tests_run; \
    if ((test)() != 0) { \
        ++tests_failed; \
        fprintf(stderr, "FAIL: %s\n", #test); \
    } \
} while (0)

int test_serial_parser(void);
int test_depth_cache(void);
int test_wifi_model(void);
int test_password_keyboard(void);
int test_menu_model(void);
int test_media_scan(void);
int test_cache_key(void);

#endif
