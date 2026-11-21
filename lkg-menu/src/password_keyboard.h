#ifndef LKG_MENU_PASSWORD_KEYBOARD_H
#define LKG_MENU_PASSWORD_KEYBOARD_H

#include <stddef.h>

#define WIFI_KEYBOARD_COLUMNS 10
#define WIFI_KEYBOARD_ROWS 5
#define WIFI_KEYBOARD_KEYS 43

typedef struct {
    int layout;
    int key_selected;
    int back_selected;
    char password[65];
    size_t password_length;
} PasswordKeyboardState;

typedef enum {
    PASSWORD_KEYBOARD_NONE = 0,
    PASSWORD_KEYBOARD_CHANGED,
    PASSWORD_KEYBOARD_BACK,
    PASSWORD_KEYBOARD_CONNECT,
    PASSWORD_KEYBOARD_TOO_LONG
} PasswordKeyboardAction;

typedef enum {
    PASSWORD_REDRAW_FULL = 1,
    PASSWORD_REDRAW_FOCUS,
    PASSWORD_REDRAW_PASSWORD,
    PASSWORD_REDRAW_KEYBOARD,
    PASSWORD_REDRAW_BACK
} PasswordRedraw;

const char *const *password_keyboard_keys(int layout);
int password_keyboard_valid(const char *password, size_t length);
PasswordKeyboardAction password_keyboard_handle(PasswordKeyboardState *state,
                                                const char *command);
PasswordRedraw password_keyboard_redraw(const char *command, int previous_back,
                                        const PasswordKeyboardState *state, int busy);

#endif
