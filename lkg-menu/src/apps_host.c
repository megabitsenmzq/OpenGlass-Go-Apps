#define _GNU_SOURCE
#include "apps_host.h"
#include "apps_view.h"
#include "depth_frame.h"
#include "password_keyboard.h"
#include "serial_parser.h"
#include "wifi_view.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>

extern char **environ;

#define APP_LOOP_WATCHDOG_THRESHOLD 5
#define APP_LOOP_WATCHDOG_GAP_MS 4000

static long long host_monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

LkgEvent apps_host_event_from_serial(const char *normalized_cmd)
{
    LkgEvent event;
    memset(&event, 0, sizeof(event));
    if (!normalized_cmd)
        return event;
    event.type = LKG_EVENT_BUTTON;
    if (strcmp(normalized_cmd, "PREV") == 0)
        event.nav = LKG_NAV_NEXT;
    else if (strcmp(normalized_cmd, "NEXT") == 0)
        event.nav = LKG_NAV_PREV;
    else if (strcmp(normalized_cmd, "PLPA") == 0)
        event.nav = LKG_NAV_CONFIRM;
    else if (strcmp(normalized_cmd, "LOOP") == 0)
        event.nav = LKG_NAV_BACK;
    else
        event.type = LKG_EVENT_NONE;
    return event;
}

LkgEvent apps_host_event_from_usb_key(UsbKeyEvent key)
{
    LkgEvent event;
    memset(&event, 0, sizeof(event));
    if (key.type == USB_KEY_NONE)
        return event;
    event.type = LKG_EVENT_KEY;
    event.keycode = (uint16_t)key.code;
    if (key.type == USB_KEY_PREV) {
        event.nav = LKG_NAV_PREV;
        event.key = key.code == KEY_LEFT ? LKG_KEY_LEFT : LKG_KEY_UP;
    } else if (key.type == USB_KEY_NEXT) {
        event.nav = LKG_NAV_NEXT;
        event.key = key.code == KEY_RIGHT ? LKG_KEY_RIGHT : LKG_KEY_DOWN;
    } else if (key.type == USB_KEY_ENTER) {
        event.nav = LKG_NAV_CONFIRM;
        event.key = LKG_KEY_ENTER;
    } else if (key.type == USB_KEY_ESCAPE) {
        event.nav = LKG_NAV_BACK;
        event.key = LKG_KEY_ESCAPE;
    } else if (key.type == USB_KEY_BACKSPACE) {
        event.key = LKG_KEY_BACKSPACE;
    } else if (key.type == USB_KEY_CHARACTER) {
        event.key = LKG_KEY_CHAR;
        event.character = (uint8_t)key.character;
    } else {
        event.type = LKG_EVENT_NONE;
    }
    return event;
}

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
        if (count == 0)
            return -1;
        received += (size_t)count;
    }
    return 0;
}

static int send_reply(int fd, uint32_t type, uint32_t seq,
                      const void *part1, size_t len1,
                      const void *part2, size_t len2)
{
    LkgMsgHeader header;
    header.type = type;
    header.length = (uint32_t)(len1 + len2);
    header.seq = seq;
    if (send_all(fd, &header, sizeof(header)) != 0)
        return -1;
    if (len1 && send_all(fd, part1, len1) != 0)
        return -1;
    if (len2 && send_all(fd, part2, len2) != 0)
        return -1;
    return 0;
}

static int send_hello_reply(int fd, uint32_t seq, const LkgAppInfo *info,
                            const int layer_fds[2], int send_fds)
{
    LkgMsgHeader header;
    struct msghdr msg;
    struct iovec iov;
    union {
        struct cmsghdr header;
        char bytes[CMSG_SPACE(sizeof(int) * 2)];
    } control;

    header.type = LKG_MSG_HELLO_REPLY;
    header.length = sizeof(*info);
    header.seq = seq;

    memset(&msg, 0, sizeof(msg));
    memset(&control, 0, sizeof(control));
    iov.iov_base = &header;
    iov.iov_len = sizeof(header);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (send_fds) {
        struct cmsghdr *cmsg;
        msg.msg_control = control.bytes;
        msg.msg_controllen = sizeof(control.bytes);
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * 2);
        memcpy(CMSG_DATA(cmsg), layer_fds, sizeof(int) * 2);
    }

    for (;;) {
        ssize_t count = sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (count != (ssize_t)sizeof(header))
            return -1;
        break;
    }
    return send_all(fd, info, sizeof(*info));
}

static int send_event_nonblocking(int fd, const LkgEvent *event)
{
    uint8_t frame[sizeof(LkgMsgHeader) + sizeof(LkgEvent)];
    LkgMsgHeader header;
    ssize_t sent;
    header.type = LKG_MSG_EVENT;
    header.length = sizeof(*event);
    header.seq = 0;
    memcpy(frame, &header, sizeof(header));
    memcpy(frame + sizeof(header), event, sizeof(*event));
    sent = send(fd, frame, sizeof(frame), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent == (ssize_t)sizeof(frame))
        return 0;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    return -1;
}

