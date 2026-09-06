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

static void signal_seen(int number)
{
    (void) number;
    signals_seen++;
}

static void on_input(const tigt_input_event *event, void *user)
{
    assert(user == controls);
    pthread_mutex_lock(&lock);
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
    uint8_t vram[TIGT_CGA_VRAM_SIZE] = { 0 };
    uint8_t crtc[TIGT_CRTC_SIZE] = { 0 };
    assert(tigt_present_bitmap(NULL, 640, 200, 640, 2) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_bitmap(pixels, 160, 200, 640, 1) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_bitmap(pixels, 640, 199, 640, 2) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_bitmap(pixels, 640, 200, 639, 2) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_bitmap(pixels, 640, 200, 640, 0) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_bitmap(pixels, 640, 200, 640, 3) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_mda(NULL, crtc, 8) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_mda(vram, NULL, 8) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_cga(NULL, crtc, 8, 0, NULL, 0) == TIGT_ERROR_ARGUMENT);
    assert(tigt_present_cga(vram, NULL, 8, 0, NULL, 0) == TIGT_ERROR_ARGUMENT);
}

int main(int argc, char **argv)
{
    const tigt_config config = { TIGT_ABI_VERSION, on_input, controls };
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
            pixels[y * 648 + x] = x >= 640 ? 0xffffff : ink ? 0xc4c4c4 : 0;
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
