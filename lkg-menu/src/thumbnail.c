#include "thumbnail.h"

#include <stddef.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <jpeglib.h>
#include <png.h>
#include <libheif/heif.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t thumbnail_hash_string(const char *text)
{
    uint64_t hash = 1469598103934665603ULL;
    while (*text) {
        hash ^= (unsigned char)*text++;
        hash *= 1099511628211ULL;
    }
    return hash;
}

int thumbnail_cache_path_in(const char *media_path, const char *cache_dir,
                            char *path, size_t size)
{
    struct stat media_stat;
    struct stat json_stat;
    char json_path[PATH_MAX];
    char key[PATH_MAX + 256];
    const char *base_dir = (cache_dir && cache_dir[0] != '\0') ?
                           cache_dir : "/home/lkg/data/cache";
    if (!media_path ||
        snprintf(json_path, sizeof(json_path), "%s.json", media_path) >= (int)sizeof(json_path))
        return -1;
    memset(&json_stat, 0, sizeof(json_stat));
    if (stat(media_path, &media_stat) != 0 ||
        (stat(json_path, &json_stat) != 0 && errno != ENOENT))
        return -1;
    snprintf(key, sizeof(key), "lkg-gallery-v6-normalized-aspect|%s|%lld|%lld|%lld|%lld",
             media_path, (long long)media_stat.st_size, (long long)media_stat.st_mtime,
             (long long)json_stat.st_size, (long long)json_stat.st_mtime);
    {
        static const char usb_media[] = "/home/lkg/data/media/USB/";
        const char *subdir = strncmp(media_path, usb_media, sizeof(usb_media) - 1) == 0 ?
                             "usb-thumbnails" : "thumbnails";
        if (snprintf(path, size, "%s/%s/%016llx.rgba", base_dir, subdir,
                     (unsigned long long)thumbnail_hash_string(key)) >= (int)size)
            return -1;
    }
    return 0;
}

int thumbnail_cache_path(const char *media_path, char *path, size_t size)
{
    return thumbnail_cache_path_in(media_path, NULL, path, size);
}

static void report_progress(int fd, size_t done, size_t total)
{
    if (fd >= 0 && total > 0)
        dprintf(fd, "out_time_us=%llu\n",
                (unsigned long long)(done * 1000000ULL / total));
}

static int resize_tile_strided(const uint8_t *tile, size_t tile_stride,
                               int tile_width, int tile_height, double aspect,
                               uint8_t *output, int output_width, int output_height)
{
    double transformed_width = aspect > 0.0 ? (int)(tile_height * aspect) : tile_width;
    double scale;
    double crop_width;
    double crop_height;
    int x;
    int y;
    if (transformed_width < 1.0)
        transformed_width = 1.0;
    scale = output_width / transformed_width;
    if (output_height / (double)tile_height > scale)
        scale = output_height / (double)tile_height;
    crop_width = output_width / scale;
    crop_height = output_height / scale;
    for (y = 0; y < output_height; ++y) {
        double source_y = (tile_height - crop_height) / 2.0 +
                          (y + 0.5) * crop_height / output_height - 0.5;
        int y0 = (int)source_y;
        int y1;
        double fy = source_y - y0;
        const uint8_t *row0;
        const uint8_t *row1;
        if (y0 < 0)
            y0 = 0;
        if (y0 >= tile_height)
            y0 = tile_height - 1;
        y1 = y0 + 1 < tile_height ? y0 + 1 : y0;
        row0 = tile + (size_t)y0 * tile_stride;
        row1 = tile + (size_t)y1 * tile_stride;
        for (x = 0; x < output_width; ++x) {
            double source_x = (transformed_width - crop_width) / 2.0 +
                              (x + 0.5) * crop_width / output_width;
            int x0;
            int x1;
            double fx;
            uint8_t *target = output + ((size_t)y * output_width + x) * 4;
            int channel;
            source_x = source_x * tile_width / transformed_width - 0.5;
            x0 = (int)source_x;
            fx = source_x - x0;
            if (x0 < 0)
                x0 = 0;
            if (x0 >= tile_width)
                x0 = tile_width - 1;
            x1 = x0 + 1 < tile_width ? x0 + 1 : x0;
            for (channel = 0; channel < 3; ++channel) {
                const uint8_t *a = row0 + (size_t)x0 * 4 + channel;
                const uint8_t *b = row0 + (size_t)x1 * 4 + channel;
                const uint8_t *c = row1 + (size_t)x0 * 4 + channel;
                const uint8_t *d = row1 + (size_t)x1 * 4 + channel;
                double top = *a + (*b - *a) * fx;
                double bottom = *c + (*d - *c) * fx;
                target[channel] = (uint8_t)(top + (bottom - top) * fy + 0.5);
            }
            target[3] = 255;
        }
    }
    return 0;
}

