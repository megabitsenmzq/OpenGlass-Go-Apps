#ifndef LKG_MENU_DEPTH_FAKES_H
#define LKG_MENU_DEPTH_FAKES_H

#include "depth.h"

typedef struct {
    int direct_upload;
    int force_full;
    uint64_t revision;
    int begin_calls;
    int cached_calls;
    int region_calls;
    int present_calls;
    int commit_calls;
    int finish_calls;
    int fail_begin;
    int fail_update;
    int fail_present;
    int last_x;
    int last_y;
    int last_width;
    int last_height;
    uint32_t packed_first[2];
    uint64_t committed_key;
} DepthFake;

extern DepthFake depth_fake;

void depth_fake_reset(void);
int test_depth_frame(void);

#endif
