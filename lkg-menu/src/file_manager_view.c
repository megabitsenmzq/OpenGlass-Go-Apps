#include "file_manager_view.h"

#include <qrencode.h>
#include <stdio.h>
#include <string.h>

void file_manager_view_draw(Display *display, Font *font, const DepthEffect *depth,
                            DepthLayer layers[2], const char *ip_address)
{
    const int margin = display->mode.hdisplay / 12;
    const int row_height = display->mode.vdisplay / 17;
    const int bar_height = row_height - 8;
    const int top_margin = row_height / 6;
    int baseline;
    Buffer *content = &layers[1].buffer;
    char url[192];
    QRcode *code;
    FT_Set_Pixel_Sizes(font->face, 0, 64);
    baseline = font_centered_baseline(font, top_margin, bar_height);
    draw_back_arrow(display, &layers[0].buffer, margin / 3,
                    top_margin + bar_height / 2, 0x101014);
    if (depth->stereo)
        draw_text_bold(display, &layers[0].buffer, font, "Back", margin + 24, baseline, 0x101014);
    else
        draw_text(display, &layers[0].buffer, font, "Back", margin + 24, baseline, 0x101014);
    if (!ip_address || !ip_address[0] || strcmp(ip_address, "Unavailable") == 0) {
        const char *message = "Please connect to the internet";
        FT_Set_Pixel_Sizes(font->face, 0, 48);
        draw_text(display, content, font, message,
                  ((int)display->mode.hdisplay - font_text_width(font, message)) / 2,
                  display->mode.vdisplay / 2, 0xe8e8e8);
        FT_Set_Pixel_Sizes(font->face, 0, 64);
        return;
    }
    snprintf(url, sizeof(url), "http://%s:5000/", ip_address);
    code = QRcode_encodeString(url, 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    if (code) {
        const int quiet = 4;
        int scale = 720 / (code->width + quiet * 2);
        int size;
        int start_x;
        int start_y;
        int row;
        if (scale < 1)
            scale = 1;
        size = (code->width + quiet * 2) * scale;
        start_x = ((int)display->mode.hdisplay - size) / 2;
        start_y = ((int)display->mode.vdisplay - size - 120) / 2;
        draw_rect(display, content, start_x, start_y, size, size, 0xffffff);
        for (row = 0; row < code->width; ++row) {
            int column;
            for (column = 0; column < code->width; ++column) {
                if (code->data[row * code->width + column] & 1)
                    draw_rect(display, content,
                              start_x + (column + quiet) * scale,
                              start_y + (row + quiet) * scale,
                              scale, scale, 0x101014);
            }
        }
        FT_Set_Pixel_Sizes(font->face, 0, 48);
        draw_text(display, content, font, url,
                  ((int)display->mode.hdisplay - font_text_width(font, url)) / 2,
                  start_y + size + 100, 0xe8e8e8);
        FT_Set_Pixel_Sizes(font->face, 0, 64);
        QRcode_free(code);
    } else {
        draw_text(display, content, font, "Unable to generate QR code",
                  margin, display->mode.vdisplay / 2, 0xe8e8e8);
    }
}
