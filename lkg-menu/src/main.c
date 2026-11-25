#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include <drm.h>
#include <drm_mode.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <qrencode.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "apps_host.h"
#include "apps_scan.h"
#include "gallery.h"
#include "file_manager_view.h"
#include "menu_view.h"
#include "serial_parser.h"
#include "wifi.h"
#include "depth.h"
#include "depth_anim.h"
#include "depth_frame.h"
#include "thumbnail.h"
#include "ui.h"
#include "usb_keyboard.h"
#include "volume.h"

volatile sig_atomic_t running = 1;

static void stop(int signal_number)
{
    (void)signal_number;
    running = 0;
}

int display_wait_for_flip(Display *display)
{
    struct pollfd pfd;
    while (display->flip_pending) {
        char events[4096];
        ssize_t count;
        ssize_t offset;
        pfd.fd = display->fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, -1) < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        count = read(display->fd, events, sizeof(events));
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return -1;
        }
        for (offset = 0; offset + (ssize_t)sizeof(struct drm_event) <= count;) {
            struct drm_event *event = (struct drm_event *)(events + offset);
            if (event->type == DRM_EVENT_FLIP_COMPLETE) {
                if (display->flip_complete)
                    display->flip_complete(display->flip_data);
                display->flip_pending = 0;
            }
            offset += event->length;
        }
    }
    return 0;
}

static int choose_display(Display *display)
{
    drmModeRes *resources = drmModeGetResources(display->fd);
    drmModeConnector *connector = NULL;
    drmModeEncoder *encoder = NULL;
    int i;
    if (!resources)
        return -1;
    for (i = 0; i < resources->count_connectors; ++i) {
        connector = drmModeGetConnector(display->fd, resources->connectors[i]);
        if (connector && connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0)
            break;
        drmModeFreeConnector(connector);
        connector = NULL;
    }
    if (!connector) {
        drmModeFreeResources(resources);
        return -1;
    }
    display->connector_id = connector->connector_id;
    display->mode = connector->modes[0];
    for (i = 0; i < connector->count_modes; ++i) {
        drmModeModeInfo *mode = &connector->modes[i];
        if ((mode->type & DRM_MODE_TYPE_PREFERRED) ||
            (mode->hdisplay == 1440 && mode->vdisplay == 2560)) {
            display->mode = *mode;
            break;
        }
    }
    if (connector->encoder_id)
        encoder = drmModeGetEncoder(display->fd, connector->encoder_id);
    if (encoder)
        display->crtc_id = encoder->crtc_id;
    if (!display->crtc_id) {
        int j;
        for (i = 0; i < connector->count_encoders && !display->crtc_id; ++i) {
            encoder = drmModeGetEncoder(display->fd, connector->encoders[i]);
            if (!encoder)
                continue;
            for (j = 0; j < resources->count_crtcs; ++j) {
                if (encoder->possible_crtcs & (1u << j)) {
                    display->crtc_id = resources->crtcs[j];
                    break;
                }
            }
            drmModeFreeEncoder(encoder);
            encoder = NULL;
        }
    }
    drmModeFreeEncoder(encoder);
    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    return display->crtc_id ? 0 : -1;
}

static int display_open(Display *display, const char *path)
{
    memset(display, 0, sizeof(*display));
    display->fd = -1;
    display->fd = open(path, O_RDWR | O_CLOEXEC);
    if (display->fd < 0 || choose_display(display) != 0)
        goto fail;
    display->saved_crtc = drmModeGetCrtc(display->fd, display->crtc_id);
    printf("Display: %s, %ux%u@%u\n", path, display->mode.hdisplay,
           display->mode.vdisplay, display->mode.vrefresh);
    return 0;
fail:
    close(display->fd);
    display->fd = -1;
    return -1;
}

static void display_close(Display *display)
{
    if (display->fd < 0)
        return;
    display_wait_for_flip(display);
    if (display->saved_crtc) {
        drmModeSetCrtc(display->fd, display->saved_crtc->crtc_id,
                       display->saved_crtc->buffer_id, display->saved_crtc->x,
                       display->saved_crtc->y, &display->connector_id, 1,
                       &display->saved_crtc->mode);
        drmModeFreeCrtc(display->saved_crtc);
    }
    close(display->fd);
    display->fd = -1;
}

#define SETTINGS_PATH "/var/lib/lkg/config/menu/lkg-menu.conf"
#define LEGACY_SETTINGS_PATH "/home/lkg/.lkg-menu.conf"
static void load_settings(int *slideshow_duration, int *button_reversed,
                          int *auto_play, int *menu_3d)
{
    FILE *file = fopen(SETTINGS_PATH, "r");
    char line[64];
    *slideshow_duration = 5;
    *button_reversed = 1;
    *auto_play = 0;
    *menu_3d = MENU_3D_SUBTLE;
    if (!file)
        file = fopen(LEGACY_SETTINGS_PATH, "r");
    if (!file)
        return;
    while (fgets(line, sizeof(line), file)) {
        int seconds;
        if (sscanf(line, "slideshow_duration=%d", &seconds) == 1 &&
            menu_duration_index(seconds) >= 0)
            *slideshow_duration = seconds;
        else if (strncmp(line, "button_orientation=original", 27) == 0)
            *button_reversed = 0;
        else if (strncmp(line, "button_orientation=reversed", 27) == 0)
            *button_reversed = 1;
        else if (strncmp(line, "auto_play=off", 13) == 0)
            *auto_play = 0;
        else if (strncmp(line, "auto_play=on", 12) == 0)
            *auto_play = 1;
        else if (strncmp(line, "menu_3d=off", 11) == 0)
            *menu_3d = MENU_3D_OFF;
        else if (strncmp(line, "menu_3d=subtle", 14) == 0)
            *menu_3d = MENU_3D_SUBTLE;
        else if (strncmp(line, "menu_3d=deep", 12) == 0 ||
                 strncmp(line, "menu_3d=on", 10) == 0)
            *menu_3d = MENU_3D_DEEP;
    }
    fclose(file);
}

