#include "wifi.h"
#include "depth_frame.h"
#include "password_keyboard.h"
#include "usb_keyboard.h"
#include "serial_parser.h"
#include "wifi_control.h"
#include "wifi_model.h"
#include "wifi_ui.h"
#include "wifi_view.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int start_scan(WifiMenu *menu)
{
    if (menu->connection.pid > 0)
        return -1;
    if (wifi_control_start_scan(&menu->scan) != 0)
        return -1;
    if (menu->selected == 1) {
        menu->selected = 0;
        menu->scroll = 0;
    }
    fprintf(stderr, "Scanning for networks...\n");
    return 0;
}

static int start_operation(WifiMenu *menu, WifiOperation operation, int saved)
{
    wifi_process_stop(&menu->scan);
    menu->operation = operation;
    menu->failure_until = 0;
    if (wifi_control_start_operation(&menu->connection, &menu->model, operation, saved,
                                     menu->password, menu->password_length) != 0)
        return -1;
    fprintf(stderr, "%s\n",
             operation == WIFI_OPERATION_DISCONNECT ? "Disconnecting..." :
             operation == WIFI_OPERATION_FORGET ? "Forgetting network..." : "Connecting...");
    return 0;
}

static int network_is_saved(const WifiMenu *menu, const WifiNetwork *network)
{
    return wifi_model_is_saved(&menu->model, network);
}

static void parse_scan_results(WifiMenu *menu)
{
    wifi_model_parse_scan(&menu->model, menu->scan.output);
}

static void update_scroll(WifiMenu *menu)
{
    if (menu->scroll < 0 || menu->scroll >= menu->model.network_count + 1)
        menu->scroll = 0;
    if (menu->selected > 0)
        menu->scroll = wifi_view_networks_page_start(menu->selected);
}

static void sort_networks_preserving_selection(WifiMenu *menu)
{
    char selected_ssid[33];
    size_t selected_length = 0;
    int track_selection = menu->page == WIFI_PAGE_NETWORKS &&
                          menu->selected >= 2 &&
                          menu->selected - 2 < menu->model.network_count;
    if (track_selection) {
        const WifiNetwork *current = &menu->model.networks[menu->selected - 2];
        selected_length = current->ssid_length;
        memcpy(selected_ssid, current->ssid, selected_length);
    }
    wifi_model_sort_networks(&menu->model);
    if (track_selection) {
        int i;
        for (i = 0; i < menu->model.network_count; ++i) {
            if (menu->model.networks[i].ssid_length == selected_length &&
                memcmp(menu->model.networks[i].ssid, selected_ssid, selected_length) == 0) {
                menu->selected = i + 2;
                update_scroll(menu);
                break;
            }
        }
    }
}

static int handle_network_command(WifiMenu *menu, const char *command)
{
    int count = menu->model.network_count + 2;
    if (menu->connection.pid > 0)
        return 0;
    if (strcmp(command, "PREV") == 0) {
        do {
            if (menu->selected == 0)
                menu->selected = menu->scroll + 1;
            else
                menu->selected = (menu->selected + 1) % count;
        } while (menu->scan.pid > 0 && menu->selected == 1);
        update_scroll(menu);
        return 1;
    }
    if (strcmp(command, "NEXT") == 0) {
        do {
            if (menu->selected == 0)
                menu->selected = menu->scroll > 0 ? menu->scroll : count - 1;
            else if (menu->selected == menu->scroll + 1)
                menu->selected = 0;
            else
                menu->selected = (menu->selected + count - 1) % count;
        } while (menu->scan.pid > 0 && menu->selected == 1);
        update_scroll(menu);
        return 1;
    }
    if (strcmp(command, "LOOP") == 0)
        return -1;
    if (strcmp(command, "PLPA") != 0)
        return 0;
    if (menu->selected == 0)
        return -1;
    if (menu->selected == 1) {
        if (menu->scan.pid > 0)
            return 1;
        if (start_scan(menu) != 0)
            fprintf(stderr, "Unable to start scan: %s\n", strerror(errno));
        return 1;
    }
    menu->model.selected_network = menu->model.networks[menu->selected - 2];
    if (wifi_menu_network_is_current(menu, &menu->model.selected_network) ||
        network_is_saved(menu, &menu->model.selected_network)) {
        menu->page = WIFI_PAGE_DETAILS;
        menu->selected = 0;
        (void)0;
        return 1;
    }
    if (menu->model.selected_network.enterprise) {
        fprintf(stderr, "Enterprise networks are not supported.\n");
        return 1;
    }
    if (menu->model.selected_network.wep) {
        fprintf(stderr, "WEP networks are not supported.\n");
        return 1;
    }
    if (menu->model.selected_network.secured) {
        menu->page = WIFI_PAGE_PASSWORD;
        menu->password_length = 0;
        menu->password[0] = '\0';
        menu->keyboard_layout = 0;
        menu->key_selected = 25;
        menu->back_selected = 1;
        fprintf(stderr, "Use Play to enter a character; select Connect when finished.\n");
    } else {
        menu->password_length = 0;
        menu->password[0] = '\0';
        if (start_operation(menu, WIFI_OPERATION_CONNECT, 0) != 0)
            fprintf(stderr, "Unable to start connection: %s\n", strerror(errno));
    }
    return 1;
}

