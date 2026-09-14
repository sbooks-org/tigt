/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#include "input.h"

#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>

struct tigt_input {
    tigt_input_callback callback;
    void *user;
    tigt_terminal_report_callback on_report;
    void *report_user;
    tigt_mouse_callback on_mouse;
    void *mouse_user;
    _Atomic uint32_t mouse_mode;
    uint32_t applied_mouse_mode;
    uint32_t mouse_buttons;
    uint8_t legacy_mouse[3];
    uint8_t legacy_remaining;
    char sequence[64];
    size_t sequence_length;
    bool discard_sequence;
    bool csi_intermediate;
    bool paste;
    uint8_t paste_end_length;
    uint32_t utf8_codepoint;
    uint32_t utf8_minimum;
    uint8_t utf8_remaining;
};

static void
input_event(tigt_input *input, uint32_t key_kind, uint32_t key_value,
            uint32_t character, uint8_t modifiers, uint8_t kind)
{
    const tigt_input_event event = {
        .key = { .kind = key_kind, .value = key_value, .character = character },
        .modifiers = modifiers,
        .kind = kind,
        .flags = input->paste ? TIGT_INPUT_PASTE : 0
    };

    if (input->callback != NULL)
        input->callback(&event, input->user);
}

static void
input_tap(tigt_input *input, uint32_t key_kind, uint32_t character, uint8_t modifiers)
{
    input_event(input, key_kind, 0, character, modifiers, TIGT_PRESS);
    input_event(input, key_kind, 0, character, modifiers, TIGT_RELEASE);
}

static uint8_t
kitty_modifiers(uint32_t modifiers)
{
    const uint32_t bits = modifiers == 0 ? 0 : modifiers - 1;
    uint8_t result = 0;

    if (bits & 1)
        result |= TIGT_MOD_SHIFT;
    if (bits & 2)
        result |= TIGT_MOD_ALT;
    if (bits & 4)
        result |= TIGT_MOD_CONTROL;
    if (bits & 8)
        result |= TIGT_MOD_SUPER;
    return result;
}

static bool
parse_number(const char **cursor, uint32_t *value)
{
    const char *position = *cursor;
    uint32_t number = 0;

    if (*position < '0' || *position > '9')
        return false;
    do {
        const uint32_t digit = (uint32_t) (*position - '0');

        if (number > (UINT32_MAX - digit) / 10)
            return false;
        number = number * 10 + digit;
        position++;
    } while (*position >= '0' && *position <= '9');
    *cursor = position;
    *value = number;
    return true;
}

static bool
parse_csi_modifiers(const char *end, uint32_t *modifiers, uint32_t *event_kind)
{
    *modifiers = 1;
    *event_kind = 1;
    if (*end == ';') {
        end++;
        if (!parse_number(&end, modifiers))
            return false;
        if (*end == ':') {
            end++;
            if (!parse_number(&end, event_kind))
                return false;
        }
    }
    return *end == '\0' && *event_kind >= 1 && *event_kind <= 3;
}

static void
handle_kitty_key(tigt_input *input, const char *parameters)
{
    const char *end = parameters;
    uint32_t codepoint;
    uint32_t modifiers;
    uint32_t event_kind;
    uint32_t key_kind = TIGT_KEY_CHAR;
    uint32_t key_value = 0;

    if (!parse_number(&end, &codepoint) || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff))
        return;
    while (*end == ':') {
        uint32_t alternate;

        end++;
        if (*end >= '0' && *end <= '9' && !parse_number(&end, &alternate))
            return;
    }
    if (!parse_csi_modifiers(end, &modifiers, &event_kind))
        return;

    switch (codepoint) {
        case 27: key_kind = TIGT_KEY_ESCAPE; break;
        case 13: key_kind = TIGT_KEY_ENTER; break;
        case 127: key_kind = TIGT_KEY_BACKSPACE; break;
        case 57358: key_kind = TIGT_KEY_FUNCTION; key_value = 16; break;
        case 57359: key_kind = TIGT_KEY_SCROLL_LOCK; break;
        case 57360: key_kind = TIGT_KEY_NUM_LOCK; break;
        case 57361: key_kind = TIGT_KEY_PRINT_SCREEN; break;
        case 57362: key_kind = TIGT_KEY_PAUSE; break;
        case 57441: key_kind = TIGT_KEY_MODIFIER; key_value = TIGT_LEFT_SHIFT; break;
        case 57442: key_kind = TIGT_KEY_MODIFIER; key_value = TIGT_LEFT_CONTROL; break;
        case 57443: key_kind = TIGT_KEY_MODIFIER; key_value = TIGT_LEFT_ALT; break;
        case 57444: key_kind = TIGT_KEY_MODIFIER; key_value = TIGT_LEFT_SUPER; break;
        case 57447: key_kind = TIGT_KEY_MODIFIER; key_value = TIGT_RIGHT_SHIFT; break;
        case 57448: key_kind = TIGT_KEY_MODIFIER; key_value = TIGT_RIGHT_CONTROL; break;
        case 57449: key_kind = TIGT_KEY_MODIFIER; key_value = TIGT_RIGHT_ALT; break;
        case 57450: key_kind = TIGT_KEY_MODIFIER; key_value = TIGT_RIGHT_SUPER; break;
        case 0xf746: key_kind = TIGT_KEY_INSERT; break;
        default:
            if (codepoint >= 57364 && codepoint <= 57398) {
                key_kind = TIGT_KEY_FUNCTION;
                key_value = codepoint - 57363;
            }
    }
    input_event(input, key_kind, key_value, codepoint, kitty_modifiers(modifiers),
                (uint8_t) (event_kind - 1));
}

