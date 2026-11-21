#define _GNU_SOURCE
#include "player.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

typedef struct {
    char **items;
    size_t count;
} Playlist;

typedef struct {
    pid_t pid;
    int fd;
    size_t index;
    char cache_dir[PATH_MAX];
    char input[256];
    size_t length;
} Worker;

typedef struct {
    Decoder decoder;
    uint8_t *frame;
    int movie;
    int failed;
    uint64_t frame_number;
    double started;
    double image_deadline;
} Current;

static volatile sig_atomic_t worker_stopped;

static double monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void playlist_free(Playlist *playlist)
{
    size_t i;
    for (i = 0; i < playlist->count; ++i)
        free(playlist->items[i]);
    free(playlist->items);
    memset(playlist, 0, sizeof(*playlist));
}

static int playlist_load(const char *path, Playlist *playlist, char *error, size_t error_size)
{
    FILE *file = fopen(path, "r");
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    if (!file) {
        set_error(error, error_size, "Failed to open playlist %s: %s", path, strerror(errno));
        return -1;
    }
    while ((length = getline(&line, &capacity, file)) >= 0) {
        char **items;
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';
        if (!length)
            continue;
        items = realloc(playlist->items, (playlist->count + 1) * sizeof(*items));
        if (!items) {
            free(line);
            fclose(file);
            set_error(error, error_size, "Failed to allocate playlist");
            return -1;
        }
        playlist->items = items;
        playlist->items[playlist->count] = strdup(line);
        if (!playlist->items[playlist->count]) {
            free(line);
            fclose(file);
            set_error(error, error_size, "Failed to allocate playlist entry");
            return -1;
        }
        ++playlist->count;
    }
    free(line);
    fclose(file);
    if (!playlist->count) {
        set_error(error, error_size, "Playlist is empty");
        return -1;
    }
    return 0;
}

static void save_current_index(const char *playlist_path, size_t index)
{
    char status_path[128];
    FILE *file;
    if (snprintf(status_path, sizeof(status_path), "%s.current", playlist_path) >=
        (int)sizeof(status_path))
        return;
    file = fopen(status_path, "w");
    if (!file)
        return;
    fprintf(file, "%lu\n", (unsigned long)index);
    fclose(file);
}

static void worker_signal(int signal_number)
{
    (void)signal_number;
    worker_stopped = 1;
}

static int preprocess_child(const char *json_path, const char *calibration_path,
                            const char *cache_dir, int progress_fd)
{
    MediaConfig media;
    Calibration calibration;
    Decoder decoder;
    Renderer *renderer = NULL;
    const char *base_cache_dir = cache_dir;
    char *cached_path = NULL;
    char selected_cache[PATH_MAX];
    char error[512] = {0};
    int result = 1;
    memset(&media, 0, sizeof(media));
    memset(&decoder, 0, sizeof(decoder));
    decoder.fd = -1;
    signal(SIGTERM, worker_signal);
    signal(SIGINT, worker_signal);
    cache_dir = media_cache_dir(json_path, cache_dir, selected_cache, sizeof(selected_cache));
    if (config_load_media(json_path, &media, &decoder, calibration_path, cache_dir,
                          NULL, error, sizeof(error)) != 0 ||
        config_load_calibration(calibration_path, &calibration, error, sizeof(error)) != 0)
        goto cleanup;
    if (media.plain && !media.quilt.movie) {
        result = 0;
        goto cleanup;
    }
    if (media.plain && !decoder.hevc) {
        result = 0;
        goto cleanup;
    }
    if (media.quilt.movie) {
        if (media.cached_path) {
            result = 0;
            goto cleanup;
        }
    } else {
        int found;
        if (decoder_probe_image(&decoder, media.media_path, error, sizeof(error)) != 0)
            goto cleanup;
        found = media_find_cached_image(json_path, calibration_path, media.media_path,
                                  cache_dir, &cached_path, error, sizeof(error));
        if (found < 0)
            goto cleanup;
        if (found) {
            result = 0;
            goto cleanup;
        }
    }
    free(cached_path);
    cached_path = NULL;
    if (media.quilt.movie && media.plain) {
        result = preprocess_plain_video(&decoder, NULL, progress_fd, json_path,
                                        calibration_path, cache_dir, &worker_stopped,
                                        &cached_path, error, sizeof(error));
    } else {
        int direct_mode;
        if (media.quilt.movie && !media.plain && decoder_can_yuv420(&decoder))
            decoder_configure_yuv420(&decoder);
        direct_mode = decoder.yuv420 ? (decoder.stereo ? 9 : 8) :
                      (decoder.stereo ? 7 : 0);
        renderer = renderer_create_offscreen("/dev/dri/card0", decoder.width, decoder.height,
                                             &media.quilt, &calibration, direct_mode,
                                             progress_fd, error, sizeof(error));
        if (!renderer)
            goto cleanup;
        if (decoder.yuv420)
            renderer_set_yuv_colorimetry(renderer, decoder.yuv_bt709,
                                         decoder.yuv_full_range);
        if (!media.plain && renderer_thumbnail_prepare(renderer, media.media_path, base_cache_dir) != 0) {
            snprintf(error, sizeof(error), "Failed to prepare thumbnail cache");
            goto cleanup;
        }
        if (media.quilt.movie && !media.plain &&
            decoder_start(&decoder, error, sizeof(error)) != 0)
            goto cleanup;
        if (media.quilt.movie)
            result = preprocess_video(&decoder, renderer, json_path, calibration_path,
                                      cache_dir, &worker_stopped, &cached_path,
                                      error, sizeof(error));
        else
            result = preprocess_image(&decoder, renderer, json_path, calibration_path,
                                      cache_dir, &worker_stopped, &cached_path,
                                      error, sizeof(error));
        if (result >= 0 && !media.plain && renderer_thumbnail_finish(renderer) != 0) {
            snprintf(error, sizeof(error), "Failed to write thumbnail cache");
            result = -1;
        }
    }
    if (result < 0 && !worker_stopped)
        fprintf(stderr, "lkg-worker: %s\n", error);
    result = result < 0 ? 1 : 0;

cleanup:
    decoder_stop(&decoder);
    renderer_destroy(renderer);
    free(cached_path);
    free(decoder.path);
    config_free_media(&media);
    return result;
}

