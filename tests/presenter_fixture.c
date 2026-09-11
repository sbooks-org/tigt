/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 * Public-API component tests: real files/pipes/PTYS, no output substitutes.
 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include "tigt_presenter.h"
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "presenter_fixture:%d: %s\n", __LINE__, #expression); \
        exit(1); \
    } \
} while (0)
#define SGR "\033[0;38;2;170;170;170;48;2;0;0;0m"
#define BLACK_SGR "\033[0;38;2;0;0;0;48;2;0;0;0m"

typedef struct {
    FILE *file;
    int master, slave;
    off_t offset;
    pthread_t thread;
    int concurrent_output, running, result, expected_result;
    tigt_presenter *presenter;
    tigt_presenter_frame frame;
    tigt_text_cell cells[21440];
} fixture;

static void
blank_screen(fixture *f)
{
    for (unsigned y = 0; y < f->frame.rows; y++)
        for (unsigned x = 0; x < f->frame.stride; x++)
            f->cells[(size_t) y * f->frame.stride + x] =
                (tigt_text_cell) { ' ', 0xaaaaaa, 0, 0 };
    f->frame.cursor_column = f->frame.cursor_row = 0;
}

static void
text(fixture *f, unsigned row, unsigned column, const char *value)
{
    while (*value)
        f->cells[(size_t) row * f->frame.stride + column++].codepoint = (unsigned char) *value++;
}

static void
position(fixture *f, unsigned row, unsigned column)
{
    f->frame.cursor_row = (uint16_t) row;
    f->frame.cursor_column = (uint16_t) column;
}

static void
create_file(fixture *f, unsigned columns, unsigned rows, unsigned hz, unsigned encoding)
{
    memset(f, 0, sizeof(*f));
    f->master = f->slave = -1;
    f->file = tmpfile();
    CHECK(f->file != NULL);
    f->frame = (tigt_presenter_frame) {
        .cells = f->cells, .columns = columns, .rows = rows, .stride = columns,
        .refresh_hz = hz
    };
    blank_screen(f);
    tigt_presenter_config config = {
        TIGT_PRESENTER_ABI_VERSION, fileno(f->file), TIGT_PRESENT_GLASS, encoding, 0
    };
    CHECK(tigt_presenter_create(&config, &f->presenter) == TIGT_OK);
}

static void
create_pty(fixture *f, unsigned guest_columns, unsigned guest_rows,
           unsigned host_columns, unsigned host_rows, unsigned reversible)
{
    memset(f, 0, sizeof(*f));
    f->master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    CHECK(f->master >= 0);
    CHECK(grantpt(f->master) == 0);
    CHECK(unlockpt(f->master) == 0);
    char *name = ptsname(f->master);
    CHECK(name != NULL);
    f->slave = open(name, O_RDWR | O_NOCTTY);
    CHECK(f->slave >= 0);
    struct termios modes;
    CHECK(tcgetattr(f->slave, &modes) == 0);
    modes.c_oflag &= ~OPOST;
    CHECK(tcsetattr(f->slave, TCSANOW, &modes) == 0);
    struct winsize size = { .ws_col = host_columns, .ws_row = host_rows };
    CHECK(ioctl(f->slave, TIOCSWINSZ, &size) == 0);
    f->frame = (tigt_presenter_frame) {
        .cells = f->cells, .columns = guest_columns, .rows = guest_rows,
        .stride = guest_columns, .refresh_hz = 50
    };
    blank_screen(f);
    tigt_presenter_config config = {
        TIGT_PRESENTER_ABI_VERSION, f->slave, TIGT_PRESENT_ADAPTIVE, TIGT_ENCODING_ASCII, reversible
    };
    CHECK(tigt_presenter_create(&config, &f->presenter) == TIGT_OK);
}

static void *
present_thread(void *opaque)
{
    fixture *f = opaque;
    f->result = tigt_presenter_present(f->presenter, &f->frame);
    return NULL;
}

/* A real terminal drains concurrently. Darwin's PTY queue can be smaller than
 * a 25-row ANSI transaction; never make the writer wait for its own reader. */
static void
submit_output(fixture *f, int expected)
{
    CHECK(!f->running);
    if (f->concurrent_output) {
        f->expected_result = expected;
        f->running = 1;
        CHECK(pthread_create(&f->thread, NULL, present_thread, f) == 0);
    } else
        CHECK(tigt_presenter_present(f->presenter, &f->frame) == expected);
}

static void
expect_bytes(fixture *f, const char *expected, size_t count)
{
    unsigned char actual[16384];
    size_t used = 0;
    if (f->file != NULL) {
        struct stat status;
        CHECK(fstat(fileno(f->file), &status) == 0);
        CHECK(status.st_size >= f->offset);
        CHECK((size_t) (status.st_size - f->offset) < sizeof(actual));
        used = (size_t) (status.st_size - f->offset);
        if (used)
            CHECK(pread(fileno(f->file), actual, used, f->offset) == (ssize_t) used);
        f->offset = status.st_size;
    } else {
        for (;;) {
            ssize_t amount = read(f->master, actual + used, sizeof(actual) - used);
            if (amount > 0) {
                used += (size_t) amount;
                CHECK(used < sizeof(actual));
            } else if (amount < 0 && errno == EINTR)
                continue;
            else {
                CHECK(amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
                if (used < count) {
                    struct pollfd descriptor = { .fd = f->master, .events = POLLIN };
                    int ready;
                    do {
                        ready = poll(&descriptor, 1, 1000);
                    } while (ready < 0 && errno == EINTR);
                    CHECK(ready >= 0);
                    if (ready > 0)
                        continue;
                }
                break;
            }
        }
    }
    if (f->running) {
        CHECK(pthread_join(f->thread, NULL) == 0);
        f->running = 0;
        CHECK(f->result == f->expected_result);
    }
    if (used != count || memcmp(actual, expected, count) != 0) {
        fprintf(stderr, "expected %zu bytes, received %zu\nexpected:", count, used);
        for (size_t i = 0; i < count; i++)
            fprintf(stderr, " %02x", (unsigned char) expected[i]);
        fprintf(stderr, "\nreceived:");
        for (size_t i = 0; i < used; i++)
            fprintf(stderr, " %02x", actual[i]);
        fputc('\n', stderr);
        exit(1);
    }
}

#define EXPECT(f, literal) expect_bytes((f), (literal), sizeof(literal) - 1)

static void
destroy(fixture *f)
{
    int borrowed = f->file != NULL ? fileno(f->file) : f->slave;
    tigt_presenter_destroy(f->presenter);
    CHECK(fcntl(borrowed, F_GETFD) >= 0);
    if (f->file != NULL)
        CHECK(fclose(f->file) == 0);
    else {
        CHECK(close(f->slave) == 0);
        CHECK(close(f->master) == 0);
    }
}

static void
notify(fixture *f, uint64_t id, unsigned row, unsigned column, const char *value,
       const tigt_presenter_boundary *boundaries, size_t boundary_count)
{
    uint32_t decoded[TIGT_PRESENTER_NOTIFICATION_TEXT_MAX];
    size_t length = strlen(value);
    CHECK(length <= TIGT_PRESENTER_NOTIFICATION_TEXT_MAX);
    for (size_t i = 0; i < length; i++)
        decoded[i] = (unsigned char) value[i];
    tigt_presenter_notification notification = {
        .operation_id = id, .text = decoded, .text_length = length,
        .boundaries = boundaries, .boundary_count = boundary_count,
        .columns = f->frame.columns, .rows = f->frame.rows,
        .start_column = column, .start_row = row
    };
    CHECK(tigt_presenter_notify(f->presenter, &notification) == TIGT_OK);
}

static void
notification_stats(fixture *f, uint64_t consumed, uint64_t discarded, uint64_t expired, uint32_t queued)
{
    tigt_presenter_notification_stats stats;
    CHECK(tigt_presenter_get_notification_stats(f->presenter, &stats) == TIGT_OK);
    CHECK(stats.consumed == consumed);
    CHECK(stats.discarded == discarded);
    CHECK(stats.expired == expired);
    CHECK(stats.queued == queued);
}

static void
editing(void)
{
    fixture f;
    create_file(&f, 20, 3, 60, TIGT_ENCODING_ASCII);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    text(&f, 0, 0, "ABC");
    position(&f, 0, 3);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "ABC");
    position(&f, 0, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\b");
    text(&f, 0, 2, "D");
    position(&f, 0, 3);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "D");
    text(&f, 0, 2, "E");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\bE");
    text(&f, 0, 2, " ");
    position(&f, 0, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\b \b");
    text(&f, 0, 0, "  ");
    position(&f, 0, 1); /* Not a screen-clear/home operation. */
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\b \b\b \b ");
    CHECK(tigt_presenter_reset(f.presenter) == TIGT_OK);
    blank_screen(&f);
    text(&f, 0, 0, "100");
    position(&f, 0, 3);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "100");
    position(&f, 0, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\r"); /* Cursor-only CR must not blank replacement text. */
    text(&f, 0, 0, "2");
    position(&f, 0, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "2");
    position(&f, 0, 3);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "00"); /* Cursor advancement beneath live rewrite digits. */
    text(&f, 0, 0, "3");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\r300");
    CHECK(tigt_presenter_reset(f.presenter) == TIGT_OK);
    blank_screen(&f);
    text(&f, 0, 0, "A");
    position(&f, 0, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A");
    text(&f, 0, 0, "B");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\bB"); /* Replacement at column zero is still a BS edit. */
    position(&f, 0, 8);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\t"); /* Forward motion confirms blank spacing, not a deletion. */
    text(&f, 0, 1, "incoming");
    position(&f, 0, 9);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\b\b\b\b\b\b\bincoming");
    CHECK(tigt_presenter_reset(f.presenter) == TIGT_OK);
    blank_screen(&f);
    position(&f, 0, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    /* VRAM text can reach vsync before the BIOS updates the CRTC cursor.
     * An unchanged guest cursor is not a new request to move left. */
    text(&f, 0, 0, "S00");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "S00");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    position(&f, 0, 3);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    position(&f, 1, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\n");
    destroy(&f);
}

static void
clears_and_attributes(void)
{
    fixture f;
    create_file(&f, 20, 3, 60, TIGT_ENCODING_UTF8);
    text(&f, 0, 0, "ABC");
    position(&f, 0, 3);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "ABC");
    blank_screen(&f);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\r   \r");
    text(&f, 0, 0, "A");
    text(&f, 1, 0, "B");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A\nB");
    blank_screen(&f);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\f");
    text(&f, 0, 0, "UVRX");
    f.cells[0].flags = TIGT_TEXT_UNDERLINE;
    f.cells[1].flags = TIGT_PRESENT_BOLD;
    f.cells[2].flags = TIGT_PRESENT_REVERSE;
    f.cells[3].flags = TIGT_TEXT_UNDERLINE | TIGT_PRESENT_BOLD;
    position(&f, 0, 4);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "_\bUV\bVR\bR_\bX\bX");
    f.cells[0].flags |= TIGT_TEXT_CURSOR | (1u << 20);
    f.cells[0].foreground = 0x123456;
    f.cells[0].background = 0xabcdef;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, ""); /* No cursor bytes, colors or unrelated attributes. */
    f.cells[4].codepoint = 0x00e9;
    f.cells[5].codepoint = tigt_cp437_codepoint(0);
    f.cells[6].codepoint = tigt_cp437_codepoint(0xff);
    f.cells[7].codepoint = 'Z';
    position(&f, 0, 8);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\xc3\xa9  Z");
    destroy(&f);
}

