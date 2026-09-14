/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "tigt_presenter.h"
#include "terminal_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

#define PRESENTER_CELLS 21440u
#define PRESENTER_BYTES (PRESENTER_CELLS * 96u + 32768u)
#define GLASS_FLAGS (TIGT_TEXT_UNDERLINE | TIGT_PRESENT_BOLD | TIGT_PRESENT_REVERSE)
#define NOTIFICATION_IDS 64u
#define NOTIFICATION_LOOKAHEAD 64u

typedef struct {
    uint64_t id;
    uint32_t text[TIGT_PRESENTER_NOTIFICATION_TEXT_MAX];
    tigt_presenter_boundary boundaries[TIGT_PRESENTER_NOTIFICATION_BOUNDARIES_MAX];
    unsigned length, boundary_count;
    uint16_t columns, rows;
} text_expectation;

typedef struct {
    uint64_t born;
    unsigned text, boundary, column;
    int row;
    bool had_text, skipped;
} expectation_position;

typedef struct {
    expectation_position position[TIGT_PRESENTER_NOTIFICATION_CAPACITY];
    unsigned order[TIGT_PRESENTER_NOTIFICATION_CAPACITY], count;
    tigt_presenter_notification_stats stats;
} expectation_queue;

typedef struct {
    size_t base[128];
    bool joined[128];
} logical_rows;

typedef struct {
    unsigned column, row;
    size_t logical_column;
    unsigned scroll_top, scroll_bottom, scroll_count;
    bool empty_baseline;
} glass_cursor;
/* Frames are never coalesced. Analysis uses reusable scratch storage; neither
 * the destination nor the committed image/cursor changes on an unrepresentable
 * submission. Hard write failures poison the session; backpressure retains one
 * transaction, including its exact byte offset and uncommitted frame state.
 * The guest cursor may move above output without moving the output cursor. */
struct tigt_presenter {
    tigt_presenter_config config;
    locale_t locale;
    bool utf8;
    bool initialized;
    bool fullscreen;
    bool recovery_boundary, recovery_text, recovery_confirming;
    unsigned recovery_ticks;
    bool pending;
    unsigned pending_ticks; /* 1/2100 s: exact 50/60/70 Hz intervals. */
    unsigned video_disabled_ticks;
    bool video_disabled_released;
    bool scroll_holding;
    unsigned scroll_ticks;
    int error;
    unsigned terminal_generation;
    bool background;
    uint16_t columns, rows;
    uint16_t guest_column, guest_row;
    unsigned output_column, output_row;
    unsigned host_columns, host_rows;
    unsigned host_column, host_row, region_top, region_height;
    bool host_cursor_known, host_wrap, matching_echo, echo_failed;
    uint32_t local_echo[TIGT_PRESENTER_LOCAL_ECHO_MAX];
    unsigned echo_start, echo_count, trial_echo_consumed;
    size_t logical_column;
    logical_rows mapping, trial_mapping;
    unsigned boundary_proof[128];
    text_expectation expectations[TIGT_PRESENTER_NOTIFICATION_CAPACITY];
    expectation_queue queue, trial_queue;
    uint64_t vsync_ticks, recent_ids[NOTIFICATION_IDS];
    unsigned recent_next;
    tigt_text_cell *image;
    tigt_text_cell *candidate;
    char *bytes;
    size_t used;
    int buffer_error;
    bool output_pending, next_fullscreen, next_glass_plan, next_recovered;
    size_t written;
    tigt_presenter_frame next_frame;
    glass_cursor next_cursor;
    unsigned next_host_columns, next_host_rows;
    unsigned next_region_top, next_region_height;
    bool registered_fullscreen;
};


typedef struct {
    unsigned top, bottom, count;
    bool incomplete;
} scroll_analysis;

static bool scroll_matches_echo(tigt_presenter *p, const tigt_presenter_frame *frame,
                                const scroll_analysis *scroll);

static void
bytes(tigt_presenter *p, const void *data, size_t count)
{
    if (p->buffer_error != TIGT_OK)
        return;
    if (p->matching_echo && p->trial_echo_consumed < p->echo_count) {
        p->buffer_error = TIGT_ERROR_UNREPRESENTABLE;
        return;
    }
    if (count > PRESENTER_BYTES - p->used) {
        p->buffer_error = TIGT_ERROR_SYSTEM;
        errno = EOVERFLOW;
        return;
    }
    memcpy(p->bytes + p->used, data, count);
    p->used += count;
}

static void
byte(tigt_presenter *p, char value)
{
    if (p->matching_echo && p->trial_echo_consumed < p->echo_count) {
        unsigned slot = (p->echo_start + p->trial_echo_consumed) % TIGT_PRESENTER_LOCAL_ECHO_MAX;
        if (value == '\r' && p->local_echo[slot] == '\n')
            return; /* The CR half of DOS CR/LF was included in host echo. */
        if (value != '\n' || p->local_echo[slot] != '\n')
            p->buffer_error = TIGT_ERROR_UNREPRESENTABLE;
        else
            p->trial_echo_consumed++;
        return;
    }
    bytes(p, &value, 1);
}

static void
sequence(tigt_presenter *p, const char *format, ...)
{
    char text[128];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (length < 0 || (size_t) length >= sizeof(text)) {
        p->buffer_error = TIGT_ERROR_SYSTEM;
        errno = EOVERFLOW;
        return;
    }
    bytes(p, text, (size_t) length);
}

/* Same scalar-to-UTF-8 encoding as the snapshot writer. No locale conversion
 * state, terminal dependency, or per-character allocation is needed. */
static void
glyph(tigt_presenter *p, uint32_t cp)
{
    unsigned char text[4];
    size_t count;
    if (!p->utf8 && cp >= 0x7f)
        cp = '?';
    if (cp < 0x80) {
        text[0] = (unsigned char) cp;
        count = 1;
    } else if (cp < 0x800) {
        text[0] = 0xc0 | (cp >> 6);
        text[1] = 0x80 | (cp & 0x3f);
        count = 2;
    } else if (cp < 0x10000) {
        text[0] = 0xe0 | (cp >> 12);
        text[1] = 0x80 | ((cp >> 6) & 0x3f);
        text[2] = 0x80 | (cp & 0x3f);
        count = 3;
    } else {
        text[0] = 0xf0 | (cp >> 18);
        text[1] = 0x80 | ((cp >> 12) & 0x3f);
        text[2] = 0x80 | ((cp >> 6) & 0x3f);
        text[3] = 0x80 | (cp & 0x3f);
        count = 4;
    }
    bytes(p, text, count);
}

static unsigned
style(const tigt_text_cell *cell)
{
    if (cell->codepoint == ' ')
        return 0;
    return (cell->flags & TIGT_TEXT_UNDERLINE) |
           ((cell->flags & (TIGT_PRESENT_BOLD | TIGT_PRESENT_REVERSE)) ? TIGT_PRESENT_BOLD : 0);
}

static bool
equal(const tigt_text_cell *a, const tigt_text_cell *b)
{
    return a->codepoint == b->codepoint && style(a) == style(b);
}

static const tigt_text_cell blank = { ' ', 0, 0, 0 };

static const tigt_text_cell *
old_cell(const tigt_presenter *p, const glass_cursor *cursor, unsigned row, unsigned column)
{
    if (!p->initialized || cursor->empty_baseline)
        return &blank;
    if (cursor->scroll_count && row >= cursor->scroll_top && row <= cursor->scroll_bottom) {
        if (row + cursor->scroll_count > cursor->scroll_bottom)
            return &blank;
        row += cursor->scroll_count;
    }
    return p->image + (size_t) row * p->columns + column;
}

static void
remove_expectation(expectation_queue *queue, unsigned index, unsigned reason)
{
    if (reason == 0)
        queue->stats.consumed++;
    else if (reason == 1)
        queue->stats.discarded++;
    else
        queue->stats.expired++;
    memmove(queue->order + index, queue->order + index + 1,
            (queue->count - index - 1) * sizeof(*queue->order));
    queue->count--;
}

static void
discard_expectations(expectation_queue *queue)
{
    queue->stats.discarded += queue->count;
    queue->count = 0;
}

static bool
at_boundary(const text_expectation *text, const expectation_position *position)
{
    return position->boundary < text->boundary_count &&
           text->boundaries[position->boundary].text_offset == position->text;
}

static void
step_expectation(const text_expectation *text, expectation_position *position)
{
    if (at_boundary(text, position)) {
        position->boundary++;
        position->row++;
        position->column = 0;
    } else {
        position->text++;
        position->column++;
    }
}

static bool
cursor_changed(const tigt_presenter *p, const tigt_presenter_frame *frame)
{
    return !p->initialized || p->guest_row != frame->cursor_row ||
           p->guest_column != frame->cursor_column;
}

/* Look only along the declared trajectory. Old matching cells can bridge a
 * changed run, but cannot alone turn an unchanged snapshot into confirmation. */
static bool
expectation_evidence(const tigt_presenter *p, const tigt_presenter_frame *frame,
                     const glass_cursor *cursor, const text_expectation *text,
                     expectation_position position)
{
    while (position.text < text->length || at_boundary(text, &position)) {
        if (at_boundary(text, &position)) {
            step_expectation(text, &position);
            continue;
        }
        if (position.row < 0 || position.row >= frame->rows || position.column >= frame->columns)
            return false;
        const tigt_text_cell *cell = p->candidate + (size_t) position.row * frame->columns + position.column;
        if (cell->codepoint != text->text[position.text])
            return false;
        if (old_cell(p, cursor, (unsigned) position.row, position.column)->codepoint != cell->codepoint)
            return true;
        if (cursor_changed(p, frame) &&
            ((frame->cursor_row == position.row && frame->cursor_column > position.column) ||
             (position.column + 1 == frame->columns && frame->cursor_row == position.row + 1 &&
              frame->cursor_column == 0)))
            return true;
        step_expectation(text, &position);
    }
    return false;
}

