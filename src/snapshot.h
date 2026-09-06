/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#ifndef TIGT_SNAPSHOT_INTERNAL_H
#define TIGT_SNAPSHOT_INTERNAL_H

#include "tigt.h"
#include <stdbool.h>

#define TIGT_MAX_TEXT_CELLS 21440

typedef struct {
    bool bitmap;
    uint16_t width;
    uint16_t height;
    uint8_t pixel_width;
    tigt_overscan overscan;
    union {
        uint32_t pixels[640 * 200];
        tigt_text_cell cells[TIGT_MAX_TEXT_CELLS];
    } content;
} tigt_native_frame;

/* Renderer supplies a coherent copy, with no borrowed application storage. */
int tigt_snapshot_capture(tigt_native_frame *frame);
void tigt_snapshot_session_reset(void);
void tigt_snapshot_session_start(void);
void tigt_snapshot_session_stop(bool shutdown);
int tigt_snapshot_environment(void);

#endif
