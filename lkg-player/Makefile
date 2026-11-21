CC ?= cc
PKG_CONFIG ?= pkg-config
TARGET ?= lkg-player
PACKAGES := gbm egl glesv2 libdrm libpng16 libjpeg libheif gstreamer-app-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0 libswscale
SOURCES := src/main.c src/error.c src/process.c src/config.c src/media.c src/gstreamer.c src/png.c src/jpeg.c src/heic.c src/preprocess.c src/renderer.c src/slideshow.c
OBJECTS := $(SOURCES:.c=.o) src/menu_thumbnail.o src/menu_cache_key.o
DEPS := $(OBJECTS:.o=.d)

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -I../lkg-menu/src -MMD -MP
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -pthread $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -lm -pthread

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/menu_thumbnail.o: ../lkg-menu/src/thumbnail.c ../lkg-menu/src/thumbnail.h src/player.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ ../lkg-menu/src/thumbnail.c

src/menu_cache_key.o: ../lkg-menu/src/cache_key.c ../lkg-menu/src/cache_key.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ ../lkg-menu/src/cache_key.c

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/local/bin/lkg-player

-include $(DEPS)

clean:
	rm -f $(TARGET) $(OBJECTS) $(DEPS)
