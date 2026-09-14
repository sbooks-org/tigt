/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#ifndef TIGT_H
#define TIGT_H
#include <stddef.h>
#include <stdint.h>
#include "tigt_mouse.h"
#ifdef __cplusplus
extern "C" {
#endif
#define TIGT_ABI_VERSION 4u
#define TIGT_OK 0
#define TIGT_ERROR_ARGUMENT -1
#define TIGT_ERROR_TERMINAL -2
#define TIGT_ERROR_BUSY -3
#define TIGT_ERROR_SYSTEM -4
#define TIGT_ERROR_UNREPRESENTABLE -5
#define TIGT_ERROR_BACKGROUND -6
#define TIGT_ERROR_UNSUPPORTED -7
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
    uint8_t flags;
} tigt_input_event;

enum { TIGT_INPUT_PASTE = 1u << 0 };


/* Input callbacks run on the input thread for a live session, and synchronously
 * for tigt_input_feed/flush. They must return promptly and must not call lifecycle
 * functions, reenter/destroy their decoder, or unwind across this C boundary.
 * Feed/flush/destroy on a decoder must be externally serialized.
 * Standalone decoders reserve no keys. Live sessions apply the terminal host
 * signal/literal-next policy before delivering keyboard callbacks. */
typedef void (*tigt_input_callback)(const tigt_input_event *event, void *user);
typedef struct tigt_input tigt_input;
/* Returns NULL on allocation failure or a NULL callback (errno = EINVAL). */
tigt_input *tigt_input_create(tigt_input_callback callback, void *user);
/* Combined decoder on the same byte stream; either callback may be NULL.
 * OFF requires on_mouse NULL; non-OFF requires on_mouse. At least one callback
 * is required. Invalid modes or callback combinations return NULL (EINVAL).
 * AUTO decodes cell SGR here; standalone decoders never probe a terminal.
 * Both callbacks follow the serialization and lifetime rules above. */
tigt_input *tigt_input_create_with_mouse(tigt_input_callback on_input, void *input_user,
                                         tigt_mouse_callback on_mouse, void *mouse_user,
                                         uint32_t mouse_mode);
void tigt_input_feed(tigt_input *input, const uint8_t *bytes, size_t length);
/* Resolve a pending bare Escape; callers choose their input timeout. */
void tigt_input_flush(tigt_input *input);
void tigt_input_destroy(tigt_input *input);

/* Bitmap output policy. Explicit modes never silently fall back. AUTO probes
 * for sixel, then selects Blocks for UTF-8 or ASCII; it never selects iTerm2. */
typedef enum {
    TIGT_GRAPHICS_AUTO = 0,
    TIGT_GRAPHICS_BLOCKS = 1,
    TIGT_GRAPHICS_SIXEL = 2,
    TIGT_GRAPHICS_ASCII = 3,
    TIGT_GRAPHICS_ITERM2 = 4,
} tigt_graphics_mode;

typedef struct tigt_config {
    uint32_t abi_version;
    tigt_input_callback on_input; /* NULL disables semantic keyboard callbacks. */
    void *user;
    uint32_t graphics_mode; /* tigt_graphics_mode; AUTO selects available output. */
    tigt_mouse_callback on_mouse;
    void *mouse_user;
    uint32_t mouse_mode; /* tigt_mouse_mode; OFF requires on_mouse NULL. */
} tigt_config;
/* One process-wide curses session. Lifecycle calls must be serialized on the
 * owning thread. Init/resume return errors rather than exiting the application.
 * Callback storage must remain valid until shutdown returns. Signal handler
 * installation is opt-in through tigt_terminal_install_signal_handlers.
 * Stdin and stdout must be TTYs; background sessions allow only glass text.
 * Background text needs a visible cursor to supply its logical position;
 * hidden-cursor native frames return UNREPRESENTABLE rather than guessing.
 * Background bitmaps return BACKGROUND. Poll tigt_terminal_poll regularly to
 * service transitions and observe asynchronous presentation failures.
 * Suspend/shutdown join in-flight callbacks before returning. Keyboard and
 * mouse callbacks are serialized on the same input worker; neither may call
 * lifecycle functions. Both callbacks NULL with mouse OFF is output-only. */
