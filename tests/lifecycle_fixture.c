/* SPDX-License-Identifier: MIT-0
 * Copyright (C) 2026 Simplebooks Foundation
 * Copyright (C) 2026 Josh Rodd
 *
 * The Rust harness supplies a private controlling PTY. This process acts as its
 * shell: jobs have a separate, nonorphaned process group in the same session.
 * Pipes acknowledge every ownership transition; no timing sleeps are used.
 */
#include "tigt.h"
#include "tigt_presenter.h"
#include "tigt_terminal.h"
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static pid_t job;
static int channel;
static struct termios baseline;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "signal test notifications require lock-free int atomics");
static atomic_int signal_count, signal_mask_ok, signal_info_ok;
static int handler_notice[2] = { -1, -1 };

static void notify_handler(void)
{
    if (handler_notice[1] >= 0) {
        const char byte = 'H';
        (void) write(handler_notice[1], &byte, 1);
    }
}

static void reap_job(void)
{
    if (job > 0) {
        kill(-job, SIGKILL);
        kill(job, SIGKILL);
        while (waitpid(job, NULL, 0) < 0 && errno == EINTR) {}
        job = 0;
    }
}

static void fail(const char *expression, int line)
{
    fprintf(stderr, "lifecycle:%d: %s (errno=%d)\n", line, expression, errno);
    reap_job();
    exit(1);
}
#define CHECK(expression) do { if (!(expression)) fail(#expression, __LINE__); } while (0)

static void timeout_handler(int number)
{
    (void) number;
    if (job > 0) {
        kill(-job, SIGKILL);
        kill(job, SIGKILL);
        while (waitpid(job, NULL, 0) < 0 && errno == EINTR) {}
    }
    _exit(124);
}

static void send_byte(char byte)
{
    ssize_t count;
    do { count = write(channel, &byte, 1); } while (count < 0 && errno == EINTR);
    CHECK(count == 1);
}

static char receive_byte(void)
{
    char byte;
    ssize_t count;
    do { count = read(channel, &byte, 1); } while (count < 0 && errno == EINTR);
    CHECK(count == 1);
    return byte;
}

static void foreground(pid_t group)
{
    sigset_t block, previous;
    sigemptyset(&block);
    sigaddset(&block, SIGTTOU);
    CHECK(sigprocmask(SIG_BLOCK, &block, &previous) == 0);
    CHECK(tcsetpgrp(STDIN_FILENO, group) == 0);
    CHECK(sigprocmask(SIG_SETMASK, &previous, NULL) == 0);
}

static void check_baseline(void)
{
    struct termios actual;
    CHECK(tcgetattr(STDIN_FILENO, &actual) == 0);
    CHECK(actual.c_iflag == baseline.c_iflag);
    CHECK(actual.c_oflag == baseline.c_oflag);
    CHECK(actual.c_cflag == baseline.c_cflag);
    /* Darwin marks pending input for canonical reprocessing when ICANON is
     * restored. PENDIN is kernel queue state, not a saved terminal setting. */
#ifdef PENDIN
    CHECK((actual.c_lflag & ~PENDIN) == (baseline.c_lflag & ~PENDIN));
#else
    CHECK(actual.c_lflag == baseline.c_lflag);
#endif
    CHECK(memcmp(actual.c_cc, baseline.c_cc, sizeof(actual.c_cc)) == 0);
    CHECK(cfgetispeed(&actual) == cfgetispeed(&baseline));
    CHECK(cfgetospeed(&actual) == cfgetospeed(&baseline));
}

static void check_raw(void)
{
    struct termios actual;
    CHECK(tcgetattr(STDIN_FILENO, &actual) == 0);
    CHECK(!(actual.c_lflag & (ICANON | ECHO | ISIG | IEXTEN)));
}

