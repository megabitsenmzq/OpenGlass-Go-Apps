#include "../test.h"
#include "apps_view.h"
#include "depth_anim.h"
#include "depth_fakes.h"
#include "file_manager_view.h"
#include "menu_view.h"
#include "gallery_view.h"
#include "prerender_view.h"
#include "ui.h"
#include "wifi_view.h"

#include <stdlib.h>

int test_usb_keyboard(void);

#define TEXT_OPS 128

typedef struct {
    char text[256];
    int x;
    int baseline;
    int size;
    int bold;
    uint32_t color;
} TextOp;

static TextOp text_ops[TEXT_OPS];
static int text_op_count;

void __real_draw_text(Display *, Buffer *, Font *, const char *, int, int, uint32_t);
void __real_draw_text_bold(Display *, Buffer *, Font *, const char *, int, int, uint32_t);

static void record_text(Font *font, const char *text, int x, int baseline,
                        uint32_t color, int bold)
{
    TextOp *op;
    if (text_op_count >= TEXT_OPS) {
        fprintf(stderr, "Too many recorded text operations\n");
        abort();
    }
    op = &text_ops[text_op_count++];
    snprintf(op->text, sizeof(op->text), "%s", text);
    op->x = x;
    op->baseline = baseline;
    op->size = font->face->size->metrics.y_ppem;
    op->bold = bold;
    op->color = color;
}

void __wrap_draw_text(Display *display, Buffer *buffer, Font *font,
                      const char *text, int x, int baseline, uint32_t color)
{
    record_text(font, text, x, baseline, color, 0);
    __real_draw_text(display, buffer, font, text, x, baseline, color);
}

void __wrap_draw_text_bold(Display *display, Buffer *buffer, Font *font,
                           const char *text, int x, int baseline, uint32_t color)
{
    record_text(font, text, x, baseline, color, 1);
    __real_draw_text_bold(display, buffer, font, text, x, baseline, color);
}

static uint32_t pixel(const Buffer *buffer, int x, int y)
{
    return *(const uint32_t *)(buffer->pixels + (size_t)y * buffer->stride + (size_t)x * 4);
}

static int buffer_create(Buffer *buffer, int width, int height)
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->stride = (uint32_t)width * 4;
    buffer->size = buffer->stride * (uint64_t)height;
    buffer->pixels = calloc(1, buffer->size);
    buffer->layered = 1;
    return buffer->pixels ? 0 : -1;
}

static int find_text(const char *text)
{
    int i;
    for (i = 0; i < text_op_count; ++i)
        if (strcmp(text_ops[i].text, text) == 0)
            return i;
    return -1;
}

static int test_cpu_drawing(Font *font)
{
    Display display = {0};
    Buffer buffer;
    display.mode.hdisplay = 160;
    display.mode.vdisplay = 90;
    TEST_ASSERT(buffer_create(&buffer, 160, 90) == 0);
    draw_rect(&display, &buffer, -10, -10, 30, 30, 0xff112233);
    TEST_ASSERT(pixel(&buffer, 0, 0) == 0xff112233);
    TEST_ASSERT(pixel(&buffer, 19, 19) == 0xff112233);
    TEST_ASSERT(pixel(&buffer, 20, 20) == 0);
    draw_rect(&display, &buffer, 150, 80, 30, 30, 0xff445566);
    TEST_ASSERT(pixel(&buffer, 159, 89) == 0xff445566);
    TEST_ASSERT(font_text_width(font, "Network") > 0);
    draw_text(&display, &buffer, font, "A", 40, 60, 0xffffffff);
    TEST_ASSERT(pixel(&buffer, 40, 55) != 0 || pixel(&buffer, 41, 55) != 0 ||
                pixel(&buffer, 42, 55) != 0 || pixel(&buffer, 43, 55) != 0);
    free(buffer.pixels);
    return 0;
}

