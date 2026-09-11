/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 * Build from the repository:
 *   cmake -S . -B build && cmake --build build --target tigt-snapshot
 * Run in a terminal: ./build/tigt-snapshot /tmp/tigt-snapshot.json 30
 * The screen gives the PID for `kill -USR1 PID` in another terminal.
 * Attributes need no font. Text PNG requires tigt_snapshot_set_font first.
 */
#include "tigt.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void put_line(tigt_text_cell *cells, const char *line)
{
    for (unsigned i = 0; i < 40; ++i) {
        cells[i].codepoint = *line ? (unsigned char)*line++ : ' ';
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tigt-snapshot.json";
    unsigned long seconds = 30;
    if (argc > 2) {
        char *end;
        errno = 0;
        seconds = strtoul(argv[2], &end, 10);
        if (errno || !*argv[2] || *end || argv[2][0] == '-') {
            fprintf(stderr, "duration must be a nonnegative integer\n");
            return 2;
        }
    }
    tigt_config config = {
        .abi_version = TIGT_ABI_VERSION, .graphics_mode = TIGT_GRAPHICS_AUTO
    };
    int result = tigt_init(&config);
    if (result != TIGT_OK) {
        fprintf(stderr, "tigt_init: %d\n", result);
        return 1;
    }
    tigt_text_cell cells[120];
    for (unsigned i = 0; i < 120; ++i) cells[i] = (tigt_text_cell){' ', 0xffffff, 0x102030, 0};
    put_line(cells, "tigt native snapshot instrumentation");
    for (unsigned i = 0; i < 40; ++i) cells[i].flags = TIGT_TEXT_UNDERLINE;
    char line[80];
    snprintf(line, sizeof(line), "kill -USR1 %ld", (long)getpid());
    put_line(cells + 40, line);
    tigt_overscan overscan = {0x203040, 8, 8, 4, 4};
    result = tigt_set_overscan(&overscan);
    if (result != TIGT_OK) goto finished;
    result = tigt_present_text(cells, 40, 3, 40);
    if (result != TIGT_OK) goto finished;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { result = TIGT_ERROR_SYSTEM; goto finished; }
    result = tigt_snapshot_write_fd(fd, TIGT_SNAPSHOT_ATTRIBUTES);
    if (close(fd) < 0 && result == TIGT_OK) result = TIGT_ERROR_SYSTEM;
    if (result != TIGT_OK) goto finished;
    result = tigt_snapshot_configure(SIGUSR1, TIGT_SNAPSHOT_ATTRIBUTES, path);
    if (result != TIGT_OK) goto finished;
    struct timespec start, now, pause = {0, 50000000};
    if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) { result = TIGT_ERROR_SYSTEM; goto finished; }
    uint64_t completed = 0;
    for (;;) {
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) { result = TIGT_ERROR_SYSTEM; break; }
        unsigned long elapsed = (unsigned long)(now.tv_sec - start.tv_sec);
        if (elapsed >= seconds) break;
        snprintf(line, sizeof(line), "Elapsed: %lu seconds", elapsed);
        put_line(cells + 80, line);
        result = tigt_present_text(cells, 40, 3, 40);
        if (result != TIGT_OK) break;
        int snapshot_result;
        uint64_t sequence;
        result = tigt_snapshot_status(&sequence, &snapshot_result);
        if (result != TIGT_OK) break;
        if (sequence != completed) {
            result = snapshot_result;
            if (result != TIGT_OK) break;
            completed = sequence;
        }
        nanosleep(&pause, NULL);
    }
finished:
    tigt_shutdown();
    if (result != TIGT_OK) {
        fprintf(stderr, "snapshot failed: %d\n", result);
        return 1;
    }
    uint64_t sequence;
    int snapshot_result;
    if (tigt_snapshot_status(&sequence, &snapshot_result) != TIGT_OK || snapshot_result != TIGT_OK) {
        fprintf(stderr, "last asynchronous snapshot failed\n");
        return 1;
    }
    printf("Snapshot: %s; %llu asynchronous requests completed\n", path, (unsigned long long)sequence);
    return 0;
}
