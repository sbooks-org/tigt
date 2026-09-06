/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "snapshot.h"

#include <errno.h>
#include <fcntl.h>
#include <png.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The handler touches only permanent, always-lock-free objects. In particular,
 * it never touches the configuration, a descriptor, or any allocated storage. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "snapshot signals need lock-free int atomics");
static atomic_int requested;
static atomic_int installed_signal;
static atomic_bool worker_running;
static pthread_mutex_t control_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool session_active;
static pthread_t worker_thread;
static bool worker_created;
static struct sigaction previous_action;
static char *configured_path;
static uint32_t configured_format;
static uint8_t snapshot_font[256 * 32];
static uint16_t snapshot_font_height;
static uint64_t completed_sequence;
static int completed_result;

/* control_mutex protects lifecycle/configuration. data_mutex protects the font
 * and completion status, and is never held while joining the worker. The worker
 * only reads configuration between creation and join, while it is immutable. */
typedef struct {
    int fd;
    int result;
    bool asynchronous;
    bool bounded;
    bool pipe_broken;
    struct timespec deadline;
    size_t used;
    uint8_t buffer[8192];
} snapshot_sink;

static void
snapshot_signal(int number)
{
    if (number == atomic_load_explicit(&installed_signal, memory_order_relaxed))
        atomic_store_explicit(&requested, 1, memory_order_relaxed);
}

static int
remaining_ms(const struct timespec *deadline)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    const int64_t nanoseconds = (int64_t) (deadline->tv_sec - now.tv_sec) * 1000000000 +
                                deadline->tv_nsec - now.tv_nsec;
    if (nanoseconds <= 0)
        return 0;
    return (int) ((nanoseconds + 999999) / 1000000);
}

static bool
sink_available(snapshot_sink *sink)
{
    if (sink->result != TIGT_OK)
        return false;
    if (sink->asynchronous && !atomic_load_explicit(&worker_running, memory_order_relaxed)) {
        errno = ECANCELED;
        sink->result = TIGT_ERROR_SYSTEM;
    } else if (sink->bounded && remaining_ms(&sink->deadline) == 0) {
        errno = ETIMEDOUT;
        sink->result = TIGT_ERROR_SYSTEM;
    }
    return sink->result == TIGT_OK;
}

static void
sink_flush(snapshot_sink *sink)
{
    size_t offset = 0;
    while (sink->result == TIGT_OK && offset < sink->used) {
        if (!sink_available(sink))
            break;
        const ssize_t written = write(sink->fd, sink->buffer + offset, sink->used - offset);
        if (written > 0) {
            offset += (size_t) written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && sink->bounded) {
            struct pollfd descriptor = { .fd = sink->fd, .events = POLLOUT };
            const int remaining = remaining_ms(&sink->deadline);
            const int ready = poll(&descriptor, 1, remaining > 20 ? 20 : remaining);
            if (ready < 0 && errno != EINTR)
                sink->result = TIGT_ERROR_SYSTEM;
            else if (ready > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                errno = (descriptor.revents & POLLNVAL) != 0 ? EBADF : EPIPE;
                sink->result = TIGT_ERROR_SYSTEM;
            }
        } else {
            if (written < 0 && errno == EPIPE)
                sink->pipe_broken = true;
            sink->result = TIGT_ERROR_SYSTEM;
        }
    }
    sink->used = 0;
}

static void
sink_bytes(snapshot_sink *sink, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    while (sink->result == TIGT_OK && length != 0) {
        size_t amount = sizeof(sink->buffer) - sink->used;
        if (amount > length)
            amount = length;
        memcpy(sink->buffer + sink->used, bytes, amount);
        sink->used += amount;
        bytes += amount;
        length -= amount;
        if (sink->used == sizeof(sink->buffer))
            sink_flush(sink);
    }
}

