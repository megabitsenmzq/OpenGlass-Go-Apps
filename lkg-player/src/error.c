#include "player.h"

#include <stdarg.h>
#include <stdio.h>

void set_error(char *error, size_t size, const char *format, ...)
{
    va_list args;
    if (!error || size == 0)
        return;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}
