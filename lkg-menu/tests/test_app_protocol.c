#define _GNU_SOURCE
#include "test.h"
#include "lkg_app.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

typedef struct {
    int host_fd;
    int mem_fds[2];
    uint32_t *layers[2];
    size_t layer_size;
    int saw_clear;
    int saw_set_depth;
    int saw_set_stereo;
    int saw_draw_rect;
    int saw_draw_text;
    int saw_draw_text_fit;
    int saw_draw_back_arrow;
    int saw_sync;
    int saw_present;
    int saw_present_rects;
    uint32_t last_rect_color;
    float last_front_depth;
    float last_back_depth;
    int text_input_calls;
    int failed;
} MockHostState;

static int mock_send_all(int fd, const void *data, size_t size)
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

static int mock_recv_all(int fd, void *data, size_t size)
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

static int mock_send_frame(int fd, uint32_t type, uint32_t seq,
                           const void *p1, size_t l1,
                           const void *p2, size_t l2)
{
    LkgMsgHeader hdr;
    hdr.type = type;
    hdr.length = (uint32_t)(l1 + l2);
    hdr.seq = seq;
    if (mock_send_all(fd, &hdr, sizeof(hdr)) != 0)
        return -1;
    if (l1 && mock_send_all(fd, p1, l1) != 0)
        return -1;
    if (l2 && mock_send_all(fd, p2, l2) != 0)
        return -1;
    return 0;
}

static int mock_send_hello_reply(int fd, uint32_t seq, const LkgAppInfo *info,
                                 const int fds[2])
{
    LkgMsgHeader hdr;
    struct msghdr msg;
    struct iovec iov;
    union {
        struct cmsghdr header;
        char bytes[CMSG_SPACE(sizeof(int) * 2)];
    } control;

    hdr.type = LKG_MSG_HELLO_REPLY;
    hdr.length = sizeof(*info);
    hdr.seq = seq;
    memset(&msg, 0, sizeof(msg));
    memset(&control, 0, sizeof(control));
    iov.iov_base = &hdr;
    iov.iov_len = sizeof(hdr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.bytes;
    msg.msg_controllen = sizeof(control.bytes);
    {
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * 2);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * 2);
    }
    if (sendmsg(fd, &msg, MSG_NOSIGNAL) != (ssize_t)sizeof(hdr))
        return -1;
    return mock_send_all(fd, info, sizeof(*info));
}

