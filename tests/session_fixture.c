/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 * Public API consumer: linked to production sources, never source-included.
 */
#include "tigt.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static bool done;
static unsigned controls[2][3];
static volatile sig_atomic_t signals_seen;
static uint32_t pixels[648 * 200];
static unsigned acknowledged_stage;

static void signal_seen(int number)
{
    (void) number;
    signals_seen++;
}

static void on_input(const tigt_input_event *event, void *user)
{
    assert(user == controls);
    pthread_mutex_lock(&lock);
    if (event->key.kind == TIGT_KEY_CHAR && event->key.character == 'n' &&
        event->kind == TIGT_PRESS) {
        acknowledged_stage++;
        pthread_cond_signal(&ready);
    }
    if (event->key.kind == TIGT_KEY_CHAR && event->modifiers == TIGT_MOD_CONTROL &&
        (event->key.character == 'c' || event->key.character == 'z')) {
        assert(event->kind <= TIGT_RELEASE);
        controls[event->key.character == 'z'][event->kind]++;
    }
    if (event->key.kind == TIGT_KEY_CHAR && event->key.character == 'q' &&
        event->kind == TIGT_PRESS) {
        done = true;
        pthread_cond_signal(&ready);
    }
    pthread_mutex_unlock(&lock);
}

static void check_termios(const struct termios *expected)
{
    struct termios actual;
    assert(tcgetattr(STDIN_FILENO, &actual) == 0);
    assert(actual.c_iflag == expected->c_iflag);
    assert(actual.c_oflag == expected->c_oflag);
    assert(actual.c_cflag == expected->c_cflag);
    /* BSD sets PENDIN when canonical input is restored. It is kernel state
       requesting reprocessing of pending input, not a shell mode setting. */
    tcflag_t lflag_mask = (tcflag_t) -1;
#ifdef PENDIN
    lflag_mask &= ~PENDIN;
#endif
    assert((actual.c_lflag & lflag_mask) == (expected->c_lflag & lflag_mask));
    assert(memcmp(actual.c_cc, expected->c_cc, sizeof(actual.c_cc)) == 0);
    assert(cfgetispeed(&actual) == cfgetispeed(expected));
    assert(cfgetospeed(&actual) == cfgetospeed(expected));
}

static void check_invalid_frames(void)
{
    tigt_text_cell cell = { 'A', 0xffffff, 0, 0 };
    assert(tigt_present_bitmap(NULL, 640, 200, 640, 2) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_bitmap(pixels, 160, 200, 640, 1) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_bitmap(pixels, 640, 199, 640, 2) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_bitmap(pixels, 640, 200, 639, 2) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_bitmap(pixels, 640, 200, 640, 0) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_bitmap(pixels, 640, 200, 640, 3) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_text(NULL, 1, 1, 1) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_text(&cell, 0, 1, 1) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_text(&cell, 1, 0, 1) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_text(&cell, 321, 1, 321) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_text(&cell, 1, 129, 1) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_text(&cell, 320, 68, 320) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_text(&cell, 2, 1, 1) == TIGT_ERROR_ARGUMENT);
    const uint32_t invalid_codepoints[] = { 0, 10, 0x7f, 0x9f, 0xd800, 0x110000, 0x301, 0x4e00 };
    for (unsigned i = 0; i < sizeof(invalid_codepoints) / sizeof(invalid_codepoints[0]); i++) {
        cell.codepoint = invalid_codepoints[i];
        assert(tigt_present_text(&cell, 1, 1, 1) == TIGT_ERROR_ARGUMENT);
    }
    cell.codepoint = 'A';
    cell.flags = 4;
    assert(tigt_present_text(&cell, 1, 1, 1) == TIGT_ERROR_ARGUMENT);
    cell.flags = 0;
    cell.foreground = 0x1000000;
    assert(tigt_present_text(&cell, 1, 1, 1) == TIGT_ERROR_ARGUMENT);
    cell.foreground = 0;
    cell.background = 0x1000000;
    assert(tigt_present_text(&cell, 1, 1, 1) == TIGT_ERROR_ARGUMENT);
    tigt_text_cell cursors[2] = { { 'A', 0, 0, TIGT_TEXT_CURSOR }, { 'B', 0, 0, TIGT_TEXT_CURSOR } };
    assert(tigt_present_text(cursors, 2, 1, 2) == TIGT_ERROR_ARGUMENT);
    assert(tigt_set_overscan(NULL) == TIGT_ERROR_ARGUMENT);
    assert(tigt_get_overscan(NULL) == TIGT_ERROR_ARGUMENT);
    tigt_overscan invalid_border = { 0x1000000, 0, 0, 0, 0 };
    assert(tigt_set_overscan(&invalid_border) == TIGT_ERROR_ARGUMENT);
}

