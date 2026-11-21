#define _GNU_SOURCE
#include "player.h"
#include "cache_key.h"
#include "heic.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t wait_for_child(pid_t pid, int *status)
{
    pid_t result;
    do {
        result = waitpid(pid, status, 0);
    } while (result < 0 && errno == EINTR);
    return result;
}

static int report_progress(Renderer *renderer, int progress_fd, double fraction,
                           uint64_t current, uint64_t total)
{
    if (renderer) {
        return total ? renderer_progress_frames(renderer, fraction, current, total) :
                       renderer_progress(renderer, fraction);
    }
    if (progress_fd >= 0) {
        char line[96];
        int length;
        if (fraction < 0)
            fraction = 0;
        if (fraction > 1)
            fraction = 1;
        if (total)
            length = snprintf(line, sizeof(line), "%.4f %llu/%llu\n", fraction,
                              (unsigned long long)current, (unsigned long long)total);
        else
            length = snprintf(line, sizeof(line), "%.4f\n", fraction);
        if (length > 0)
            (void)write(progress_fd, line, (size_t)length);
    }
    return 0;
}

static uint64_t compute_cache_hash(LkgCacheKind kind, const char *json_path,
                                   const char *calibration_path, const char *media_path,
                                   Renderer *renderer, int progress_fd,
                                   char *error, size_t error_size)
{
    uint64_t hash = 0;
    if (lkg_cache_media_hash(kind, media_path, json_path, calibration_path,
                             NULL, 0, &hash) != 0) {
        set_error(error, error_size, "Failed to compute cache key for %s: %s",
                  media_path, strerror(errno));
        return 0;
    }
    if ((renderer || progress_fd >= 0) &&
        report_progress(renderer, progress_fd, 0.05, 0, 0) != 0) {
        set_error(error, error_size, "Failed to display file hash progress");
        return 0;
    }
    return hash;
}

static uint64_t cache_hash(const char *json_path, const char *calibration_path,
                           const char *media_path, Renderer *renderer,
                           char *error, size_t error_size)
{
    return compute_cache_hash(LKG_CACHE_VIDEO, json_path, calibration_path,
                              media_path, renderer, -1, error, error_size);
}

static uint64_t image_cache_hash(const char *json_path, const char *calibration_path,
                                 const char *media_path, Renderer *renderer,
                                 char *error, size_t error_size)
{
    return compute_cache_hash(LKG_CACHE_IMAGE, json_path, calibration_path,
                              media_path, renderer, -1, error, error_size);
}

int find_cached_image(const char *json_path, const char *calibration_path,
                      const char *media_path, const char *cache_dir,
                      char **cached_path, char *error, size_t error_size)
{
    uint64_t hash = image_cache_hash(json_path, calibration_path, media_path,
                                     NULL, error, error_size);
    char filename[32];
    char *path;
    if (!hash)
        return -1;
    snprintf(filename, sizeof(filename), "%016llx.rgb", (unsigned long long)hash);
    path = malloc(strlen(cache_dir) + strlen(filename) + 2);
    if (!path) {
        set_error(error, error_size, "Out of memory");
        return -1;
    }
    sprintf(path, "%s/%s", cache_dir, filename);
    if (access(path, R_OK) != 0) {
        memcpy(path + strlen(path) - 4, ".png", 5);
        if (access(path, R_OK) != 0) {
            free(path);
            return 0;
        }
    }
    *cached_path = path;
    return 1;
}

int find_cached_video(const char *json_path, const char *calibration_path,
                      const char *media_path, const char *cache_dir,
                      char **cached_path, char *error, size_t error_size)
{
    uint64_t hash = cache_hash(json_path, calibration_path, media_path,
                               NULL, error, error_size);
    char filename[32];
    char *path;
    if (!hash)
        return -1;
    snprintf(filename, sizeof(filename), "%016llx.mp4", (unsigned long long)hash);
    path = malloc(strlen(cache_dir) + strlen(filename) + 2);
    if (!path) {
        set_error(error, error_size, "Out of memory");
        return -1;
    }
    sprintf(path, "%s/%s", cache_dir, filename);
    if (access(path, R_OK) != 0) {
        free(path);
        return 0;
    }
    {
        struct stat status;
        if (stat(path, &status) == 0 && status.st_size == 0) {
            decoder_discard_cached(path);
            free(path);
            return 0;
        }
    }
    *cached_path = path;
    return 1;
}

int find_cached_plain_video(const char *json_path, const char *calibration_path,
                            const char *media_path, const char *cache_dir,
                            char **cached_path, char *error, size_t error_size)
{
    uint64_t hash = cache_hash(json_path, calibration_path, media_path,
                               NULL, error, error_size);
    char filename[32];
    char *path;
    struct stat status;
    if (!hash)
        return -1;
    hash = lkg_cache_plain_video_hash(hash);
    snprintf(filename, sizeof(filename), "%016llx.mp4", (unsigned long long)hash);
    path = malloc(strlen(cache_dir) + strlen(filename) + 2);
    if (!path) {
        set_error(error, error_size, "Out of memory");
        return -1;
    }
    sprintf(path, "%s/%s", cache_dir, filename);
    if (access(path, R_OK) != 0) {
        free(path);
        return 0;
    }
    if (stat(path, &status) == 0 && status.st_size == 0) {
        decoder_discard_cached(path);
        free(path);
        return 0;
    }
    *cached_path = path;
    return 1;
}

static int write_all(int fd, const uint8_t *data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = write(fd, data + offset, size - offset);
        if (count > 0)
            offset += (size_t)count;
        else if (count < 0 && errno == EINTR)
            continue;
        else
            return -1;
    }
    return 0;
}

static int sync_path(const char *path)
{
    int fd = open(path, O_RDONLY);
    int result;
    if (fd < 0)
        return -1;
    do {
        result = fsync(fd);
    } while (result < 0 && errno == EINTR);
    if (close(fd) != 0 && result == 0)
        result = -1;
    return result;
}

static int cache_video_width(int width)
{
    return (width + 63) & ~63;
}