/* Resynchronization requires a unique three-cell anchor with at least two
 * nonspaces, both in the queued text and on screen. Its position is fixed by
 * the original geometry; this is not a free substring search over the image. */
static bool
resynchronize(tigt_presenter *p, const tigt_presenter_frame *frame,
              const glass_cursor *cursor, const text_expectation *text,
              expectation_position *position)
{
    expectation_position probe = *position;
    unsigned scanned = 0;
    while (probe.text < text->length && scanned < NOTIFICATION_LOOKAHEAD) {
        bool boundary = at_boundary(text, &probe);
        step_expectation(text, &probe);
        if (!boundary)
            scanned++;
        if (at_boundary(text, &probe) || probe.text + 3 > text->length ||
            probe.row < 0 || probe.row >= frame->rows || probe.column + 3 > frame->columns)
            continue;
        if (probe.boundary < text->boundary_count &&
            text->boundaries[probe.boundary].text_offset < probe.text + 3)
            continue;
        const uint32_t *anchor = text->text + probe.text;
        unsigned nonspaces = (anchor[0] != ' ') + (anchor[1] != ' ') + (anchor[2] != ' ');
        if (nonspaces < 2)
            continue;
        const tigt_text_cell *cell = p->candidate + (size_t) probe.row * frame->columns + probe.column;
        if (cell[0].codepoint != anchor[0] || cell[1].codepoint != anchor[1] ||
            cell[2].codepoint != anchor[2] || !expectation_evidence(p, frame, cursor, text, probe))
            continue;
        unsigned occurrences = 0;
        for (unsigned q = 0; q < p->trial_queue.count; q++) {
            unsigned slot = p->trial_queue.order[q];
            const text_expectation *other = p->expectations + slot;
            for (unsigned i = p->trial_queue.position[slot].text; i + 3 <= other->length; i++)
                if (other->text[i] == anchor[0] && other->text[i + 1] == anchor[1] &&
                    other->text[i + 2] == anchor[2])
                    occurrences++;
        }
        if (occurrences != 1)
            continue;
        occurrences = 0;
        for (unsigned y = 0; y < frame->rows; y++)
            for (unsigned x = 0; x + 3 <= frame->columns; x++) {
                const tigt_text_cell *other = p->candidate + (size_t) y * frame->columns + x;
                if (other[0].codepoint == anchor[0] && other[1].codepoint == anchor[1] &&
                    other[2].codepoint == anchor[2])
                    occurrences++;
            }
        if (occurrences == 1) {
            probe.skipped = true;
            probe.had_text = false;
            *position = probe;
            return true;
        }
    }
    return false;
}

static bool
following_expectation(const tigt_presenter *p, const tigt_presenter_frame *frame,
                      const glass_cursor *cursor, unsigned current,
                      const expectation_position *destination)
{
    for (unsigned q = current + 1; q < p->trial_queue.count; q++) {
        unsigned slot = p->trial_queue.order[q];
        const expectation_position *next = p->trial_queue.position + slot;
        const text_expectation *text = p->expectations + slot;
        if (text->columns == frame->columns && text->rows == frame->rows &&
            next->row == destination->row && next->column == destination->column &&
            expectation_evidence(p, frame, cursor, text, *next))
            return true;
    }
    return false;
}

static void
match_expectations(tigt_presenter *p, const tigt_presenter_frame *frame, const glass_cursor *cursor)
{
    expectation_queue *queue = &p->trial_queue;
    for (unsigned q = 0; q < queue->count;) {
        unsigned slot = queue->order[q];
        const text_expectation *text = p->expectations + slot;
        expectation_position *position = queue->position + slot;
        bool discard = text->columns != frame->columns || text->rows != frame->rows;
        unsigned proofs[128] = { 0 };
        while (!discard && (position->text < text->length || at_boundary(text, position))) {
            if (at_boundary(text, position)) {
                expectation_position destination = *position;
                unsigned kind = text->boundaries[position->boundary].kind;
                step_expectation(text, &destination);
                bool crossed = cursor_changed(p, frame) && frame->cursor_row == destination.row &&
                               frame->cursor_column == destination.column;
                expectation_position next_text = destination;
                while (at_boundary(text, &next_text))
                    step_expectation(text, &next_text);
                if (next_text.text < text->length && next_text.row >= 0 && next_text.row < frame->rows &&
                    next_text.column < frame->columns) {
                    const tigt_text_cell *cell =
                        p->candidate + (size_t) next_text.row * frame->columns + next_text.column;
                    if (cell->codepoint != text->text[next_text.text] &&
                        old_cell(p, cursor, (unsigned) next_text.row, next_text.column)->codepoint != cell->codepoint) {
                        discard = true;
                        break;
                    }
                }
                bool evidence = expectation_evidence(p, frame, cursor, text, destination) ||
                                following_expectation(p, frame, cursor, q, &destination);
                if ((!position->had_text && kind == TIGT_BOUNDARY_SOFT_WRAP) ||
                    (!evidence && !crossed))
                    break;
                if (destination.row <= 0 || destination.row >= frame->rows) {
                    discard = true;
                    break;
                }
                if (kind == TIGT_BOUNDARY_NEWLINE ||
                    proofs[destination.row] != TIGT_BOUNDARY_NEWLINE)
                    proofs[destination.row] = kind;
                *position = destination;
                continue;
            }
            if (position->row < 0 || position->row >= frame->rows || position->column >= frame->columns) {
                discard = !resynchronize(p, frame, cursor, text, position);
                continue;
            }
            const tigt_text_cell *cell =
                p->candidate + (size_t) position->row * frame->columns + position->column;
            if (cell->codepoint != text->text[position->text]) {
                if (old_cell(p, cursor, (unsigned) position->row, position->column)->codepoint == cell->codepoint)
                    break;
                discard = !resynchronize(p, frame, cursor, text, position);
                continue;
            }
            if (!expectation_evidence(p, frame, cursor, text, *position))
                break;
            position->had_text = true;
            step_expectation(text, position);
        }
        if (!discard)
            for (unsigned y = 1; y < frame->rows; y++)
                if (proofs[y] == TIGT_BOUNDARY_NEWLINE ||
                    (proofs[y] != 0 && p->boundary_proof[y] != TIGT_BOUNDARY_NEWLINE))
                    p->boundary_proof[y] = proofs[y];
        if (discard || (position->text == text->length && position->boundary == text->boundary_count))
            remove_expectation(queue, q, discard || position->skipped ? 1 : 0);
        else
            q++;
    }
}

static void
glass_glyph(tigt_presenter *p, const tigt_text_cell *cell)
{
    if (p->trial_echo_consumed < p->echo_count) {
        unsigned slot = (p->echo_start + p->trial_echo_consumed) % TIGT_PRESENTER_LOCAL_ECHO_MAX;
        if (p->local_echo[slot] != cell->codepoint)
            p->buffer_error = TIGT_ERROR_UNREPRESENTABLE;
        else
            p->trial_echo_consumed++;
        return;
    }
    if (cell->codepoint != ' ' && (cell->flags & TIGT_TEXT_UNDERLINE))
        bytes(p, "_\b", 2);
    glyph(p, cell->codepoint);
    if (cell->codepoint != ' ' && (cell->flags & (TIGT_PRESENT_BOLD | TIGT_PRESENT_REVERSE))) {
        byte(p, '\b');
        glyph(p, cell->codepoint);
    }
}

static void
forward(tigt_presenter *p, glass_cursor *cursor, const tigt_presenter_frame *frame,
        unsigned target)
{
    const tigt_text_cell *row = p->candidate + (size_t) cursor->row * frame->columns;
    while (cursor->column < target) {
        unsigned stop = cursor->column + (unsigned) (8 - cursor->logical_column % 8);
        bool spaces = p->trial_echo_consumed == p->echo_count &&
                      stop <= target && stop > cursor->column + 1;
        for (unsigned x = cursor->column; spaces && x < stop; x++)
            spaces = row[x].codepoint == ' ' &&
                     old_cell(p, cursor, cursor->row, x)->codepoint == ' ';
        if (spaces) {
            byte(p, '\t');
            cursor->logical_column += stop - cursor->column;
            cursor->column = stop;
        } else {
            glass_glyph(p, row + cursor->column);
            cursor->column++;
            cursor->logical_column++;
        }
    }
}

static void
left(tigt_presenter *p, glass_cursor *cursor, unsigned target)
{
    size_t logical_target = p->trial_mapping.base[cursor->row] + target;
    if (logical_target == 0 && cursor->logical_column != 0) {
        byte(p, '\r');
        cursor->logical_column = 0;
    } else {
        while (cursor->logical_column > logical_target) {
            byte(p, '\b');
            cursor->logical_column--;
        }
    }
    if (cursor->column > target)
        cursor->column = target;
}