static void
sink_printf(snapshot_sink *sink, const char *format, ...)
{
    char text[512];
    va_list args;
    if (sink->result != TIGT_OK)
        return;
    va_start(args, format);
    const int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (length < 0 || (size_t) length >= sizeof(text)) {
        sink->result = TIGT_ERROR_SYSTEM;
        return;
    }
    sink_bytes(sink, text, (size_t) length);
}

static void
sink_codepoint(snapshot_sink *sink, uint32_t codepoint)
{
    uint8_t bytes[4];
    size_t length;
    if (codepoint < 0x80) {
        bytes[0] = (uint8_t) codepoint;
        length = 1;
    } else if (codepoint < 0x800) {
        bytes[0] = 0xc0 | (codepoint >> 6);
        bytes[1] = 0x80 | (codepoint & 0x3f);
        length = 2;
    } else if (codepoint < 0x10000) {
        bytes[0] = 0xe0 | (codepoint >> 12);
        bytes[1] = 0x80 | ((codepoint >> 6) & 0x3f);
        bytes[2] = 0x80 | (codepoint & 0x3f);
        length = 3;
    } else {
        bytes[0] = 0xf0 | (codepoint >> 18);
        bytes[1] = 0x80 | ((codepoint >> 12) & 0x3f);
        bytes[2] = 0x80 | ((codepoint >> 6) & 0x3f);
        bytes[3] = 0x80 | (codepoint & 0x3f);
        length = 4;
    }
    sink_bytes(sink, bytes, length);
}

static bool
cp437_glyph(uint32_t codepoint, uint8_t *glyph)
{
    /* Prefer the printable canonical space, not blank display slots 00/ff. */
    if (codepoint >= 0x20 && codepoint < 0x7f) {
        *glyph = (uint8_t) codepoint;
        return true;
    }
    for (unsigned value = 1; value < 256; value++) {
        if (value >= 0x20 && value < 0x7f)
            continue;
        if (tigt_cp437_codepoint((uint8_t) value) == codepoint) {
            *glyph = (uint8_t) value;
            return true;
        }
    }
    *glyph = '?';
    return false;
}

static uint8_t
ibm16_color(uint32_t color)
{
    static const uint32_t palette[16] = {
        0x000000, 0x0000aa, 0x00aa00, 0x00aaaa,
        0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
        0x555555, 0x5555ff, 0x55ff55, 0x55ffff,
        0xff5555, 0xff55ff, 0xffff55, 0xffffff
    };
    uint32_t best_distance = UINT32_MAX;
    uint8_t best = 0;
    for (uint8_t index = 0; index < 16; index++) {
        const int red = (int) ((color >> 16) & 255) - (int) ((palette[index] >> 16) & 255);
        const int green = (int) ((color >> 8) & 255) - (int) ((palette[index] >> 8) & 255);
        const int blue = (int) (color & 255) - (int) (palette[index] & 255);
        const uint32_t distance = red * red + green * green + blue * blue;
        if (distance < best_distance) {
            best_distance = distance;
            best = index;
        }
    }
    return best;
}

static uint8_t
legacy_attribute(const tigt_text_cell *cell)
{
    return ibm16_color(cell->foreground) | (ibm16_color(cell->background) << 4);
}