static void
geometry_and_scrolling(void)
{
    fixture f;
    const unsigned widths[] = { 20, 40, 80, 132, 320 };
    for (unsigned i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        create_file(&f, widths[i], 3, 50, TIGT_ENCODING_ASCII);
        text(&f, 0, 0, "A");
        text(&f, 0, 8, "B");
        text(&f, 0, 16, "C");
        position(&f, 0, 17);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, "A\tB\tC");
        text(&f, 1, 0, "D");
        text(&f, 2, 0, "E");
        position(&f, 2, 1);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, "\nD\nE");
        blank_screen(&f);
        text(&f, 0, 0, "D");
        text(&f, 1, 0, "E");
        text(&f, 2, 0, "F");
        position(&f, 2, 1);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, "\nF");
        blank_screen(&f);
        text(&f, 0, 0, "F");
        text(&f, 1, 0, "G");
        text(&f, 2, 0, "H");
        position(&f, 2, 1);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, "\nG\nH");
        destroy(&f);
    }
    create_file(&f, 20, 3, 50, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "01234567890123456789");
    position(&f, 1, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "01234567890123456789\n"); /* Explicit guest boundary NL. */
    text(&f, 1, 0, "B");
    position(&f, 2, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "B\n");
    blank_screen(&f);
    text(&f, 0, 0, "B");
    position(&f, 2, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\n"); /* Scroll can retain an unchanged empty bottom row. */
    destroy(&f);
    create_file(&f, 20, 4, 50, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "H");
    text(&f, 1, 0, "A");
    text(&f, 2, 0, "B");
    text(&f, 3, 0, "C");
    position(&f, 3, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "H\nA\nB\nC");
    text(&f, 1, 0, "B");
    text(&f, 2, 0, "C");
    text(&f, 3, 0, "D");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\nD"); /* A scrolling block beneath a stationary heading. */
    destroy(&f);
}

static void
sequential_scroll_suffix(void)
{
    fixture f;
    create_file(&f, 80, 3, 50, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "old");
    text(&f, 1, 0, "listing");
    text(&f, 2, 0, "COMMAND  COM");
    position(&f, 2, 12);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "old\nlisting\nCOMMAND  COM");
    /* A fast guest finishes the current partial DIR row, then scrolls and
     * starts the next row between snapshots. No wrap/echo notification exists. */
    blank_screen(&f);
    text(&f, 0, 0, "listing");
    text(&f, 1, 0, "COMMAND  COM    17792  10-20-83  12:00p");
    text(&f, 2, 0, "ANSI");
    position(&f, 2, 4);
    text(&f, 1, 0, "X");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, ""); /* A changed emitted prefix cannot masquerade as a scroll. */
    text(&f, 1, 0, "C");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\t17792  10-20-83  12:00p\nANSI");
    destroy(&f);

    create_file(&f, 80, 3, 50, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "old");
    text(&f, 1, 0, "listing");
    text(&f, 2, 0, "COMMAND  COM    KEEP");
    position(&f, 2, 20);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "old\nlisting\nCOMMAND  COM\tKEEP");
    position(&f, 2, 12);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\b\b\b\b\b\b\b\b");
    blank_screen(&f);
    text(&f, 0, 0, "listing");
    text(&f, 1, 0, "COMMAND  COM    17792  10-20-83  12:00p");
    text(&f, 2, 0, "ANSI");
    position(&f, 2, 4);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, ""); /* Pre-existing nonblank suffix is not an append proof. */
    destroy(&f);

    create_file(&f, 20, 3, 50, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "old");
    text(&f, 1, 0, "listing");
    text(&f, 2, 0, "MORE 3");
    position(&f, 2, 6);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "old\nlisting\nMORE 3");
    blank_screen(&f);
    text(&f, 0, 0, "listing");
    text(&f, 1, 0, "MORE 384");
    text(&f, 2, 0, "BASIC 12");
    position(&f, 2, 7); /* The final glyph is painted before CRTC advancement. */
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "84\nBASIC 12\b");
    text(&f, 0, 0, "MORE 384");
    text(&f, 2, 8, "3");
    position(&f, 2, 0);
    for (unsigned i = 0; i < 8; i++)
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, "");
    blank_screen(&f);
    text(&f, 0, 0, "MORE 384");
    text(&f, 1, 0, "BASIC 123");
    text(&f, 2, 0, "NEXT");
    position(&f, 2, 4);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "23\nNEXT"); /* Replace at the lagged cursor, then append once. */
    destroy(&f);
}

static void
multiple_row_scrolls(void)
{
    fixture f;
    create_file(&f, 8, 9, 60, TIGT_ENCODING_ASCII);
    const char *rows[] = { "A", "B", "C", "D", "E", "F", "G", "H", "I",
                           "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T" };
    for (unsigned y = 0; y < 9; y++)
        text(&f, y, 0, rows[y]);
    position(&f, 8, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A\nB\nC\nD\nE\nF\nG\nH\nI");
    const unsigned shifts[] = { 2, 3, 6 };
    const char *suffixes[] = { "\nJ\nK", "\nL\nM\nN", "\nO\nP\nQ\nR\nS\nT" };
    unsigned origin = 0;
    for (unsigned step = 0; step < 3; step++) {
        origin += shifts[step];
        blank_screen(&f);
        for (unsigned y = 0; y < 9; y++)
            text(&f, y, 0, rows[origin + y]);
        position(&f, 8, 1);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        expect_bytes(&f, suffixes[step], strlen(suffixes[step]));
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, ""); /* One commit per snapshot, not one per shifted row. */
    }
    destroy(&f);
}

static void
progressive_scroll_copy(void)
{
    fixture f;
    create_file(&f, 8, 6, 60, TIGT_ENCODING_ASCII);
    const char *rows[] = { "alpha", "bravo", "charlie", "delta", "echo", "foxtrot" };
    for (unsigned y = 0; y < 6; y++)
        text(&f, y, 0, rows[y]);
    position(&f, 5, 7);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "alpha\nbravo\ncharlie\ndelta\necho\nfoxtrot");
    tigt_text_cell committed[48];
    memcpy(committed, f.cells, sizeof(committed));
    /* A two-row REP MOVSW has copied two rows and three cells of the next.
     * Repeated vsyncs do not prove that the remaining VRAM write completed. */
    memcpy(f.cells, committed + 16, 19 * sizeof(*f.cells));
    for (unsigned i = 0; i < 8; i++) {
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
        EXPECT(&f, "");
    }
    memcpy(f.cells, committed + 16, 32 * sizeof(*f.cells));
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, ""); /* Retained rows align, but the exposed rows are stale. */
    text(&f, 4, 0, "        ");
    text(&f, 5, 0, "   ");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, ""); /* The final erase is still only a prefix of a row. */
    text(&f, 4, 0, "golf    ");
    text(&f, 5, 0, "hotel   ");
    position(&f, 5, 5);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\ngolf\nhotel");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    text(&f, 5, 5, "!");
    position(&f, 5, 6);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "!"); /* Cursor and logical row mapping advance only on commit. */
    destroy(&f);
}

static void
windowed_scroll_copy(void)
{
    fixture f;
    create_file(&f, 8, 8, 60, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "HEADER");
    for (unsigned y = 2; y <= 6; y++) {
        const char value[] = { (char) ('A' + y - 2), 0 };
        text(&f, y, 0, value);
    }
    position(&f, 6, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "HEADER\n\nA\nB\nC\nD\nE");
    text(&f, 2, 0, "C");
    text(&f, 3, 0, "D");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, "");
    text(&f, 4, 0, "E");
    text(&f, 5, 0, "F");
    text(&f, 6, 0, "G");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\nF\nG"); /* Neither heading nor unchanged blank borders replay. */
    destroy(&f);
}

