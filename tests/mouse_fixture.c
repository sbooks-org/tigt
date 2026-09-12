/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#include "../src/input.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct events {
    tigt_mouse_event mouse[128];
    tigt_input_event keys[32];
    size_t mouse_count, key_count, report_count, order_count;
    char order[256];
};

static void collect_mouse(const tigt_mouse_event *event, void *user)
{
    struct events *events = user;
    assert(events->mouse_count < sizeof(events->mouse) / sizeof(events->mouse[0]));
    assert(!(event->flags & (TIGT_MOUSE_FRAME_VALID | TIGT_MOUSE_INSIDE_FRAME)));
    if (event->kind == TIGT_MOUSE_DOWN || event->kind == TIGT_MOUSE_UP ||
        event->kind == TIGT_MOUSE_SCROLL) {
        assert(events->order_count != 0 && events->order[events->order_count - 1] == 'm');
        assert(events->mouse_count != 0);
        const tigt_mouse_event *previous = &events->mouse[events->mouse_count - 1];
        assert(previous->kind == TIGT_MOUSE_MOVE);
        assert(previous->x == event->x && previous->y == event->y);
        assert(previous->coordinates == event->coordinates);
        assert(previous->flags & TIGT_MOUSE_POSITION_VALID);
    }
    events->mouse[events->mouse_count++] = *event;
    assert(events->order_count + 1 < sizeof(events->order));
    events->order[events->order_count++] = 'm';
}

static void collect_key(const tigt_input_event *event, void *user)
{
    struct events *events = user;
    assert(events->key_count < sizeof(events->keys) / sizeof(events->keys[0]));
    events->keys[events->key_count++] = *event;
    assert(events->order_count + 1 < sizeof(events->order));
    events->order[events->order_count++] = 'k';
}

static void collect_report(const char *parameters, uint8_t final, void *user)
{
    struct events *events = user;
    assert(final == 'y');
    assert(strcmp(parameters, "?1016;2$") == 0);
    events->report_count++;
    assert(events->order_count + 1 < sizeof(events->order));
    events->order[events->order_count++] = 'r';
}

static void feed(tigt_input *input, const char *text)
{
    tigt_input_feed(input, (const uint8_t *) text, strlen(text));
}

static void check_interleaved_fragments(void)
{
    const char stream[] = "a\033[<28;5;6M\033[<32;6;7M\033[<2;6;7M"
                          "\033[<0;6;7m\033[<66;6;7M\033[M#&'"
                          "\033[?1016;2$y\033[1;5Ab";
    for (size_t split = 0; split < sizeof(stream); split++) {
        struct events events = {0};
        tigt_input *input = tigt_input_create_with_mouse(collect_key, &events,
                                                        collect_mouse, &events, TIGT_MOUSE_AUTO);
        assert(input != NULL);
        tigt_input_set_terminal_report_callback(input, collect_report, &events);
        tigt_input_feed(input, (const uint8_t *) stream, split);
        /* Timeout must not turn a fragmented mouse/report CSI into keys. */
        if (split != 0 && stream[split - 1] != '\033')
            tigt_input_flush(input);
        tigt_input_feed(input, (const uint8_t *) stream + split, sizeof(stream) - 1 - split);
        assert(events.mouse_count == 11 && events.key_count == 5 && events.report_count == 1);
        assert(strcmp(events.order, "kk" "mm" "m" "mm" "mm" "mm" "mm" "r" "kkk") == 0);
        assert(events.keys[0].key.character == 'a' && events.keys[4].key.character == 'b');
        assert(events.keys[2].key.kind == TIGT_KEY_UP);
        assert(events.keys[2].modifiers == TIGT_MOD_CONTROL);
        assert(events.mouse[0].x == 4 && events.mouse[0].y == 5);
        assert(events.mouse[0].coordinates == TIGT_MOUSE_COORD_CELLS);
        assert(events.mouse[1].kind == TIGT_MOUSE_DOWN);
        assert(events.mouse[1].button == TIGT_MOUSE_BUTTON_LEFT);
        assert(events.mouse[1].buttons == TIGT_MOUSE_LEFT);
        assert(events.mouse[1].modifiers == (TIGT_MOD_SHIFT | TIGT_MOD_ALT | TIGT_MOD_CONTROL));
        assert(events.mouse[2].kind == TIGT_MOUSE_MOVE && events.mouse[2].x == 5);
        assert(events.mouse[4].buttons == (TIGT_MOUSE_LEFT | TIGT_MOUSE_RIGHT));
        assert(events.mouse[6].kind == TIGT_MOUSE_UP && events.mouse[6].buttons == TIGT_MOUSE_RIGHT);
        assert(events.mouse[8].kind == TIGT_MOUSE_SCROLL && events.mouse[8].scroll_x == -1);
        assert(events.mouse[8].scroll_y == 0 && events.mouse[8].buttons == TIGT_MOUSE_RIGHT);
        assert(events.mouse[10].kind == TIGT_MOUSE_UP);
        assert(events.mouse[10].button == TIGT_MOUSE_BUTTON_RIGHT && events.mouse[10].buttons == 0);
        tigt_input_destroy(input);
    }
}

