CC ?= cc
PKG_CONFIG ?= pkg-config
ROOT := ../..
BINARY := $(ROOT)/build/tests/ui-tests
SOURCES := test_ui.c $(ROOT)/src/ui.c $(ROOT)/src/menu_view.c $(ROOT)/src/menu_model.c \
	$(ROOT)/src/wifi_view.c $(ROOT)/src/wifi_model.c $(ROOT)/src/password_keyboard.c \
	$(ROOT)/src/gallery_view.c $(ROOT)/src/prerender_view.c $(ROOT)/src/file_manager_view.c \
	$(ROOT)/src/depth_frame.c $(ROOT)/src/usb_keyboard.c $(ROOT)/src/text_fit.c depth_fakes.c test_depth_frame.c \
	test_usb_keyboard.c
PACKAGES := freetype2 libdrm libqrencode

CPPFLAGS += -D_DEFAULT_SOURCE -I$(ROOT)/src
CFLAGS ?= -O0 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wshadow $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
LDFLAGS += -Wl,--wrap=draw_text -Wl,--wrap=draw_text_bold -Wl,--wrap=ioctl
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -lm

.PHONY: test clean

test: $(BINARY)
	$(BINARY)

$(BINARY): $(SOURCES) $(ROOT)/src/ui.h $(ROOT)/src/menu_view.h $(ROOT)/src/menu_model.h
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

clean:
	rm -f $(BINARY)