static int save_settings_to(const char *path, int seconds, int button_reversed,
                            int auto_play, int menu_3d)
{
    char temporary_path[PATH_MAX];
    const char *menu_3d_value = menu_3d == MENU_3D_DEEP ? "deep" :
                                menu_3d == MENU_3D_SUBTLE ? "subtle" : "off";
    FILE *file;
    int failed;
    if (snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", path) >=
        (int)sizeof(temporary_path))
        return -1;
    file = fopen(temporary_path, "w");
    if (!file)
        return -1;
    failed = fprintf(file, "slideshow_duration=%d\nbutton_orientation=%s\nauto_play=%s\nmenu_3d=%s\n",
                     seconds, button_reversed ? "reversed" : "original",
                     auto_play ? "on" : "off", menu_3d_value) < 0;
    if (fclose(file) != 0)
        failed = 1;
    if (failed) {
        unlink(temporary_path);
        return -1;
    }
    if (rename(temporary_path, path) != 0) {
        unlink(temporary_path);
        return -1;
    }
    return 0;
}

static int save_settings(int seconds, int button_reversed, int auto_play, int menu_3d)
{
    if (save_settings_to(SETTINGS_PATH, seconds, button_reversed, auto_play, menu_3d) == 0)
        return 0;
    return save_settings_to(LEGACY_SETTINGS_PATH, seconds, button_reversed, auto_play, menu_3d);
}

static int clear_cache_directory(const char *path)
{
    DIR *directory = opendir(path);
    struct dirent *entry;
    int result = 0;
    if (!directory)
        return errno == ENOENT ? 0 : -1;
    while ((entry = readdir(directory)) != NULL) {
        char child[PATH_MAX];
        struct stat status;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >=
            (int)sizeof(child) || lstat(child, &status) != 0) {
            result = -1;
            continue;
        }
        if (S_ISDIR(status.st_mode)) {
            if (clear_cache_directory(child) != 0 || rmdir(child) != 0)
                result = -1;
        } else if (unlink(child) != 0) {
            result = -1;
        }
    }
    closedir(directory);
    return result;
}

static int read_key_value(const char *path, const char *key, char *value, size_t size)
{
    FILE *file = fopen(path, "r");
    char line[512];
    size_t key_length = strlen(key);
    int found = -1;
    if (!file)
        return -1;
    while (fgets(line, sizeof(line), file)) {
        char *start;
        char *end;
        size_t length;
        if (strncmp(line, key, key_length) != 0)
            continue;
        start = line + key_length;
        while (*start == ' ' || *start == '\t')
            ++start;
        if (*start != ':' && *start != '=')
            continue;
        ++start;
        while (*start == ' ' || *start == '\t')
            ++start;
        end = start + strlen(start);
        while (end > start && (end[-1] == '\n' || end[-1] == '\r' ||
                               end[-1] == ' ' || end[-1] == '\t'))
            --end;
        length = (size_t)(end - start);
        if (length >= size)
            length = size - 1;
        memcpy(value, start, length);
        value[length] = '\0';
        found = 0;
        break;
    }
    fclose(file);
    return found;
}

static int read_json_string(const char *path, const char *key, char *value, size_t size)
{
    FILE *file = fopen(path, "rb");
    char buffer[4096];
    char needle[64];
    size_t read_size;
    const char *cursor;
    const char *start;
    const char *end;
    size_t length;
    if (!file || size == 0)
        return -1;
    read_size = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[read_size] = '\0';
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) >= (int)sizeof(needle))
        return -1;
    cursor = strstr(buffer, needle);
    if (!cursor)
        return -1;
    cursor = strchr(cursor + strlen(needle), ':');
    if (!cursor)
        return -1;
    ++cursor;
    while (*cursor && isspace((unsigned char)*cursor))
        ++cursor;
    if (*cursor != '"')
        return -1;
    start = cursor + 1;
    end = strchr(start, '"');
    if (!end || end == start)
        return -1;
    length = (size_t)(end - start);
    if (length >= size)
        length = size - 1;
    memcpy(value, start, length);
    value[length] = '\0';
    return 0;
}

static int read_unquoted_key_value(const char *path, const char *key,
                                   char *out, size_t out_size)
{
    char raw[128];
    size_t length;
    const char *value = raw;
    if (read_key_value(path, key, raw, sizeof(raw)) != 0 || out_size == 0)
        return -1;
    length = strlen(raw);
    if (length >= 2 && raw[0] == '"' && raw[length - 1] == '"') {
        raw[length - 1] = '\0';
        value = raw + 1;
    }
    snprintf(out, out_size, "%s", value);
    return 0;
}

