/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "tigt.h"
#include "snapshot.h"
#include "graphics.h"
#include "input.h"
#include "palette.h"
#include "tigt_presenter.h"
#include "terminal_internal.h"

#include <curses.h>
#include <errno.h>
#include <locale.h>
#include <langinfo.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#define TERMINAL_MAX_COLUMNS 320
#define TERMINAL_MAX_ROWS 128
#define TERMINAL_MAX_CELLS TIGT_MAX_TEXT_CELLS

static pthread_mutex_t renderer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t renderer_thread;
static pthread_t input_thread;
static tigt_text_cell renderer_text_cells[TERMINAL_MAX_CELLS];
static uint16_t renderer_text_columns;
static uint16_t renderer_text_rows;
static tigt_overscan renderer_overscan;
static uint32_t renderer_display_technology;
static bool renderer_text_output_seen;
static uint32_t renderer_bitmap_pixels[640 * 200];
static uint8_t renderer_bitmap_indices[640 * 200];
static uint16_t renderer_bitmap_width;
static uint8_t renderer_bitmap_pixel_width;
static bool renderer_bitmap_valid;
static bool renderer_has_frame;
static atomic_bool renderer_active;
static atomic_bool renderer_running;
static atomic_bool input_running;
static bool renderer_initialized;
static bool renderer_thread_created;
static bool input_thread_created;
static SCREEN *renderer_screen;
static tigt_config renderer_config;
static tigt_input *renderer_input;
static bool terminal_captured;
static bool session_requested;
static tigt_presenter *background_presenter;
static tigt_input_event held_keys[256];
static unsigned held_key_count;
static unsigned native_epoch;
static uint32_t held_mouse_buttons;
static void terminal_report(const char *parameters, uint8_t final, void *user);
static void terminal_transition(unsigned changes);
/* Negotiated protocol and published display geometry are protected by renderer_mutex. */
static uint32_t terminal_mouse_mode;
static bool terminal_mouse_pixels;
static struct {
    bool valid, pixels;
    uint16_t width, height, columns, rows;
    double scale_x, scale_y, cell_width, cell_height;
} pointer_projection;
static uint64_t renderer_frame_serial;
static uint64_t rendered_frame_serial;
static bool terminal_default_colors_available;
static uint32_t renderer_graphics_mode;
static tigt_ascii *renderer_ascii;
static bool rendered_image;
static struct winsize rendered_window;
typedef struct {
    uint16_t columns, aspect_width, aspect_height;
} image_layout_t;
static const image_layout_t default_image_layout = { 80, 4, 3 };
static image_layout_t renderer_image_layout;
/* Renderer-owned dimensions, calculated with the frame under renderer_mutex. */
static uint16_t rendered_image_width, rendered_image_height;
static struct {
    bool pending, sixel;
    uint16_t pixel_width, pixel_height, cell_width, cell_height;
    uint16_t columns, rows;
    uint16_t max_width, max_height;
} terminal_graphics;
typedef enum {
    TERMINAL_PALETTE_INVALID,
    TERMINAL_PALETTE_TEXT,
    TERMINAL_PALETTE_BITMAP_THEMED,
    TERMINAL_PALETTE_BITMAP_EXPLICIT
} terminal_palette_t;

static terminal_palette_t terminal_palette;
typedef struct {
    chtype style;
    char glyph[8];
} terminal_cell_t;

static terminal_cell_t rendered_cells[TERMINAL_MAX_CELLS];
static uint16_t rendered_columns;
static uint16_t rendered_rows;
static bool rendered_cells_valid;
static bool cga_pair_initialized[136];
static bool terminal_cursor_style_valid;
static bool terminal_cursor_visible;
static bool terminal_cursor_position_valid;
static uint16_t terminal_cursor_position;

static const uint16_t cp437_extended[128] = {
    0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7, 0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5,
    0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9, 0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192,
    0x00e1, 0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba, 0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
    0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f, 0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b, 0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
    0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4, 0x03a6, 0x0398, 0x03a9, 0x03b4, 0x221e, 0x03c6, 0x03b5, 0x2229,
    0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248, 0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0
};

uint32_t
tigt_cp437_codepoint(uint8_t character)
{
    static const uint16_t controls[32] = {
        0x0020, 0x263a, 0x263b, 0x2661, 0x2662, 0x2667, 0x2664, 0x2022,
        0x25d8, 0x25cb, 0x25d9, 0x2642, 0x2640, 0x266a, 0x266b, 0x263c,
        0x25ba, 0x25c4, 0x2195, 0x203c, 0x00b6, 0x00a7, 0x25ac, 0x21a8,
        0x2191, 0x2193, 0x2192, 0x2190, 0x221f, 0x2194, 0x25b2, 0x25bc
    };

    if (character == 0 || character == 0xff)
        return 0x0020;
    if (character < 32)
        return controls[character];
    if (character == 127)
        return 0x2302;
    if (character < 128)
        return character;
    return cp437_extended[character - 128];
}

static const char *
codepoint_utf8(uint32_t codepoint, char output[8])
{
    if (codepoint == 0x263a || codepoint == 0x203c) {
        /* Keep the existing CP437 glyphs in text, rather than emoji, style. */
        memcpy(output, codepoint == 0x263a ? "\xe2\x98\xba\xef\xb8\x8e" :
                                          "\xe2\x80\xbc\xef\xb8\x8e", 7);
        return output;
    }

    if (codepoint < 0x80) {
        output[0] = (char) codepoint;
        output[1] = '\0';
    } else if (codepoint < 0x800) {
        output[0] = (char) (0xc0 | (codepoint >> 6));
        output[1] = (char) (0x80 | (codepoint & 0x3f));
        output[2] = '\0';
    } else if (codepoint < 0x10000) {
        output[0] = (char) (0xe0 | (codepoint >> 12));
        output[1] = (char) (0x80 | ((codepoint >> 6) & 0x3f));
        output[2] = (char) (0x80 | (codepoint & 0x3f));
        output[3] = '\0';
    } else {
        output[0] = (char) (0xf0 | (codepoint >> 18));
        output[1] = (char) (0x80 | ((codepoint >> 12) & 0x3f));
        output[2] = (char) (0x80 | ((codepoint >> 6) & 0x3f));
        output[3] = (char) (0x80 | (codepoint & 0x3f));
        output[4] = '\0';
    }
    return output;
}


static const short cga_5153_xterm[16] = {
    16, 20, 40, 44, 160, 164, 136, 251,
    239, 62, 83, 87, 167, 207, 227, 231
};

static void
select_terminal_palette(terminal_palette_t palette)
{
    if (terminal_palette == palette)
        return;

    /* Text and bitmap rendering share the eight-bit curses pair bank. */
    memset(cga_pair_initialized, 0, sizeof(cga_pair_initialized));
    rendered_cells_valid = false;
    terminal_palette = palette;
}

static chtype
text_cell_attributes(uint8_t foreground, uint8_t background)
{
    select_terminal_palette(TERMINAL_PALETTE_TEXT);

    if (has_colors()) {
        /* chtype stores only eight pair bits, even when COLOR_PAIRS is larger.
           Share each unordered color pair and reverse it when necessary. */
        const uint8_t low = foreground < background ? foreground : background;
        const uint8_t high = foreground < background ? background : foreground;
        const int pair = 2 + high * (high + 1) / 2 + low;
        const bool high_color = COLORS >= 256;
        const short terminal_foreground = high_color ? cga_5153_xterm[low] :
                                          (low < COLORS ? low : low & 7);
        const short terminal_background = high_color ? cga_5153_xterm[high] :
                                          (high < COLORS ? high : high & 7);

        if (pair < COLOR_PAIRS) {
            if (!cga_pair_initialized[pair - 2]) {
                if (init_pair(pair, terminal_foreground, terminal_background) == ERR)
                    return foreground >= 8 ? A_BOLD : A_NORMAL;
                cga_pair_initialized[pair - 2] = true;
            }
            return COLOR_PAIR(pair) | (foreground > background ? A_REVERSE : A_NORMAL) |
                   (!high_color && low >= COLORS ? A_BOLD : A_NORMAL);
        }
    }
    return foreground >= 8 ? A_BOLD : A_NORMAL;
}

static chtype
bitmap_cell_attributes(uint8_t foreground, uint8_t background, terminal_palette_t palette)
{
    /* A text cell cannot emphasize its background independently. Preserve
     * contrast with explicit colors when 7 and 15 share the same cell. */
    const bool themed = terminal_default_colors_available &&
                        palette == TERMINAL_PALETTE_BITMAP_THEMED &&
                        !((foreground == 7 && background == 15) ||
                          (foreground == 15 && background == 7));
    const bool themed_foreground = themed && (foreground == 7 || foreground == 15);
    const bool themed_background = themed && background == 0;
    const chtype intensity = themed_foreground && foreground == 15 ? A_BOLD : A_NORMAL;

    if (has_colors()) {
        const uint8_t low = foreground < background ? foreground : background;
        const uint8_t high = foreground < background ? background : foreground;
        const int pair = 2 + high * (high + 1) / 2 + low;
        const bool high_color = COLORS >= 256;
        const short terminal_foreground = themed_foreground ? -1 :
                                          high_color ? cga_5153_xterm[foreground] :
                                          (foreground < COLORS ? foreground : foreground & 7);
        const short terminal_background = themed_background ? -1 :
                                          high_color ? cga_5153_xterm[background] :
                                          (background < COLORS ? background : background & 7);

        if (pair < COLOR_PAIRS) {
            if (!cga_pair_initialized[pair - 2]) {
                if (init_pair(pair, terminal_foreground, terminal_background) == ERR)
                    return intensity;
                cga_pair_initialized[pair - 2] = true;
            }
            return COLOR_PAIR(pair) | intensity |
                   (!themed_foreground && !high_color && foreground >= COLORS ? A_BOLD : A_NORMAL);
        }
    }
    return intensity;
}

