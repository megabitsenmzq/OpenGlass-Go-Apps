#include "player.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static volatile sig_atomic_t pause_requested;
static volatile sig_atomic_t playback_ready;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stopped = 1;
}

static void handle_pause(int signal_number)
{
    (void)signal_number;
    pause_requested = !pause_requested;
}

/*
 * The input is the media filename with .json appended. Plain media uses a
 * nonexistent sidecar; quilt media reads its view layout from that sidecar.
 * Calibration comes from --calibration, LKG_CALIBRATION, /run/lkg/visual.json,
 * or the media's LKG_calibration directory. --once, --image-duration and
 * --no-restore control single-item lifetime and DRM restoration. --snapshot
 * writes the first interleaved frame as PPM.
 *
 * Playlist mode keeps DRM and renderer state in one process. control-fd accepts
 * newline-delimited PLPA, PREV, NEXT and LOOP commands. SIGUSR1 toggles pause.
 * --prerender processes all uncached playlist entries; --prerender-item handles
 * one entry. Workers are spawned from /proc/self/exe with only a progress pipe,
 * not the parent's DRM/EGL/GStreamer state. progress-fd reports item and frame
 * progress. --cache-dir overrides $HOME/data/cache; USB media uses its usb
 * subdirectory when the default is active. --drm-device selects the KMS device.
 *
 * Cache keys include paths, media size/mtime, sidecar and calibration contents,
 * and a preprocessing version. Quilt/spatial videos are interleaved into MP4;
 * plain HEVC is transcoded to aligned H.264/AAC, while plain H.264 plays directly.
 * Cached NV12 video uses Cedrus DMA-BUFs: DRM overlay for plain video and GPU
 * direct 4 for light-field video. LKG_PROFILE_DMA=1 enables frame timing logs.
 */

static void usage(const char *program)
{
    printf("Usage: %s [options] <media.json>\n"
           "\n"
           "Play LKG quilt media with a sidecar JSON file.\n"
           "Media without a sidecar JSON is played as a plain full image or video.\n"
           "\n"
           "Options:\n"
           "  --calibration FILE  Use the specified original visual.json\n"
           "  --drm-device PATH  DRM device, default /dev/dri/card0\n"
           "  --cache-dir PATH   Preprocessing cache directory\n"
           "  --once             Show one image frame or play a video once, then exit\n"
           "  --image-duration S Show an image for the specified number of seconds\n"
           "  --no-restore       Keep the current frame on exit\n"
           "  --playlist FILE    Play a playlist in a persistent display process\n"
           "  --start-index N    Initial 0-based index in playlist mode\n"
           "  --manual           Disable auto-advance in playlist mode\n"
           "  --control-fd FD    Control pipe for playlist mode\n"
           "  --prerender        Preprocess every uncached playlist item without playback\n"
           "  --prerender-item PATH Preprocess one media JSON and exit\n"
           "  --progress-fd FD   Write preprocessing progress to FD\n"
           "  --snapshot FILE    Save the first frame as PPM and exit\n"
           "  --help             Show this help\n"
           "\n"
           "Ctrl-C exits. SIGUSR1 toggles pause. Playlist mode uses offscreen workers.\n",
           program);
}

static char *directory_join(const char *json_path, const char *name)
{
    const char *slash = strrchr(json_path, '/');
    size_t directory_length = slash ? (size_t)(slash - json_path + 1) : 0;
    size_t name_length = strlen(name);
    char *path = malloc(directory_length + name_length + 1);
    if (!path)
        return NULL;
    memcpy(path, json_path, directory_length);
    memcpy(path + directory_length, name, name_length + 1);
    return path;
}

static char *find_calibration(const char *json_path)
{
    const char *fixed[] = {
        "/run/lkg/visual.json"
    };
    char *path;
    size_t i;

    if (getenv("LKG_CALIBRATION") && access(getenv("LKG_CALIBRATION"), R_OK) == 0)
        return strdup(getenv("LKG_CALIBRATION"));
    for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i) {
        if (access(fixed[i], R_OK) == 0)
            return strdup(fixed[i]);
    }
    path = directory_join(json_path, "LKG_calibration/visual.json");
    if (path && access(path, R_OK) == 0)
        return path;
    free(path);
    return NULL;
}

