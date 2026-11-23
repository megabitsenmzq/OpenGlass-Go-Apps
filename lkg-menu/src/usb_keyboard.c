#include "usb_keyboard.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static long long monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int bit_is_set(const unsigned long *bits, int bit)
{
    return (bits[bit / (8 * sizeof(unsigned long))] &
            (1UL << (bit % (8 * sizeof(unsigned long))))) != 0;
}

static int set_grab(UsbKeyboard *keyboard, int grabbed)
{
    int error;
    if (keyboard->fd < 0)
        return 0;
    if (ioctl(keyboard->fd, EVIOCGRAB, grabbed) >= 0)
        return 0;
    error = errno;
    usb_keyboard_close(keyboard);
    keyboard->next_scan_ms = 0;
    {
        long long now = monotonic_ms();
        if (now >= keyboard->next_error_ms) {
            fprintf(stderr, "Unable to %s USB keyboard: %s\n",
                    grabbed ? "grab" : "release", strerror(error));
            keyboard->next_error_ms = now + 5000;
        }
    }
    errno = error;
    return -1;
}

static int open_keyboard(UsbKeyboard *keyboard)
{
    DIR *directory = opendir("/dev/input");
    struct dirent *entry;
    if (!directory)
        return -1;
    while ((entry = readdir(directory)) != NULL) {
        char path[512];
        unsigned long bits[16] = {0};
        int fd;
        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0 &&
            bit_is_set(bits, KEY_A) && bit_is_set(bits, KEY_Z) &&
            bit_is_set(bits, KEY_0) && bit_is_set(bits, KEY_ENTER)) {
            keyboard->fd = fd;
            keyboard->shift = 0;
            keyboard->caps_lock = 0;
            if (keyboard->exclusive && set_grab(keyboard, 1) != 0)
                continue;
            closedir(directory);
            fprintf(stderr, "USB keyboard connected: %s\n", path);
            return 0;
        }
        close(fd);
    }
    closedir(directory);
    return -1;
}

void usb_keyboard_init(UsbKeyboard *keyboard)
{
    memset(keyboard, 0, sizeof(*keyboard));
    keyboard->fd = -1;
    keyboard->exclusive = 1;
}

void usb_keyboard_close(UsbKeyboard *keyboard)
{
    if (keyboard->fd >= 0)
        close(keyboard->fd);
    keyboard->fd = -1;
    keyboard->shift = 0;
    keyboard->caps_lock = 0;
    keyboard->next_scan_ms = 0;
}

void usb_keyboard_set_exclusive(UsbKeyboard *keyboard, int exclusive)
{
    keyboard->exclusive = exclusive;
    if (keyboard->fd < 0) {
        if (exclusive)
            (void)open_keyboard(keyboard);
        return;
    }
    (void)set_grab(keyboard, exclusive);
}

static char shifted_character(int code)
{
    static const char characters[KEY_MAX + 1] = {
        [KEY_1] = '!', [KEY_2] = '@', [KEY_3] = '#', [KEY_4] = '$',
        [KEY_5] = '%', [KEY_6] = '^', [KEY_7] = '&', [KEY_8] = '*',
        [KEY_9] = '(', [KEY_0] = ')', [KEY_MINUS] = '_', [KEY_EQUAL] = '+',
        [KEY_LEFTBRACE] = '{', [KEY_RIGHTBRACE] = '}', [KEY_SEMICOLON] = ':',
        [KEY_APOSTROPHE] = '"', [KEY_GRAVE] = '~', [KEY_BACKSLASH] = '|',
        [KEY_COMMA] = '<', [KEY_DOT] = '>', [KEY_SLASH] = '?'
    };
    return code >= 0 && code <= KEY_MAX ? characters[code] : 0;
}

