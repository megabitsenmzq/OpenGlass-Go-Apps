#include "depth_gpu.h"

#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drmMode.h>

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int fd;
    uint32_t id;
} DepthFramebuffer;

typedef struct {
    struct gbm_bo *bo;
    EGLImageKHR image;
    GLuint texture;
    GLuint fbo;
    DepthFramebuffer *fb;
    int dirty_x0;
    int dirty_y0;
    int dirty_x1;
    int dirty_y1;
    int dirty_full;
} ScanoutSlot;

typedef struct {
    GLuint texture;
    int x;
    int y;
    int width;
    int height;
} GalleryPatch;

typedef struct {
    GLint layer[2];
    GLint resolution;
    GLint frag_offset;
    GLint layer_origin;
    GLint layer_size;
    GLint disparity;
    GLint bar_range;
    GLint pitch;
    GLint tilt;
    GLint center;
    GLint fringe;
    GLint inv_view;
    GLint flip_subp;
    GLint interlace;
    GLint stereo;
} DepthUniforms;

typedef struct {
    GLint layer[2];
    GLint resolution;
    GLint inv_resolution;
    GLint back_disparity;
    GLint first_disparity;
    GLint second_disparity;
    GLint first_bounds;
    GLint second_bounds;
    GLint bar_range;
    GLint pitch;
    GLint tilt;
    GLint center;
    GLint fringe;
    GLint inv_view;
    GLint flip_subp;
    GLint has_back;
    GLint scroll_y;
    GLint scroll_limit_y;
    GLint source_max_y;
} DepthAnimUniforms;

struct DepthGpu {
    struct gbm_device *gbm;
    struct gbm_surface *gbm_surface;
    struct gbm_bo *scanout_bo;
    struct gbm_bo *pending_bo;
    Display *owner;
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    GLuint program;
    DepthUniforms uniforms;
    GLuint anim_program;
    DepthAnimUniforms anim_uniforms;
    GLint blit_cache_uniform;
    GLuint blit_flip_program;
    GLint blit_flip_cache_uniform;
    int unpack_subimage;
    GLuint textures[2];
    GLenum layer_filter;
    int textures_initialized;
    uint32_t *upload_scratch;
    size_t upload_scratch_bytes;
    GLuint blit_program;
    GLuint cache_texture;
    GLuint cache_framebuffer;
    int cache_width;
    int cache_height;
    int layer_width;
    int layer_height;
    GLuint gallery_base_texture;
    int gallery_base_width;
    int gallery_base_height;
    GalleryPatch gallery_patches[9][2];
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    int partial_scanout;
    int front_slot;
    int pending_slot;
    ScanoutSlot slots[2];
    uint32_t current_framebuffer;
    uint64_t cache_revision;
    int failed;
};

static void cache_uniform_locations(DepthGpu *gpu);

static uint64_t cache_revision_counter;

static void advance_cache_revision(DepthGpu *gpu)
{
    gpu->cache_revision = ++cache_revision_counter;
}

uint64_t depth_gpu_cache_revision(const DepthGpu *gpu)
{
    return gpu ? gpu->cache_revision : 0;
}

static const char *vertex_source =
    "attribute vec2 a_position;\n"
    "void main() { gl_Position = vec4(a_position, 0.0, 1.0); }\n";

static const char *fragment_source =
    "precision highp float;\n"
    "uniform sampler2D u_layer0;\n"
    "uniform sampler2D u_layer1;\n"
    "uniform vec2 u_resolution;\n"
    "uniform vec2 u_frag_offset;\n"
    "uniform vec2 u_layer_origin;\n"
    "uniform vec2 u_layer_size;\n"
    "uniform vec2 u_disparity;\n"
    "uniform vec2 u_bar_range;\n"
    "uniform float u_pitch;\n"
    "uniform float u_tilt;\n"
    "uniform float u_center;\n"
    "uniform float u_fringe;\n"
    "uniform int u_inv_view;\n"
    "uniform int u_flip_subp;\n"
    "uniform int u_interlace;\n"
    "uniform int u_stereo;\n"
    "vec3 base_color(vec2 source) {\n"
    "  if (source.y >= u_bar_range.x && source.y < u_bar_range.y)\n"
    "    return vec3(232.0 / 255.0);\n"
    "  return vec3(16.0 / 255.0, 16.0 / 255.0, 20.0 / 255.0);\n"
    "}\n"
    "vec4 sample_texel_premul(sampler2D image, float x, float y) {\n"
    "  if (x < 0.0 || x >= u_layer_size.x || y < 0.0 || y >= u_layer_size.y)\n"
    "    return vec4(0.0);\n"
    "  return texture2D(image, (vec2(x, y) + 0.5) / u_layer_size);\n"
    "}\n"
    "vec4 layer_pixel(sampler2D image, float disparity, float angle, vec2 source) {\n"
    "  float local_y = source.y - u_layer_origin.y;\n"
    "  if (local_y < 0.0 || local_y >= u_layer_size.y) return vec4(0.0);\n"
    "  float local_x = source.x - u_layer_origin.x;\n"
    "  if (disparity == 0.0) return sample_texel_premul(image, local_x, local_y);\n"
    "  local_x += disparity * angle;\n"
    "  float x0 = floor(local_x);\n"
    "  float frac = local_x - x0;\n"
    "  vec4 left = sample_texel_premul(image, x0, local_y);\n"
    "  vec4 right = sample_texel_premul(image, x0 + 1.0, local_y);\n"
    "  return mix(left, right, frac);\n"
    "}\n"
    "vec3 composite_angle(float angle, vec2 source, vec3 background) {\n"
    "  vec3 color = background;\n"
    "  vec4 foreground = layer_pixel(u_layer1, u_disparity.y, angle, source);\n"
    "  color = foreground.rgb + color * (1.0 - foreground.a);\n"
    "  foreground = layer_pixel(u_layer0, u_disparity.x, angle, source);\n"
    "  color = foreground.rgb + color * (1.0 - foreground.a);\n"
    "  return color;\n"
    "}\n"
    "vec3 flat_color(vec2 source, vec3 background) {\n"
    "  vec2 local = source - u_layer_origin;\n"
    "  vec3 color = background;\n"
    "  vec4 foreground = sample_texel_premul(u_layer1, local.x, local.y);\n"
    "  color = foreground.rgb + color * (1.0 - foreground.a);\n"
    "  foreground = sample_texel_premul(u_layer0, local.x, local.y);\n"
    "  color = foreground.rgb + color * (1.0 - foreground.a);\n"
    "  return color;\n"
    "}\n"
    "void main() {\n"
    "  vec2 frag = gl_FragCoord.xy + u_frag_offset;\n"
    "  vec2 source = vec2(frag.x - 0.5, u_resolution.y - frag.y - 0.5);\n"
    "  vec3 background = base_color(source);\n"
    "  if (u_interlace == 0) { gl_FragColor = vec4(background, 1.0); return; }\n"
    "  if (u_stereo == 0) {\n"
    "    gl_FragColor = vec4(flat_color(source, background), 1.0);\n"
    "    return;\n"
    "  }\n"
    "  vec2 pixel = frag / u_resolution;\n"
    "  vec3 color = background;\n"
    "  for (int channel = 0; channel < 3; ++channel) {\n"
    "    int subpixel = u_flip_subp != 0 ? 2 - channel : channel;\n"
    "    float phase = (pixel.x + float(subpixel) / (3.0 * u_resolution.x) + pixel.y * u_tilt) * u_pitch - u_center + u_fringe;\n"
    "    phase = u_inv_view != 0 ? 1.0 - fract(phase) : fract(phase);\n"
    "    float angle = 2.0 * phase - 1.0;\n"
    "    vec3 sample_color = composite_angle(angle, source, background);\n"
    "    color[channel] = sample_color[channel];\n"
    "  }\n"
    "  gl_FragColor = vec4(color, 1.0);\n"
    "}\n";

static const char *blit_vertex_source =
    "attribute vec2 a_position;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = a_position * 0.5 + 0.5; gl_Position = vec4(a_position, 0.0, 1.0); }\n";

static const char *blit_flip_vertex_source =
    "attribute vec2 a_position;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = vec2(a_position.x * 0.5 + 0.5, 0.5 - a_position.y * 0.5);\n"
    "  gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "}\n";

static const char *blit_fragment_source =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_cache;\n"
    "void main() { gl_FragColor = texture2D(u_cache, v_uv); }\n";

