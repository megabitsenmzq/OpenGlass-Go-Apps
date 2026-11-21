#include "../test.h"
#include "usb_keyboard.h"

#include <stdarg.h>
#include <string.h>

static int grab_requests[16];
static int grab_request_count;

int __real_ioctl(int fd, unsigned long request, ...);

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list arguments;
    void *argument;
    int result;
    va_start(arguments, request);
    if (request == EVIOCGRAB) {
        int grabbed = va_arg(arguments, int);
        va_end(arguments);
        if (grab_request_count >= (int)(sizeof(grab_requests) / sizeof(grab_requests[0])))
            return -1;
        grab_requests[grab_request_count++] = grabbed;
        return 0;
    }
    argument = va_arg(arguments, void *);
    va_end(arguments);
    result = __real_ioctl(fd, request, argument);
    return result;
}

static struct input_event key_event(int code, int value)
{
    struct input_event event;
    memset(&event, 0, sizeof(event));
    event.type = EV_KEY;
    event.code = code;
    event.value = value;
    return event;
}

int test_usb_keyboard(void)
{
    UsbKeyboard keyboard;
    UsbKeyEvent result;
    struct input_event event;
    char command[16];
    usb_keyboard_init(&keyboard);

    TEST_ASSERT(keyboard.exclusive == 1);
    keyboard.fd = 1000;
    usb_keyboard_set_exclusive(&keyboard, 0);
    TEST_ASSERT(grab_request_count == 1 && grab_requests[0] == 0);
    usb_keyboard_set_exclusive(&keyboard, 1);
    TEST_ASSERT(grab_request_count == 2 && grab_requests[1] == 1);
    keyboard.fd = -1;

    event = key_event(KEY_A, 1);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(result.type == USB_KEY_CHARACTER && result.character == 'a');
    event = key_event(KEY_LEFTSHIFT, 1);
    (void)usb_keyboard_translate(&keyboard, &event);
    event = key_event(KEY_LEFTSHIFT, 2);
    (void)usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(keyboard.shift == 1);
    event = key_event(KEY_A, 1);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(result.type == USB_KEY_CHARACTER && result.character == 'A');
    event = key_event(KEY_1, 1);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(result.type == USB_KEY_CHARACTER && result.character == '!');
    event = key_event(KEY_LEFTSHIFT, 0);
    (void)usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(keyboard.shift == 0);
    event = key_event(KEY_1, 1);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(result.type == USB_KEY_CHARACTER && result.character == '1');
    event = key_event(KEY_CAPSLOCK, 1);
    (void)usb_keyboard_translate(&keyboard, &event);
    event = key_event(KEY_A, 1);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(result.character == 'A');
    event = key_event(KEY_A, 0);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(result.type == USB_KEY_NONE);
    event = key_event(KEY_1, 1);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(result.type == USB_KEY_CHARACTER && result.character == '1');
    event = key_event(KEY_BACKSPACE, 1);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(result.type == USB_KEY_BACKSPACE);
    event = key_event(KEY_KPENTER, 1);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(result.type == USB_KEY_ENTER);
    event = key_event(KEY_ESC, 1);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(result.type == USB_KEY_ESCAPE);
    event = key_event(KEY_LEFT, 1);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(result.type == USB_KEY_PREV);
    TEST_ASSERT(usb_keyboard_command(result, command, sizeof(command)) == 1);
    TEST_ASSERT(strcmp(command, "PREV") == 0);
    event = key_event(KEY_UP, 1);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(result.type == USB_KEY_PREV);
    event = key_event(KEY_RIGHT, 1);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(result.type == USB_KEY_NEXT);
    TEST_ASSERT(usb_keyboard_command(result, command, sizeof(command)) == 1);
    TEST_ASSERT(strcmp(command, "NEXT") == 0);
    event = key_event(KEY_DOWN, 1);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(result.type == USB_KEY_NEXT);
    TEST_ASSERT(usb_keyboard_command(result, command, sizeof(command)) == 1);
    TEST_ASSERT(strcmp(command, "NEXT") == 0);
    event = key_event(KEY_ENTER, 1);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(usb_keyboard_command(result, command, sizeof(command)) == 1);
    TEST_ASSERT(strcmp(command, "PLPA") == 0);
    event = key_event(KEY_ESC, 1);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(usb_keyboard_command(result, command, sizeof(command)) == 1);
    TEST_ASSERT(strcmp(command, "LOOP") == 0);
    event = key_event(KEY_ESC, 2);
    TEST_ASSERT(usb_keyboard_translate(&keyboard, &event).type == USB_KEY_NONE);
    event = key_event(KEY_ENTER, 2);
    TEST_ASSERT(usb_keyboard_translate(&keyboard, &event).type == USB_KEY_NONE);
    event = key_event(KEY_LEFT, 2);
    TEST_ASSERT(usb_keyboard_translate(&keyboard, &event).type == USB_KEY_NONE);
    event = key_event(KEY_A, 1);
    result = usb_keyboard_translate(&keyboard, &event);
    TEST_ASSERT(usb_keyboard_command(result, command, sizeof(command)) == 0);
    strcpy(command, "PREV");
    usb_keyboard_prepare_command(command, 0);
    TEST_ASSERT(strcmp(command, "PREV") == 0);
    usb_keyboard_prepare_command(command, 1);
    TEST_ASSERT(strcmp(command, "NEXT") == 0);
    usb_keyboard_prepare_command(command, 0);
    TEST_ASSERT(strcmp(command, "NEXT") == 0);
    usb_keyboard_prepare_command(command, 1);
    TEST_ASSERT(strcmp(command, "PREV") == 0);
    usb_keyboard_close(&keyboard);
    return 0;
}