static void
geometry_from_blank_baseline(void)
{
    fixture f;
    create_file(&f, 80, 25, 60, TIGT_ENCODING_ASCII);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    const char wrapped[] = "0123456789012345678901234567890123456789tail";
    const tigt_presenter_boundary old_boundaries[] = {
        { 40, TIGT_BOUNDARY_NEWLINE }, { sizeof(wrapped) - 1, TIGT_BOUNDARY_NEWLINE }
    };
    notify(&f, 1, 0, 0, wrapped, old_boundaries, 2);
    f.frame.columns = f.frame.stride = 40;
    blank_screen(&f);
    const tigt_presenter_boundary new_boundaries[] = {
        { 40, TIGT_BOUNDARY_SOFT_WRAP }, { sizeof(wrapped) - 1, TIGT_BOUNDARY_NEWLINE }
    };
    notify(&f, 2, 0, 0, wrapped, new_boundaries, 2);
    text(&f, 0, 0, "0123456789012345678901234567890123456789");
    text(&f, 1, 0, "tail");
    position(&f, 2, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "0123456789012345678901234567890123456789tail\n");
    notification_stats(&f, 1, 1, 0, 0); /* Old geometry cannot override the new wrap. */
    destroy(&f);

    create_file(&f, 40, 1, 60, TIGT_ENCODING_ASCII);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    f.frame.columns = f.frame.stride = 80;
    f.frame.rows = 2;
    blank_screen(&f);
    text(&f, 1, 0, "W");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\nW"); /* New rows/columns compare against a blank baseline. */
    destroy(&f);

    /* Seed cells beyond a later narrow baseline. Matching a wider frame must
     * not use that stale storage to reject an as-yet-unpainted prediction. */
    create_file(&f, 132, 3, 60, TIGT_ENCODING_ASCII);
    text(&f, 0, 40, "X");
    text(&f, 1, 0, "X");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\t\t\t\t\tX\nX");
    f.frame.columns = f.frame.stride = 40;
    f.frame.rows = 1;
    blank_screen(&f);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\f");
    f.frame.columns = f.frame.stride = 132;
    f.frame.rows = 3;
    blank_screen(&f);
    char wide_message[94];
    memset(wide_message, 'B', 92);
    wide_message[92] = 'Z';
    wide_message[93] = '\0';
    const tigt_presenter_boundary wide_boundaries[] = {
        { 92, TIGT_BOUNDARY_SOFT_WRAP }, { 93, TIGT_BOUNDARY_NEWLINE }
    };
    notify(&f, 1, 0, 40, wide_message, wide_boundaries, 2);
    text(&f, 0, 0, "A");
    position(&f, 0, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A"); /* Predictions still cannot emit unobserved text. */
    notification_stats(&f, 0, 0, 0, 1);
    for (unsigned x = 40; x < 132; x++)
        f.cells[x].codepoint = 'B';
    text(&f, 1, 0, "Z");
    position(&f, 2, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    char wide_output[99];
    memset(wide_output, '\t', 5);
    memcpy(wide_output + 5, wide_message, 93);
    wide_output[98] = '\n';
    expect_bytes(&f, wide_output, sizeof(wide_output));
    notification_stats(&f, 1, 0, 0, 0);
    destroy(&f);

    create_file(&f, 80, 25, 60, TIGT_ENCODING_ASCII);
    text(&f, 2, 0, "X");
    position(&f, 2, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\n\nX");
    f.frame.columns = f.frame.stride = 40;
    blank_screen(&f);
    text(&f, 0, 0, "M0");
    position(&f, 1, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, ""); /* A nonblank old geometry still cannot be discarded. */
    destroy(&f);
}

static void
scroll_hold_deadline_and_disabled(void)
{
    fixture f;
    create_file(&f, 8, 6, 60, TIGT_ENCODING_ASCII);
    for (unsigned y = 0; y < 6; y++) {
        const char value[] = { (char) ('A' + y), 0 };
        text(&f, y, 0, value);
    }
    position(&f, 5, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A\nB\nC\nD\nE\nF");
    text(&f, 0, 0, "B");
    text(&f, 1, 0, "C");
    /* A stalled copy cannot freeze a real application forever. Progress
     * inside the bounded interval must not restart the deadline either. */
    for (unsigned i = 0; i < 20; i++) {
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
        EXPECT(&f, "");
    }
    text(&f, 2, 0, "D");
    unsigned pending = 0;
    int result;
    do {
        result = tigt_presenter_present(f.presenter, &f.frame);
        EXPECT(&f, "");
        pending++;
    } while (result == TIGT_PRESENTER_PENDING && pending <= 20);
    CHECK(result == TIGT_ERROR_UNREPRESENTABLE);
    destroy(&f);

    create_file(&f, 8, 4, 60, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "A");
    text(&f, 1, 0, "B");
    text(&f, 2, 0, "C");
    text(&f, 3, 0, "D");
    position(&f, 3, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A\nB\nC\nD");
    text(&f, 0, 0, "B");
    f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED | TIGT_PRESENT_VIDEO_MEMORY_CHANGED;
    for (unsigned i = 0; i < 20; i++) {
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
        EXPECT(&f, "");
        f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED;
    }
    text(&f, 1, 0, "C");
    f.frame.hints |= TIGT_PRESENT_VIDEO_MEMORY_CHANGED;
    pending = 0;
    do {
        result = tigt_presenter_present(f.presenter, &f.frame);
        if (result == TIGT_PRESENTER_PENDING)
            EXPECT(&f, "");
        pending++;
        f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED;
    } while (result == TIGT_PRESENTER_PENDING && pending <= 20);
    CHECK(result == TIGT_OK);
    EXPECT(&f, "\f"); /* A disabled copy may outlast 200ms, but not remain frozen. */
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, ""); /* No repeated blank or restarted scroll interval. */
    destroy(&f);

    create_file(&f, 8, 4, 60, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "A");
    text(&f, 1, 0, "B");
    text(&f, 2, 0, "C");
    text(&f, 3, 0, "D");
    position(&f, 3, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A\nB\nC\nD");
    text(&f, 0, 0, "B");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, "");
    blank_screen(&f);
    position(&f, 3, 1);
    f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED | TIGT_PRESENT_VIDEO_MEMORY_CHANGED;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\f"); /* Explicit hardware blank wins over an active scroll hold. */
    f.frame.hints = 0;
    blank_screen(&f);
    text(&f, 0, 0, "R");
    position(&f, 0, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "R");
    destroy(&f);
}

static void
scroll_hold_local_echo(void)
{
    fixture f;
    create_file(&f, 8, 4, 60, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "old");
    text(&f, 1, 0, "listing");
    text(&f, 2, 0, "last");
    text(&f, 3, 0, "A>");
    position(&f, 3, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "old\nlisting\nlast\nA>");
    const uint32_t echo[] = { 'D', 'I', 'R', '\n' };
    CHECK(write(fileno(f.file), "DIR\n", 4) == 4);
    CHECK(tigt_presenter_local_echo(f.presenter, echo, 4) == TIGT_OK);
    EXPECT(&f, "DIR\n");
    text(&f, 0, 0, "listing ");
    text(&f, 3, 2, "DIR");
    for (unsigned i = 0; i < 8; i++)
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, "");
    blank_screen(&f);
    text(&f, 0, 0, "listing");
    text(&f, 1, 0, "last");
    text(&f, 2, 0, "A>DIR");
    text(&f, 3, 0, "result");
    position(&f, 3, 6);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "result"); /* Retained prompt, cooked echo and LF each appear once. */
    destroy(&f);
}

static void
overlapping_and_ambiguous_scrolls(void)
{
    fixture f;
    create_file(&f, 8, 8, 60, TIGT_ENCODING_ASCII);
    for (unsigned y = 0; y < 8; y++) {
        const char value[] = { (char) ('A' + y), 0 };
        text(&f, y, 0, value);
    }
    position(&f, 7, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A\nB\nC\nD\nE\nF\nG\nH");
    /* One scroll completed between samples, then the next started. The
     * uncopied suffix is already shifted once, not the committed image. */
    const char *partial[] = { "C", "D", "D", "E", "F", "G", "H", "I" };
    for (unsigned y = 0; y < 8; y++)
        text(&f, y, 0, partial[y]);
    for (unsigned i = 0; i < 8; i++)
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, "");
    for (unsigned y = 2; y < 6; y++) {
        const char value[] = { (char) ('C' + y), 0 };
        text(&f, y, 0, value);
    }
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, ""); /* Both newly exposed rows still retain earlier contents. */
    text(&f, 6, 0, "I");
    text(&f, 7, 0, "J");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\nI\nJ");
    destroy(&f);

    create_file(&f, 8, 6, 60, TIGT_ENCODING_ASCII);
    for (unsigned y = 0; y < 6; y++)
        text(&f, y, 0, y % 2 ? "B" : "A");
    position(&f, 5, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A\nB\nA\nB\nA\nB");
    for (unsigned y = 0; y < 5; y++)
        text(&f, y, 0, y % 2 ? "A" : "B");
    text(&f, 5, 0, "C");
    for (unsigned i = 0; i < 8; i++)
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, ""); /* Repeated anchors cannot prove one versus three rows. */
    destroy(&f);
}

static void
scrolling_after_partial_line(void)
{
    fixture f;
    create_file(&f, 20, 3, 60, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "A");
    text(&f, 1, 0, "B");
    text(&f, 2, 0, "C");
    position(&f, 2, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A\nB\nC");
    /* Ordinary text finishes its current row, then CR/LF scrolls before vsync.
     * The retained prefix proves where to resume; no soft-wrap hint is needed. */
    blank_screen(&f);
    text(&f, 0, 0, "B");
    text(&f, 1, 0, "CDEF");
    text(&f, 2, 0, "G");
    position(&f, 2, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "DEF\nG");
    destroy(&f);

    create_file(&f, 20, 3, 60, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "A");
    text(&f, 1, 0, "B");
    text(&f, 2, 0, "C");
    position(&f, 2, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A\nB\nC");
    blank_screen(&f);
    text(&f, 0, 0, "B");
    text(&f, 1, 0, "XDEF"); /* A changed emitted prefix does not prove a scroll. */
    text(&f, 2, 0, "G");
    position(&f, 2, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, "");
    destroy(&f);
}

static void
confirmation_and_recovery(void)
{
    fixture f;
    for (unsigned hz = 50; hz <= 70; hz += 10) {
        create_file(&f, 20, 3, hz, TIGT_ENCODING_ASCII);
        text(&f, 0, 0, "A");
        text(&f, 1, 0, "B");
        position(&f, 1, 1);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, "A\nB");
        position(&f, 0, 1);
        for (unsigned i = 0; i < 20; i++)
            CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, ""); /* Cursor-up alone never starts confirmation. */
        text(&f, 0, 0, "X");
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
        EXPECT(&f, "");
        for (unsigned interval = 1; interval < hz / 10; interval++) {
            CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
            EXPECT(&f, "");
        }
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_ERROR_UNREPRESENTABLE);
        EXPECT(&f, "");
        text(&f, 0, 0, "A");
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_ERROR_UNREPRESENTABLE);
        CHECK(tigt_presenter_reset(f.presenter) == TIGT_OK);
        EXPECT(&f, "");
        blank_screen(&f);
        text(&f, 0, 0, "R");
        position(&f, 0, 1);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, "R");
        destroy(&f);

        create_file(&f, 20, 3, hz, TIGT_ENCODING_ASCII);
        text(&f, 0, 0, "A");
        text(&f, 1, 0, "B");
        position(&f, 1, 1);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, "A\nB");
        text(&f, 0, 0, "X");
        text(&f, 1, 1, "C");
        position(&f, 1, 2);
        for (unsigned i = 0; i < hz / 10; i++)
            CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
        EXPECT(&f, ""); /* Even the representable suffix must stay transactional. */
        text(&f, 0, 0, "A");
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, "C"); /* Recover against committed, not rejected, baseline. */
        text(&f, 0, 0, "Y");
        for (unsigned i = 0; i < hz / 10; i++)
            CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
        EXPECT(&f, ""); /* Recovery restarted the entire confirmation interval. */
        blank_screen(&f);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, "\f");
        destroy(&f);
    }
}

