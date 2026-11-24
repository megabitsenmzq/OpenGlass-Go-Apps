#include "menu_model.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE(array) (int)(sizeof(array) / sizeof((array)[0]))
#define NO_RULE { ACTION_NONE, -1, -1 }
#define RULE(action, page, selected) { action, page, selected }

typedef struct {
    MenuAction action;
    int target_page;
    int target_selected;
} MenuItemRule;

typedef struct {
    int page;
    const char **items;
    int count;
    const MenuItemRule *rules;
    int parent;
    int return_focus;
} MenuPageDef;

static const char *main_items[] = {
    "Gallery", "File Manager", "Desktop (Sway)", "Settings", "Return to TTY", "Power Off"
};

static const char *main_items_with_apps[] = {
    "Apps", "Gallery", "File Manager", "Desktop (Sway)", "Settings", "Return to TTY", "Power Off"
};

static const char *file_manager_items[] = { "Back" };

static const char *settings_items[] = {
    "Back", "Pre-render All", "Clear Pre-render Cache", "Slideshow Duration",
    "Button Direction", "3D Menu", "Auto Play", "Network", "Volume", "Info"
};

static const char *info_items[] = {
    "Back", "Version", "Serial", "CPU", "Memory", "GPU", "Display", "Disk", "OS",
    "Kernel Version", "SSH", "IP Address"
};

static const char *clear_cache_items[] = { "Back", "Confirm" };
static const char *direction_items[] = { "Back", "Original", "Reversed" };
static const char *menu_3d_items[] = { "Back", "Off", "Subtle", "Deep" };
static const char *auto_play_items[] = { "Back", "Off", "On" };
static const char *volume_items[] = {
    "Back", "Mute", "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"
};

static const char *duration_items[] = {
    "Back", "1 second", "2 seconds", "3 seconds", "4 seconds", "5 seconds",
    "6 seconds", "7 seconds", "8 seconds", "9 seconds", "10 seconds", "30 seconds",
    "1 minute", "5 minutes", "10 minutes", "30 minutes", "1 hour"
};

static const int duration_values[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 30, 60, 300, 600, 1800, 3600
};

static const MenuItemRule main_rules[] = {
    RULE(ACTION_GALLERY, -1, -1),
    RULE(ACTION_NONE, PAGE_FILE_MANAGER, 0),
    RULE(ACTION_SWAY, -1, -1),
    RULE(ACTION_NONE, PAGE_SETTINGS, 0),
    RULE(ACTION_TTY, -1, -1),
    RULE(ACTION_POWER_OFF, -1, -1)
};

static const MenuItemRule main_rules_with_apps[] = {
    RULE(ACTION_APPS, -1, -1),
    RULE(ACTION_GALLERY, -1, -1),
    RULE(ACTION_NONE, PAGE_FILE_MANAGER, 0),
    RULE(ACTION_SWAY, -1, -1),
    RULE(ACTION_NONE, PAGE_SETTINGS, 0),
    RULE(ACTION_TTY, -1, -1),
    RULE(ACTION_POWER_OFF, -1, -1)
};

static const MenuItemRule file_manager_rules[] = {
    RULE(ACTION_NONE, PAGE_MAIN, 1)
};

static const MenuItemRule settings_rules[] = {
    RULE(ACTION_NONE, PAGE_MAIN, 3),
    RULE(ACTION_PRERENDER, -1, -1),
    RULE(ACTION_NONE, PAGE_CLEAR_CACHE, 0),
    RULE(ACTION_NONE, PAGE_SLIDESHOW_DURATION, 0),
    RULE(ACTION_NONE, PAGE_BUTTON_DIRECTION, 0),
    RULE(ACTION_NONE, PAGE_3D_MENU, 0),
    RULE(ACTION_NONE, PAGE_AUTO_PLAY, 0),
    RULE(ACTION_WIFI, -1, -1),
    RULE(ACTION_NONE, PAGE_VOLUME, 0),
    RULE(ACTION_NONE, PAGE_INFO, 0)
};

static const MenuItemRule info_rules[] = {
    RULE(ACTION_NONE, PAGE_SETTINGS, 9),
    NO_RULE, NO_RULE, NO_RULE, NO_RULE, NO_RULE,
    NO_RULE, NO_RULE, NO_RULE, NO_RULE, NO_RULE,
    NO_RULE
};

static const MenuItemRule clear_cache_rules[] = {
    RULE(ACTION_NONE, PAGE_SETTINGS, 2),
    RULE(ACTION_CLEAR_CACHE, -1, -1)
};

static const MenuItemRule direction_rules[] = {
    RULE(ACTION_NONE, PAGE_SETTINGS, 4),
    RULE(ACTION_NONE, PAGE_SETTINGS, 4),
    RULE(ACTION_NONE, PAGE_SETTINGS, 4)
};

static const MenuItemRule auto_play_rules[] = {
    RULE(ACTION_NONE, PAGE_SETTINGS, 6),
    RULE(ACTION_NONE, PAGE_SETTINGS, 6),
    RULE(ACTION_NONE, PAGE_SETTINGS, 6)
};

