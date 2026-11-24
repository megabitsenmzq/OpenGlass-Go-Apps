#ifndef LKG_MENU_APPS_SCAN_H
#define LKG_MENU_APPS_SCAN_H

#include "lkg_app_protocol.h"

#include <stddef.h>

#define APPS_ROOT "/home/lkg/data/apps"

typedef struct {
    char *dir_name;
    char *dir_path;
    char *exec_path;
    char *name;
    char *version;
    char *description;
    char *author;
    char *font_path;
    LkgRenderMode render_mode;
    int api_version;
} ExternalApp;

typedef struct {
    ExternalApp *items;
    size_t count;
} ExternalAppList;

int apps_scan_directory(ExternalAppList *list, const char *directory);
int apps_scan_has_apps(const char *directory);
void apps_list_free(ExternalAppList *list);

#endif
