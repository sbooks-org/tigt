/* SPDX-License-Identifier: MIT-0 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "terminal_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* Lock-free integer atomics are required by the handler path, not an optional
 * optimization. Saved descriptors/termios are immutable while captured. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "terminal signals require lock-free int atomics");
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "terminal signals require lock-free pointer atomics");
enum { OWN_INPUT = 1, OWN_OUTPUT = 2, OWN_KEYBOARD = 4, OWN_MOUSE = 8, OWN_SCREEN = 16 };
static atomic_int captured, owned, blocked, failure, events, generation, desired_raw;
static atomic_int desired_keyboard, desired_mouse, probe_mode, input_disabled, input_generation;
static atomic_int deferred_restore, faulted;
static int input_fd = -1, output_fd = -1, cleanup_fd = -1;
static atomic_int pending_protocols, pending_fullscreen;
static bool input_tty, output_tty;
static struct termios baseline_input, baseline_output;
static pthread_mutex_t io_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static void (*owner_transition)(unsigned);
static int last_foreground;
static const char keyboard_on[] = "\033[>u\033[=11;1u\033[?2004s\033[?2004h";
static const char keyboard_off[] = "\033[?2004l\033[?2004r\033[<u";
static const char mouse_off[] = "\033[?9;1000;1002;1003;1005;1006;1015;1016l\033[?9;1000;1002;1003;1005;1006;1015;1016r";
static const char screen_off[] = "\033[0m\033[0 q\033[?25h\033[?1049l";

void tigt_terminal_record_error(int error)
{
    int expected = TIGT_OK;
    if (error < TIGT_OK)
        atomic_compare_exchange_strong(&failure, &expected, error);
}

void tigt_terminal_request_restore(void)
{
    atomic_store(&faulted, 0);
    atomic_store(&deferred_restore, 1);
}
int tigt_terminal_status(void) { return atomic_load(&failure); }
unsigned tigt_terminal_generation(void) { return (unsigned) atomic_load(&generation); }
int tigt_terminal_is_released(void) { return atomic_load(&blocked) != 0; }
int tigt_terminal_is_faulted(void) { return atomic_load(&faulted); }

int tigt_terminal_fd_foreground(int fd)
{
    pid_t group = tcgetpgrp(fd);
    if (group >= 0)
        return group == getpgrp();
    /* An ordinary file/pipe is usable as glass. A TTY without a controlling
     * foreground group is not permission to enter fullscreen or read it. */
    return !isatty(fd);
}

int tigt_terminal_is_foreground(void)
{
    if (!atomic_load(&captured))
        return 0;
    return (!input_tty || atomic_load(&input_disabled) || tcgetpgrp(input_fd) == getpgrp()) &&
           (!output_tty || tcgetpgrp(output_fd) == getpgrp());
}

static int release_write(const char *text, size_t length)
{
    if (cleanup_fd < 0 || write(cleanup_fd, text, length) != (ssize_t) length) {
        tigt_terminal_record_error(TIGT_ERROR_SYSTEM);
        return TIGT_ERROR_SYSTEM;
    }
    return TIGT_OK;
}

void tigt_terminal_pending_fullscreen(int delta)
{
    atomic_fetch_add(&pending_fullscreen, delta);
}

void tigt_terminal_release(void)
{
    int saved_errno = errno;
    bool was_active = atomic_exchange(&blocked, 1) == 0;
    int state = atomic_exchange(&owned, 0);
    /* The borrowed stdout may have failed, closed, or been replaced. Cleanup
     * belongs to the original captured TTY, never to its replacement fd. */
    bool output_foreground = cleanup_fd >= 0 && tcgetpgrp(cleanup_fd) == getpgrp();
    bool foreground = (!input_tty || atomic_load(&input_disabled) || tcgetpgrp(input_fd) == getpgrp()) &&
                      (!output_tty || output_foreground);
    if (atomic_load(&captured) && !foreground &&
        atomic_load(&pending_fullscreen) > 0)
        tigt_terminal_record_error(TIGT_ERROR_BACKGROUND);
    if (state != 0) {
        /* tcsetattr may raise TTOU even without TOSTOP. Block it locally, not
         * by changing the application's disposition or terminal flags. */
        sigset_t set, old;
        sigemptyset(&set);
        sigaddset(&set, SIGTTOU);
        sigaddset(&set, SIGPIPE);
        /* pthread_sigmask is async-signal-safe. sigprocmask is unspecified in
         * threaded programs and changes every thread's mask on Darwin. */
        pthread_sigmask(SIG_BLOCK, &set, &old);
        if ((state & OWN_OUTPUT) && tcsetattr(cleanup_fd, TCSANOW, &baseline_output) < 0)
            tigt_terminal_record_error(TIGT_ERROR_SYSTEM);
        if ((state & OWN_INPUT) && tcsetattr(input_fd, TCSANOW, &baseline_input) < 0)
            tigt_terminal_record_error(TIGT_ERROR_SYSTEM);
        if (output_foreground) {
            if (state & OWN_KEYBOARD) release_write(keyboard_off, sizeof(keyboard_off) - 1);
            if (state & OWN_MOUSE) release_write(mouse_off, sizeof(mouse_off) - 1);
            if (state & OWN_SCREEN) release_write(screen_off, sizeof(screen_off) - 1);
        } else {
            atomic_fetch_or(&pending_protocols, state & (OWN_KEYBOARD | OWN_MOUSE | OWN_SCREEN));
        }
        pthread_sigmask(SIG_SETMASK, &old, NULL);
    }
    if (atomic_load(&captured) && (state != 0 || was_active)) {
        atomic_fetch_add(&generation, 1);
        atomic_fetch_or(&events, TIGT_TERMINAL_RELEASED | TIGT_TERMINAL_INPUT_RESET);
    }
    errno = saved_errno;
}

