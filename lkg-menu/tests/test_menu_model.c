#include "test.h"
#include "menu_model.h"

static MenuAction command(MenuState *state, const char *name, int *changed)
{
    char message[64];
    snprintf(message, sizeof(message), "{\"cmd\":\"%s\"}", name);
    return menu_handle_message(state, message, changed, NULL, NULL);
}

static int save_calls;
static int save_settings(int seconds, int reversed, int auto_play, int menu_3d, void *context)
{
    (void)seconds;
    (void)reversed;
    (void)auto_play;
    (void)menu_3d;
    (void)context;
    ++save_calls;
    return 0;
}

int test_menu_model(void)
{
    int page;
    int changed = 0;
    MenuState state = { .page = PAGE_MAIN, .slideshow_duration = 5, .button_reversed = 1 };
    const int counts[] = { 6, 10, 2, 17, 3, 4, 3, 12, 11, 1 };
    for (page = PAGE_MAIN; page <= PAGE_FILE_MANAGER; ++page) {
        const char **items;
        int count;
        menu_items(page, 0, &items, &count);
        TEST_ASSERT(count == counts[page]);
        TEST_ASSERT(items[0] != NULL);
        state.page = page;
        state.selected = 0;
        TEST_ASSERT(command(&state, "PREV", &changed) == ACTION_NONE);
        TEST_ASSERT(state.selected == 1 % count);
        state.selected = 0;
        TEST_ASSERT(command(&state, "NEXT", &changed) == ACTION_NONE);
        TEST_ASSERT(state.selected == (page == PAGE_VOLUME ? 0 : count - 1));
    }
    state.page = PAGE_VOLUME;
    state.selected = counts[PAGE_VOLUME] - 1;
    changed = 0;
    TEST_ASSERT(command(&state, "PREV", &changed) == ACTION_NONE);
    TEST_ASSERT(state.selected == counts[PAGE_VOLUME] - 1 && changed == 0);
    TEST_ASSERT(menu_duration_index(5) == 4);
    TEST_ASSERT(menu_duration_index(3600) == 15);
    TEST_ASSERT(menu_duration_index(11) == -1);
    TEST_ASSERT(menu_volume_index(0) == 0);
    TEST_ASSERT(menu_volume_index(40) == 4);
    TEST_ASSERT(menu_volume_index(96) == 10);
    TEST_ASSERT(menu_volume_index(100) == 10);

    state.page = PAGE_MAIN;
    state.selected = 3;
    TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_NONE);
    TEST_ASSERT(state.page == PAGE_SETTINGS && state.selected == 0);
    TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_NONE);
    TEST_ASSERT(state.page == PAGE_MAIN && state.selected == 3);
    state.selected = 1;
    TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_NONE);
    TEST_ASSERT(state.page == PAGE_FILE_MANAGER && state.selected == 0);
    TEST_ASSERT(command(&state, "LOOP", &changed) == ACTION_NONE);
    TEST_ASSERT(state.page == PAGE_MAIN && state.selected == 1);

    {
        const int pages[] = {
            PAGE_CLEAR_CACHE, PAGE_SLIDESHOW_DURATION, PAGE_BUTTON_DIRECTION,
            PAGE_3D_MENU, PAGE_AUTO_PLAY, PAGE_VOLUME, PAGE_INFO
        };
        const int settings_selection[] = { 2, 3, 4, 5, 6, 8, 9 };
        const int return_selection[] = { 2, 3, 4, 5, 6, 8, 9 };
        int i;
        for (i = 0; i < 7; ++i) {
            state.page = PAGE_SETTINGS;
            state.selected = settings_selection[i];
            TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_NONE);
            TEST_ASSERT(state.page == pages[i]);
            TEST_ASSERT(command(&state, "LOOP", &changed) == ACTION_NONE);
            TEST_ASSERT(state.page == PAGE_SETTINGS);
            TEST_ASSERT(state.selected == return_selection[i]);
        }
    }

    state.page = PAGE_SETTINGS;
    state.selected = 8;
    state.volume = 40;
    TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_NONE);
    TEST_ASSERT(state.page == PAGE_VOLUME && state.selected == 5);
    state.selected = 6;
    save_calls = 0;
    TEST_ASSERT(menu_handle_message(&state, "{\"cmd\":\"PLPA\"}", &changed,
                                    save_settings, NULL) == ACTION_NONE);
    TEST_ASSERT(state.page == PAGE_SETTINGS && state.selected == 8);
    TEST_ASSERT(save_calls == 0);

    state.page = PAGE_SETTINGS;
    state.selected = 7;
    TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_WIFI);
    TEST_ASSERT(state.page == PAGE_SETTINGS && state.selected == 7);
    state.page = PAGE_SETTINGS;
    state.selected = 1;
    TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_PRERENDER);
    TEST_ASSERT(state.page == PAGE_SETTINGS && state.selected == 1);
    state.page = PAGE_CLEAR_CACHE;
    state.selected = 1;
    TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_CLEAR_CACHE);
    menu_return_to_parent(&state.page, &state.selected, 0);
    TEST_ASSERT(state.page == PAGE_SETTINGS && state.selected == 2);
    state.page = PAGE_MAIN;
    state.selected = 0;
    TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_GALLERY);
    TEST_ASSERT(state.page == PAGE_MAIN && state.selected == 0);
    state.selected = 2;
    TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_SWAY);
    state.selected = 4;
    TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_TTY);
    state.selected = 5;
    TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_POWER_OFF);
    state.selected = 0;
    TEST_ASSERT(command(&state, "SHUT", &changed) == ACTION_POWER_OFF);
    TEST_ASSERT(command(&state, "USBD", &changed) == ACTION_USB_DESKTOP);

    state.page = PAGE_SLIDESHOW_DURATION;
    state.selected = 6;
    save_calls = 0;
    TEST_ASSERT(menu_handle_message(&state, "{\"cmd\":\"PLPA\"}", &changed,
                                    save_settings, NULL) == ACTION_NONE);
    TEST_ASSERT(state.slideshow_duration == 6);
    TEST_ASSERT(state.page == PAGE_SETTINGS && state.selected == 3);
    TEST_ASSERT(save_calls == 1);

    state.page = PAGE_MAIN;
    state.selected = 0;
    state.button_reversed = 0;
    TEST_ASSERT(command(&state, "PREV", &changed) == ACTION_NONE);
    TEST_ASSERT(state.selected == 5);
    changed = 0;
    TEST_ASSERT(menu_handle_message(&state, "invalid", &changed, NULL, NULL) == ACTION_NONE);
    TEST_ASSERT(changed == 0);

    {
        const char **items = NULL;
        int count = 0;
        menu_items(PAGE_MAIN, 1, &items, &count);
        TEST_ASSERT(count == 7);
        TEST_STRING(items[0], "Apps");
        TEST_STRING(items[1], "Gallery");
        TEST_STRING(items[6], "Power Off");

        state.has_apps = 1;
        state.button_reversed = 1;
        state.page = PAGE_MAIN;
        state.selected = 0;
        TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_APPS);
        state.selected = 1;
        TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_GALLERY);
        state.selected = 2;
        TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_NONE);
        TEST_ASSERT(state.page == PAGE_FILE_MANAGER && state.selected == 0);
        TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_NONE);
        TEST_ASSERT(state.page == PAGE_MAIN && state.selected == 2);
        state.selected = 4;
        TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_NONE);
        TEST_ASSERT(state.page == PAGE_SETTINGS && state.selected == 0);
        TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_NONE);
        TEST_ASSERT(state.page == PAGE_MAIN && state.selected == 4);
        TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_NONE);
        TEST_ASSERT(state.page == PAGE_SETTINGS && state.selected == 0);
        TEST_ASSERT(command(&state, "LOOP", &changed) == ACTION_NONE);
        TEST_ASSERT(state.page == PAGE_MAIN && state.selected == 4);
        state.selected = 0;
        TEST_ASSERT(command(&state, "NEXT", &changed) == ACTION_NONE);
        TEST_ASSERT(state.selected == 6);
        TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_POWER_OFF);
    }

    {
        const char **items = NULL;
        int count = 0;
        menu_items(PAGE_3D_MENU, 0, &items, &count);
        TEST_ASSERT(count == 4);
        TEST_STRING(items[0], "Back");
        TEST_STRING(items[1], "Off");
        TEST_STRING(items[2], "Subtle");
        TEST_STRING(items[3], "Deep");

        state.has_apps = 0;
        state.button_reversed = 1;
        state.menu_3d = MENU_3D_SUBTLE;
        state.page = PAGE_SETTINGS;
        state.selected = 5;
        TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_NONE);
        TEST_ASSERT(state.page == PAGE_3D_MENU && state.selected == 2);
        state.selected = 3;
        save_calls = 0;
        TEST_ASSERT(menu_handle_message(&state, "{\"cmd\":\"PLPA\"}", &changed,
                                        save_settings, NULL) == ACTION_NONE);
        TEST_ASSERT(state.menu_3d == MENU_3D_DEEP);
        TEST_ASSERT(state.page == PAGE_SETTINGS && state.selected == 5);
        TEST_ASSERT(save_calls == 1);

        TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_NONE);
        TEST_ASSERT(state.page == PAGE_3D_MENU && state.selected == 3);
        state.selected = 1;
        TEST_ASSERT(menu_handle_message(&state, "{\"cmd\":\"PLPA\"}", &changed,
                                        save_settings, NULL) == ACTION_NONE);
        TEST_ASSERT(state.menu_3d == MENU_3D_OFF);
        TEST_ASSERT(save_calls == 2);

        TEST_ASSERT(command(&state, "PLPA", &changed) == ACTION_NONE);
        TEST_ASSERT(state.page == PAGE_3D_MENU && state.selected == 1);
        state.selected = 2;
        TEST_ASSERT(menu_handle_message(&state, "{\"cmd\":\"PLPA\"}", &changed,
                                        save_settings, NULL) == ACTION_NONE);
        TEST_ASSERT(state.menu_3d == MENU_3D_SUBTLE);
        TEST_ASSERT(save_calls == 3);
    }
    return 0;
}