static int test_menu_semantics(Display *display, Font *font, DepthLayer layers[2])
{
    DepthEffect depth = {0};
    SystemInfo info = {
        .version = "OpenGlass Go 1.1.0",
        .serial = "LKG-TEST01", .cpu = "4 x Cortex-A53", .memory = "2.0 GiB",
        .gpu = "Mali-G31", .display = "1440 x 2560", .disk = "16 GiB",
        .os = "Debian", .kernel = "6.18", .ssh = "lkg@lkg.local",
        .ip = "192.0.2.1"
    };
    const char *expected[] = {
        "Gallery", "File Manager", "Desktop (Sway)", "Settings", "Return to TTY", "Power Off"
    };
    const char *expected_with_apps[] = {
        "Apps", "Gallery", "File Manager", "Desktop (Sway)", "Settings", "Return to TTY", "Power Off"
    };
    int i;
    text_op_count = 0;
    menu_view_draw_page(display, font, &depth, layers, PAGE_MAIN, 0, 0, &info);
    TEST_ASSERT(text_op_count == 6);
    for (i = 0; i < 6; ++i) {
        TEST_STRING(text_ops[i].text, expected[i]);
        TEST_ASSERT(text_ops[i].x == 144);
        TEST_ASSERT(text_ops[i].size == 64);
        TEST_ASSERT(text_ops[i].color == (i == 0 ? 0xff101014 : 0xffd8d8d8));
        if (i)
            TEST_ASSERT(text_ops[i].baseline - text_ops[i - 1].baseline == 150);
    }

    text_op_count = 0;
    menu_view_draw_page(display, font, &depth, layers, PAGE_MAIN, 1, 0, &info);
    TEST_ASSERT(text_op_count == 7);
    for (i = 0; i < 7; ++i)
        TEST_STRING(text_ops[i].text, expected_with_apps[i]);

    depth.stereo = 1;
    text_op_count = 0;
    menu_view_draw_page(display, font, &depth, layers, PAGE_SETTINGS, 0, 0, &info);
    TEST_ASSERT(text_op_count == 10);
    TEST_ASSERT(text_ops[0].bold == 1);
    TEST_ASSERT(text_ops[1].bold == 0);
    TEST_ASSERT(find_text("Network") >= 0);
    TEST_ASSERT(find_text("Connected") < 0);

    text_op_count = 0;
    menu_view_draw_page(display, font, &depth, layers, PAGE_INFO, 0, 1, &info);
    TEST_ASSERT(find_text("Version") >= 0);
    i = find_text("OpenGlass Go 1.1.0");
    TEST_ASSERT(i >= 0);
    TEST_ASSERT(text_ops[i].size == 48);
    i = find_text("Version");
    TEST_ASSERT(text_ops[i].size == 32);
    TEST_ASSERT(find_text("Serial") >= 0);
    i = find_text("LKG-TEST01");
    TEST_ASSERT(i >= 0);
    TEST_ASSERT(text_ops[i].size == 48);
    i = find_text("Serial");
    TEST_ASSERT(text_ops[i].size == 32);
    TEST_ASSERT(find_text("CPU") >= 0);
    i = find_text("4 x Cortex-A53");
    TEST_ASSERT(i >= 0);
    TEST_ASSERT(text_ops[i].size == 48);
    i = find_text("CPU");
    TEST_ASSERT(text_ops[i].size == 32);
    TEST_ASSERT(find_text("SSH") >= 0);
    i = find_text("lkg@lkg.local");
    TEST_ASSERT(i >= 0);
    TEST_ASSERT(text_ops[i].size == 48);
    return 0;
}

static int test_focus_dirty_pixels(Display *display, Font *font, DepthLayer layers[2])
{
    DepthEffect depth = {0};
    SystemInfo info = {0};
    uint8_t *saved[2];
    const int row_height = 150;
    const int start_y = (2560 - row_height * 6) / 2;
    int layer;
    int y;
    menu_view_draw_page(display, font, &depth, layers, PAGE_MAIN, 0, 0, &info);
    saved[0] = malloc(layers[0].buffer.size);
    saved[1] = malloc(layers[1].buffer.size);
    TEST_ASSERT(saved[0] && saved[1]);
    memcpy(saved[0], layers[0].buffer.pixels, layers[0].buffer.size);
    memcpy(saved[1], layers[1].buffer.pixels, layers[1].buffer.size);
    menu_view_draw_row(display, font, &depth, layers, PAGE_MAIN, 0, 0, 1, &info);
    menu_view_draw_row(display, font, &depth, layers, PAGE_MAIN, 0, 1, 1, &info);
    for (layer = 0; layer < 2; ++layer) {
        for (y = 0; y < 2560; ++y) {
            int old_row = y >= start_y && y < start_y + row_height;
            int new_row = y >= start_y + row_height && y < start_y + row_height * 2;
            size_t offset = (size_t)y * layers[layer].buffer.stride;
            if (!old_row && !new_row)
                TEST_ASSERT(memcmp(layers[layer].buffer.pixels + offset,
                                   saved[layer] + offset, layers[layer].buffer.stride) == 0);
        }
        TEST_ASSERT(memcmp(layers[layer].buffer.pixels + (size_t)start_y * layers[layer].buffer.stride,
                           saved[layer] + (size_t)start_y * layers[layer].buffer.stride,
                           (size_t)row_height * layers[layer].buffer.stride) != 0);
        TEST_ASSERT(memcmp(layers[layer].buffer.pixels + (size_t)(start_y + row_height) * layers[layer].buffer.stride,
                           saved[layer] + (size_t)(start_y + row_height) * layers[layer].buffer.stride,
                           (size_t)row_height * layers[layer].buffer.stride) != 0);
    }
    for (y = start_y; y < start_y + row_height; ++y) {
        size_t offset = (size_t)y * layers[0].buffer.stride;
        int x;
        for (x = 0; x < 1440; ++x)
            TEST_ASSERT(*(uint32_t *)(layers[0].buffer.pixels + offset + (size_t)x * 4) == 0);
    }
    free(saved[0]);
    free(saved[1]);
    return 0;
}