static void
video_disable_glass(void)
{
    fixture f;
    for (unsigned hz = 50; hz <= 60; hz += 10) {
        /* A disabled CRTC does not reset its logical cursor. This multiline
         * erase would otherwise enter the 100ms representability gate. */
        create_file(&f, 20, 3, hz, TIGT_ENCODING_ASCII);
        text(&f, 0, 0, "A");
        text(&f, 1, 0, "B");
        position(&f, 1, 1);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, "A\nB");
        blank_screen(&f);
        position(&f, 1, 1);
        f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED;
        for (unsigned i = 1; i < hz / 5; i++) {
            CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
            EXPECT(&f, "");
        }
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, "\f");
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, ""); /* Neither a new hold nor phantom cursor whitespace. */
        f.frame.hints = 0;
        text(&f, 0, 0, "R");
        position(&f, 0, 1);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, "R");
        blank_screen(&f);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, "\r \r"); /* Enabled CLS remains immediate. */
        destroy(&f);

        for (unsigned held = 0; held <= 1; held++) {
            create_file(&f, 20, 3, hz, TIGT_ENCODING_ASCII);
            text(&f, 0, 0, "A");
            text(&f, 1, 0, "B");
            position(&f, 1, 1);
            CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
            EXPECT(&f, "A\nB");
            blank_screen(&f);
            position(&f, 1, 1);
            f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED;
            if (held) {
                CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
                EXPECT(&f, "");
            }
            /* Raw memory changed; decoded disabled cells are identical. */
            f.frame.hints |= TIGT_PRESENT_VIDEO_MEMORY_CHANGED;
            CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
            EXPECT(&f, "\f");
            f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED;
            CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
            EXPECT(&f, "");
            f.frame.hints = 0;
            text(&f, 0, 0, "R");
            position(&f, 0, 1);
            CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
            EXPECT(&f, "R");
            blank_screen(&f);
            f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED;
            CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
            EXPECT(&f, ""); /* Reenable rearms, even after a memory bypass. */
            CHECK(tigt_presenter_reset(f.presenter) == TIGT_OK);
            for (unsigned i = 1; i < hz / 5; i++)
                CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
            CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
            EXPECT(&f, ""); /* Reset restarts the interval and empty baseline. */
            destroy(&f);
        }
    }

    create_file(&f, 20, 3, 60, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "A");
    position(&f, 0, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A");
    blank_screen(&f);
    f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED;
    for (unsigned i = 0; i < 6; i++)
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    f.frame.refresh_hz = 50;
    for (unsigned i = 0; i < 4; i++)
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, ""); /* 6/60 + 4/50 seconds = 180ms. */
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\r \r");
    destroy(&f);
}

static void
video_disable_echo_and_controls(void)
{
    fixture f;
    create_file(&f, 20, 3, 60, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "A>");
    position(&f, 0, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A>");
    CHECK(write(fileno(f.file), "X\n", 2) == 2);
    const uint32_t echo[] = { 'X', '\n' };
    CHECK(tigt_presenter_local_echo(f.presenter, echo, 2) == TIGT_OK);
    EXPECT(&f, "X\n");
    blank_screen(&f);
    position(&f, 2, 5);
    f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED;
    for (unsigned i = 0; i < 11; i++)
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, "");
    f.frame.hints = 0;
    blank_screen(&f);
    text(&f, 0, 0, "A>X");
    text(&f, 1, 0, "R");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "R"); /* Held cursor/image/echo must not consume or replay X/LF. */
    blank_screen(&f);
    f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED;
    for (unsigned i = 0; i < 11; i++)
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, ""); /* Reenable cancelled the first nearly-expired interval. */
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\f");
    destroy(&f);

    create_file(&f, 20, 3, 60, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "A");
    text(&f, 1, 0, "B");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A\nB");
    blank_screen(&f);
    position(&f, 1, 1);
    f.frame.hints = TIGT_PRESENT_VIDEO_MEMORY_CHANGED;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, ""); /* Enabled blank at non-home cursor is not a hardware clear. */
    f.frame.hints |= TIGT_PRESENT_VIDEO_DISABLED;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\f"); /* Disabled clear cancels an already-pending failure. */
    f.frame.hints = TIGT_PRESENT_VIDEO_MEMORY_CHANGED;
    text(&f, 0, 0, "A");
    text(&f, 1, 0, "B");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A\nB");
    text(&f, 0, 0, "X");
    for (unsigned i = 0; i < 6; i++)
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_ERROR_UNREPRESENTABLE);
    blank_screen(&f);
    f.frame.hints |= TIGT_PRESENT_VIDEO_DISABLED;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_ERROR_UNREPRESENTABLE);
    EXPECT(&f, ""); /* Hints cannot bypass a sticky failure. */
    destroy(&f);
}

static void
fallback_at(fixture *f, unsigned column, unsigned row)
{
    text(f, 0, 0, "A");
    text(f, 1, 0, "B");
    position(f, 1, 1);
    CHECK(tigt_presenter_present(f->presenter, &f->frame) == TIGT_OK);
    EXPECT(f, "A\nB");
    text(f, 0, 0, "X");
    for (unsigned i = 0; i < 5; i++) {
        CHECK(tigt_presenter_present(f->presenter, &f->frame) == TIGT_PRESENTER_PENDING);
        EXPECT(f, "");
    }
    CHECK(tigt_presenter_present(f->presenter, &f->frame) == TIGT_PRESENTER_NEEDS_CURSOR);
    EXPECT(f, "");
    CHECK(tigt_presenter_observe_cursor(f->presenter, column, row) == TIGT_OK);
    submit_output(f, TIGT_PRESENTER_FULLSCREEN);
}

static void
fallback(fixture *f)
{
    struct winsize size;
    CHECK(ioctl(f->slave, TIOCGWINSZ, &size) == 0);
    fallback_at(f, 2, size.ws_row);
}

static void
video_disable_fullscreen(void)
{
    fixture f;
    for (unsigned hz = 50; hz <= 60; hz += 10) {
        create_pty(&f, 4, 2, 10, 6, 1);
        fallback(&f);
        EXPECT(&f, "\033[6;1H\n\n\033[5;1H\033[2K\033[6;1H\033[2K"
                   "\033[5;1H" SGR "X   \033[6;1H" SGR "B   \033[0m\033[6;2H");
        f.frame.refresh_hz = hz;
        blank_screen(&f);
        position(&f, 1, 1);
        f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED;
        for (unsigned i = 1; i < hz / 5; i++) {
            CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
            EXPECT(&f, "");
        }
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
        EXPECT(&f, "\033[5;1H" BLACK_SGR "    \033[6;1H" BLACK_SGR "    \033[0m\033[6;2H");
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
        EXPECT(&f, "");
        f.frame.hints = 0;
        text(&f, 0, 0, "R");
        position(&f, 0, 1);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
        EXPECT(&f, "\033[5;1H" SGR "R   \033[6;1H" SGR "    \033[0m\033[5;2H");
        blank_screen(&f);
        f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED;
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
        EXPECT(&f, "");
        f.frame.hints = 0;
        text(&f, 0, 0, "Q");
        position(&f, 0, 1);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
        EXPECT(&f, "\033[5;1H" SGR "Q\033[0m\033[5;2H");
        blank_screen(&f);
        position(&f, 1, 1);
        f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED | TIGT_PRESENT_VIDEO_MEMORY_CHANGED;
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
        EXPECT(&f, "\033[5;1H" BLACK_SGR "    \033[6;1H" BLACK_SGR "    \033[0m\033[6;2H");
        f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED;
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
        EXPECT(&f, "");
        destroy(&f);
    }

    /* Disabled non-scroll raw text is never an image to paint. An unrelated
     * edit blanks immediately, without requesting or inventing a host origin. */
    create_pty(&f, 4, 2, 10, 6, 0);
    text(&f, 0, 0, "A");
    text(&f, 1, 0, "B");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A\nB");
    text(&f, 0, 0, "X");
    f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED | TIGT_PRESENT_VIDEO_MEMORY_CHANGED;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\f");
    f.frame.hints = TIGT_PRESENT_VIDEO_DISABLED;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    f.frame.hints = 0;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "X\nB");
    destroy(&f);
}

static void
settle_recovery(fixture *f)
{
    for (unsigned i = 1; i <= f->frame.refresh_hz / 10; i++) {
        submit_output(f, i == f->frame.refresh_hz / 10 ? TIGT_OK : TIGT_PRESENTER_FULLSCREEN);
        EXPECT(f, ""); /* Switching modes must not erase or replay the region. */
    }
}

static void
scroll_hold_fullscreen(void)
{
    fixture f;
    create_pty(&f, 4, 4, 100, 107, 1);
    fallback_at(&f, 7, 53);
    EXPECT(&f, "\033[54;1H\033[2K\033[55;1H\033[2K\033[56;1H\033[2K\033[57;1H\033[2K"
               "\033[54;1H" SGR "X   \033[55;1H" SGR "B   \033[56;1H" SGR "    "
               "\033[57;1H" SGR "    \033[0m\033[55;2H");
    text(&f, 0, 0, "A");
    text(&f, 2, 0, "C");
    text(&f, 3, 0, "D");
    position(&f, 3, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "\033[54;1H" SGR "A\033[56;1H" SGR "C\033[57;1H" SGR "D\033[0m\033[57;2H");
    position(&f, 0, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "\033[54;2H");
    text(&f, 1, 0, "C");
    for (unsigned i = 0; i < 8; i++) {
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
        EXPECT(&f, ""); /* The cursor need not be inside a copied window. */
    }
    text(&f, 2, 0, "D");
    text(&f, 3, 0, "E");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "\033[55;1H" SGR "C\033[56;1H" SGR "D\033[57;1H" SGR "E\033[0m\033[54;2H");
    position(&f, 3, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "\033[57;2H");
    text(&f, 0, 0, "Z");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "\033[54;1H" SGR "Z\033[0m\033[57;2H"); /* Restart recovery after an edit. */
    text(&f, 0, 0, "C");
    text(&f, 1, 0, "D");
    for (unsigned i = 0; i < 8; i++) {
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
        EXPECT(&f, ""); /* No torn delta paint and no cursor repositioning. */
    }
    text(&f, 2, 0, "E");
    text(&f, 3, 0, "F");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "\033[54;1H" SGR "C\033[55;1H" SGR "D\033[56;1H" SGR "E\033[57;1H" SGR "F\033[0m\033[57;2H");
    settle_recovery(&f);
    text(&f, 3, 1, "!");
    position(&f, 3, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "!"); /* Recovery keeps the committed cursor, not region top. */
    destroy(&f);
}

