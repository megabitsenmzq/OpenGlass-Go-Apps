#if !defined(_POSIX_C_SOURCE) && !defined(_GNU_SOURCE) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "lkg_app.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define LKG_APP_EVENT_QUEUE_SIZE 64

struct LkgApp {
    int fd;
    LkgAppInfo info;
    uint32_t *layers[2];
    int layer_fds[2];
    uint32_t next_seq;
    LkgEvent events[LKG_APP_EVENT_QUEUE_SIZE];
    size_t event_head;
    size_t event_count;
    LkgTextMetrics large_metrics;
    int have_large_metrics;
};

static int send_all(int fd, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t sent = 0;
    while (sent < size) {
        ssize_t count = send(fd, bytes + sent, size - sent, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (count == 0)
            return -1;
        sent += (size_t)count;
    }
    return 0;
}

static int recv_all(int fd, void *data, size_t size)
{
    uint8_t *bytes = (uint8_t *)data;
    size_t received = 0;
    while (received < size) {
        ssize_t count = recv(fd, bytes + received, size - received, 0);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (count == 0) {
            errno = EPIPE;
            return -1;
        }
        received += (size_t)count;
    }
    return 0;
}

static int send_message(LkgApp *app, uint32_t type,
                        const void *part1, size_t len1,
                        const void *part2, size_t len2,
                        const void *part3, size_t len3,
                        const void *part4, size_t len4)
{
    LkgMsgHeader header;
    size_t total = len1 + len2 + len3 + len4;
    if (!app || app->fd < 0 || total > LKG_APP_MSG_MAX_PAYLOAD) {
        errno = EINVAL;
        return -1;
    }
    header.type = type;
    header.length = (uint32_t)total;
    header.seq = ++app->next_seq;
    if (send_all(app->fd, &header, sizeof(header)) != 0)
        return -1;
    if (len1 && send_all(app->fd, part1, len1) != 0)
        return -1;
    if (len2 && send_all(app->fd, part2, len2) != 0)
        return -1;
    if (len3 && send_all(app->fd, part3, len3) != 0)
        return -1;
    if (len4 && send_all(app->fd, part4, len4) != 0)
        return -1;
    return 0;
}

static void push_event(LkgApp *app, const LkgEvent *event)
{
    size_t tail;
    if (app->event_count >= LKG_APP_EVENT_QUEUE_SIZE)
        return;
    tail = (app->event_head + app->event_count) % LKG_APP_EVENT_QUEUE_SIZE;
    app->events[tail] = *event;
    ++app->event_count;
}

static int pop_event(LkgApp *app, LkgEvent *event)
{
    if (!app->event_count)
        return 0;
    *event = app->events[app->event_head];
    app->event_head = (app->event_head + 1) % LKG_APP_EVENT_QUEUE_SIZE;
    --app->event_count;
    return 1;
}

static int recv_frame(LkgApp *app, LkgMsgHeader *header, void *payload, size_t capacity)
{
    uint8_t discard[256];
    size_t remaining;
    if (recv_all(app->fd, header, sizeof(*header)) != 0)
        return -1;
    if (header->length > LKG_APP_MSG_MAX_PAYLOAD) {
        errno = EPROTO;
        return -1;
    }
    if (header->length <= capacity) {
        if (header->length && recv_all(app->fd, payload, header->length) != 0)
            return -1;
        return 0;
    }
    if (capacity && recv_all(app->fd, payload, capacity) != 0)
        return -1;
    remaining = header->length - capacity;
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
        if (recv_all(app->fd, discard, chunk) != 0)
            return -1;
        remaining -= chunk;
    }
    return 0;
}

static int wait_reply(LkgApp *app, uint32_t expected_type, LkgMsgHeader *header,
                      void *payload, size_t capacity)
{
    for (;;) {
        if (recv_frame(app, header, payload, capacity) != 0)
            return -1;
        if (header->type == LKG_MSG_EVENT) {
            if (header->length >= sizeof(LkgEvent) && capacity >= sizeof(LkgEvent)) {
                LkgEvent ev;
                memcpy(&ev, payload, sizeof(ev));
                push_event(app, &ev);
            }
            continue;
        }
        if (header->type == expected_type)
            return 0;
        errno = EPROTO;
        return -1;
    }
}

