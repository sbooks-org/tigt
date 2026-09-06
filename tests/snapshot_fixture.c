/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 * Public C consumer. All captures are made by the production implementation.
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "tigt.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *directory;
static const char *extensions[] = {"png", "utf8", "ascii", "cp437", "ansi", "cells", "json"};
static volatile sig_atomic_t handled;

static void app_handler(int number) { handled = number; }

static void path_for(char path[1024], const char *name)
{
    int count = snprintf(path, 1024, "%s/%s", directory, name);
    assert(count > 0 && count < 1024);
}

static int capture(const char *name, uint32_t format)
{
    char path[1024];
    path_for(path, name);
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    assert(fd >= 0);
    int result = tigt_snapshot_write_fd(fd, format);
    assert(fcntl(fd, F_GETFD) >= 0); /* borrowed, even on failure */
    assert(close(fd) == 0);
    return result;
}

static void all_formats(const char *scene, bool png)
{
    for (uint32_t format = png ? 0 : 1; format <= TIGT_SNAPSHOT_ATTRIBUTES; ++format) {
        char name[128];
        snprintf(name, sizeof(name), "%s.%s", scene, extensions[format]);
        assert(capture(name, format) == TIGT_OK);
    }
}

static uint64_t sequence(void)
{
    uint64_t sequence;
    int result;
    assert(tigt_snapshot_status(&sequence, &result) == TIGT_OK);
    return sequence;
}

static void await_request(uint64_t before, int expected)
{
    struct timespec delay = {0, 1000000};
    for (int attempt = 0; attempt < 5000; ++attempt) {
        uint64_t after;
        int result;
        assert(tigt_snapshot_status(&after, &result) == TIGT_OK);
        if (after != before) {
            assert(after == before + 1);
            assert(result == expected);
            return;
        }
        nanosleep(&delay, NULL);
    }
    assert(!"snapshot worker did not complete within five seconds");
}

static void request(const char *name, uint32_t format, int expected)
{
    char path[1024];
    path_for(path, name);
    const int configured = tigt_snapshot_configure(SIGUSR1, format, path);
    if (configured != TIGT_OK) {
        struct sigaction action;
        sigaction(SIGUSR1, NULL, &action);
        fprintf(stderr, "configure %s: %d (errno=%d, handler=%p, flags=%x)\n",
                path, configured, errno, (void *) action.sa_handler, action.sa_flags);
    }
    assert(configured == TIGT_OK);
    memset(path, 'X', strlen(path)); /* configuration owns a copy */
    uint64_t before = sequence();
    assert(raise(SIGUSR1) == 0);
    await_request(before, expected);
}

static void expect_handler(int signal_number, void (*handler)(int))
{
    struct sigaction action;
    assert(sigaction(signal_number, NULL, &action) == 0);
    assert(action.sa_handler == handler);
}

static void install_handler(int signal_number, void (*handler)(int))
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    assert(sigaction(signal_number, &action, NULL) == 0);
}

static void check_signal_ownership(void)
{
    char path[1024];
    path_for(path, "owned.json");
    install_handler(SIGUSR2, app_handler);
    assert(tigt_snapshot_configure(SIGUSR2, TIGT_SNAPSHOT_ATTRIBUTES, path) == TIGT_ERROR_BUSY);
    assert(raise(SIGUSR2) == 0);
    assert(handled == SIGUSR2);
    expect_handler(SIGUSR2, app_handler);
    install_handler(SIGUSR2, SIG_IGN);
    assert(tigt_snapshot_configure(SIGUSR2, TIGT_SNAPSHOT_ATTRIBUTES, path) == TIGT_ERROR_BUSY);
    expect_handler(SIGUSR2, SIG_IGN);
    install_handler(SIGUSR2, SIG_DFL);
    assert(tigt_snapshot_configure(SIGUSR2, TIGT_SNAPSHOT_ATTRIBUTES, path) == TIGT_OK);
    uint64_t before = sequence();
    assert(raise(SIGUSR2) == 0);
    await_request(before, TIGT_OK);
    assert(tigt_snapshot_configure(0, 0, NULL) == TIGT_OK);
    expect_handler(SIGUSR2, SIG_DFL);
    assert(tigt_snapshot_configure(SIGUSR1, TIGT_SNAPSHOT_ATTRIBUTES, path) == TIGT_OK);
    install_handler(SIGUSR1, app_handler); /* do not clobber a later application change */
    assert(tigt_snapshot_configure(0, 0, NULL) == TIGT_OK);
    expect_handler(SIGUSR1, app_handler);
    assert(raise(SIGUSR1) == 0);
    assert(handled == SIGUSR1);
    install_handler(SIGUSR1, SIG_DFL);
}

