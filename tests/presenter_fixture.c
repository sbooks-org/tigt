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

typedef struct {
    FILE *file;
    int master, slave;
    off_t offset;
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
    EXPECT(&f, "\b \b\b \b");
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
    EXPECT(&f, ""); /* Blank cursor motion cannot blank incoming text. */
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
geometry_from_blank_baseline(void)
{
    fixture f;
    create_file(&f, 80, 25, 60, TIGT_ENCODING_ASCII);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "");
    f.frame.columns = f.frame.stride = 40;
    blank_screen(&f);
    text(&f, 0, 0, "M0");
    position(&f, 1, 0);
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "M0\n"); /* No invented clear between observed blank and new text. */
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
fallback(fixture *f)
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
    CHECK(tigt_presenter_present(f->presenter, &f->frame) == TIGT_PRESENTER_FULLSCREEN);
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
        CHECK(tigt_presenter_present(f.presenter, &f.frame) ==
              (reversible ? TIGT_OK : TIGT_PRESENTER_FULLSCREEN));
        if (reversible)
            EXPECT(&f, "\033[5;1H\033[2K\033[6;1H\033[2K\033[6;1H\n\nR");
        else
            EXPECT(&f, "\033[5;1H" SGR "R\033[0m\033[5;2H");
        if (reversible)
            notification_stats(&f, 1, 0, 0, 0); /* Recovery plans twice, consumes once. */
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
    CHECK(tigt_presenter_present(f.presenter, &f.frame) == TIGT_OK);
    EXPECT(&f, "\033[1;1H\033[2K\033[1;1H\n\n\nR"); /* Guest rows, not clipped rows. */
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
    f.frame.hints = 1;
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
    geometry_from_blank_baseline();
    confirmation_and_recovery();
    adaptive();
    validation_and_io();
    encoding();
    notification_matching();
    notification_false_and_resync();
    notification_logical_edits();
    notification_lifetime_and_transactions();
    nonblocking_transactions();
    return 0;
}
