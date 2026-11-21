#include "media_scan.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

int media_extension(const char *name)
{
    static const char *extensions[] = {
        ".png", ".jpg", ".jpeg", ".heic", ".heif", ".bmp", ".gif", ".webp",
        ".mp4", ".mov", ".mkv", ".webm", ".avi", ".m4v"
    };
    const char *extension = strrchr(name, '.');
    size_t i;
    if (!extension)
        return 0;
    for (i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
        if (strcasecmp(extension, extensions[i]) == 0)
            return 1;
    return 0;
}

int video_extension(const char *name)
{
    static const char *extensions[] = {
        ".mp4", ".mov", ".mkv", ".webm", ".avi", ".m4v"
    };
    const char *extension = strrchr(name, '.');
    size_t i;
    if (!extension)
        return 0;
    for (i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
        if (strcasecmp(extension, extensions[i]) == 0)
            return 1;
    return 0;
}

const char *media_thumbnail_path(const GalleryItem *item)
{
    return item->is_directory ? item->cover_path : item->path;
}

void media_items_clear(GalleryItem **items, size_t count)
{
    size_t i;
    if (!items || !*items)
        return;
    for (i = 0; i < count; ++i) {
        free((*items)[i].name);
        free((*items)[i].path);
        free((*items)[i].cover_path);
        free((*items)[i].thumbnail);
    }
    free(*items);
    *items = NULL;
}

int media_item_compare(const void *left, const void *right)
{
    const GalleryItem *a = left;
    const GalleryItem *b = right;
    if (a->is_directory != b->is_directory)
        return b->is_directory - a->is_directory;
    return strcasecmp(a->name, b->name);
}

static int folder_find_cover(GalleryItem *folder)
{
    DIR *directory = opendir(folder->path);
    struct dirent *entry;
    char best_path[PATH_MAX] = {0};
    char best_name[NAME_MAX + 1] = {0};
    if (!directory)
        return 0;
    while ((entry = readdir(directory)) != NULL) {
        char path[PATH_MAX];
        struct stat status;
        if (entry->d_name[0] == '.' || !media_extension(entry->d_name) ||
            (best_name[0] && strcasecmp(entry->d_name, best_name) >= 0) ||
            snprintf(path, sizeof(path), "%s/%s", folder->path, entry->d_name) >=
                (int)sizeof(path) ||
            lstat(path, &status) != 0 || !S_ISREG(status.st_mode))
            continue;
        strcpy(best_path, path);
        strcpy(best_name, entry->d_name);
    }
    closedir(directory);
    if (best_path[0]) {
        folder->cover_path = strdup(best_path);
        if (!folder->cover_path)
            return -1;
    }
    return 0;
}

int media_scan_directory(GalleryItem **items, size_t *count, const char *directory)
{
    DIR *dir;
    struct dirent *entry;
    char path[PATH_MAX];
    size_t i;
    *items = NULL;
    *count = 0;
    if (strlen(directory) >= sizeof(path))
        return -1;
    dir = opendir(directory);
    if (!dir)
        return -1;
    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        GalleryItem *allocation;
        GalleryItem *item;
        int is_directory;
        if (entry->d_name[0] == '.')
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >=
            (int)sizeof(path))
            continue;
        if (lstat(path, &st) != 0)
            continue;
        is_directory = S_ISDIR(st.st_mode);
        if (!is_directory && (!S_ISREG(st.st_mode) || !media_extension(entry->d_name)))
            continue;
        allocation = realloc(*items, (*count + 1) * sizeof(*allocation));
        if (!allocation) {
            closedir(dir);
            media_items_clear(items, *count);
            *count = 0;
            return -1;
        }
        *items = allocation;
        item = &(*items)[*count];
        memset(item, 0, sizeof(*item));
        item->name = strdup(entry->d_name);
        item->path = strdup(path);
        item->is_directory = is_directory;
        if (!item->name || !item->path) {
            free(item->name);
            free(item->path);
            memset(item, 0, sizeof(*item));
            closedir(dir);
            media_items_clear(items, *count);
            *count = 0;
            return -1;
        }
        ++*count;
    }
    closedir(dir);
    qsort(*items, *count, sizeof(**items), media_item_compare);
    for (i = 0; i < *count; ++i) {
        if ((*items)[i].is_directory && folder_find_cover(&(*items)[i]) != 0) {
            media_items_clear(items, *count);
            *count = 0;
            return -1;
        }
    }
    return 0;
}
