#include "wifi_control.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define WIFI_INTERFACE "wlan0"

void wifi_process_initialize(WifiProcess *process)
{
    memset(process, 0, sizeof(*process));
    process->pid = -1;
    process->output_fd = -1;
}

static int write_all(int fd, const void *data, size_t size)
{
    const char *bytes = data;
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = write(fd, bytes + offset, size - offset);
        if (count > 0) {
            offset += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int run_wpa_command(const char *command, char *output, size_t output_size)
{
    const char *control_path = "/run/wpa_supplicant/" WIFI_INTERFACE;
    struct sockaddr_un local;
    struct sockaddr_un remote;
    struct pollfd pfd;
    char request[32];
    ssize_t count;
    size_t i;
    int fd;
    int result = -1;
    int saved_errno;
    if (output_size < 2 || strlen(command) >= sizeof(request))
        return -1;
    output[0] = '\0';
    for (i = 0; command[i]; ++i)
        request[i] = (char)toupper((unsigned char)command[i]);
    request[i] = '\0';
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        goto fail;
    memset(&local, 0, sizeof(local));
    memset(&remote, 0, sizeof(remote));
    local.sun_family = AF_UNIX;
    remote.sun_family = AF_UNIX;
    snprintf(local.sun_path, sizeof(local.sun_path), "/run/user/1000/lkg-menu-wifi-%ld", (long)getpid());
    snprintf(remote.sun_path, sizeof(remote.sun_path), "%s", control_path);
    unlink(local.sun_path);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0 ||
        connect(fd, (struct sockaddr *)&remote, sizeof(remote)) != 0)
        goto fail;
    count = send(fd, request, strlen(request), 0);
    if (count != (ssize_t)strlen(request))
        goto fail;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    for (;;) {
        result = poll(&pfd, 1, 15000);
        if (result < 0 && errno == EINTR)
            continue;
        break;
    }
    if (result <= 0) {
        if (!result)
            errno = ETIMEDOUT;
        goto fail;
    }
    count = recv(fd, output, output_size - 1, 0);
    if (count < 0)
        goto fail;
    output[count] = '\0';
    result = 0;
fail:
    saved_errno = errno;
    if (fd >= 0) {
        close(fd);
        unlink(local.sun_path);
    }
    if (result != 0)
        snprintf(output, output_size, "%s", strerror(saved_errno));
    errno = saved_errno;
    return result;
}

static int open_wpa_event_socket(char local_path[108])
{
    struct sockaddr_un local;
    struct sockaddr_un remote;
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    memset(&local, 0, sizeof(local));
    memset(&remote, 0, sizeof(remote));
    local.sun_family = AF_UNIX;
    remote.sun_family = AF_UNIX;
    snprintf(local.sun_path, sizeof(local.sun_path), "/run/user/1000/lkg-menu-wifi-events-%ld", (long)getpid());
    snprintf(remote.sun_path, sizeof(remote.sun_path), "/run/wpa_supplicant/" WIFI_INTERFACE);
    unlink(local.sun_path);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0 ||
        connect(fd, (struct sockaddr *)&remote, sizeof(remote)) != 0) {
        close(fd);
        unlink(local.sun_path);
        return -1;
    }
    snprintf(local_path, 108, "%s", local.sun_path);
    return fd;
}

static int receive_wpa_message(int fd, char *output, size_t output_size, int timeout)
{
    struct pollfd pfd;
    ssize_t count;
    int result;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    do {
        result = poll(&pfd, 1, timeout);
    } while (result < 0 && errno == EINTR);
    if (result <= 0)
        return -1;
    count = recv(fd, output, output_size - 1, 0);
    if (count < 0)
        return -1;
    output[count] = '\0';
    return 0;
}

static int attached_wpa_command(int fd, const char *command)
{
    char output[512];
    int attempt;
    if (send(fd, command, strlen(command), 0) != (ssize_t)strlen(command))
        return -1;
    for (attempt = 0; attempt < 16; ++attempt) {
        if (receive_wpa_message(fd, output, sizeof(output), 15000) != 0)
            return -1;
        if (strncmp(output, "OK", 2) == 0)
            return 0;
        if (strncmp(output, "FAIL", 4) == 0 || strncmp(output, "UNKNOWN", 7) == 0)
            return -1;
    }
    return -1;
}