static const char *anim_fragment_source =
    "precision highp float;\n"
    "uniform sampler2D u_layer0;\n"
    "uniform sampler2D u_layer1;\n"
    "uniform vec2 u_resolution;\n"
    "uniform vec2 u_inv_resolution;\n"
    "uniform float u_back_disparity;\n"
    "uniform float u_first_disparity;\n"
    "uniform float u_second_disparity;\n"
    "uniform vec4 u_first_bounds;\n"
    "uniform vec4 u_second_bounds;\n"
    "uniform vec2 u_bar_range;\n"
    "uniform float u_pitch;\n"
    "uniform float u_tilt;\n"
    "uniform float u_center;\n"
    "uniform float u_fringe;\n"
    "uniform int u_inv_view;\n"
    "uniform int u_flip_subp;\n"
    "uniform int u_has_back;\n"
    "uniform float u_scroll_y;\n"
    "uniform float u_scroll_limit_y;\n"
    "uniform float u_source_max_y;\n"
    "vec3 base_color(vec2 source) {\n"
    "  if (source.y >= u_bar_range.x && source.y < u_bar_range.y)\n"
    "    return vec3(232.0 / 255.0);\n"
    "  return vec3(16.0 / 255.0, 16.0 / 255.0, 20.0 / 255.0);\n"
    "}\n"
    "vec4 sample_bounded_x(sampler2D image, float disp, vec4 bounds, float angle, vec2 sample_pos) {\n"
    "  float lx = sample_pos.x + disp * angle;\n"
    "  if (lx >= bounds.x && lx <= bounds.z - 1.0)\n"
    "    return texture2D(image, vec2(lx + 0.5, sample_pos.y + 0.5) * u_inv_resolution);\n"
    "  if (lx <= bounds.x - 1.0 || lx >= bounds.z)\n"
    "    return vec4(0.0);\n"
    "  float sx = clamp(lx, bounds.x, bounds.z - 1.0);\n"
    "  float w = min(1.0, min(lx - bounds.x + 1.0, bounds.z - lx));\n"
    "  return texture2D(image, vec2(sx + 0.5, sample_pos.y + 0.5) * u_inv_resolution) * w;\n"
    "}\n"
    "vec4 sample_back(float angle, vec2 sample_pos) {\n"
    "  float lx = sample_pos.x + u_back_disparity * angle;\n"
    "  if (lx >= 0.0 && lx <= u_resolution.x - 1.0)\n"
    "    return texture2D(u_layer1, vec2(lx + 0.5, sample_pos.y + 0.5) * u_inv_resolution);\n"
    "  if (lx <= -1.0 || lx >= u_resolution.x)\n"
    "    return vec4(0.0);\n"
    "  float sx = clamp(lx, 0.0, u_resolution.x - 1.0);\n"
    "  float w = min(1.0, min(lx + 1.0, u_resolution.x - lx));\n"
    "  return texture2D(u_layer1, vec2(sx + 0.5, sample_pos.y + 0.5) * u_inv_resolution) * w;\n"
    "}\n"
    "vec3 composite_anim(float angle, vec2 sample_pos, vec3 base, bool in_first_y, bool in_second_y) {\n"
    "  vec3 color = vec3(0.0);\n"
    "  float rem = 1.0;\n"
    "  if (in_first_y) {\n"
    "    vec4 top = sample_bounded_x(u_layer0, u_first_disparity, u_first_bounds, angle, sample_pos);\n"
    "    if (top.a >= 0.999)\n"
    "      return top.rgb;\n"
    "    color = top.rgb;\n"
    "    rem = 1.0 - top.a;\n"
    "  }\n"
    "  if (in_second_y) {\n"
    "    vec4 mid = sample_bounded_x(u_layer0, u_second_disparity, u_second_bounds, angle, sample_pos);\n"
    "    if (mid.a > 0.0) {\n"
    "      color += mid.rgb * rem;\n"
    "      rem *= (1.0 - mid.a);\n"
    "      if (rem <= 0.001)\n"
    "        return color;\n"
    "    }\n"
    "  }\n"
    "  if (u_has_back == 0)\n"
    "    return color + base * rem;\n"
    "  vec4 bg = sample_back(angle, sample_pos);\n"
    "  vec3 under = bg.rgb + base * (1.0 - bg.a);\n"
    "  return color + under * rem;\n"
    "}\n"
    "void main() {\n"
    "  vec2 frag = gl_FragCoord.xy;\n"
    "  vec2 source = vec2(frag.x - 0.5, u_resolution.y - frag.y - 0.5);\n"
    "  vec3 base = base_color(source);\n"
    "  bool scrolled = u_scroll_limit_y > 0.0 && source.y < u_scroll_limit_y;\n"
    "  float sy = scrolled ? source.y + u_scroll_y : source.y;\n"
    "  if (scrolled && (sy < 0.0 || (u_source_max_y > 0.0 && sy >= u_source_max_y))) {\n"
    "    gl_FragColor = vec4(base, 1.0);\n"
    "    return;\n"
    "  }\n"
    "  bool in_first_y = u_first_bounds.z > u_first_bounds.x && sy >= u_first_bounds.y && sy < u_first_bounds.w;\n"
    "  bool in_second_y = u_second_bounds.z > u_second_bounds.x && sy >= u_second_bounds.y && sy < u_second_bounds.w;\n"
    "  if (!in_first_y && !in_second_y && u_has_back == 0) {\n"
    "    gl_FragColor = vec4(base, 1.0);\n"
    "    return;\n"
    "  }\n"
    "  vec2 sample_pos = vec2(source.x, sy);\n"
    "  vec2 pixel = frag * u_inv_resolution;\n"
    "  float base_phase = (pixel.x + pixel.y * u_tilt) * u_pitch - u_center + u_fringe;\n"
    "  float sub_step = u_pitch / (3.0 * u_resolution.x);\n"
    "  float p0 = u_flip_subp != 0 ? base_phase + 2.0 * sub_step : base_phase;\n"
    "  float p1 = base_phase + sub_step;\n"
    "  float p2 = u_flip_subp != 0 ? base_phase : base_phase + 2.0 * sub_step;\n"
    "  p0 = u_inv_view != 0 ? 1.0 - fract(p0) : fract(p0);\n"
    "  p1 = u_inv_view != 0 ? 1.0 - fract(p1) : fract(p1);\n"
    "  p2 = u_inv_view != 0 ? 1.0 - fract(p2) : fract(p2);\n"
    "  if (!in_first_y && !in_second_y && u_has_back != 0) {\n"
    "    vec4 br = sample_back(2.0 * p0 - 1.0, sample_pos);\n"
    "    vec4 bg = sample_back(2.0 * p1 - 1.0, sample_pos);\n"
    "    vec4 bb = sample_back(2.0 * p2 - 1.0, sample_pos);\n"
    "    gl_FragColor = vec4(br.r + base.r * (1.0 - br.a),\n"
    "                        bg.g + base.g * (1.0 - bg.a),\n"
    "                        bb.b + base.b * (1.0 - bb.a), 1.0);\n"
    "    return;\n"
    "  }\n"
    "  vec3 r = composite_anim(2.0 * p0 - 1.0, sample_pos, base, in_first_y, in_second_y);\n"
    "  vec3 g = composite_anim(2.0 * p1 - 1.0, sample_pos, base, in_first_y, in_second_y);\n"
    "  vec3 b = composite_anim(2.0 * p2 - 1.0, sample_pos, base, in_first_y, in_second_y);\n"
    "  gl_FragColor = vec4(r.r, g.g, b.b, 1.0);\n"
    "}\n";

static int has_extension(const char *extensions, const char *name)
{
    const char *position = extensions;
    size_t length = strlen(name);
    if (!extensions)
        return 0;
    while ((position = strstr(position, name)) != NULL) {
        if ((position == extensions || position[-1] == ' ') &&
            (position[length] == ' ' || position[length] == '\0'))
            return 1;
        position += length;
    }
    return 0;
}

static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint status;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "Depth shader compilation failed: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint create_program(const char *vertex_shader_source, const char *fragment_shader_source)
{
    GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
    GLuint fragment;
    GLuint program;
    GLint status;
    if (!vertex)
        return 0;
    fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
    if (!fragment) {
        glDeleteShader(vertex);
        return 0;
    }
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "a_position");
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    if (!status) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "Depth shader linking failed: %s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static void framebuffer_destroy(struct gbm_bo *bo, void *data)
{
    DepthFramebuffer *framebuffer = data;
    (void)bo;
    if (framebuffer) {
        if (framebuffer->id)
            drmModeRmFB(framebuffer->fd, framebuffer->id);
        free(framebuffer);
    }
}

