/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 *
 * Exercise the production renderer, not a reimplementation.
 * The Rust PTY integration tests compile and run this with wide curses.
 */
#define _XOPEN_SOURCE_EXTENDED 1
#include "../src/tigt.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static const uint32_t rgb[16] = {
    0x000000,0x0000c4,0x00c400,0x00c4c4,0xc40000,0xc400c4,0xc47e00,0xc4c4c4,
    0x4e4e4e,0x4e4edc,0x4edc4e,0x4ef3f3,0xdc4e4e,0xf34ef3,0xf3f34e,0xffffff
};
/* Extra storage deliberately poisoned in the visible-bounds regression. */
static uint32_t pixels[640 * 201];

enum policy { NEITHER, INTENSE, NORMAL, EXPLICIT };


static unsigned glyph_mask(wchar_t ch)
{
    /* Decode the public Unicode layout, independently of the renderer table. */
    if (ch == L' ') return 0;
    if (ch == 0x258c) return 21;
    if (ch == 0x2590) return 42;
    if (ch == 0x2588) return 63;
    if (ch >= 0x1fb00 && ch <= 0x1fb13) return ch - 0x1fb00 + 1;
    if (ch >= 0x1fb14 && ch <= 0x1fb27) return ch - 0x1fb14 + 22;
    if (ch >= 0x1fb28 && ch <= 0x1fb3b) return ch - 0x1fb28 + 43;
    assert(!"not a sextant glyph");
    return 0;
}

static void check_cell(int row, int column, const uint8_t expected[6], enum policy policy)
{
    cchar_t cell;
    wchar_t glyph[CCHARW_MAX];
    attr_t style;
    short pair, fg, bg;
    assert(mvwin_wch(stdscr, row, column, &cell) == OK);
    assert(getcchar(&cell, glyph, &style, &pair, NULL) == OK);
    assert(pair >= 0 && pair <= 255);
    assert(pair_content(pair, &fg, &bg) == OK);
    assert(!(style & (A_REVERSE | A_STANDOUT | A_INVIS)));
    bool normal = false, intense = false;
    for (unsigned bit = 0; bit < 6; bit++) {
        normal |= expected[bit] == 7;
        intense |= expected[bit] == 15;
    }
    const bool themed = policy != EXPLICIT && !(normal && intense);
    if (!themed || !intense)
        assert(!(style & A_BOLD));
    const unsigned mask = glyph_mask(glyph[0]);
    for (unsigned bit = 0; bit < 6; bit++) {
        const bool foreground = (mask & (1u << bit)) != 0;
        const short actual = foreground ? fg : bg;
        const uint8_t color = expected[bit];
        if (color == 0 && themed) {
            assert(!foreground && actual == -1);
        } else if ((color == 15 || color == 7) && themed) {
            assert(foreground && actual == -1);
            assert(!!(style & A_BOLD) == (color == 15));
        } else {
            assert(actual == cga_5153_xterm[color]);
        }
    }
}

static void fill(unsigned color)
{
    for (unsigned i = 0; i < sizeof(pixels) / sizeof(pixels[0]); i++)
        pixels[i] = rgb[color];
}

static void set_cell(unsigned row, unsigned column, unsigned mask, unsigned fg, unsigned bg)
{
    for (unsigned bit = 0; bit < 6; bit++) {
        unsigned x = column * 4 + (bit & 1) * 2;
        unsigned y = row * 3 + bit / 2;
        pixels[y * 640 + x] = pixels[y * 640 + x + 1] = rgb[(mask >> bit) & 1 ? fg : bg];
    }
}

static void check_masks(unsigned fg, unsigned bg, enum policy policy)
{
    for (unsigned mask = 0; mask < 64; mask++) {
        uint8_t expected[6];
        for (unsigned bit = 0; bit < 6; bit++)
            expected[bit] = (mask >> bit) & 1 ? fg : bg;
        check_cell(0, mask, expected, policy);
    }
}

