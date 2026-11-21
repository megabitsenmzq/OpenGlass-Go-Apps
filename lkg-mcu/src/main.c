#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define RESPONSE_MAX 4096
#define DEFAULT_DEVICE "/dev/ttyS5"

typedef struct {
    char serial[64];
    float pitch;
    float slope;
    float center;
} Calibration;

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s init --output FILE --template FILE [--device SERIAL]\n"
            "       %s init --print --template FILE [--device SERIAL]\n"
            "       %s poweroff [--device SERIAL]\n"
            "       %s watch [--device SERIAL]\n",
            program, program, program, program);
}

static int base64_decode(const char *text, uint8_t *output, size_t output_size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t length = strlen(text);
    size_t input = 0;
    size_t written = 0;
    if (length % 4 != 0)
        return -1;
    while (input < length) {
        uint32_t value = 0;
        int padding = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text[input + i];
            const char *position;
            if (c == '=') {
                ++padding;
                value <<= 6;
                continue;
            }
            position = strchr(alphabet, c);
            if (!position)
                return -1;
            value = (value << 6) | (uint32_t)(position - alphabet);
        }
        if (written + 3 - (size_t)padding > output_size)
            return -1;
        output[written++] = (uint8_t)(value >> 16);
        if (padding < 2)
            output[written++] = (uint8_t)(value >> 8);
        if (padding < 1)
            output[written++] = (uint8_t)value;
        input += 4;
    }
    return (int)written;
}

static int json_string(const char *json, const char *key, char *output, size_t output_size)
{
    char pattern[64];
    const char *start;
    const char *end;
    size_t length;
    if (snprintf(pattern, sizeof(pattern), "\"%s\":\"", key) >= (int)sizeof(pattern))
        return -1;
    start = strstr(json, pattern);
    if (!start)
        return -1;
    start += strlen(pattern);
    end = strchr(start, '"');
    if (!end)
        return -1;
    length = (size_t)(end - start);
    if (length == 0 || length >= output_size)
        return -1;
    memcpy(output, start, length);
    output[length] = '\0';
    return 0;
}

static float float32_le(const uint8_t *bytes)
{
    uint32_t bits = (uint32_t)bytes[0] |
                    ((uint32_t)bytes[1] << 8) |
                    ((uint32_t)bytes[2] << 16) |
                    ((uint32_t)bytes[3] << 24);
    float output;
    memcpy(&output, &bits, sizeof(output));
    return output;
}

static int parse_calibration(const char *json, Calibration *calibration)
{
    char pitch_slope[64];
    char center[64];
    uint8_t decoded[8];
    if (!strstr(json, "\"cmd\":\"CALI\"") || !strstr(json, "\"rid\":1") ||
        !strstr(json, "\"ack\":0"))
        return 1;
    if (json_string(json, "serial", calibration->serial, sizeof(calibration->serial)) != 0 ||
        json_string(json, "pitchslope", pitch_slope, sizeof(pitch_slope)) != 0 ||
        json_string(json, "center", center, sizeof(center)) != 0)
        return -1;
    for (size_t i = 0; calibration->serial[i]; ++i) {
        unsigned char c = (unsigned char)calibration->serial[i];
        if (!isalnum(c) && c != '-' && c != '_')
            return -1;
    }
    if (base64_decode(pitch_slope, decoded, sizeof(decoded)) != 8)
        return -1;
    calibration->pitch = float32_le(decoded);
    calibration->slope = float32_le(decoded + 4);
    if (base64_decode(center, decoded, sizeof(decoded)) != 4)
        return -1;
    calibration->center = float32_le(decoded);
    return 0;
}

static int serial_open(const char *path)
{
    struct termios settings;
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (tcgetattr(fd, &settings) != 0) {
        close(fd);
        return -1;
    }
    cfmakeraw(&settings);
    cfsetispeed(&settings, B115200);
    cfsetospeed(&settings, B115200);
    settings.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    settings.c_cflag |= CS8 | CLOCAL | CREAD;
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &settings) != 0 || tcflush(fd, TCIOFLUSH) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_request(int fd, const char *request)
{
    size_t length = strlen(request);
    size_t written = 0;
    while (written < length) {
        ssize_t count = write(fd, request + written, length - written);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
            struct pollfd descriptor = {.fd = fd, .events = POLLOUT};
            if (poll(&descriptor, 1, 100) >= 0)
                continue;
        }
        if (count <= 0)
            return -1;
        written += (size_t)count;
    }
    return 0;
}