static int test_wifi_semantics(Display *display, Font *font, DepthLayer layers[2])
{
    DepthEffect depth = {0};
    WifiNetwork network = {0};
    const char *values[] = {
        "TestNet", "192.0.2.2", "192.0.2.1", "00:11", "22:33",
        "-50 dBm", "5180 MHz", "100 Mb/s", "WPA2"
    };
    int index;
    snprintf(network.ssid, sizeof(network.ssid), "TestNet");
    network.ssid_length = 7;
    snprintf(network.display_ssid, sizeof(network.display_ssid), "TestNet");
    network.signal = -50;
    TEST_ASSERT(WIFI_VISIBLE_ROWS == 17);
    TEST_ASSERT(wifi_view_networks_page_start(0) == 0);
    TEST_ASSERT(wifi_view_networks_page_start(1) == 0);
    TEST_ASSERT(wifi_view_networks_page_start(16) == 0);
    TEST_ASSERT(wifi_view_networks_page_start(17) == 16);
    TEST_ASSERT(wifi_view_networks_page_start(32) == 16);
    TEST_ASSERT(wifi_view_networks_page_start(33) == 32);
    TEST_ASSERT(wifi_view_networks_start_y(display, 4) == 980);
    TEST_ASSERT(wifi_view_networks_start_y(display, 17) == 5);
    TEST_ASSERT(wifi_view_networks_row_y(display, 17, 0, 16) == 5);
    TEST_ASSERT(wifi_view_networks_row_y(display, 17, 17, 16) == 155);
    TEST_ASSERT(wifi_view_networks_row_y(display, 17, 32, 16) == 2405);
    TEST_ASSERT(wifi_view_networks_row_y(display, 2, 0, 32) == 1130);
    text_op_count = 0;
    wifi_view_draw_network_row(display, font, &depth, layers, 0, 360, 0, 1, 1,
                               NULL, "TestNet", 7);
    wifi_view_draw_network_row(display, font, &depth, layers, 1, 510, 0, 1, 1,
                               NULL, "TestNet", 7);
    wifi_view_draw_network_row(display, font, &depth, layers, 2, 660, 0, 1, 1,
                               &network, "TestNet", 7);
    TEST_ASSERT(text_op_count == 3);
    TEST_STRING(text_ops[0].text, "Back");
    TEST_STRING(text_ops[1].text, "Scanning...");
    TEST_STRING(text_ops[2].text, "TestNet");
    TEST_ASSERT(text_ops[0].color == 0xff101014);
    TEST_ASSERT(text_ops[1].color == 0xffd8d8d8);
    TEST_ASSERT(find_text("Connected") < 0);
    TEST_ASSERT(find_text("192.0.2.2") < 0);

    text_op_count = 0;
    wifi_view_draw_details_row(display, font, &depth, layers, 1, 150, 1, 1, 1, values);
    wifi_view_draw_details_row(display, font, &depth, layers, 3, 450, 1, 1, 1, values);
    wifi_view_draw_details_row(display, font, &depth, layers, 4, 600, 1, 1, 1, values);
    TEST_ASSERT(find_text("Disconnect") >= 0);
    index = find_text("SSID");
    TEST_ASSERT(index >= 0 && text_ops[index].size == 32);
    index = find_text("TestNet");
    TEST_ASSERT(index >= 0 && text_ops[index].size == 48);
    index = find_text("192.0.2.2");
    TEST_ASSERT(index >= 0 && text_ops[index].size == 48);
    return 0;
}

static int test_wifi_password_and_popup(Display *display, Font *font, DepthLayer layers[2])
{
    DepthEffect depth = {0};
    int x;
    int y;
    int width;
    int height;
    int index;
    text_op_count = 0;
    wifi_view_draw_password_back(display, font, &depth, layers, 1, 1);
    wifi_view_draw_password_field(display, font, layers, "secret");
    wifi_view_draw_password_key(display, font, &depth, layers, 0, 1, 0);
    wifi_view_draw_password_key(display, font, &depth, layers, 10, 0, 1);
    wifi_view_draw_password_key(display, font, &depth, layers, 40, 0, 0);
    TEST_STRING(text_ops[0].text, "Back");
    TEST_ASSERT(text_ops[0].size == 64 && text_ops[0].bold == 0);
    TEST_STRING(text_ops[1].text, "secret");
    TEST_ASSERT(find_text("******") < 0);
    TEST_ASSERT(find_text("1") >= 0);
    TEST_ASSERT(find_text("Q") >= 0);
    index = find_text("#+=");
    TEST_ASSERT(index >= 0 && text_ops[index].size == 42);

    memset(layers[0].buffer.pixels, 0, layers[0].buffer.size);
    memset(layers[1].buffer.pixels, 0, layers[1].buffer.size);
    wifi_view_draw_password_key(display, font, &depth, layers, 30, 0, 1);
    wifi_view_key_geometry(display, 30, &x, &y, &width, &height);
    TEST_ASSERT(pixel(&layers[1].buffer, x + 2, y + 2) == 0xffffffff);

    memset(layers[0].buffer.pixels, 0, layers[0].buffer.size);
    memset(layers[1].buffer.pixels, 0, layers[1].buffer.size);
    text_op_count = 0;
    wifi_view_draw_operation(display, font, &depth, layers, 0, "Connecting...", NULL);
    TEST_ASSERT(find_text("Connecting...") >= 0);
    TEST_ASSERT(pixel(&layers[1].buffer, 10, 1210) == 0xffffffff);
    TEST_ASSERT(pixel(&layers[0].buffer, 10, 1210) == 0);
    TEST_ASSERT(pixel(&layers[0].buffer, 70, 1265) == 0xff000000);
    text_op_count = 0;
    wifi_view_draw_operation(display, font, &depth, layers, 1, "Connection failed", "Authentication timed out");
    index = find_text("Connection failed");
    TEST_ASSERT(index >= 0);
    TEST_ASSERT(text_ops[index].size == 64 && text_ops[index].color == 0xff000000);
    index = find_text("Authentication timed out");
    TEST_ASSERT(index >= 0);
    TEST_ASSERT(text_ops[index].size == 48 && text_ops[index].color == 0xff000000);
    TEST_ASSERT(pixel(&layers[1].buffer, 720, 1280) == 0xffffffff);
    TEST_ASSERT(pixel(&layers[1].buffer, 10, 1360) == 0xffffffff);
    TEST_ASSERT(pixel(&layers[0].buffer, 70, 1265) == 0);
    TEST_ASSERT(pixel(&layers[0].buffer, 56, 1264) == 0xff000000);
    depth.stereo = 1;
    text_op_count = 0;
    wifi_view_draw_operation(display, font, &depth, layers, 1, "Connection failed",
                             "Authentication timed out");
    index = find_text("Connection failed");
    TEST_ASSERT(index >= 0 && text_ops[index].bold);
    index = find_text("Authentication timed out");
    TEST_ASSERT(index >= 0 && text_ops[index].bold);
    return 0;
}