static void check_overscan(const tigt_overscan *expected)
{
    tigt_overscan actual;
    assert(tigt_get_overscan(&actual) == TIGT_OK);
    assert(actual.color == expected->color && actual.left == expected->left &&
           actual.right == expected->right && actual.top == expected->top &&
           actual.bottom == expected->bottom);
    /* Getter output is caller-owned; modifying it cannot alter stored state. */
    memset(&actual, 0xff, sizeof(actual));
    assert(tigt_get_overscan(&actual) == TIGT_OK);
    assert(actual.color == expected->color && actual.left == expected->left &&
           actual.right == expected->right && actual.top == expected->top &&
           actual.bottom == expected->bottom);
}

static void wait_for_stage(unsigned stage)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 20;
    pthread_mutex_lock(&lock);
    while (acknowledged_stage < stage) {
        int result = pthread_cond_timedwait(&ready, &lock, &deadline);
        if (result == ETIMEDOUT)
            fprintf(stderr, "PTY driver did not acknowledge text stage %u\n", stage);
        assert(result == 0);
    }
    assert(acknowledged_stage == stage);
    pthread_mutex_unlock(&lock);
}

static int check_resolved_text(const tigt_config *config, const struct termios *original)
{
    const tigt_overscan zero = { 0 };
    tigt_overscan border = { 0xf34ef3, 9, 17, 3, 11 };
    tigt_overscan copy = border;
    check_overscan(&zero);
    assert(tigt_set_overscan(&copy) == TIGT_OK);
    memset(&copy, 0, sizeof(copy));
    check_overscan(&border);
    tigt_suspend();
    check_termios(original);
    assert(tigt_set_overscan(&zero) == TIGT_ERROR_BUSY);
    assert(tigt_get_overscan(&copy) == TIGT_ERROR_BUSY);
    const tigt_text_cell blank = { ' ', 0, 0, 0 };
    assert(tigt_present_text(&blank, 1, 1, 1) == TIGT_ERROR_BUSY);
    assert(tigt_resume() == TIGT_OK);
    check_overscan(&border);

    /* Each step is acknowledged only after the PTY consumer sees its complete
       frame. Padding is deliberately invalid and not part of the text frame. */
    const uint32_t glyphs[8] = { 'A', 'B', 'C', 'D', 0x263a, 0xe9, 0x2500, 0x2588 };
    for (unsigned stage = 0; stage < 5; stage++) {
        if (stage == 1) {
            border = (tigt_overscan) { 0x4ef3f3, 23, 1, 15, 5 };
            assert(tigt_set_overscan(&border) == TIGT_OK);
        }
        if (stage == 3) {
            for (unsigned i = 0; i < sizeof(pixels) / sizeof(pixels[0]); i++)
                pixels[i] = 0x0000c4;
            assert(tigt_present_bitmap(pixels, 640, 200, 648, 2) == TIGT_OK);
            memset(pixels, 0xff, sizeof(pixels));
        } else {
            tigt_text_cell cells[10];
            memset(cells, 0xff, sizeof(cells));
            for (unsigned i = 0; i < 8; i++) {
                cells[i / 4 * 6 + i % 4] = (tigt_text_cell) {
                    glyphs[i], stage == 0 ? 0x00c400 : 0xc40000,
                    stage == 0 ? 0x0000c4 : 0x00c4c4,
                    stage == 2 ? 0 : i == 0 ? TIGT_TEXT_UNDERLINE : i == 1 ? TIGT_TEXT_CURSOR : 0
                };
            }
            assert(tigt_present_text(cells, 4, 2, 6) == TIGT_OK);
            memset(cells, 0, sizeof(cells));
        }
        check_overscan(&border);
        wait_for_stage(stage + 1);
    }
    tigt_shutdown();
    check_termios(original);
    assert(tigt_get_overscan(&copy) == TIGT_ERROR_BUSY);
    assert(tigt_set_overscan(&zero) == TIGT_ERROR_BUSY);
    assert(tigt_init(config) == TIGT_OK);
    check_overscan(&zero);
    tigt_shutdown();
    check_termios(original);
    puts("PASS resolved text colors, flags, transitions, copy and overscan lifecycle");
    return 0;
}

