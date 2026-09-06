/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#include "tigt.h"

#include <curses.h>
#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MDA_COLUMNS 80
#define MDA_ROWS 25
#define MDA_CELL_COUNT (MDA_COLUMNS * MDA_ROWS)
#define MDA_VRAM_SIZE 0x1000
#define MDA_CRTC_REGISTER_COUNT 32
#define MDA_CURSOR_HALF_PERIOD_NS 160590000ULL
#define MDA_TEXT_HALF_PERIOD_NS 321180000ULL
#define CGA_CURSOR_HALF_PERIOD_NS 133511348ULL
#define CGA_TEXT_HALF_PERIOD_NS 267022696ULL
#define MDA_INTENSE_REVERSE_PAIR 1
#define CGA_VRAM_SIZE 0x4000
#define CGA_640X200_COLUMNS 320
#define CGA_640X200_ROWS 67
#define TERMINAL_MAX_CELLS (CGA_640X200_COLUMNS * CGA_640X200_ROWS)

enum {
    MDA_CRTC_MAX_SCANLINE_ADDR = 9,
    MDA_CRTC_CURSOR_START = 10,
    MDA_CRTC_CURSOR_END = 11,
    MDA_CRTC_START_ADDR_HIGH = 12,
    MDA_CRTC_START_ADDR_LOW = 13,
    MDA_CRTC_CURSOR_ADDR_HIGH = 14,
    MDA_CRTC_CURSOR_ADDR_LOW = 15,
    MDA_MODE_VIDEO_ENABLE = 1 << 3,
    MDA_MODE_BLINK = 1 << 5
};

static pthread_mutex_t renderer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t renderer_thread;
static pthread_t input_thread;
static uint8_t renderer_vram[MDA_VRAM_SIZE];
static uint8_t renderer_crtc[MDA_CRTC_REGISTER_COUNT];
static uint8_t renderer_mode;
static uint8_t renderer_cga_vram[CGA_VRAM_SIZE];
static uint32_t renderer_bitmap_pixels[640 * 200];
static uint8_t renderer_bitmap_indices[640 * 200];
static uint16_t renderer_bitmap_width;
static uint8_t renderer_bitmap_pixel_width;
static bool renderer_bitmap_valid;
static bool renderer_is_cga;
static uint64_t renderer_cga_frame;
static bool renderer_has_frame;
static bool renderer_active;
static atomic_bool renderer_running;
static atomic_bool input_running;
static bool renderer_initialized;
static bool renderer_thread_created;
static bool input_thread_created;
static SCREEN *renderer_screen;
static tigt_config renderer_config;
static tigt_input *renderer_input;
static struct termios saved_input_termios;
static struct termios saved_output_termios;
static bool terminal_modes_saved;
static bool terminal_keyboard_enabled;
static uint64_t renderer_frame_serial;
static uint64_t rendered_frame_serial;
static bool terminal_bright_background_available;
static bool terminal_default_colors_available;
typedef enum {
    TERMINAL_PALETTE_INVALID,
    TERMINAL_PALETTE_TEXT,
    TERMINAL_PALETTE_BITMAP_NEITHER,
    TERMINAL_PALETTE_BITMAP_INTENSE,
    TERMINAL_PALETTE_BITMAP_NORMAL,
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
static int terminal_cursor_shape = -1;
static bool terminal_cursor_visible;
static bool terminal_cursor_position_valid;
static uint16_t terminal_cursor_position;
static uint64_t pending_cga_blank_frame;

static uint64_t
monotonic_nanoseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((uint64_t) now.tv_sec * 1000000000ULL) + now.tv_nsec;
}

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

static uint32_t
cp437_codepoint(uint8_t character)
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
cp437_utf8(uint8_t character, char output[8])
{
    if (character == 1) {
        memcpy(output, "\xe2\x98\xba\xef\xb8\x8e", 7);
        return output;
    }
    if (character == 19) {
        memcpy(output, "\xe2\x80\xbc\xef\xb8\x8e", 7);
        return output;
    }
    const uint32_t codepoint = cp437_codepoint(character);

    if (codepoint < 0x80) {
        output[0] = (char) codepoint;
        output[1] = '\0';
    } else if (codepoint < 0x800) {
        output[0] = (char) (0xc0 | (codepoint >> 6));
        output[1] = (char) (0x80 | (codepoint & 0x3f));
        output[2] = '\0';
    } else {
        output[0] = (char) (0xe0 | (codepoint >> 12));
        output[1] = (char) (0x80 | ((codepoint >> 6) & 0x3f));
        output[2] = (char) (0x80 | (codepoint & 0x3f));
        output[3] = '\0';
    }
    return output;
}