static void *mock_host_thread(void *arg)
{
    MockHostState *host = (MockHostState *)arg;
    for (;;) {
        LkgMsgHeader hdr;
        uint8_t buf[LKG_APP_MSG_MAX_PAYLOAD];
        if (mock_recv_all(host->host_fd, &hdr, sizeof(hdr)) != 0)
            break;
        if (hdr.length > sizeof(buf) ||
            (hdr.length && mock_recv_all(host->host_fd, buf, hdr.length) != 0)) {
            host->failed = 1;
            break;
        }
        if (hdr.type == LKG_MSG_HELLO) {
            LkgAppInfo info;
            memset(&info, 0, sizeof(info));
            info.api_version = LKG_APP_API_VERSION;
            info.render_mode = LKG_RENDER_HOSTED;
            info.screen_width = 240;
            info.screen_height = 34;
            info.row_height = 2;
            info.margin = 20;
            info.top_margin = 0;
            info.bar_gap = 8;
            info.menu_3d = 1;
            info.button_reversed = 1;
            info.font_large = LKG_FONT_SIZE_LARGE;
            info.font_medium = LKG_FONT_SIZE_MEDIUM;
            info.font_caption = LKG_FONT_SIZE_CAPTION;
            info.font_small = LKG_FONT_SIZE_SMALL;
            info.font_key = LKG_FONT_SIZE_KEY;
            info.depth_front = LKG_DEPTH_POP_OUT;
            info.depth_back = LKG_DEPTH_RECESSED;
            info.depth_zero = LKG_DEPTH_SCREEN;
            info.color_bg = LKG_COLOR_BG;
            info.color_bar = LKG_COLOR_BAR;
            info.color_text_active = LKG_COLOR_TEXT_ACTIVE;
            info.color_text = LKG_COLOR_TEXT;
            info.color_text_bright = LKG_COLOR_TEXT_BRIGHT;
            info.color_text_muted = LKG_COLOR_TEXT_MUTED;
            info.color_surface = LKG_COLOR_SURFACE;
            info.color_surface_light = LKG_COLOR_SURFACE_LIGHT;
            info.layer_stride = 240 * 4;
            info.layer_size = host->layer_size;
            if (mock_send_hello_reply(host->host_fd, hdr.seq, &info, host->mem_fds) != 0) {
                host->failed = 1;
                break;
            }
        } else if (hdr.type == LKG_MSG_CLEAR) {
            ++host->saw_clear;
        } else if (hdr.type == LKG_MSG_SET_DEPTH) {
            LkgSetDepthPayload p;
            memcpy(&p, buf, sizeof(p));
            host->last_front_depth = p.front_disparity;
            host->last_back_depth = p.back_disparity;
            ++host->saw_set_depth;
        } else if (hdr.type == LKG_MSG_SET_STEREO) {
            ++host->saw_set_stereo;
        } else if (hdr.type == LKG_MSG_DRAW_RECT) {
            LkgDrawRectPayload p;
            memcpy(&p, buf, sizeof(p));
            host->last_rect_color = p.color;
            ++host->saw_draw_rect;
        } else if (hdr.type == LKG_MSG_DRAW_TEXT) {
            LkgDrawTextPayload p;
            memcpy(&p, buf, sizeof(p));
            if (p.max_width > 0)
                ++host->saw_draw_text_fit;
            else
                ++host->saw_draw_text;
        } else if (hdr.type == LKG_MSG_DRAW_BACK_ARROW) {
            ++host->saw_draw_back_arrow;
        } else if (hdr.type == LKG_MSG_MEASURE_TEXT) {
            LkgMeasureReplyPayload reply;
            reply.status = 0;
            reply.metrics.width = 120;
            reply.metrics.ascender = 50;
            reply.metrics.descender = -14;
            reply.metrics.height = 64;
            if (mock_send_frame(host->host_fd, LKG_MSG_MEASURE_REPLY, hdr.seq,
                                &reply, sizeof(reply), NULL, 0) != 0) {
                host->failed = 1;
                break;
            }
        } else if (hdr.type == LKG_MSG_SYNC) {
            LkgReplyPayload reply;
            reply.status = 0;
            ++host->saw_sync;
            if (mock_send_frame(host->host_fd, LKG_MSG_REPLY, hdr.seq,
                                &reply, sizeof(reply), NULL, 0) != 0) {
                host->failed = 1;
                break;
            }
        } else if (hdr.type == LKG_MSG_PRESENT) {
            LkgPresentPayload p;
            LkgReplyPayload reply;
            memcpy(&p, buf, sizeof(p));
            if (p.full)
                ++host->saw_present;
            else
                ++host->saw_present_rects;
            if (host->saw_present == 1 && host->saw_present_rects == 0) {
                /* Inject an asynchronous event right before the PRESENT reply! */
                LkgEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = LKG_EVENT_KEY;
                ev.key = LKG_KEY_CHAR;
                ev.character = 'x';
                ev.keycode = 45;
                if (mock_send_frame(host->host_fd, LKG_MSG_EVENT, 0,
                                    &ev, sizeof(ev), NULL, 0) != 0) {
                    host->failed = 1;
                    break;
                }
            }
            reply.status = 0;
            if (mock_send_frame(host->host_fd, LKG_MSG_REPLY, hdr.seq,
                                &reply, sizeof(reply), NULL, 0) != 0) {
                host->failed = 1;
                break;
            }
        } else if (hdr.type == LKG_MSG_TEXT_INPUT) {
            LkgTextInputReplyPayload reply;
            ++host->text_input_calls;
            if (host->text_input_calls == 1) {
                const char *entered = "Hello SDK";
                reply.accepted = 1;
                reply.text_len = (uint32_t)strlen(entered);
                if (mock_send_frame(host->host_fd, LKG_MSG_TEXT_INPUT_REPLY, hdr.seq,
                                    &reply, sizeof(reply), entered, reply.text_len) != 0) {
                    host->failed = 1;
                    break;
                }
            } else {
                reply.accepted = 0;
                reply.text_len = 0;
                if (mock_send_frame(host->host_fd, LKG_MSG_TEXT_INPUT_REPLY, hdr.seq,
                                    &reply, sizeof(reply), NULL, 0) != 0) {
                    host->failed = 1;
                    break;
                }
            }
        }
    }
    return NULL;
}