int tigt_terminal_capture(int in, int out)
{
    if (in < 0 || out < 0 || fcntl(in, F_GETFD) < 0 || fcntl(out, F_GETFD) < 0)
        return TIGT_ERROR_ARGUMENT;
    pthread_mutex_lock(&lifecycle_mutex);
    if (atomic_load(&captured)) {
        pthread_mutex_unlock(&lifecycle_mutex);
        return TIGT_ERROR_BUSY;
    }
    bool in_tty = isatty(in), out_tty = isatty(out);
    struct termios in_mode, out_mode;
    if ((in_tty && tcgetattr(in, &in_mode) < 0) ||
        (out_tty && tcgetattr(out, &out_mode) < 0)) {
        pthread_mutex_unlock(&lifecycle_mutex);
        return TIGT_ERROR_SYSTEM;
    }
    int fd = -1;
    if (out_tty) {
        char name[1024];
        if (ttyname_r(out, name, sizeof(name)) != 0 ||
            (fd = open(name, O_WRONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC)) < 0) {
            pthread_mutex_unlock(&lifecycle_mutex);
            return TIGT_ERROR_SYSTEM;
        }
    }
    input_fd = in;
    output_fd = out;
    cleanup_fd = fd;
    input_tty = in_tty;
    output_tty = out_tty;
    if (in_tty) baseline_input = in_mode;
    if (out_tty) baseline_output = out_mode;
    atomic_store(&owned, 0);
    atomic_store(&pending_protocols, 0);
    atomic_store(&failure, TIGT_OK);
    atomic_store(&events, 0);
    atomic_store(&desired_raw, 0);
    atomic_store(&input_disabled, 0);
    atomic_store(&desired_keyboard, 0);
    atomic_store(&desired_mouse, TIGT_MOUSE_OFF);
    atomic_store(&probe_mode, 0);
    atomic_store(&deferred_restore, 0);
    atomic_store(&faulted, 0);
    atomic_store(&captured, 1);
    last_foreground = tigt_terminal_is_foreground();
    atomic_store(&blocked, !last_foreground);
    atomic_fetch_add(&generation, 1);
    pthread_mutex_unlock(&lifecycle_mutex);
    return TIGT_OK;
}

void tigt_terminal_forget(void)
{
    pthread_mutex_lock(&lifecycle_mutex);
    pthread_mutex_lock(&io_mutex);
    tigt_terminal_release();
    atomic_store(&captured, 0);
    owner_transition = NULL;
    if (cleanup_fd >= 0) close(cleanup_fd);
    cleanup_fd = -1;
    input_fd = output_fd = -1;
    pthread_mutex_unlock(&io_mutex);
    pthread_mutex_unlock(&lifecycle_mutex);
}

static int normal_write(const char *text)
{
    size_t length = strlen(text);
    while (length != 0) {
        if (!tigt_terminal_is_foreground())
            return TIGT_ERROR_BACKGROUND;
        ssize_t count = write(output_fd, text, length);
        if (count > 0) { text += count; length -= (size_t) count; }
        else if (count < 0 && errno == EINTR) continue;
        else return TIGT_ERROR_SYSTEM;
    }
    return TIGT_OK;
}