static void collect_system_info(Display *display, SystemInfo *info)
{
    char part[64];
    char total[64];
    char available[64];
    char driver[64];
    char compatible[128];
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    struct statvfs disk_status;
    struct utsname kernel_info;
    struct ifaddrs *addresses = NULL;
    struct ifaddrs *address;

    if (read_unquoted_key_value("/etc/openglass-release", "PRETTY_NAME",
                                info->version, sizeof(info->version)) != 0 &&
        read_unquoted_key_value("/etc/openglass-release", "VERSION",
                                info->version, sizeof(info->version)) != 0)
        snprintf(info->version, sizeof(info->version), "Unavailable");

    if (read_json_string("/run/lkg/visual.json", "serial", info->serial,
                         sizeof(info->serial)) != 0)
        snprintf(info->serial, sizeof(info->serial), "Unavailable");

    if (cores < 1)
        cores = 1;
    if (read_key_value("/proc/cpuinfo", "CPU part", part, sizeof(part)) == 0 &&
        strcmp(part, "0xd03") == 0)
        snprintf(info->cpu, sizeof(info->cpu), "%ld x Cortex-A53", cores);
    else
        snprintf(info->cpu, sizeof(info->cpu), "%ld cores", cores);

    if (read_key_value("/proc/meminfo", "MemTotal", total, sizeof(total)) == 0 &&
        read_key_value("/proc/meminfo", "MemAvailable", available, sizeof(available)) == 0)
        snprintf(info->memory, sizeof(info->memory), "%.1f GiB total, %.1f GiB available",
                 strtod(total, NULL) / 1048576.0,
                 strtod(available, NULL) / 1048576.0);
    else
        snprintf(info->memory, sizeof(info->memory), "Unavailable");

    if (read_key_value("/sys/class/drm/renderD128/device/uevent", "DRIVER",
                       driver, sizeof(driver)) == 0 &&
        read_key_value("/sys/class/drm/renderD128/device/uevent", "OF_COMPATIBLE_0",
                       compatible, sizeof(compatible)) == 0 &&
        strstr(compatible, "sun50i-h616-mali"))
        snprintf(info->gpu, sizeof(info->gpu), "Mali-G31 (%s)", driver);
    else
        snprintf(info->gpu, sizeof(info->gpu), "Unavailable");

    snprintf(info->display, sizeof(info->display), "%u x %u @ %u Hz",
             display->mode.hdisplay, display->mode.vdisplay, display->mode.vrefresh);
    if (statvfs("/home/lkg/data", &disk_status) == 0 ||
        statvfs("/", &disk_status) == 0)
        snprintf(info->disk, sizeof(info->disk), "%.1f GiB total, %.1f GiB available",
                 (double)disk_status.f_blocks * disk_status.f_frsize / 1073741824.0,
                 (double)disk_status.f_bavail * disk_status.f_frsize / 1073741824.0);
    else
        snprintf(info->disk, sizeof(info->disk), "Unavailable");
    if (read_unquoted_key_value("/etc/os-release", "PRETTY_NAME",
                                info->os, sizeof(info->os)) != 0)
        snprintf(info->os, sizeof(info->os), "Unavailable");
    if (uname(&kernel_info) == 0)
        snprintf(info->kernel, sizeof(info->kernel), "%s", kernel_info.release);
    else
        snprintf(info->kernel, sizeof(info->kernel), "Unavailable");
    {
        struct passwd *user_info = getpwuid(getuid());
        char hostname[64];
        if (user_info && user_info->pw_name && user_info->pw_name[0] != '\0' &&
            gethostname(hostname, sizeof(hostname)) == 0) {
            hostname[sizeof(hostname) - 1] = '\0';
            if (hostname[0] != '\0')
                snprintf(info->ssh, sizeof(info->ssh), "%s@%s.local",
                         user_info->pw_name, hostname);
            else
                snprintf(info->ssh, sizeof(info->ssh), "Unavailable");
        } else {
            snprintf(info->ssh, sizeof(info->ssh), "Unavailable");
        }
    }
    snprintf(info->ip, sizeof(info->ip), "Unavailable");
    if (getifaddrs(&addresses) == 0) {
        for (address = addresses; address; address = address->ifa_next) {
            const struct sockaddr_in *ipv4;
            if (!address->ifa_addr || address->ifa_addr->sa_family != AF_INET ||
                !(address->ifa_flags & IFF_UP) || (address->ifa_flags & IFF_LOOPBACK))
                continue;
            ipv4 = (const struct sockaddr_in *)address->ifa_addr;
            if (inet_ntop(AF_INET, &ipv4->sin_addr, info->ip, sizeof(info->ip)))
                break;
        }
        freeifaddrs(addresses);
    }
}

static void draw_file_manager(Display *display, Font *font, const DepthEffect *depth,
                              DepthLayer layers[2])
{
    SystemInfo info;
    collect_system_info(display, &info);
    file_manager_view_draw(display, font, depth, layers, info.ip);
}

static uint64_t menu_content_key(int page, int has_apps, int menu_3d)
{
    return ((uint64_t)page << 3) |
           ((uint64_t)(menu_3d & 3) << 1) |
           (uint64_t)(has_apps ? 1 : 0);
}

static void menu_row_bounds(const Display *display, int page, int has_apps,
                            int row, DepthAnimRect *rect)
{
    const char **items;
    int count;
    const int row_height = (int)display->mode.vdisplay / 17;
    int start_y;
    int y;
    menu_items(page, has_apps, &items, &count);
    start_y = ((int)display->mode.vdisplay - row_height * count) / 2;
    y = start_y + row * row_height;
    rect->x0 = 0;
    rect->y0 = y;
    rect->x1 = (int)display->mode.hdisplay;
    rect->y1 = y + row_height;
    rect->disparity = 0.0f;
}

static int menu_anim_settle_row(Display *display, Font *font, const DepthEffect *depth,
                                DepthLayer layers[2], int page, int has_apps,
                                int row, int selected, const SystemInfo *info)
{
    DepthAnimRect rect;
    menu_row_bounds(display, page, has_apps, row, &rect);
    menu_view_draw_row(display, font, depth, layers, page, has_apps, row, selected, info);
    return depth_anim_upload_rect(display, depth, layers,
                                  rect.x0, rect.y0,
                                  rect.x1 - rect.x0, rect.y1 - rect.y0);
}

static int menu_anim_finish(Display *display, Font *font, const DepthEffect *depth,
                            DepthLayer layers[2], DepthCacheState *cache_state,
                            DepthAnim *anim, int page, int has_apps,
                            int selected, const SystemInfo *info)
{
    if (!anim->active)
        return 0;
    if (menu_anim_settle_row(display, font, depth, layers, page, has_apps,
                             anim->old_id, selected, info) != 0)
        return -1;
    depth_cache_commit(depth, cache_state, anim->content_key);
    anim->active = 0;
    return 0;
}