static int receive_hello_reply(LkgApp *app)
{
    LkgMsgHeader header;
    uint8_t *buffer = (uint8_t *)&header;
    size_t received = 0;
    int got_fds = 0;
    union {
        struct cmsghdr header;
        char bytes[CMSG_SPACE(sizeof(int) * 2)];
    } control;

    while (received < sizeof(header)) {
        struct msghdr msg;
        struct iovec iov;
        ssize_t count;
        memset(&msg, 0, sizeof(msg));
        memset(&control, 0, sizeof(control));
        iov.iov_base = buffer + received;
        iov.iov_len = sizeof(header) - received;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        if (!got_fds) {
            msg.msg_control = control.bytes;
            msg.msg_controllen = sizeof(control.bytes);
        }
        count = recvmsg(app->fd, &msg, 0);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (count == 0) {
            errno = EPIPE;
            return -1;
        }
        if (!got_fds) {
            struct cmsghdr *cmsg;
            for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
                    cmsg->cmsg_len >= CMSG_LEN(sizeof(int) * 2)) {
                    memcpy(app->layer_fds, CMSG_DATA(cmsg), sizeof(app->layer_fds));
                    got_fds = 1;
                    break;
                }
            }
        }
        received += (size_t)count;
    }

    if (header.type != LKG_MSG_HELLO_REPLY || header.length != sizeof(app->info)) {
        errno = EPROTO;
        return -1;
    }
    if (recv_all(app->fd, &app->info, sizeof(app->info)) != 0)
        return -1;
    if (app->info.api_version != LKG_APP_API_VERSION) {
        errno = EPROTO;
        return -1;
    }
    if (app->info.render_mode == LKG_RENDER_HOSTED) {
        int i;
        if (!got_fds || app->layer_fds[0] < 0 || app->layer_fds[1] < 0 ||
            app->info.layer_size == 0) {
            errno = EPROTO;
            return -1;
        }
        for (i = 0; i < 2; ++i) {
            void *mapped = mmap(NULL, (size_t)app->info.layer_size,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                app->layer_fds[i], 0);
            if (mapped == MAP_FAILED)
                return -1;
            app->layers[i] = (uint32_t *)mapped;
        }
    }
    return 0;
}

LkgApp *lkg_app_open_fd(int fd)
{
    LkgApp *app;
    LkgHelloPayload hello;
    if (fd < 0) {
        errno = EINVAL;
        return NULL;
    }
    app = calloc(1, sizeof(*app));
    if (!app)
        return NULL;
    app->fd = fd;
    app->layer_fds[0] = -1;
    app->layer_fds[1] = -1;
    hello.api_version = LKG_APP_API_VERSION;
    if (send_message(app, LKG_MSG_HELLO, &hello, sizeof(hello),
                     NULL, 0, NULL, 0, NULL, 0) != 0 ||
        receive_hello_reply(app) != 0) {
        lkg_app_close(app);
        return NULL;
    }
    return app;
}

LkgApp *lkg_app_open(void)
{
    const char *env = getenv(LKG_APP_FD_ENV);
    char *end = NULL;
    long parsed;
    if (!env || !env[0]) {
        errno = ENOENT;
        return NULL;
    }
    errno = 0;
    parsed = strtol(env, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
        errno = EINVAL;
        return NULL;
    }
    return lkg_app_open_fd((int)parsed);
}

void lkg_app_close(LkgApp *app)
{
    int i;
    if (!app)
        return;
    for (i = 0; i < 2; ++i) {
        if (app->layers[i] && app->info.layer_size > 0)
            munmap(app->layers[i], (size_t)app->info.layer_size);
        if (app->layer_fds[i] >= 0)
            close(app->layer_fds[i]);
    }
    if (app->fd >= 0)
        close(app->fd);
    free(app);
}

const LkgAppInfo *lkg_app_info(const LkgApp *app)
{
    return app ? &app->info : NULL;
}

int lkg_app_fd(const LkgApp *app)
{
    return app ? app->fd : -1;
}