static pid_t start_encoder(const char *path, const char *source_path,
                           int has_audio, int audio_aac,
                           int width, int height, double fps, int threads, int *input_fd)
{
    int pipefd[2];
    pid_t pid;
    pid_t parent_pid = getpid();
    int padded_width = cache_video_width(width);
    char size[64];
    char filter[64];
    char rate[64];
    char thread_count[16];
    snprintf(size, sizeof(size), "%dx%d", width, height);
    if (padded_width != width)
        snprintf(filter, sizeof(filter), "format=yuv420p,pad=%d:%d:0:0", padded_width, height);
    else
        snprintf(filter, sizeof(filter), "format=yuv420p");
    snprintf(rate, sizeof(rate), "%.6f", fps);
    snprintf(thread_count, sizeof(thread_count), "%d", threads);
    if (pipe(pipefd) != 0)
        return -1;
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
#ifdef F_SETPIPE_SZ
    (void)fcntl(pipefd[1], F_SETPIPE_SZ, 1024 * 1024);
#endif
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        char *argv[64];
        int argc = 0;
        if (process_bind_parent(parent_pid) != 0)
            _exit(1);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        argv[argc++] = "ffmpeg";
        argv[argc++] = "-v";
        argv[argc++] = "error";
        argv[argc++] = "-y";
        argv[argc++] = "-thread_queue_size";
        argv[argc++] = "1";
        argv[argc++] = "-f";
        argv[argc++] = "rawvideo";
        argv[argc++] = "-pix_fmt";
        argv[argc++] = "bgra";
        argv[argc++] = "-video_size";
        argv[argc++] = size;
        argv[argc++] = "-framerate";
        argv[argc++] = rate;
        argv[argc++] = "-i";
        argv[argc++] = "pipe:0";
        if (has_audio) {
            argv[argc++] = "-vn";
            argv[argc++] = "-thread_queue_size";
            argv[argc++] = "1";
            argv[argc++] = "-i";
            argv[argc++] = (char *)source_path;
            argv[argc++] = "-map";
            argv[argc++] = "0:v:0";
            argv[argc++] = "-map";
            argv[argc++] = "1:a:0?";
        } else {
            argv[argc++] = "-map";
            argv[argc++] = "0:v:0";
            argv[argc++] = "-an";
        }
        argv[argc++] = "-filter_threads";
        argv[argc++] = thread_count;
        argv[argc++] = "-filter_complex_threads";
        argv[argc++] = thread_count;
        argv[argc++] = "-vf";
        argv[argc++] = filter;
        argv[argc++] = "-c:v";
        argv[argc++] = "libx264";
        argv[argc++] = "-preset";
        argv[argc++] = "ultrafast";
        argv[argc++] = "-crf";
        argv[argc++] = "18";
        argv[argc++] = "-threads";
        argv[argc++] = thread_count;
        argv[argc++] = "-pix_fmt";
        argv[argc++] = "yuv420p";
        if (has_audio) {
            if (audio_aac) {
                argv[argc++] = "-c:a";
                argv[argc++] = "copy";
            } else {
                argv[argc++] = "-c:a";
                argv[argc++] = "aac";
                argv[argc++] = "-b:a";
                argv[argc++] = "192k";
            }
        }
        argv[argc++] = "-f";
        argv[argc++] = "mp4";
        argv[argc++] = (char *)path;
        argv[argc] = NULL;
        execvp("ffmpeg", argv);
        _exit(127);
    }
    close(pipefd[0]);
    *input_fd = pipefd[1];
    return pid;
}

static double monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

enum {
    SLOT_EMPTY,
    SLOT_SOURCE_READY,
    SLOT_RGB_READY
};

static int source_file_matches(const char *path, dev_t device, ino_t inode)
{
    struct stat status;

    return stat(path, &status) == 0 && status.st_dev == device && status.st_ino == inode;
}

typedef struct {
    Decoder *decoder;
    int encoder_fd;
    volatile sig_atomic_t *stopped;
    dev_t source_device;
    ino_t source_inode;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint8_t *source[2];
    uint8_t *rgb[2];
    size_t rgb_size;
    int slots;
    int state[2];
    int decode_done;
    int decode_result;
    int decode_error;
    int render_done;
    int writer_done;
    int write_failed;
    int write_error;
    int failed;
    uint64_t written;
    double decode_seconds;
    double encode_write_seconds;
} PreprocessPipeline;

static void *preprocess_decode_thread(void *data)
{
    PreprocessPipeline *pipeline = data;
    int slot = 0;
    for (;;) {
        int result;
        double started;
        pthread_mutex_lock(&pipeline->mutex);
        while (pipeline->state[slot] != SLOT_EMPTY && !pipeline->failed &&
               !*pipeline->stopped)
            pthread_cond_wait(&pipeline->cond, &pipeline->mutex);
        if (pipeline->failed || *pipeline->stopped) {
            pthread_mutex_unlock(&pipeline->mutex);
            break;
        }
        pthread_mutex_unlock(&pipeline->mutex);
        started = monotonic_seconds();
        if (!source_file_matches(pipeline->decoder->path,
                                 pipeline->source_device, pipeline->source_inode)) {
            result = -1;
            errno = ENOENT;
        } else {
            result = decoder_read_frame(pipeline->decoder, pipeline->source[slot]);
        }
        if (result < 0)
            pipeline->decode_error = errno;
        pipeline->decode_seconds += monotonic_seconds() - started;
        pthread_mutex_lock(&pipeline->mutex);
        if (result > 0)
            pipeline->state[slot] = SLOT_SOURCE_READY;
        else
            pipeline->decode_done = 1;
        pipeline->decode_result = result;
        if (result < 0)
            pipeline->failed = 1;
        pthread_cond_broadcast(&pipeline->cond);
        pthread_mutex_unlock(&pipeline->mutex);
        if (result <= 0)
            break;
        slot = (slot + 1) % pipeline->slots;
    }
    return NULL;
}