static const char *default_cache_dir(char *buffer, size_t size)
{
    const char *base = getenv("HOME");
    if (!base)
        return "/tmp/lkg-player-cache";
    snprintf(buffer, size, "%s/data/cache", base);
    return buffer;
}

static double monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static int sleep_until(double deadline)
{
    double remaining = deadline - monotonic_seconds();
    struct timespec delay;
    if (remaining <= 0)
        return 0;
    delay.tv_sec = (time_t)remaining;
    delay.tv_nsec = (long)((remaining - (double)delay.tv_sec) * 1000000000.0);
    while (nanosleep(&delay, &delay) != 0) {
        if (errno != EINTR)
            return -1;
        if (stopped)
            return 0;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *json_path = NULL;
    const char *calibration_path = NULL;
    const char *drm_device = "/dev/dri/card0";
    const char *snapshot_path = NULL;
    const char *playlist_path = NULL;
    size_t start_index = 0;
    int manual = 0;
    int control_fd = -1;
    int progress_fd = -1;
    pid_t parent_pid = 0;
    int prerender = 0;
    int prerender_item = 0;
    char cache_buffer[PATH_MAX];
    char selected_cache_buffer[PATH_MAX];
    const char *cache_dir = default_cache_dir(cache_buffer, sizeof(cache_buffer));
    const char *base_cache_dir = cache_dir;
    char *found_calibration = NULL;
    int once = 0;
    int frame_ready = 0;
    int paused = 0;
    int restore_crtc = 1;
    int i;
    int exit_code = 1;
    char error[512] = {0};
    MediaConfig media;
    Calibration calibration;
    Decoder decoder;
    Renderer *renderer = NULL;
    uint8_t *frame = NULL;
    uint64_t frame_number = 0;
    double start_time;
    double image_duration = 0;
    double image_deadline = 0;
    double pause_started = 0;

    memset(&media, 0, sizeof(media));
    memset(&decoder, 0, sizeof(decoder));
    decoder.fd = -1;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else if (strcmp(argv[i], "--no-restore") == 0) {
            restore_crtc = 0;
        } else if (strcmp(argv[i], "--calibration") == 0 && i + 1 < argc) {
            calibration_path = argv[++i];
        } else if (strcmp(argv[i], "--drm-device") == 0 && i + 1 < argc) {
            drm_device = argv[++i];
        } else if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) {
            cache_dir = argv[++i];
        } else if (strcmp(argv[i], "--image-duration") == 0 && i + 1 < argc) {
            char *end;
            image_duration = strtod(argv[++i], &end);
            if (*end != '\0' || image_duration <= 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--playlist") == 0 && i + 1 < argc) {
            playlist_path = argv[++i];
        } else if (strcmp(argv[i], "--start-index") == 0 && i + 1 < argc) {
            char *end;
            unsigned long long value;
            if (argv[i + 1][0] == '-') {
                usage(argv[0]);
                return 2;
            }
            value = strtoull(argv[++i], &end, 10);
            if (*end != '\0') {
                usage(argv[0]);
                return 2;
            }
            start_index = (size_t)value;
        } else if (strcmp(argv[i], "--manual") == 0) {
            manual = 1;
        } else if (strcmp(argv[i], "--control-fd") == 0 && i + 1 < argc) {
            control_fd = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--prerender") == 0) {
            prerender = 1;
        } else if (strcmp(argv[i], "--prerender-item") == 0 && i + 1 < argc) {
            prerender_item = 1;
            json_path = argv[++i];
        } else if (strcmp(argv[i], "--progress-fd") == 0 && i + 1 < argc) {
            progress_fd = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--parent-pid") == 0 && i + 1 < argc) {
            char *end;
            unsigned long long value = strtoull(argv[++i], &end, 10);
            if (*end != '\0' || value == 0 || value > INT_MAX) {
                usage(argv[0]);
                return 2;
            }
            parent_pid = (pid_t)value;
        } else if (strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc) {
            snapshot_path = argv[++i];
        } else if (argv[i][0] == '-' || json_path) {
            usage(argv[0]);
            return 2;
        } else {
            json_path = argv[i];
        }
    }
    if (parent_pid > 0 && process_bind_parent(parent_pid) != 0) {
        fprintf(stderr, "lkg-player: Parent process is unavailable\n");
        return 1;
    }
    if (!json_path && !playlist_path) {
        usage(argv[0]);
        return 2;
    }
    {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        sigemptyset(&action.sa_mask);
        action.sa_handler = handle_signal;
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGQUIT, &action, NULL);
        action.sa_handler = handle_pause;
        action.sa_flags = SA_RESTART;
        sigaction(SIGUSR1, &action, NULL);
        action.sa_flags = 0;
        action.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &action, NULL);
    }
    if (prerender_item) {
        int result;
        if (!json_path || prerender || playlist_path) {
            usage(argv[0]);
            return 2;
        }
        if (!calibration_path) {
            found_calibration = find_calibration(json_path);
            calibration_path = found_calibration;
        }
        if (!calibration_path) {
            fprintf(stderr, "lkg-player: visual.json not found\n");
            return 1;
        }
        result = prerender_item_run(json_path, calibration_path, cache_dir, progress_fd);
        free(found_calibration);
        return result;
    }
    if (playlist_path) {
        int result;
        if (!prerender && control_fd < 0) {
            fprintf(stderr, "lkg-player: --playlist requires --control-fd\n");
            return 2;
        }
        if (!calibration_path) {
            found_calibration = find_calibration(playlist_path);
            calibration_path = found_calibration;
        }
        if (!calibration_path) {
            fprintf(stderr, "lkg-player: visual.json not found\n");
            free(found_calibration);
            return 1;
        }
        if (prerender)
            result = prerender_run(playlist_path, calibration_path, cache_dir,
                                   progress_fd, &stopped);
        else
            result = slideshow_run(playlist_path, calibration_path, cache_dir, drm_device,
                                   control_fd, image_duration, restore_crtc, start_index,
                                   manual, &stopped);
        free(found_calibration);
        return result;
    }
    if (!calibration_path) {
        found_calibration = find_calibration(json_path);
        calibration_path = found_calibration;
    }
    if (!calibration_path) {
        snprintf(error, sizeof(error),
                 "visual.json not found; specify it with --calibration or LKG_CALIBRATION");
        goto fail;
    }
    base_cache_dir = cache_dir;
    cache_dir = media_cache_dir(json_path, cache_dir, selected_cache_buffer,
                                sizeof(selected_cache_buffer));
    if (config_load_media(json_path, &media, &decoder, calibration_path, cache_dir,
                          NULL, error, sizeof(error)) != 0)
        goto fail;
    if (config_load_calibration(calibration_path, &calibration, error, sizeof(error)) != 0)
        goto fail;
    QuiltConfig direct = {
        .columns = 1, .rows = 1, .views = 1,
        .aspect = 1440.0f / 2560.0f, .zoom = 1.0f
    };
    int cached_direct = 0;
    int prestarted = 0;
    int renderer_ready = 0;
    char *cached_path = NULL;
    if (media.plain && media.quilt.movie) {
        if (media.cached_path) {
            if (decoder_probe_cached(&decoder, media.cached_path, error, sizeof(error)) != 0)
                goto fail;
        }
        if (media.cached_path || !decoder.hevc) {
            media.quilt.aspect = (float)decoder.width / decoder.height;
            decoder_configure_nv12(&decoder);
            cached_direct = 5;
            if (gstreamer_start_paused(&decoder, error, sizeof(error)) != 0)
                goto fail;
            prestarted = 1;
            renderer = renderer_create(drm_device, decoder.width, decoder.height,
                                       &media.quilt, &calibration, 5, NULL,
                                       restore_crtc, 0, error, sizeof(error));
            if (!renderer)
                goto fail;
            renderer_ready = 1;
        } else {
            renderer = renderer_create(drm_device, 1, 1, &direct, &calibration,
                                       6, NULL, restore_crtc, 1, error, sizeof(error));
            if (!renderer)
                goto fail;
            if (preprocess_plain_video(&decoder, renderer, -1, json_path, calibration_path,
                                       cache_dir, &stopped, &cached_path,
                                       error, sizeof(error)) < 0)
                goto fail;
            free(decoder.path);
            decoder.path = NULL;
            if (decoder_probe_cached(&decoder, cached_path, error, sizeof(error)) != 0)
                goto fail;
            free(cached_path);
            cached_path = NULL;
            media.quilt.aspect = (float)decoder.width / decoder.height;
            decoder_configure_nv12(&decoder);
            cached_direct = 5;
        }
    } else if (media.plain) {
        if (decoder_probe_image(&decoder, media.media_path, error, sizeof(error)) != 0)
            goto fail;
        media.quilt.aspect = (float)decoder.width / decoder.height;
        cached_direct = 4;
        renderer = renderer_create(drm_device, decoder.width, decoder.height,
                                   &media.quilt, &calibration, 0, NULL,
                                   restore_crtc, 0, error, sizeof(error));
        if (!renderer)
            goto fail;
        renderer_ready = 1;
    } else if (media.quilt.movie) {
        if (media.cached_path) {
            cached_direct = 2;
            if (decoder_probe_cached(&decoder, media.cached_path, error, sizeof(error)) != 0)
                goto fail;
            decoder_configure_nv12(&decoder);
            if (gstreamer_start_paused(&decoder, error, sizeof(error)) != 0)
                goto fail;
            prestarted = 1;
            renderer = renderer_create(drm_device, decoder.width, decoder.height,
                                       &direct, &calibration, 4, NULL,
                                       restore_crtc, 0, error, sizeof(error));
            if (!renderer)
                goto fail;
            renderer_ready = 1;
        } else {
            renderer = renderer_create(drm_device, 1, 1, &direct, &calibration,
                                       6, NULL, restore_crtc, 1, error, sizeof(error));
            if (!renderer)
                goto fail;
        }
    } else {
        if (decoder_probe_image(&decoder, media.media_path, error, sizeof(error)) != 0)
            goto fail;
        {
            int found = media_find_cached_image(json_path, calibration_path, decoder.path,
                                                cache_dir, &cached_path, error, sizeof(error));
            if (found < 0)
                goto fail;
            if (found) {
                free(decoder.path);
                decoder.path = NULL;
                if (decoder_probe_image(&decoder, cached_path, error, sizeof(error)) != 0)
                    goto fail;
                cached_direct = 1;
                renderer = renderer_create(drm_device, decoder.width, decoder.height,
                                           &direct, &calibration, 6, NULL,
                                           restore_crtc, 0, error, sizeof(error));
                if (!renderer)
                    goto fail;
                renderer_ready = 1;
            } else {
                renderer = renderer_create(drm_device, 1, 1, &direct, &calibration,
                                           6, NULL, restore_crtc, 1, error, sizeof(error));
                if (!renderer)
                    goto fail;
            }
        }
    }
    renderer_track_pause(renderer, &pause_requested);
    free(cached_path);
    cached_path = NULL;
    if (decoder.width < media.quilt.columns || decoder.height < media.quilt.rows) {
        snprintf(error, sizeof(error), "Media dimensions are smaller than the quilt grid");
        goto fail;
    }
    printf("Media: %s (%dx%d, %.3f fps)\n", media.media_path,
           decoder.width, decoder.height, decoder.fps);
    printf("Quilt: %dx%d, %d views, aspect=%.6g, zoom=%.6g\n",
           media.quilt.columns, media.quilt.rows, media.quilt.views,
           media.quilt.aspect, media.quilt.zoom);
    printf("Calibration: %s\n", calibration_path);
    if (decoder.stereo)
        printf("Stereo disparity adjustment: %.6f\n", media.quilt.stereo_disparity);

    if (!renderer_ready) {
        int direct_mode;
        if (media.quilt.movie && !media.plain && !cached_direct &&
            decoder_can_yuv420(&decoder))
            decoder_configure_yuv420(&decoder);
        direct_mode = cached_direct == 2 ? 4 :
                      cached_direct == 1 ? 6 : cached_direct == 5 ? 5 :
                      decoder.yuv420 ? (decoder.stereo ? 9 : 8) :
                      decoder.stereo ? 7 : 0;
        if (renderer_reconfigure(renderer, decoder.width, decoder.height,
                                 cached_direct == 1 || cached_direct == 2 ? &direct : &media.quilt,
                                 direct_mode, error, sizeof(error)) != 0)
            goto fail;
        if (decoder.yuv420)
            renderer_set_yuv_colorimetry(renderer, decoder.yuv_bt709,
                                         decoder.yuv_full_range);
    }
    if (cached_direct == 1) {
        frame = malloc(decoder.frame_size);
        if (!frame) {
            snprintf(error, sizeof(error), "Failed to allocate playback frame buffer");
            goto fail;
        }
        if (cache_read_rgb(decoder.path, decoder.width, decoder.height, frame,
                         error, sizeof(error)) != 0)
            goto fail;
        frame_ready = 1;
    } else if (cached_direct == 2) {
        if ((prestarted ? gstreamer_pause(&decoder, 0) :
                          decoder_start(&decoder, error, sizeof(error))) != 0 ||
            decoder_read_frame(&decoder, NULL) != 1) {
            decoder_discard_cached(decoder.path);
            snprintf(error, sizeof(error), "Failed to read first frame of preprocessed cache");
            goto fail;
        }
        frame_ready = 1;
        printf("Playing preprocessed cache: %s (%dx%d)\n", decoder.path,
               decoder.width, decoder.height);
    } else if (cached_direct == 5) {
        if ((prestarted ? gstreamer_pause(&decoder, 0) :
                          decoder_start(&decoder, error, sizeof(error))) != 0) {
            if (!error[0])
                snprintf(error, sizeof(error), "Failed to start GStreamer playback");
            goto fail;
        }
        if (decoder_read_frame(&decoder, NULL) != 1) {
            snprintf(error, sizeof(error), "Failed to decode the first plain video frame");
            goto fail;
        }
        if (renderer_draw_dma(renderer, &decoder, error, sizeof(error)) != 0)
            goto fail;
        frame_ready = 1;
        printf("Plain video NV12 hardware direct display: %s (%dx%d)\n", decoder.path,
               decoder.width, decoder.height);
    } else if (cached_direct == 4) {
        int streamed = image_stream_upload(decoder.path, renderer, &stopped,
                                           error, sizeof(error));
        if (streamed < 0)
            goto fail;
        if (streamed > 0) {
            if (renderer_refresh(renderer, error, sizeof(error)) != 0)
                goto fail;
        } else if (decoder.heic) {
            if (decoder_start(&decoder, error, sizeof(error)) != 0 ||
                renderer_draw(renderer, decoder.still_frame, error, sizeof(error)) != 0) {
                if (!error[0])
                    snprintf(error, sizeof(error), "Failed to display plain image");
                goto fail;
            }
            decoder_stop(&decoder);
        } else {
            frame = malloc(decoder.frame_size);
            if (!frame) {
                snprintf(error, sizeof(error), "Failed to allocate plain image frame buffer");
                goto fail;
            }
            if (decoder_start(&decoder, error, sizeof(error)) != 0 ||
                decoder_read_frame(&decoder, frame) != 1 ||
                renderer_draw(renderer, frame, error, sizeof(error)) != 0) {
                snprintf(error, sizeof(error), "Failed to display plain image");
                goto fail;
            }
            decoder_stop(&decoder);
            free(frame);
            frame = NULL;
        }
        frame_number = 1;
        image_deadline = monotonic_seconds() + image_duration;
        playback_ready = 1;
        printf("Plain image direct display: %s (%dx%d)\n", decoder.path,
               decoder.width, decoder.height);
        if (snapshot_path) {
            if (renderer_snapshot(renderer, snapshot_path, error, sizeof(error)) != 0)
                goto fail;
            printf("Saved interleaved frame: %s\n", snapshot_path);
            exit_code = 0;
            goto cleanup;
        }
    } else {
        int result;
        if (renderer_thumbnail_prepare(renderer, media.media_path, base_cache_dir) != 0) {
            snprintf(error, sizeof(error), "Failed to prepare thumbnail cache");
            goto fail;
        }
        if (media.quilt.movie && decoder_start(&decoder, error, sizeof(error)) != 0)
            goto fail;
        if (media.quilt.movie)
            result = preprocess_video(&decoder, renderer, json_path, calibration_path,
                                      cache_dir, &stopped, &cached_path, error, sizeof(error));
        else
            result = preprocess_image(&decoder, renderer, json_path, calibration_path,
                                      cache_dir, &stopped, &cached_path, error, sizeof(error));
        if (result >= 0 && renderer_thumbnail_finish(renderer) != 0) {
            snprintf(error, sizeof(error), "Failed to write thumbnail cache");
            goto fail;
        }
        if (result < 0)
            goto fail;
        if (renderer_progress(renderer, 0.94) != 0) {
            snprintf(error, sizeof(error), "Failed to display cache open progress");
            goto fail;
        }
        decoder_stop(&decoder);
        free(decoder.path);
        decoder.path = NULL;
        if (media.quilt.movie) {
            if (decoder_probe_cached(&decoder, cached_path, error, sizeof(error)) != 0) {
                free(cached_path);
                goto fail;
            }
            decoder_configure_nv12(&decoder);
        } else if (decoder_probe_image(&decoder, cached_path, error, sizeof(error)) != 0) {
            free(cached_path);
            goto fail;
        }
        free(cached_path);
        if (media.quilt.movie) {
            if (decoder_start(&decoder, error, sizeof(error)) != 0)
                goto fail;
            result = decoder_read_frame(&decoder, NULL);
        } else {
            frame = malloc(decoder.frame_size);
            if (!frame) {
                snprintf(error, sizeof(error), "Failed to allocate playback frame buffer");
                goto fail;
            }
            result = cache_read_rgb(decoder.path, decoder.width, decoder.height, frame,
                                  error, sizeof(error)) == 0 ? 1 : 0;
        }
        if (result != 1) {
            snprintf(error, sizeof(error), "Failed to read first frame of preprocessed cache");
            goto fail;
        }
        frame_ready = 1;
        if (renderer_reconfigure(renderer, decoder.width, decoder.height,
                                 &direct, media.quilt.movie ? 4 : 6, error, sizeof(error)) != 0)
            goto fail;
        if (renderer_progress(renderer, 0.96) != 0) {
            snprintf(error, sizeof(error), "Failed to display cache initialization progress");
            goto fail;
        }
        if (renderer_progress(renderer, 0.98) != 0) {
            snprintf(error, sizeof(error), "Failed to display final frame preparation progress");
            goto fail;
        }
        printf("Playing preprocessed cache: %s (%dx%d)\n", decoder.path,
               decoder.width, decoder.height);
    }

    start_time = monotonic_seconds();
    while (!stopped) {
        int result;
        if (playback_ready && pause_requested != paused) {
            if (!paused) {
                paused = 1;
                pause_started = monotonic_seconds();
                if (media.quilt.movie && decoder.hardware && gstreamer_pause(&decoder, 1) != 0) {
                    snprintf(error, sizeof(error), "Failed to pause GStreamer decoding");
                    goto fail;
                }
                if (renderer_set_paused(renderer, 1, error, sizeof(error)) != 0)
                    goto fail;
            } else {
                double delay = monotonic_seconds() - pause_started;
                start_time += delay;
                image_deadline += delay;
                if (media.quilt.movie && decoder.hardware && gstreamer_pause(&decoder, 0) != 0) {
                    snprintf(error, sizeof(error), "Failed to resume GStreamer decoding");
                    goto fail;
                }
                if (renderer_set_paused(renderer, 0, error, sizeof(error)) != 0)
                    goto fail;
                paused = 0;
            }
        }
        if (paused) {
            sleep_until(monotonic_seconds() + 0.05);
            continue;
        }
        if (!media.quilt.movie && frame_number > 0) {
            double now = monotonic_seconds();
            if (image_duration > 0 && now >= image_deadline)
                break;
            if (image_duration > 0 && image_deadline < now + 0.1)
                now = image_deadline;
            else
                now += 0.1;
            sleep_until(now);
            continue;
        }
        result = frame_ready ? 1 : decoder_read_frame(&decoder, frame);
        frame_ready = 0;
        if (result < 0) {
            snprintf(error, sizeof(error), "Failed to read FFmpeg frame: %s", strerror(errno));
            goto fail;
        }
        if (result == 0) {
            if (media.quilt.movie && frame_number == 0) {
                decoder_stop(&decoder);
                snprintf(error, sizeof(error), "Video produced no decoded frames");
                goto fail;
            }
            if (once || !media.quilt.movie) {
                decoder_stop(&decoder);
                break;
            }
            if (decoder.hardware) {
                if (gstreamer_restart(&decoder) != 0) {
                    snprintf(error, sizeof(error), "Failed to loop GStreamer decoding");
                    goto fail;
                }
            } else {
                decoder_stop(&decoder);
                if (decoder_start(&decoder, error, sizeof(error)) != 0)
                    goto fail;
            }
            frame_number = 0;
            start_time = monotonic_seconds();
            continue;
        }
        if (decoder.hardware) {
            if (renderer_draw_dma(renderer, &decoder, error, sizeof(error)) != 0)
                goto fail;
        } else if (renderer_draw(renderer, frame, error, sizeof(error)) != 0)
            goto fail;
        if (!media.quilt.movie && frame) {
            free(frame);
            frame = NULL;
        }
        if (frame_number == 0)
            printf("First frame displayed\n");
        if (!media.quilt.movie && frame_number == 0) {
            start_time = monotonic_seconds();
            image_deadline = start_time + image_duration;
        }
        playback_ready = 1;
        if (snapshot_path) {
            if (renderer_snapshot(renderer, snapshot_path, error, sizeof(error)) != 0)
                goto fail;
            printf("Saved interleaved frame: %s\n", snapshot_path);
            break;
        }
        if (!media.quilt.movie && once && image_duration <= 0)
            break;
        ++frame_number;
        if (media.quilt.movie && !decoder.hardware)
            sleep_until(start_time + (double)frame_number / decoder.fps);
    }
    if (media.quilt.movie && !snapshot_path) {
        double elapsed = monotonic_seconds() - start_time;
        printf("Playback statistics: %llu frames, %.3f seconds, %.2f fps\n",
               (unsigned long long)frame_number, elapsed,
               elapsed > 0 ? frame_number / elapsed : 0.0);
    }
    exit_code = 0;
    goto cleanup;