static void
encode_text(snapshot_sink *sink, const tigt_native_frame *frame, uint32_t format)
{
    for (uint16_t row = 0; row < frame->height && sink->result == TIGT_OK; row++) {
        const tigt_text_cell *cells = frame->content.cells + (size_t) row * frame->width;
        uint16_t end = frame->width;
        if (format != TIGT_SNAPSHOT_CELLS) {
            while (end != 0 && cells[end - 1].codepoint == 0x20)
                end--;
        }
        for (uint16_t column = 0; column < end; column++) {
            const tigt_text_cell *cell = &cells[column];
            if (format == TIGT_SNAPSHOT_CELLS) {
                uint8_t pair[2];
                cp437_glyph(cell->codepoint, &pair[0]);
                pair[1] = legacy_attribute(cell);
                sink_bytes(sink, pair, sizeof(pair));
            } else if (format == TIGT_SNAPSHOT_ASCII || format == TIGT_SNAPSHOT_CP437) {
                uint8_t glyph = '?';
                if (format == TIGT_SNAPSHOT_CP437)
                    cp437_glyph(cell->codepoint, &glyph);
                else if (cell->codepoint >= 0x20 && cell->codepoint < 0x7f)
                    glyph = (uint8_t) cell->codepoint;
                sink_bytes(sink, &glyph, 1);
            } else {
                if (format == TIGT_SNAPSHOT_ANSI &&
                    (column == 0 || cell->foreground != cells[column - 1].foreground ||
                     cell->background != cells[column - 1].background ||
                     (cell->flags != 0) != (cells[column - 1].flags != 0))) {
                    sink_printf(sink, "\033[0;38;2;%u;%u;%u;48;2;%u;%u;%u%sm",
                                (cell->foreground >> 16) & 255, (cell->foreground >> 8) & 255,
                                cell->foreground & 255, (cell->background >> 16) & 255,
                                (cell->background >> 8) & 255, cell->background & 255,
                                cell->flags != 0 ? ";4" : "");
                }
                sink_codepoint(sink, cell->codepoint);
            }
        }
        if (format == TIGT_SNAPSHOT_ANSI)
            sink_bytes(sink, "\033[0m", 4);
        if (format != TIGT_SNAPSHOT_CELLS)
            sink_bytes(sink, "\n", 1);
    }
}

static void
encode_attributes(snapshot_sink *sink, const tigt_native_frame *frame)
{
    const tigt_overscan *overscan = &frame->overscan;
    sink_printf(sink,
                "{\"schema_version\":1,\"kind\":\"%s\","
                "\"dimensions\":{\"width\":%u,\"height\":%u,\"pixel_width\":%u},"
                "\"overscan\":{\"color\":%u,\"left\":%u,\"right\":%u,\"top\":%u,\"bottom\":%u}",
                frame->bitmap ? "bitmap" : "text", frame->width / frame->pixel_width,
                frame->height, frame->pixel_width, overscan->color,
                overscan->left, overscan->right, overscan->top, overscan->bottom);
    if (!frame->bitmap) {
        sink_printf(sink, ",\"columns\":%u,\"rows\":%u,\"cells\":[", frame->width, frame->height);
        const size_t count = (size_t) frame->width * frame->height;
        for (size_t index = 0; index < count && sink->result == TIGT_OK; index++) {
            const tigt_text_cell *cell = &frame->content.cells[index];
            uint8_t glyph;
            cp437_glyph(cell->codepoint, &glyph);
            sink_printf(sink,
                        "%s{\"codepoint\":%u,\"foreground\":%u,\"background\":%u,\"flags\":%u,"
                        "\"legacy\":{\"glyph\":%u,\"attribute\":%u}}",
                        index == 0 ? "" : ",", cell->codepoint, cell->foreground,
                        cell->background, cell->flags, glyph, legacy_attribute(cell));
        }
        sink_bytes(sink, "]", 1);
    }
    sink_bytes(sink, "}\n", 2);
}

static void
png_sink_write(png_structp png, png_bytep bytes, png_size_t length)
{
    snapshot_sink *sink = png_get_io_ptr(png);
    sink_bytes(sink, bytes, length);
    if (sink->result != TIGT_OK)
        png_error(png, "snapshot write failed");
}

static void
png_sink_flush(png_structp png)
{
    snapshot_sink *sink = png_get_io_ptr(png);
    sink_flush(sink);
    if (sink->result != TIGT_OK)
        png_error(png, "snapshot flush failed");
}

static void
png_failure(png_structp png, png_const_charp message)
{
    (void) message;
    png_longjmp(png, 1);
}

static void
png_warning_ignore(png_structp png, png_const_charp message)
{
    (void) png;
    (void) message;
}