static void
adaptive(void)
{
    fixture f;
    for (unsigned reversible = 0; reversible <= 1; reversible++) {
        create_pty(&f, 4, 2, 10, 6, reversible);
        fallback(&f);
        EXPECT(&f, "\033[6;1H\n\n\033[5;1H\033[2K\033[6;1H\033[2K"
                   "\033[5;1H" SGR "X   \033[6;1H" SGR "B   \033[0m\033[6;2H");
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
        EXPECT(&f, "");
        blank_screen(&f);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
        EXPECT(&f, "\033[5;1H" SGR " \033[6;1H" SGR " \033[0m\033[5;1H");
        if (reversible) {
            position(&f, 1, 0);
            CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
            EXPECT(&f, "\033[6;1H"); /* Blank cursor motion keeps recovery armed. */
        }
        if (reversible)
            notify(&f, 1, 0, 0, "R", NULL, 0);
        text(&f, 0, 0, "R");
        position(&f, 0, 1);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
        EXPECT(&f, "\033[5;1H" SGR "R\033[0m\033[5;2H");
        if (reversible) {
            settle_recovery(&f);
            notification_stats(&f, 1, 0, 0, 0);
        }
        destroy(&f);
    }
    create_pty(&f, 4, 2, 10, 6, 0);
    fallback(&f);
    EXPECT(&f, "\033[6;1H\n\n\033[5;1H\033[2K\033[6;1H\033[2K"
               "\033[5;1H" SGR "X   \033[6;1H" SGR "B   \033[0m\033[6;2H");
    struct winsize small = { .ws_col = 2, .ws_row = 1 };
    CHECK(ioctl(f.slave, TIOCSWINSZ, &small) == 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "\033[1;1H\033[2K\033[1;1H" SGR "X \033[0m\033[1;2H");
    text(&f, 1, 0, "Z"); /* Below clipped guest area: no host writes. */
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "");
    destroy(&f);

    create_pty(&f, 4, 3, 2, 1, 1);
    fallback(&f);
    EXPECT(&f, "\033[1;1H\n\033[1;1H\033[2K\033[1;1H" SGR "X \033[0m\033[1;2H");
    blank_screen(&f);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "\033[1;1H" SGR " \033[0m\033[1;1H");
    text(&f, 0, 0, "R");
    position(&f, 0, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "\033[1;1H" SGR "R\033[0m\033[1;2H");
    settle_recovery(&f);
    destroy(&f);
}

static void
adaptive_origin(void)
{
    /* All positions use the same 107-row viewport. Only the bottom case
     * scrolls; a later cursor delta and reversible recovery keep the origin. */
    const unsigned rows[] = { 2, 53, 107 };
    fixture f;
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        create_pty(&f, 4, 25, 100, 107, 1);
        f.concurrent_output = 1;
        fallback_at(&f, i == 1 ? 7 : 1, rows[i]);
        unsigned top = i == 2 ? 83 : rows[i] + (i == 1);
        char expected[4096];
        int count = 0;
        if (i == 2) {
            count = snprintf(expected, sizeof(expected), "\033[107;1H");
            for (unsigned y = 0; y < 24; y++)
                expected[count++] = '\n';
        }
        for (unsigned y = 0; y < 25; y++)
            count += snprintf(expected + count, sizeof(expected) - (size_t) count,
                              "\033[%u;1H\033[2K", top + y);
        for (unsigned y = 0; y < 25; y++)
            count += snprintf(expected + count, sizeof(expected) - (size_t) count,
                "\033[%u;1H" SGR "%s", top + y, y == 0 ? "X   " : y == 1 ? "B   " : "    ");
        count += snprintf(expected + count, sizeof(expected) - (size_t) count,
                          "\033[0m\033[%u;2H", top + 1);
        expect_bytes(&f, expected, (size_t) count);
        if (i == 1) {
            struct winsize larger = { .ws_col = 100, .ws_row = 120 };
            CHECK(ioctl(f.slave, TIOCSWINSZ, &larger) == 0);
            submit_output(&f, TIGT_PRESENTER_FULLSCREEN);
            expect_bytes(&f, expected, (size_t) count); /* Grow without reanchoring. */
        }
        position(&f, 0, 2);
        submit_output(&f, TIGT_PRESENTER_FULLSCREEN);
        count = snprintf(expected, sizeof(expected), "\033[%u;3H", top);
        expect_bytes(&f, expected, (size_t) count);
        blank_screen(&f);
        submit_output(&f, TIGT_PRESENTER_FULLSCREEN);
        count = snprintf(expected, sizeof(expected),
            "\033[%u;1H" SGR " \033[%u;1H" SGR " \033[0m\033[%u;1H", top, top + 1, top);
        expect_bytes(&f, expected, (size_t) count);
        text(&f, 0, 0, "R");
        position(&f, 0, 1);
        submit_output(&f, TIGT_PRESENTER_FULLSCREEN);
        count = snprintf(expected, sizeof(expected),
                         "\033[%u;1H" SGR "R\033[0m\033[%u;2H", top, top);
        expect_bytes(&f, expected, (size_t) count);
        settle_recovery(&f);
        destroy(&f);
    }
}

static void
sequential_recovery(void)
{
    fixture f;
    create_pty(&f, 4, 4, 100, 107, 1);
    fallback_at(&f, 7, 53);
    EXPECT(&f, "\033[54;1H\033[2K\033[55;1H\033[2K\033[56;1H\033[2K\033[57;1H\033[2K"
               "\033[54;1H" SGR "X   \033[55;1H" SGR "B   \033[56;1H" SGR "    "
               "\033[57;1H" SGR "    \033[0m\033[55;2H");
    text(&f, 1, 1, "C");
    position(&f, 1, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "\033[55;2H" SGR "C\033[0m\033[55;3H");
    for (unsigned i = 0; i < 10; i++)
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, ""); /* Idle and same-line updates cannot arm recovery. */
    text(&f, 2, 0, "A>");
    position(&f, 2, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "\033[56;1H" SGR "A>\033[0m\033[56;3H");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    text(&f, 0, 0, "Y");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "\033[54;1H" SGR "Y\033[0m\033[56;3H");
    for (unsigned i = 0; i < 10; i++)
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, ""); /* A transient sequential suffix must not oscillate modes. */
    position(&f, 3, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "\033[57;1H");
    for (unsigned i = 0; i < 10; i++)
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, ""); /* A newline needs actual subsequent text. */
    text(&f, 3, 0, "A>");
    position(&f, 3, 1); /* The BIOS has not yet advanced past the final glyph. */
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "\033[57;1H" SGR "A>\033[0m\033[57;2H");
    for (unsigned i = 0; i < 10; i++)
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "");
    position(&f, 3, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_FULLSCREEN);
    EXPECT(&f, "\033[57;3H");
    settle_recovery(&f);
    /* The recovered cursor is the retained prompt, not a replay at region top.
     * A cooked command wraps and scrolls the guest while its echo stays local. */
    const uint32_t dir[] = { 'D', 'I', 'R', '\n' };
    CHECK(tigt_presenter_local_echo(f.presenter, dir, 4) == TIGT_OK);
    blank_screen(&f);
    text(&f, 0, 0, "A>");
    text(&f, 1, 0, "A>DI");
    text(&f, 2, 0, "R");
    position(&f, 3, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    text(&f, 3, 0, "DIR");
    position(&f, 3, 3);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "DIR");
    destroy(&f);
}

