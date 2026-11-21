CC ?= cc
TARGET ?= lkg-mcu

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

.PHONY: all clean

all: $(TARGET)

$(TARGET): src/main.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/main.c

clean:
	rm -f $(TARGET)
