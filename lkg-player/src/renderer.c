#include "player.h"
#include "thumbnail.h"

#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

typedef struct {
    GLuint texture;
    int first_column;
    int first_row;
    int columns;
    int rows;
    int source_x;
    int source_y;
    int width;
    int height;
} Chunk;

typedef struct {
    uint32_t id;
    int fd;
} Framebuffer;

typedef struct {
    GLint chunk[LKG_MAX_CHUNKS];
    GLint nv12_uv;
    GLint resolution;
    GLint source_size;
    GLint tile_size;
    GLint columns;
    GLint rows;
    GLint views;
    GLint chunk_tile_columns;
    GLint chunk_tile_rows;
    GLint aspect;
    GLint zoom;
    GLint position;
    GLint focus;
    GLint stereo_disparity;
    GLint pitch;
    GLint tilt;
    GLint center;
    GLint fringe;
    GLint inv_view;
    GLint flip_x;
    GLint flip_y;
    GLint flip_subp;
    GLint reverse_views;
    GLint clamp_edges;
    GLint flip_output_y;
    GLint yuv_y;
    GLint yuv_coeffs;
} RendererUniforms;

#define LKG_DMA_CACHE_ENTRIES 16

typedef struct {
    int used;
    dev_t devices[2];
    ino_t inos[2];
    uint32_t strides[2];
    uint32_t offsets[2];
    int width;
    int height;
    uint64_t age;
    EGLImageKHR images[2];
    uint32_t fb;
} DmaCacheEntry;

struct Renderer {
    int fd;
    uint32_t connector_id;
    uint32_t crtc_id;
    drmModeCrtc *saved_crtc;
    drmModeModeInfo mode;
    int crtc_set;
    int flip_pending;
    int restore_crtc;
    int preserve_on_exit;
    uint32_t retire_buffer;
    pid_t retire_keeper;
    char *retire_pid_path;

    struct gbm_device *gbm;
    struct gbm_surface *gbm_surface;
    struct gbm_bo *scanout_bo;
    struct gbm_bo *pending_bo;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;

    GLuint program;
    RendererUniforms uniforms;
    GLuint progress_program;
    GLuint background_program;
    GLint background_uniform;
    GLuint background_texture;
    double progress_started;
    double progress_fraction;
    uint64_t progress_frame_current;
    uint64_t progress_frame_total;
    GLuint nv12_uv_texture;
    Chunk chunks[LKG_MAX_CHUNKS];
    int chunk_count;
    int chunk_columns;
    int chunk_rows;
    int chunk_tile_columns;
    int chunk_tile_rows;
    int source_w;
    int source_h;
    int direct;
    int unpack_subimage;
    int use_view_table;
    int startup;
    int paused;
    volatile sig_atomic_t *pause_requested;
    int offscreen;
    int progress_fd;
    int progress_has_frame;
    int error_screen;
    void *dma_images[2];
    DmaCacheEntry dma_cache[LKG_DMA_CACHE_ENTRIES];
    uint64_t dma_cache_clock;
    uint32_t overlay_plane;
    uint32_t overlay_fb;
    int overlay_active;
    uint8_t *upload_buffer;
    uint8_t *thumbnail_tile;
    int thumbnail_active;
    int thumbnail_captured;
    int thumbnail_x;
    int thumbnail_y;
    int thumbnail_w;
    int thumbnail_h;
    char thumbnail_path[PATH_MAX];
    char thumbnail_temporary[PATH_MAX + 32];
    int tile_w;
    int tile_h;
    QuiltConfig quilt;
    Calibration calibration;
    int yuv_bt709;
    int yuv_full_range;
    double profile_upload;
    double profile_draw;
    double profile_readback;
    double profile_convert;
    int profile_preprocess;
    int profile_dma;
    uint64_t profile_dma_frames;
    double profile_dma_import;
    double profile_dma_gpu;
    double profile_dma_wait;
    double profile_vblank_interval;
    double profile_last_vblank;
};

static double monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void dma_images_destroy(Renderer *renderer);
static void overlay_disable(Renderer *renderer);
static void draw_pause_geometry(Renderer *renderer);
static void set_uniforms(Renderer *renderer);
static int wait_for_flip(Renderer *renderer, char *error, size_t error_size);

static const char *vertex_source =
    "attribute vec2 a_position;\n"
    "uniform int u_flip_output_y;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = a_position * 0.5 + 0.5;\n"
    "  gl_Position = vec4(a_position.x, u_flip_output_y != 0 ? -a_position.y : a_position.y, 0.0, 1.0);\n"
    "}\n";

static const char *fragment_source =
    "#if DIRECT == 5\n"
    "precision mediump float;\n"
    "#else\n"
    "precision highp float;\n"
    "#endif\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_chunk0;\n"
    "uniform sampler2D u_chunk1;\n"
    "uniform sampler2D u_chunk2;\n"
    "uniform sampler2D u_chunk3;\n"
    "uniform sampler2D u_chunk4;\n"
    "uniform sampler2D u_chunk5;\n"
    "uniform sampler2D u_chunk6;\n"
    "uniform sampler2D u_chunk7;\n"
    "uniform sampler2D u_nv12_uv;\n"
    "uniform vec2 u_resolution;\n"
    "uniform vec2 u_source_size;\n"
    "uniform vec2 u_tile_size;\n"
    "uniform int u_columns;\n"
    "uniform int u_rows;\n"
    "uniform int u_views;\n"
    "uniform int u_chunk_tile_columns;\n"
    "uniform int u_chunk_tile_rows;\n"
    "uniform float u_aspect;\n"
    "uniform float u_zoom;\n"
    "uniform vec2 u_position;\n"
    "uniform float u_focus;\n"
    "uniform float u_stereo_disparity;\n"
    "uniform float u_pitch;\n"
    "uniform float u_tilt;\n"
    "uniform float u_center;\n"
    "uniform float u_fringe;\n"
    "uniform int u_inv_view;\n"
    "uniform int u_flip_x;\n"
    "uniform int u_flip_y;\n"
    "uniform int u_flip_subp;\n"
    "uniform int u_reverse_views;\n"
    "uniform int u_clamp_edges;\n"
    "#if DIRECT == 8 || DIRECT == 9\n"
    "uniform vec2 u_yuv_y;\n"
    "uniform vec4 u_yuv_coeffs;\n"
    "#endif\n"
    "#if USE_VIEW_TABLE\n"
    "uniform vec4 u_view_rects[VIEW_COUNT];\n"
    "uniform int u_view_chunks[VIEW_COUNT];\n"
    "#endif\n"
    "\n"
    "vec4 sample_chunk(int chunk, vec2 uv) {\n"
    "#if CHUNK_COUNT == 1\n"
    "  return texture2D(u_chunk0, uv);\n"
    "#elif CHUNK_COUNT == 2\n"
    "  if (chunk == 0) return texture2D(u_chunk0, uv);\n"
    "  return texture2D(u_chunk1, uv);\n"
    "#else\n"
    "  if (chunk == 0) return texture2D(u_chunk0, uv);\n"
    "  if (chunk == 1) return texture2D(u_chunk1, uv);\n"
    "  if (chunk == 2) return texture2D(u_chunk2, uv);\n"
    "  if (chunk == 3) return texture2D(u_chunk3, uv);\n"
    "  if (chunk == 4) return texture2D(u_chunk4, uv);\n"
    "  if (chunk == 5) return texture2D(u_chunk5, uv);\n"
    "  if (chunk == 6) return texture2D(u_chunk6, uv);\n"
    "  return texture2D(u_chunk7, uv);\n"
    "#endif\n"
    "}\n"
    "\n"
    "float channel_value(vec4 color, int channel) {\n"
    "  if (channel == 0) return color.r;\n"
    "  if (channel == 1) return color.g;\n"
    "  return color.b;\n"
    "}\n"
    "\n"
    "#if DIRECT == 7 || DIRECT == 9\n"
    "int stereo_eye(int view) {\n"
    "  if (u_reverse_views != 0) view = u_views - 1 - view;\n"
    "  return view < u_views / 2 ? 0 : 1;\n"
    "}\n"
    "\n"
    "float sample_stereo_eye(int eye, vec2 safe_uv, int channel) {\n"
    "  vec2 stereo_uv = safe_uv;\n"
    "  stereo_uv.x += eye == 0 ? u_stereo_disparity * 0.5 : -u_stereo_disparity * 0.5;\n"
    "  stereo_uv = clamp(stereo_uv, vec2(0.0), vec2(1.0));\n"
    "#if DIRECT == 7\n"
    "  vec4 c = eye == 0 ? texture2D(u_chunk0, stereo_uv) : texture2D(u_chunk1, stereo_uv);\n"
    "  return channel_value(c, channel);\n"
    "#else\n"
    "  float y = (eye == 0 ? texture2D(u_chunk0, stereo_uv) : texture2D(u_chunk3, stereo_uv)).r;\n"
    "  float ys = y * u_yuv_y.x + u_yuv_y.y;\n"
    "  if (channel == 0) {\n"
    "    float v = (eye == 0 ? texture2D(u_chunk2, stereo_uv) : texture2D(u_chunk5, stereo_uv)).r;\n"
    "    return clamp(ys + (v - 0.5019608) * u_yuv_coeffs.x, 0.0, 1.0);\n"
    "  }\n"
    "  if (channel == 1) {\n"
    "    float u = (eye == 0 ? texture2D(u_chunk1, stereo_uv) : texture2D(u_chunk4, stereo_uv)).r;\n"
    "    float v = (eye == 0 ? texture2D(u_chunk2, stereo_uv) : texture2D(u_chunk5, stereo_uv)).r;\n"
    "    return clamp(ys + (u - 0.5019608) * u_yuv_coeffs.y + (v - 0.5019608) * u_yuv_coeffs.z, 0.0, 1.0);\n"
    "  }\n"
    "  float u = (eye == 0 ? texture2D(u_chunk1, stereo_uv) : texture2D(u_chunk4, stereo_uv)).r;\n"
    "  return clamp(ys + (u - 0.5019608) * u_yuv_coeffs.w, 0.0, 1.0);\n"
    "#endif\n"
    "}\n"
    "#elif DIRECT == 8\n"
    "float sample_view_yuv(int view, vec2 safe_uv, vec2 chroma_uv, int channel) {\n"
    "  if (u_reverse_views != 0) view = u_views - 1 - view;\n"
    "  vec4 rect;\n"
    "#if USE_VIEW_TABLE\n"
    "  rect = u_view_rects[view];\n"
    "#else\n"
    "  int column = view - (view / u_columns) * u_columns;\n"
    "  int row = u_rows - 1 - view / u_columns;\n"
    "  int source_w = int(u_source_size.x);\n"
    "  int source_h = int(u_source_size.y);\n"
    "  int tile_x = column * source_w / u_columns;\n"
    "  int tile_y = row * source_h / u_rows;\n"
    "  int tile_w = (column + 1) * source_w / u_columns - tile_x;\n"
    "  int tile_h = (row + 1) * source_h / u_rows - tile_y;\n"
    "  rect = vec4(vec2(tile_x, tile_y) / u_source_size, vec2(tile_w, tile_h) / u_source_size);\n"
    "#endif\n"
    "  vec2 y_uv = rect.xy + safe_uv * rect.zw;\n"
    "  vec2 c_uv = rect.xy + chroma_uv * rect.zw;\n"
    "  float y = texture2D(u_chunk0, y_uv).r;\n"
    "  float ys = y * u_yuv_y.x + u_yuv_y.y;\n"
    "  if (channel == 0) {\n"
    "    float v = texture2D(u_chunk2, c_uv).r;\n"
    "    return clamp(ys + (v - 0.5019608) * u_yuv_coeffs.x, 0.0, 1.0);\n"
    "  }\n"
    "  if (channel == 1) {\n"
    "    float u = texture2D(u_chunk1, c_uv).r;\n"
    "    float v = texture2D(u_chunk2, c_uv).r;\n"
    "    return clamp(ys + (u - 0.5019608) * u_yuv_coeffs.y + (v - 0.5019608) * u_yuv_coeffs.z, 0.0, 1.0);\n"
    "  }\n"
    "  float u = texture2D(u_chunk1, c_uv).r;\n"
    "  return clamp(ys + (u - 0.5019608) * u_yuv_coeffs.w, 0.0, 1.0);\n"
    "}\n"
    "#else\n"
    "vec4 sample_view(int view, vec2 safe_uv) {\n"
    "  if (u_reverse_views != 0) view = u_views - 1 - view;\n"
    "#if USE_VIEW_TABLE\n"
    "  vec4 rect = u_view_rects[view];\n"
    "  return sample_chunk(u_view_chunks[view], rect.xy + safe_uv * rect.zw);\n"
    "#else\n"
    "  int column = view - (view / u_columns) * u_columns;\n"
    "  int row = u_rows - 1 - view / u_columns;\n"
    "  int source_w = int(u_source_size.x);\n"
    "  int source_h = int(u_source_size.y);\n"
    "  int tile_x = column * source_w / u_columns;\n"
    "  int tile_y = row * source_h / u_rows;\n"
    "  int tile_w = (column + 1) * source_w / u_columns - tile_x;\n"
    "  int tile_h = (row + 1) * source_h / u_rows - tile_y;\n"
    "#if CHUNK_COUNT == 1\n"
    "  vec2 uv = (vec2(tile_x, tile_y) + safe_uv * vec2(tile_w, tile_h)) / u_source_size;\n"
    "  return texture2D(u_chunk0, uv);\n"
    "#else\n"
    "  int chunk_x = column / u_chunk_tile_columns;\n"
    "  int chunk_y = row / u_chunk_tile_rows;\n"
    "  int first_column = chunk_x * u_chunk_tile_columns;\n"
    "  int first_row = chunk_y * u_chunk_tile_rows;\n"
    "  int actual_columns = min(u_chunk_tile_columns, u_columns - first_column);\n"
    "  int actual_rows = min(u_chunk_tile_rows, u_rows - first_row);\n"
    "  int chunk_left = first_column * source_w / u_columns;\n"
    "  int chunk_top = first_row * source_h / u_rows;\n"
    "  int chunk_w = (first_column + actual_columns) * source_w / u_columns - chunk_left;\n"
    "  int chunk_h = (first_row + actual_rows) * source_h / u_rows - chunk_top;\n"
    "  vec2 uv = (vec2(tile_x - chunk_left, tile_y - chunk_top) + safe_uv * vec2(tile_w, tile_h)) / vec2(chunk_w, chunk_h);\n"
    "  return sample_chunk(chunk_y * ((u_columns + u_chunk_tile_columns - 1) / u_chunk_tile_columns) + chunk_x, uv);\n"
    "#endif\n"
    "#endif\n"
    "}\n"
    "#endif\n"
    "\n"
    "void main() {\n"
    "  vec2 screen_uv = vec2(v_uv.x, 1.0 - v_uv.y);\n"
    "  vec2 pixel = v_uv;\n"
    "  if (u_flip_x != 0) { screen_uv.x = 1.0 - screen_uv.x; pixel.x = 1.0 - pixel.x; }\n"
    "  if (u_flip_y != 0) { screen_uv.y = 1.0 - screen_uv.y; pixel.y = 1.0 - pixel.y; }\n"
    "#if DIRECT == 2 || DIRECT == 5 || DIRECT == 6\n"
    "  float direct_display_aspect = u_resolution.x / u_resolution.y;\n"
    "  vec2 direct_scale = vec2(u_aspect / direct_display_aspect, 1.0);\n"
    "  direct_scale /= min(direct_scale.x, direct_scale.y);\n"
    "  vec2 direct_uv = clamp((screen_uv - 0.5) / direct_scale + 0.5, 0.0, 1.0);\n"
    "#else\n"
    "  vec2 direct_uv = screen_uv;\n"
    "#endif\n"
    "#if DIRECT >= 4 && DIRECT <= 5\n"
    "#if DIRECT == 4\n"
    "  vec2 rotated = direct_uv * u_resolution / u_source_size;\n"
    "#else\n"
    "  vec2 rotated = direct_uv;\n"
    "#endif\n"
    "  float luma = texture2D(u_chunk0, rotated).r;\n"
    "  vec2 chroma = texture2D(u_nv12_uv, rotated).rg;\n"
    "  float r = 1.164 * (luma - 0.0625) + 1.596 * (chroma.y - 0.5);\n"
    "  float g = 1.164 * (luma - 0.0625) - 0.392 * (chroma.x - 0.5) - 0.813 * (chroma.y - 0.5);\n"
    "  float b = 1.164 * (luma - 0.0625) + 2.018 * (chroma.x - 0.5);\n"
    "  gl_FragColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);\n"
    "  return;\n"
    "#elif DIRECT && DIRECT < 7\n"
    "  gl_FragColor = vec4(texture2D(u_chunk0, direct_uv).rgb, 1.0);\n"
    "  return;\n"
    "#endif\n"
    "  float display_aspect = u_resolution.x / u_resolution.y;\n"
    "  vec2 scale = vec2(u_aspect / display_aspect, 1.0) / u_zoom;\n"
    "  if (u_clamp_edges != 0)\n"
    "    scale /= min(scale.x, scale.y);\n"
    "  vec2 framed;\n"
    "  if (u_clamp_edges != 0) {\n"
    "    vec2 crop_size = 1.0 / scale;\n"
    "    vec2 crop_origin = 0.5 - 0.5 * crop_size - u_position * crop_size;\n"
    "    crop_origin = clamp(crop_origin, vec2(0.0), vec2(1.0) - crop_size);\n"
    "    framed = crop_origin + screen_uv * crop_size;\n"
    "  } else {\n"
    "    framed = (screen_uv - 0.5 - u_position) / scale + 0.5;\n"
    "    if (framed.x < 0.0 || framed.x > 1.0 || framed.y < 0.0 || framed.y > 1.0) {\n"
    "      gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "      return;\n"
    "    }\n"
    "  }\n"
    "  vec3 color;\n"
    "  for (int channel = 0; channel < 3; ++channel) {\n"
    "    int subpixel = u_flip_subp != 0 ? 2 - channel : channel;\n"
    "    float phase = (pixel.x + float(subpixel) / (3.0 * u_resolution.x) + pixel.y * u_tilt) * u_pitch - u_center + u_fringe + 0.5 / float(u_views);\n"
    "    phase = u_inv_view != 0 ? 1.0 - fract(phase) : fract(phase);\n"
    "    phase = clamp(phase, 0.00001, 0.99999);\n"
    "    float view = phase * float(u_views);\n"
    "    int lower = min(int(floor(view)), u_views - 1);\n"
    "    int upper = lower + 1;\n"
    "    if (upper == u_views) upper = 0;\n"
    "    vec2 focused = framed;\n"
    "    focused.x += (phase * 2.0 - 1.0) * u_focus;\n"
    "    vec2 inset = 0.5 / u_tile_size;\n"
    "    vec2 safe_uv = clamp(focused, inset, 1.0 - inset);\n"
    "#if DIRECT == 7 || DIRECT == 9\n"
    "    int eye_a = stereo_eye(lower);\n"
    "    int eye_b = stereo_eye(upper);\n"
    "    float val_a = sample_stereo_eye(eye_a, safe_uv, channel);\n"
    "    color[channel] = eye_a == eye_b ? val_a : mix(val_a, sample_stereo_eye(eye_b, safe_uv, channel), view - floor(view));\n"
    "#elif DIRECT == 8\n"
    "    vec2 chroma_inset = 1.0 / u_tile_size;\n"
    "    vec2 chroma_uv = clamp(focused, chroma_inset, 1.0 - chroma_inset);\n"
    "    float val_a = sample_view_yuv(lower, safe_uv, chroma_uv, channel);\n"
    "    float val_b = sample_view_yuv(upper, safe_uv, chroma_uv, channel);\n"
    "    color[channel] = mix(val_a, val_b, view - floor(view));\n"
    "#else\n"
    "    vec4 a = sample_view(lower, safe_uv);\n"
    "    vec4 b = sample_view(upper, safe_uv);\n"
    "    color[channel] = channel_value(mix(a, b, view - floor(view)), channel);\n"
    "#endif\n"
    "  }\n"
    "  gl_FragColor = vec4(color, 1.0);\n"
    "}\n";