static void
advance_row(tigt_presenter *p, glass_cursor *cursor, const tigt_presenter_frame *frame)
{
    unsigned echoed_columns = 0;
    for (unsigned i = p->trial_echo_consumed;
         i < p->echo_count && echoed_columns < frame->columns - cursor->column; i++) {
        if (p->local_echo[(p->echo_start + i) % TIGT_PRESENTER_LOCAL_ECHO_MAX] == '\n')
            break;
        echoed_columns++;
    }
    bool echoed_wrap = p->trial_echo_consumed < p->echo_count &&
                       echoed_columns >= frame->columns - cursor->column;
    bool echoed_newline = p->trial_echo_consumed < p->echo_count &&
        p->local_echo[(p->echo_start + p->trial_echo_consumed) % TIGT_PRESENTER_LOCAL_ECHO_MAX] == '\n';
    unsigned next = cursor->row + 1;
    bool joined = echoed_wrap || (!echoed_newline &&
        (p->boundary_proof[next] == TIGT_BOUNDARY_SOFT_WRAP ||
         (p->boundary_proof[next] != TIGT_BOUNDARY_NEWLINE && p->trial_mapping.joined[next])));
    if (joined) {
        forward(p, cursor, frame, frame->columns);
        if (p->trial_mapping.base[cursor->row] > SIZE_MAX - frame->columns)
            p->buffer_error = TIGT_ERROR_UNREPRESENTABLE;
        else
            p->trial_mapping.base[next] = p->trial_mapping.base[cursor->row] + frame->columns;
    } else {
        byte(p, '\n');
        cursor->logical_column = 0;
        p->trial_mapping.base[next] = 0;
    }
    p->trial_mapping.joined[next] = joined;
    cursor->row = next;
    cursor->column = 0;
}

static bool
retreat_row(tigt_presenter *p, glass_cursor *cursor, unsigned row, unsigned column)
{
    for (unsigned y = cursor->row; y > row; y--)
        if (!p->trial_mapping.joined[y])
            return false;
    cursor->row = row;
    cursor->column = column;
    left(p, cursor, column);
    return true;
}

static bool
screen_blank(const tigt_text_cell *cells, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (cells[i].codepoint != ' ')
            return false;
    return true;
}

static bool
clear_frame(const tigt_presenter *p, const tigt_presenter_frame *frame)
{
    return ((frame->hints & TIGT_PRESENT_VIDEO_DISABLED) ||
            (frame->cursor_column == 0 && frame->cursor_row == 0)) &&
           screen_blank(p->candidate, (size_t) frame->columns * frame->rows);
}

static void
clear_glass(tigt_presenter *p, glass_cursor *cursor)
{
    unsigned end = 0;
    bool first_line = p->image[0].codepoint != ' ';
    for (unsigned y = 0; y < p->rows; y++) {
        for (unsigned x = 0; x < p->columns; x++) {
            if (p->image[(size_t) y * p->columns + x].codepoint == ' ')
                continue;
            if (y != 0)
                first_line = false;
            else
                end = x + 1;
        }
    }
    if (first_line) {
        byte(p, '\r');
        for (unsigned x = 0; x < end; x++)
            byte(p, ' ');
        byte(p, '\r');
    } else {
        byte(p, '\f');
    }
    cursor->column = cursor->row = 0;
    cursor->logical_column = 0;
}

static bool
row_equal(const tigt_text_cell *a, const tigt_text_cell *b, unsigned columns)
{
    for (unsigned x = 0; x < columns; x++)
        if (!equal(a + x, b + x))
            return false;
    return true;
}

/* An exposed row which still contains its old text, possibly after a
 * left-to-right blanking prefix, is not evidence of fresh output. In
 * particular, finishing the copy before clearing the bottom is not a scroll
 * completion. Identical newly printed rows are deliberately ambiguous. */
static bool
uncleared_row(const tigt_text_cell *previous, const tigt_text_cell *row, unsigned columns)
{
    unsigned x = 0;
    while (x < columns && row[x].codepoint == ' ')
        x++;
    return x < columns && row_equal(previous + x, row + x, columns - x);
}

/* The CRTC cursor can lag text already painted and committed. Retain that
 * entire visible prefix, not just the current output position: a later scroll
 * may append after its last glyph without overwriting any emitted text.
 * Wrapped lines and local echo keep their existing stronger suffix proof. */
static unsigned
retained_output_columns(const tigt_presenter *p)
{
    unsigned width = p->output_column < p->columns ? p->output_column : p->columns;
    if (width && (p->mapping.base[p->output_row] != 0 || p->echo_count != 0))
        return width;
    const tigt_text_cell *row = p->image + (size_t) p->output_row * p->columns;
    unsigned end = p->columns;
    while (end > width && row[end - 1].codepoint == ' ')
        end--;
    return end;
}

static bool
copying_rows(const tigt_presenter *p, unsigned top, unsigned bottom, unsigned shift, unsigned width)
{
    size_t first = (size_t) top * p->columns;
    size_t copied_end = (size_t) (bottom + 1 - shift) * p->columns;
    size_t distance = (size_t) shift * p->columns;
    size_t output = (size_t) p->output_row * p->columns;
    /* A previous scroll may have completed between snapshots before this copy
     * began. Its untouched suffix then has a smaller positive displacement,
     * not zero. Fresh rows below that retained suffix do not prove alignment. */
    for (unsigned base = 0; base < shift; base++) {
        size_t end = (size_t) (bottom + 1 - base) * p->columns;
        size_t old_distance = (size_t) base * p->columns;
        bool copying = true, matches = true, moved_text = false, retained_text = false;
        for (size_t i = first; matches && i < end; i++) {
            const tigt_text_cell *cell = p->candidate + i;
            if (copying && (i >= copied_end || !equal(p->image + i + distance, cell)))
                copying = false;
            bool unchanged = equal(p->image + i + old_distance, cell);
            bool suffix = i + old_distance >= output + width &&
                          i + old_distance < output + p->columns;
            if (!copying && !unchanged && !suffix)
                matches = false;
            if (cell->codepoint != ' ') {
                moved_text = moved_text || (copying && !unchanged);
                retained_text = retained_text || (!copying && unchanged);
            }
        }
        if (matches && moved_text && (base == 0 || retained_text))
            return true;
    }
    return false;
}

/* Recognize an upward row copy in display order, not a collection of unrelated
 * matching rows. At a vsync there may be any number of copied rows, one partly
 * copied row, and an untouched (possibly already shifted) suffix. The last
 * committed image remains the source. No retained text means no scroll proof.
 *
 * Unchanged leading rows need not be moved in the logical map. Include an
 * unchanged bottom through the output cursor: a blank bottom row commonly
 * survives a real scroll, including scrolling within a window. */
static scroll_analysis
scrolling(tigt_presenter *p, const tigt_presenter_frame *frame)
{
    scroll_analysis found = { 0 };
    if (!p->initialized || p->columns != frame->columns || p->rows != frame->rows ||
        clear_frame(p, frame))
        return found;
    unsigned top = 0, bottom = frame->rows - 1;
    while (top < frame->rows && row_equal(p->image + (size_t) top * p->columns,
                                         p->candidate + (size_t) top * p->columns, p->columns))
        top++;
    if (top == frame->rows)
        return found;
    while (bottom > top && row_equal(p->image + (size_t) bottom * p->columns,
                                     p->candidate + (size_t) bottom * p->columns, p->columns))
        bottom--;
    if (bottom < p->output_row)
        bottom = p->output_row;
    if (bottom < frame->cursor_row)
        bottom = frame->cursor_row;
    unsigned shifts = p->output_row >= top ? p->output_row - top : 0;
    unsigned width = retained_output_columns(p);
    for (unsigned shift = 1; shift <= shifts; shift++) {
        bool matches = true, text = false;
        for (unsigned y = top; matches && y + shift <= bottom; y++) {
            const tigt_text_cell *previous = p->image + (size_t) (y + shift) * p->columns;
            const tigt_text_cell *row = p->candidate + (size_t) y * p->columns;
            bool row_matches = row_equal(previous, row, p->columns);
            /* Preserve the emitted prefix when output finished its current
             * line before the copy. Wrapped output and echo retain their
             * existing stronger proof for editing the suffix. */
            if (!row_matches && y + shift == p->output_row)
                row_matches = row_equal(previous, row, width);
            matches = matches && row_matches;
            text = text || !screen_blank(previous, p->columns);
        }
        if (!matches) {
            found.incomplete = found.incomplete || copying_rows(p, top, bottom, shift, width);
            continue;
        }
        if (!text)
            continue;
        bool uncleared = false;
        for (unsigned base = 0; !uncleared && base < shift; base++)
            for (unsigned y = bottom + 1 - shift; !uncleared && y + base <= bottom; y++)
                uncleared = uncleared_row(p->image + (size_t) (y + base) * p->columns,
                                           p->candidate + (size_t) y * p->columns, p->columns);
        scroll_analysis candidate = { .top = top, .bottom = bottom, .count = shift };
        /* Local echo is already-emitted evidence, not a prediction. It can
         * distinguish repeated prompts which geometry alone cannot align.
         * This is only a scratch plan: the held echo/map/queue stay intact. */
        if (!uncleared && p->echo_count && !scroll_matches_echo(p, frame, &candidate))
            continue;
        if (uncleared || found.count != 0)
            found.incomplete = true;
        else {
            found.top = top;
            found.bottom = bottom;
            found.count = shift;
        }
    }
    /* A fullscreen application can keep its cursor outside the copied window.
     * Such a copy cannot move the glass output map, but must still be held
     * before repainting. Use the untouched viewport suffix as evidence unless
     * a completed alignment already reaches an observed output frontier. */
    if (p->fullscreen && !found.incomplete &&
        (!found.count || (p->output_row < bottom && frame->cursor_row < bottom))) {
        for (unsigned shift = 1; !found.incomplete && shift < frame->rows - top; shift++)
            found.incomplete = copying_rows(p, top, frame->rows - 1, shift, width);
    }
    return found;
}

