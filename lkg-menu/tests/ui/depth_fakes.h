#ifndef LKG_MENU_DEPTH_FAKES_H
#define LKG_MENU_DEPTH_FAKES_H

#include "depth.h"

typedef struct {
    int direct_upload;
    int force_full;
    uint64_t revision;
    int begin_calls;
    int upload_calls;
    int anim_region_calls;
    int anim_clear_calls;
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
    int last_bar_top;
    int last_bar_bottom;
    int last_has_back;
    int last_clip_x;
    int last_clip_y;
    int last_clip_width;
    int last_clip_height;
    float last_first_disparity;
    float last_second_disparity;
    uint32_t packed_first[2];
    uint64_t committed_key;
} DepthFake;

extern DepthFake depth_fake;

void depth_fake_reset(void);
int test_depth_frame(void);

#endif
