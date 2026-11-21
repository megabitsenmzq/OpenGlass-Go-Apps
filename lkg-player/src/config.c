#include "player.h"
#include "heic.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

const char *media_cache_dir(const char *json_path, const char *base_dir,
                            char *buffer, size_t size)
{
    static const char usb_media[] = "/home/lkg/data/media/USB/";
    static const char default_cache[] = "/home/lkg/data/cache";
    if (strncmp(json_path, usb_media, sizeof(usb_media) - 1) == 0 &&
        strcmp(base_dir, default_cache) == 0 &&
        snprintf(buffer, size, "%s/usb", base_dir) < (int)size)
        return buffer;
    return base_dir;
}

int media_find_cached_image(const char *json_path, const char *calibration_path,
                            const char *media_path, const char *base_dir,
                            char **cached_path, char *error, size_t error_size)
{
    char buffer[PATH_MAX];
    const char *selected = media_cache_dir(json_path, base_dir, buffer, sizeof(buffer));
    return find_cached_image(json_path, calibration_path, media_path,
                             selected, cached_path, error, error_size);
}

static char *read_text_file(const char *path, char *error, size_t error_size)
{
    FILE *file = fopen(path, "rb");
    char *text;
    long length;
    size_t read_size;

    if (!file) {
        set_error(error, error_size, "Failed to open %s: %s", path, strerror(errno));
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        set_error(error, error_size, "Failed to read %s", path);
        fclose(file);
        return NULL;
    }
    text = malloc((size_t)length + 1);
    if (!text) {
        set_error(error, error_size, "Out of memory");
        fclose(file);
        return NULL;
    }
    read_size = fread(text, 1, (size_t)length, file);
    if (read_size != (size_t)length || ferror(file)) {
        set_error(error, error_size, "Failed to read %s", path);
        free(text);
        fclose(file);
        return NULL;
    }
    text[length] = '\0';
    fclose(file);
    return text;
}

static const char *skip_space(const char *p)
{
    while (*p && isspace((unsigned char)*p))
        ++p;
    return p;
}

static const char *skip_string(const char *p)
{
    if (*p != '"')
        return NULL;
    ++p;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1])
            p += 2;
        else
            ++p;
    }
    return *p == '"' ? p + 1 : NULL;
}

static int string_equals(const char *start, const char *end, const char *key)
{
    size_t length = (size_t)(end - start);
    return strlen(key) == length && memcmp(start, key, length) == 0;
}

static const char *json_key(const char *json, const char *key)
{
    const char *p = skip_space(json);
    int depth = 1;
    if (*p != '{')
        return NULL;
    ++p;
    while (*p) {
        if (*p == '"') {
            const char *key_start = p + 1;
            const char *after = skip_string(p);
            const char *colon;
            if (!after)
                return NULL;
            colon = skip_space(after);
            if (depth == 1 && string_equals(key_start, after - 1, key) && *colon == ':')
                return skip_space(colon + 1);
            p = after;
        } else {
            if (*p == '{' || *p == '[') {
                ++depth;
            } else if (*p == '}' || *p == ']') {
                if (--depth == 0)
                    return NULL;
            }
            ++p;
        }
    }
    return NULL;
}

static int json_number(const char *json, const char *key, double *out)
{
    const char *value = json_key(json, key);
    char *end;
    double number;
    if (!value)
        return 0;
    errno = 0;
    number = strtod(value, &end);
    if (end == value || errno == ERANGE || !isfinite(number))
        return 0;
    *out = number;
    return 1;
}