static int handle_details_command(WifiMenu *menu, const char *command)
{
    int connected = wifi_menu_network_is_current(menu, &menu->model.selected_network);
    int count = 3 + (connected ? 9 : 1);
    WifiOperation operation;
    if (menu->connection.pid > 0)
        return 0;
    if (strcmp(command, "PREV") == 0) {
        menu->selected = (menu->selected + 1) % count;
        return 1;
    }
    if (strcmp(command, "NEXT") == 0) {
        menu->selected = (menu->selected + count - 1) % count;
        return 1;
    }
    if (strcmp(command, "LOOP") == 0) {
        menu->page = WIFI_PAGE_NETWORKS;
        menu->selected = 0;
        menu->scroll = 0;
        (void)0;
        return 1;
    }
    if (strcmp(command, "PLPA") != 0)
        return 0;
    if (menu->selected == 0) {
        menu->page = WIFI_PAGE_NETWORKS;
        menu->selected = 0;
        menu->scroll = 0;
        (void)0;
        return 1;
    }
    if (menu->selected >= 3)
        return 0;
    operation = menu->selected == 2 ? WIFI_OPERATION_FORGET :
                connected ? WIFI_OPERATION_DISCONNECT : WIFI_OPERATION_CONNECT;
    if (start_operation(menu, operation, 1) != 0)
        fprintf(stderr, "Unable to start operation: %s\n", strerror(errno));
    return 1;
}

static int handle_password_command(WifiMenu *menu, const char *command);

static void submit_password(WifiMenu *menu)
{
    if (!password_keyboard_valid(menu->password, menu->password_length)) {
        fprintf(stderr, "Use 8-63 printable characters, or exactly 64 hex digits.\n");
        snprintf(menu->connection.output, sizeof(menu->connection.output),
                 "Use 8-63 printable characters, or exactly 64 hex digits.");
        menu->failure_until = wifi_ui_monotonic_seconds() + 1.0;
    } else if (start_operation(menu, WIFI_OPERATION_CONNECT, 0) != 0) {
        fprintf(stderr, "Unable to start connection: %s\n", strerror(errno));
        snprintf(menu->connection.output, sizeof(menu->connection.output),
                 "Unable to start connection: %s", strerror(errno));
        menu->failure_until = wifi_ui_monotonic_seconds() + 1.0;
    }
}

static int handle_usb_key(WifiMenu *menu, UsbKeyEvent event)
{
    if (menu->page != WIFI_PAGE_PASSWORD || menu->connection.pid > 0 || menu->failure_until)
        return 0;
    if (event.type == USB_KEY_PREV || event.type == USB_KEY_NEXT) {
        const char *command = event.type == USB_KEY_PREV ? "NEXT" : "PREV";
        int previous_back = menu->back_selected;
        int action = handle_password_command(menu, command);
        PasswordKeyboardState state = {
            .layout = menu->keyboard_layout,
            .key_selected = menu->key_selected,
            .back_selected = menu->back_selected
        };
        return action ? password_keyboard_redraw(command, previous_back, &state, 0) : 0;
    }
    if (event.type == USB_KEY_CHARACTER) {
        if (menu->password_length < sizeof(menu->password) - 1) {
            menu->password[menu->password_length++] = event.character;
            menu->password[menu->password_length] = '\0';
        }
        return PASSWORD_REDRAW_PASSWORD;
    }
    if (event.type == USB_KEY_BACKSPACE) {
        if (menu->password_length)
            --menu->password_length;
        menu->password[menu->password_length] = '\0';
        return PASSWORD_REDRAW_PASSWORD;
    }
    if (event.type == USB_KEY_ENTER) {
        submit_password(menu);
        return PASSWORD_REDRAW_FULL;
    }
    if (event.type == USB_KEY_ESCAPE) {
        int action = handle_password_command(menu, "LOOP");
        return action ? PASSWORD_REDRAW_FULL : 0;
    }
    return 0;
}

