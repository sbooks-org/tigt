/* SPDX-License-Identifier: MIT-0 */
#ifndef TIGT_PRESENTER_H
#define TIGT_PRESENTER_H
#include "tigt.h"
#ifdef __cplusplus
extern "C" {
#endif
#define TIGT_PRESENTER_ABI_VERSION 1u
#define TIGT_ERROR_UNREPRESENTABLE -5
#define TIGT_PRESENTER_PENDING 1
#define TIGT_PRESENTER_FULLSCREEN 2
#define TIGT_PRESENTER_WOULD_BLOCK 4
#define TIGT_PRESENTER_NEEDS_CURSOR 5
#define TIGT_PRESENTER_LOCAL_ECHO_MAX 4096u

enum { TIGT_PRESENT_GLASS = 0, TIGT_PRESENT_ADAPTIVE = 1 };
enum { TIGT_ENCODING_LOCALE = 0, TIGT_ENCODING_UTF8 = 1, TIGT_ENCODING_ASCII = 2 };
/* Additional flags accepted by this presenter only. */
enum { TIGT_PRESENT_BOLD = 1u << 2, TIGT_PRESENT_REVERSE = 1u << 3 };
/* Frame hints, independent of cell flags and speculative notifications. */
enum {
    TIGT_PRESENT_VIDEO_DISABLED = 1u << 0,
    TIGT_PRESENT_VIDEO_MEMORY_CHANGED = 1u << 1
};
typedef struct {
    uint32_t abi_version;
    int output_fd;
    uint32_t mode;
    uint32_t encoding;
    /* 0: retain fallback. 1: resume at its retained cursor after observed
     * sequential row advancement and text (or clear and text), with 100ms at
     * the text frontier. Recovery never clears or replays the host region. */
    uint32_t reversible;
} tigt_presenter_config;
typedef struct {
    /* Underlying decoded text, including while VIDEO_DISABLED: decode with
     * hardware blanking bypassed. The presenter alone masks disabled output. */
    const tigt_text_cell *cells;
    uint16_t columns, rows, stride;
    uint16_t cursor_column, cursor_row;
    uint16_t refresh_hz; /* 50, 60, or reserved future VGA cadence 70. */
    /* VIDEO_DISABLED means hardware output is disabled, not an ordinary blank.
     * VIDEO_MEMORY_CHANGED means any raw VRAM byte differs from the previous
     * snapshot submitted here (including attributes/offscreen bytes); set it
     * for the initial snapshot. Comparing while skipping submission must not
     * consume that notification. Zero preserves ordinary presentation.
     * Unknown bits are invalid. Hardware-disabled raw cells are never painted. */
    uint32_t hints;
} tigt_presenter_frame;
typedef struct tigt_presenter tigt_presenter;

#define TIGT_PRESENTER_NOTIFICATION_CAPACITY 32u
#define TIGT_PRESENTER_NOTIFICATION_TEXT_MAX 2048u
#define TIGT_PRESENTER_NOTIFICATION_BOUNDARIES_MAX 128u
#define TIGT_PRESENTER_NOTIFICATION_MAX_AGE_MS 2000u
#define TIGT_PRESENTER_NOTIFY_DROPPED 3
enum { TIGT_BOUNDARY_SOFT_WRAP = 1, TIGT_BOUNDARY_NEWLINE = 2 };
typedef struct {
    size_t text_offset;
    uint32_t kind;
} tigt_presenter_boundary;
typedef struct {
    uint64_t operation_id;
    const uint32_t *text;
    size_t text_length;
    const tigt_presenter_boundary *boundaries;
    size_t boundary_count;
    uint16_t columns, rows, start_column, start_row;
} tigt_presenter_notification;
typedef struct {
    uint64_t consumed, discarded, expired;
    uint32_t queued;
} tigt_presenter_notification_stats;

/* Optional, speculative observations; never write output. The observer supplies
 * decoded, single-cell display scalars, not bytes or control characters. Expand
 * tabs into cells and anchor separate operations after CR/backspace/cursor moves.
 * Boundaries occur AFTER text_offset scalars, in nondecreasing offset order.
 * A soft wrap must occur at the right edge; a newline advances a row and resets
 * the column. At the bottom edge either boundary scrolls the whole display.
 * Supply every crossed boundary, including an explicit newline after a wrap.
 *
 * notify copies both arrays. Nonzero operation IDs deduplicate all outstanding
 * observations and the most recent 64 accepted/cancelled IDs. Reusing an ID
 * does not refresh its age. Overflow (including an oversized payload) discards
 * all outstanding predictions and the incoming operation, returning DROPPED;
 * it cannot change the confirmed image or logical output mapping.
 *
 * Matching is position-anchored, exact, incremental, and snapshot-authoritative.
 * Unchanged frames are not mismatches. A soft boundary needs confirmed preceding
 * text and following text, or an observed cursor crossing;
 * skipped/resynchronized text never confirms an earlier boundary. Ambiguous resynchronization fails
 * closed. Predictions expire after MAX_AGE_MS of valid guest vsync time, independently
 * of the presenter's 100ms representability interval. Reset, geometry changes,
 * observed clears, and fullscreen entry invalidate outstanding predictions.
 * Matches/discards commit only with successful presentation; vsync aging and
 * explicit notify/cancel/reset operations are independent of that transaction.
 * Confirmed wraps retain a physical-to-logical column mapping for later edits.
 * A zero-text NEWLINE notification can identify an observed explicit LF.
 *
 * Stats count operations, not scalars, and remain cumulative across reset.
 * A resynchronized operation with an unobserved prefix counts as discarded.
 * cancel is idempotent and also retains the ID in the bounded dedup history.
 * Reset clears dedup history. All three operations return OK on success;
 * notify also returns DROPPED on overflow. Null arguments and ID zero are invalid.
 */
int tigt_presenter_notify(tigt_presenter *presenter, const tigt_presenter_notification *notification);
int tigt_presenter_cancel(tigt_presenter *presenter, uint64_t operation_id);
int tigt_presenter_get_notification_stats(const tigt_presenter *presenter,
                                          tigt_presenter_notification_stats *stats);
/* Actual host observations, never speculative output predictions. Coordinates
 * are one-based, as in a terminal cursor-position report. The consumer owns
 * querying/demultiplexing replies; the presenter never reads input. Adaptive
 * entry returns NEEDS_CURSOR without output if its host position is unknown.
 * Forget the observation after untracked external output or terminal resume.
 */
int tigt_presenter_observe_cursor(tigt_presenter *presenter, unsigned column, unsigned row);
int tigt_presenter_forget_cursor(tigt_presenter *presenter);
/* Register text ALREADY displayed by the same host TTY's line discipline,
 * before delivering its keys to the guest. Supply the final edited line as
 * single-cell scalars, LF and TAB; erased input is absent. Tabs expand at the
 * logical output column. The next guest output must confirm this exact stream.
 * Confirmation suppresses those glyphs/newlines, not their cursor/mapping
 * updates; known guest wraps do not insert extra host newlines. This is separate
 * from notify and does not change notification statistics.
 *
 * The copied queue holds LOCAL_ECHO_MAX expanded scalars. Overflow or a
 * contradictory guest output is unrepresentable, never silently replayed.
 * A clear, geometry change, fullscreen entry or reset ends local accounting.
 * Invalid scalars return ARGUMENT; overflow returns UNREPRESENTABLE. Requires
 * an initialized glass baseline. Pipe input has no local echo to register.
 */
int tigt_presenter_local_echo(tigt_presenter *presenter, const uint32_t *text, size_t length);
/* Output-only; submit once per vsync, including unchanged frames.
 * The caller serializes all operations and owns the borrowed output fd. Never
 * share output with a live curses session. Adaptive mode requires a TTY fd.
 * No stdin reads, termios raw mode, alternate screen, or signal handlers.
 * Returned status: OK (glass), PENDING (confirmation, scroll, or disable hold),
 * NEEDS_CURSOR (observation), FULLSCREEN, WOULD_BLOCK, or error. PENDING retains
 * the current presentation/input mode, including when already fullscreen.
 * A copied row prefix/partial row followed by an untouched (possibly already
 * shifted) suffix, or uncleared exposed rows, holds the committed image/cursor/
 * echo/map for at most 500ms from first detection. Progress/idle observations
 * do not renew the deadline. Coherent, uniquely aligned completion commits
 * once; ambiguous repeated rows cannot invent scrollback. Timeout takes the
 * ordinary glass error/adaptive fallback, including when already fullscreen.
 * Ordinary unrepresentable edits still use the separate 100ms confirmation.
 * Hardware-disabled output ordinarily holds for 200ms unless VRAM changes.
 * The first disabled vsync counts: 10 frames at 50Hz, 12 at 60Hz. Changed VRAM
 * bypasses that hold for the disable interval; reenable/reset starts fresh.
 * Exception: recognized scrolling (even a completed copy while still disabled)
 * retains the prior presentation under the same bounded 500ms scroll deadline.
 * All other disabled memory changes show hardware black immediately; raw text
 * is never painted while disabled. Enabled blank/CLS frames are not debounced.
 * UNREPRESENTABLE and hard I/O errors are sticky until reset. Ordinary output
 * backpressure returns WOULD_BLOCK and retains a resumable transaction.
 * Destroy frees state, never closes the fd. Reset starts a new empty glass
 * baseline without emitting output; consumer must have prepared its destination.
 * Reversible adaptive recovery also needs 100ms of representable frontier
 * observations; cursor motion/idle/same-line rewrites alone cannot arm it.
 * Logical cursor position is independent of TIGT_TEXT_CURSOR/visibility.
 */
int tigt_presenter_create(const tigt_presenter_config *config, tigt_presenter **output);
int tigt_presenter_present(tigt_presenter *presenter, const tigt_presenter_frame *frame);
/* Bounded output: requires O_NONBLOCK on the borrowed fd; does not change it.
 * Each call attempts at most one write, never polls, sleeps, or retries EINTR.
 * WOULD_BLOCK means that exact unwritten bytes and the pending frame are owned
 * by the presenter. This includes short writes and EINTR, not just EAGAIN.
 * Resume without resubmitting the frame; neither frame time nor notification
 * matching advances on resume. Completion returns OK, PENDING, or FULLSCREEN.
 * Input frame storage may be reused immediately after submission returns.
 *
 * There is one bounded transaction, not a frame queue. Until it completes,
 * present (either variant), notify, cancel, observe_cursor, forget_cursor, and
 * local_echo return BUSY without mutation. Resume without a pending transaction
 * returns BUSY. Stats remain readable.
 * Callers can service cancellation/suspension between attempts and wait for
 * writability themselves. Reset/destroy discard pending output immediately,
 * without writing or rolling back emitted bytes; prepare the destination before
 * continuing after reset, especially if cancellation split an escape sequence.
 *
 * The original present API continues writing until complete on blocking fds.
 * It also preserves a transaction on EAGAIN; set O_NONBLOCK before resuming.
 * On platforms that ignore O_NONBLOCK for regular files, file writes may block.
 */
int tigt_presenter_present_nonblocking(tigt_presenter *presenter, const tigt_presenter_frame *frame);
int tigt_presenter_resume(tigt_presenter *presenter);
/* Nonzero if fullscreen is committed or a pending fullscreen transaction has
 * emitted any bytes. Read after present/resume, including errors, before reset
 * when deciding whether terminal restoration is needed. This is current state,
 * not a historical latch; completed glass recovery and reset clear it.
 * Performs no I/O. NULL returns zero. Caller still serializes access.
 */
int tigt_presenter_fullscreen_output_started(const tigt_presenter *presenter);
int tigt_presenter_reset(tigt_presenter *presenter);
void tigt_presenter_destroy(tigt_presenter *presenter);
#ifdef __cplusplus
}
#endif
#endif
