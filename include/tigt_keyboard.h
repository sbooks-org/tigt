/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#ifndef TIGT_KEYBOARD_H
#define TIGT_KEYBOARD_H

#include "tigt.h"
#include <pc_xt_keyboard.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional adapter: the application owns mapper and links pc-xt-keyboard.
 * Both version-1 semantic event ABIs use the same numeric discriminants.
 * Copy fields, not object representations: no aliasing/layout assumption.
 * Buffer capacity and mapper validity are checked by the mapper itself.
 */
static inline size_t
tigt_keyboard_handle(void *mapper, const tigt_input_event *event,
                     uint8_t *bytes, size_t capacity)
{
    pc_xt_keyboard_v1_input_event input;

    if (event == NULL)
        return PC_XT_KEYBOARD_V1_ERROR;

    input.key.kind = event->key.kind;
    input.key.value = event->key.value;
    input.key.character = event->key.character;
    input.modifiers = event->modifiers;
    input.kind = event->kind;
    return pc_xt_keyboard_v1_handle(mapper, &input, bytes, capacity);
}

#ifdef __cplusplus
}
#endif
#endif