static const char *
cga_sextant_utf8(uint8_t mask, char output[8])
{
    static const uint32_t sextants[64] = {
        0x0020, 0x1fb00, 0x1fb01, 0x1fb02, 0x1fb03, 0x1fb04, 0x1fb05, 0x1fb06,
        0x1fb07, 0x1fb08, 0x1fb09, 0x1fb0a, 0x1fb0b, 0x1fb0c, 0x1fb0d, 0x1fb0e,
        0x1fb0f, 0x1fb10, 0x1fb11, 0x1fb12, 0x1fb13, 0x258c, 0x1fb14, 0x1fb15,
        0x1fb16, 0x1fb17, 0x1fb18, 0x1fb19, 0x1fb1a, 0x1fb1b, 0x1fb1c, 0x1fb1d,
        0x1fb1e, 0x1fb1f, 0x1fb20, 0x1fb21, 0x1fb22, 0x1fb23, 0x1fb24, 0x1fb25,
        0x1fb26, 0x1fb27, 0x2590, 0x1fb28, 0x1fb29, 0x1fb2a, 0x1fb2b, 0x1fb2c,
        0x1fb2d, 0x1fb2e, 0x1fb2f, 0x1fb30, 0x1fb31, 0x1fb32, 0x1fb33, 0x1fb34,
        0x1fb35, 0x1fb36, 0x1fb37, 0x1fb38, 0x1fb39, 0x1fb3a, 0x1fb3b, 0x2588
    };
    const uint32_t codepoint = sextants[mask];

    if (codepoint < 0x80) {
        output[0] = (char) codepoint;
        output[1] = '\0';
    } else if (codepoint < 0x800) {
        output[0] = (char) (0xc0 | (codepoint >> 6));
        output[1] = (char) (0x80 | (codepoint & 0x3f));
        output[2] = '\0';
    } else if (codepoint < 0x10000) {
        output[0] = (char) (0xe0 | (codepoint >> 12));
        output[1] = (char) (0x80 | ((codepoint >> 6) & 0x3f));
        output[2] = (char) (0x80 | (codepoint & 0x3f));
        output[3] = '\0';
    } else {
        output[0] = (char) (0xf0 | (codepoint >> 18));
        output[1] = (char) (0x80 | ((codepoint >> 12) & 0x3f));
        output[2] = (char) (0x80 | ((codepoint >> 6) & 0x3f));
        output[3] = (char) (0x80 | (codepoint & 0x3f));
        output[4] = '\0';
    }
    return output;
}

static uint8_t
cga_rendered_color_index(uint32_t color)
{
    static const uint32_t cga_5153_rgb[16] = {
        0x000000, 0x0000c4, 0x00c400, 0x00c4c4,
        0xc40000, 0xc400c4, 0xc47e00, 0xc4c4c4,
        0x4e4e4e, 0x4e4edc, 0x4edc4e, 0x4ef3f3,
        0xdc4e4e, 0xf34ef3, 0xf3f34e, 0xffffff
    };
    const int red = (color >> 16) & 0xff;
    const int green = (color >> 8) & 0xff;
    const int blue = color & 0xff;
    uint32_t closest_distance = UINT32_MAX;
    uint8_t closest = 0;

    for (uint8_t index = 0; index < 16; index++) {
        const int red_delta = red - ((cga_5153_rgb[index] >> 16) & 0xff);
        const int green_delta = green - ((cga_5153_rgb[index] >> 8) & 0xff);
        const int blue_delta = blue - (cga_5153_rgb[index] & 0xff);
        const uint32_t distance = red_delta * red_delta + green_delta * green_delta +
                                  blue_delta * blue_delta;

        if (distance < closest_distance) {
            closest_distance = distance;
            closest = index;
        }
    }
    return closest;
}



static bool
set_cursor(bool visible)
{
    bool changed = false;

    if (visible != terminal_cursor_visible) {
        curs_set(visible ? 1 : 0);
        terminal_cursor_visible = visible;
        changed = true;
    }
    if (visible && (!terminal_cursor_style_valid || changed)) {
        /* Set the steady style after cnorm, which can alter cursor blinking. */
        fputs("\033[?12l\033[4 q", stdout);
        terminal_cursor_style_valid = true;
        changed = true;
    }
    if (changed)
        fflush(stdout);
    return changed;
}

/* Raw images and curses share one writer. Invalidate curses' physical-screen
 * model whenever an image is removed; erase() alone cannot remove raw images. */
static void
leave_image_graphics(void)
{
    if (!rendered_image)
        return;
    if (renderer_graphics_mode == TIGT_GRAPHICS_SIXEL)
        fputs("\033[?80;1070r", stdout);
    fputs("\033[0m\033[2J\033[H", stdout);
    /* Curses can write directly to the fd: drain stdio first, so cleanup
     * cannot erase replacement text or the restored shell screen. */
    fflush(stdout);
    rendered_image = false;
    rendered_cells_valid = false;
    terminal_cursor_position_valid = false;
    clearok(stdscr, true);
}

/* Called under renderer_mutex. Whole-window reports belong to the dimensions
 * at probe time: infer cell metrics rather than reuse stale bounds on resize. */
static uint64_t
queried_pixel_extent(uint16_t pixels, uint16_t cell_pixels, uint16_t cells,
                     uint16_t queried_cells)
{
    if (cell_pixels != 0 && cells != 0)
        return (uint64_t) cell_pixels * cells;
    if (pixels != 0 && queried_cells != 0 && cells != 0) {
        const uint64_t extent = (uint64_t) pixels * cells / queried_cells;
        return extent != 0 ? extent : 1;
    }
    return cells == queried_cells ? pixels : 0;
}

static void
size_image_graphics(const struct winsize *window)
{
    const image_layout_t layout = renderer_image_layout;
    uint64_t target_width = 640;
    if (window->ws_xpixel != 0 && window->ws_col != 0)
        target_width = (uint64_t) layout.columns * window->ws_xpixel / window->ws_col;
    else if (terminal_graphics.cell_width != 0)
        target_width = (uint64_t) layout.columns * terminal_graphics.cell_width;
    else if (terminal_graphics.pixel_width != 0 && terminal_graphics.columns != 0)
        target_width = (uint64_t) layout.columns * terminal_graphics.pixel_width /
                       terminal_graphics.columns;

    uint64_t available_width = window->ws_xpixel;
    uint64_t available_height = window->ws_ypixel;
    if (available_width == 0)
        available_width = queried_pixel_extent(terminal_graphics.pixel_width,
                                               terminal_graphics.cell_width,
                                               window->ws_col, terminal_graphics.columns);
    if (available_height == 0)
        available_height = queried_pixel_extent(terminal_graphics.pixel_height,
                                                terminal_graphics.cell_height,
                                                window->ws_row, terminal_graphics.rows);
    uint64_t width = target_width != 0 ? target_width : 1;
    uint64_t height = 4096;
    if (width > 4096) width = 4096;
    if (available_width != 0 && width > available_width) width = available_width;
    if (available_height != 0 && height > available_height) height = available_height;
    if (renderer_graphics_mode == TIGT_GRAPHICS_SIXEL) {
        if (terminal_graphics.max_width != 0 && width > terminal_graphics.max_width)
            width = terminal_graphics.max_width;
        if (terminal_graphics.max_height != 0 && height > terminal_graphics.max_height)
            height = terminal_graphics.max_height;
    }
    /* Fit using the requested ratio directly, avoiding compounded rounding. */
    if (width * layout.aspect_height <= height * layout.aspect_width)
        height = width * layout.aspect_height / layout.aspect_width;
    else
        width = height * layout.aspect_width / layout.aspect_height;
    rendered_image_width = (uint16_t) (width != 0 ? width : 1);
    rendered_image_height = (uint16_t) (height != 0 ? height : 1);
}