static void destroy_image_khr(EGLDisplay egl_display, PFNEGLDESTROYIMAGEKHRPROC destroy_fn,
                              EGLImageKHR image)
{
    if (image != EGL_NO_IMAGE_KHR && destroy_fn)
        destroy_fn(egl_display, image);
}

static DepthFramebuffer *framebuffer_for_bo(DepthGpu *gpu, Display *display,
                                             struct gbm_bo *bo)
{
    DepthFramebuffer *framebuffer = gbm_bo_get_user_data(bo);
    uint32_t handles[4] = {0};
    uint32_t strides[4] = {0};
    uint32_t offsets[4] = {0};
    uint64_t modifiers[4] = {0};
    uint64_t modifier;
    int added = -1;
    (void)gpu;
    if (framebuffer)
        return framebuffer;
    framebuffer = calloc(1, sizeof(*framebuffer));
    if (!framebuffer)
        return NULL;
    framebuffer->fd = display->fd;
    handles[0] = gbm_bo_get_handle(bo).u32;
    strides[0] = gbm_bo_get_stride(bo);
    offsets[0] = gbm_bo_get_offset(bo, 0);
    modifier = gbm_bo_get_modifier(bo);
    if (modifier != DRM_FORMAT_MOD_INVALID && modifier != DRM_FORMAT_MOD_LINEAR) {
        modifiers[0] = modifier;
        added = drmModeAddFB2WithModifiers(display->fd, gbm_bo_get_width(bo),
                                           gbm_bo_get_height(bo), gbm_bo_get_format(bo),
                                           handles, strides, offsets, modifiers,
                                           &framebuffer->id, DRM_MODE_FB_MODIFIERS);
    }
    if (added != 0 &&
        (modifier == DRM_FORMAT_MOD_INVALID || modifier == DRM_FORMAT_MOD_LINEAR ||
         errno == ENOTSUP)) {
        added = drmModeAddFB2(display->fd, gbm_bo_get_width(bo),
                              gbm_bo_get_height(bo), gbm_bo_get_format(bo),
                              handles, strides, offsets, &framebuffer->id, 0);
    }
    if (added != 0) {
        free(framebuffer);
        return NULL;
    }
    gbm_bo_set_user_data(bo, framebuffer, framebuffer_destroy);
    return framebuffer;
}

static void page_flip_complete(void *data)
{
    DepthGpu *gpu = data;
    if (gpu->partial_scanout) {
        if (gpu->pending_slot >= 0) {
            gpu->front_slot = gpu->pending_slot;
            gpu->pending_slot = -1;
        }
        return;
    }
    if (!gpu->pending_bo)
        return;
    if (gpu->scanout_bo)
        gbm_surface_release_buffer(gpu->gbm_surface, gpu->scanout_bo);
    gpu->scanout_bo = gpu->pending_bo;
    gpu->pending_bo = NULL;
}

static void mark_slots_dirty(DepthGpu *gpu, int x, int y, int width, int height)
{
    int idx;
    int x1 = x + width;
    int y1 = y + height;
    if (!gpu->partial_scanout || width <= 0 || height <= 0)
        return;
    for (idx = 0; idx < 2; ++idx) {
        ScanoutSlot *entry = &gpu->slots[idx];
        if (entry->dirty_full)
            continue;
        if (entry->dirty_x1 <= entry->dirty_x0 || entry->dirty_y1 <= entry->dirty_y0) {
            entry->dirty_x0 = x;
            entry->dirty_y0 = y;
            entry->dirty_x1 = x1;
            entry->dirty_y1 = y1;
        } else {
            if (x < entry->dirty_x0)
                entry->dirty_x0 = x;
            if (y < entry->dirty_y0)
                entry->dirty_y0 = y;
            if (x1 > entry->dirty_x1)
                entry->dirty_x1 = x1;
            if (y1 > entry->dirty_y1)
                entry->dirty_y1 = y1;
        }
    }
}

static void mark_slots_dirty_full(DepthGpu *gpu)
{
    int idx;
    if (!gpu->partial_scanout)
        return;
    for (idx = 0; idx < 2; ++idx)
        gpu->slots[idx].dirty_full = 1;
}

static void scanout_slots_destroy_gl(DepthGpu *gpu)
{
    int idx;
    for (idx = 0; idx < 2; ++idx) {
        ScanoutSlot *entry = &gpu->slots[idx];
        if (entry->fbo)
            glDeleteFramebuffers(1, &entry->fbo);
        if (entry->texture)
            glDeleteTextures(1, &entry->texture);
        entry->fbo = 0;
        entry->texture = 0;
    }
}

static void scanout_slots_destroy_buffers(DepthGpu *gpu)
{
    int idx;
    for (idx = 0; idx < 2; ++idx) {
        ScanoutSlot *entry = &gpu->slots[idx];
        if (entry->image != EGL_NO_IMAGE_KHR && gpu->display != EGL_NO_DISPLAY)
            destroy_image_khr(gpu->display, gpu->destroy_image, entry->image);
        if (entry->bo)
            gbm_bo_destroy(entry->bo);
        memset(entry, 0, sizeof(*entry));
        entry->image = EGL_NO_IMAGE_KHR;
    }
    gpu->partial_scanout = 0;
    gpu->front_slot = -1;
    gpu->pending_slot = -1;
}