static void *preprocess_write_thread(void *data)
{
    PreprocessPipeline *pipeline = data;
    int slot = 0;
    for (;;) {
        int result;
        double started;
        pthread_mutex_lock(&pipeline->mutex);
        while (pipeline->state[slot] != SLOT_RGB_READY && !pipeline->render_done &&
               !pipeline->failed && !*pipeline->stopped)
            pthread_cond_wait(&pipeline->cond, &pipeline->mutex);
        if (pipeline->state[slot] != SLOT_RGB_READY) {
            pthread_mutex_unlock(&pipeline->mutex);
            break;
        }
        pthread_mutex_unlock(&pipeline->mutex);
        started = monotonic_seconds();
        result = write_all(pipeline->encoder_fd, pipeline->rgb[slot], pipeline->rgb_size);
        if (result != 0)
            pipeline->write_error = errno;
        pipeline->encode_write_seconds += monotonic_seconds() - started;
        pthread_mutex_lock(&pipeline->mutex);
        if (result != 0) {
            pipeline->write_failed = 1;
            pipeline->failed = 1;
        } else {
            ++pipeline->written;
        }
        pipeline->state[slot] = SLOT_EMPTY;
        pthread_cond_broadcast(&pipeline->cond);
        pthread_mutex_unlock(&pipeline->mutex);
        if (result != 0)
            break;
        slot = (slot + 1) % pipeline->slots;
    }
    pthread_mutex_lock(&pipeline->mutex);
    pipeline->writer_done = 1;
    pthread_cond_broadcast(&pipeline->cond);
    pthread_mutex_unlock(&pipeline->mutex);
    return NULL;
}

static int preprocess_frames(Decoder *decoder, Renderer *renderer, int encoder_fd, int slots,
                             dev_t source_device, ino_t source_inode,
                             volatile sig_atomic_t *stopped, uint64_t *frames,
                             double *decode_seconds, double *render_seconds,
                             double *encode_write_seconds, double *progress_seconds,
                             char *error, size_t error_size)
{
    PreprocessPipeline pipeline = {
        .decoder = decoder,
        .encoder_fd = encoder_fd,
        .stopped = stopped,
        .source_device = source_device,
        .source_inode = source_inode,
        .rgb_size = (size_t)renderer_width(renderer) * renderer_height(renderer) * 4,
        .slots = slots,
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER
    };
    pthread_t decode_thread;
    pthread_t write_thread;
    size_t rgb_size = (size_t)renderer_width(renderer) * renderer_height(renderer) * 4;
    double last_progress = monotonic_seconds();
    time_t last_report = 0;
    int decode_started = 0;
    int write_started = 0;
    int render_error = 0;
    int slot = 0;
    int i;
    *frames = 0;
    for (i = 0; i < slots; ++i) {
        pipeline.source[i] = malloc(decoder->frame_size);
        pipeline.rgb[i] = malloc(rgb_size);
        if (!pipeline.source[i] || !pipeline.rgb[i]) {
            set_error(error, error_size, "Not enough memory for preprocessing pipeline buffers");
            render_error = 1;
        }
    }
    if (!render_error) {
        if (pthread_create(&decode_thread, NULL, preprocess_decode_thread, &pipeline) == 0)
            decode_started = 1;
        else
            render_error = 1;
        if (!render_error && pthread_create(&write_thread, NULL, preprocess_write_thread, &pipeline) == 0)
            write_started = 1;
        else if (!render_error)
            render_error = 1;
        if (render_error) {
            pthread_mutex_lock(&pipeline.mutex);
            pipeline.failed = 1;
            pipeline.render_done = 1;
            pthread_cond_broadcast(&pipeline.cond);
            pthread_mutex_unlock(&pipeline.mutex);
            set_error(error, error_size, "Failed to start preprocessing pipeline");
        }
    }
    while (!render_error) {
        double stage_started;
        pthread_mutex_lock(&pipeline.mutex);
        while (pipeline.state[slot] != SLOT_SOURCE_READY && !pipeline.decode_done &&
               !pipeline.failed && !*stopped)
            pthread_cond_wait(&pipeline.cond, &pipeline.mutex);
        if (pipeline.state[slot] != SLOT_SOURCE_READY) {
            pthread_mutex_unlock(&pipeline.mutex);
            break;
        }
        pthread_mutex_unlock(&pipeline.mutex);
        stage_started = monotonic_seconds();
        if (decoder->yuv420)
            renderer_set_yuv_colorimetry(renderer, decoder->yuv_bt709,
                                         decoder->yuv_full_range);
        if (renderer_render_to_bgra(renderer, pipeline.source[slot], pipeline.rgb[slot],
                                    error, error_size) != 0) {
            render_error = 1;
        }
        *render_seconds += monotonic_seconds() - stage_started;
        pthread_mutex_lock(&pipeline.mutex);
        if (!render_error)
            pipeline.state[slot] = SLOT_RGB_READY;
        else
            pipeline.failed = 1;
        pthread_cond_broadcast(&pipeline.cond);
        pthread_mutex_unlock(&pipeline.mutex);
        if (render_error)
            break;
        ++*frames;
        stage_started = monotonic_seconds();
        if ((decoder->total_frames && *frames == decoder->total_frames) ||
            stage_started - last_progress >= 0.1) {
            double fraction = decoder->total_frames ?
                              0.05 + 0.85 * *frames / decoder->total_frames :
                              0.05 + 0.85 * (*frames % 100) / 100.0;
            if (fraction > 0.90)
                fraction = 0.90;
            if (renderer_progress_frames(renderer, fraction, *frames,
                                         decoder->total_frames) != 0) {
                set_error(error, error_size, "Failed to display preprocessing progress");
                render_error = 1;
            }
            last_progress = stage_started;
        }
        *progress_seconds += monotonic_seconds() - stage_started;
        {
            time_t now = time(NULL);
            if (now != last_report) {
                printf("\rPreprocessing: %llu frames", (unsigned long long)*frames);
                fflush(stdout);
                last_report = now;
            }
        }
        slot = (slot + 1) % pipeline.slots;
    }
    pthread_mutex_lock(&pipeline.mutex);
    pipeline.render_done = 1;
    if (render_error)
        pipeline.failed = 1;
    pthread_cond_broadcast(&pipeline.cond);
    while (write_started && !pipeline.writer_done)
        pthread_cond_wait(&pipeline.cond, &pipeline.mutex);
    pthread_mutex_unlock(&pipeline.mutex);
    if (decode_started)
        pthread_join(decode_thread, NULL);
    if (write_started)
        pthread_join(write_thread, NULL);
    if (!render_error && pipeline.failed) {
        if (pipeline.write_failed)
            set_error(error, error_size, "Failed to write preprocessed video: %s",
                      strerror(pipeline.write_error));
        else
            set_error(error, error_size, "Failed to read source video: %s",
                      strerror(pipeline.decode_error));
        render_error = 1;
    }
    if (!render_error && *stopped) {
        set_error(error, error_size, "Preprocessing cancelled");
        render_error = 1;
    }
    if (!render_error)
        *frames = pipeline.written;
    *decode_seconds = pipeline.decode_seconds;
    *encode_write_seconds = pipeline.encode_write_seconds;
    for (i = 0; i < 2; ++i) {
        free(pipeline.source[i]);
        free(pipeline.rgb[i]);
    }
    pthread_cond_destroy(&pipeline.cond);
    pthread_mutex_destroy(&pipeline.mutex);
    return render_error ? -1 : 0;
}

