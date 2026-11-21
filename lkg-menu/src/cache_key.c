#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _XOPEN_SOURCE 700
#endif
#include "cache_key.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const LKG_VIDEO_CACHE_VERSION =
    "lkg-player-v13-portrait-aligned-video-cache";
static const char *const LKG_IMAGE_CACHE_VERSION =
    "lkg-image-v11-window-clamp-exact-tiles";

static uint64_t cache_hash_bytes(uint64_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    size_t i;
    for (i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t cache_hash_path(uint64_t hash, const char *path)
{
    char *resolved = realpath(path, NULL);
    const char *value = resolved ? resolved : path;
    hash = cache_hash_bytes(hash, value, strlen(value) + 1);
    free(resolved);
    return hash;
}

static int cache_hash_file(uint64_t *hash, const char *path)
{
    uint8_t buffer[65536];
    FILE *file = fopen(path, "rb");
    size_t count;
    int result = 0;
    if (!file)
        return -1;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
        *hash = cache_hash_bytes(*hash, buffer, count);
    if (ferror(file))
        result = -1;
    fclose(file);
    return result;
}

uint64_t lkg_cache_plain_video_hash(uint64_t video_hash)
{
    static const char tag[] = "plain-h264-aligned-v1";
    return cache_hash_bytes(video_hash, tag, sizeof(tag));
}

int lkg_cache_media_hash(LkgCacheKind kind, const char *media_path,
                         const char *json_path, const char *calibration_path,
                         const uint8_t *calibration_data, size_t calibration_size,
                         uint64_t *hash_out)
{
    uint64_t hash = 1469598103934665603ULL;
    const char *version = kind == LKG_CACHE_IMAGE ? LKG_IMAGE_CACHE_VERSION :
                                                    LKG_VIDEO_CACHE_VERSION;
    struct stat media_stat;
    if (!media_path || !json_path || !calibration_path || !hash_out) {
        errno = EINVAL;
        return -1;
    }
    if (stat(media_path, &media_stat) != 0)
        return -1;
    hash = cache_hash_bytes(hash, version, strlen(version));
    hash = cache_hash_path(hash, media_path);
    hash = cache_hash_path(hash, json_path);
    hash = cache_hash_path(hash, calibration_path);
    hash = cache_hash_bytes(hash, &media_stat.st_size, sizeof(media_stat.st_size));
#if defined(__APPLE__)
    hash = cache_hash_bytes(hash, &media_stat.st_mtimespec.tv_sec,
                            sizeof(media_stat.st_mtimespec.tv_sec));
    hash = cache_hash_bytes(hash, &media_stat.st_mtimespec.tv_nsec,
                            sizeof(media_stat.st_mtimespec.tv_nsec));
#else
    hash = cache_hash_bytes(hash, &media_stat.st_mtim.tv_sec,
                            sizeof(media_stat.st_mtim.tv_sec));
    hash = cache_hash_bytes(hash, &media_stat.st_mtim.tv_nsec,
                            sizeof(media_stat.st_mtim.tv_nsec));
#endif
    if (access(json_path, F_OK) != 0 && errno == ENOENT) {
        static const char no_sidecar[] = "no-sidecar";
        hash = cache_hash_bytes(hash, no_sidecar, sizeof(no_sidecar));
    } else if (cache_hash_file(&hash, json_path) != 0) {
        return -1;
    }
    if (calibration_data) {
        hash = cache_hash_bytes(hash, calibration_data, calibration_size);
    } else if (cache_hash_file(&hash, calibration_path) != 0) {
        return -1;
    }
    if (kind == LKG_CACHE_PLAIN_VIDEO)
        hash = lkg_cache_plain_video_hash(hash);
    *hash_out = hash;
    return 0;
}