static int scanout_slots_init_with_flags(DepthGpu *gpu, Display *display,
                                         uint32_t bo_flags, int has_mods,
                                         PFNEGLCREATEIMAGEKHRPROC create_image,
                                         PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target_tex)
{
    const int width = display->mode.hdisplay;
    const int height = display->mode.vdisplay;
    int idx;
    for (idx = 0; idx < 2; ++idx) {
        ScanoutSlot *entry = &gpu->slots[idx];
        EGLint attrs[20];
        int attr_count = 0;
        int dmabuf_fd;
        uint32_t stride;
        uint32_t offset;
        uint64_t modifier;
        entry->image = EGL_NO_IMAGE_KHR;
        entry->bo = gbm_bo_create(gpu->gbm, (uint32_t)width, (uint32_t)height,
                                  GBM_FORMAT_ARGB8888, bo_flags);
        if (!entry->bo)
            goto fail;
        entry->fb = framebuffer_for_bo(gpu, display, entry->bo);
        if (!entry->fb)
            goto fail;
        dmabuf_fd = gbm_bo_get_fd(entry->bo);
        if (dmabuf_fd < 0)
            goto fail;
        stride = gbm_bo_get_stride(entry->bo);
        offset = gbm_bo_get_offset(entry->bo, 0);
        modifier = gbm_bo_get_modifier(entry->bo);
        attrs[attr_count++] = EGL_WIDTH;
        attrs[attr_count++] = width;
        attrs[attr_count++] = EGL_HEIGHT;
        attrs[attr_count++] = height;
        attrs[attr_count++] = EGL_LINUX_DRM_FOURCC_EXT;
        attrs[attr_count++] = DRM_FORMAT_ARGB8888;
        attrs[attr_count++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        attrs[attr_count++] = dmabuf_fd;
        attrs[attr_count++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        attrs[attr_count++] = (EGLint)offset;
        attrs[attr_count++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        attrs[attr_count++] = (EGLint)stride;
        if (has_mods && modifier != DRM_FORMAT_MOD_INVALID) {
            attrs[attr_count++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            attrs[attr_count++] = (EGLint)(modifier & 0xffffffffu);
            attrs[attr_count++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            attrs[attr_count++] = (EGLint)(modifier >> 32);
        }
        attrs[attr_count++] = EGL_NONE;
        entry->image = create_image(gpu->display, EGL_NO_CONTEXT,
                                    EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
        close(dmabuf_fd);
        if (entry->image == EGL_NO_IMAGE_KHR)
            goto fail;
        glGenTextures(1, &entry->texture);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, entry->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        target_tex(GL_TEXTURE_2D, entry->image);
        glGenFramebuffers(1, &entry->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, entry->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               entry->texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
            glGetError() != GL_NO_ERROR)
            goto fail;
        entry->dirty_full = 1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;
fail:
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    while (glGetError() != GL_NO_ERROR) {}
    scanout_slots_destroy_gl(gpu);
    scanout_slots_destroy_buffers(gpu);
    return -1;
}

static void scanout_slots_init(DepthGpu *gpu, Display *display)
{
    const char *egl_extensions = eglQueryString(gpu->display, EGL_EXTENSIONS);
    const char *gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
    int has_mods = has_extension(egl_extensions, "EGL_EXT_image_dma_buf_import_modifiers");
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target_tex;
    gpu->front_slot = -1;
    gpu->pending_slot = -1;
    gpu->slots[0].image = EGL_NO_IMAGE_KHR;
    gpu->slots[1].image = EGL_NO_IMAGE_KHR;
    if (!has_extension(egl_extensions, "EGL_KHR_image_base") ||
        !has_extension(egl_extensions, "EGL_EXT_image_dma_buf_import") ||
        !has_extension(gl_extensions, "GL_OES_EGL_image"))
        return;
    create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    gpu->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    target_tex = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!create_image || !gpu->destroy_image || !target_tex)
        return;
    gpu->blit_flip_program = create_program(blit_flip_vertex_source, blit_fragment_source);
    if (!gpu->blit_flip_program)
        return;
    gpu->blit_flip_cache_uniform = glGetUniformLocation(gpu->blit_flip_program, "u_cache");
    glUseProgram(gpu->blit_flip_program);
    glUniform1i(gpu->blit_flip_cache_uniform, 0);
    if (scanout_slots_init_with_flags(gpu, display,
                                      GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING,
                                      has_mods, create_image, target_tex) == 0 ||
        scanout_slots_init_with_flags(gpu, display,
                                      GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING |
                                      GBM_BO_USE_LINEAR,
                                      has_mods, create_image, target_tex) == 0) {
        gpu->partial_scanout = 1;
        return;
    }
    if (gpu->blit_flip_program) {
        glDeleteProgram(gpu->blit_flip_program);
        gpu->blit_flip_program = 0;
    }
}

static void release_bake_textures(DepthGpu *gpu)
{
    int cell;
    int slot;
    if (gpu->gallery_base_texture)
        glDeleteTextures(1, &gpu->gallery_base_texture);
    gpu->gallery_base_texture = 0;
    gpu->gallery_base_width = 0;
    gpu->gallery_base_height = 0;
    for (cell = 0; cell < 9; ++cell) {
        for (slot = 0; slot < 2; ++slot) {
            if (gpu->gallery_patches[cell][slot].texture)
                glDeleteTextures(1, &gpu->gallery_patches[cell][slot].texture);
            memset(&gpu->gallery_patches[cell][slot], 0,
                   sizeof(gpu->gallery_patches[cell][slot]));
        }
    }
}

void depth_gpu_gallery_release_bake(DepthGpu *gpu)
{
    if (!gpu || gpu->display == EGL_NO_DISPLAY ||
        gpu->context == EGL_NO_CONTEXT || gpu->surface == EGL_NO_SURFACE)
        return;
    if (!eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context))
        return;
    release_bake_textures(gpu);
}

static void destroy_gl(DepthGpu *gpu)
{
    if (gpu->owner)
        display_wait_for_flip(gpu->owner);
    if (gpu->display != EGL_NO_DISPLAY) {
        if (gpu->context != EGL_NO_CONTEXT && gpu->surface != EGL_NO_SURFACE &&
            eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context)) {
            if (gpu->textures[0])
                glDeleteTextures(2, gpu->textures);
            if (gpu->program)
                glDeleteProgram(gpu->program);
            if (gpu->anim_program)
                glDeleteProgram(gpu->anim_program);
            if (gpu->blit_program)
                glDeleteProgram(gpu->blit_program);
            if (gpu->blit_flip_program)
                glDeleteProgram(gpu->blit_flip_program);
            if (gpu->cache_texture)
                glDeleteTextures(1, &gpu->cache_texture);
            if (gpu->cache_framebuffer)
                glDeleteFramebuffers(1, &gpu->cache_framebuffer);
            scanout_slots_destroy_gl(gpu);
            release_bake_textures(gpu);
        }
        eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        scanout_slots_destroy_buffers(gpu);
        if (gpu->surface != EGL_NO_SURFACE)
            eglDestroySurface(gpu->display, gpu->surface);
        if (gpu->context != EGL_NO_CONTEXT)
            eglDestroyContext(gpu->display, gpu->context);
        eglTerminate(gpu->display);
    } else {
        scanout_slots_destroy_buffers(gpu);
    }
    if (gpu->scanout_bo)
        gbm_surface_release_buffer(gpu->gbm_surface, gpu->scanout_bo);
    if (gpu->pending_bo)
        gbm_surface_release_buffer(gpu->gbm_surface, gpu->pending_bo);
    if (gpu->owner && gpu->owner->flip_data == gpu) {
        gpu->owner->flip_complete = NULL;
        gpu->owner->flip_data = NULL;
    }
    if (gpu->gbm_surface)
        gbm_surface_destroy(gpu->gbm_surface);
    if (gpu->gbm)
        gbm_device_destroy(gpu->gbm);
    free(gpu->upload_scratch);
    free(gpu);
}

DepthGpu *depth_gpu_create(Display *display)
{
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    const char *client_extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    DepthGpu *gpu = calloc(1, sizeof(*gpu));
    EGLConfig config;
    EGLint count;
    if (!gpu)
        return NULL;
    gpu->owner = display;
    display->flip_complete = page_flip_complete;
    display->flip_data = gpu;
    gpu->display = EGL_NO_DISPLAY;
    gpu->context = EGL_NO_CONTEXT;
    gpu->surface = EGL_NO_SURFACE;
    gpu->slots[0].image = EGL_NO_IMAGE_KHR;
    gpu->slots[1].image = EGL_NO_IMAGE_KHR;
    gpu->front_slot = -1;
    gpu->pending_slot = -1;
    gpu->gbm = gbm_create_device(display->fd);
    if (!gpu->gbm)
        goto fail;
    gpu->gbm_surface = gbm_surface_create(gpu->gbm, display->mode.hdisplay,
                                          display->mode.vdisplay, GBM_FORMAT_ARGB8888,
                                          GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    if (!gpu->gbm_surface)
        goto fail;
    if (has_extension(client_extensions, "EGL_EXT_platform_base")) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (get_platform_display)
            gpu->display = get_platform_display(EGL_PLATFORM_GBM_KHR, gpu->gbm, NULL);
    }
    if (gpu->display == EGL_NO_DISPLAY)
        gpu->display = eglGetDisplay((EGLNativeDisplayType)gpu->gbm);
    if (gpu->display == EGL_NO_DISPLAY || !eglInitialize(gpu->display, NULL, NULL))
        goto fail;
    if (!eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(gpu->display, config_attributes, &config, 1, &count) || count < 1)
        goto fail;
    gpu->context = eglCreateContext(gpu->display, config, EGL_NO_CONTEXT, context_attributes);
    if (gpu->context == EGL_NO_CONTEXT)
        goto fail;
    gpu->surface = eglCreateWindowSurface(gpu->display, config,
                                          (EGLNativeWindowType)gpu->gbm_surface, NULL);
    if (gpu->surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context))
        goto fail;
    gpu->program = create_program(vertex_source, fragment_source);
    if (!gpu->program)
        goto fail;
    cache_uniform_locations(gpu);
    gpu->unpack_subimage = has_extension((const char *)glGetString(GL_EXTENSIONS),
                                         "GL_EXT_unpack_subimage");
    glGenTextures(2, gpu->textures);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    fprintf(stderr, "Depth subimage upload: %s\n",
            depth_gpu_direct_upload(gpu) ? "enabled" : "unavailable");
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    scanout_slots_init(gpu, display);
    fprintf(stderr, "Depth partial scanout: %s\n",
            gpu->partial_scanout ? "enabled" : "unavailable");
    return gpu;
fail:
    fprintf(stderr, "Depth GPU initialization failed (EGL error 0x%x)\n", eglGetError());
    destroy_gl(gpu);
    return NULL;
}

void depth_gpu_destroy(DepthGpu *gpu)
{
    if (gpu)
        destroy_gl(gpu);
}

static int present(DepthGpu *gpu, Display *display)
{
    struct gbm_bo *bo;
    DepthFramebuffer *framebuffer;
    glFinish();
    if (!eglSwapBuffers(gpu->display, gpu->surface)) {
        fprintf(stderr, "Depth eglSwapBuffers failed: 0x%x\n", eglGetError());
        return -1;
    }
    bo = gbm_surface_lock_front_buffer(gpu->gbm_surface);
    if (!bo) {
        fprintf(stderr, "Depth GBM front buffer lock failed\n");
        return -1;
    }
    framebuffer = framebuffer_for_bo(gpu, display, bo);
    if (!framebuffer) {
        gbm_surface_release_buffer(gpu->gbm_surface, bo);
        fprintf(stderr, "Depth framebuffer creation failed: %s\n", strerror(errno));
        return -1;
    }
    if (!display->active_framebuffer ||
        display->active_framebuffer != gpu->current_framebuffer) {
        if (drmModeSetCrtc(display->fd, display->crtc_id, framebuffer->id, 0, 0,
                           &display->connector_id, 1, &display->mode) != 0) {
            gbm_surface_release_buffer(gpu->gbm_surface, bo);
            fprintf(stderr, "Depth mode set failed: %s\n", strerror(errno));
            return -1;
        }
        if (gpu->scanout_bo)
            gbm_surface_release_buffer(gpu->gbm_surface, gpu->scanout_bo);
        gpu->scanout_bo = bo;
    } else {
        if (drmModePageFlip(display->fd, display->crtc_id, framebuffer->id,
                            DRM_MODE_PAGE_FLIP_EVENT, display) != 0) {
            gbm_surface_release_buffer(gpu->gbm_surface, bo);
            fprintf(stderr, "Depth page flip failed: %s\n", strerror(errno));
            return -1;
        }
        gpu->pending_bo = bo;
        display->flip_pending = 1;
    }
    display->active_framebuffer = framebuffer->id;
    gpu->current_framebuffer = framebuffer->id;
    return 0;
}

static void cache_uniform_locations(DepthGpu *gpu)
{
    static const char *sampler_names[] = { "u_layer0", "u_layer1" };
    DepthUniforms *uniforms = &gpu->uniforms;
    int i;
    for (i = 0; i < 2; ++i)
        uniforms->layer[i] = glGetUniformLocation(gpu->program, sampler_names[i]);
    uniforms->resolution = glGetUniformLocation(gpu->program, "u_resolution");
    uniforms->frag_offset = glGetUniformLocation(gpu->program, "u_frag_offset");
    uniforms->layer_origin = glGetUniformLocation(gpu->program, "u_layer_origin");
    uniforms->layer_size = glGetUniformLocation(gpu->program, "u_layer_size");
    uniforms->disparity = glGetUniformLocation(gpu->program, "u_disparity");
    uniforms->bar_range = glGetUniformLocation(gpu->program, "u_bar_range");
    uniforms->pitch = glGetUniformLocation(gpu->program, "u_pitch");
    uniforms->tilt = glGetUniformLocation(gpu->program, "u_tilt");
    uniforms->center = glGetUniformLocation(gpu->program, "u_center");
    uniforms->fringe = glGetUniformLocation(gpu->program, "u_fringe");
    uniforms->inv_view = glGetUniformLocation(gpu->program, "u_inv_view");
    uniforms->flip_subp = glGetUniformLocation(gpu->program, "u_flip_subp");
    uniforms->interlace = glGetUniformLocation(gpu->program, "u_interlace");
    uniforms->stereo = glGetUniformLocation(gpu->program, "u_stereo");
    glUseProgram(gpu->program);
    for (i = 0; i < 2; ++i)
        glUniform1i(uniforms->layer[i], i);
}

static void set_uniforms(DepthGpu *gpu, Display *display, const DepthEffect *effect,
                         const DepthLayer *layers, int bar_top, int bar_bottom)
{
    const DepthUniforms *uniforms = &gpu->uniforms;
    float pitch = effect->pitch * effect->screen_w / effect->dpi *
                  sinf(atanf(fabsf(effect->slope)));
    float tilt = effect->screen_h / (effect->screen_w * effect->slope);
    glUseProgram(gpu->program);
    glUniform2f(uniforms->resolution, display->mode.hdisplay, display->mode.vdisplay);
    glUniform2f(uniforms->frag_offset, 0.0f, 0.0f);
    glUniform2f(uniforms->layer_origin, 0.0f, 0.0f);
    glUniform2f(uniforms->layer_size, display->mode.hdisplay, display->mode.vdisplay);
    glUniform2f(uniforms->disparity, layers[0].disparity, layers[1].disparity);
    glUniform2f(uniforms->bar_range, bar_top, bar_bottom);
    glUniform1f(uniforms->pitch, pitch);
    glUniform1f(uniforms->tilt, tilt);
    glUniform1f(uniforms->center, effect->center);
    glUniform1f(uniforms->fringe, effect->fringe);
    glUniform1i(uniforms->inv_view, effect->inv_view);
    glUniform1i(uniforms->flip_subp, effect->flip_subp);
}

static void draw_vertices(void)
{
    static const float vertices[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static int gallery_cache_init(DepthGpu *gpu, Display *display)
{
    const int width = display->mode.hdisplay;
    const int height = display->mode.vdisplay;
    if (!gpu->blit_program) {
        gpu->blit_program = create_program(blit_vertex_source, blit_fragment_source);
        if (gpu->blit_program) {
            gpu->blit_cache_uniform = glGetUniformLocation(gpu->blit_program, "u_cache");
            glUseProgram(gpu->blit_program);
            glUniform1i(gpu->blit_cache_uniform, 0);
        }
    }
    if (!gpu->blit_program)
        return -1;
    if (!gpu->cache_texture) {
        glGenTextures(1, &gpu->cache_texture);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gpu->cache_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (!gpu->cache_framebuffer)
        glGenFramebuffers(1, &gpu->cache_framebuffer);
    if (gpu->cache_width != width || gpu->cache_height != height) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gpu->cache_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, gpu->cache_framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               gpu->cache_texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return -1;
        gpu->cache_width = width;
        gpu->cache_height = height;
    }
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}

int depth_gpu_gallery_begin(DepthGpu *gpu, Display *display)
{
    if (gpu->failed ||
        !eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context) ||
        gallery_cache_init(gpu, display) != 0)
        return -1;
    glBindFramebuffer(GL_FRAMEBUFFER, gpu->cache_framebuffer);
    glViewport(0, 0, gpu->cache_width, gpu->cache_height);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(16.0f / 255.0f, 16.0f / 255.0f, 20.0f / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    mark_slots_dirty_full(gpu);
    advance_cache_revision(gpu);
    return 0;
}

int depth_gpu_direct_upload(DepthGpu *gpu)
{
    return gpu && gpu->unpack_subimage;
}

static uint32_t premultiply_partial(uint32_t pixel, uint32_t alpha)
{
    uint32_t t = (pixel & 0xffu) * alpha + 128u;
    uint32_t b = (t + (t >> 8)) >> 8;
    uint32_t g;
    uint32_t r;
    t = ((pixel >> 8) & 0xffu) * alpha + 128u;
    g = (t + (t >> 8)) >> 8;
    t = ((pixel >> 16) & 0xffu) * alpha + 128u;
    r = (t + (t >> 8)) >> 8;
    return (alpha << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t *ensure_upload_scratch(DepthGpu *gpu, size_t pixel_count)
{
    size_t bytes;
    if (pixel_count > SIZE_MAX / sizeof(uint32_t))
        return NULL;
    bytes = pixel_count * sizeof(uint32_t);
    if (gpu->upload_scratch_bytes < bytes) {
        uint32_t *buf = realloc(gpu->upload_scratch, bytes);
        if (!buf)
            return NULL;
        gpu->upload_scratch = buf;
        gpu->upload_scratch_bytes = bytes;
    }
    return gpu->upload_scratch;
}

static void premultiply_rect(const uint32_t *src, uint32_t src_stride_pixels,
                             int width, int height, uint32_t *dst)
{
    int row;
    for (row = 0; row < height; ++row) {
        const uint32_t *src_row = src + (size_t)row * (size_t)src_stride_pixels;
        uint32_t *dst_row = dst + (size_t)row * (size_t)width;
        int col;
        for (col = 0; col < width; ++col) {
            uint32_t p = src_row[col];
            uint32_t a = p >> 24;
            if (a == 0u)
                dst_row[col] = 0u;
            else if (a == 255u)
                dst_row[col] = p;
            else
                dst_row[col] = premultiply_partial(p, a);
        }
    }
}

static void set_layer_filter(DepthGpu *gpu, GLenum filter)
{
    int idx;
    if (!gpu->textures_initialized) {
        for (idx = 0; idx < 2; ++idx) {
            glActiveTexture(GL_TEXTURE0 + idx);
            glBindTexture(GL_TEXTURE_2D, gpu->textures[idx]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)filter);
        }
        gpu->layer_filter = filter;
        gpu->textures_initialized = 1;
        return;
    }
    if (gpu->layer_filter == filter)
        return;
    for (idx = 0; idx < 2; ++idx) {
        glActiveTexture(GL_TEXTURE0 + idx);
        glBindTexture(GL_TEXTURE_2D, gpu->textures[idx]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)filter);
    }
    gpu->layer_filter = filter;
}

static int gallery_upload(DepthGpu *gpu, Display *display, const DepthLayer *layers,
                          const uint8_t *const pixels[2],
                          int x, int y, int width, int height)
{
    const int screen_w = display->mode.hdisplay;
    const int screen_h = display->mode.vdisplay;
    int direct_upload = depth_gpu_direct_upload(gpu);
    uint32_t *scratch;
    int i;
    if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
        x + width > screen_w || y + height > screen_h)
        return -1;
    if (direct_upload &&
        (!layers[0].buffer.pixels || !layers[1].buffer.pixels ||
         layers[0].buffer.stride != (uint32_t)screen_w * 4 ||
         layers[1].buffer.stride != (uint32_t)screen_w * 4))
        return -1;
    if (!direct_upload && (!pixels || !pixels[0] || !pixels[1]))
        return -1;
    scratch = ensure_upload_scratch(gpu, (size_t)width * (size_t)height);
    if (!scratch)
        return -1;
    if (direct_upload && (gpu->layer_width != screen_w || gpu->layer_height != screen_h)) {
        for (i = 0; i < 2; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, gpu->textures[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, screen_w, screen_h, 0,
                         GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
        }
        gpu->layer_width = screen_w;
        gpu->layer_height = screen_h;
    }
    if (!gpu->textures_initialized)
        set_layer_filter(gpu, GL_NEAREST);
    for (i = 0; i < 2; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, gpu->textures[i]);
        if (direct_upload) {
            const uint32_t stride_pixels = layers[i].buffer.stride / 4;
            const uint32_t *src = (const uint32_t *)(layers[i].buffer.pixels +
                                  (size_t)y * (size_t)layers[i].buffer.stride +
                                  (size_t)x * 4);
            premultiply_rect(src, stride_pixels, width, height, scratch);
            glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, 0);
            glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, 0);
            glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height,
                            GL_BGRA_EXT, GL_UNSIGNED_BYTE, scratch);
        } else {
            const uint32_t *src = (const uint32_t *)pixels[i];
            premultiply_rect(src, (uint32_t)width, width, height, scratch);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                         GL_BGRA_EXT, GL_UNSIGNED_BYTE, scratch);
        }
    }
    if (!direct_upload) {
        gpu->layer_width = width;
        gpu->layer_height = height;
    }
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}