int preprocess_video(Decoder *decoder, Renderer *renderer, const char *json_path,
                     const char *calibration_path, const char *cache_dir,
                     volatile sig_atomic_t *stopped, char **cached_path,
                     char *error, size_t error_size)
{
    uint64_t hash = cache_hash(json_path, calibration_path, decoder->path, renderer, error, error_size);
    char filename[32];
    char *path;
    char *temporary;
    pid_t encoder = -1;
    int encoder_fd = -1;
    int threads = decoder_preprocessing_threads(decoder);
    int slots = decoder->frame_size > 48 * 1024 * 1024 ? 1 : 2;
    struct stat source_status;
    int status;
    uint64_t frames = 0;

    *cached_path = NULL;
    if (!hash)
        return -1;
    snprintf(filename, sizeof(filename), "%016llx.mp4", (unsigned long long)hash);
    path = malloc(strlen(cache_dir) + strlen(filename) + 2);
    if (!path) {
        set_error(error, error_size, "Out of memory");
        return -1;
    }
    sprintf(path, "%s/%s", cache_dir, filename);
    if (access(path, R_OK) == 0) {
        printf("Using preprocessed cache: %s\n", path);
        *cached_path = path;
        return 1;
    }
    if (stat(decoder->path, &source_status) != 0) {
        set_error(error, error_size, "Source media is unavailable: %s", strerror(errno));
        free(path);
        return -1;
    }
    if (mkdir(cache_dir, 0755) != 0 && errno != EEXIST) {
        set_error(error, error_size, "Failed to create cache directory %s: %s", cache_dir, strerror(errno));
        free(path);
        return -1;
    }
    temporary = malloc(strlen(path) + 64);
    if (!temporary) {
        free(path);
        set_error(error, error_size, "Out of memory");
        return -1;
    }
    sprintf(temporary, "%s.tmp.%d", path, getpid());
    printf("First playback requires interleaved video preprocessing: %s\n", temporary);
    printf("Preprocess resources: %d threads, %d slots\n", threads, slots);
    encoder = start_encoder(temporary, decoder->path,
                            decoder->has_audio, decoder->audio_aac,
                            renderer_width(renderer), renderer_height(renderer),
                            decoder->fps, threads, &encoder_fd);
    if (encoder < 0) {
        set_error(error, error_size, "Failed to start FFmpeg encoder: %s", strerror(errno));
        goto fail;
    }
    if (renderer_progress(renderer, 0.05) != 0) {
        set_error(error, error_size, "Failed to display preprocessing progress");
        goto fail;
    }
    {
        double decode_seconds = 0;
        double render_seconds = 0;
        double encode_write_seconds = 0;
        double progress_seconds = 0;
        double encode_finish_seconds;
        double total_started = monotonic_seconds();
        double stage_started;
        if (preprocess_frames(decoder, renderer, encoder_fd, slots,
                              source_status.st_dev, source_status.st_ino, stopped, &frames,
                              &decode_seconds, &render_seconds, &encode_write_seconds,
                              &progress_seconds, error, error_size) != 0)
            goto fail;
        stage_started = monotonic_seconds();
        if (!frames && !*stopped) {
            set_error(error, error_size, "Source video produced no decoded frames");
            goto fail;
        }
        printf("\rPreprocessing: %llu frames, finalizing encoding\n", (unsigned long long)frames);
        close(encoder_fd);
        encoder_fd = -1;
        if (wait_for_child(encoder, &status) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            set_error(error, error_size, "FFmpeg failed to encode preprocessed video");
            encoder = 0;
            goto fail;
        }
        encode_finish_seconds = monotonic_seconds() - stage_started;
        encoder = 0;
        if (*stopped) {
            set_error(error, error_size, "Preprocessing cancelled");
            goto fail;
        }
        if (renderer_progress(renderer, 0.92) != 0) {
            set_error(error, error_size, "Failed to display preprocessing progress");
            goto fail;
        }
        if (!source_file_matches(decoder->path, source_status.st_dev, source_status.st_ino)) {
            set_error(error, error_size, "Source media disappeared during preprocessing");
            goto fail;
        }
        if (sync_path(temporary) != 0) {
            set_error(error, error_size, "Failed to sync preprocessed video: %s", strerror(errno));
            goto fail;
        }
        if (rename(temporary, path) != 0) {
            set_error(error, error_size, "Failed to save preprocessing cache: %s", strerror(errno));
            goto fail;
        }
        decoder_write_cached_metadata(path, cache_video_width(renderer_width(renderer)),
                                      renderer_height(renderer), decoder->fps, frames,
                                      decoder->has_audio);
        if (sync_path(cache_dir) != 0) {
            set_error(error, error_size, "Failed to sync preprocessed cache directory: %s",
                      strerror(errno));
            decoder_discard_cached(path);
            goto fail;
        }
        {
            double total_seconds = monotonic_seconds() - total_started;
            double upload_seconds;
            double draw_seconds;
            double readback_seconds;
            double convert_seconds;
            renderer_get_profile(renderer, &upload_seconds, &draw_seconds,
                                 &readback_seconds, &convert_seconds);
            printf("Preprocess timing: decode %.3fs, upload/render/readback %.3fs, encode writes %.3fs, "
                   "progress %.3fs, encoder finalization %.3fs; total %.3fs, %.2f fps\n",
                   decode_seconds, render_seconds, encode_write_seconds,
                   progress_seconds, encode_finish_seconds, total_seconds,
                   total_seconds > 0 ? frames / total_seconds : 0.0);
            printf("Render details: quilt upload %.3fs, GPU draw %.3fs, pixel readback %.3fs, "
                   "format conversion %.3fs\n", upload_seconds, draw_seconds,
                   readback_seconds, convert_seconds);
        }
    }
    decoder_stop(decoder);
    free(temporary);
    *cached_path = path;
    return 0;

fail:
    if (encoder_fd >= 0)
        close(encoder_fd);
    if (encoder > 0) {
        kill(encoder, SIGTERM);
        wait_for_child(encoder, NULL);
    }
    unlink(temporary);
    free(temporary);
    free(path);
    return -1;
}

