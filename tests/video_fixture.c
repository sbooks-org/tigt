/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 * Public register/VRAM consumer, synchronized with observed terminal frames.
 */
#include "tigt_video.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static unsigned acknowledged;

static void on_input(const tigt_input_event *event, void *user)
{
    (void) user;
    if (event->key.kind != TIGT_KEY_CHAR || event->key.character != 'n' ||
        event->kind != TIGT_PRESS)
        return;
    pthread_mutex_lock(&lock);
    acknowledged++;
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&lock);
}

static void wait_for_stage(unsigned stage)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 20;
    pthread_mutex_lock(&lock);
    while (acknowledged < stage)
        assert(pthread_cond_timedwait(&ready, &lock, &deadline) == 0);
    pthread_mutex_unlock(&lock);
}

static void crtc(tigt_video *video, uint8_t index, uint8_t value)
{
    tigt_video_write(video, 0x3d4, index);
    tigt_video_write(video, 0x3d5, value);
}

int main(void)
{
    uint8_t vram[16384] = {0};
    tigt_video *video = tigt_video_create(TIGT_VIDEO_CGA);
    assert(video != NULL);
    crtc(video, 1, 40);
    crtc(video, 12, 0x1f);
    crtc(video, 13, 0xff);
    tigt_video_write(video, 0x3d8, 0x28);
    vram[16382] = 'H'; vram[16383] = 0x1e;
    vram[0] = 'i'; vram[1] = 0x1e;
    vram[78] = 'R'; vram[79] = 0x2f;

    /* Decode works before initialization, without a terminal session. */
    tigt_video_frame frame;
    assert(tigt_video_decode(video, vram, sizeof(vram), 1, &frame) == TIGT_OK);
    assert(frame.kind == TIGT_VIDEO_TEXT && frame.width == 40 && frame.height == 25);
    assert(frame.cells[0].codepoint == 'H' && frame.cells[1].codepoint == 'i');
    assert(frame.cells[40].codepoint == 'R');
    assert(tigt_video_present(video, vram, sizeof(vram), 1) == TIGT_ERROR_BUSY);

    const tigt_config config = { TIGT_ABI_VERSION, on_input, NULL, TIGT_GRAPHICS_BLOCKS };
    assert(tigt_init(&config) == TIGT_OK);
    assert(tigt_video_present(video, vram, sizeof(vram), 1) == TIGT_OK);
    wait_for_stage(1);

    /* Text -> 320 graphics -> 640 graphics grows/reuses the decoder buffer. */
    memset(vram, 0x55, sizeof(vram));
    tigt_video_write(video, 0x3d8, 0x0a);
    tigt_video_write(video, 0x3d9, 0);
    assert(tigt_video_present(video, vram, sizeof(vram), 1) == TIGT_OK);
    wait_for_stage(2);

    memset(vram, 0x80, sizeof(vram));
    tigt_video_write(video, 0x3d8, 0x1a);
    tigt_video_write(video, 0x3d9, 4);
    assert(tigt_video_present(video, vram, sizeof(vram), 1) == TIGT_OK);
    wait_for_stage(3);

    tigt_shutdown();
    tigt_video_destroy(video);
    return 0;
}
