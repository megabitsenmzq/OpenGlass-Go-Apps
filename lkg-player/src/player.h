#ifndef LKG_PLAYER_H
#define LKG_PLAYER_H

#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <sys/types.h>

#define LKG_MAX_CHUNKS 8

typedef struct {
    int columns;
    int rows;
    int views;
    float aspect;
    float zoom;
    float position_x;
    float position_y;
    float focus;
    float stereo_disparity;
    int reverse_views;
    int movie;
    int clamp_edges;
} QuiltConfig;

typedef struct {
    float pitch;
    float slope;
    float center;
    float fringe;
    int dpi;
    int screen_w;
    int screen_h;
    int inv_view;
    int flip_image_x;
    int flip_image_y;
    int flip_subp;
} Calibration;

typedef struct {
    char *media_path;
    char *cached_path;
    int cached_plain;
    QuiltConfig quilt;
    int plain;
} MediaConfig;

typedef struct {
    pid_t pid;
    int fd;
    int width;
    int height;
    double fps;
    uint64_t total_frames;
    size_t frame_size;
    size_t read_size;
    int bgra;
    int yuv420;
    int yuv_bt709;
    int yuv_full_range;
    int hardware;
    int decode_threads;
    int movie;
    int has_audio;
    int audio_aac;
    int rotated_90;
    int heic;
    int hevc;
    int spatial;
    int stereo;
    int stereo_left_index;
    int stereo_right_index;
    double spatial_disparity;
    uint8_t *still_frame;
    int nv12_width;
    int nv12_height;
    int dma_fds[2];
    uint32_t dma_strides[2];
    uint32_t dma_offsets[2];
    void *gst_pipeline;
    void *gst_sink;
    void *gst_sample;
    void *gst_previous_sample;
    void *sws_context;
    int keep_previous_sample;
    char *path;
} Decoder;

typedef struct Renderer Renderer;

void set_error(char *error, size_t size, const char *format, ...);
int process_bind_parent(pid_t parent_pid);

const char *media_cache_dir(const char *json_path, const char *base_dir,
                            char *buffer, size_t size);
int media_find_cached_image(const char *json_path, const char *calibration_path,
                            const char *media_path, const char *base_dir,
                            char **cached_path, char *error, size_t error_size);
int config_load_media(const char *json_path, MediaConfig *out, Decoder *decoder,
                      const char *calibration_path, const char *cache_dir,
                      Renderer *progress_renderer, char *error, size_t error_size);
int config_load_calibration(const char *path, Calibration *out, char *error, size_t error_size);
void config_free_media(MediaConfig *media);

int decoder_probe(Decoder *decoder, const char *path, char *error, size_t error_size);
int decoder_probe_cached(Decoder *decoder, const char *path, char *error, size_t error_size);
void decoder_configure_nv12(Decoder *decoder);
int decoder_can_yuv420(const Decoder *decoder);
void decoder_configure_yuv420(Decoder *decoder);
void decoder_write_cached_metadata(const char *path, int width, int height,
                                   double fps, uint64_t total_frames, int has_audio);
void decoder_discard_cached(const char *path);
int decoder_probe_image(Decoder *decoder, const char *path, char *error, size_t error_size);
int decoder_preprocessing_threads(const Decoder *decoder);
int decoder_start(Decoder *decoder, char *error, size_t error_size);
int decoder_read_frame(Decoder *decoder, uint8_t *frame);
void decoder_stop(Decoder *decoder);
int gstreamer_start(Decoder *decoder, char *error, size_t error_size);
int gstreamer_start_paused(Decoder *decoder, char *error, size_t error_size);
int gstreamer_start_software(Decoder *decoder, char *error, size_t error_size);
int gstreamer_read(Decoder *decoder, char *error, size_t error_size);
int gstreamer_read_software(Decoder *decoder, uint8_t *frame, char *error, size_t error_size);
int gstreamer_restart(Decoder *decoder);
int gstreamer_pause(Decoder *decoder, int paused);
void gstreamer_stop(Decoder *decoder);

Renderer *renderer_create(const char *drm_device, int source_w, int source_h,
                          const QuiltConfig *quilt, const Calibration *calibration,
                          int direct, const char *progress_background, int restore_crtc,
                          int show_progress, char *error, size_t error_size);
Renderer *renderer_create_offscreen(const char *drm_device, int source_w, int source_h,
                                    const QuiltConfig *quilt, const Calibration *calibration,
                                    int direct, int progress_fd, char *error, size_t error_size);