int depth_gpu_gallery_upload(DepthGpu *gpu, Display *display, const DepthLayer *layers,
                             int x, int y, int width, int height)
{
    if (gpu->failed || !depth_gpu_direct_upload(gpu) ||
        !eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context))
        return -1;
    return gallery_upload(gpu, display, layers, NULL, x, y, width, height);
}

static int cached_region(DepthGpu *gpu, Display *display, const DepthEffect *effect,
                         const DepthLayer *layers, const uint8_t *const pixels[2],
                         int x, int y, int width, int height,
                         int clip_x, int clip_y, int clip_width, int clip_height, int upload,
                         int bar_top, int bar_bottom)
{
    const int screen_w = display->mode.hdisplay;
    const int screen_h = display->mode.vdisplay;
    int direct_upload;
    int band_y;
    if (gpu->failed || x < 0 || y < 0 || width <= 0 || height <= 0 ||
        x + width > screen_w || y + height > screen_h ||
        clip_x < x || clip_y < y || clip_width <= 0 || clip_height <= 0 ||
        clip_x + clip_width > x + width || clip_y + clip_height > y + height ||
        !eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context) ||
        gallery_cache_init(gpu, display) != 0)
        return -1;
    if (upload && gallery_upload(gpu, display, layers, pixels, x, y, width, height) != 0)
        return -1;
    direct_upload = depth_gpu_direct_upload(gpu);
    set_layer_filter(gpu, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, gpu->cache_framebuffer);
    glViewport(0, 0, gpu->cache_width, gpu->cache_height);
    glEnable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    set_uniforms(gpu, display, effect, layers, bar_top, bar_bottom);
    glUniform2f(gpu->uniforms.layer_origin,
                direct_upload ? 0.0f : x, direct_upload ? 0.0f : y);
    glUniform2f(gpu->uniforms.layer_size,
                direct_upload ? screen_w : width, direct_upload ? screen_h : height);
    glUniform1i(gpu->uniforms.interlace, 1);
    glUniform1i(gpu->uniforms.stereo, effect->stereo);
    for (band_y = 0; band_y < clip_height; band_y += 1024) {
        int band_height = clip_height - band_y < 1024 ? clip_height - band_y : 1024;
        glScissor(clip_x, gpu->cache_height - clip_y - band_y - band_height,
                  clip_width, band_height);
        draw_vertices();
        glFlush();
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (glGetError() != GL_NO_ERROR)
        return -1;
    mark_slots_dirty(gpu, clip_x, clip_y, clip_width, clip_height);
    advance_cache_revision(gpu);
    return 0;
}

