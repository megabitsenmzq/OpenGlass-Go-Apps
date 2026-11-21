#include "heic.h"
#include "player.h"

#include <libheif/heif.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPATIAL_COLUMNS 6
#define SPATIAL_ROWS 10
#define SPATIAL_VIEWS (SPATIAL_COLUMNS * SPATIAL_ROWS)

typedef struct {
    uint8_t *pixels;
    int width;
    int height;
} HeicImage;

static int heic_initialized;

static int set_heif_error(char *error, size_t size, const char *action, struct heif_error detail)
{
    set_error(error, size, "%s: %s", action, detail.message);
    return -1;
}

static int is_heif_container(const char *path)
{
    FILE *file = fopen(path, "rb");
    uint8_t header[12];
    size_t count;
    if (!file)
        return 0;
    count = fread(header, 1, sizeof(header), file);
    fclose(file);
    return count == sizeof(header) && memcmp(header + 4, "ftyp", 4) == 0;
}

static struct heif_context *heic_open(const char *path, char *error, size_t error_size)
{
    struct heif_context *context;
    struct heif_error detail;
    struct heif_security_limits *limits;
    if (!is_heif_container(path)) {
        set_error(error, error_size, "Not a HEIC/HEIF container");
        return NULL;
    }
    if (!heic_initialized) {
        detail = heif_init(NULL);
        if (detail.code != heif_error_Ok) {
            set_heif_error(error, error_size, "Failed to initialize libheif", detail);
            return NULL;
        }
        heic_initialized = 1;
    }
    context = heif_context_alloc();
    if (!context) {
        set_error(error, error_size, "Failed to allocate HEIC context");
        return NULL;
    }
    heif_context_set_max_decoding_threads(context, 2);
    limits = heif_context_get_security_limits(context);
    if (limits) {
        if (limits->max_number_of_tiles < 256)
            limits->max_number_of_tiles = 256;
        if (limits->max_children_per_box < 256)
            limits->max_children_per_box = 256;
    }
    detail = heif_context_read_from_file(context, path, NULL);
    if (detail.code != heif_error_Ok) {
        set_heif_error(error, error_size, "Failed to read HEIC file", detail);
        heif_context_free(context);
        return NULL;
    }
    return context;
}

static int find_stereo(struct heif_context *context, uint32_t group_type,
                       uint32_t *left_id, uint32_t *right_id)
{
    int count = 0;
    struct heif_entity_group *groups = heif_context_get_entity_groups(context, group_type, 0, &count);
    int found = 0;
    int i;
    for (i = 0; i < count; ++i) {
        if (groups[i].num_entities >= 2) {
            if (left_id)
                *left_id = groups[i].entities[0];
            if (right_id)
                *right_id = groups[i].entities[1];
            found = 1;
            break;
        }
    }
    if (groups)
        heif_entity_groups_release(groups, count);
    return found;
}

int heic_spatial_pair(const char *path, uint32_t *left_id, uint32_t *right_id)
{
    char error[256];
    struct heif_context *context = heic_open(path, error, sizeof(error));
    int found;
    if (!context)
        return 0;
    found = find_stereo(context, heif_entity_group_ster, left_id, right_id) ||
            find_stereo(context, heif_entity_group_stem, left_id, right_id);
    heif_context_free(context);
    return found;
}

int heic_primary_dimensions(const char *path, int *width, int *height,
                            char *error, size_t error_size)
{
    struct heif_context *context = heic_open(path, error, error_size);
    struct heif_image_handle *handle = NULL;
    struct heif_error detail;
    int result = -1;
    if (!context)
        return -1;
    detail = heif_context_get_primary_image_handle(context, &handle);
    if (detail.code != heif_error_Ok) {
        set_heif_error(error, error_size, "Failed to find HEIC primary image", detail);
        goto cleanup;
    }
    *width = heif_image_handle_get_width(handle);
    *height = heif_image_handle_get_height(handle);
    if (*width <= 0 || *height <= 0) {
        set_error(error, error_size, "Invalid HEIC image dimensions");
        goto cleanup;
    }
    result = 0;

cleanup:
    if (handle)
        heif_image_handle_release(handle);
    heif_context_free(context);
    return result;
}

