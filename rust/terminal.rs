// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

//! Process-terminal ownership shared by native sessions and external presenters.
//!
//! [`release`] and [`request_restore`] are signal-handler-safe. Release restores
//! the captured baseline without joining workers or freeing ownership. Call
//! `poll` on the owner in normal context; restoration is foreground-only.
//! Signal installation is opt-in and chains the dispositions present at installation.
//! A [`crate::Session`] captures automatically; do not create [`Terminal`] beside it.

use crate::{Error, InputEvent, check_status, ffi};
use std::{
    marker::PhantomData,
    os::fd::{AsRawFd, BorrowedFd},
    rc::Rc,
};

/// An explicitly supplied prior disposition for one managed process signal.
///
/// This retains flags that a platform's `sigaction` readback may omit (notably
/// Darwin's SA_RESETHAND). Unlisted signals use their discovered dispositions.
/// Installation copies the action; the supplied slice need not remain alive.
#[repr(C)]
pub struct SignalAction {
    pub signal_number: libc::c_int,
    pub action: libc::sigaction,
}

/// Changes serviced by a normal-context terminal poll.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Events(u32);

impl Events {
    pub const RESTORED: Self = Self(1);
    pub const RELEASED: Self = Self(2);
    pub const RESIZED: Self = Self(4);
    /// Release held guest keys/buttons and invalidate the observed host cursor.
    pub const INPUT_RESET: Self = Self(8);

    pub const fn bits(self) -> u32 {
        self.0
    }
    pub const fn contains(self, other: Self) -> bool {
        self.0 & other.0 == other.0
    }
    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }

    pub(crate) fn from_status(status: i32) -> Result<Self, Error> {
        if status < 0 {
            check_status(status)?;
        }
        Ok(Self(status as u32))
    }
}

/// Desired input policy. Cooked mode uses host canonical editing and echo.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InputMode {
    Cooked,
    /// Raw semantic events are subject to host controls and Ctrl+V quoting.
    Raw {
        keyboard_reporting: bool,
    },
}

/// Restores the saved terminal baseline, idempotently and async-signal-safely.
///
/// This neither joins workers nor releases ownership. It never allocates, locks,
/// uses stdio/curses, or changes signal dispositions. Normal owners must later
/// poll, restore, or shut down. Only protocols recorded as active are disabled;
/// the saved termios is restored exactly, including an initially disabled echo.
pub fn release() {
    unsafe { ffi::tigt_terminal_release() };
}

/// Schedules a foreground-aware restore without performing lifecycle work.
/// This is async-signal-safe for an application-owned SIGCONT handler. The
/// owner must subsequently call `poll` in normal context, including when resumed
/// into the background; no protocol or input mode is restored there.
pub fn request_restore() {
    unsafe { ffi::tigt_terminal_request_restore() };
}

/// An external presenter's exclusive, borrowed process-terminal owner.
///
/// The descriptors remain open for this object's lifetime. This type is neither
/// Send nor Sync; all normal-context operations and any external reads/writes
/// must be serialized on its owner. It does not own a presenter or input decoder.
/// Drop restores the baseline and forgets capture; handlers installed through
/// this object are uninstalled without disturbing registrations it did not own.
pub struct Terminal<'fd> {
    _input: BorrowedFd<'fd>,
    _output: BorrowedFd<'fd>,
    signals_installed: bool,
    _owner_thread: PhantomData<Rc<()>>,
}

impl<'fd> Terminal<'fd> {
    /// Captures the existing baseline without assuming it has echo enabled.
    /// A live capture or native session returns [`Error::Busy`].
    pub fn capture(input: BorrowedFd<'fd>, output: BorrowedFd<'fd>) -> Result<Self, Error> {
        check_status(unsafe { ffi::tigt_terminal_capture(input.as_raw_fd(), output.as_raw_fd()) })?;
        Ok(Self {
            _input: input,
            _output: output,
            signals_installed: false,
            _owner_thread: PhantomData,
        })
    }

    pub fn set_input_mode(&mut self, mode: InputMode) -> Result<(), Error> {
        let (raw, keyboard) = match mode {
            InputMode::Cooked => (0, 0),
            InputMode::Raw { keyboard_reporting } => (1, i32::from(keyboard_reporting)),
        };
        check_status(unsafe { ffi::tigt_terminal_set_input_mode(raw, keyboard) })
    }

