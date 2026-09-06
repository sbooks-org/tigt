/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#include "tigt.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

struct events {
    tigt_input_event values[64];
    size_t count;
};

static volatile sig_atomic_t signals_seen;

static void signal_seen(int signal_number)
{
    (void) signal_number;
    signals_seen++;
}

static void collect(const tigt_input_event *event, void *user)
{
    struct events *events = user;
    assert(events->count < sizeof(events->values) / sizeof(events->values[0]));
    events->values[events->count++] = *event;
}

static void check(const struct events *events, size_t index, uint32_t key,
                  uint32_t character, uint8_t modifiers, uint8_t kind)
{
    assert(index < events->count);
    const tigt_input_event *event = &events->values[index];
    assert(event->key.kind == key);
    assert(event->key.character == character);
    assert(event->modifiers == modifiers);
    assert(event->kind == kind);
}

static void check_plain(void)
{
    struct events events = { 0 };
    tigt_input *input = tigt_input_create(collect, &events);
    assert(input != NULL);
    const uint8_t bytes[] = { 'x', 1, 3, 26, '\r', '\n', '\b', 127 };
    tigt_input_feed(input, bytes, sizeof(bytes));
    assert(events.count == 16);
    for (unsigned kind = 0; kind < 2; kind++) {
        const uint8_t event_kind = kind ? TIGT_RELEASE : TIGT_PRESS;
        check(&events, kind, TIGT_KEY_CHAR, 'x', 0, event_kind);
        check(&events, 2 + kind, TIGT_KEY_CHAR, 'a', TIGT_MOD_CONTROL, event_kind);
        check(&events, 4 + kind, TIGT_KEY_CHAR, 'c', TIGT_MOD_CONTROL, event_kind);
        check(&events, 6 + kind, TIGT_KEY_CHAR, 'z', TIGT_MOD_CONTROL, event_kind);
        check(&events, 8 + kind, TIGT_KEY_ENTER, 0, 0, event_kind);
        check(&events, 10 + kind, TIGT_KEY_ENTER, 0, 0, event_kind);
        check(&events, 12 + kind, TIGT_KEY_BACKSPACE, 0, 0, event_kind);
        check(&events, 14 + kind, TIGT_KEY_BACKSPACE, 0, 0, event_kind);
    }
    tigt_input_destroy(input);
}

static void check_incremental(void)
{
    const char bytes[] = "\033[99;5:1u\033[99;5:2u\033[99;5:3u"
                         "\033[122;5:1u\033[122;5:3u\033[1;5A\033[15~";
    /* Every split includes boundaries inside numeric parameters and finals. */
    for (size_t split = 0; split <= strlen(bytes); split++) {
        struct events events = { 0 };
        tigt_input *input = tigt_input_create(collect, &events);
        assert(input != NULL);
        tigt_input_feed(input, (const uint8_t *) bytes, split);
        tigt_input_feed(input, (const uint8_t *) bytes + split, strlen(bytes) - split);
        tigt_input_flush(input);
        assert(events.count == 7);
        check(&events, 0, TIGT_KEY_CHAR, 'c', TIGT_MOD_CONTROL, TIGT_PRESS);
        check(&events, 1, TIGT_KEY_CHAR, 'c', TIGT_MOD_CONTROL, TIGT_REPEAT);
        check(&events, 2, TIGT_KEY_CHAR, 'c', TIGT_MOD_CONTROL, TIGT_RELEASE);
        check(&events, 3, TIGT_KEY_CHAR, 'z', TIGT_MOD_CONTROL, TIGT_PRESS);
        check(&events, 4, TIGT_KEY_CHAR, 'z', TIGT_MOD_CONTROL, TIGT_RELEASE);
        check(&events, 5, TIGT_KEY_UP, 0, TIGT_MOD_CONTROL, TIGT_PRESS);
        check(&events, 6, TIGT_KEY_FUNCTION, 0, 0, TIGT_PRESS);
        assert(events.values[6].key.value == 5);
        tigt_input_destroy(input);
    }
}

static void check_escape_and_modifier(void)
{
    struct events events = { 0 };
    tigt_input *input = tigt_input_create(collect, &events);
    assert(input != NULL);
    tigt_input_feed(input, (const uint8_t *) "\033", 1);
    assert(events.count == 0);
    tigt_input_flush(input);
    assert(events.count == 2);
    check(&events, 0, TIGT_KEY_ESCAPE, 0, 0, TIGT_PRESS);
    check(&events, 1, TIGT_KEY_ESCAPE, 0, 0, TIGT_RELEASE);
    tigt_input_flush(input);
    assert(events.count == 2);
    const char sequence[] = "\033[57448;5:3u";
    for (size_t i = 0; i < strlen(sequence); i++)
        tigt_input_feed(input, (const uint8_t *) sequence + i, 1);
    assert(events.count == 3);
    assert(events.values[2].key.kind == TIGT_KEY_MODIFIER);
    assert(events.values[2].key.value == TIGT_RIGHT_CONTROL);
    assert(events.values[2].modifiers == TIGT_MOD_CONTROL);
    assert(events.values[2].kind == TIGT_RELEASE);
    tigt_input_destroy(input);
}

int main(void)
{
    assert(signal(SIGINT, signal_seen) != SIG_ERR);
    assert(signal(SIGTSTP, signal_seen) != SIG_ERR);
    check_plain();
    check_incremental();
    check_escape_and_modifier();
    assert(signals_seen == 0);
    puts("PASS semantic controls, incremental CSI/Kitty events, Escape timeout, modifier release");
    return 0;
}