static const MenuItemRule volume_rules[] = {
    RULE(ACTION_NONE, PAGE_SETTINGS, 8),
    RULE(ACTION_NONE, PAGE_SETTINGS, 8),
    RULE(ACTION_NONE, PAGE_SETTINGS, 8),
    RULE(ACTION_NONE, PAGE_SETTINGS, 8),
    RULE(ACTION_NONE, PAGE_SETTINGS, 8),
    RULE(ACTION_NONE, PAGE_SETTINGS, 8),
    RULE(ACTION_NONE, PAGE_SETTINGS, 8),
    RULE(ACTION_NONE, PAGE_SETTINGS, 8),
    RULE(ACTION_NONE, PAGE_SETTINGS, 8),
    RULE(ACTION_NONE, PAGE_SETTINGS, 8),
    RULE(ACTION_NONE, PAGE_SETTINGS, 8),
    RULE(ACTION_NONE, PAGE_SETTINGS, 8)
};

static const MenuItemRule menu_3d_rules[] = {
    RULE(ACTION_NONE, PAGE_SETTINGS, 5),
    RULE(ACTION_NONE, PAGE_SETTINGS, 5),
    RULE(ACTION_NONE, PAGE_SETTINGS, 5),
    RULE(ACTION_NONE, PAGE_SETTINGS, 5)
};

static const MenuItemRule duration_rules[] = {
    RULE(ACTION_NONE, PAGE_SETTINGS, 3),
    RULE(ACTION_NONE, PAGE_SETTINGS, 3),
    RULE(ACTION_NONE, PAGE_SETTINGS, 3),
    RULE(ACTION_NONE, PAGE_SETTINGS, 3),
    RULE(ACTION_NONE, PAGE_SETTINGS, 3),
    RULE(ACTION_NONE, PAGE_SETTINGS, 3),
    RULE(ACTION_NONE, PAGE_SETTINGS, 3),
    RULE(ACTION_NONE, PAGE_SETTINGS, 3),
    RULE(ACTION_NONE, PAGE_SETTINGS, 3),
    RULE(ACTION_NONE, PAGE_SETTINGS, 3),
    RULE(ACTION_NONE, PAGE_SETTINGS, 3),
    RULE(ACTION_NONE, PAGE_SETTINGS, 3),
    RULE(ACTION_NONE, PAGE_SETTINGS, 3),
    RULE(ACTION_NONE, PAGE_SETTINGS, 3),
    RULE(ACTION_NONE, PAGE_SETTINGS, 3),
    RULE(ACTION_NONE, PAGE_SETTINGS, 3),
    RULE(ACTION_NONE, PAGE_SETTINGS, 3)
};

static const MenuPageDef pages[] = {
    { PAGE_MAIN, main_items, ARRAY_SIZE(main_items), main_rules, -1, -1 },
    { PAGE_SETTINGS, settings_items, ARRAY_SIZE(settings_items), settings_rules, PAGE_MAIN, 3 },
    { PAGE_CLEAR_CACHE, clear_cache_items, ARRAY_SIZE(clear_cache_items), clear_cache_rules,
      PAGE_SETTINGS, 2 },
    { PAGE_SLIDESHOW_DURATION, duration_items, ARRAY_SIZE(duration_items), duration_rules,
      PAGE_SETTINGS, 3 },
    { PAGE_BUTTON_DIRECTION, direction_items, ARRAY_SIZE(direction_items), direction_rules,
      PAGE_SETTINGS, 4 },
    { PAGE_3D_MENU, menu_3d_items, ARRAY_SIZE(menu_3d_items), menu_3d_rules,
      PAGE_SETTINGS, 5 },
    { PAGE_AUTO_PLAY, auto_play_items, ARRAY_SIZE(auto_play_items), auto_play_rules,
      PAGE_SETTINGS, 6 },
    { PAGE_VOLUME, volume_items, ARRAY_SIZE(volume_items), volume_rules,
      PAGE_SETTINGS, 8 },
    { PAGE_INFO, info_items, ARRAY_SIZE(info_items), info_rules, PAGE_SETTINGS, 9 },
    { PAGE_FILE_MANAGER, file_manager_items, ARRAY_SIZE(file_manager_items),
      file_manager_rules, PAGE_MAIN, 1 }
};

static const MenuPageDef *page_definition(int page)
{
    size_t i;
    for (i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i)
        if (pages[i].page == page)
            return &pages[i];
    return &pages[sizeof(pages) / sizeof(pages[0]) - 1];
}

static void effective_page_info(int page, int has_apps, const char ***items,
                                int *count, const MenuItemRule **rules)
{
    const MenuPageDef *definition = page_definition(page);
    if (page == PAGE_MAIN && has_apps) {
        if (items)
            *items = main_items_with_apps;
        if (count)
            *count = ARRAY_SIZE(main_items_with_apps);
        if (rules)
            *rules = main_rules_with_apps;
        return;
    }
    if (items)
        *items = definition->items;
    if (count)
        *count = definition->count;
    if (rules)
        *rules = definition->rules;
}

