/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#ifndef TIGT_INPUT_INTERNAL_H
#define TIGT_INPUT_INTERNAL_H
#include "tigt.h"
#include "tigt_mouse.h"

/* Reports share the decoder's fragmented CSI state, never a competing reader.
 * Called synchronously by feed; configure before starting the input thread. */
typedef void (*tigt_terminal_report_callback)(const char *parameters, uint8_t final, void *user);
void tigt_input_set_terminal_report_callback(tigt_input *input,
                                            tigt_terminal_report_callback callback, void *user);

/* Atomic, callable from the renderer while feed is running. Callback setup is
 * immutable; enabling a decoder created without on_mouse has no effect.
 * AUTO means cell coordinates until negotiation selects PIXELS or CELLS.
 * A change resets held-button inference at the next mouse report, without
 * inventing releases. Destroy must remain externally serialized.
 *
 * Legacy reports have byte-limited coordinates (1..223 on the wire); UTF-8
 * 1005, urxvt 1015 and extra buttons are unsupported. Legacy anonymous release
 * releases every tracked button, or reports UP with button NONE if unknown.
 * Missing terminal reports cannot be reconstructed. X10 clicks synthesize UP;
 * neither focus changes nor timeouts imply pointer departure. Frame geometry
 * is intentionally absent here and is attached only by the live renderer. */
void tigt_input_set_mouse_mode(tigt_input *input, uint32_t mode);
#endif
