/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#ifndef TIGT_VIDEO_H
#define TIGT_VIDEO_H

#include "tigt.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TIGT_VIDEO_MDA = 0,
    TIGT_VIDEO_CGA = 1,
    TIGT_VIDEO_PCJR = 2,
};

enum {
    TIGT_VIDEO_TEXT = 0,
    TIGT_VIDEO_BITMAP = 1,
};

typedef struct tigt_video tigt_video;
typedef struct {
    uint32_t kind;
    uint16_t width;
    uint16_t height;
    const tigt_text_cell *cells;
    const uint32_t *pixels;
} tigt_video_frame;

/* Standalone register/VRAM decoder; no terminal or locale is needed to decode.
 * PCjr supports only the CGA-compatible view of a caller-selected 16 KiB bank.
 * Initial state is 80-column text, start address zero, video/blink disabled,
 * palette zero and CRTC index zero. Invalid adapter returns NULL/EINVAL;
 * allocation failure returns NULL/ENOMEM. Externally serialize each device. */
tigt_video *tigt_video_create(uint32_t adapter);
/* NULL is safe for destroy and write. */
void tigt_video_destroy(tigt_video *video);
/* MDA: 3B0..3B7 mirrored CRTC index/data, 3B8 mode. CGA/PCjr: 3D0..3D7
 * mirrored CRTC index/data, 3D8 mode, 3D9 color. Only CRTC 1 (width), 0C/0D
 * (word start address) are tracked; all other registers/ports are ignored.
 * Cursor, timing, gate-array/page registers and hardware readback are excluded. */
void tigt_video_write(tigt_video *video, uint16_t port, uint8_t value);
/* Requires a full borrowed aperture (MDA 4096 bytes, CGA/PCjr 16384 bytes).
 * Extra bytes are ignored; VRAM is not retained. Text is 25 rows, width 80 for
 * MDA or 40/80 for CGA/PCjr, selected by CRTC 1, not mode bit 0. CGA graphics
 * require CRTC 1 = 40 and produce 320x200 2bpp or 640x200 1bpp RGB pixels.
 * Video disable produces black output. Nonzero blink_on is the visible phase.
 * No cursor flags, borders, overscan or composite decoding are produced.
 *
 * Success returns a compact row-major frame owned by the decoder, valid until
 * the next decode/present or destruction. The inactive pointer is NULL.
 * Storage grows only as needed and is reused across frames and mode changes.
 * Invalid arguments/width return ARGUMENT, allocation failure SYSTEM;
 * errors leave the output frame unchanged. */
int tigt_video_decode(tigt_video *video, const uint8_t *vram, size_t length,
                      int blink_on, tigt_video_frame *frame);
/* Text-only decode with explicit geometry for this call, without changing CRTC
 * state. Columns 1..320, rows 1..128, at most 21440 cells; graphics mode and
 * invalid geometry/aperture return ARGUMENT. Uses the same full borrowed
 * aperture, word start/wrap, attributes, video enable and blink as decode.
 * Success returns an owned text frame of the supplied dimensions and row stride
 * columns, with the same lifetime/reuse rules. Errors leave both the output
 * descriptor and previously decoded storage unchanged. */
int tigt_video_decode_text(tigt_video *video, const uint8_t *vram, size_t length,
                           uint16_t columns, uint16_t rows, int blink_on,
                           tigt_video_frame *frame);
/* Decode, select MDA/Generic display technology, then submit through the native
 * text/bitmap API (bitmap pixel_width = 1). Does not alter overscan. Existing
 * session lifecycle and submission serialization rules apply; errors propagate. */
int tigt_video_present(tigt_video *video, const uint8_t *vram, size_t length,
                       int blink_on);

#ifdef __cplusplus
}
#endif
#endif
