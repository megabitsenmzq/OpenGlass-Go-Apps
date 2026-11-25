CC ?= cc
HOST_CC ?= cc
PKG_CONFIG ?= pkg-config
TARGET ?= lkg-menu
PACKAGES := gbm egl glesv2 libdrm freetype2 libpng16 libjpeg libqrencode libheif

APP_SOURCES := src/main.c src/gallery.c src/gallery_playback.c src/gallery_prerender.c \
	src/gallery_view.c src/wifi.c \
	src/cache_key.c src/thumbnail.c \
	src/depth.c src/depth_anim.c src/depth_cache.c src/depth_frame.c src/depth_gpu.c src/file_manager_view.c \
	src/media_scan.c src/menu_model.c src/menu_view.c src/usb_keyboard.c \
	src/password_keyboard.c src/prerender_view.c src/serial_parser.c src/text_fit.c src/ui.c \
	src/volume.c src/wifi_control.c src/wifi_model.c src/wifi_ui.c src/wifi_view.c \
	src/apps_scan.c src/apps_view.c src/apps_host.c
APP_HEADERS := $(wildcard src/*.h) $(wildcard sdk/*.h)
UNIT_TEST_SOURCES := tests/test_main.c tests/test_serial_parser.c \
	tests/test_depth_cache.c tests/test_wifi_model.c tests/test_password_keyboard.c \
	tests/test_menu_model.c tests/test_media_scan.c tests/test_cache_key.c \
	tests/test_apps_scan.c tests/test_app_protocol.c \
	src/serial_parser.c src/depth_cache.c src/cache_key.c \
	src/wifi_model.c src/password_keyboard.c src/menu_model.c src/media_scan.c \
	src/apps_scan.c sdk/lkg_app.c
UNIT_TEST_BINARY := build/tests/unit-tests

CPPFLAGS += -D_DEFAULT_SOURCE -Isdk
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wshadow $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -lm
HOST_CFLAGS ?= -O0 -g
HOST_CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wshadow

.PHONY: all clean install test test-ui check

all: $(TARGET)

$(TARGET): $(APP_SOURCES) $(APP_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(APP_SOURCES) $(LDLIBS)

$(UNIT_TEST_BINARY): $(UNIT_TEST_SOURCES) tests/test.h src/serial_parser.h \
	src/depth_cache.h src/wifi_model.h src/password_keyboard.h src/menu_model.h \
	src/apps_scan.h sdk/lkg_app.h sdk/lkg_app_protocol.h
	mkdir -p $(dir $@)
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) -Isrc -Isdk -o $@ $(UNIT_TEST_SOURCES) -lpthread

test: $(UNIT_TEST_BINARY)
	./$(UNIT_TEST_BINARY)

test-ui:
	$(MAKE) -C tests/ui test

check: test test-ui

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/local/bin/lkg-menu

clean:
	rm -f $(TARGET)
	rm -rf build
	$(MAKE) -C tests/ui clean
