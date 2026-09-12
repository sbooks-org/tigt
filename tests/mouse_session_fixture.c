/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#include "tigt.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static tigt_mouse_event events[32];
static size_t count;
static unsigned key_events;
static int finished;

static void mouse(const tigt_mouse_event *event, void *user)
{
    assert(user == events);
    pthread_mutex_lock(&mutex);
    assert(count < sizeof(events) / sizeof(*events));
    events[count++] = *event;
    if (event->kind == TIGT_MOUSE_SCROLL && event->scroll_y == 1) {
        finished = 1;
        pthread_cond_signal(&changed);
    }
    pthread_mutex_unlock(&mutex);
}

static void key(const tigt_input_event *event, void *user)
{
    assert(user == &key_events);
    assert(event->key.kind == TIGT_KEY_CHAR && event->key.character == 'k');
    key_events++;
}

static void restored(const struct termios *before)
{
    struct termios after;
    assert(tcgetattr(STDIN_FILENO, &after) == 0);
    assert(before->c_iflag == after.c_iflag && before->c_oflag == after.c_oflag);
    assert(before->c_cflag == after.c_cflag);
#ifdef PENDIN
    assert((before->c_lflag & ~PENDIN) == (after.c_lflag & ~PENDIN));
#else
    assert(before->c_lflag == after.c_lflag);
#endif
    assert(memcmp(before->c_cc, after.c_cc, sizeof(before->c_cc)) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 5);
    const uint32_t graphics = (uint32_t) strtoul(argv[1], NULL, 10);
    const uint32_t mode = (uint32_t) strtoul(argv[2], NULL, 10);
    const int combined = strcmp(argv[4], "combined") == 0;
    struct termios original;
    assert(tcgetattr(STDIN_FILENO, &original) == 0);
    const tigt_config config = {
        .abi_version = TIGT_ABI_VERSION, .graphics_mode = graphics,
        .on_input = combined ? key : NULL, .user = &key_events,
        .on_mouse = mouse, .mouse_user = events, .mouse_mode = mode
    };
    assert(tigt_init(&config) == TIGT_OK);
    static uint32_t pixels[640 * 200];
    for (size_t i = 0; i < sizeof(pixels) / sizeof(*pixels); i++) pixels[i] = 0x224499;
    tigt_text_cell text[40 * 25];
    for (size_t i = 0; i < sizeof(text) / sizeof(*text); i++)
        text[i] = (tigt_text_cell) { .codepoint = 'T', .foreground = 0xffffff };
    for (unsigned stage = 0; stage < 2; stage++) {
        if (stage != 0) assert(tigt_resume() == TIGT_OK);
        if (stage == 0)
            assert(tigt_present_bitmap(pixels, 640, 200, 640, 2) == TIGT_OK);
        else
            assert(tigt_present_text(text, 40, 25, 40) == TIGT_OK);
        const struct timespec settle = { .tv_nsec = 150000000 };
        nanosleep(&settle, NULL);
        FILE *progress = fopen(argv[3], "w");
        assert(progress != NULL);
        fprintf(progress, "%u %u\n", stage, tigt_get_mouse_mode());
        assert(fclose(progress) == 0);
        pthread_mutex_lock(&mutex);
        while (!finished) pthread_cond_wait(&changed, &mutex);
        pthread_mutex_unlock(&mutex);
        tigt_suspend();
        restored(&original);
        assert(tigt_get_mouse_mode() == TIGT_MOUSE_OFF);
        assert(key_events == (combined ? 2u * (stage + 1) : 0));
        putchar('\n');
        for (size_t i = 0; i < count; i++) {
            const tigt_mouse_event *e = &events[i];
            printf("@mouse %u %zu %u %u %u %u %d %d %.12f %.12f %d %d\n",
                   stage, i, e->kind, e->flags, e->button, e->buttons,
                   e->x, e->y, e->frame_x, e->frame_y, e->scroll_x, e->scroll_y);
        }
        fflush(stdout);
        count = 0;
        finished = 0;
    }
    tigt_shutdown();
    restored(&original);
    return 0;
}