static void start_capture(void)
{
    CHECK(tigt_terminal_capture(STDIN_FILENO, STDOUT_FILENO) == TIGT_OK);
    CHECK(tigt_terminal_capture(STDIN_FILENO, STDOUT_FILENO) == TIGT_ERROR_BUSY);
    CHECK(tigt_terminal_set_input_mode(1, 1) == TIGT_OK);
    check_raw();
}

static void finish_capture(void)
{
    tigt_terminal_release();
    tigt_terminal_release();
    check_baseline();
    tigt_terminal_forget();
    tigt_terminal_uninstall_signal_handlers();
}

/* The child blocks until the supervisor makes its process group foreground. */
static int begin_job(void)
{
    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    CHECK(fflush(NULL) == 0);
    job = fork();
    CHECK(job >= 0);
    if (!job) {
        close(sockets[0]);
        channel = sockets[1];
        CHECK(setpgid(0, 0) == 0);
        send_byte('R');
        CHECK(receive_byte() == 'S');
        return 1;
    }
    close(sockets[1]);
    channel = sockets[0];
    CHECK(receive_byte() == 'R');
    foreground(job);
    send_byte('S');
    return 0;
}

static int finish_job(void)
{
    int status;
    pid_t result;
    do { result = waitpid(job, &status, 0); } while (result < 0 && errno == EINTR);
    CHECK(result == job);
    job = 0;
    close(channel);
    foreground(getpgrp());
    check_baseline();
    return status;
}

static void simple_handler(int number)
{
    (void) number;
    signal_count++;
    notify_handler();
}

static void wait_handler(void)
{
    char byte;
    ssize_t count;
    do { count = read(handler_notice[0], &byte, 1); } while (count < 0 && errno == EINTR);
    CHECK(count == 1 && byte == 'H');
}

static void info_handler(int number, siginfo_t *info, void *context)
{
    sigset_t mask;
    signal_count++;
    signal_info_ok = info != NULL && context != NULL && info->si_signo == number;
    signal_mask_ok = sigprocmask(SIG_SETMASK, NULL, &mask) == 0 &&
                     sigismember(&mask, SIGUSR1) == 1 && sigismember(&mask, number) == 1;
    notify_handler();
}

static void fatal_handler(int number, siginfo_t *info, void *context)
{
    info_handler(number, info, context);
    _exit(signal_info_ok && signal_mask_ok ? 74 : 75);
}

