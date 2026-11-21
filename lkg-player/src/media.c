#define _GNU_SOURCE
#include "player.h"
#include "heic.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t wait_for_child(pid_t pid, int *status)
{
    pid_t result;
    do {
        result = waitpid(pid, status, 0);
    } while (result < 0 && errno == EINTR);
    return result;
}

static char *capture_command(char *const argv[], char *error, size_t error_size)
{
    int pipefd[2];
    pid_t pid;
    pid_t parent_pid = getpid();
    char *output = NULL;
    size_t used = 0;
    size_t capacity = 4096;
    int status;

    if (pipe(pipefd) != 0) {
        set_error(error, error_size, "Failed to create decoder pipe: %s", strerror(errno));
        return NULL;
    }
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    pid = fork();
    if (pid < 0) {
        set_error(error, error_size, "Failed to start ffprobe: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }
    if (pid == 0) {
        if (process_bind_parent(parent_pid) != 0)
            _exit(1);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    output = malloc(capacity);
    if (!output) {
        close(pipefd[0]);
        kill(pid, SIGTERM);
        wait_for_child(pid, NULL);
        set_error(error, error_size, "Out of memory");
        return NULL;
    }
    for (;;) {
        ssize_t count;
        if (used + 2048 + 1 > capacity) {
            char *larger;
            capacity *= 2;
            larger = realloc(output, capacity);
            if (!larger) {
                free(output);
                close(pipefd[0]);
                kill(pid, SIGTERM);
                wait_for_child(pid, NULL);
                set_error(error, error_size, "Out of memory");
                return NULL;
            }
            output = larger;
        }
        count = read(pipefd[0], output + used, 2048);
        if (count > 0)
            used += (size_t)count;
        else if (count < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    close(pipefd[0]);
    output[used] = '\0';
    if (wait_for_child(pid, &status) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        set_error(error, error_size, "ffprobe could not identify the media file (check that FFmpeg is installed)");
        free(output);
        return NULL;
    }
    return output;
}

static double parse_rate(const char *text)
{
    double numerator = 0;
    double denominator = 1;
    if (sscanf(text, "%lf/%lf", &numerator, &denominator) < 1 || denominator == 0)
        return 0;
    return numerator / denominator;
}

static int spatial_view_order(const char *ids, const char *positions,
                              int *left_index, int *right_index)
{
    int id[2];
    int position[2];
    char extra;
    if (sscanf(ids, "%d,%d%c", &id[0], &id[1], &extra) != 2 ||
        sscanf(positions, "%d,%d%c", &position[0], &position[1], &extra) != 2)
        return 0;
    if (position[0] == 1 && position[1] == 2) {
        *left_index = 0;
        *right_index = 1;
        return 1;
    }
    if (position[0] == 2 && position[1] == 1) {
        *left_index = 1;
        *right_index = 0;
        return 1;
    }
    return 0;
}

static uint16_t mp4_be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t mp4_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t mp4_be64(const uint8_t *p)
{
    return ((uint64_t)mp4_be32(p) << 32) | mp4_be32(p + 4);
}

static int mp4_tag_is(const uint8_t *p, const char tag[4])
{
    return memcmp(p, tag, 4) == 0;
}

static int mp4_next_box(const uint8_t *data, size_t end, size_t *pos,
                        const uint8_t **tag_out, size_t *payload_start,
                        size_t *payload_end)
{
    uint64_t size;
    size_t header = 8;
    if (*pos + 8 > end)
        return 0;
    size = mp4_be32(data + *pos);
    *tag_out = data + *pos + 4;
    if (size == 1) {
        if (*pos + 16 > end)
            return 0;
        size = mp4_be64(data + *pos + 8);
        header = 16;
    } else if (size == 0) {
        size = (uint64_t)(end - *pos);
    }
    if (size < header || size > (uint64_t)(end - *pos))
        return 0;
    *payload_start = *pos + header;
    *payload_end = *pos + (size_t)size;
    *pos = *payload_end;
    return 1;
}

typedef struct {
    int is_video;
    int is_audio;
    int audio_aac;
    int rotated_90;
    int unsupported;
    int hevc;
    int width;
    int height;
    uint32_t timescale;
    uint64_t duration;
    uint64_t sample_count;
    uint64_t stts_samples;
    uint64_t stts_duration;
} Mp4TrackInfo;

static void mp4_parse_stbl(const uint8_t *data, size_t start, size_t end, Mp4TrackInfo *track)
{
    size_t pos = start;
    const uint8_t *tag;
    size_t ps, pe;
    while (mp4_next_box(data, end, &pos, &tag, &ps, &pe)) {
        if (mp4_tag_is(tag, "stsd") && pe - ps >= 16) {
            uint32_t count = mp4_be32(data + ps + 4);
            size_t epos = ps + 8;
            const uint8_t *etag;
            size_t eps, epe;
            if (count >= 1 && mp4_next_box(data, pe, &epos, &etag, &eps, &epe)) {
                if (track->is_video) {
                    if (mp4_tag_is(etag, "avc1") || mp4_tag_is(etag, "avc3")) {
                        track->hevc = 0;
                    } else if (mp4_tag_is(etag, "hvc1") || mp4_tag_is(etag, "hev1")) {
                        track->hevc = 1;
                    } else {
                        track->unsupported = 1;
                        return;
                    }
                    if (epe - eps < 78) {
                        track->unsupported = 1;
                        return;
                    }
                    track->width = mp4_be16(data + eps + 24);
                    track->height = mp4_be16(data + eps + 26);
                    /* Fall back to ffprobe for MV-HEVC / Apple Spatial Video */
                    {
                        size_t cpos = eps + 78;
                        const uint8_t *ctag;
                        size_t cps, cpe;
                        while (mp4_next_box(data, epe, &cpos, &ctag, &cps, &cpe)) {
                            if (mp4_tag_is(ctag, "lhvC") || mp4_tag_is(ctag, "vexu") ||
                                mp4_tag_is(ctag, "stri") || mp4_tag_is(ctag, "hero")) {
                                track->unsupported = 1;
                                return;
                            }
                        }
                    }
                } else if (track->is_audio) {
                    if (mp4_tag_is(etag, "mp4a"))
                        track->audio_aac = 1;
                }
            }
        } else if (mp4_tag_is(tag, "stsz") && pe - ps >= 12) {
            track->sample_count = mp4_be32(data + ps + 8);
        } else if (mp4_tag_is(tag, "stts") && pe - ps >= 8) {
            uint32_t entries = mp4_be32(data + ps + 4);
            uint32_t i;
            size_t off = ps + 8;
            for (i = 0; i < entries && off + 8 <= pe; ++i, off += 8) {
                uint64_t cnt = mp4_be32(data + off);
                uint64_t delta = mp4_be32(data + off + 4);
                track->stts_samples += cnt;
                track->stts_duration += cnt * delta;
            }
        }
    }
}

static void mp4_parse_mdia(const uint8_t *data, size_t start, size_t end, Mp4TrackInfo *track)
{
    size_t pos = start;
    const uint8_t *tag;
    size_t ps, pe;
    size_t minf_start = 0, minf_end = 0;
    while (mp4_next_box(data, end, &pos, &tag, &ps, &pe)) {
        if (mp4_tag_is(tag, "mdhd") && pe - ps >= 20) {
            uint8_t version = data[ps];
            if (version == 0 && pe - ps >= 20) {
                track->timescale = mp4_be32(data + ps + 12);
                track->duration = mp4_be32(data + ps + 16);
            } else if (version == 1 && pe - ps >= 32) {
                track->timescale = mp4_be32(data + ps + 20);
                track->duration = mp4_be64(data + ps + 24);
            }
        } else if (mp4_tag_is(tag, "hdlr") && pe - ps >= 12) {
            if (mp4_tag_is(data + ps + 8, "vide"))
                track->is_video = 1;
            else if (mp4_tag_is(data + ps + 8, "soun"))
                track->is_audio = 1;
        } else if (mp4_tag_is(tag, "minf")) {
            minf_start = ps;
            minf_end = pe;
        }
    }
    if ((track->is_video || track->is_audio) && minf_end > minf_start) {
        size_t mpos = minf_start;
        while (mp4_next_box(data, minf_end, &mpos, &tag, &ps, &pe)) {
            if (mp4_tag_is(tag, "stbl")) {
                mp4_parse_stbl(data, ps, pe, track);
                break;
            }
        }
    }
}

static void mp4_parse_tkhd(const uint8_t *data, size_t start, size_t end, Mp4TrackInfo *track)
{
    uint8_t version;
    size_t matrix_offset;
    const uint8_t *m;
    int32_t a, b, c, d;
    int64_t aa, ab, ac, ad;
    if (end - start < 1)
        return;
    version = data[start];
    if (version == 0)
        matrix_offset = 40;
    else if (version == 1)
        matrix_offset = 52;
    else
        return;
    if (end - start < matrix_offset + 36)
        return;
    m = data + start + matrix_offset;
    a = (int32_t)mp4_be32(m + 0);
    b = (int32_t)mp4_be32(m + 4);
    c = (int32_t)mp4_be32(m + 12);
    d = (int32_t)mp4_be32(m + 16);
    aa = a < 0 ? -(int64_t)a : (int64_t)a;
    ab = b < 0 ? -(int64_t)b : (int64_t)b;
    ac = c < 0 ? -(int64_t)c : (int64_t)c;
    ad = d < 0 ? -(int64_t)d : (int64_t)d;
    if (aa < ab && ad < ac)
        track->rotated_90 = 1;
}

static int probe_mp4_mov(Decoder *decoder, const char *path)
{
    FILE *file = fopen(path, "rb");
    struct stat st;
    uint64_t file_size;
    uint64_t offset = 0;
    uint8_t *moov = NULL;
    size_t moov_size = 0;
    uint32_t movie_timescale = 0;
    uint64_t movie_duration = 0;
    Mp4TrackInfo video;
    int have_video = 0;
    int has_audio = 0;
    int audio_aac = 0;
    double fps = 0;
    int probed = 0;

    if (!file)
        return 0;
    if (fstat(fileno(file), &st) != 0 || st.st_size < 16)
        goto out;
    file_size = (uint64_t)st.st_size;
    while (offset + 8 <= file_size) {
        uint8_t header[16];
        uint64_t box_size;
        size_t header_size = 8;
        if (fseeko(file, (off_t)offset, SEEK_SET) != 0 ||
            fread(header, 1, 8, file) != 8)
            goto out;
        if (offset == 0 &&
            !mp4_tag_is(header + 4, "ftyp") && !mp4_tag_is(header + 4, "wide") &&
            !mp4_tag_is(header + 4, "free") && !mp4_tag_is(header + 4, "mdat") &&
            !mp4_tag_is(header + 4, "moov") && !mp4_tag_is(header + 4, "skip"))
            goto out;
        box_size = mp4_be32(header);
        if (box_size == 1) {
            if (offset + 16 > file_size || fread(header + 8, 1, 8, file) != 8)
                goto out;
            box_size = mp4_be64(header + 8);
            header_size = 16;
        } else if (box_size == 0) {
            box_size = file_size - offset;
        }
        if (box_size < header_size || box_size > file_size - offset)
            goto out;
        if (mp4_tag_is(header + 4, "moov")) {
            uint64_t payload = box_size - header_size;
            if (payload == 0 || payload > 16ULL * 1024 * 1024)
                goto out;
            moov = malloc((size_t)payload);
            if (!moov)
                goto out;
            if (fread(moov, 1, (size_t)payload, file) != (size_t)payload)
                goto out;
            moov_size = (size_t)payload;
            break;
        }
        offset += box_size;
    }
    if (!moov)
        goto out;

    memset(&video, 0, sizeof(video));
    {
        size_t pos = 0;
        const uint8_t *tag;
        size_t ps, pe;
        while (mp4_next_box(moov, moov_size, &pos, &tag, &ps, &pe)) {
            if (mp4_tag_is(tag, "mvhd") && pe - ps >= 20) {
                uint8_t version = moov[ps];
                if (version == 0 && pe - ps >= 20) {
                    movie_timescale = mp4_be32(moov + ps + 12);
                    movie_duration = mp4_be32(moov + ps + 16);
                } else if (version == 1 && pe - ps >= 32) {
                    movie_timescale = mp4_be32(moov + ps + 20);
                    movie_duration = mp4_be64(moov + ps + 24);
                }
            } else if (mp4_tag_is(tag, "trak")) {
                size_t tpos = ps;
                const uint8_t *ttag;
                size_t tps, tpe;
                Mp4TrackInfo track;
                memset(&track, 0, sizeof(track));
                while (mp4_next_box(moov, pe, &tpos, &ttag, &tps, &tpe)) {
                    if (mp4_tag_is(ttag, "tkhd")) {
                        mp4_parse_tkhd(moov, tps, tpe, &track);
                    } else if (mp4_tag_is(ttag, "mdia")) {
                        mp4_parse_mdia(moov, tps, tpe, &track);
                    }
                }
                if (track.unsupported)
                    goto out;
                if (track.is_audio && !has_audio) {
                    has_audio = 1;
                    audio_aac = track.audio_aac;
                }
                if (track.is_video && !have_video) {
                    video = track;
                    have_video = 1;
                }
            }
        }
    }
    if (!have_video || video.width <= 0 || video.height <= 0)
        goto out;
    if ((size_t)video.width > SIZE_MAX / ((size_t)video.height * 3))
        goto out;
    if (video.timescale > 0 && video.stts_duration > 0 && video.stts_samples > 0)
        fps = (double)video.stts_samples * (double)video.timescale / (double)video.stts_duration;
    else if (video.timescale > 0 && video.duration > 0 && video.sample_count > 0)
        fps = (double)video.sample_count * (double)video.timescale / (double)video.duration;
    else if (movie_timescale > 0 && movie_duration > 0 && video.sample_count > 0)
        fps = (double)video.sample_count * (double)movie_timescale / (double)movie_duration;
    if (fps <= 0 || fps >= 1000)
        goto out;

    memset(decoder, 0, sizeof(*decoder));
    decoder->fd = -1;
    decoder->width = video.width;
    decoder->height = video.height;
    decoder->fps = fps;
    decoder->total_frames = video.sample_count ? video.sample_count : video.stts_samples;
    if (!decoder->total_frames && movie_timescale > 0 && movie_duration > 0)
        decoder->total_frames = (uint64_t)((double)movie_duration / movie_timescale * fps + 0.5);
    decoder->has_audio = has_audio;
    decoder->audio_aac = audio_aac;
    decoder->rotated_90 = video.rotated_90;
    decoder->hevc = video.hevc;
    decoder->yuv_bt709 = decoder->height >= 720;
    decoder->read_size = (size_t)decoder->width * (size_t)decoder->height * 3;
    decoder->frame_size = decoder->read_size;
    decoder->path = strdup(path);
    if (!decoder->path)
        goto out;
    probed = 1;

out:
    free(moov);
    fclose(file);
    return probed;
}

int decoder_probe(Decoder *decoder, const char *path, char *error, size_t error_size)
{
    char *argv[] = {
        "ffprobe", "-v", "error",
        "-show_entries", "stream=index,codec_type,codec_name,width,height,avg_frame_rate,nb_frames,color_space,color_range,view_ids_available,view_pos_available:stream_side_data=horizontal_disparity_adjustment,rotation:format=duration",
        "-of", "default=noprint_wrappers=1", (char *)path, NULL
    };
    char *output;
    char *line;
    char *save = NULL;
    char view_ids[64] = {0};
    char view_positions[64] = {0};
    char stream_codec[32] = {0};
    double duration = 0;
    int video_stream = 0;
    int audio_stream = 0;
    int audio_count = 0;
    int color_space_set = 0;

    if (probe_mp4_mov(decoder, path))
        return 0;

    memset(decoder, 0, sizeof(*decoder));
    decoder->fd = -1;
    decoder->fps = 30;
    output = capture_command(argv, error, error_size);
    if (!output)
        return -1;
    for (line = strtok_r(output, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (strncmp(line, "index=", 6) == 0) {
            video_stream = 0;
            audio_stream = 0;
            stream_codec[0] = '\0';
        } else if (strncmp(line, "codec_type=", 11) == 0) {
            video_stream = strcmp(line + 11, "video") == 0;
            audio_stream = strcmp(line + 11, "audio") == 0;
            if (audio_stream) {
                ++audio_count;
                decoder->has_audio = 1;
                if (audio_count == 1 && strcmp(stream_codec, "aac") == 0)
                    decoder->audio_aac = 1;
            }
        } else if (strncmp(line, "codec_name=", 11) == 0) {
            snprintf(stream_codec, sizeof(stream_codec), "%s", line + 11);
            if (strcmp(stream_codec, "hevc") == 0 || strcmp(stream_codec, "h265") == 0)
                decoder->hevc = 1;
            if (audio_stream && audio_count == 1 && strcmp(stream_codec, "aac") == 0)
                decoder->audio_aac = 1;
        } else if (video_stream && strncmp(line, "width=", 6) == 0)
            decoder->width = atoi(line + 6);
        else if (video_stream && strncmp(line, "height=", 7) == 0)
            decoder->height = atoi(line + 7);
        else if (video_stream && strncmp(line, "avg_frame_rate=", 15) == 0) {
            double fps = parse_rate(line + 15);
            if (fps > 0 && fps < 1000)
                decoder->fps = fps;
        } else if (video_stream && strncmp(line, "nb_frames=", 10) == 0)
            decoder->total_frames = strtoull(line + 10, NULL, 10);
        else if (video_stream && strncmp(line, "color_space=", 12) == 0) {
            if (strcmp(line + 12, "bt709") == 0) {
                decoder->yuv_bt709 = 1;
                color_space_set = 1;
            } else if (strcmp(line + 12, "bt470bg") == 0 ||
                       strcmp(line + 12, "smpte170m") == 0) {
                decoder->yuv_bt709 = 0;
                color_space_set = 1;
            }
        } else if (video_stream && strncmp(line, "color_range=", 12) == 0) {
            if (strcmp(line + 12, "pc") == 0)
                decoder->yuv_full_range = 1;
            else if (strcmp(line + 12, "tv") == 0)
                decoder->yuv_full_range = 0;
        } else if (video_stream && strncmp(line, "view_ids_available=", 19) == 0)
            snprintf(view_ids, sizeof(view_ids), "%s", line + 19);
        else if (video_stream && strncmp(line, "view_pos_available=", 19) == 0)
            snprintf(view_positions, sizeof(view_positions), "%s", line + 19);
        else if (video_stream && strncmp(line, "horizontal_disparity_adjustment=", 32) == 0) {
            double disparity = parse_rate(line + 32);
            decoder->spatial_disparity = disparity >= -1.0 && disparity <= 1.0 ? disparity : 0;
        } else if (video_stream && strncmp(line, "rotation=", 9) == 0) {
            int degrees = atoi(line + 9) % 360;
            if (degrees < 0)
                degrees += 360;
            if (degrees == 90 || degrees == 270)
                decoder->rotated_90 = 1;
        } else if (strncmp(line, "duration=", 9) == 0)
            duration = atof(line + 9);
    }
    if (!color_space_set)
        decoder->yuv_bt709 = decoder->height >= 720;
    if (!decoder->total_frames && duration > 0)
        decoder->total_frames = (uint64_t)(duration * decoder->fps + 0.5);
    free(output);
    if (decoder->width <= 0 || decoder->height <= 0) {
        set_error(error, error_size, "Media has no valid video dimensions");
        return -1;
    }
    if ((size_t)decoder->width > SIZE_MAX / ((size_t)decoder->height * 3)) {
        set_error(error, error_size, "Media dimensions are too large");
        return -1;
    }
    decoder->stereo = spatial_view_order(view_ids, view_positions,
                                         &decoder->stereo_left_index,
                                         &decoder->stereo_right_index);
    decoder->read_size = (size_t)decoder->width * (size_t)decoder->height * 3;
    if (decoder->stereo && decoder->read_size > SIZE_MAX / 2) {
        set_error(error, error_size, "Stereo media dimensions are too large");
        return -1;
    }
    decoder->frame_size = decoder->read_size * (decoder->stereo ? 2 : 1);
    decoder->path = strdup(path);
    if (!decoder->path) {
        set_error(error, error_size, "Out of memory");
        return -1;
    }
    return 0;
}

void decoder_discard_cached(const char *path)
{
    char *metadata_path = malloc(strlen(path) + 6);
    unlink(path);
    if (metadata_path) {
        sprintf(metadata_path, "%s.meta", path);
        unlink(metadata_path);
        free(metadata_path);
    }
}

int decoder_probe_cached(Decoder *decoder, const char *path, char *error, size_t error_size)
{
    char *metadata_path = malloc(strlen(path) + 6);
    FILE *file;
    char line[256];
    int width = 0;
    int height = 0;
    double fps = 0;
    unsigned long long total_frames = 0;
    int has_audio = -1;
    int fields = 0;
    int valid = 0;
    if (!metadata_path) {
        set_error(error, error_size, "Out of memory");
        return -1;
    }
    sprintf(metadata_path, "%s.meta", path);
    file = fopen(metadata_path, "r");
    free(metadata_path);
    if (file) {
        if (fgets(line, sizeof(line), file))
            fields = sscanf(line, "%d %d %lf %llu %d", &width, &height, &fps,
                            &total_frames, &has_audio);
        valid = fields >= 4 && width > 0 && height > 0 && fps > 0 && fps < 1000;
        fclose(file);
    }
    if (!valid) {
        int result = decoder_probe(decoder, path, error, error_size);
        if (result != 0)
            decoder_discard_cached(path);
        return result;
    }
    if (has_audio < 0)
        return decoder_probe(decoder, path, error, error_size);
    memset(decoder, 0, sizeof(*decoder));
    decoder->fd = -1;
    decoder->width = width;
    decoder->height = height;
    decoder->fps = fps;
    decoder->total_frames = total_frames;
    decoder->has_audio = has_audio;
    decoder->frame_size = (size_t)width * (size_t)height * 3;
    decoder->read_size = decoder->frame_size;
    decoder->path = strdup(path);
    if (!decoder->path) {
        set_error(error, error_size, "Out of memory");
        return -1;
    }
    return 0;
}

void decoder_configure_nv12(Decoder *decoder)
{
    decoder->bgra = 1;
    decoder->hardware = 1;
    decoder->nv12_width = decoder->width;
    decoder->nv12_height = decoder->height;
    decoder->read_size = (size_t)decoder->width * decoder->height * 3 / 2;
    decoder->frame_size = decoder->read_size;
}

int decoder_can_yuv420(const Decoder *decoder)
{
    return decoder && decoder->width >= 2 && decoder->height >= 2 &&
           (decoder->width % 2) == 0 && (decoder->height % 2) == 0 &&
           decoder->width <= 16384 && decoder->height <= 16384;
}

void decoder_configure_yuv420(Decoder *decoder)
{
    decoder->yuv420 = 1;
    decoder->read_size = (size_t)decoder->width * (size_t)decoder->height * 3 / 2;
    decoder->frame_size = decoder->read_size * (decoder->stereo ? 2 : 1);
}

void decoder_write_cached_metadata(const char *path, int width, int height,
                                   double fps, uint64_t total_frames, int has_audio)
{
    char *metadata_path = malloc(strlen(path) + 6);
    FILE *file;
    if (!metadata_path)
        return;
    sprintf(metadata_path, "%s.meta", path);
    file = fopen(metadata_path, "w");
    if (file) {
        int failed = fprintf(file, "%d %d %.9f %llu %d\n", width, height, fps,
                             (unsigned long long)total_frames, has_audio) < 0;
        if (fflush(file) != 0)
            failed = 1;
        if (!failed) {
            int sync_result;
            do {
                sync_result = fsync(fileno(file));
            } while (sync_result < 0 && errno == EINTR);
            if (sync_result < 0)
                failed = 1;
        }
        if (fclose(file) != 0)
            failed = 1;
        if (failed)
            unlink(metadata_path);
    }
    free(metadata_path);
}

static int decoder_set_image(Decoder *decoder, const char *path, int width, int height,
                             char *error, size_t error_size)
{
    memset(decoder, 0, sizeof(*decoder));
    decoder->fd = -1;
    decoder->fps = 25;
    decoder->total_frames = 1;
    decoder->width = width;
    decoder->height = height;
    if (width <= 0 || height <= 0 ||
        (size_t)width > SIZE_MAX / ((size_t)height * 3)) {
        set_error(error, error_size, "Invalid image dimensions");
        return -1;
    }
    decoder->frame_size = (size_t)width * (size_t)height * 3;
    decoder->read_size = decoder->frame_size;
    decoder->path = strdup(path);
    if (!decoder->path) {
        set_error(error, error_size, "Out of memory");
        return -1;
    }
    return 0;
}

int decoder_probe_image(Decoder *decoder, const char *path, char *error, size_t error_size)
{
    static const uint8_t png_signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    uint8_t header[24];
    FILE *file = fopen(path, "rb");
    size_t count;
    if (!file)
        return decoder_probe(decoder, path, error, error_size);
    count = fread(header, 1, sizeof(header), file);
    if (count == sizeof(header) && memcmp(header, "LKGRGB01", 8) == 0) {
        int width = (int)((uint32_t)header[8] | (uint32_t)header[9] << 8 |
                          (uint32_t)header[10] << 16 | (uint32_t)header[11] << 24);
        int height = (int)((uint32_t)header[12] | (uint32_t)header[13] << 8 |
                           (uint32_t)header[14] << 16 | (uint32_t)header[15] << 24);
        fclose(file);
        return decoder_set_image(decoder, path, width, height, error, error_size);
    }
    if (count == sizeof(header) && memcmp(header, png_signature, sizeof(png_signature)) == 0) {
        int width = (int)((uint32_t)header[16] << 24 | (uint32_t)header[17] << 16 |
                          (uint32_t)header[18] << 8 | header[19]);
        int height = (int)((uint32_t)header[20] << 24 | (uint32_t)header[21] << 16 |
                           (uint32_t)header[22] << 8 | header[23]);
        fclose(file);
        return decoder_set_image(decoder, path, width, height, error, error_size);
    }
    if (count >= 2 && header[0] == 0xff && header[1] == 0xd8) {
        if (fseek(file, 2, SEEK_SET) == 0) {
            for (;;) {
                int marker;
                uint8_t length_bytes[2];
                int length;
                do {
                    marker = fgetc(file);
                    if (marker == EOF)
                        break;
                } while (marker != 0xff);
                if (marker == EOF)
                    break;
                do {
                    marker = fgetc(file);
                } while (marker == 0xff);
                if (marker == EOF || marker == 0xd9 || marker == 0xda)
                    break;
                if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd8))
                    continue;
                if (fread(length_bytes, 1, 2, file) != 2)
                    break;
                length = ((int)length_bytes[0] << 8) | length_bytes[1];
                if (length < 2)
                    break;
                if ((marker >= 0xc0 && marker <= 0xc3) ||
                    (marker >= 0xc5 && marker <= 0xc7) ||
                    (marker >= 0xc9 && marker <= 0xcb) ||
                    (marker >= 0xcd && marker <= 0xcf)) {
                    uint8_t dimensions[5];
                    if (length < 7 || fread(dimensions, 1, sizeof(dimensions), file) != sizeof(dimensions))
                        break;
                    fclose(file);
                    return decoder_set_image(decoder, path,
                        (dimensions[3] << 8) | dimensions[4],
                        (dimensions[1] << 8) | dimensions[2], error, error_size);
                }
                if (fseek(file, length - 2, SEEK_CUR) != 0)
                    break;
            }
        }
    }
    fclose(file);
    if (heic_spatial_pair(path, NULL, NULL)) {
        int result = decoder_set_image(decoder, path, 800, 600, error, error_size);
        if (result == 0) {
            decoder->spatial = 1;
            decoder->stereo = 1;
            decoder->frame_size = decoder->read_size * 2;
        }
        return result;
    }
    {
        int width = 0;
        int height = 0;
        if (heic_primary_dimensions(path, &width, &height, error, error_size) == 0) {
            int result = decoder_set_image(decoder, path, width, height, error, error_size);
            if (result == 0)
                decoder->heic = 1;
            return result;
        }
        if (count >= 12 && memcmp(header + 4, "ftyp", 4) == 0)
            return -1;
    }
    return decoder_probe(decoder, path, error, error_size);
}

static unsigned long long preprocessing_available_kb(void)
{
    FILE *file = fopen("/proc/meminfo", "r");
    char line[256];
    unsigned long long available = 0;

    if (!file)
        return 0;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "MemAvailable: %llu kB", &available) == 1)
            break;
    }
    fclose(file);
    return available;
}

int decoder_preprocessing_threads(const Decoder *decoder)
{
    uint64_t pixels = (uint64_t)decoder->width * decoder->height *
                      (decoder->stereo ? 2ULL : 1ULL);
    unsigned long long available_kb = preprocessing_available_kb();

    if (pixels >= 24000000 || (available_kb && available_kb < 1024 * 1024))
        return 1;
    if (pixels >= 8000000 || decoder->stereo ||
        (available_kb && available_kb < 1536 * 1024))
        return 2;
    return 4;
}

int decoder_start(Decoder *decoder, char *error, size_t error_size)
{
    int pipefd[2];
    if (decoder->heic)
        return heic_decode_primary(decoder->path, decoder->width, decoder->height,
                                   &decoder->still_frame, error, error_size);
    if (decoder->hardware)
        return gstreamer_start(decoder, error, error_size);
    if (!decoder->stereo) {
        decoder->decode_threads = decoder_preprocessing_threads(decoder);
        return gstreamer_start_software(decoder, error, error_size);
    }
    pid_t pid;
    pid_t parent_pid = getpid();
    int threads = decoder_preprocessing_threads(decoder);
    char thread_count[16];

    snprintf(thread_count, sizeof(thread_count), "%d", threads);
    if (pipe(pipefd) != 0) {
        set_error(error, error_size, "Failed to create FFmpeg pipe: %s", strerror(errno));
        return -1;
    }
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
#ifdef F_SETPIPE_SZ
    (void)fcntl(pipefd[1], F_SETPIPE_SZ, 1024 * 1024);
#endif
    pid = fork();
    if (pid < 0) {
        set_error(error, error_size, "Failed to start FFmpeg: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        if (process_bind_parent(parent_pid) != 0)
            _exit(1);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        {
            uint64_t rgb_frame = (uint64_t)decoder->width * (uint64_t)decoder->height * 3 * 2;
            uint64_t limit_bytes = 1024ULL * 1024 * 1024 + rgb_frame * 4;
            struct rlimit address_limit = {limit_bytes, limit_bytes};
            if (setrlimit(RLIMIT_AS, &address_limit) != 0) {
                perror("Failed to limit stereo decoder memory");
                _exit(1);
            }
        }
        execlp("ffmpeg", "ffmpeg", "-v", "fatal", "-threads", thread_count,
               "-view_ids", "-1",
               "-i", decoder->path, "-map", "0:v:0", "-an", "-sn", "-dn",
               "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt",
               decoder->yuv420 ? "yuv420p" : "rgb24", "pipe:1", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    decoder->fd = pipefd[0];
    decoder->pid = pid;
    return 0;
}

static int decoder_read_ffmpeg_frame(Decoder *decoder, uint8_t *frame)
{
    size_t offset = 0;
    while (offset < decoder->read_size) {
        ssize_t count = read(decoder->fd, frame + offset, decoder->read_size - offset);
        if (count > 0) {
            offset += (size_t)count;
        } else if (count == 0) {
            int status;
            pid_t result = 0;
            if (offset > 0) {
                errno = EIO;
                return -1;
            }
            if (decoder->pid > 0) {
                do {
                    result = waitpid(decoder->pid, &status, 0);
                } while (result < 0 && errno == EINTR);
                if (result == decoder->pid)
                    decoder->pid = 0;
                if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                    errno = EIO;
                    return -1;
                }
            }
            return 0;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    return 1;
}

int decoder_read_frame(Decoder *decoder, uint8_t *frame)
{
    if (decoder->heic) {
        if (!decoder->still_frame || !frame)
            return -1;
        memcpy(frame, decoder->still_frame, decoder->read_size);
        return 1;
    }
    if (decoder->hardware) {
        char error[256] = {0};
        int result = gstreamer_read(decoder, error, sizeof(error));
        if (result < 0)
            fprintf(stderr, "GStreamer: %s\n", error);
        return result;
    }
    if (decoder->gst_pipeline) {
        char error[256] = {0};
        int result = gstreamer_read_software(decoder, frame, error, sizeof(error));
        if (result < 0)
            fprintf(stderr, "GStreamer software decoder: %s\n", error);
        return result;
    }
    if (decoder->stereo) {
        int input_index;
        if (!frame) {
            errno = EINVAL;
            return -1;
        }
        for (input_index = 0; input_index < 2; ++input_index) {
            int output_index = input_index == decoder->stereo_left_index ? 0 : 1;
            int result = decoder_read_ffmpeg_frame(decoder,
                frame + (size_t)output_index * decoder->read_size);
            if (result != 1) {
                if (result == 0 && input_index > 0) {
                    errno = EIO;
                    return -1;
                }
                return result;
            }
        }
        return 1;
    }
    return decoder_read_ffmpeg_frame(decoder, frame);
}

void decoder_stop(Decoder *decoder)
{
    if (decoder->heic) {
        free(decoder->still_frame);
        decoder->still_frame = NULL;
        return;
    }
    if (decoder->hardware || decoder->gst_pipeline) {
        gstreamer_stop(decoder);
        return;
    }
    if (decoder->fd >= 0) {
        close(decoder->fd);
        decoder->fd = -1;
    }
    if (decoder->pid > 0) {
        int status;
        kill(decoder->pid, SIGTERM);
        while (waitpid(decoder->pid, &status, 0) < 0 && errno == EINTR)
            ;
        decoder->pid = 0;
    }
}