static void text_scene(void)
{
    tigt_text_cell cells[10] = {
        {'A', 0x123456, 0x654321, TIGT_TEXT_UNDERLINE},
        {0xe9, 0x00aa00, 0x0000aa, 0},
        {0x2588, 0xffffff, 0, TIGT_TEXT_CURSOR},
        {' ', 0xffffff, 0, 0},
        {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
        {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
        {' ', 0, 0xffffff, 0},
        {'B', 0xaa0000, 0x00aaaa, 0},
        {0x250c, 0x555555, 0xaaaaaa, 0},
        {' ', 0xffffff, 0, 0},
    };
    assert(tigt_present_text(cells, 4, 2, 6) == TIGT_OK);
    memset(cells, 0, sizeof(cells)); /* not retained; padding was never a cell */
    assert(capture("missing-font.png", TIGT_SNAPSHOT_PNG) == TIGT_ERROR_ARGUMENT);
    assert(tigt_snapshot_set_font(NULL, 8) == TIGT_ERROR_ARGUMENT);
    uint8_t font[256 * 32];
    memset(font, 0, sizeof(font));
    assert(tigt_snapshot_set_font(font, 0) == TIGT_ERROR_ARGUMENT);
    assert(tigt_snapshot_set_font(font, 33) == TIGT_ERROR_ARGUMENT);
    assert(tigt_snapshot_set_font(font, 1) == TIGT_OK);
    assert(capture("height1.png", TIGT_SNAPSHOT_PNG) == TIGT_OK);
    assert(tigt_snapshot_set_font(font, 32) == TIGT_OK);
    assert(capture("height32.png", TIGT_SNAPSHOT_PNG) == TIGT_OK);
    for (unsigned glyph = 0; glyph < 256; ++glyph)
        for (unsigned row = 0; row < 8; ++row)
            font[glyph * 8 + row] = glyph == 32 ? 0 : (uint8_t)((glyph * 37 + row * 19) ^ (glyph << (row % 3)));
    assert(tigt_snapshot_set_font(font, 8) == TIGT_OK);
    memset(font, 0, sizeof(font));
    uint64_t before = sequence();
    all_formats("text", true);
    assert(sequence() == before); /* synchronous captures are not async completions */
    for (uint32_t format = 0; format <= TIGT_SNAPSHOT_ATTRIBUTES; ++format) {
        char name[128];
        snprintf(name, sizeof(name), "signal.%s", extensions[format]);
        request(name, format, TIGT_OK);
    }
    check_signal_ownership();
    assert(tigt_snapshot_set_font(NULL, 0) == TIGT_OK);
    assert(capture("cleared-font.png", TIGT_SNAPSHOT_PNG) == TIGT_ERROR_ARGUMENT);
    assert(tigt_snapshot_set_font(font, 8) == TIGT_OK);
    tigt_text_cell blank[6];
    for (size_t i = 0; i < 6; ++i) blank[i] = (tigt_text_cell){' ', 0x123456, 0x654321, 0};
    assert(tigt_present_text(blank, 3, 2, 3) == TIGT_OK);
    all_formats("blank", true);
    tigt_text_cell unmapped = {0x3bb, 0x123456, 0x654321, 0};
    assert(tigt_present_text(&unmapped, 1, 1, 1) == TIGT_OK);
    all_formats("unmapped", false);
    assert(capture("unmapped.png", TIGT_SNAPSHOT_PNG) == TIGT_ERROR_ARGUMENT);
    request("unmapped-signal.png", TIGT_SNAPSHOT_PNG, TIGT_ERROR_ARGUMENT);
}

static void bitmap_scenes(void)
{
    for (unsigned width = 320; width <= 640; width += 320) {
        unsigned stride = width + 5;
        size_t count = 199 * stride + width; /* no padding beyond last row */
        uint32_t *pixels = malloc(count * sizeof(*pixels));
        assert(pixels);
        for (unsigned pixel_width = 1; pixel_width <= 2; ++pixel_width) {
            for (size_t i = 0; i < count; ++i) pixels[i] = 0xdeadbeef;
            for (unsigned y = 0; y < 200; ++y)
                for (unsigned x = 0; x < width; ++x)
                    pixels[y * stride + x] = 0xab000000 | ((x & 255) << 16) | (y << 8) | ((x * 3 + y * 5) & 255);
            assert(tigt_present_bitmap(pixels, width, 200, stride, pixel_width) == TIGT_OK);
            memset(pixels, 0, count * sizeof(*pixels));
            char name[128];
            snprintf(name, sizeof(name), "bitmap-%u-%u.png", width, pixel_width);
            assert(capture(name, TIGT_SNAPSHOT_PNG) == TIGT_OK);
            snprintf(name, sizeof(name), "bitmap-%u-%u.json", width, pixel_width);
            assert(capture(name, TIGT_SNAPSHOT_ATTRIBUTES) == TIGT_OK);
            for (uint32_t format = TIGT_SNAPSHOT_UTF8; format <= TIGT_SNAPSHOT_CELLS; ++format)
                assert(capture("bitmap-invalid", format) == TIGT_ERROR_ARGUMENT);
        }
        free(pixels);
    }
    request("bitmap-signal.png", TIGT_SNAPSHOT_PNG, TIGT_OK);
    request("missing-parent/output.png", TIGT_SNAPSHOT_PNG, TIGT_ERROR_SYSTEM);
    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);
    assert(close(pipe_fds[0]) == 0);
#ifdef __APPLE__
    assert(fcntl(pipe_fds[1], F_GETNOSIGPIPE) == 0);
#endif
    assert(tigt_snapshot_write_fd(pipe_fds[1], TIGT_SNAPSHOT_PNG) == TIGT_ERROR_SYSTEM);
    assert(fcntl(pipe_fds[1], F_GETFD) >= 0);
#ifdef __APPLE__
    assert(fcntl(pipe_fds[1], F_GETNOSIGPIPE) == 0);
    assert(fcntl(pipe_fds[1], F_SETNOSIGPIPE, 1) == 0);
    assert(tigt_snapshot_write_fd(pipe_fds[1], TIGT_SNAPSHOT_PNG) == TIGT_ERROR_SYSTEM);
    assert(fcntl(pipe_fds[1], F_GETNOSIGPIPE) == 1);
#endif
    assert(close(pipe_fds[1]) == 0); /* broken pipe did not deliver SIGPIPE */
    char path[1024];
    path_for(path, "bitmap-640-2.png");
    int readonly = open(path, O_RDONLY);
    assert(readonly >= 0);
    assert(tigt_snapshot_write_fd(readonly, TIGT_SNAPSHOT_PNG) == TIGT_ERROR_SYSTEM);
    assert(close(readonly) == 0);
}

static double monotonic_seconds(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static void fifo_failures_and_shutdown(void)
{
    char path[1024];
    path_for(path, "capture.fifo");
    assert(mkfifo(path, 0600) == 0);
    request("capture.fifo", TIGT_SNAPSHOT_PNG, TIGT_ERROR_SYSTEM);
    int reader = open(path, O_RDONLY | O_NONBLOCK);
    assert(reader >= 0);
    int writer = open(path, O_WRONLY | O_NONBLOCK);
    assert(writer >= 0);
    uint8_t block[4096] = {0};
    while (write(writer, block, sizeof(block)) > 0) {}
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    double started = monotonic_seconds();
    request("capture.fifo", TIGT_SNAPSHOT_PNG, TIGT_ERROR_SYSTEM);
    assert(monotonic_seconds() - started < 2.0);
    /* An outstanding write to a full pipe must not hold shutdown hostage. */
    assert(raise(SIGUSR1) == 0);
    started = monotonic_seconds();
    tigt_shutdown();
    assert(monotonic_seconds() - started < 2.0);
    expect_handler(SIGUSR1, SIG_DFL);
    assert(close(writer) == 0);
    assert(close(reader) == 0);
    assert(unlink(path) == 0);
}

static void environment_opt_in(const tigt_config *config)
{
    for (int number = 1; number <= 2; ++number) {
        char path[1024], name[64];
        snprintf(name, sizeof(name), "environment%d.utf8", number);
        path_for(path, name);
        assert(setenv("TIGT_SNAPSHOT_PATH", path, 1) == 0);
        assert(setenv("TIGT_SNAPSHOT_FORMAT", "utf8", 1) == 0);
        if (number == 1) assert(unsetenv("TIGT_SNAPSHOT_SIGNAL") == 0);
        else assert(setenv("TIGT_SNAPSHOT_SIGNAL", "USR2", 1) == 0);
        assert(tigt_init(config) == TIGT_OK);
        tigt_text_cell cell = {'E', 0xffffff, 0, 0};
        assert(tigt_present_text(&cell, 1, 1, 1) == TIGT_OK);
        uint64_t before = sequence();
        assert(raise(number == 1 ? SIGUSR1 : SIGUSR2) == 0);
        await_request(before, TIGT_OK);
        tigt_shutdown();
        expect_handler(SIGUSR1, SIG_DFL);
        expect_handler(SIGUSR2, SIG_DFL);
    }
    assert(setenv("TIGT_SNAPSHOT_FORMAT", "not-a-format", 1) == 0);
    assert(tigt_init(config) == TIGT_ERROR_ARGUMENT);
    expect_handler(SIGUSR2, SIG_DFL);
    assert(unsetenv("TIGT_SNAPSHOT_PATH") == 0);
    assert(unsetenv("TIGT_SNAPSHOT_FORMAT") == 0);
    assert(unsetenv("TIGT_SNAPSHOT_SIGNAL") == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    directory = argv[1];
    tigt_config config = {.abi_version = TIGT_ABI_VERSION};
    assert(tigt_init(&config) == TIGT_OK);
    expect_handler(SIGUSR1, SIG_DFL);
    expect_handler(SIGUSR2, SIG_DFL);
    assert(sequence() == 0);
    assert(capture("no-frame", TIGT_SNAPSHOT_UTF8) == TIGT_ERROR_BUSY);
    assert(tigt_snapshot_write_fd(-1, TIGT_SNAPSHOT_UTF8) == TIGT_ERROR_ARGUMENT);
    assert(capture("invalid-format", 999) == TIGT_ERROR_ARGUMENT);
    request("no-frame-signal", TIGT_SNAPSHOT_UTF8, TIGT_ERROR_BUSY);
    assert(tigt_snapshot_configure(0, 0, NULL) == TIGT_OK);
    expect_handler(SIGUSR1, SIG_DFL);
    tigt_overscan overscan = {0x102030, 1, 2, 3, 4};
    assert(tigt_set_overscan(&overscan) == TIGT_OK);
    text_scene();
    char resumed_path[1024];
    path_for(resumed_path, "resumed.utf8");
    assert(tigt_snapshot_configure(SIGUSR1, TIGT_SNAPSHOT_UTF8, resumed_path) == TIGT_OK);
    tigt_suspend();
    uint64_t before = sequence();
    assert(raise(SIGUSR1) == 0);
    await_request(before, TIGT_ERROR_BUSY);
    assert(tigt_resume() == TIGT_OK);
    before = sequence();
    assert(raise(SIGUSR1) == 0);
    await_request(before, TIGT_OK);
    bitmap_scenes();
    fifo_failures_and_shutdown();
    assert(tigt_init(&config) == TIGT_OK);
    assert(sequence() == 0);
    assert(capture("new-session", TIGT_SNAPSHOT_PNG) == TIGT_ERROR_BUSY);
    tigt_text_cell cell = {'A', 0xffffff, 0, 0};
    assert(tigt_present_text(&cell, 1, 1, 1) == TIGT_OK);
    assert(capture("new-session-font.png", TIGT_SNAPSHOT_PNG) == TIGT_ERROR_ARGUMENT);
    tigt_shutdown();
    environment_opt_in(&config);
    puts("C snapshot fixture completed");
    return 0;
}
