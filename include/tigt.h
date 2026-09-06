/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#ifndef TIGT_H
#define TIGT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define TIGT_ABI_VERSION 2u
#define TIGT_OK 0
#define TIGT_ERROR_ARGUMENT -1
#define TIGT_ERROR_TERMINAL -2
#define TIGT_ERROR_BUSY -3
#define TIGT_ERROR_SYSTEM -4
typedef enum {
    TIGT_PRESS = 0,
    TIGT_REPEAT = 1,
    TIGT_RELEASE = 2,
} tigt_input_kind;

enum {
    TIGT_MOD_SHIFT = 1u << 0,
    TIGT_MOD_CONTROL = 1u << 1,
    TIGT_MOD_ALT = 1u << 2,
    TIGT_MOD_SUPER = 1u << 3,
};

typedef enum {
    TIGT_KEY_CHAR = 0,
    TIGT_KEY_BACKSPACE = 1,
    TIGT_KEY_DELETE = 2,
    TIGT_KEY_INSERT = 3,
    TIGT_KEY_ENTER = 4,
    TIGT_KEY_LEFT = 5,
    TIGT_KEY_RIGHT = 6,
    TIGT_KEY_UP = 7,
    TIGT_KEY_DOWN = 8,
    TIGT_KEY_HOME = 9,
    TIGT_KEY_END = 10,
    TIGT_KEY_PAGE_UP = 11,
    TIGT_KEY_PAGE_DOWN = 12,
    TIGT_KEY_PRINT_SCREEN = 13,
    TIGT_KEY_PAUSE = 14,
    TIGT_KEY_SCROLL_LOCK = 15,
    TIGT_KEY_NUM_LOCK = 16,
    TIGT_KEY_KEYPAD_BEGIN = 17,
    TIGT_KEY_ESCAPE = 18,
    TIGT_KEY_NULL = 19,
    TIGT_KEY_FUNCTION = 20,
    TIGT_KEY_MODIFIER = 21,
} tigt_key_kind;

typedef enum {
    TIGT_LEFT_SHIFT = 0,
    TIGT_RIGHT_SHIFT = 1,
    TIGT_LEFT_CONTROL = 2,
    TIGT_RIGHT_CONTROL = 3,
    TIGT_LEFT_ALT = 4,
    TIGT_RIGHT_ALT = 5,
    TIGT_LEFT_SUPER = 6,
    TIGT_RIGHT_SUPER = 7,
    TIGT_OTHER_MODIFIER = 8,
} tigt_modifier_key;

typedef struct {
    uint32_t kind;
    uint32_t value;
    uint32_t character;
} tigt_input_key;

typedef struct {
    tigt_input_key key;
    uint8_t modifiers;
    uint8_t kind;
} tigt_input_event;


/* Input callbacks run on the input thread for a live session, and synchronously
 * for tigt_input_feed/flush. They must return promptly and must not call lifecycle
 * functions, reenter/destroy their decoder, or unwind across this C boundary.
 * Feed/flush/destroy on a decoder must be externally serialized.
 * No key is reserved: applications decide what Ctrl+C and Ctrl+Z mean. */
typedef void (*tigt_input_callback)(const tigt_input_event *event, void *user);
typedef struct tigt_input tigt_input;
/* Returns NULL on allocation failure or a NULL callback (errno = EINVAL). */
tigt_input *tigt_input_create(tigt_input_callback callback, void *user);
void tigt_input_feed(tigt_input *input, const uint8_t *bytes, size_t length);
/* Resolve a pending bare Escape; callers choose their input timeout. */
void tigt_input_flush(tigt_input *input);
void tigt_input_destroy(tigt_input *input);

typedef struct tigt_config {
    uint32_t abi_version;
    tigt_input_callback on_input; /* NULL makes this an output-only session. */
    void *user;
} tigt_config;
/* One process-wide curses session. Lifecycle calls must be serialized on the
 * owning thread. Init/resume return errors rather than exiting the application.
 * Callback storage must remain valid until shutdown returns. SIGINT/SIGTSTP
 * disposition belongs to the application; suspend restores shell state.
 * Stdin and stdout must be TTYs, including for output-only sessions.
 * Suspend/shutdown join in-flight callbacks before returning. */
int tigt_init(const tigt_config *config);
int tigt_resume(void);
void tigt_suspend(void);
void tigt_shutdown(void);
typedef struct {
    uint32_t codepoint;
    uint32_t foreground;
    uint32_t background;
    uint32_t flags;
} tigt_text_cell;

enum {
    TIGT_TEXT_UNDERLINE = 1u << 0,
    TIGT_TEXT_CURSOR = 1u << 1,
};

typedef struct {
    uint32_t color;
    uint16_t left;
    uint16_t right;
    uint16_t top;
    uint16_t bottom;
} tigt_overscan;

/* Frame submissions copy input and may run on a producer thread. Do not race
 * lifecycle operations. Submissions and overscan access require an active,
 * unsuspended session (otherwise TIGT_ERROR_BUSY). Invalid arguments return
 * TIGT_ERROR_ARGUMENT without changing the stored frame or metadata.
 *
 * RGB values use 0x00RRGGBB. Text and overscan require a zero high byte;
 * bitmap pixels ignore the high byte for compatibility with native buffers.
 * Rendered colours are quantized to the existing 16-colour PC display palette,
 * then approximated by the terminal's available colours.
 *
 * Bitmap stride is in pixels. Width320/640, height200, pixel_width1/2
 * (backing pixels per terminal logical pixel). The existing bitmap policy
 * uses terminal defaults for black/white when its frame palette permits.
 * 160x200 can be expanded horizontally by the caller, as for PCjr video. */
int tigt_present_bitmap(const uint32_t *pixels, uint16_t width, uint16_t height,
                        uint16_t stride, uint8_t pixel_width);
/* Text stride is in cells and must be >= columns. Columns1..320, rows1..128,
 * with at most 21440 visible cells; stride padding is ignored. Each codepoint
 * must be a Unicode scalar occupying exactly one terminal column according
 * to wcwidth in the session's LC_CTYPE locale: controls, nonspacing combining
 * marks, zero-width and wide characters are rejected. Colours are explicit RGB.
 * Only the flags above are accepted. At most one cell may carry CURSOR,
 * meaning a currently visible cursor; tigt shows a steady terminal underline
 * cursor there, with no host-generated blink. Callers resolve display enable,
 * text/cursor blink and hardware attributes before submitting. */
int tigt_present_text(const tigt_text_cell *cells, uint16_t columns, uint16_t rows,
                      uint16_t stride);
/* CP437 display glyphs, including graphical control characters and the house
 * at 0x7f. The blank display characters 0x00 and 0xff map to U+0020. */
uint32_t tigt_cp437_codepoint(uint8_t character);
/* Stored metadata only: overscan is not drawn. Dimensions are native backing
 * pixels BEFORE host vertical line doubling; color uses the RGB format above.
 * Set/get copy under the frame mutex. Metadata is independent of bitmap/text
 * submissions: callers serialize metadata and frames when ordering matters.
 * Init/shutdown reset it to all zeroes; suspend/resume preserve it. */
int tigt_set_overscan(const tigt_overscan *overscan);
int tigt_get_overscan(tigt_overscan *overscan);
#ifdef __cplusplus
}
#endif
#endif
