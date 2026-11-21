#include "player.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/video.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <string.h>

static gboolean propose_allocation(GstAppSink *sink, GstQuery *query, gpointer user_data)
{
    (void)sink;
    (void)user_data;
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
    return TRUE;
}

static int gstreamer_start_state(Decoder *decoder, GstState initial_state,
                                 char *error, size_t error_size)
{
    GstElement *pipeline;
    GstElement *src;
    GstElement *sink;
    GstAppSinkCallbacks callbacks;
    GError *gst_error = NULL;
    char *description = g_strdup_printf(
        "filesrc name=src ! qtdemux name=demux "
        "demux. ! queue ! h264parse ! v4l2slh264dec ! "
        "video/x-raw(memory:DMABuf),format=DMA_DRM ! "
        "appsink name=sink sync=true emit-signals=false max-buffers=1 drop=true %s",
        decoder->has_audio ?
            "demux. ! queue ! decodebin ! audioconvert ! audioresample ! "
            "audio/x-raw,format=S32LE,rate=48000,channels=2 ! pulsesink" : "");

    gst_init(NULL, NULL);
    pipeline = gst_parse_launch(description, &gst_error);
    g_free(description);
    if (gst_error || !pipeline) {
        set_error(error, error_size, "Failed to create GStreamer pipeline: %s",
                  gst_error ? gst_error->message : "Unknown error");
        if (pipeline)
            gst_object_unref(pipeline);
        if (gst_error)
            g_error_free(gst_error);
        return -1;
    }
    src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (!src || !sink) {
        set_error(error, error_size, "GStreamer pipeline is incomplete");
        if (src)
            gst_object_unref(src);
        if (sink)
            gst_object_unref(sink);
        gst_object_unref(pipeline);
        return -1;
    }
    g_object_set(src, "location", decoder->path, NULL);
    gst_object_unref(src);
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.propose_allocation = propose_allocation;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, NULL, NULL);
    decoder->gst_pipeline = pipeline;
    decoder->gst_sink = sink;
    if (gst_element_set_state(pipeline, initial_state) == GST_STATE_CHANGE_FAILURE) {
        set_error(error, error_size, "Failed to start GStreamer decoding");
        gstreamer_stop(decoder);
        return -1;
    }
    return 0;
}

int gstreamer_start(Decoder *decoder, char *error, size_t error_size)
{
    return gstreamer_start_state(decoder, GST_STATE_PLAYING, error, error_size);
}

int gstreamer_start_paused(Decoder *decoder, char *error, size_t error_size)
{
    return gstreamer_start_state(decoder, GST_STATE_PAUSED, error, error_size);
}

enum {
    SOFTWARE_AUTOPLUG_TRY = 0,
    SOFTWARE_AUTOPLUG_SKIP = 2
};

static int software_autoplug_select(GstElement *bin, GstPad *pad,
                                    GstCaps *caps, GstElementFactory *factory,
                                    gpointer user_data)
{
    const char *klass;
    (void)bin;
    (void)pad;
    (void)caps;
    (void)user_data;
    if (!gst_element_factory_list_is_type(factory, GST_ELEMENT_FACTORY_TYPE_DECODER))
        return SOFTWARE_AUTOPLUG_TRY;
    klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
    if (klass && strstr(klass, "Hardware"))
        return SOFTWARE_AUTOPLUG_SKIP;
    return SOFTWARE_AUTOPLUG_TRY;
}

static void software_element_added(GstBin *bin, GstElement *element, gpointer user_data)
{
    GstElementFactory *factory = gst_element_get_factory(element);
    const char *name;
    int threads = GPOINTER_TO_INT(user_data);
    (void)bin;
    if (!factory || threads <= 0)
        return;
    name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    if (name && strncmp(name, "avdec_", 6) == 0 &&
        g_object_class_find_property(G_OBJECT_GET_CLASS(element), "max-threads"))
        g_object_set(element, "max-threads", threads, NULL);
}