static int apply_input(void)
{
    if (!input_tty) return TIGT_OK;
    struct termios mode = baseline_input;
    if (!atomic_load(&input_disabled) && atomic_load(&desired_raw)) {
        mode.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
        mode.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        mode.c_cflag = (mode.c_cflag & ~(CSIZE | PARENB)) | CS8;
        mode.c_cc[VMIN] = 0;
        mode.c_cc[VTIME] = 0;
    } else if (!atomic_load(&input_disabled)) {
        mode.c_lflag |= ICANON | ECHO;
    }
    if (atomic_load(&probe_mode)) {
        mode.c_lflag &= ~(ICANON | ECHO | ECHONL);
        mode.c_cc[VMIN] = 0;
        mode.c_cc[VTIME] = 0;
    }
    atomic_fetch_or(&owned, OWN_INPUT);
    return tcsetattr(input_fd, TCSANOW, &mode) == 0 ? TIGT_OK : TIGT_ERROR_SYSTEM;
}

static int apply_protocols(void)
{
    if (!output_tty) return TIGT_OK;
    int state = atomic_load(&owned), result = TIGT_OK;
    bool keyboard = !atomic_load(&input_disabled) && atomic_load(&desired_keyboard);
    if (keyboard && !(state & OWN_KEYBOARD)) {
        atomic_fetch_or(&owned, OWN_KEYBOARD);
        result = normal_write(keyboard_on);
    } else if (!keyboard && (state & OWN_KEYBOARD)) {
        result = normal_write(keyboard_off);
        if (result == TIGT_OK) atomic_fetch_and(&owned, ~OWN_KEYBOARD);
    }
    return result;
}

int tigt_terminal_set_input_mode(int raw, int keyboard)
{
    if ((raw != 0 && raw != 1) || (keyboard != 0 && keyboard != 1))
        return TIGT_ERROR_ARGUMENT;
    if (!atomic_load(&captured)) return TIGT_ERROR_ARGUMENT;
    pthread_mutex_lock(&io_mutex);
    bool reenable = atomic_exchange(&input_disabled, 0) != 0;
    atomic_store(&desired_raw, raw);
    atomic_store(&desired_keyboard, keyboard);
    int result = TIGT_OK;
    if (tigt_terminal_is_foreground() && !atomic_load(&blocked)) {
        result = apply_input();
        if (result == TIGT_OK) result = apply_protocols();
        if (result == TIGT_OK && reenable && atomic_load(&desired_mouse) != TIGT_MOUSE_OFF)
            result = tigt_terminal_mouse((uint32_t) atomic_load(&desired_mouse));
        if (result != TIGT_OK) tigt_terminal_release();
    }
    pthread_mutex_unlock(&io_mutex);
    if (result != TIGT_OK) tigt_terminal_record_error(result);
    if (reenable && result == TIGT_OK) {
        atomic_fetch_or(&events, TIGT_TERMINAL_INPUT_RESET);
        if (owner_transition != NULL) {
            owner_transition(TIGT_TERMINAL_INPUT_RESET);
            result = tigt_terminal_status();
        }
    }
    return result;
}

int tigt_terminal_input_disabled(void) { return atomic_load(&input_disabled); }

int tigt_terminal_disable_input(void)
{
    if (!atomic_load(&captured)) return TIGT_ERROR_ARGUMENT;
    pthread_mutex_lock(&io_mutex);
    bool changed = atomic_exchange(&input_disabled, 1) == 0;
    if (changed) atomic_fetch_add(&input_generation, 1);
    atomic_store(&probe_mode, 0);
    int state = atomic_fetch_and(&owned, ~(OWN_INPUT | OWN_KEYBOARD | OWN_MOUSE));
    sigset_t set, previous;
    sigemptyset(&set);
    sigaddset(&set, SIGTTOU);
    pthread_sigmask(SIG_BLOCK, &set, &previous);
    int result = TIGT_OK;
    if ((state & OWN_INPUT) && tcsetattr(input_fd, TCSANOW, &baseline_input) < 0) {
        atomic_fetch_or(&owned, OWN_INPUT);
        result = TIGT_ERROR_SYSTEM;
    }
    bool foreground = cleanup_fd >= 0 && tcgetpgrp(cleanup_fd) == getpgrp();
    if (foreground) {
        if ((state & OWN_KEYBOARD) && release_write(keyboard_off, sizeof(keyboard_off) - 1) != TIGT_OK) {
            atomic_fetch_or(&owned, OWN_KEYBOARD);
            result = TIGT_ERROR_SYSTEM;
        }
        if ((state & OWN_MOUSE) && release_write(mouse_off, sizeof(mouse_off) - 1) != TIGT_OK) {
            atomic_fetch_or(&owned, OWN_MOUSE);
            result = TIGT_ERROR_SYSTEM;
        }
    } else
        atomic_fetch_or(&pending_protocols, state & (OWN_KEYBOARD | OWN_MOUSE));
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    pthread_mutex_unlock(&io_mutex);
    if (changed) {
        atomic_fetch_or(&events, TIGT_TERMINAL_INPUT_RESET);
        /* Joining an input worker while holding the I/O gate would deadlock
         * a reader waiting to finish its bounded read. Output stays active. */
        if (owner_transition != NULL) owner_transition(TIGT_TERMINAL_INPUT_RESET);
    }
    if (result != TIGT_OK) tigt_terminal_record_error(result);
    return result;
}