static int
encode_png(snapshot_sink *sink, const tigt_native_frame *frame,
           const uint8_t *font, uint16_t font_height)
{
    uint8_t glyphs[TIGT_MAX_TEXT_CELLS];
    if (!frame->bitmap) {
        if (font_height == 0)
            return TIGT_ERROR_ARGUMENT;
        const size_t count = (size_t) frame->width * frame->height;
        for (size_t index = 0; index < count; index++) {
            if (!cp437_glyph(frame->content.cells[index].codepoint, &glyphs[index]))
                return TIGT_ERROR_ARGUMENT;
        }
    }
    const uint32_t width = frame->bitmap ? frame->width / frame->pixel_width : frame->width * 8u;
    const uint32_t height = frame->bitmap ? frame->height : frame->height * (uint32_t) font_height;
    uint8_t *row = malloc((size_t) width * 3);
    if (row == NULL)
        return TIGT_ERROR_SYSTEM;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, png_failure, png_warning_ignore);
    if (png == NULL) {
        free(row);
        return TIGT_ERROR_SYSTEM;
    }
    png_infop info = png_create_info_struct(png);
    if (info == NULL) {
        png_destroy_write_struct(&png, NULL);
        free(row);
        return TIGT_ERROR_SYSTEM;
    }
    if (setjmp(png_jmpbuf(png))) {
        const int saved_errno = errno;
        png_destroy_write_struct(&png, &info);
        free(row);
        errno = saved_errno;
        return TIGT_ERROR_SYSTEM;
    }
    png_set_write_fn(png, sink, png_sink_write, png_sink_flush);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (uint32_t y = 0; y < height; y++) {
        if (!sink_available(sink))
            png_error(png, "snapshot cancelled or timed out");
        for (uint32_t x = 0; x < width; x++) {
            uint32_t color;
            if (frame->bitmap) {
                color = frame->content.pixels[(size_t) y * frame->width + x * frame->pixel_width];
            } else {
                const size_t index = (size_t) (y / font_height) * frame->width + x / 8;
                const tigt_text_cell *cell = &frame->content.cells[index];
                const unsigned line = y % font_height;
                const bool marked = (font[(size_t) glyphs[index] * font_height + line] & (0x80u >> (x % 8))) != 0 ||
                                    (cell->flags != 0 && line == (unsigned) font_height - 1);
                color = marked ? cell->foreground : cell->background;
            }
            row[x * 3] = (uint8_t) (color >> 16);
            row[x * 3 + 1] = (uint8_t) (color >> 8);
            row[x * 3 + 2] = (uint8_t) color;
        }
        png_write_row(png, row);
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    free(row);
    return sink->result;
}