static void
handle_csi_key(tigt_input *input, const char *parameters, uint8_t final)
{
    const char *end = parameters;
    uint32_t key_value = 1;
    uint32_t modifiers;
    uint32_t event_kind;
    uint32_t key_kind;

    if (*end != '\0' && *end != ';' && !parse_number(&end, &key_value))
        return;
    if (!parse_csi_modifiers(end, &modifiers, &event_kind))
        return;

    switch (final) {
        case 'A': key_kind = TIGT_KEY_UP; break;
        case 'B': key_kind = TIGT_KEY_DOWN; break;
        case 'C': key_kind = TIGT_KEY_RIGHT; break;
        case 'D': key_kind = TIGT_KEY_LEFT; break;
        case 'H': key_kind = TIGT_KEY_HOME; break;
        case 'F': key_kind = TIGT_KEY_END; break;
        case 'P': key_kind = TIGT_KEY_FUNCTION; key_value = 1; break;
        case 'Q': key_kind = TIGT_KEY_FUNCTION; key_value = 2; break;
        case 'S': key_kind = TIGT_KEY_FUNCTION; key_value = 4; break;
        case '~':
            switch (key_value) {
                case 2: key_kind = TIGT_KEY_INSERT; break;
                case 200: input->paste = true; return;
                case 3: key_kind = TIGT_KEY_DELETE; break;
                case 5: key_kind = TIGT_KEY_PAGE_UP; break;
                case 6: key_kind = TIGT_KEY_PAGE_DOWN; break;
                case 7: key_kind = TIGT_KEY_HOME; break;
                case 8: key_kind = TIGT_KEY_END; break;
                case 11: case 12: case 13: case 14: case 15:
                    key_kind = TIGT_KEY_FUNCTION;
                    key_value -= 10;
                    break;
                case 17: case 18: case 19: case 20: case 21:
                    key_kind = TIGT_KEY_FUNCTION;
                    key_value -= 11;
                    break;
                case 23: case 24:
                    key_kind = TIGT_KEY_FUNCTION;
                    key_value -= 12;
                    break;
                case 25: case 26:
                    key_kind = TIGT_KEY_FUNCTION;
                    key_value -= 12;
                    break;
                case 28:
                    key_kind = TIGT_KEY_FUNCTION;
                    key_value = 15;
                    break;
                default:
                    return;
            }
            break;
        default:
            return;
    }
    input_event(input, key_kind, key_value, 0, kitty_modifiers(modifiers),
                (uint8_t) (event_kind - 1));
}

static void
handle_ss3_key(tigt_input *input, uint8_t final)
{
    switch (final) {
        case 'A': case 'B': case 'C': case 'D': case 'F': case 'H':
        case 'P': case 'Q': case 'S':
            handle_csi_key(input, "", final);
            break;
        case 'E':
            input_event(input, TIGT_KEY_KEYPAD_BEGIN, 0, 0, 0, TIGT_PRESS);
            break;
        case 'R':
            /* CSI R reports cursor position; only SS3 R is the F3 key. */
            input_event(input, TIGT_KEY_FUNCTION, 3, 0, 0, TIGT_PRESS);
            break;
        default:
            break;
    }
}