static void
render_image_graphics(const uint32_t *pixels, uint16_t width, uint16_t height, uint8_t pixel_width)
{
    const uint16_t output_width = rendered_image_width;
    const uint16_t output_height = rendered_image_height;
    if (!rendered_image || !rendered_cells_valid ||
        rendered_columns != output_width || rendered_rows != output_height) {
        rendered_cells_valid = false;
        if (erase() == ERR || clearok(stdscr, true) == ERR || refresh() == ERR)
            return;
    }
    set_cursor(false);
    int result = TIGT_OK;
    if (!rendered_image && renderer_graphics_mode == TIGT_GRAPHICS_SIXEL &&
        fputs("\033[?80;1070s\033[?80;1070h", stdout) == EOF)
        result = TIGT_ERROR_SYSTEM;
    /* Track partial output too, so text/suspend/exit still remove it. */
    rendered_image = true;
    if (fputs("\0337\033[H", stdout) == EOF)
        result = TIGT_ERROR_SYSTEM;
    if (result == TIGT_OK) {
        result = renderer_graphics_mode == TIGT_GRAPHICS_ITERM2 ?
            tigt_iterm2_write(stdout, pixels, width, height, pixel_width,
                              output_width, output_height) :
            tigt_sixel_write(stdout, pixels, width, height, pixel_width,
                             output_width, output_height);
    }
    if (fputs("\0338", stdout) == EOF)
        result = TIGT_ERROR_SYSTEM;
    const int flushed = fflush(stdout);
    rendered_columns = output_width;
    rendered_rows = output_height;
    rendered_cells_valid = result == TIGT_OK && flushed == 0 && !ferror(stdout);
    terminal_cursor_position_valid = false;
}

static void
render_ascii_graphics(const uint32_t *pixels, uint16_t width, uint16_t height, uint8_t pixel_width,
                      terminal_palette_t palette)
{
    uint16_t columns = COLS > TERMINAL_MAX_COLUMNS ? TERMINAL_MAX_COLUMNS : COLS;
    uint16_t rows = LINES > TERMINAL_MAX_ROWS ? TERMINAL_MAX_ROWS : LINES;
    if (columns == 0 || rows == 0)
        return;
    if ((uint32_t) columns * rows > TERMINAL_MAX_CELLS)
        rows = TERMINAL_MAX_CELLS / columns;
    const tigt_ascii_cell *cells;
    if (tigt_ascii_render(renderer_ascii, pixels, width, height, pixel_width,
                          columns, rows, palette == TERMINAL_PALETTE_BITMAP_THEMED, &cells) != TIGT_OK)
        return;
    bool changed = false;
    if (!rendered_cells_valid || rendered_columns != columns || rendered_rows != rows) {
        erase();
        rendered_cells_valid = false;
        rendered_columns = columns;
        rendered_rows = rows;
        changed = true;
    }
    for (uint16_t row = 0; row < rows; row++) {
        for (uint16_t column = 0; column < columns; column++) {
            const size_t index = (size_t) row * columns + column;
            uint8_t foreground = cells[index].foreground, background = cells[index].background;
            uint8_t glyph = cells[index].glyph;
            if (foreground == background) glyph = ' ';
            /* A space has no ink. Do not let libcaca's unused foreground
             * falsely turn a solid themed background into a 7/15 mixed cell. */
            if (glyph == ' ') foreground = 0;
            const bool reverse = (foreground == 0 && background != 0) ||
                                 (background != 0 &&
                                  (palette == TERMINAL_PALETTE_BITMAP_THEMED ?
                                   foreground == 7 && background == 15 : foreground > background));
            if (reverse) {
                const uint8_t swap = foreground;
                foreground = background;
                background = swap;
            }
            const terminal_cell_t cell = {
                .glyph = { (char) glyph, '\0' },
                .style = bitmap_cell_attributes(foreground, background, palette) |
                         (reverse ? A_REVERSE : A_NORMAL)
            };
            if (!rendered_cells_valid || cell.style != rendered_cells[index].style ||
                strcmp(cell.glyph, rendered_cells[index].glyph) != 0) {
                attrset(cell.style);
                mvaddstr(row, column, cell.glyph);
                attrset(A_NORMAL);
                rendered_cells[index] = cell;
                changed = true;
            }
        }
    }
    terminal_cursor_position_valid = false;
    changed |= set_cursor(false);
    if (changed) refresh();
    rendered_cells_valid = true;
}
/* Eligibility is a property of the full native image, not of its quantized or
 * sampled projection. Multiple regular gray values still count separately. */
static bool
bitmap_is_themed(const uint32_t *pixels, size_t count)
{
    uint32_t unique[3];
    size_t used = 0;
    for (size_t index = 0; index < count; index++) {
        const uint32_t rgb = pixels[index] & 0xffffff;
        const uint8_t level = rgb & 0xff;
        if (rgb != 0 && rgb != 0xffffff &&
            (level <= 128 || rgb != (uint32_t) level * 0x010101))
            return false;
        size_t color = 0;
        while (color < used && unique[color] != rgb) color++;
        if (color == used) {
            if (used == 3) return false;
            unique[used++] = rgb;
        }
    }
    return true;
}

static void
render_bitmap_graphics(const uint32_t *pixels, uint16_t width, uint16_t height,
                       uint8_t pixel_width, bool display_enabled)
{
    uint16_t presence = 0;
    bool changed = false;

    if (width == 0 || width > 640 || height == 0 || height > 200 || pixel_width == 0)
        return;

    const uint16_t columns = width / (2 * pixel_width);
    const uint16_t rows = (height + 2) / 3;

    if (columns == 0 || rows == 0 || columns * rows > TERMINAL_MAX_CELLS)
        return;

    static const uint32_t black[640 * 200];
    if (!display_enabled) pixels = black;
    if (renderer_graphics_mode == TIGT_GRAPHICS_SIXEL ||
        renderer_graphics_mode == TIGT_GRAPHICS_ITERM2) {
        render_image_graphics(pixels, width, height, pixel_width);
        return;
    }
    leave_image_graphics();
    const bool themed = bitmap_is_themed(pixels, (size_t) width * height);
    const terminal_palette_t palette = themed ? TERMINAL_PALETTE_BITMAP_THEMED :
                                               TERMINAL_PALETTE_BITMAP_EXPLICIT;
    select_terminal_palette(palette);
    if (renderer_graphics_mode == TIGT_GRAPHICS_ASCII) {
        render_ascii_graphics(pixels, width, height, pixel_width, palette);
        return;
    }
    for (size_t pixel = 0; pixel < (size_t) width * height; pixel++) {
        const uint32_t rgb = pixels[pixel] & 0xffffff;
        const uint8_t color = themed ? (rgb == 0 ? 0 : rgb == 0xffffff ? 15 : 7) :
                                      cga_rendered_color_index(rgb);
        renderer_bitmap_indices[pixel] = color;
        presence |= 1u << color;
    }
    if (!rendered_cells_valid || rendered_columns != columns || rendered_rows != rows) {
        erase();
        rendered_cells_valid = false;
        rendered_columns = columns;
        rendered_rows = rows;
        changed = true;
    }

    for (uint16_t row = 0; row < rows; row++) {
        for (uint16_t column = 0; column < columns; column++) {
            char utf8[8];
            uint8_t colors[6] = { 0 };
            uint8_t color_counts[6] = { 0 };
            uint8_t samples[6];
            uint8_t color_count = 0;
            uint8_t background = 0;
            uint8_t foreground = 0;
            uint8_t mask = 0;
            const uint16_t bitmap_x = column * 2 * pixel_width;
            const uint16_t source_y = row * 3;
            const size_t cell_index = row * columns + column;
            terminal_cell_t cell;

            for (uint8_t pixel = 0; pixel < 6; pixel++) {
                const uint16_t x = bitmap_x + ((pixel & 1) * pixel_width);
                const uint16_t y = source_y + pixel / 2 < height ? source_y + pixel / 2 : height - 1;
                const uint8_t color = renderer_bitmap_indices[y * width + x];
                uint8_t index;

                samples[pixel] = color;

                for (index = 0; index < color_count && colors[index] != color; index++)
                    ;
                if (index == color_count)
                    colors[color_count++] = color;
                color_counts[index]++;
            }

            for (uint8_t index = 0; index < color_count; index++) {
                if (color_counts[index] > color_counts[background]) {
                    foreground = background;
                    background = index;
                } else if (index != background &&
                           (foreground == background || color_counts[index] > color_counts[foreground])) {
                    foreground = index;
                }
            }
            if (foreground == background && color_count > 1)
                foreground = background == 0 ? 1 : 0;

            for (uint8_t pixel = 0; pixel < 6; pixel++) {
                const uint8_t color = samples[pixel];

                if (colors[foreground] == color)
                    mask |= 1 << pixel;
            }

            foreground = colors[foreground];
            background = colors[background];
            const uint8_t preferred_foreground = palette == TERMINAL_PALETTE_BITMAP_THEMED ?
                                                 ((presence & (1u << 15)) != 0 ? 15 : 7) : 0;
            if (foreground == background) {
                /* Black uses the background role; all other solid colors use ink. */
                mask = foreground == 0 ? 0 : 0x3f;
            } else if ((preferred_foreground != 0 && background == preferred_foreground) ||
                       ((preferred_foreground == 0 || foreground != preferred_foreground) &&
                        (foreground == 0 || (background != 0 && foreground < background)))) {
                const uint8_t swap = foreground;

                foreground = background;
                background = swap;
                mask ^= 0x3f;
            }
            cell.style = bitmap_cell_attributes(foreground, background, palette);
            strcpy(cell.glyph, cga_sextant_utf8(mask, utf8));

            if (!rendered_cells_valid || cell.style != rendered_cells[cell_index].style ||
                strcmp(cell.glyph, rendered_cells[cell_index].glyph) != 0) {
                attrset(cell.style);
                mvaddstr(row, column, cell.glyph);
                attrset(A_NORMAL);
                rendered_cells[cell_index] = cell;
                changed = true;
            }
        }
    }

    terminal_cursor_position_valid = false;
    changed |= set_cursor(false);
    if (changed)
        refresh();
    rendered_cells_valid = true;
}