static int perform_wpa_scan(char *output, size_t output_size)
{
    char local_path[108];
    int fd = open_wpa_event_socket(local_path);
    int result = -1;
    if (fd < 0) {
        snprintf(output, output_size, "Unable to open scan event socket: %s", strerror(errno));
        return -1;
    }
    if (attached_wpa_command(fd, "ATTACH") != 0 ||
        attached_wpa_command(fd, "SCAN") != 0) {
        snprintf(output, output_size, "Unable to start scan");
        goto done;
    }
    for (;;) {
        if (receive_wpa_message(fd, output, output_size, 15000) != 0) {
            snprintf(output, output_size, "Scan completion event timed out");
            goto done;
        }
        if (strstr(output, "CTRL-EVENT-SCAN-RESULTS")) {
            result = run_wpa_command("scan_results", output, output_size);
            goto done;
        }
        if (strstr(output, "CTRL-EVENT-SCAN-FAILED")) {
            snprintf(output, output_size, "Scan failed in wpa_supplicant");
            goto done;
        }
    }
done:
    close(fd);
    unlink(local_path);
    return result;
}

void wifi_process_stop(WifiProcess *process)
{
    int status;
    if (process->output_fd >= 0) {
        close(process->output_fd);
        process->output_fd = -1;
    }
    if (process->pid > 0) {
        kill(process->pid, SIGTERM);
        while (waitpid(process->pid, &status, 0) < 0 && errno == EINTR)
            ;
    }
    process->pid = -1;
}