static int handle_password_command(WifiMenu *menu, const char *command)
{
    PasswordKeyboardState state = {
        .layout = menu->keyboard_layout,
        .key_selected = menu->key_selected,
        .back_selected = menu->back_selected,
        .password_length = menu->password_length
    };
    PasswordKeyboardAction action;
    if (menu->connection.pid > 0)
        return 0;
    memcpy(state.password, menu->password, sizeof(state.password));
    action = password_keyboard_handle(&state, command);
    menu->keyboard_layout = state.layout;
    menu->key_selected = state.key_selected;
    menu->back_selected = state.back_selected;
    menu->password_length = state.password_length;
    memcpy(menu->password, state.password, sizeof(menu->password));
    if (action == PASSWORD_KEYBOARD_BACK) {
        menu->page = WIFI_PAGE_NETWORKS;
        (void)0;
    } else if (action == PASSWORD_KEYBOARD_TOO_LONG) {
        fprintf(stderr, "Password is too long.\n");
    } else if (action == PASSWORD_KEYBOARD_CONNECT) {
        submit_password(menu);
    }
    return action != PASSWORD_KEYBOARD_NONE;
}

int wifi_run(Display *display, Font *font, const DepthEffect *depth,
             DepthLayer layers[2], int serial_fd, UsbKeyboard *keyboard, int button_reversed)
{
    WifiMenu menu;
    SerialParser parser;
    int changed = PASSWORD_REDRAW_FULL;
    int cursor_previous = 0;
    int cursor_previous_back = 0;
    int result = 0;
    memset(&menu, 0, sizeof(menu));
    memset(&parser, 0, sizeof(parser));
    wifi_process_initialize(&menu.scan);
    wifi_process_initialize(&menu.connection);
    wifi_process_initialize(&menu.update);
    menu.display = display;
    menu.font = font;
    menu.depth = depth;
    menu.layers = layers;
    menu.page = WIFI_PAGE_NETWORKS;
    if (wifi_control_start_update(&menu.update, &menu.model) != 0)
        fprintf(stderr, "Unable to start Wi-Fi status update\n");
    if (start_scan(&menu) != 0)
        fprintf(stderr, "Unable to start scan: %s\n", strerror(errno));
    while (running) {
        char command[16];
        int event;
        if (menu.failure_until && wifi_ui_monotonic_seconds() >= menu.failure_until) {
            menu.failure_until = 0;
            changed = PASSWORD_REDRAW_FULL;
        }
        if (wifi_process_poll(&menu.update)) {
            size_t offset = offsetof(WifiModel, current_ssid);
            if (menu.update.length == sizeof(WifiModel)) {
                memcpy((char *)&menu.model + offset, menu.update.output + offset,
                       sizeof(WifiModel) - offset);
                sort_networks_preserving_selection(&menu);
            }
            changed = PASSWORD_REDRAW_FULL;
        }
        if (wifi_process_poll(&menu.scan)) {
            if (menu.scan.status == 0) {
                parse_scan_results(&menu);
                printf("Wi-Fi scan completed: %d networks\n", menu.model.network_count);
                if (menu.page == WIFI_PAGE_NETWORKS) {
                    if (menu.model.network_count)
                        fprintf(stderr, "%d networks found. Select one to connect.\n", menu.model.network_count);
                    else
                        fprintf(stderr, "No networks found. Select Scan again to retry.\n");
                    if (menu.selected >= menu.model.network_count + 2)
                        menu.selected = 0;
                    update_scroll(&menu);
                }
            } else {
                fprintf(stderr, "Wi-Fi scan failed with status %d: %.512s\n",
                        menu.scan.status, menu.scan.output);
                if (menu.page == WIFI_PAGE_NETWORKS)
                    fprintf(stderr, "Scan failed. Select Scan again to retry.\n");
            }
            fflush(stdout);
            if (wifi_control_start_update(&menu.update, &menu.model) != 0)
                    fprintf(stderr, "Unable to refresh Wi-Fi status\n");
            changed = PASSWORD_REDRAW_FULL;
        }
        if (wifi_process_poll(&menu.connection)) {
            if (menu.connection.status == 0) {
                memset(menu.password, 0, sizeof(menu.password));
                menu.password_length = 0;
                menu.failure_until = 0;
                menu.page = WIFI_PAGE_NETWORKS;
                menu.selected = 0;
                menu.scroll = 0;
                if (wifi_control_start_update(&menu.update, &menu.model) != 0)
                    fprintf(stderr, "Unable to refresh Wi-Fi status\n");
                fprintf(stderr, "%s\n",
                         menu.operation == WIFI_OPERATION_DISCONNECT ? "Disconnected." :
                         menu.operation == WIFI_OPERATION_FORGET ? "Network forgotten." :
                         "Connected and saved for future boots.");
            } else {
                char *newline = strchr(menu.connection.output, '\n');
                if (newline)
                    *newline = '\0';
                if (menu.connection.output[0])
                    fprintf(stderr, "%.255s\n", menu.connection.output);
                else
                    fprintf(stderr, "Operation failed; the previous network was restored.\n");
                if (wifi_control_start_update(&menu.update, &menu.model) != 0)
                    fprintf(stderr, "Unable to refresh Wi-Fi status\n");
                if (menu.operation == WIFI_OPERATION_CONNECT)
                    menu.failure_until = wifi_ui_monotonic_seconds() + 1.0;
            }
            changed = PASSWORD_REDRAW_FULL;
        }
        if (changed) {
            int draw_result;
            if (menu.connection.pid > 0 || menu.failure_until)
                draw_result = wifi_ui_draw_operation(&menu);
            else if (menu.page != WIFI_PAGE_PASSWORD)
                draw_result = wifi_ui_draw_current_page(&menu);
            else if (changed == PASSWORD_REDRAW_FOCUS)
                draw_result = wifi_ui_draw_password_cursor(&menu, cursor_previous, 0);
            else if (changed == PASSWORD_REDRAW_PASSWORD)
                draw_result = wifi_ui_update_password_field(&menu);
            else if (changed == PASSWORD_REDRAW_KEYBOARD)
                draw_result = wifi_ui_update_password_keyboard(&menu);
            else if (changed == PASSWORD_REDRAW_BACK)
                draw_result = wifi_ui_draw_password_cursor(&menu, cursor_previous, 1);
            else
                draw_result = wifi_ui_draw_password(&menu);
            if (draw_result != 0) {
                result = -1;
                break;
            }
            changed = 0;
        }
        if (menu.page == WIFI_PAGE_PASSWORD) {
            UsbKeyEvent key_event = usb_keyboard_next(keyboard);
            if (key_event.type != USB_KEY_NONE) {
                cursor_previous = menu.key_selected;
                cursor_previous_back = menu.back_selected;
                changed = handle_usb_key(&menu, key_event);
                if (changed)
                    continue;
            }
            event = 0;
        } else {
            event = usb_keyboard_next_command(keyboard, command, sizeof(command));
            if (event > 0)
                usb_keyboard_prepare_command(command, button_reversed);
        }
        if (event == 0)
            event = serial_parser_next_with_fd(serial_fd, &parser, command, sizeof(command),
                                               100, keyboard->fd);
        if (event < 0) {
            result = -1;
            break;
        }
        if (!event)
            continue;
        normalize_button_command(command, button_reversed);
        if (strcmp(command, "SHUT") == 0) {
            result = SERIAL_RESULT_POWER_OFF;
            break;
        }
        if (strcmp(command, "USBD") == 0) {
            result = SERIAL_RESULT_USB_DESKTOP;
            break;
        }
        if (menu.failure_until)
            continue;
        cursor_previous = menu.key_selected;
        cursor_previous_back = menu.back_selected;
        if (menu.page == WIFI_PAGE_NETWORKS)
            event = handle_network_command(&menu, command);
        else if (menu.page == WIFI_PAGE_DETAILS)
            event = handle_details_command(&menu, command);
        else
            event = handle_password_command(&menu, command);
        printf("Wi-Fi button: %s, page=%d, selected=%d, back=%d, password_length=%zu\n",
               command, menu.page,
               menu.page == WIFI_PAGE_PASSWORD ? menu.key_selected : menu.selected,
               menu.back_selected, menu.password_length);
        fflush(stdout);
        if (event < 0)
            break;
        if (event > 0) {
            changed = PASSWORD_REDRAW_FULL;
            if (menu.page == WIFI_PAGE_PASSWORD) {
                PasswordKeyboardState state = {
                    .layout = menu.keyboard_layout,
                    .key_selected = menu.key_selected,
                    .back_selected = menu.back_selected
                };
                changed = password_keyboard_redraw(command, cursor_previous_back, &state,
                                                   menu.connection.pid > 0 || menu.failure_until);
            }
        }
    }
    wifi_process_stop(&menu.scan);
    wifi_process_stop(&menu.connection);
    wifi_process_stop(&menu.update);
    memset(menu.password, 0, sizeof(menu.password));
    FT_Set_Pixel_Sizes(font->face, 0, 64);
    return result;
}
