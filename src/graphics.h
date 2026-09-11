/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#ifndef TIGT_GRAPHICS_H
#define TIGT_GRAPHICS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct tigt_ascii tigt_ascii;

typedef struct {
    uint8_t glyph;
    uint8_t foreground;
    uint8_t background;
} tigt_ascii_cell;

/* Availability describes the build, not terminal capabilities. Creation owns an
 * offscreen canvas only, never a display, input device or terminal. NULL means
 * allocation/library failure (errno), or ENOSYS without libcaca. Destruction
 * accepts NULL. Instances and their borrowed results require serialized access.
 */
bool tigt_ascii_available(void);
tigt_ascii *tigt_ascii_create(void);
void tigt_ascii_destroy(tigt_ascii *ascii);

/* Source is tightly packed 0x00RRGGBB pixels (high byte ignored), width 1..640,
 * height 1..200 and pixel_width 1 or 2; width must be divisible by pixel_width.
 * Destination dimensions are 1..4096 each. Conversion samples the first backing
 * pixel of each logical pixel, then fills the caller's destination rectangle.
 * themed requires at most three distinct source colors, each black, white,
 * or neutral gray with R=G=B in 129..254. These grays are normalized to IBM7
 * before conversion; returned colors are restricted to {0,7,15}, retaining
 * normal versus bright emphasis. Otherwise returned colors are IBM16 indices
 * 0..15 approximating the original RGB input through libcaca. Glyphs are
 * printable 7-bit ASCII, including space. Theme eligibility is never inferred
 * from colors quantized by libcaca.
 *
 * On TIGT_OK, *cells borrows columns*rows row-major cells until the next render
 * or destruction. Canvas, dither and result storage are reused across frames.
 * Failure clears *cells: TIGT_ERROR_ARGUMENT for invalid arguments/theme input,
 * TIGT_ERROR_SYSTEM for allocation/library failure, TIGT_ERROR_TERMINAL in a
 * build without libcaca. libcaca may allocate its own per-render scratch.
 */
int tigt_ascii_render(tigt_ascii *ascii, const uint32_t *pixels,
                      uint16_t width, uint16_t height, uint8_t pixel_width,
                      uint16_t columns, uint16_t rows, bool themed,
                      const tigt_ascii_cell **cells);

/* Write one complete 7-bit DCS sixel image, with an explicit RGB palette,
 * square raster pixels and full coverage (including black). No cursor or
 * terminal mode changes. Bounds and source layout match the ASCII contract;
 * output dimensions are raster pixels, not cells. Nearest-neighbor resampling
 * fills the caller's aspect-corrected rectangle, including its final partial
 * six-pixel band. Sources with <=256 distinct colors preserve their RGB values
 * to sixel's integer-percent channel precision (rounded to nearest percent).
 * Larger palettes use deterministic population-weighted median-cut over a
 * 5-bit/channel histogram, with centroids averaged from the original 8-bit
 * samples, reducing to at most 256 colors. No theme substitution is performed.
 * Calls must be serialized: bounded static workspace is reused across frames,
 * without per-frame allocation by this encoder.
 *
 * The stream remains owned by the caller and is flushed on success. Returns
 * TIGT_OK, TIGT_ERROR_ARGUMENT (before writing anything), or TIGT_ERROR_SYSTEM
 * on stream failure. An I/O failure can leave an incomplete DCS on the stream.
 */
int tigt_sixel_write(FILE *output, const uint32_t *pixels,
                     uint16_t width, uint16_t height, uint8_t pixel_width,
                     uint16_t output_width, uint16_t output_height);

#endif