static void
render_text(const tigt_text_cell *cells, uint16_t columns, uint16_t rows, bool cursor_allowed)
{
    bool changed = false;
    uint16_t cursor_position = UINT16_MAX;
    leave_image_graphics();

    select_terminal_palette(TERMINAL_PALETTE_TEXT);
    if (!rendered_cells_valid || rendered_columns != columns || rendered_rows != rows) {
        erase();
        rendered_cells_valid = false;
        rendered_columns = columns;
        rendered_rows = rows;
        changed = true;
    }

    for (uint16_t row = 0; row < rows; row++) {
        for (uint16_t column = 0; column < columns; column++) {
            const size_t cell_index = (size_t) row * columns + column;
            const tigt_text_cell *source = &cells[cell_index];
            const uint8_t foreground = cga_rendered_color_index(source->foreground);
            const uint8_t background = cga_rendered_color_index(source->background);
            terminal_cell_t cell = {
                .style = text_cell_attributes(foreground, background) |
                         ((source->flags & TIGT_TEXT_UNDERLINE) != 0 ? A_UNDERLINE : A_NORMAL)
            };

            codepoint_utf8(source->codepoint, cell.glyph);
            if ((source->flags & TIGT_TEXT_CURSOR) != 0)
                cursor_position = (uint16_t) cell_index;
            if (!rendered_cells_valid || cell.style != rendered_cells[cell_index].style ||
                strcmp(cell.glyph, rendered_cells[cell_index].glyph) != 0) {
                attrset(cell.style);
                mvaddstr(row, column, cell.glyph);
                attrset(A_NORMAL);
                rendered_cells[cell_index] = cell;
                changed = true;
            }
        }
    }

    bool show_cursor = cursor_allowed && cursor_position != UINT16_MAX;

    if (show_cursor && (changed || !terminal_cursor_position_valid ||
                        cursor_position != terminal_cursor_position)) {
        /* Drawing cells also moves curses' cursor, even if its target is fixed. */
        if (move(cursor_position / columns, cursor_position % columns) == ERR) {
            show_cursor = false;
        } else {
            terminal_cursor_position = cursor_position;
            terminal_cursor_position_valid = true;
            changed = true;
        }
    }
    if (!show_cursor)
        terminal_cursor_position_valid = false;
    if (changed) {
        refresh();
        /* refresh can emit cnorm: restore a steady style after its output. */
        terminal_cursor_style_valid = false;
    }
    set_cursor(show_cursor);
    rendered_cells_valid = true;
}

/* Both the display and snapshot encoders consume this same native-frame copy.
 * Callers hold renderer_mutex; no application buffers survive submission. */
static void
copy_native_frame(tigt_native_frame *frame)
{
    frame->bitmap = renderer_bitmap_valid;
    frame->width = frame->bitmap ? renderer_bitmap_width : renderer_text_columns;
    frame->height = frame->bitmap ? 200 : renderer_text_rows;
    frame->pixel_width = frame->bitmap ? renderer_bitmap_pixel_width : 1;
    frame->overscan = renderer_overscan;
    if (frame->bitmap)
        memcpy(frame->content.pixels, renderer_bitmap_pixels,
               (size_t) frame->width * frame->height * sizeof(*frame->content.pixels));
    else
        memcpy(frame->content.cells, renderer_text_cells,
               (size_t) frame->width * frame->height * sizeof(*frame->content.cells));
}

int
tigt_snapshot_capture(tigt_native_frame *frame)
{
    pthread_mutex_lock(&renderer_mutex);
    if (!renderer_active || !renderer_has_frame) {
        pthread_mutex_unlock(&renderer_mutex);
        return TIGT_ERROR_BUSY;
    }
    copy_native_frame(frame);
    pthread_mutex_unlock(&renderer_mutex);
    return TIGT_OK;
}

/* Publish only completed output, never a frame merely submitted by a caller.
 * The block renderer packs 2x3 logical pixels per cell; its final partial row
 * must not stretch the source height across a rounded-up cell count. */
static void
publish_pointer_projection(const tigt_native_frame *frame)
{
    pthread_mutex_lock(&renderer_mutex);
    pointer_projection.valid = rendered_cells_valid && rendered_columns != 0 && rendered_rows != 0;
    pointer_projection.pixels = rendered_image;
    pointer_projection.width = frame->bitmap ? frame->width / frame->pixel_width : frame->width;
    pointer_projection.height = frame->height;
    pointer_projection.columns = rendered_window.ws_col;
    pointer_projection.rows = rendered_window.ws_row;
    if (pointer_projection.valid) {
        pointer_projection.scale_x = (double) pointer_projection.width / rendered_columns;
        pointer_projection.scale_y = (double) pointer_projection.height / rendered_rows;
        if (frame->bitmap && renderer_graphics_mode == TIGT_GRAPHICS_BLOCKS) {
            pointer_projection.scale_x = 2;
            pointer_projection.scale_y = 3;
        }
    }
    pointer_projection.cell_width = rendered_window.ws_xpixel && rendered_window.ws_col ?
        (double) rendered_window.ws_xpixel / rendered_window.ws_col :
        terminal_graphics.cell_width ? terminal_graphics.cell_width :
        terminal_graphics.columns ? (double) terminal_graphics.pixel_width / terminal_graphics.columns : 0;
    pointer_projection.cell_height = rendered_window.ws_ypixel && rendered_window.ws_row ?
        (double) rendered_window.ws_ypixel / rendered_window.ws_row :
        terminal_graphics.cell_height ? terminal_graphics.cell_height :
        terminal_graphics.rows ? (double) terminal_graphics.pixel_height / terminal_graphics.rows : 0;
    pthread_mutex_unlock(&renderer_mutex);
}

static void
terminal_input(const tigt_input_event *event, void *user)
{
    (void) user;
    int result = tigt_terminal_filter_input(event);
    if (result <= 0 || renderer_config.on_input == NULL) return;
    unsigned index = 0;
    while (index < held_key_count &&
           (held_keys[index].key.kind != event->key.kind ||
            held_keys[index].key.value != event->key.value ||
            held_keys[index].key.character != event->key.character))
        index++;
    if (event->kind == TIGT_RELEASE) {
        if (index < held_key_count) held_keys[index] = held_keys[--held_key_count];
    } else if (index == held_key_count) {
        if (held_key_count == 256) {
            tigt_terminal_record_error(TIGT_ERROR_SYSTEM);
            return;
        }
        held_keys[held_key_count++] = *event;
    }
    renderer_config.on_input(event, renderer_config.user);
}

static void
release_guest_keys(void)
{
    while (held_key_count != 0) {
        tigt_input_event event = held_keys[--held_key_count];
        event.kind = TIGT_RELEASE;
        renderer_config.on_input(&event, renderer_config.user);
    }
    if (renderer_config.on_mouse != NULL) {
        for (unsigned button = TIGT_MOUSE_BUTTON_LEFT; button <= TIGT_MOUSE_BUTTON_RIGHT; button++) {
            uint32_t bit = 1u << (button - 1);
            if (!(held_mouse_buttons & bit)) continue;
            held_mouse_buttons &= ~bit;
            tigt_mouse_event event = {
                .kind = TIGT_MOUSE_UP, .button = button, .buttons = held_mouse_buttons,
                .flags = TIGT_MOUSE_SYNTHETIC
            };
            renderer_config.on_mouse(&event, renderer_config.mouse_user);
        }
    }
}

static int
glass_frame(const tigt_text_cell *cells, uint16_t columns, uint16_t rows)
{
    unsigned cursor = 0;
    bool found = false;
    for (unsigned i = 0; i < (unsigned) columns * rows; i++)
        if (cells[i].flags & TIGT_TEXT_CURSOR) { cursor = i; found = true; break; }
    /* Native frames do not report a hidden logical cursor. Never invent one
     * in order to make fullscreen output appear representable as scrollback. */
    if (!found) return TIGT_ERROR_UNREPRESENTABLE;
    tigt_presenter_frame frame = {
        .cells = cells, .columns = columns, .rows = rows, .stride = columns,
        .cursor_column = cursor % columns, .cursor_row = cursor / columns,
        .refresh_hz = 50
    };
    return tigt_presenter_present(background_presenter, &frame);
}