static void stop_child(pid_t pid)
{
    int status;
    int attempt;
    if (pid <= 0)
        return;
    kill(pid, SIGTERM);
    for (attempt = 0; attempt < 30; ++attempt) {
        pid_t res = waitpid(pid, &status, WNOHANG);
        if (res == pid || (res < 0 && errno != EINTR))
            return;
        usleep(50000);
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
}

static int prepare_exec_fd(const char *exec_path, int *use_fexecve)
{
    int src_fd;
    int mem_fd;
    uint8_t buffer[65536];
    *use_fexecve = 0;
    if (access(exec_path, X_OK) == 0)
        return -1;
    src_fd = open(exec_path, O_RDONLY | O_CLOEXEC);
    if (src_fd < 0)
        return -2;
    mem_fd = memfd_create("lkg-ext-app", MFD_CLOEXEC);
    if (mem_fd < 0) {
        close(src_fd);
        return -2;
    }
    for (;;) {
        ssize_t nread = read(src_fd, buffer, sizeof(buffer));
        size_t offset = 0;
        if (nread < 0) {
            if (errno == EINTR)
                continue;
            close(src_fd);
            close(mem_fd);
            return -2;
        }
        if (nread == 0)
            break;
        while (offset < (size_t)nread) {
            ssize_t nwritten = write(mem_fd, buffer + offset, (size_t)nread - offset);
            if (nwritten < 0) {
                if (errno == EINTR)
                    continue;
                close(src_fd);
                close(mem_fd);
                return -2;
            }
            offset += (size_t)nwritten;
        }
    }
    close(src_fd);
    if (fchmod(mem_fd, 0700) != 0 || lseek(mem_fd, 0, SEEK_SET) != 0) {
        close(mem_fd);
        return -2;
    }
    *use_fexecve = 1;
    return mem_fd;
}

typedef struct {
    Display *display;
    Font *font;
    DepthEffect *depth;
    DepthLayer *app_layers;
    int layer_fds[2];
    DepthLayer *modal_layers;
    DepthCacheState cache_state;
    LkgAppInfo info;
    int menu_3d;
    int app_stereo;
    int frame_initialized;
    int last_bar_top;
    int last_bar_bottom;
    int sock_fd;
    int serial_fd;
    UsbKeyboard *keyboard;
    SerialParser *parser;
    int button_reversed;
    int serial_result;
} AppHostSession;

static int create_shared_layers(Display *display, const DepthEffect *depth,
                                DepthLayer app_layers[2], int layer_fds[2])
{
    uint32_t stride = display->mode.hdisplay * 4;
    size_t size = (size_t)stride * display->mode.vdisplay;
    const float disparities[2] = {
        depth->stereo ? depth->front_disparity : LKG_DEPTH_SUBTLE_POP_OUT,
        depth->stereo ? depth->back_disparity : LKG_DEPTH_SUBTLE_RECESSED
    };
    int i;
    memset(app_layers, 0, sizeof(DepthLayer) * 2);
    layer_fds[0] = -1;
    layer_fds[1] = -1;
    for (i = 0; i < 2; ++i) {
        const char *tag = i == 0 ? "lkg-app-layer0" : "lkg-app-layer1";
        void *mapped;
        layer_fds[i] = memfd_create(tag, MFD_CLOEXEC);
        if (layer_fds[i] < 0 || ftruncate(layer_fds[i], (off_t)size) != 0)
            goto fail;
        mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, layer_fds[i], 0);
        if (mapped == MAP_FAILED)
            goto fail;
        app_layers[i].disparity = disparities[i];
        app_layers[i].buffer.layered = 1;
        app_layers[i].buffer.stride = stride;
        app_layers[i].buffer.size = size;
        app_layers[i].buffer.pixels = (uint8_t *)mapped;
    }
    return 0;
fail:
    for (i = 0; i < 2; ++i) {
        if (app_layers[i].buffer.pixels)
            munmap(app_layers[i].buffer.pixels, app_layers[i].buffer.size);
        if (layer_fds[i] >= 0)
            close(layer_fds[i]);
        layer_fds[i] = -1;
    }
    memset(app_layers, 0, sizeof(DepthLayer) * 2);
    return -1;
}

static void destroy_shared_layers(DepthLayer app_layers[2], int layer_fds[2])
{
    int i;
    for (i = 0; i < 2; ++i) {
        if (app_layers[i].buffer.pixels && app_layers[i].buffer.size > 0)
            munmap(app_layers[i].buffer.pixels, app_layers[i].buffer.size);
        if (layer_fds[i] >= 0)
            close(layer_fds[i]);
        layer_fds[i] = -1;
    }
    memset(app_layers, 0, sizeof(DepthLayer) * 2);
}

static void fill_app_info(LkgAppInfo *info, const Display *display,
                          const DepthEffect *depth, LkgRenderMode mode,
                          int button_reversed)
{
    int row_height = (int)display->mode.vdisplay / 17;
    memset(info, 0, sizeof(*info));
    info->api_version = LKG_APP_API_VERSION;
    info->render_mode = (uint32_t)mode;
    info->screen_width = (int32_t)display->mode.hdisplay;
    info->screen_height = (int32_t)display->mode.vdisplay;
    info->row_height = row_height;
    info->margin = (int32_t)display->mode.hdisplay / 12;
    info->top_margin = row_height / 6;
    info->bar_gap = 8;
    info->menu_3d = depth->stereo ? 1 : 0;
    info->button_reversed = button_reversed ? 1 : 0;
    info->font_large = LKG_FONT_SIZE_LARGE;
    info->font_medium = LKG_FONT_SIZE_MEDIUM;
    info->font_caption = LKG_FONT_SIZE_CAPTION;
    info->font_small = LKG_FONT_SIZE_SMALL;
    info->font_key = LKG_FONT_SIZE_KEY;
    info->depth_front = depth->stereo ? depth->front_disparity : LKG_DEPTH_SUBTLE_POP_OUT;
    info->depth_back = depth->stereo ? depth->back_disparity : LKG_DEPTH_SUBTLE_RECESSED;
    info->depth_zero = LKG_DEPTH_SCREEN;
    info->color_bg = LKG_COLOR_BG;
    info->color_bar = LKG_COLOR_BAR;
    info->color_text_active = LKG_COLOR_TEXT_ACTIVE;
    info->color_text = LKG_COLOR_TEXT;
    info->color_text_bright = LKG_COLOR_TEXT_BRIGHT;
    info->color_text_muted = LKG_COLOR_TEXT_MUTED;
    info->color_surface = LKG_COLOR_SURFACE;
    info->color_surface_light = LKG_COLOR_SURFACE_LIGHT;
    if (mode == LKG_RENDER_HOSTED) {
        info->layer_stride = display->mode.hdisplay * 4;
        info->layer_size = (uint64_t)info->layer_stride * display->mode.vdisplay;
    }
}

static int modal_draw_cursor(AppHostSession *session, DepthCacheState *modal_cache,
                             const PasswordKeyboardState *kb, int previous_key,
                             int include_back)
{
    Display *display = session->display;
    Font *font = session->font;
    const DepthEffect *depth = session->depth;
    DepthLayer *layers = session->modal_layers;
    int old_x, old_y, old_w, old_h;
    int new_x, new_y, new_w, new_h;
    DepthFrame frame;
    if (depth_frame_begin(&frame, display, depth, layers, modal_cache, 0, 0) != 0)
        return -1;
    if (include_back) {
        int row_height = (int)display->mode.vdisplay / 17;
        int top = row_height / 6;
        wifi_view_draw_password_back(display, font, depth, layers, kb->back_selected, 1);
        depth_frame_update_rect(&frame, 0, top, display->mode.hdisplay, row_height, 0, 0);
    }
    wifi_view_key_geometry(display, previous_key, &old_x, &old_y, &old_w, &old_h);
    wifi_view_key_geometry(display, kb->key_selected, &new_x, &new_y, &new_w, &new_h);
    draw_rect(display, &layers[0].buffer, old_x, old_y, old_w, old_h, 0);
    draw_rect(display, &layers[1].buffer, new_x, new_y, new_w, new_h, 0);
    wifi_view_draw_password_key(display, font, depth, layers, previous_key, 0, kb->layout);
    wifi_view_draw_password_key(display, font, depth, layers, kb->key_selected,
                                !kb->back_selected, kb->layout);
    depth_frame_update_rect(&frame, old_x, old_y, old_w, old_h, 0, 0);
    depth_frame_update_rect(&frame, new_x, new_y, new_w, new_h, 0, 0);
    return depth_frame_present(&frame);
}