static void
apply_scroll(glass_cursor *cursor, const scroll_analysis *scroll)
{
    cursor->scroll_top = scroll->top;
    cursor->scroll_bottom = scroll->bottom;
    cursor->scroll_count = scroll->count;
    cursor->row -= scroll->count;
}

static int
analyze(tigt_presenter *p, const tigt_presenter_frame *frame, glass_cursor *cursor)
{
    bool disabled_blank = (frame->hints & TIGT_PRESENT_VIDEO_DISABLED) && clear_frame(p, frame);
    if (p->echo_failed && !disabled_blank && !(p->initialized && clear_frame(p, frame) &&
        !screen_blank(p->image, (size_t) p->columns * p->rows)))
        return TIGT_ERROR_UNREPRESENTABLE;
    if (p->initialized && clear_frame(p, frame) &&
        !screen_blank(p->image, (size_t) p->columns * p->rows)) {
        clear_glass(p, cursor);
        return p->buffer_error;
    }
    if (disabled_blank) {
        cursor->row = cursor->column = 0;
        cursor->logical_column = 0;
        return TIGT_OK;
    }
    if (p->initialized && (p->columns != frame->columns || p->rows != frame->rows)) {
        if (clear_frame(p, frame)) {
            cursor->row = cursor->column = 0;
            cursor->logical_column = 0;
            return TIGT_OK;
        }
        if (!screen_blank(p->image, (size_t) p->columns * p->rows))
            return TIGT_ERROR_UNREPRESENTABLE;
        /* No visible text needs relocation. Adopt the new geometry at the
         * current output line without inventing a clear. */
        cursor->row = 0;
        left(p, cursor, 0);
    }
    for (unsigned y = 0; y < frame->rows; y++) {
        const tigt_text_cell *row = p->candidate + (size_t) y * frame->columns;
        unsigned first = frame->columns, last = 0;
        for (unsigned x = 0; x < frame->columns; x++) {
            if (!equal(old_cell(p, cursor, y, x), row + x)) {
                if (first == frame->columns)
                    first = x;
                last = x + 1;
            }
        }
        if (first == frame->columns)
            continue;
        if (y < cursor->row && !retreat_row(p, cursor, y, frame->columns))
            return TIGT_ERROR_UNREPRESENTABLE;
        while (cursor->row < y)
            advance_row(p, cursor, frame);
        /* A suffix deletion is the conventional backspace-space-backspace
         * edit, right to left. Merely moving the guest cursor never erases. */
        bool erase = last <= cursor->column;
        for (unsigned x = first; erase && x < last; x++)
            erase = row[x].codepoint == ' ';
        if (erase) {
            left(p, cursor, last);
            while (cursor->column > first) {
                bytes(p, "\b \b", 3);
                cursor->column--;
                cursor->logical_column--;
            }
        } else {
            if (last == cursor->column && first + 1 == last) {
                byte(p, '\b');
                cursor->column--;
                cursor->logical_column--;
            } else
                left(p, cursor, first);
            forward(p, cursor, frame, first);
            forward(p, cursor, frame, last);
        }
    }
    /* Upward motion by itself is harmless. It only becomes a problem when a
     * subsequent text delta actually needs bytes above the emitted cursor. */
    while (cursor->row < frame->cursor_row)
        advance_row(p, cursor, frame);
    if (frame->cursor_row < cursor->row && cursor_changed(p, frame)) {
        /* A confirmed wrapped line can be edited above its physical row.
         * Unrelated upward cursor motion retains the historical glass policy. */
        bool joined = true;
        for (unsigned y = cursor->row; y > frame->cursor_row; y--)
            joined = joined && p->trial_mapping.joined[y];
        if (joined)
            retreat_row(p, cursor, frame->cursor_row, frame->cursor_column);
    }
    if (cursor->row == frame->cursor_row && cursor->column > frame->cursor_column &&
        p->initialized && (p->guest_column != frame->cursor_column || p->guest_row != frame->cursor_row))
        left(p, cursor, frame->cursor_column);
    if (cursor->row == frame->cursor_row && cursor->column < frame->cursor_column) {
        /* The observed cursor confirms spaces as well as visible glyphs.
         * Dropping trailing prompt spaces puts subsequent host local echo at
         * a different column from the guest and makes its exact match fail. */
        forward(p, cursor, frame, frame->cursor_column);
    }
    return p->buffer_error;
}

static void
prepare_plan(tigt_presenter *p, const tigt_presenter_frame *frame, glass_cursor *cursor)
{
    p->trial_echo_consumed = 0;
    p->matching_echo = true;
    p->trial_queue = p->queue;
    p->trial_mapping = p->mapping;
    bool resized = p->initialized && (p->columns != frame->columns || p->rows != frame->rows);
    bool blank_baseline = resized && screen_blank(p->image, (size_t) p->columns * p->rows);
    if (p->fullscreen && screen_blank(p->image, (size_t) p->columns * p->rows))
        memset(&p->trial_mapping, 0, sizeof(p->trial_mapping));
    memset(p->boundary_proof, 0, sizeof(p->boundary_proof));
    p->used = 0;
    p->buffer_error = TIGT_OK;
    if (((frame->hints & TIGT_PRESENT_VIDEO_DISABLED) && clear_frame(p, frame)) ||
        (p->initialized &&
         ((resized && !blank_baseline) ||
          (clear_frame(p, frame) && !screen_blank(p->image, (size_t) p->columns * p->rows))))) {
        discard_expectations(&p->trial_queue);
        p->trial_echo_consumed = p->echo_count;
        memset(&p->trial_mapping, 0, sizeof(p->trial_mapping));
        return;
    }
    if (blank_baseline) {
        /* Match new-geometry predictions against the same blank baseline as
         * analysis, never cells indexed with the old dimensions. The matcher
         * still rejects each prediction carrying the previous geometry. */
        cursor->empty_baseline = true;
        memset(&p->trial_mapping, 0, sizeof(p->trial_mapping));
    }
    if (cursor->scroll_count) {
        for (unsigned y = cursor->scroll_top; y <= cursor->scroll_bottom; y++) {
            unsigned source = y + cursor->scroll_count;
            p->trial_mapping.base[y] = source <= cursor->scroll_bottom ? p->mapping.base[source] : 0;
            p->trial_mapping.joined[y] = source <= cursor->scroll_bottom && p->mapping.joined[source];
        }
        for (unsigned q = 0; q < p->trial_queue.count; q++) {
            expectation_position *position = p->trial_queue.position + p->trial_queue.order[q];
            if (position->row >= (int) cursor->scroll_top && position->row <= (int) cursor->scroll_bottom)
                position->row -= (int) cursor->scroll_count;
        }
    }
    match_expectations(p, frame, cursor);
}

static bool
scroll_matches_echo(tigt_presenter *p, const tigt_presenter_frame *frame,
                    const scroll_analysis *scroll)
{
    glass_cursor cursor = { .column = p->output_column, .row = p->output_row,
                            .logical_column = p->logical_column };
    apply_scroll(&cursor, scroll);
    prepare_plan(p, frame, &cursor);
    return analyze(p, frame, &cursor) == TIGT_OK;
}

static int
plan_glass(tigt_presenter *p, const tigt_presenter_frame *frame, glass_cursor *cursor,
           const scroll_analysis *scroll)
{
    *cursor = (glass_cursor) { .column = p->output_column, .row = p->output_row,
                              .logical_column = p->logical_column };
    if (scroll->count && (p->mapping.base[p->output_row] != 0 || p->echo_count != 0))
        apply_scroll(cursor, scroll);
    prepare_plan(p, frame, cursor);
    int result = analyze(p, frame, cursor);
    if (result != TIGT_ERROR_UNREPRESENTABLE)
        return result;
    *cursor = (glass_cursor) { .column = p->output_column, .row = p->output_row,
                              .logical_column = p->logical_column };
    if (!scroll->count)
        return result;
    apply_scroll(cursor, scroll);
    prepare_plan(p, frame, cursor);
    return analyze(p, frame, cursor);
}

static int
observe_terminal(tigt_presenter *p)
{
    bool background = !tigt_terminal_fd_foreground(p->config.output_fd);
    unsigned epoch = tigt_terminal_generation();
    if (background != p->background || epoch != p->terminal_generation) {
        p->background = background;
        p->terminal_generation = epoch;
        p->host_cursor_known = false;
        if (p->output_pending && p->next_fullscreen) {
            /* A retained fullscreen byte transaction cannot turn into glass
             * halfway through an escape sequence or be replayed on resume. */
            p->error = TIGT_ERROR_BACKGROUND;
            tigt_terminal_record_error(p->error);
            return p->error;
        }
        if (p->fullscreen) {
            p->fullscreen = false;
            p->pending = false;
            p->pending_ticks = 0;
            p->echo_count = 0;
            p->host_wrap = false;
            discard_expectations(&p->queue);
        }
    }
    return TIGT_OK;
}