static int update_plain_progress(Renderer *renderer, int progress_fd,
                                 FILE *progress, uint64_t total_frames,
                                 uint64_t *last_frame)
{
    char line[256];

    while (fgets(line, sizeof(line), progress)) {
        unsigned long long current;
        double fraction = 0.05;
        if (sscanf(line, "frame=%llu", &current) != 1)
            continue;
        if (current == *last_frame ||
            (current != 1 && current != total_frames && current < *last_frame + 10))
            continue;
        if (total_frames) {
            fraction += (0.92 - 0.05) * current / total_frames;
            if (fraction > 0.92)
                fraction = 0.92;
        }
        if (report_progress(renderer, progress_fd, fraction, current, total_frames) != 0)
            return -1;
        *last_frame = current;
    }
    clearerr(progress);
    return 0;
}

int preprocess_plain_video(Decoder *decoder, Renderer *renderer, int progress_fd,
                           const char *json_path, const char *calibration_path,
                           const char *cache_dir, volatile sig_atomic_t *stopped,
                           char **cached_path, char *error, size_t error_size)
{
    uint64_t hash = compute_cache_hash(LKG_CACHE_VIDEO, json_path, calibration_path,
                                       decoder->path, renderer, progress_fd,
                                       error, error_size);
    int display_width = decoder->rotated_90 ? decoder->height : decoder->width;
    int display_height = decoder->rotated_90 ? decoder->width : decoder->height;
    int max_width = display_height >= display_width ? 1472 : 2560;
    int max_height = 2560;
    int target_width = display_width;
    int target_height = display_height;
    int output_width;
    int output_height;
    char scale[64];
    char filename[32];
    char *path;
    char *temporary = NULL;
    pid_t encoder = -1;
    pid_t gst_decoder = -1;
    pid_t parent_pid = getpid();
    int progress_pipe[2] = {-1, -1};
    FILE *progress = NULL;
    uint64_t last_frame = 0;
    struct stat source_status;
    int status = 0;
    int result = -1;

    *cached_path = NULL;
    if (!hash)
        return -1;
    hash = lkg_cache_plain_video_hash(hash);
    if (target_width > max_width) {
        target_height = (int)((double)target_height * max_width / target_width + 0.5);
        target_width = max_width;
    }
    if (target_height > max_height) {
        target_width = (int)((double)target_width * max_height / target_height + 0.5);
        target_height = max_height;
    }
    output_width = (target_width + 32) / 64 * 64;
    if (output_width < 64)
        output_width = 64;
    output_height = (int)((double)display_height * output_width / display_width + 0.5);
    output_height = (output_height + 1) & ~1;
    if (output_height > max_height && output_width > 64) {
        output_width = (target_width / 64) * 64;
        if (output_width < 64)
            output_width = 64;
        output_height = (int)((double)display_height * output_width / display_width + 0.5);
        output_height = (output_height + 1) & ~1;
    }
    if (output_height < 2)
        output_height = 2;
    snprintf(scale, sizeof(scale), "scale=%d:%d", output_width, output_height);
    snprintf(filename, sizeof(filename), "%016llx.mp4", (unsigned long long)hash);
    path = malloc(strlen(cache_dir) + strlen(filename) + 2);
    if (!path) {
        set_error(error, error_size, "Out of memory");
        return -1;
    }
    sprintf(path, "%s/%s", cache_dir, filename);
    if (access(path, R_OK) == 0) {
        struct stat status;
        if (stat(path, &status) == 0 && status.st_size > 0) {
            printf("Using H.264 transcode cache: %s\n", path);
            *cached_path = path;
            return 1;
        }
        decoder_discard_cached(path);
    }
    if (stat(decoder->path, &source_status) != 0) {
        set_error(error, error_size, "Source media is unavailable: %s", strerror(errno));
        goto cleanup;
    }
    if (mkdir(cache_dir, 0755) != 0 && errno != EEXIST) {
        set_error(error, error_size, "Failed to create cache directory %s: %s",
                  cache_dir, strerror(errno));
        goto cleanup;
    }
    temporary = malloc(strlen(path) + 64);
    if (!temporary) {
        set_error(error, error_size, "Out of memory");
        goto cleanup;
    }
    sprintf(temporary, "%s.tmp.%d", path, getpid());
    printf("Transcoding H.265 to H.264 (%dx%d -> %dx%d): %s\n",
           display_width, display_height, output_width, output_height, temporary);
    if (report_progress(renderer, progress_fd, 0.05, 0, 0) != 0)
        goto cleanup;
    if (pipe(progress_pipe) != 0) {
        set_error(error, error_size, "Failed to create FFmpeg progress pipe: %s", strerror(errno));
        goto cleanup;
    }
    fcntl(progress_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(progress_pipe[1], F_SETFD, FD_CLOEXEC);
    {
        int threads = decoder_preprocessing_threads(decoder);
        char thread_count[16];
        int use_hw_hevc = decoder->hevc && !decoder->rotated_90 &&
                          decoder->width >= 64 && decoder->height >= 64 &&
                          decoder->width <= 4096 && decoder->height <= 4096 &&
                          (decoder->width % 2) == 0 && (decoder->height % 2) == 0 &&
                          decoder->fps > 0;
        int raw_pipe[2] = {-1, -1};
        snprintf(thread_count, sizeof(thread_count), "%d", threads);
        if (use_hw_hevc && pipe(raw_pipe) != 0)
            use_hw_hevc = 0;
        if (use_hw_hevc) {
            fcntl(raw_pipe[0], F_SETFD, FD_CLOEXEC);
            fcntl(raw_pipe[1], F_SETFD, FD_CLOEXEC);
#ifdef F_SETPIPE_SZ
            (void)fcntl(raw_pipe[1], F_SETPIPE_SZ, 1024 * 1024);
#endif
            gst_decoder = fork();
            if (gst_decoder < 0) {
                close(raw_pipe[0]);
                close(raw_pipe[1]);
                raw_pipe[0] = -1;
                raw_pipe[1] = -1;
                use_hw_hevc = 0;
            } else if (gst_decoder == 0) {
                char location_arg[PATH_MAX + 16];
                if (process_bind_parent(parent_pid) != 0)
                    _exit(1);
                if (dup2(raw_pipe[1], STDOUT_FILENO) < 0)
                    _exit(127);
                close(raw_pipe[0]);
                close(raw_pipe[1]);
                close(progress_pipe[0]);
                close(progress_pipe[1]);
                snprintf(location_arg, sizeof(location_arg), "location=%s", decoder->path);
                execlp("gst-launch-1.0", "gst-launch-1.0", "-q",
                       "filesrc", location_arg, "!", "qtdemux", "!",
                       "h265parse", "!", "v4l2slh265dec", "!",
                       "video/x-raw,format=NV12", "!", "fdsink", (char *)NULL);
                _exit(127);
            }
            if (use_hw_hevc) {
                close(raw_pipe[1]);
                raw_pipe[1] = -1;
            }
        }
        printf("Plain transcode resources: %d threads%s\n", threads,
               use_hw_hevc ? " (v4l2slh265dec)" : "");
        encoder = fork();
        if (encoder < 0) {
            if (raw_pipe[0] >= 0)
                close(raw_pipe[0]);
            if (gst_decoder > 0) {
                kill(gst_decoder, SIGTERM);
                wait_for_child(gst_decoder, NULL);
            }
            set_error(error, error_size, "Failed to start FFmpeg transcoder: %s", strerror(errno));
            goto cleanup;
        }
        if (encoder == 0) {
            char *argv[64];
            char size_arg[32];
            char rate_arg[32];
            int argc = 0;
            if (process_bind_parent(parent_pid) != 0)
                _exit(1);
            if (dup2(progress_pipe[1], STDOUT_FILENO) < 0)
                _exit(127);
            if (use_hw_hevc) {
                if (dup2(raw_pipe[0], STDIN_FILENO) < 0)
                    _exit(127);
                close(raw_pipe[0]);
            }
            close(progress_pipe[0]);
            close(progress_pipe[1]);
            argv[argc++] = "ffmpeg";
            argv[argc++] = "-nostdin";
            argv[argc++] = "-v";
            argv[argc++] = "error";
            argv[argc++] = "-y";
            argv[argc++] = "-progress";
            argv[argc++] = "pipe:1";
            argv[argc++] = "-nostats";
            if (use_hw_hevc) {
                snprintf(size_arg, sizeof(size_arg), "%dx%d", decoder->width, decoder->height);
                snprintf(rate_arg, sizeof(rate_arg), "%.6f", decoder->fps);
                argv[argc++] = "-f";
                argv[argc++] = "rawvideo";
                argv[argc++] = "-pix_fmt";
                argv[argc++] = "nv12";
                argv[argc++] = "-s";
                argv[argc++] = size_arg;
                argv[argc++] = "-r";
                argv[argc++] = rate_arg;
                argv[argc++] = "-i";
                argv[argc++] = "pipe:0";
                if (decoder->has_audio) {
                    argv[argc++] = "-vn";
                    argv[argc++] = "-i";
                    argv[argc++] = decoder->path;
                }
                argv[argc++] = "-map";
                argv[argc++] = "0:v:0";
                if (decoder->has_audio) {
                    argv[argc++] = "-map";
                    argv[argc++] = "1:a:0?";
                } else {
                    argv[argc++] = "-an";
                }
            } else {
                argv[argc++] = "-threads";
                argv[argc++] = thread_count;
                argv[argc++] = "-i";
                argv[argc++] = decoder->path;
                argv[argc++] = "-map";
                argv[argc++] = "0:v:0";
                if (decoder->has_audio) {
                    argv[argc++] = "-map";
                    argv[argc++] = "0:a:0?";
                } else {
                    argv[argc++] = "-an";
                }
                argv[argc++] = "-fps_mode";
                argv[argc++] = "passthrough";
            }
            argv[argc++] = "-sn";
            argv[argc++] = "-dn";
            argv[argc++] = "-filter_threads";
            argv[argc++] = thread_count;
            argv[argc++] = "-vf";
            argv[argc++] = scale;
            argv[argc++] = "-c:v";
            argv[argc++] = "libx264";
            argv[argc++] = "-preset";
            argv[argc++] = "ultrafast";
            argv[argc++] = "-crf";
            argv[argc++] = "18";
            argv[argc++] = "-threads";
            argv[argc++] = thread_count;
            argv[argc++] = "-pix_fmt";
            argv[argc++] = "yuv420p";
            if (decoder->has_audio) {
                if (decoder->audio_aac) {
                    argv[argc++] = "-c:a";
                    argv[argc++] = "copy";
                } else {
                    argv[argc++] = "-c:a";
                    argv[argc++] = "aac";
                    argv[argc++] = "-b:a";
                    argv[argc++] = "192k";
                }
            }
            argv[argc++] = "-f";
            argv[argc++] = "mp4";
            argv[argc++] = temporary;
            argv[argc] = NULL;
            execvp("ffmpeg", argv);
            _exit(127);
        }
        if (raw_pipe[0] >= 0)
            close(raw_pipe[0]);
    }
    close(progress_pipe[1]);
    progress_pipe[1] = -1;
    if (fcntl(progress_pipe[0], F_SETFL, O_NONBLOCK) != 0) {
        set_error(error, error_size, "Failed to configure FFmpeg progress pipe: %s", strerror(errno));
        goto cleanup;
    }
    progress = fdopen(progress_pipe[0], "r");
    if (!progress) {
        set_error(error, error_size, "Failed to read FFmpeg progress: %s", strerror(errno));
        goto cleanup;
    }
    for (;;) {
        pid_t wait_result;
        if (!source_file_matches(decoder->path, source_status.st_dev, source_status.st_ino)) {
            set_error(error, error_size, "Source media disappeared during transcoding");
            goto cleanup;
        }
        if (update_plain_progress(renderer, progress_fd, progress, decoder->total_frames, &last_frame) != 0) {
            set_error(error, error_size, "Failed to display transcode progress");
            goto cleanup;
        }
        wait_result = waitpid(encoder, &status, WNOHANG);
        if (wait_result == encoder)
            break;
        if (wait_result < 0 && errno != EINTR) {
            set_error(error, error_size, "Failed to wait for FFmpeg transcoder: %s", strerror(errno));
            goto cleanup;
        }
        if (*stopped) {
            if (gst_decoder > 0) {
                kill(gst_decoder, SIGTERM);
                wait_for_child(gst_decoder, NULL);
                gst_decoder = 0;
            }
            kill(encoder, SIGTERM);
            wait_for_child(encoder, &status);
            encoder = 0;
            set_error(error, error_size, "Preprocessing cancelled");
            goto cleanup;
        }
        {
            struct timespec delay = {0, 50000000};
            nanosleep(&delay, NULL);
        }
    }
    if (update_plain_progress(renderer, progress_fd, progress, decoder->total_frames, &last_frame) != 0) {
        set_error(error, error_size, "Failed to display final transcode progress");
        goto cleanup;
    }
    encoder = 0;
    if (gst_decoder > 0) {
        int gst_status = 0;
        wait_for_child(gst_decoder, &gst_status);
        gst_decoder = 0;
        if (!WIFEXITED(gst_status) || WEXITSTATUS(gst_status) != 0) {
            set_error(error, error_size, "GStreamer v4l2slh265dec failed to decode H.265 video");
            goto cleanup;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        set_error(error, error_size, "FFmpeg failed to transcode H.265 video");
        goto cleanup;
    }
    if (report_progress(renderer, progress_fd, 0.92, 0, 0) != 0)
        goto cleanup;
    if (!source_file_matches(decoder->path, source_status.st_dev, source_status.st_ino)) {
        set_error(error, error_size, "Source media disappeared during transcoding");
        goto cleanup;
    }
    if (sync_path(temporary) != 0) {
        set_error(error, error_size, "Failed to sync H.264 transcode: %s", strerror(errno));
        goto cleanup;
    }
    if (rename(temporary, path) != 0) {
        set_error(error, error_size, "Failed to save H.264 transcode cache: %s", strerror(errno));
        goto cleanup;
    }
    decoder_write_cached_metadata(path, output_width, output_height,
                                  decoder->fps, decoder->total_frames, decoder->has_audio);
    if (sync_path(cache_dir) != 0) {
        set_error(error, error_size, "Failed to sync H.264 cache directory: %s", strerror(errno));
        decoder_discard_cached(path);
        goto cleanup;
    }
    *cached_path = path;
    path = NULL;
    result = 0;

cleanup:
    if (progress)
        fclose(progress);
    else if (progress_pipe[0] >= 0)
        close(progress_pipe[0]);
    if (progress_pipe[1] >= 0)
        close(progress_pipe[1]);
    if (gst_decoder > 0) {
        kill(gst_decoder, SIGTERM);
        wait_for_child(gst_decoder, NULL);
    }
    if (encoder > 0) {
        kill(encoder, SIGTERM);
        wait_for_child(encoder, NULL);
    }
    if (temporary)
        unlink(temporary);
    free(temporary);
    free(path);
    return result;
}

static int is_png_file(const char *path)
{
    static const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    uint8_t header[sizeof(signature)];
    FILE *file = fopen(path, "rb");
    size_t count;
    if (!file)
        return 0;
    count = fread(header, 1, sizeof(header), file);
    fclose(file);
    return count == sizeof(header) && memcmp(header, signature, sizeof(header)) == 0;
}

static int is_jpeg_file(const char *path)
{
    uint8_t header[3];
    FILE *file = fopen(path, "rb");
    size_t count;
    if (!file)
        return 0;
    count = fread(header, 1, sizeof(header), file);
    fclose(file);
    return count == sizeof(header) && header[0] == 0xff &&
           header[1] == 0xd8 && header[2] == 0xff;
}

int image_stream_upload(const char *path, Renderer *renderer,
                        volatile sig_atomic_t *stopped, char *error, size_t error_size)
{
    if (is_png_file(path))
        return png_stream_upload(path, renderer, stopped, error, error_size) == 0 ? 1 : -1;
    if (is_jpeg_file(path))
        return jpeg_stream_upload(path, renderer, stopped, error, error_size) == 0 ? 1 : -1;
    return 0;
}

int preprocess_image(Decoder *decoder, Renderer *renderer, const char *json_path,
                     const char *calibration_path, const char *cache_dir,
                     volatile sig_atomic_t *stopped, char **cached_path,
                     char *error, size_t error_size)
{
    uint64_t hash = image_cache_hash(json_path, calibration_path, decoder->path, renderer, error, error_size);
    char filename[32];
    char *path = NULL;
    char *temporary = NULL;
    uint8_t *source = NULL;
    uint8_t *rgb = NULL;
    size_t rgb_size = (size_t)renderer_width(renderer) * renderer_height(renderer) * 3;
    int result = -1;

    *cached_path = NULL;
    if (!hash)
        return -1;
    snprintf(filename, sizeof(filename), "%016llx.rgb", (unsigned long long)hash);
    path = malloc(strlen(cache_dir) + strlen(filename) + 2);
    if (!path) {
        set_error(error, error_size, "Out of memory");
        goto cleanup;
    }
    sprintf(path, "%s/%s", cache_dir, filename);
    if (access(path, R_OK) != 0) {
        memcpy(path + strlen(path) - 4, ".png", 5);
        if (access(path, R_OK) == 0) {
            printf("Using preprocessed cache: %s\n", path);
            *cached_path = path;
            return 1;
        }
        memcpy(path + strlen(path) - 4, ".rgb", 5);
    } else {
        printf("Using preprocessed cache: %s\n", path);
        *cached_path = path;
        return 1;
    }
    if (mkdir(cache_dir, 0755) != 0 && errno != EEXIST) {
        set_error(error, error_size, "Failed to create cache directory %s: %s", cache_dir, strerror(errno));
        goto cleanup;
    }
    temporary = malloc(strlen(path) + 64);
    rgb = malloc(rgb_size);
    if (!temporary || !rgb) {
        set_error(error, error_size, "Not enough memory for image preprocessing buffers");
        goto cleanup;
    }
    sprintf(temporary, "%s.tmp.%d", path, getpid());
    printf("First open requires interleaved image preprocessing\n");
    if (renderer_progress(renderer, 0.05) != 0) {
        set_error(error, error_size, "Failed to display preprocessing progress");
        goto cleanup;
    }
    if (decoder->spatial) {
        if (heic_decode_spatial_stereo(decoder->path, decoder->width, decoder->height,
                                       &source, error, error_size) != 0)
            goto cleanup;
        if (renderer_render_to_rgb(renderer, source, rgb, error, error_size) != 0)
            goto cleanup;
    } else if (is_png_file(decoder->path) || is_jpeg_file(decoder->path)) {
        int uploaded = is_png_file(decoder->path) ?
            png_stream_upload(decoder->path, renderer, stopped, error, error_size) :
            jpeg_stream_upload(decoder->path, renderer, stopped, error, error_size);
        if (uploaded != 0)
            goto cleanup;
        if (renderer_read_rgb(renderer, rgb, error, error_size) != 0)
            goto cleanup;
    } else if (decoder->heic) {
        if (decoder_start(decoder, error, error_size) != 0)
            goto cleanup;
        if (renderer_render_to_rgb(renderer, decoder->still_frame, rgb, error, error_size) != 0) {
            decoder_stop(decoder);
            goto cleanup;
        }
        decoder_stop(decoder);
    } else {
        int decoded;
        source = malloc(decoder->frame_size);
        if (!source) {
            set_error(error, error_size, "Failed to allocate image decoding buffer");
            goto cleanup;
        }
        if (decoder_start(decoder, error, error_size) != 0)
            goto cleanup;
        decoded = decoder_read_frame(decoder, source);
        decoder_stop(decoder);
        if (decoded != 1) {
            set_error(error, error_size, "Image produced no decoded frames");
            goto cleanup;
        }
        if (renderer_render_to_rgb(renderer, source, rgb, error, error_size) != 0)
            goto cleanup;
    }
    if (renderer_progress(renderer, 0.85) != 0) {
        set_error(error, error_size, "Failed to display image readback progress");
        goto cleanup;
    }
    if (renderer_progress(renderer, 0.90) != 0) {
        set_error(error, error_size, "Failed to display cache write progress");
        goto cleanup;
    }
    {
        uint8_t header[16] = {'L', 'K', 'G', 'R', 'G', 'B', '0', '1', 0};
        int width = renderer_width(renderer);
        int height = renderer_height(renderer);
        int cache_fd;
        header[8] = (uint8_t)width;
        header[9] = (uint8_t)(width >> 8);
        header[10] = (uint8_t)(width >> 16);
        header[11] = (uint8_t)(width >> 24);
        header[12] = (uint8_t)height;
        header[13] = (uint8_t)(height >> 8);
        header[14] = (uint8_t)(height >> 16);
        header[15] = (uint8_t)(height >> 24);
        cache_fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (cache_fd < 0) {
            set_error(error, error_size, "Failed to write RGB image cache: %s", strerror(errno));
            goto cleanup;
        }
        if (write_all(cache_fd, header, sizeof(header)) != 0 ||
            write_all(cache_fd, rgb, rgb_size) != 0) {
            close(cache_fd);
            set_error(error, error_size, "Failed to write RGB image cache: %s", strerror(errno));
            goto cleanup;
        }
        if (close(cache_fd) != 0) {
            set_error(error, error_size, "Failed to finish RGB image cache: %s", strerror(errno));
            goto cleanup;
        }
    }
    if (renderer_progress(renderer, 0.92) != 0) {
        set_error(error, error_size, "Failed to display preprocessing progress");
        goto cleanup;
    }
    if (sync_path(temporary) != 0) {
        set_error(error, error_size, "Failed to sync image cache: %s", strerror(errno));
        goto cleanup;
    }
    if (rename(temporary, path) != 0) {
        set_error(error, error_size, "Failed to save image cache: %s", strerror(errno));
        goto cleanup;
    }
    if (sync_path(cache_dir) != 0) {
        set_error(error, error_size, "Failed to sync image cache directory: %s", strerror(errno));
        unlink(path);
        goto cleanup;
    }
    *cached_path = path;
    result = 0;

cleanup:
    if (temporary)
        unlink(temporary);
    free(source);
    free(rgb);
    free(temporary);
    if (!*cached_path)
        free(path);
    return result;
}