static const char *progress_fragment_source =
    "precision mediump float;\n"
    "void main() {\n"
    "  gl_FragColor = vec4(0.9, 0.9, 0.9, 1.0);\n"
    "}\n";

static const char *background_fragment_source =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_background;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(u_background, v_uv);\n"
    "}\n";

static int has_extension(const char *extensions, const char *name)
{
    const char *found = extensions;
    size_t length = strlen(name);
    if (!extensions)
        return 0;
    while ((found = strstr(found, name)) != NULL) {
        if ((found == extensions || found[-1] == ' ') &&
            (found[length] == '\0' || found[length] == ' '))
            return 1;
        found += length;
    }
    return 0;
}

static GLuint compile_shader(GLenum type, const char *source, int chunk_count, int direct,
                             int view_count, int use_view_table,
                             char *error, size_t error_size)
{
    GLuint shader = glCreateShader(type);
    GLint compiled = GL_FALSE;
    char definition[160];
    const char *sources[2] = {definition, source};
    snprintf(definition, sizeof(definition),
             "#define CHUNK_COUNT %d\n#define DIRECT %d\n"
             "#define VIEW_COUNT %d\n#define USE_VIEW_TABLE %d\n",
             chunk_count, direct, view_count, use_view_table);
    if (type == GL_FRAGMENT_SHADER)
        glShaderSource(shader, 2, sources, NULL);
    else
        glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[4096];
        GLsizei length = 0;
        glGetShaderInfoLog(shader, sizeof(log), &length, log);
        set_error(error, error_size, "GLSL compilation failed: %.*s", (int)length, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint create_program(const char *source, int chunk_count, int direct,
                             int view_count, int use_view_table,
                             char *error, size_t error_size)
{
    GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source, 0, 0, 1, 0,
                                   error, error_size);
    GLuint fragment;
    GLuint program;
    GLint linked = GL_FALSE;
    if (!vertex)
        return 0;
    fragment = compile_shader(GL_FRAGMENT_SHADER, source, chunk_count, direct,
                              view_count, use_view_table, error, error_size);
    if (!fragment) {
        glDeleteShader(vertex);
        return 0;
    }
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "a_position");
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096];
        GLsizei length = 0;
        glGetProgramInfoLog(program, sizeof(log), &length, log);
        set_error(error, error_size, "GLSL linking failed: %.*s", (int)length, log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static int create_background_program(Renderer *renderer, char *error, size_t error_size)
{
    if (renderer->background_program)
        return 0;
    renderer->background_program = create_program(background_fragment_source, 1, 1, 1, 0,
                                                   error, error_size);
    if (!renderer->background_program)
        return -1;
    renderer->background_uniform = glGetUniformLocation(renderer->background_program,
                                                        "u_background");
    if (renderer->background_uniform < 0) {
        set_error(error, error_size, "Background sampler uniform not found");
        glDeleteProgram(renderer->background_program);
        renderer->background_program = 0;
        return -1;
    }
    glUseProgram(renderer->background_program);
    glUniform1i(renderer->background_uniform, 0);
    return 0;
}

static void initialize_progress_background(Renderer *renderer, const char *path)
{
    FILE *file = NULL;
    uint8_t *rgb = NULL;
    char magic[3] = {0};
    char shader_error[256] = {0};
    int width = 0;
    int height = 0;
    int maximum = 0;
    size_t size;
    if (!path)
        return;
    file = fopen(path, "rb");
    if (!file)
        return;
    if (fscanf(file, "%2s %d %d %d", magic, &width, &height, &maximum) != 4 ||
        strcmp(magic, "P6") != 0 || maximum != 255 ||
        width != renderer->mode.hdisplay || height != renderer->mode.vdisplay)
        goto fail;
    fgetc(file);
    size = (size_t)width * height * 3;
    rgb = malloc(size);
    if (!rgb || fread(rgb, 1, size, file) != size)
        goto fail;
    fclose(file);
    file = NULL;
    if (create_background_program(renderer, shader_error, sizeof(shader_error)) != 0)
        goto fail;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &renderer->background_texture);
    glBindTexture(GL_TEXTURE_2D, renderer->background_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, rgb);
    free(rgb);
    rgb = NULL;
    if (glGetError() != GL_NO_ERROR)
        goto fail;
    if (!renderer->restore_crtc && renderer->saved_crtc) {
        size_t pid_path_size = strlen(path) + 5;
        char *pid_path = malloc(pid_path_size);
        renderer->retire_buffer = renderer->saved_crtc->buffer_id;
        if (pid_path) {
            int keeper = 0;
            snprintf(pid_path, pid_path_size, "%s.pid", path);
            file = fopen(pid_path, "r");
            if (file && fscanf(file, "%d", &keeper) == 1 && keeper > 0) {
                renderer->retire_keeper = keeper;
                renderer->retire_pid_path = pid_path;
            } else {
                free(pid_path);
            }
            if (file)
                fclose(file);
            file = NULL;
        }
    }
    printf("Loaded slideshow transition background: %s\n", path);
    return;

fail:
    if (file)
        fclose(file);
    free(rgb);
    if (renderer->background_texture)
        glDeleteTextures(1, &renderer->background_texture);
    if (renderer->background_program)
        glDeleteProgram(renderer->background_program);
    renderer->background_texture = 0;
    renderer->background_program = 0;
    fprintf(stderr, "lkg-player: Failed to load transition background%s%s\n",
            shader_error[0] ? ": " : "", shader_error);
}

static int upload_view_table(Renderer *renderer, char *error, size_t error_size)
{
    int count = renderer->quilt.views;
    float *rects;
    int *chunk_ids;
    GLint rects_location;
    GLint chunks_location;
    int view;
    int result = 0;
    if (!renderer->use_view_table)
        return 0;
    rects = malloc((size_t)count * 4 * sizeof(*rects));
    chunk_ids = malloc((size_t)count * sizeof(*chunk_ids));
    if (!rects || !chunk_ids) {
        set_error(error, error_size, "Failed to allocate view UV table");
        free(rects);
        free(chunk_ids);
        return -1;
    }
    for (view = 0; view < count; ++view) {
        int column = view % renderer->quilt.columns;
        int row = renderer->quilt.rows - 1 - view / renderer->quilt.columns;
        int chunk_x = column / renderer->chunk_tile_columns;
        int chunk_y = row / renderer->chunk_tile_rows;
        int chunk_id = chunk_y * renderer->chunk_columns + chunk_x;
        Chunk *chunk = &renderer->chunks[chunk_id];
        int tile_x = (int)((int64_t)column * renderer->source_w / renderer->quilt.columns);
        int tile_y = (int)((int64_t)row * renderer->source_h / renderer->quilt.rows);
        int tile_w = (int)((int64_t)(column + 1) * renderer->source_w /
                           renderer->quilt.columns) - tile_x;
        int tile_h = (int)((int64_t)(row + 1) * renderer->source_h /
                           renderer->quilt.rows) - tile_y;
        rects[view * 4] = (float)(tile_x - chunk->source_x) / chunk->width;
        rects[view * 4 + 1] = (float)(tile_y - chunk->source_y) / chunk->height;
        rects[view * 4 + 2] = (float)tile_w / chunk->width;
        rects[view * 4 + 3] = (float)tile_h / chunk->height;
        chunk_ids[view] = chunk_id;
    }
    glUseProgram(renderer->program);
    rects_location = glGetUniformLocation(renderer->program, "u_view_rects[0]");
    chunks_location = glGetUniformLocation(renderer->program, "u_view_chunks[0]");
    if (rects_location < 0 || (!renderer->direct && renderer->chunk_count > 1 && chunks_location < 0)) {
        set_error(error, error_size, "View UV table uniform not found");
        result = -1;
    } else {
        glUniform4fv(rects_location, count, rects);
        if (!renderer->direct && renderer->chunk_count > 1)
            glUniform1iv(chunks_location, count, chunk_ids);
        if (glGetError() != GL_NO_ERROR) {
            set_error(error, error_size, "Failed to upload view UV table");
            result = -1;
        }
    }
    free(rects);
    free(chunk_ids);
    return result;
}