int prerender_item_run(const char *json_path, const char *calibration_path,
                       const char *cache_dir, int progress_fd)
{
    worker_stopped = 0;
    return preprocess_child(json_path, calibration_path, cache_dir, progress_fd);
}

static void unlink_worker_temporary(const char *directory, const char *suffix)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    size_t suffix_length = strlen(suffix);
    if (!dir)
        return;
    while ((entry = readdir(dir)) != NULL) {
        size_t name_length = strlen(entry->d_name);
        char path[PATH_MAX];
        if (name_length <= suffix_length ||
            strcmp(entry->d_name + name_length - suffix_length, suffix) != 0)
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) < (int)sizeof(path))
            unlink(path);
    }
    closedir(dir);
}

static void worker_cleanup_temporary(const char *cache_dir, pid_t pid)
{
    char suffix[32];
    char thumbnails[PATH_MAX];
    snprintf(suffix, sizeof(suffix), ".tmp.%d", (int)pid);
    unlink_worker_temporary(cache_dir, suffix);
    if (snprintf(thumbnails, sizeof(thumbnails), "%s/thumbnails", cache_dir) <
        (int)sizeof(thumbnails))
        unlink_worker_temporary(thumbnails, suffix);
}

int prerender_run(const char *playlist_path, const char *calibration_path,
                  const char *cache_dir, int progress_fd,
                  volatile sig_atomic_t *stopped)
{
    Playlist playlist;
    size_t i;
    int failed = 0;
    memset(&playlist, 0, sizeof(playlist));
    if (playlist_load(playlist_path, &playlist, NULL, 0) != 0)
        return 1;
    for (i = 0; i < playlist.count && !*stopped; ++i) {
        pid_t pid;
        pid_t parent_pid = getpid();
        int status = 0;
        if (progress_fd >= 0)
            dprintf(progress_fd, "item=%llu\n", (unsigned long long)i);
        worker_stopped = 0;
        pid = fork();
        if (pid == 0) {
            int result;
            if (process_bind_parent(parent_pid) != 0)
                _exit(1);
            setpgid(0, 0);
            result = preprocess_child(playlist.items[i], calibration_path,
                                      cache_dir, progress_fd);
            _exit(result);
        }
        if (pid < 0) {
            failed = 1;
        } else {
            setpgid(pid, pid);
            for (;;) {
                pid_t result = waitpid(pid, &status, 0);
                if (result == pid)
                    break;
                if (result < 0 && errno == EINTR) {
                    if (*stopped)
                        kill(-pid, SIGTERM);
                    continue;
                }
                if (result < 0) {
                    status = 1;
                    break;
                }
            }
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                failed = 1;
            {
                char selected[PATH_MAX];
                const char *selected_cache = media_cache_dir(playlist.items[i], cache_dir,
                                                              selected, sizeof(selected));
                worker_cleanup_temporary(selected_cache, pid);
            }
        }
        if (progress_fd >= 0)
            dprintf(progress_fd, "done=%llu\n", (unsigned long long)(i + 1));
    }
    playlist_free(&playlist);
    return failed ? 1 : 0;
}