static int
write_snapshot(int fd, uint32_t format, bool asynchronous, bool bounded)
{
    tigt_native_frame *frame = malloc(sizeof(*frame));
    if (frame == NULL)
        return TIGT_ERROR_SYSTEM;
    uint8_t font[256 * 32];
    uint16_t font_height = 0;
    /* Include the selected font in the immutable copy, so shutdown cannot clear
     * it between frame capture and rasterization. Renderer never takes data_mutex
     * while holding its own frame mutex. */
    pthread_mutex_lock(&data_mutex);
    int result = tigt_snapshot_capture(frame);
    if (result == TIGT_OK && !frame->bitmap && format == TIGT_SNAPSHOT_PNG) {
        font_height = snapshot_font_height;
        if (font_height != 0)
            memcpy(font, snapshot_font, 256u * font_height);
    }
    pthread_mutex_unlock(&data_mutex);
    if (result != TIGT_OK) {
        free(frame);
        return result;
    }
    if (frame->bitmap && format != TIGT_SNAPSHOT_PNG && format != TIGT_SNAPSHOT_ATTRIBUTES) {
        free(frame);
        return TIGT_ERROR_ARGUMENT;
    }
    snapshot_sink sink = { .fd = fd, .result = TIGT_OK, .asynchronous = asynchronous, .bounded = bounded };
    if (bounded) {
        if (clock_gettime(CLOCK_MONOTONIC, &sink.deadline) < 0) {
            free(frame);
            return TIGT_ERROR_SYSTEM;
        }
        sink.deadline.tv_nsec += 250000000;
        if (sink.deadline.tv_nsec >= 1000000000) {
            sink.deadline.tv_sec++;
            sink.deadline.tv_nsec -= 1000000000;
        }
    }
#if defined(__APPLE__)
    /* Darwin directs pipe SIGPIPE at the process, not the writing thread.
     * F_SETNOSIGPIPE is shared by dup'd descriptors (and by socket aliases), so
     * duplication cannot isolate it. The caller must exclusively own use of
     * this open file description during capture; restore its original setting
     * before returning. Never change the application's signal disposition. */
    const int no_sigpipe = fcntl(fd, F_GETNOSIGPIPE);
    if (no_sigpipe < 0 || (no_sigpipe == 0 && fcntl(fd, F_SETNOSIGPIPE, 1) < 0)) {
        const int saved_errno = errno;
        free(frame);
        errno = saved_errno;
        return TIGT_ERROR_SYSTEM;
    }
#else
    /* Linux directs pipe SIGPIPE at the writing thread. Preserve an
     * already-pending SIGPIPE, and consume only a newly generated one. */
    sigset_t pipe_set, old_mask, pending;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    const int mask_result = pthread_sigmask(SIG_BLOCK, &pipe_set, &old_mask);
    if (mask_result != 0) {
        free(frame);
        errno = mask_result;
        return TIGT_ERROR_SYSTEM;
    }
    if (sigpending(&pending) < 0) {
        const int saved_errno = errno;
        pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
        free(frame);
        errno = saved_errno;
        return TIGT_ERROR_SYSTEM;
    }
    const bool pipe_was_pending = sigismember(&pending, SIGPIPE) == 1;
#endif
    if (format == TIGT_SNAPSHOT_PNG)
        result = encode_png(&sink, frame, font, font_height);
    else if (format == TIGT_SNAPSHOT_ATTRIBUTES)
        encode_attributes(&sink, frame);
    else
        encode_text(&sink, frame, format);
    if (result == TIGT_OK) {
        sink_flush(&sink);
        result = sink.result;
    }
    int saved_errno = errno;
#if defined(__APPLE__)
    if (no_sigpipe == 0 && fcntl(fd, F_SETNOSIGPIPE, no_sigpipe) < 0 && result == TIGT_OK) {
        saved_errno = errno;
        result = TIGT_ERROR_SYSTEM;
    }
#else
    if (sink.pipe_broken && !pipe_was_pending &&
        sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
        int received;
        sigwait(&pipe_set, &received);
    }
    const int restore_result = pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    if (restore_result != 0 && result == TIGT_OK) {
        saved_errno = restore_result;
        result = TIGT_ERROR_SYSTEM;
    }
#endif
    free(frame);
    errno = saved_errno;
    return result;
}

int
tigt_snapshot_write_fd(int fd, uint32_t format)
{
    if (fd < 0 || format > TIGT_SNAPSHOT_ATTRIBUTES)
        return TIGT_ERROR_ARGUMENT;
    return write_snapshot(fd, format, false, false);
}

int
tigt_snapshot_set_font(const uint8_t *font, uint16_t height)
{
    if ((font == NULL && height != 0) || (font != NULL && (height == 0 || height > 32)))
        return TIGT_ERROR_ARGUMENT;
    pthread_mutex_lock(&control_mutex);
    if (!session_active) {
        pthread_mutex_unlock(&control_mutex);
        return TIGT_ERROR_BUSY;
    }
    pthread_mutex_lock(&data_mutex);
    if (font != NULL)
        memcpy(snapshot_font, font, 256u * height);
    snapshot_font_height = height;
    pthread_mutex_unlock(&data_mutex);
    pthread_mutex_unlock(&control_mutex);
    return TIGT_OK;
}

int
tigt_snapshot_status(uint64_t *sequence, int *result)
{
    if (sequence == NULL || result == NULL)
        return TIGT_ERROR_ARGUMENT;
    pthread_mutex_lock(&data_mutex);
    *sequence = completed_sequence;
    *result = completed_result;
    pthread_mutex_unlock(&data_mutex);
    return TIGT_OK;
}