int tigt_terminal_set_probe_mode(int enabled)
{
    if ((enabled != 0 && enabled != 1) || !atomic_load(&captured))
        return TIGT_ERROR_ARGUMENT;
    if (enabled && atomic_load(&input_disabled)) return TIGT_ERROR_BUSY;
    pthread_mutex_lock(&io_mutex);
    int result = TIGT_ERROR_BACKGROUND;
    if (tigt_terminal_is_foreground() && !atomic_load(&blocked)) {
        atomic_store(&probe_mode, enabled);
        result = apply_input();
    } else if (!enabled) {
        atomic_store(&probe_mode, 0);
        result = TIGT_OK;
    }
    pthread_mutex_unlock(&io_mutex);
    return result;
}

void tigt_terminal_screen(int enabled)
{
    if (enabled) atomic_fetch_or(&owned, OWN_SCREEN | OWN_OUTPUT | (input_tty ? OWN_INPUT : 0));
    else {
        int result = normal_write("\033[0 q");
        if (result == TIGT_OK) atomic_fetch_and(&owned, ~OWN_SCREEN);
        else tigt_terminal_record_error(result);
    }
}

int tigt_terminal_mouse(uint32_t mode)
{
    atomic_store(&desired_mouse, (int) mode);
    if (atomic_load(&input_disabled)) return TIGT_OK;
    if (!tigt_terminal_is_foreground()) return TIGT_ERROR_BACKGROUND;
    if (mode == TIGT_MOUSE_OFF) return TIGT_OK;
    if (!(atomic_load(&owned) & OWN_MOUSE)) {
        atomic_fetch_or(&owned, OWN_MOUSE);
        int result = normal_write("\033[?9;1000;1002;1003;1005;1006;1015;1016s\033[?9;1000;1002;1003;1005;1006;1015;1016l");
        if (result != TIGT_OK) return result;
    }
    return normal_write(mode == TIGT_MOUSE_PIXELS ? "\033[?1006;1016h\033[?1000h\033[?1002h\033[?1003h" :
                        mode == TIGT_MOUSE_X10 ? "\033[?9h" : "\033[?1006h\033[?1000h\033[?1002h\033[?1003h");
}

int tigt_terminal_restore(void)
{
    if (!atomic_load(&captured)) return TIGT_ERROR_ARGUMENT;
    atomic_store(&faulted, 0);
    pthread_mutex_lock(&io_mutex);
    if (!tigt_terminal_is_foreground()) {
        tigt_terminal_release();
        atomic_store(&deferred_restore, 1);
        pthread_mutex_unlock(&io_mutex);
        return TIGT_ERROR_BACKGROUND;
    }
    atomic_store(&deferred_restore, 0);
    bool was_released = atomic_exchange(&blocked, 0) != 0;
    int pending = atomic_exchange(&pending_protocols, 0);
    if (pending & OWN_KEYBOARD) release_write(keyboard_off, sizeof(keyboard_off) - 1);
    if (pending & OWN_MOUSE) release_write(mouse_off, sizeof(mouse_off) - 1);
    if (pending & OWN_SCREEN) release_write(screen_off, sizeof(screen_off) - 1);
    atomic_store(&probe_mode, 0);
    int result = apply_input();
    if (result == TIGT_OK) result = apply_protocols();
    if (result == TIGT_OK && !atomic_load(&input_disabled) &&
        atomic_load(&desired_mouse) != TIGT_MOUSE_OFF)
        result = tigt_terminal_mouse((uint32_t) atomic_load(&desired_mouse));
    if (result != TIGT_OK) {
        tigt_terminal_record_error(result);
        tigt_terminal_release();
    } else if (was_released) {
        atomic_fetch_add(&generation, 1);
        atomic_fetch_or(&events, TIGT_TERMINAL_RESTORED | TIGT_TERMINAL_INPUT_RESET);
    }
    pthread_mutex_unlock(&io_mutex);
    return result;
}