static void menu_anim_cancel(Display *display, Font *font, const DepthEffect *depth,
                             DepthLayer layers[2], DepthCacheState *cache_state,
                             DepthAnim *anim, int page, int has_apps, int menu_3d,
                             int selected, const SystemInfo *info)
{
    uint64_t key = menu_content_key(page, has_apps, menu_3d);
    if (!anim->active)
        return;
    if (cache_state->initialized && anim->content_key == key) {
        int i;
        if (menu_anim_settle_row(display, font, depth, layers, page, has_apps,
                                 anim->old_id, selected, info) == 0) {
            anim->new_rect.disparity = depth->front_disparity;
            for (i = 0; i < anim->clip_count; ++i) {
                if (depth_gallery_anim_region(display, depth, layers,
                                              &anim->new_rect, NULL,
                                              anim->clip_x[i], anim->clip_y[i],
                                              anim->clip_w[i], anim->clip_h[i],
                                              anim->bar_top, anim->bar_bottom, 1) != 0) {
                    anim->active = 0;
                    return;
                }
            }
            depth_cache_commit(depth, cache_state, key);
        }
    }
    anim->active = 0;
}

static int menu_anim_step(Display *display, Font *font, const DepthEffect *depth,
                          DepthLayer layers[2], DepthCacheState *cache_state,
                          DepthAnim *anim, int page, int has_apps,
                          int selected, const SystemInfo *info)
{
    int finished = 0;
    if (!anim->active)
        return 0;
    if (depth_anim_step(anim, display, depth, layers, "Menu", &finished) != 0)
        return -1;
    if (finished)
        return menu_anim_finish(display, font, depth, layers, cache_state,
                                anim, page, has_apps, selected, info);
    return 0;
}

static int menu_anim_begin(Display *display, Font *font, const DepthEffect *depth,
                           DepthLayer layers[2], DepthCacheState *cache_state,
                           DepthAnim *anim, int page, int has_apps, int menu_3d,
                           int cached_page, int old_row, int new_row,
                           const SystemInfo *info)
{
    uint64_t key = menu_content_key(page, has_apps, menu_3d);
    const int screen_w = (int)display->mode.hdisplay;
    const int row_height = (int)display->mode.vdisplay / 17;
    float old_start = depth->front_disparity;
    float new_start = depth->back_disparity;
    int bar_top;
    int bar_bottom;
    DepthAnimRect old_rect;
    DepthAnimRect new_rect;
    if (page == PAGE_FILE_MANAGER || !depth->stereo ||
        !depth_gallery_direct_upload(display, depth) ||
        cached_page != page || old_row < 0 || old_row == new_row ||
        (!anim->active ? depth_cache_needs_full(depth, cache_state, key)
                       : (!cache_state->initialized || cache_state->content_key != key)))
        return 0;
    if (anim->active) {
        DepthAnim prev = *anim;
        if (old_row == prev.new_id)
            old_start = prev.cur_new_disparity;
        if (new_row == prev.old_id)
            new_start = prev.cur_old_disparity;
        if (prev.old_id != old_row && prev.old_id != new_row) {
            int cx, cy, cw, ch;
            if (menu_anim_settle_row(display, font, depth, layers, page, has_apps,
                                     prev.old_id, new_row, info) != 0)
                return -1;
            depth_anim_clip_rect(display, depth,
                                 prev.old_rect.x0, prev.old_rect.y0,
                                 prev.old_rect.x1 - prev.old_rect.x0,
                                 prev.old_rect.y1 - prev.old_rect.y0,
                                 &cx, &cy, &cw, &ch);
            if (cw > 0 && ch > 0 &&
                depth_gallery_anim_region(display, depth, layers, NULL, NULL,
                                          cx, cy, cw, ch, 0, 0, 1) != 0)
                return -1;
        }
        anim->active = 0;
    }
    menu_row_bounds(display, page, has_apps, old_row, &old_rect);
    menu_row_bounds(display, page, has_apps, new_row, &new_rect);
    bar_top = new_rect.y0;
    bar_bottom = new_rect.y0 + row_height - 8;
    menu_view_draw_row_anim(display, font, depth, layers, page, has_apps, old_row, 0, info);
    menu_view_draw_row_anim(display, font, depth, layers, page, has_apps, new_row, 1, info);
    depth_anim_tight_x(display, &layers[0].buffer, old_rect.y0, old_rect.y1 - old_rect.y0,
                       old_rect.x0, old_rect.x1, &old_rect.x0, &old_rect.x1);
    depth_anim_tight_x(display, &layers[0].buffer, new_rect.y0, new_rect.y1 - new_rect.y0,
                       new_rect.x0, new_rect.x1, &new_rect.x0, &new_rect.x1);
    if (depth_anim_upload_rect(display, depth, layers,
                               old_rect.x0, old_rect.y0,
                               old_rect.x1 - old_rect.x0,
                               old_rect.y1 - old_rect.y0) != 0 ||
        depth_anim_upload_rect(display, depth, layers,
                               new_rect.x0, new_rect.y0,
                               new_rect.x1 - new_rect.x0,
                               new_rect.y1 - new_rect.y0) != 0 ||
        depth_gallery_anim_clear(display, depth, 0, old_rect.y0, screen_w,
                                 old_rect.y1 - old_rect.y0, bar_top, bar_bottom) != 0 ||
        depth_gallery_anim_clear(display, depth, 0, new_rect.y0, screen_w,
                                 new_rect.y1 - new_rect.y0, bar_top, bar_bottom) != 0)
        return -1;
    depth_anim_start(anim, display, depth, old_row, new_row,
                     &old_rect, &new_rect,
                     bar_top, bar_bottom, 0,
                     key, old_start, new_start);
    if (menu_anim_step(display, font, depth, layers, cache_state,
                       anim, page, has_apps, new_row, info) != 0)
        return -1;
    return 1;
}