static void
handle_plain_key(tigt_input *input, uint8_t byte)
{
    if (input->utf8_remaining != 0) {
        if ((byte & 0xc0) == 0x80) {
            input->utf8_codepoint = (input->utf8_codepoint << 6) | (byte & 0x3f);
            if (--input->utf8_remaining == 0) {
                const uint32_t codepoint = input->utf8_codepoint;
                const bool valid = codepoint >= input->utf8_minimum && codepoint <= 0x10ffff &&
                                   (codepoint < 0xd800 || codepoint > 0xdfff);

                if (valid && codepoint == 0xf746 && !input->paste)
                    input_tap(input, TIGT_KEY_INSERT, 0, 0);
                else
                    input_tap(input, TIGT_KEY_CHAR, valid ? codepoint : 0xfffd, 0);
            }
            return;
        }
        input->utf8_remaining = 0;
        input_tap(input, TIGT_KEY_CHAR, 0xfffd, 0);
    }
    if (input->paste && byte < 0x80)
        input_tap(input, TIGT_KEY_CHAR, byte, 0);
    else if (byte == '\b' || byte == 127)
        input_tap(input, TIGT_KEY_BACKSPACE, 0, 0);
    else if (byte == '\r' || byte == '\n')
        input_tap(input, TIGT_KEY_ENTER, 0, 0);
    else if (byte < 32 && byte != '\t')
        input_tap(input, TIGT_KEY_CHAR, byte == 0 ? '@' : byte <= 26 ? 'a' + byte - 1 : '@' + byte,
                  TIGT_MOD_CONTROL);
    else if (byte < 0x80)
        input_tap(input, TIGT_KEY_CHAR, byte, 0);
    else if (byte >= 0xc2 && byte <= 0xf4) {
        input->utf8_remaining = byte < 0xe0 ? 1 : byte < 0xf0 ? 2 : 3;
        input->utf8_minimum = byte < 0xe0 ? 0x80 : byte < 0xf0 ? 0x800 : 0x10000;
        input->utf8_codepoint = byte & (byte < 0xe0 ? 0x1f : byte < 0xf0 ? 0x0f : 0x07);
    } else
        input_tap(input, TIGT_KEY_CHAR, 0xfffd, 0);
}

static void
handle_paste_byte(tigt_input *input, uint8_t byte)
{
    static const uint8_t end[] = "\033[201~";

    if (byte == end[input->paste_end_length]) {
        if (++input->paste_end_length == sizeof(end) - 1) {
            if (input->utf8_remaining != 0) {
                input->utf8_remaining = 0;
                input_tap(input, TIGT_KEY_CHAR, 0xfffd, 0);
            }
            input->paste_end_length = 0;
            input->paste = false;
        }
        return;
    }
    /* A partial end marker is payload unless all six bytes match. */
    for (uint8_t index = 0; index < input->paste_end_length; index++)
        handle_plain_key(input, end[index]);
    input->paste_end_length = 0;
    if (byte == end[0])
        input->paste_end_length = 1;
    else
        handle_plain_key(input, byte);
}

static void
mouse_move(tigt_input *input, tigt_mouse_event *event)
{
    event->kind = TIGT_MOUSE_MOVE;
    event->button = TIGT_MOUSE_BUTTON_NONE;
    event->buttons = input->mouse_buttons;
    input->on_mouse(event, input->mouse_user);
}

static void
mouse_button(tigt_input *input, tigt_mouse_event *event, uint32_t button,
             bool release, bool synthetic)
{
    mouse_move(input, event);
    const uint32_t mask = button == TIGT_MOUSE_BUTTON_NONE ? 0 : 1u << (button - 1);

    if (release)
        input->mouse_buttons &= ~mask;
    else
        input->mouse_buttons |= mask;
    event->kind = release ? TIGT_MOUSE_UP : TIGT_MOUSE_DOWN;
    event->button = button;
    event->buttons = input->mouse_buttons;
    if (synthetic)
        event->flags |= TIGT_MOUSE_SYNTHETIC;
    input->on_mouse(event, input->mouse_user);
}