int tigt_terminal_begin_io(int special)
{
    pthread_mutex_lock(&io_mutex);
    if (atomic_load(&faulted)) {
        pthread_mutex_unlock(&io_mutex);
        return 0;
    }
    if (atomic_load(&captured) && atomic_load(&blocked) && tigt_terminal_is_foreground()) {
        pthread_mutex_unlock(&io_mutex);
        return 0;
    }
    if (special && atomic_load(&captured) &&
        (!tigt_terminal_is_foreground() || atomic_load(&blocked))) {
        if (!tigt_terminal_is_foreground()) {
            tigt_terminal_release();
            atomic_store(&deferred_restore, 1);
        }
        pthread_mutex_unlock(&io_mutex);
        return 0;
    }
    return 1;
}
void tigt_terminal_end_io(void) { pthread_mutex_unlock(&io_mutex); }
void tigt_terminal_set_owner(void (*transition)(unsigned)) { owner_transition = transition; }

int tigt_terminal_poll(void)
{
    if (!atomic_load(&captured)) return tigt_terminal_status();
    pthread_mutex_lock(&lifecycle_mutex);
    int foreground = tigt_terminal_is_foreground();
    if (!foreground) {
        pthread_mutex_lock(&io_mutex);
        tigt_terminal_release();
        atomic_store(&deferred_restore, 1);
        pthread_mutex_unlock(&io_mutex);
    } else if (!atomic_load(&faulted) && (!last_foreground || atomic_load(&deferred_restore))) {
        pthread_mutex_lock(&io_mutex);
        tigt_terminal_release();
        pthread_mutex_unlock(&io_mutex);
        tigt_terminal_restore();
    }
    last_foreground = foreground;
    unsigned changes = (unsigned) atomic_exchange(&events, 0);
    if (owner_transition != NULL && changes != 0)
        owner_transition(changes);
    pthread_mutex_unlock(&lifecycle_mutex);
    int error = tigt_terminal_status();
    return error < TIGT_OK ? error : (int) changes;
}

/* The filter is confined to one decoder consumer. Generation changes discard
 * bookkeeping lazily; handlers never touch non-atomic key data. */
static unsigned filter_generation;
static unsigned filter_input_generation;
static bool literal_next, quoted, prefix_down;
static tigt_input_key quoted_key;
static unsigned suppressed;
static bool same_key(tigt_input_key a, tigt_input_key b)
{
    return a.kind == b.kind && a.value == b.value && a.character == b.character;
}
int tigt_terminal_filter_input(const tigt_input_event *event)
{
    if (event == NULL || event->kind > TIGT_RELEASE) return TIGT_ERROR_ARGUMENT;
    unsigned epoch = tigt_terminal_generation();
    unsigned input_epoch = (unsigned) atomic_load(&input_generation);
    if (filter_generation != epoch || filter_input_generation != input_epoch) {
        literal_next = quoted = prefix_down = false;
        suppressed = 0;
        filter_generation = epoch;
        filter_input_generation = input_epoch;
    }
    if (atomic_load(&input_disabled)) return 0;
    if (event->flags & TIGT_INPUT_PASTE) return 1;
    if (!atomic_load(&desired_raw)) return 1;
    if (!tigt_terminal_is_foreground() || atomic_load(&blocked)) return 0;
    if (quoted && same_key(event->key, quoted_key)) {
        if (event->kind == TIGT_RELEASE) quoted = false;
        return 1;
    }
    unsigned character = event->key.character;
    if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    bool control = event->key.kind == TIGT_KEY_CHAR &&
                   (event->modifiers & TIGT_MOD_CONTROL) &&
                   !(event->modifiers & (TIGT_MOD_ALT | TIGT_MOD_SUPER));
    bool quote_key = control && character == 'v';
    if (quote_key && prefix_down && event->kind != TIGT_PRESS) {
        if (event->kind == TIGT_RELEASE) prefix_down = false;
        return 0;
    }
    if (literal_next && event->kind != TIGT_RELEASE && event->key.kind != TIGT_KEY_MODIFIER) {
        literal_next = false;
        quoted = true;
        quoted_key = event->key;
        return 1;
    }
    if (quote_key) {
        if (event->kind == TIGT_PRESS) { literal_next = true; prefix_down = true; }
        return 0;
    }
    unsigned bit = control && character == 'c' ? 1 : control && character == '\\' ? 2 :
                   control && character == 'z' ? 4 : control && character == 't' ? 8 : 0;
    if (!bit) return 1;
    if (event->kind == TIGT_RELEASE) { suppressed &= ~bit; return 0; }
    if (event->kind == TIGT_REPEAT || (suppressed & bit)) return 0;
    suppressed |= bit;
    int signal_number = bit == 1 ? SIGINT : bit == 2 ? SIGQUIT : bit == 4 ? SIGTSTP : 0;
#ifdef SIGINFO
    if (bit == 8) signal_number = SIGINFO;
#endif
    if (signal_number == 0) {
        tigt_terminal_record_error(TIGT_ERROR_UNSUPPORTED);
        return TIGT_ERROR_UNSUPPORTED;
    }
    /* With no installed wrapper the input worker itself can quiesce the
     * writer; it holds no I/O lock while invoking semantic callbacks. */
    if (signal_number != 0
#ifdef SIGINFO
        && signal_number != SIGINFO
#endif
       ) {
        pthread_mutex_lock(&io_mutex);
        tigt_terminal_release();
        pthread_mutex_unlock(&io_mutex);
    }
    if (kill(getpid(), signal_number) < 0) {
        tigt_terminal_record_error(TIGT_ERROR_SYSTEM);
        return TIGT_ERROR_SYSTEM;
    }
    return 0;
}