int tigt_init(const tigt_config *config);
int tigt_resume(void);
void tigt_suspend(void);
void tigt_shutdown(void);
/* Resolved mode: AUTO before resolution and after shutdown. Resume resolves
 * again. The requested mode remains unchanged for the lifetime of a session.
 * Unavailable cell modes return TIGT_ERROR_TERMINAL at init/resume. Explicit
 * SIXEL/ITERM2 trust the caller's choice of a supporting terminal; iTerm2
 * support is not probed. */
uint32_t tigt_get_graphics_mode(void);
/* AUTO when no session exists. There is no runtime mode setter. */
uint32_t tigt_get_requested_graphics_mode(void);
/* Resolved live mouse protocol: OFF, CELLS, PIXELS, or X10, never AUTO.
 * AUTO probes pixel reporting and falls back to cell SGR. Explicit PIXELS
 * trusts the caller's terminal selection. OFF when reporting is inactive. */
uint32_t tigt_get_mouse_mode(void);
/* Sixel and iTerm2 presentation only: native frames/snapshots and Blocks/ASCII
 * are unchanged. Request columns 1..320 and a nonzero display aspect ratio
 * (aspect_width:aspect_height, each 1..65535). Defaults are 80 columns and 4:3.
 * Both native bitmap widths fill the same target rectangle. Sixel resamples
 * with nearest-neighbor scaling; iTerm2 sends a native logical-resolution PNG
 * for terminal-side scaling. The target uses ioctl pixel width/columns, then a
 * queried cell width, then queried text-area pixel width/columns. Without width
 * metrics, it uses a fixed 640-pixel target width, not a measured 80-column
 * rectangle. The target is aspect-corrected and fitted to known terminal pixel
 * bounds and 4096 pixels per axis, rounding down to at least 1. Reported sixel
 * limits apply only to sixel. Queries require owned input on the output TTY:
 * AUTO/SIXEL query geometry and sixel capabilities/limits; ITERM2 queries only
 * geometry (14t/16t). Output-only sessions use ioctl geometry without probing
 * or reading stdin.
 *
 * An initialized active OR suspended session is required; otherwise ARGUMENT.
 * Invalid arguments return ARGUMENT atomically. Changes schedule the unchanged
 * current image frame for redraw. Serialize with submissions when ordering
 * matters; updates are synchronized with rendering and may run on a producer thread.
 * Suspend/resume preserve the layout; init/shutdown restore the defaults. */
int tigt_set_image_layout(uint16_t columns, uint16_t aspect_width, uint16_t aspect_height);
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

enum {
    TIGT_DISPLAY_GENERIC = 0,
    TIGT_DISPLAY_MDA = 1,
};

typedef struct {
    uint32_t color;
    uint16_t left;
    uint16_t right;
    uint16_t top;
    uint16_t bottom;
} tigt_overscan;

/* Frame submissions copy input and may run on a producer thread. Do not race
 * lifecycle operations. Submissions, display technology and overscan access
 * require an active, unsuspended session (otherwise TIGT_ERROR_BUSY). Invalid
 * arguments return TIGT_ERROR_ARGUMENT without changing stored state.
 *
 * RGB values use 0x00RRGGBB. Text and overscan require a zero high byte;
 * bitmap pixels ignore the high byte for compatibility with native buffers.
 * Native bitmap RGB is retained exactly. Sixel preserves up to 256 colours
 * within its percentage-channel precision and quantizes larger colour sets;
 * iTerm2 normalizes to a 320x200 RGB PNG (640-wide pairs averaged per channel,
 * rounded up; 160-wide pixels duplicated). Native snapshots remain unchanged.
 * Cell backends approximate colours using terminal capabilities. Identical resolved bitmap
 * submissions do not schedule redraws: high bytes and row padding are ignored,
 * but all backing pixels, dimensions, pixel_width and text/bitmap transitions
 * participate. Layout changes, resize and resume still invalidate the display.
 *
 * Bitmap stride is in pixels. Width320/640, height200, pixel_width1/2
 * (backing pixels per terminal logical pixel). Blocks/ASCII may use terminal
 * defaults only with at most three distinct source RGB values, all black,
 * white, or neutral grey with R=G=B in 129..254. All source pixels participate,
 * including horizontally skipped pixels. Grey maps to regular foreground and
 * white to intense foreground. Sixel and iTerm2 always use explicit RGB, never
 * theme foreground/bold colours.
 * 160x200 can be expanded horizontally by the caller, as for PCjr video. */
