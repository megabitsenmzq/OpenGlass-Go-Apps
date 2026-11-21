#include "player.h"

#include <setjmp.h>
#include <stdio.h>
#include <jpeglib.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    struct jpeg_error_mgr manager;
    jmp_buf jump;
} JpegError;

static void jpeg_failure(j_common_ptr cinfo)
{
    JpegError *error = (JpegError *)cinfo->err;
    (*cinfo->err->output_message)(cinfo);
    longjmp(error->jump, 1);
}

int jpeg_stream_upload(const char *path, Renderer *renderer,
                       volatile sig_atomic_t *stopped, char *error, size_t error_size)
{
    FILE *file = NULL;
    struct jpeg_decompress_struct jpeg;
    JpegError jpeg_error;
    uint8_t * volatile rows = NULL;
    volatile int result = -1;
    const JDIMENSION band_rows = 256;

    file = fopen(path, "rb");
    if (!file) {
        set_error(error, error_size, "Failed to open JPEG %s", path);
        return -1;
    }
    memset(&jpeg, 0, sizeof(jpeg));
    jpeg.err = jpeg_std_error(&jpeg_error.manager);
    jpeg_error.manager.error_exit = jpeg_failure;
    if (setjmp(jpeg_error.jump)) {
        set_error(error, error_size, "JPEG decoding failed: %s", path);
        goto cleanup;
    }
    jpeg_create_decompress(&jpeg);
    jpeg_stdio_src(&jpeg, file);
    jpeg_read_header(&jpeg, TRUE);
    jpeg.out_color_space = JCS_RGB;
    if (jpeg_start_decompress(&jpeg) != TRUE || jpeg.output_components != 3 ||
        jpeg.output_width == 0 || jpeg.output_height == 0) {
        set_error(error, error_size, "JPEG cannot be converted to RGB24");
        goto cleanup;
    }
    if (jpeg.output_width > SIZE_MAX / (3 * band_rows)) {
        set_error(error, error_size, "JPEG row buffer is too large");
        goto cleanup;
    }
    rows = malloc((size_t)jpeg.output_width * 3 * band_rows);
    if (!rows) {
        set_error(error, error_size, "Failed to allocate JPEG row buffer");
        goto cleanup;
    }
    if (renderer_progress(renderer, 0.34) != 0) {
        set_error(error, error_size, "Failed to display preprocessing progress");
        goto cleanup;
    }
    while (jpeg.output_scanline < jpeg.output_height && !*stopped) {
        JDIMENSION source_y = jpeg.output_scanline;
        JDIMENSION count = jpeg.output_height - source_y < band_rows ?
                           jpeg.output_height - source_y : band_rows;
        JDIMENSION row;
        for (row = 0; row < count; ++row) {
            JSAMPROW target = (JSAMPROW)(rows + (size_t)row * jpeg.output_width * 3);
            if (jpeg_read_scanlines(&jpeg, &target, 1) != 1) {
                set_error(error, error_size, "JPEG scanline decoding failed");
                goto cleanup;
            }
        }
        if (renderer_upload_rows(renderer, (int)source_y, (int)count,
                                 (const uint8_t *)rows, error, error_size) != 0)
            goto cleanup;
        if (renderer_progress(renderer,
                0.34 + 0.46 * jpeg.output_scanline / jpeg.output_height) != 0) {
            set_error(error, error_size, "Failed to display preprocessing progress");
            goto cleanup;
        }
    }
    if (*stopped) {
        set_error(error, error_size, "Image preprocessing cancelled");
        goto cleanup;
    }
    if (jpeg_finish_decompress(&jpeg) != TRUE) {
        set_error(error, error_size, "JPEG decoding did not complete");
        goto cleanup;
    }
    result = 0;

cleanup:
    free((void *)rows);
    jpeg_destroy_decompress(&jpeg);
    if (file)
        fclose(file);
    return result;
}
