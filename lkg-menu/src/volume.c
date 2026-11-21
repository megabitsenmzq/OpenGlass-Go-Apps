#include "volume.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t preview_pid = -1;

static void redirect_null(int stdout_too)
{
    int fd = open("/dev/null", O_RDWR);
    if (fd < 0)
        return;
    dup2(fd, STDIN_FILENO);
    if (stdout_too)
        dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
        close(fd);
}

static long long monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int reap_pid(pid_t pid, int *status, int timeout_ms)
{
    long long deadline = monotonic_ms() + timeout_ms;
    for (;;) {
        pid_t result = waitpid(pid, status, WNOHANG);
        if (result == pid)
            return 1;
        if (result < 0 && errno != EINTR)
            return -1;
        if (monotonic_ms() >= deadline)
            return 0;
        {
            struct timespec delay = {0, 10000000};
            while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
                ;
        }
    }
}

static int wait_for_pid(pid_t pid, int timeout_ms)
{
    int status;
    int result = reap_pid(pid, &status, timeout_ms);
    if (result != 0)
        return result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
    kill(pid, SIGTERM);
    result = reap_pid(pid, &status, 200);
    if (!result) {
        kill(pid, SIGKILL);
        result = reap_pid(pid, &status, 200);
    }
    return -1;
}

static int run_wpctl(const char *operation, const char *value)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        redirect_null(1);
        execl("/usr/bin/wpctl", "wpctl", operation, "@DEFAULT_AUDIO_SINK@", value,
              (char *)NULL);
        _exit(127);
    }
    return wait_for_pid(pid, 2000);
}

int volume_get_percent(void)
{
    int descriptors[2];
    char output[128];
    ssize_t length = 0;
    pid_t pid;
    float value;
    int percent;
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
        close(descriptors[0]);
        dup2(descriptors[1], STDOUT_FILENO);
        close(descriptors[1]);
        redirect_null(0);
        execl("/usr/bin/wpctl", "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@",
              (char *)NULL);
        _exit(127);
    }
    close(descriptors[1]);
    {
        long long deadline = monotonic_ms() + 2000;
        long long remaining;
        while (length < (ssize_t)sizeof(output) - 1) {
            struct pollfd pfd = {descriptors[0], POLLIN, 0};
            remaining = deadline - monotonic_ms();
            ssize_t count;
            int poll_result;
            if (remaining <= 0)
                break;
            poll_result = poll(&pfd, 1, (int)remaining);
            if (poll_result < 0 && errno == EINTR)
                continue;
            if (poll_result <= 0)
                break;
            count = read(descriptors[0], output + length, sizeof(output) - 1 - (size_t)length);
            if (count > 0)
                length += count;
            else if (count == 0 || errno != EINTR)
                break;
        }
        close(descriptors[0]);
        remaining = deadline - monotonic_ms();
        if (wait_for_pid(pid, remaining > 0 ? (int)remaining : 0) != 0)
            return -1;
    }
    output[length] = '\0';
    if (strstr(output, "[MUTED]"))
        return 0;
    if (sscanf(output, "Volume: %f", &value) != 1)
        return -1;
    percent = (int)(value * 100.0f + 0.5f);
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    return percent;
}

int volume_set_percent(int percent)
{
    char value[16];
    if (percent < 0 || percent > 100)
        return -1;
    if (percent == 0)
        return run_wpctl("set-mute", "1");
    if (run_wpctl("set-mute", "0") != 0)
        return -1;
    snprintf(value, sizeof(value), "%.2f", percent / 100.0);
    return run_wpctl("set-volume", value);
}

void volume_preview_stop(void)
{
    if (preview_pid <= 0)
        return;
    kill(preview_pid, SIGTERM);
    (void)wait_for_pid(preview_pid, 500);
    preview_pid = -1;
}

void volume_preview_poll(void)
{
    int result;
    if (preview_pid <= 0)
        return;
    do {
        result = waitpid(preview_pid, NULL, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == preview_pid || (result < 0 && errno == ECHILD))
        preview_pid = -1;
}

void volume_preview_play(void)
{
    pid_t pid;
    volume_preview_stop();
    pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        redirect_null(1);
        execl("/usr/bin/pw-play", "pw-play",
              "/usr/share/sounds/freedesktop/stereo/audio-volume-change.oga",
              (char *)NULL);
        _exit(127);
    }
    preview_pid = pid;
}