int depth_gpu_cached_region(DepthGpu *gpu, Display *display, const DepthEffect *effect,
                            const DepthLayer *layers, int x, int y, int width, int height,
                            int bar_top, int bar_bottom)
{
    return cached_region(gpu, display, effect, layers, NULL, x, y, width, height,
                         x, y, width, height, 1, bar_top, bar_bottom);
}

int depth_gpu_gallery_region(DepthGpu *gpu, Display *display, const DepthEffect *effect,
                             const DepthLayer *layers, const uint8_t *const pixels[2],
                             int x, int y, int width, int height,
                             int clip_x, int clip_y, int clip_width, int clip_height, int upload)
{
    return cached_region(gpu, display, effect, layers, pixels, x, y, width, height,
                         clip_x, clip_y, clip_width, clip_height, upload, 0, 0);
}

static int anim_program_init(DepthGpu *gpu)
{
    DepthAnimUniforms *u = &gpu->anim_uniforms;
    if (gpu->anim_program)
        return 0;
    gpu->anim_program = create_program(vertex_source, anim_fragment_source);
    if (!gpu->anim_program)
        return -1;
    u->layer[0] = glGetUniformLocation(gpu->anim_program, "u_layer0");
    u->layer[1] = glGetUniformLocation(gpu->anim_program, "u_layer1");
    u->resolution = glGetUniformLocation(gpu->anim_program, "u_resolution");
    u->inv_resolution = glGetUniformLocation(gpu->anim_program, "u_inv_resolution");
    u->back_disparity = glGetUniformLocation(gpu->anim_program, "u_back_disparity");
    u->first_disparity = glGetUniformLocation(gpu->anim_program, "u_first_disparity");
    u->second_disparity = glGetUniformLocation(gpu->anim_program, "u_second_disparity");
    u->first_bounds = glGetUniformLocation(gpu->anim_program, "u_first_bounds");
    u->second_bounds = glGetUniformLocation(gpu->anim_program, "u_second_bounds");
    u->bar_range = glGetUniformLocation(gpu->anim_program, "u_bar_range");
    u->pitch = glGetUniformLocation(gpu->anim_program, "u_pitch");
    u->tilt = glGetUniformLocation(gpu->anim_program, "u_tilt");
    u->center = glGetUniformLocation(gpu->anim_program, "u_center");
    u->fringe = glGetUniformLocation(gpu->anim_program, "u_fringe");
    u->inv_view = glGetUniformLocation(gpu->anim_program, "u_inv_view");
    u->flip_subp = glGetUniformLocation(gpu->anim_program, "u_flip_subp");
    u->has_back = glGetUniformLocation(gpu->anim_program, "u_has_back");
    u->scroll_y = glGetUniformLocation(gpu->anim_program, "u_scroll_y");
    u->scroll_limit_y = glGetUniformLocation(gpu->anim_program, "u_scroll_limit_y");
    u->source_max_y = glGetUniformLocation(gpu->anim_program, "u_source_max_y");
    glUseProgram(gpu->anim_program);
    glUniform1i(u->layer[0], 0);
    glUniform1i(u->layer[1], 1);
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}