static int check_display_technology(const tigt_config *config, const struct termios *original,
                                    const char *directory)
{
    for (unsigned stage = 0; stage < 17; stage++) {
        if (stage == 4 || stage == 7) {
            tigt_suspend();
            check_termios(original);
            assert(tigt_set_display_technology(TIGT_DISPLAY_GENERIC) == TIGT_ERROR_BUSY);
            assert(tigt_resume() == TIGT_OK);
        }
        if (stage == 15 || stage == 16) {
            tigt_shutdown();
            check_termios(original);
            assert(tigt_set_display_technology(TIGT_DISPLAY_MDA) == TIGT_ERROR_BUSY);
            assert(tigt_init(config) == TIGT_OK);
        }
        if (stage == 9 || stage == 13)
            assert(tigt_set_display_technology(TIGT_DISPLAY_GENERIC) == TIGT_OK);
        if (stage != 0 && stage != 15)
            assert(tigt_set_display_technology(TIGT_DISPLAY_MDA) == TIGT_OK);
        if (stage == 2) {
            assert(tigt_set_display_technology(UINT32_MAX) == TIGT_ERROR_ARGUMENT);
            const tigt_text_cell rejected[] = {
                { 'X', 0xaaaaaa, 0, TIGT_TEXT_CURSOR },
                { 'Y', 0xaaaaaa, 0, TIGT_TEXT_CURSOR }
            };
            assert(tigt_present_text(rejected, 2, 1, 2) == TIGT_ERROR_ARGUMENT);
        }
        if (stage == 3 || stage == 8)
            assert(tigt_present_bitmap(pixels, 640, 200, 648, 2) == TIGT_OK);

        /* Technology-only stages must redraw the retained cursor, with no new
         * frame to disguise a missed invalidation. Other stages carry a unique
         * invisible glyph so the PTY driver can acknowledge actual rendering. */
        if (stage != 1 && stage != 12 && stage != 13) {
            tigt_text_cell cells[10];
            for (unsigned i = 0; i < 10; i++)
                cells[i] = (tigt_text_cell) { ' ', 0xaaaaaa, 0, 0 };
            cells[0] = (tigt_text_cell) { 'a' + stage, 0, 0, 0 };
            /* Visible padding must never count as first output. */
            cells[4] = cells[5] = (tigt_text_cell) { 'X', 0xaaaaaa, 0, 0 };
            if (stage == 2) {
                for (unsigned column = 0; column < 4; column++)
                    cells[6 + column] = (tigt_text_cell) { 'A', 0, 0, 0 };
            }
            if (stage == 3) {
                const uint32_t blanks[] = { 0x00a0, 0x2002, 0x202f, 0x2800 };
                for (unsigned column = 0; column < 4; column++)
                    cells[6 + column].codepoint = blanks[column];
            }
            if (stage == 5)
                cells[6].codepoint = 'X';
            if (stage == 10 || stage == 11) {
                cells[6].flags = TIGT_TEXT_UNDERLINE;
                if (stage == 10)
                    cells[6].foreground = 0;
            }
            cells[6 + stage % 4].flags |= TIGT_TEXT_CURSOR;
            assert(tigt_present_text(cells, 4, 2, 6) == TIGT_OK);
            memset(cells, 0, sizeof(cells));
        }
        char path[1024];
        assert(snprintf(path, sizeof(path), "%s/display-%u.json", directory, stage) <
               (int) sizeof(path));
        FILE *snapshot = fopen(path, "wb");
        assert(snapshot != NULL);
        assert(tigt_snapshot_write_fd(fileno(snapshot), TIGT_SNAPSHOT_ATTRIBUTES) == TIGT_OK);
        assert(fclose(snapshot) == 0);
        wait_for_stage(stage + 1);
    }
    tigt_shutdown();
    check_termios(original);
    return 0;
}