static void cache_uniform_locations(Renderer *renderer)
{
    static const char *sampler_names[LKG_MAX_CHUNKS] = {
        "u_chunk0", "u_chunk1", "u_chunk2", "u_chunk3",
        "u_chunk4", "u_chunk5", "u_chunk6", "u_chunk7"
    };
    RendererUniforms *uniforms = &renderer->uniforms;
    int i;
    for (i = 0; i < LKG_MAX_CHUNKS; ++i)
        uniforms->chunk[i] = glGetUniformLocation(renderer->program, sampler_names[i]);
    uniforms->nv12_uv = glGetUniformLocation(renderer->program, "u_nv12_uv");
    uniforms->resolution = glGetUniformLocation(renderer->program, "u_resolution");
    uniforms->source_size = glGetUniformLocation(renderer->program, "u_source_size");
    uniforms->tile_size = glGetUniformLocation(renderer->program, "u_tile_size");
    uniforms->columns = glGetUniformLocation(renderer->program, "u_columns");
    uniforms->rows = glGetUniformLocation(renderer->program, "u_rows");
    uniforms->views = glGetUniformLocation(renderer->program, "u_views");
    uniforms->chunk_tile_columns = glGetUniformLocation(renderer->program, "u_chunk_tile_columns");
    uniforms->chunk_tile_rows = glGetUniformLocation(renderer->program, "u_chunk_tile_rows");
    uniforms->aspect = glGetUniformLocation(renderer->program, "u_aspect");
    uniforms->zoom = glGetUniformLocation(renderer->program, "u_zoom");
    uniforms->position = glGetUniformLocation(renderer->program, "u_position");
    uniforms->focus = glGetUniformLocation(renderer->program, "u_focus");
    uniforms->stereo_disparity = glGetUniformLocation(renderer->program, "u_stereo_disparity");
    uniforms->pitch = glGetUniformLocation(renderer->program, "u_pitch");
    uniforms->tilt = glGetUniformLocation(renderer->program, "u_tilt");
    uniforms->center = glGetUniformLocation(renderer->program, "u_center");
    uniforms->fringe = glGetUniformLocation(renderer->program, "u_fringe");
    uniforms->inv_view = glGetUniformLocation(renderer->program, "u_inv_view");
    uniforms->flip_x = glGetUniformLocation(renderer->program, "u_flip_x");
    uniforms->flip_y = glGetUniformLocation(renderer->program, "u_flip_y");
    uniforms->flip_subp = glGetUniformLocation(renderer->program, "u_flip_subp");
    uniforms->reverse_views = glGetUniformLocation(renderer->program, "u_reverse_views");
    uniforms->clamp_edges = glGetUniformLocation(renderer->program, "u_clamp_edges");
    uniforms->flip_output_y = glGetUniformLocation(renderer->program, "u_flip_output_y");
    uniforms->yuv_y = glGetUniformLocation(renderer->program, "u_yuv_y");
    uniforms->yuv_coeffs = glGetUniformLocation(renderer->program, "u_yuv_coeffs");
}

static int create_renderer_program(Renderer *renderer, char *error, size_t error_size)
{
    GLint max_uniforms = 0;
    glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &max_uniforms);
    renderer->use_view_table = (renderer->direct == 0 || renderer->direct == 8) &&
                               renderer->quilt.views > 0 &&
                               renderer->quilt.views * 2 + 32 <= max_uniforms;
    renderer->program = create_program(fragment_source, renderer->chunk_count,
                                       renderer->direct, renderer->quilt.views,
                                       renderer->use_view_table, error, error_size);
    if (!renderer->program)
        return -1;
    cache_uniform_locations(renderer);
    set_uniforms(renderer);
    return upload_view_table(renderer, error, error_size);
}

static int choose_crtc(Renderer *renderer, char *error, size_t error_size)
{
    drmModeRes *resources = drmModeGetResources(renderer->fd);
    drmModeConnector *connector = NULL;
    drmModeEncoder *encoder = NULL;
    int i;
    int j;

    if (!resources) {
        set_error(error, error_size, "drmModeGetResources failed: %s", strerror(errno));
        return -1;
    }
    for (i = 0; i < resources->count_connectors; ++i) {
        connector = drmModeGetConnector(renderer->fd, resources->connectors[i]);
        if (connector && connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0)
            break;
        drmModeFreeConnector(connector);
        connector = NULL;
    }
    if (!connector) {
        set_error(error, error_size, "No connected DRM connector with a display mode");
        drmModeFreeResources(resources);
        return -1;
    }
    renderer->connector_id = connector->connector_id;
    renderer->mode = connector->modes[0];
    for (i = 0; i < connector->count_modes; ++i) {
        drmModeModeInfo *mode = &connector->modes[i];
        if ((mode->type & DRM_MODE_TYPE_PREFERRED) ||
            (mode->hdisplay == 1440 && mode->vdisplay == 2560)) {
            renderer->mode = *mode;
            if (mode->hdisplay == 1440 && mode->vdisplay == 2560)
                break;
        }
    }
    if (connector->encoder_id)
        encoder = drmModeGetEncoder(renderer->fd, connector->encoder_id);
    for (i = 0; i < connector->count_encoders && !encoder; ++i)
        encoder = drmModeGetEncoder(renderer->fd, connector->encoders[i]);
    if (!encoder) {
        set_error(error, error_size, "DRM encoder not found");
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        return -1;
    }
    if (encoder->crtc_id) {
        renderer->crtc_id = encoder->crtc_id;
    } else {
        for (i = 0; i < resources->count_crtcs; ++i) {
            if (encoder->possible_crtcs & (1u << i)) {
                renderer->crtc_id = resources->crtcs[i];
                break;
            }
        }
    }
    if (!renderer->crtc_id && connector->encoder_id) {
        drmModeFreeEncoder(encoder);
        for (j = 0; j < connector->count_encoders && !renderer->crtc_id; ++j) {
            encoder = drmModeGetEncoder(renderer->fd, connector->encoders[j]);
            if (!encoder)
                continue;
            for (i = 0; i < resources->count_crtcs; ++i) {
                if (encoder->possible_crtcs & (1u << i)) {
                    renderer->crtc_id = resources->crtcs[i];
                    break;
                }
            }
            if (!renderer->crtc_id) {
                drmModeFreeEncoder(encoder);
                encoder = NULL;
            }
        }
    }
    if (!renderer->crtc_id) {
        set_error(error, error_size, "No available CRTC found");
        drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        return -1;
    }
    renderer->saved_crtc = drmModeGetCrtc(renderer->fd, renderer->crtc_id);
    drmModeFreeEncoder(encoder);
    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    return 0;
}

static int initialize_egl(Renderer *renderer, char *error, size_t error_size)
{
    static const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    static const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
    };
    EGLConfig config;
    EGLint count;
    const char *client_extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

    renderer->gbm = gbm_create_device(renderer->fd);
    if (!renderer->gbm) {
        set_error(error, error_size, "gbm_create_device failed");
        return -1;
    }
    renderer->gbm_surface = gbm_surface_create(renderer->gbm,
        renderer->mode.hdisplay, renderer->mode.vdisplay, GBM_FORMAT_ARGB8888,
        GBM_BO_USE_RENDERING | (renderer->offscreen ? 0 : GBM_BO_USE_SCANOUT));
    if (!renderer->gbm_surface) {
        set_error(error, error_size, "gbm_surface_create failed");
        return -1;
    }
    if (has_extension(client_extensions, "EGL_EXT_platform_base")) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (get_platform_display)
            renderer->egl_display = get_platform_display(EGL_PLATFORM_GBM_KHR, renderer->gbm, NULL);
    }
    if (renderer->egl_display == EGL_NO_DISPLAY)
        renderer->egl_display = eglGetDisplay((EGLNativeDisplayType)renderer->gbm);
    if (renderer->egl_display == EGL_NO_DISPLAY || !eglInitialize(renderer->egl_display, NULL, NULL)) {
        set_error(error, error_size, "eglInitialize failed (0x%x)", eglGetError());
        return -1;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(renderer->egl_display, config_attributes, &config, 1, &count) || count < 1) {
        set_error(error, error_size, "No GLES2 EGL config found (0x%x)", eglGetError());
        return -1;
    }
    renderer->egl_context = eglCreateContext(renderer->egl_display, config,
                                             EGL_NO_CONTEXT, context_attributes);
    if (renderer->egl_context == EGL_NO_CONTEXT) {
        set_error(error, error_size, "eglCreateContext failed (0x%x)", eglGetError());
        return -1;
    }
    renderer->egl_surface = eglCreateWindowSurface(renderer->egl_display, config,
                                                  (EGLNativeWindowType)renderer->gbm_surface, NULL);
    if (renderer->egl_surface == EGL_NO_SURFACE) {
        set_error(error, error_size, "eglCreateWindowSurface failed (0x%x)", eglGetError());
        return -1;
    }
    if (!eglMakeCurrent(renderer->egl_display, renderer->egl_surface,
                        renderer->egl_surface, renderer->egl_context)) {
        set_error(error, error_size, "eglMakeCurrent failed (0x%x)", eglGetError());
        return -1;
    }
    printf("GPU: %s, %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER));
    {
        GLint read_format = 0;
        GLint read_type = 0;
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &read_format);
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &read_type);
        printf("Readback: format 0x%x, type 0x%x\n", read_format, read_type);
    }
    return 0;
}