static int test_gallery_cells(Display *display, Font *font, DepthLayer layers[2])
{
    const int x = 15;
    const int y = 21;
    const int arrow_x = x + (THUMB_WIDTH / 2 - 120) / 2;
    const int play_x = x + THUMB_WIDTH / 2 + (THUMB_WIDTH / 2 - 96) / 2 + 8;
    GalleryItem item = {0};
    uint8_t *thumbnail = malloc(THUMB_BYTES);
    uint8_t *reference = malloc(layers[0].buffer.size + layers[1].buffer.size);
    int row;
    int column;
    TEST_ASSERT(thumbnail != NULL);
    TEST_ASSERT(reference != NULL);
    TEST_ASSERT(gallery_default_cursor(0) == 0);
    TEST_ASSERT(gallery_default_cursor(1) == 0);
    TEST_ASSERT(gallery_default_cursor(20) == 0);
    memset(layers[0].buffer.pixels, 0, layers[0].buffer.size);
    memset(layers[1].buffer.pixels, 0, layers[1].buffer.size);
    gallery_view_draw_cell(display, font, layers, 0, 0, NULL, 1, 0);
    TEST_ASSERT(pixel(&layers[0].buffer, arrow_x - 2, y + THUMB_HEIGHT / 2) == 0xffe8e8e8u);
    TEST_ASSERT(pixel(&layers[0].buffer, arrow_x - 1, y + THUMB_HEIGHT / 2) == 0);
    TEST_ASSERT(pixel(&layers[0].buffer, arrow_x, y + THUMB_HEIGHT / 2) == 0);
    TEST_ASSERT(pixel(&layers[1].buffer, arrow_x, y + THUMB_HEIGHT / 2) == 0x101014);
    TEST_ASSERT(pixel(&layers[1].buffer, play_x, y + THUMB_HEIGHT / 2) == 0xe8e8e8);
    gallery_view_draw_cell(display, font, layers, 0, 0, NULL, 1, 1);
    TEST_ASSERT(pixel(&layers[0].buffer, play_x - 2, y + THUMB_HEIGHT / 2) == 0xffe8e8e8u);
    TEST_ASSERT(pixel(&layers[0].buffer, play_x - 1, y + THUMB_HEIGHT / 2) == 0);
    TEST_ASSERT(pixel(&layers[0].buffer, play_x, y + THUMB_HEIGHT / 2) == 0);
    TEST_ASSERT(pixel(&layers[1].buffer, play_x, y + THUMB_HEIGHT / 2) == 0x101014);
    TEST_ASSERT(pixel(&layers[1].buffer, arrow_x, y + THUMB_HEIGHT / 2) == 0xe8e8e8);

    for (row = 0; row < THUMB_HEIGHT; ++row) {
        for (column = 0; column < THUMB_WIDTH; ++column) {
            uint8_t *pixel_data = thumbnail + ((size_t)row * THUMB_WIDTH + column) * 4;
            pixel_data[0] = (uint8_t)column;
            pixel_data[1] = (uint8_t)row;
            pixel_data[2] = 80;
            pixel_data[3] = 255;
        }
    }
    item.name = "Generated 01";
    item.thumbnail = thumbnail;
    item.thumbnail_state = 1;
    text_op_count = 0;
    gallery_view_draw_cell(display, font, layers, 1, 1, &item, 0, 0);
    TEST_ASSERT(text_op_count == 1);
    TEST_ASSERT(strncmp(text_ops[0].text, "Generated", 9) == 0);
    TEST_ASSERT(text_ops[0].size == 43 && text_ops[0].color == 0xffffff);
    TEST_ASSERT(pixel(&layers[0].buffer, 495, 21) == 0xff500000);
    text_op_count = 0;
    gallery_view_draw_cell(display, font, layers, 1, 0, &item, 0, 0);
    TEST_ASSERT(text_op_count == 0);
    gallery_view_move_cell(display, font, layers, 1, 1, &item);
    memcpy(reference, layers[0].buffer.pixels, layers[0].buffer.size);
    memcpy(reference + layers[0].buffer.size, layers[1].buffer.pixels, layers[1].buffer.size);
    memset(layers[0].buffer.pixels, 0, layers[0].buffer.size);
    memset(layers[1].buffer.pixels, 0, layers[1].buffer.size);
    gallery_view_draw_cell(display, font, layers, 0, 0, NULL, 1, 1);
    gallery_view_draw_cell(display, font, layers, 1, 1, &item, 0, 0);
    TEST_ASSERT(memcmp(reference, layers[0].buffer.pixels, layers[0].buffer.size) == 0);
    TEST_ASSERT(memcmp(reference + layers[0].buffer.size,
                       layers[1].buffer.pixels, layers[1].buffer.size) == 0);
    gallery_view_move_cell(display, font, layers, 1, 0, &item);
    memcpy(reference, layers[0].buffer.pixels, layers[0].buffer.size);
    memcpy(reference + layers[0].buffer.size, layers[1].buffer.pixels, layers[1].buffer.size);
    memset(layers[0].buffer.pixels, 0, layers[0].buffer.size);
    memset(layers[1].buffer.pixels, 0, layers[1].buffer.size);
    gallery_view_draw_cell(display, font, layers, 0, 0, NULL, 1, 1);
    gallery_view_draw_cell(display, font, layers, 1, 0, &item, 0, 0);
    TEST_ASSERT(memcmp(reference, layers[0].buffer.pixels, layers[0].buffer.size) == 0);
    TEST_ASSERT(memcmp(reference + layers[0].buffer.size,
                       layers[1].buffer.pixels, layers[1].buffer.size) == 0);
    {
        GalleryItem folder = {0};
        folder.name = "Folder";
        folder.is_directory = 1;
        folder.cover_path = "generated-cover";
        folder.progress = 0.5f;
        gallery_view_draw_cell(display, font, layers, 2, 0, &folder, 0, 0);
        TEST_ASSERT(pixel(&layers[1].buffer, 995, 751) == 0xe8e8e8);
        folder.thumbnail = thumbnail;
        folder.thumbnail_state = 1;
        gallery_view_draw_cell(display, font, layers, 2, 0, &folder, 0, 0);
        TEST_ASSERT(pixel(&layers[1].buffer, 995, 751) != 0xe8e8e8);
        TEST_ASSERT(pixel(&layers[1].buffer, 995, 751) != 0x404048);
    }
    gallery_view_draw_cell_anim(display, font, layers, 1, 0, &item);
    TEST_ASSERT(pixel(&layers[0].buffer, 495, 21) == 0xff500000);
    TEST_ASSERT(pixel(&layers[0].buffer, 487, 13) == 0);
    TEST_ASSERT(pixel(&layers[1].buffer, 487, 13) == 0x101014);
    gallery_view_draw_special_anim(display, layers, 1, 0);
    TEST_ASSERT(pixel(&layers[0].buffer, play_x - 2, y + THUMB_HEIGHT / 2) == 0xffe8e8e8u);
    TEST_ASSERT(pixel(&layers[0].buffer, play_x - 1, y + THUMB_HEIGHT / 2) == 0);
    TEST_ASSERT(pixel(&layers[0].buffer, play_x, y + THUMB_HEIGHT / 2) == 0);
    TEST_ASSERT(pixel(&layers[0].buffer, arrow_x, y + THUMB_HEIGHT / 2) == 0xe8e8e8u);
    TEST_ASSERT(pixel(&layers[0].buffer, x + THUMB_WIDTH / 2 - 9, y + THUMB_HEIGHT / 2) == 0);
    TEST_ASSERT(pixel(&layers[1].buffer, arrow_x, y + THUMB_HEIGHT / 2) == 0x101014);
    free(reference);
    free(thumbnail);
    return 0;
}

