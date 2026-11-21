#include "test.h"
#include "password_keyboard.h"

static PasswordKeyboardAction activate(PasswordKeyboardState *state)
{
    return password_keyboard_handle(state, "PLPA");
}

int test_password_keyboard(void)
{
    PasswordKeyboardState state = { .key_selected = 25, .back_selected = 1 };
    TEST_ASSERT(password_keyboard_handle(&state, "PREV") == PASSWORD_KEYBOARD_CHANGED);
    TEST_ASSERT(state.back_selected == 0 && state.key_selected == 0);
    TEST_ASSERT(password_keyboard_redraw("PREV", 1, &state, 0) == PASSWORD_REDRAW_BACK);
    TEST_ASSERT(activate(&state) == PASSWORD_KEYBOARD_CHANGED);
    TEST_STRING(state.password, "1");
    TEST_ASSERT(password_keyboard_redraw("PLPA", 0, &state, 0) == PASSWORD_REDRAW_PASSWORD);
    TEST_ASSERT(password_keyboard_handle(&state, "NEXT") == PASSWORD_KEYBOARD_CHANGED);
    TEST_ASSERT(state.back_selected == 1);
    TEST_ASSERT(password_keyboard_handle(&state, "NEXT") == PASSWORD_KEYBOARD_CHANGED);
    TEST_ASSERT(state.back_selected == 0 && state.key_selected == WIFI_KEYBOARD_KEYS - 1);
    TEST_ASSERT(password_keyboard_handle(&state, "PREV") == PASSWORD_KEYBOARD_CHANGED);
    TEST_ASSERT(state.back_selected == 1);

    state.back_selected = 0;
    state.key_selected = 30;
    TEST_ASSERT(activate(&state) == PASSWORD_KEYBOARD_CHANGED);
    TEST_ASSERT(state.layout == 1);
    TEST_STRING(password_keyboard_keys(state.layout)[10], "Q");
    TEST_ASSERT(password_keyboard_redraw("PLPA", 0, &state, 0) == PASSWORD_REDRAW_KEYBOARD);
    TEST_ASSERT(activate(&state) == PASSWORD_KEYBOARD_CHANGED);
    TEST_ASSERT(state.layout == 0);
    state.key_selected = 40;
    TEST_ASSERT(activate(&state) == PASSWORD_KEYBOARD_CHANGED);
    TEST_ASSERT(state.layout == 2);
    TEST_STRING(password_keyboard_keys(state.layout)[10], "!");
    TEST_ASSERT(activate(&state) == PASSWORD_KEYBOARD_CHANGED);
    TEST_ASSERT(state.layout == 0);

    state.key_selected = 10;
    TEST_ASSERT(activate(&state) == PASSWORD_KEYBOARD_CHANGED);
    TEST_STRING(state.password, "1q");
    state.key_selected = 41;
    TEST_ASSERT(activate(&state) == PASSWORD_KEYBOARD_CHANGED);
    TEST_STRING(state.password, "1q ");
    state.key_selected = 29;
    TEST_ASSERT(activate(&state) == PASSWORD_KEYBOARD_CHANGED);
    TEST_STRING(state.password, "1q");
    TEST_ASSERT(activate(&state) == PASSWORD_KEYBOARD_CHANGED);
    TEST_STRING(state.password, "1");

    state.key_selected = 42;
    TEST_ASSERT(activate(&state) == PASSWORD_KEYBOARD_CONNECT);
    TEST_ASSERT(password_keyboard_redraw("PLPA", 0, &state, 0) == PASSWORD_REDRAW_FULL);
    TEST_ASSERT(password_keyboard_handle(&state, "LOOP") == PASSWORD_KEYBOARD_BACK);
    TEST_ASSERT(state.password_length == 0 && state.password[0] == '\0');
    state.back_selected = 1;
    TEST_ASSERT(activate(&state) == PASSWORD_KEYBOARD_BACK);
    TEST_ASSERT(state.back_selected == 0);

    TEST_ASSERT(password_keyboard_valid("12345678", 8) == 1);
    TEST_ASSERT(password_keyboard_valid("1234567", 7) == 0);
    TEST_ASSERT(password_keyboard_valid("1234567\n", 8) == 0);
    TEST_ASSERT(password_keyboard_valid("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 64) == 1);
    TEST_ASSERT(password_keyboard_valid("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg", 64) == 0);

    memset(&state, 0, sizeof(state));
    state.key_selected = 0;
    state.password_length = 64;
    memset(state.password, 'a', 64);
    TEST_ASSERT(activate(&state) == PASSWORD_KEYBOARD_TOO_LONG);
    TEST_ASSERT(state.password_length == 64);
    return 0;
}