static void
local_echo(void)
{
    fixture f;
    const uint32_t ver[] = { 'V', 'E', 'R', '\n' };
    create_file(&f, 20, 4, 60, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "A>");
    position(&f, 0, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A>");
    /* The host already displayed the final edited input before guest keys
     * arrived. Confirming each letter, CR and LF must emit no second copy. */
    CHECK(write(fileno(f.file), "VER\n", 4) == 4);
    CHECK(tigt_presenter_local_echo(f.presenter, ver, 4) == TIGT_OK);
    EXPECT(&f, "VER\n");
    for (unsigned i = 0; i < 3; i++) {
        f.cells[2 + i].codepoint = ver[i];
        position(&f, 0, 3 + i);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, "");
    }
    position(&f, 0, 0); /* Separately observed DOS CR before LF. */
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    position(&f, 1, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    text(&f, 1, 0, "DOS");
    position(&f, 1, 3);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "DOS");
    notification_stats(&f, 0, 0, 0, 0);
    destroy(&f);

    create_file(&f, 5, 4, 60, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "A>");
    position(&f, 0, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A>");
    const uint32_t wrapped[] = { 'a', 'b', 'c', 'd', 'e', '\n' };
    CHECK(tigt_presenter_local_echo(f.presenter, wrapped, 6) == TIGT_OK);
    text(&f, 0, 2, "abc");
    text(&f, 1, 0, "de");
    position(&f, 1, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    position(&f, 2, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    text(&f, 2, 0, "A>");
    position(&f, 2, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A>");
    destroy(&f);

    create_file(&f, 8, 3, 60, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "old");
    text(&f, 1, 0, "line");
    text(&f, 2, 0, "A>");
    position(&f, 2, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "old\nline\nA>");
    CHECK(tigt_presenter_local_echo(f.presenter, ver, 4) == TIGT_OK);
    blank_screen(&f);
    text(&f, 0, 0, "line");
    text(&f, 1, 0, "A>VER");
    position(&f, 2, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    text(&f, 2, 0, "OK");
    position(&f, 2, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "OK");
    destroy(&f);

    create_file(&f, 8, 3, 60, TIGT_ENCODING_ASCII);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    CHECK(tigt_presenter_local_echo(f.presenter, ver, 4) == TIGT_OK);
    text(&f, 0, 0, "VX");
    position(&f, 0, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, "");
    text(&f, 0, 0, "VER");
    position(&f, 1, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, ""); /* Failed plans must not consume the matched V prefix. */
    text(&f, 1, 0, "OK");
    position(&f, 1, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "OK");
    destroy(&f);

    create_file(&f, 8, 3, 60, TIGT_ENCODING_ASCII);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    uint32_t full[TIGT_PRESENTER_LOCAL_ECHO_MAX];
    for (unsigned i = 0; i < TIGT_PRESENTER_LOCAL_ECHO_MAX; i++)
        full[i] = 'x';
    CHECK(tigt_presenter_local_echo(f.presenter, full, TIGT_PRESENTER_LOCAL_ECHO_MAX) == TIGT_OK);
    CHECK(tigt_presenter_local_echo(f.presenter, full, 1) == TIGT_ERROR_UNREPRESENTABLE);
    text(&f, 0, 0, "x");
    position(&f, 0, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
    EXPECT(&f, ""); /* Overflow cannot silently drop an already-displayed prefix. */
    CHECK(tigt_presenter_reset(f.presenter) == TIGT_OK);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "x");
    destroy(&f);
}

static void
prompt_space_echo(void)
{
    /* PC DOS 2.10 LINK trace: Object Modules prompt at row14,col23,
     * response ';' at col23, then a separate CR/LF and diagnostic. The blank
     * before input must be emitted before host echo is registered. */
    fixture f;
    create_file(&f, 80, 25, 60, TIGT_ENCODING_ASCII);
    text(&f, 14, 0, "Object Modules [.OBJ]: ");
    position(&f, 14, 23);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\n\n\n\n\n\n\n\n\n\n\n\n\n\nObject Modules [.OBJ]: ");
    CHECK(write(fileno(f.file), ";\n", 2) == 2);
    const uint32_t response[] = { ';', '\n' };
    CHECK(tigt_presenter_local_echo(f.presenter, response, 2) == TIGT_OK);
    EXPECT(&f, ";\n");
    text(&f, 14, 23, ";");
    position(&f, 14, 24);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    position(&f, 14, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    position(&f, 15, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    text(&f, 15, 0, "No object modules specified.");
    text(&f, 17, 0, "A>");
    position(&f, 17, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "No object modules specified.\n\nA>");
    destroy(&f);
}

static void
validation_and_io(void)
{
    fixture f;
    create_file(&f, 20, 3, 60, TIGT_ENCODING_ASCII);
    tigt_presenter_config adaptive_config = {
        TIGT_PRESENTER_ABI_VERSION, fileno(f.file), TIGT_PRESENT_ADAPTIVE, TIGT_ENCODING_ASCII, 0
    };
    tigt_presenter *other = (tigt_presenter *) 1;
    CHECK(tigt_presenter_create(&adaptive_config, &other) == TIGT_ERROR_TERMINAL);
    CHECK(other == NULL);
    f.frame.hints = 1u << 2;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_ERROR_ARGUMENT);
    f.frame.hints = 0;
    f.frame.refresh_hz = 59;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_ERROR_ARGUMENT);
    f.frame.refresh_hz = 60;
    f.frame.stride = 19;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_ERROR_ARGUMENT);
    f.frame.stride = 20;
    f.frame.cursor_column = 20;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_ERROR_ARGUMENT);
    f.frame.cursor_column = 0;
    f.frame.columns = 321;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_ERROR_ARGUMENT);
    f.frame.columns = 320;
    f.frame.stride = 320;
    f.frame.rows = 68; /* Under each axis limit, over the visible-cell limit. */
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_ERROR_ARGUMENT);
    f.frame.columns = f.frame.stride = 20;
    f.frame.rows = 3;
    const uint32_t bad[] = { 0, 0x1b, 0x7f, 0x85, 0xd800, 0x110000 };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        f.cells[0].codepoint = bad[i];
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_ERROR_ARGUMENT);
    }
    f.cells[0].codepoint = 'Q';
    f.cells[0].foreground = 0xffaaaaaa;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_ERROR_ARGUMENT);
    f.cells[0].foreground = 0xaaaaaa;
    position(&f, 0, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "Q"); /* Invalid submissions neither emit nor poison state. */
    CHECK(tigt_presenter_reset(f.presenter) == TIGT_OK);
    f.frame.stride = 21;
    blank_screen(&f);
    f.cells[20].codepoint = 0; /* Invalid padding is deliberately ignored. */
    text(&f, 1, 0, "S");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\nS");
    destroy(&f);

    int descriptors[2];
    CHECK(pipe(descriptors) == 0);
    tigt_presenter_config config = {
        TIGT_PRESENTER_ABI_VERSION, descriptors[1], TIGT_PRESENT_GLASS, TIGT_ENCODING_ASCII, 0
    };
    CHECK(tigt_presenter_create(&config, &other) == TIGT_OK);
    CHECK(close(descriptors[0]) == 0);
    const uint32_t observed = 'S';
    const tigt_presenter_notification notification = {
        .operation_id = 1, .text = &observed, .text_length = 1,
        .columns = f.frame.columns, .rows = f.frame.rows, .start_row = 1
    };
    CHECK(tigt_presenter_notify(other, &notification) == TIGT_OK);
    CHECK(tigt_presenter_present(other, &f.frame) == TIGT_ERROR_SYSTEM);
    CHECK(tigt_presenter_present(other, &f.frame) == TIGT_ERROR_SYSTEM);
    tigt_presenter_notification_stats stats;
    CHECK(tigt_presenter_get_notification_stats(other, &stats) == TIGT_OK);
    CHECK(stats.queued == 1 && stats.consumed == 0 && stats.discarded == 0);
    tigt_presenter_destroy(other);
    CHECK(fcntl(descriptors[1], F_GETFD) >= 0);
    CHECK(close(descriptors[1]) == 0);
    CHECK(tigt_presenter_reset(NULL) == TIGT_ERROR_ARGUMENT);
    CHECK(tigt_presenter_present(NULL, NULL) == TIGT_ERROR_ARGUMENT);
    tigt_presenter_destroy(NULL);
}

static char *
save_environment(const char *name)
{
    const char *value = getenv(name);
    return value == NULL ? NULL : strdup(value);
}

static void
restore_environment(const char *name, char *value)
{
    CHECK(value == NULL ? unsetenv(name) == 0 : setenv(name, value, 1) == 0);
    free(value);
}

static void
encoding(void)
{
    fixture f;
    char *all = save_environment("LC_ALL");
    char *ctype = save_environment("LC_CTYPE");
    char *lang = save_environment("LANG");
    char *locale_before = strdup(setlocale(LC_CTYPE, NULL));
    CHECK(locale_before != NULL);
    CHECK(setenv("LC_ALL", "C", 1) == 0);
    CHECK(setenv("LC_CTYPE", "en_US.UTF-8", 1) == 0);
    CHECK(setenv("LANG", "en_US.UTF-8", 1) == 0);
    create_file(&f, 20, 2, 60, TIGT_ENCODING_LOCALE);
    f.cells[0].codepoint = 0x00e9;
    position(&f, 0, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "?"); /* LC_ALL wins over LC_CTYPE and LANG. */
    CHECK(strcmp(setlocale(LC_CTYPE, NULL), locale_before) == 0);
    destroy(&f);
    create_file(&f, 20, 2, 60, TIGT_ENCODING_UTF8);
    f.cells[0].codepoint = 0x00e9;
    position(&f, 0, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\xc3\xa9"); /* Forced UTF-8 does not mutate global C locale. */
    CHECK(strcmp(setlocale(LC_CTYPE, NULL), locale_before) == 0);
    f.cells[1].codepoint = 0x0301;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_ERROR_ARGUMENT);
    f.cells[1].codepoint = 0x4e00;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_ERROR_ARGUMENT);
    EXPECT(&f, "");
    destroy(&f);
    restore_environment("LC_ALL", all);
    restore_environment("LC_CTYPE", ctype);
    restore_environment("LANG", lang);
    free(locale_before);
}