static int
write_output(tigt_presenter *p, bool nonblocking)
{
    if (p->buffer_error != TIGT_OK)
        return p->buffer_error;
    if (observe_terminal(p) < TIGT_OK) return p->error;
    if (p->next_fullscreen && !tigt_terminal_fd_foreground(p->config.output_fd)) {
        tigt_terminal_record_error(TIGT_ERROR_BACKGROUND);
        return TIGT_ERROR_BACKGROUND;
    }
    if (p->written == p->used)
        return TIGT_OK;
#if defined(__APPLE__)
    /* Follow snapshot.c's borrowed-description SIGPIPE policy on Darwin. */
    int no_sigpipe = fcntl(p->config.output_fd, F_GETNOSIGPIPE);
    if (no_sigpipe < 0 || (no_sigpipe == 0 && fcntl(p->config.output_fd, F_SETNOSIGPIPE, 1) < 0))
        return TIGT_ERROR_SYSTEM;
#else
    sigset_t pipe_set, old_mask, pending;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    int mask_result = pthread_sigmask(SIG_BLOCK, &pipe_set, &old_mask);
    if (mask_result != 0) {
        errno = mask_result;
        return TIGT_ERROR_SYSTEM;
    }
    if (sigpending(&pending) < 0) {
        int saved = errno;
        pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
        errno = saved;
        return TIGT_ERROR_SYSTEM;
    }
    bool pipe_was_pending = sigismember(&pending, SIGPIPE) == 1;
#endif
    int result = TIGT_OK, saved = 0;
    while (p->written < p->used) {
        if (p->next_fullscreen && !tigt_terminal_fd_foreground(p->config.output_fd)) {
            result = TIGT_ERROR_BACKGROUND;
            tigt_terminal_record_error(result);
            break;
        }
        ssize_t count = write(p->config.output_fd, p->bytes + p->written, p->used - p->written);
        if (count > 0) {
            p->written += (size_t) count;
            if (!nonblocking || p->written == p->used)
                continue;
            result = TIGT_PRESENTER_WOULD_BLOCK;
            break;
        } else if (count < 0 && errno == EINTR && !nonblocking)
            continue;
        else {
            saved = count == 0 ? EIO : errno;
            result = count < 0 && (saved == EAGAIN || saved == EWOULDBLOCK ||
                     (nonblocking && saved == EINTR)) ?
                     TIGT_PRESENTER_WOULD_BLOCK : TIGT_ERROR_SYSTEM;
            break;
        }
    }
#if defined(__APPLE__)
    if (no_sigpipe == 0 && fcntl(p->config.output_fd, F_SETNOSIGPIPE, no_sigpipe) < 0 && result >= TIGT_OK) {
        saved = errno;
        result = TIGT_ERROR_SYSTEM;
    }
#else
    if (saved == EPIPE && !pipe_was_pending && sigpending(&pending) == 0 &&
        sigismember(&pending, SIGPIPE) == 1) {
        int received;
        sigwait(&pipe_set, &received);
    }
    int restore = pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    if (restore != 0 && result >= TIGT_OK) {
        saved = restore;
        result = TIGT_ERROR_SYSTEM;
    }
#endif
    if (result != TIGT_OK)
        errno = saved;
    return result;
}

static void
commit(tigt_presenter *p, const tigt_presenter_frame *frame)
{
    memcpy(p->image, p->candidate, (size_t) frame->columns * frame->rows * sizeof(*p->image));
    p->columns = frame->columns;
    p->rows = frame->rows;
    p->guest_column = frame->cursor_column;
    p->guest_row = frame->cursor_row;
    p->initialized = true;
}

/* Track only bytes we actually wrote. Host line-discipline output is accounted
 * separately and invalidates this observation until the consumer reports CPR. */
static void
track_host_output(tigt_presenter *p, size_t offset)
{
    if (!p->host_cursor_known || offset == p->used)
        return;
    struct termios modes;
    if (tcgetattr(p->config.output_fd, &modes) < 0) {
        p->host_cursor_known = false;
        return;
    }
    bool newline_cr = (modes.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR);
    for (size_t i = offset; i < p->used; i++) {
        unsigned char cp = (unsigned char) p->bytes[i];
        if (cp == '\n' || cp == '\f') {
            if (p->host_row < p->host_rows)
                p->host_row++;
            if (cp == '\n' && newline_cr)
                p->host_column = 1;
            p->host_wrap = false;
        } else if (cp == '\r') {
            p->host_column = 1;
            p->host_wrap = false;
        } else if (cp == '\b') {
            if (p->host_column > 1)
                p->host_column--;
            p->host_wrap = false;
        } else if (cp == '\t') {
            unsigned next = ((p->host_column - 1) / 8 + 1) * 8 + 1;
            p->host_column = next < p->host_columns ? next : p->host_columns;
            p->host_wrap = false;
        } else if (cp >= 0x20 && (cp & 0xc0) != 0x80) {
            if (p->host_wrap) {
                if (p->host_row < p->host_rows)
                    p->host_row++;
                p->host_column = 1;
            }
            p->host_wrap = p->host_column == p->host_columns;
            if (!p->host_wrap)
                p->host_column++;
        }
    }
}

static void
commit_glass_plan(tigt_presenter *p, const glass_cursor *cursor)
{
    p->echo_start = (p->echo_start + p->trial_echo_consumed) % TIGT_PRESENTER_LOCAL_ECHO_MAX;
    p->echo_count -= p->trial_echo_consumed;
    p->echo_failed = false;
    p->queue = p->trial_queue;
    p->mapping = p->trial_mapping;
    p->output_column = cursor->column;
    p->output_row = cursor->row;
    p->logical_column = cursor->logical_column;
}

static void
commit_glass(tigt_presenter *p, const tigt_presenter_frame *frame, const glass_cursor *cursor)
{
    commit(p, frame);
    commit_glass_plan(p, cursor);
}

/* Fullscreen frames remain authoritative while a shadow glass trajectory is
 * confirmed. A newline (or observed clear) followed by new text at the output
 * frontier is evidence of sequential output; idle frames and same-line status
 * rewrites alone are not. Require a full 100ms without an incompatible change
 * before switching input modes. No fullscreen text is replayed on recovery. */
static bool
recovering(tigt_presenter *p, const tigt_presenter_frame *frame, const glass_cursor *cursor)
{
    if (clear_frame(p, frame)) {
        p->recovery_boundary = true;
        p->recovery_text = p->recovery_confirming = false;
        p->recovery_ticks = 0;
        return false;
    }
    if (p->columns != frame->columns || p->rows != frame->rows)
        goto incompatible;
    unsigned start_row = p->output_row - cursor->scroll_count;
    bool text = false;
    bool frontier = cursor->row == frame->cursor_row && cursor->column == frame->cursor_column;
    for (unsigned y = 0; y < frame->rows; y++) {
        for (unsigned x = 0; x < frame->columns; x++) {
            const tigt_text_cell *cell = p->candidate + (size_t) y * frame->columns + x;
            if ((y > cursor->row || (y == cursor->row && x >= cursor->column)) &&
                cell->codepoint != ' ')
                frontier = false;
            const tigt_text_cell *previous = old_cell(p, cursor, y, x);
            if (equal(previous, cell))
                continue;
            if (y < start_row || (y == start_row && x < p->output_column) ||
                previous->codepoint != ' ')
                goto incompatible;
            text = text || cell->codepoint != ' ';
        }
    }
    if (cursor->row > start_row)
        p->recovery_boundary = true;
    p->recovery_text = p->recovery_text || (p->recovery_boundary && text);
    if (!frontier || !p->recovery_text) {
        /* VRAM can reach vsync before its CRTC cursor. Keep observed text
         * evidence, but start the quiet interval only when the cursor catches
         * up and no retained text remains beneath or beyond it. */
        p->recovery_confirming = false;
        p->recovery_ticks = 0;
    } else if (p->recovery_confirming)
        p->recovery_ticks += 2100u / frame->refresh_hz;
    else
        p->recovery_confirming = true;
    return p->recovery_confirming && p->recovery_ticks >= 210;

incompatible:
    p->recovery_boundary = p->recovery_text = p->recovery_confirming = false;
    p->recovery_ticks = 0;
    return false;
}

static int
finish_output(tigt_presenter *p, bool nonblocking)
{
    if (!tigt_terminal_begin_io(p->next_fullscreen)) {
        if (p->next_fullscreen) {
            p->error = TIGT_ERROR_BACKGROUND;
            tigt_terminal_record_error(p->error);
            return p->error;
        }
        return TIGT_ERROR_BUSY;
    }
    int result = write_output(p, nonblocking);
    tigt_terminal_end_io();
    if (result == TIGT_PRESENTER_WOULD_BLOCK)
        return result;
    if (result != TIGT_OK) {
        p->error = result;
        return result;
    }
    if (p->next_fullscreen) {
        const tigt_presenter_frame *frame = &p->next_frame;
        unsigned height = p->next_region_height;
        unsigned width = frame->columns < p->next_host_columns ?
                         frame->columns : p->next_host_columns;
        bool entering = !p->fullscreen;
        commit(p, frame);
        p->host_columns = p->next_host_columns;
        p->host_rows = p->next_host_rows;
        p->region_top = p->next_region_top;
        p->region_height = height;
        p->host_row = p->region_top + (frame->cursor_row < height ? frame->cursor_row : height - 1);
        p->host_column = 1 + (frame->cursor_column < width ? frame->cursor_column : width - 1);
        p->host_cursor_known = true;
        p->host_wrap = p->echo_failed = false;
        p->echo_count = 0;
        if (p->next_glass_plan)
            commit_glass_plan(p, &p->next_cursor);
        else {
            discard_expectations(&p->queue);
            memset(&p->mapping, 0, sizeof(p->mapping));
            p->output_column = frame->cursor_column;
            p->output_row = frame->cursor_row;
            p->logical_column = frame->cursor_column;
        }
        p->fullscreen = !(p->next_recovered && frame->cursor_row < height &&
                          frame->cursor_column < p->host_columns);
        if (entering || !p->fullscreen) {
            p->recovery_boundary = p->recovery_text = p->recovery_confirming = false;
            p->recovery_ticks = 0;
        }
    } else {
        track_host_output(p, 0);
        commit_glass(p, &p->next_frame, &p->next_cursor);
        p->fullscreen = false;
    }
    if (p->registered_fullscreen) {
        tigt_terminal_pending_fullscreen(-1);
        p->registered_fullscreen = false;
    }
    p->pending = p->output_pending = false;
    p->pending_ticks = 0;
    return p->fullscreen ? TIGT_PRESENTER_FULLSCREEN : TIGT_OK;
}