static void
terminal_mouse(const tigt_mouse_event *event, void *user)
{
    if (!tigt_terminal_is_foreground() || tigt_terminal_is_released()) return;
    (void) user;
    tigt_mouse_event mapped = *event;
    held_mouse_buttons = event->buttons;
    pthread_mutex_lock(&renderer_mutex);
    if ((event->flags & TIGT_MOUSE_POSITION_VALID) && pointer_projection.valid) {
        const bool pixels = event->coordinates == TIGT_MOUSE_COORD_PIXELS;
        const bool metrics = pointer_projection.cell_width > 0 && pointer_projection.cell_height > 0;
        if (pixels == pointer_projection.pixels || metrics) {
            double x = (double) event->x + 0.5, y = (double) event->y + 0.5;
            bool visible = true;
            if (!pixels)
                visible = x < pointer_projection.columns && y < pointer_projection.rows;
            else if (metrics)
                visible = x < pointer_projection.columns * pointer_projection.cell_width &&
                          y < pointer_projection.rows * pointer_projection.cell_height;
            if (pixels && !pointer_projection.pixels) {
                x /= pointer_projection.cell_width;
                y /= pointer_projection.cell_height;
            } else if (!pixels && pointer_projection.pixels) {
                x *= pointer_projection.cell_width;
                y *= pointer_projection.cell_height;
            }
            mapped.frame_x = x * pointer_projection.scale_x;
            mapped.frame_y = y * pointer_projection.scale_y;
            mapped.flags |= TIGT_MOUSE_FRAME_VALID;
            if (visible && mapped.frame_x >= 0 && mapped.frame_y >= 0 &&
                mapped.frame_x < pointer_projection.width && mapped.frame_y < pointer_projection.height)
                mapped.flags |= TIGT_MOUSE_INSIDE_FRAME;
        }
    }
    pthread_mutex_unlock(&renderer_mutex);
    renderer_config.on_mouse(&mapped, renderer_config.mouse_user);
}

static void
render_frame(void)
{
    /* Only the renderer thread owns this copy; keep it off small pthread stacks. */
    static tigt_native_frame snapshot;
    struct winsize window = { 0 };
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0 && window.ws_col != 0 && window.ws_row != 0 &&
        (window.ws_col != rendered_window.ws_col || window.ws_row != rendered_window.ws_row ||
         window.ws_xpixel != rendered_window.ws_xpixel || window.ws_ypixel != rendered_window.ws_ypixel)) {
        if (window.ws_col != rendered_window.ws_col || window.ws_row != rendered_window.ws_row)
            resizeterm(window.ws_row, window.ws_col);
        rendered_window = window;
        rendered_cells_valid = false;
    }
    pthread_mutex_lock(&renderer_mutex);
    if (!renderer_has_frame ||
        (rendered_cells_valid && renderer_frame_serial == rendered_frame_serial)) {
        pthread_mutex_unlock(&renderer_mutex);
        return;
    }
    const uint64_t frame_serial = renderer_frame_serial;
    const bool cursor_allowed = renderer_display_technology != TIGT_DISPLAY_MDA ||
                                renderer_text_output_seen;
    copy_native_frame(&snapshot);
    if (snapshot.bitmap && (renderer_graphics_mode == TIGT_GRAPHICS_SIXEL ||
                            renderer_graphics_mode == TIGT_GRAPHICS_ITERM2))
        size_image_graphics(&rendered_window);
    pointer_projection.valid = false;
    pthread_mutex_unlock(&renderer_mutex);

    if (snapshot.bitmap)
        render_bitmap_graphics(snapshot.content.pixels, snapshot.width, snapshot.height,
                               snapshot.pixel_width, true);
    else
        render_text(snapshot.content.cells, snapshot.width, snapshot.height, cursor_allowed);
    if (rendered_cells_valid) rendered_frame_serial = frame_serial;
    if (renderer_config.on_mouse != NULL)
        publish_pointer_projection(&snapshot);
}


static void *
input_main(void *unused)
{
    struct pollfd descriptor = { .fd = STDIN_FILENO, .events = POLLIN };
    uint8_t bytes[128];
    unsigned epoch = tigt_terminal_generation();

    (void) unused;
    while (atomic_load_explicit(&input_running, memory_order_relaxed)) {
        if (epoch != tigt_terminal_generation()) {
            release_guest_keys();
            pthread_mutex_lock(&renderer_mutex);
            tigt_input_destroy(renderer_input);
            renderer_input = tigt_input_create_with_mouse(
                terminal_input, NULL,
                renderer_config.on_mouse != NULL ? terminal_mouse : NULL, NULL,
                renderer_config.mouse_mode);
            if (renderer_input == NULL) {
                tigt_terminal_record_error(TIGT_ERROR_SYSTEM);
                pthread_mutex_unlock(&renderer_mutex);
                break;
            }
            tigt_input_set_terminal_report_callback(renderer_input, terminal_report, NULL);
            tigt_input_set_mouse_mode(renderer_input, terminal_mouse_mode);
            pthread_mutex_unlock(&renderer_mutex);
            epoch = tigt_terminal_generation();
        }
        if (!tigt_terminal_is_foreground() || tigt_terminal_is_released()) {
            const struct timespec delay = { .tv_nsec = 25000000 };
            nanosleep(&delay, NULL);
            continue;
        }
        const int ready = poll(&descriptor, 1, 25);

        if (!atomic_load_explicit(&input_running, memory_order_relaxed))
            break;
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0) {
            if (tigt_terminal_is_foreground() && !tigt_terminal_is_released())
                tigt_input_flush(renderer_input);
            continue;
        }
        if ((descriptor.revents & POLLIN) != 0) {
            if (!tigt_terminal_begin_io(1)) continue;
            const ssize_t length = read(STDIN_FILENO, bytes, sizeof(bytes));
            tigt_terminal_end_io();

            if (length > 0)
                tigt_input_feed(renderer_input, bytes, (size_t) length);
            else if (length == 0 || (errno != EINTR && errno != EAGAIN))
                break;
        } else if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            break;
        }
    }
    release_guest_keys();
    return NULL;
}

static void *
renderer_main(void *unused)
{
    unsigned epoch = tigt_terminal_generation();
    const struct timespec interval = { .tv_sec = 0, .tv_nsec = 20000000 };

    (void) unused;
    while (atomic_load_explicit(&renderer_running, memory_order_relaxed)) {
        if (tigt_terminal_begin_io(1)) {
            if (epoch != tigt_terminal_generation()) {
                epoch = tigt_terminal_generation();
                rendered_cells_valid = false;
                terminal_cursor_style_valid = false;
                terminal_cursor_position_valid = false;
                pthread_mutex_lock(&renderer_mutex);
                pointer_projection.valid = false;
                pthread_mutex_unlock(&renderer_mutex);
            }
            if (renderer_screen != NULL) render_frame();
            tigt_terminal_end_io();
        } else if (!tigt_terminal_is_faulted() && !tigt_terminal_is_foreground()) {
            /* No terminal probes, reads, protocols or curses in background.
             * A pending accepted bitmap fails observably, not by dropping it. */
            pthread_mutex_lock(&renderer_mutex);
            if (renderer_has_frame && renderer_bitmap_valid) {
                if (renderer_frame_serial != rendered_frame_serial)
                    tigt_terminal_record_error(TIGT_ERROR_BACKGROUND);
            } else if (renderer_has_frame) {
                int result = glass_frame(renderer_text_cells, renderer_text_columns, renderer_text_rows);
                if (result < TIGT_OK) tigt_terminal_record_error(result);
            }
            pthread_mutex_unlock(&renderer_mutex);
        }
        nanosleep(&interval, NULL);
    }
    return NULL;
}


static void
stop_session(void)
{
    pthread_mutex_lock(&renderer_mutex);
    renderer_active = false;
    pthread_mutex_unlock(&renderer_mutex);
    tigt_snapshot_session_stop(false);
    /* Joining, not cancellation, lets every application callback finish safely. */
    atomic_store_explicit(&input_running, false, memory_order_relaxed);
    atomic_store_explicit(&renderer_running, false, memory_order_relaxed);
    if (input_thread_created) {
        pthread_join(input_thread, NULL);
        input_thread_created = false;
    }
    if (renderer_thread_created) {
        pthread_join(renderer_thread, NULL);
        renderer_thread_created = false;
    }
    pthread_mutex_lock(&renderer_mutex);
    pointer_projection.valid = false;
    terminal_mouse_mode = TIGT_MOUSE_OFF;
    terminal_mouse_pixels = false;
    terminal_graphics.pending = false;
    pthread_mutex_unlock(&renderer_mutex);
    tigt_input_destroy(renderer_input);
    renderer_input = NULL;
    /* Workers are quiescent. Curses teardown must not write after an async
     * release or when another foreground process owns the terminal. */
    if (renderer_screen != NULL) {
        if (tigt_terminal_begin_io(1)) {
            leave_image_graphics();
            if (endwin() == OK && fflush(stdout) != EOF)
                tigt_terminal_screen(0);
            else
                tigt_terminal_record_error(TIGT_ERROR_SYSTEM);
            tigt_terminal_end_io();
        }
        delscreen(renderer_screen);
        renderer_screen = NULL;
    }
    tigt_ascii_destroy(renderer_ascii);
    renderer_ascii = NULL;
    if (terminal_captured) tigt_terminal_release();
}

