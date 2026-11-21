#ifndef LKG_MENU_DEPTH_CACHE_H
#define LKG_MENU_DEPTH_CACHE_H

#include <stdint.h>

typedef struct {
    uint64_t revision;
    uint64_t content_key;
    int initialized;
} DepthCacheState;

int depth_cache_needs_full_revision(const DepthCacheState *state, uint64_t content_key,
                                    uint64_t gpu_revision, int gpu_available);
void depth_cache_commit_revision(DepthCacheState *state, uint64_t content_key,
                                 uint64_t gpu_revision, int gpu_available);

#endif
