#include "apps_scan.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_APP_JSON_SIZE 16384u

typedef struct {
    const char *cursor;
    const char *end;
} JsonReader;

static void json_skip_ws(JsonReader *reader)
{
    while (reader->cursor < reader->end) {
        char c = *reader->cursor;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            ++reader->cursor;
        else
            break;
    }
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

static int parse_hex4(JsonReader *reader, unsigned int *out)
{
    unsigned int val = 0;
    int i;
    if (reader->end - reader->cursor < 4)
        return -1;
    for (i = 0; i < 4; ++i) {
        int d = hex_digit(reader->cursor[i]);
        if (d < 0)
            return -1;
        val = (val << 4) | (unsigned int)d;
    }
    reader->cursor += 4;
    *out = val;
    return 0;
}

static int append_utf8(char *buf, size_t cap, size_t *len, unsigned int codepoint)
{
    if (codepoint == 0)
        return -1;
    if (codepoint <= 0x7Fu) {
        if (*len + 1 >= cap)
            return -1;
        buf[(*len)++] = (char)codepoint;
    } else if (codepoint <= 0x7FFu) {
        if (*len + 2 >= cap)
            return -1;
        buf[(*len)++] = (char)(0xC0u | (codepoint >> 6));
        buf[(*len)++] = (char)(0x80u | (codepoint & 0x3Fu));
    } else if (codepoint <= 0xFFFFu) {
        if (*len + 3 >= cap)
            return -1;
        buf[(*len)++] = (char)(0xE0u | (codepoint >> 12));
        buf[(*len)++] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        buf[(*len)++] = (char)(0x80u | (codepoint & 0x3Fu));
    } else if (codepoint <= 0x10FFFFu) {
        if (*len + 4 >= cap)
            return -1;
        buf[(*len)++] = (char)(0xF0u | (codepoint >> 18));
        buf[(*len)++] = (char)(0x80u | ((codepoint >> 12) & 0x3Fu));
        buf[(*len)++] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        buf[(*len)++] = (char)(0x80u | (codepoint & 0x3Fu));
    } else {
        return -1;
    }
    return 0;
}

static int json_parse_string(JsonReader *reader, char *out, size_t out_cap)
{
    size_t len = 0;
    json_skip_ws(reader);
    if (reader->cursor >= reader->end || *reader->cursor != '"')
        return -1;
    ++reader->cursor;
    while (reader->cursor < reader->end) {
        unsigned char uc = (unsigned char)*reader->cursor++;
        if (uc == '"') {
            if (out && out_cap > 0)
                out[len] = '\0';
            return 0;
        }
        if (uc < 0x20u)
            return -1;
        if (uc == '\\') {
            char esc;
            if (reader->cursor >= reader->end)
                return -1;
            esc = *reader->cursor++;
            if (esc == '"' || esc == '\\' || esc == '/') {
                if (out) {
                    if (len + 1 >= out_cap)
                        return -1;
                    out[len++] = esc;
                }
            } else if (esc == 'b' || esc == 'f' || esc == 'n' || esc == 'r' || esc == 't') {
                char mapped = esc == 'b' ? '\b' : esc == 'f' ? '\f' :
                              esc == 'n' ? '\n' : esc == 'r' ? '\r' : '\t';
                if (out) {
                    if (len + 1 >= out_cap)
                        return -1;
                    out[len++] = mapped;
                }
            } else if (esc == 'u') {
                unsigned int cp = 0;
                if (parse_hex4(reader, &cp) != 0)
                    return -1;
                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    unsigned int low = 0;
                    if (reader->end - reader->cursor < 2 ||
                        reader->cursor[0] != '\\' || reader->cursor[1] != 'u')
                        return -1;
                    reader->cursor += 2;
                    if (parse_hex4(reader, &low) != 0 || low < 0xDC00u || low > 0xDFFFu)
                        return -1;
                    cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                    return -1;
                }
                if (out && append_utf8(out, out_cap, &len, cp) != 0)
                    return -1;
            } else {
                return -1;
            }
        } else if (out) {
            if (len + 1 >= out_cap)
                return -1;
            out[len++] = (char)uc;
        }
    }
    return -1;
}