static void release_twice(void)
{
    if (begin_job()) {
        start_capture();
        unsigned before = tigt_terminal_generation();
        tigt_terminal_release();
        CHECK(tigt_terminal_generation() != before);
        check_baseline();
        tigt_terminal_release();
        check_baseline();
        CHECK(tigt_terminal_restore() == TIGT_OK);
        check_raw();
        finish_capture();
        _exit(0);
    }
    int status = finish_job();
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

enum disposition { CUSTOM, SIGINFO_CUSTOM, IGNORED, DEFAULTED, FATAL_CUSTOM };
static void chain_signal(int number, enum disposition kind, int close_stdout)
{
    if (begin_job()) {
        const struct rlimit no_core = { 0, 0 };
        CHECK(setrlimit(RLIMIT_CORE, &no_core) == 0);
        CHECK(pipe(handler_notice) == 0);
        struct sigaction prior;
        memset(&prior, 0, sizeof(prior));
        sigemptyset(&prior.sa_mask);
        sigaddset(&prior.sa_mask, SIGUSR1);
        if (kind == SIGINFO_CUSTOM || kind == FATAL_CUSTOM) {
            prior.sa_flags = SA_SIGINFO;
            prior.sa_sigaction = kind == FATAL_CUSTOM ? fatal_handler : info_handler;
        } else {
            prior.sa_handler = kind == CUSTOM ? simple_handler :
                               kind == IGNORED ? SIG_IGN : SIG_DFL;
        }
        CHECK(sigaction(number, &prior, NULL) == 0);
        start_capture();
        CHECK(tigt_terminal_install_signal_handlers() == TIGT_OK);
        CHECK(tigt_terminal_install_signal_handlers() == TIGT_OK);
        if (close_stdout)
            CHECK(close(STDOUT_FILENO) == 0);
        CHECK(raise(number) == 0);
        if (kind == DEFAULTED || kind == FATAL_CUSTOM)
            for (;;) pause(); /* Dispatcher/default delivery may complete later. */
        if (kind != IGNORED)
            wait_handler();
        /* Uninstall drains any queued ignored disposition before checking. */
        tigt_terminal_uninstall_signal_handlers();
        CHECK(signal_count == (kind == IGNORED ? 0 : 1));
        if (kind == SIGINFO_CUSTOM)
            CHECK(signal_info_ok && signal_mask_ok);
        check_baseline();
        tigt_terminal_uninstall_signal_handlers();
        struct sigaction restored;
        CHECK(sigaction(number, NULL, &restored) == 0);
        CHECK((restored.sa_flags & SA_SIGINFO) == (prior.sa_flags & SA_SIGINFO));
        CHECK(sigismember(&restored.sa_mask, SIGUSR1) == 1);
        if (kind == SIGINFO_CUSTOM)
            CHECK(restored.sa_sigaction == prior.sa_sigaction);
        else
            CHECK(restored.sa_handler == prior.sa_handler);
        tigt_terminal_forget();
        _exit(0);
    }
    int status = finish_job();
    if (kind == DEFAULTED)
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == number);
    else if (kind == FATAL_CUSTOM)
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 74);
    else
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void informational_signal(int number)
{
    if (begin_job()) {
        CHECK(pipe(handler_notice) == 0);
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_flags = SA_SIGINFO;
        action.sa_sigaction = info_handler;
        sigemptyset(&action.sa_mask);
        sigaddset(&action.sa_mask, SIGUSR1);
        CHECK(sigaction(number, &action, NULL) == 0);
        start_capture();
        CHECK(tigt_terminal_install_signal_handlers() == TIGT_OK);
        unsigned generation = tigt_terminal_generation();
        CHECK(raise(number) == 0);
        wait_handler();
        CHECK(signal_count == 1 && signal_info_ok && signal_mask_ok);
        check_raw();
        CHECK(tigt_terminal_generation() == generation);
        int changes = tigt_terminal_poll();
        CHECK(changes >= 0);
        if (number == SIGWINCH)
            CHECK(changes & TIGT_TERMINAL_RESIZED);
        check_raw();
        finish_capture();
        _exit(0);
    }
    int status = finish_job();
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void request_restore_handler(int number)
{
    (void) number;
    tigt_terminal_request_restore();
    notify_handler();
}

static void custom_continue_request(void)
{
    if (begin_job()) {
        CHECK(pipe(handler_notice) == 0);
        CHECK(signal(SIGCONT, request_restore_handler) != SIG_ERR);
        start_capture();
        tigt_terminal_release();
        check_baseline();
        CHECK(raise(SIGCONT) == 0);
        wait_handler();
        check_baseline(); /* Request is signal-safe; it performs no restore. */
        int changes = tigt_terminal_poll();
        CHECK(changes >= 0 && (changes & TIGT_TERMINAL_RESTORED));
        check_raw();
        finish_capture();
        _exit(0);
    }
    int status = finish_job();
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void invalid_action_overrides(void)
{
    if (begin_job()) {
        CHECK(pipe(handler_notice) == 0);
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = simple_handler;
        sigemptyset(&action.sa_mask);
        CHECK(sigaction(SIGTERM, &action, NULL) == 0);
        start_capture();
        const tigt_terminal_signal_action duplicate[] = {
            { SIGTERM, action }, { SIGTERM, action }
        };
        const tigt_terminal_signal_action unmanaged[] = {
            { SIGTERM, action }, { SIGUSR1, action }
        };
        CHECK(tigt_terminal_install_signal_handlers_with_actions(NULL, 1) == TIGT_ERROR_ARGUMENT);
        CHECK(tigt_terminal_install_signal_handlers_with_actions(duplicate, 2) == TIGT_ERROR_ARGUMENT);
        CHECK(tigt_terminal_install_signal_handlers_with_actions(unmanaged, 2) == TIGT_ERROR_ARGUMENT);
        /* The valid first entry in either rejected list must not partially
         * install cleanup or change the existing signal's observable behavior. */
        CHECK(raise(SIGTERM) == 0);
        wait_handler();
        CHECK(signal_count == 1);
        check_raw();
        const tigt_terminal_signal_action known = { SIGTERM, action };
        CHECK(tigt_terminal_install_signal_handlers_with_actions(&known, 1) == TIGT_OK);
        CHECK(raise(SIGTERM) == 0);
        wait_handler();
        CHECK(signal_count == 2);
        check_baseline();
        finish_capture();
        _exit(0);
    }
    int status = finish_job();
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void one_shot_disposition(void)
{
    if (begin_job()) {
        const struct rlimit no_core = { 0, 0 };
        CHECK(setrlimit(RLIMIT_CORE, &no_core) == 0);
        CHECK(pipe(handler_notice) == 0);
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_flags = SA_SIGINFO | SA_RESETHAND;
        action.sa_sigaction = info_handler;
        sigemptyset(&action.sa_mask);
        sigaddset(&action.sa_mask, SIGUSR1);
        CHECK(sigaction(SIGTERM, &action, NULL) == 0);
        start_capture();
        const tigt_terminal_signal_action known = { SIGTERM, action };
        sigset_t caller_mask, returned_mask;
        CHECK(pthread_sigmask(SIG_SETMASK, NULL, &caller_mask) == 0);
        CHECK(tigt_terminal_install_signal_handlers_with_actions(&known, 1) == TIGT_OK);
        CHECK(tigt_terminal_install_signal_handlers_with_actions(&known, 1) == TIGT_ERROR_BUSY);
        CHECK(tigt_terminal_install_signal_handlers() == TIGT_OK);
        CHECK(raise(SIGTERM) == 0);
        wait_handler();
        CHECK(signal_count == 1 && signal_info_ok);
        CHECK(pthread_sigmask(SIG_SETMASK, NULL, &returned_mask) == 0);
        CHECK(sigismember(&returned_mask, SIGTERM) == sigismember(&caller_mask, SIGTERM));
        CHECK(sigismember(&returned_mask, SIGUSR1) == sigismember(&caller_mask, SIGUSR1));
        tigt_terminal_uninstall_signal_handlers();
        CHECK(sigaction(SIGTERM, NULL, &action) == 0);
        CHECK(action.sa_handler == SIG_DFL);
        CHECK(tigt_terminal_restore() == TIGT_OK);
        CHECK(tigt_terminal_install_signal_handlers() == TIGT_OK);
        CHECK(raise(SIGTERM) == 0);
        for (;;) pause();
    }
    int status = finish_job();
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
}

static void stop_continue(int number)
{
    if (begin_job()) {
        start_capture();
        CHECK(tigt_terminal_install_signal_handlers() == TIGT_OK);
        CHECK(raise(number) == 0);
        /* Parent resumed us in the background first. */
        CHECK(receive_byte() == 'B');
        CHECK(!tigt_terminal_is_foreground());
        (void) tigt_terminal_poll();
        check_baseline();
        CHECK(tigt_terminal_restore() == TIGT_ERROR_BACKGROUND);
        check_baseline();
        send_byte('B');
        CHECK(receive_byte() == 'F');
        CHECK(tigt_terminal_is_foreground());
        (void) tigt_terminal_poll();
        CHECK(tigt_terminal_restore() == TIGT_OK);
        check_raw();
        finish_capture();
        _exit(0);
    }
    int status;
    pid_t result;
    do { result = waitpid(job, &status, WUNTRACED); } while (result < 0 && errno == EINTR);
    CHECK(result == job && WIFSTOPPED(status));
    CHECK(WSTOPSIG(status) == number);
    check_baseline();
    foreground(getpgrp());
    CHECK(kill(job, SIGCONT) == 0);
    send_byte('B');
    CHECK(receive_byte() == 'B');
    check_baseline();
    foreground(job);
    CHECK(kill(job, SIGCONT) == 0);
    send_byte('F');
    status = finish_job();
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static uint32_t pixels[320 * 200];
static uint8_t indices[320 * 200];
static void background_bitmap(int accepted_first)
{
    if (begin_job()) {
        const tigt_config config = { .abi_version = TIGT_ABI_VERSION,
                                     .graphics_mode = accepted_first ? TIGT_GRAPHICS_SIXEL :
                                                                      TIGT_GRAPHICS_BLOCKS };
        CHECK(tigt_init(&config) == TIGT_OK);
        if (accepted_first)
            CHECK(tigt_present_bitmap(pixels, 320, 200, 320, 1) == TIGT_OK);
        send_byte('A');
        CHECK(receive_byte() == 'B');
        CHECK(!tigt_terminal_is_foreground());
        if (accepted_first) {
            /* Force a retained-image redraw only after background ownership is
             * acknowledged. No new bitmap is submitted, and no renderer timing
             * assumption is needed to prove a pending, ineligible presentation. */
            CHECK(tigt_set_image_layout(79, 4, 3) == TIGT_OK);
            CHECK(tigt_terminal_poll() == TIGT_ERROR_BACKGROUND);
            CHECK(tigt_terminal_status() == TIGT_ERROR_BACKGROUND);
            CHECK(tigt_terminal_status() == TIGT_ERROR_BACKGROUND);
        } else {
            CHECK(tigt_present_bitmap(pixels, 320, 200, 320, 1) == TIGT_ERROR_BACKGROUND);
            CHECK(tigt_present_indexed_bitmap(indices, 320, 200, 320, 1, NULL, 0) ==
                  TIGT_ERROR_BACKGROUND);
        }
        tigt_shutdown();
        check_baseline();
        _exit(0);
    }
    CHECK(receive_byte() == 'A');
    foreground(getpgrp());
    send_byte('B');
    int status = finish_job();
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void background_glass(void)
{
    if (begin_job()) {
        CHECK(tigt_terminal_capture(STDIN_FILENO, STDOUT_FILENO) == TIGT_OK);
        tigt_presenter *presenter = NULL;
        const tigt_presenter_config config = { .abi_version = TIGT_PRESENTER_ABI_VERSION,
            .output_fd = STDOUT_FILENO, .mode = TIGT_PRESENT_ADAPTIVE,
            .encoding = TIGT_ENCODING_UTF8, .reversible = 1 };
        CHECK(tigt_presenter_create(&config, &presenter) == TIGT_OK);
        send_byte('A');
        CHECK(receive_byte() == 'B');
        (void) tigt_terminal_poll();
        CHECK(!tigt_terminal_is_foreground());
        const tigt_text_cell cells[4] = {
            { 'a', 0xaaaaaa, 0, 0 }, { ' ', 0xaaaaaa, 0, 0 },
            { ' ', 0xaaaaaa, 0, 0 }, { ' ', 0xaaaaaa, 0, 0 }
        };
        tigt_presenter_frame frame = { .cells = cells, .columns = 2, .rows = 2,
            .stride = 2, .cursor_column = 1, .cursor_row = 0, .refresh_hz = 60 };
        CHECK(tigt_presenter_present(presenter, &frame) == TIGT_OK);
        check_baseline();
        tigt_presenter_destroy(presenter);
        finish_capture();
        _exit(0);
    }
    CHECK(receive_byte() == 'A');
    foreground(getpgrp());
    CHECK(write(STDOUT_FILENO, "\nGLASS-BG-BEGIN\n", 16) == 16);
    send_byte('B');
    int status = finish_job();
    CHECK(write(STDOUT_FILENO, "\nGLASS-BG-END\n", 14) == 14);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static unsigned forwarded[3][3];
static unsigned paste_controls;
static int input_done;
static int unsupported_info;
static void filtered_input(const tigt_input_event *event, void *user)
{
    (void) user;
    int result = tigt_terminal_filter_input(event);
    if (result == TIGT_ERROR_UNSUPPORTED) {
        unsupported_info++;
        return;
    }
    CHECK(result >= 0);
    if (!result)
        return;
    if (event->flags & TIGT_INPUT_PASTE) {
        if (event->key.kind == TIGT_KEY_CHAR && event->key.character == 3 &&
            event->kind == TIGT_PRESS)
            paste_controls++;
        return;
    }
    if (event->key.kind == TIGT_KEY_CHAR && event->modifiers == TIGT_MOD_CONTROL) {
        unsigned index = event->key.character == 'c' ? 0 :
                         event->key.character == 'v' ? 1 : 2;
        CHECK(event->key.character == 'c' || event->key.character == 'v' ||
              event->key.character == 'z');
        CHECK(event->kind <= TIGT_RELEASE);
        forwarded[index][event->kind]++;
    }
    if (event->key.kind == TIGT_KEY_CHAR && event->key.character == 'q' &&
        event->kind == TIGT_PRESS)
        input_done = 1;
}

static void quoted_controls(void)
{
    if (begin_job()) {
        CHECK(pipe(handler_notice) == 0);
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = simple_handler;
        sigemptyset(&action.sa_mask);
        CHECK(sigaction(SIGINT, &action, NULL) == 0);
        CHECK(sigaction(SIGQUIT, &action, NULL) == 0);
        CHECK(sigaction(SIGTSTP, &action, NULL) == 0);
#ifdef SIGINFO
        CHECK(sigaction(SIGINFO, &action, NULL) == 0);
#endif
        start_capture();
        tigt_input *decoder = tigt_input_create(filtered_input, NULL);
        CHECK(decoder != NULL);
        const char ready[] = "\nQUOTE-RAW-READY\n";
        CHECK(write(STDOUT_FILENO, ready, sizeof(ready) - 1) == sizeof(ready) - 1);
        while (!input_done) {
            unsigned char bytes[128];
            struct pollfd input = { .fd = STDIN_FILENO, .events = POLLIN };
            int ready;
            do { ready = poll(&input, 1, -1); } while (ready < 0 && errno == EINTR);
            CHECK(ready == 1);
            ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
            if (count < 0 && errno == EINTR)
                continue;
            CHECK(count > 0);
            tigt_input_feed(decoder, bytes, count);
        }
        CHECK(forwarded[0][TIGT_PRESS] == 1 && forwarded[0][TIGT_REPEAT] == 1 &&
              forwarded[0][TIGT_RELEASE] == 1);
        CHECK(forwarded[1][TIGT_PRESS] == 1 && forwarded[1][TIGT_REPEAT] == 0 &&
              forwarded[1][TIGT_RELEASE] == 1);
        CHECK(forwarded[2][TIGT_PRESS] == 1 && forwarded[2][TIGT_REPEAT] == 0 &&
              forwarded[2][TIGT_RELEASE] == 1);
        CHECK(paste_controls == 1);
        CHECK(signal_count == 0);
        /* Quoting did not change signal policy: unquoted gestures signal once
         * each, restoring after each terminal release before the next gesture. */
        const unsigned char host_keys[] = { 3, 28, 26, 20 };
        for (unsigned i = 0; i < sizeof(host_keys); i++) {
            tigt_input_feed(decoder, &host_keys[i], 1);
#ifndef SIGINFO
            if (host_keys[i] != 20)
#endif
                wait_handler();
            CHECK(tigt_terminal_restore() == TIGT_OK);
        }
#ifdef SIGINFO
        CHECK(signal_count == 4 && unsupported_info == 0);
#else
        CHECK(signal_count == 3 && unsupported_info > 0);
#endif
        CHECK(tigt_terminal_set_input_mode(0, 0) == TIGT_OK);
        const char cooked[] = "\nQUOTE-COOKED-READY\n";
        CHECK(write(STDOUT_FILENO, cooked, sizeof(cooked) - 1) == sizeof(cooked) - 1);
        char line[16];
        ssize_t count;
        do { count = read(STDIN_FILENO, line, sizeof(line)); } while (count < 0 && errno == EINTR);
        CHECK(count == 3 && line[0] == 3 && line[1] == 22 && line[2] == '\n');
        sig_atomic_t before = signal_count;
        tigt_input_feed(decoder, (const unsigned char *) line, count);
        CHECK(signal_count == before);
        CHECK(forwarded[0][TIGT_PRESS] == 2 && forwarded[0][TIGT_RELEASE] == 2);
        CHECK(forwarded[1][TIGT_PRESS] == 2 && forwarded[1][TIGT_RELEASE] == 2);
        tigt_input_destroy(decoder);
        finish_capture();
        _exit(0);
    }
    int status = finish_job();
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
    CHECK(setlocale(LC_ALL, "") != NULL);
    CHECK(tcgetpgrp(STDIN_FILENO) == getpgrp());
    CHECK(tcgetattr(STDIN_FILENO, &baseline) == 0);
    /* A noecho shell baseline must not be replaced by a guessed sane mode. */
    baseline.c_lflag &= ~ECHO;
    baseline.c_lflag |= ICANON | ISIG | IEXTEN;
    baseline.c_cc[VLNEXT] = 22;
    CHECK(tcsetattr(STDIN_FILENO, TCSANOW, &baseline) == 0);
    CHECK(atexit(reap_job) == 0);
    CHECK(signal(SIGALRM, timeout_handler) != SIG_ERR);
    alarm(25);
    release_twice();
    const int terminating[] = { SIGINT, SIGQUIT, SIGTERM, SIGPIPE, SIGHUP,
#ifdef SIGPWR
        SIGPWR,
#endif
    };
    for (unsigned i = 0; i < sizeof(terminating) / sizeof(terminating[0]); i++) {
        chain_signal(terminating[i], CUSTOM, 0);
        chain_signal(terminating[i], SIGINFO_CUSTOM, 0);
        chain_signal(terminating[i], IGNORED, 0);
        chain_signal(terminating[i], DEFAULTED, 0);
    }
    puts("PIPE-CLEANUP-BEGIN");
    chain_signal(SIGPIPE, CUSTOM, 1);
    chain_signal(SIGPIPE, DEFAULTED, 1);
    puts("PIPE-CLEANUP-END");
    one_shot_disposition();
    invalid_action_overrides();
    informational_signal(SIGWINCH);
    informational_signal(SIGCONT);
#ifdef SIGINFO
    informational_signal(SIGINFO);
#endif
    custom_continue_request();
    const int fatal[] = { SIGSEGV, SIGILL, SIGFPE, SIGBUS };
    for (unsigned i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++) {
        chain_signal(fatal[i], DEFAULTED, 0);
        chain_signal(fatal[i], FATAL_CUSTOM, 0);
    }
    stop_continue(SIGTSTP);
    stop_continue(SIGTTIN);
    stop_continue(SIGTTOU);
    background_bitmap(0);
    background_bitmap(1);
    background_glass();
    quoted_controls();
    alarm(0);
    puts("PASS terminal lifecycle, signal chaining, job control, background output and quoting");
    return 0;
}
