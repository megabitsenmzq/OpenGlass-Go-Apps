#include "gallery_internal.h"
#include "prerender_view.h"
#include "gallery_view.h"
#include "thumbnail.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <xf86drm.h>

static int prerender_thumbnail_begin(ThumbWorker *worker, GalleryItem *item)
{
    return gallery_thumbnail_begin(worker, item);
}

static pid_t start_prerender(PlaybackList *list, size_t index,
                             char *playlist_path, int *progress_fd)
{
    int descriptors[2];
    int file_descriptor;
    FILE *file;
    pid_t pid;
    char parent[16];
    strcpy(playlist_path, "/home/lkg/data/cache/.prerender-XXXXXX");
    file_descriptor = mkstemp(playlist_path);
    if (file_descriptor < 0)
        return -1;
    file = fdopen(file_descriptor, "w");
    if (!file) {
        close(file_descriptor);
        unlink(playlist_path);
        return -1;
    }
    fprintf(file, "%s.json\n", list->items[index].path);
    if (fclose(file) != 0 || pipe(descriptors) != 0) {
        unlink(playlist_path);
        return -1;
    }
    snprintf(parent, sizeof(parent), "%d", (int)getpid());
    pid = fork();
    if (pid == 0) {
        char progress[16];
        close(descriptors[0]);
        setpgid(0, 0);
        snprintf(progress, sizeof(progress), "%d", descriptors[1]);
        execl(PLAYER_PATH, PLAYER_PATH, "--cache-dir", PLAYER_CACHE,
              "--calibration", CALIBRATION_PATH, "--prerender",
              "--playlist", playlist_path, "--progress-fd", progress,
              "--parent-pid", parent, (char *)NULL);
        _exit(127);
    }
    close(descriptors[1]);
    if (pid < 0) {
        close(descriptors[0]);
        unlink(playlist_path);
        return -1;
    }
    setpgid(pid, pid);
    fcntl(descriptors[0], F_SETFD, FD_CLOEXEC);
    fcntl(descriptors[0], F_SETFL, fcntl(descriptors[0], F_GETFL, 0) | O_NONBLOCK);
    *progress_fd = descriptors[0];
    return pid;
}

typedef struct {
    DepthLayer *layers;
    DepthCacheState cache_state;
    size_t done;
    size_t total;
    double item_progress;
    uint64_t frame_current;
    uint64_t frame_total;
    int show_frames;
    int finished;
    int thumbnail_phase;
    char current_path[PATH_MAX];
} PrerenderRenderer;

static int prerender_draw(Display *display, Font *font, const DepthEffect *depth,
                          PrerenderRenderer *renderer, size_t done, size_t total,
                          double item_progress, uint64_t frame_current, uint64_t frame_total,
                          int show_frames, int finished, int thumbnail_phase,
                          const char *current_path)
{
    const int row_height = display->mode.vdisplay / 17;
    const int top_margin = row_height / 6;
    const int overall_y = 1010;
    const int current_y = 1630;
    const int width = display->mode.hdisplay;
    const int height = display->mode.vdisplay;
    DepthFrame frame;
    PrerenderViewState previous;
    PrerenderViewState next;
    PrerenderViewDirty dirty;
    int full;
    int result;
    if (depth_frame_begin(&frame, display, depth, renderer->layers,
                          &renderer->cache_state, 0, 0) != 0)
        return -1;
    full = frame.full;
    previous = (PrerenderViewState){
        .done = renderer->done, .total = renderer->total,
        .item_progress = renderer->item_progress,
        .frame_current = renderer->frame_current, .frame_total = renderer->frame_total,
        .show_frames = renderer->show_frames, .finished = renderer->finished,
        .thumbnail_phase = renderer->thumbnail_phase, .current_path = renderer->current_path
    };
    next = (PrerenderViewState){
        .done = done, .total = total, .item_progress = item_progress,
        .frame_current = frame_current, .frame_total = frame_total,
        .show_frames = show_frames, .finished = finished,
        .thumbnail_phase = thumbnail_phase, .current_path = current_path
    };
    dirty = prerender_view_dirty(&previous, &next, full);
    if (!dirty.overall && !dirty.current && !dirty.path) {
        depth_frame_close(&frame);
        return 0;
    }
    prerender_view_draw(display, font, renderer->layers, &next, dirty);
    if (full) {
        result = depth_frame_update_rect(&frame, 0, 0, width, height,
                                         top_margin, top_margin + row_height - 8);
    } else {
        result = 0;
        if (dirty.overall)
            result = depth_frame_update_rect(&frame, 0, overall_y - 100, width, 220, 0, 0);
        if (result == 0 && dirty.current)
            result = depth_frame_update_rect(&frame, 0, current_y - 100, width, 220, 0, 0);
        if (result == 0 && dirty.path)
            result = depth_frame_update_rect(&frame, 0, height - 120, width, 120, 0, 0);
    }
    result = depth_frame_present(&frame);
    if (result == 0) {
        renderer->done = done;
        renderer->total = total;
        renderer->item_progress = item_progress;
        renderer->frame_current = frame_current;
        renderer->frame_total = frame_total;
        renderer->show_frames = show_frames;
        renderer->finished = finished;
        renderer->thumbnail_phase = thumbnail_phase;
        snprintf(renderer->current_path, sizeof(renderer->current_path), "%s",
                 current_path ? current_path : "");
    }
    return result;
}

