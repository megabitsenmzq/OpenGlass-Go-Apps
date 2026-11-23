#ifndef LKG_MENU_MODEL_H
#define LKG_MENU_MODEL_H

typedef enum {
    PAGE_MAIN = 0,
    PAGE_SETTINGS,
    PAGE_CLEAR_CACHE,
    PAGE_SLIDESHOW_DURATION,
    PAGE_BUTTON_DIRECTION,
    PAGE_3D_MENU,
    PAGE_AUTO_PLAY,
    PAGE_VOLUME,
    PAGE_INFO,
    PAGE_FILE_MANAGER
} MenuPage;

typedef enum {
    ACTION_NONE = 0,
    ACTION_APPS,
    ACTION_GALLERY,
    ACTION_SWAY,
    ACTION_TTY,
    ACTION_POWER_OFF,
    ACTION_CLEAR_CACHE,
    ACTION_PRERENDER,
    ACTION_WIFI,
    ACTION_USB_DESKTOP
} MenuAction;

typedef enum {
    MENU_3D_OFF = 0,
    MENU_3D_SUBTLE = 1,
    MENU_3D_DEEP = 2
} Menu3dMode;

typedef struct {
    int page;
    int selected;
    int has_apps;
    int slideshow_duration;
    int button_reversed;
    int auto_play;
    int menu_3d;
    int volume;
} MenuState;

typedef int (*MenuSaveSettings)(int seconds, int button_reversed, int auto_play,
                                int menu_3d, void *context);

void menu_items(int page, int has_apps, const char ***items, int *count);
void menu_return_to_parent(int *page, int *selected, int has_apps);
int menu_duration_index(int seconds);
int menu_volume_index(int percent);
MenuAction menu_handle_message(MenuState *state, const char *message, int *changed,
                               MenuSaveSettings save_settings, void *save_context);

#endif