static int draw_menu(Display *display, Font *font, int page, int has_apps,
                     int menu_3d, int selected, int previous_selected,
                     SystemInfo *info, const DepthEffect *depth,
                     DepthCacheState *cache_state, DepthLayer layers[2])
{
    const char **items;
    int count;
    const int row_height = display->mode.vdisplay / 17;
    DepthFrame frame;
    int total_height;
    int start_y;
    int result = 0;
    int i;
    uint64_t content_key = menu_content_key(page, has_apps, menu_3d);
    if (depth_frame_begin(&frame, display, depth, layers, cache_state, content_key, 0) != 0)
        return -1;
    if (page == PAGE_FILE_MANAGER) {
        if (frame.full) {
            memset(layers[0].buffer.pixels, 0, layers[0].buffer.size);
            memset(layers[1].buffer.pixels, 0, layers[1].buffer.size);
            draw_file_manager(display, font, depth, layers);
            result = depth_frame_update_rect(&frame, 0, 0, display->mode.hdisplay,
                                             display->mode.vdisplay, row_height / 6,
                                             row_height / 6 + row_height - 8);
        }
        return depth_frame_present(&frame);
    }
    menu_items(page, has_apps, &items, &count);
    if (page == PAGE_INFO && frame.full)
        collect_system_info(display, info);
    total_height = row_height * count;
    start_y = ((int)display->mode.vdisplay - total_height) / 2;
    if (frame.full) {
        menu_view_draw_page(display, font, depth, layers, page, has_apps, selected, info);
        result = depth_frame_update_rect(&frame, 0, start_y, display->mode.hdisplay,
                                         total_height, start_y + selected * row_height,
                                         start_y + selected * row_height + row_height - 8);
    } else {
        int rows[2] = { previous_selected, selected };
        for (i = 0; i < 2; ++i) {
            int row = rows[i];
            int y = start_y + row * row_height;
            int bar_top = row == selected ? y : 0;
            int bar_bottom = row == selected ? y + row_height - 8 : 0;
            if (i == 1 && row == rows[0])
                continue;
            menu_view_draw_row(display, font, depth, layers, page, has_apps, row, selected, info);
            result = depth_frame_update_rect(&frame, 0, y, display->mode.hdisplay,
                                             row_height, bar_top, bar_bottom);
            if (result != 0)
                break;
        }
    }
    return depth_frame_present(&frame);
}

static int serial_open(const char *path)
{
    struct termios attributes;
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (tcgetattr(fd, &attributes) != 0) {
        close(fd);
        return -1;
    }
    cfmakeraw(&attributes);
    cfsetispeed(&attributes, B115200);
    cfsetospeed(&attributes, B115200);
    attributes.c_cflag |= CLOCAL | CREAD;
    attributes.c_cflag &= ~CRTSCTS;
    attributes.c_cc[VMIN] = 0;
    attributes.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &attributes) != 0 || tcflush(fd, TCIOFLUSH) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int save_menu_settings(int seconds, int button_reversed, int auto_play,
                              int menu_3d, void *context)
{
    (void)context;
    return save_settings(seconds, button_reversed, auto_play, menu_3d);
}

static MenuAction handle_message(char *message, int *page, int has_apps, int *selected,
                                 int *changed, int *slideshow_duration,
                                 int *button_reversed, int *auto_play, int *menu_3d)
{
    int previous_page = *page;
    int previous_selected = *selected;
    MenuState state = {
        .page = *page,
        .selected = *selected,
        .has_apps = has_apps,
        .slideshow_duration = *slideshow_duration,
        .button_reversed = *button_reversed,
        .auto_play = *auto_play,
        .menu_3d = *menu_3d,
        .volume = *page == PAGE_SETTINGS && *selected == 8 ? volume_get_percent() : 0
    };
    MenuAction action;
    if (state.volume < 0) {
        fprintf(stderr, "Unable to read PipeWire volume\n");
        state.volume = 0;
    }
    action = menu_handle_message(&state, message, changed, save_menu_settings, NULL);
    *page = state.page;
    *selected = state.selected;
    *slideshow_duration = state.slideshow_duration;
    *button_reversed = state.button_reversed;
    *auto_play = state.auto_play;
    *menu_3d = state.menu_3d;
    if (previous_page == PAGE_VOLUME && *page == PAGE_VOLUME &&
        *selected != previous_selected && *selected > 0) {
        int percent = (*selected - 1) * 10;
        if (volume_set_percent(percent) != 0)
            fprintf(stderr, "Unable to set PipeWire volume to %d%%\n", percent);
        else if (percent > 0)
            volume_preview_play();
    }
    return action;
}

static MenuAction consume_serial(int fd, char *message, size_t *message_length,
                                 int *page, int has_apps, int *selected, int *changed,
                                 int *slideshow_duration, int *button_reversed,
                                 int *auto_play, int *menu_3d)
{
    char input[512];
    ssize_t count;
    MenuAction action = ACTION_NONE;
    while ((count = read(fd, input, sizeof(input))) > 0) {
        ssize_t i;
        for (i = 0; i < count; ++i) {
            char c = input[i];
            if (c == '\a') {
                *message_length = 0;
                message[0] = '\0';
                continue;
            }
            if (c == '\r')
                continue;
            if (c == '\n') {
                if (*message_length) {
                    MenuAction next_action;
                    message[*message_length] = '\0';
                    next_action = handle_message(message, page, has_apps, selected, changed,
                                                 slideshow_duration, button_reversed,
                                                 auto_play, menu_3d);
                    if (next_action != ACTION_NONE)
                        action = next_action;
                }
                *message_length = 0;
                continue;
            }
            if (*message_length < 511) {
                message[(*message_length)++] = c;
                message[*message_length] = '\0';
            } else {
                *message_length = 0;
            }
        }
    }
    if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        return (MenuAction)-1;
    return action;
}

/*
 * Serial commands are PREV/NEXT (move or change media), PLPA (open/play/pause),
 * LOOP (long-press return), SHUT (power off), and USBD (USB desktop mode).
 * USB keyboards are normalized to the same commands. Button Direction affects
 * physical navigation, while keyboard arrows always retain their directions.
 *
 * Pages are rendered from two CPU layers through EGL/GBM and OpenGL ES directly
 * to DRM/KMS. DepthCacheState tracks GPU versions and page fingerprints; clean
 * pages reuse cached output and dirty rows/cells are composited incrementally.
 * Gallery baking is interruptible and runs only for changed content. Playback
 * suspends menu GPU resources so the player can own DRM and Cedrus buffers.
 *
 * Gallery scans /home/lkg/data/media. Mounted USB media appears as USB; an empty
 * mountpoint is hidden. Internal and USB caches are separate. Folder playback
 * returns focus to the last media path reported by the player's playlist.
 * Wi-Fi changes run through the privileged lkg-wifi helper, volume through
 * wpctl, and File Manager through a child dufs process on port 5000.
 * Settings persist in /var/lib/lkg/config/menu/lkg-menu.conf (with fallback to
 * /home/lkg/.lkg-menu.conf). System services and privileged helpers are
 * installed by lkg-armbian, not by this binary.
 */

