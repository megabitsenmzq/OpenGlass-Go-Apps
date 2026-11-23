#include "gallery_internal.h"
#include "gallery_view.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <xf86drm.h>

void gallery_playback_free(PlaybackList *list)
{
    size_t i;
    for (i = 0; i < list->count; ++i) {
        free(list->items[i].name);
        free(list->items[i].path);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int playback_add(PlaybackList *list, const char *name, const char *path)
{
    GalleryItem *items = realloc(list->items, (list->count + 1) * sizeof(*items));
    GalleryItem *item;
    if (!items)
        return -1;
    list->items = items;
    item = &list->items[list->count];
    memset(item, 0, sizeof(*item));
    item->name = strdup(name);
    item->path = strdup(path);
    if (!item->name || !item->path) {
        free(item->name);
        free(item->path);
        memset(item, 0, sizeof(*item));
        return -1;
    }
    ++list->count;
    return 0;
}

int gallery_playback_collect(PlaybackList *list, const char *directory, int recursive)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    GalleryItem *entries = NULL;
    size_t count = 0;
    size_t i;
    int result = -1;
    if (!dir)
        return -1;
    while ((entry = readdir(dir))) {
        char path[PATH_MAX];
        struct stat st;
        GalleryItem *resized;
        GalleryItem *item;
        if (entry->d_name[0] == '.')
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >= (int)sizeof(path) ||
            lstat(path, &st) != 0)
            continue;
        if (!S_ISDIR(st.st_mode) && (!S_ISREG(st.st_mode) || !media_extension(entry->d_name)))
            continue;
        resized = realloc(entries, (count + 1) * sizeof(*entries));
        if (!resized)
            goto cleanup;
        entries = resized;
        item = &entries[count];
        memset(item, 0, sizeof(*item));
        item->name = strdup(entry->d_name);
        item->path = strdup(path);
        item->is_directory = S_ISDIR(st.st_mode);
        if (!item->name || !item->path)
            goto cleanup;
        ++count;
    }
    closedir(dir);
    dir = NULL;
    qsort(entries, count, sizeof(*entries), media_item_compare);
    for (i = 0; i < count; ++i) {
        if (entries[i].is_directory) {
            if (recursive && gallery_playback_collect(list, entries[i].path, 1) != 0)
                goto cleanup;
        } else if (playback_add(list, entries[i].name, entries[i].path) != 0) {
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    if (dir)
        closedir(dir);
    for (i = 0; i < count; ++i) {
        free(entries[i].name);
        free(entries[i].path);
    }
    free(entries);
    return result;
}

static pid_t start_slideshow(PlaybackList *list, size_t start_index, double image_duration,
                             int manual, int *control_fd, char *playlist_path)
{
    int sockets[2];
    int file_descriptor;
    FILE *file;
    pid_t pid;
    size_t i;
    char parent[16];
    (void)mkdir("/home/lkg/data", 0755);
    (void)mkdir("/home/lkg/data/cache", 0755);
    strcpy(playlist_path, "/home/lkg/data/cache/.slideshow-XXXXXX");
    file_descriptor = mkstemp(playlist_path);
    if (file_descriptor < 0)
        return -1;
    file = fdopen(file_descriptor, "w");
    if (!file) {
        close(file_descriptor);
        unlink(playlist_path);
        return -1;
    }
    for (i = 0; i < list->count; ++i)
        fprintf(file, "%s.json\n", list->items[i].path);
    if (fclose(file) != 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        unlink(playlist_path);
        return -1;
    }
    snprintf(parent, sizeof(parent), "%d", (int)getpid());
    pid = fork();
    if (pid == 0) {
        char control[16];
        close(sockets[1]);
        snprintf(control, sizeof(control), "%d", sockets[0]);
        if (manual) {
            char start[32];
            snprintf(start, sizeof(start), "%lu", (unsigned long)start_index);
            execl(PLAYER_PATH, PLAYER_PATH, "--cache-dir", PLAYER_CACHE,
                  "--calibration", CALIBRATION_PATH, "--no-restore",
                  "--playlist", playlist_path, "--start-index", start,
                  "--manual", "--control-fd", control,
                  "--parent-pid", parent, (char *)NULL);
        } else {
            char duration[32];
            snprintf(duration, sizeof(duration), "%.0f", image_duration);
            execl(PLAYER_PATH, PLAYER_PATH, "--cache-dir", PLAYER_CACHE,
                  "--calibration", CALIBRATION_PATH, "--no-restore",
                  "--image-duration", duration, "--playlist", playlist_path,
                  "--control-fd", control, "--parent-pid", parent, (char *)NULL);
        }
        _exit(127);
    }
    close(sockets[0]);
    if (pid < 0) {
        close(sockets[1]);
        unlink(playlist_path);
        return -1;
    }
    fcntl(sockets[1], F_SETFD, FD_CLOEXEC);
    *control_fd = sockets[1];
    return pid;
}

void gallery_stop_player(pid_t pid, int signal_number)
{
    int status;
    int attempt;
    kill(pid, signal_number);
    for (attempt = 0; attempt < 40; ++attempt) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid || (result < 0 && errno != EINTR))
            return;
        usleep(50000);
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
}


void gallery_reveal(Gallery *gallery, const char *media_path)
{
    char directory[PATH_MAX];
    char *slash;
    size_t i;
    if (strlen(media_path) >= sizeof(directory))
        return;
    strcpy(directory, media_path);
    slash = strrchr(directory, '/');
    if (!slash)
        return;
    *slash = '\0';
    if (gallery_scan(gallery, directory) != 0)
        return;
    for (i = 0; i < gallery->count; ++i) {
        if (strcmp(gallery->items[i].path, media_path) == 0) {
            if (i < FIRST_PAGE_ITEMS) {
                gallery->page = 0;
                gallery->cursor = (int)i + 2;
            } else {
                size_t later = i - FIRST_PAGE_ITEMS;
                gallery->page = 1 + (int)(later / PAGE_ITEMS);
                gallery->cursor = (int)(later % PAGE_ITEMS);
            }
            return;
        }
    }
}

int gallery_resume_gpu(Display *display, Gallery *gallery)
{
    int stereo = gallery->depth->stereo;
    float front = gallery->depth->front_disparity;
    float back = gallery->depth->back_disparity;
    if (depth_effect_open(gallery->depth, display, CALIBRATION_PATH) != 0)
        return -1;
    gallery->depth->stereo = stereo;
    gallery->depth->front_disparity = front;
    gallery->depth->back_disparity = back;
    printf("Menu GPU resources resumed after playback\n");
    return 0;
}

int gallery_play_media(Display *display, Font *font, Gallery *gallery,
                      GalleryItem *selected, int recursive, double image_duration,
                      int serial_fd, UsbKeyboard *keyboard, SerialParser *parser,
                      int button_reversed)
{
    PlaybackList list;
    size_t index = 0;
    size_t i;
    pid_t pid = -1;
    int child_status = 0;
    int child_failed = 0;
    int control_fd = -1;
    int depth_suspended = 0;
    int layers_suspended = 0;
    int result = 0;
    char playlist_path[64] = {0};
    memset(&list, 0, sizeof(list));
    if (gallery_playback_collect(&list, gallery->directory, recursive) != 0 || list.count == 0)
        goto cleanup;
    if (selected) {
        for (i = 0; i < list.count; ++i) {
            if (strcmp(list.items[i].path, selected->path) == 0) {
                index = i;
                break;
            }
        }
    }
    if (recursive) {
        unlink(TRANSITION_PATH);
        unlink(TRANSITION_PID_PATH);
    }
    gallery_release_memory(gallery);
    if (gallery->cache.layers) {
        depth_layer_destroy(&gallery->cache.layers[0]);
        depth_layer_destroy(&gallery->cache.layers[1]);
        layers_suspended = 1;
    }
    depth_effect_close(gallery->depth);
    depth_suspended = 1;
    printf("Menu GPU and layer resources suspended for playback\n");
    if (drmDropMaster(display->fd) != 0)
        goto cleanup;
    pid = start_slideshow(&list, index, image_duration, !recursive,
                          &control_fd, playlist_path);
    if (pid < 0)
        child_failed = 1;
    while (pid > 0 && running) {
        char command[16];
        int event = usb_keyboard_next_command(keyboard, command, sizeof(command));
        pid_t wait_result;
        if (event > 0)
            usb_keyboard_prepare_command(command, button_reversed);
        if (event == 0)
            event = serial_parser_next_with_fd(serial_fd, parser, command, sizeof(command),
                                               50, keyboard->fd);
        if (event > 0)
            normalize_button_command(command, button_reversed);
        if (event > 0 && strcmp(command, "SHUT") == 0) {
            gallery_stop_player(pid, SIGTERM);
            pid = -1;
            result = SERIAL_RESULT_POWER_OFF;
            break;
        }
        if (event > 0 && strcmp(command, "USBD") == 0) {
            gallery_stop_player(pid, SIGTERM);
            pid = -1;
            result = SERIAL_RESULT_USB_DESKTOP;
            break;
        }
        if (event > 0 && strcmp(command, "LOOP") == 0) {
            gallery_stop_player(pid, SIGTERM);
            pid = -1;
            break;
        }
        if (event > 0) {
            char packet[20];
            int length = snprintf(packet, sizeof(packet), "%s\n", command);
            (void)send(control_fd, packet, (size_t)length, MSG_NOSIGNAL);
        }
        if (event < 0) {
            gallery_stop_player(pid, SIGTERM);
            pid = -1;
            break;
        }
        wait_result = waitpid(pid, &child_status, WNOHANG);
        if (wait_result == pid) {
            if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
                child_failed = 1;
            pid = -1;
            break;
        }
    }
    if (playlist_path[0]) {
        char status_path[80];
        FILE *status_file;
        unsigned long status_index;
        snprintf(status_path, sizeof(status_path), "%s.current", playlist_path);
        status_file = fopen(status_path, "r");
        if (status_file) {
            if (fscanf(status_file, "%lu", &status_index) == 1 && status_index < list.count)
                index = (size_t)status_index;
            fclose(status_file);
        }
        unlink(status_path);
    }
    if (control_fd >= 0) {
        close(control_fd);
        control_fd = -1;
    }
    if (pid > 0)
        gallery_stop_player(pid, SIGTERM);
    if (playlist_path[0]) {
        unlink(playlist_path);
        playlist_path[0] = '\0';
    }
    while (drmSetMaster(display->fd) != 0 && errno == EBUSY)
        usleep(50000);
    if (gallery_resume_gpu(display, gallery) == 0)
        depth_suspended = 0;
    if (layers_suspended && gallery->cache.layers &&
        depth_layer_create(display, &gallery->cache.layers[0],
                           gallery->depth->front_disparity) == 0 &&
        depth_layer_create(display, &gallery->cache.layers[1],
                           gallery->depth->back_disparity) == 0) {
        layers_suspended = 0;
    } else if (layers_suspended && gallery->cache.layers) {
        depth_layer_destroy(&gallery->cache.layers[0]);
        depth_layer_destroy(&gallery->cache.layers[1]);
    }
    if (index < list.count)
        gallery_reveal(gallery, list.items[index].path);
    if (child_failed && !depth_suspended && !layers_suspended) {
        fprintf(stderr, "Media playback or preprocessing failed\n");
        (void)gallery_view_show_error(display, gallery->depth, gallery->cache.layers);
        usleep(2000000);
    }
    tcflush(serial_fd, TCIFLUSH);

cleanup:
    if (control_fd >= 0)
        close(control_fd);
    if (playlist_path[0]) {
        char status_path[80];
        snprintf(status_path, sizeof(status_path), "%s.current", playlist_path);
        unlink(status_path);
        unlink(playlist_path);
    }
    if (recursive) {
        unlink(TRANSITION_PATH);
        unlink(TRANSITION_PID_PATH);
    }
    if (depth_suspended) {
        while (drmSetMaster(display->fd) != 0 && errno == EBUSY)
            usleep(50000);
        if (gallery_resume_gpu(display, gallery) == 0)
            depth_suspended = 0;
    }
    if (layers_suspended && gallery->cache.layers) {
        if (depth_layer_create(display, &gallery->cache.layers[0],
                               gallery->depth->front_disparity) == 0 &&
            depth_layer_create(display, &gallery->cache.layers[1],
                               gallery->depth->back_disparity) == 0) {
            layers_suspended = 0;
        } else {
            depth_layer_destroy(&gallery->cache.layers[0]);
            depth_layer_destroy(&gallery->cache.layers[1]);
        }
    }
    gallery_playback_free(&list);
    if (depth_suspended || layers_suspended)
        return -1;
    if (result != 0)
        return result;
    return gallery_draw(display, font, gallery);
}