int lkg_app_poll_event(LkgApp *app, LkgEvent *event, int timeout_ms)
{
    struct pollfd pfd;
    if (!app || !event || app->fd < 0) {
        errno = EINVAL;
        return -1;
    }
    memset(event, 0, sizeof(*event));
    if (pop_event(app, event))
        return 1;
    pfd.fd = app->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    for (;;) {
        int ready = poll(&pfd, 1, timeout_ms);
        if (ready < 0) {
            if (errno == EINTR)
                return 0;
            return -1;
        }
        if (ready == 0)
            return 0;
        if (pfd.revents & POLLIN) {
            LkgMsgHeader header;
            uint8_t buffer[LKG_APP_MSG_MAX_PAYLOAD];
            if (recv_frame(app, &header, buffer, sizeof(buffer)) != 0)
                return -1;
            if (header.type == LKG_MSG_EVENT && header.length >= sizeof(*event)) {
                memcpy(event, buffer, sizeof(*event));
                return 1;
            }
            if (timeout_ms > 0)
                timeout_ms = 0;
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EPIPE;
            return -1;
        }
    }
}

int lkg_app_text_input(LkgApp *app, const char *title,
                       const char *initial_text, char *out_text, size_t out_size)
{
    LkgTextInputPayload request;
    LkgMsgHeader header;
    uint8_t buffer[LKG_APP_MSG_MAX_PAYLOAD];
    LkgTextInputReplyPayload reply;
    size_t title_len;
    size_t initial_len;
    size_t copy_len;
    if (!app || !out_text || out_size == 0) {
        errno = EINVAL;
        return -1;
    }
    out_text[0] = '\0';
    if (!title)
        title = "";
    if (!initial_text)
        initial_text = "";
    title_len = strlen(title);
    initial_len = strlen(initial_text);
    if (initial_len > LKG_APP_TEXT_INPUT_MAX)
        initial_len = LKG_APP_TEXT_INPUT_MAX;
    if (sizeof(request) + title_len + initial_len > LKG_APP_MSG_MAX_PAYLOAD) {
        errno = EINVAL;
        return -1;
    }
    request.title_len = (uint32_t)title_len;
    request.initial_len = (uint32_t)initial_len;
    if (send_message(app, LKG_MSG_TEXT_INPUT,
                     &request, sizeof(request),
                     title, title_len,
                     initial_text, initial_len,
                     NULL, 0) != 0)
        return -1;
    if (wait_reply(app, LKG_MSG_TEXT_INPUT_REPLY, &header, buffer, sizeof(buffer)) != 0)
        return -1;
    if (header.length < sizeof(reply)) {
        errno = EPROTO;
        return -1;
    }
    memcpy(&reply, buffer, sizeof(reply));
    if (reply.accepted < 0 || sizeof(reply) + reply.text_len > header.length)
        return -1;
    if (reply.accepted == 0)
        return 0;
    copy_len = reply.text_len;
    if (copy_len >= out_size)
        copy_len = out_size - 1;
    if (copy_len > 0)
        memcpy(out_text, buffer + sizeof(reply), copy_len);
    out_text[copy_len] = '\0';
    return 1;
}