static int test_prerender_states(Display *display, Font *font, DepthLayer layers[2])
{
    PrerenderViewState previous = {0};
    PrerenderViewState next = {
        .done = 1, .total = 4, .item_progress = 0.25,
        .frame_current = 10, .frame_total = 40, .show_frames = 1,
        .current_path = "/home/lkg/data/media/generated.mp4"
    };
    PrerenderViewDirty dirty = prerender_view_dirty(&previous, &next, 1);
    uint8_t *saved;
    TEST_ASSERT(dirty.full && dirty.overall && dirty.current && dirty.path);
    text_op_count = 0;
    prerender_view_draw(display, font, layers, &next, dirty);
    TEST_ASSERT(find_text("Overall") >= 0);
    TEST_ASSERT(find_text("1 / 4") >= 0);
    TEST_ASSERT(find_text("Pre-render") >= 0);
    TEST_ASSERT(find_text("10 / 40 frames") >= 0);
    TEST_ASSERT(find_text(next.current_path) >= 0);

    saved = malloc(layers[1].buffer.size);
    TEST_ASSERT(saved != NULL);
    memcpy(saved, layers[1].buffer.pixels, layers[1].buffer.size);
    previous = next;
    next.done = 2;
    next.item_progress = 0.5;
    next.frame_current = 20;
    dirty = prerender_view_dirty(&previous, &next, 0);
    TEST_ASSERT(!dirty.full && dirty.overall && dirty.current && !dirty.path);
    text_op_count = 0;
    prerender_view_draw(display, font, layers, &next, dirty);
    TEST_ASSERT(find_text("2 / 4") >= 0);
    TEST_ASSERT(find_text("20 / 40 frames") >= 0);
    TEST_ASSERT(find_text(next.current_path) < 0);
    TEST_ASSERT(memcmp(layers[1].buffer.pixels + (size_t)2440 * layers[1].buffer.stride,
                       saved + (size_t)2440 * layers[1].buffer.stride,
                       (size_t)120 * layers[1].buffer.stride) == 0);
    free(saved);
    previous = next;
    next.finished = 1;
    dirty = prerender_view_dirty(&previous, &next, 0);
    text_op_count = 0;
    prerender_view_draw(display, font, layers, &next, dirty);
    TEST_ASSERT(find_text("Complete") >= 0);
    return 0;
}