static int json_parse_int(JsonReader *reader, int *out)
{
    const char *start;
    char buf[32];
    char *endptr = NULL;
    long val;
    size_t len;
    json_skip_ws(reader);
    start = reader->cursor;
    if (reader->cursor < reader->end && *reader->cursor == '-')
        ++reader->cursor;
    if (reader->cursor >= reader->end || !isdigit((unsigned char)*reader->cursor))
        return -1;
    while (reader->cursor < reader->end && isdigit((unsigned char)*reader->cursor))
        ++reader->cursor;
    if (reader->cursor < reader->end &&
        (*reader->cursor == '.' || *reader->cursor == 'e' || *reader->cursor == 'E'))
        return -1;
    len = (size_t)(reader->cursor - start);
    if (len == 0 || len >= sizeof(buf))
        return -1;
    memcpy(buf, start, len);
    buf[len] = '\0';
    errno = 0;
    val = strtol(buf, &endptr, 10);
    if (errno != 0 || !endptr || *endptr != '\0' || val < INT_MIN || val > INT_MAX)
        return -1;
    *out = (int)val;
    return 0;
}

static int json_skip_value(JsonReader *reader, int depth);

static int json_skip_literal(JsonReader *reader, const char *lit)
{
    size_t len = strlen(lit);
    if ((size_t)(reader->end - reader->cursor) < len ||
        memcmp(reader->cursor, lit, len) != 0)
        return -1;
    reader->cursor += len;
    return 0;
}

static int json_skip_number(JsonReader *reader)
{
    if (reader->cursor < reader->end && *reader->cursor == '-')
        ++reader->cursor;
    if (reader->cursor >= reader->end || !isdigit((unsigned char)*reader->cursor))
        return -1;
    while (reader->cursor < reader->end && isdigit((unsigned char)*reader->cursor))
        ++reader->cursor;
    if (reader->cursor < reader->end && *reader->cursor == '.') {
        ++reader->cursor;
        if (reader->cursor >= reader->end || !isdigit((unsigned char)*reader->cursor))
            return -1;
        while (reader->cursor < reader->end && isdigit((unsigned char)*reader->cursor))
            ++reader->cursor;
    }
    if (reader->cursor < reader->end && (*reader->cursor == 'e' || *reader->cursor == 'E')) {
        ++reader->cursor;
        if (reader->cursor < reader->end && (*reader->cursor == '+' || *reader->cursor == '-'))
            ++reader->cursor;
        if (reader->cursor >= reader->end || !isdigit((unsigned char)*reader->cursor))
            return -1;
        while (reader->cursor < reader->end && isdigit((unsigned char)*reader->cursor))
            ++reader->cursor;
    }
    return 0;
}

static int json_skip_value(JsonReader *reader, int depth)
{
    char c;
    if (depth > 32)
        return -1;
    json_skip_ws(reader);
    if (reader->cursor >= reader->end)
        return -1;
    c = *reader->cursor;
    if (c == '"')
        return json_parse_string(reader, NULL, 0);
    if (c == '{') {
        ++reader->cursor;
        json_skip_ws(reader);
        if (reader->cursor < reader->end && *reader->cursor == '}') {
            ++reader->cursor;
            return 0;
        }
        for (;;) {
            if (json_parse_string(reader, NULL, 0) != 0)
                return -1;
            json_skip_ws(reader);
            if (reader->cursor >= reader->end || *reader->cursor != ':')
                return -1;
            ++reader->cursor;
            if (json_skip_value(reader, depth + 1) != 0)
                return -1;
            json_skip_ws(reader);
            if (reader->cursor >= reader->end)
                return -1;
            if (*reader->cursor == '}') {
                ++reader->cursor;
                return 0;
            }
            if (*reader->cursor != ',')
                return -1;
            ++reader->cursor;
        }
    }
    if (c == '[') {
        ++reader->cursor;
        json_skip_ws(reader);
        if (reader->cursor < reader->end && *reader->cursor == ']') {
            ++reader->cursor;
            return 0;
        }
        for (;;) {
            if (json_skip_value(reader, depth + 1) != 0)
                return -1;
            json_skip_ws(reader);
            if (reader->cursor >= reader->end)
                return -1;
            if (*reader->cursor == ']') {
                ++reader->cursor;
                return 0;
            }
            if (*reader->cursor != ',')
                return -1;
            ++reader->cursor;
        }
    }
    if (c == 't')
        return json_skip_literal(reader, "true");
    if (c == 'f')
        return json_skip_literal(reader, "false");
    if (c == 'n')
        return json_skip_literal(reader, "null");
    return json_skip_number(reader);
}