/* Ordinary handlers enqueue fixed-size records, then return. A dispatcher
 * waits for the I/O gate before restoring; an interrupted renderer/input
 * callback can finish without self-joining or writing after restoration. */
static const int managed_signals[] = {
    SIGINT, SIGQUIT, SIGTERM, SIGPIPE, SIGHUP, SIGTSTP, SIGTTIN, SIGTTOU,
    SIGCONT, SIGWINCH, SIGSEGV, SIGILL, SIGFPE,
#ifdef SIGBUS
    SIGBUS,
#endif
#ifdef SIGINFO
    SIGINFO,
#endif
#ifdef SIGPWR
    SIGPWR,
#endif
};
#define SIGNAL_COUNT (sizeof(managed_signals) / sizeof(managed_signals[0]))
static struct sigaction prior_actions[SIGNAL_COUNT];
/* Signal handlers never mutate saved structs. One-shot consumption is an
 * atomic claim, including simultaneous synchronous faults on different threads. */
static atomic_int handlers_installed, stopped_handlers, consumed_actions;
static int signal_pipe[2] = { -1, -1 };
static pthread_t signal_thread;
typedef struct { int number; siginfo_t info; sigset_t mask; } signal_record;
_Static_assert(sizeof(signal_record) <= 512, "signal record must fit POSIX PIPE_BUF");
static _Atomic(signal_record *) replaying_signal;
static void signal_handler(int number, siginfo_t *info, void *context);

static int signal_index(int number)
{
    for (size_t i = 0; i < SIGNAL_COUNT; i++)
        if (managed_signals[i] == number) return (int) i;
    return -1;
}
static bool fatal_signal(int number)
{
    return number == SIGSEGV || number == SIGILL || number == SIGFPE
#ifdef SIGBUS
        || number == SIGBUS
#endif
        ;
}
static struct sigaction wrapper_action(const struct sigaction *prior)
{
    struct sigaction action = *prior;
    action.sa_sigaction = signal_handler;
    action.sa_flags = (prior->sa_flags & (SA_RESTART | SA_ONSTACK | SA_NODEFER)) | SA_SIGINFO;
    return action;
}

static struct sigaction effective_action(size_t index)
{
    struct sigaction action = prior_actions[index];
    if ((unsigned) atomic_load(&consumed_actions) & (1u << index)) {
        action = (struct sigaction) { 0 };
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
    }
    return action;
}

static void rearm_stopped_handlers(void)
{
    unsigned pending = (unsigned) atomic_exchange(&stopped_handlers, 0);
    if (!atomic_load(&handlers_installed)) return;
    for (size_t i = 0; i < SIGNAL_COUNT; i++) {
        if (!(pending & (1u << i))) continue;
        struct sigaction current;
        if (sigaction(managed_signals[i], NULL, &current) == 0 && current.sa_handler == SIG_DFL) {
            struct sigaction prior = effective_action(i);
            struct sigaction action = wrapper_action(&prior);
            sigaction(managed_signals[i], &action, NULL);
        }
    }
}