static int create_chunks(Renderer *renderer, char *error, size_t error_size)
{
    GLint max_texture;
    GLint max_units;
    int i;
    if (renderer->direct == 4 || renderer->direct == 5) {
        renderer->chunk_count = 1;
        renderer->chunk_columns = 1;
        renderer->chunk_rows = 1;
        renderer->chunk_tile_columns = 1;
        renderer->chunk_tile_rows = 1;
        renderer->tile_w = renderer->source_w;
        renderer->tile_h = renderer->source_h;
        printf("NV12 DMA-BUF display: %dx%d\n", renderer->source_w, renderer->source_h);
        return 0;
    }
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &max_units);
    if (renderer->direct == 7) {
        if (renderer->source_w > max_texture || renderer->source_h > max_texture ||
            max_units < 2) {
            set_error(error, error_size, "Stereo video exceeds the GPU texture limits");
            return -1;
        }
        renderer->chunk_count = 2;
        renderer->chunk_columns = 2;
        renderer->chunk_rows = 1;
        renderer->chunk_tile_columns = 1;
        renderer->chunk_tile_rows = 1;
        renderer->tile_w = renderer->source_w;
        renderer->tile_h = renderer->source_h;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (i = 0; i < 2; ++i) {
            Chunk *chunk = &renderer->chunks[i];
            chunk->first_column = i;
            chunk->columns = 1;
            chunk->rows = 1;
            chunk->width = renderer->source_w;
            chunk->height = renderer->source_h;
            glGenTextures(1, &chunk->texture);
            glBindTexture(GL_TEXTURE_2D, chunk->texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, chunk->width, chunk->height,
                         0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
            if (glGetError() != GL_NO_ERROR) {
                set_error(error, error_size, "GPU failed to allocate stereo textures");
                return -1;
            }
        }
        printf("Stereo GPU textures: 2x %dx%d\n", renderer->source_w, renderer->source_h);
        return 0;
    }
    if (renderer->direct == 8 || renderer->direct == 9) {
        int planes = renderer->direct == 8 ? 3 : 6;
        if (renderer->source_w > max_texture || renderer->source_h > max_texture ||
            (renderer->source_w % 2) != 0 || (renderer->source_h % 2) != 0 ||
            max_units < planes) {
            set_error(error, error_size, "YUV420 video exceeds the GPU texture limits");
            return -1;
        }
        renderer->chunk_count = planes;
        if (renderer->direct == 8) {
            renderer->chunk_columns = 1;
            renderer->chunk_rows = 1;
            renderer->chunk_tile_columns = renderer->quilt.columns;
            renderer->chunk_tile_rows = renderer->quilt.rows;
            renderer->tile_w = renderer->source_w / renderer->quilt.columns;
            renderer->tile_h = renderer->source_h / renderer->quilt.rows;
            if (renderer->tile_w < 1 || renderer->tile_h < 1) {
                set_error(error, error_size, "Invalid quilt tile dimensions");
                return -1;
            }
        } else {
            renderer->chunk_columns = 2;
            renderer->chunk_rows = 1;
            renderer->chunk_tile_columns = 1;
            renderer->chunk_tile_rows = 1;
            renderer->tile_w = renderer->source_w;
            renderer->tile_h = renderer->source_h;
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (i = 0; i < planes; ++i) {
            Chunk *chunk = &renderer->chunks[i];
            int plane = i % 3;
            chunk->width = plane == 0 ? renderer->source_w : renderer->source_w / 2;
            chunk->height = plane == 0 ? renderer->source_h : renderer->source_h / 2;
            chunk->source_x = 0;
            chunk->source_y = 0;
            glGenTextures(1, &chunk->texture);
            glBindTexture(GL_TEXTURE_2D, chunk->texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, chunk->width, chunk->height,
                         0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
            if (glGetError() != GL_NO_ERROR) {
                set_error(error, error_size, "GPU failed to allocate YUV420 textures");
                return -1;
            }
        }
        printf("%s YUV420 GPU textures: %sY %dx%d, UV %dx%d\n",
               renderer->direct == 8 ? "Quilt" : "Stereo",
               renderer->direct == 8 ? "" : "2x ",
               renderer->source_w, renderer->source_h,
               renderer->source_w / 2, renderer->source_h / 2);
        return 0;
    }
    renderer->tile_w = renderer->source_w / renderer->quilt.columns;
    renderer->tile_h = renderer->source_h / renderer->quilt.rows;
    if (renderer->tile_w < 1 || renderer->tile_h < 1 || renderer->tile_w > max_texture ||
        renderer->tile_h > max_texture) {
        set_error(error, error_size, "A single quilt view exceeds the GPU texture limit of %d", max_texture);
        return -1;
    }
    renderer->chunk_tile_columns = max_texture /
        ((renderer->source_w + renderer->quilt.columns - 1) / renderer->quilt.columns);
    renderer->chunk_tile_rows = max_texture /
        ((renderer->source_h + renderer->quilt.rows - 1) / renderer->quilt.rows);
    if (renderer->chunk_tile_columns < 1)
        renderer->chunk_tile_columns = 1;
    if (renderer->chunk_tile_rows < 1)
        renderer->chunk_tile_rows = 1;
    if (renderer->chunk_tile_columns > renderer->quilt.columns)
        renderer->chunk_tile_columns = renderer->quilt.columns;
    if (renderer->chunk_tile_rows > renderer->quilt.rows)
        renderer->chunk_tile_rows = renderer->quilt.rows;
    {
        const size_t max_chunk_bytes = 64 * 1024 * 1024;
        size_t tile_bytes = (size_t)renderer->tile_w * renderer->tile_h * 3;
        size_t row_bytes = (size_t)renderer->chunk_tile_columns * tile_bytes;
        size_t max_rows;
        if (row_bytes > max_chunk_bytes && renderer->chunk_tile_columns > 1) {
            renderer->chunk_tile_columns = max_chunk_bytes / tile_bytes;
            if (renderer->chunk_tile_columns < 1)
                renderer->chunk_tile_columns = 1;
            row_bytes = (size_t)renderer->chunk_tile_columns * tile_bytes;
        }
        max_rows = row_bytes ? max_chunk_bytes / row_bytes : 1;
        if (max_rows < 1)
            max_rows = 1;
        if (renderer->chunk_tile_rows > (int)max_rows)
            renderer->chunk_tile_rows = (int)max_rows;
    }
    renderer->chunk_columns = (renderer->quilt.columns + renderer->chunk_tile_columns - 1) /
                              renderer->chunk_tile_columns;
    renderer->chunk_rows = (renderer->quilt.rows + renderer->chunk_tile_rows - 1) /
                           renderer->chunk_tile_rows;
    renderer->chunk_count = renderer->chunk_columns * renderer->chunk_rows;
    if (renderer->chunk_count > LKG_MAX_CHUNKS || renderer->chunk_count > max_units) {
        set_error(error, error_size, "Quilt requires %d textures, but the GPU supports only %d",
                  renderer->chunk_count, max_units < LKG_MAX_CHUNKS ? max_units : LKG_MAX_CHUNKS);
        return -1;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (i = 0; i < renderer->chunk_count; ++i) {
        Chunk *chunk = &renderer->chunks[i];
        int chunk_x = i % renderer->chunk_columns;
        int chunk_y = i / renderer->chunk_columns;
        chunk->first_column = chunk_x * renderer->chunk_tile_columns;
        chunk->first_row = chunk_y * renderer->chunk_tile_rows;
        chunk->columns = renderer->quilt.columns - chunk->first_column;
        chunk->rows = renderer->quilt.rows - chunk->first_row;
        if (chunk->columns > renderer->chunk_tile_columns)
            chunk->columns = renderer->chunk_tile_columns;
        if (chunk->rows > renderer->chunk_tile_rows)
            chunk->rows = renderer->chunk_tile_rows;
        chunk->source_x = (int)((int64_t)chunk->first_column * renderer->source_w /
                                renderer->quilt.columns);
        chunk->source_y = (int)((int64_t)chunk->first_row * renderer->source_h /
                                renderer->quilt.rows);
        chunk->width = (int)((int64_t)(chunk->first_column + chunk->columns) *
                             renderer->source_w / renderer->quilt.columns) - chunk->source_x;
        chunk->height = (int)((int64_t)(chunk->first_row + chunk->rows) *
                              renderer->source_h / renderer->quilt.rows) - chunk->source_y;
        glGenTextures(1, &chunk->texture);
        glBindTexture(GL_TEXTURE_2D, chunk->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0,
                     renderer->direct == 6 ||
                     (!renderer->direct && renderer->unpack_subimage) ? GL_RGB : GL_RGBA,
                     chunk->width, chunk->height, 0,
                     renderer->direct == 6 ||
                     (!renderer->direct && renderer->unpack_subimage) ? GL_RGB : GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        if (glGetError() != GL_NO_ERROR) {
            set_error(error, error_size, "GPU failed to allocate quilt textures");
            return -1;
        }
        if (renderer->startup &&
            renderer_progress(renderer, 0.1 * (i + 1) / renderer->chunk_count) != 0) {
            set_error(error, error_size, "Failed to display initialization progress");
            return -1;
        }
    }
    if (!renderer->direct && !renderer->unpack_subimage) {
        size_t max_chunk_size = 0;
        for (i = 0; i < renderer->chunk_count; ++i) {
            size_t size = (size_t)renderer->chunks[i].width *
                          renderer->chunks[i].height * 4;
            if (size > max_chunk_size)
                max_chunk_size = size;
        }
        renderer->upload_buffer = malloc(max_chunk_size);
        if (!renderer->upload_buffer) {
            set_error(error, error_size, "Failed to allocate texture upload buffer");
            return -1;
        }
    }
    printf("GPU chunks: %dx%d (%d textures), view size %dx%d, texture limit %d\n",
           renderer->chunk_columns, renderer->chunk_rows, renderer->chunk_count,
           renderer->tile_w, renderer->tile_h, max_texture);
    return 0;
}

static int mkdir_parents(const char *file_path)
{
    char directory[PATH_MAX];
    char *slash;
    char *cursor;
    if (snprintf(directory, sizeof(directory), "%s", file_path) >= (int)sizeof(directory))
        return -1;
    slash = strrchr(directory, '/');
    if (!slash || slash == directory)
        return 0;
    *slash = '\0';
    for (cursor = directory + 1; *cursor; ++cursor) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir(directory, 0755) != 0 && errno != EEXIST)
                return -1;
            *cursor = '/';
        }
    }
    if (mkdir(directory, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int renderer_thumbnail_prepare(Renderer *renderer, const char *media_path,
                               const char *cache_dir)
{
    int view;
    int view_x;
    int view_y;
    if (renderer->direct == 7 || renderer->direct == 9)
        return 0;
    if (renderer->thumbnail_active ||
        thumbnail_cache_path_in(media_path, cache_dir, renderer->thumbnail_path,
                                sizeof(renderer->thumbnail_path)) != 0 ||
        access(renderer->thumbnail_path, R_OK) == 0)
        return 0;
    if (mkdir_parents(renderer->thumbnail_path) != 0)
        return -1;
    view = (renderer->quilt.columns * renderer->quilt.rows - 1) / 2;
    view_x = view % renderer->quilt.columns;
    view_y = view / renderer->quilt.columns;
    renderer->thumbnail_x = view_x * renderer->source_w / renderer->quilt.columns;
    renderer->thumbnail_y = view_y * renderer->source_h / renderer->quilt.rows;
    renderer->thumbnail_w = (view_x + 1) * renderer->source_w / renderer->quilt.columns -
                            renderer->thumbnail_x;
    renderer->thumbnail_h = (view_y + 1) * renderer->source_h / renderer->quilt.rows -
                            renderer->thumbnail_y;
    renderer->thumbnail_tile = malloc((size_t)renderer->thumbnail_w *
                                      renderer->thumbnail_h * 4);
    if (!renderer->thumbnail_tile)
        return -1;
    snprintf(renderer->thumbnail_temporary, sizeof(renderer->thumbnail_temporary),
             "%s.tmp.%d", renderer->thumbnail_path, getpid());
    renderer->thumbnail_captured = 0;
    renderer->thumbnail_active = 1;
    return 0;
}

static void thumbnail_collect_rows(Renderer *renderer, int source_y, int rows,
                                   const uint8_t *rgb)
{
    int start;
    int end;
    int row;
    if (!renderer->thumbnail_active || renderer->thumbnail_captured)
        return;
    start = source_y > renderer->thumbnail_y ? source_y : renderer->thumbnail_y;
    end = source_y + rows < renderer->thumbnail_y + renderer->thumbnail_h ?
          source_y + rows : renderer->thumbnail_y + renderer->thumbnail_h;
    for (row = start; row < end; ++row) {
        const uint8_t *source = rgb + ((size_t)(row - source_y) * renderer->source_w +
                                       renderer->thumbnail_x) * 3;
        uint8_t *target = renderer->thumbnail_tile +
                          (size_t)(row - renderer->thumbnail_y) * renderer->thumbnail_w * 4;
        int column;
        for (column = 0; column < renderer->thumbnail_w; ++column) {
            target[column * 4] = source[column * 3];
            target[column * 4 + 1] = source[column * 3 + 1];
            target[column * 4 + 2] = source[column * 3 + 2];
            target[column * 4 + 3] = 255;
        }
    }
    if (end == renderer->thumbnail_y + renderer->thumbnail_h)
        renderer->thumbnail_captured = 1;
}

int renderer_thumbnail_finish(Renderer *renderer)
{
    int view;
    int result;
    if (!renderer->thumbnail_active)
        return 0;
    result = renderer->thumbnail_captured ? 0 : -1;
    if (result != 0)
        fprintf(stderr, "Thumbnail capture is incomplete\n");
    view = (renderer->quilt.columns * renderer->quilt.rows - 1) / 2;
    if (result == 0)
        result = thumbnail_frame_write(renderer->thumbnail_temporary,
                                       renderer->quilt.columns, renderer->quilt.rows, view,
                                       renderer->quilt.aspect, renderer->thumbnail_tile,
                                       renderer->thumbnail_w, renderer->thumbnail_h, 450, 800);
    if (result != 0 && renderer->thumbnail_captured)
        fprintf(stderr, "Thumbnail frame write failed: %s\n", strerror(errno));
    if (result == 0 && rename(renderer->thumbnail_temporary, renderer->thumbnail_path) != 0) {
        fprintf(stderr, "Thumbnail cache rename failed: %s\n", strerror(errno));
        result = -1;
    }
    if (result != 0)
        unlink(renderer->thumbnail_temporary);
    else {
        printf("Thumbnail cache: %s\n", renderer->thumbnail_path);
        fflush(stdout);
    }
    free(renderer->thumbnail_tile);
    renderer->thumbnail_tile = NULL;
    renderer->thumbnail_active = 0;
    return result;
}

static uint8_t clamp_byte(float value)
{
    if (value <= 0.0f)
        return 0;
    if (value >= 255.0f)
        return 255;
    return (uint8_t)(value + 0.5f);
}

static void thumbnail_collect_yuv420(Renderer *renderer, const uint8_t *frame)
{
    int w = renderer->source_w;
    int h = renderer->source_h;
    int uv_w = w / 2;
    const uint8_t *y_plane = frame;
    const uint8_t *u_plane = frame + (size_t)w * (size_t)h;
    const uint8_t *v_plane = u_plane + (size_t)uv_w * (size_t)(h / 2);
    float y_scale = renderer->yuv_full_range ? 1.0f : (255.0f / 219.0f);
    float y_bias = renderer->yuv_full_range ? 0.0f : -16.0f;
    float c_scale = renderer->yuv_full_range ? 1.0f : (255.0f / 224.0f);
    float cr_r = (renderer->yuv_bt709 ? 1.5748f : 1.402f) * c_scale;
    float cb_g = (renderer->yuv_bt709 ? -0.187324f : -0.344136f) * c_scale;
    float cr_g = (renderer->yuv_bt709 ? -0.468124f : -0.714136f) * c_scale;
    float cb_b = (renderer->yuv_bt709 ? 1.8556f : 1.772f) * c_scale;
    int row;
    if (!renderer->thumbnail_active || renderer->thumbnail_captured)
        return;
    for (row = 0; row < renderer->thumbnail_h; ++row) {
        int sy = renderer->thumbnail_y + row;
        const uint8_t *y_row = y_plane + (size_t)sy * (size_t)w + renderer->thumbnail_x;
        const uint8_t *u_row = u_plane + (size_t)(sy / 2) * (size_t)uv_w;
        const uint8_t *v_row = v_plane + (size_t)(sy / 2) * (size_t)uv_w;
        uint8_t *target = renderer->thumbnail_tile +
                          (size_t)row * (size_t)renderer->thumbnail_w * 4;
        int col;
        for (col = 0; col < renderer->thumbnail_w; ++col) {
            int sx = renderer->thumbnail_x + col;
            float y = ((float)y_row[col] + y_bias) * y_scale;
            float u = (float)u_row[sx / 2] - 128.0f;
            float v = (float)v_row[sx / 2] - 128.0f;
            target[col * 4] = clamp_byte(y + cr_r * v);
            target[col * 4 + 1] = clamp_byte(y + cb_g * u + cr_g * v);
            target[col * 4 + 2] = clamp_byte(y + cb_b * u);
            target[col * 4 + 3] = 255;
        }
    }
    renderer->thumbnail_captured = 1;
}

static int upload_chunks(Renderer *renderer, const uint8_t *frame)
{
    int i;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (renderer->direct == 8 || renderer->direct == 9) {
        int w = renderer->source_w;
        int h = renderer->source_h;
        int uv_w = w / 2;
        int uv_h = h / 2;
        size_t y_size = (size_t)w * (size_t)h;
        size_t uv_size = (size_t)uv_w * (size_t)uv_h;
        size_t eye_size = y_size + uv_size * 2;
        int eyes = renderer->direct == 8 ? 1 : 2;
        int eye;
        if (renderer->direct == 8)
            thumbnail_collect_yuv420(renderer, frame);
        for (eye = 0; eye < eyes; ++eye) {
            const uint8_t *base = frame + (size_t)eye * eye_size;
            glBindTexture(GL_TEXTURE_2D, renderer->chunks[eye * 3].texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                            GL_LUMINANCE, GL_UNSIGNED_BYTE, base);
            glBindTexture(GL_TEXTURE_2D, renderer->chunks[eye * 3 + 1].texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_w, uv_h,
                            GL_LUMINANCE, GL_UNSIGNED_BYTE, base + y_size);
            glBindTexture(GL_TEXTURE_2D, renderer->chunks[eye * 3 + 2].texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_w, uv_h,
                            GL_LUMINANCE, GL_UNSIGNED_BYTE, base + y_size + uv_size);
        }
        return glGetError() == GL_NO_ERROR ? 0 : -1;
    }
    if (renderer->direct == 7) {
        size_t eye_size = (size_t)renderer->source_w * renderer->source_h * 3;
        for (i = 0; i < 2; ++i) {
            glBindTexture(GL_TEXTURE_2D, renderer->chunks[i].texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                            renderer->source_w, renderer->source_h,
                            GL_RGB, GL_UNSIGNED_BYTE, frame + (size_t)i * eye_size);
        }
        return glGetError() == GL_NO_ERROR ? 0 : -1;
    }
    if (renderer->direct) {
        glBindTexture(GL_TEXTURE_2D, renderer->chunks[0].texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, renderer->source_w, renderer->source_h,
                        renderer->direct == 6 ? GL_RGB : GL_BGRA_EXT,
                        GL_UNSIGNED_BYTE, frame);
        return glGetError() == GL_NO_ERROR ? 0 : -1;
    }
    thumbnail_collect_rows(renderer, 0, renderer->source_h, frame);
    for (i = 0; i < renderer->chunk_count; ++i) {
        Chunk *chunk = &renderer->chunks[i];
        int width = chunk->width;
        int height = chunk->height;
        glBindTexture(GL_TEXTURE_2D, chunk->texture);
        if (renderer->unpack_subimage) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, renderer->source_w);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, chunk->source_x);
            glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, chunk->source_y);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                            GL_RGB, GL_UNSIGNED_BYTE, frame);
        } else {
            int row;
            for (row = 0; row < height; ++row) {
                size_t source_x = (size_t)chunk->source_x;
                size_t source_y = (size_t)chunk->source_y + row;
                const uint8_t *source = frame + (source_y * renderer->source_w + source_x) * 3;
                uint8_t *target = renderer->upload_buffer + (size_t)row * width * 4;
                int column;
                for (column = 0; column < width; ++column) {
                    target[column * 4] = source[column * 3];
                    target[column * 4 + 1] = source[column * 3 + 1];
                    target[column * 4 + 2] = source[column * 3 + 2];
                    target[column * 4 + 3] = 255;
                }
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                            GL_RGBA, GL_UNSIGNED_BYTE, renderer->upload_buffer);
        }
    }
    if (renderer->unpack_subimage) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, 0);
    }
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}