    /// Disables input while retaining the presenter's output lease.
    ///
    /// After stopping its input reader (for example on stdin EOF), an external
    /// owner can restore the exact captured input termios, including saved
    /// noecho, and disable keyboard/mouse reporting without suspending valid
    /// glass output. Input and probes remain disabled across restoration until
    /// an explicit [`Self::set_input_mode`] call. This does not change the output
    /// generation or relinquish capture. Call only in normal owner context.
    pub fn disable_input(&mut self) -> Result<(), Error> {
        check_status(unsafe { ffi::tigt_terminal_disable_input() })
    }

    /// Temporarily disables canonical buffering/echo for a foreground query.
    /// Kernel signal and literal-next handling remain intact in cooked mode.
    /// Disabling restores the desired mode without flushing unread input.
    pub fn set_probe_mode(&mut self, enabled: bool) -> Result<(), Error> {
        check_status(unsafe { ffi::tigt_terminal_set_probe_mode(i32::from(enabled)) })
    }

    /// Returns true for guest input, false for a consumed host gesture.
    ///
    /// Apply once to each event from an externally owned decoder, never to an
    /// already-filtered live-session callback. Raw Ctrl+C, Ctrl+backslash and
    /// Ctrl+Z signal the host; Ctrl+T requires platform SIGINFO support. Ctrl+V
    /// quotes one complete gesture, including repeats/release. Cooked bytes and
    /// bracketed-paste payload are forwarded without duplicate host signals.
    pub fn filter_input(&mut self, event: InputEvent) -> Result<bool, Error> {
        match unsafe { ffi::tigt_terminal_filter_input(&event.to_raw()) } {
            0 => Ok(false),
            1 => Ok(true),
            status => {
                check_status(status)?;
                Err(Error::UnexpectedStatus(status))
            }
        }
    }

    /// Finishes deferred signal/job-control work in normal context.
    /// Persistent failures are returned even without another frame submission.
    pub fn poll(&mut self) -> Result<Events, Error> {
        Events::from_status(unsafe { ffi::tigt_terminal_poll() })
    }

    /// Reads the persistent asynchronous error; capture starts a new status epoch.
    pub fn status(&self) -> Result<(), Error> {
        check_status(unsafe { ffi::tigt_terminal_status() })
    }

    /// Changes on release/restore even when a sticky error makes `poll` fail.
    /// Compare around polls to release guest state and invalidate cursor state.
    pub fn generation(&self) -> u32 {
        unsafe { ffi::tigt_terminal_generation() }
    }

    pub fn is_foreground(&self) -> bool {
        unsafe { ffi::tigt_terminal_is_foreground() != 0 }
    }

    /// Restores the retained foreground configuration, never while backgrounded.
    pub fn restore(&mut self) -> Result<(), Error> {
        check_status(unsafe { ffi::tigt_terminal_restore() })
    }

    /// Opts into cleanup and chaining for supported process signals.
    /// Serialize with all application signal-disposition changes.
    pub fn install_signal_handlers(&mut self) -> Result<(), Error> {
        check_status(unsafe { ffi::tigt_terminal_install_signal_handlers() })?;
        self.signals_installed = true;
        Ok(())
    }

    /// Installs handlers using explicit prior actions for the listed signals.
    ///
    /// No allocation or action copying occurs in Rust. Duplicate/unmanaged
    /// signals are rejected before installation; nonempty overrides return
    /// [`Error::Busy`] when handlers are already installed. An empty slice has
    /// the same idempotent behavior as [`Self::install_signal_handlers`].
    ///
    /// # Safety
    ///
    /// Every action must be a valid platform `sigaction`, with valid handler
    /// pointers matching its flags. Handlers and anything they access must
    /// remain valid until the disposition is replaced, including after TIGT
    /// restores it on uninstall. Handlers must obey signal-context requirements
    /// and must never unwind across C. Serialize signal-disposition changes.
    pub unsafe fn install_signal_handlers_with_actions(
        &mut self,
        actions: &[SignalAction],
    ) -> Result<(), Error> {
        check_status(unsafe {
            ffi::tigt_terminal_install_signal_handlers_with_actions(actions.as_ptr(), actions.len())
        })?;
        self.signals_installed = true;
        Ok(())
    }

    pub fn uninstall_signal_handlers(&mut self) {
        if self.signals_installed {
            unsafe { ffi::tigt_terminal_uninstall_signal_handlers() };
            self.signals_installed = false;
        }
    }
}

impl Drop for Terminal<'_> {
    fn drop(&mut self) {
        release();
        unsafe { ffi::tigt_terminal_forget() };
        self.uninstall_signal_handlers();
    }
}