static void
notification_matching(void)
{
    fixture f;
    create_file(&f, 4, 4, 60, TIGT_ENCODING_ASCII);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    const tigt_presenter_boundary wrap = { 4, TIGT_BOUNDARY_SOFT_WRAP };
    notify(&f, 1, 0, 0, "ABCDEFG", &wrap, 1);
    EXPECT(&f, ""); /* Predictions cannot write output. */
    text(&f, 0, 0, "AB");
    position(&f, 0, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "AB");
    notification_stats(&f, 0, 0, 0, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    notification_stats(&f, 0, 0, 0, 1);
    text(&f, 0, 2, "CD");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "CD"); /* VRAM can precede the cursor update. */
    text(&f, 1, 0, "EFG");
    position(&f, 1, 3);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "EFG");
    notification_stats(&f, 1, 0, 0, 0);
    notify(&f, 1, 0, 0, "ABCDEFG", &wrap, 1);
    notification_stats(&f, 1, 0, 0, 0); /* Completed IDs remain deduplicated. */
    const tigt_presenter_boundary single_wrap = { 1, TIGT_BOUNDARY_SOFT_WRAP };
    notify(&f, 2, 1, 3, "H", &single_wrap, 1);
    notify(&f, 3, 2, 0, "IJ", NULL, 0);
    text(&f, 1, 3, "H");
    text(&f, 2, 0, "IJ");
    position(&f, 2, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "HIJ");
    notification_stats(&f, 3, 0, 0, 0);
    destroy(&f);

    create_file(&f, 4, 3, 60, TIGT_ENCODING_ASCII);
    uint32_t borrowed[] = { 'A', 'B', 'C', 'D' };
    tigt_presenter_boundary borrowed_wrap = wrap;
    tigt_presenter_notification notification = {
        .operation_id = 1, .text = borrowed, .text_length = 4,
        .boundaries = &borrowed_wrap, .boundary_count = 1,
        .columns = 4, .rows = 3
    };
    CHECK(tigt_presenter_notify(f.presenter, &notification) == TIGT_OK);
    borrowed[0] = 'X';
    borrowed_wrap.kind = TIGT_BOUNDARY_NEWLINE;
    text(&f, 0, 0, "ABCD");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "ABCD");
    notification_stats(&f, 0, 0, 0, 1);
    position(&f, 1, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, ""); /* Delayed terminal wrap uses copied payload and cursor evidence. */
    notification_stats(&f, 1, 0, 0, 0);
    position(&f, 2, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\n"); /* A consumed wrap cannot swallow a later explicit NL. */
    destroy(&f);

    create_file(&f, 4, 3, 60, TIGT_ENCODING_ASCII);
    notify(&f, 1, 0, 0, "ABCDE", &wrap, 1);
    text(&f, 0, 0, "ABCD");
    text(&f, 1, 0, "E");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "ABCDE");
    position(&f, 0, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\r");
    const tigt_presenter_boundary newline = { 0, TIGT_BOUNDARY_NEWLINE };
    notify(&f, 2, 0, 0, "", &newline, 1);
    position(&f, 1, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\n"); /* Explicit LF overrides an earlier joined physical boundary. */
    text(&f, 1, 0, "F");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "F");
    notification_stats(&f, 2, 0, 0, 0);
    destroy(&f);

    create_file(&f, 4, 3, 60, TIGT_ENCODING_ASCII);
    const uint32_t accented[] = { 'A', 'B', 'C', 0x00e9, 'Z' };
    const tigt_presenter_notification decoded = {
        .operation_id = 1, .text = accented, .text_length = 5,
        .boundaries = &wrap, .boundary_count = 1, .columns = 4, .rows = 3
    };
    CHECK(tigt_presenter_notify(f.presenter, &decoded) == TIGT_OK);
    text(&f, 0, 0, "ABC");
    f.cells[3].codepoint = 0x00e7;
    text(&f, 1, 0, "Z");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "ABC?\nZ"); /* Distinct guest scalars must not match via ASCII '?'. */
    notification_stats(&f, 0, 1, 0, 0);
    CHECK(tigt_presenter_reset(f.presenter) == TIGT_OK);
    CHECK(tigt_presenter_notify(f.presenter, &decoded) == TIGT_OK);
    f.cells[3].codepoint = 0x00e9;
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "ABC?Z"); /* The matching scalar still confirms before ASCII fallback. */
    notification_stats(&f, 1, 1, 0, 0);
    destroy(&f);

    for (unsigned split = 0; split <= 1; split++) {
        create_file(&f, 20, 3, 60, TIGT_ENCODING_ASCII);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        const char message[] = "Hello, wrapped glass-TTY!";
        const tigt_presenter_boundary message_boundaries[] = {
            { 20, TIGT_BOUNDARY_SOFT_WRAP }, { sizeof(message) - 1, TIGT_BOUNDARY_NEWLINE }
        };
        if (split) {
            notify(&f, 1, 0, 0, "Hello, wrapped glass", message_boundaries, 1);
            const tigt_presenter_boundary ending = { 5, TIGT_BOUNDARY_NEWLINE };
            notify(&f, 2, 1, 0, "-TTY!", &ending, 1);
        } else
            notify(&f, 1, 0, 0, message, message_boundaries, 2);
        for (unsigned count = 1; count < sizeof(message); count++) {
            f.cells[count - 1].codepoint = (unsigned char) message[count - 1];
            position(&f, count / 20, count % 20);
            CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        }
        position(&f, 2, 0);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, "Hello, wrapped glass-TTY!\n");
        notification_stats(&f, split + 1, 0, 0, 0);
        destroy(&f);
    }
}

static void
notification_false_and_resync(void)
{
    fixture f;
    const tigt_presenter_boundary wrap = { 4, TIGT_BOUNDARY_SOFT_WRAP };
    create_file(&f, 4, 3, 60, TIGT_ENCODING_ASCII);
    notify(&f, 1, 0, 0, "ABCDZ", &wrap, 1);
    text(&f, 0, 0, "ABCD");
    text(&f, 1, 0, "X");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "ABCD\nX");
    notification_stats(&f, 0, 1, 0, 0);
    CHECK(tigt_presenter_cancel(f.presenter, 1) == TIGT_OK);
    CHECK(tigt_presenter_cancel(f.presenter, 1) == TIGT_OK);
    notification_stats(&f, 0, 1, 0, 0);
    CHECK(tigt_presenter_reset(f.presenter) == TIGT_OK);
    blank_screen(&f);
    notify(&f, 1, 0, 0, "ABCDZ", &wrap, 1);
    text(&f, 0, 0, "ABCD");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "ABCD");
    CHECK(tigt_presenter_cancel(f.presenter, 1) == TIGT_OK);
    position(&f, 1, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\n"); /* An incomplete cancelled prediction cannot suppress NL. */
    destroy(&f);

    create_file(&f, 4, 3, 60, TIGT_ENCODING_ASCII);
    notify(&f, 1, 0, 0, "ABCDZ", &wrap, 1);
    text(&f, 0, 0, "ABCD");
    position(&f, 1, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "ABCD"); /* Actual cursor crossing confirms a nonterminal wrap too. */
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    text(&f, 1, 0, "X");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "X"); /* A false remainder cannot revoke the already observed crossing. */
    notification_stats(&f, 0, 1, 0, 0);
    position(&f, 2, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\n"); /* The failed remainder cannot suppress a later explicit newline. */
    destroy(&f);

    create_file(&f, 8, 3, 60, TIGT_ENCODING_ASCII);
    const tigt_presenter_boundary edge = { 8, TIGT_BOUNDARY_SOFT_WRAP };
    notify(&f, 1, 0, 0, "XXABCDEFZ", &edge, 1);
    text(&f, 0, 0, "YYABCDEF");
    text(&f, 1, 0, "Z");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "YYABCDEFZ"); /* Unique anchored suffix can confirm only its later wrap. */
    notification_stats(&f, 0, 1, 0, 0);
    destroy(&f);

    create_file(&f, 8, 3, 60, TIGT_ENCODING_ASCII);
    notify(&f, 1, 0, 0, "XXABCABCZ", &edge, 1);
    text(&f, 0, 0, "YYABCABC");
    text(&f, 1, 0, "Z");
    text(&f, 2, 0, "BCABC"); /* Every candidate three-cell suffix is ambiguous. */
    position(&f, 2, 5);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "YYABCABC\nZ\nBCABC");
    notification_stats(&f, 0, 1, 0, 0);
    destroy(&f);

    create_file(&f, 4, 3, 60, TIGT_ENCODING_ASCII);
    notify(&f, 1, 0, 0, "XX  Z", &wrap, 1);
    text(&f, 0, 0, "YY");
    text(&f, 1, 0, "Z");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "YY\nZ"); /* Lone spaces cannot anchor a lookahead recovery. */
    notification_stats(&f, 0, 1, 0, 0);
    destroy(&f);
}

static void
notification_logical_edits(void)
{
    fixture f;
    create_file(&f, 5, 4, 60, TIGT_ENCODING_ASCII);
    const tigt_presenter_boundary wrap = { 5, TIGT_BOUNDARY_SOFT_WRAP };
    notify(&f, 1, 0, 0, "ABCDEF", &wrap, 1);
    text(&f, 0, 0, "ABCDE");
    text(&f, 1, 0, "F");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "ABCDEF");
    text(&f, 1, 3, "G");
    position(&f, 1, 4);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\tG"); /* Guest col3 is logical col8, not physical tab stop8. */
    text(&f, 1, 0, "Z");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\b\b\b\bZ"); /* Guest col0 is not output CR after a wrap. */
    position(&f, 0, 4);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\b\b");
    text(&f, 0, 4, "X");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "XZ");
    text(&f, 1, 0, " ");
    position(&f, 1, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\b \b");
    position(&f, 2, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\n");
    destroy(&f);

    create_file(&f, 4, 3, 60, TIGT_ENCODING_ASCII);
    const tigt_presenter_boundary wraps[] = {
        { 4, TIGT_BOUNDARY_SOFT_WRAP }, { 8, TIGT_BOUNDARY_SOFT_WRAP }
    };
    notify(&f, 1, 0, 0, "ABCDEFGHIJ", wraps, 2);
    text(&f, 0, 0, "ABCD");
    text(&f, 1, 0, "EFGH");
    text(&f, 2, 0, "IJ");
    position(&f, 2, 2);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "ABCDEFGHIJ");
    const tigt_presenter_boundary bottom_wrap = { 2, TIGT_BOUNDARY_SOFT_WRAP };
    notify(&f, 2, 2, 2, "KLM", &bottom_wrap, 1);
    blank_screen(&f);
    text(&f, 0, 0, "EFGH");
    text(&f, 1, 0, "IJKL");
    text(&f, 2, 0, "M");
    position(&f, 2, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "KLM");
    notification_stats(&f, 2, 0, 0, 0);
    text(&f, 2, 0, "N");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\bN");
    position(&f, 1, 3);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\b\b");
    text(&f, 1, 3, "Z");
    position(&f, 2, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "ZN");
    destroy(&f);

    create_file(&f, 4, 4, 60, TIGT_ENCODING_ASCII);
    const tigt_presenter_boundary adjacent[] = {
        { 4, TIGT_BOUNDARY_SOFT_WRAP }, { 4, TIGT_BOUNDARY_NEWLINE }
    };
    notify(&f, 1, 0, 0, "ABCDE", adjacent, 2);
    text(&f, 0, 0, "ABCD");
    text(&f, 2, 0, "E");
    position(&f, 2, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "ABCD\nE");
    notification_stats(&f, 1, 0, 0, 0);
    destroy(&f);
}