static void forward_signal(int number, siginfo_t *info, void *context, const sigset_t *mask)
{
    int index = signal_index(number);
    if (index < 0) return;
    struct sigaction prior = effective_action((size_t) index);
    if (prior.sa_handler != SIG_DFL && prior.sa_handler != SIG_IGN &&
        (prior.sa_flags & SA_RESETHAND) &&
        ((unsigned) atomic_fetch_or(&consumed_actions, 1u << index) & (1u << index)))
        prior = effective_action((size_t) index);
    bool fatal = fatal_signal(number);
    if (prior.sa_handler != SIG_DFL && prior.sa_handler != SIG_IGN) {
        sigset_t saved;
        if (mask != NULL) pthread_sigmask(SIG_SETMASK, mask, &saved);
        if (prior.sa_flags & SA_SIGINFO) prior.sa_sigaction(number, info, context);
        else prior.sa_handler(number);
        if (mask != NULL) pthread_sigmask(SIG_SETMASK, &saved, NULL);
        return;
    } else if (prior.sa_handler == SIG_IGN || (!fatal && (number == SIGCONT || number == SIGWINCH
#ifdef SIGINFO
                           || number == SIGINFO
#endif
                          ))) return;
    sigaction(number, &prior, NULL);
    if (number == SIGTSTP || number == SIGTTIN || number == SIGTTOU)
        atomic_fetch_or(&stopped_handlers, 1u << index);
    sigset_t one, saved;
    sigemptyset(&one);
    sigaddset(&one, number);
    pthread_sigmask(SIG_UNBLOCK, &one, &saved);
    /* Terminating defaults stay installed. Replacing one immediately after a
     * nested raise can consume deferred delivery in another wrapper. Stop
     * wrappers are rearmed only by an actual subsequent SIGCONT delivery. */
    raise(number);
    pthread_sigmask(SIG_SETMASK, &saved, NULL);
}

static void signal_handler(int number, siginfo_t *info, void *context)
{
    int saved_errno = errno;
    /* Do not access C thread-local storage here: first-touch TLV resolution
     * can allocate in a signal handler on Darwin. pthread_self is async-safe;
     * the dispatcher identity is fixed before any wrappers are installed. */
    signal_record *record = pthread_self() == signal_thread ? atomic_load(&replaying_signal) : NULL;
    if (record != NULL && record->number == number) {
        atomic_store(&replaying_signal, NULL);
        forward_signal(number, &record->info, context, &record->mask);
        errno = saved_errno;
        return;
    }
    if (fatal_signal(number)) {
        atomic_store(&faulted, 1);
        atomic_store(&deferred_restore, 0);
        tigt_terminal_release();
        forward_signal(number, info, context, NULL);
        errno = saved_errno;
        return;
    }
    if (number == SIGCONT || number == SIGWINCH
#ifdef SIGINFO
        || number == SIGINFO
#endif
       ) {
        if (number == SIGCONT) {
            rearm_stopped_handlers();
            atomic_store(&deferred_restore, 1);
        }
        if (number == SIGWINCH) atomic_fetch_or(&events, TIGT_TERMINAL_RESIZED);
        forward_signal(number, info, context, NULL);
    } else {
        signal_record record;
        record.number = number;
        if (info != NULL) record.info = *info;
        else memset(&record.info, 0, sizeof(record.info));
        pthread_sigmask(SIG_SETMASK, NULL, &record.mask);
        if (atomic_exchange(&blocked, 1) == 0 && atomic_load(&captured)) {
            atomic_fetch_add(&generation, 1);
            atomic_fetch_or(&events, TIGT_TERMINAL_RELEASED | TIGT_TERMINAL_INPUT_RESET);
        }
        if (write(signal_pipe[1], &record, sizeof(record)) != (ssize_t) sizeof(record)) {
            tigt_terminal_record_error(TIGT_ERROR_SYSTEM);
            tigt_terminal_release();
            forward_signal(number, info, context, NULL);
        }
    }
    errno = saved_errno;
}

static void *signal_main(void *unused)
{
    (void) unused;
    sigset_t faults;
    sigemptyset(&faults);
    for (size_t i = 0; i < SIGNAL_COUNT; i++)
        if (fatal_signal(managed_signals[i])) sigaddset(&faults, managed_signals[i]);
    pthread_sigmask(SIG_UNBLOCK, &faults, NULL);
    signal_record record;
    while (1) {
        ssize_t length = read(signal_pipe[0], &record, sizeof(record));
        if (length < 0 && errno == EINTR) continue;
        if (length != (ssize_t) sizeof(record) || record.number == 0) break;
        pthread_mutex_lock(&io_mutex);
        tigt_terminal_release();
        pthread_mutex_unlock(&io_mutex);
        int index = signal_index(record.number);
        if (index < 0) continue;
        struct sigaction prior = effective_action((size_t) index);
        if (prior.sa_handler == SIG_DFL || prior.sa_handler == SIG_IGN) {
            forward_signal(record.number, &record.info, NULL, &record.mask);
            continue;
        }
        sigset_t delivery = record.mask, saved;
        sigdelset(&delivery, record.number);
        atomic_store(&replaying_signal, &record);
        pthread_sigmask(SIG_SETMASK, &delivery, &saved);
        raise(record.number);
        pthread_sigmask(SIG_SETMASK, &saved, NULL);
        atomic_store(&replaying_signal, NULL);
    }
    return NULL;
}

