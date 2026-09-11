/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#include <tigt.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static atomic_bool stop;
static void on_input(const tigt_input_event *event, void *unused)
{
    (void)unused;
    if (event->kind == TIGT_PRESS && event->key.kind == TIGT_KEY_CHAR &&
        (event->key.character == 'q' ||
         ((event->modifiers & TIGT_MOD_CONTROL) && event->key.character == 'c')))
        atomic_store(&stop, 1);
}

int main(int argc, char **argv)
{
    int once = 0;
    uint32_t graphics = TIGT_GRAPHICS_AUTO;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--once")) {
            once = 1;
        } else if (!strcmp(argv[i], "--graphics") && i + 1 < argc) {
            const char *mode = argv[++i];
            if (!strcmp(mode, "auto")) graphics = TIGT_GRAPHICS_AUTO;
            else if (!strcmp(mode, "blocks")) graphics = TIGT_GRAPHICS_BLOCKS;
            else if (!strcmp(mode, "sixel")) graphics = TIGT_GRAPHICS_SIXEL;
            else if (!strcmp(mode, "ascii")) graphics = TIGT_GRAPHICS_ASCII;
            else {
                fprintf(stderr, "unknown graphics mode: %s\n", mode);
                return 2;
            }
        } else {
            fprintf(stderr, "usage: %s [--once] [--graphics auto|blocks|sixel|ascii]\n", argv[0]);
            return !strcmp(argv[i], "--help") ? 0 : 2;
        }
    }
    const tigt_config config = { TIGT_ABI_VERSION, once ? NULL : on_input, NULL, graphics };
    int result = tigt_init(&config);
    if (result != TIGT_OK) {
        fprintf(stderr, "tigt_init failed: %d (run inside a terminal)\n", result);
        return 1;
    }
    static uint32_t frame[320 * 200];
    const uint32_t colors[] = {0x000000, 0x0000c4, 0x00c400, 0xc47e00, 0xffffff};
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 320; ++x)
            frame[y * 320 + x] = colors[((x / 16) + (y / 16)) % 5];
    result = tigt_present_bitmap(frame, 320, 200, 320, 1);
    const struct timespec interval = {0, 20000000};
    for (int count = 0; result == TIGT_OK && !atomic_load(&stop) && (!once || count < 10); ++count)
        nanosleep(&interval, NULL);
    tigt_shutdown();
    return result == TIGT_OK ? 0 : 1;
}