static void
notification_lifetime_and_transactions(void)
{
    fixture f;
    const tigt_presenter_boundary wrap = { 4, TIGT_BOUNDARY_SOFT_WRAP };
    for (unsigned hz = 50; hz <= 70; hz += 10) {
        create_file(&f, 4, 3, hz, TIGT_ENCODING_ASCII);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        notify(&f, 1, 0, 0, "ABCDE", &wrap, 1);
        for (unsigned i = 1; i < hz * 2; i++)
            CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        notification_stats(&f, 0, 0, 0, 1);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        notification_stats(&f, 0, 0, 1, 0);
        EXPECT(&f, "");
        text(&f, 0, 0, "ABCD");
        text(&f, 1, 0, "E");
        position(&f, 1, 1);
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
        EXPECT(&f, "ABCD\nE");
        destroy(&f);
    }
    create_file(&f, 4, 3, 60, TIGT_ENCODING_ASCII);
    for (unsigned id = 1; id <= TIGT_PRESENTER_NOTIFICATION_CAPACITY; id++)
        notify(&f, id, 0, 0, "ABCDE", &wrap, 1);
    uint32_t value = 'Q';
    tigt_presenter_notification overflow = {
        .operation_id = 33, .text = &value, .text_length = 1, .columns = 4, .rows = 3
    };
    CHECK(tigt_presenter_notify(f.presenter, &overflow) == TIGT_PRESENTER_NOTIFY_DROPPED);
    notification_stats(&f, 0, 33, 0, 0);
    EXPECT(&f, "");
    text(&f, 0, 0, "ABCD");
    text(&f, 1, 0, "E");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "ABCD\nE");
    notify(&f, 34, 1, 1, "FGHZ", &(tigt_presenter_boundary) { 3, TIGT_BOUNDARY_SOFT_WRAP }, 1);
    blank_screen(&f);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\f");
    notification_stats(&f, 0, 34, 0, 0);
    notify(&f, 35, 0, 0, "ABCDE", &wrap, 1);
    f.frame.columns = f.frame.stride = 5;
    blank_screen(&f);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    notification_stats(&f, 0, 35, 0, 0);
    notify(&f, 36, 0, 0, "Q", NULL, 0);
    CHECK(tigt_presenter_reset(f.presenter) == TIGT_OK);
    notification_stats(&f, 0, 36, 0, 0);
    notify(&f, 36, 0, 0, "Q", NULL, 0); /* Reset also resets operation identity. */
    text(&f, 0, 0, "Q");
    position(&f, 0, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "Q");
    notification_stats(&f, 1, 36, 0, 0);
    destroy(&f);

    create_file(&f, 5, 3, 60, TIGT_ENCODING_ASCII);
    text(&f, 0, 0, "A");
    text(&f, 1, 0, "B");
    position(&f, 1, 1);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "A\nB");
    notify(&f, 1, 1, 1, "CDEFZ", &wrap, 1);
    text(&f, 0, 0, "X");
    text(&f, 1, 1, "CDEF");
    text(&f, 2, 0, "Z");
    position(&f, 2, 1);
    for (unsigned i = 0; i < 4; i++) {
        CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_PENDING);
        notification_stats(&f, 0, 0, 0, 1);
        EXPECT(&f, "");
    }
    text(&f, 0, 0, "A");
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "CDEFZ");
    notification_stats(&f, 1, 0, 0, 0);
    destroy(&f);
}

static size_t
fill_pipe(int fd)
{
    char padding[4096];
    memset(padding, '#', sizeof(padding));
    size_t filled = 0;
    for (size_t chunk = sizeof(padding); chunk != 0; chunk = chunk == 1 ? 0 : 1) {
        for (;;) {
            ssize_t count = write(fd, padding, chunk);
            if (count < 0) {
                CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
                break;
            }
            CHECK(count > 0);
            filled += (size_t) count;
        }
    }
    return filled;
}

static void
discard_padding(int fd, size_t count)
{
    char buffer[4096];
    while (count != 0) {
        size_t chunk = count < sizeof(buffer) ? count : sizeof(buffer);
        ssize_t received = read(fd, buffer, chunk);
        CHECK(received > 0);
        for (ssize_t i = 0; i < received; i++)
            CHECK(buffer[i] == '#');
        count -= (size_t) received;
    }
}

static size_t
drain_pipe(int fd, char *bytes, size_t capacity)
{
    size_t used = 0;
    for (;;) {
        CHECK(used < capacity);
        ssize_t count = read(fd, bytes + used, capacity - used);
        if (count < 0) {
            CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
            return used;
        }
        CHECK(count > 0);
        used += (size_t) count;
    }
}

static void
nonblocking_transactions(void)
{
    fixture f;
    create_file(&f, 320, 67, 60, TIGT_ENCODING_ASCII);
    for (unsigned i = 0; i < 21440; i++) {
        f.cells[i].codepoint = 'A' + i % 26;
        f.cells[i].flags = TIGT_PRESENT_BOLD | TIGT_TEXT_UNDERLINE;
    }
    position(&f, 66, 319);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    struct stat status;
    CHECK(fstat(fileno(f.file), &status) == 0);
    size_t length = (size_t) status.st_size;
    CHECK(length > 4096); /* Force a partial transaction with one page available. */
    char *expected = malloc(length), *actual = malloc(length + 1);
    CHECK(expected != NULL && actual != NULL);
    CHECK(pread(fileno(f.file), expected, length, 0) == (ssize_t) length);
    tigt_presenter_destroy(f.presenter);
    CHECK(fclose(f.file) == 0);
    f.file = NULL;
    int descriptors[2];
    CHECK(pipe(descriptors) == 0);
    f.master = descriptors[0];
    f.slave = descriptors[1];
    tigt_presenter_config config = {
        TIGT_PRESENTER_ABI_VERSION, f.slave, TIGT_PRESENT_GLASS, TIGT_ENCODING_ASCII, 0
    };
    CHECK(tigt_presenter_create(&config, &f.presenter) == TIGT_OK);
    CHECK(tigt_presenter_present_nonblocking(f.presenter, &f.frame) == TIGT_ERROR_ARGUMENT);
    CHECK(fcntl(f.master, F_SETFL, fcntl(f.master, F_GETFL) | O_NONBLOCK) == 0);
    CHECK(fcntl(f.slave, F_SETFL, fcntl(f.slave, F_GETFL) | O_NONBLOCK) == 0);
    notify(&f, 1, 0, 0, "A", NULL, 0);
    size_t padding = fill_pipe(f.slave);
    CHECK(padding >= 4096);
    CHECK(tigt_presenter_present_nonblocking(f.presenter, &f.frame) == TIGT_PRESENTER_WOULD_BLOCK);
    for (unsigned i = 0; i < 200; i++)
        CHECK(tigt_presenter_resume(f.presenter) == TIGT_PRESENTER_WOULD_BLOCK);
    notification_stats(&f, 0, 0, 0, 1); /* Resumes are not additional guest vsyncs. */
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_ERROR_BUSY);
    CHECK(tigt_presenter_present_nonblocking(f.presenter, &f.frame) == TIGT_ERROR_BUSY);
    const uint32_t observed = 'B';
    const tigt_presenter_notification observation = {
        .operation_id = 2, .text = &observed, .text_length = 1,
        .columns = 320, .rows = 67
    };
    CHECK(tigt_presenter_notify(f.presenter, &observation) == TIGT_ERROR_BUSY);
    CHECK(tigt_presenter_cancel(f.presenter, 1) == TIGT_ERROR_BUSY);
    CHECK(tigt_presenter_observe_cursor(f.presenter, 1, 1) == TIGT_ERROR_BUSY);
    CHECK(tigt_presenter_forget_cursor(f.presenter) == TIGT_ERROR_BUSY);
    CHECK(tigt_presenter_local_echo(f.presenter, &observed, 1) == TIGT_ERROR_BUSY);
    discard_padding(f.master, 4096);
    CHECK(tigt_presenter_resume(f.presenter) == TIGT_PRESENTER_WOULD_BLOCK);
    f.cells[0].codepoint = 'Z'; /* The pending frame must own its original cells. */
    discard_padding(f.master, padding - 4096);
    size_t used = drain_pipe(f.master, actual, length + 1);
    CHECK(used > 0 && used < length);
    notification_stats(&f, 0, 0, 0, 1); /* Partial output has not committed matches. */
    int result = TIGT_PRESENTER_WOULD_BLOCK;
    for (unsigned attempts = 0; result == TIGT_PRESENTER_WOULD_BLOCK; attempts++) {
        CHECK(attempts < length);
        result = tigt_presenter_resume(f.presenter);
        used += drain_pipe(f.master, actual + used, length + 1 - used);
    }
    CHECK(result == TIGT_OK && used == length && memcmp(actual, expected, length) == 0);
    notification_stats(&f, 1, 0, 0, 0);
    f.cells[0].codepoint = 'A';
    CHECK(tigt_presenter_present_nonblocking(f.presenter, &f.frame) == TIGT_OK);
    CHECK(drain_pipe(f.master, actual, length + 1) == 0);
    CHECK(tigt_presenter_resume(f.presenter) == TIGT_ERROR_BUSY);
    notification_stats(&f, 1, 0, 0, 0);

    /* Reset cancels a partially emitted transaction immediately and does not
     * accidentally commit its notification or leak its tail into later output. */
    CHECK(tigt_presenter_reset(f.presenter) == TIGT_OK);
    notify(&f, 3, 0, 0, "A", NULL, 0);
    padding = fill_pipe(f.slave);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_PRESENTER_WOULD_BLOCK);
    discard_padding(f.master, 4096);
    CHECK(tigt_presenter_resume(f.presenter) == TIGT_PRESENTER_WOULD_BLOCK);
    CHECK(tigt_presenter_reset(f.presenter) == TIGT_OK);
    CHECK(tigt_presenter_resume(f.presenter) == TIGT_ERROR_BUSY);
    notification_stats(&f, 1, 1, 0, 0);
    discard_padding(f.master, padding - 4096);
    used = drain_pipe(f.master, actual, length + 1);
    CHECK(used > 0 && used < length && memcmp(actual, expected, used) == 0);
    blank_screen(&f);
    text(&f, 0, 0, "Q");
    position(&f, 0, 1);
    CHECK(tigt_presenter_present_nonblocking(f.presenter, &f.frame) == TIGT_OK);
    CHECK(drain_pipe(f.master, actual, length + 1) == 1 && actual[0] == 'Q');
    destroy(&f);
    free(expected);
    free(actual);
}

int
main(void)
{
    editing();
    clears_and_attributes();
    scrolling_after_partial_line();
    geometry_and_scrolling();
    sequential_scroll_suffix();
    multiple_row_scrolls();
    progressive_scroll_copy();
    windowed_scroll_copy();
    scroll_hold_deadline_and_disabled();
    scroll_hold_local_echo();
    overlapping_and_ambiguous_scrolls();
    geometry_from_blank_baseline();
    confirmation_and_recovery();
    video_disable_glass();
    video_disable_echo_and_controls();
    video_disable_fullscreen();
    scroll_hold_fullscreen();
    adaptive();
    adaptive_origin();
    local_echo();
    prompt_space_echo();
    sequential_recovery();
    validation_and_io();
    encoding();
    notification_matching();
    notification_false_and_resync();
    notification_logical_edits();
    notification_lifetime_and_transactions();
    nonblocking_transactions();
    return 0;
}