static int install_signal_handlers(const tigt_terminal_signal_action *actions, size_t count)
{
    if ((count != 0 && actions == NULL) || count > SIGNAL_COUNT)
        return TIGT_ERROR_ARGUMENT;
    unsigned seen = 0;
    for (size_t i = 0; i < count; i++) {
        int index = signal_index(actions[i].signal_number);
        if (index < 0 || (seen & (1u << index)))
            return TIGT_ERROR_ARGUMENT;
        seen |= 1u << index;
    }
    pthread_mutex_lock(&lifecycle_mutex);
    if (atomic_load(&handlers_installed)) {
        pthread_mutex_unlock(&lifecycle_mutex);
        return count != 0 ? TIGT_ERROR_BUSY : TIGT_OK;
    }
    atomic_store(&stopped_handlers, 0);
    atomic_store(&consumed_actions, 0);
    if (pipe(signal_pipe) < 0) goto failure;
    if (fcntl(signal_pipe[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(signal_pipe[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(signal_pipe[1], F_SETFL, O_NONBLOCK) < 0) goto failure;
    for (size_t i = 0; i < SIGNAL_COUNT; i++)
        if (sigaction(managed_signals[i], NULL, &prior_actions[i]) < 0) goto failure;
    for (size_t i = 0; i < count; i++)
        prior_actions[signal_index(actions[i].signal_number)] = actions[i].action;
    sigset_t set, previous;
    sigemptyset(&set);
    for (size_t i = 0; i < SIGNAL_COUNT; i++) sigaddset(&set, managed_signals[i]);
    int result = pthread_sigmask(SIG_BLOCK, &set, &previous);
    if (result != 0) { errno = result; goto failure; }
    result = pthread_create(&signal_thread, NULL, signal_main, NULL);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (result != 0) { errno = result; goto failure; }
    atomic_store(&handlers_installed, 1);
    for (size_t i = 0; i < SIGNAL_COUNT; i++) {
        struct sigaction action = wrapper_action(&prior_actions[i]);
        if (sigaction(managed_signals[i], &action, NULL) < 0) {
            pthread_mutex_unlock(&lifecycle_mutex);
            tigt_terminal_uninstall_signal_handlers();
            return TIGT_ERROR_SYSTEM;
        }
    }
    pthread_mutex_unlock(&lifecycle_mutex);
    return TIGT_OK;
failure:
    if (signal_pipe[0] >= 0) close(signal_pipe[0]);
    if (signal_pipe[1] >= 0) close(signal_pipe[1]);
    signal_pipe[0] = signal_pipe[1] = -1;
    pthread_mutex_unlock(&lifecycle_mutex);
    return TIGT_ERROR_SYSTEM;
}

int tigt_terminal_install_signal_handlers(void)
{
    return install_signal_handlers(NULL, 0);
}

int tigt_terminal_install_signal_handlers_with_actions(
    const tigt_terminal_signal_action *actions, size_t count)
{
    return install_signal_handlers(actions, count);
}

void tigt_terminal_uninstall_signal_handlers(void)
{
    pthread_mutex_lock(&lifecycle_mutex);
    if (atomic_exchange(&handlers_installed, 0)) {
        /* Keep wrappers in place while draining queued deliveries so replay
         * retains original siginfo and supplies a live signal context. */
        /* Closing the write end wakes a blocked reader after queued records.
         * The dispatcher never calls uninstall, and must not outlive its fds. */
        close(signal_pipe[1]);
        pthread_join(signal_thread, NULL);
        close(signal_pipe[0]);
        for (size_t i = 0; i < SIGNAL_COUNT; i++) {
            struct sigaction current;
            if (sigaction(managed_signals[i], NULL, &current) == 0 &&
                (current.sa_flags & SA_SIGINFO) && current.sa_sigaction == signal_handler) {
                struct sigaction prior = effective_action(i);
                sigaction(managed_signals[i], &prior, NULL);
            }
        }
        signal_pipe[0] = signal_pipe[1] = -1;
    }
    pthread_mutex_unlock(&lifecycle_mutex);
}