static void
handle_mouse(tigt_input *input, uint32_t code, uint32_t x, uint32_t y,
             bool release, bool legacy, bool negative_position)
{
    const uint32_t mode = atomic_load_explicit(&input->mouse_mode, memory_order_relaxed);

    if (mode != input->applied_mouse_mode) {
        input->mouse_buttons = 0;
        input->applied_mouse_mode = mode;
    }
    if (mode == TIGT_MOUSE_OFF || input->on_mouse == NULL)
        return;
    tigt_mouse_event event = {
        .coordinates = !legacy && mode == TIGT_MOUSE_PIXELS
                       ? TIGT_MOUSE_COORD_PIXELS : TIGT_MOUSE_COORD_CELLS
    };

    /* Kitty mouse.c defines LEAVE_INDICATOR as (1 << 8), not bit 7.
     * All other bits and even out-of-window/negative coordinates are ignored. */
    if (!legacy && mode == TIGT_MOUSE_PIXELS && (code & 256) != 0) {
        event.kind = TIGT_MOUSE_LEAVE;
        event.buttons = input->mouse_buttons;
        input->on_mouse(&event, input->mouse_user);
        return;
    }
    if ((code & ~127u) != 0 || negative_position || x == 0 || y == 0 ||
        x - 1 > INT32_MAX || y - 1 > INT32_MAX)
        return;
    const uint32_t button = code & 3;
    const bool motion = (code & 32) != 0;
    const bool wheel = (code & 64) != 0;

    if ((motion && (wheel || release)) || (wheel && release))
        return;
    if (mode == TIGT_MOUSE_X10 && (motion || release || (!wheel && button == 3)))
        return;
    event.flags = TIGT_MOUSE_POSITION_VALID;
    event.x = (int32_t) (x - 1);
    event.y = (int32_t) (y - 1);
    if (code & 4)
        event.modifiers |= TIGT_MOD_SHIFT;
    if (code & 8)
        event.modifiers |= TIGT_MOD_ALT;
    if (code & 16)
        event.modifiers |= TIGT_MOD_CONTROL;
    if (wheel) {
        mouse_move(input, &event);
        event.kind = TIGT_MOUSE_SCROLL;
        if (button < 2)
            event.scroll_y = button == 0 ? -1 : 1;
        else
            event.scroll_x = button == 2 ? -1 : 1;
        input->on_mouse(&event, input->mouse_user);
    } else if (motion) {
        if (button == 3)
            input->mouse_buttons = 0;
        else
            input->mouse_buttons |= 1u << button;
        mouse_move(input, &event);
    } else if (legacy && button == 3) {
        /* Legacy release identifies no button. Clear all known held buttons,
         * never guess one from the last press in a multi-button chord. */
        const uint32_t held = input->mouse_buttons;

        for (uint32_t index = 0; index < 3; index++)
            if (held & (1u << index))
                mouse_button(input, &event, index + 1, true, false);
        if (held == 0)
            mouse_button(input, &event, TIGT_MOUSE_BUTTON_NONE, true, false);
    } else if (button < 3) {
        mouse_button(input, &event, button + 1, release, false);
        if (mode == TIGT_MOUSE_X10)
            mouse_button(input, &event, button + 1, true, true);
    }
}

static void
handle_sgr_mouse(tigt_input *input, const char *parameters, uint8_t final)
{
    const char *end = parameters + 1;
    uint32_t code, x, y;
    bool negative_position = false;

    if (!parse_number(&end, &code) || *end++ != ';')
        return;
    if (*end == '-') {
        negative_position = true;
        end++;
    }
    if (!parse_number(&end, &x) || *end++ != ';')
        return;
    if (*end == '-') {
        negative_position = true;
        end++;
    }
    if (!parse_number(&end, &y) || *end != '\0')
        return;
    handle_mouse(input, code, x, y, final == 'm', false, negative_position);
}

tigt_input *
tigt_input_create(tigt_input_callback callback, void *user)
{
    return tigt_input_create_with_mouse(callback, user, NULL, NULL, TIGT_MOUSE_OFF);
}

tigt_input *
tigt_input_create_with_mouse(tigt_input_callback on_input, void *input_user,
                             tigt_mouse_callback on_mouse, void *mouse_user,
                             uint32_t mouse_mode)
{
    if (mouse_mode > TIGT_MOUSE_X10 ||
        (mouse_mode == TIGT_MOUSE_OFF) != (on_mouse == NULL) ||
        (on_input == NULL && on_mouse == NULL)) {
        errno = EINVAL;
        return NULL;
    }
    tigt_input *input = calloc(1, sizeof(*input));

    if (input != NULL) {
        input->callback = on_input;
        input->user = input_user;
        input->on_mouse = on_mouse;
        input->mouse_user = mouse_user;
        atomic_init(&input->mouse_mode, mouse_mode);
        input->applied_mouse_mode = mouse_mode;
    }
    return input;
}

