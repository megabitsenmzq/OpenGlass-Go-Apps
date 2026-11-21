#include "test.h"
#include "media_scan.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static void touch(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (file)
        fclose(file);
}

int test_media_scan(void)
{
    char directory[] = "/tmp/lkg-media-scan-XXXXXX";
    char folder[256];
    char path[512];
    GalleryItem *items = NULL;
    size_t count = 0;
    TEST_ASSERT(media_extension("photo.JPG") == 1);
    TEST_ASSERT(media_extension("spatial.HEIC") == 1);
    TEST_ASSERT(media_extension("image.heif") == 1);
    TEST_ASSERT(media_extension("notes.txt") == 0);
    TEST_ASSERT(video_extension("movie.MP4") == 1);
    TEST_ASSERT(video_extension("photo.jpg") == 0);
    TEST_ASSERT(video_extension("spatial.HEIC") == 0);
    TEST_ASSERT(mkdtemp(directory) != NULL);
    snprintf(folder, sizeof(folder), "%s/Folder", directory);
    TEST_ASSERT(mkdir(folder, 0700) == 0);
    snprintf(path, sizeof(path), "%s/z.jpg", directory);
    touch(path);
    snprintf(path, sizeof(path), "%s/0.mp4", directory);
    touch(path);
    snprintf(path, sizeof(path), "%s/ignore.txt", directory);
    touch(path);
    snprintf(path, sizeof(path), "%s/b.png", folder);
    touch(path);
    snprintf(path, sizeof(path), "%s/a.jpg", folder);
    touch(path);
    snprintf(path, sizeof(path), "%s/z.HEIC", folder);
    touch(path);
    snprintf(path, sizeof(path), "%s/0.mp4", folder);
    touch(path);
    snprintf(path, sizeof(path), "%s/Loop", directory);
    TEST_ASSERT(symlink(".", path) == 0);

    TEST_ASSERT(media_scan_directory(&items, &count, directory) == 0);
    TEST_ASSERT(count == 3);
    TEST_STRING(items[0].name, "Folder");
    TEST_ASSERT(items[0].is_directory == 1);
    TEST_STRING(items[1].name, "0.mp4");
    TEST_STRING(items[2].name, "z.jpg");
    TEST_ASSERT(strstr(items[0].cover_path, "/0.mp4") != NULL);
    TEST_ASSERT(media_thumbnail_path(&items[0]) == items[0].cover_path);
    TEST_ASSERT(media_thumbnail_path(&items[1]) == items[1].path);
    media_items_clear(&items, count);
    TEST_ASSERT(items == NULL);

    snprintf(path, sizeof(path), "%s/Loop", directory);
    unlink(path);
    snprintf(path, sizeof(path), "%s/b.png", folder);
    unlink(path);
    snprintf(path, sizeof(path), "%s/a.jpg", folder);
    unlink(path);
    snprintf(path, sizeof(path), "%s/z.HEIC", folder);
    unlink(path);
    snprintf(path, sizeof(path), "%s/0.mp4", folder);
    unlink(path);
    rmdir(folder);
    snprintf(path, sizeof(path), "%s/z.jpg", directory);
    unlink(path);
    snprintf(path, sizeof(path), "%s/0.mp4", directory);
    unlink(path);
    snprintf(path, sizeof(path), "%s/ignore.txt", directory);
    unlink(path);
    rmdir(directory);
    return 0;
}