void menu_items(int page, int has_apps, const char ***items, int *count)
{
    effective_page_info(page, has_apps, items, count, NULL);
}

void menu_return_to_parent(int *page, int *selected, int has_apps)
{
    const MenuPageDef *definition = page_definition(*page);
    if (definition->parent >= 0) {
        *page = definition->parent;
        *selected = definition->return_focus +
                    (definition->parent == PAGE_MAIN && has_apps ? 1 : 0);
    }
}

int menu_duration_index(int seconds)
{
    int i;
    for (i = 0; i < ARRAY_SIZE(duration_values); ++i)
        if (duration_values[i] == seconds)
            return i;
    return -1;
}

int menu_volume_index(int percent)
{
    if (percent <= 0)
        return 0;
    if (percent >= 100)
        return 10;
    return (percent + 5) / 10;
}

static void save(MenuState *state, MenuSaveSettings save_settings, void *save_context)
{
    if (save_settings && save_settings(state->slideshow_duration, state->button_reversed,
                                      state->auto_play, state->menu_3d, save_context) != 0)
        fprintf(stderr, "Unable to save settings: %s\n", strerror(errno));
}

static void select_settings_page(MenuState *state, const MenuItemRule *rule,
                                 MenuSaveSettings save_settings, void *save_context)
{
    int target_selected = rule->target_selected +
                          (rule->target_page == PAGE_MAIN && state->has_apps ? 1 : 0);
    if (state->page == PAGE_SETTINGS) {
        if (state->selected == 3)
            state->selected = menu_duration_index(state->slideshow_duration) + 1;
        else if (state->selected == 4)
            state->selected = state->button_reversed ? 2 : 1;
        else if (state->selected == 5)
            state->selected = state->menu_3d == MENU_3D_DEEP ? 3 :
                              state->menu_3d == MENU_3D_SUBTLE ? 2 : 1;
        else if (state->selected == 6)
            state->selected = state->auto_play ? 2 : 1;
        else if (state->selected == 8)
            state->selected = menu_volume_index(state->volume) + 1;
        else
            state->selected = target_selected;
    } else {
        if (state->selected > 0) {
            if (state->page == PAGE_SLIDESHOW_DURATION)
                state->slideshow_duration = duration_values[state->selected - 1];
            else if (state->page == PAGE_BUTTON_DIRECTION)
                state->button_reversed = state->selected == 2;
            else if (state->page == PAGE_3D_MENU)
                state->menu_3d = state->selected == 3 ? MENU_3D_DEEP :
                                 state->selected == 2 ? MENU_3D_SUBTLE : MENU_3D_OFF;
            else if (state->page == PAGE_AUTO_PLAY)
                state->auto_play = state->selected == 2;
            if (state->page != PAGE_CLEAR_CACHE && state->page != PAGE_VOLUME)
                save(state, save_settings, save_context);
        }
        state->selected = target_selected;
    }
}

MenuAction menu_handle_message(MenuState *state, const char *message, int *changed,
                               MenuSaveSettings save_settings, void *save_context)
{
    char command[16];
    const MenuPageDef *definition = page_definition(state->page);
    int item_count = 0;
    const MenuItemRule *rules = NULL;
    effective_page_info(state->page, state->has_apps, NULL, &item_count, &rules);
    if (sscanf(message, "{\"cmd\":\"%15[^\"]\"", command) != 1)
        return ACTION_NONE;
    if (strcmp(command, "SHUT") == 0)
        return ACTION_POWER_OFF;
    if (strcmp(command, "USBD") == 0)
        return ACTION_USB_DESKTOP;
    if (!state->button_reversed) {
        if (strcmp(command, "PREV") == 0)
            strcpy(command, "NEXT");
        else if (strcmp(command, "NEXT") == 0)
            strcpy(command, "PREV");
    }
    if (strcmp(command, "PREV") == 0) {
        if (state->selected + 1 < item_count) {
            ++state->selected;
            *changed = 1;
        } else if (state->page != PAGE_VOLUME) {
            state->selected = 0;
            *changed = 1;
        }
    } else if (strcmp(command, "NEXT") == 0) {
        if (state->selected > 0) {
            --state->selected;
            *changed = 1;
        } else if (state->page != PAGE_VOLUME) {
            state->selected = item_count - 1;
            *changed = 1;
        }
    } else if (strcmp(command, "PLPA") == 0) {
        const MenuItemRule *rule = &rules[state->selected];
        if (rule->action != ACTION_NONE)
            return rule->action;
        if (rule->target_page >= 0) {
            select_settings_page(state, rule, save_settings, save_context);
            state->page = rule->target_page;
            *changed = 1;
        } else if (state->page != PAGE_INFO) {
            *changed = 1;
        }
    } else if (strcmp(command, "LOOP") == 0 && definition->parent >= 0) {
        menu_return_to_parent(&state->page, &state->selected, state->has_apps);
        *changed = 1;
    } else {
        return ACTION_NONE;
    }
    printf("Button: %s, page=%d, selected=%d\n", command, state->page, state->selected);
    fflush(stdout);
    return ACTION_NONE;
}