int gstreamer_start_software(Decoder *decoder, char *error, size_t error_size)
{
    GstElement *pipeline;
    GstElement *src;
    GstElement *decode;
    GstElement *sink;
    GError *gst_error = NULL;
    char *description = g_strdup_printf(
        "filesrc name=src ! decodebin name=decode ! video/x-raw ! "
        "queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! "
        "videoconvert ! video/x-raw,format=%s ! "
        "appsink name=sink sync=false emit-signals=false max-buffers=1 drop=false",
        (decoder->movie || decoder->yuv420) ? "I420" : "RGB");

    gst_init(NULL, NULL);
    pipeline = gst_parse_launch(description, &gst_error);
    g_free(description);
    if (gst_error || !pipeline) {
        set_error(error, error_size, "Failed to create software GStreamer pipeline: %s",
                  gst_error ? gst_error->message : "Unknown error");
        if (pipeline)
            gst_object_unref(pipeline);
        if (gst_error)
            g_error_free(gst_error);
        return -1;
    }
    src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    decode = gst_bin_get_by_name(GST_BIN(pipeline), "decode");
    sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (!src || !decode || !sink) {
        set_error(error, error_size, "Software GStreamer pipeline is incomplete");
        if (src)
            gst_object_unref(src);
        if (decode)
            gst_object_unref(decode);
        if (sink)
            gst_object_unref(sink);
        gst_object_unref(pipeline);
        return -1;
    }
    g_object_set(src, "location", decoder->path, NULL);
    gst_object_unref(src);
    g_signal_connect(decode, "autoplug-select", G_CALLBACK(software_autoplug_select), NULL);
    g_signal_connect(decode, "element-added", G_CALLBACK(software_element_added),
                     GINT_TO_POINTER(decoder->decode_threads));
    decoder->gst_pipeline = pipeline;
    decoder->gst_sink = sink;
    gst_object_unref(decode);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        set_error(error, error_size, "Failed to start software GStreamer decoding");
        gstreamer_stop(decoder);
        return -1;
    }
    return 0;
}

static int gstreamer_bus_error(GstBus *bus, char *error, size_t error_size)
{
    GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    GError *gst_error = NULL;
    if (!message)
        return 0;
    gst_message_parse_error(message, &gst_error, NULL);
    set_error(error, error_size, "%s", gst_error ? gst_error->message : "Unknown GStreamer error");
    if (gst_error)
        g_error_free(gst_error);
    gst_message_unref(message);
    return -1;
}

int gstreamer_read(Decoder *decoder, char *error, size_t error_size)
{
    GstSample *sample = NULL;
    GstBuffer *buffer;
    GstVideoMeta *meta;
    GstBus *bus;
    GstClockTime deadline;
    int result = -1;
    int i;

    bus = gst_element_get_bus(decoder->gst_pipeline);
    if (!bus) {
        set_error(error, error_size, "GStreamer pipeline has no message bus");
        return -1;
    }
    deadline = gst_util_get_timestamp() + 5 * GST_SECOND;
    for (;;) {
        if (gstreamer_bus_error(bus, error, error_size) != 0)
            goto out;
        sample = gst_app_sink_try_pull_sample(decoder->gst_sink, 100 * GST_MSECOND);
        if (sample)
            break;
        if (gst_app_sink_is_eos(decoder->gst_sink)) {
            result = 0;
            goto out;
        }
        if (gst_util_get_timestamp() >= deadline) {
            set_error(error, error_size, "Timed out waiting for a decoded frame");
            goto out;
        }
    }
    buffer = gst_sample_get_buffer(sample);
    if (!buffer || gst_buffer_n_memory(buffer) < 1) {
        set_error(error, error_size, "GStreamer output contains no video frame");
        gst_sample_unref(sample);
        goto out;
    }
    meta = gst_buffer_get_video_meta(buffer);
    for (i = 0; i < 2; ++i) {
        guint index = gst_buffer_n_memory(buffer) == 1 ? 0 : i;
        GstMemory *memory = gst_buffer_peek_memory(buffer, index);
        if (!gst_is_dmabuf_memory(memory)) {
            set_error(error, error_size, "GStreamer output is not DMA-BUF");
            gst_sample_unref(sample);
            goto out;
        }
        decoder->dma_fds[i] = gst_dmabuf_memory_get_fd(memory);
        decoder->dma_offsets[i] = index == 0 && meta ? meta->offset[i] : memory->offset;
        decoder->dma_strides[i] = meta ? meta->stride[i] : decoder->nv12_width;
    }
    if (decoder->keep_previous_sample) {
        if (decoder->gst_previous_sample)
            gst_sample_unref(decoder->gst_previous_sample);
        decoder->gst_previous_sample = decoder->gst_sample;
    } else {
        if (decoder->gst_previous_sample) {
            gst_sample_unref(decoder->gst_previous_sample);
            decoder->gst_previous_sample = NULL;
        }
        if (decoder->gst_sample)
            gst_sample_unref(decoder->gst_sample);
    }
    decoder->gst_sample = sample;
    result = 1;
out:
    gst_object_unref(bus);
    return result;
}