static void
terminal_report(const char *parameters, uint8_t final, void *user)
{
    (void) user;
    const bool private = *parameters == '?';
    if (private) parameters++;
    unsigned values[32];
    size_t count = 0;
    bool mode_report = false;
    while (*parameters != '\0') {
        if (count == 32 || *parameters < '0' || *parameters > '9') return;
        unsigned value = 0;
        do {
            value = value * 10 + (unsigned) (*parameters++ - '0');
            if (value > 65535) return;
        } while (*parameters >= '0' && *parameters <= '9');
        values[count++] = value;
        if (*parameters == '\0') break;
        if (final == 'y' && parameters[0] == '$' && parameters[1] == '\0') {
            mode_report = true;
            break;
        }
        if (*parameters++ != ';' || *parameters == '\0') return;
    }
    pthread_mutex_lock(&renderer_mutex);
    if (terminal_graphics.pending) {
        if (mode_report && private && count == 2 && values[0] == 1016)
            terminal_mouse_pixels = values[1] >= 1 && values[1] <= 3;
        if (renderer_config.graphics_mode != TIGT_GRAPHICS_ITERM2 &&
            private && final == 'c' && count >= 2 &&
            (values[0] == 12 || (values[0] >= 62 && values[0] <= 65))) {
            for (size_t index = 1; index < count; index++)
                if (values[index] == 4) terminal_graphics.sixel = true;
        } else if (renderer_config.graphics_mode != TIGT_GRAPHICS_ITERM2 &&
                   private && final == 'S' && count == 4 &&
                   values[0] == 2 && values[1] == 0 && values[2] != 0 && values[3] != 0) {
            terminal_graphics.sixel = true;
            terminal_graphics.max_width = values[2];
            terminal_graphics.max_height = values[3];
        } else if (!private && final == 't' && count == 3 && values[1] != 0 && values[2] != 0) {
            if (values[0] == 4) {
                terminal_graphics.pixel_height = values[1];
                terminal_graphics.pixel_width = values[2];
            } else if (values[0] == 6) {
                terminal_graphics.cell_height = values[1];
                terminal_graphics.cell_width = values[2];
            }
        }
    }
    pthread_mutex_unlock(&renderer_mutex);
}

static int
resolve_terminal_modes(void)
{
    uint32_t selected = renderer_config.graphics_mode;
    const char *codeset = nl_langinfo(CODESET);
    const bool utf8 = strcmp(codeset, "UTF-8") == 0 || strcmp(codeset, "UTF8") == 0;
    struct stat input, output;
    const bool graphics_probe = selected == TIGT_GRAPHICS_AUTO || selected == TIGT_GRAPHICS_SIXEL ||
                                selected == TIGT_GRAPHICS_ITERM2;
    const bool mouse = renderer_config.on_mouse != NULL;
    const bool probe = tigt_terminal_is_foreground() && !tigt_terminal_is_released() &&
                       (graphics_probe || mouse) && renderer_input != NULL &&
                       fstat(STDIN_FILENO, &input) == 0 && fstat(STDOUT_FILENO, &output) == 0 &&
                       input.st_rdev == output.st_rdev && input.st_ino == output.st_ino;
    if (probe) {
        pthread_mutex_lock(&renderer_mutex);
        terminal_graphics.pending = true;
        pthread_mutex_unlock(&renderer_mutex);
        if (!tigt_terminal_begin_io(1)) return TIGT_ERROR_BACKGROUND;
        const char *query = graphics_probe && selected != TIGT_GRAPHICS_ITERM2 ?
                            "\033[16t\033[14t\033[?2;1;0S\033[c" : "\033[16t\033[14t";
        int output_result = fputs(query, stdout) == EOF ||
            (mouse && renderer_config.mouse_mode == TIGT_MOUSE_AUTO &&
             fputs("\033[?1016$p", stdout) == EOF) || fflush(stdout) == EOF;
        tigt_terminal_end_io();
        if (output_result) return TIGT_ERROR_SYSTEM;
        struct timespec start, now;
        if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return TIGT_ERROR_SYSTEM;
        const struct timespec interval = { .tv_nsec = 5000000 };
        do {
            nanosleep(&interval, NULL);
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return TIGT_ERROR_SYSTEM;
        } while ((now.tv_sec - start.tv_sec) * 1000000000LL + now.tv_nsec - start.tv_nsec < 150000000);
    }
    pthread_mutex_lock(&renderer_mutex);
    terminal_graphics.pending = false;
    if (selected == TIGT_GRAPHICS_AUTO)
        selected = terminal_graphics.sixel ? TIGT_GRAPHICS_SIXEL :
                   utf8 ? TIGT_GRAPHICS_BLOCKS : TIGT_GRAPHICS_ASCII;
    pthread_mutex_unlock(&renderer_mutex);
    if (selected == TIGT_GRAPHICS_BLOCKS && !utf8)
        return TIGT_ERROR_TERMINAL;
    if (selected == TIGT_GRAPHICS_ASCII) {
        if (!tigt_ascii_available()) return TIGT_ERROR_TERMINAL;
        renderer_ascii = tigt_ascii_create();
        if (renderer_ascii == NULL) return TIGT_ERROR_SYSTEM;
    }
    pthread_mutex_lock(&renderer_mutex);
    renderer_graphics_mode = selected;
    uint32_t mouse_mode = renderer_config.mouse_mode;
    if (mouse_mode == TIGT_MOUSE_AUTO)
        mouse_mode = terminal_mouse_pixels ? TIGT_MOUSE_PIXELS : TIGT_MOUSE_CELLS;
    terminal_mouse_mode = mouse_mode;
    pthread_mutex_unlock(&renderer_mutex);
    if (mouse) {
        pthread_mutex_lock(&renderer_mutex);
        if (renderer_input != NULL) tigt_input_set_mouse_mode(renderer_input, mouse_mode);
        pthread_mutex_unlock(&renderer_mutex);
        if (!tigt_terminal_begin_io(1)) return TIGT_ERROR_BACKGROUND;
        int result = tigt_terminal_mouse(mouse_mode);
        tigt_terminal_end_io();
        if (result != TIGT_OK) return result;
    }
    return TIGT_OK;
}

uint32_t
tigt_get_mouse_mode(void)
{
    pthread_mutex_lock(&renderer_mutex);
    const uint32_t mode = terminal_mouse_mode;
    pthread_mutex_unlock(&renderer_mutex);
    return mode;
}

uint32_t
tigt_get_graphics_mode(void)
{
    pthread_mutex_lock(&renderer_mutex);
    const uint32_t mode = renderer_graphics_mode;
    pthread_mutex_unlock(&renderer_mutex);
    return mode;
}

uint32_t
tigt_get_requested_graphics_mode(void)
{
    pthread_mutex_lock(&renderer_mutex);
    const uint32_t mode = renderer_config.graphics_mode;
    pthread_mutex_unlock(&renderer_mutex);
    return mode;
}

int
tigt_set_image_layout(uint16_t columns, uint16_t aspect_width, uint16_t aspect_height)
{
    if (columns == 0 || columns > TERMINAL_MAX_COLUMNS ||
        aspect_width == 0 || aspect_height == 0)
        return TIGT_ERROR_ARGUMENT;
    pthread_mutex_lock(&renderer_mutex);
    if (!renderer_initialized) {
        pthread_mutex_unlock(&renderer_mutex);
        return TIGT_ERROR_ARGUMENT;
    }
    if (renderer_image_layout.columns != columns ||
        renderer_image_layout.aspect_width != aspect_width ||
        renderer_image_layout.aspect_height != aspect_height) {
        renderer_image_layout = (image_layout_t) { columns, aspect_width, aspect_height };
        if (renderer_graphics_mode == TIGT_GRAPHICS_SIXEL ||
            renderer_graphics_mode == TIGT_GRAPHICS_ITERM2)
            renderer_frame_serial++;
    }
    pthread_mutex_unlock(&renderer_mutex);
    return TIGT_OK;
}