static int valid_relative_exec(const char *exec_rel)
{
    const char *p = exec_rel;
    size_t len = strlen(exec_rel);
    if (len == 0 || len > 255 || exec_rel[0] == '/')
        return 0;
    while (*p) {
        const char *seg_start = p;
        size_t seg_len;
        while (*p && *p != '/') {
            if ((unsigned char)*p < 0x20u || *p == '\\')
                return 0;
            ++p;
        }
        seg_len = (size_t)(p - seg_start);
        if (seg_len == 0)
            return 0;
        if (seg_len == 1 && seg_start[0] == '.')
            return 0;
        if (seg_len == 2 && seg_start[0] == '.' && seg_start[1] == '.')
            return 0;
        if (*p == '/') {
            ++p;
            if (*p == '\0')
                return 0;
        }
    }
    return 1;
}

static void external_app_free(ExternalApp *app)
{
    free(app->dir_name);
    free(app->dir_path);
    free(app->exec_path);
    free(app->name);
    free(app->version);
    free(app->description);
    free(app->author);
    free(app->font_path);
    memset(app, 0, sizeof(*app));
}

static int parse_app_json(const char *json_path, const char *dir_name,
                          const char *dir_path, ExternalApp *app)
{
    FILE *file = fopen(json_path, "rb");
    char *buffer;
    size_t read_len;
    JsonReader reader;
    char name[128] = {0};
    char exec_rel[256] = {0};
    char version[64] = {0};
    char description[256] = {0};
    char author[128] = {0};
    char render[32] = "hosted";
    char font[PATH_MAX] = {0};
    int api_version = (int)LKG_APP_API_VERSION;
    char exec_full[PATH_MAX];
    char font_full[PATH_MAX];
    struct stat exec_st;
    int first = 1;

    if (!file)
        return -1;
    buffer = malloc(MAX_APP_JSON_SIZE + 1);
    if (!buffer) {
        fclose(file);
        return -1;
    }
    read_len = fread(buffer, 1, MAX_APP_JSON_SIZE + 1, file);
    fclose(file);
    if (read_len == 0 || read_len > MAX_APP_JSON_SIZE) {
        free(buffer);
        return -1;
    }
    buffer[read_len] = '\0';
    reader.cursor = buffer;
    reader.end = buffer + read_len;

    json_skip_ws(&reader);
    if (reader.cursor >= reader.end || *reader.cursor != '{') {
        free(buffer);
        return -1;
    }
    ++reader.cursor;

    for (;;) {
        char key[64];
        json_skip_ws(&reader);
        if (reader.cursor >= reader.end)
            goto fail;
        if (*reader.cursor == '}') {
            ++reader.cursor;
            break;
        }
        if (!first) {
            if (*reader.cursor != ',')
                goto fail;
            ++reader.cursor;
            json_skip_ws(&reader);
        }
        first = 0;
        if (json_parse_string(&reader, key, sizeof(key)) != 0)
            goto fail;
        json_skip_ws(&reader);
        if (reader.cursor >= reader.end || *reader.cursor != ':')
            goto fail;
        ++reader.cursor;
        if (strcmp(key, "name") == 0) {
            if (json_parse_string(&reader, name, sizeof(name)) != 0)
                goto fail;
        } else if (strcmp(key, "exec") == 0) {
            if (json_parse_string(&reader, exec_rel, sizeof(exec_rel)) != 0)
                goto fail;
        } else if (strcmp(key, "version") == 0) {
            if (json_parse_string(&reader, version, sizeof(version)) != 0)
                goto fail;
        } else if (strcmp(key, "description") == 0) {
            if (json_parse_string(&reader, description, sizeof(description)) != 0)
                goto fail;
        } else if (strcmp(key, "author") == 0) {
            if (json_parse_string(&reader, author, sizeof(author)) != 0)
                goto fail;
        } else if (strcmp(key, "render") == 0) {
            if (json_parse_string(&reader, render, sizeof(render)) != 0)
                goto fail;
        } else if (strcmp(key, "font") == 0) {
            if (json_parse_string(&reader, font, sizeof(font)) != 0)
                goto fail;
        } else if (strcmp(key, "api_version") == 0) {
            if (json_parse_int(&reader, &api_version) != 0)
                goto fail;
        } else {
            if (json_skip_value(&reader, 0) != 0)
                goto fail;
        }
    }

    json_skip_ws(&reader);
    if (reader.cursor != reader.end)
        goto fail;
    free(buffer);
    buffer = NULL;

    if (!name[0] || !valid_relative_exec(exec_rel))
        return -1;
    if (api_version < 1 || api_version > (int)LKG_APP_API_VERSION)
        return -1;
    if (strcmp(render, "hosted") == 0)
        app->render_mode = LKG_RENDER_HOSTED;
    else if (strcmp(render, "drm") == 0)
        app->render_mode = LKG_RENDER_DRM;
    else
        return -1;
    if (font[0] && font[0] != '/' && !valid_relative_exec(font))
        return -1;

    if (snprintf(exec_full, sizeof(exec_full), "%s/%s", dir_path, exec_rel) >=
        (int)sizeof(exec_full))
        return -1;
    if (stat(exec_full, &exec_st) != 0 || !S_ISREG(exec_st.st_mode))
        return -1;

    app->dir_name = strdup(dir_name);
    app->dir_path = strdup(dir_path);
    app->exec_path = strdup(exec_full);
    app->name = strdup(name);
    app->version = strdup(version);
    app->description = strdup(description);
    app->author = strdup(author);
    app->api_version = api_version;
    if (font[0] == '/') {
        app->font_path = strdup(font);
    } else if (font[0] && snprintf(font_full, sizeof(font_full), "%s/%s", dir_path, font) <
               (int)sizeof(font_full)) {
        app->font_path = strdup(font_full);
    }
    if (!app->dir_name || !app->dir_path || !app->exec_path || !app->name ||
        !app->version || !app->description || !app->author || (font[0] && !app->font_path)) {
        external_app_free(app);
        return -1;
    }
    return 0;

fail:
    free(buffer);
    return -1;
}

