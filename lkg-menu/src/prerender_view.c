#include "prerender_view.h"
#include "gallery_view.h"

#include <string.h>

static void prerender_bar(Display *display, Buffer *buffer, int x, int y,
                          int width, double progress)
{
    if (progress < 0)
        progress = 0;
    if (progress > 1)
        progress = 1;
    draw_rect(display, buffer, x, y, width, 24, 0x404048);
    if (progress > 0)
        draw_rect(display, buffer, x, y, (int)(width * progress), 24, 0xe8e8e8);
}

static void prerender_centered_text(Display *display, Buffer *buffer, Font *font,
                                    const char *text, int baseline, uint32_t color)
{
    draw_text(display, buffer, font, text,
              ((int)display->mode.hdisplay - font_text_width(font, text)) / 2,
              baseline, color);
}

PrerenderViewDirty prerender_view_dirty(const PrerenderViewState *previous,
                                        const PrerenderViewState *next, int full)
{
    PrerenderViewDirty dirty;
    const char *previous_path = previous->current_path ? previous->current_path : "";
    const char *next_path = next->current_path ? next->current_path : "";
    dirty.full = full;
    dirty.overall = full || previous->done != next->done || previous->total != next->total;
    dirty.current = full || previous->item_progress != next->item_progress ||
                    previous->frame_current != next->frame_current ||
                    previous->frame_total != next->frame_total ||
                    previous->show_frames != next->show_frames ||
                    previous->finished != next->finished ||
                    previous->thumbnail_phase != next->thumbnail_phase ||
                    previous->total != next->total;
    dirty.path = full || strcmp(previous_path, next_path) != 0;
    return dirty;
}

void prerender_view_draw(Display *display, Font *font, DepthLayer layers[2],
                         const PrerenderViewState *state, PrerenderViewDirty dirty)
{
    const int margin = display->mode.hdisplay / 12;
    const int row_height = display->mode.vdisplay / 17;
    const int top_margin = row_height / 6;
    const int bar_x = 120;
    const int bar_width = display->mode.hdisplay - 240;
    const int overall_y = 1010;
    const int current_y = 1630;
    const int width = display->mode.hdisplay;
    const int height = display->mode.vdisplay;
    Buffer *buffer = &layers[1].buffer;
    double overall = state->total ? (double)state->done / state->total : 1;
    char text[96];
    if (dirty.full) {
        int ascender;
        int descender;
        int baseline;
        memset(layers[0].buffer.pixels, 0, layers[0].buffer.size);
        memset(layers[1].buffer.pixels, 0, layers[1].buffer.size);
        FT_Set_Pixel_Sizes(font->face, 0, 64);
        ascender = font->face->size->metrics.ascender >> 6;
        descender = font->face->size->metrics.descender >> 6;
        baseline = top_margin + (row_height - (ascender - descender)) / 2 + ascender;
        draw_back_arrow(display, &layers[0].buffer, margin / 3,
                        top_margin + row_height / 2, 0x101014);
        draw_text(display, &layers[0].buffer, font, "Back",
                  margin + 24, baseline, 0x101014);
    }
    FT_Set_Pixel_Sizes(font->face, 0, 48);
    if (dirty.overall) {
        draw_rect(display, &layers[0].buffer, 0, overall_y - 100, width, 220, 0);
        draw_rect(display, buffer, 0, overall_y - 100, width, 220, 0);
        draw_text(display, buffer, font, "Overall", bar_x, overall_y - 50, 0xd8d8d8);
        prerender_bar(display, buffer, bar_x, overall_y, bar_width, overall);
        snprintf(text, sizeof(text), "%llu / %llu", (unsigned long long)state->done,
                 (unsigned long long)state->total);
        prerender_centered_text(display, buffer, font, text, overall_y + 90, 0xe8e8e8);
    }
    if (dirty.current) {
        draw_rect(display, &layers[0].buffer, 0, current_y - 100, width, 220, 0);
        draw_rect(display, buffer, 0, current_y - 100, width, 220, 0);
        draw_text(display, buffer, font,
                  state->finished ? "Complete" : state->thumbnail_phase ? "Thumbnail" : "Pre-render",
                  bar_x, current_y - 50, 0xd8d8d8);
        prerender_bar(display, buffer, bar_x, current_y, bar_width,
                      state->finished ? 1 : state->item_progress);
        if (state->show_frames && state->frame_total) {
            snprintf(text, sizeof(text), "%llu / %llu frames",
                     (unsigned long long)state->frame_current,
                     (unsigned long long)state->frame_total);
            prerender_centered_text(display, buffer, font, text, current_y + 90, 0xe8e8e8);
        }
        if (!state->total)
            prerender_centered_text(display, buffer, font, "No media found",
                                    current_y + 90, 0xe8e8e8);
    }
    if (dirty.path) {
        draw_rect(display, &layers[0].buffer, 0, height - 120, width, 120, 0);
        draw_rect(display, buffer, 0, height - 120, width, 120, 0);
        if (state->current_path)
            gallery_view_draw_text_start(display, buffer, font, state->current_path,
                                         30, height - 24, width - 60, 0xa0a0a0);
    }
}