static int resize_tile(const uint8_t *tile, int tile_width, int tile_height,
                       double aspect, uint8_t *output, int output_width, int output_height)
{
    return resize_tile_strided(tile, (size_t)tile_width * 4, tile_width, tile_height,
                               aspect, output, output_width, output_height);
}

int thumbnail_frame_write(const char *output_path, int columns, int rows, int view,
                          double aspect, const uint8_t *tile, int tile_width, int tile_height,
                          int output_width, int output_height)
{
    uint8_t *output;
    FILE *file;
    int result = 0;
    if (!tile || columns < 1 || rows < 1 || view < 0 || view >= columns * rows ||
        tile_width < 1 || tile_height < 1)
        return -1;
    output = malloc((size_t)output_width * output_height * 4);
    if (!output)
        return -1;
    resize_tile(tile, tile_width, tile_height, aspect, output, output_width, output_height);
    file = fopen(output_path, "wb");
    if (!file || fwrite(output, 4, (size_t)output_width * output_height, file) !=
                    (size_t)output_width * output_height)
        result = -1;
    if (file && fclose(file) != 0)
        result = -1;
    free(output);
    return result;
}

static int decode_png(const char *path, int columns, int rows, int view,
                      uint8_t **tile_out, int *width_out, int *height_out, int progress_fd)
{
    FILE *file = fopen(path, "rb");
    png_structp png = NULL;
    png_infop info = NULL;
    uint8_t *volatile row = NULL;
    uint8_t *volatile tile = NULL;
    png_uint_32 width;
    png_uint_32 height;
    int bit_depth;
    int color_type;
    int passes;
    int pass;
    int view_x = view % columns;
    int view_y = view / columns;
    int x0;
    int x1;
    int y0;
    int y1;
    int tile_width;
    int tile_height;
    int result = -1;
    if (!file)
        return -1;
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    info = png ? png_create_info_struct(png) : NULL;
    if (!info)
        goto cleanup;
    if (setjmp(png_jmpbuf(png)))
        goto cleanup;
    png_init_io(png, file);
    png_read_info(png, info);
    width = png_get_image_width(png, info);
    height = png_get_image_height(png, info);
    bit_depth = png_get_bit_depth(png, info);
    color_type = png_get_color_type(png, info);
    if (bit_depth == 16)
        png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
    } else if (!(color_type & PNG_COLOR_MASK_ALPHA)) {
        png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    if (png_get_channels(png, info) != 4)
        goto cleanup;
    x0 = view_x * (int)width / columns;
    x1 = (view_x + 1) * (int)width / columns;
    y0 = view_y * (int)height / rows;
    y1 = (view_y + 1) * (int)height / rows;
    tile_width = x1 - x0;
    tile_height = y1 - y0;
    if (tile_width <= 0 || tile_height <= 0 ||
        (size_t)tile_width > SIZE_MAX / ((size_t)tile_height * 4))
        goto cleanup;
    tile = malloc((size_t)tile_width * tile_height * 4);
    row = malloc(png_get_rowbytes(png, info));
    if (!tile || !row)
        goto cleanup;
    for (pass = 0; pass < passes; ++pass) {
        int y;
        int stop_y = passes == 1 ? y1 : (int)height;
        size_t total_rows = passes == 1 ? (size_t)y1 : (size_t)passes * height;
        for (y = 0; y < stop_y; ++y) {
            png_read_row(png, row, NULL);
            if (pass == passes - 1 && y >= y0 && y < y1)
                memcpy(tile + (size_t)(y - y0) * tile_width * 4,
                       row + (size_t)x0 * 4, (size_t)tile_width * 4);
            if ((y & 31) == 0)
                report_progress(progress_fd, (size_t)pass * height + (size_t)y,
                                total_rows);
        }
    }
    *tile_out = tile;
    *width_out = tile_width;
    *height_out = tile_height;
    tile = NULL;
    result = 0;

cleanup:
    free(row);
    free(tile);
    if (png)
        png_destroy_read_struct(&png, info ? &info : NULL, NULL);
    fclose(file);
    return result;
}

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