static void usage(const char *program)
{
    printf("Usage: %s [--drm-device /dev/dri/card0] [--serial /dev/ttyS5] [--font PATH] [--apps-dir PATH]\n", program);
}

static int launch_sway(Display *display, pid_t *sway_pid)
{
    uint32_t menu_framebuffer = display->active_framebuffer;
    pid_t pid = fork();
    int ready = 0;
    int i;
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1) != 0 ||
            setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/1000/bus", 1) != 0)
            _exit(127);
        execl("/usr/bin/sway", "sway", (char *)NULL);
        _exit(127);
    }
    if (display->saved_crtc) {
        drmModeFreeCrtc(display->saved_crtc);
        display->saved_crtc = NULL;
    }
    if (drmDropMaster(display->fd) != 0) {
        display_close(display);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -1;
    }
    for (i = 0; i < 200; ++i) {
        drmModeCrtc *crtc = drmModeGetCrtc(display->fd, display->crtc_id);
        if (crtc) {
            if (crtc->buffer_id && crtc->buffer_id != menu_framebuffer)
                ready = 1;
            drmModeFreeCrtc(crtc);
        }
        if (ready)
            break;
        usleep(50000);
    }
    display_close(display);
    if (!ready) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -1;
    }
    *sway_pid = pid;
    return 0;
}

static int wait_for_menu_return(int serial_fd, pid_t sway_pid)
{
    SerialParser parser;
    memset(&parser, 0, sizeof(parser));
    while (running) {
        char command[16];
        int event;
        int status;
        if (sway_pid > 0) {
            pid_t result = waitpid(sway_pid, &status, WNOHANG);
            if (result == sway_pid || (result < 0 && errno == ECHILD)) {
                printf("Sway exited; returning to menu\n");
                fflush(stdout);
                return 0;
            }
        }
        event = serial_parser_next(serial_fd, &parser, command, sizeof(command), 100);
        if (event < 0)
            return -1;
        if (event > 0 && strcmp(command, "SHUT") == 0)
            return SERIAL_RESULT_POWER_OFF;
        if (event > 0 && strcmp(command, "USBD") == 0)
            return SERIAL_RESULT_USB_DESKTOP;
        if (event > 0 && strcmp(command, "LOOP") == 0) {
            int attempt;
            printf("Long press received; returning to menu\n");
            fflush(stdout);
            if (sway_pid <= 0)
                return 0;
            kill(sway_pid, SIGTERM);
            for (attempt = 0; attempt < 100; ++attempt) {
                pid_t result = waitpid(sway_pid, &status, WNOHANG);
                if (result == sway_pid || (result < 0 && errno == ECHILD))
                    return 0;
                usleep(50000);
            }
            kill(sway_pid, SIGKILL);
            while (waitpid(sway_pid, &status, 0) < 0 && errno == EINTR)
                ;
            return 0;
        }
    }
    return -1;
}

static MenuAction action_for_serial_result(int result)
{
    if (result == SERIAL_RESULT_POWER_OFF)
        return ACTION_POWER_OFF;
    if (result == SERIAL_RESULT_USB_DESKTOP)
        return ACTION_USB_DESKTOP;
    return ACTION_NONE;
}

static pid_t start_dufs(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/local/bin/dufs", "dufs", "--bind", "0.0.0.0",
              "--port", "5000", "--allow-upload", "--allow-delete",
              "--allow-search", "--allow-archive", "/home/lkg/data", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        printf("dufs started on port 5000\n");
        fflush(stdout);
    } else {
        fprintf(stderr, "Unable to start dufs: %s\n", strerror(errno));
    }
    return pid;
}

static void stop_dufs(pid_t *pid)
{
    int status;
    int attempt;
    if (*pid <= 0)
        return;
    kill(*pid, SIGTERM);
    for (attempt = 0; attempt < 20; ++attempt) {
        pid_t result = waitpid(*pid, &status, WNOHANG);
        if (result == *pid || (result < 0 && errno != EINTR)) {
            printf("dufs stopped\n");
            fflush(stdout);
            *pid = -1;
            return;
        }
        usleep(50000);
    }
    kill(*pid, SIGKILL);
    while (waitpid(*pid, &status, 0) < 0 && errno == EINTR)
        ;
    *pid = -1;
}

#define DEFAULT_FONT_PATH "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
#define FALLBACK_FONT_PATH "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

