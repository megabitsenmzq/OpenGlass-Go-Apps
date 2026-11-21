#include "test.h"
#include "depth_cache.h"

int test_depth_cache(void)
{
    DepthCacheState state = {0};
    TEST_ASSERT(depth_cache_needs_full_revision(&state, 10, 1, 1) == 1);
    depth_cache_commit_revision(&state, 10, 1, 1);
    TEST_ASSERT(state.initialized == 1);
    TEST_ASSERT(depth_cache_needs_full_revision(&state, 10, 1, 1) == 0);
    TEST_ASSERT(depth_cache_needs_full_revision(&state, 11, 1, 1) == 1);
    TEST_ASSERT(depth_cache_needs_full_revision(&state, 10, 2, 1) == 1);
    TEST_ASSERT(depth_cache_needs_full_revision(&state, 10, 1, 0) == 1);
    depth_cache_commit_revision(&state, 10, 1, 0);
    TEST_ASSERT(state.initialized == 0);
    depth_cache_commit_revision(&state, 11, 2, 1);
    TEST_ASSERT(depth_cache_needs_full_revision(&state, 11, 2, 1) == 0);
    return 0;
}