static int
begin_output(tigt_presenter *p, const tigt_presenter_frame *frame,
             const glass_cursor *cursor, bool fullscreen, bool recovered, bool nonblocking)
{
    p->next_frame = *frame;
    p->next_frame.cells = p->candidate;
    p->next_frame.stride = frame->columns;
    p->next_fullscreen = fullscreen;
    p->next_glass_plan = cursor != NULL;
    p->next_recovered = recovered;
    if (cursor != NULL)
        p->next_cursor = *cursor;
    p->written = 0;
    p->output_pending = true;
    if (fullscreen) {
        tigt_terminal_pending_fullscreen(1);
        p->registered_fullscreen = true;
    }
    return finish_output(p, nonblocking);
}

static int
require_nonblocking(const tigt_presenter *p)
{
    int flags = fcntl(p->config.output_fd, F_GETFL);
    if (flags < 0)
        return TIGT_ERROR_SYSTEM;
    return (flags & O_NONBLOCK) ? TIGT_OK : TIGT_ERROR_ARGUMENT;
}

static int
terminal_size(tigt_presenter *p, unsigned *columns, unsigned *rows)
{
    struct winsize size;
    if (ioctl(p->config.output_fd, TIOCGWINSZ, &size) < 0 || size.ws_col == 0 || size.ws_row == 0)
        return TIGT_ERROR_TERMINAL;
    *columns = size.ws_col;
    *rows = size.ws_row;
    return TIGT_OK;
}

static void
prepare_region(tigt_presenter *p, unsigned top, unsigned height)
{
    for (unsigned y = top; y < top + height; y++)
        sequence(p, "\033[%u;1H\033[2K", y);
}

static void
ansi_style(tigt_presenter *p, const tigt_text_cell *cell)
{
    uint32_t fg = cell->foreground, bg = cell->background;
    sequence(p, "\033[0%s%s%s;38;2;%u;%u;%u;48;2;%u;%u;%um",
             (cell->flags & TIGT_PRESENT_BOLD) ? ";1" : "",
             (cell->flags & TIGT_TEXT_UNDERLINE) ? ";4" : "",
             (cell->flags & TIGT_PRESENT_REVERSE) ? ";7" : "",
             (fg >> 16) & 255, (fg >> 8) & 255, fg & 255,
             (bg >> 16) & 255, (bg >> 8) & 255, bg & 255);
}

static bool
ansi_equal(const tigt_text_cell *a, const tigt_text_cell *b)
{
    return a->codepoint == b->codepoint && a->foreground == b->foreground &&
           a->background == b->background && ((a->flags ^ b->flags) & GLASS_FLAGS) == 0;
}

static int
draw_fullscreen(tigt_presenter *p, const tigt_presenter_frame *frame, bool entering,
                const glass_cursor *cursor, bool recovered, bool nonblocking)
{
    if (!tigt_terminal_fd_foreground(p->config.output_fd))
        return TIGT_ERROR_UNREPRESENTABLE;
    unsigned host_columns, host_rows;
    int result = terminal_size(p, &host_columns, &host_rows);
    if (result != TIGT_OK)
        return result;
    bool resize = !p->host_cursor_known || host_columns != p->host_columns || host_rows != p->host_rows ||
                  p->columns != frame->columns || p->rows != frame->rows;
    unsigned height = frame->rows < host_rows ? frame->rows : host_rows;
    unsigned width = frame->columns < host_columns ? frame->columns : host_columns;
    if (entering && (!p->host_cursor_known || host_columns != p->host_columns ||
                     host_rows != p->host_rows))
        return TIGT_PRESENTER_NEEDS_CURSOR;
    unsigned top = entering ? p->host_row + (p->host_column > 1 || p->host_wrap) : p->region_top;
    if (!entering && top > host_rows)
        top = host_rows;
    unsigned scroll = top + height - 1 > host_rows ? top + height - 1 - host_rows : 0;
    top -= scroll;
    p->used = 0;
    p->buffer_error = TIGT_OK;
    p->matching_echo = false;
    if (entering || resize) {
        if (scroll) {
            sequence(p, "\033[%u;1H", host_rows);
            for (unsigned y = 0; y < scroll; y++)
                byte(p, '\n');
        }
        unsigned old_top = p->region_top > scroll ? p->region_top - scroll : 1;
        unsigned end = top + height;
        if (!entering && old_top + p->region_height > end)
            end = old_top + p->region_height;
        if (end > host_rows + 1)
            end = host_rows + 1;
        prepare_region(p, top, end - top);
    }
    bool emitted = entering || resize;
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width;) {
            size_t index = (size_t) y * frame->columns + x;
            if (!entering && !resize && ansi_equal(p->image + index, p->candidate + index)) {
                x++;
                continue;
            }
            const tigt_text_cell *cell = p->candidate + index;
            sequence(p, "\033[%u;%uH", top + y, x + 1);
            ansi_style(p, cell);
            do {
                glyph(p, p->candidate[index].codepoint);
                x++;
                index++;
            } while (x < width && p->candidate[index].foreground == cell->foreground &&
                     p->candidate[index].background == cell->background &&
                     ((p->candidate[index].flags ^ cell->flags) & GLASS_FLAGS) == 0 &&
                     (entering || resize || !ansi_equal(p->image + index, p->candidate + index)));
            emitted = true;
        }
    }
    if (emitted)
        bytes(p, "\033[0m", 4);
    if (emitted || p->guest_column != frame->cursor_column || p->guest_row != frame->cursor_row) {
        unsigned y = frame->cursor_row < height ? frame->cursor_row : height - 1;
        unsigned x = frame->cursor_column < width ? frame->cursor_column : width - 1;
        sequence(p, "\033[%u;%uH", top + y, x + 1);
    }
    p->next_host_columns = host_columns;
    p->next_host_rows = host_rows;
    p->next_region_top = top;
    p->next_region_height = height;
    return begin_output(p, frame, cursor, true, recovered, nonblocking);
}

static int
validate(tigt_presenter *p, const tigt_presenter_frame *frame)
{
    if (frame == NULL || frame->cells == NULL || frame->columns == 0 || frame->columns > 320 ||
        frame->rows == 0 || frame->rows > 128 || frame->stride < frame->columns ||
        (size_t) frame->columns * frame->rows > PRESENTER_CELLS ||
        frame->cursor_column >= frame->columns || frame->cursor_row >= frame->rows ||
        (frame->refresh_hz != 50 && frame->refresh_hz != 60 && frame->refresh_hz != 70) ||
        (frame->hints & ~(TIGT_PRESENT_VIDEO_DISABLED | TIGT_PRESENT_VIDEO_MEMORY_CHANGED)) != 0)
        return TIGT_ERROR_ARGUMENT;
    locale_t previous = (locale_t) 0;
    if (p->utf8) {
        previous = uselocale(p->locale);
        if (previous == (locale_t) 0)
            return TIGT_ERROR_SYSTEM;
    }
    int result = TIGT_OK;
    for (unsigned y = 0; y < frame->rows && result == TIGT_OK; y++) {
        for (unsigned x = 0; x < frame->columns; x++) {
            const tigt_text_cell *cell = frame->cells + (size_t) y * frame->stride + x;
            uint32_t cp = cell->codepoint;
            if (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || cp > 0x10ffff ||
                (cp >= 0xd800 && cp <= 0xdfff) ||
                ((cell->foreground | cell->background) & 0xff000000) != 0 ||
                (p->utf8 && wcwidth((wchar_t) cp) != 1)) {
                result = TIGT_ERROR_ARGUMENT;
                break;
            }
            p->candidate[(size_t) y * frame->columns + x] = *cell;
        }
    }
    if (p->utf8 && uselocale(previous) == (locale_t) 0)
        result = TIGT_ERROR_SYSTEM;
    return result;
}

static bool
utf8_codeset(const char *name)
{
    return name != NULL && (strcmp(name, "UTF-8") == 0 || strcmp(name, "UTF8") == 0 ||
                            strcmp(name, "utf-8") == 0 || strcmp(name, "utf8") == 0);
}