static void worker_cancel(Worker *worker)
{
    int status;
    int attempt;
    double started;
    if (worker->pid <= 0)
        return;
    started = monotonic_seconds();
    fprintf(stderr, "Cancelling preprocessing worker %d\n", (int)worker->pid);
    kill(-worker->pid, SIGTERM);
    for (attempt = 0; attempt < 10; ++attempt) {
        pid_t result = waitpid(worker->pid, &status, WNOHANG);
        if (result == worker->pid || (result < 0 && errno != EINTR))
            break;
        struct timespec delay = {0, 20000000};
        nanosleep(&delay, NULL);
    }
    if (waitpid(worker->pid, &status, WNOHANG) == 0) {
        kill(-worker->pid, SIGKILL);
        while (waitpid(worker->pid, &status, 0) < 0 && errno == EINTR)
            ;
    }
    if (worker->fd >= 0)
        close(worker->fd);
    worker_cleanup_temporary(worker->cache_dir, worker->pid);
    memset(worker, 0, sizeof(*worker));
    worker->fd = -1;
    fprintf(stderr, "Preprocessing worker cancelled in %.3fs\n", monotonic_seconds() - started);
}

static int worker_start(Worker *worker, size_t index, const char *json_path,
                        const char *calibration_path, const char *cache_dir,
                        char *error, size_t error_size)
{
    int descriptors[2];
    char progress_argument[16];
    char parent_argument[16];
    char selected[PATH_MAX];
    const char *selected_cache = media_cache_dir(json_path, cache_dir, selected, sizeof(selected));
    char *arguments[] = {
        "lkg-player", "--prerender-item", (char *)json_path,
        "--calibration", (char *)calibration_path,
        "--cache-dir", (char *)cache_dir,
        "--progress-fd", progress_argument,
        "--parent-pid", parent_argument, NULL
    };
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int attributes_ready = 0;
    int spawn_error;
    if (pipe(descriptors) != 0) {
        set_error(error, error_size, "Failed to create preprocessing progress pipe: %s", strerror(errno));
        return -1;
    }
    worker->index = index;
    snprintf(worker->cache_dir, sizeof(worker->cache_dir), "%s", selected_cache);
    snprintf(progress_argument, sizeof(progress_argument), "%d", STDERR_FILENO + 1);
    snprintf(parent_argument, sizeof(parent_argument), "%d", (int)getpid());
    spawn_error = posix_spawn_file_actions_init(&actions);
    if (spawn_error != 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        set_error(error, error_size, "Failed to initialize preprocessing worker: %s",
                  strerror(spawn_error));
        return -1;
    }
    spawn_error = posix_spawnattr_init(&attributes);
    if (spawn_error == 0) {
        attributes_ready = 1;
        spawn_error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    }
    if (spawn_error == 0)
        spawn_error = posix_spawn_file_actions_adddup2(&actions, descriptors[1],
                                                       STDERR_FILENO + 1);
    if (spawn_error == 0)
        spawn_error = posix_spawn_file_actions_addclosefrom_np(&actions,
                                                               STDERR_FILENO + 2);
    if (spawn_error == 0)
        spawn_error = posix_spawn(&worker->pid, "/proc/self/exe", &actions, &attributes,
                                  arguments, environ);
    if (attributes_ready)
        posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_error != 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        set_error(error, error_size, "Failed to start preprocessing worker: %s",
                  strerror(spawn_error));
        return -1;
    }
    close(descriptors[1]);
    worker->fd = descriptors[0];
    fcntl(worker->fd, F_SETFL, fcntl(worker->fd, F_GETFL, 0) | O_NONBLOCK);
    return 0;
}

