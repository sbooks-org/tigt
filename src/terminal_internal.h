/* SPDX-License-Identifier: MIT-0 */
#ifndef TIGT_TERMINAL_INTERNAL_H
#define TIGT_TERMINAL_INTERNAL_H
#include "tigt_terminal.h"
/* One writer gate serializes normal release against terminal I/O. Never take
 * it in a handler. A successful begin must be paired with end on every path.
 * special=0 permits glass output in the background, never terminal protocols. */
int tigt_terminal_begin_io(int special);
void tigt_terminal_end_io(void);
int tigt_terminal_fd_foreground(int fd);
void tigt_terminal_record_error(int error);
int tigt_terminal_is_released(void);
void tigt_terminal_set_owner(void (*transition)(unsigned events));
int tigt_terminal_is_faulted(void);
int tigt_terminal_input_disabled(void);
void tigt_terminal_pending_fullscreen(int delta);
/* Called before curses/protocol mutation so partial setup is releasable. */
void tigt_terminal_screen(int enabled);
int tigt_terminal_mouse(uint32_t mode);
#endif