int
tigt_presenter_create(const tigt_presenter_config *config, tigt_presenter **output)
{
    if (output == NULL)
        return TIGT_ERROR_ARGUMENT;
    *output = NULL;
    if (config == NULL || config->abi_version != TIGT_PRESENTER_ABI_VERSION ||
        config->output_fd < 0 || config->mode > TIGT_PRESENT_ADAPTIVE ||
        config->encoding > TIGT_ENCODING_ASCII || config->reversible > 1)
        return TIGT_ERROR_ARGUMENT;
    int flags = fcntl(config->output_fd, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) == O_RDONLY)
        return TIGT_ERROR_ARGUMENT;
    if (config->mode == TIGT_PRESENT_ADAPTIVE && !isatty(config->output_fd))
        return TIGT_ERROR_TERMINAL;
    tigt_presenter *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return TIGT_ERROR_SYSTEM;
    p->config = *config;
    p->terminal_generation = tigt_terminal_generation();
    p->background = !tigt_terminal_fd_foreground(config->output_fd);
    if (config->encoding == TIGT_ENCODING_LOCALE) {
        p->locale = newlocale(LC_CTYPE_MASK, "", (locale_t) 0);
        if (p->locale == (locale_t) 0) {
            free(p);
            return TIGT_ERROR_ARGUMENT;
        }
        p->utf8 = utf8_codeset(nl_langinfo_l(CODESET, p->locale));
    } else if (config->encoding == TIGT_ENCODING_UTF8) {
        p->locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t) 0);
        if (p->locale == (locale_t) 0)
            p->locale = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", (locale_t) 0);
        if (p->locale == (locale_t) 0) {
            free(p);
            return TIGT_ERROR_TERMINAL;
        }
        p->utf8 = true;
    }
    p->image = malloc(PRESENTER_CELLS * sizeof(*p->image));
    p->candidate = malloc(PRESENTER_CELLS * sizeof(*p->candidate));
    p->bytes = malloc(PRESENTER_BYTES);
    if (p->image == NULL || p->candidate == NULL || p->bytes == NULL) {
        tigt_presenter_destroy(p);
        return TIGT_ERROR_SYSTEM;
    }
    *output = p;
    return TIGT_OK;
}

int
tigt_presenter_observe_cursor(tigt_presenter *p, unsigned column, unsigned row)
{
    if (p == NULL || column == 0 || row == 0)
        return TIGT_ERROR_ARGUMENT;
    if (p->output_pending)
        return TIGT_ERROR_BUSY;
    unsigned columns, rows;
    int result = terminal_size(p, &columns, &rows);
    if (result != TIGT_OK)
        return result;
    if (column > columns || row > rows)
        return TIGT_ERROR_ARGUMENT;
    p->host_columns = columns;
    p->host_rows = rows;
    p->host_column = column;
    p->host_row = row;
    p->host_wrap = false;
    p->host_cursor_known = true;
    return TIGT_OK;
}

int
tigt_presenter_forget_cursor(tigt_presenter *p)
{
    if (p == NULL)
        return TIGT_ERROR_ARGUMENT;
    if (p->output_pending)
        return TIGT_ERROR_BUSY;
    p->host_cursor_known = false;
    return TIGT_OK;
}

int
tigt_presenter_local_echo(tigt_presenter *p, const uint32_t *text, size_t length)
{
    if (p == NULL || (length != 0 && text == NULL))
        return TIGT_ERROR_ARGUMENT;
    if (p->output_pending)
        return TIGT_ERROR_BUSY;
    if (!p->initialized || p->fullscreen)
        return TIGT_ERROR_ARGUMENT;
    if (p->error != TIGT_OK)
        return p->error;
    if (p->echo_failed)
        return TIGT_ERROR_UNREPRESENTABLE;
    if (length == 0)
        return TIGT_OK;
    size_t column = p->logical_column;
    for (unsigned i = 0; i < p->echo_count; i++)
        column = p->local_echo[(p->echo_start + i) % TIGT_PRESENTER_LOCAL_ECHO_MAX] == '\n' ?
                 0 : column + 1;
    size_t start_column = column, count = 0;
    locale_t previous = (locale_t) 0;
    if (p->utf8) {
        previous = uselocale(p->locale);
        if (previous == (locale_t) 0)
            return TIGT_ERROR_SYSTEM;
    }
    int result = TIGT_OK;
    for (size_t i = 0; i < length; i++) {
        uint32_t cp = text[i];
        if (cp != '\n' && cp != '\t' &&
            (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || cp > 0x10ffff ||
             (cp >= 0xd800 && cp <= 0xdfff) || (p->utf8 && wcwidth((wchar_t) cp) != 1))) {
            result = TIGT_ERROR_ARGUMENT;
            break;
        }
        size_t width = cp == '\t' ? 8 - column % 8 : 1;
        if (width > TIGT_PRESENTER_LOCAL_ECHO_MAX - p->echo_count - count) {
            result = TIGT_ERROR_UNREPRESENTABLE;
            break;
        }
        count += width;
        column = cp == '\n' ? 0 : column + width;
    }
    if (p->utf8 && uselocale(previous) == (locale_t) 0)
        return TIGT_ERROR_SYSTEM;
    if (result == TIGT_ERROR_UNREPRESENTABLE) {
        p->echo_failed = true;
        p->host_cursor_known = false;
    }
    if (result != TIGT_OK)
        return result;
    column = start_column;
    for (size_t i = 0; i < length; i++) {
        uint32_t cp = text[i];
        size_t width = cp == '\t' ? 8 - column % 8 : 1;
        for (size_t j = 0; j < width; j++)
            p->local_echo[(p->echo_start + p->echo_count++) % TIGT_PRESENTER_LOCAL_ECHO_MAX] =
                cp == '\t' ? ' ' : cp;
        column = cp == '\n' ? 0 : column + width;
    }
    p->host_cursor_known = false;
    return TIGT_OK;
}

int
tigt_presenter_notify(tigt_presenter *p, const tigt_presenter_notification *notification)
{
    if (p == NULL || notification == NULL || notification->operation_id == 0 ||
        notification->columns == 0 || notification->columns > 320 ||
        notification->rows == 0 || notification->rows > 128 ||
        (size_t) notification->columns * notification->rows > PRESENTER_CELLS ||
        notification->start_column >= notification->columns || notification->start_row >= notification->rows ||
        (notification->text_length != 0 && notification->text == NULL) ||
        (notification->boundary_count != 0 && notification->boundaries == NULL) ||
        (notification->text_length == 0 && notification->boundary_count == 0))
        return TIGT_ERROR_ARGUMENT;
    if (p->error != TIGT_OK)
        return p->error;
    if (p->output_pending)
        return TIGT_ERROR_BUSY;
    for (unsigned q = 0; q < p->queue.count; q++)
        if (p->expectations[p->queue.order[q]].id == notification->operation_id)
            return TIGT_OK;
    for (unsigned i = 0; i < NOTIFICATION_IDS; i++)
        if (p->recent_ids[i] == notification->operation_id)
            return TIGT_OK;
    if (notification->text_length > TIGT_PRESENTER_NOTIFICATION_TEXT_MAX ||
        notification->boundary_count > TIGT_PRESENTER_NOTIFICATION_BOUNDARIES_MAX) {
        discard_expectations(&p->queue);
        p->queue.stats.discarded++;
        return TIGT_PRESENTER_NOTIFY_DROPPED;
    }
    size_t previous = 0;
    unsigned column = notification->start_column;
    for (size_t i = 0; i < notification->boundary_count; i++) {
        const tigt_presenter_boundary *boundary = notification->boundaries + i;
        if (boundary->text_offset < previous || boundary->text_offset > notification->text_length ||
            boundary->text_offset - previous > notification->columns - column ||
            (boundary->kind != TIGT_BOUNDARY_SOFT_WRAP && boundary->kind != TIGT_BOUNDARY_NEWLINE))
            return TIGT_ERROR_ARGUMENT;
        column += (unsigned) (boundary->text_offset - previous);
        if (boundary->kind == TIGT_BOUNDARY_SOFT_WRAP &&
            (column != notification->columns || boundary->text_offset == 0))
            return TIGT_ERROR_ARGUMENT;
        previous = boundary->text_offset;
        column = 0;
    }
    if (notification->text_length - previous > notification->columns - column)
        return TIGT_ERROR_ARGUMENT;
    locale_t previous_locale = (locale_t) 0;
    if (p->utf8) {
        previous_locale = uselocale(p->locale);
        if (previous_locale == (locale_t) 0)
            return TIGT_ERROR_SYSTEM;
    }
    bool valid = true;
    for (size_t i = 0; i < notification->text_length; i++) {
        uint32_t cp = notification->text[i];
        if (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff) || (p->utf8 && wcwidth((wchar_t) cp) != 1)) {
            valid = false;
            break;
        }
    }
    if (p->utf8 && uselocale(previous_locale) == (locale_t) 0)
        return TIGT_ERROR_SYSTEM;
    if (!valid)
        return TIGT_ERROR_ARGUMENT;
    if (p->queue.count == TIGT_PRESENTER_NOTIFICATION_CAPACITY) {
        discard_expectations(&p->queue);
        p->queue.stats.discarded++;
        return TIGT_PRESENTER_NOTIFY_DROPPED;
    }
    bool occupied[TIGT_PRESENTER_NOTIFICATION_CAPACITY] = { false };
    for (unsigned q = 0; q < p->queue.count; q++)
        occupied[p->queue.order[q]] = true;
    unsigned slot = 0;
    while (occupied[slot])
        slot++;
    text_expectation *text = p->expectations + slot;
    text->id = notification->operation_id;
    text->length = (unsigned) notification->text_length;
    text->boundary_count = (unsigned) notification->boundary_count;
    text->columns = notification->columns;
    text->rows = notification->rows;
    if (text->length)
        memcpy(text->text, notification->text, text->length * sizeof(*text->text));
    if (text->boundary_count)
        memcpy(text->boundaries, notification->boundaries, text->boundary_count * sizeof(*text->boundaries));
    p->queue.position[slot] = (expectation_position) {
        .born = p->vsync_ticks, .row = notification->start_row, .column = notification->start_column
    };
    p->queue.order[p->queue.count++] = slot;
    p->recent_ids[p->recent_next++ % NOTIFICATION_IDS] = notification->operation_id;
    return TIGT_OK;
}