static int receive_response(int fd, char *response, size_t response_size)
{
    size_t response_length = 0;
    int started = 0;
    for (int attempts = 0; attempts < 20; ++attempts) {
        struct pollfd descriptor = {.fd = fd, .events = POLLIN};
        char data[256];
        ssize_t count;
        int ready = poll(&descriptor, 1, 100);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            continue;
        count = read(fd, data, sizeof(data));
        if (count < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (count <= 0)
            return -1;
        for (ssize_t i = 0; i < count; ++i) {
            char c = data[i];
            if (c == '\x07') {
                started = 1;
                response_length = 0;
                continue;
            }
            if (c == '\r') {
                if (started && response_length > 0) {
                    response[response_length] = '\0';
                    return 0;
                }
                started = 0;
                continue;
            }
            if (c == '\n' || !started)
                continue;
            if (response_length + 1 >= response_size)
                return -1;
            response[response_length++] = c;
        }
    }
    return 1;
}

static int response_matches(const char *response, const char *command, int request_id)
{
    char id[24];
    snprintf(id, sizeof(id), "\"rid\":%d", request_id);
    return strstr(response, "\"cmd\":\"") && strstr(response, command) &&
           strstr(response, id) && strstr(response, "\"ack\":0");
}

static int request_response(int fd, const char *request, const char *command,
                            int request_id, char *response, size_t response_size)
{
    if (send_request(fd, request) != 0)
        return -1;
    for (int attempts = 0; attempts < 4; ++attempts) {
        int received = receive_response(fd, response, response_size);
        if (received < 0)
            return -1;
        if (received == 0 && response_matches(response, command, request_id))
            return 0;
    }
    return 1;
}

static int response_mode(const char *response)
{
    int mode;
    const char *value = strstr(response, "\"val\":");
    return value && sscanf(value, "\"val\":%d", &mode) == 1 ? mode : -1;
}

static int power_is_requested(int fd)
{
    static const char request[] = "\x07{\"cmd\":\"POWG\",\"rid\":2}\r";
    char response[RESPONSE_MAX];
    if (request_response(fd, request, "\"POWG\"", 2, response, sizeof(response)) != 0)
        return -1;
    if (strstr(response, "\"msg\":{\"val\":true}"))
        return 1;
    if (strstr(response, "\"msg\":{\"val\":false}"))
        return 0;
    return -1;
}

static int request_power_off(int fd)
{
    static const char request[] = "\x07{\"cmd\":\"POWS\",\"rid\":1,\"msg\":{\"val\":false}}\r";
    char response[RESPONSE_MAX];
    int requested = power_is_requested(fd);
    if (requested == 0) {
        printf("MCU power-off already requested\n");
        return 0;
    }
    if (requested < 0) {
        fprintf(stderr, "MCU power state unknown; not sending the power toggle\n");
        return -1;
    }
    if (request_response(fd, request, "\"POWS\"", 1, response, sizeof(response)) != 0)
        return -1;
    usleep(100000);
    return 0;
}

static int poweroff_command(const char *device)
{
    int fd = serial_open(device);
    int result;
    if (fd < 0) {
        fprintf(stderr, "Unable to open MCU serial port %s: %s\n", device, strerror(errno));
        return 0;
    }
    result = request_power_off(fd);
    close(fd);
    if (result != 0) {
        fprintf(stderr, "MCU power-off request was not acknowledged\n");
        return 0;
    }
    printf("MCU power-off request acknowledged\n");
    return 0;
}

static int notify_online(int fd)
{
    static const char request[] = "\x07{\"cmd\":\"PION\",\"rid\":2,\"msg\":{\"val\":true}}\r";
    char response[RESPONSE_MAX];
    int received;
    if (send_request(fd, request) != 0)
        return -1;
    received = receive_response(fd, response, sizeof(response));
    if (received != 0)
        return received;
    return strstr(response, "\"cmd\":\"PION\"") &&
           strstr(response, "\"rid\":2") && strstr(response, "\"ack\":0") ? 0 : 1;
}

static int hardware_read(const char *device, Calibration *calibration)
{
    static const char request[] = "\x07{\"cmd\":\"CALI\",\"rid\":1}\r";
    char response[RESPONSE_MAX];
    int fd = serial_open(device);
    int result = -1;
    if (fd < 0)
        return -1;
    if (notify_online(fd) != 0)
        fprintf(stderr, "lkg-mcu: MCU online notification was not acknowledged\n");
    if (send_request(fd, request) != 0)
        goto done;
    for (int attempts = 0; attempts < 4; ++attempts) {
        int received = receive_response(fd, response, sizeof(response));
        if (received < 0)
            goto done;
        if (received == 0) {
            result = parse_calibration(response, calibration);
            if (result <= 0)
                goto done;
        }
    }
    result = 1;
done:
    close(fd);
    if (result == 0)
        fprintf(stderr, "lkg-mcu: read STM32 calibration via %s\n", device);
    return result;
}

static int run_program(char *const arguments[])
{
    pid_t pid = fork();
    int status;
    if (pid == 0) {
        execv(arguments[0], arguments);
        _exit(127);
    }
    if (pid < 0)
        return -1;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int run_systemctl(const char *action, const char *unit)
{
    char *arguments[] = {"/usr/bin/systemctl", (char *)action, (char *)unit, NULL};
    return run_program(arguments);
}

static const char *usb_disk_unit(void)
{
    if (access("/etc/systemd/system/lkg-usb-disk.service", F_OK) == 0)
        return "lkg-usb-disk.service";
    if (access("/etc/systemd/system/lkg-usb-disk-test.service", F_OK) == 0)
        return "lkg-usb-disk-test.service";
    return NULL;
}

static int restore_standalone(void)
{
    const char *unit = usb_disk_unit();
    char *local[] = {"/usr/local/sbin/lkg-usb-disk", "local", NULL};
    if (!unit || run_systemctl("stop", unit) != 0 || run_program(local) != 0)
        return -1;
    return run_systemctl("start", "lkg-menu.service");
}

static int watch_command(const char *device)
{
    static const char request[] = "\x07{\"cmd\":\"MODG\",\"rid\":3}\r";
    char response[RESPONSE_MAX];
    int fd = serial_open(device);
    int previous_mode = -1;
    if (fd < 0) {
        fprintf(stderr, "Unable to open MCU serial port %s: %s\n", device, strerror(errno));
        return 1;
    }
    while (access("/run/lkg-usb-disk-mode", F_OK) == 0) {
        int mode = -1;
        if (send_request(fd, request) != 0)
            break;
        for (int attempts = 0; attempts < 4; ++attempts) {
            int received = receive_response(fd, response, sizeof(response));
            if (received != 0)
                continue;
            if (strstr(response, "\"cmd\":\"SHUT\"")) {
                printf("MCU shutdown requested\n");
                fflush(stdout);
                char *poweroff[] = {"/usr/bin/systemctl", "poweroff", NULL};
                close(fd);
                return run_program(poweroff) == 0 ? 0 : 1;
            }
            if (!response_matches(response, "\"MODG\"", 3))
                continue;
            mode = response_mode(response);
            if (mode >= 0)
                break;
        }
        if (mode < 0) {
            sleep(1);
            continue;
        }
        if (mode != previous_mode) {
            printf("MCU mode: %d\n", mode);
            fflush(stdout);
            previous_mode = mode;
        }
        if (mode == 6) {
            int result;
            printf("Standalone mode active; mounting local data and starting menu\n");
            fflush(stdout);
            close(fd);
            result = restore_standalone();
            return result == 0 ? 0 : 1;
        }
        sleep(1);
    }
    close(fd);
    return 0;
}

static char *read_text(const char *path)
{
    int fd = open(path, O_RDONLY);
    struct stat status;
    char *text;
    ssize_t total = 0;
    if (fd < 0)
        return NULL;
    if (fstat(fd, &status) != 0 || status.st_size <= 0 || status.st_size > 65536) {
        close(fd);
        return NULL;
    }
    text = malloc((size_t)status.st_size + 1);
    if (!text) {
        close(fd);
        return NULL;
    }
    while (total < status.st_size) {
        ssize_t count = read(fd, text + total, (size_t)(status.st_size - total));
        if (count <= 0) {
            free(text);
            close(fd);
            return NULL;
        }
        total += count;
    }
    text[total] = '\0';
    close(fd);
    return text;
}

static int replace_span(char **json, const char *start, const char *end, const char *value)
{
    size_t prefix = (size_t)(start - *json);
    size_t suffix = strlen(end);
    char *updated = malloc(prefix + strlen(value) + suffix + 1);
    if (!updated)
        return -1;
    memcpy(updated, *json, prefix);
    strcpy(updated + prefix, value);
    strcpy(updated + prefix + strlen(value), end);
    free(*json);
    *json = updated;
    return 0;
}

static int replace_number(char **json, const char *key, float number)
{
    char pattern[64];
    char value[64];
    const char *object;
    const char *value_key;
    const char *colon;
    const char *start;
    const char *end;
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    object = strstr(*json, pattern);
    if (!object)
        return -1;
    value_key = strstr(object, "\"value\"");
    if (!value_key)
        return -1;
    colon = strchr(value_key, ':');
    if (!colon)
        return -1;
    start = colon + 1;
    while (isspace((unsigned char)*start))
        ++start;
    end = start;
    while (strchr("+-0123456789.eE", *end) && *end)
        ++end;
    if (end == start)
        return -1;
    snprintf(value, sizeof(value), "%.9g", number);
    return replace_span(json, start, end, value);
}

static int replace_serial(char **json, const char *serial)
{
    const char *key = strstr(*json, "\"serial\"");
    const char *colon;
    const char *start;
    const char *end;
    if (!key)
        return -1;
    colon = strchr(key, ':');
    if (!colon)
        return -1;
    start = strchr(colon, '"');
    if (!start)
        return -1;
    ++start;
    end = strchr(start, '"');
    if (!end)
        return -1;
    return replace_span(json, start, end, serial);
}

static char *calibration_json(const Calibration *calibration, const char *template_path)
{
    char *json = read_text(template_path);
    if (!json)
        return NULL;
    if (replace_serial(&json, calibration->serial) != 0 ||
        replace_number(&json, "pitch", calibration->pitch) != 0 ||
        replace_number(&json, "slope", calibration->slope) != 0 ||
        replace_number(&json, "center", calibration->center) != 0) {
        free(json);
        return NULL;
    }
    return json;
}

static int write_atomic(const char *path, const char *contents, size_t length)
{
    char temporary[4096];
    int fd;
    ssize_t written = 0;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary))
        return -1;
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    while ((size_t)written < length) {
        ssize_t count = write(fd, contents + written, length - (size_t)written);
        if (count <= 0) {
            close(fd);
            unlink(temporary);
            return -1;
        }
        written += count;
    }
    if (fsync(fd) != 0 || close(fd) != 0 || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int calibration_main(int argc, char **argv)
{
    const char *output = NULL;
    const char *template_path = NULL;
    const char *device = DEFAULT_DEVICE;
    int print = 0;
    Calibration calibration;
    char *json;
    memset(&calibration, 0, sizeof(calibration));
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
            output = argv[++i];
        else if (strcmp(argv[i], "--template") == 0 && i + 1 < argc)
            template_path = argv[++i];
        else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc)
            device = argv[++i];
        else if (strcmp(argv[i], "--print") == 0)
            print = 1;
        else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if ((!output && !print) || !template_path) {
        usage(argv[0]);
        return 2;
    }
    if (hardware_read(device, &calibration) != 0) {
        fprintf(stderr, "lkg-mcu: unable to read STM32 calibration via %s\n", device);
        return 1;
    }
    json = calibration_json(&calibration, template_path);
    if (!json) {
        fprintf(stderr, "lkg-mcu: unable to construct visual.json from %s\n", template_path);
        return 1;
    }
    if (print && fwrite(json, 1, strlen(json), stdout) != strlen(json)) {
        free(json);
        return 1;
    }
    if (output && write_atomic(output, json, strlen(json)) != 0) {
        fprintf(stderr, "lkg-mcu: unable to write %s: %s\n", output, strerror(errno));
        free(json);
        return 1;
    }
    free(json);
    return 0;
}

int main(int argc, char **argv)
{
    const char *device = DEFAULT_DEVICE;
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return 0;
    }
    if (strcmp(argv[1], "init") == 0)
        return calibration_main(argc - 1, argv + 1);
    if (strcmp(argv[1], "poweroff") != 0 && strcmp(argv[1], "watch") != 0) {
        usage(argv[0]);
        return 2;
    }
    if (argc == 4 && strcmp(argv[2], "--device") == 0)
        device = argv[3];
    else if (argc != 2) {
        usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "poweroff") == 0)
        return poweroff_command(device);
    return watch_command(device);
}
