/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#ifndef TIGT_MOUSE_H
#define TIGT_MOUSE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TIGT_MOUSE_OFF = 0,
    TIGT_MOUSE_AUTO = 1,
    TIGT_MOUSE_CELLS = 2,
    TIGT_MOUSE_PIXELS = 3,
    TIGT_MOUSE_X10 = 4
} tigt_mouse_mode;

typedef enum {
    TIGT_MOUSE_MOVE = 0,
    TIGT_MOUSE_DOWN = 1,
    TIGT_MOUSE_UP = 2,
    TIGT_MOUSE_SCROLL = 3,
    TIGT_MOUSE_LEAVE = 4
} tigt_mouse_kind;

typedef enum {
    TIGT_MOUSE_BUTTON_NONE = 0,
    TIGT_MOUSE_BUTTON_LEFT = 1,
    TIGT_MOUSE_BUTTON_MIDDLE = 2,
    TIGT_MOUSE_BUTTON_RIGHT = 3
} tigt_mouse_button;

enum {
    TIGT_MOUSE_LEFT = 1u << 0,
    TIGT_MOUSE_MIDDLE = 1u << 1,
    TIGT_MOUSE_RIGHT = 1u << 2,
    TIGT_MOUSE_POSITION_VALID = 1u << 0,
    TIGT_MOUSE_FRAME_VALID = 1u << 1,
    TIGT_MOUSE_INSIDE_FRAME = 1u << 2,
    TIGT_MOUSE_SYNTHETIC = 1u << 3
};

typedef enum {
    TIGT_MOUSE_COORD_CELLS = 0,
    TIGT_MOUSE_COORD_PIXELS = 1
} tigt_mouse_coordinates;

/* One mouse transition. x/y are zero-based terminal cells or integer pixels;
 * position validity is explicit. frame_x/y, when FRAME_VALID is set, are in
 * the displayed native bitmap's logical pixels or text frame's cells, with
 * fractional values retained. Coarse cell reports use the cell centre. Raw
 * terminal pixel reports do not imply physical sub-pixel precision.
 * Frame coordinates are not clamped; INSIDE_FRAME indicates hit testing.
 * Offscreen/unknown geometry never fabricates a valid frame position.
 * Button masks describe held buttons; scroll_x/y are wheel steps, positive
 * right/down. Modifiers use TIGT_MOD_* from tigt.h. MOVE precedes each
 * position-bearing DOWN/UP/SCROLL, even if no separate motion was reported.
 * X10 has no release reports: its press-only clicks emit a synthetic UP.
 * LEAVE is a real Kitty pixel-mode leave report, not keyboard focus loss;
 * its coordinate fields are invalid. No timeout is interpreted as departure.
 * These protocols do not identify touch contacts or multitouch; mouse-emulated
 * taps remain mouse clicks, not invented touch events.
 */
typedef struct {
    uint32_t kind;
    uint32_t button;
    uint32_t buttons;
    uint32_t modifiers;
    uint32_t coordinates;
    uint32_t flags;
    int32_t x;
    int32_t y;
    int32_t scroll_x;
    int32_t scroll_y;
    double frame_x;
    double frame_y;
} tigt_mouse_event;

typedef void (*tigt_mouse_callback)(const tigt_mouse_event *event, void *user);
#ifdef __cplusplus
}
#endif
#endif