int tigt_present_bitmap(const uint32_t *pixels, uint16_t width, uint16_t height,
                        uint16_t stride, uint8_t pixel_width);
/* Indexed counterpart, with the same dimensions/stride/pixel-width rules.
 * NULL palette with palette_size 0 selects the standard IBM16 palette and
 * requires indices 0..15. Otherwise supply 1..256 exact 0x00RRGGBB entries;
 * high bytes must be zero. Every visible source index must be in range.
 * Indices and palette are copied/resolved before return; invalid input leaves
 * stored state unchanged. Native snapshots preserve resolved RGB exactly. */
int tigt_present_indexed_bitmap(const uint8_t *indices, uint16_t width, uint16_t height,
                                uint16_t stride, uint8_t pixel_width,
                                const uint32_t *palette, uint16_t palette_size);
/* Text stride is in cells and must be >= columns. Columns1..320, rows1..128,
 * with at most 21440 visible cells; stride padding is ignored. Each codepoint
 * must be a Unicode scalar occupying exactly one terminal column according
 * to wcwidth in the session's LC_CTYPE locale: controls, nonspacing combining
 * marks, zero-width and wide characters are rejected. Colours are explicit RGB.
 * Only the flags above are accepted. At most one cell may carry CURSOR,
 * meaning a currently visible cursor; tigt shows a steady terminal underline
 * cursor there, with no host-generated blink. Callers resolve display enable,
 * text/cursor blink and hardware attributes before submitting. MDA technology
 * can suppress only the initial rendered cursor; native snapshots are unchanged. */
int tigt_present_text(const tigt_text_cell *cells, uint16_t columns, uint16_t rows,
                      uint16_t stride);
/* CP437 display glyphs, including graphical control characters and the house
 * at 0x7f. The blank display characters 0x00 and 0xff map to U+0020. */
uint32_t tigt_cp437_codepoint(uint8_t character);
/* Select a terminal presentation policy, not a hardware decoder. Generic (the
 * initial default) honors every submitted cursor. MDA hides the terminal cursor
 * until a submitted text cell has distinct resolved foreground/background RGB
 * and a nonblank, non-whitespace glyph or UNDERLINE. CURSOR alone is not output.
 * Every accepted text submission participates, even if never rendered. Once
 * output appears, clearing the screen does not rearm suppression. Bitmap frames
 * neither release nor rearm it. Native frame contents/flags are never changed.
 *
 * Repeating the same hint is a no-op. An actual technology change resets the
 * latch and schedules the current frame for redraw, even without a new frame.
 * Set before submitting the new technology's frame; serialize with submissions
 * when ordering matters. Suspend/resume preserve the hint and latch;
 * init/shutdown reset to Generic. Unsupported values return ARGUMENT. */
int tigt_set_display_technology(uint32_t technology);
/* Stored metadata only: overscan is not drawn. Dimensions are native backing
 * pixels BEFORE host vertical line doubling; color uses the RGB format above.
 * Set/get copy under the frame mutex. Metadata is independent of bitmap/text
 * submissions: callers serialize metadata and frames when ordering matters.
 * Init/shutdown reset it to all zeroes; suspend/resume preserve it. */
int tigt_set_overscan(const tigt_overscan *overscan);
int tigt_get_overscan(tigt_overscan *overscan);

typedef enum {
    TIGT_SNAPSHOT_PNG = 0,
    TIGT_SNAPSHOT_UTF8 = 1,
    TIGT_SNAPSHOT_ASCII = 2,
    TIGT_SNAPSHOT_CP437 = 3,
    TIGT_SNAPSHOT_ANSI = 4,
    TIGT_SNAPSHOT_CELLS = 5,
    TIGT_SNAPSHOT_ATTRIBUTES = 6,
} tigt_snapshot_format;

