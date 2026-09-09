/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#include "tigt.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>

struct tigt_input {
    tigt_input_callback callback;
    void *user;
    char sequence[64];
    size_t sequence_length;
    bool discard_sequence;
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
        .kind = kind
    };

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
        default:
            break;
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

                input_tap(input, TIGT_KEY_CHAR, valid ? codepoint : 0xfffd, 0);
            }
            return;
        }
        input->utf8_remaining = 0;
        input_tap(input, TIGT_KEY_CHAR, 0xfffd, 0);
    }
    if (byte == '\b' || byte == 127)
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

tigt_input *
tigt_input_create(tigt_input_callback callback, void *user)
{
    if (callback == NULL) {
        errno = EINVAL;
        return NULL;
    }
    tigt_input *input = calloc(1, sizeof(*input));

    if (input != NULL) {
        input->callback = callback;
        input->user = user;
    }
    return input;
}

void
tigt_input_feed(tigt_input *input, const uint8_t *bytes, size_t length)
{
    if (input == NULL || bytes == NULL)
        return;
    for (size_t index = 0; index < length; index++) {
        const uint8_t byte = bytes[index];

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
                if (byte == 'u')
                    handle_kitty_key(input, input->sequence + 2);
                else
                    handle_csi_key(input, input->sequence + 2, byte);
            }
            input->sequence_length = 0;
            input->discard_sequence = false;
        } else if ((byte >= '0' && byte <= '9') || byte == ';' || byte == ':') {
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
