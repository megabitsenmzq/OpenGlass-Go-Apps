#include "player.h"

#include <png.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int png_stream_upload(const char *path, Renderer *renderer,
                      volatile sig_atomic_t *stopped, char *error, size_t error_size)
{
    FILE * volatile file = NULL;
    png_structp png = NULL;
    png_infop info = NULL;
    uint8_t * volatile rows = NULL;
    png_uint_32 width;
    png_uint_32 height;
    png_uint_32 volatile y = 0;
    int volatile result = -1;
    const int band_rows = 256;

    file = fopen(path, "rb");
    if (!file) {
        set_error(error, error_size, "Failed to open PNG %s", path);
        return -1;
    }
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    info = png_create_info_struct(png);
    if (!png || !info) {
        set_error(error, error_size, "Failed to create PNG decoder");
        goto cleanup;
    }
    if (setjmp(png_jmpbuf(png))) {
        set_error(error, error_size, "PNG decoding failed: %s", path);
        goto cleanup;
    }
    png_init_io(png, file);
    png_read_info(png, info);
    width = png_get_image_width(png, info);
    height = png_get_image_height(png, info);
    if (width == 0 || height == 0) {
        set_error(error, error_size, "Invalid PNG dimensions");
        goto cleanup;
    }
    if (png_get_interlace_type(png, info) != PNG_INTERLACE_NONE) {
        set_error(error, error_size, "Interlaced PNG is not supported by the streaming path");
        goto cleanup;
    }
    if (png_get_bit_depth(png, info) == 16)
        png_set_strip_16(png);
    if (png_get_color_type(png, info) == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (png_get_color_type(png, info) == PNG_COLOR_TYPE_GRAY &&
        png_get_bit_depth(png, info) < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (png_get_color_type(png, info) == PNG_COLOR_TYPE_GRAY ||
        png_get_color_type(png, info) == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_set_strip_alpha(png);
    png_read_update_info(png, info);
    if (png_get_channels(png, info) != 3 || png_get_rowbytes(png, info) != (size_t)width * 3) {
        set_error(error, error_size, "PNG cannot be converted to RGB24");
        goto cleanup;
    }
    if (width > SIZE_MAX / (3 * band_rows)) {
        set_error(error, error_size, "PNG row buffer is too large");
        goto cleanup;
    }
    rows = malloc((size_t)width * 3 * band_rows);
    if (!rows) {
        set_error(error, error_size, "Failed to allocate PNG row buffer");
        goto cleanup;
    }
    if (renderer_progress(renderer, 0.34) != 0) {
        set_error(error, error_size, "Failed to display preprocessing progress");
        goto cleanup;
    }
    while (y < height && !*stopped) {
        int count = height - y < (png_uint_32)band_rows ? (int)(height - y) : band_rows;
        int row;
        for (row = 0; row < count; ++row)
            png_read_row(png, rows + (size_t)row * width * 3, NULL);
        if (renderer_upload_rows(renderer, (int)y, count, rows, error, error_size) != 0)
            goto cleanup;
        y += count;
        if (renderer_progress(renderer, 0.34 + 0.46 * y / height) != 0) {
            set_error(error, error_size, "Failed to display preprocessing progress");
            goto cleanup;
        }
    }
    if (*stopped) {
        set_error(error, error_size, "Image preprocessing cancelled");
        goto cleanup;
    }
    png_read_end(png, info);
    result = 0;

cleanup:
    free(rows);
    if (png)
        png_destroy_read_struct(&png, &info, NULL);
    if (file)
        fclose(file);
    return result;
}

int png_read_rgb(const char *path, int width, int height, uint8_t *rgb,
                 char *error, size_t error_size)
{
    FILE * volatile file = NULL;
    png_structp png = NULL;
    png_infop info = NULL;
    png_bytep * volatile rows = NULL;
    int volatile result = -1;
    int y;

    file = fopen(path, "rb");
    if (!file) {
        set_error(error, error_size, "Failed to open cached PNG %s", path);
        return -1;
    }
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    info = png_create_info_struct(png);
    if (!png || !info) {
        set_error(error, error_size, "Failed to create cached PNG decoder");
        goto cleanup;
    }
    if (setjmp(png_jmpbuf(png))) {
        set_error(error, error_size, "Cached PNG decoding failed: %s", path);
        goto cleanup;
    }
    png_init_io(png, file);
    png_read_info(png, info);
    if (png_get_bit_depth(png, info) == 16)
        png_set_strip_16(png);
    if (png_get_color_type(png, info) == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (png_get_color_type(png, info) == PNG_COLOR_TYPE_GRAY &&
        png_get_bit_depth(png, info) < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (png_get_color_type(png, info) == PNG_COLOR_TYPE_GRAY ||
        png_get_color_type(png, info) == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_set_strip_alpha(png);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    if (png_get_image_width(png, info) != (png_uint_32)width ||
        png_get_image_height(png, info) != (png_uint_32)height ||
        png_get_channels(png, info) != 3 ||
        png_get_rowbytes(png, info) != (size_t)width * 3) {
        set_error(error, error_size, "Cached PNG has incorrect dimensions or format");
        goto cleanup;
    }
    rows = malloc((size_t)height * sizeof(*rows));
    if (!rows) {
        set_error(error, error_size, "Failed to allocate cached PNG row index");
        goto cleanup;
    }
    for (y = 0; y < height; ++y)
        rows[y] = rgb + (size_t)y * width * 3;
    png_read_image(png, rows);
    png_read_end(png, info);
    result = 0;

cleanup:
    free(rows);
    if (png)
        png_destroy_read_struct(&png, &info, NULL);
    if (file)
        fclose(file);
    return result;
}

int cache_read_rgb(const char *path, int width, int height, uint8_t *rgb,
                   char *error, size_t error_size)
{
    static const uint8_t magic[8] = {'L', 'K', 'G', 'R', 'G', 'B', '0', '1'};
    uint8_t header[16];
    size_t size = (size_t)width * height * 3;
    FILE *file = fopen(path, "rb");
    size_t count;
    int result = 0;
    if (!file) {
        set_error(error, error_size, "Failed to open image cache %s", path);
        return -1;
    }
    count = fread(header, 1, sizeof(header), file);
    if (count != sizeof(header) || memcmp(header, magic, sizeof(magic)) != 0) {
        fclose(file);
        return png_read_rgb(path, width, height, rgb, error, error_size);
    }
    if ((int)((uint32_t)header[8] | (uint32_t)header[9] << 8 |
              (uint32_t)header[10] << 16 | (uint32_t)header[11] << 24) != width ||
        (int)((uint32_t)header[12] | (uint32_t)header[13] << 8 |
              (uint32_t)header[14] << 16 | (uint32_t)header[15] << 24) != height) {
        set_error(error, error_size, "RGB image cache has incorrect dimensions");
        result = -1;
    } else if (fread(rgb, 1, size, file) != size) {
        set_error(error, error_size, "RGB image cache is truncated");
        result = -1;
    }
    fclose(file);
    return result;
}
