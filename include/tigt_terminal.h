/* SPDX-License-Identifier: MIT-0 */
#ifndef TIGT_TERMINAL_H
#define TIGT_TERMINAL_H
#include "tigt.h"
#include <signal.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    TIGT_TERMINAL_RESTORED = 1u << 0,
    TIGT_TERMINAL_RELEASED = 1u << 1,
    TIGT_TERMINAL_RESIZED = 1u << 2,
    TIGT_TERMINAL_INPUT_RESET = 1u << 3
};

/* One process-terminal owner. The fds are borrowed until forget; pipes are
 * permitted for an external glass presenter. Capture never changes terminal
 * state and returns BUSY if already captured. Native sessions capture for you.
 * Normal-context lifecycle operations must be serialized on the owning thread,
 * outside callbacks. Signal-handler installation is independent and opt-in. */
int tigt_terminal_capture(int input_fd, int output_fd);
void tigt_terminal_forget(void);
/* Async-signal-safe, idempotent, best-effort release using captured state only.
 * No threads are joined here. Normal-context poll/suspend/shutdown quiesces
 * native workers. Restores the exact baseline termios, including saved noecho.
 * A release failure is persistent in status. Never promises SIGKILL recovery. */
void tigt_terminal_release(void);
/* Async-signal-safe request (for an application's SIGCONT handler). The next
 * normal-context poll restores only if this process owns the foreground. */
void tigt_terminal_request_restore(void);
/* Restore the requested foreground policy. In the background it remains
 * released and returns BACKGROUND; it never enables raw input or protocols. */
int tigt_terminal_restore(void);
/* Service foreground changes, deferred CONT/WINCH and native worker lifecycle.
 * Returns transition bits, or the persistent negative error. Always compare
 * generation as well: errors must not hide a release/restore invalidation.
 * Poll regularly even with no frame submissions. */
int tigt_terminal_poll(void);
int tigt_terminal_status(void);
unsigned tigt_terminal_generation(void);
/* Nonzero only if every captured TTY is owned by this process group. Pipes
 * require no foreground ownership; no captured owner returns zero. */
int tigt_terminal_is_foreground(void);
/* Raw mode disables kernel ISIG/IEXTEN; filter_input owns host keys. Cooked
 * mode enables canonical/echo and preserves baseline ISIG/IEXTEN/VLNEXT so
 * quoted bytes already processed by the kernel are never signalled twice.
 * No input flushing. The requested policy survives release/backgrounding. */
int tigt_terminal_set_input_mode(int raw, int keyboard_reporting);
/* Temporary noecho/noncanonical cursor-query input, preserving the desired
 * mode's signal/quoting policy. Foreground only; no input flushing. Disable
 * before switching policy, and bypass filter for buffered cooked/query bytes. */
int tigt_terminal_set_probe_mode(int enabled);
/* External raw decoder callback: 1 forwards, 0 consumes, negative reports an
 * error. Native callbacks have already been filtered. Pasted input bypasses
 * policy. Ctrl-C/\\/Z/T mean INT/QUIT/TSTP/INFO (T is UNSUPPORTED without INFO).
 * Ctrl-V quotes the next complete gesture, including repeats and release;
 * Ctrl-V Ctrl-V forwards exactly the second gesture. Cooked input bypasses
 * this filter because the line discipline has already applied ISIG/VLNEXT.
 * Serialize on the input consumer; discard decoder/held guest state whenever
 * generation changes. */
int tigt_terminal_filter_input(const tigt_input_event *event);
typedef struct {
    int signal_number;
    struct sigaction action;
} tigt_terminal_signal_action;

/* Chain prior dispositions, SA_SIGINFO, masks and default/ignore behavior.
 * HUP is termination, CONT defers restoration, WINCH defers resize, INFO is
 * informational. Fatal signals only perform minimal release then preserve
 * crash-handler/default termination semantics. TIGT itself never reinitializes
 * from a fault; a custom handler retains ownership of its ucontext.
 * Ordinary teardown signals are deferred until in-flight output quiesces.
 * Their prior handlers receive original siginfo on the dispatcher thread with
 * its live delivery context, not the original interrupted thread's context.
 * Uninstall restores only dispositions still owned by this library. */
int tigt_terminal_install_signal_handlers(void);
/* Authoritative prior dispositions for selected managed signals. Use the
 * application's original sigaction declarations when the OS query omits
 * flags (Darwin does not report SA_RESETHAND). Unlisted signals are queried.
 * NULL with nonzero count, duplicate or unmanaged signals return ARGUMENT
 * without mutation. Nonempty overrides while installed return BUSY; an empty
 * span, like the no-argument convenience function, is idempotent.
 * Normal owning-thread context only; action callback storage must outlive
 * installation and any restored disposition. */
int tigt_terminal_install_signal_handlers_with_actions(
    const tigt_terminal_signal_action *actions, size_t count);
void tigt_terminal_uninstall_signal_handlers(void);
#ifdef __cplusplus
}
#endif
#endif