int gallery_prerender(Display *display, Font *font, const DepthEffect *depth,
                      DepthLayer layers[2], int serial_fd, UsbKeyboard *keyboard)
{
    PlaybackList list;
    SerialParser parser;
    ThumbWorker thumb;
    GalleryItem thumb_item;
    PrerenderRenderer renderer;
    char playlist_path[64] = {0};
    char input_line[256];
    size_t input_length = 0;
    size_t done = 0;
    size_t current = 0;
    size_t total_tasks;
    double item_progress = 0;
    double last_draw = 0;
    uint64_t frame_current = 0;
    uint64_t frame_total = 0;
    pid_t pid = -1;
    int progress_fd = -1;
    int thumbnail_phase = 0;
    int finished = 0;
    int changed = 1;
    int result = 0;
    memset(&list, 0, sizeof(list));
    memset(&parser, 0, sizeof(parser));
    memset(&thumb, 0, sizeof(thumb));
    memset(&thumb_item, 0, sizeof(thumb_item));
    memset(&renderer, 0, sizeof(renderer));
    renderer.layers = layers;
    thumb.pid = -1;
    thumb.fd = -1;
    if (gallery_playback_collect(&list, MEDIA_ROOT, 1) != 0) {
        gallery_playback_free(&list);
        return -1;
    }
    total_tasks = list.count;
    if (list.count)
        pid = start_prerender(&list, current, playlist_path, &progress_fd);
    else
        finished = 1;
    if (list.count && pid < 0) {
        gallery_playback_free(&list);
        return -1;
    }
    while (running) {
        char command[16];
        char input[512];
        ssize_t count;
        int event;
        int status;
        if (!thumbnail_phase && !finished && pid < 0) {
            if (current >= list.count) {
                done = list.count;
                item_progress = 1;
                finished = 1;
            } else {
                item_progress = 0;
                pid = start_prerender(&list, current, playlist_path, &progress_fd);
                if (pid < 0) {
                    ++current;
                    done = current;
                }
            }
            changed = 1;
        }
        if (thumbnail_phase && !finished && thumb.pid < 0 && current < list.count) {
            memset(&thumb_item, 0, sizeof(thumb_item));
            thumb_item.path = list.items[current].path;
            if (prerender_thumbnail_begin(&thumb, &thumb_item) > 0) {
                item_progress = thumb.progress;
            } else {
                free(thumb_item.thumbnail);
                thumb_item.thumbnail = NULL;
                ++current;
                done = current;
                thumbnail_phase = 0;
                item_progress = 0;
            }
            changed = 1;
        }
        if (changed) {
            double now = gallery_monotonic_seconds();
            if (!renderer.cache_state.initialized || finished || now - last_draw >= 0.1) {
                int show_frames = !thumbnail_phase && !finished && current < list.count &&
                                  video_extension(list.items[current].path);
                if (prerender_draw(display, font, depth, &renderer, done, total_tasks,
                                   item_progress, frame_current, frame_total, show_frames,
                                   finished, thumbnail_phase,
                                   current < list.count ? list.items[current].path : NULL) != 0) {
                    result = -1;
                    break;
                }
                last_draw = now;
                changed = 0;
            }
        }
        if (!finished && pid < 0 && thumb.pid < 0)
            continue;
        event = usb_keyboard_next_command(keyboard, command, sizeof(command));
        if (event == 0)
            event = serial_parser_next_with_fd(serial_fd, &parser, command, sizeof(command),
                                               pid > 0 || thumb.pid > 0 ? 50 : 100,
                                               keyboard->fd);
        if (event < 0)
            break;
        if (event > 0 && strcmp(command, "SHUT") == 0) {
            result = SERIAL_RESULT_POWER_OFF;
            break;
        }
        if (event > 0 && strcmp(command, "USBD") == 0) {
            result = SERIAL_RESULT_USB_DESKTOP;
            break;
        }
        if (event > 0 && (strcmp(command, "PLPA") == 0 || strcmp(command, "LOOP") == 0))
            break;
        while (!thumbnail_phase && progress_fd >= 0 &&
               (count = read(progress_fd, input, sizeof(input))) > 0) {
            ssize_t i;
            for (i = 0; i < count; ++i) {
                char c = input[i];
                if (c != '\n') {
                    if (input_length < sizeof(input_line) - 1)
                        input_line[input_length++] = c;
                    continue;
                }
                input_line[input_length] = '\0';
                input_length = 0;
                if (strncmp(input_line, "item=", 5) == 0) {
                    item_progress = 0;
                    frame_current = 0;
                    frame_total = 0;
                } else if (strncmp(input_line, "done=", 5) == 0) {
                    item_progress = 1;
                } else {
                    unsigned long long current_frame;
                    unsigned long long total_frames;
                    char *end;
                    double progress = strtod(input_line, &end);
                    if (end != input_line) {
                        item_progress = progress;
                        if (sscanf(input_line, "%*f %llu/%llu",
                                   &current_frame, &total_frames) == 2) {
                            frame_current = current_frame;
                            frame_total = total_frames;
                        }
                    }
                }
                changed = 1;
            }
        }
        if (thumbnail_phase && thumb.pid > 0) {
            count = read(thumb.fd, input, sizeof(input) - 1);
            if (count > 0) {
                input[count] = '\0';
                gallery_thumbnail_progress(input, thumb.duration, &thumb.progress);
                item_progress = thumb.duration > 0.0 ? thumb.progress : 0.5;
                changed = 1;
            } else if (count == 0) {
                close(thumb.fd);
                thumb.fd = -1;
            }
            if (waitpid(thumb.pid, &status, WNOHANG) == thumb.pid) {
                if (thumb.fd >= 0)
                    close(thumb.fd);
                thumb.fd = -1;
                thumb.pid = -1;
                thumb.item = NULL;
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
                    rename(thumb.temporary, thumb.cache_path) != 0 ||
                    !gallery_load_thumbnail(&thumb_item))
                    unlink(thumb.temporary);
                free(thumb_item.thumbnail);
                thumb_item.thumbnail = NULL;
                ++current;
                done = current;
                thumbnail_phase = 0;
                item_progress = 0;
                changed = 1;
            }
        } else if (pid > 0 && waitpid(pid, &status, WNOHANG) == pid) {
            pid = -1;
            if (progress_fd >= 0) {
                close(progress_fd);
                progress_fd = -1;
            }
            if (playlist_path[0]) {
                unlink(playlist_path);
                playlist_path[0] = '\0';
            }
            thumbnail_phase = 1;
            item_progress = 0;
            frame_current = 0;
            frame_total = 0;
            changed = 1;
        }
    }
    if (pid > 0) {
        kill(-pid, SIGTERM);
        gallery_stop_player(pid, SIGTERM);
    }
    gallery_worker_stop(&thumb);
    free(thumb_item.thumbnail);
    if (progress_fd >= 0)
        close(progress_fd);
    if (playlist_path[0])
        unlink(playlist_path);
    gallery_playback_free(&list);
    tcflush(serial_fd, TCIFLUSH);
    return result;
}