int renderer_upload_rows(Renderer *renderer, int source_y, int rows,
                         const uint8_t *rgb, char *error, size_t error_size)
{
    int i;
    if (source_y < 0 || rows <= 0 || source_y + rows > renderer->source_h) {
        set_error(error, error_size, "Invalid image row upload range");
        return -1;
    }
    thumbnail_collect_rows(renderer, source_y, rows, rgb);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (i = 0; i < renderer->chunk_count; ++i) {
        Chunk *chunk = &renderer->chunks[i];
        int chunk_y = chunk->source_y;
        int chunk_height = chunk->height;
        int start = source_y > chunk_y ? source_y : chunk_y;
        int end = source_y + rows < chunk_y + chunk_height ?
                  source_y + rows : chunk_y + chunk_height;
        int width = chunk->width;
        if (start >= end)
            continue;
        glBindTexture(GL_TEXTURE_2D, chunk->texture);
        if (renderer->unpack_subimage) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, renderer->source_w);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, chunk->source_x);
            glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, start - source_y);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, start - chunk_y,
                            width, end - start, GL_RGB, GL_UNSIGNED_BYTE, rgb);
        } else {
            int row;
            for (row = start; row < end; ++row) {
                const uint8_t *source = rgb + ((size_t)(row - source_y) * renderer->source_w +
                                               (size_t)chunk->source_x) * 3;
                uint8_t *target = renderer->upload_buffer + (size_t)(row - start) * width * 4;
                int column;
                for (column = 0; column < width; ++column) {
                    target[column * 4] = source[column * 3];
                    target[column * 4 + 1] = source[column * 3 + 1];
                    target[column * 4 + 2] = source[column * 3 + 2];
                    target[column * 4 + 3] = 255;
                }
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, start - chunk_y,
                            width, end - start, GL_RGBA, GL_UNSIGNED_BYTE,
                            renderer->upload_buffer);
        }
    }
    if (renderer->unpack_subimage) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, 0);
    }
    if (glGetError() != GL_NO_ERROR) {
        set_error(error, error_size, "Failed to upload image texture chunk");
        return -1;
    }
    return 0;
}

static void set_uniforms(Renderer *renderer)
{
    const RendererUniforms *uniforms = &renderer->uniforms;
    float pitch = renderer->calibration.pitch * renderer->calibration.screen_w /
                  renderer->calibration.dpi *
                  sinf(atanf(fabsf(renderer->calibration.slope)));
    float tilt = renderer->calibration.screen_h /
                 (renderer->calibration.screen_w * renderer->calibration.slope);
    int i;

    glUseProgram(renderer->program);
    if (renderer->direct >= 4 && renderer->direct <= 5)
        glUniform1i(uniforms->nv12_uv, 1);
    for (i = 0; i < renderer->chunk_count; ++i)
        glUniform1i(uniforms->chunk[i], i);
    glUniform2f(uniforms->resolution, renderer->mode.hdisplay, renderer->mode.vdisplay);
    glUniform2f(uniforms->source_size, renderer->source_w, renderer->source_h);
    glUniform2f(uniforms->tile_size, renderer->tile_w, renderer->tile_h);
    glUniform1i(uniforms->columns, renderer->quilt.columns);
    glUniform1i(uniforms->rows, renderer->quilt.rows);
    glUniform1i(uniforms->views, renderer->quilt.views);
    glUniform1i(uniforms->chunk_tile_columns, renderer->chunk_tile_columns);
    glUniform1i(uniforms->chunk_tile_rows, renderer->chunk_tile_rows);
    glUniform1f(uniforms->aspect, renderer->quilt.aspect);
    glUniform1f(uniforms->zoom, renderer->quilt.zoom);
    glUniform2f(uniforms->position, renderer->quilt.position_x, -renderer->quilt.position_y);
    glUniform1f(uniforms->focus, renderer->quilt.focus);
    glUniform1f(uniforms->stereo_disparity, renderer->quilt.stereo_disparity);
    glUniform1f(uniforms->pitch, pitch);
    glUniform1f(uniforms->tilt, tilt);
    glUniform1f(uniforms->center, renderer->calibration.center);
    glUniform1f(uniforms->fringe, renderer->calibration.fringe);
    glUniform1i(uniforms->inv_view, renderer->calibration.inv_view);
    glUniform1i(uniforms->flip_x, renderer->calibration.flip_image_x);
    glUniform1i(uniforms->flip_y, renderer->calibration.flip_image_y);
    glUniform1i(uniforms->flip_subp, renderer->calibration.flip_subp);
    glUniform1i(uniforms->reverse_views, renderer->quilt.reverse_views);
    glUniform1i(uniforms->clamp_edges, renderer->quilt.clamp_edges);
    if (uniforms->yuv_y >= 0 && uniforms->yuv_coeffs >= 0) {
        float y_scale = renderer->yuv_full_range ? 1.0f : (255.0f / 219.0f);
        float y_offset = renderer->yuv_full_range ? 0.0f : (-16.0f / 219.0f);
        float c_scale = renderer->yuv_full_range ? 1.0f : (255.0f / 224.0f);
        float cr_r = (renderer->yuv_bt709 ? 1.5748f : 1.402f) * c_scale;
        float cb_g = (renderer->yuv_bt709 ? -0.187324f : -0.344136f) * c_scale;
        float cr_g = (renderer->yuv_bt709 ? -0.468124f : -0.714136f) * c_scale;
        float cb_b = (renderer->yuv_bt709 ? 1.8556f : 1.772f) * c_scale;
        glUniform2f(uniforms->yuv_y, y_scale, y_offset);
        glUniform4f(uniforms->yuv_coeffs, cr_r, cb_g, cr_g, cb_b);
    }
}

void renderer_set_yuv_colorimetry(Renderer *renderer, int bt709, int full_range)
{
    int next_bt709 = bt709 ? 1 : 0;
    int next_full = full_range ? 1 : 0;
    if (!renderer)
        return;
    if (renderer->yuv_bt709 == next_bt709 && renderer->yuv_full_range == next_full)
        return;
    renderer->yuv_bt709 = next_bt709;
    renderer->yuv_full_range = next_full;
    if (renderer->program && renderer->uniforms.yuv_y >= 0 &&
        renderer->uniforms.yuv_coeffs >= 0) {
        float y_scale = next_full ? 1.0f : (255.0f / 219.0f);
        float y_offset = next_full ? 0.0f : (-16.0f / 219.0f);
        float c_scale = next_full ? 1.0f : (255.0f / 224.0f);
        float cr_r = (next_bt709 ? 1.5748f : 1.402f) * c_scale;
        float cb_g = (next_bt709 ? -0.187324f : -0.344136f) * c_scale;
        float cr_g = (next_bt709 ? -0.468124f : -0.714136f) * c_scale;
        float cb_b = (next_bt709 ? 1.8556f : 1.772f) * c_scale;
        glUseProgram(renderer->program);
        glUniform2f(renderer->uniforms.yuv_y, y_scale, y_offset);
        glUniform4f(renderer->uniforms.yuv_coeffs, cr_r, cb_g, cr_g, cb_b);
    }
}

static void draw_scene_internal(Renderer *renderer, int flip_output_y)
{
    static const float vertices[] = {
        -1, -1, 1, -1, -1, 1,
        -1, 1, 1, -1, 1, 1
    };
    int i;
    if (wait_for_flip(renderer, NULL, 0) != 0)
        return;
    glViewport(0, 0, renderer->mode.hdisplay, renderer->mode.vdisplay);
    if (!renderer->direct || renderer->direct == 8) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glUseProgram(renderer->program);
    if (renderer->uniforms.flip_output_y >= 0)
        glUniform1i(renderer->uniforms.flip_output_y, flip_output_y);
    for (i = 0; i < renderer->chunk_count; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, renderer->chunks[i].texture);
    }
    if (renderer->direct >= 4 && renderer->direct <= 5) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, renderer->nv12_uv_texture);
    }
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnable(GL_SCISSOR_TEST);
    {
        const int bands = (renderer->direct == 0 || renderer->direct == 8) ? 8 : 1;
        int band;
        for (band = 0; band < bands; ++band) {
            int y = band * renderer->mode.vdisplay / bands;
            int next_y = (band + 1) * renderer->mode.vdisplay / bands;
            glScissor(0, y, renderer->mode.hdisplay, next_y - y);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glFlush();
        }
    }
    glDisable(GL_SCISSOR_TEST);
}

static void draw_scene(Renderer *renderer)
{
    draw_scene_internal(renderer, 0);
}

static void framebuffer_destroy(struct gbm_bo *bo, void *data)
{
    Framebuffer *framebuffer = data;
    (void)bo;
    if (framebuffer) {
        if (framebuffer->id)
            drmModeRmFB(framebuffer->fd, framebuffer->id);
        free(framebuffer);
    }
}

static Framebuffer *framebuffer_for_bo(Renderer *renderer, struct gbm_bo *bo)
{
    Framebuffer *framebuffer = gbm_bo_get_user_data(bo);
    uint32_t handles[4] = {0};
    uint32_t strides[4] = {0};
    uint32_t offsets[4] = {0};
    if (framebuffer)
        return framebuffer;
    framebuffer = calloc(1, sizeof(*framebuffer));
    if (!framebuffer)
        return NULL;
    framebuffer->fd = renderer->fd;
    handles[0] = gbm_bo_get_handle(bo).u32;
    strides[0] = gbm_bo_get_stride(bo);
    offsets[0] = 0;
    if (drmModeAddFB2(renderer->fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
                      gbm_bo_get_format(bo), handles, strides, offsets,
                      &framebuffer->id, 0) != 0) {
        free(framebuffer);
        return NULL;
    }
    gbm_bo_set_user_data(bo, framebuffer, framebuffer_destroy);
    return framebuffer;
}

static void page_flip_complete(int fd, unsigned int sequence, unsigned int seconds,
                               unsigned int usec, void *data)
{
    Renderer *renderer = data;
    (void)fd;
    (void)sequence;
    if (renderer->profile_dma) {
        double timestamp = (double)seconds + (double)usec / 1000000.0;
        if (renderer->profile_last_vblank > 0) {
            double interval = timestamp - renderer->profile_last_vblank;
            if (interval > 0 && interval < 0.1)
                renderer->profile_vblank_interval += interval;
        }
        renderer->profile_last_vblank = timestamp;
    }
    if (renderer->scanout_bo)
        gbm_surface_release_buffer(renderer->gbm_surface, renderer->scanout_bo);
    renderer->scanout_bo = renderer->pending_bo;
    renderer->pending_bo = NULL;
    renderer->flip_pending = 0;
}

static int wait_for_flip(Renderer *renderer, char *error, size_t error_size)
{
    drmEventContext context;
    memset(&context, 0, sizeof(context));
    context.version = DRM_EVENT_CONTEXT_VERSION;
    context.page_flip_handler = page_flip_complete;
    while (renderer->flip_pending) {
        struct pollfd fd = {renderer->fd, POLLIN, 0};
        int result = poll(&fd, 1, 2000);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0) {
            set_error(error, error_size, "Waiting for DRM page flip %s",
                      result == 0 ? "timed out" : strerror(errno));
            return -1;
        }
        if (drmHandleEvent(renderer->fd, &context) != 0) {
            set_error(error, error_size, "drmHandleEvent failed");
            return -1;
        }
    }
    return 0;
}

