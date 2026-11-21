#ifndef LKG_CACHE_KEY_H
#define LKG_CACHE_KEY_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    LKG_CACHE_IMAGE = 0,
    LKG_CACHE_VIDEO,
    LKG_CACHE_PLAIN_VIDEO
} LkgCacheKind;

uint64_t lkg_cache_plain_video_hash(uint64_t video_hash);

int lkg_cache_media_hash(LkgCacheKind kind, const char *media_path,
                         const char *json_path, const char *calibration_path,
                         const uint8_t *calibration_data, size_t calibration_size,
                         uint64_t *hash_out);

#endif