static void check_legacy_and_x10(void)
{
    const uint8_t chord[] = {27, '[', 'M', 32, 255, 233,
                            27, '[', 'M', 33, 255, 233,
                            27, '[', 'M', 35, 255, 233};
    struct events events = {0};
    tigt_input *input = tigt_input_create_with_mouse(NULL, NULL, collect_mouse, &events,
                                                    TIGT_MOUSE_CELLS);
    assert(input != NULL);
    for (size_t i = 0; i < sizeof(chord); i++) {
        tigt_input_feed(input, chord + i, 1);
        if (chord[i] != '\033')
            tigt_input_flush(input);
    }
    assert(events.mouse_count == 8);
    assert(events.mouse[1].x == 222 && events.mouse[1].y == 200);
    assert(events.mouse[3].button == TIGT_MOUSE_BUTTON_MIDDLE);
    assert(events.mouse[3].buttons == (TIGT_MOUSE_LEFT | TIGT_MOUSE_MIDDLE));
    assert(events.mouse[5].button == TIGT_MOUSE_BUTTON_LEFT);
    assert(events.mouse[5].buttons == TIGT_MOUSE_MIDDLE);
    assert(events.mouse[7].button == TIGT_MOUSE_BUTTON_MIDDLE && events.mouse[7].buttons == 0);
    tigt_input_set_mouse_mode(input, TIGT_MOUSE_X10);
    tigt_input_feed(input, chord, 6);
    assert(events.mouse_count == 12);
    assert(events.mouse[9].kind == TIGT_MOUSE_DOWN);
    assert(events.mouse[11].kind == TIGT_MOUSE_UP && events.mouse[11].buttons == 0);
    assert(events.mouse[11].flags & TIGT_MOUSE_SYNTHETIC);
    tigt_input_feed(input, chord + 12, 6);
    feed(input, "\033[<32;2;2M\033[<0;2;2m");
    assert(events.mouse_count == 12); /* X10 is press-only, never a stuck drag. */
    tigt_input_destroy(input);
}