static void check_transition(unsigned ink, unsigned rare, enum policy themed)
{
    fill(0);
    for (unsigned mask = 0; mask < 64; mask++)
        set_cell(0, mask, mask, ink, 0);
    render_bitmap_graphics(pixels, 640, 200, 2, true);
    check_masks(ink, 0, themed);
    /* A rare visible color outside the selected cells, even on a physical
       pixel skipped by sextant sampling, must change the WHOLE frame policy. */
    pixels[199 * 640 + 639] = rgb[rare];
    render_bitmap_graphics(pixels, 640, 200, 2, true);
    check_masks(ink, 0, EXPLICIT);
    pixels[199 * 640 + 639] = rgb[0];
    render_bitmap_graphics(pixels, 640, 200, 2, true);
    check_masks(ink, 0, themed);
}

static void check_policy(void)
{
    check_transition(15, 2, INTENSE);
    check_transition(7, 2, NORMAL);

    /* All three theme roles may coexist, including regular/emphasized cells.
       The latter retain explicit contrast because bold cannot style a background. */
    fill(0);
    for (unsigned mask = 0; mask < 64; mask++) {
        set_cell(0, mask, mask, 15, 7);
        set_cell(1, mask, mask, 7, 0);
    }
    render_bitmap_graphics(pixels, 640, 200, 2, true);
    check_masks(15, 7, NORMAL);
    for (unsigned mask = 0; mask < 64; mask++) {
        uint8_t expected[6];
        for (unsigned bit = 0; bit < 6; bit++)
            expected[bit] = (mask >> bit) & 1 ? 7 : 0;
        check_cell(1, mask, expected, NORMAL);
    }

    /* Exact RGB classification precedes quantization and sees unsampled pixels.
       A fourth distinct neutral gray also disables theming for the whole frame. */
    const uint32_t regular[] = { 0x818181, 0xaaaaaa, 0xcccccc, 0xfefefe };
    const uint8_t gray[6] = { 7, 7, 7, 7, 7, 7 };
    for (unsigned index = 0; index < sizeof(regular) / sizeof(regular[0]); index++) {
        fill(0);
        for (unsigned y = 0; y < 3; y++)
            for (unsigned x = 0; x < 4; x++) pixels[y * 640 + x] = regular[index];
        render_bitmap_graphics(pixels, 640, 200, 2, true);
        check_cell(0, 0, gray, NORMAL);
    }
    fill(7);
    pixels[199 * 640 + 639] = 0xc4c5c4;
    render_bitmap_graphics(pixels, 640, 200, 2, true);
    check_cell(0, 0, gray, EXPLICIT);
    pixels[199 * 640 + 639] = 0x808080;
    render_bitmap_graphics(pixels, 640, 200, 2, true);
    check_cell(0, 0, gray, EXPLICIT);
    pixels[199 * 640 + 638] = 0;
    pixels[199 * 640 + 639] = 0xffffff;
    pixels[199 * 640 + 637] = 0xcccccc;
    render_bitmap_graphics(pixels, 640, 200, 2, true);
    check_cell(0, 0, gray, EXPLICIT);

    /* Every unordered pair (including both orientations and same-color cells)
       shares the bounded bank without losing exact C colors. */
    fill(0);
    for (unsigned fg = 0; fg < 16; fg++)
        for (unsigned bg = 0; bg < 16; bg++)
            set_cell(1 + fg, bg, 0x13, fg, bg);
    render_bitmap_graphics(pixels, 640, 200, 2, true);
    for (unsigned fg = 0; fg < 16; fg++)
        for (unsigned bg = 0; bg < 16; bg++) {
            uint8_t expected[6];
            for (unsigned bit = 0; bit < 6; bit++)
                expected[bit] = (0x13 >> bit) & 1 ? fg : bg;
            check_cell(1 + fg, bg, expected, EXPLICIT);
        }

    /* One chromatic color rules out even default black for the whole frame. */
    for (unsigned fg = 0; fg < 16; fg++) {
        if (fg == 7 || fg == 15) continue;
        fill(0);
        for (unsigned mask = 0; mask < 64; mask++)
            set_cell(0, mask, mask, fg, 0);
        render_bitmap_graphics(pixels, 640, 200, 2, true);
        check_masks(fg, 0, fg == 0 ? NEITHER : EXPLICIT);
    }

    /* Last scanline remains distinct; only the nonexistent 201st is repeated. */
    fill(0);
    pixels[199 * 640] = pixels[199 * 640 + 1] = rgb[15];
    render_bitmap_graphics(pixels, 640, 200, 2, true);
    const uint8_t bottom[6] = { 0, 0, 15, 0, 15, 0 };
    check_cell(66, 0, bottom, INTENSE);

    /* Neither row padding nor memory after a narrower frame is visible. */
    fill(7);
    for (unsigned i = 0; i < 320 * 200; i++) pixels[i] = rgb[15];
    render_bitmap_graphics(pixels, 320, 200, 1, true);
    const uint8_t white[6] = { 15, 15, 15, 15, 15, 15 };
    check_cell(0, 0, white, INTENSE);
    check_cell(66, 159, white, INTENSE);
    render_bitmap_graphics(pixels, 320, 200, 1, false);
    const uint8_t black[6] = { 0, 0, 0, 0, 0, 0 };
    check_cell(0, 0, black, NEITHER);
    check_cell(66, 159, black, NEITHER);

    /* Solid themed ink must use a foreground full block, not default bg. */
    for (unsigned ink = 7; ink <= 15; ink += 8) {
        fill(ink);
        render_bitmap_graphics(pixels, 640, 200, 2, true);
        uint8_t expected[6];
        memset(expected, ink, sizeof(expected));
        check_cell(0, 0, expected, ink == 15 ? INTENSE : NORMAL);
    }
#if defined(TIGT_HAVE_LIBCACA) && TIGT_HAVE_LIBCACA
    /* libcaca may assign an unused regular foreground to a white space.
       Only its visible background should determine theme eligibility/style. */
    renderer_ascii = tigt_ascii_create();
    assert(renderer_ascii != NULL);
    renderer_graphics_mode = TIGT_GRAPHICS_ASCII;
    rendered_cells_valid = false;
    for (unsigned ink = 7; ink <= 15; ink += 8) {
        fill(ink);
        render_bitmap_graphics(pixels, 640, 200, 2, true);
        cchar_t cell;
        wchar_t glyph[CCHARW_MAX];
        attr_t style;
        short pair, foreground, background;
        assert(mvwin_wch(stdscr, 0, 0, &cell) == OK);
        assert(getcchar(&cell, glyph, &style, &pair, NULL) == OK);
        assert(pair_content(pair, &foreground, &background) == OK);
        assert(foreground == -1 && background == -1);
        assert(!!(style & A_BOLD) == (ink == 15));
    }
    tigt_ascii_destroy(renderer_ascii);
    renderer_ascii = NULL;
    renderer_graphics_mode = TIGT_GRAPHICS_BLOCKS;
#endif
}