int gstreamer_read_software(Decoder *decoder, uint8_t *frame, char *error, size_t error_size)
{
    GstSample *sample = NULL;
    GstBuffer *buffer;
    GstCaps *caps;
    GstVideoInfo info;
    GstVideoFrame video_frame;
    GstBus *bus;
    GstClockTime deadline;
    size_t row_size = (size_t)decoder->width * 3;
    int mapped = 0;
    int result = -1;
    int row;

    if (!frame) {
        set_error(error, error_size, "Software decoding requires a frame buffer");
        return -1;
    }
    bus = gst_element_get_bus(decoder->gst_pipeline);
    if (!bus) {
        set_error(error, error_size, "Software GStreamer pipeline has no message bus");
        return -1;
    }
    deadline = gst_util_get_timestamp() + 30 * GST_SECOND;
    for (;;) {
        if (gstreamer_bus_error(bus, error, error_size) != 0)
            goto out;
        sample = gst_app_sink_try_pull_sample(decoder->gst_sink, 100 * GST_MSECOND);
        if (sample)
            break;
        if (gst_app_sink_is_eos(decoder->gst_sink)) {
            result = 0;
            goto out;
        }
        if (gst_util_get_timestamp() >= deadline) {
            set_error(error, error_size, "Timed out waiting for a software decoded frame");
            goto out;
        }
    }
    buffer = gst_sample_get_buffer(sample);
    caps = gst_sample_get_caps(sample);
    if (!buffer || !caps || !gst_video_info_from_caps(&info, caps) ||
        GST_VIDEO_INFO_WIDTH(&info) != decoder->width ||
        GST_VIDEO_INFO_HEIGHT(&info) != decoder->height ||
        (decoder->yuv420 ?
            (GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_I420 ||
             decoder->read_size != (size_t)decoder->width * (size_t)decoder->height * 3 / 2) :
            ((GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_RGB &&
              GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_I420) ||
             decoder->read_size != row_size * decoder->height))) {
        set_error(error, error_size, "Unexpected software decoded video layout");
        goto out;
    }
    if (!gst_video_frame_map(&video_frame, &info, buffer, GST_MAP_READ)) {
        set_error(error, error_size, "Failed to map software decoded video frame");
        goto out;
    }
    mapped = 1;
    if (decoder->yuv420) {
        GstVideoColorimetry color = GST_VIDEO_INFO_COLORIMETRY(&info);
        uint8_t *dst = frame;
        int plane;
        if (color.matrix == GST_VIDEO_COLOR_MATRIX_BT709)
            decoder->yuv_bt709 = 1;
        else if (color.matrix == GST_VIDEO_COLOR_MATRIX_BT601)
            decoder->yuv_bt709 = 0;
        if (color.range == GST_VIDEO_COLOR_RANGE_0_255)
            decoder->yuv_full_range = 1;
        else if (color.range == GST_VIDEO_COLOR_RANGE_16_235)
            decoder->yuv_full_range = 0;
        for (plane = 0; plane < 3; ++plane) {
            int plane_h = plane == 0 ? decoder->height : decoder->height / 2;
            size_t plane_w = (size_t)(plane == 0 ? decoder->width : decoder->width / 2);
            const uint8_t *src = GST_VIDEO_FRAME_PLANE_DATA(&video_frame, plane);
            int src_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, plane);
            if (src_stride < 0 || (size_t)src_stride < plane_w) {
                set_error(error, error_size, "Unexpected software YUV420 plane stride");
                goto out;
            }
            if ((size_t)src_stride == plane_w) {
                memcpy(dst, src, plane_w * (size_t)plane_h);
                dst += plane_w * (size_t)plane_h;
            } else {
                for (row = 0; row < plane_h; ++row) {
                    memcpy(dst, src + (size_t)row * (size_t)src_stride, plane_w);
                    dst += plane_w;
                }
            }
        }
    } else if (GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_I420) {
        struct SwsContext *context = decoder->sws_context;
        const uint8_t *source[4] = {NULL};
        uint8_t *destination[4] = {frame};
        int source_strides[4] = {0};
        int destination_strides[4] = {(int)row_size};
        int source_range = GST_VIDEO_INFO_COLORIMETRY(&info).range == GST_VIDEO_COLOR_RANGE_0_255;
        int matrix = GST_VIDEO_INFO_COLORIMETRY(&info).matrix == GST_VIDEO_COLOR_MATRIX_BT709 ?
                     SWS_CS_ITU709 : SWS_CS_ITU601;
        const int *coefficients = sws_getCoefficients(matrix);
        int plane;
        if (!context) {
            context = sws_getContext(decoder->width, decoder->height,
                                     source_range ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P,
                                     decoder->width, decoder->height, AV_PIX_FMT_RGB24,
                                     SWS_FAST_BILINEAR, NULL, NULL, NULL);
            if (!context) {
                set_error(error, error_size, "Failed to initialize software color conversion");
                goto out;
            }
            sws_setColorspaceDetails(context, coefficients, source_range, coefficients, 1,
                                     0, 1 << 16, 1 << 16);
            decoder->sws_context = context;
        }
        for (plane = 0; plane < 3; ++plane) {
            source[plane] = GST_VIDEO_FRAME_PLANE_DATA(&video_frame, plane);
            source_strides[plane] = GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, plane);
        }
        if (sws_scale(context, source, source_strides, 0, decoder->height,
                      destination, destination_strides) != decoder->height) {
            set_error(error, error_size, "Failed to convert software decoded frame to RGB");
            goto out;
        }
    } else {
        for (row = 0; row < decoder->height; ++row)
            memcpy(frame + (size_t)row * row_size,
                   (const uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&video_frame, 0) +
                   (size_t)row * GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, 0), row_size);
    }
    result = 1;