int main(int argc, char **argv)
{
    const tigt_config config = { TIGT_ABI_VERSION, on_input, controls, TIGT_GRAPHICS_BLOCKS };
    if (argc == 2 && strcmp(argv[1], "--no-terminal") == 0) {
        assert(tigt_init(&config) == TIGT_ERROR_TERMINAL);
        tigt_shutdown();
        puts("PASS no-terminal initialization returns an error");
        return 0;
    }
    struct termios original;
    assert(tcgetattr(STDIN_FILENO, &original) == 0);
    assert(signal(SIGINT, signal_seen) != SIG_ERR);
    assert(signal(SIGTSTP, signal_seen) != SIG_ERR);
    tigt_config invalid = config;
    invalid.abi_version++;
    assert(tigt_init(&invalid) == TIGT_ERROR_ARGUMENT);
    assert(tigt_init(&config) == TIGT_OK);
    assert(tigt_init(&config) == TIGT_ERROR_BUSY);
    check_invalid_frames();
    if (argc == 2 && strcmp(argv[1], "--text-tests") == 0)
        return check_resolved_text(&config, &original);
    if (argc == 3 && strcmp(argv[1], "--display-tests") == 0)
        return check_display_technology(&config, &original, argv[2]);
    tigt_suspend();
    check_termios(&original);
    assert(tigt_resume() == TIGT_OK);

    /* A padded source catches stride errors; mutate immediately after present
       so subsequent redraws must use the API-owned copy, not borrowed memory. */
    for (unsigned y = 0; y < 200; y++) {
        for (unsigned x = 0; x < 648; x++) {
            const unsigned code = (y / 8 * 40 + x / 16) % 128;
            const unsigned row = y % 8;
            const unsigned value = (code * 37u + row * 19u) ^ (code << (row % 3));
            const uint8_t bits = code == 0 ? 0 : code == 127 ? 255 : value & 255;
            const bool ink = (bits & (0x80 >> (x / 2 % 8))) != 0;
            /* Native bitmap producers may supply opaque ARGB: alpha is ignored. */
            pixels[y * 648 + x] = 0xff000000u | (x >= 640 ? 0xffffff : ink ? 0xc4c4c4 : 0);
        }
    }
    assert(tigt_present_bitmap(pixels, 640, 200, 648, 2) == TIGT_OK);
    memset(pixels, 0xff, sizeof(pixels));

    /* The PTY driver sends q only after independently decoding the exact full
       frame. There is no arbitrary sleep standing in for renderer completion. */
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 20;
    pthread_mutex_lock(&lock);
    while (!done) {
        int result = pthread_cond_timedwait(&ready, &lock, &deadline);
        if (result == ETIMEDOUT) {
            fprintf(stderr, "PTY driver did not acknowledge the exact rendered frame\n");
            break;
        }
        assert(result == 0);
    }
    pthread_mutex_unlock(&lock);
    tigt_shutdown();
    assert(done);
    for (unsigned key = 0; key < 2; key++) {
        assert(controls[key][TIGT_PRESS] == 2);
        assert(controls[key][TIGT_REPEAT] == 1);
        assert(controls[key][TIGT_RELEASE] == 2);
    }
    assert(signals_seen == 0);
    check_termios(&original);
    struct sigaction action;
    assert(sigaction(SIGINT, NULL, &action) == 0 && action.sa_handler == signal_seen);
    assert(sigaction(SIGTSTP, NULL, &action) == 0 && action.sa_handler == signal_seen);
    tigt_shutdown();
    check_termios(&original);
    puts("PASS copied frame, live controls, suspend/resume, normal shutdown and terminal restoration");
    return 0;
}
