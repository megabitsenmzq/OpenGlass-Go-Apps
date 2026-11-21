#include "serial_parser.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern volatile sig_atomic_t running;

void serial_parser_feed(SerialParser *parser, const char *input, size_t size)
{
    size_t i;
    for (i = 0; i < size; ++i) {
        char character = input[i];
        if (character == '\a') {
            parser->length = 0;
            continue;
        }
        if (character == '\r')
            continue;
        if (character == '\n') {
            if (parser->length) {
                char parsed[16];
                parser->data[parser->length] = '\0';
                parser->length = 0;
                if (sscanf(parser->data, "{\"cmd\":\"%15[^\"]\"", parsed) == 1 &&
                    parser->count < SERIAL_QUEUE_SIZE) {
                    size_t tail = (parser->head + parser->count) % SERIAL_QUEUE_SIZE;
                    snprintf(parser->commands[tail], sizeof(parser->commands[tail]), "%s", parsed);
                    ++parser->count;
                }
            }
            continue;
        }
        if (parser->length < sizeof(parser->data) - 1)
            parser->data[parser->length++] = character;
        else
            parser->length = 0;
    }
}

int serial_parser_take(SerialParser *parser, char *command, size_t size)
{
    if (!parser->count)
        return 0;
    snprintf(command, size, "%s", parser->commands[parser->head]);
    parser->head = (parser->head + 1) % SERIAL_QUEUE_SIZE;
    --parser->count;
    return 1;
}

int serial_parser_next_with_fd(int fd, SerialParser *parser, char *command, size_t size,
                               int timeout, int other_fd)
{
    struct pollfd fds[2];
    if (serial_parser_take(parser, command, size))
        return 1;
    for (;;) {
        char input[512];
        ssize_t count;
        fds[0].fd = fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = other_fd;
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        if (poll(fds, 2, timeout) < 0) {
            if (errno == EINTR)
                return running ? 0 : -1;
            return -1;
        }
        if (fds[1].revents)
            return 0;
        if (!(fds[0].revents & POLLIN))
            return fds[0].revents & (POLLERR | POLLHUP | POLLNVAL) ? -1 : 0;
        count = read(fd, input, sizeof(input));
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            return -1;
        }
        if (count > 0)
            serial_parser_feed(parser, input, (size_t)count);
        if (serial_parser_take(parser, command, size))
            return 1;
    }
}

int serial_parser_next(int fd, SerialParser *parser, char *command, size_t size, int timeout)
{
    return serial_parser_next_with_fd(fd, parser, command, size, timeout, -1);
}

void normalize_button_command(char *command, int button_reversed)
{
    if (button_reversed)
        return;
    if (strcmp(command, "PREV") == 0)
        strcpy(command, "NEXT");
    else if (strcmp(command, "NEXT") == 0)
        strcpy(command, "PREV");
}