/* Copy the native frame and overscan under the frame mutex, then synchronously
 * encode to a borrowed writable fd; never closes it. May run on a producer or
 * callback thread, including concurrently with shutdown. An inactive session
 * or missing frame returns BUSY. Invalid format/fd returns ARGUMENT, allocation
 * or I/O failure SYSTEM. A blocking fd can block this calling thread.
 * The caller must serialize access to the fd and its duplicates until return.
 * On macOS, SIGPIPE suppression is temporarily set on the shared open-file
 * description and restored afterwards; the process signal disposition is untouched.
 *
 * PNG is RGB, without overscan borders or host scaling. Bitmap dimensions are
 * width/pixel_width by 200, sampling the first backing pixel of each logical
 * pixel and ignoring its high byte. Text uses the caller-supplied CP437 font,
 * width columns*8, height rows*font_height; UNDERLINE and CURSOR draw the bottom
 * scanline in foreground. Missing font or an unmappable glyph returns ARGUMENT.
 *
 * UTF8/ASCII/CP437 are text-only, one newline per row, trailing U+0020 cells
 * trimmed. ASCII and CP437 replace unrepresentable codepoints with '?'; CP437
 * uses display glyph mapping, canonical space 0x20. ANSI is text-only UTF-8,
 * trailing spaces trimmed, with explicit 24-bit SGR colors/underline, reset
 * and newline after every row, no cursor positioning; CURSOR projects to underline.
 *
 * CELLS is text-only raw row-major pairs: CP437 glyph, then fg|(bg<<4).
 * Colors use nearest standard IBM16 RGB by squared distance, ties to the lower
 * index. This projection is lossy: no original hardware attributes are stored.
 * ATTRIBUTES is lossless text metadata JSON (bitmap metadata omits cells):
 * {schema_version:1,kind:"text"|"bitmap",
 *  dimensions:{width,height,pixel_width},
 *  overscan:{color,left,right,top,bottom},
 *  columns,rows,cells:[{codepoint,foreground,background,flags,
 *                      legacy:{glyph,attribute}}]}.
 * All numeric values are decimal integers; cells are row-major. columns/rows
 * and cells exist only for text, whose dimensions are cells and pixel_width=1.
 * Bitmap dimensions are logical pixels; RGB fields retain submitted values. */
int tigt_snapshot_write_fd(int fd, uint32_t format);

/* Copy 256 glyphs, each height bytes, MSB-left, width 8, height 1..32.
 * NULL with height 0 clears; other invalid combinations return ARGUMENT.
 * Requires an active session. Font survives suspend, not shutdown or new init.
 * The application owns font licensing; tigt supplies no font or ROM. */
int tigt_snapshot_set_font(const uint8_t *font, uint16_t height);

/* Opt-in asynchronous snapshots on SIGUSR1 or SIGUSR2. Copies path; accepts
 * regular files (created mode 0600 if absent) and existing named FIFOs, not
 * symlinks or other file kinds. Only a default signal disposition can be taken;
 * existing handlers or SIG_IGN return BUSY. No signal is installed normally.
 * Signal handler only flags work; repeated signals may coalesce. A worker
 * writes outside signal context. Regular files are truncated for each request.
 * FIFO opens are nonblocking: no reader fails immediately, a stalled reader
 * fails within 250ms; failures can leave partial output. SIGPIPE is not raised
 * by snapshot writes. Disable/shutdown joins this worker, restores the prior
 * disposition unless the application replaced ours, and frees the copied path.
 *
 * signal_number=0 disables (format/path ignored), even when inactive. Otherwise
 * requires an active session and a valid format/path. Configuration is serialized
 * internally; like other lifecycle calls it must not run inside a signal handler.
 * Suspend preserves configuration; requests while suspended complete with BUSY.
 * Serialize application sigaction changes with configure/disable/shutdown.
 * At init only, TIGT_SNAPSHOT_PATH opts in, TIGT_SNAPSHOT_FORMAT selects
 * png/utf8/ascii/cp437/ansi/cells/attributes (default png), and
 * TIGT_SNAPSHOT_SIGNAL selects USR1/USR2 (default USR1). Invalid opt-in settings
 * fail init rather than silently disabling capture. */
int tigt_snapshot_configure(int signal_number, uint32_t format, const char *path);

/* Last asynchronous completion, including failures. Sequence starts at zero
 * on init, increments once per completed request; initial result is OK.
 * Disable/shutdown preserve status until a new init. Both pointers required.
 * Synchronous write_fd calls do not affect this status. */
int tigt_snapshot_status(uint64_t *sequence, int *result);
#ifdef __cplusplus
}
#endif
#endif