int depth_gpu_gallery_anim_scroll_region(DepthGpu *gpu, Display *display, const DepthEffect *effect,
                                         const DepthLayer *layers,
                                         const DepthAnimRect *first, const DepthAnimRect *second,
                                         int clip_x, int clip_y, int clip_width, int clip_height,
                                         int bar_top, int bar_bottom, int has_back,
                                         float scroll_y, int scroll_limit_y, int source_max_y)
{
    const int screen_w = display->mode.hdisplay;
    const int screen_h = display->mode.vdisplay;
    float pitch;
    float tilt;
    int i;
    if (gpu->failed || !depth_gpu_direct_upload(gpu) ||
        gpu->layer_width != screen_w || gpu->layer_height != screen_h ||
        clip_x < 0 || clip_y < 0 || clip_width <= 0 || clip_height <= 0 ||
        clip_x + clip_width > screen_w || clip_y + clip_height > screen_h ||
        !eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context) ||
        gallery_cache_init(gpu, display) != 0 ||
        anim_program_init(gpu) != 0)
        return -1;
    if (!first && second) {
        first = second;
        second = NULL;
    }
    if (first && second && second->disparity > first->disparity) {
        const DepthAnimRect *tmp = first;
        first = second;
        second = tmp;
    }
    set_layer_filter(gpu, GL_LINEAR);
    for (i = 0; i < 2; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, gpu->textures[i]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, gpu->cache_framebuffer);
    glViewport(0, 0, gpu->cache_width, gpu->cache_height);
    glEnable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glUseProgram(gpu->anim_program);
    pitch = effect->pitch * effect->screen_w / effect->dpi *
            sinf(atanf(fabsf(effect->slope)));
    tilt = effect->screen_h / (effect->screen_w * effect->slope);
    glUniform2f(gpu->anim_uniforms.resolution, (float)screen_w, (float)screen_h);
    glUniform2f(gpu->anim_uniforms.inv_resolution,
                1.0f / (float)screen_w, 1.0f / (float)screen_h);
    glUniform1f(gpu->anim_uniforms.back_disparity, layers[1].disparity);
    if (first) {
        glUniform1f(gpu->anim_uniforms.first_disparity, first->disparity);
        glUniform4f(gpu->anim_uniforms.first_bounds,
                    (float)first->x0, (float)first->y0,
                    (float)first->x1, (float)first->y1);
    } else {
        glUniform1f(gpu->anim_uniforms.first_disparity, 0.0f);
        glUniform4f(gpu->anim_uniforms.first_bounds, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    if (second) {
        glUniform1f(gpu->anim_uniforms.second_disparity, second->disparity);
        glUniform4f(gpu->anim_uniforms.second_bounds,
                    (float)second->x0, (float)second->y0,
                    (float)second->x1, (float)second->y1);
    } else {
        glUniform1f(gpu->anim_uniforms.second_disparity, 0.0f);
        glUniform4f(gpu->anim_uniforms.second_bounds, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    glUniform2f(gpu->anim_uniforms.bar_range, (float)bar_top, (float)bar_bottom);
    glUniform1f(gpu->anim_uniforms.pitch, pitch);
    glUniform1f(gpu->anim_uniforms.tilt, tilt);
    glUniform1f(gpu->anim_uniforms.center, effect->center);
    glUniform1f(gpu->anim_uniforms.fringe, effect->fringe);
    glUniform1i(gpu->anim_uniforms.inv_view, effect->inv_view);
    glUniform1i(gpu->anim_uniforms.flip_subp, effect->flip_subp);
    glUniform1i(gpu->anim_uniforms.has_back, has_back ? 1 : 0);
    glUniform1f(gpu->anim_uniforms.scroll_y, scroll_y);
    glUniform1f(gpu->anim_uniforms.scroll_limit_y, (float)scroll_limit_y);
    glUniform1f(gpu->anim_uniforms.source_max_y, (float)source_max_y);
    glScissor(clip_x, gpu->cache_height - clip_y - clip_height, clip_width, clip_height);
    draw_vertices();
    glFlush();
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (glGetError() != GL_NO_ERROR)
        return -1;
    mark_slots_dirty(gpu, clip_x, clip_y, clip_width, clip_height);
    advance_cache_revision(gpu);
    return 0;
}

int depth_gpu_gallery_anim_region(DepthGpu *gpu, Display *display, const DepthEffect *effect,
                                  const DepthLayer *layers,
                                  const DepthAnimRect *first, const DepthAnimRect *second,
                                  int clip_x, int clip_y, int clip_width, int clip_height,
                                  int bar_top, int bar_bottom, int has_back)
{
    return depth_gpu_gallery_anim_scroll_region(gpu, display, effect, layers,
                                                first, second,
                                                clip_x, clip_y, clip_width, clip_height,
                                                bar_top, bar_bottom, has_back,
                                                0.0f, 0, 0);
}

static void clear_cache_band(int cache_height, int x, int y0, int y1, int width,
                             float r, float g, float b)
{
    if (y1 <= y0)
        return;
    glScissor(x, cache_height - y1, width, y1 - y0);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

int depth_gpu_gallery_anim_clear(DepthGpu *gpu, Display *display,
                                 int clip_x, int clip_y, int clip_width, int clip_height,
                                 int bar_top, int bar_bottom)
{
    const int screen_w = display->mode.hdisplay;
    const int screen_h = display->mode.vdisplay;
    int y0 = clip_y;
    int y1 = clip_y + clip_height;
    if (gpu->failed || clip_x < 0 || clip_y < 0 || clip_width <= 0 || clip_height <= 0 ||
        clip_x + clip_width > screen_w || clip_y + clip_height > screen_h ||
        !eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context) ||
        gallery_cache_init(gpu, display) != 0)
        return -1;
    glBindFramebuffer(GL_FRAMEBUFFER, gpu->cache_framebuffer);
    glViewport(0, 0, gpu->cache_width, gpu->cache_height);
    glEnable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    if (bar_bottom > bar_top && y1 > bar_top && y0 < bar_bottom) {
        int ov0 = y0 > bar_top ? y0 : bar_top;
        int ov1 = y1 < bar_bottom ? y1 : bar_bottom;
        clear_cache_band(gpu->cache_height, clip_x, y0, ov0, clip_width,
                         16.0f / 255.0f, 16.0f / 255.0f, 20.0f / 255.0f);
        clear_cache_band(gpu->cache_height, clip_x, ov0, ov1, clip_width,
                         232.0f / 255.0f, 232.0f / 255.0f, 232.0f / 255.0f);
        clear_cache_band(gpu->cache_height, clip_x, ov1, y1, clip_width,
                         16.0f / 255.0f, 16.0f / 255.0f, 20.0f / 255.0f);
    } else {
        clear_cache_band(gpu->cache_height, clip_x, y0, y1, clip_width,
                         16.0f / 255.0f, 16.0f / 255.0f, 20.0f / 255.0f);
    }
    glFlush();
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (glGetError() != GL_NO_ERROR)
        return -1;
    mark_slots_dirty(gpu, clip_x, clip_y, clip_width, clip_height);
    advance_cache_revision(gpu);
    return 0;
}

int depth_gpu_gallery_bake_base(DepthGpu *gpu, Display *display, const DepthEffect *effect,
                                const DepthLayer *layers, const uint8_t *const pixels[2],
                                int band)
{
    const int screen_w = display->mode.hdisplay;
    const int screen_h = display->mode.vdisplay;
    int band_y = band * GALLERY_BAKE_BAND_HEIGHT;
    int band_height;
    int allocated = 0;
    if (gpu->failed || band < 0 || band_y >= screen_h ||
        (!depth_gpu_direct_upload(gpu) && band == 0 && !pixels) ||
        !eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context) ||
        gallery_cache_init(gpu, display) != 0)
        return -1;
    if (band == 0) {
        if (gallery_upload(gpu, display, layers, pixels, 0, 0, screen_w, screen_h) != 0)
            return -1;
        if (!gpu->gallery_base_texture)
            glGenTextures(1, &gpu->gallery_base_texture);
        if (gpu->gallery_base_width != screen_w || gpu->gallery_base_height != screen_h) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, gpu->gallery_base_texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, screen_w, screen_h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            gpu->gallery_base_width = screen_w;
            gpu->gallery_base_height = screen_h;
            allocated = 1;
        }
    }
    if (!gpu->gallery_base_texture)
        return -1;
    set_layer_filter(gpu, GL_NEAREST);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gpu->textures[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gpu->textures[1]);
    glBindFramebuffer(GL_FRAMEBUFFER, gpu->cache_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           gpu->gallery_base_texture, 0);
    if (allocated && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               gpu->cache_texture, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return -1;
    }
    glViewport(0, 0, screen_w, screen_h);
    glEnable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    set_uniforms(gpu, display, effect, layers, 0, 0);
    glUniform2f(gpu->uniforms.frag_offset, 0.0f, 0.0f);
    glUniform2f(gpu->uniforms.layer_origin, 0.0f, 0.0f);
    glUniform2f(gpu->uniforms.layer_size, screen_w, screen_h);
    glUniform1i(gpu->uniforms.interlace, 1);
    glUniform1i(gpu->uniforms.stereo, effect->stereo);
    band_height = screen_h - band_y < GALLERY_BAKE_BAND_HEIGHT ?
                  screen_h - band_y : GALLERY_BAKE_BAND_HEIGHT;
    glScissor(0, screen_h - band_y - band_height, screen_w, band_height);
    draw_vertices();
    glFlush();
    glDisable(GL_SCISSOR_TEST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           gpu->cache_texture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}

int depth_gpu_gallery_bake_patch(DepthGpu *gpu, Display *display, const DepthEffect *effect,
                                 const DepthLayer *layers, const uint8_t *const pixels[2],
                                 int cell, int slot,
                                 int src_x, int src_y, int src_width, int src_height,
                                 int patch_x, int patch_y, int patch_width, int patch_height,
                                 int band)
{
    const int screen_w = display->mode.hdisplay;
    const int screen_h = display->mode.vdisplay;
    int direct_upload;
    int band_y = band * GALLERY_BAKE_BAND_HEIGHT;
    int band_height;
    int allocated = 0;
    GalleryPatch *patch;
    if (gpu->failed || cell < 0 || cell >= 9 || slot < 0 || slot > 1 ||
        src_x < 0 || src_y < 0 || src_width <= 0 || src_height <= 0 ||
        src_x + src_width > screen_w || src_y + src_height > screen_h ||
        patch_x < 0 || patch_y < 0 || patch_width <= 0 || patch_height <= 0 ||
        patch_x + patch_width > screen_w || patch_y + patch_height > screen_h ||
        band < 0 || band_y >= patch_height ||
        (!depth_gpu_direct_upload(gpu) && band == 0 && !pixels) ||
        !eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context) ||
        gallery_cache_init(gpu, display) != 0)
        return -1;
    patch = &gpu->gallery_patches[cell][slot];
    if (band == 0) {
        if (gallery_upload(gpu, display, layers, pixels,
                           src_x, src_y, src_width, src_height) != 0)
            return -1;
        if (!patch->texture)
            glGenTextures(1, &patch->texture);
        if (patch->width != patch_width || patch->height != patch_height) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, patch->texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, patch_width, patch_height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            patch->width = patch_width;
            patch->height = patch_height;
            allocated = 1;
        }
        patch->x = patch_x;
        patch->y = patch_y;
    }
    if (!patch->texture)
        return -1;
    direct_upload = depth_gpu_direct_upload(gpu);
    set_layer_filter(gpu, GL_NEAREST);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gpu->textures[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gpu->textures[1]);
    glBindFramebuffer(GL_FRAMEBUFFER, gpu->cache_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           patch->texture, 0);
    if (allocated && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               gpu->cache_texture, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return -1;
    }
    glViewport(0, 0, patch_width, patch_height);
    glEnable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    set_uniforms(gpu, display, effect, layers, 0, 0);
    glUniform2f(gpu->uniforms.frag_offset,
                (float)patch_x, (float)(screen_h - (patch_y + patch_height)));
    glUniform2f(gpu->uniforms.layer_origin,
                direct_upload ? 0.0f : (float)src_x,
                direct_upload ? 0.0f : (float)src_y);
    glUniform2f(gpu->uniforms.layer_size,
                direct_upload ? (float)screen_w : (float)src_width,
                direct_upload ? (float)screen_h : (float)src_height);
    glUniform1i(gpu->uniforms.interlace, 1);
    glUniform1i(gpu->uniforms.stereo, effect->stereo);
    band_height = patch_height - band_y < GALLERY_BAKE_BAND_HEIGHT ?
                  patch_height - band_y : GALLERY_BAKE_BAND_HEIGHT;
    glScissor(0, patch_height - band_y - band_height, patch_width, band_height);
    draw_vertices();
    glFlush();
    glDisable(GL_SCISSOR_TEST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           gpu->cache_texture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}

int depth_gpu_gallery_compose(DepthGpu *gpu, Display *display, int selected_cell, int slot)
{
    const int screen_w = display->mode.hdisplay;
    const int screen_h = display->mode.vdisplay;
    const GalleryPatch *patch;
    int gl_y;
    if (gpu->failed || selected_cell < 0 || selected_cell >= 9 || slot < 0 || slot > 1 ||
        !gpu->gallery_base_texture ||
        !gpu->gallery_patches[selected_cell][slot].texture ||
        !eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context))
        return -1;
    patch = &gpu->gallery_patches[selected_cell][slot];
    gl_y = screen_h - (patch->y + patch->height);
    glBindFramebuffer(GL_FRAMEBUFFER, gpu->cache_framebuffer);
    glDisable(GL_BLEND);
    glUseProgram(gpu->blit_program);
    glActiveTexture(GL_TEXTURE0);
    glViewport(0, 0, screen_w, screen_h);
    glDisable(GL_SCISSOR_TEST);
    glBindTexture(GL_TEXTURE_2D, gpu->gallery_base_texture);
    draw_vertices();
    glViewport(patch->x, gl_y, patch->width, patch->height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(patch->x, gl_y, patch->width, patch->height);
    glBindTexture(GL_TEXTURE_2D, patch->texture);
    draw_vertices();
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (glGetError() != GL_NO_ERROR)
        return -1;
    mark_slots_dirty_full(gpu);
    advance_cache_revision(gpu);
    return 0;
}

static void draw_quad_rect(int dx, int dy, int dw, int dh, int screen_w, int screen_h)
{
    float x0 = 2.0f * (float)dx / (float)screen_w - 1.0f;
    float x1 = 2.0f * (float)(dx + dw) / (float)screen_w - 1.0f;
    float y0 = 2.0f * (float)dy / (float)screen_h - 1.0f;
    float y1 = 2.0f * (float)(dy + dh) / (float)screen_h - 1.0f;
    float vertices[8];
    vertices[0] = x0;
    vertices[1] = y0;
    vertices[2] = x1;
    vertices[3] = y0;
    vertices[4] = x0;
    vertices[5] = y1;
    vertices[6] = x1;
    vertices[7] = y1;
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static int present_partial(DepthGpu *gpu, Display *display)
{
    const int screen_w = display->mode.hdisplay;
    const int screen_h = display->mode.vdisplay;
    int slot_idx;
    ScanoutSlot *entry;
    int dx = 0;
    int dy = 0;
    int dw = 0;
    int dh = 0;
    if (display_wait_for_flip(display) != 0 ||
        !eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context))
        return -1;
    slot_idx = (gpu->front_slot == 0) ? 1 : 0;
    entry = &gpu->slots[slot_idx];
    if (entry->dirty_full) {
        dw = screen_w;
        dh = screen_h;
    } else if (entry->dirty_x1 > entry->dirty_x0 && entry->dirty_y1 > entry->dirty_y0) {
        dx = entry->dirty_x0;
        dy = entry->dirty_y0;
        dw = entry->dirty_x1 - entry->dirty_x0;
        dh = entry->dirty_y1 - entry->dirty_y0;
    }
    if (dw > 0 && dh > 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, entry->fbo);
        glViewport(0, 0, screen_w, screen_h);
        glEnable(GL_SCISSOR_TEST);
        glScissor(dx, dy, dw, dh);
        glDisable(GL_BLEND);
        glUseProgram(gpu->blit_flip_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gpu->cache_texture);
        draw_quad_rect(dx, dy, dw, dh, screen_w, screen_h);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glFinish();
        if (glGetError() != GL_NO_ERROR)
            return -1;
        entry->dirty_full = 0;
        entry->dirty_x0 = 0;
        entry->dirty_y0 = 0;
        entry->dirty_x1 = 0;
        entry->dirty_y1 = 0;
    }
    if (!display->active_framebuffer ||
        display->active_framebuffer != gpu->current_framebuffer) {
        if (drmModeSetCrtc(display->fd, display->crtc_id, entry->fb->id, 0, 0,
                           &display->connector_id, 1, &display->mode) != 0) {
            fprintf(stderr, "Depth mode set failed: %s\n", strerror(errno));
            return -1;
        }
        gpu->front_slot = slot_idx;
        gpu->pending_slot = -1;
    } else {
        if (drmModePageFlip(display->fd, display->crtc_id, entry->fb->id,
                            DRM_MODE_PAGE_FLIP_EVENT, display) != 0) {
            fprintf(stderr, "Depth page flip failed: %s\n", strerror(errno));
            return -1;
        }
        gpu->pending_slot = slot_idx;
        display->flip_pending = 1;
    }
    display->active_framebuffer = entry->fb->id;
    gpu->current_framebuffer = entry->fb->id;
    return 0;
}

int depth_gpu_gallery_present(DepthGpu *gpu, Display *display)
{
    if (gpu->failed || !gpu->cache_texture)
        return -1;
    if (gpu->partial_scanout)
        return present_partial(gpu, display);
    if (display_wait_for_flip(display) != 0 ||
        !eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context))
        return -1;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, display->mode.hdisplay, display->mode.vdisplay);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(gpu->blit_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gpu->cache_texture);
    draw_vertices();
    return present(gpu, display);
}
