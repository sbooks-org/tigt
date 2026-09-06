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
    const int once = argc == 2 && !strcmp(argv[1], "--once");
    const tigt_config config = { TIGT_ABI_VERSION, once ? NULL : on_input, NULL };
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