void
tigt_input_set_mouse_mode(tigt_input *input, uint32_t mode)
{
    if (input != NULL && mode <= TIGT_MOUSE_X10 &&
        (mode == TIGT_MOUSE_OFF || input->on_mouse != NULL))
        atomic_store_explicit(&input->mouse_mode, mode, memory_order_relaxed);
}

void
tigt_input_set_terminal_report_callback(tigt_input *input,
                                       tigt_terminal_report_callback callback, void *user)
{
    if (input != NULL) {
        input->on_report = callback;
        input->report_user = user;
    }
}

void
tigt_input_feed(tigt_input *input, const uint8_t *bytes, size_t length)
{
    if (input == NULL || bytes == NULL)
        return;
    for (size_t index = 0; index < length; index++) {
        const uint8_t byte = bytes[index];

        if (input->paste) {
            handle_paste_byte(input, byte);
            continue;
        }

        if (input->legacy_remaining != 0) {
            input->legacy_mouse[3 - input->legacy_remaining] = byte;
            if (--input->legacy_remaining == 0 &&
                input->legacy_mouse[0] >= 32 &&
                input->legacy_mouse[1] >= 33 && input->legacy_mouse[2] >= 33)
                handle_mouse(input, input->legacy_mouse[0] - 32,
                             input->legacy_mouse[1] - 32, input->legacy_mouse[2] - 32,
                             false, true, false);
            continue;
        }

        if (input->sequence_length == 1 && byte != '[' && byte != 'O') {
            input->sequence_length = 0;
            input_tap(input, TIGT_KEY_ESCAPE, 0, 0);
        }
        if (byte == '\033') {
            if (input->utf8_remaining != 0) {
                input->utf8_remaining = 0;
                input_tap(input, TIGT_KEY_CHAR, 0xfffd, 0);
            }
            input->sequence[0] = (char) byte;
            input->sequence_length = 1;
            input->discard_sequence = false;
            input->csi_intermediate = false;
        } else if (input->sequence_length == 0) {
            handle_plain_key(input, byte);
        } else if (input->sequence_length == 1) {
            input->sequence[input->sequence_length++] = (char) byte;
        } else if (input->sequence[1] == 'O') {
            handle_ss3_key(input, byte);
            input->sequence_length = 0;
        } else if (byte >= 0x40 && byte <= 0x7e) {
            input->sequence[input->sequence_length] = '\0';
            if (!input->discard_sequence) {
                if (input->on_report != NULL &&
                    (byte == 'c' || byte == 'S' || byte == 't' || byte == 'y'))
                    input->on_report(input->sequence + 2, byte, input->report_user);
                if (byte == 'M' && input->sequence_length == 2)
                    input->legacy_remaining = 3;
                else if ((byte == 'M' || byte == 'm') && input->sequence[2] == '<')
                    handle_sgr_mouse(input, input->sequence + 2, byte);
                else if (byte == 'u')
                    handle_kitty_key(input, input->sequence + 2);
                else
                    handle_csi_key(input, input->sequence + 2, byte);
            }
            input->sequence_length = 0;
            input->discard_sequence = false;
        } else if (byte >= 0x20 && byte <= 0x3f) {
            if (byte < 0x30 && !(byte == '-' && input->sequence_length > 2 &&
                                 input->sequence[2] == '<'))
                input->csi_intermediate = true;
            else if (input->csi_intermediate)
                input->discard_sequence = true;
            if (input->sequence_length < sizeof(input->sequence) - 1)
                input->sequence[input->sequence_length++] = (char) byte;
            else
                input->discard_sequence = true;
        } else {
            input->discard_sequence = true;
        }
    }
}

void
tigt_input_flush(tigt_input *input)
{
    if (input != NULL && input->sequence_length == 1) {
        input->sequence_length = 0;
        input_tap(input, TIGT_KEY_ESCAPE, 0, 0);
    }
}

void
tigt_input_destroy(tigt_input *input)
{
    free(input);
}