static int test_fexecve_non_exec_binary(void)
{
    const char *candidate = access("/bin/true", R_OK) == 0 ? "/bin/true" : "/usr/bin/true";
    char tmp_path[] = "/tmp/lkg-fexecve-XXXXXX";
    int tmp_fd;
    int src_fd;
    int mem_fd;
    uint8_t buf[4096];
    pid_t pid;
    int status = 0;

    src_fd = open(candidate, O_RDONLY | O_CLOEXEC);
    TEST_ASSERT(src_fd >= 0);
    tmp_fd = mkstemp(tmp_path);
    TEST_ASSERT(tmp_fd >= 0);
    for (;;) {
        ssize_t n = read(src_fd, buf, sizeof(buf));
        TEST_ASSERT(n >= 0);
        if (n == 0)
            break;
        TEST_ASSERT(write(tmp_fd, buf, (size_t)n) == n);
    }
    close(src_fd);
    close(tmp_fd);
    TEST_ASSERT(chmod(tmp_path, 0644) == 0);

    /* Load non-executable file into memfd and fexecve it */
    src_fd = open(tmp_path, O_RDONLY | O_CLOEXEC);
    TEST_ASSERT(src_fd >= 0);
    mem_fd = memfd_create("lkg-test-exec", MFD_CLOEXEC);
    TEST_ASSERT(mem_fd >= 0);
    for (;;) {
        ssize_t n = read(src_fd, buf, sizeof(buf));
        TEST_ASSERT(n >= 0);
        if (n == 0)
            break;
        TEST_ASSERT(write(mem_fd, buf, (size_t)n) == n);
    }
    close(src_fd);
    unlink(tmp_path);
    TEST_ASSERT(fchmod(mem_fd, 0700) == 0);
    TEST_ASSERT(lseek(mem_fd, 0, SEEK_SET) == 0);

    pid = fork();
    TEST_ASSERT(pid >= 0);
    if (pid == 0) {
        char *argv_exec[] = { "true", NULL };
        fexecve(mem_fd, argv_exec, environ);
        _exit(127);
    }
    close(mem_fd);
    TEST_ASSERT(waitpid(pid, &status, 0) == pid);
    TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}