static int present(Renderer *renderer, char *error, size_t error_size)
{
    struct gbm_bo *bo;
    Framebuffer *framebuffer;
    double wait_started = renderer->profile_dma ? monotonic_seconds() : 0;

    if (wait_for_flip(renderer, error, error_size) != 0)
        return -1;
    if (renderer->profile_dma)
        renderer->profile_dma_wait += monotonic_seconds() - wait_started;
    if (renderer->paused) {
        glViewport(0, 0, renderer->mode.hdisplay, renderer->mode.vdisplay);
        glEnable(GL_SCISSOR_TEST);
        draw_pause_geometry(renderer);
        glDisable(GL_SCISSOR_TEST);
    }
    if (!eglSwapBuffers(renderer->egl_display, renderer->egl_surface)) {
        set_error(error, error_size, "eglSwapBuffers failed (0x%x)", eglGetError());
        return -1;
    }
    bo = gbm_surface_lock_front_buffer(renderer->gbm_surface);
    if (!bo) {
        set_error(error, error_size, "gbm_surface_lock_front_buffer failed");
        return -1;
    }
    framebuffer = framebuffer_for_bo(renderer, bo);
    if (!framebuffer) {
        gbm_surface_release_buffer(renderer->gbm_surface, bo);
        set_error(error, error_size, "drmModeAddFB2 failed: %s", strerror(errno));
        return -1;
    }
    if (!renderer->crtc_set) {
        if (drmModeSetCrtc(renderer->fd, renderer->crtc_id, framebuffer->id, 0, 0,
                           &renderer->connector_id, 1, &renderer->mode) != 0) {
            gbm_surface_release_buffer(renderer->gbm_surface, bo);
            set_error(error, error_size, "drmModeSetCrtc failed: %s (stop Sway or other display owners)", strerror(errno));
            return -1;
        }
        if (renderer->scanout_bo)
            gbm_surface_release_buffer(renderer->gbm_surface, renderer->scanout_bo);
        renderer->scanout_bo = bo;
        renderer->crtc_set = 1;
    } else {
        if (drmModePageFlip(renderer->fd, renderer->crtc_id, framebuffer->id,
                            DRM_MODE_PAGE_FLIP_EVENT, renderer) != 0) {
            gbm_surface_release_buffer(renderer->gbm_surface, bo);
            set_error(error, error_size, "drmModePageFlip failed: %s", strerror(errno));
            return -1;
        }
        renderer->pending_bo = bo;
        renderer->flip_pending = 1;
    }
    if (renderer->retire_buffer && renderer->retire_buffer != framebuffer->id) {
        drmModeRmFB(renderer->fd, renderer->retire_buffer);
        if (renderer->saved_crtc)
            renderer->saved_crtc->buffer_id = 0;
        renderer->retire_buffer = 0;
    }
    if (renderer->retire_keeper > 0) {
        kill(renderer->retire_keeper, SIGTERM);
        renderer->retire_keeper = 0;
    }
    if (renderer->retire_pid_path) {
        unlink(renderer->retire_pid_path);
        free(renderer->retire_pid_path);
        renderer->retire_pid_path = NULL;
    }
    return 0;
}

