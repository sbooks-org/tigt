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

static void check_ss3(void)
{
    const struct {
        char final;
        uint32_t key, value;
    } keys[] = {
        { 'H', TIGT_KEY_HOME, 1 }, { 'F', TIGT_KEY_END, 1 },
        { 'A', TIGT_KEY_UP, 1 }, { 'B', TIGT_KEY_DOWN, 1 },
        { 'C', TIGT_KEY_RIGHT, 1 }, { 'D', TIGT_KEY_LEFT, 1 },
        { 'E', TIGT_KEY_KEYPAD_BEGIN, 0 },
        { 'P', TIGT_KEY_FUNCTION, 1 }, { 'Q', TIGT_KEY_FUNCTION, 2 },
        { 'R', TIGT_KEY_FUNCTION, 3 }, { 'S', TIGT_KEY_FUNCTION, 4 }
    };
    for (size_t key = 0; key < sizeof(keys) / sizeof(keys[0]); key++) {
        const uint8_t sequence[] = { '\033', 'O', (uint8_t) keys[key].final };
        for (size_t split = 0; split <= sizeof(sequence); split++) {
            struct events events = { 0 };
            tigt_input *input = tigt_input_create(collect, &events);
            assert(input != NULL);
            tigt_input_feed(input, sequence, split);
            if (split < sizeof(sequence))
                assert(events.count == 0); /* No premature Escape or literal O. */
            tigt_input_feed(input, sequence + split, sizeof(sequence) - split);
            tigt_input_flush(input);
            assert(events.count == 1);
            check(&events, 0, keys[key].key, 0, 0, TIGT_PRESS);
            if (keys[key].key == TIGT_KEY_FUNCTION)
                assert(events.values[0].key.value == keys[key].value);
            /* CSI R is a cursor report, not the SS3 F3 key. */
            const char report[] = "\033[12;34R";
            tigt_input_feed(input, (const uint8_t *) report, strlen(report));
            assert(events.count == 1);
            tigt_input_destroy(input);
        }
    }
}

static void check_terminal_reports(void)
{
    const char stream[] = "a\033[?62;4;22c\033[?2;0;640;400S\033[6;16;8tb";
    struct events events = {0};
    tigt_input *input = tigt_input_create(collect, &events);
    assert(input != NULL);
    for (size_t index = 0; index < sizeof(stream) - 1; index++)
        tigt_input_feed(input, (const uint8_t *) stream + index, 1);
    tigt_input_flush(input);
    assert(events.count == 4);
    check(&events, 0, TIGT_KEY_CHAR, 'a', 0, TIGT_PRESS);
    check(&events, 1, TIGT_KEY_CHAR, 'a', 0, TIGT_RELEASE);
    check(&events, 2, TIGT_KEY_CHAR, 'b', 0, TIGT_PRESS);
    check(&events, 3, TIGT_KEY_CHAR, 'b', 0, TIGT_RELEASE);
    tigt_input_destroy(input);
}

int main(void)
{
    assert(signal(SIGINT, signal_seen) != SIG_ERR);
    assert(signal(SIGTSTP, signal_seen) != SIG_ERR);
    check_plain();
    check_incremental();
    check_escape_and_modifier();
    check_ss3();
    check_terminal_reports();
    assert(signals_seen == 0);
    puts("PASS semantic controls, incremental CSI/Kitty events, Escape timeout, modifier release");
    return 0;
}
