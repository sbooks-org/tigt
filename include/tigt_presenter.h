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

enum { TIGT_PRESENT_GLASS = 0, TIGT_PRESENT_ADAPTIVE = 1 };
enum { TIGT_ENCODING_LOCALE = 0, TIGT_ENCODING_UTF8 = 1, TIGT_ENCODING_ASCII = 2 };
/* Additional flags accepted by this presenter only. */
enum { TIGT_PRESENT_BOLD = 1u << 2, TIGT_PRESENT_REVERSE = 1u << 3 };
typedef struct {
    uint32_t abi_version;
    int output_fd;
    uint32_t mode;
    uint32_t encoding;
    uint32_t reversible;
} tigt_presenter_config;
typedef struct {
    const tigt_text_cell *cells;
    uint16_t columns, rows, stride;
    uint16_t cursor_column, cursor_row;
    uint16_t refresh_hz; /* 50, 60, or reserved future VGA cadence 70. */
    /* Reserved; must be zero. Notifications are separate from frame hints. */
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
/* Output-only, synchronous; submit once per vsync, including unchanged frames.
 * The caller serializes all operations and owns the borrowed output fd. Never
 * share output with a live curses session. Adaptive mode requires a TTY fd.
 * No stdin reads, termios raw mode, alternate screen, or signal handlers.
 * Returned status: OK (glass), PENDING (confirmation), FULLSCREEN, or error.
 * UNREPRESENTABLE is sticky until reset; I/O errors are likewise terminal.
 * Destroy frees state, never closes the fd. Reset starts a new empty glass
 * baseline without emitting output; consumer must have prepared its destination.
 * Logical cursor position is independent of TIGT_TEXT_CURSOR/visibility.
 */
int tigt_presenter_create(const tigt_presenter_config *config, tigt_presenter **output);
int tigt_presenter_present(tigt_presenter *presenter, const tigt_presenter_frame *frame);
int tigt_presenter_reset(tigt_presenter *presenter);
void tigt_presenter_destroy(tigt_presenter *presenter);
#ifdef __cplusplus
}
#endif
#endif
