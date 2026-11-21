#ifndef LKG_MENU_SERIAL_PARSER_H
#define LKG_MENU_SERIAL_PARSER_H

#include <stddef.h>

#define SERIAL_QUEUE_SIZE 16
#define SERIAL_RESULT_POWER_OFF 1
#define SERIAL_RESULT_USB_DESKTOP 2

typedef struct {
    char data[512];
    size_t length;
    char commands[SERIAL_QUEUE_SIZE][16];
    size_t head;
    size_t count;
} SerialParser;

void serial_parser_feed(SerialParser *parser, const char *input, size_t size);
int serial_parser_take(SerialParser *parser, char *command, size_t size);
int serial_parser_next(int fd, SerialParser *parser, char *command, size_t size, int timeout);
int serial_parser_next_with_fd(int fd, SerialParser *parser, char *command, size_t size,
                               int timeout, int other_fd);
void normalize_button_command(char *command, int button_reversed);

#endif