static int decode_jpeg(const char *path, int columns, int rows, int view,
                       uint8_t **tile_out, int *width_out, int *height_out, int progress_fd)
{
    FILE *file = fopen(path, "rb");
    struct jpeg_decompress_struct jpeg;
    JpegError error;
    uint8_t *volatile row = NULL;
    uint8_t *volatile tile = NULL;
    int view_x = view % columns;
    int view_y = view / columns;
    int x0;
    int x1;
    int y0;
    int y1;
    int tile_width;
    int tile_height;
    int scale = 1;
    int result = -1;
    if (!file)
        return -1;
    memset(&jpeg, 0, sizeof(jpeg));
    jpeg.err = jpeg_std_error(&error.manager);
    error.manager.error_exit = jpeg_failure;
    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&jpeg);
        free(row);
        free(tile);
        fclose(file);
        return -1;
    }
    jpeg_create_decompress(&jpeg);
    jpeg_stdio_src(&jpeg, file);
    jpeg_read_header(&jpeg, TRUE);
    while (scale < 8) {
        int next = scale * 2;
        int original_width = (view_x + 1) * (int)jpeg.image_width / columns -
                             view_x * (int)jpeg.image_width / columns;
        int original_height = (view_y + 1) * (int)jpeg.image_height / rows -
                              view_y * (int)jpeg.image_height / rows;
        if (original_width / next < 256 || original_height / next < 256)
            break;
        scale = next;
    }
    jpeg.scale_num = 1;
    jpeg.scale_denom = (unsigned int)scale;
    jpeg.out_color_space = JCS_RGB;
    jpeg_start_decompress(&jpeg);
    x0 = view_x * (int)jpeg.output_width / columns;
    x1 = (view_x + 1) * (int)jpeg.output_width / columns;
    y0 = view_y * (int)jpeg.output_height / rows;
    y1 = (view_y + 1) * (int)jpeg.output_height / rows;
    tile_width = x1 - x0;
    tile_height = y1 - y0;
    if (tile_width <= 0 || tile_height <= 0 ||
        (size_t)tile_width > SIZE_MAX / ((size_t)tile_height * 4))
        goto cleanup;
    tile = calloc((size_t)tile_width * tile_height, 4);
    row = malloc((size_t)jpeg.output_width * jpeg.output_components);
    if (!tile || !row)
        goto cleanup;
    while ((int)jpeg.output_scanline < y1 && jpeg.output_scanline < jpeg.output_height) {
        int y = (int)jpeg.output_scanline;
        int x;
        if (jpeg_read_scanlines(&jpeg, &row, 1) != 1)
            goto cleanup;
        if (y >= y0 && y < y1) {
            uint8_t *target = tile + (size_t)(y - y0) * tile_width * 4;
            for (x = 0; x < tile_width; ++x) {
                const uint8_t *source = row + (size_t)(x0 + x) * 3;
                target[x * 4] = source[0];
                target[x * 4 + 1] = source[1];
                target[x * 4 + 2] = source[2];
                target[x * 4 + 3] = 255;
            }
        }
        if ((y & 31) == 0)
            report_progress(progress_fd, (size_t)y, (size_t)y1);
    }
    *tile_out = tile;
    *width_out = tile_width;
    *height_out = tile_height;
    tile = NULL;
    result = 0;