static bool
cell_is_black(uint8_t attribute)
{
    return attribute == 0x00 || attribute == 0x08 || attribute == 0x80 || attribute == 0x88;
}

static chtype
cell_attributes(uint8_t attribute, bool blink_mode)
{
    const uint8_t foreground = attribute & 0x0f;
    const uint8_t background = attribute >> 4;

    if (cell_is_black(attribute))
        return A_NORMAL;
    if (foreground == 1)
        return A_UNDERLINE;
    if (foreground == 9)
        return A_BOLD | A_UNDERLINE;
    if (foreground >= 2 && foreground <= 7)
        return A_NORMAL;
    if (foreground >= 0x0a)
        return A_BOLD;
    if (background == 7 || background == 0x0f)
        return A_REVERSE | ((background == 0x0f && !blink_mode) ? A_BOLD : A_NORMAL);
    return foreground == 8 ? A_BOLD : A_NORMAL;
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
cga_cell_attributes(uint8_t attribute, bool blink_mode)
{
    const uint8_t foreground = attribute & 0x0f;
    uint8_t background = attribute >> 4;

    select_terminal_palette(TERMINAL_PALETTE_TEXT);

    if (blink_mode)
        background &= 0x07;
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
                init_pair(pair, terminal_foreground, terminal_background);
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
    const bool themed_foreground = terminal_default_colors_available &&
                                  ((palette == TERMINAL_PALETTE_BITMAP_INTENSE && foreground == 15) ||
                                   (palette == TERMINAL_PALETTE_BITMAP_NORMAL && foreground == 7));
    const bool themed_background = terminal_default_colors_available && background == 0 &&
                                  palette != TERMINAL_PALETTE_BITMAP_EXPLICIT;
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

static const char *
cursor_glyph(uint8_t cursor_start, uint8_t cursor_end, uint8_t cursor_height, uint8_t cell_height)
{
    if (cursor_height == cell_height)
        return NULL;
    if (cursor_end < cursor_start || cursor_end < cell_height - 3)
        return NULL;
    if (cursor_height == 1 && cursor_start == cell_height - 2)
        return "_";

    switch (cursor_height) {
        case 1:
        case 2:
            return "\xe2\x96\x81";
        case 3:
        case 4:
            return "\xe2\x96\x82";
        case 5:
        case 6:
            return "\xe2\x96\x83";
        case 7:
            return "\xe2\x96\x84";
        case 8:
        case 9:
            return "\xe2\x96\x85";
        case 10:
        case 11:
            return "\xe2\x96\x86";
        case 12:
        case 13:
            return "\xe2\x96\x87";
        default:
            return NULL;
    }
}


static bool
set_cursor(bool visible, bool block)
{
    bool changed = false;

    if (visible && terminal_cursor_shape != block) {
        fputs(block ? "\033[2 q" : "\033[4 q", stdout);
        fflush(stdout);
        terminal_cursor_shape = block;
        changed = true;
    }
    if (visible != terminal_cursor_visible) {
        curs_set(visible ? 1 : 0);
        terminal_cursor_visible = visible;
        changed = true;
    }
    return changed;
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

    /* Scan every visible pixel, including pixels skipped by horizontal sampling
       or sextant color reduction. Padding below the last scanline is excluded. */
    for (size_t pixel = 0; pixel < (size_t) width * height; pixel++) {
        const uint8_t color = display_enabled ? cga_rendered_color_index(pixels[pixel]) : 0;

        renderer_bitmap_indices[pixel] = color;
        presence |= 1u << color;
    }
    const bool has_normal = (presence & (1u << 7)) != 0;
    const bool has_intense = (presence & (1u << 15)) != 0;
    const terminal_palette_t palette = has_normal && has_intense ? TERMINAL_PALETTE_BITMAP_EXPLICIT :
                                       has_intense ? TERMINAL_PALETTE_BITMAP_INTENSE :
                                       has_normal ? TERMINAL_PALETTE_BITMAP_NORMAL :
                                       TERMINAL_PALETTE_BITMAP_NEITHER;
    select_terminal_palette(palette);
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
            const uint8_t preferred_foreground = palette == TERMINAL_PALETTE_BITMAP_INTENSE ? 15 :
                                                 palette == TERMINAL_PALETTE_BITMAP_NORMAL ? 7 : 0;
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
    changed |= set_cursor(false, false);
    if (changed)
        refresh();
    rendered_cells_valid = true;
}


static void
render_mda(void)
{
    /* Only the renderer thread owns this snapshot; keep it off small pthread stacks. */
    static uint32_t bitmap_pixels[640 * 200];
    uint16_t bitmap_width;
    uint8_t bitmap_pixel_width;
    bool bitmap_valid;
    uint8_t vram[CGA_VRAM_SIZE];
    uint8_t crtc[MDA_CRTC_REGISTER_COUNT];
    uint8_t mode;
    bool is_cga;
    uint64_t cga_frame;
    const uint64_t now = monotonic_nanoseconds();


    pthread_mutex_lock(&renderer_mutex);
    if (!renderer_has_frame) {
        pthread_mutex_unlock(&renderer_mutex);
        return;
    }
    const uint64_t frame_serial = renderer_frame_serial;
    if (renderer_bitmap_valid && rendered_cells_valid && frame_serial == rendered_frame_serial) {
        pthread_mutex_unlock(&renderer_mutex);
        return;
    }
    is_cga = renderer_is_cga;
    cga_frame = renderer_cga_frame;
    bitmap_valid = renderer_bitmap_valid;
    bitmap_width = renderer_bitmap_width;
    bitmap_pixel_width = renderer_bitmap_pixel_width;
    if (bitmap_valid)
        memcpy(bitmap_pixels, renderer_bitmap_pixels, bitmap_width * 200 * sizeof(*bitmap_pixels));
    if (!bitmap_valid) {
        memcpy(vram, is_cga ? renderer_cga_vram : renderer_vram, is_cga ? CGA_VRAM_SIZE : MDA_VRAM_SIZE);
        memcpy(crtc, renderer_crtc, sizeof(crtc));
    }
    mode = renderer_mode;
    pthread_mutex_unlock(&renderer_mutex);
    if (bitmap_valid) {
        render_bitmap_graphics(bitmap_pixels, bitmap_width, 200, bitmap_pixel_width, true);
        rendered_frame_serial = frame_serial;
        return;
    }

    const uint16_t columns = is_cga ? crtc[1] : MDA_COLUMNS;
    const uint16_t rows = is_cga ? crtc[6] : MDA_ROWS;
    const uint16_t cell_count = columns * rows;
    const uint16_t address_mask = is_cga ? 0x3fff : 0x0fff;
    const bool text_mode = !is_cga || (mode & (1 << 1)) == 0;
    const uint64_t cursor_half_period = is_cga ? CGA_CURSOR_HALF_PERIOD_NS : MDA_CURSOR_HALF_PERIOD_NS;
    const uint64_t text_half_period = is_cga ? CGA_TEXT_HALF_PERIOD_NS : MDA_TEXT_HALF_PERIOD_NS;
    const bool cursor_blink_visible = (now / cursor_half_period) % 2 == 0;
    const bool text_blink_visible = (now / text_half_period) % 2 == 0;
    const uint16_t start = ((uint16_t) crtc[MDA_CRTC_START_ADDR_HIGH] << 8) | crtc[MDA_CRTC_START_ADDR_LOW];
    const uint16_t cursor = (((uint16_t) crtc[MDA_CRTC_CURSOR_ADDR_HIGH] << 8) | crtc[MDA_CRTC_CURSOR_ADDR_LOW]) & address_mask;
    const uint8_t cursor_start = crtc[MDA_CRTC_CURSOR_START] & 0x1f;
    const uint8_t cursor_end = crtc[MDA_CRTC_CURSOR_END] & 0x1f;
    const uint8_t cell_height = (crtc[MDA_CRTC_MAX_SCANLINE_ADDR] & 0x1f) + 1;
    const uint8_t cursor_height = cursor_end >= cursor_start ? cursor_end - cursor_start + 1 :
                                  cell_height - cursor_start + cursor_end + 1;
    const bool cursor_disabled = (crtc[MDA_CRTC_CURSOR_START] & 0x60) == 0x20;
    const bool cursor_in_display = cursor >= start && cursor < start + cell_count;
    const bool display_enabled = text_mode && (mode & MDA_MODE_VIDEO_ENABLE) != 0;
    if (columns == 0 || rows == 0 || cell_count > TERMINAL_MAX_CELLS)
        return;
    select_terminal_palette(TERMINAL_PALETTE_TEXT);

    if (is_cga && !display_enabled && rendered_cells_valid) {
        if (pending_cga_blank_frame == 0)
            pending_cga_blank_frame = cga_frame;
        if (cga_frame < pending_cga_blank_frame + 2)
            return;
    } else
        pending_cga_blank_frame = 0;
    if (!rendered_cells_valid || rendered_columns != columns || rendered_rows != rows) {
        erase();
        rendered_cells_valid = false;
        rendered_columns = columns;
        rendered_rows = rows;
    }
    const bool show_cursor = display_enabled && cursor_blink_visible && !cursor_disabled &&
                             cursor_height && cursor_height <= cell_height && cursor_in_display;
    const bool full_cell_cursor = show_cursor && cursor_height == cell_height;
    const uint8_t cursor_character = display_enabled ? vram[(cursor * 2) & address_mask] : ' ';
    const bool cursor_over_underscore = cursor_character == '_';
    const bool cursor_over_blank = cursor_character == 0 || cursor_character == ' ' || cursor_character == 0xff;
    const char *const software_cursor_glyph = show_cursor && !full_cell_cursor &&
                                                  (cursor_over_underscore || cursor_over_blank) ?
                                                  cursor_glyph(cursor_start, cursor_end, cursor_height, cell_height) :
                                                  NULL;
    const bool software_cursor = full_cell_cursor || software_cursor_glyph != NULL;
    const bool blink_mode = (mode & MDA_MODE_BLINK) != 0;

    bool changed = false;

    for (uint16_t row = 0; row < rows; row++) {
        for (uint16_t column = 0; column < columns; column++) {
            char utf8[8];
            const uint16_t address = (start + row * columns + column) & address_mask;
            const uint8_t attribute = display_enabled ? vram[((address * 2) + 1) & address_mask] : 0x07;
            const bool blinking_cell = blink_mode && (attribute & 0x80) != 0;
            const bool black_cell = !is_cga && cell_is_black(attribute);
            const uint8_t character = black_cell ? ' ' : (display_enabled ? vram[(address * 2) & address_mask] : ' ');
            const bool cursor_cell = software_cursor && address == cursor;
            const bool underscore_cursor = cursor_cell && cursor_height == 1 &&
                                           cursor_start == cell_height - 2 && character == '_';
            const size_t cell_index = row * columns + column;
            terminal_cell_t cell = {
                .style = display_enabled && !black_cell ?
                             (is_cga ? cga_cell_attributes(attribute, blink_mode) :
                                       cell_attributes(attribute, blink_mode)) :
                             A_NORMAL
            };
            const char *const glyph = cursor_cell && software_cursor_glyph != NULL ?
                                          (underscore_cursor ? "\xe2\x96\x81" : software_cursor_glyph) :
                                          cp437_utf8(character, utf8);

            if (full_cell_cursor && cursor_cell)
                cell.style = A_NORMAL | A_REVERSE | A_INVIS;
            if (blinking_cell && !text_blink_visible && !cursor_cell)
                cell.style |= A_INVIS;
            strcpy(cell.glyph, glyph);

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

    if (display_enabled && !cursor_disabled && cursor_in_display) {
        const uint16_t cursor_position = cursor - start;

        if (!terminal_cursor_position_valid || cursor_position != terminal_cursor_position) {
            move(cursor_position / columns, cursor_position % columns);
            terminal_cursor_position = cursor_position;
            terminal_cursor_position_valid = true;
            changed = true;
        }
    }
    changed |= set_cursor(show_cursor && !software_cursor, cursor_height > 7);
    if (changed)
        refresh();
    rendered_cells_valid = true;
}


static void *
input_main(void *unused)
{
    struct pollfd descriptor = { .fd = STDIN_FILENO, .events = POLLIN };
    uint8_t bytes[128];

    (void) unused;
    while (atomic_load_explicit(&input_running, memory_order_relaxed)) {
        const int ready = poll(&descriptor, 1, 25);

        if (!atomic_load_explicit(&input_running, memory_order_relaxed))
            break;
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0) {
            tigt_input_flush(renderer_input);
            continue;
        }
        if ((descriptor.revents & POLLIN) != 0) {
            const ssize_t length = read(STDIN_FILENO, bytes, sizeof(bytes));

            if (length > 0)
                tigt_input_feed(renderer_input, bytes, (size_t) length);
            else if (length == 0 || (errno != EINTR && errno != EAGAIN))
                break;
        } else if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            break;
        }
    }
    return NULL;
}

static void *
renderer_main(void *unused)
{
    const struct timespec interval = { .tv_sec = 0, .tv_nsec = 20000000 };

    (void) unused;
    while (atomic_load_explicit(&renderer_running, memory_order_relaxed)) {
        render_mda();
        nanosleep(&interval, NULL);
    }
    return NULL;
}

static void
restore_terminal_mode(int descriptor, const struct termios *mode)
{
    while (tcsetattr(descriptor, TCSANOW, mode) < 0 && errno == EINTR)
        ;
}

static void
stop_session(void)
{
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
    tigt_input_destroy(renderer_input);
    renderer_input = NULL;
    if (terminal_keyboard_enabled) {
        fputs("\033[<u", stdout);
        terminal_keyboard_enabled = false;
    }
    if (renderer_screen != NULL) {
        fputs("\033[0 q", stdout);
        curs_set(1);
        endwin();
        delscreen(renderer_screen);
        renderer_screen = NULL;
    }
    fflush(stdout);
    if (terminal_modes_saved) {
        restore_terminal_mode(STDOUT_FILENO, &saved_output_termios);
        restore_terminal_mode(STDIN_FILENO, &saved_input_termios);
        terminal_modes_saved = false;
    }
    renderer_active = false;
}

int
tigt_resume(void)
{
    static const int application_signals[] = { SIGINT, SIGQUIT, SIGTSTP };
    struct sigaction saved_signals[sizeof(application_signals) / sizeof(application_signals[0])];
    int result = TIGT_ERROR_TERMINAL;
    int thread_error;

    if (!renderer_initialized)
        return TIGT_ERROR_ARGUMENT;
    if (renderer_active)
        return TIGT_OK;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return TIGT_ERROR_TERMINAL;
    if (tcgetattr(STDIN_FILENO, &saved_input_termios) < 0 ||
        tcgetattr(STDOUT_FILENO, &saved_output_termios) < 0)
        return TIGT_ERROR_SYSTEM;
    terminal_modes_saved = true;
    if (setlocale(LC_CTYPE, "") == NULL)
        goto failure;
    for (size_t index = 0; index < sizeof(application_signals) / sizeof(application_signals[0]); index++) {
        if (sigaction(application_signals[index], NULL, &saved_signals[index]) < 0) {
            result = TIGT_ERROR_SYSTEM;
            goto failure;
        }
    }
    /* Unlike initscr, newterm reports unavailable terminals without exiting.
       ncurses may install default job-control handlers: leave policy to the caller. */
    renderer_screen = newterm(NULL, stdout, stdin);
    for (size_t index = 0; index < sizeof(application_signals) / sizeof(application_signals[0]); index++)
        sigaction(application_signals[index], &saved_signals[index], NULL);
    if (renderer_screen == NULL)
        goto failure;
    rendered_cells_valid = false;
    terminal_palette = TERMINAL_PALETTE_INVALID;
    memset(cga_pair_initialized, 0, sizeof(cga_pair_initialized));
    terminal_default_colors_available = false;
    terminal_bright_background_available = false;
    terminal_cursor_shape = -1;
    terminal_cursor_visible = false;
    terminal_cursor_position_valid = false;
    pending_cga_blank_frame = 0;
    if (has_colors() && start_color() == OK) {
        terminal_default_colors_available = use_default_colors() == OK;
        if (terminal_default_colors_available && COLORS >= 16 &&
            init_pair(MDA_INTENSE_REVERSE_PAIR, COLOR_WHITE + 8, -1) == OK)
            terminal_bright_background_available = true;
    }
    if (renderer_config.on_input != NULL) {
        renderer_input = tigt_input_create(renderer_config.on_input, renderer_config.user);
        if (renderer_input == NULL) {
            result = TIGT_ERROR_SYSTEM;
            goto failure;
        }
        if (raw() == ERR || noecho() == ERR)
            goto failure;
        terminal_keyboard_enabled = true;
        if (fputs("\033[>u\033[=11;1u", stdout) == EOF || fflush(stdout) == EOF)
            goto failure;
    }
    curs_set(0);
    if (refresh() == ERR)
        goto failure;
    atomic_store_explicit(&renderer_running, true, memory_order_relaxed);
    thread_error = pthread_create(&renderer_thread, NULL, renderer_main, NULL);
    if (thread_error != 0) {
        errno = thread_error;
        result = TIGT_ERROR_SYSTEM;
        goto failure;
    }
    renderer_thread_created = true;
    renderer_active = true;
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
    return TIGT_OK;

failure:
    stop_session();
    return result;
}

void
tigt_suspend(void)
{
    if (renderer_initialized)
        stop_session();
}

int
tigt_init(const tigt_config *config)
{
    if (config == NULL || config->abi_version != TIGT_ABI_VERSION)
        return TIGT_ERROR_ARGUMENT;
    if (renderer_initialized)
        return TIGT_ERROR_BUSY;
    renderer_config = *config;
    renderer_has_frame = false;
    renderer_bitmap_valid = false;
    renderer_cga_frame = 0;
    renderer_frame_serial = 0;
    rendered_frame_serial = 0;
    renderer_initialized = true;
    const int result = tigt_resume();

    if (result != TIGT_OK) {
        renderer_initialized = false;
        memset(&renderer_config, 0, sizeof(renderer_config));
    }
    return result;
}

void
tigt_shutdown(void)
{
    if (!renderer_initialized)
        return;
    stop_session();
    renderer_initialized = false;
    renderer_has_frame = false;
    renderer_bitmap_valid = false;
    memset(&renderer_config, 0, sizeof(renderer_config));
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
    pthread_mutex_lock(&renderer_mutex);
    for (uint16_t row = 0; row < height; row++)
        memcpy(renderer_bitmap_pixels + row * width, pixels + (size_t) row * stride, width * sizeof(*pixels));
    renderer_bitmap_width = width;
    renderer_bitmap_pixel_width = pixel_width;
    renderer_bitmap_valid = true;
    renderer_is_cga = false;
    renderer_has_frame = true;
    renderer_frame_serial++;
    pthread_mutex_unlock(&renderer_mutex);
    return TIGT_OK;
}

int
tigt_present_mda(const uint8_t *vram, const uint8_t *crtc, uint8_t mode)
{
    if (vram == NULL || crtc == NULL)
        return TIGT_ERROR_ARGUMENT;
    if (!renderer_active)
        return TIGT_ERROR_BUSY;
    pthread_mutex_lock(&renderer_mutex);
    memcpy(renderer_vram, vram, sizeof(renderer_vram));
    memcpy(renderer_crtc, crtc, sizeof(renderer_crtc));
    renderer_mode = mode;
    renderer_is_cga = false;
    renderer_bitmap_valid = false;
    renderer_has_frame = true;
    renderer_frame_serial++;
    pthread_mutex_unlock(&renderer_mutex);
    return TIGT_OK;
}

int
tigt_present_cga(const uint8_t *vram, const uint8_t *crtc, uint8_t mode,
                 int source_y, const uint32_t *pixels, uint16_t stride)
{
    if (vram == NULL || crtc == NULL || source_y < 0)
        return TIGT_ERROR_ARGUMENT;
    if ((mode & (1 << 1)) != 0 && pixels != NULL) {
        if (stride < 640 ||
            (size_t) source_y > ((size_t) PTRDIFF_MAX / sizeof(*pixels) - ((size_t) 199 * stride + 640)) / stride)
            return TIGT_ERROR_ARGUMENT;
        return tigt_present_bitmap(pixels + (size_t) source_y * stride, 640, 200, stride,
                                   (mode & (1 << 4)) != 0 ? 1 : 2);
    }
    if (crtc[1] == 0 || crtc[6] == 0 || (size_t) crtc[1] * crtc[6] > TERMINAL_MAX_CELLS)
        return TIGT_ERROR_ARGUMENT;
    if (!renderer_active)
        return TIGT_ERROR_BUSY;
    pthread_mutex_lock(&renderer_mutex);
    memcpy(renderer_cga_vram, vram, sizeof(renderer_cga_vram));
    renderer_bitmap_valid = false;
    memcpy(renderer_crtc, crtc, sizeof(renderer_crtc));
    renderer_mode = mode;
    renderer_is_cga = true;
    renderer_cga_frame++;
    renderer_has_frame = true;
    renderer_frame_serial++;
    pthread_mutex_unlock(&renderer_mutex);
    return TIGT_OK;
}
