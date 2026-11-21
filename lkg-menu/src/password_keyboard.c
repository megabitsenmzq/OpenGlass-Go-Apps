#include "password_keyboard.h"

#include <ctype.h>
#include <string.h>

static const char *const keyboard_lower[WIFI_KEYBOARD_KEYS] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", NULL,
    NULL, "z", "x", "c", "v", "b", "n", "m", ".", "-",
    NULL, NULL, NULL
};

static const char *const keyboard_upper[WIFI_KEYBOARD_KEYS] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", NULL,
    NULL, "Z", "X", "C", "V", "B", "N", "M", ".", "-",
    NULL, NULL, NULL
};

static const char *const keyboard_symbols[WIFI_KEYBOARD_KEYS] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
    "!", "@", "#", "$", "%", "^", "&", "*", "(", ")",
    "~", "`", "-", "_", "=", "+", "/", "\\", "|", NULL,
    "[", "]", "{", "}", "<", ">", "?", ":", ";", "\"",
    NULL, NULL, NULL
};

const char *const *password_keyboard_keys(int layout)
{
    if (layout == 0)
        return keyboard_lower;
    if (layout == 1)
        return keyboard_upper;
    return keyboard_symbols;
}

int password_keyboard_valid(const char *password, size_t length)
{
    size_t i;
    if (length == 64) {
        for (i = 0; i < length; ++i)
            if (!isxdigit((unsigned char)password[i]))
                return 0;
        return 1;
    }
    if (length < 8 || length > 63)
        return 0;
    for (i = 0; i < length; ++i)
        if ((unsigned char)password[i] < 32 || (unsigned char)password[i] > 126)
            return 0;
    return 1;
}

static PasswordKeyboardAction leave_password(PasswordKeyboardState *state)
{
    memset(state->password, 0, sizeof(state->password));
    state->password_length = 0;
    state->back_selected = 0;
    return PASSWORD_KEYBOARD_BACK;
}

PasswordKeyboardAction password_keyboard_handle(PasswordKeyboardState *state,
                                                const char *command)
{
    const char *const *keys = password_keyboard_keys(state->layout);
    if (strcmp(command, "PREV") == 0) {
        if (state->back_selected) {
            state->back_selected = 0;
            state->key_selected = 0;
        } else if (state->key_selected == WIFI_KEYBOARD_KEYS - 1) {
            state->back_selected = 1;
        } else {
            ++state->key_selected;
        }
        return PASSWORD_KEYBOARD_CHANGED;
    }
    if (strcmp(command, "NEXT") == 0) {
        if (state->back_selected) {
            state->back_selected = 0;
            state->key_selected = WIFI_KEYBOARD_KEYS - 1;
        } else if (state->key_selected == 0) {
            state->back_selected = 1;
        } else {
            --state->key_selected;
        }
        return PASSWORD_KEYBOARD_CHANGED;
    }
    if (strcmp(command, "LOOP") == 0)
        return leave_password(state);
    if (strcmp(command, "PLPA") != 0)
        return PASSWORD_KEYBOARD_NONE;
    if (state->back_selected)
        return leave_password(state);
    if (state->key_selected == 29) {
        if (state->password_length)
            --state->password_length;
        state->password[state->password_length] = '\0';
    } else if (state->key_selected == 30 && state->layout != 2) {
        state->layout = state->layout == 1 ? 0 : 1;
    } else if (state->key_selected == 40) {
        state->layout = state->layout == 2 ? 0 : 2;
    } else if (state->key_selected == 41) {
        if (state->password_length < 64)
            state->password[state->password_length++] = ' ';
        state->password[state->password_length] = '\0';
    } else if (state->key_selected == 42) {
        return PASSWORD_KEYBOARD_CONNECT;
    } else {
        size_t length = strlen(keys[state->key_selected]);
        if (state->password_length + length > 64)
            return PASSWORD_KEYBOARD_TOO_LONG;
        memcpy(state->password + state->password_length, keys[state->key_selected], length);
        state->password_length += length;
        state->password[state->password_length] = '\0';
    }
    return PASSWORD_KEYBOARD_CHANGED;
}

PasswordRedraw password_keyboard_redraw(const char *command, int previous_back,
                                        const PasswordKeyboardState *state, int busy)
{
    if (strcmp(command, "PREV") == 0 || strcmp(command, "NEXT") == 0)
        return previous_back == state->back_selected ? PASSWORD_REDRAW_FOCUS : PASSWORD_REDRAW_BACK;
    if (strcmp(command, "PLPA") == 0 && !busy && !state->back_selected) {
        if (state->key_selected == 30 || state->key_selected == 40)
            return PASSWORD_REDRAW_KEYBOARD;
        if (state->key_selected != 42)
            return PASSWORD_REDRAW_PASSWORD;
    }
    return PASSWORD_REDRAW_FULL;
}