int main(int argc, char **argv)
{
    const char *drm_path = "/dev/dri/card0";
    const char *serial_path = "/dev/ttyS5";
    const char *font_path = NULL;
    const char *apps_dir = APPS_ROOT;
    Display display;
    Font font;
    DepthEffect depth;
    DepthLayer layers[2];
    UsbKeyboard keyboard;
    DepthCacheState cache_state = {0};
    DepthAnim menu_anim = {0};
    SystemInfo info;
    struct pollfd pfd[2];
    struct sigaction action;
    MenuAction requested_action = ACTION_NONE;
    char message[512];
    size_t message_length = 0;
    int serial_fd;
    pid_t dufs_pid = -1;
    int page = PAGE_MAIN;
    int has_apps = 0;
    int selected = 0;
    int changed = 1;
    int cached_page = -1;
    int cached_selected = -1;
    int slideshow_duration;
    int button_reversed;
    int auto_play;
    int pending_auto_play;
    int menu_3d;
    int i;
    if (argc == 2 && strcmp(argv[1], "--cache-cleanup") == 0)
        return gallery_cache_cleanup_run();
    if (argc == 11 && strcmp(argv[1], "--thumb-worker") == 0) {
        int columns = atoi(argv[4]);
        int rows = atoi(argv[5]);
        int view = atoi(argv[6]);
        double aspect = strtod(argv[7], NULL);
        int width = atoi(argv[8]);
        int height = atoi(argv[9]);
        int progress_fd = atoi(argv[10]);
        int result = thumbnail_image_generate(argv[2], argv[3], columns, rows, view,
                                              aspect, width, height, progress_fd);
        if (progress_fd >= 0)
            close(progress_fd);
        return result == 0 ? 0 : 1;
    }
    load_settings(&slideshow_duration, &button_reversed, &auto_play, &menu_3d);
    pending_auto_play = auto_play;
    memset(&display, 0, sizeof(display));
    usb_keyboard_init(&keyboard);
    display.fd = -1;
    for (i = 1; i < argc; ++i) {
        if (i + 1 < argc && strcmp(argv[i], "--drm-device") == 0) {
            drm_path = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--serial") == 0) {
            serial_path = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--font") == 0) {
            font_path = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--apps-dir") == 0) {
            apps_dir = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
initialize_menu:
    usb_keyboard_set_exclusive(&keyboard, 1);
    memset(&display, 0, sizeof(display));
    display.fd = -1;
    memset(&cache_state, 0, sizeof(cache_state));
    memset(&menu_anim, 0, sizeof(menu_anim));
    requested_action = ACTION_NONE;
    page = PAGE_MAIN;
    has_apps = apps_scan_has_apps(apps_dir);
    selected = 0;
    changed = 1;
    cached_page = -1;
    cached_selected = -1;
    message_length = 0;
    if (display_open(&display, drm_path) != 0) {
        if (strcmp(drm_path, "/dev/dri/card0") != 0 || display_open(&display, "/dev/dri/card1") != 0) {
            fprintf(stderr, "Unable to initialize DRM display\n");
            return 1;
        }
    }
    if (font_path) {
        if (font_open(&font, font_path, 64) != 0) {
            fprintf(stderr, "Unable to load font: %s\n", font_path);
            display_close(&display);
            return 1;
        }
    } else if (font_open(&font, DEFAULT_FONT_PATH, 64) != 0) {
        fprintf(stderr, "Unable to load %s; falling back to %s\n",
                DEFAULT_FONT_PATH, FALLBACK_FONT_PATH);
        if (font_open(&font, FALLBACK_FONT_PATH, 64) != 0) {
            fprintf(stderr, "Unable to load fallback font: %s\n", FALLBACK_FONT_PATH);
            display_close(&display);
            return 1;
        }
    }
    font.utf8 = 1;
    if (depth_effect_open(&depth, &display, "/run/lkg/visual.json") != 0) {
        fprintf(stderr, "Unable to initialize depth rendering\n");
        depth_effect_close(&depth);
        font_close(&font);
        display_close(&display);
        return 1;
    }
    depth_effect_apply_mode(&depth, NULL, menu_3d);
    memset(layers, 0, sizeof(layers));
    if (depth_layer_create(&display, &layers[0], depth.front_disparity) != 0 ||
        depth_layer_create(&display, &layers[1], depth.back_disparity) != 0) {
        fprintf(stderr, "Unable to allocate menu depth layers\n");
        depth_layer_destroy(&layers[0]);
        depth_layer_destroy(&layers[1]);
        depth_effect_close(&depth);
        font_close(&font);
        display_close(&display);
        return 1;
    }
    serial_fd = serial_open(serial_path);
    if (serial_fd < 0) {
        fprintf(stderr, "Unable to open serial port %s: %s\n", serial_path, strerror(errno));
        depth_layer_destroy(&layers[0]);
        depth_layer_destroy(&layers[1]);
        depth_effect_close(&depth);
        font_close(&font);
        display_close(&display);
        return 1;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, NULL);
    pfd[0].fd = serial_fd;
    pfd[0].events = POLLIN;
    pfd[1].events = POLLIN;
    if (pending_auto_play)
        requested_action = ACTION_GALLERY;
menu_loop:
    while (running && requested_action == ACTION_NONE) {
        volume_preview_poll();
        if (page == PAGE_FILE_MANAGER && dufs_pid < 0)
            dufs_pid = start_dufs();
        else if (page != PAGE_FILE_MANAGER && dufs_pid > 0)
            stop_dufs(&dufs_pid);
        if (changed) {
            int anim_result = menu_anim_begin(&display, &font, &depth, layers,
                                              &cache_state, &menu_anim, page,
                                              has_apps, menu_3d, cached_page,
                                              cached_selected, selected, &info);
            if (anim_result < 0) {
                fprintf(stderr, "Display update failed: %s\n", strerror(errno));
                break;
            }
            if (anim_result == 0) {
                menu_anim_cancel(&display, &font, &depth, layers, &cache_state,
                                 &menu_anim, page, has_apps, menu_3d,
                                 selected, &info);
                if (draw_menu(&display, &font, page, has_apps, menu_3d, selected,
                              cached_selected, &info, &depth, &cache_state, layers) != 0) {
                    fprintf(stderr, "Display update failed: %s\n", strerror(errno));
                    break;
                }
            }
            cached_page = page;
            cached_selected = selected;
            changed = 0;
        } else if (menu_anim.active) {
            if (menu_anim_step(&display, &font, &depth, layers, &cache_state,
                               &menu_anim, page, has_apps, selected, &info) != 0) {
                fprintf(stderr, "Display update failed: %s\n", strerror(errno));
                break;
            }
        }
        {
            char command[16];
            if (usb_keyboard_next_command(&keyboard, command, sizeof(command)) > 0) {
                char keyboard_message[32];
                usb_keyboard_prepare_command(command, button_reversed);
                snprintf(keyboard_message, sizeof(keyboard_message), "{\"cmd\":\"%s\"}", command);
                requested_action = handle_message(keyboard_message, &page, has_apps, &selected,
                                                  &changed, &slideshow_duration, &button_reversed,
                                                  &auto_play, &menu_3d);
                depth_effect_apply_mode(&depth, layers, menu_3d);
                continue;
            }
        }
        pfd[1].fd = keyboard.fd;
        if (poll(pfd, 2, menu_anim.active ? 0 : 100) < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "poll failed: %s\n", strerror(errno));
            break;
        }
        if (pfd[0].revents & POLLIN) {
            requested_action = consume_serial(serial_fd, message, &message_length,
                                              &page, has_apps, &selected, &changed,
                                              &slideshow_duration, &button_reversed,
                                              &auto_play, &menu_3d);
            depth_effect_apply_mode(&depth, layers, menu_3d);
            if ((int)requested_action < 0) {
                fprintf(stderr, "Serial read failed: %s\n", strerror(errno));
                requested_action = ACTION_NONE;
                break;
            }
        }
        if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "Serial port disconnected\n");
            break;
        }
    }
    stop_dufs(&dufs_pid);
    volume_preview_stop();
    menu_anim.active = 0;
    if (requested_action == ACTION_APPS) {
        int apps_result = apps_run(&display, &font, &depth, layers, serial_fd, &keyboard,
                                   button_reversed, apps_dir);
        requested_action = action_for_serial_result(apps_result);
        if (requested_action == ACTION_NONE && apps_result != 0)
            fprintf(stderr, "Apps view closed after an input error\n");
        has_apps = apps_scan_has_apps(apps_dir);
        if (!has_apps && page == PAGE_MAIN && selected >= 6)
            selected = 0;
        cache_state.initialized = 0;
        cached_page = -1;
        cached_selected = -1;
        changed = 1;
        tcflush(serial_fd, TCIFLUSH);
        if (running && requested_action == ACTION_NONE)
            goto menu_loop;
    }
    if (requested_action == ACTION_GALLERY) {
        int gallery_result = gallery_run(&display, &font, &depth, layers, serial_fd, &keyboard,
                                         slideshow_duration, button_reversed, pending_auto_play);
        pending_auto_play = 0;
        requested_action = action_for_serial_result(gallery_result);
        cache_state.initialized = 0;
        cached_page = -1;
        cached_selected = -1;
        changed = 1;
        tcflush(serial_fd, TCIFLUSH);
        if (running && requested_action == ACTION_NONE)
            goto menu_loop;
    }
    if (requested_action == ACTION_PRERENDER) {
        int prerender_result = gallery_prerender(&display, &font, &depth, layers,
                                                 serial_fd, &keyboard);
        requested_action = action_for_serial_result(prerender_result);
        if (requested_action == ACTION_NONE) {
            if (prerender_result != 0)
                fprintf(stderr, "Unable to start batch pre-rendering: %s\n", strerror(errno));
        }
        cache_state.initialized = 0;
        cached_page = -1;
        cached_selected = -1;
        changed = 1;
        tcflush(serial_fd, TCIFLUSH);
        if (running && requested_action == ACTION_NONE)
            goto menu_loop;
    }
    if (requested_action == ACTION_CLEAR_CACHE) {
        if (clear_cache_directory("/home/lkg/data/cache") != 0)
            fprintf(stderr, "Unable to completely clear pre-render cache: %s\n", strerror(errno));
        else
            printf("Pre-render cache cleared\n");
        requested_action = ACTION_NONE;
        menu_return_to_parent(&page, &selected, has_apps);
        cache_state.initialized = 0;
        cached_page = -1;
        cached_selected = -1;
        changed = 1;
        tcflush(serial_fd, TCIFLUSH);
        if (running)
            goto menu_loop;
    }
    if (requested_action == ACTION_WIFI) {
        int wifi_result = wifi_run(&display, &font, &depth, layers, serial_fd, &keyboard,
                                   button_reversed);
        requested_action = action_for_serial_result(wifi_result);
        if (requested_action == ACTION_NONE) {
            if (wifi_result != 0)
                fprintf(stderr, "Wi-Fi settings closed after an input error\n");
        }
        cache_state.initialized = 0;
        cached_page = -1;
        cached_selected = -1;
        changed = 1;
        tcflush(serial_fd, TCIFLUSH);
        if (running && requested_action == ACTION_NONE)
            goto menu_loop;
    }
    depth_layer_destroy(&layers[0]);
    depth_layer_destroy(&layers[1]);
    depth_effect_close(&depth);
    font_close(&font);
    if (requested_action == ACTION_SWAY || requested_action == ACTION_TTY) {
        pid_t sway_pid = -1;
        int background_result;
        usb_keyboard_set_exclusive(&keyboard, 0);
        usb_keyboard_close(&keyboard);
        if (requested_action == ACTION_SWAY && launch_sway(&display, &sway_pid) != 0) {
            fprintf(stderr, "Sway did not take over the display; returning to menu\n");
            close(serial_fd);
            pending_auto_play = 0;
            goto initialize_menu;
        }
        if (requested_action == ACTION_TTY)
            display_close(&display);
        background_result = wait_for_menu_return(serial_fd, sway_pid);
        pending_auto_play = 0;
        requested_action = action_for_serial_result(background_result);
        if (requested_action == ACTION_NONE) {
            close(serial_fd);
            if (background_result == 0 && running)
                goto initialize_menu;
            return running ? 1 : 0;
        }
    }
    close(serial_fd);
    usb_keyboard_close(&keyboard);
    display_close(&display);
    if (requested_action == ACTION_POWER_OFF) {
        execl("/usr/bin/systemctl", "systemctl", "poweroff", (char *)NULL);
        fprintf(stderr, "Unable to power off: %s\n", strerror(errno));
        return 1;
    }
    if (requested_action == ACTION_USB_DESKTOP) {
        execl("/usr/local/sbin/lkg-usb-disk", "lkg-usb-disk", "desktop", (char *)NULL);
        fprintf(stderr, "Unable to start USB disk mode: %s\n", strerror(errno));
        return 1;
    }
    return running ? 1 : 0;
}