static int app_compare(const void *left_ptr, const void *right_ptr)
{
    const ExternalApp *a = left_ptr;
    const ExternalApp *b = right_ptr;
    int cmp = strcasecmp(a->name, b->name);
    if (cmp != 0)
        return cmp;
    return strcmp(a->dir_name, b->dir_name);
}

void apps_list_free(ExternalAppList *list)
{
    size_t i;
    if (!list)
        return;
    for (i = 0; i < list->count; ++i)
        external_app_free(&list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

int apps_scan_directory(ExternalAppList *list, const char *directory)
{
    DIR *dir;
    struct dirent *entry;
    if (!list || !directory)
        return -1;
    memset(list, 0, sizeof(*list));
    dir = opendir(directory);
    if (!dir)
        return -1;
    while ((entry = readdir(dir)) != NULL) {
        char dir_path[PATH_MAX];
        char json_path[PATH_MAX];
        struct stat st;
        struct stat json_st;
        ExternalApp parsed;
        ExternalApp *resized;
        if (entry->d_name[0] == '.')
            continue;
        if (snprintf(dir_path, sizeof(dir_path), "%s/%s", directory, entry->d_name) >=
            (int)sizeof(dir_path))
            continue;
        if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        if (snprintf(json_path, sizeof(json_path), "%s/app.json", dir_path) >=
            (int)sizeof(json_path))
            continue;
        if (stat(json_path, &json_st) != 0 || !S_ISREG(json_st.st_mode))
            continue;
        memset(&parsed, 0, sizeof(parsed));
        if (parse_app_json(json_path, entry->d_name, dir_path, &parsed) != 0)
            continue;
        resized = realloc(list->items, (list->count + 1) * sizeof(*resized));
        if (!resized) {
            external_app_free(&parsed);
            closedir(dir);
            apps_list_free(list);
            return -1;
        }
        list->items = resized;
        list->items[list->count++] = parsed;
    }
    closedir(dir);
    if (list->count > 1)
        qsort(list->items, list->count, sizeof(*list->items), app_compare);
    return 0;
}

int apps_scan_has_apps(const char *directory)
{
    ExternalAppList list;
    int has = 0;
    if (apps_scan_directory(&list, directory) == 0) {
        has = list.count > 0;
        apps_list_free(&list);
    }
    return has;
}
