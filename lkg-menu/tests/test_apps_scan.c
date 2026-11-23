#include "test.h"
#include "apps_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_text(const char *path, const char *content, mode_t mode)
{
    FILE *file = fopen(path, "wb");
    size_t len;
    if (!file)
        return -1;
    len = strlen(content);
    if (len > 0 && fwrite(content, 1, len, file) != len) {
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0)
        return -1;
    if (chmod(path, mode) != 0)
        return -1;
    return 0;
}

static void make_app(const char *root, const char *folder, const char *json,
                     const char *exec_name, mode_t exec_mode)
{
    char dir[512];
    char path[768];
    snprintf(dir, sizeof(dir), "%s/%s", root, folder);
    mkdir(dir, 0755);
    if (json) {
        snprintf(path, sizeof(path), "%s/app.json", dir);
        (void)write_text(path, json, 0644);
    }
    if (exec_name) {
        snprintf(path, sizeof(path), "%s/%s", dir, exec_name);
        (void)write_text(path, "ELF", exec_mode);
    }
}

int test_apps_scan(void)
{
    char root[] = "/tmp/lkg-apps-scan-XXXXXX";
    char path[768];
    ExternalAppList list;

    TEST_ASSERT(apps_scan_has_apps("/tmp/lkg-apps-nonexistent-xyz") == 0);
    TEST_ASSERT(apps_scan_directory(&list, "/tmp/lkg-apps-nonexistent-xyz") != 0);

    TEST_ASSERT(mkdtemp(root) != NULL);
    TEST_ASSERT(apps_scan_has_apps(root) == 0);
    TEST_ASSERT(apps_scan_directory(&list, root) == 0);
    TEST_ASSERT(list.count == 0);
    apps_list_free(&list);

    /* Valid hosted app (note non-executable 0644 file as on exFAT) */
    make_app(root, "z-demo",
             "{\n"
             "  \"extra\": {\"name\": \"Nested\"},\n"
             "  \"name\": \"Zeta \\\"Demo\\\"\",\n"
             "  \"exec\": \"demo\",\n"
             "  \"version\": \"1.2.0\",\n"
             "  \"description\": \"Test app\",\n"
             "  \"author\": \"LKG\",\n"
             "  \"render\": \"hosted\",\n"
             "  \"api_version\": 1\n"
             "}\n",
             "demo", 0644);

    /* Valid drm app */
    make_app(root, "a-drm",
             "{\"name\": \"Alpha DRM\", \"exec\": \"alpha\", \"render\": \"drm\"}",
             "alpha", 0755);

    /* Hidden folder should be ignored */
    make_app(root, ".hidden",
             "{\"name\": \"Hidden\", \"exec\": \"hid\"}",
             "hid", 0755);

    /* Invalid JSON */
    make_app(root, "bad-json", "{broken", "bin", 0755);

    /* Missing exec binary */
    make_app(root, "missing-bin",
             "{\"name\": \"Missing Bin\", \"exec\": \"nope\"}",
             NULL, 0);

    /* Path traversal rejected */
    make_app(root, "traversal",
             "{\"name\": \"Traversal\", \"exec\": \"../a-drm/alpha\"}",
             "ok", 0755);

    /* Absolute path rejected */
    make_app(root, "abs-exec",
             "{\"name\": \"Abs\", \"exec\": \"/bin/sh\"}",
             "ok", 0755);

    /* Unsupported render mode rejected */
    make_app(root, "bad-render",
             "{\"name\": \"Bad Render\", \"exec\": \"ok\", \"render\": \"wayland\"}",
             "ok", 0755);

    /* Unsupported api_version rejected */
    make_app(root, "bad-api",
             "{\"name\": \"Future\", \"exec\": \"ok\", \"api_version\": 99}",
             "ok", 0755);

    /* Empty name rejected */
    make_app(root, "empty-name",
             "{\"name\": \"\", \"exec\": \"ok\"}",
             "ok", 0755);

    TEST_ASSERT(apps_scan_has_apps(root) == 1);
    TEST_ASSERT(apps_scan_directory(&list, root) == 0);
    TEST_ASSERT(list.count == 2);

    TEST_STRING(list.items[0].name, "Alpha DRM");
    TEST_STRING(list.items[0].dir_name, "a-drm");
    TEST_ASSERT(list.items[0].render_mode == LKG_RENDER_DRM);
    TEST_ASSERT(list.items[0].api_version == 1);

    TEST_STRING(list.items[1].name, "Zeta \"Demo\"");
    TEST_STRING(list.items[1].dir_name, "z-demo");
    TEST_STRING(list.items[1].version, "1.2.0");
    TEST_STRING(list.items[1].description, "Test app");
    TEST_STRING(list.items[1].author, "LKG");
    TEST_ASSERT(list.items[1].render_mode == LKG_RENDER_HOSTED);
    apps_list_free(&list);

    /* Clean up temporary directories */
    {
        const char *dirs[] = {
            "z-demo/app.json", "z-demo/demo", "z-demo",
            "a-drm/app.json", "a-drm/alpha", "a-drm",
            ".hidden/app.json", ".hidden/hid", ".hidden",
            "bad-json/app.json", "bad-json/bin", "bad-json",
            "missing-bin/app.json", "missing-bin",
            "traversal/app.json", "traversal/ok", "traversal",
            "abs-exec/app.json", "abs-exec/ok", "abs-exec",
            "bad-render/app.json", "bad-render/ok", "bad-render",
            "bad-api/app.json", "bad-api/ok", "bad-api",
            "empty-name/app.json", "empty-name/ok", "empty-name"
        };
        size_t i;
        for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
            snprintf(path, sizeof(path), "%s/%s", root, dirs[i]);
            (void)unlink(path);
            (void)rmdir(path);
        }
        (void)rmdir(root);
    }
    return 0;
}
