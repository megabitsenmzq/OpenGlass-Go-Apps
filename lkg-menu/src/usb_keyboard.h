#ifndef LKG_MENU_USB_KEYBOARD_H
#define LKG_MENU_USB_KEYBOARD_H

#include <linux/input.h>
#include <stddef.h>

typedef struct {
    int fd;
    int shift;
    int caps_lock;
    int exclusive;
    long long next_scan_ms;
    long long next_error_ms;
} UsbKeyboard;

typedef enum {
    USB_KEY_NONE = 0,
    USB_KEY_CHARACTER,
    USB_KEY_BACKSPACE,
    USB_KEY_ENTER,
    USB_KEY_ESCAPE,
    USB_KEY_PREV,
    USB_KEY_NEXT
} UsbKeyType;

typedef struct {
    UsbKeyType type;
    char character;
    int code;
} UsbKeyEvent;

void usb_keyboard_init(UsbKeyboard *keyboard);
void usb_keyboard_close(UsbKeyboard *keyboard);
void usb_keyboard_set_exclusive(UsbKeyboard *keyboard, int exclusive);
UsbKeyEvent usb_keyboard_translate(UsbKeyboard *keyboard, const struct input_event *event);
UsbKeyEvent usb_keyboard_next(UsbKeyboard *keyboard);
int usb_keyboard_command(UsbKeyEvent event, char *command, size_t size);
int usb_keyboard_next_command(UsbKeyboard *keyboard, char *command, size_t size);
void usb_keyboard_prepare_command(char *command, int button_reversed);

#endif