int test_app_protocol(void)
{
    int sockets[2];
    MockHostState host;
    pthread_t thread;
    LkgApp *app;
    const LkgAppInfo *info;
    LkgTextMetrics metrics;
    LkgEvent event;
    LkgRectUpdate rects[2];
    char input_buf[65];
    uint32_t *front_px;
    uint32_t *back_px;
    int i;

    memset(&host, 0, sizeof(host));
    host.layer_size = 240 * 34 * 4;
    for (i = 0; i < 2; ++i) {
        host.mem_fds[i] = memfd_create("lkg-test-layer", MFD_CLOEXEC);
        TEST_ASSERT(host.mem_fds[i] >= 0);
        TEST_ASSERT(ftruncate(host.mem_fds[i], (off_t)host.layer_size) == 0);
        host.layers[i] = mmap(NULL, host.layer_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, host.mem_fds[i], 0);
        TEST_ASSERT(host.layers[i] != MAP_FAILED);
    }
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    host.host_fd = sockets[0];
    TEST_ASSERT(pthread_create(&thread, NULL, mock_host_thread, &host) == 0);

    app = lkg_app_open_fd(sockets[1]);
    TEST_ASSERT(app != NULL);
    info = lkg_app_info(app);
    TEST_ASSERT(info != NULL);
    TEST_ASSERT(info->api_version == LKG_APP_API_VERSION);
    TEST_ASSERT(info->render_mode == LKG_RENDER_HOSTED);
    TEST_ASSERT(info->depth_front == LKG_DEPTH_POP_OUT);
    TEST_ASSERT(info->depth_back == LKG_DEPTH_RECESSED);
    TEST_ASSERT(LKG_DEPTH_SUBTLE_POP_OUT == 35.0f);
    TEST_ASSERT(LKG_DEPTH_SUBTLE_RECESSED == -28.0f);
    TEST_ASSERT(LKG_DEPTH_DEEP_POP_OUT == 70.0f);
    TEST_ASSERT(LKG_DEPTH_DEEP_RECESSED == -56.0f);
    TEST_ASSERT(info->font_large == LKG_FONT_SIZE_LARGE);
    TEST_ASSERT(info->font_medium == LKG_FONT_SIZE_MEDIUM);
    TEST_ASSERT(info->font_caption == LKG_FONT_SIZE_CAPTION);
    TEST_ASSERT(info->font_small == LKG_FONT_SIZE_SMALL);
    TEST_ASSERT(info->color_bg == LKG_COLOR_BG);
    TEST_ASSERT(lkg_app_fd(app) == sockets[1]);

    front_px = lkg_app_layer_pixels(app, LKG_LAYER_FRONT);
    back_px = lkg_app_layer_pixels(app, LKG_LAYER_BACK);
    TEST_ASSERT(front_px != NULL && back_px != NULL);
    front_px[0] = 0x11223344u;
    back_px[1] = 0x55667788u;
    TEST_ASSERT(host.layers[0][0] == 0x11223344u);
    TEST_ASSERT(host.layers[1][1] == 0x55667788u);

    TEST_ASSERT(lkg_app_clear(app) == 0);
    TEST_ASSERT(lkg_app_set_depth(app, 35.0f, -28.0f) == 0);
    TEST_ASSERT(lkg_app_set_stereo(app, 0) == 0);
    TEST_ASSERT(lkg_app_draw_rect(app, LKG_LAYER_FRONT, 1, 2, 3, 4, 0xffaabbccu) == 0);
    TEST_ASSERT(lkg_app_draw_text(app, LKG_LAYER_BACK, "Hi", 10, 20, LKG_FONT_SIZE_MEDIUM,
                                  LKG_COLOR_TEXT, 0) == 0);
    TEST_ASSERT(lkg_app_draw_text_fit(app, LKG_LAYER_FRONT, "Fit", 10, 20, LKG_FONT_SIZE_LARGE,
                                      LKG_COLOR_TEXT_ACTIVE, 100, 1) == 0);
    TEST_ASSERT(lkg_app_draw_back_arrow(app, LKG_LAYER_FRONT, 4, 8, LKG_COLOR_TEXT_ACTIVE) == 0);
    TEST_ASSERT(lkg_app_measure_text(app, "Ag", LKG_FONT_SIZE_LARGE, &metrics) == 0);
    TEST_ASSERT(metrics.width == 120 && metrics.height == 64);
    TEST_ASSERT(lkg_app_row_baseline(100, 150, &metrics) == 100 + (150 - 64) / 2 + 50);
    TEST_ASSERT(lkg_app_draw_menu_row(app, "Row", 0, 1, 1) == 0);
    TEST_ASSERT(lkg_app_sync(app) == 0);
    TEST_ASSERT(lkg_app_present(app, 0, 8) == 0);

    /* Interleaved event received during lkg_app_present must be queued and returned here */
    TEST_ASSERT(lkg_app_poll_event(app, &event, 0) == 1);
    TEST_ASSERT(event.type == LKG_EVENT_KEY && event.key == LKG_KEY_CHAR &&
                event.character == 'x' && event.keycode == 45);
    TEST_ASSERT(lkg_app_poll_event(app, &event, 0) == 0);

    rects[0].x = 0; rects[0].y = 0; rects[0].width = 16; rects[0].height = 1;
    rects[0].bar_top = 0; rects[0].bar_bottom = 0;
    rects[1].x = 0; rects[1].y = 1; rects[1].width = 16; rects[1].height = 1;
    rects[1].bar_top = 1; rects[1].bar_bottom = 2;
    TEST_ASSERT(lkg_app_present_rects(app, rects, 2) == 0);

    TEST_ASSERT(lkg_app_text_input(app, "Edit Greeting", "Init", input_buf, sizeof(input_buf)) == 1);
    TEST_STRING(input_buf, "Hello SDK");
    TEST_ASSERT(lkg_app_text_input(app, "Edit Greeting", NULL, input_buf, sizeof(input_buf)) == 0);
    TEST_STRING(input_buf, "");

    lkg_app_close(app);
    close(host.host_fd);
    pthread_join(thread, NULL);

    TEST_ASSERT(!host.failed);
    TEST_ASSERT(host.saw_clear == 1);
    TEST_ASSERT(host.saw_set_depth == 1 && host.last_front_depth == 35.0f &&
                host.last_back_depth == -28.0f);
    TEST_ASSERT(host.saw_set_stereo == 1);
    TEST_ASSERT(host.saw_draw_rect == 3);
    TEST_ASSERT(host.saw_draw_text == 1);
    TEST_ASSERT(host.saw_draw_text_fit == 2);
    TEST_ASSERT(host.saw_draw_back_arrow == 2);
    TEST_ASSERT(host.saw_sync == 1);
    TEST_ASSERT(host.saw_present == 1);
    TEST_ASSERT(host.saw_present_rects == 1);
    TEST_ASSERT(host.text_input_calls == 2);

    for (i = 0; i < 2; ++i) {
        munmap(host.layers[i], host.layer_size);
        close(host.mem_fds[i]);
    }

    TEST_ASSERT(test_fexecve_non_exec_binary() == 0);
    return 0;
}