static int ensure_hosted(const LkgApp *app)
{
    if (!app) {
        errno = EINVAL;
        return -1;
    }
    if (app->info.render_mode != LKG_RENDER_HOSTED) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

int lkg_app_set_depth(LkgApp *app, float front_disparity, float back_disparity)
{
    LkgSetDepthPayload payload;
    if (ensure_hosted(app) != 0)
        return -1;
    payload.front_disparity = front_disparity;
    payload.back_disparity = back_disparity;
    return send_message(app, LKG_MSG_SET_DEPTH, &payload, sizeof(payload),
                        NULL, 0, NULL, 0, NULL, 0);
}

int lkg_app_set_stereo(LkgApp *app, int stereo_mode)
{
    LkgSetStereoPayload payload;
    if (ensure_hosted(app) != 0)
        return -1;
    payload.stereo = stereo_mode ? 1 : 0;
    return send_message(app, LKG_MSG_SET_STEREO, &payload, sizeof(payload),
                        NULL, 0, NULL, 0, NULL, 0);
}

int lkg_app_clear(LkgApp *app)
{
    return lkg_app_clear_layer(app, -1);
}

int lkg_app_clear_layer(LkgApp *app, int layer)
{
    LkgClearPayload payload;
    if (ensure_hosted(app) != 0)
        return -1;
    if (layer < -1 || layer > 1) {
        errno = EINVAL;
        return -1;
    }
    payload.layer = layer;
    return send_message(app, LKG_MSG_CLEAR, &payload, sizeof(payload),
                        NULL, 0, NULL, 0, NULL, 0);
}

int lkg_app_draw_rect(LkgApp *app, int layer, int x, int y, int width, int height,
                      uint32_t color)
{
    LkgDrawRectPayload payload;
    if (ensure_hosted(app) != 0)
        return -1;
    if (layer < 0 || layer > 1) {
        errno = EINVAL;
        return -1;
    }
    payload.layer = layer;
    payload.x = x;
    payload.y = y;
    payload.width = width;
    payload.height = height;
    payload.color = color;
    return send_message(app, LKG_MSG_DRAW_RECT, &payload, sizeof(payload),
                        NULL, 0, NULL, 0, NULL, 0);
}

static int draw_text_common(LkgApp *app, int layer, const char *text, int x, int baseline,
                            int font_size, uint32_t color, int max_width, int bold)
{
    LkgDrawTextPayload payload;
    size_t text_len;
    if (ensure_hosted(app) != 0)
        return -1;
    if (layer < 0 || layer > 1 || !text) {
        errno = EINVAL;
        return -1;
    }
    text_len = strlen(text);
    if (sizeof(payload) + text_len > LKG_APP_MSG_MAX_PAYLOAD)
        text_len = LKG_APP_MSG_MAX_PAYLOAD - sizeof(payload);
    payload.layer = layer;
    payload.x = x;
    payload.baseline = baseline;
    payload.font_size = font_size;
    payload.color = color;
    payload.max_width = max_width;
    payload.bold = bold ? 1 : 0;
    payload.text_len = (uint32_t)text_len;
    return send_message(app, LKG_MSG_DRAW_TEXT, &payload, sizeof(payload),
                        text, text_len, NULL, 0, NULL, 0);
}

int lkg_app_draw_text(LkgApp *app, int layer, const char *text, int x, int baseline,
                      int font_size, uint32_t color, int bold)
{
    return draw_text_common(app, layer, text, x, baseline, font_size, color, 0, bold);
}

int lkg_app_draw_text_fit(LkgApp *app, int layer, const char *text, int x, int baseline,
                          int font_size, uint32_t color, int max_width, int bold)
{
    if (max_width <= 0) {
        errno = EINVAL;
        return -1;
    }
    return draw_text_common(app, layer, text, x, baseline, font_size, color, max_width, bold);
}

int lkg_app_draw_back_arrow(LkgApp *app, int layer, int x, int center_y, uint32_t color)
{
    LkgDrawBackArrowPayload payload;
    if (ensure_hosted(app) != 0)
        return -1;
    if (layer < 0 || layer > 1) {
        errno = EINVAL;
        return -1;
    }
    payload.layer = layer;
    payload.x = x;
    payload.center_y = center_y;
    payload.color = color;
    return send_message(app, LKG_MSG_DRAW_BACK_ARROW, &payload, sizeof(payload),
                        NULL, 0, NULL, 0, NULL, 0);
}

int lkg_app_measure_text(LkgApp *app, const char *text, int font_size,
                         LkgTextMetrics *metrics)
{
    LkgMeasureTextPayload payload;
    LkgMsgHeader header;
    uint8_t buffer[LKG_APP_MSG_MAX_PAYLOAD];
    LkgMeasureReplyPayload reply;
    size_t text_len;
    if (ensure_hosted(app) != 0)
        return -1;
    if (!text || !metrics) {
        errno = EINVAL;
        return -1;
    }
    text_len = strlen(text);
    if (sizeof(payload) + text_len > LKG_APP_MSG_MAX_PAYLOAD)
        text_len = LKG_APP_MSG_MAX_PAYLOAD - sizeof(payload);
    payload.font_size = font_size;
    payload.text_len = (uint32_t)text_len;
    if (send_message(app, LKG_MSG_MEASURE_TEXT, &payload, sizeof(payload),
                     text, text_len, NULL, 0, NULL, 0) != 0)
        return -1;
    if (wait_reply(app, LKG_MSG_MEASURE_REPLY, &header, buffer, sizeof(buffer)) != 0)
        return -1;
    if (header.length < sizeof(reply)) {
        errno = EPROTO;
        return -1;
    }
    memcpy(&reply, buffer, sizeof(reply));
    if (reply.status != 0)
        return -1;
    *metrics = reply.metrics;
    return 0;
}

int lkg_app_row_baseline(int y, int row_height, const LkgTextMetrics *metrics)
{
    if (!metrics)
        return y + row_height / 2;
    return y + (row_height - (metrics->ascender - metrics->descender)) / 2 + metrics->ascender;
}

int lkg_app_draw_menu_row(LkgApp *app, const char *label, int y, int active, int is_back)
{
    const LkgAppInfo *info = lkg_app_info(app);
    int layer;
    uint32_t color;
    int bold;
    int baseline;
    int max_width;
    if (ensure_hosted(app) != 0 || !label)
        return -1;
    if (!app->have_large_metrics) {
        if (lkg_app_measure_text(app, "Ag", info->font_large, &app->large_metrics) != 0)
            return -1;
        app->have_large_metrics = 1;
    }
    if (lkg_app_draw_rect(app, LKG_LAYER_FRONT, 0, y, info->screen_width,
                          info->row_height, LKG_COLOR_TRANSPARENT) != 0 ||
        lkg_app_draw_rect(app, LKG_LAYER_BACK, 0, y, info->screen_width,
                          info->row_height, LKG_COLOR_TRANSPARENT) != 0)
        return -1;
    layer = active ? LKG_LAYER_FRONT : LKG_LAYER_BACK;
    color = active ? info->color_text_active : info->color_text;
    bold = active && info->menu_3d;
    baseline = lkg_app_row_baseline(y, info->row_height, &app->large_metrics);
    if (is_back &&
        lkg_app_draw_back_arrow(app, layer, info->margin / 3,
                                y + info->row_height / 2, color) != 0)
        return -1;
    max_width = info->screen_width - info->margin * 2 - 24;
    return lkg_app_draw_text_fit(app, layer, label, info->margin + 24, baseline,
                                 info->font_large, color, max_width, bold);
}

uint32_t *lkg_app_layer_pixels(LkgApp *app, int layer)
{
    if (!app || app->info.render_mode != LKG_RENDER_HOSTED || layer < 0 || layer > 1)
        return NULL;
    return app->layers[layer];
}

int lkg_app_sync(LkgApp *app)
{
    LkgMsgHeader header;
    uint8_t buffer[LKG_APP_MSG_MAX_PAYLOAD];
    LkgReplyPayload reply;
    if (ensure_hosted(app) != 0)
        return -1;
    if (send_message(app, LKG_MSG_SYNC, NULL, 0, NULL, 0, NULL, 0, NULL, 0) != 0)
        return -1;
    if (wait_reply(app, LKG_MSG_REPLY, &header, buffer, sizeof(buffer)) != 0)
        return -1;
    if (header.length < sizeof(reply)) {
        errno = EPROTO;
        return -1;
    }
    memcpy(&reply, buffer, sizeof(reply));
    return reply.status;
}

static int send_present(LkgApp *app, int full, const LkgRectUpdate *rects, size_t count)
{
    LkgPresentPayload payload;
    LkgMsgHeader header;
    uint8_t buffer[LKG_APP_MSG_MAX_PAYLOAD];
    LkgReplyPayload reply;
    if (ensure_hosted(app) != 0)
        return -1;
    if (count > LKG_APP_MAX_PRESENT_RECTS || (count > 0 && !rects)) {
        errno = EINVAL;
        return -1;
    }
    payload.full = full ? 1 : 0;
    payload.rect_count = (uint32_t)count;
    if (send_message(app, LKG_MSG_PRESENT,
                     &payload, sizeof(payload),
                     rects, count * sizeof(*rects),
                     NULL, 0, NULL, 0) != 0)
        return -1;
    if (wait_reply(app, LKG_MSG_REPLY, &header, buffer, sizeof(buffer)) != 0)
        return -1;
    if (header.length < sizeof(reply)) {
        errno = EPROTO;
        return -1;
    }
    memcpy(&reply, buffer, sizeof(reply));
    return reply.status;
}

int lkg_app_present(LkgApp *app, int bar_top, int bar_bottom)
{
    LkgRectUpdate rect;
    if (ensure_hosted(app) != 0)
        return -1;
    rect.x = 0;
    rect.y = 0;
    rect.width = app->info.screen_width;
    rect.height = app->info.screen_height;
    rect.bar_top = bar_top;
    rect.bar_bottom = bar_bottom;
    return send_present(app, 1, &rect, 1);
}

int lkg_app_present_rect(LkgApp *app, int x, int y, int width, int height,
                         int bar_top, int bar_bottom)
{
    LkgRectUpdate rect;
    rect.x = x;
    rect.y = y;
    rect.width = width;
    rect.height = height;
    rect.bar_top = bar_top;
    rect.bar_bottom = bar_bottom;
    return send_present(app, 0, &rect, 1);
}

int lkg_app_present_rects(LkgApp *app, const LkgRectUpdate *rects, size_t count)
{
    if (count == 0)
        return 0;
    return send_present(app, 0, rects, count);
}

int lkg_app_upload_rect(LkgApp *app, int x, int y, int width, int height)
{
    LkgUploadRectPayload payload;
    if (ensure_hosted(app) != 0)
        return -1;
    payload.x = x;
    payload.y = y;
    payload.width = width;
    payload.height = height;
    return send_message(app, LKG_MSG_UPLOAD_RECT, &payload, sizeof(payload),
                        NULL, 0, NULL, 0, NULL, 0);
}

int lkg_app_present_anim(LkgApp *app, const LkgAnimRect *rects, size_t rect_count,
                         const LkgClipRect *clips, size_t clip_count,
                         int bar_top, int bar_bottom, int has_back,
                         int present, int commit_cache)
{
    LkgAnimPresentPayload payload;
    LkgMsgHeader header;
    uint8_t buffer[LKG_APP_MSG_MAX_PAYLOAD];
    LkgReplyPayload reply;
    if (ensure_hosted(app) != 0)
        return -1;
    if (rect_count > 2 || clip_count == 0 || clip_count > 2 ||
        (rect_count > 0 && !rects) || !clips) {
        errno = EINVAL;
        return -1;
    }
    payload.rect_count = (uint32_t)rect_count;
    payload.clip_count = (uint32_t)clip_count;
    payload.bar_top = bar_top;
    payload.bar_bottom = bar_bottom;
    payload.has_back = has_back ? 1 : 0;
    payload.present = present ? 1 : 0;
    payload.commit_cache = commit_cache ? 1 : 0;
    if (send_message(app, LKG_MSG_ANIM_PRESENT,
                     &payload, sizeof(payload),
                     rects, rect_count * sizeof(*rects),
                     clips, clip_count * sizeof(*clips),
                     NULL, 0) != 0)
        return -1;
    if (wait_reply(app, LKG_MSG_REPLY, &header, buffer, sizeof(buffer)) != 0)
        return -1;
    if (header.length < sizeof(reply)) {
        errno = EPROTO;
        return -1;
    }
    memcpy(&reply, buffer, sizeof(reply));
    return reply.status;
}

int lkg_app_present_anim_scroll(LkgApp *app, const LkgAnimRect *rects, size_t rect_count,
                                const LkgClipRect *clips, size_t clip_count,
                                int bar_top, int bar_bottom, int has_back,
                                float scroll_y, int scroll_limit_y, int source_max_y,
                                int clear_cache, int present, int commit_cache)
{
    LkgAnimScrollPresentPayload payload;
    LkgMsgHeader header;
    uint8_t buffer[LKG_APP_MSG_MAX_PAYLOAD];
    LkgReplyPayload reply;
    if (ensure_hosted(app) != 0)
        return -1;
    if (rect_count > 2 || clip_count == 0 || clip_count > 2 ||
        (rect_count > 0 && !rects) || !clips ||
        scroll_limit_y < 0 || source_max_y < 0) {
        errno = EINVAL;
        return -1;
    }
    payload.rect_count = (uint32_t)rect_count;
    payload.clip_count = (uint32_t)clip_count;
    payload.bar_top = bar_top;
    payload.bar_bottom = bar_bottom;
    payload.has_back = has_back ? 1 : 0;
    payload.present = present ? 1 : 0;
    payload.commit_cache = commit_cache ? 1 : 0;
    payload.clear_cache = clear_cache ? 1 : 0;
    payload.scroll_y = scroll_y;
    payload.scroll_limit_y = scroll_limit_y;
    payload.source_max_y = source_max_y;
    if (send_message(app, LKG_MSG_ANIM_SCROLL_PRESENT,
                     &payload, sizeof(payload),
                     rects, rect_count * sizeof(*rects),
                     clips, clip_count * sizeof(*clips),
                     NULL, 0) != 0)
        return -1;
    if (wait_reply(app, LKG_MSG_REPLY, &header, buffer, sizeof(buffer)) != 0)
        return -1;
    if (header.length < sizeof(reply)) {
        errno = EPROTO;
        return -1;
    }
    memcpy(&reply, buffer, sizeof(reply));
    return reply.status;
}