int
tigt_presenter_cancel(tigt_presenter *p, uint64_t operation_id)
{
    if (p == NULL || operation_id == 0)
        return TIGT_ERROR_ARGUMENT;
    if (p->output_pending)
        return TIGT_ERROR_BUSY;
    for (unsigned q = 0; q < p->queue.count; q++)
        if (p->expectations[p->queue.order[q]].id == operation_id) {
            remove_expectation(&p->queue, q, 1);
            break;
        }
    for (unsigned i = 0; i < NOTIFICATION_IDS; i++)
        if (p->recent_ids[i] == operation_id)
            return TIGT_OK;
    p->recent_ids[p->recent_next++ % NOTIFICATION_IDS] = operation_id;
    return TIGT_OK;
}

int
tigt_presenter_get_notification_stats(const tigt_presenter *p, tigt_presenter_notification_stats *stats)
{
    if (p == NULL || stats == NULL)
        return TIGT_ERROR_ARGUMENT;
    *stats = p->queue.stats;
    stats->queued = p->queue.count;
    return TIGT_OK;
}

static int
present(tigt_presenter *p, const tigt_presenter_frame *frame, bool nonblocking)
{
    if (p == NULL)
        return TIGT_ERROR_ARGUMENT;
    int terminal_result = observe_terminal(p);
    if (terminal_result < TIGT_OK) return terminal_result;
    if (p->error != TIGT_OK)
        return p->error;
    if (p->output_pending)
        return TIGT_ERROR_BUSY;
    if (nonblocking) {
        int mode_result = require_nonblocking(p);
        if (mode_result != TIGT_OK)
            return mode_result;
    }
    int result = validate(p, frame);
    if (result != TIGT_OK)
        return result;
    p->vsync_ticks += 2100u / frame->refresh_hz;
    for (unsigned q = 0; q < p->queue.count;) {
        const expectation_position *position = p->queue.position + p->queue.order[q];
        if (p->vsync_ticks - position->born >=
            (uint64_t) TIGT_PRESENTER_NOTIFICATION_MAX_AGE_MS * 21u / 10u)
            remove_expectation(&p->queue, q, 2);
        else
            q++;
    }
    /* Hold before planning: the retained image, logical cursor, echo and
     * recovery trajectory describe what was actually emitted. Aging above is
     * independent, just as it is during representability confirmation. */
    bool disabled = (frame->hints & TIGT_PRESENT_VIDEO_DISABLED) != 0;
    if (!disabled) {
        p->video_disabled_ticks = 0;
        p->video_disabled_released = false;
    } else {
        if (frame->hints & TIGT_PRESENT_VIDEO_MEMORY_CHANGED)
            p->video_disabled_released = true;
        if (!p->video_disabled_released) {
            p->video_disabled_ticks += 2100u / frame->refresh_hz;
            if (p->video_disabled_ticks >= 420)
                p->video_disabled_released = true;
        }
    }
    scroll_analysis scroll = scrolling(p, frame);
    if (scroll.incomplete || (disabled && scroll.count)) {
        if (!p->scroll_holding) {
            p->scroll_holding = true;
            p->scroll_ticks = 0;
        } else if (p->scroll_ticks < 1050)
            p->scroll_ticks += 2100u / frame->refresh_hz;
        /* Inspect the underlying text even during hardware blanking. A proven
         * scroll can finish while video is still disabled, but must not paint
         * its raw cells. Neither progress nor completion renews this 500ms
         * deadline; a stopped copy eventually takes the ordinary fallback. */
        if (p->scroll_ticks < 1050)
            return TIGT_PRESENTER_PENDING;
        if (!disabled) {
            p->pending = true;
            p->pending_ticks = 210;
        }
    } else {
        p->scroll_holding = false;
        p->scroll_ticks = 0;
    }
    if (disabled) {
        if (!p->video_disabled_released)
            return TIGT_PRESENTER_PENDING;
        /* Non-scroll VRAM changes still release the ordinary disable hold
         * immediately. Once released (or timed out), show only hardware black,
         * never the underlying cells used to recognize a copying operation. */
        for (size_t i = 0; i < (size_t) frame->columns * frame->rows; i++)
            p->candidate[i] = blank;
        scroll = (scroll_analysis) { 0 };
    }
    if (p->fullscreen) {
        glass_cursor cursor;
        unsigned old_column = p->output_column, old_row = p->output_row;
        size_t old_logical_column = p->logical_column;
        bool blank_baseline = screen_blank(p->image, (size_t) p->columns * p->rows);
        if (blank_baseline) {
            /* A blank fullscreen baseline has no retained text. Probe from
             * its origin, but leave committed state intact during backpressure. */
            p->output_column = p->output_row = 0;
            p->logical_column = 0;
        }
        bool planned = !scroll.incomplete && p->config.reversible &&
                       plan_glass(p, frame, &cursor, &scroll) == TIGT_OK;
        bool recovered = planned && recovering(p, frame, &cursor);
        p->output_column = old_column;
        p->output_row = old_row;
        p->logical_column = old_logical_column;
        if (!planned) {
            p->recovery_boundary = p->recovery_text = p->recovery_confirming = false;
            p->recovery_ticks = 0;
        }
        /* The shadow map and recovery decision commit only after the entire
         * fullscreen repaint, including its cursor, has reached the host.
         * Recovery continues in place without clearing or replaying the region. */
        result = draw_fullscreen(p, frame, false, planned ? &cursor : NULL, recovered, nonblocking);
        if (result < TIGT_OK)
            p->error = result;
        return result;
    }
    glass_cursor cursor;
    result = scroll.incomplete ? TIGT_ERROR_UNREPRESENTABLE : plan_glass(p, frame, &cursor, &scroll);
    if (result == TIGT_OK)
        return begin_output(p, frame, &cursor, false, false, nonblocking);
    else if (result == TIGT_ERROR_UNREPRESENTABLE) {
        if (!p->pending) {
            p->pending = true;
            p->pending_ticks = 0;
        } else
            p->pending_ticks += 2100u / frame->refresh_hz;
        if (p->pending_ticks < 210)
            return TIGT_PRESENTER_PENDING;
        if (p->config.mode == TIGT_PRESENT_ADAPTIVE && !p->background)
            result = draw_fullscreen(p, frame, true, NULL, false, nonblocking);
    }
    if (result < TIGT_OK)
        p->error = result;
    return result;
}

int
tigt_presenter_present(tigt_presenter *p, const tigt_presenter_frame *frame)
{
    return present(p, frame, false);
}

int
tigt_presenter_present_nonblocking(tigt_presenter *p, const tigt_presenter_frame *frame)
{
    return present(p, frame, true);
}

int
tigt_presenter_resume(tigt_presenter *p)
{
    if (p == NULL)
        return TIGT_ERROR_ARGUMENT;
    int terminal_result = observe_terminal(p);
    if (terminal_result < TIGT_OK) return terminal_result;
    if (p->error != TIGT_OK)
        return p->error;
    if (!p->output_pending)
        return TIGT_ERROR_BUSY;
    int result = require_nonblocking(p);
    return result == TIGT_OK ? finish_output(p, true) : result;
}

int
tigt_presenter_fullscreen_output_started(const tigt_presenter *p)
{
    return p != NULL && (p->fullscreen ||
        (p->output_pending && p->next_fullscreen && p->written != 0));
}

int
tigt_presenter_reset(tigt_presenter *p)
{
    if (p == NULL)
        return TIGT_ERROR_ARGUMENT;
    if (p->registered_fullscreen) {
        tigt_terminal_pending_fullscreen(-1);
        p->registered_fullscreen = false;
    }
    p->initialized = p->fullscreen = p->pending = p->output_pending = false;
    p->recovery_boundary = p->recovery_text = p->recovery_confirming = false;
    p->pending_ticks = p->recovery_ticks = 0;
    p->video_disabled_ticks = 0;
    p->video_disabled_released = false;
    p->scroll_holding = false;
    p->scroll_ticks = 0;
    p->error = p->buffer_error = TIGT_OK;
    p->columns = p->rows = p->guest_column = p->guest_row = 0;
    p->output_column = p->output_row = p->host_columns = p->host_rows = 0;
    p->host_column = p->host_row = p->region_top = p->region_height = 0;
    p->host_cursor_known = p->host_wrap = p->matching_echo = p->echo_failed = false;
    p->echo_start = p->echo_count = p->trial_echo_consumed = 0;
    p->used = p->written = 0;
    discard_expectations(&p->queue);
    memset(&p->mapping, 0, sizeof(p->mapping));
    memset(p->recent_ids, 0, sizeof(p->recent_ids));
    p->logical_column = p->vsync_ticks = p->recent_next = 0;
    p->terminal_generation = tigt_terminal_generation();
    p->background = !tigt_terminal_fd_foreground(p->config.output_fd);
    return TIGT_OK;
}

void
tigt_presenter_destroy(tigt_presenter *p)
{
    if (p == NULL)
        return;
    if (p->registered_fullscreen) tigt_terminal_pending_fullscreen(-1);
    if (p->locale != (locale_t) 0)
        freelocale(p->locale);
    free(p->image);
    free(p->candidate);
    free(p->bytes);
    free(p);
}

/* Vertical tabs, additional character sets (notably ISO-8859-1), terminal
 * querying, cooked input and editing-key handling remain consumer work. */