static int modal_present_region(AppHostSession *session, DepthCacheState *modal_cache,
                                int x, int y, int width, int height)
{
    DepthFrame frame;
    if (depth_frame_begin(&frame, session->display, session->depth, session->modal_layers,
                          modal_cache, 0, 0) != 0)
        return -1;
    depth_frame_update_rect(&frame, x, y, width, height, 0, 0);
    return depth_frame_present(&frame);
}

static int run_text_input_modal(AppHostSession *session, const char *title,
                                const char *initial_text, char *out_text,
                                size_t out_size)
{
    PasswordKeyboardState kb;
    DepthCacheState modal_cache;
    int changed = PASSWORD_REDRAW_FULL;
    int previous_key = 25;
    int previous_back = 1;
    int result = 0;

    memset(&kb, 0, sizeof(kb));
    memset(&modal_cache, 0, sizeof(modal_cache));
    kb.layout = 0;
    kb.key_selected = 25;
    kb.back_selected = 1;
    if (initial_text && initial_text[0]) {
        snprintf(kb.password, sizeof(kb.password), "%s", initial_text);
        kb.password_length = strlen(kb.password);
    }

    session->depth->stereo = session->menu_3d;
    session->modal_layers[0].disparity = session->depth->front_disparity;
    session->modal_layers[1].disparity = session->depth->back_disparity;

    while (running) {
        UsbKeyEvent key_event;
        char command[16];
        int event = 0;

        if (changed) {
            int draw_result = 0;
            if (changed == PASSWORD_REDRAW_FOCUS) {
                draw_result = modal_draw_cursor(session, &modal_cache, &kb, previous_key, 0);
            } else if (changed == PASSWORD_REDRAW_BACK) {
                draw_result = modal_draw_cursor(session, &modal_cache, &kb, previous_key, 1);
            } else if (changed == PASSWORD_REDRAW_PASSWORD) {
                wifi_view_draw_password_field(session->display, session->font,
                                              session->modal_layers, kb.password);
                draw_result = modal_present_region(session, &modal_cache,
                                                   0, 535, session->display->mode.hdisplay, 140);
            } else if (changed == PASSWORD_REDRAW_KEYBOARD) {
                int top, bottom, kx, kw, kh, i;
                int width = (int)session->display->mode.hdisplay;
                wifi_view_key_geometry(session->display, 0, &kx, &top, &kw, &kh);
                wifi_view_key_geometry(session->display, WIFI_KEYBOARD_KEYS - 1,
                                       &kx, &bottom, &kw, &kh);
                bottom += kh;
                draw_rect(session->display, &session->modal_layers[0].buffer,
                          0, top, width, bottom - top, 0);
                draw_rect(session->display, &session->modal_layers[1].buffer,
                          0, top, width, bottom - top, 0);
                for (i = 0; i < WIFI_KEYBOARD_KEYS; ++i)
                    wifi_view_draw_password_key(session->display, session->font,
                                                session->depth, session->modal_layers, i,
                                                i == kb.key_selected && !kb.back_selected,
                                                kb.layout);
                draw_result = modal_present_region(session, &modal_cache,
                                                   0, top, width, bottom - top);
            } else {
                DepthFrame frame;
                if (depth_frame_begin(&frame, session->display, session->depth,
                                      session->modal_layers, &modal_cache, 0, 1) != 0)
                    return -1;
                apps_view_draw_text_input(session->display, session->font, session->depth,
                                          session->modal_layers, title, kb.password,
                                          kb.layout, kb.key_selected, kb.back_selected, 1);
                depth_frame_update_rect(&frame, 0, 0, session->display->mode.hdisplay,
                                        session->display->mode.vdisplay, 0, 0);
                draw_result = depth_frame_present(&frame);
            }
            if (draw_result != 0)
                return -1;
            changed = 0;
        }

        key_event = usb_keyboard_next(session->keyboard);
        if (key_event.type != USB_KEY_NONE) {
            previous_key = kb.key_selected;
            previous_back = kb.back_selected;
            if (key_event.type == USB_KEY_PREV || key_event.type == USB_KEY_NEXT) {
                const char *cmd = key_event.type == USB_KEY_PREV ? "NEXT" : "PREV";
                if (password_keyboard_handle(&kb, cmd) != PASSWORD_KEYBOARD_NONE)
                    changed = password_keyboard_redraw(cmd, previous_back, &kb, 0);
            } else if (key_event.type == USB_KEY_CHARACTER) {
                if (kb.password_length < sizeof(kb.password) - 1) {
                    kb.password[kb.password_length++] = key_event.character;
                    kb.password[kb.password_length] = '\0';
                    changed = PASSWORD_REDRAW_PASSWORD;
                }
            } else if (key_event.type == USB_KEY_BACKSPACE) {
                if (kb.password_length)
                    --kb.password_length;
                kb.password[kb.password_length] = '\0';
                changed = PASSWORD_REDRAW_PASSWORD;
            } else if (key_event.type == USB_KEY_ENTER) {
                result = 1;
                break;
            } else if (key_event.type == USB_KEY_ESCAPE) {
                result = 0;
                break;
            }
            if (changed)
                continue;
        }

        event = serial_parser_next_with_fd(session->serial_fd, session->parser,
                                           command, sizeof(command), 100,
                                           session->keyboard->fd);
        if (event < 0)
            return -1;
        if (!event) {
            struct pollfd spfd;
            spfd.fd = session->sock_fd;
            spfd.events = POLLERR | POLLHUP;
            spfd.revents = 0;
            if (poll(&spfd, 1, 0) > 0 && (spfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
                return -1;
            continue;
        }
        normalize_button_command(command, session->button_reversed);
        if (strcmp(command, "SHUT") == 0) {
            session->serial_result = SERIAL_RESULT_POWER_OFF;
            return -1;
        }
        if (strcmp(command, "USBD") == 0) {
            session->serial_result = SERIAL_RESULT_USB_DESKTOP;
            return -1;
        }
        previous_key = kb.key_selected;
        previous_back = kb.back_selected;
        {
            PasswordKeyboardAction act = password_keyboard_handle(&kb, command);
            if (act == PASSWORD_KEYBOARD_BACK) {
                result = 0;
                break;
            }
            if (act == PASSWORD_KEYBOARD_CONNECT) {
                result = 1;
                break;
            }
            if (act != PASSWORD_KEYBOARD_NONE)
                changed = password_keyboard_redraw(command, previous_back, &kb, 0);
        }
    }

    if (!running)
        return -1;

    if (result == 1)
        snprintf(out_text, out_size, "%s", kb.password);
    else if (out_size > 0)
        out_text[0] = '\0';

    session->depth->stereo = session->app_stereo;
    FT_Set_Pixel_Sizes(session->font->face, 0, 64);

    if (session->frame_initialized) {
        DepthFrame restore;
        if (depth_frame_begin(&restore, session->display, session->depth,
                              session->app_layers, &session->cache_state, 1, 1) == 0) {
            depth_frame_update_rect(&restore, 0, 0, session->display->mode.hdisplay,
                                    session->display->mode.vdisplay,
                                    session->last_bar_top, session->last_bar_bottom);
            (void)depth_frame_present(&restore);
        }
    }
    return result;
}

static int clamp_font_size(int font_size)
{
    if (font_size < 8)
        return 8;
    if (font_size > 256)
        return 256;
    return font_size;
}

static int handle_client_message(AppHostSession *session)
{
    LkgMsgHeader header;
    uint8_t payload[LKG_APP_MSG_MAX_PAYLOAD];
    if (recv_all(session->sock_fd, &header, sizeof(header)) != 0)
        return -1;
    if (header.length > LKG_APP_MSG_MAX_PAYLOAD)
        return -1;
    if (header.length && recv_all(session->sock_fd, payload, header.length) != 0)
        return -1;

    if (header.type == LKG_MSG_HELLO) {
        return send_hello_reply(session->sock_fd, header.seq, &session->info,
                                session->layer_fds,
                                session->info.render_mode == LKG_RENDER_HOSTED);
    }
    if (session->info.render_mode != LKG_RENDER_HOSTED) {
        if (header.type == LKG_MSG_MEASURE_TEXT) {
            LkgMeasureReplyPayload reply;
            memset(&reply, 0, sizeof(reply));
            reply.status = -1;
            return send_reply(session->sock_fd, LKG_MSG_MEASURE_REPLY, header.seq,
                              &reply, sizeof(reply), NULL, 0);
        }
        if (header.type == LKG_MSG_TEXT_INPUT) {
            LkgTextInputReplyPayload reply;
            memset(&reply, 0, sizeof(reply));
            reply.accepted = -1;
            return send_reply(session->sock_fd, LKG_MSG_TEXT_INPUT_REPLY, header.seq,
                              &reply, sizeof(reply), NULL, 0);
        }
        if (header.type == LKG_MSG_SYNC || header.type == LKG_MSG_PRESENT) {
            LkgReplyPayload reply;
            reply.status = -1;
            return send_reply(session->sock_fd, LKG_MSG_REPLY, header.seq,
                              &reply, sizeof(reply), NULL, 0);
        }
        return 0;
    }

    switch (header.type) {
    case LKG_MSG_CLEAR: {
        LkgClearPayload req;
        if (header.length < sizeof(req))
            return -1;
        memcpy(&req, payload, sizeof(req));
        if (req.layer == -1 || req.layer == 0)
            memset(session->app_layers[0].buffer.pixels, 0,
                   session->app_layers[0].buffer.size);
        if (req.layer == -1 || req.layer == 1)
            memset(session->app_layers[1].buffer.pixels, 0,
                   session->app_layers[1].buffer.size);
        return 0;
    }
    case LKG_MSG_SET_DEPTH: {
        LkgSetDepthPayload req;
        if (header.length < sizeof(req))
            return -1;
        memcpy(&req, payload, sizeof(req));
        session->app_layers[0].disparity = req.front_disparity;
        session->app_layers[1].disparity = req.back_disparity;
        return 0;
    }
    case LKG_MSG_SET_STEREO: {
        LkgSetStereoPayload req;
        if (header.length < sizeof(req))
            return -1;
        memcpy(&req, payload, sizeof(req));
        session->app_stereo = req.stereo ? 1 : 0;
        session->depth->stereo = session->app_stereo;
        return 0;
    }
    case LKG_MSG_DRAW_RECT: {
        LkgDrawRectPayload req;
        if (header.length < sizeof(req))
            return -1;
        memcpy(&req, payload, sizeof(req));
        if (req.layer == 0 || req.layer == 1)
            draw_rect(session->display, &session->app_layers[req.layer].buffer,
                      req.x, req.y, req.width, req.height, req.color);
        return 0;
    }
    case LKG_MSG_DRAW_TEXT: {
        LkgDrawTextPayload req;
        char text[LKG_APP_MSG_MAX_PAYLOAD + 1];
        int font_size;
        if (header.length < sizeof(req))
            return -1;
        memcpy(&req, payload, sizeof(req));
        if (sizeof(req) + req.text_len > header.length)
            return -1;
        memcpy(text, payload + sizeof(req), req.text_len);
        text[req.text_len] = '\0';
        if (req.layer == 0 || req.layer == 1) {
            Buffer *buf = &session->app_layers[req.layer].buffer;
            font_size = clamp_font_size(req.font_size);
            FT_Set_Pixel_Sizes(session->font->face, 0, (FT_UInt)font_size);
            if (req.max_width > 0)
                draw_text_fit(session->display, buf, session->font, text,
                              req.x, req.baseline, req.color, req.max_width, req.bold);
            else if (req.bold)
                draw_text_bold(session->display, buf, session->font, text,
                               req.x, req.baseline, req.color);
            else
                draw_text(session->display, buf, session->font, text,
                          req.x, req.baseline, req.color);
            FT_Set_Pixel_Sizes(session->font->face, 0, 64);
        }
        return 0;
    }
    case LKG_MSG_DRAW_BACK_ARROW: {
        LkgDrawBackArrowPayload req;
        if (header.length < sizeof(req))
            return -1;
        memcpy(&req, payload, sizeof(req));
        if (req.layer == 0 || req.layer == 1)
            draw_back_arrow(session->display, &session->app_layers[req.layer].buffer,
                            req.x, req.center_y, req.color);
        return 0;
    }
    case LKG_MSG_MEASURE_TEXT: {
        LkgMeasureTextPayload req;
        LkgMeasureReplyPayload reply;
        char text[LKG_APP_MSG_MAX_PAYLOAD + 1];
        int font_size;
        memset(&reply, 0, sizeof(reply));
        if (header.length < sizeof(req))
            return -1;
        memcpy(&req, payload, sizeof(req));
        if (sizeof(req) + req.text_len > header.length)
            return -1;
        memcpy(text, payload + sizeof(req), req.text_len);
        text[req.text_len] = '\0';
        font_size = clamp_font_size(req.font_size);
        FT_Set_Pixel_Sizes(session->font->face, 0, (FT_UInt)font_size);
        reply.status = 0;
        reply.metrics.width = font_text_width(session->font, text);
        reply.metrics.ascender = (int32_t)(session->font->face->size->metrics.ascender >> 6);
        reply.metrics.descender = (int32_t)(session->font->face->size->metrics.descender >> 6);
        reply.metrics.height = reply.metrics.ascender - reply.metrics.descender;
        FT_Set_Pixel_Sizes(session->font->face, 0, 64);
        return send_reply(session->sock_fd, LKG_MSG_MEASURE_REPLY, header.seq,
                          &reply, sizeof(reply), NULL, 0);
    }
    case LKG_MSG_SYNC: {
        LkgReplyPayload reply;
        reply.status = 0;
        return send_reply(session->sock_fd, LKG_MSG_REPLY, header.seq,
                          &reply, sizeof(reply), NULL, 0);
    }
    case LKG_MSG_PRESENT: {
        LkgPresentPayload req;
        LkgReplyPayload reply;
        DepthFrame frame;
        int screen_w = (int)session->display->mode.hdisplay;
        int screen_h = (int)session->display->mode.vdisplay;
        int force_full;
        uint32_t i;
        if (header.length < sizeof(req))
            return -1;
        memcpy(&req, payload, sizeof(req));
        if (req.rect_count > LKG_APP_MAX_PRESENT_RECTS ||
            sizeof(req) + (size_t)req.rect_count * sizeof(LkgRectUpdate) > header.length) {
            reply.status = -1;
            return send_reply(session->sock_fd, LKG_MSG_REPLY, header.seq,
                              &reply, sizeof(reply), NULL, 0);
        }
        force_full = req.full || !session->frame_initialized;
        if (depth_frame_begin(&frame, session->display, session->depth,
                              session->app_layers, &session->cache_state, 1, force_full) != 0) {
            reply.status = -1;
            return send_reply(session->sock_fd, LKG_MSG_REPLY, header.seq,
                              &reply, sizeof(reply), NULL, 0);
        }
        if (req.rect_count == 0) {
            if (frame.full)
                (void)depth_frame_update_rect(&frame, 0, 0, screen_w, screen_h, 0, 0);
        } else {
            for (i = 0; i < req.rect_count; ++i) {
                LkgRectUpdate r;
                int rx0, ry0, rx1, ry1;
                memcpy(&r, payload + sizeof(req) + (size_t)i * sizeof(r), sizeof(r));
                rx0 = r.x < 0 ? 0 : r.x;
                ry0 = r.y < 0 ? 0 : r.y;
                rx1 = (long long)r.x + r.width > screen_w ? screen_w : (int)((long long)r.x + r.width);
                ry1 = (long long)r.y + r.height > screen_h ? screen_h : (int)((long long)r.y + r.height);
                if (rx1 > rx0 && ry1 > ry0) {
                    if (depth_frame_update_rect(&frame, rx0, ry0, rx1 - rx0, ry1 - ry0,
                                                r.bar_top, r.bar_bottom) != 0)
                        break;
                }
                if (r.bar_bottom > r.bar_top) {
                    session->last_bar_top = r.bar_top;
                    session->last_bar_bottom = r.bar_bottom;
                } else if (rx0 == 0 && ry0 == 0 && rx1 == screen_w && ry1 == screen_h) {
                    session->last_bar_top = 0;
                    session->last_bar_bottom = 0;
                }
            }
        }
        reply.status = depth_frame_present(&frame);
        if (reply.status == 0)
            session->frame_initialized = 1;
        return send_reply(session->sock_fd, LKG_MSG_REPLY, header.seq,
                          &reply, sizeof(reply), NULL, 0);
    }
    case LKG_MSG_TEXT_INPUT: {
        LkgTextInputPayload req;
        LkgTextInputReplyPayload reply;
        char title[256] = {0};
        char initial[LKG_APP_TEXT_INPUT_MAX + 1] = {0};
        char entered[LKG_APP_TEXT_INPUT_MAX + 1] = {0};
        size_t offset;
        size_t len;
        int modal_res;
        if (header.length < sizeof(req))
            return -1;
        memcpy(&req, payload, sizeof(req));
        if ((size_t)req.title_len + req.initial_len > header.length - sizeof(req))
            return -1;
        offset = sizeof(req);
        len = req.title_len < sizeof(title) - 1 ? req.title_len : sizeof(title) - 1;
        memcpy(title, payload + offset, len);
        offset += req.title_len;
        len = req.initial_len < sizeof(initial) - 1 ? req.initial_len : sizeof(initial) - 1;
        memcpy(initial, payload + offset, len);

        modal_res = run_text_input_modal(session, title, initial,
                                         entered, sizeof(entered));
        memset(&reply, 0, sizeof(reply));
        if (modal_res < 0) {
            reply.accepted = -1;
            (void)send_reply(session->sock_fd, LKG_MSG_TEXT_INPUT_REPLY, header.seq,
                             &reply, sizeof(reply), NULL, 0);
            return -1;
        }
        reply.accepted = modal_res;
        reply.text_len = modal_res == 1 ? (uint32_t)strlen(entered) : 0;
        return send_reply(session->sock_fd, LKG_MSG_TEXT_INPUT_REPLY, header.seq,
                          &reply, sizeof(reply), entered, reply.text_len);
    }
    default:
        return -1;
    }
}

static void show_app_error(Display *display, Font *font, const DepthEffect *depth,
                           DepthLayer layers[2], int serial_fd, UsbKeyboard *keyboard,
                           SerialParser *parser, int button_reversed,
                           const char *title, const char *detail)
{
    DepthFrame frame;
    float back_disparity = layers[1].disparity;
    long long deadline = host_monotonic_ms() + 1500;
    layers[1].disparity = 0.0f;
    if (depth_frame_begin(&frame, display, depth, layers, NULL, 0, 1) == 0) {
        memset(layers[0].buffer.pixels, 0, layers[0].buffer.size);
        memset(layers[1].buffer.pixels, 0, layers[1].buffer.size);
        apps_view_draw_status(display, font, depth, layers, 1, title, detail);
        depth_frame_update_rect(&frame, 0, 0, display->mode.hdisplay,
                                display->mode.vdisplay, 0, 0);
        (void)depth_frame_present(&frame);
    }
    layers[1].disparity = back_disparity;
    while (running && host_monotonic_ms() < deadline) {
        char command[16];
        UsbKeyEvent key_event = usb_keyboard_next(keyboard);
        if (key_event.type != USB_KEY_NONE)
            break;
        if (serial_parser_next_with_fd(serial_fd, parser, command, sizeof(command),
                                       50, keyboard->fd) > 0) {
            normalize_button_command(command, button_reversed);
            break;
        }
    }
}

static void restore_menu_drm_and_layers(Display *display, DepthEffect *depth,
                                        DepthLayer layers[2], int *depth_suspended,
                                        int *layers_suspended, int saved_stereo,
                                        float saved_front, float saved_back)
{
    if (*depth_suspended) {
        while (drmSetMaster(display->fd) != 0 && errno == EBUSY)
            usleep(50000);
        if (depth_effect_open(depth, display, "/run/lkg/visual.json") == 0) {
            depth->stereo = saved_stereo;
            depth->front_disparity = saved_front;
            depth->back_disparity = saved_back;
            *depth_suspended = 0;
        }
    }
    if (*layers_suspended) {
        if (depth_layer_create(display, &layers[0], saved_front) == 0 &&
            depth_layer_create(display, &layers[1], saved_back) == 0) {
            *layers_suspended = 0;
        } else {
            depth_layer_destroy(&layers[0]);
            depth_layer_destroy(&layers[1]);
        }
    }
}

static int launch_external_app(Display *display, Font *font, DepthEffect *depth,
                               DepthLayer layers[2], int serial_fd, UsbKeyboard *keyboard,
                               SerialParser *parser, int button_reversed,
                               const ExternalApp *app)
{
    int sockets[2] = { -1, -1 };
    int exec_fd = -1;
    int use_fexecve = 0;
    pid_t pid = -1;
    int child_status = 0;
    int child_exited = 0;
    int killed_by_host = 0;
    int saved_stereo = depth->stereo;
    float saved_front = depth->front_disparity;
    float saved_back = depth->back_disparity;
    int depth_suspended = 0;
    int layers_suspended = 0;
    int loop_watchdog_count = 0;
    long long last_loop_ms = 0;
    DepthLayer app_layers[2];
    AppHostSession session;
    struct timeval rcv_timeout;

    memset(app_layers, 0, sizeof(app_layers));
    memset(&session, 0, sizeof(session));
    session.display = display;
    session.font = font;
    session.depth = depth;
    session.app_layers = app_layers;
    session.layer_fds[0] = -1;
    session.layer_fds[1] = -1;
    session.modal_layers = layers;
    session.menu_3d = saved_stereo;
    session.app_stereo = saved_stereo;
    session.serial_fd = serial_fd;
    session.keyboard = keyboard;
    session.parser = parser;
    session.button_reversed = button_reversed;

    if (app->render_mode == LKG_RENDER_HOSTED) {
        if (create_shared_layers(display, depth, app_layers, session.layer_fds) != 0) {
            show_app_error(display, font, depth, layers, serial_fd, keyboard, parser,
                           button_reversed, "Launch failed", "Unable to allocate app layers");
            return 0;
        }
    } else {
        depth_layer_destroy(&layers[0]);
        depth_layer_destroy(&layers[1]);
        layers_suspended = 1;
        depth_effect_close(depth);
        depth_suspended = 1;
        if (drmDropMaster(display->fd) != 0) {
            restore_menu_drm_and_layers(display, depth, layers, &depth_suspended,
                                        &layers_suspended, saved_stereo,
                                        saved_front, saved_back);
            return depth_suspended || layers_suspended ? -1 : 0;
        }
    }

    exec_fd = prepare_exec_fd(app->exec_path, &use_fexecve);
    if (exec_fd == -2) {
        destroy_shared_layers(app_layers, session.layer_fds);
        restore_menu_drm_and_layers(display, depth, layers, &depth_suspended,
                                    &layers_suspended, saved_stereo,
                                    saved_front, saved_back);
        if (depth_suspended || layers_suspended)
            return -1;
        show_app_error(display, font, depth, layers, serial_fd, keyboard, parser,
                       button_reversed, "Launch failed", "Unable to open app binary");
        return 0;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        if (exec_fd >= 0)
            close(exec_fd);
        destroy_shared_layers(app_layers, session.layer_fds);
        restore_menu_drm_and_layers(display, depth, layers, &depth_suspended,
                                    &layers_suspended, saved_stereo,
                                    saved_front, saved_back);
        return depth_suspended || layers_suspended ? -1 : 0;
    }

    fcntl(sockets[0], F_SETFD, FD_CLOEXEC);
    rcv_timeout.tv_sec = 2;
    rcv_timeout.tv_usec = 0;
    setsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

    pid = fork();
    if (pid == 0) {
        struct sigaction dfl;
        char fd_str[16];
        char ld_buf[PATH_MAX * 2];
        const char *old_ld;
        char *argv_exec[2];

        close(sockets[0]);
        memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        sigaction(SIGINT, &dfl, NULL);
        sigaction(SIGTERM, &dfl, NULL);
        sigaction(SIGPIPE, &dfl, NULL);

        snprintf(fd_str, sizeof(fd_str), "%d", sockets[1]);
        setenv(LKG_APP_FD_ENV, fd_str, 1);
        setenv(LKG_APP_DIR_ENV, app->dir_path, 1);
        setenv(LKG_APP_NAME_ENV, app->name, 1);
        setenv(LKG_DATA_DIR_ENV, "/home/lkg/data", 1);
        setenv(LKG_CALIBRATION_ENV, "/run/lkg/visual.json", 1);
        old_ld = getenv("LD_LIBRARY_PATH");
        if (old_ld && old_ld[0]) {
            if (snprintf(ld_buf, sizeof(ld_buf), "%s:%s", app->dir_path, old_ld) <
                (int)sizeof(ld_buf))
                setenv("LD_LIBRARY_PATH", ld_buf, 1);
        } else {
            setenv("LD_LIBRARY_PATH", app->dir_path, 1);
        }
        if (chdir(app->dir_path) != 0)
            _exit(127);

        argv_exec[0] = app->exec_path;
        argv_exec[1] = NULL;
        if (use_fexecve && exec_fd >= 0)
            fexecve(exec_fd, argv_exec, environ);
        else
            execv(app->exec_path, argv_exec);
        _exit(127);
    }

    close(sockets[1]);
    if (exec_fd >= 0)
        close(exec_fd);

    if (pid < 0) {
        close(sockets[0]);
        destroy_shared_layers(app_layers, session.layer_fds);
        restore_menu_drm_and_layers(display, depth, layers, &depth_suspended,
                                    &layers_suspended, saved_stereo,
                                    saved_front, saved_back);
        return depth_suspended || layers_suspended ? -1 : 0;
    }

    session.sock_fd = sockets[0];
    fill_app_info(&session.info, display, depth, app->render_mode, button_reversed);
    printf("Launched external app: %s (%s)\n", app->name, app->exec_path);
    fflush(stdout);

    while (running) {
        struct pollfd pfds[3];
        int nfds = 2;
        int ready;
        pid_t waited;

        waited = waitpid(pid, &child_status, WNOHANG);
        if (waited == pid) {
            child_exited = 1;
            pid = -1;
            break;
        }

        for (;;) {
            UsbKeyEvent key_event = usb_keyboard_next(keyboard);
            LkgEvent app_event;
            if (key_event.type == USB_KEY_NONE)
                break;
            app_event = apps_host_event_from_usb_key(key_event);
            if (app_event.type != LKG_EVENT_NONE)
                (void)send_event_nonblocking(session.sock_fd, &app_event);
        }

        for (;;) {
            char command[16];
            if (!serial_parser_take(parser, command, sizeof(command)))
                break;
            normalize_button_command(command, button_reversed);
            if (strcmp(command, "SHUT") == 0 || strcmp(command, "USBD") == 0) {
                LkgEvent quit_event;
                memset(&quit_event, 0, sizeof(quit_event));
                quit_event.type = LKG_EVENT_QUIT;
                (void)send_event_nonblocking(session.sock_fd, &quit_event);
                session.serial_result = strcmp(command, "SHUT") == 0 ?
                    SERIAL_RESULT_POWER_OFF : SERIAL_RESULT_USB_DESKTOP;
                killed_by_host = 1;
                goto stop_session;
            }
            {
                LkgEvent app_event = apps_host_event_from_serial(command);
                if (app_event.type != LKG_EVENT_NONE) {
                    (void)send_event_nonblocking(session.sock_fd, &app_event);
                    if (app_event.nav == LKG_NAV_BACK) {
                        long long now = host_monotonic_ms();
                        if (loop_watchdog_count > 0 &&
                            now - last_loop_ms <= APP_LOOP_WATCHDOG_GAP_MS) {
                            ++loop_watchdog_count;
                        } else {
                            loop_watchdog_count = 1;
                        }
                        last_loop_ms = now;
                        if (loop_watchdog_count >= APP_LOOP_WATCHDOG_THRESHOLD) {
                            printf("LOOP watchdog triggered (%d consecutive LOOPs) for %s\n",
                                   loop_watchdog_count, app->name);
                            fflush(stdout);
                            killed_by_host = 1;
                            goto stop_session;
                        }
                    } else {
                        loop_watchdog_count = 0;
                    }
                }
            }
        }

        pfds[0].fd = session.sock_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = serial_fd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;
        if (keyboard->fd >= 0) {
            pfds[nfds].fd = keyboard->fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }

        ready = poll(pfds, (nfds_t)nfds, 50);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            session.serial_result = -1;
            break;
        }

        if (pfds[1].revents & POLLIN) {
            char input[512];
            ssize_t nread = read(serial_fd, input, sizeof(input));
            if (nread < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                session.serial_result = -1;
                break;
            }
            if (nread > 0)
                serial_parser_feed(parser, input, (size_t)nread);
        }
        if (pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            session.serial_result = -1;
            break;
        }

        if (pfds[0].revents & POLLIN) {
            if (handle_client_message(&session) != 0)
                break;
        }
        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
    }

stop_session:
    if (session.sock_fd >= 0) {
        close(session.sock_fd);
        session.sock_fd = -1;
    }
    if (pid > 0 && !killed_by_host && running) {
        int attempt;
        for (attempt = 0; attempt < 50; ++attempt) {
            pid_t res = waitpid(pid, &child_status, WNOHANG);
            if (res == pid) {
                child_exited = 1;
                pid = -1;
                break;
            }
            if (res < 0 && errno != EINTR) {
                pid = -1;
                break;
            }
            usleep(10000);
        }
    }
    if (pid > 0) {
        if (!killed_by_host && !running)
            killed_by_host = 1;
        stop_child(pid);
        pid = -1;
    }
    destroy_shared_layers(app_layers, session.layer_fds);
    depth->stereo = saved_stereo;
    depth->front_disparity = saved_front;
    depth->back_disparity = saved_back;
    FT_Set_Pixel_Sizes(font->face, 0, 64);

    restore_menu_drm_and_layers(display, depth, layers, &depth_suspended,
                                &layers_suspended, saved_stereo,
                                saved_front, saved_back);

    if (depth_suspended || layers_suspended)
        return -1;
    if (session.serial_result != 0)
        return session.serial_result;

    if (child_exited && !killed_by_host && running &&
        (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)) {
        char detail[128];
        if (WIFEXITED(child_status))
            snprintf(detail, sizeof(detail), "%s (exit %d)",
                     app->name, WEXITSTATUS(child_status));
        else if (WIFSIGNALED(child_status))
            snprintf(detail, sizeof(detail), "%s (signal %d)",
                     app->name, WTERMSIG(child_status));
        else
            snprintf(detail, sizeof(detail), "%s", app->name);
        show_app_error(display, font, depth, layers, serial_fd, keyboard, parser,
                       button_reversed, "App exited with error", detail);
    }

    tcflush(serial_fd, TCIFLUSH);
    return 0;
}

static int draw_apps_page(Display *display, Font *font, const DepthEffect *depth,
                          DepthLayer layers[2], DepthCacheState *cache_state,
                          const ExternalAppList *list, int selected, int scroll,
                          int *cached_selected, int *cached_scroll, int force_full)
{
    int width = (int)display->mode.hdisplay;
    int height = (int)display->mode.vdisplay;
    int count = (int)list->count + 1;
    int end = scroll + APPS_VISIBLE_ROWS;
    int visible_rows;
    DepthFrame frame;
    int result = 0;
    if (end > count)
        end = count;
    visible_rows = count - scroll;
    if (visible_rows > APPS_VISIBLE_ROWS)
        visible_rows = APPS_VISIBLE_ROWS;

    if (depth_frame_begin(&frame, display, depth, layers, cache_state, 1,
                          force_full || *cached_scroll != scroll) != 0)
        return -1;

    if (frame.full) {
        int row;
        int sel_y = apps_view_row_y(display, visible_rows, selected, scroll);
        memset(layers[0].buffer.pixels, 0, layers[0].buffer.size);
        memset(layers[1].buffer.pixels, 0, layers[1].buffer.size);
        apps_view_draw_row(display, font, depth, layers, 0,
                           apps_view_row_y(display, visible_rows, 0, scroll),
                           selected, 1, NULL);
        for (row = scroll + 1; row < end; ++row) {
            apps_view_draw_row(display, font, depth, layers, row,
                               apps_view_row_y(display, visible_rows, row, scroll),
                               selected, 1, &list->items[row - 1]);
        }
        result = depth_frame_update_rect(&frame, 0, 0, width, height,
                                         sel_y, sel_y + APPS_ROW_HEIGHT - 8);
    } else {
        int rows[2] = { *cached_selected, selected };
        int i;
        for (i = 0; i < 2; ++i) {
            int row = rows[i];
            int y;
            if (i == 1 && row == rows[0])
                continue;
            if (row < 0 || row >= count)
                continue;
            y = apps_view_row_y(display, visible_rows, row, scroll);
            draw_rect(display, &layers[0].buffer, 0, y, width, APPS_ROW_HEIGHT, 0);
            draw_rect(display, &layers[1].buffer, 0, y, width, APPS_ROW_HEIGHT, 0);
            apps_view_draw_row(display, font, depth, layers, row, y, selected, 1,
                               row >= 1 ? &list->items[row - 1] : NULL);
            result = depth_frame_update_rect(&frame, 0, y, width, APPS_ROW_HEIGHT,
                                             row == selected ? y : 0,
                                             row == selected ? y + APPS_ROW_HEIGHT - 8 : 0);
            if (result != 0)
                break;
        }
    }

    result = depth_frame_present(&frame);
    if (result == 0) {
        *cached_selected = selected;
        *cached_scroll = scroll;
    }
    return result;
}

int apps_run(Display *display, Font *font, DepthEffect *depth,
             DepthLayer layers[2], int serial_fd, UsbKeyboard *keyboard,
             int button_reversed, const char *apps_dir)
{
    ExternalAppList list;
    SerialParser parser;
    DepthCacheState cache_state;
    int selected = 0;
    int scroll = 0;
    int cached_selected = -1;
    int cached_scroll = -1;
    int changed = 1;
    int force_full = 1;
    int result = 0;

    memset(&list, 0, sizeof(list));
    memset(&parser, 0, sizeof(parser));
    memset(&cache_state, 0, sizeof(cache_state));
    if (apps_scan_directory(&list, apps_dir ? apps_dir : APPS_ROOT) != 0 || list.count == 0) {
        apps_list_free(&list);
        return 0;
    }

    layers[0].disparity = depth->front_disparity;
    layers[1].disparity = depth->back_disparity;

    while (running) {
        char command[16];
        int event;
        int count = (int)list.count + 1;

        if (changed) {
            if (draw_apps_page(display, font, depth, layers, &cache_state,
                               &list, selected, scroll, &cached_selected,
                               &cached_scroll, force_full) != 0) {
                result = -1;
                break;
            }
            changed = 0;
            force_full = 0;
        }

        event = usb_keyboard_next_command(keyboard, command, sizeof(command));
        if (event > 0)
            usb_keyboard_prepare_command(command, button_reversed);
        if (event == 0)
            event = serial_parser_next_with_fd(serial_fd, &parser, command,
                                               sizeof(command), 100, keyboard->fd);
        if (event < 0) {
            result = -1;
            break;
        }
        if (!event)
            continue;

        normalize_button_command(command, button_reversed);
        if (strcmp(command, "SHUT") == 0) {
            result = SERIAL_RESULT_POWER_OFF;
            break;
        }
        if (strcmp(command, "USBD") == 0) {
            result = SERIAL_RESULT_USB_DESKTOP;
            break;
        }
        if (strcmp(command, "LOOP") == 0)
            break;
        if (strcmp(command, "PREV") == 0) {
            if (selected == 0)
                selected = scroll + 1;
            else
                selected = (selected + 1) % count;
            scroll = apps_view_page_start(selected);
            changed = 1;
        } else if (strcmp(command, "NEXT") == 0) {
            if (selected == 0)
                selected = scroll > 0 ? scroll : count - 1;
            else if (selected == scroll + 1)
                selected = 0;
            else
                selected = (selected + count - 1) % count;
            scroll = apps_view_page_start(selected);
            changed = 1;
        } else if (strcmp(command, "PLPA") == 0) {
            if (selected == 0)
                break;
            if (selected >= 1 && selected - 1 < (int)list.count) {
                char launched_dir[NAME_MAX + 1];
                int launch_res;
                snprintf(launched_dir, sizeof(launched_dir), "%s",
                         list.items[selected - 1].dir_name);
                launch_res = launch_external_app(display, font, depth, layers,
                                                 serial_fd, keyboard, &parser,
                                                 button_reversed, &list.items[selected - 1]);
                if (launch_res != 0) {
                    result = launch_res;
                    break;
                }
                apps_list_free(&list);
                if (apps_scan_directory(&list, apps_dir ? apps_dir : APPS_ROOT) != 0 ||
                    list.count == 0) {
                    result = 0;
                    break;
                }
                selected = 0;
                {
                    size_t idx;
                    for (idx = 0; idx < list.count; ++idx) {
                        if (strcmp(list.items[idx].dir_name, launched_dir) == 0) {
                            selected = (int)idx + 1;
                            break;
                        }
                    }
                }
                scroll = apps_view_page_start(selected);
                cache_state.initialized = 0;
                cached_selected = -1;
                cached_scroll = -1;
                changed = 1;
                force_full = 1;
            }
        }
    }

    apps_list_free(&list);
    FT_Set_Pixel_Sizes(font->face, 0, 64);
    return result;
}
