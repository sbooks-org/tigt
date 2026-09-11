/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#ifndef TIGT_INPUT_INTERNAL_H
#define TIGT_INPUT_INTERNAL_H
#include "tigt.h"

/* Reports share the decoder's fragmented CSI state, never a competing reader.
 * Called synchronously by feed; configure before starting the input thread. */
typedef void (*tigt_terminal_report_callback)(const char *parameters, uint8_t final, void *user);
void tigt_input_set_terminal_report_callback(tigt_input *input,
                                            tigt_terminal_report_callback callback, void *user);
#endif