static int
write_configured_snapshot(void)
{
    const int fd = open(configured_path, O_WRONLY | O_CREAT | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return TIGT_ERROR_SYSTEM;
    struct stat metadata;
    int result = TIGT_ERROR_SYSTEM;
    if (fstat(fd, &metadata) == 0 && (S_ISREG(metadata.st_mode) || S_ISFIFO(metadata.st_mode))) {
        if (!S_ISREG(metadata.st_mode) || ftruncate(fd, 0) == 0)
            result = write_snapshot(fd, configured_format, true, S_ISFIFO(metadata.st_mode));
    }
    int saved_errno = errno;
    if (close(fd) < 0 && result == TIGT_OK) {
        saved_errno = errno;
        result = TIGT_ERROR_SYSTEM;
    }
    errno = saved_errno;
    return result;
}

static void *
snapshot_worker(void *unused)
{
    const struct timespec interval = { .tv_nsec = 10000000 };
    (void) unused;
    while (atomic_load_explicit(&worker_running, memory_order_relaxed)) {
        if (atomic_exchange_explicit(&requested, 0, memory_order_relaxed) != 0) {
            const int result = write_configured_snapshot();
            pthread_mutex_lock(&data_mutex);
            completed_sequence++;
            completed_result = result;
            pthread_mutex_unlock(&data_mutex);
        }
        nanosleep(&interval, NULL);
    }
    return NULL;
}

static void
stop_worker(void)
{
    atomic_store_explicit(&worker_running, false, memory_order_relaxed);
    if (worker_created) {
        pthread_join(worker_thread, NULL);
        worker_created = false;
    }
    atomic_store_explicit(&requested, 0, memory_order_relaxed);
    free(configured_path);
    configured_path = NULL;
}

/* Called with control_mutex held. No worker operation acquires that mutex. */
static int
disable_configuration(void)
{
    int result = TIGT_OK;
    const int number = atomic_exchange_explicit(&installed_signal, 0, memory_order_relaxed);
    if (number != 0) {
        struct sigaction current;
        if (sigaction(number, NULL, &current) < 0)
            result = TIGT_ERROR_SYSTEM;
        else if ((current.sa_flags & SA_SIGINFO) == 0 && current.sa_handler == snapshot_signal &&
                 sigaction(number, &previous_action, NULL) < 0)
            result = TIGT_ERROR_SYSTEM;
    }
    stop_worker();
    return result;
}

int
tigt_snapshot_configure(int signal_number, uint32_t format, const char *path)
{
    if (signal_number != 0 &&
        ((signal_number != SIGUSR1 && signal_number != SIGUSR2) ||
         format > TIGT_SNAPSHOT_ATTRIBUTES || path == NULL || path[0] == '\0'))
        return TIGT_ERROR_ARGUMENT;
    pthread_mutex_lock(&control_mutex);
    if (signal_number == 0) {
        const int result = disable_configuration();
        pthread_mutex_unlock(&control_mutex);
        return result;
    }
    if (!session_active) {
        pthread_mutex_unlock(&control_mutex);
        return TIGT_ERROR_BUSY;
    }
    struct stat metadata;
    if (lstat(path, &metadata) == 0) {
        if (!S_ISREG(metadata.st_mode) && !S_ISFIFO(metadata.st_mode)) {
            pthread_mutex_unlock(&control_mutex);
            return TIGT_ERROR_ARGUMENT;
        }
    } else if (errno != ENOENT) {
        pthread_mutex_unlock(&control_mutex);
        return TIGT_ERROR_SYSTEM;
    }
    struct sigaction action;
    if (sigaction(signal_number, NULL, &action) < 0) {
        pthread_mutex_unlock(&control_mutex);
        return TIGT_ERROR_SYSTEM;
    }
    const bool ours = signal_number == atomic_load_explicit(&installed_signal, memory_order_relaxed) &&
                      (action.sa_flags & SA_SIGINFO) == 0 && action.sa_handler == snapshot_signal;
    /* SIG_DFL ignores action flags; macOS may report SA_SIGINFO on it. */
    if (!ours && action.sa_handler != SIG_DFL) {
        pthread_mutex_unlock(&control_mutex);
        return TIGT_ERROR_BUSY;
    }
    char *copy = strdup(path);
    if (copy == NULL) {
        pthread_mutex_unlock(&control_mutex);
        return TIGT_ERROR_SYSTEM;
    }
    int result = TIGT_OK;
    if (ours)
        stop_worker();
    else
        result = disable_configuration();
    if (result != TIGT_OK) {
        free(copy);
        pthread_mutex_unlock(&control_mutex);
        return result;
    }
    configured_path = copy;
    configured_format = format;
    atomic_store_explicit(&worker_running, true, memory_order_relaxed);
    const int thread_error = pthread_create(&worker_thread, NULL, snapshot_worker, NULL);
    if (thread_error != 0) {
        errno = thread_error;
        disable_configuration();
        pthread_mutex_unlock(&control_mutex);
        return TIGT_ERROR_SYSTEM;
    }
    worker_created = true;
    if (!ours) {
        memset(&action, 0, sizeof(action));
        sigemptyset(&action.sa_mask);
        action.sa_handler = snapshot_signal;
        action.sa_flags = SA_RESTART;
        atomic_store_explicit(&installed_signal, signal_number, memory_order_relaxed);
        if (sigaction(signal_number, &action, &previous_action) < 0) {
            atomic_store_explicit(&installed_signal, 0, memory_order_relaxed);
            disable_configuration();
            result = TIGT_ERROR_SYSTEM;
        }
    }
    pthread_mutex_unlock(&control_mutex);
    return result;
}

void
tigt_snapshot_session_reset(void)
{
    pthread_mutex_lock(&control_mutex);
    pthread_mutex_lock(&data_mutex);
    snapshot_font_height = 0;
    completed_sequence = 0;
    completed_result = TIGT_OK;
    pthread_mutex_unlock(&data_mutex);
    pthread_mutex_unlock(&control_mutex);
}

void
tigt_snapshot_session_start(void)
{
    pthread_mutex_lock(&control_mutex);
    session_active = true;
    pthread_mutex_unlock(&control_mutex);
}

void
tigt_snapshot_session_stop(bool shutdown)
{
    pthread_mutex_lock(&control_mutex);
    session_active = false;
    if (shutdown) {
        disable_configuration();
        pthread_mutex_lock(&data_mutex);
        snapshot_font_height = 0;
        pthread_mutex_unlock(&data_mutex);
    }
    pthread_mutex_unlock(&control_mutex);
}

int
tigt_snapshot_environment(void)
{
    const char *path = getenv("TIGT_SNAPSHOT_PATH");
    if (path == NULL)
        return TIGT_OK;
    static const char *const names[] = { "png", "utf8", "ascii", "cp437", "ansi", "cells", "attributes" };
    const char *format_name = getenv("TIGT_SNAPSHOT_FORMAT");
    uint32_t format = TIGT_SNAPSHOT_PNG;
    if (format_name != NULL) {
        for (format = 0; format <= TIGT_SNAPSHOT_ATTRIBUTES; format++) {
            if (strcmp(format_name, names[format]) == 0)
                break;
        }
        if (format > TIGT_SNAPSHOT_ATTRIBUTES)
            return TIGT_ERROR_ARGUMENT;
    }
    const char *signal_name = getenv("TIGT_SNAPSHOT_SIGNAL");
    int number = SIGUSR1;
    if (signal_name != NULL) {
        if (strcmp(signal_name, "USR2") == 0)
            number = SIGUSR2;
        else if (strcmp(signal_name, "USR1") != 0)
            return TIGT_ERROR_ARGUMENT;
    }
    return tigt_snapshot_configure(number, format, path);
}