static int decode_handle(const struct heif_image_handle *handle, HeicImage *output,
                         char *error, size_t error_size)
{
    struct heif_image *image = NULL;
    struct heif_error detail;
    const uint8_t *plane;
    size_t stride = 0;
    int y;
    memset(output, 0, sizeof(*output));
    detail = heif_decode_image(handle, &image, heif_colorspace_RGB,
                               heif_chroma_interleaved_RGB, NULL);
    if (detail.code != heif_error_Ok)
        return set_heif_error(error, error_size, "Failed to decode HEIC image", detail);
    output->width = heif_image_get_width(image, heif_channel_interleaved);
    output->height = heif_image_get_height(image, heif_channel_interleaved);
    plane = heif_image_get_plane_readonly2(image, heif_channel_interleaved, &stride);
    if (output->width <= 0 || output->height <= 0 || !plane ||
        (size_t)output->width > SIZE_MAX / 3 ||
        (size_t)output->height > SIZE_MAX / ((size_t)output->width * 3)) {
        set_error(error, error_size, "Invalid decoded HEIC image");
        heif_image_release(image);
        return -1;
    }
    output->pixels = malloc((size_t)output->width * output->height * 3);
    if (!output->pixels) {
        set_error(error, error_size, "Not enough memory for decoded HEIC image");
        heif_image_release(image);
        return -1;
    }
    for (y = 0; y < output->height; ++y)
        memcpy(output->pixels + (size_t)y * output->width * 3,
               plane + (size_t)y * stride, (size_t)output->width * 3);
    heif_image_release(image);
    return 0;
}

static int decode_item(struct heif_context *context, uint32_t id, HeicImage *output,
                       char *error, size_t error_size)
{
    struct heif_image_handle *handle = NULL;
    struct heif_error detail = heif_context_get_image_handle(context, id, &handle);
    int result;
    if (detail.code != heif_error_Ok)
        return set_heif_error(error, error_size, "Failed to find HEIC image item", detail);
    result = decode_handle(handle, output, error, error_size);
    heif_image_handle_release(handle);
    return result;
}

int heic_decode_primary(const char *path, int expected_width, int expected_height,
                        uint8_t **rgb, char *error, size_t error_size)
{
    struct heif_context *context = heic_open(path, error, error_size);
    struct heif_image_handle *handle = NULL;
    struct heif_error detail;
    HeicImage image;
    int result = -1;
    *rgb = NULL;
    if (!context)
        return -1;
    detail = heif_context_get_primary_image_handle(context, &handle);
    if (detail.code != heif_error_Ok) {
        set_heif_error(error, error_size, "Failed to find HEIC primary image", detail);
        goto cleanup;
    }
    if (decode_handle(handle, &image, error, error_size) != 0)
        goto cleanup;
    if (image.width != expected_width || image.height != expected_height) {
        set_error(error, error_size, "HEIC dimensions changed during decoding");
        free(image.pixels);
        goto cleanup;
    }
    *rgb = image.pixels;
    result = 0;

cleanup:
    if (handle)
        heif_image_handle_release(handle);
    heif_context_free(context);
    return result;
}

static void measure_image(const HeicImage *image, double *mean, double *deviation)
{
    double sum[3] = {0};
    double square[3] = {0};
    uint64_t count = 0;
    int x;
    int y;
    int channel;
    for (y = 0; y < image->height; y += 8) {
        for (x = 0; x < image->width; x += 8) {
            const uint8_t *pixel = image->pixels + ((size_t)y * image->width + x) * 3;
            for (channel = 0; channel < 3; ++channel) {
                sum[channel] += pixel[channel];
                square[channel] += (double)pixel[channel] * pixel[channel];
            }
            ++count;
        }
    }
    for (channel = 0; channel < 3; ++channel) {
        double variance;
        mean[channel] = sum[channel] / count;
        variance = square[channel] / count - mean[channel] * mean[channel];
        deviation[channel] = variance > 0 ? sqrt(variance) : 1;
    }
}

static double sample_channel(const HeicImage *image, double x, double y, int channel)
{
    int x0;
    int y0;
    int x1;
    int y1;
    double fx;
    double fy;
    double top;
    double bottom;
    const uint8_t *a;
    const uint8_t *b;
    const uint8_t *c;
    const uint8_t *d;
    x = fmax(0, fmin(image->width - 1.001, x));
    y = fmax(0, fmin(image->height - 1.001, y));
    x0 = (int)x;
    y0 = (int)y;
    x1 = x0 + 1 < image->width ? x0 + 1 : x0;
    y1 = y0 + 1 < image->height ? y0 + 1 : y0;
    fx = x - x0;
    fy = y - y0;
    a = image->pixels + ((size_t)y0 * image->width + x0) * 3 + channel;
    b = image->pixels + ((size_t)y0 * image->width + x1) * 3 + channel;
    c = image->pixels + ((size_t)y1 * image->width + x0) * 3 + channel;
    d = image->pixels + ((size_t)y1 * image->width + x1) * 3 + channel;
    top = *a + (*b - *a) * fx;
    bottom = *c + (*d - *c) * fx;
    return top + (bottom - top) * fy;
}