int
tigt_resume(void)
{
    static const int application_signals[] = { SIGINT, SIGQUIT, SIGTSTP, SIGWINCH };
    struct sigaction saved_signals[sizeof(application_signals) / sizeof(application_signals[0])];
    int result = TIGT_ERROR_TERMINAL;
    int thread_error;

    if (!renderer_initialized)
        return TIGT_ERROR_ARGUMENT;
    if (renderer_active) {
        if (tigt_terminal_is_foreground() && tigt_terminal_is_released()) {
            result = tigt_terminal_restore();
            if (result == TIGT_OK) result = tigt_terminal_poll();
            return result < TIGT_OK ? result : TIGT_OK;
        }
        return TIGT_OK;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return TIGT_ERROR_TERMINAL;
    session_requested = true;
    if (!terminal_captured) {
        result = tigt_terminal_capture(STDIN_FILENO, STDOUT_FILENO);
        if (result != TIGT_OK) return result;
        terminal_captured = true;
        tigt_terminal_set_owner(terminal_transition);
    }
    if (background_presenter == NULL) {
        tigt_presenter_config glass = {
            .abi_version = TIGT_PRESENTER_ABI_VERSION, .output_fd = STDOUT_FILENO,
            .mode = TIGT_PRESENT_GLASS, .encoding = TIGT_ENCODING_LOCALE
        };
        result = tigt_presenter_create(&glass, &background_presenter);
        if (result != TIGT_OK) goto failure;
    }
    if (setlocale(LC_CTYPE, "") == NULL) { result = TIGT_ERROR_TERMINAL; goto failure; }
    if (!tigt_terminal_is_foreground()) goto start_workers;
    result = tigt_terminal_restore();
    if (result != TIGT_OK) goto failure;
    result = TIGT_ERROR_TERMINAL;
    for (size_t index = 0; index < sizeof(application_signals) / sizeof(application_signals[0]); index++) {
        if (sigaction(application_signals[index], NULL, &saved_signals[index]) < 0) {
            result = TIGT_ERROR_SYSTEM;
            goto failure;
        }
    }
    /* Unlike initscr, newterm reports unavailable terminals without exiting.
       ncurses may install default job-control handlers: leave policy to the caller. */
    if (!tigt_terminal_begin_io(1)) { result = TIGT_ERROR_BACKGROUND; goto failure; }
    tigt_terminal_screen(1);
    renderer_screen = newterm(NULL, stdout, stdin);
    for (size_t index = 0; index < sizeof(application_signals) / sizeof(application_signals[0]); index++)
        sigaction(application_signals[index], &saved_signals[index], NULL);
    tigt_terminal_end_io();
    if (renderer_screen == NULL)
        goto failure;
    memset(&rendered_window, 0, sizeof(rendered_window));
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &rendered_window);
    if (rendered_window.ws_col == 0) rendered_window.ws_col = COLS;
    if (rendered_window.ws_row == 0) rendered_window.ws_row = LINES;
    pthread_mutex_lock(&renderer_mutex);
    memset(&terminal_graphics, 0, sizeof(terminal_graphics));
    terminal_graphics.columns = rendered_window.ws_col;
    terminal_graphics.rows = rendered_window.ws_row;
    pthread_mutex_unlock(&renderer_mutex);
    rendered_cells_valid = false;
    terminal_palette = TERMINAL_PALETTE_INVALID;
    memset(cga_pair_initialized, 0, sizeof(cga_pair_initialized));
    terminal_default_colors_available = false;
    terminal_cursor_style_valid = false;
    terminal_cursor_visible = false;
    terminal_cursor_position_valid = false;
    if (!tigt_terminal_begin_io(1)) { result = TIGT_ERROR_BACKGROUND; goto failure; }
    if (has_colors() && start_color() == OK)
        terminal_default_colors_available = use_default_colors() == OK;
    tigt_terminal_end_io();
    if (renderer_config.on_input != NULL || renderer_config.on_mouse != NULL) {
        renderer_input = tigt_input_create_with_mouse(
            terminal_input, NULL,
            renderer_config.on_mouse != NULL ? terminal_mouse : NULL, NULL, renderer_config.mouse_mode);
        if (renderer_input == NULL) {
            result = TIGT_ERROR_SYSTEM;
            goto failure;
        }
        tigt_input_set_terminal_report_callback(renderer_input, terminal_report, NULL);
        result = tigt_terminal_set_input_mode(1, renderer_config.on_input != NULL);
        if (result != TIGT_OK) goto failure;
    }
    if (!tigt_terminal_begin_io(1)) { result = TIGT_ERROR_BACKGROUND; goto failure; }
    curs_set(0);
    int refresh_result = refresh();
    tigt_terminal_end_io();
    if (refresh_result == ERR) goto failure;
    if (renderer_input != NULL) {
        atomic_store_explicit(&input_running, true, memory_order_relaxed);
        thread_error = pthread_create(&input_thread, NULL, input_main, NULL);
        if (thread_error != 0) {
            errno = thread_error;
            result = TIGT_ERROR_SYSTEM;
            goto failure;
        }
        input_thread_created = true;
    }
    result = resolve_terminal_modes();
    if (result != TIGT_OK) goto failure;
start_workers:
    atomic_store_explicit(&renderer_running, true, memory_order_relaxed);
    thread_error = pthread_create(&renderer_thread, NULL, renderer_main, NULL);
    if (thread_error != 0) {
        errno = thread_error;
        result = TIGT_ERROR_SYSTEM;
        goto failure;
    }
    renderer_thread_created = true;
    pthread_mutex_lock(&renderer_mutex);
    renderer_active = true;
    pthread_mutex_unlock(&renderer_mutex);
    tigt_snapshot_session_start();
    return TIGT_OK;

failure:
    stop_session();
    return result;
}

static void
terminal_transition(unsigned changes)
{
    if (!renderer_initialized || !session_requested) return;
    if ((changes & (TIGT_TERMINAL_RELEASED | TIGT_TERMINAL_RESTORED)) &&
        native_epoch != tigt_terminal_generation()) {
        bool foreground = tigt_terminal_is_foreground();
        bool restart = !tigt_terminal_is_faulted() &&
                       (!foreground || (changes & TIGT_TERMINAL_RESTORED));
        if (!foreground) {
            pthread_mutex_lock(&renderer_mutex);
            if (renderer_has_frame && renderer_bitmap_valid &&
                renderer_frame_serial != rendered_frame_serial)
                tigt_terminal_record_error(TIGT_ERROR_BACKGROUND);
            pthread_mutex_unlock(&renderer_mutex);
        }
        stop_session();
        if (restart) {
            int result = tigt_resume();
            if (result != TIGT_OK) tigt_terminal_record_error(result);
        }
        native_epoch = tigt_terminal_generation();
    }
    if (changes & TIGT_TERMINAL_RESIZED) {
        pthread_mutex_lock(&renderer_mutex);
        renderer_frame_serial++;
        pthread_mutex_unlock(&renderer_mutex);
    }
}

void
tigt_suspend(void)
{
    session_requested = false;
    if (renderer_initialized)
        stop_session();
}

int
tigt_init(const tigt_config *config)
{
    if (config == NULL || config->abi_version != TIGT_ABI_VERSION ||
        config->graphics_mode > TIGT_GRAPHICS_ITERM2 || config->mouse_mode > TIGT_MOUSE_X10 ||
        ((config->on_mouse == NULL) != (config->mouse_mode == TIGT_MOUSE_OFF)))
        return TIGT_ERROR_ARGUMENT;
    if (renderer_initialized)
        return TIGT_ERROR_BUSY;
    tigt_snapshot_session_reset();
    pthread_mutex_lock(&renderer_mutex);
    renderer_config = *config;
    renderer_has_frame = false;
    renderer_bitmap_valid = false;
    memset(&renderer_overscan, 0, sizeof(renderer_overscan));
    renderer_display_technology = TIGT_DISPLAY_GENERIC;
    renderer_text_output_seen = false;
    renderer_image_layout = default_image_layout;
    renderer_frame_serial = 0;
    rendered_frame_serial = 0;
    renderer_initialized = true;
    pthread_mutex_unlock(&renderer_mutex);
    int result = tigt_resume();
    if (result == TIGT_OK)
        result = tigt_snapshot_environment();

    if (result != TIGT_OK) {
        stop_session();
        tigt_presenter_destroy(background_presenter);
        background_presenter = NULL;
        if (terminal_captured) tigt_terminal_forget();
        terminal_captured = false;
        tigt_snapshot_session_stop(true);
        pthread_mutex_lock(&renderer_mutex);
        renderer_initialized = false;
        renderer_image_layout = default_image_layout;
        memset(&renderer_config, 0, sizeof(renderer_config));
        renderer_graphics_mode = TIGT_GRAPHICS_AUTO;
        pthread_mutex_unlock(&renderer_mutex);
    }
    return result;
}

void
tigt_shutdown(void)
{
    if (!renderer_initialized)
        return;
    session_requested = false;
    stop_session();
    tigt_presenter_destroy(background_presenter);
    background_presenter = NULL;
    if (terminal_captured) tigt_terminal_forget();
    terminal_captured = false;
    tigt_snapshot_session_stop(true);
    pthread_mutex_lock(&renderer_mutex);
    renderer_initialized = false;
    renderer_has_frame = false;
    renderer_bitmap_valid = false;
    memset(&renderer_overscan, 0, sizeof(renderer_overscan));
    renderer_display_technology = TIGT_DISPLAY_GENERIC;
    renderer_text_output_seen = false;
    renderer_graphics_mode = TIGT_GRAPHICS_AUTO;
    renderer_image_layout = default_image_layout;
    memset(&renderer_config, 0, sizeof(renderer_config));
    pthread_mutex_unlock(&renderer_mutex);
}

