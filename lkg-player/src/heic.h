#ifndef LKG_PLAYER_HEIC_H
#define LKG_PLAYER_HEIC_H

#include <stddef.h>
#include <stdint.h>

int heic_spatial_pair(const char *path, uint32_t *left_id, uint32_t *right_id);
int heic_primary_dimensions(const char *path, int *width, int *height,
                            char *error, size_t error_size);
int heic_decode_primary(const char *path, int expected_width, int expected_height,
                        uint8_t **rgb, char *error, size_t error_size);
int heic_decode_spatial_stereo(const char *path, int eye_width, int eye_height,
                               uint8_t **rgb, char *error, size_t error_size);

#endif
