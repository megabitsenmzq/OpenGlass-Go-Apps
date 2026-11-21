#ifndef LKG_MENU_MEDIA_SCAN_H
#define LKG_MENU_MEDIA_SCAN_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *name;
    char *path;
    char *cover_path;
    int is_directory;
    uint8_t *thumbnail;
    int thumbnail_state;
    float progress;
} GalleryItem;

int media_extension(const char *name);
int video_extension(const char *name);
const char *media_thumbnail_path(const GalleryItem *item);
int media_item_compare(const void *left, const void *right);
void media_items_clear(GalleryItem **items, size_t count);
int media_scan_directory(GalleryItem **items, size_t *count, const char *directory);

#endif
