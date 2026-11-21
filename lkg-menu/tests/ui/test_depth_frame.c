#include "../test.h"
#include "depth_fakes.h"
#include "depth_frame.h"

#include <stdlib.h>

static void setup(Display *display, DepthEffect *effect, DepthLayer layers[2],
                  DepthCacheState *state)
{
    int layer;
    memset(display, 0, sizeof(*display));
    memset(effect, 0, sizeof(*effect));
    memset(state, 0, sizeof(*state));
    display->mode.hdisplay = 256;
    display->mode.vdisplay = 64;
    effect->gpu = (void *)1;
    state->initialized = 1;
    state->content_key = 7;
    state->revision = 1;
    for (layer = 0; layer < 2; ++layer) {
        layers[layer].buffer.stride = 256 * 4;
        layers[layer].buffer.size = 256 * 64 * 4;
        layers[layer].buffer.pixels = calloc(1, layers[layer].buffer.size);
    }
    layers[0].disparity = 70.0f;
    layers[1].disparity = -56.0f;
}

static void teardown(DepthLayer layers[2])
{
    free(layers[0].buffer.pixels);
    free(layers[1].buffer.pixels);
}

int test_depth_frame(void)
{
    Display display;
    DepthEffect effect;
    DepthLayer layers[2];
    DepthCacheState state;
    DepthFrame frame;
    depth_fake_reset();
    setup(&display, &effect, layers, &state);
    TEST_ASSERT(layers[0].buffer.pixels && layers[1].buffer.pixels);
    TEST_ASSERT(depth_frame_begin(&frame, &display, &effect, layers, &state, 7, 0) == 0);
    TEST_ASSERT(frame.full == 0 && depth_fake.begin_calls == 0);
    TEST_ASSERT(depth_frame_update_rect(&frame, 0, 0, 10, 10, 0, 0) == 0);
    TEST_ASSERT(depth_frame_update_rect(&frame, 20, 20, 10, 10, 0, 0) == 0);
    TEST_ASSERT(depth_fake.cached_calls == 2 && depth_fake.present_calls == 0);
    TEST_ASSERT(depth_fake.last_x == 0 && depth_fake.last_width == 100);
    TEST_ASSERT(depth_frame_present(&frame) == 0);
    TEST_ASSERT(depth_fake.present_calls == 1 && depth_fake.commit_calls == 1);
    TEST_ASSERT(depth_fake.committed_key == 7 && state.content_key == 7);
    TEST_ASSERT(frame.packed[0] == NULL && frame.packed[1] == NULL);

    depth_fake_reset();
    depth_fake.direct_upload = 0;
    *(uint32_t *)layers[0].buffer.pixels = 0x11223344;
    *(uint32_t *)layers[1].buffer.pixels = 0x55667788;
    TEST_ASSERT(depth_frame_begin(&frame, &display, &effect, layers, &state, 7, 0) == 0);
    TEST_ASSERT(depth_frame_update_rect(&frame, 0, 0, 4, 4, 0, 0) == 0);
    TEST_ASSERT(depth_fake.region_calls == 1 && depth_fake.cached_calls == 0);
    TEST_ASSERT(depth_fake.packed_first[0] == 0x11223344);
    TEST_ASSERT(depth_fake.packed_first[1] == 0x55667788);
    TEST_ASSERT(depth_frame_present(&frame) == 0);
    TEST_ASSERT(depth_fake.present_calls == 1 && depth_fake.commit_calls == 1);

    depth_fake_reset();
    depth_fake.fail_update = 1;
    TEST_ASSERT(depth_frame_begin(&frame, &display, &effect, layers, &state, 7, 0) == 0);
    TEST_ASSERT(depth_frame_update_rect(&frame, 0, 0, 4, 4, 0, 0) == -1);
    TEST_ASSERT(depth_frame_present(&frame) == -1);
    TEST_ASSERT(depth_fake.present_calls == 0 && depth_fake.commit_calls == 0);

    depth_fake_reset();
    depth_fake.fail_present = 1;
    TEST_ASSERT(depth_frame_begin(&frame, &display, &effect, layers, &state, 7, 0) == 0);
    TEST_ASSERT(depth_frame_update_rect(&frame, 0, 0, 4, 4, 0, 0) == 0);
    TEST_ASSERT(depth_frame_present(&frame) == -1);
    TEST_ASSERT(depth_fake.present_calls == 1 && depth_fake.commit_calls == 0);

    depth_fake_reset();
    depth_fake.revision = 2;
    TEST_ASSERT(depth_frame_begin(&frame, &display, &effect, layers, &state, 7, 0) == 0);
    TEST_ASSERT(frame.full == 1 && depth_fake.begin_calls == 1);
    TEST_ASSERT(depth_frame_update_rect(&frame, 0, 0, 64, 64, 0, 0) == 0);
    TEST_ASSERT(depth_frame_present(&frame) == 0);
    TEST_ASSERT(state.revision == 2);

    depth_fake_reset();
    TEST_ASSERT(depth_frame_begin(&frame, &display, &effect, layers, NULL, 0, 0) == 0);
    TEST_ASSERT(depth_frame_update_rect(&frame, 0, 0, 4, 4, 0, 0) == 0);
    TEST_ASSERT(depth_frame_present(&frame) == 0);
    TEST_ASSERT(depth_fake.present_calls == 1 && depth_fake.commit_calls == 0);

    depth_fake_reset();
    TEST_ASSERT(depth_frame_begin(&frame, &display, &effect, layers, &state, 7, 0) == 0);
    TEST_ASSERT(depth_frame_update_rect(&frame, 0, 0, 4, 4, 0, 0) == 0);
    frame.defer_cache_commit = 1;
    TEST_ASSERT(depth_frame_present(&frame) == 0);
    TEST_ASSERT(depth_fake.commit_calls == 0);
    frame.defer_cache_commit = 0;
    depth_frame_sync_cache(&frame);
    TEST_ASSERT(depth_fake.commit_calls == 1);
    teardown(layers);
    return 0;
}
