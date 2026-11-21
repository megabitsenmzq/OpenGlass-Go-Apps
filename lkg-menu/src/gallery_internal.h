#ifndef LKG_MENU_GALLERY_INTERNAL_H
#define LKG_MENU_GALLERY_INTERNAL_H

#include "depth_frame.h"
#include "media_scan.h"
#include "serial_parser.h"
#include "usb_keyboard.h"

#include <limits.h>

#define MEDIA_ROOT "/home/lkg/data/media"
#define THUMBNAIL_ROOT "/home/lkg/data/cache/thumbnails"
#define PLAYER_PATH "/home/lkg/bin/lkg-player"
#define CALIBRATION_PATH "/run/lkg/visual.json"
#define PLAYER_CACHE "/home/lkg/data/cache"
#define TRANSITION_PATH "/home/lkg/data/cache/.slideshow-frame.ppm"
#define TRANSITION_PID_PATH "/home/lkg/data/cache/.slideshow-frame.ppm.pid"
#define PAGE_ITEMS 9
#define FIRST_PAGE_ITEMS 8

typedef struct {
    const GalleryItem *item;
    const uint8_t *thumbnail;
    int thumbnail_state;
    float progress;
    int selected;
} GalleryCellState;

typedef struct {
    DepthCacheState cache_state;
    int page;
    int cursor;
    int layers_cursor;
    int displayed_cached;
    GalleryCellState cells[9];
    DepthLayer *layers;
    uint8_t *packed[2];
    size_t packed_size;
    int base_valid;
    uint8_t valid[9][2];
    int bake_active;
    int bake_base;
    int bake_cell;
    int bake_slot;
    int bake_band;
} GalleryCache;

typedef struct {
    GalleryItem *items;
    size_t count;
    char directory[PATH_MAX];
    DepthEffect *depth;
    GalleryCache cache;
    int page;
    int cursor;
    int allow_cache_bake;
} Gallery;

typedef struct {
    GalleryItem *item;
    pid_t pid;
    int fd;
    double duration;
    float progress;
    size_t order[PAGE_ITEMS * 3];
    size_t order_count;
    size_t order_position;
    int ticks;
    char cache_path[PATH_MAX];
    char temporary[PATH_MAX + 32];
} ThumbWorker;

typedef struct {
    GalleryItem *items;
    size_t count;
} PlaybackList;

double gallery_monotonic_seconds(void);
int gallery_thumbnail_path(GalleryItem *item, char *path, size_t size);
int gallery_load_thumbnail(GalleryItem *item);
int gallery_quilt_dimensions(GalleryItem *item, int *columns, int *rows,
                             double *duration, double *aspect);
int gallery_thumbnail_begin(ThumbWorker *worker, GalleryItem *item);
void gallery_worker_stop(ThumbWorker *worker);
void gallery_thumbnail_progress(char *text, double duration, float *progress);
int gallery_scan(Gallery *gallery, const char *directory);
void gallery_release_memory(Gallery *gallery);
int gallery_playback_collect(PlaybackList *list, const char *directory, int recursive);
int gallery_draw(Display *display, Font *font, Gallery *gallery);
void gallery_reveal(Gallery *gallery, const char *media_path);
int gallery_resume_gpu(Display *display, Gallery *gallery);
void gallery_playback_free(PlaybackList *list);
void gallery_stop_player(pid_t pid, int signal_number);
int gallery_play_media(Display *display, Font *font, Gallery *gallery,
                       GalleryItem *selected, int recursive, double image_duration,
                       int serial_fd, UsbKeyboard *keyboard, SerialParser *parser,
                       int button_reversed);

#endif