static int test_file_manager(Display *display, Font *font, DepthLayer layers[2])
{
    DepthEffect depth = {0};
    int index;
    text_op_count = 0;
    file_manager_view_draw(display, font, &depth, layers, "192.0.2.1");
    TEST_ASSERT(find_text("Back") >= 0);
    index = find_text("http://192.0.2.1:5000/");
    TEST_ASSERT(index >= 0 && text_ops[index].size == 48);
    TEST_ASSERT(find_text("Unable to generate QR code") < 0);
    memset(layers[0].buffer.pixels, 0, layers[0].buffer.size);
    memset(layers[1].buffer.pixels, 0, layers[1].buffer.size);
    text_op_count = 0;
    file_manager_view_draw(display, font, &depth, layers, "Unavailable");
    index = find_text("Please connect to the internet");
    TEST_ASSERT(index >= 0 && text_ops[index].size == 48);
    TEST_ASSERT(find_text("http://192.0.2.1:5000/") < 0);
    TEST_ASSERT(find_text("Unable to generate QR code") < 0);
    return 0;
}

static int test_apps_view(Display *display, Font *font, DepthLayer layers[2])
{
    DepthEffect depth = {0};
    ExternalApp app = {0};
    int idx;
    app.name = "Hello World";
    TEST_ASSERT(APPS_VISIBLE_ROWS == 17);
    TEST_ASSERT(apps_view_page_start(0) == 0);
    TEST_ASSERT(apps_view_page_start(1) == 0);
    TEST_ASSERT(apps_view_page_start(16) == 0);
    TEST_ASSERT(apps_view_page_start(17) == 16);
    TEST_ASSERT(apps_view_start_y(display, 2) == 1130);
    TEST_ASSERT(apps_view_row_y(display, 2, 0, 0) == 1130);
    TEST_ASSERT(apps_view_row_y(display, 2, 1, 0) == 1280);

    depth.stereo = 1;
    memset(layers[0].buffer.pixels, 0, layers[0].buffer.size);
    memset(layers[1].buffer.pixels, 0, layers[1].buffer.size);
    text_op_count = 0;
    apps_view_draw_row(display, font, &depth, layers, 0, 1130, 1, 1, NULL);
    apps_view_draw_row(display, font, &depth, layers, 1, 1280, 1, 1, &app);
    TEST_ASSERT(text_op_count == 2);
    TEST_STRING(text_ops[0].text, "Back");
    TEST_ASSERT(text_ops[0].color == 0xffd8d8d8 && text_ops[0].bold == 0);
    TEST_STRING(text_ops[1].text, "Hello World");
    TEST_ASSERT(text_ops[1].color == 0xff101014 && text_ops[1].size == 64 && text_ops[1].bold == 1);

    text_op_count = 0;
    apps_view_draw_text_input(display, font, &depth, layers, "Edit Greeting",
                              "World", 0, 25, 1, 1);
    TEST_ASSERT(find_text("Back") >= 0);
    idx = find_text("Edit");
    TEST_ASSERT(idx >= 0 && text_ops[idx].size == 48);
    idx = find_text("Edit Greeting");
    TEST_ASSERT(idx >= 0 && text_ops[idx].size == 64);
    TEST_ASSERT(find_text("Hello World") < 0);
    TEST_ASSERT(find_text("Input") >= 0);
    TEST_ASSERT(find_text("World") >= 0);

    text_op_count = 0;
    apps_view_draw_status(display, font, &depth, layers, 1, "App exited with error",
                          "Hello World (exit 1)");
    TEST_ASSERT(find_text("App exited with error") >= 0);
    TEST_ASSERT(find_text("Hello World (exit 1)") >= 0);
    return 0;
}