static void clear_error_rect(Renderer *renderer, int x, int y, int width, int height,
                             float red, float green, float blue)
{
    (void)renderer;
    glScissor(x, y, width, height);
    glClearColor(red, green, blue, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void draw_pause_geometry(Renderer *renderer)
{
    int screen_w = renderer->mode.hdisplay;
    int screen_h = renderer->mode.vdisplay;
    if (renderer->paused) {
        clear_error_rect(renderer, screen_w - 164, screen_h - 164,
                         140, 140, 0.05f, 0.05f, 0.05f);
        clear_error_rect(renderer, screen_w - 88, screen_h - 134, 28, 80, 1, 1, 1);
        clear_error_rect(renderer, screen_w - 128, screen_h - 134, 28, 80, 1, 1, 1);
    }
}

static void draw_error_geometry(Renderer *renderer)
{
    int screen_w = renderer->mode.hdisplay;
    int screen_h = renderer->mode.vdisplay;
    int center_x = screen_w / 2;
    int center_y = screen_h / 2;
    const int scale = 3;
    int row;
    glViewport(0, 0, screen_w, screen_h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(16.0f / 255.0f, 16.0f / 255.0f, 20.0f / 255.0f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    for (row = 0; row <= 162; ++row) {
        int half_width = row < 144 ? 11 + 88 * row / 144 :
                                     99 - 11 * (row - 144) / 18;
        int top = center_y + (-82 + row) * scale;
        clear_error_rect(renderer, center_x - half_width * scale,
                         screen_h - top - scale, (half_width * 2 + 1) * scale,
                         scale, 1.0f, 0.8f, 0.0f);
    }
    clear_error_rect(renderer, center_x - 12 * scale,
                     screen_h - center_y - 20 * scale,
                     24 * scale, 70 * scale,
                     16.0f / 255.0f, 16.0f / 255.0f, 20.0f / 255.0f);
    clear_error_rect(renderer, center_x - 12 * scale,
                     screen_h - center_y - 62 * scale,
                     24 * scale, 24 * scale,
                     16.0f / 255.0f, 16.0f / 255.0f, 20.0f / 255.0f);
    glDisable(GL_SCISSOR_TEST);
}

int renderer_show_error(Renderer *renderer, char *error, size_t error_size)
{
    overlay_disable(renderer);
    renderer->error_screen = 1;
    draw_error_geometry(renderer);
    if (present(renderer, error, error_size) != 0)
        return -1;
    renderer->progress_has_frame = 1;
    return 0;
}

static void capture_progress_background(Renderer *renderer)
{
    char error[256] = {0};
    if (!renderer->progress_has_frame)
        return;
    if (create_background_program(renderer, error, sizeof(error)) != 0)
        return;
    if (renderer->error_screen)
        draw_error_geometry(renderer);
    else
        draw_scene(renderer);
    if (!renderer->background_texture)
        glGenTextures(1, &renderer->background_texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, renderer->background_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0,
                     renderer->mode.hdisplay, renderer->mode.vdisplay, 0);
}

static void draw_frame_count(Renderer *renderer, uint64_t current, uint64_t total, int bar_y)
{
    static const char *glyphs[11][7] = {
        {"01110", "10001", "10011", "10101", "11001", "10001", "01110"},
        {"00100", "01100", "00100", "00100", "00100", "00100", "01110"},
        {"01110", "10001", "00001", "00010", "00100", "01000", "11111"},
        {"11110", "00001", "00001", "01110", "00001", "00001", "11110"},
        {"00010", "00110", "01010", "10010", "11111", "00010", "00010"},
        {"11111", "10000", "10000", "11110", "00001", "00001", "11110"},
        {"01110", "10000", "10000", "11110", "10001", "10001", "01110"},
        {"11111", "00001", "00010", "00100", "01000", "01000", "01000"},
        {"01110", "10001", "10001", "01110", "10001", "10001", "01110"},
        {"01110", "10001", "10001", "01111", "00001", "00001", "01110"},
        {"00001", "00010", "00010", "00100", "01000", "01000", "10000"}
    };
    char text[48];
    float vertices[48 * 35 * 6 * 2];
    const int scale = 7;
    size_t vertex_count = 0;
    int length;
    int text_width;
    int x;
    int bottom = bar_y - 14 - 7 * scale;
    int i;
    snprintf(text, sizeof(text), "%llu/%llu",
             (unsigned long long)current, (unsigned long long)total);
    length = (int)strlen(text);
    text_width = length * 6 * scale - scale;
    x = ((int)renderer->mode.hdisplay - text_width) / 2;
    for (i = 0; i < length; ++i) {
        int glyph = text[i] == '/' ? 10 : text[i] - '0';
        int row;
        if (glyph < 0 || glyph > 10)
            continue;
        for (row = 0; row < 7; ++row) {
            int column;
            for (column = 0; column < 5; ++column) {
                if (glyphs[glyph][row][column] == '1') {
                    float left = 2.0f * (x + i * 6 * scale + column * scale) /
                                 renderer->mode.hdisplay - 1.0f;
                    float right = 2.0f * (x + i * 6 * scale + (column + 1) * scale) /
                                  renderer->mode.hdisplay - 1.0f;
                    float lower = 2.0f * (bottom + (6 - row) * scale) /
                                  renderer->mode.vdisplay - 1.0f;
                    float upper = 2.0f * (bottom + (7 - row) * scale) /
                                  renderer->mode.vdisplay - 1.0f;
                    float *vertex = vertices + vertex_count * 2;
                    vertex[0] = left;
                    vertex[1] = lower;
                    vertex[2] = right;
                    vertex[3] = lower;
                    vertex[4] = left;
                    vertex[5] = upper;
                    vertex[6] = left;
                    vertex[7] = upper;
                    vertex[8] = right;
                    vertex[9] = lower;
                    vertex[10] = right;
                    vertex[11] = upper;
                    vertex_count += 6;
                }
            }
        }
    }
    if (!vertex_count)
        return;
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(renderer->progress_program);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)vertex_count);
}

static int renderer_progress_internal(Renderer *renderer, double fraction,
                                      uint64_t frame_current, uint64_t frame_total)
{
    static const float vertices[] = {
        -1, -1, 1, -1, -1, 1,
        -1, 1, 1, -1, 1, 1
    };
    int width = renderer->mode.hdisplay * 3 / 5;
    int height = 12;
    int x = (renderer->mode.hdisplay - width) / 2;
    int y = (renderer->mode.vdisplay - height) / 2;
    int filled;
    if (fraction < 0)
        fraction = 0;
    if (fraction > 1)
        fraction = 1;
    if (fraction < renderer->progress_fraction)
        fraction = renderer->progress_fraction;
    renderer->progress_fraction = fraction;
    renderer->progress_frame_current = frame_current;
    renderer->progress_frame_total = frame_total;
    if (!renderer->overlay_active)
        overlay_disable(renderer);
    if (renderer->offscreen) {
        char line[96];
        int length;
        if (frame_total)
            length = snprintf(line, sizeof(line), "%.4f %llu/%llu\n", fraction,
                              (unsigned long long)frame_current,
                              (unsigned long long)frame_total);
        else
            length = snprintf(line, sizeof(line), "%.4f\n", fraction);
        if (renderer->progress_fd >= 0 && length > 0)
            (void)write(renderer->progress_fd, line, (size_t)length);
        return 0;
    }
    if (renderer->pause_requested)
        renderer->paused = *renderer->pause_requested;
    filled = (int)(width * fraction + 0.5);
    glViewport(0, 0, renderer->mode.hdisplay, renderer->mode.vdisplay);
    glDisable(GL_SCISSOR_TEST);
    if (renderer->overlay_active) {
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    } else if (renderer->background_texture) {
        glUseProgram(renderer->background_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, renderer->background_texture);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    } else if (renderer->error_screen) {
        draw_error_geometry(renderer);
    } else if (renderer->progress_has_frame) {
        draw_scene(renderer);
    } else {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, width, height);
    glClearColor(0.15f, 0.15f, 0.15f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (filled > 0) {
        glScissor(x, y, filled, height);
        glClearColor(0.9f, 0.9f, 0.9f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    if (frame_total)
        draw_frame_count(renderer, frame_current, frame_total, y);
    glDisable(GL_SCISSOR_TEST);
    return present(renderer, NULL, 0);
}

int renderer_progress(Renderer *renderer, double fraction)
{
    return renderer_progress_internal(renderer, fraction, 0, 0);
}

int renderer_progress_frames(Renderer *renderer, double fraction,
                             uint64_t current, uint64_t total)
{
    return renderer_progress_internal(renderer, fraction, current, total);
}

void renderer_clear_progress_background(Renderer *renderer)
{
    if (renderer->background_texture)
        glDeleteTextures(1, &renderer->background_texture);
    if (renderer->background_program)
        glDeleteProgram(renderer->background_program);
    renderer->background_texture = 0;
    renderer->background_program = 0;
    renderer->progress_has_frame = 0;
}

void renderer_begin_progress(Renderer *renderer, int movie)
{
    renderer->progress_started = monotonic_seconds();
    renderer->progress_fraction = 0;
    if (!renderer->offscreen) {
        if (movie) {
            overlay_disable(renderer);
            renderer_clear_progress_background(renderer);
        } else if (!renderer->overlay_active) {
            capture_progress_background(renderer);
        }
        if (!renderer->background_texture) {
            char error[256] = {0};
            const uint32_t black = 0xff000000;
            if (create_background_program(renderer, error, sizeof(error)) == 0) {
                glGenTextures(1, &renderer->background_texture);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, renderer->background_texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, &black);
            }
        }
    }
}

int renderer_refresh(Renderer *renderer, char *error, size_t error_size)
{
    if (renderer->error_screen)
        draw_error_geometry(renderer);
    else
        draw_scene(renderer);
    return present(renderer, error, error_size);
}

int renderer_set_paused(Renderer *renderer, int paused, char *error, size_t error_size)
{
    renderer->paused = paused;
    if (renderer->overlay_active) {
        glViewport(0, 0, renderer->mode.hdisplay, renderer->mode.vdisplay);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        return present(renderer, error, error_size);
    }
    if (renderer->background_texture)
        return renderer_progress_internal(renderer, renderer->progress_fraction,
                                          renderer->progress_frame_current,
                                          renderer->progress_frame_total);
    return renderer_refresh(renderer, error, error_size);
}

void renderer_track_pause(Renderer *renderer, volatile sig_atomic_t *requested)
{
    renderer->pause_requested = requested;
}

static Renderer *renderer_create_internal(const char *drm_device, int source_w, int source_h,
                          const QuiltConfig *quilt, const Calibration *calibration,
                          int direct, const char *progress_background, int offscreen,
                          int progress_fd, int restore_crtc, int show_progress,
                          char *error, size_t error_size)
{
    Renderer *renderer = calloc(1, sizeof(*renderer));
    if (!renderer) {
        set_error(error, error_size, "Out of memory");
        return NULL;
    }
    renderer->fd = -1;
    renderer->egl_display = EGL_NO_DISPLAY;
    renderer->egl_context = EGL_NO_CONTEXT;
    renderer->egl_surface = EGL_NO_SURFACE;
    renderer->source_w = source_w;
    renderer->source_h = source_h;
    renderer->direct = direct;
    renderer->offscreen = offscreen;
    renderer->progress_fd = progress_fd;
    renderer->restore_crtc = restore_crtc;
    renderer->quilt = *quilt;
    renderer->calibration = *calibration;
    renderer->yuv_bt709 = 1;
    renderer->yuv_full_range = 0;
    renderer->progress_started = monotonic_seconds();
    renderer->profile_preprocess = getenv("LKG_PROFILE_PREPROCESS") != NULL;
    renderer->profile_dma = getenv("LKG_PROFILE_DMA") != NULL;
    renderer->fd = open(drm_device, O_RDWR | O_CLOEXEC);
    if (renderer->fd < 0) {
        set_error(error, error_size, "Failed to open %s: %s", drm_device, strerror(errno));
        goto fail;
    }
    if (choose_crtc(renderer, error, error_size) != 0)
        goto fail;
    printf("Display: %s, %dx%d@%u\n", drm_device, renderer->mode.hdisplay,
           renderer->mode.vdisplay, renderer->mode.vrefresh);
    if (initialize_egl(renderer, error, error_size) != 0)
        goto fail;
    if (!offscreen) {
        renderer->progress_program = create_program(progress_fragment_source, 1, 1, 1, 0,
                                                    error, error_size);
        if (!renderer->progress_program)
            goto fail;
    }
    renderer->unpack_subimage = has_extension((const char *)glGetString(GL_EXTENSIONS),
                                               "GL_EXT_unpack_subimage");
    printf("RGB subimage upload: %s\n", renderer->unpack_subimage ? "enabled" : "unavailable");
    if (show_progress) {
        initialize_progress_background(renderer, progress_background);
        renderer->startup = 1;
        if (renderer_progress(renderer, 0) != 0)
            goto fail;
    }
    if (create_chunks(renderer, error, error_size) != 0)
        goto fail;
    renderer->startup = 0;
    if (create_renderer_program(renderer, error, error_size) != 0)
        goto fail;
    return renderer;

fail:
    renderer_destroy(renderer);
    return NULL;
}

Renderer *renderer_create(const char *drm_device, int source_w, int source_h,
                          const QuiltConfig *quilt, const Calibration *calibration,
                          int direct, const char *progress_background, int restore_crtc,
                          int show_progress, char *error, size_t error_size)
{
    return renderer_create_internal(drm_device, source_w, source_h, quilt, calibration,
                                    direct, progress_background, 0, -1, restore_crtc,
                                    show_progress, error, error_size);
}

Renderer *renderer_create_offscreen(const char *drm_device, int source_w, int source_h,
                                    const QuiltConfig *quilt, const Calibration *calibration,
                                    int direct, int progress_fd, char *error, size_t error_size)
{
    return renderer_create_internal(drm_device, source_w, source_h, quilt, calibration,
                                    direct, NULL, 1, progress_fd, 0, 1, error, error_size);
}

static int same_quilt(const QuiltConfig *a, const QuiltConfig *b)
{
    return a->columns == b->columns && a->rows == b->rows &&
           a->views == b->views && a->aspect == b->aspect &&
           a->zoom == b->zoom && a->position_x == b->position_x &&
           a->position_y == b->position_y && a->focus == b->focus &&
           a->stereo_disparity == b->stereo_disparity &&
           a->reverse_views == b->reverse_views && a->movie == b->movie &&
           a->clamp_edges == b->clamp_edges;
}

int renderer_reconfigure(Renderer *renderer, int source_w, int source_h,
                         const QuiltConfig *quilt, int direct, char *error, size_t error_size)
{
    int i;
    int reuse = renderer->program && renderer->source_w == source_w &&
                renderer->source_h == source_h && renderer->direct == direct &&
                same_quilt(&renderer->quilt, quilt);
    overlay_disable(renderer);
    renderer->progress_has_frame = 0;
    renderer->error_screen = 0;
    if (renderer->background_texture)
        glDeleteTextures(1, &renderer->background_texture);
    if (renderer->background_program)
        glDeleteProgram(renderer->background_program);
    renderer->background_texture = 0;
    renderer->background_program = 0;
    dma_images_destroy(renderer);
    if (reuse) {
        if (renderer->nv12_uv_texture) {
            glDeleteTextures(1, &renderer->nv12_uv_texture);
            renderer->nv12_uv_texture = 0;
        }
        return 0;
    }
    glDeleteProgram(renderer->program);
    renderer->program = 0;
    for (i = 0; i < renderer->chunk_count; ++i)
        glDeleteTextures(1, &renderer->chunks[i].texture);
    if (renderer->nv12_uv_texture) {
        glDeleteTextures(1, &renderer->nv12_uv_texture);
        renderer->nv12_uv_texture = 0;
    }
    memset(renderer->chunks, 0, sizeof(renderer->chunks));
    renderer->chunk_count = 0;
    free(renderer->upload_buffer);
    renderer->upload_buffer = NULL;
    renderer->source_w = source_w;
    renderer->source_h = source_h;
    renderer->quilt = *quilt;
    renderer->direct = direct;
    if (create_chunks(renderer, error, error_size) != 0)
        return -1;
    return create_renderer_program(renderer, error, error_size);
}

int renderer_draw(Renderer *renderer, const uint8_t *frame, char *error, size_t error_size)
{
    int result;
    if (upload_chunks(renderer, frame) != 0) {
        set_error(error, error_size, "Failed to upload quilt textures");
        return -1;
    }
    draw_scene(renderer);
    result = present(renderer, error, error_size);
    if (result == 0)
        renderer->progress_has_frame = 1;
    return result;
}

static EGLImageKHR create_image_khr(EGLDisplay display, EGLContext context,
                                    EGLenum target, EGLClientBuffer buffer,
                                    const EGLint *attributes)
{
    PFNEGLCREATEIMAGEKHRPROC function = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    return function ? function(display, context, target, buffer, attributes) : EGL_NO_IMAGE_KHR;
}

static void destroy_image_khr(EGLDisplay display, EGLImageKHR image)
{
    PFNEGLDESTROYIMAGEKHRPROC function = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    if (function)
        function(display, image);
}

static void image_target_texture(GLenum target, GLeglImageOES image)
{
    static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC function;
    static int initialized;
    if (!initialized) {
        function = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");
        initialized = 1;
    }
    if (function)
        function(target, image);
}

static void dma_cache_entry_destroy(Renderer *renderer, DmaCacheEntry *entry)
{
    int plane;
    for (plane = 0; plane < 2; ++plane) {
        if (entry->images[plane])
            destroy_image_khr(renderer->egl_display, entry->images[plane]);
    }
    if (entry->fb)
        drmModeRmFB(renderer->fd, entry->fb);
    memset(entry, 0, sizeof(*entry));
}

static DmaCacheEntry *dma_cache_entry(Renderer *renderer, const Decoder *decoder)
{
    DmaCacheEntry *unused = NULL;
    DmaCacheEntry *oldest = NULL;
    struct stat status[2];
    int i;
    for (i = 0; i < 2; ++i) {
        if (fstat(decoder->dma_fds[i], &status[i]) != 0)
            return NULL;
    }
    for (i = 0; i < LKG_DMA_CACHE_ENTRIES; ++i) {
        DmaCacheEntry *entry = &renderer->dma_cache[i];
        int current;
        if (!entry->used) {
            if (!unused)
                unused = entry;
            continue;
        }
        if (entry->devices[0] == status[0].st_dev &&
            entry->devices[1] == status[1].st_dev &&
            entry->inos[0] == status[0].st_ino && entry->inos[1] == status[1].st_ino &&
            entry->strides[0] == decoder->dma_strides[0] &&
            entry->strides[1] == decoder->dma_strides[1] &&
            entry->offsets[0] == decoder->dma_offsets[0] &&
            entry->offsets[1] == decoder->dma_offsets[1] &&
            entry->width == decoder->nv12_width && entry->height == decoder->nv12_height) {
            entry->age = ++renderer->dma_cache_clock;
            return entry;
        }
        current = (entry->fb && entry->fb == renderer->overlay_fb) ||
                  (entry->images[0] && entry->images[0] == renderer->dma_images[0]) ||
                  (entry->images[1] && entry->images[1] == renderer->dma_images[1]);
        if (!current && (!oldest || entry->age < oldest->age))
            oldest = entry;
    }
    if (!unused) {
        unused = oldest;
        if (unused)
            dma_cache_entry_destroy(renderer, unused);
    }
    if (!unused)
        return NULL;
    unused->used = 1;
    unused->devices[0] = status[0].st_dev;
    unused->devices[1] = status[1].st_dev;
    unused->inos[0] = status[0].st_ino;
    unused->inos[1] = status[1].st_ino;
    unused->strides[0] = decoder->dma_strides[0];
    unused->strides[1] = decoder->dma_strides[1];
    unused->offsets[0] = decoder->dma_offsets[0];
    unused->offsets[1] = decoder->dma_offsets[1];
    unused->width = decoder->nv12_width;
    unused->height = decoder->nv12_height;
    unused->age = ++renderer->dma_cache_clock;
    return unused;
}

static void dma_images_destroy(Renderer *renderer)
{
    int i;
    for (i = 0; i < LKG_DMA_CACHE_ENTRIES; ++i)
        dma_cache_entry_destroy(renderer, &renderer->dma_cache[i]);
    renderer->dma_images[0] = NULL;
    renderer->dma_images[1] = NULL;
    renderer->overlay_fb = 0;
}

static uint32_t overlay_find_plane(Renderer *renderer)
{
    drmModeRes *resources = drmModeGetResources(renderer->fd);
    drmModePlaneRes *planes = drmModeGetPlaneResources(renderer->fd);
    uint32_t result = 0;
    int crtc_index = -1;
    uint32_t i;

    if (!resources || !planes)
        goto done;
    for (i = 0; i < (uint32_t)resources->count_crtcs; ++i) {
        if (resources->crtcs[i] == renderer->crtc_id) {
            crtc_index = (int)i;
            break;
        }
    }
    if (crtc_index < 0)
        goto done;
    for (i = 0; i < planes->count_planes; ++i) {
        drmModePlane *plane = drmModeGetPlane(renderer->fd, planes->planes[i]);
        drmModeObjectProperties *properties;
        uint32_t j;
        int supported = 0;
        if (!plane)
            continue;
        if (!(plane->possible_crtcs & (1u << crtc_index)))
            goto next_plane;
        for (j = 0; j < plane->count_formats; ++j) {
            if (plane->formats[j] == DRM_FORMAT_NV12) {
                supported = 1;
                break;
            }
        }
        if (!supported)
            goto next_plane;
        properties = drmModeObjectGetProperties(renderer->fd, plane->plane_id,
                                                DRM_MODE_OBJECT_PLANE);
        if (!properties)
            goto next_plane;
        for (j = 0; j < properties->count_props; ++j) {
            drmModePropertyPtr property = drmModeGetProperty(renderer->fd,
                                                           properties->props[j]);
            if (property && strcmp(property->name, "zpos") == 0 &&
                drmModeObjectSetProperty(renderer->fd, plane->plane_id,
                                         DRM_MODE_OBJECT_PLANE, property->prop_id, 0) != 0) {
                drmModeFreeProperty(property);
                drmModeFreeObjectProperties(properties);
                goto next_plane;
            }
            if (property)
                drmModeFreeProperty(property);
        }
        drmModeFreeObjectProperties(properties);
        result = plane->plane_id;

next_plane:
        drmModeFreePlane(plane);
        if (result)
            break;
    }

done:
    if (planes)
        drmModeFreePlaneResources(planes);
    if (resources)
        drmModeFreeResources(resources);
    return result;
}

static uint32_t overlay_create_fb(Renderer *renderer, const Decoder *decoder)
{
    uint32_t handles[4] = {0};
    uint32_t pitches[4] = {0};
    uint32_t offsets[4] = {0};
    uint32_t fb = 0;
    int i;

    for (i = 0; i < 2; ++i) {
        if (drmPrimeFDToHandle(renderer->fd, decoder->dma_fds[i], &handles[i]) != 0)
            goto done;
        pitches[i] = decoder->dma_strides[i];
        offsets[i] = decoder->dma_offsets[i];
    }
    if (drmModeAddFB2(renderer->fd, decoder->nv12_width, decoder->nv12_height,
                      DRM_FORMAT_NV12, handles, pitches, offsets, &fb, 0) != 0)
        fb = 0;

done:
    for (i = 0; i < 2; ++i) {
        int j;
        int duplicate = 0;
        for (j = 0; j < i; ++j)
            duplicate |= handles[i] && handles[i] == handles[j];
        if (handles[i] && !duplicate)
            drmCloseBufferHandle(renderer->fd, handles[i]);
    }
    return fb;
}

static int overlay_draw(Renderer *renderer, Decoder *decoder,
                        char *error, size_t error_size)
{
    uint64_t crop_x = 0, crop_y = 0;
    uint64_t crop_w = decoder->nv12_width;
    uint64_t crop_h = decoder->nv12_height;
    DmaCacheEntry *entry;
    uint32_t fb;
    int first = !renderer->overlay_active;

    if (first) {
        renderer->overlay_plane = overlay_find_plane(renderer);
        if (!renderer->overlay_plane)
            return 0;
    }
    entry = dma_cache_entry(renderer, decoder);
    if (!entry) {
        set_error(error, error_size, "Failed to cache NV12 DMA-BUF");
        return -1;
    }
    if (!entry->fb)
        entry->fb = overlay_create_fb(renderer, decoder);
    fb = entry->fb;
    if (!fb) {
        if (!first) {
            set_error(error, error_size, "Failed to import NV12 DRM framebuffer: %s",
                      strerror(errno));
            return -1;
        }
        overlay_disable(renderer);
        return 0;
    }
    if (crop_w * renderer->mode.vdisplay > crop_h * renderer->mode.hdisplay) {
        crop_w = crop_h * renderer->mode.hdisplay / renderer->mode.vdisplay;
        crop_w &= ~1ULL;
        crop_x = ((decoder->nv12_width - crop_w) / 2) & ~1ULL;
    } else {
        crop_h = crop_w * renderer->mode.vdisplay / renderer->mode.hdisplay;
        crop_h &= ~1ULL;
        crop_y = ((decoder->nv12_height - crop_h) / 2) & ~1ULL;
    }
    int presented = 0;
    if (first && !renderer->crtc_set) {
        glViewport(0, 0, renderer->mode.hdisplay, renderer->mode.vdisplay);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        if (present(renderer, error, error_size) != 0) {
            overlay_disable(renderer);
            return -1;
        }
        presented = 1;
    }
    if (!crop_w || !crop_h ||
        drmModeSetPlane(renderer->fd, renderer->overlay_plane, renderer->crtc_id, fb, 0,
                        0, 0, renderer->mode.hdisplay, renderer->mode.vdisplay,
                        (uint32_t)crop_x << 16, (uint32_t)crop_y << 16,
                        (uint32_t)crop_w << 16, (uint32_t)crop_h << 16) != 0) {
        drmModeRmFB(renderer->fd, fb);
        entry->fb = 0;
        if (!first) {
            set_error(error, error_size, "drmModeSetPlane NV12 failed: %s", strerror(errno));
            return -1;
        }
        overlay_disable(renderer);
        return 0;
    }
    renderer->overlay_fb = fb;
    if (first) {
        if (!presented) {
            glViewport(0, 0, renderer->mode.hdisplay, renderer->mode.vdisplay);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            if (present(renderer, error, error_size) != 0) {
                overlay_disable(renderer);
                return -1;
            }
        }
        renderer->overlay_active = 1;
        decoder->keep_previous_sample = 1;
        printf("Plain video NV12 DRM overlay: plane %u\n", renderer->overlay_plane);
    }
    renderer->progress_has_frame = 1;
    return 1;
}

static void overlay_disable(Renderer *renderer)
{
    if (!renderer)
        return;
    if (renderer->overlay_plane)
        drmModeSetPlane(renderer->fd, renderer->overlay_plane, renderer->crtc_id,
                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    renderer->overlay_plane = 0;
    renderer->overlay_fb = 0;
    renderer->overlay_active = 0;
}

void renderer_release_dma(Renderer *renderer)
{
    if (!renderer)
        return;
    overlay_disable(renderer);
    if (renderer->direct != 4 && renderer->direct != 5)
        return;
    dma_images_destroy(renderer);
    if (renderer->chunks[0].texture) {
        glDeleteTextures(1, &renderer->chunks[0].texture);
        renderer->chunks[0].texture = 0;
    }
    if (renderer->nv12_uv_texture) {
        glDeleteTextures(1, &renderer->nv12_uv_texture);
        renderer->nv12_uv_texture = 0;
    }
}

static EGLImageKHR import_dma_plane(Renderer *renderer, const Decoder *decoder,
                                    int plane, int width, int height, uint32_t format)
{
    const EGLint attributes[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)format,
        EGL_DMA_BUF_PLANE0_FD_EXT, decoder->dma_fds[plane],
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)decoder->dma_offsets[plane],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)decoder->dma_strides[plane],
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0,
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
        EGL_NONE
    };
    return create_image_khr(renderer->egl_display, EGL_NO_CONTEXT,
                            EGL_LINUX_DMA_BUF_EXT, NULL, attributes);
}

static void bind_dma_texture(GLuint *texture, EGLImageKHR image, GLint filter)
{
    if (!*texture)
        glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    image_target_texture(GL_TEXTURE_2D, image);
}

int renderer_draw_dma(Renderer *renderer, Decoder *decoder, char *error, size_t error_size)
{
    DmaCacheEntry *entry;
    int result = -1;
    double started = renderer->profile_dma ? monotonic_seconds() : 0;
    if (renderer->direct == 5) {
        if (overlay_draw(renderer, decoder, error, error_size) != 1) {
            if (error && error_size && !error[0])
                set_error(error, error_size, "NV12 DRM overlay is unavailable: %s",
                          strerror(errno));
            return -1;
        }
        return 0;
    }
    entry = dma_cache_entry(renderer, decoder);
    if (!entry) {
        set_error(error, error_size, "Failed to cache NV12 DMA-BUF");
        return -1;
    }
    if (!entry->images[0])
        entry->images[0] = import_dma_plane(renderer, decoder, 0,
            decoder->nv12_width, decoder->nv12_height, DRM_FORMAT_R8);
    if (!entry->images[0]) {
        set_error(error, error_size, "Failed to import Y-plane EGLImage (0x%x)", eglGetError());
        return -1;
    }
    if (!entry->images[1])
        entry->images[1] = import_dma_plane(renderer, decoder, 1,
            decoder->nv12_width / 2, decoder->nv12_height / 2, DRM_FORMAT_GR88);
    if (!entry->images[1]) {
        set_error(error, error_size, "Failed to import UV-plane EGLImage (0x%x)", eglGetError());
        dma_images_destroy(renderer);
        return -1;
    }
    renderer->dma_images[0] = entry->images[0];
    renderer->dma_images[1] = entry->images[1];
    bind_dma_texture(&renderer->chunks[0].texture, renderer->dma_images[0],
                     renderer->direct == 4 ? GL_NEAREST : GL_LINEAR);
    bind_dma_texture(&renderer->nv12_uv_texture, renderer->dma_images[1],
                     renderer->direct == 4 ? GL_NEAREST : GL_LINEAR);
    if (glGetError() != GL_NO_ERROR) {
        set_error(error, error_size, "Failed to bind DMA-BUF textures");
        dma_images_destroy(renderer);
        return -1;
    }
    if (renderer->profile_dma)
        renderer->profile_dma_import += monotonic_seconds() - started;
    draw_scene(renderer);
    if (renderer->profile_dma) {
        started = monotonic_seconds();
        glFinish();
        renderer->profile_dma_gpu += monotonic_seconds() - started;
    }
    if (present(renderer, error, error_size) == 0) {
        decoder->keep_previous_sample = 1;
        renderer->progress_has_frame = 1;
        result = 0;
    }
    if (renderer->profile_dma && ++renderer->profile_dma_frames % 60 == 0) {
        double frames = (double)renderer->profile_dma_frames;
        fprintf(stderr, "DMA profile: vblank %.2f ms, import+bind %.2f ms, GPU %.2f ms, flip wait %.2f ms\n",
                renderer->profile_vblank_interval * 1000.0 / frames,
                renderer->profile_dma_import * 1000.0 / frames,
                renderer->profile_dma_gpu * 1000.0 / frames,
                renderer->profile_dma_wait * 1000.0 / frames);
    }
    return result;
}

static void convert_readback_rgb(const Renderer *renderer, const uint8_t *pixels, uint8_t *rgb)
{
    int width = renderer->mode.hdisplay;
    int height = renderer->mode.vdisplay;
    size_t row_size = (size_t)width * 3;
    int x;
    int y;
    for (y = 0; y < height; ++y)
        for (x = 0; x < width; ++x)
            memcpy(rgb + (size_t)(height - 1 - y) * row_size + (size_t)x * 3,
                   pixels + ((size_t)y * width + x) * 4, 3);
}

int renderer_read_rgb(Renderer *renderer, uint8_t *rgb, char *error, size_t error_size)
{
    int width = renderer->mode.hdisplay;
    int height = renderer->mode.vdisplay;
    uint8_t *pixels;
    double started = monotonic_seconds();

    draw_scene(renderer);
    glFinish();
    renderer->profile_draw += monotonic_seconds() - started;
    started = monotonic_seconds();
    pixels = malloc((size_t)width * height * 4);
    if (!pixels) {
        set_error(error, error_size, "Failed to allocate preprocessing readback buffer");
        return -1;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (glGetError() != GL_NO_ERROR) {
        set_error(error, error_size, "Failed to read preprocessed frame");
        free(pixels);
        return -1;
    }
    renderer->profile_readback += monotonic_seconds() - started;
    started = monotonic_seconds();
    convert_readback_rgb(renderer, pixels, rgb);
    free(pixels);
    renderer->profile_convert += monotonic_seconds() - started;
    return 0;
}

int renderer_render_to_rgb(Renderer *renderer, const uint8_t *frame, uint8_t *rgb,
                           char *error, size_t error_size)
{
    double started = monotonic_seconds();
    if (upload_chunks(renderer, frame) != 0) {
        set_error(error, error_size, "Failed to upload quilt textures");
        return -1;
    }
    renderer->profile_upload += monotonic_seconds() - started;
    return renderer_read_rgb(renderer, rgb, error, error_size);
}

int renderer_render_to_bgra(Renderer *renderer, const uint8_t *frame, uint8_t *bgra,
                            char *error, size_t error_size)
{
    double started = monotonic_seconds();
    if (upload_chunks(renderer, frame) != 0) {
        set_error(error, error_size, "Failed to upload quilt textures");
        return -1;
    }
    renderer->profile_upload += monotonic_seconds() - started;
    started = monotonic_seconds();
    draw_scene_internal(renderer, 1);
    if (renderer->profile_preprocess)
        glFinish();
    renderer->profile_draw += monotonic_seconds() - started;
    started = monotonic_seconds();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, renderer->mode.hdisplay, renderer->mode.vdisplay,
                 GL_BGRA_EXT, GL_UNSIGNED_BYTE, bgra);
    if (glGetError() != GL_NO_ERROR) {
        set_error(error, error_size, "Failed to read preprocessed frame");
        return -1;
    }
    renderer->profile_readback += monotonic_seconds() - started;
    return 0;
}

void renderer_get_profile(Renderer *renderer, double *upload, double *draw,
                          double *readback, double *convert)
{
    *upload = renderer->profile_upload;
    *draw = renderer->profile_draw;
    *readback = renderer->profile_readback;
    *convert = renderer->profile_convert;
}

int renderer_snapshot(Renderer *renderer, const char *path, char *error, size_t error_size)
{
    int width = renderer->mode.hdisplay;
    int height = renderer->mode.vdisplay;
    size_t row_size = (size_t)width * 3;
    uint8_t *pixels = NULL;
    uint8_t *rgb;
    FILE *file;
    int x;
    int y;

    rgb = malloc(row_size * height);
    if (!rgb) {
        set_error(error, error_size, "Failed to allocate snapshot buffer");
        return -1;
    }
    draw_scene(renderer);
    glFinish();
    pixels = malloc((size_t)width * height * 4);
    if (!pixels) {
        set_error(error, error_size, "Failed to allocate snapshot buffer");
        free(rgb);
        return -1;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (glGetError() != GL_NO_ERROR) {
        set_error(error, error_size, "glReadPixels failed");
        free(pixels);
        free(rgb);
        return -1;
    }
    for (y = 0; y < height; ++y)
        for (x = 0; x < width; ++x)
            memcpy(rgb + (size_t)y * row_size + (size_t)x * 3,
                   pixels + ((size_t)(height - 1 - y) * width + x) * 4, 3);
    file = fopen(path, "wb");
    if (!file) {
        set_error(error, error_size, "Failed to create %s: %s", path, strerror(errno));
        free(pixels);
        free(rgb);
        return -1;
    }
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (y = 0; y < height; ++y)
        fwrite(rgb + (size_t)y * row_size, 1, row_size, file);
    free(pixels);
    free(rgb);
    if (ferror(file) || fclose(file) != 0) {
        set_error(error, error_size, "Failed to write snapshot %s", path);
        return -1;
    }
    return present(renderer, error, error_size);
}

void renderer_destroy(Renderer *renderer)
{
    int i;
    if (!renderer)
        return;
    if (renderer->egl_display != EGL_NO_DISPLAY && renderer->egl_context != EGL_NO_CONTEXT &&
        renderer->egl_surface != EGL_NO_SURFACE)
        eglMakeCurrent(renderer->egl_display, renderer->egl_surface,
                       renderer->egl_surface, renderer->egl_context);
    if (renderer->flip_pending)
        wait_for_flip(renderer, NULL, 0);
    overlay_disable(renderer);
    if (renderer->saved_crtc && renderer->restore_crtc)
        drmModeSetCrtc(renderer->fd, renderer->saved_crtc->crtc_id,
                       renderer->saved_crtc->buffer_id, renderer->saved_crtc->x,
                       renderer->saved_crtc->y, &renderer->connector_id, 1,
                       &renderer->saved_crtc->mode);
    dma_images_destroy(renderer);
    if (!renderer->preserve_on_exit && renderer->gbm_surface) {
        if (renderer->scanout_bo)
            gbm_surface_release_buffer(renderer->gbm_surface, renderer->scanout_bo);
        if (renderer->pending_bo)
            gbm_surface_release_buffer(renderer->gbm_surface, renderer->pending_bo);
    }
    if (renderer->program)
        glDeleteProgram(renderer->program);
    if (renderer->progress_program)
        glDeleteProgram(renderer->progress_program);
    if (renderer->background_program)
        glDeleteProgram(renderer->background_program);
    if (renderer->background_texture)
        glDeleteTextures(1, &renderer->background_texture);
    for (i = 0; i < renderer->chunk_count; ++i)
        if (renderer->chunks[i].texture)
            glDeleteTextures(1, &renderer->chunks[i].texture);
    if (renderer->nv12_uv_texture)
        glDeleteTextures(1, &renderer->nv12_uv_texture);
    if (renderer->egl_display != EGL_NO_DISPLAY && !renderer->preserve_on_exit) {
        if (renderer->egl_surface != EGL_NO_SURFACE)
            eglDestroySurface(renderer->egl_display, renderer->egl_surface);
        if (renderer->egl_context != EGL_NO_CONTEXT)
            eglDestroyContext(renderer->egl_display, renderer->egl_context);
        eglTerminate(renderer->egl_display);
    }
    if (renderer->gbm_surface && !renderer->preserve_on_exit)
        gbm_surface_destroy(renderer->gbm_surface);
    if (renderer->gbm && !renderer->preserve_on_exit)
        gbm_device_destroy(renderer->gbm);
    if (renderer->saved_crtc)
        drmModeFreeCrtc(renderer->saved_crtc);
    if (renderer->fd >= 0)
        close(renderer->fd);
    if (renderer->thumbnail_active)
        unlink(renderer->thumbnail_temporary);
    free(renderer->thumbnail_tile);
    free(renderer->upload_buffer);
    free(renderer);
}

int renderer_width(const Renderer *renderer)
{
    return renderer ? renderer->mode.hdisplay : 0;
}

int renderer_height(const Renderer *renderer)
{
    return renderer ? renderer->mode.vdisplay : 0;
}
