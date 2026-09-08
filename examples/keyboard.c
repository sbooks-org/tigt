/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 * Optional example: compile with the keyboard package's include directory and
 * link libtigt plus libpc_xt_keyboard and their platform dependencies.
 * No display session is opened. Pipe terminal input bytes into stdin.
 */
#include <tigt_keyboard.h>
#include <stdio.h>

static void emit(const tigt_input_event *event, void *mapper)
{
    pc_xt_keyboard_v1_key_event keys[PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS];
    size_t count = tigt_keyboard_handle(mapper, event, keys, PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS);
    if (count == PC_XT_KEYBOARD_V1_ERROR)
        return;
    for (size_t i = 0; i < count; ++i)
        printf("%03x:%s ", keys[i].key, keys[i].down ? "down" : "up");
}

int main(void)
{
    void *mapper = pc_xt_keyboard_v1_create(PC_XT_KEYBOARD_V1_XT_SET1);
    if (mapper == NULL)
        return 1;
    tigt_input *input = tigt_input_create(emit, mapper);
    if (input == NULL) {
        pc_xt_keyboard_v1_destroy(mapper);
        return 1;
    }
    int byte;
    while ((byte = getchar()) != EOF) {
        const uint8_t value = (uint8_t)byte;
        tigt_input_feed(input, &value, 1);
    }
    tigt_input_flush(input);
    tigt_input_destroy(input);
    pc_xt_keyboard_v1_destroy(mapper);
    putchar('\n');
    return ferror(stdin) ? 1 : 0;
}