static int test_utf8_app_font(Display *display, Font *menu_font, DepthLayer layers[2],
                              const char *font_path)
{
    static const unsigned int codepoints[] = {
        0x3053, 0x3053, 0x306b, 0x3057, 0x304b, 0x306a,
        0x3044, 0x3063, 0x3066, 0x8a00, 0x3063, 0x3066
    };
    Font app_font;
    int expected_width = 0;
    size_t i;
    TEST_ASSERT(font_open(&app_font, font_path, 64) == 0);
    app_font.utf8 = 1;
    for (i = 0; i < sizeof(codepoints) / sizeof(codepoints[0]); ++i) {
        FT_UInt glyph = FT_Get_Char_Index(app_font.face, codepoints[i]);
        TEST_ASSERT(FT_Load_Glyph(app_font.face, glyph, FT_LOAD_DEFAULT) == 0);
        expected_width += app_font.face->glyph->advance.x >> 6;
    }
    TEST_ASSERT(font_text_width(&app_font, "ここにしかないって言って") == expected_width);
    text_op_count = 0;
    draw_text_fit(display, &layers[0].buffer, &app_font, "ここここ", 0, 100, 0xffffffffu,
                  font_text_width(&app_font, "こ") + font_text_width(&app_font, "..."), 0);
    TEST_ASSERT(text_op_count == 1);
    TEST_STRING(text_ops[0].text, "こ...");
    TEST_ASSERT(menu_font->utf8 == 0);
    font_close(&app_font);
    return 0;
}

static int layer_row_has_nonzero(const Buffer *buffer, int y, int height)
{
    int row;
    for (row = y; row < y + height; ++row) {
        const uint32_t *line = (const uint32_t *)(buffer->pixels + (size_t)row * buffer->stride);
        int column;
        for (column = 0; column < 1440; ++column) {
            if (line[column] != 0)
                return 1;
        }
    }
    return 0;
}

static int test_menu_and_wifi_animations(Display *display, Font *font, DepthLayer layers[2])
{
    DepthEffect depth = {0};
    SystemInfo info = {0};
    ExternalApp app = {0};
    WifiNetwork net = {0};
    const char *values[] = {
        "Net", "10.0.0.2", "10.0.0.1", "00:11", "22:33",
        "-50", "5180", "100", "WPA2"
    };
    DepthAnim anim;
    DepthAnimRect rect_a = { 0, 500, 1440, 650, 0.0f };
    DepthAnimRect rect_b = { 0, 650, 1440, 800, 0.0f };
    DepthAnimRect rect_c = { 0, 1100, 1440, 1250, 0.0f };
    int finished = 0;
    int kx, ky, kw, kh;
    depth.stereo = 1;
    depth.front_disparity = 35.0f;
    depth.back_disparity = -28.0f;
    app.name = "Music";
    snprintf(net.display_ssid, sizeof(net.display_ssid), "TestNet");
    snprintf(net.ssid, sizeof(net.ssid), "TestNet");
    net.ssid_length = 7;
    net.signal = -50;

    memset(layers[0].buffer.pixels, 0, layers[0].buffer.size);
    memset(layers[1].buffer.pixels, 0, layers[1].buffer.size);
    text_op_count = 0;
    menu_view_draw_row_anim(display, font, &depth, layers, PAGE_MAIN, 0, 0, 0, &info);
    menu_view_draw_row_anim(display, font, &depth, layers, PAGE_MAIN, 0, 1, 1, &info);
    TEST_ASSERT(text_op_count == 2);
    TEST_ASSERT(text_ops[0].color == 0xffd8d8d8 && text_ops[0].bold == 0);
    TEST_ASSERT(text_ops[1].color == 0xff101014 && text_ops[1].bold == 1);
    {
        int row_height = (int)display->mode.vdisplay / 17;
        int start_y = ((int)display->mode.vdisplay - row_height * 6) / 2;
        TEST_ASSERT(layer_row_has_nonzero(&layers[0].buffer, start_y, row_height));
        TEST_ASSERT(!layer_row_has_nonzero(&layers[1].buffer, start_y, row_height * 2));
    }

    text_op_count = 0;
    apps_view_draw_row_anim(display, font, &depth, layers, 0, 1130, 0, NULL);
    apps_view_draw_row_anim(display, font, &depth, layers, 1, 1280, 1, &app);
    TEST_ASSERT(text_op_count == 2);
    TEST_ASSERT(text_ops[0].color == 0xffd8d8d8 && text_ops[0].bold == 0);
    TEST_ASSERT(text_ops[1].color == 0xff101014 && text_ops[1].bold == 1);
    TEST_ASSERT(!layer_row_has_nonzero(&layers[1].buffer, 1130, APPS_ROW_HEIGHT * 2));

    text_op_count = 0;
    wifi_view_draw_network_row_anim(display, font, &depth, layers, 2, 360, 0, 0,
                                     &net, "Other", 5);
    wifi_view_draw_details_row_anim(display, font, &depth, layers, 1, 510, 1, 0, values);
    TEST_ASSERT(text_op_count == 2);
    TEST_ASSERT(text_ops[0].color == 0xffd8d8d8 && text_ops[0].bold == 0);
    TEST_ASSERT(text_ops[1].color == 0xff101014 && text_ops[1].bold == 1);
    TEST_ASSERT(layer_row_has_nonzero(&layers[0].buffer, 360, WIFI_ROW_HEIGHT));
    TEST_ASSERT(!layer_row_has_nonzero(&layers[1].buffer, 360, WIFI_ROW_HEIGHT * 2));

    wifi_view_draw_password_key_anim(display, font, &depth, layers, 30, 0, 1);
    wifi_view_key_geometry(display, 30, &kx, &ky, &kw, &kh);
    TEST_ASSERT(pixel(&layers[0].buffer, kx + 2, ky + 2) == 0xffffffffu);
    TEST_ASSERT(pixel(&layers[1].buffer, kx + 2, ky + 2) == 0u);

    wifi_view_draw_password_back_anim(display, font, &depth, layers, 1);
    TEST_ASSERT(pixel(&layers[0].buffer, 10, 25 + 10) == 0xffe8e8e8u);
    TEST_ASSERT(pixel(&layers[1].buffer, 10, 25 + 10) == 0u);
    wifi_view_draw_password_back_anim(display, font, &depth, layers, 0);
    TEST_ASSERT(pixel(&layers[0].buffer, 10, 25 + 10) == 0u);
    TEST_ASSERT(pixel(&layers[1].buffer, 10, 25 + 10) == 0u);

    memset(&anim, 0, sizeof(anim));
    depth_anim_set_clips(&anim, display, &depth, &rect_a, &rect_b);
    TEST_ASSERT(anim.clip_count == 1);
    TEST_ASSERT(anim.clip_y[0] == 500 && anim.clip_h[0] == 300);
    depth_anim_set_clips(&anim, display, &depth, &rect_a, &rect_c);
    TEST_ASSERT(anim.clip_count == 2);

    depth_fake_reset();
    depth_anim_start(&anim, display, &depth, 0, 1, &rect_a, &rect_b,
                     650, 792, 99, depth.front_disparity, depth.back_disparity);
    TEST_ASSERT(depth_anim_step(&anim, display, &depth, layers, NULL, &finished) == 0);
    TEST_ASSERT(!finished);
    TEST_ASSERT(depth_fake.anim_region_calls == 1 && depth_fake.present_calls == 1);
    TEST_ASSERT(depth_fake.last_bar_top == 650 && depth_fake.last_bar_bottom == 792);
    anim.started -= 1.0;
    TEST_ASSERT(depth_anim_step(&anim, display, &depth, layers, NULL, &finished) == 0);
    TEST_ASSERT(finished);
    TEST_ASSERT(anim.cur_old_disparity == depth.back_disparity);
    TEST_ASSERT(anim.cur_new_disparity == depth.front_disparity);
    return 0;
}