int
tigt_present_bitmap(const uint32_t *pixels, uint16_t width, uint16_t height,
                    uint16_t stride, uint8_t pixel_width)
{
    if (pixels == NULL || (width != 320 && width != 640) || height != 200 ||
        stride < width || (pixel_width != 1 && pixel_width != 2))
        return TIGT_ERROR_ARGUMENT;
    if (!renderer_active)
        return TIGT_ERROR_BUSY;
    if (!tigt_terminal_is_foreground()) {
        tigt_terminal_record_error(TIGT_ERROR_BACKGROUND);
        return TIGT_ERROR_BACKGROUND;
    }
    pthread_mutex_lock(&renderer_mutex);
    if (!renderer_active || tigt_terminal_is_released()) {
        pthread_mutex_unlock(&renderer_mutex);
        return TIGT_ERROR_BUSY;
    }
    if (!tigt_terminal_is_foreground()) {
        pthread_mutex_unlock(&renderer_mutex);
        tigt_terminal_record_error(TIGT_ERROR_BACKGROUND);
        return TIGT_ERROR_BACKGROUND;
    }
    bool changed = !renderer_bitmap_valid || renderer_bitmap_width != width ||
                   renderer_bitmap_pixel_width != pixel_width;
    for (uint16_t row = 0; row < height; row++) {
        for (uint16_t column = 0; column < width; column++) {
            const size_t index = (size_t) row * width + column;
            const uint32_t rgb = pixels[(size_t) row * stride + column] & 0xffffff;
            if (renderer_bitmap_pixels[index] != rgb) {
                renderer_bitmap_pixels[index] = rgb;
                changed = true;
            }
        }
    }
    renderer_bitmap_width = width;
    renderer_bitmap_pixel_width = pixel_width;
    renderer_bitmap_valid = true;
    renderer_has_frame = true;
    if (changed) renderer_frame_serial++;
    pthread_mutex_unlock(&renderer_mutex);
    return TIGT_OK;
}

int
tigt_present_indexed_bitmap(const uint8_t *indices, uint16_t width, uint16_t height,
                            uint16_t stride, uint8_t pixel_width,
                            const uint32_t *palette, uint16_t palette_size)
{
    if (indices == NULL || (width != 320 && width != 640) || height != 200 ||
        stride < width || (pixel_width != 1 && pixel_width != 2) ||
        (palette == NULL ? palette_size != 0 : palette_size == 0 || palette_size > 256))
        return TIGT_ERROR_ARGUMENT;
    if (palette == NULL) {
        palette = tigt_ibm16_palette;
        palette_size = 16;
    }
    for (uint16_t index = 0; index < palette_size; index++)
        if ((palette[index] & 0xff000000) != 0) return TIGT_ERROR_ARGUMENT;
    for (uint16_t row = 0; row < height; row++)
        for (uint16_t column = 0; column < width; column++)
            if (indices[(size_t) row * stride + column] >= palette_size)
                return TIGT_ERROR_ARGUMENT;
    if (!renderer_active) return TIGT_ERROR_BUSY;
    if (!tigt_terminal_is_foreground()) {
        tigt_terminal_record_error(TIGT_ERROR_BACKGROUND);
        return TIGT_ERROR_BACKGROUND;
    }
    pthread_mutex_lock(&renderer_mutex);
    if (!renderer_active || tigt_terminal_is_released()) {
        pthread_mutex_unlock(&renderer_mutex);
        return TIGT_ERROR_BUSY;
    }
    if (!tigt_terminal_is_foreground()) {
        pthread_mutex_unlock(&renderer_mutex);
        tigt_terminal_record_error(TIGT_ERROR_BACKGROUND);
        return TIGT_ERROR_BACKGROUND;
    }
    bool changed = !renderer_bitmap_valid || renderer_bitmap_width != width ||
                   renderer_bitmap_pixel_width != pixel_width;
    for (uint16_t row = 0; row < height; row++) {
        for (uint16_t column = 0; column < width; column++) {
            const size_t index = (size_t) row * width + column;
            const uint32_t rgb = palette[indices[(size_t) row * stride + column]];
            if (renderer_bitmap_pixels[index] != rgb) {
                renderer_bitmap_pixels[index] = rgb;
                changed = true;
            }
        }
    }
    renderer_bitmap_width = width;
    renderer_bitmap_pixel_width = pixel_width;
    renderer_bitmap_valid = true;
    renderer_has_frame = true;
    if (changed) renderer_frame_serial++;
    pthread_mutex_unlock(&renderer_mutex);
    return TIGT_OK;
}

/* Test resolved source cells, not sampled terminal frames or the cursor itself.
 * Nonbreaking spaces and the empty braille pattern are blank but not iswspace
 * in every supported locale. Underlining any blank can still make it visible. */
static bool
text_cell_has_output(const tigt_text_cell *cell)
{
    return cell->foreground != cell->background &&
           ((cell->flags & TIGT_TEXT_UNDERLINE) != 0 ||
            (!iswspace((wint_t) cell->codepoint) &&
             cell->codepoint != 0x00a0 && cell->codepoint != 0x2007 &&
             cell->codepoint != 0x202f && cell->codepoint != 0x2800));
}

int
tigt_present_text(const tigt_text_cell *cells, uint16_t columns, uint16_t rows,
                  uint16_t stride)
{
    bool cursor_present = false;

    if (cells == NULL || columns == 0 || columns > TERMINAL_MAX_COLUMNS ||
        rows == 0 || rows > TERMINAL_MAX_ROWS || stride < columns ||
        (size_t) columns * rows > TERMINAL_MAX_CELLS)
        return TIGT_ERROR_ARGUMENT;
    if (!renderer_active)
        return TIGT_ERROR_BUSY;
    for (uint16_t row = 0; row < rows; row++) {
        for (uint16_t column = 0; column < columns; column++) {
            const tigt_text_cell *cell = &cells[(size_t) row * stride + column];

            if ((cell->foreground & 0xff000000u) != 0 ||
                (cell->background & 0xff000000u) != 0 ||
                (cell->flags & ~(TIGT_TEXT_UNDERLINE | TIGT_TEXT_CURSOR)) != 0 ||
                cell->codepoint > 0x10ffff ||
                (cell->codepoint >= 0xd800 && cell->codepoint <= 0xdfff) ||
                cell->codepoint < 0x20 ||
                (cell->codepoint >= 0x7f && cell->codepoint <= 0x9f) ||
                wcwidth((wchar_t) cell->codepoint) != 1)
                return TIGT_ERROR_ARGUMENT;
            if ((cell->flags & TIGT_TEXT_CURSOR) != 0) {
                if (cursor_present)
                    return TIGT_ERROR_ARGUMENT;
                cursor_present = true;
            }
        }
    }
    if (!tigt_terminal_is_foreground() && !cursor_present)
        return TIGT_ERROR_UNREPRESENTABLE;
    if (tigt_terminal_is_foreground() && tigt_terminal_is_released())
        return TIGT_ERROR_BUSY;

    pthread_mutex_lock(&renderer_mutex);
    if (!renderer_active) {
        pthread_mutex_unlock(&renderer_mutex);
        return TIGT_ERROR_BUSY;
    }
    for (uint16_t row = 0; row < rows; row++) {
        tigt_text_cell *destination = renderer_text_cells + (size_t) row * columns;
        memcpy(destination, cells + (size_t) row * stride, columns * sizeof(*cells));
        if (renderer_display_technology == TIGT_DISPLAY_MDA && !renderer_text_output_seen) {
            for (uint16_t column = 0; column < columns; column++) {
                if (text_cell_has_output(&destination[column])) {
                    renderer_text_output_seen = true;
                    break;
                }
            }
        }
    }
    renderer_text_columns = columns;
    renderer_text_rows = rows;
    renderer_bitmap_valid = false;
    renderer_has_frame = true;
    renderer_frame_serial++;
    pthread_mutex_unlock(&renderer_mutex);
    return TIGT_OK;
}

int
tigt_set_display_technology(uint32_t technology)
{
    if (technology != TIGT_DISPLAY_GENERIC && technology != TIGT_DISPLAY_MDA)
        return TIGT_ERROR_ARGUMENT;
    pthread_mutex_lock(&renderer_mutex);
    if (!renderer_active) {
        pthread_mutex_unlock(&renderer_mutex);
        return TIGT_ERROR_BUSY;
    }
    if (renderer_display_technology != technology) {
        renderer_display_technology = technology;
        renderer_text_output_seen = false;
        renderer_frame_serial++;
    }
    pthread_mutex_unlock(&renderer_mutex);
    return TIGT_OK;
}

int
tigt_set_overscan(const tigt_overscan *overscan)
{
    if (overscan == NULL)
        return TIGT_ERROR_ARGUMENT;
    if (!renderer_active)
        return TIGT_ERROR_BUSY;
    if ((overscan->color & 0xff000000u) != 0)
        return TIGT_ERROR_ARGUMENT;
    pthread_mutex_lock(&renderer_mutex);
    if (!renderer_active) {
        pthread_mutex_unlock(&renderer_mutex);
        return TIGT_ERROR_BUSY;
    }
    renderer_overscan = *overscan;
    pthread_mutex_unlock(&renderer_mutex);
    return TIGT_OK;
}

int
tigt_get_overscan(tigt_overscan *overscan)
{
    if (overscan == NULL)
        return TIGT_ERROR_ARGUMENT;
    if (!renderer_active)
        return TIGT_ERROR_BUSY;
    pthread_mutex_lock(&renderer_mutex);
    if (!renderer_active) {
        pthread_mutex_unlock(&renderer_mutex);
        return TIGT_ERROR_BUSY;
    }
    *overscan = renderer_overscan;
    pthread_mutex_unlock(&renderer_mutex);
    return TIGT_OK;
}