static void check_malformed_and_disabled(void)
{
    const char invalid[] = "\033[<4294967296;1;1M\033[<0;4294967296;1M"
                           "\033[<0;2147483649;1M\033[<0;0;1M\033[<0;-1;1M"
                           "\033[<0;;1M\033[<0;1;1;2M\033[<128;1;1M\033[<256;1;1M"
                           "\033[<64;1;1m\033[<96;1;1M\033[?1A\033[<1A\033[<97u"
                           "\033[1$A\033[1$2A\033[32;1;1M"
                           "\033[<00000000000000000000000000000000000000000000000000000000000000000;1;1M"
                           "\033[M\033[Az";
    struct events events = {0};
    tigt_input *input = tigt_input_create_with_mouse(collect_key, &events,
                                                    collect_mouse, &events, TIGT_MOUSE_CELLS);
    assert(input != NULL);
    for (size_t i = 0; i < sizeof(invalid) - 1; i++)
        tigt_input_feed(input, (const uint8_t *) invalid + i, 1);
    assert(events.mouse_count == 0 && events.key_count == 2);
    assert(events.keys[0].key.character == 'z' && events.keys[1].key.character == 'z');
    tigt_input_set_mouse_mode(input, TIGT_MOUSE_OFF);
    feed(input, "\033[<0;1;1M\033[M !!q");
    assert(events.mouse_count == 0 && events.key_count == 4);
    assert(events.keys[2].key.character == 'q');
    tigt_input_set_mouse_mode(input, TIGT_MOUSE_CELLS);
    feed(input, "\033[<0;2147483648;1M");
    assert(events.mouse_count == 2 && events.mouse[1].x == INT32_MAX);
    tigt_input_destroy(input);

    memset(&events, 0, sizeof(events));
    input = tigt_input_create(collect_key, &events);
    assert(input != NULL);
    feed(input, "\033[<0;1;1M\033[M !!z");
    assert(events.mouse_count == 0 && events.key_count == 2);
    assert(events.keys[0].key.character == 'z');
    tigt_input_destroy(input);
}

static void check_pixel_leave_and_wheel(void)
{
    struct events events = {0};
    tigt_input *input = tigt_input_create_with_mouse(NULL, NULL, collect_mouse, &events,
                                                    TIGT_MOUSE_PIXELS);
    assert(input != NULL);
    /* Normal keyboard input and focus reports must be harmless in mouse-only mode. */
    feed(input, "a\033[A\033[I\033[O\033[<0;11;21M");
    assert(events.mouse_count == 2 && events.mouse[1].x == 10 && events.mouse[1].y == 20);
    assert(events.mouse[1].coordinates == TIGT_MOUSE_COORD_PIXELS);
    /* The actual Kitty leave bit wins over ALL other button/modifier bits. */
    feed(input, "\033[<511;-200;0M");
    assert(events.mouse_count == 3 && events.mouse[2].kind == TIGT_MOUSE_LEAVE);
    assert(events.mouse[2].flags == 0 && events.mouse[2].modifiers == 0);
    assert(events.mouse[2].button == TIGT_MOUSE_BUTTON_NONE);
    assert(events.mouse[2].buttons == TIGT_MOUSE_LEFT);
    feed(input, "\033[I\033[O");
    tigt_input_flush(input);
    assert(events.mouse_count == 3);
    tigt_input_set_mouse_mode(input, TIGT_MOUSE_CELLS);
    feed(input, "\033[<288;2;2M\033[<64;2;2M\033[<65;2;2M\033[<67;2;2M\033[<35;2;2M");
    assert(events.mouse_count == 10);
    assert(events.mouse[4].scroll_y == -1 && events.mouse[4].buttons == 0);
    assert(events.mouse[6].scroll_y == 1);
    assert(events.mouse[8].scroll_x == 1);
    assert(events.mouse[9].kind == TIGT_MOUSE_MOVE && events.mouse[9].buttons == 0);
    assert(events.mouse[9].coordinates == TIGT_MOUSE_COORD_CELLS);
    tigt_input_destroy(input);
}

int main(void)
{
    struct events events = {0};
    errno = 0;
    assert(tigt_input_create(NULL, NULL) == NULL && errno == EINVAL);
    assert(tigt_input_create_with_mouse(NULL, NULL, NULL, NULL, TIGT_MOUSE_OFF) == NULL);
    assert(tigt_input_create_with_mouse(collect_key, &events, NULL, NULL, TIGT_MOUSE_CELLS) == NULL);
    assert(tigt_input_create_with_mouse(NULL, NULL, collect_mouse, &events, TIGT_MOUSE_OFF) == NULL);
    assert(tigt_input_create_with_mouse(NULL, NULL, collect_mouse, &events, UINT32_MAX) == NULL);
    check_interleaved_fragments();
    check_legacy_and_x10();
    check_malformed_and_disabled();
    check_pixel_leave_and_wheel();
    puts("PASS interleaved fragmented mouse, release/order, bounds, X10 and Kitty leave");
    return 0;
}