out:
    if (mapped)
        gst_video_frame_unmap(&video_frame);
    if (sample)
        gst_sample_unref(sample);
    gst_object_unref(bus);
    return result;
}

int gstreamer_restart(Decoder *decoder)
{
    if (!decoder->gst_pipeline)
        return -1;
    return gst_element_seek_simple(decoder->gst_pipeline, GST_FORMAT_TIME,
                                   GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 0) ? 0 : -1;
}

int gstreamer_pause(Decoder *decoder, int paused)
{
    if (!decoder->gst_pipeline)
        return -1;
    return gst_element_set_state(decoder->gst_pipeline,
                                 paused ? GST_STATE_PAUSED : GST_STATE_PLAYING) ==
           GST_STATE_CHANGE_FAILURE ? -1 : 0;
}

void gstreamer_stop(Decoder *decoder)
{
    if (decoder->sws_context) {
        sws_freeContext(decoder->sws_context);
        decoder->sws_context = NULL;
    }
    if (decoder->gst_previous_sample) {
        gst_sample_unref(decoder->gst_previous_sample);
        decoder->gst_previous_sample = NULL;
    }
    if (decoder->gst_sample) {
        gst_sample_unref(decoder->gst_sample);
        decoder->gst_sample = NULL;
    }
    if (decoder->gst_pipeline)
        gst_element_set_state(decoder->gst_pipeline, GST_STATE_NULL);
    if (decoder->gst_sink)
        gst_object_unref(decoder->gst_sink);
    if (decoder->gst_pipeline)
        gst_object_unref(decoder->gst_pipeline);
    decoder->gst_sink = NULL;
    decoder->gst_pipeline = NULL;
    decoder->keep_previous_sample = 0;
}