static void resample_eye(const HeicImage *image, uint8_t *target,
                         int target_width, int target_height)
{
    int x;
    int y;
    int channel;
    for (y = 0; y < target_height; ++y) {
        double source_y = (y + 0.5) * image->height / (double)target_height - 0.5;
        size_t row = (size_t)y * target_width * 3;
        for (x = 0; x < target_width; ++x) {
            double source_x = (x + 0.5) * image->width / (double)target_width - 0.5;
            size_t pixel = row + (size_t)x * 3;
            for (channel = 0; channel < 3; ++channel) {
                double value = sample_channel(image, source_x, source_y, channel);
                target[pixel + channel] = (uint8_t)fmax(0, fmin(255, value + 0.5));
            }
        }
    }
}

int heic_decode_spatial_stereo(const char *path, int eye_width, int eye_height,
                               uint8_t **rgb, char *error, size_t error_size)
{
    struct heif_context *context = heic_open(path, error, error_size);
    uint32_t left_id = 0;
    uint32_t right_id = 0;
    HeicImage eye;
    uint8_t *stereo = NULL;
    double left_mean[3];
    double right_mean[3];
    double left_deviation[3];
    double right_deviation[3];
    double color_scale[3];
    int left_width = 0;
    int left_height = 0;
    size_t eye_pixels;
    size_t eye_bytes;
    size_t i;
    int channel;
    int result = -1;
    memset(&eye, 0, sizeof(eye));
    *rgb = NULL;
    if (!context)
        return -1;
    if (eye_width <= 0 || eye_height <= 0 ||
        (size_t)eye_width > SIZE_MAX / 6 ||
        (size_t)eye_height > SIZE_MAX / ((size_t)eye_width * 6)) {
        set_error(error, error_size, "Invalid spatial stereo dimensions");
        goto cleanup;
    }
    if (!find_stereo(context, heif_entity_group_ster, &left_id, &right_id) &&
        !find_stereo(context, heif_entity_group_stem, &left_id, &right_id)) {
        set_error(error, error_size, "HEIC file has no stereo pair");
        goto cleanup;
    }
    eye_pixels = (size_t)eye_width * eye_height;
    eye_bytes = eye_pixels * 3;
    stereo = malloc(eye_bytes * 2);
    if (!stereo) {
        set_error(error, error_size, "Not enough memory for spatial stereo buffers");
        goto cleanup;
    }
    if (decode_item(context, left_id, &eye, error, error_size) != 0)
        goto cleanup;
    left_width = eye.width;
    left_height = eye.height;
    measure_image(&eye, left_mean, left_deviation);
    resample_eye(&eye, stereo, eye_width, eye_height);
    free(eye.pixels);
    memset(&eye, 0, sizeof(eye));

    if (decode_item(context, right_id, &eye, error, error_size) != 0)
        goto cleanup;
    if (eye.width != left_width || eye.height != left_height) {
        set_error(error, error_size, "HEIC stereo dimensions do not match");
        goto cleanup;
    }
    measure_image(&eye, right_mean, right_deviation);
    resample_eye(&eye, stereo + eye_bytes, eye_width, eye_height);
    free(eye.pixels);
    memset(&eye, 0, sizeof(eye));

    for (channel = 0; channel < 3; ++channel)
        color_scale[channel] = right_deviation[channel] / left_deviation[channel];
    for (i = 0; i < eye_pixels; ++i) {
        uint8_t *pixel = stereo + i * 3;
        for (channel = 0; channel < 3; ++channel) {
            double value = (pixel[channel] - left_mean[channel]) * color_scale[channel] +
                           right_mean[channel];
            pixel[channel] = (uint8_t)fmax(0, fmin(255, value + 0.5));
        }
    }
    printf("HEIC stereo: %dx%d -> 2x %dx%d, left item %u, right item %u\n",
           left_width, left_height, eye_width, eye_height, left_id, right_id);
    *rgb = stereo;
    stereo = NULL;
    result = 0;

cleanup:
    free(stereo);
    free(eye.pixels);
    heif_context_free(context);
    return result;
}
