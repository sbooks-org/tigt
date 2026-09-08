/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#include "tigt_video.h"
#include "palette.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct tigt_video {
    uint32_t adapter;
    uint16_t start;
    uint8_t index;
    uint8_t columns;
    uint8_t mode;
    uint8_t color;
    void *buffer;
    size_t capacity;
};

tigt_video *
tigt_video_create(uint32_t adapter)
{
    if (adapter != TIGT_VIDEO_MDA && adapter != TIGT_VIDEO_CGA && adapter != TIGT_VIDEO_PCJR) {
        errno = EINVAL;
        return NULL;
    }
    tigt_video *video = calloc(1, sizeof(*video));
    if (video == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    video->adapter = adapter;
    video->columns = 80;
    return video;
}

void
tigt_video_destroy(tigt_video *video)
{
    if (video != NULL) {
        free(video->buffer);
        free(video);
    }
}

void
tigt_video_write(tigt_video *video, uint16_t port, uint8_t value)
{
    if (video == NULL)
        return;
    const uint16_t base = video->adapter == TIGT_VIDEO_MDA ? 0x3b0 : 0x3d0;
    if (port >= base && port <= base + 7) {
        if (!(port & 1)) {
            video->index = value & 0x1f;
        } else {
            switch (video->index) {
            case 1:
                video->columns = value;
                break;
            case 0x0c:
                video->start = ((value & 0x3f) << 8) | (video->start & 0xff);
                break;
            case 0x0d:
                video->start = (video->start & 0x3f00) | value;
                break;
            default:
                break;
            }
        }
    } else if (port == base + 8) {
        video->mode = value;
    } else if (video->adapter != TIGT_VIDEO_MDA && port == base + 9) {
        video->color = value;
    }
}

static void
decode_text(const tigt_video *video, const uint8_t *vram, bool blink_on,
            tigt_text_cell *cells)
{
    const size_t count = (size_t) video->columns * 25;
    if (!(video->mode & 0x08)) {
        for (size_t i = 0; i < count; i++)
            cells[i] = (tigt_text_cell) { .codepoint = ' ' };
        return;
    }
    const bool mono = video->adapter == TIGT_VIDEO_MDA;
    const size_t mask = mono ? 0x0fff : 0x3fff;
    const bool blink = (video->mode & 0x20) != 0;
    for (size_t i = 0; i < count; i++) {
        const size_t address = (((size_t) video->start + i) * 2) & mask;
        const uint8_t attr = vram[address + 1];
        uint8_t foreground;
        uint8_t background;
        uint32_t flags = 0;
        if (mono) {
            foreground = (attr & 8) ? 15 : 7;
            background = 0;
            /* Bit 7 never adds MDA colors, even when blink is disabled. */
            switch (attr & 0x7f) {
            case 0x70:
                foreground = 0;
                background = 15;
                break;
            case 0x78:
                foreground = 7;
                background = 15;
                break;
            case 0x00:
            case 0x08:
                foreground = 0;
                break;
            default:
                break;
            }
            if ((attr & 7) == 1)
                flags = TIGT_TEXT_UNDERLINE;
        } else {
            foreground = attr & 15;
            background = (attr >> 4) & (blink ? 7 : 15);
        }
        if (blink && (attr & 0x80) && !blink_on)
            foreground = background;
        cells[i] = (tigt_text_cell) {
            .codepoint = tigt_cp437_codepoint(vram[address]),
            .foreground = tigt_ibm16_palette[foreground],
            .background = tigt_ibm16_palette[background],
            .flags = flags
        };
    }
}

static void
decode_bitmap(const tigt_video *video, const uint8_t *vram, uint16_t width,
              uint32_t *pixels)
{
    if (!(video->mode & 0x08)) {
        memset(pixels, 0, (size_t) width * 200 * sizeof(*pixels));
        return;
    }
    uint32_t colors[4];
    if (width == 640) {
        colors[0] = 0;
        colors[1] = tigt_ibm16_palette[video->color & 15];
    } else {
        const uint8_t intensity = (video->color & 0x10) ? 8 : 0;
        colors[0] = tigt_ibm16_palette[video->color & 15];
        if (video->mode & 0x04) {
            colors[1] = tigt_ibm16_palette[intensity + 3];
            colors[2] = tigt_ibm16_palette[intensity + 4];
            colors[3] = tigt_ibm16_palette[intensity + 7];
        } else if (video->color & 0x20) {
            colors[1] = tigt_ibm16_palette[intensity + 3];
            colors[2] = tigt_ibm16_palette[intensity + 5];
            colors[3] = tigt_ibm16_palette[intensity + 7];
        } else {
            colors[1] = tigt_ibm16_palette[intensity + 2];
            colors[2] = tigt_ibm16_palette[intensity + 4];
            colors[3] = tigt_ibm16_palette[intensity + 6];
        }
    }
    for (size_t y = 0; y < 200; y++) {
        const size_t row = (size_t) video->start * 2 + (y / 2) * 80;
        const size_t bank = (y & 1) * 0x2000;
        for (size_t byte_x = 0; byte_x < 80; byte_x++) {
            const uint8_t value = vram[((row + byte_x) & 0x1fff) + bank];
            if (width == 640) {
                for (int shift = 7; shift >= 0; shift--)
                    *pixels++ = colors[(value >> shift) & 1];
            } else {
                for (int shift = 6; shift >= 0; shift -= 2)
                    *pixels++ = colors[(value >> shift) & 3];
            }
        }
    }
}

int
tigt_video_decode(tigt_video *video, const uint8_t *vram, size_t length,
                  int blink_on, tigt_video_frame *frame)
{
    if (video == NULL || vram == NULL || frame == NULL)
        return TIGT_ERROR_ARGUMENT;
    const bool mono = video->adapter == TIGT_VIDEO_MDA;
    if (length < (mono ? 4096u : 16384u))
        return TIGT_ERROR_ARGUMENT;
    const bool graphics = !mono && (video->mode & 0x02);
    if (graphics ? video->columns != 40 :
        video->columns != 80 && (mono || video->columns != 40))
        return TIGT_ERROR_ARGUMENT;
    const uint16_t width = graphics ? ((video->mode & 0x10) ? 640 : 320) : video->columns;
    const uint16_t height = graphics ? 200 : 25;
    const size_t bytes = (size_t) width * height *
                         (graphics ? sizeof(uint32_t) : sizeof(tigt_text_cell));
    if (bytes > video->capacity) {
        void *buffer = realloc(video->buffer, bytes);
        if (buffer == NULL) {
            errno = ENOMEM;
            return TIGT_ERROR_SYSTEM;
        }
        video->buffer = buffer;
        video->capacity = bytes;
    }
    if (graphics)
        decode_bitmap(video, vram, width, video->buffer);
    else
        decode_text(video, vram, blink_on != 0, video->buffer);
    *frame = (tigt_video_frame) {
        .kind = graphics ? TIGT_VIDEO_BITMAP : TIGT_VIDEO_TEXT,
        .width = width,
        .height = height,
        .cells = graphics ? NULL : video->buffer,
        .pixels = graphics ? video->buffer : NULL
    };
    return TIGT_OK;
}

int
tigt_video_present(tigt_video *video, const uint8_t *vram, size_t length, int blink_on)
{
    tigt_video_frame frame;
    int result = tigt_video_decode(video, vram, length, blink_on, &frame);
    if (result != TIGT_OK)
        return result;
    result = tigt_set_display_technology(video->adapter == TIGT_VIDEO_MDA ?
                                         TIGT_DISPLAY_MDA : TIGT_DISPLAY_GENERIC);
    if (result != TIGT_OK)
        return result;
    if (frame.kind == TIGT_VIDEO_TEXT)
        return tigt_present_text(frame.cells, frame.width, frame.height, frame.width);
    return tigt_present_bitmap(frame.pixels, frame.width, frame.height, frame.width, 1);
}