static int json_boolean(const char *json, const char *key, int *out)
{
    const char *value = json_key(json, key);
    if (!value)
        return 0;
    if (strncmp(value, "true", 4) == 0) {
        *out = 1;
        return 1;
    }
    if (strncmp(value, "false", 5) == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

static int positive_int(const char *json, const char *key, int *out)
{
    double value;
    if (!json_number(json, key, &value) || value < 1 || value > 1024 || floor(value) != value)
        return 0;
    *out = (int)value;
    return 1;
}

static int video_path(const char *path)
{
    static const char *extensions[] = {
        ".mp4", ".mov", ".mkv", ".webm", ".avi", ".m4v"
    };
    const char *extension = strrchr(path, '.');
    size_t i;
    if (!extension)
        return 0;
    for (i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
        if (strcasecmp(extension, extensions[i]) == 0)
            return 1;
    return 0;
}

static void config_apply_spatial(MediaConfig *out, const Decoder *decoder)
{
    if (heic_spatial_pair(out->media_path, NULL, NULL)) {
        out->plain = 0;
        out->quilt.columns = 6;
        out->quilt.rows = 10;
        out->quilt.views = 60;
        out->quilt.aspect = 4.0f / 3.0f;
        out->quilt.movie = 0;
        out->quilt.clamp_edges = 1;
        return;
    }
    if (!video_path(out->media_path) || !decoder->stereo)
        return;
    out->plain = 0;
    out->quilt.columns = 6;
    out->quilt.rows = 10;
    out->quilt.views = 60;
    out->quilt.aspect = (float)decoder->width / decoder->height;
    out->quilt.movie = 1;
    out->quilt.clamp_edges = 1;
    out->quilt.stereo_disparity = (float)decoder->spatial_disparity;
}

static int config_probe_video(const char *json_path, MediaConfig *out, Decoder *decoder,
                              const char *calibration_path, const char *cache_dir,
                              Renderer *progress_renderer, char *error, size_t error_size)
{
    char buffer[PATH_MAX];
    const char *selected = media_cache_dir(json_path, cache_dir, buffer, sizeof(buffer));
    int found = find_cached_video(json_path, calibration_path, out->media_path,
                                  selected, &out->cached_path, error, error_size);
    if (found < 0)
        return -1;
    if (found) {
        out->plain = 0;
        out->cached_plain = 0;
        return 0;
    }
    if (out->plain) {
        found = find_cached_plain_video(json_path, calibration_path, out->media_path,
                                        selected, &out->cached_path, error, error_size);
        if (found < 0)
            return -1;
        if (found) {
            out->cached_plain = 1;
            return 0;
        }
    }
    if (progress_renderer && !out->plain) {
        renderer_begin_progress(progress_renderer, 1);
        renderer_progress(progress_renderer, 0);
    }
    if (decoder_probe(decoder, out->media_path, error, error_size) != 0)
        return -1;
    decoder->movie = out->quilt.movie;
    config_apply_spatial(out, decoder);
    return 0;
}

int config_load_media(const char *json_path, MediaConfig *out, Decoder *decoder,
                      const char *calibration_path, const char *cache_dir,
                      Renderer *progress_renderer, char *error, size_t error_size)
{
    char *json;
    const char *settings;
    size_t path_length = strlen(json_path);
    double value;
    int flag;

    memset(out, 0, sizeof(*out));
    memset(decoder, 0, sizeof(*decoder));
    decoder->fd = -1;
    if (path_length < 6 || strcmp(json_path + path_length - 5, ".json") != 0) {
        set_error(error, error_size, "Provide an original sidecar JSON file (for example image_qs8x6.png.json)");
        return -1;
    }
    if (access(json_path, F_OK) != 0 && errno == ENOENT) {
        out->plain = 1;
        out->quilt.columns = 1;
        out->quilt.rows = 1;
        out->quilt.views = 1;
        out->quilt.aspect = 1440.0f / 2560.0f;
        out->quilt.zoom = 1.0f;
        out->quilt.clamp_edges = 1;
        out->media_path = strndup(json_path, path_length - 5);
        if (!out->media_path) {
            set_error(error, error_size, "Out of memory");
            return -1;
        }
        out->quilt.movie = video_path(out->media_path);
        if (access(out->media_path, R_OK) != 0) {
            set_error(error, error_size, "Media file not found: %s", out->media_path);
            config_free_media(out);
            return -1;
        }
        if (out->quilt.movie)
            return config_probe_video(json_path, out, decoder, calibration_path,
                                      cache_dir, progress_renderer, error, error_size);
        config_apply_spatial(out, decoder);
        return 0;
    }
    json = read_text_file(json_path, error, error_size);
    if (!json)
        return -1;

    settings = json_key(json, "quilt_settings");
    if (!settings || !positive_int(settings, "viewX", &out->quilt.columns) ||
        !positive_int(settings, "viewY", &out->quilt.rows)) {
        set_error(error, error_size, "%s has no valid quilt_settings.viewX/viewY", json_path);
        free(json);
        return -1;
    }
    out->quilt.views = out->quilt.columns * out->quilt.rows;
    if (json_number(settings, "viewTotal", &value)) {
        if (value < 1 || value > out->quilt.views || floor(value) != value) {
            set_error(error, error_size, "Invalid viewTotal");
            free(json);
            return -1;
        }
        out->quilt.views = (int)value;
    }
    out->quilt.aspect = 1440.0f / 2560.0f;
    if (json_number(settings, "aspect", &value)) {
        if (value <= 0) {
            set_error(error, error_size, "Invalid quilt aspect");
            free(json);
            return -1;
        }
        out->quilt.aspect = (float)value;
    }
    out->quilt.zoom = 1.0f;
    if (json_number(json, "zoom", &value)) {
        if (value <= 0 || value > 1) {
            set_error(error, error_size, "zoom must be in the range (0, 1]");
            free(json);
            return -1;
        }
        out->quilt.zoom = (float)value;
    }
    if (json_number(json, "position_x", &value))
        out->quilt.position_x = (float)value;
    if (json_number(json, "position_y", &value))
        out->quilt.position_y = (float)value;
    if (json_number(json, "focus", &value))
        out->quilt.focus = (float)value;
    if (json_boolean(json, "invertViews", &flag))
        out->quilt.reverse_views = flag;
    if (json_boolean(json, "viewOrderReversed", &flag))
        out->quilt.reverse_views ^= flag;
    if (json_boolean(json, "movie", &flag))
        out->quilt.movie = flag;
    out->quilt.clamp_edges = !out->quilt.movie;

    out->media_path = strndup(json_path, path_length - 5);
    if (!out->media_path) {
        set_error(error, error_size, "Out of memory");
        free(json);
        return -1;
    }
    if (access(out->media_path, R_OK) != 0) {
        set_error(error, error_size, "Media file referenced by JSON not found: %s", out->media_path);
        config_free_media(out);
        free(json);
        return -1;
    }
    free(json);
    if (out->quilt.movie)
        return config_probe_video(json_path, out, decoder, calibration_path,
                                  cache_dir, progress_renderer, error, error_size);
    config_apply_spatial(out, decoder);
    return 0;
}

static int calibration_value(const char *json, const char *key, double *out)
{
    const char *section = json_key(json, key);
    return section && json_number(section, "value", out);
}

int config_load_calibration(const char *path, Calibration *out, char *error, size_t error_size)
{
    char *json = read_text_file(path, error, error_size);
    double value;
    if (!json)
        return -1;
    memset(out, 0, sizeof(*out));
    out->screen_w = 1440;
    out->screen_h = 2560;
    out->dpi = 491;
    out->inv_view = 1;

    if (!calibration_value(json, "pitch", &value) || value <= 0) {
        set_error(error, error_size, "%s has no valid pitch.value", path);
        goto fail;
    }
    out->pitch = (float)value;
    if (!calibration_value(json, "slope", &value) || value == 0) {
        set_error(error, error_size, "%s has no valid slope.value", path);
        goto fail;
    }
    out->slope = (float)value;
    if (!calibration_value(json, "center", &value)) {
        set_error(error, error_size, "%s has no center.value", path);
        goto fail;
    }
    out->center = (float)value;
    if (calibration_value(json, "fringe", &value))
        out->fringe = (float)value;
    if (calibration_value(json, "DPI", &value)) {
        if (value <= 0 || value > 10000) {
            set_error(error, error_size, "Invalid DPI.value");
            goto fail;
        }
        out->dpi = (int)value;
    }
    if (calibration_value(json, "screenW", &value) && value > 0 && value <= 16384)
        out->screen_w = (int)value;
    if (calibration_value(json, "screenH", &value) && value > 0 && value <= 16384)
        out->screen_h = (int)value;
    if (calibration_value(json, "invView", &value))
        out->inv_view = value != 0;
    if (calibration_value(json, "flipImageX", &value))
        out->flip_image_x = value != 0;
    if (calibration_value(json, "flipImageY", &value))
        out->flip_image_y = value != 0;
    if (calibration_value(json, "flipSubp", &value))
        out->flip_subp = value != 0;
    free(json);
    return 0;

fail:
    free(json);
    return -1;
}

void config_free_media(MediaConfig *media)
{
    free(media->media_path);
    media->media_path = NULL;
    free(media->cached_path);
    media->cached_path = NULL;
}