int renderer_reconfigure(Renderer *renderer, int source_w, int source_h,
                         const QuiltConfig *quilt, int direct, char *error, size_t error_size);
int renderer_draw(Renderer *renderer, const uint8_t *frame, char *error, size_t error_size);
int renderer_draw_dma(Renderer *renderer, Decoder *decoder, char *error, size_t error_size);
void renderer_release_dma(Renderer *renderer);
int renderer_upload_rows(Renderer *renderer, int source_y, int rows,
                         const uint8_t *rgb, char *error, size_t error_size);
int renderer_thumbnail_prepare(Renderer *renderer, const char *media_path,
                               const char *cache_dir);
int renderer_thumbnail_finish(Renderer *renderer);
int renderer_read_rgb(Renderer *renderer, uint8_t *rgb, char *error, size_t error_size);
int renderer_render_to_rgb(Renderer *renderer, const uint8_t *frame, uint8_t *rgb,
                           char *error, size_t error_size);
int renderer_render_to_bgra(Renderer *renderer, const uint8_t *frame, uint8_t *bgra,
                            char *error, size_t error_size);
void renderer_get_profile(Renderer *renderer, double *upload, double *draw,
                          double *readback, double *convert);
void renderer_set_yuv_colorimetry(Renderer *renderer, int bt709, int full_range);
int renderer_progress(Renderer *renderer, double fraction);
int renderer_progress_frames(Renderer *renderer, double fraction,
                             uint64_t current, uint64_t total);
void renderer_begin_progress(Renderer *renderer, int movie);
void renderer_clear_progress_background(Renderer *renderer);
int renderer_refresh(Renderer *renderer, char *error, size_t error_size);
int renderer_show_error(Renderer *renderer, char *error, size_t error_size);
int renderer_set_paused(Renderer *renderer, int paused, char *error, size_t error_size);
void renderer_track_pause(Renderer *renderer, volatile sig_atomic_t *requested);
int renderer_snapshot(Renderer *renderer, const char *path, char *error, size_t error_size);
void renderer_destroy(Renderer *renderer);
int renderer_width(const Renderer *renderer);
int renderer_height(const Renderer *renderer);

int png_stream_upload(const char *path, Renderer *renderer,
                      volatile sig_atomic_t *stopped, char *error, size_t error_size);
int jpeg_stream_upload(const char *path, Renderer *renderer,
                       volatile sig_atomic_t *stopped, char *error, size_t error_size);
int image_stream_upload(const char *path, Renderer *renderer,
                        volatile sig_atomic_t *stopped, char *error, size_t error_size);
int png_read_rgb(const char *path, int width, int height, uint8_t *rgb,
                 char *error, size_t error_size);
int cache_read_rgb(const char *path, int width, int height, uint8_t *rgb,
                   char *error, size_t error_size);

int find_cached_video(const char *json_path, const char *calibration_path,
                      const char *media_path, const char *cache_dir,
                      char **cached_path, char *error, size_t error_size);
int find_cached_plain_video(const char *json_path, const char *calibration_path,
                            const char *media_path, const char *cache_dir,
                            char **cached_path, char *error, size_t error_size);
int preprocess_video(Decoder *decoder, Renderer *renderer, const char *json_path,
                     const char *calibration_path, const char *cache_dir,
                     volatile sig_atomic_t *stopped, char **cached_path,
                     char *error, size_t error_size);
int preprocess_plain_video(Decoder *decoder, Renderer *renderer, int progress_fd,
                           const char *json_path, const char *calibration_path,
                           const char *cache_dir, volatile sig_atomic_t *stopped,
                           char **cached_path, char *error, size_t error_size);
int find_cached_image(const char *json_path, const char *calibration_path,
                      const char *media_path, const char *cache_dir,
                      char **cached_path, char *error, size_t error_size);
int preprocess_image(Decoder *decoder, Renderer *renderer, const char *json_path,
                     const char *calibration_path, const char *cache_dir,
                     volatile sig_atomic_t *stopped, char **cached_path,
                     char *error, size_t error_size);

int slideshow_run(const char *playlist_path, const char *calibration_path,
                  const char *cache_dir, const char *drm_device, int control_fd,
                  double image_duration, int restore_crtc, size_t start_index,
                  int manual, volatile sig_atomic_t *stopped);
int prerender_run(const char *playlist_path, const char *calibration_path,
                  const char *cache_dir, int progress_fd,
                  volatile sig_atomic_t *stopped);
int prerender_item_run(const char *json_path, const char *calibration_path,
                       const char *cache_dir, int progress_fd);

#endif