fail:
    fprintf(stderr, "lkg-player: %s\n", error[0] ? error : "Unknown error");
    if (!renderer) {
        if (!calibration_path) {
            found_calibration = find_calibration(json_path);
            calibration_path = found_calibration;
        }
        if (calibration_path && config_load_calibration(calibration_path, &calibration, NULL, 0) == 0) {
            QuiltConfig direct = {
                .columns = 1, .rows = 1, .views = 1,
                .aspect = 1440.0f / 2560.0f, .zoom = 1.0f
            };
            renderer = renderer_create(drm_device, 1, 1, &direct, &calibration,
                                       6, NULL, restore_crtc, 0, NULL, 0);
        }
    }
    if (renderer && !stopped && !once && image_duration <= 0 &&
        renderer_show_error(renderer, NULL, 0) == 0) {
        while (!stopped) {
            if (pause_requested != paused) {
                paused = pause_requested;
                if (renderer_set_paused(renderer, paused, NULL, 0) != 0)
                    break;
            }
            sleep_until(monotonic_seconds() + 0.05);
        }
    }
cleanup:
    decoder_stop(&decoder);
    renderer_destroy(renderer);
    free(frame);
    free(decoder.path);
    config_free_media(&media);
    free(found_calibration);
    return exit_code;
}