static int worker_update(Worker *worker, Renderer *renderer)
{
    char input[256];
    ssize_t count;
    int status;
    int updated = 0;
    int has_frames = 0;
    double progress = 0;
    uint64_t frame_current = 0;
    uint64_t frame_total = 0;
    pid_t result;
    if (worker->pid <= 0)
        return 0;
    for (;;) {
        count = read(worker->fd, input, sizeof(input));
        if (count > 0) {
            ssize_t i;
            for (i = 0; i < count; ++i) {
                char c = input[i];
                if (c == '\n') {
                    unsigned long long current;
                    unsigned long long total;
                    worker->input[worker->length] = '\0';
                    if (sscanf(worker->input, "%lf %llu/%llu",
                               &progress, &current, &total) == 3) {
                        frame_current = current;
                        frame_total = total;
                        has_frames = 1;
                    } else {
                        progress = strtod(worker->input, NULL);
                        has_frames = 0;
                    }
                    updated = 1;
                    worker->length = 0;
                } else if (worker->length < sizeof(worker->input) - 1) {
                    worker->input[worker->length++] = c;
                }
            }
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    if (updated) {
        if (has_frames)
            renderer_progress_frames(renderer, progress, frame_current, frame_total);
        else
            renderer_progress(renderer, progress);
    }
    result = waitpid(worker->pid, &status, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR))
        return 0;
    close(worker->fd);
    worker->fd = -1;
    worker->pid = -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 1 : -1;
}

static void current_clear(Current *current)
{
    decoder_stop(&current->decoder);
    free(current->decoder.path);
    free(current->frame);
    memset(current, 0, sizeof(*current));
    current->decoder.fd = -1;
}

static int current_load_plain(Current *current, Renderer *renderer, const MediaConfig *media,
                              const char *json_path, const char *calibration_path,
                              const char *cache_dir, char *error, size_t error_size)
{
    volatile sig_atomic_t stopped = 0;
    if (media->quilt.movie) {
        QuiltConfig plain;
        if (media->cached_path) {
            if (decoder_probe_cached(&current->decoder, media->cached_path, error, error_size) != 0)
                return -1;
        } else if (current->decoder.hevc) {
            char *cached_path = NULL;
            if (preprocess_plain_video(&current->decoder, renderer, -1, json_path,
                                       calibration_path, cache_dir, &stopped, &cached_path,
                                       error, error_size) < 0)
                return -1;
            free(current->decoder.path);
            current->decoder.path = NULL;
            if (decoder_probe_cached(&current->decoder, cached_path, error, error_size) != 0) {
                free(cached_path);
                return -1;
            }
            free(cached_path);
        }
        plain = media->quilt;
        plain.aspect = (float)current->decoder.width / current->decoder.height;
        decoder_configure_nv12(&current->decoder);
        if (renderer_reconfigure(renderer, current->decoder.width, current->decoder.height,
                                 &plain, 5, error, error_size) != 0)
            return -1;
        if (decoder_start(&current->decoder, error, error_size) != 0 ||
            decoder_read_frame(&current->decoder, NULL) != 1 ||
            renderer_draw_dma(renderer, &current->decoder, error, error_size) != 0) {
            decoder_stop(&current->decoder);
            if (!error[0])
                set_error(error, error_size, "Failed to play plain video");
            return -1;
        }
    } else {
        int streamed;
        if (decoder_probe_image(&current->decoder, media->media_path, error, error_size) != 0)
            return -1;
        current->decoder.frame_size = (size_t)current->decoder.width *
                                      current->decoder.height * 3;
        current->decoder.read_size = current->decoder.frame_size;
        current->decoder.bgra = 0;
        QuiltConfig plain = media->quilt;
        plain.aspect = (float)current->decoder.width / current->decoder.height;
        plain.clamp_edges = 1;
        if (renderer_reconfigure(renderer, current->decoder.width, current->decoder.height,
                                 &plain, 0, error, error_size) != 0)
            return -1;
        streamed = image_stream_upload(current->decoder.path, renderer, &stopped,
                                       error, error_size);
        if (streamed < 0)
            return -1;
        if (streamed > 0) {
            if (renderer_refresh(renderer, error, error_size) != 0)
                return -1;
        } else if (current->decoder.heic) {
            if (decoder_start(&current->decoder, error, error_size) != 0 ||
                renderer_draw(renderer, current->decoder.still_frame, error, error_size) != 0) {
                if (!error[0])
                    set_error(error, error_size, "Failed to display plain image");
                return -1;
            }
            decoder_stop(&current->decoder);
        } else {
            current->frame = malloc(current->decoder.frame_size);
            if (!current->frame) {
                set_error(error, error_size, "Failed to allocate plain image frame buffer");
                return -1;
            }
            if (decoder_start(&current->decoder, error, error_size) != 0 ||
                decoder_read_frame(&current->decoder, current->frame) != 1 ||
                renderer_draw(renderer, current->frame, error, error_size) != 0) {
                set_error(error, error_size, "Failed to display plain image");
                return -1;
            }
            decoder_stop(&current->decoder);
            free(current->frame);
            current->frame = NULL;
        }
    }
    current->movie = media->quilt.movie;
    current->frame_number = 1;
    current->started = monotonic_seconds();
    printf("Playing plain media directly: %s (%dx%d)\n", current->decoder.path,
           current->decoder.width, current->decoder.height);
    return 0;
}

static int current_load_cached(Current *current, Renderer *renderer, const char *json_path,
                               const char *calibration_path, const char *cache_dir,
                               char *error, size_t error_size)
{
    MediaConfig media;
    char *cached_path = NULL;
    int found;
    int result = -1;
    memset(&media, 0, sizeof(media));
    if (config_load_media(json_path, &media, &current->decoder, calibration_path, cache_dir,
                          renderer, error, error_size) != 0)
        goto cleanup;
    current->movie = media.quilt.movie;
    if (media.plain && media.quilt.movie && current->decoder.hevc && !media.cached_path) {
        result = 1;
        goto cleanup;
    }
    if (media.plain) {
        renderer_release_dma(renderer);
        result = current_load_plain(current, renderer, &media, json_path, calibration_path,
                                    cache_dir, error, error_size);
        goto cleanup;
    }
    if (media.quilt.movie) {
        if (media.cached_path) {
            cached_path = media.cached_path;
            media.cached_path = NULL;
            found = 1;
        } else {
            found = 0;
        }
    } else
        found = media_find_cached_image(json_path, calibration_path, media.media_path,
                                  cache_dir, &cached_path, error, error_size);
    if (found < 0)
        goto cleanup;
    if (!found) {
        result = 1;
        goto cleanup;
    }
    renderer_release_dma(renderer);
    if (media.quilt.movie) {
        if (decoder_probe_cached(&current->decoder, cached_path, error, error_size) != 0) {
            decoder_discard_cached(cached_path);
            result = 1;
            goto cleanup;
        }
        decoder_configure_nv12(&current->decoder);
    } else {
        if (decoder_probe_image(&current->decoder, cached_path, error, error_size) != 0)
            goto cleanup;
        current->frame = malloc(current->decoder.frame_size);
        if (!current->frame) {
            set_error(error, error_size, "Failed to allocate image frame");
            goto cleanup;
        }
        if (cache_read_rgb(current->decoder.path, current->decoder.width,
                         current->decoder.height, current->frame,
                         error, error_size) != 0)
            goto cleanup;
    }
    {
        QuiltConfig direct = {
            .columns = 1, .rows = 1, .views = 1,
            .aspect = 1440.0f / 2560.0f, .zoom = 1.0f
        };
        if (renderer_reconfigure(renderer, current->decoder.width, current->decoder.height,
                                 &direct, media.quilt.movie ? 4 : 6,
                                 error, error_size) != 0)
            goto cleanup;
    }
    if (media.quilt.movie) {
        if (decoder_start(&current->decoder, error, error_size) != 0 ||
            decoder_read_frame(&current->decoder, NULL) != 1) {
            decoder_discard_cached(current->decoder.path);
            result = 1;
            goto cleanup;
        }
        if (renderer_draw_dma(renderer, &current->decoder, error, error_size) != 0)
            goto cleanup;
    } else if (renderer_draw(renderer, current->frame, error, error_size) != 0) {
        goto cleanup;
    } else {
        free(current->frame);
        current->frame = NULL;
    }
    current->movie = media.quilt.movie;
    current->frame_number = 1;
    current->started = monotonic_seconds();
    result = 0;

cleanup:
    free(cached_path);
    config_free_media(&media);
    return result;
}

static int current_switch(Current *current, Renderer *renderer, const char *json_path,
                          const char *calibration_path, const char *cache_dir,
                          char *error, size_t error_size)
{
    Current next;
    char selected_cache[PATH_MAX];
    int result;
    memset(&next, 0, sizeof(next));
    next.decoder.fd = -1;
    cache_dir = media_cache_dir(json_path, cache_dir, selected_cache, sizeof(selected_cache));
    result = current_load_cached(&next, renderer, json_path, calibration_path,
                                 cache_dir, error, error_size);
    if (result == 0) {
        current_clear(current);
        *current = next;
    } else {
        current->movie = next.movie;
        current_clear(&next);
    }
    return result;
}

static int current_show_error(Current *current, Renderer *renderer, double image_duration,
                              char *error, size_t error_size)
{
    decoder_stop(&current->decoder);
    current->movie = 0;
    current->failed = 1;
    current->frame_number = 0;
    current->started = monotonic_seconds();
    current->image_deadline = current->started + image_duration;
    return renderer_show_error(renderer, error, error_size);
}

static int apply_pause(Current *current, Renderer *renderer, int paused,
                       char *error, size_t error_size)
{
    if (current->movie && current->decoder.gst_pipeline &&
        gstreamer_pause(&current->decoder, paused) != 0) {
        set_error(error, error_size, "Failed to change video pause state");
        return -1;
    }
    return renderer_set_paused(renderer, paused, error, error_size);
}

int slideshow_run(const char *playlist_path, const char *calibration_path,
                  const char *cache_dir, const char *drm_device, int control_fd,
                  double image_duration, int restore_crtc, size_t start_index,
                  int manual, volatile sig_atomic_t *stopped)
{
    Playlist playlist;
    Calibration calibration;
    Current current;
    Worker worker;
    Renderer *renderer = NULL;
    QuiltConfig direct = {
        .columns = 1, .rows = 1, .views = 1,
        .aspect = 1440.0f / 2560.0f, .zoom = 1.0f
    };
    char error[512] = {0};
    char command_input[256];
    size_t command_length = 0;
    size_t index = 0;
    size_t reported_index = (size_t)-1;
    int paused = 0;
    double pause_started = 0;
    int exit_code = 1;
    int finished = 0;
    memset(&playlist, 0, sizeof(playlist));
    memset(&current, 0, sizeof(current));
    memset(&worker, 0, sizeof(worker));
    current.decoder.fd = -1;
    worker.fd = -1;
    if (playlist_load(playlist_path, &playlist, error, sizeof(error)) != 0 ||
        config_load_calibration(calibration_path, &calibration, error, sizeof(error)) != 0)
        goto cleanup;
    if (start_index < playlist.count)
        index = start_index;
    renderer = renderer_create(drm_device, 1, 1, &direct, &calibration,
                               6, NULL, restore_crtc, 0, error, sizeof(error));
    if (!renderer)
        goto cleanup;
    fcntl(control_fd, F_SETFL, fcntl(control_fd, F_GETFL, 0) | O_NONBLOCK);
    {
        int load_result = current_switch(&current, renderer, playlist.items[index],
            calibration_path, cache_dir, error, sizeof(error));
        if (load_result == 1) {
            renderer_begin_progress(renderer, current.movie);
            renderer_progress(renderer, 0);
            if (worker_start(&worker, index, playlist.items[index], calibration_path, cache_dir,
                             error, sizeof(error)) != 0)
                goto cleanup;
        } else if (load_result < 0) {
            if (current_show_error(&current, renderer, image_duration,
                                   error, sizeof(error)) != 0)
                goto cleanup;
        } else if (current.decoder.path == NULL) {
            goto cleanup;
        }
    }
    current.image_deadline = current.started + image_duration;
    if (paused && apply_pause(&current, renderer, paused, error, sizeof(error)) != 0)
        goto cleanup;

    while (!finished && !*stopped) {
        struct pollfd descriptors[2];
        if (reported_index != index) {
            save_current_index(playlist_path, index);
            reported_index = index;
        }
        int timeout = 50;
        int result;
        descriptors[0].fd = control_fd;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = worker.fd;
        descriptors[1].events = worker.fd >= 0 ? POLLIN : 0;
        descriptors[1].revents = 0;
        if (!paused && worker.pid <= 0 && current.movie && current.decoder.hardware) {
            timeout = 0;
        } else if (!paused && worker.pid <= 0 && (current.decoder.path || current.failed) &&
                   !(current.movie && current.decoder.hardware) &&
                   (current.movie || !manual)) {
            double deadline = current.movie ?
                current.started + (double)current.frame_number / current.decoder.fps :
                current.image_deadline;
            double remaining = (deadline - monotonic_seconds()) * 1000.0;
            if (remaining < 0)
                remaining = 0;
            if (remaining < timeout)
                timeout = (int)remaining;
        }
        result = poll(descriptors, 2, timeout);
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0) {
            set_error(error, sizeof(error), "Playback control poll failed: %s", strerror(errno));
            goto cleanup;
        }
        if (descriptors[0].revents & (POLLIN | POLLHUP)) {
            char input[256];
            ssize_t count = read(control_fd, input, sizeof(input));
            if (count == 0)
                break;
            if (count > 0) {
                ssize_t i;
                for (i = 0; i < count && !finished; ++i) {
                    char c = input[i];
                    if (c != '\n') {
                        if (command_length < 255)
                            command_input[command_length++] = c;
                        continue;
                    }
                    command_input[command_length] = '\0';
                    command_length = 0;
                    fprintf(stderr, "Control command: %s\n", command_input);
                    if (strcmp(command_input, "LOOP") == 0) {
                        finished = 1;
                    } else if (strcmp(command_input, "PLPA") == 0) {
                        if (manual && !current.movie) {
                            finished = 1;
                        } else if (!manual || (worker.pid <= 0 && !current.failed)) {
                            double now = monotonic_seconds();
                            if (!paused) {
                                paused = 1;
                                pause_started = now;
                            } else {
                                double delay = now - pause_started;
                                current.started += delay;
                                current.image_deadline += delay;
                                paused = 0;
                            }
                            if (apply_pause(&current, renderer, paused,
                                            error, sizeof(error)) != 0)
                                goto cleanup;
                        }
                    } else if (strcmp(command_input, "PREV") == 0 ||
                               strcmp(command_input, "NEXT") == 0) {
                        size_t target = strcmp(command_input, "PREV") == 0 ?
                            (index + 1) % playlist.count :
                            (index + playlist.count - 1) % playlist.count;
                        int load_result;
                        if (target == index)
                            continue;
                        if (manual)
                            paused = 0;
                        worker_cancel(&worker);
                        decoder_stop(&current.decoder);
                        load_result = current_switch(&current, renderer,
                            playlist.items[target], calibration_path, cache_dir,
                            error, sizeof(error));
                        if (load_result < 0) {
                            index = target;
                            if (current_show_error(&current, renderer, image_duration,
                                                   error, sizeof(error)) != 0 ||
                                apply_pause(&current, renderer, paused,
                                            error, sizeof(error)) != 0)
                                goto cleanup;
                        } else if (load_result == 0) {
                            index = target;
                            current.image_deadline = current.started + image_duration;
                            if (apply_pause(&current, renderer, paused,
                                            error, sizeof(error)) != 0)
                                goto cleanup;
                        } else {
                            index = target;
                            renderer_begin_progress(renderer, current.movie);
                            renderer_progress(renderer, 0);
                            if (worker_start(&worker, target, playlist.items[target], calibration_path,
                                             cache_dir, error, sizeof(error)) != 0)
                                goto cleanup;
                        }
                    }
                }
            }
        }
        if (finished)
            break;
        if (worker.pid > 0) {
            int worker_result = worker_update(&worker, renderer);
            if (worker_result != 0) {
                size_t target = worker.index;
                int load_result;
                if (worker_result < 0) {
                    index = target;
                    set_error(error, sizeof(error), "Preprocessing worker failed");
                    if (current_show_error(&current, renderer, image_duration,
                                           error, sizeof(error)) != 0 ||
                        apply_pause(&current, renderer, paused,
                                    error, sizeof(error)) != 0)
                        goto cleanup;
                    continue;
                }
                load_result = current_switch(&current, renderer,
                    playlist.items[target], calibration_path, cache_dir,
                    error, sizeof(error));
                index = target;
                if (load_result != 0) {
                    if (load_result > 0)
                        set_error(error, sizeof(error), "Preprocessed media is unavailable");
                    if (current_show_error(&current, renderer, image_duration,
                                           error, sizeof(error)) != 0)
                        goto cleanup;
                } else {
                    current.image_deadline = current.started + image_duration;
                }
                if (apply_pause(&current, renderer, paused, error, sizeof(error)) != 0)
                    goto cleanup;
            }
            continue;
        }
        if (paused || (!current.decoder.path && !current.failed))
            continue;
        if (current.movie) {
            int frame_result = decoder_read_frame(&current.decoder, current.frame);
            if (frame_result < 0) {
                set_error(error, sizeof(error), "Failed to read cached video");
                if (current_show_error(&current, renderer, image_duration,
                                       error, sizeof(error)) != 0 ||
                    apply_pause(&current, renderer, paused, error, sizeof(error)) != 0)
                    goto cleanup;
                continue;
            }
            if (frame_result == 0) {
                int load_result;
                size_t target = (index + 1) % playlist.count;
                if (manual || monotonic_seconds() < current.image_deadline) {
                    int restart_result;
                    restart_result = current.decoder.hardware ?
                        gstreamer_restart(&current.decoder) :
                        (decoder_stop(&current.decoder),
                         decoder_start(&current.decoder, error, sizeof(error)));
                    if (restart_result != 0) {
                        if (current_show_error(&current, renderer, image_duration,
                                               error, sizeof(error)) != 0 ||
                            apply_pause(&current, renderer, paused,
                                        error, sizeof(error)) != 0)
                            goto cleanup;
                        continue;
                    }
                    current.frame_number = 0;
                    current.started = monotonic_seconds();
                    continue;
                }
                decoder_stop(&current.decoder);
                load_result = current_switch(&current, renderer,
                    playlist.items[target], calibration_path, cache_dir,
                    error, sizeof(error));
                if (load_result < 0) {
                    index = target;
                    if (current_show_error(&current, renderer, image_duration,
                                           error, sizeof(error)) != 0 ||
                        apply_pause(&current, renderer, paused,
                                    error, sizeof(error)) != 0)
                        goto cleanup;
                } else if (load_result == 0) {
                    index = target;
                    current.image_deadline = current.started + image_duration;
                    if (apply_pause(&current, renderer, paused,
                                    error, sizeof(error)) != 0)
                        goto cleanup;
                } else {
                    index = target;
                    renderer_begin_progress(renderer, current.movie);
                    renderer_progress(renderer, 0);
                    if (worker_start(&worker, target, playlist.items[target], calibration_path,
                                     cache_dir, error, sizeof(error)) != 0)
                        goto cleanup;
                }
            } else {
                int draw_result = current.decoder.hardware ?
                    renderer_draw_dma(renderer, &current.decoder, error, sizeof(error)) :
                    renderer_draw(renderer, current.frame, error, sizeof(error));
                if (draw_result != 0) {
                    if (current_show_error(&current, renderer, image_duration,
                                           error, sizeof(error)) != 0 ||
                        apply_pause(&current, renderer, paused,
                                    error, sizeof(error)) != 0)
                        goto cleanup;
                    continue;
                }
                ++current.frame_number;
            }
        } else if (!manual && monotonic_seconds() >= current.image_deadline) {
            size_t target = (index + 1) % playlist.count;
            int load_result = current_switch(&current, renderer,
                playlist.items[target], calibration_path, cache_dir,
                error, sizeof(error));
            if (load_result < 0) {
                index = target;
                if (current_show_error(&current, renderer, image_duration,
                                       error, sizeof(error)) != 0 ||
                    apply_pause(&current, renderer, paused,
                                error, sizeof(error)) != 0)
                    goto cleanup;
            } else if (load_result == 0) {
                index = target;
                current.image_deadline = current.started + image_duration;
                if (apply_pause(&current, renderer, paused,
                                error, sizeof(error)) != 0)
                    goto cleanup;
            } else {
                index = target;
                renderer_begin_progress(renderer, current.movie);
                renderer_progress(renderer, 0);
                if (worker_start(&worker, target, playlist.items[target], calibration_path,
                                 cache_dir, error, sizeof(error)) != 0)
                    goto cleanup;
            }
        }
    }
    exit_code = 0;

cleanup:
    if (error[0])
        fprintf(stderr, "lkg-slideshow: %s\n", error);
    worker_cancel(&worker);
    current_clear(&current);
    renderer_destroy(renderer);
    playlist_free(&playlist);
    return exit_code;
}