cleanup:
    free(row);
    free(tile);
    jpeg_destroy_decompress(&jpeg);
    fclose(file);
    return result;
}

static struct heif_image_handle *select_heic_thumbnail(struct heif_image_handle *primary)
{
    int count = heif_image_handle_get_number_of_thumbnails(primary);
    heif_item_id ids[8];
    struct heif_image_handle *best = NULL;
    int best_area = 0;
    int i;
    if (count <= 0)
        return NULL;
    if (count > (int)(sizeof(ids) / sizeof(ids[0])))
        count = (int)(sizeof(ids) / sizeof(ids[0]));
    count = heif_image_handle_get_list_of_thumbnail_IDs(primary, ids, count);
    for (i = 0; i < count; ++i) {
        struct heif_image_handle *candidate = NULL;
        int width;
        int height;
        int area;
        if (heif_image_handle_get_thumbnail(primary, ids[i], &candidate).code != heif_error_Ok ||
            !candidate)
            continue;
        width = heif_image_handle_get_width(candidate);
        height = heif_image_handle_get_height(candidate);
        area = width * height;
        if (width >= 200 && height >= 200 && area > best_area) {
            if (best)
                heif_image_handle_release(best);
            best = candidate;
            best_area = area;
        } else {
            heif_image_handle_release(candidate);
        }
    }
    return best;
}

static int decode_heic_resized(const char *path, int columns, int rows, int view,
                               double aspect, uint8_t *output,
                               int output_width, int output_height, int progress_fd)
{
    static int initialized;
    struct heif_context *context = NULL;
    struct heif_image_handle *handle = NULL;
    struct heif_image_handle *thumb_handle = NULL;
    struct heif_image_handle *decode_target = NULL;
    struct heif_image *image = NULL;
    struct heif_security_limits *limits;
    const uint8_t *plane = NULL;
    size_t stride = 0;
    int width = 0;
    int height = 0;
    int view_x = view % columns;
    int view_y = view / columns;
    int x0;
    int x1;
    int y0;
    int y1;
    int tile_width;
    int tile_height;
    int result = -1;
    if (!initialized) {
        if (heif_init(NULL).code != heif_error_Ok)
            return -1;
        initialized = 1;
    }
    context = heif_context_alloc();
    if (!context)
        goto cleanup;
    heif_context_set_max_decoding_threads(context, 2);
    limits = heif_context_get_security_limits(context);
    if (limits) {
        if (limits->max_number_of_tiles < 256)
            limits->max_number_of_tiles = 256;
        if (limits->max_children_per_box < 256)
            limits->max_children_per_box = 256;
    }
    if (heif_context_read_from_file(context, path, NULL).code != heif_error_Ok ||
        heif_context_get_primary_image_handle(context, &handle).code != heif_error_Ok)
        goto cleanup;
    if (columns == 1 && rows == 1)
        thumb_handle = select_heic_thumbnail(handle);
    decode_target = thumb_handle ? thumb_handle : handle;
    if (heif_decode_image(decode_target, &image, heif_colorspace_RGB,
                          heif_chroma_interleaved_RGBA, NULL).code != heif_error_Ok) {
        if (!thumb_handle ||
            heif_decode_image(handle, &image, heif_colorspace_RGB,
                              heif_chroma_interleaved_RGBA, NULL).code != heif_error_Ok)
            goto cleanup;
    }
    report_progress(progress_fd, 1, 2);
    width = heif_image_get_width(image, heif_channel_interleaved);
    height = heif_image_get_height(image, heif_channel_interleaved);
    plane = heif_image_get_plane_readonly2(image, heif_channel_interleaved, &stride);
    if (width <= 0 || height <= 0 || !plane)
        goto cleanup;
    x0 = view_x * width / columns;
    x1 = (view_x + 1) * width / columns;
    y0 = view_y * height / rows;
    y1 = (view_y + 1) * height / rows;
    tile_width = x1 - x0;
    tile_height = y1 - y0;
    if (tile_width <= 0 || tile_height <= 0)
        goto cleanup;
    resize_tile_strided(plane + (size_t)y0 * stride + (size_t)x0 * 4, stride,
                        tile_width, tile_height, aspect,
                        output, output_width, output_height);
    result = 0;

cleanup:
    if (image)
        heif_image_release(image);
    if (thumb_handle)
        heif_image_handle_release(thumb_handle);
    if (handle)
        heif_image_handle_release(handle);
    if (context)
        heif_context_free(context);
    return result;
}