int wifi_process_poll(WifiProcess *process)
{
    int status;
    pid_t result;
    if (process->pid <= 0)
        return 0;
    for (;;) {
        char input[4096];
        ssize_t count = read(process->output_fd, input, sizeof(input));
        if (count > 0) {
            if (process->length < sizeof(process->output) - 1) {
                size_t available = sizeof(process->output) - 1 - process->length;
                size_t copy = (size_t)count < available ? (size_t)count : available;
                memcpy(process->output + process->length, input, copy);
                process->length += copy;
                process->output[process->length] = '\0';
            }
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    result = waitpid(process->pid, &status, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR))
        return 0;
    if (result == process->pid)
        process->status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    else
        process->status = -1;
    for (;;) {
        char input[4096];
        ssize_t count = read(process->output_fd, input, sizeof(input));
        if (count > 0) {
            if (process->length < sizeof(process->output) - 1) {
                size_t available = sizeof(process->output) - 1 - process->length;
                size_t copy = (size_t)count < available ? (size_t)count : available;
                memcpy(process->output + process->length, input, copy);
                process->length += copy;
                process->output[process->length] = '\0';
            }
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    close(process->output_fd);
    process->output_fd = -1;
    process->pid = -1;
    return 1;
}

int wifi_control_start_scan(WifiProcess *process)
{
    int descriptors[2];
    pid_t pid;
    if (process->pid > 0)
        return -1;
    wifi_process_stop(process);
    process->length = 0;
    process->output[0] = '\0';
    if (pipe(descriptors) != 0)
        return -1;
    fcntl(descriptors[0], F_SETFD, FD_CLOEXEC);
    fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
    pid = fork();
    if (pid < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    if (pid == 0) {
        char output[WIFI_OUTPUT_SIZE];
        int result = perform_wpa_scan(output, sizeof(output));
        if (result != 0) {
            dprintf(descriptors[1], "%s\n", output);
            close(descriptors[1]);
            _exit(1);
        }
        write_all(descriptors[1], output, strlen(output));
        close(descriptors[1]);
        _exit(0);
    }
    close(descriptors[1]);
    fcntl(descriptors[0], F_SETFL, fcntl(descriptors[0], F_GETFL) | O_NONBLOCK);
    process->pid = pid;
    process->output_fd = descriptors[0];
    return 0;
}

int wifi_control_start_operation(WifiProcess *process, WifiModel *model,
                                 WifiOperation operation, int saved,
                                 const char *password, size_t password_length)
{
    int input_pipe[2];
    int output_pipe[2];
    char ssid_hex[65];
    const char *action = operation == WIFI_OPERATION_DISCONNECT ? "disconnect" :
                         operation == WIFI_OPERATION_FORGET ? "forget" :
                         saved ? "connect-saved" : "connect";
    size_t i;
    pid_t pid;
    if (process->pid > 0)
        return -1;
    wifi_process_stop(process);
    process->length = 0;
    process->output[0] = '\0';
    for (i = 0; i < model->selected_network.ssid_length; ++i)
        snprintf(ssid_hex + i * 2, 3, "%02x", (unsigned char)model->selected_network.ssid[i]);
    ssid_hex[model->selected_network.ssid_length * 2] = '\0';
    if (pipe(input_pipe) != 0)
        return -1;
    if (pipe(output_pipe) != 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        return -1;
    }
    fcntl(input_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(input_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(output_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(output_pipe[1], F_SETFD, FD_CLOEXEC);
    pid = fork();
    if (pid < 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        close(input_pipe[1]);
        close(output_pipe[0]);
        dup2(input_pipe[0], STDIN_FILENO);
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        close(input_pipe[0]);
        close(output_pipe[1]);
        if (operation == WIFI_OPERATION_DISCONNECT)
            execl("/usr/bin/sudo", "sudo", "-n", "/usr/local/sbin/lkg-wifi",
                  action, (char *)NULL);
        else
            execl("/usr/bin/sudo", "sudo", "-n", "/usr/local/sbin/lkg-wifi",
                  action, ssid_hex, (char *)NULL);
        _exit(127);
    }
    close(input_pipe[0]);
    close(output_pipe[1]);
    if (operation == WIFI_OPERATION_CONNECT && !saved &&
        (write_all(input_pipe[1], password, password_length) != 0 ||
         write_all(input_pipe[1], "\n", 1) != 0)) {
        close(input_pipe[1]);
        close(output_pipe[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -1;
    }
    close(input_pipe[1]);
    fcntl(output_pipe[0], F_SETFL, fcntl(output_pipe[0], F_GETFL) | O_NONBLOCK);
    process->pid = pid;
    process->output_fd = output_pipe[0];
    return 0;
}

int wifi_control_start_update(WifiProcess *process, const WifiModel *model)
{
    int descriptors[2];
    pid_t pid;
    wifi_process_stop(process);
    process->length = 0;
    process->output[0] = '\0';
    if (pipe(descriptors) != 0)
        return -1;
    fcntl(descriptors[0], F_SETFD, FD_CLOEXEC);
    fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
    pid = fork();
    if (pid < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    if (pid == 0) {
        WifiModel updated = *model;
        int saved_result;
        wifi_control_update_current(&updated);
        saved_result = wifi_control_update_saved(&updated);
        if (write_all(descriptors[1], &updated, sizeof(updated)) != 0)
            saved_result = -1;
        close(descriptors[0]);
        close(descriptors[1]);
        _exit(saved_result == 0 ? 0 : 1);
    }
    close(descriptors[1]);
    fcntl(descriptors[0], F_SETFL, fcntl(descriptors[0], F_GETFL) | O_NONBLOCK);
    process->pid = pid;
    process->output_fd = descriptors[0];
    return 0;
}

int wifi_control_update_saved(WifiModel *model)
{
    FILE *input;
    char line[128];
    int status;
    model->saved_network_count = 0;
    input = popen("/usr/bin/sudo -n /usr/local/sbin/lkg-wifi list", "r");
    if (!input)
        return -1;
    while (fgets(line, sizeof(line), input)) {
        size_t length = strcspn(line, "\r\n");
        size_t bytes = length / 2;
        size_t i;
        if (!length || length > 64 || length % 2 || model->saved_network_count >= WIFI_MAX_SAVED_NETWORKS)
            continue;
        for (i = 0; i < length; ++i)
            if (wifi_model_hex_value(line[i]) < 0)
                break;
        if (i != length)
            continue;
        for (i = 0; i < bytes; ++i)
            model->saved_ssids[model->saved_network_count][i] =
                (char)((wifi_model_hex_value(line[i * 2]) << 4) | wifi_model_hex_value(line[i * 2 + 1]));
        model->saved_ssids[model->saved_network_count][bytes] = '\0';
        model->saved_ssid_lengths[model->saved_network_count] = bytes;
        ++model->saved_network_count;
    }
    status = pclose(input);
    return status == 0 ? 0 : -1;
}


static void update_default_route(WifiModel *model)
{
    FILE *routes = fopen("/proc/net/route", "r");
    char line[512];
    if (!routes)
        return;
    while (fgets(line, sizeof(line), routes)) {
        char interface[16];
        unsigned long destination;
        unsigned long gateway;
        unsigned int flags;
        if (sscanf(line, "%15s %lx %lx %x", interface, &destination, &gateway, &flags) == 4 &&
            strcmp(interface, WIFI_INTERFACE) == 0 && destination == 0 && (flags & 2)) {
            snprintf(model->router_address, sizeof(model->router_address), "%lu.%lu.%lu.%lu",
                     gateway & 255, (gateway >> 8) & 255,
                     (gateway >> 16) & 255, (gateway >> 24) & 255);
            break;
        }
    }
    fclose(routes);
}

void wifi_control_update_current(WifiModel *model)
{
    char output[4096];
    char *line;
    char *save = NULL;
    int completed = 0;
    model->current_ssid[0] = '\0';
    model->current_ssid_length = 0;
    snprintf(model->ip_address, sizeof(model->ip_address), "Unavailable");
    snprintf(model->router_address, sizeof(model->router_address), "Unavailable");
    snprintf(model->mac_address, sizeof(model->mac_address), "Unavailable");
    snprintf(model->bssid, sizeof(model->bssid), "Unavailable");
    snprintf(model->signal, sizeof(model->signal), "Unavailable");
    snprintf(model->frequency, sizeof(model->frequency), "Unavailable");
    snprintf(model->link_speed, sizeof(model->link_speed), "Unavailable");
    snprintf(model->security, sizeof(model->security), "Unavailable");
    if (run_wpa_command("status", output, sizeof(output)) != 0)
        return;
    line = strtok_r(output, "\n", &save);
    while (line) {
        if (strncmp(line, "ssid=", 5) == 0)
            model->current_ssid_length = wifi_model_decode_ssid(line + 5, model->current_ssid);
        else if (strcmp(line, "wpa_state=COMPLETED") == 0)
            completed = 1;
        else if (strncmp(line, "ip_address=", 11) == 0)
            snprintf(model->ip_address, sizeof(model->ip_address), "%s", line + 11);
        else if (strncmp(line, "address=", 8) == 0)
            snprintf(model->mac_address, sizeof(model->mac_address), "%s", line + 8);
        else if (strncmp(line, "bssid=", 6) == 0)
            snprintf(model->bssid, sizeof(model->bssid), "%s", line + 6);
        else if (strncmp(line, "freq=", 5) == 0)
            snprintf(model->frequency, sizeof(model->frequency), "%s MHz", line + 5);
        else if (strncmp(line, "key_mgmt=", 9) == 0)
            snprintf(model->security, sizeof(model->security), "%s", line + 9);
        line = strtok_r(NULL, "\n", &save);
    }
    if (!completed) {
        model->current_ssid[0] = '\0';
        model->current_ssid_length = 0;
        return;
    }
    update_default_route(model);
    if (run_wpa_command("signal_poll", output, sizeof(output)) != 0)
        return;
    save = NULL;
    line = strtok_r(output, "\n", &save);
    while (line) {
        if (strncmp(line, "RSSI=", 5) == 0)
            snprintf(model->signal, sizeof(model->signal), "%s dBm", line + 5);
        else if (strncmp(line, "LINKSPEED=", 10) == 0)
            snprintf(model->link_speed, sizeof(model->link_speed), "%s Mb/s", line + 10);
        line = strtok_r(NULL, "\n", &save);
    }
}
