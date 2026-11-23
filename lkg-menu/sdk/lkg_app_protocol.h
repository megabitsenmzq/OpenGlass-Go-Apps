#ifndef LKG_APP_PROTOCOL_H
#define LKG_APP_PROTOCOL_H

#include <stdint.h>

#define LKG_APP_API_VERSION 1u

#define LKG_APP_FD_ENV "LKG_APP_FD"
#define LKG_APP_DIR_ENV "LKG_APP_DIR"
#define LKG_APP_NAME_ENV "LKG_APP_NAME"
#define LKG_DATA_DIR_ENV "LKG_DATA_DIR"
#define LKG_CALIBRATION_ENV "LKG_CALIBRATION"

#define LKG_APP_MSG_MAX_PAYLOAD 4096u
#define LKG_APP_MAX_PRESENT_RECTS 64u
#define LKG_APP_TEXT_INPUT_MAX 64u

/* Standard menu font sizes (in pixels at 1440x2560) */
#define LKG_FONT_SIZE_LARGE 64
#define LKG_FONT_SIZE_KEY 56
#define LKG_FONT_SIZE_MEDIUM 48
#define LKG_FONT_SIZE_CAPTION 43
#define LKG_FONT_SIZE_SMALL 32

/* Standard 3D menu layer depths (horizontal disparity in pixels) */
#define LKG_DEPTH_SUBTLE_POP_OUT 35.0f
#define LKG_DEPTH_SUBTLE_RECESSED -28.0f
#define LKG_DEPTH_DEEP_POP_OUT 70.0f
#define LKG_DEPTH_DEEP_RECESSED -56.0f
#define LKG_DEPTH_POP_OUT LKG_DEPTH_SUBTLE_POP_OUT
#define LKG_DEPTH_RECESSED LKG_DEPTH_SUBTLE_RECESSED
#define LKG_DEPTH_SCREEN 0.0f

/* Layer indices passed to drawing functions */
#define LKG_LAYER_FRONT 0
#define LKG_LAYER_BACK 1

/* Standard menu ARGB colors (0xAARRGGBB) */
#define LKG_COLOR_TRANSPARENT 0x00000000u
#define LKG_COLOR_BG 0xff101014u
#define LKG_COLOR_BAR 0xffe8e8e8u
#define LKG_COLOR_TEXT_ACTIVE 0xff101014u
#define LKG_COLOR_TEXT 0xffd8d8d8u
#define LKG_COLOR_TEXT_BRIGHT 0xffffffffu
#define LKG_COLOR_TEXT_MUTED 0xffa0a0a0u
#define LKG_COLOR_SURFACE 0xff202028u
#define LKG_COLOR_SURFACE_LIGHT 0xff303038u
#define LKG_COLOR_TRACK 0xff404048u
#define LKG_COLOR_WARNING 0xffffcc00u

typedef enum {
    LKG_RENDER_HOSTED = 0,
    LKG_RENDER_DRM = 1
} LkgRenderMode;

typedef enum {
    LKG_EVENT_NONE = 0,
    LKG_EVENT_BUTTON,
    LKG_EVENT_KEY,
    LKG_EVENT_QUIT
} LkgEventType;

typedef enum {
    LKG_NAV_NONE = 0,
    LKG_NAV_PREV,
    LKG_NAV_NEXT,
    LKG_NAV_CONFIRM,
    LKG_NAV_BACK
} LkgNav;

typedef enum {
    LKG_KEY_NONE = 0,
    LKG_KEY_CHAR,
    LKG_KEY_BACKSPACE,
    LKG_KEY_ENTER,
    LKG_KEY_ESCAPE,
    LKG_KEY_UP,
    LKG_KEY_DOWN,
    LKG_KEY_LEFT,
    LKG_KEY_RIGHT
} LkgKey;

typedef struct {
    uint32_t type;
    uint32_t nav;
    uint32_t key;
    uint8_t character;
    uint8_t reserved;
    uint16_t keycode;
} LkgEvent;

typedef struct {
    uint32_t api_version;
    uint32_t render_mode;
    int32_t screen_width;
    int32_t screen_height;
    int32_t row_height;
    int32_t margin;
    int32_t top_margin;
    int32_t bar_gap;
    int32_t menu_3d;
    int32_t button_reversed;
    int32_t font_large;
    int32_t font_medium;
    int32_t font_caption;
    int32_t font_small;
    int32_t font_key;
    float depth_front;
    float depth_back;
    float depth_zero;
    uint32_t color_bg;
    uint32_t color_bar;
    uint32_t color_text_active;
    uint32_t color_text;
    uint32_t color_text_bright;
    uint32_t color_text_muted;
    uint32_t color_surface;
    uint32_t color_surface_light;
    uint32_t layer_stride;
    uint64_t layer_size;
} LkgAppInfo;

typedef struct {
    int32_t width;
    int32_t ascender;
    int32_t descender;
    int32_t height;
} LkgTextMetrics;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t bar_top;
    int32_t bar_bottom;
} LkgRectUpdate;

typedef enum {
    LKG_MSG_HELLO = 1,
    LKG_MSG_HELLO_REPLY,
    LKG_MSG_CLEAR,
    LKG_MSG_SET_DEPTH,
    LKG_MSG_SET_STEREO,
    LKG_MSG_DRAW_RECT,
    LKG_MSG_DRAW_TEXT,
    LKG_MSG_DRAW_BACK_ARROW,
    LKG_MSG_MEASURE_TEXT,
    LKG_MSG_MEASURE_REPLY,
    LKG_MSG_SYNC,
    LKG_MSG_PRESENT,
    LKG_MSG_TEXT_INPUT,
    LKG_MSG_TEXT_INPUT_REPLY,
    LKG_MSG_REPLY,
    LKG_MSG_EVENT
} LkgMsgType;

typedef struct {
    uint32_t type;
    uint32_t length;
    uint32_t seq;
} LkgMsgHeader;

typedef struct {
    uint32_t api_version;
} LkgHelloPayload;

typedef struct {
    int32_t layer;
} LkgClearPayload;

typedef struct {
    float front_disparity;
    float back_disparity;
} LkgSetDepthPayload;

typedef struct {
    int32_t stereo;
} LkgSetStereoPayload;

typedef struct {
    int32_t layer;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint32_t color;
} LkgDrawRectPayload;

typedef struct {
    int32_t layer;
    int32_t x;
    int32_t baseline;
    int32_t font_size;
    uint32_t color;
    int32_t max_width;
    int32_t bold;
    uint32_t text_len;
} LkgDrawTextPayload;

typedef struct {
    int32_t layer;
    int32_t x;
    int32_t center_y;
    uint32_t color;
} LkgDrawBackArrowPayload;

typedef struct {
    int32_t font_size;
    uint32_t text_len;
} LkgMeasureTextPayload;

typedef struct {
    int32_t status;
    LkgTextMetrics metrics;
} LkgMeasureReplyPayload;

typedef struct {
    int32_t full;
    uint32_t rect_count;
} LkgPresentPayload;

typedef struct {
    uint32_t title_len;
    uint32_t initial_len;
} LkgTextInputPayload;

typedef struct {
    int32_t accepted;
    uint32_t text_len;
} LkgTextInputReplyPayload;

typedef struct {
    int32_t status;
} LkgReplyPayload;

#endif
