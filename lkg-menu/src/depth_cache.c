#include "depth_cache.h"

int depth_cache_needs_full_revision(const DepthCacheState *state, uint64_t content_key,
                                    uint64_t gpu_revision, int gpu_available)
{
    return !state->initialized || !gpu_available || state->content_key != content_key ||
           state->revision != gpu_revision;
}

void depth_cache_commit_revision(DepthCacheState *state, uint64_t content_key,
                                 uint64_t gpu_revision, int gpu_available)
{
    state->revision = gpu_revision;
    state->content_key = content_key;
    state->initialized = gpu_available;
}