static void synthetic_font(uint8_t font[128][8])
{
    /* Original deterministic patterns, including empty and full glyphs. */
    for (unsigned code = 0; code < 128; code++)
        for (unsigned row = 0; row < 8; row++) {
            unsigned value = (code * 37u + row * 19u) ^ (code << (row % 3));
            font[code][row] = code == 0 ? 0 : code == 127 ? 255 : value & 255;
        }
}

static int check_unsampled_output(void)
{
    const tigt_config config = {
        .abi_version = TIGT_ABI_VERSION, .graphics_mode = TIGT_GRAPHICS_BLOCKS
    };
    assert(tigt_init(&config) == TIGT_OK);
    /* Hold sampling, not submission: a transient output frame must release the
     * cursor even when the terminal only ever receives the subsequent clear. */
    atomic_store_explicit(&renderer_running, false, memory_order_relaxed);
    assert(pthread_join(renderer_thread, NULL) == 0);
    renderer_thread_created = false;
    assert(tigt_set_display_technology(TIGT_DISPLAY_MDA) == TIGT_OK);
    tigt_text_cell cells[4] = {
        { 'X', 0xaaaaaa, 0, 0 }, { ' ', 0xaaaaaa, 0, 0 },
        { ' ', 0xaaaaaa, 0, 0 }, { ' ', 0xaaaaaa, 0, TIGT_TEXT_CURSOR }
    };
    assert(tigt_present_text(cells, 4, 1, 4) == TIGT_OK);
    cells[0].codepoint = ' ';
    assert(tigt_present_text(cells, 4, 1, 4) == TIGT_OK);
    assert(tigt_set_display_technology(TIGT_DISPLAY_MDA) == TIGT_OK);
    render_frame();
    /* Output-only mode leaves canonical stdin intact. The driver responds only
     * after observing blank cells and the released cursor at column three. */
    assert(getchar() == 'n');
    tigt_shutdown();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--unsampled-output") == 0)
        return check_unsampled_output();
    const char *rom_path = NULL;
    unsigned long font_offset = 0xfa6e;
    unsigned argument = 1;
    bool policy_tests = argc == 2 && strcmp(argv[1], "--policy-tests") == 0;
    if (!policy_tests) {
        while (argument < (unsigned) argc && argv[argument][0] == '-') {
            if (argument + 1 >= (unsigned) argc) break;
            if (strcmp(argv[argument], "--rom") == 0)
                rom_path = argv[++argument];
            else if (strcmp(argv[argument], "--font-offset") == 0) {
                char *end;
                font_offset = strtoul(argv[++argument], &end, 0);
                if (*end != '\0') return 2;
            } else break;
            argument++;
        }
        if (argument + 2 != (unsigned) argc) {
            fprintf(stderr, "usage: renderer_fixture --policy-tests | "
                    "[--rom FILE] [--font-offset OFFSET] FOREGROUND BACKGROUND\n"
                    "Without --rom, uses an original synthetic font. ROM offset defaults to 0xfa6e.\n");
            return 2;
        }
    }
    uint8_t font[128][8];
    synthetic_font(font);
    if (rom_path != NULL) {
        FILE *rom = fopen(rom_path, "rb");
        if (rom == NULL) {
            perror(rom_path);
            return 2;
        }
        if (fseek(rom, (long) font_offset, SEEK_SET) != 0 ||
            fread(font, 1, sizeof(font), rom) != sizeof(font)) {
            fprintf(stderr, "%s: need 1024 font bytes at offset 0x%lx\n", rom_path, font_offset);
            fclose(rom);
            return 2;
        }
        fclose(rom);
    }
    unsigned fg = 0, bg = 0;
    if (!policy_tests) {
        char *end;
        fg = strtoul(argv[argument], &end, 0);
        if (*end != '\0' || fg >= 16) return 2;
        bg = strtoul(argv[argument + 1], &end, 0);
        if (*end != '\0' || bg >= 16) return 2;
    }
    assert(setlocale(LC_CTYPE, "") != NULL);
    assert(initscr() != NULL);
    assert(start_color() == OK);
    terminal_default_colors_available = use_default_colors() == OK;
    assert(terminal_default_colors_available && COLORS >= 256);
    assert(LINES >= 67 && COLS >= 320);
    if (policy_tests) {
        check_policy();
    } else {
        for (int y = 0; y < 200; y++)
            for (int x = 0; x < 320; x++) {
                int ch = (y / 8 * 40 + x / 8) % 128;
                int bit = (font[ch][y % 8] >> (7 - x % 8)) & 1;
                pixels[y * 640 + x * 2] = pixels[y * 640 + x * 2 + 1] = rgb[bit ? fg : bg];
            }
        render_bitmap_graphics(pixels, 640, 200, 2, true);
    }
    endwin();
    return 0;
}