int main(void)
{
    const char *font_path = getenv("LKG_TEST_FONT");
    Display display = {0};
    DepthLayer layers[2];
    Font font;
    int tests_run = 0;
    int tests_failed = 0;
    int result = 0;
    if (!font_path)
        font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    if (font_open(&font, font_path, 64) != 0) {
        fprintf(stderr, "Unable to open test font: %s\n", font_path);
        return 1;
    }
    display.mode.hdisplay = 1440;
    display.mode.vdisplay = 2560;
    memset(layers, 0, sizeof(layers));
    if (buffer_create(&layers[0].buffer, 1440, 2560) != 0 ||
        buffer_create(&layers[1].buffer, 1440, 2560) != 0) {
        fprintf(stderr, "Unable to allocate offscreen layers\n");
        return 1;
    }
    ++tests_run;
    if (test_cpu_drawing(&font) != 0)
        ++tests_failed;
    ++tests_run;
    if (test_menu_semantics(&display, &font, layers) != 0)
        ++tests_failed;
    ++tests_run;
    if (test_focus_dirty_pixels(&display, &font, layers) != 0)
        ++tests_failed;
    ++tests_run;
    if (test_wifi_semantics(&display, &font, layers) != 0)
        ++tests_failed;
    ++tests_run;
    if (test_wifi_password_and_popup(&display, &font, layers) != 0)
        ++tests_failed;
    ++tests_run;
    if (test_gallery_cells(&display, &font, layers) != 0)
        ++tests_failed;
    ++tests_run;
    if (test_prerender_states(&display, &font, layers) != 0)
        ++tests_failed;
    ++tests_run;
    if (test_file_manager(&display, &font, layers) != 0)
        ++tests_failed;
    ++tests_run;
    if (test_depth_frame() != 0)
        ++tests_failed;
    ++tests_run;
    if (test_usb_keyboard() != 0)
        ++tests_failed;
    ++tests_run;
    if (test_apps_view(&display, &font, layers) != 0)
        ++tests_failed;
    ++tests_run;
    if (test_utf8_app_font(&display, &font, layers, font_path) != 0)
        ++tests_failed;
    ++tests_run;
    if (test_menu_and_wifi_animations(&display, &font, layers) != 0)
        ++tests_failed;
    free(layers[0].buffer.pixels);
    free(layers[1].buffer.pixels);
    font_close(&font);
    if (tests_failed) {
        fprintf(stderr, "%d/%d UI tests failed\n", tests_failed, tests_run);
        result = 1;
    } else {
        printf("All %d UI tests passed\n", tests_run);
    }
    return result;
}
