#include "test.h"
#include "cache_key.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    size_t length = strlen(text);
    if (!file)
        return -1;
    if (fwrite(text, 1, length, file) != length || fclose(file) != 0)
        return -1;
    return 0;
}

int test_cache_key(void)
{
    char dir_template[] = "/tmp/lkg-cache-key-test-XXXXXX";
    char *dir = mkdtemp(dir_template);
    char media_path[PATH_MAX];
    char json_path[PATH_MAX];
    char cal_path[PATH_MAX];
    const char *cal_text = "{\"pitch\":{\"value\":47.5}}";
    uint64_t video_file_hash = 0;
    uint64_t video_mem_hash = 0;
    uint64_t plain_hash = 0;
    uint64_t image_hash = 0;
    uint64_t sidecar_hash = 0;
    uint64_t sidecar_hash_2 = 0;

    TEST_ASSERT(dir != NULL);
    snprintf(media_path, sizeof(media_path), "%s/clip.mp4", dir);
    snprintf(json_path, sizeof(json_path), "%s/clip.mp4.json", dir);
    snprintf(cal_path, sizeof(cal_path), "%s/visual.json", dir);
    TEST_ASSERT(write_text(media_path, "dummy-media-bytes") == 0);
    TEST_ASSERT(write_text(cal_path, cal_text) == 0);

    /* File-backed calibration and preloaded calibration bytes must produce identical hashes */
    TEST_ASSERT(lkg_cache_media_hash(LKG_CACHE_VIDEO, media_path, json_path, cal_path,
                                     NULL, 0, &video_file_hash) == 0);
    TEST_ASSERT(lkg_cache_media_hash(LKG_CACHE_VIDEO, media_path, json_path, cal_path,
                                     (const uint8_t *)cal_text, strlen(cal_text),
                                     &video_mem_hash) == 0);
    TEST_ASSERT(video_file_hash != 0);
    TEST_ASSERT(video_file_hash == video_mem_hash);

    /* Plain video hash must match lkg_cache_plain_video_hash(video_hash) */
    TEST_ASSERT(lkg_cache_media_hash(LKG_CACHE_PLAIN_VIDEO, media_path, json_path, cal_path,
                                     NULL, 0, &plain_hash) == 0);
    TEST_ASSERT(plain_hash == lkg_cache_plain_video_hash(video_file_hash));
    TEST_ASSERT(plain_hash != video_file_hash);

    /* Image hash differs from video hash */
    TEST_ASSERT(lkg_cache_media_hash(LKG_CACHE_IMAGE, media_path, json_path, cal_path,
                                     NULL, 0, &image_hash) == 0);
    TEST_ASSERT(image_hash != video_file_hash);

    /* Adding and changing sidecar updates hash */
    TEST_ASSERT(write_text(json_path, "{\"quilt_settings\":{\"viewX\":8,\"viewY\":6}}") == 0);
    TEST_ASSERT(lkg_cache_media_hash(LKG_CACHE_VIDEO, media_path, json_path, cal_path,
                                     NULL, 0, &sidecar_hash) == 0);
    TEST_ASSERT(sidecar_hash != video_file_hash);
    TEST_ASSERT(write_text(json_path, "{\"quilt_settings\":{\"viewX\":5,\"viewY\":9}}") == 0);
    TEST_ASSERT(lkg_cache_media_hash(LKG_CACHE_VIDEO, media_path, json_path, cal_path,
                                     NULL, 0, &sidecar_hash_2) == 0);
    TEST_ASSERT(sidecar_hash_2 != sidecar_hash);

    unlink(media_path);
    unlink(json_path);
    unlink(cal_path);
    rmdir(dir);
    return 0;
}
