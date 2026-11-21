#ifndef LKG_MENU_THUMBNAIL_H
#define LKG_MENU_THUMBNAIL_H

#include <stddef.h>
#include <stdint.h>

int thumbnail_cache_path(const char *media_path, char *path, size_t size);
int thumbnail_cache_path_in(const char *media_path, const char *cache_dir,
                            char *path, size_t size);
int thumbnail_frame_write(const char *output_path, int columns, int rows, int view,
                          double aspect, const uint8_t *tile, int tile_width, int tile_height,
                          int output_width, int output_height);
int thumbnail_image_supported(const char *path);
int thumbnail_image_generate(const char *path, const char *output_path,
                             int columns, int rows, int view, double aspect,
                             int output_width, int output_height, int progress_fd);

#endif