static int image_format(const char *path)
{
    static const uint8_t png_signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    static const char *heif_brands[] = {
        "heic", "heix", "hevc", "hevx", "heim", "heis", "hevm", "hevs", "mif1", "msf1"
    };
    uint8_t header[12];
    FILE *file = fopen(path, "rb");
    size_t count;
    size_t i;
    if (!file)
        return 0;
    count = fread(header, 1, sizeof(header), file);
    fclose(file);
    if (count == sizeof(header) && memcmp(header, png_signature, sizeof(png_signature)) == 0)
        return 1;
    if (count >= 3 && header[0] == 0xff && header[1] == 0xd8 && header[2] == 0xff)
        return 2;
    if (count == sizeof(header) && memcmp(header + 4, "ftyp", 4) == 0) {
        for (i = 0; i < sizeof(heif_brands) / sizeof(heif_brands[0]); ++i)
            if (memcmp(header + 8, heif_brands[i], 4) == 0)
                return 3;
    }
    return 0;
}

int thumbnail_image_supported(const char *path)
{
    return image_format(path) != 0;
}

int thumbnail_image_generate(const char *path, const char *output_path,
                             int columns, int rows, int view, double aspect,
                             int output_width, int output_height, int progress_fd)
{
    uint8_t *tile = NULL;
    uint8_t *output = NULL;
    FILE *file = NULL;
    int format = image_format(path);
    int tile_width = 0;
    int tile_height = 0;
    int result = -1;
    if (columns < 1 || rows < 1 || view < 0 || view >= columns * rows)
        return -1;
    output = malloc((size_t)output_width * output_height * 4);
    if (!output)
        goto cleanup;
    if (format == 1) {
        if (decode_png(path, columns, rows, view, &tile, &tile_width, &tile_height,
                       progress_fd) != 0)
            goto cleanup;
        resize_tile(tile, tile_width, tile_height, aspect, output, output_width, output_height);
    } else if (format == 2) {
        if (decode_jpeg(path, columns, rows, view, &tile, &tile_width, &tile_height,
                        progress_fd) != 0)
            goto cleanup;
        resize_tile(tile, tile_width, tile_height, aspect, output, output_width, output_height);
    } else if (format == 3) {
        if (decode_heic_resized(path, columns, rows, view, aspect, output,
                                output_width, output_height, progress_fd) != 0)
            goto cleanup;
    } else {
        goto cleanup;
    }
    file = fopen(output_path, "wb");
    if (!file)
        goto cleanup;
    if (fwrite(output, 4, (size_t)output_width * output_height, file) !=
        (size_t)output_width * output_height)
        goto cleanup;
    if (fclose(file) != 0) {
        file = NULL;
        goto cleanup;
    }
    file = NULL;
    report_progress(progress_fd, 1, 1);
    if (progress_fd >= 0)
        dprintf(progress_fd, "progress=end\n");
    result = 0;

cleanup:
    if (file)
        fclose(file);
    free(output);
    free(tile);
    return result;
}