UsbKeyEvent usb_keyboard_translate(UsbKeyboard *keyboard, const struct input_event *event)
{
    static const char letters[KEY_MAX + 1] = {
        [KEY_Q] = 'q', [KEY_W] = 'w', [KEY_E] = 'e', [KEY_R] = 'r',
        [KEY_T] = 't', [KEY_Y] = 'y', [KEY_U] = 'u', [KEY_I] = 'i',
        [KEY_O] = 'o', [KEY_P] = 'p', [KEY_A] = 'a', [KEY_S] = 's',
        [KEY_D] = 'd', [KEY_F] = 'f', [KEY_G] = 'g', [KEY_H] = 'h',
        [KEY_J] = 'j', [KEY_K] = 'k', [KEY_L] = 'l', [KEY_Z] = 'z',
        [KEY_X] = 'x', [KEY_C] = 'c', [KEY_V] = 'v', [KEY_B] = 'b',
        [KEY_N] = 'n', [KEY_M] = 'm'
    };
    static const char characters[KEY_MAX + 1] = {
        [KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3', [KEY_4] = '4',
        [KEY_5] = '5', [KEY_6] = '6', [KEY_7] = '7', [KEY_8] = '8',
        [KEY_9] = '9', [KEY_0] = '0', [KEY_MINUS] = '-', [KEY_EQUAL] = '=',
        [KEY_LEFTBRACE] = '[', [KEY_RIGHTBRACE] = ']', [KEY_SEMICOLON] = ';',
        [KEY_APOSTROPHE] = '\'', [KEY_GRAVE] = '`', [KEY_BACKSLASH] = '\\',
        [KEY_COMMA] = ',', [KEY_DOT] = '.', [KEY_SLASH] = '/', [KEY_SPACE] = ' ',
        [KEY_KP0] = '0', [KEY_KP1] = '1', [KEY_KP2] = '2', [KEY_KP3] = '3',
        [KEY_KP4] = '4', [KEY_KP5] = '5', [KEY_KP6] = '6', [KEY_KP7] = '7',
        [KEY_KP8] = '8', [KEY_KP9] = '9', [KEY_KPDOT] = '.',
        [KEY_KPMINUS] = '-', [KEY_KPPLUS] = '+', [KEY_KPSLASH] = '/',
        [KEY_KPASTERISK] = '*'
    };
    UsbKeyEvent result = { USB_KEY_NONE, 0, 0 };
    int code;
    if (event->type != EV_KEY)
        return result;
    code = event->code;
    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
        if (event->value == 1)
            ++keyboard->shift;
        else if (event->value == 0 && keyboard->shift > 0)
            --keyboard->shift;
        return result;
    }
    if (code == KEY_CAPSLOCK) {
        if (event->value == 1)
            keyboard->caps_lock = !keyboard->caps_lock;
        return result;
    }
    if (event->value != 1)
        return result;
    if (code == KEY_LEFT || code == KEY_UP) {
        result.type = USB_KEY_PREV;
        result.code = code;
    } else if (code == KEY_RIGHT || code == KEY_DOWN) {
        result.type = USB_KEY_NEXT;
        result.code = code;
    } else if (code == KEY_BACKSPACE) {
        result.type = USB_KEY_BACKSPACE;
        result.code = code;
    } else if (code == KEY_ENTER || code == KEY_KPENTER) {
        result.type = USB_KEY_ENTER;
        result.code = code;
    } else if (code == KEY_ESC) {
        result.type = USB_KEY_ESCAPE;
        result.code = code;
    } else if (code >= 0 && code <= KEY_MAX && letters[code]) {
        result.type = USB_KEY_CHARACTER;
        result.character = letters[code];
        if (keyboard->caps_lock ^ (keyboard->shift > 0))
            result.character = (char)(result.character - 'a' + 'A');
        result.code = code;
    } else if (keyboard->shift > 0 && shifted_character(code)) {
        result.type = USB_KEY_CHARACTER;
        result.character = shifted_character(code);
        result.code = code;
    } else if (code >= 0 && code <= KEY_MAX && characters[code]) {
        result.type = USB_KEY_CHARACTER;
        result.character = characters[code];
        result.code = code;
    }
    return result;
}

UsbKeyEvent usb_keyboard_next(UsbKeyboard *keyboard)
{
    UsbKeyEvent result = { USB_KEY_NONE, 0, 0 };
    if (keyboard->fd < 0) {
        long long now = monotonic_ms();
        if (now < keyboard->next_scan_ms)
            return result;
        keyboard->next_scan_ms = now + 1000;
        if (open_keyboard(keyboard) != 0)
            return result;
    }
    for (;;) {
        struct input_event event;
        ssize_t count = read(keyboard->fd, &event, sizeof(event));
        if (count == (ssize_t)sizeof(event)) {
            result = usb_keyboard_translate(keyboard, &event);
            if (result.type != USB_KEY_NONE)
                return result;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return result;
        usb_keyboard_close(keyboard);
        keyboard->next_scan_ms = 0;
        result.type = USB_KEY_NONE;
        result.character = 0;
        result.code = 0;
        return result;
    }
}

int usb_keyboard_command(UsbKeyEvent event, char *command, size_t size)
{
    const char *name = NULL;
    if (!command || !size)
        return -1;
    if (event.type == USB_KEY_PREV)
        name = "PREV";
    else if (event.type == USB_KEY_NEXT)
        name = "NEXT";
    else if (event.type == USB_KEY_ENTER)
        name = "PLPA";
    else if (event.type == USB_KEY_ESCAPE)
        name = "LOOP";
    if (!name)
        return 0;
    if (snprintf(command, size, "%s", name) >= (int)size)
        return -1;
    return 1;
}

int usb_keyboard_next_command(UsbKeyboard *keyboard, char *command, size_t size)
{
    return usb_keyboard_command(usb_keyboard_next(keyboard), command, size);
}

void usb_keyboard_prepare_command(char *command, int button_reversed)
{
    if (!button_reversed)
        return;
    if (strcmp(command, "PREV") == 0)
        strcpy(command, "NEXT");
    else if (strcmp(command, "NEXT") == 0)
        strcpy(command, "PREV");
}
