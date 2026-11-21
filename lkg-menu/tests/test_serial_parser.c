#include "test.h"
#include "serial_parser.h"

#include <unistd.h>

static void feed(SerialParser *parser, const char *text)
{
    serial_parser_feed(parser, text, strlen(text));
}

int test_serial_parser(void)
{
    SerialParser parser = {0};
    char command[16];
    const char *expected[] = { "NEXT", "PREV", "PLPA" };
    int i;
    feed(&parser, "{\"cmd\":\"NEXT\"}\n{\"cmd\":\"PREV\"}\n{\"cmd\":\"PLPA\"}\n");
    for (i = 0; i < 3; ++i) {
        TEST_ASSERT(serial_parser_take(&parser, command, sizeof(command)) == 1);
        TEST_STRING(command, expected[i]);
    }
    TEST_ASSERT(serial_parser_take(&parser, command, sizeof(command)) == 0);

    feed(&parser, "{\"cmd\":\"NE");
    TEST_ASSERT(parser.count == 0);
    feed(&parser, "XT\"}\r\n");
    TEST_ASSERT(serial_parser_take(&parser, command, sizeof(command)) == 1);
    TEST_STRING(command, "NEXT");

    feed(&parser, "{\"cmd\":\"PREV\"}\a{\"cmd\":\"PLPA\"}\ninvalid\n");
    TEST_ASSERT(serial_parser_take(&parser, command, sizeof(command)) == 1);
    TEST_STRING(command, "PLPA");
    TEST_ASSERT(serial_parser_take(&parser, command, sizeof(command)) == 0);

    memset(&parser, 0, sizeof(parser));
    for (i = 0; i < SERIAL_QUEUE_SIZE; ++i) {
        char input[64];
        snprintf(input, sizeof(input), "{\"cmd\":\"CMD%02d\"}\n", i);
        feed(&parser, input);
    }
    for (i = 0; i < 8; ++i)
        TEST_ASSERT(serial_parser_take(&parser, command, sizeof(command)) == 1);
    for (i = 0; i < 8; ++i) {
        char input[64];
        snprintf(input, sizeof(input), "{\"cmd\":\"CMD%02d\"}\n", i);
        feed(&parser, input);
    }
    for (i = 8; i < SERIAL_QUEUE_SIZE + 8; ++i) {
        snprintf(command, sizeof(command), "CMD%02d", i % SERIAL_QUEUE_SIZE);
        {
            char actual[16];
            TEST_ASSERT(serial_parser_take(&parser, actual, sizeof(actual)) == 1);
            TEST_STRING(actual, command);
        }
    }

    {
        int descriptors[2];
        static const char input[] = "{\"cmd\":\"NEXT\"}\n{\"cmd\":\"PREV\"}\n";
        TEST_ASSERT(pipe(descriptors) == 0);
        TEST_ASSERT(write(descriptors[1], input, sizeof(input) - 1) == (ssize_t)(sizeof(input) - 1));
        TEST_ASSERT(serial_parser_next(descriptors[0], &parser, command, sizeof(command), 0) == 1);
        TEST_STRING(command, "NEXT");
        TEST_ASSERT(serial_parser_next(descriptors[0], &parser, command, sizeof(command), 0) == 1);
        TEST_STRING(command, "PREV");
        close(descriptors[0]);
        close(descriptors[1]);
    }

    {
        int serial[2];
        int keyboard[2];
        TEST_ASSERT(pipe(serial) == 0 && pipe(keyboard) == 0);
        TEST_ASSERT(write(keyboard[1], "x", 1) == 1);
        TEST_ASSERT(serial_parser_next_with_fd(serial[0], &parser, command, sizeof(command),
                                               1000, keyboard[0]) == 0);
        close(serial[0]);
        close(serial[1]);
        close(keyboard[0]);
        close(keyboard[1]);
    }

    snprintf(command, sizeof(command), "PREV");
    normalize_button_command(command, 0);
    TEST_STRING(command, "NEXT");
    normalize_button_command(command, 1);
    TEST_STRING(command, "NEXT");
    return 0;
}
