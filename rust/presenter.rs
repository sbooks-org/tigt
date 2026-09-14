// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

//! Output-only glass-TTY and adaptive text presentation.
//!
//! Unlike [`crate::Session`], this API does not use curses, read input, change
//! termios, install signal handlers, or enter the alternate screen. Submit every
//! guest vsync, including unchanged frames, with the independent logical cursor.
//! Serialize writes to the destination and its duplicates; never share it with a
//! live curses session. The borrowed descriptor must outlive the presenter.
//! Use [`Presenter::present_nonblocking`] and [`Presenter::resume`] with an
//! `O_NONBLOCK` descriptor to service host control between bounded write attempts.
//! Background TTY output is forced to nonadaptive glass: never request a cursor
//! report or switch input modes/protocols until foreground ownership returns.
//! Use [`crate::terminal::Terminal`] for shared input and signal lifecycle policy.

use crate::TextCell;
use std::{
    ffi::{c_int, c_uint, c_void},
    fmt,
    os::fd::{AsRawFd, BorrowedFd},
    ptr::{self, NonNull},
};

pub const ABI_VERSION: u32 = 1;
/// Maximum decoded scalars in bounded already-displayed local-echo accounting.
pub const LOCAL_ECHO_MAX: usize = 4096;
/// Presenter-only flag; not accepted by the curses text API.
pub const TEXT_BOLD: u32 = 1 << 2;
/// Presenter-only flag; glass-TTY treats reverse video as bold overprinting.
pub const TEXT_REVERSE: u32 = 1 << 3;
/// Frame hint: hardware output is disabled, not merely displaying blank cells.
/// Supply underlying unblanked text in [`Frame::cells`]; tigt masks disabled output.
pub const VIDEO_DISABLED: u32 = 1 << 0;
/// Frame hint: any raw VRAM byte changed since the last submitted snapshot.
/// Include attributes/offscreen memory and set this for the initial snapshot.
/// Comparisons during skipped submissions must not consume this notification.
pub const VIDEO_MEMORY_CHANGED: u32 = 1 << 1;

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    /// Stream representable text; report confirmed failure to the consumer.
    Glass = 0,
    /// Start in glass mode, falling back to a normal-screen region on a TTY.
    Adaptive = 1,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Encoding {
    /// Use LC_ALL, LC_CTYPE, then LANG; non-UTF-8 locales degrade to ASCII.
    Locale = 0,
    Utf8 = 1,
    /// Printable seven-bit ASCII, with unsupported glyphs degraded by C.
    Ascii = 2,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Reversibility {
    /// Stay full-screen after adaptive fallback until an explicit reset.
    OneWay = 0,
    /// Resume at the retained cursor after sequential row advancement and text
    /// (or clear and text), confirmed for 100 ms at the text frontier.
    /// Recovery neither clears the region nor replays its contents.
    Reversible = 1,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Config {
    pub mode: Mode,
    pub encoding: Encoding,
    /// Controls adaptive fallback only; glass mode never falls back.
    pub reversibility: Reversibility,
}

/// Guest vsync cadence, not a host-side timer or sleep request.
#[repr(u16)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RefreshRate {
    Hz50 = 50,
    Hz60 = 60,
    /// Reserves the seven-frame confirmation cadence; does not implement VGA.
    Hz70 = 70,
}

/// Zero-based guest cursor, independent of visibility and TEXT_CURSOR flags.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Cursor {
    pub column: u16,
    pub row: u16,
}

/// A snapshot borrowed only for a submission call, including nonblocking calls.
///
/// `stride` counts cells, not bytes. Padding after the final row is unnecessary.
/// The wrapper checks geometry and slice bounds; C validates cell contents.
/// Colors and flags use the existing [`TextCell`] layout. C retains its validated
/// candidate image, never this slice, while a nonblocking transaction is pending.
#[derive(Clone, Copy, Debug)]
pub struct Frame<'a> {
    /// Underlying decoded text, with hardware blanking bypassed even when
    /// [`VIDEO_DISABLED`] is set. Disabled raw cells are inspected, never painted.
    pub cells: &'a [TextCell],
    pub columns: u16,
    pub rows: u16,
    pub stride: u16,
    pub cursor: Cursor,
    pub refresh_rate: RefreshRate,
    /// Zero for ordinary output, or a combination of [`VIDEO_DISABLED`] and
    /// [`VIDEO_MEMORY_CHANGED`]. Unknown bits are rejected.
    pub hints: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    Glass,
    /// Confirmation, scroll, or disable hold; keep submitting without changing input mode.
    Pending,
    /// Adaptive presentation is using the normal-screen clipped region.
    Fullscreen,
    /// No output was written: observe the current host cursor, then resume vsyncs.
    /// This is not an active full-screen transition.
    NeedsCursor,
}

impl Status {
    fn from_status(status: c_int) -> Result<Self, Error> {
        match status {
            0 => Ok(Self::Glass),
            1 => Ok(Self::Pending),
            2 => Ok(Self::Fullscreen),
            5 => Ok(Self::NeedsCursor),
            other => Err(Error::from_status(other)),
        }
    }
}

/// Completion of one submitted vsync, or output that still needs draining.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Progress {
    Complete(Status),
    /// Call [`Presenter::resume`] without resubmitting the frame.
    /// Includes short writes and interrupted writes, not only `EAGAIN`.
    WouldBlock,
}

impl Progress {
    fn from_status(status: c_int) -> Result<Self, Error> {
        if status == 4 {
            Ok(Self::WouldBlock)
        } else {
            Status::from_status(status).map(Self::Complete)
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Argument,
    /// Adaptive mode requires a terminal destination.
    Terminal,
    Busy,
    /// A synchronous submission encountered backpressure; its transaction is
    /// retained. Set `O_NONBLOCK` and use [`Presenter::resume`] to finish it.
    WouldBlock,
    /// Allocation or hard I/O failure. Hard I/O failures are terminal until reset.
    System,
    /// Confirmed glass-TTY failure (ABORT); sticky until an explicit reset.
    Unrepresentable,
    /// A pending full-screen transaction lost foreground terminal ownership.
    Background,
    /// The host operation is unavailable on this platform.
    Unsupported,
    /// An unrecognized C status, retained without discarding its numeric value.
    UnexpectedStatus(i32),
}

impl Error {
    fn from_status(status: c_int) -> Self {
        match status {
            -1 => Self::Argument,
            -2 => Self::Terminal,
            -3 => Self::Busy,
            -4 => Self::System,
            -5 => Self::Unrepresentable,
            -6 => Self::Background,
            -7 => Self::Unsupported,
            4 => Self::WouldBlock,
            other => Self::UnexpectedStatus(other),
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Argument => f.write_str("invalid tigt presenter argument or frame buffer"),
            Self::Terminal => f.write_str("tigt adaptive presentation requires a terminal"),
            Self::Busy => f.write_str("the tigt presenter is unavailable"),
            Self::WouldBlock => f.write_str("tigt presenter output needs resuming"),
            Self::System => f.write_str("a tigt presenter system operation failed"),
            Self::Unrepresentable => {
                f.write_str("guest text cannot be represented as glass-TTY output")
            }
            Self::Background => {
                f.write_str("tigt full-screen output requires the foreground terminal")
            }
            Self::Unsupported => f.write_str("the tigt presenter host operation is unsupported"),
            Self::UnexpectedStatus(status) => {
                write!(f, "unexpected tigt presenter status {status}")
            }
        }
    }
}

impl std::error::Error for Error {}

/// Boundary after `text_offset` decoded display scalars in an expectation.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BoundaryKind {
    SoftWrap = 1,
    Newline = 2,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Boundary {
    pub text_offset: usize,
    pub kind: BoundaryKind,
}

/// Speculative output, copied by [`Presenter::notify`], never emitted directly.
///
/// Text contains guest-decoded display scalars, not output-encoded bytes or
/// control characters. Expand tabs and describe CR/backspace writes as separate
/// anchored operations. Boundaries are ordered offsets after display scalars.
#[derive(Clone, Copy, Debug)]
pub struct Notification<'a> {
    pub operation_id: u64,
    pub text: &'a [u32],
    pub boundaries: &'a [Boundary],
    pub columns: u16,
    pub rows: u16,
    pub start: Cursor,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NotifyStatus {
    Accepted,
    /// Queue capacity was exceeded; speculation was discarded, not output.
    Dropped,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct NotificationStats {
    pub consumed: u64,
    pub discarded: u64,
    pub expired: u64,
    pub queued: u32,
}

#[repr(C)]
struct RawNotification {
    operation_id: u64,
    text: *const u32,
    text_length: usize,
    boundaries: *const Boundary,
    boundary_count: usize,
    columns: u16,
    rows: u16,
    start_column: u16,
    start_row: u16,
}

#[repr(C)]
struct RawConfig {
    abi_version: u32,
    output_fd: c_int,
    mode: u32,
    encoding: u32,
    reversible: u32,
}

#[repr(C)]
struct RawFrame {
    cells: *const TextCell,
    columns: u16,
    rows: u16,
    stride: u16,
    cursor_column: u16,
    cursor_row: u16,
    refresh_hz: u16,
    hints: u32,
}

impl Frame<'_> {
    fn raw(self) -> Result<RawFrame, Error> {
        crate::validate_text(self.cells, self.columns, self.rows, self.stride)
            .map_err(|_| Error::Argument)?;
        if self.cursor.column >= self.columns || self.cursor.row >= self.rows {
            return Err(Error::Argument);
        }
        Ok(RawFrame {
            cells: self.cells.as_ptr(),
            columns: self.columns,
            rows: self.rows,
            stride: self.stride,
            cursor_column: self.cursor.column,
            cursor_row: self.cursor.row,
            refresh_hz: self.refresh_rate as u16,
            hints: self.hints,
        })
    }
}

unsafe extern "C" {
    fn tigt_presenter_create(config: *const RawConfig, output: *mut *mut c_void) -> c_int;
    fn tigt_presenter_present(presenter: *mut c_void, frame: *const RawFrame) -> c_int;
    fn tigt_presenter_observe_cursor(presenter: *mut c_void, column: c_uint, row: c_uint) -> c_int;
    fn tigt_presenter_forget_cursor(presenter: *mut c_void) -> c_int;
    fn tigt_presenter_local_echo(presenter: *mut c_void, text: *const u32, length: usize) -> c_int;
    fn tigt_presenter_present_nonblocking(presenter: *mut c_void, frame: *const RawFrame) -> c_int;
    fn tigt_presenter_resume(presenter: *mut c_void) -> c_int;
    fn tigt_presenter_fullscreen_output_started(presenter: *const c_void) -> c_int;
    fn tigt_presenter_reset(presenter: *mut c_void) -> c_int;
    fn tigt_presenter_destroy(presenter: *mut c_void);
    fn tigt_presenter_notify(presenter: *mut c_void, notification: *const RawNotification)
    -> c_int;
    fn tigt_presenter_cancel(presenter: *mut c_void, operation_id: u64) -> c_int;
    fn tigt_presenter_get_notification_stats(
        presenter: *const c_void,
        stats: *mut NotificationStats,
    ) -> c_int;
}

/// Owns presentation state, but never owns or closes its output descriptor.
///
/// Mutable borrows serialize all operations. Like the existing video adapter,
/// the opaque pointer makes this type neither Send nor Sync. The fd borrow
/// prevents closing its owner while the presenter remains in use; writes through
/// other handles must still be serialized by the application. Synchronous output
/// can block a call. Dropping or resetting does not clear emitted output.
/// On macOS, SIGPIPE suppression temporarily changes the borrowed open-file
/// description's no-SIGPIPE flag and restores it before returning, so the
/// serialization requirement also covers duplicate descriptors.
pub struct Presenter<'fd> {
    raw: NonNull<c_void>,
    _output: BorrowedFd<'fd>,
}

impl<'fd> Presenter<'fd> {
    /// Creates an empty glass baseline without acquiring a curses session.
    ///
    /// Prepare the destination before starting. Adaptive mode rejects non-TTY
    /// descriptors; glass mode also supports redirected output and ignores host
    /// geometry. No descriptor duplication or ownership transfer occurs.
    pub fn new(output: BorrowedFd<'fd>, config: Config) -> Result<Self, Error> {
        let raw_config = RawConfig {
            abi_version: ABI_VERSION,
            output_fd: output.as_raw_fd(),
            mode: config.mode as u32,
            encoding: config.encoding as u32,
            reversible: config.reversibility as u32,
        };
        let mut raw = ptr::null_mut();
        // C copies configuration and returns its owned opaque allocation through
        // the out-pointer. The retained BorrowedFd protects the stored integer.
        let status = unsafe { tigt_presenter_create(&raw_config, &mut raw) };
        if status != 0 {
            return Err(Error::from_status(status));
        }
        Ok(Self {
            raw: NonNull::new(raw).ok_or(Error::System)?,
            _output: output,
        })
    }

    /// Submits one vsync, including when cells and cursor are unchanged.
    ///
    /// Cursor-up alone is not failure. A genuinely unrepresentable update yields
    /// Pending for five/six/seven elapsed vsync intervals at 50/60/70 Hz
    /// (100 ms; six/seven/eight bad observations including first detection).
    /// Recovery cancels confirmation. Confirmed glass failure returns
    /// [`Error::Unrepresentable`]; adaptive mode returns [`Status::NeedsCursor`]
    /// without writing if it needs a fresh host position, or [`Status::Fullscreen`]
    /// once fallback is active. On NeedsCursor, obtain a host DSR response, call
    /// [`Self::observe_cursor`], and resubmit at the next guest vsync.
    /// Recognized upward copies (including a partial row, an already-shifted
    /// untouched suffix, or uncleared exposed rows) retain the committed image,
    /// cursor, echo and logical mapping for at most 500 ms from first detection.
    /// Progress and identical observations never renew that deadline. Coherent,
    /// uniquely aligned completion commits once; ambiguous repeated rows cannot
    /// invent scrollback. Timeout takes ordinary glass error/adaptive fallback.
    /// The hold applies in glass and fullscreen, without aging recovery evidence.
    /// Notifications still age on every valid vsync.
    ///
    /// [`VIDEO_DISABLED`] ordinarily holds the previous presentation for 200 ms:
    /// ten 50 Hz or twelve 60 Hz submissions, counting the first disabled frame.
    /// [`VIDEO_MEMORY_CHANGED`] releases that ordinary hold for the remainder of
    /// the disable interval. Exception: recognized scrolling, including a
    /// completed copy while still disabled, uses the same 500 ms scroll deadline.
    /// Other disabled memory changes show hardware black immediately. Underlying
    /// raw cells are never painted while disabled. Reenable/reset starts a fresh
    /// ordinary disable interval; enabled blanks are never held. These intervals
    /// are independent of the ordinary 100 ms confirmation/recovery.
    /// Use [`Self::notify`] for speculative, ordered output expectations.
    /// Only matching screen evidence can confirm their soft-wrap annotations.
    pub fn present(&mut self, frame: Frame<'_>) -> Result<Status, Error> {
        let raw_frame = frame.raw()?;
        // Bounds above cover every visible cell. C retains its validated image,
        // not the caller's slice or this descriptor.
        Status::from_status(unsafe { tigt_presenter_present(self.raw.as_ptr(), &raw_frame) })
    }

    /// Records the current one-based host cursor after all preceding output.
    ///
    /// Neither reads nor writes. Zero coordinates return [`Error::Argument`].
    /// The consumer obtains the actual position (for example, from a DSR reply),
    /// demultiplexing terminal replies from user input. The presenter tracks its
    /// own subsequent output and clears any pending [`Status::NeedsCursor`].
    /// Returns [`Error::Busy`] without mutation while output is pending.
    pub fn observe_cursor(&mut self, column: u32, row: u32) -> Result<(), Error> {
        match unsafe { tigt_presenter_observe_cursor(self.raw.as_ptr(), column, row) } {
            0 => Ok(()),
            other => Err(Error::from_status(other)),
        }
    }

    /// Invalidates host position after external output or terminal resume.
    ///
    /// Emits nothing. Adaptive fallback requests a fresh observation when needed;
    /// call this before fallback if other output may have moved the host cursor.
    /// Returns [`Error::Busy`] without mutation while output is pending.
    pub fn forget_cursor(&mut self) -> Result<(), Error> {
        match unsafe { tigt_presenter_forget_cursor(self.raw.as_ptr()) } {
            0 => Ok(()),
            other => Err(Error::from_status(other)),
        }
    }

    /// Records a finalized host-edited line that the terminal already displayed.
    ///
    /// Call before delivering any of its keys to the guest, and only when input
    /// and output refer to the same echoing TTY; never register piped input.
    /// Fold cooked editing into the final decoded single-cell Unicode scalars,
    /// allowing TAB and the already-displayed LF, not raw editing keystrokes.
    ///
    /// Unlike [`Self::notify`], this accounts for actual output. Confirmed guest
    /// glyphs, newlines and soft wraps consume that accounting without duplicate
    /// output. A mismatch fails representability rather than replaying host echo.
    /// Invalid input returns [`Error::Argument`]; exceeding the bounded
    /// [`LOCAL_ECHO_MAX`] capacity returns [`Error::Unrepresentable`].
    /// C copies the slice before returning and emits nothing. This invalidates
    /// the host cursor observation because local echo has moved it ahead.
    /// Returns [`Error::Busy`] without mutation while output is pending.
    pub fn local_echo(&mut self, text: &[u32]) -> Result<(), Error> {
        // The slice remains live for this call; C validates and copies its data.
        match unsafe { tigt_presenter_local_echo(self.raw.as_ptr(), text.as_ptr(), text.len()) } {
            0 => Ok(()),
            other => Err(Error::from_status(other)),
        }
    }

    /// Submits one vsync with at most one write syscall and no waiting.
    ///
    /// Requires caller-set `O_NONBLOCK`; a blocking descriptor is rejected before
    /// accepting the frame. [`Progress::WouldBlock`] retains exact unwritten bytes
    /// and the candidate frame. Service host control and, if appropriate, wait
    /// for writability before calling [`Self::resume`]. Do not resubmit the frame.
    /// Short writes and `EINTR` also yield; no output is duplicated on resumption.
    ///
    /// Only one bounded transaction exists. New submissions, [`Self::notify`],
    /// [`Self::cancel`], [`Self::observe_cursor`], [`Self::forget_cursor`], and
    /// [`Self::local_echo`] return [`Error::Busy`] until it completes or is reset.
    /// The input slice may be reused as soon as this call returns.
    ///
    /// POSIX may ignore `O_NONBLOCK` on regular files, whose writes can still
    /// block in the kernel. Pipes and terminals support nonblocking output.
    pub fn present_nonblocking(&mut self, frame: Frame<'_>) -> Result<Progress, Error> {
        let raw_frame = frame.raw()?;
        Progress::from_status(unsafe {
            tigt_presenter_present_nonblocking(self.raw.as_ptr(), &raw_frame)
        })
    }

    /// Attempts at most one nonblocking write of the pending transaction.
    ///
    /// Requires `O_NONBLOCK`. Does not advance guest vsync time or reconsider
    /// notifications. Completion commits the frame and notification decisions
    /// once; its status then describes the new presentation mode.
    /// Returns [`Error::Busy`] when there is no pending output. Suspension or
    /// cancellation can be serviced between attempts without entering C again.
    pub fn resume(&mut self) -> Result<Progress, Error> {
        Progress::from_status(unsafe { tigt_presenter_resume(self.raw.as_ptr()) })
    }

    /// Whether fullscreen output is committed or has begun emitting bytes.
    ///
    /// Read after submission/resumption, including on errors, before resetting
    /// to decide whether terminal restoration controls are needed. Ordinary
    /// glass output and blocked fullscreen entry with no emitted bytes return
    /// false. This is current state, not a historical latch: it remains true
    /// during pending glass recovery, then clears on completion or reset.
    /// Performs no I/O; callers may publish the result for emergency recovery.
    pub fn fullscreen_output_started(&self) -> bool {
        unsafe { tigt_presenter_fullscreen_output_started(self.raw.as_ptr()) != 0 }
    }

    /// Queues an expectation without emitting text or retaining its slices.
    ///
    /// Serialize notifications with frame submissions. Repeated operation IDs
    /// are deduplicated; observe one semantic source or share IDs when observing
    /// both a DOS call and its nested BIOS writes.
    pub fn notify(&mut self, notification: Notification<'_>) -> Result<NotifyStatus, Error> {
        let raw = RawNotification {
            operation_id: notification.operation_id,
            text: notification.text.as_ptr(),
            text_length: notification.text.len(),
            boundaries: notification.boundaries.as_ptr(),
            boundary_count: notification.boundaries.len(),
            columns: notification.columns,
            rows: notification.rows,
            start_column: notification.start.column,
            start_row: notification.start.row,
        };
        // repr(C) boundary storage and both slices remain live for this call.
        // C validates and copies the payload before returning.
        match unsafe { tigt_presenter_notify(self.raw.as_ptr(), &raw) } {
            0 => Ok(NotifyStatus::Accepted),
            3 => Ok(NotifyStatus::Dropped),
            other => Err(Error::from_status(other)),
        }
    }

    /// Withdraws an operation's remaining prediction without changing output.
    pub fn cancel(&mut self, operation_id: u64) -> Result<(), Error> {
        match unsafe { tigt_presenter_cancel(self.raw.as_ptr(), operation_id) } {
            0 => Ok(()),
            other => Err(Error::from_status(other)),
        }
    }

    pub fn notification_stats(&self) -> Result<NotificationStats, Error> {
        let mut stats = NotificationStats::default();
        // C writes the repr(C) counters; no pointers are retained.
        match unsafe { tigt_presenter_get_notification_stats(self.raw.as_ptr(), &mut stats) } {
            0 => Ok(stats),
            other => Err(Error::from_status(other)),
        }
    }

    /// Starts a new empty glass baseline, clearing sticky failures.
    ///
    /// Emits nothing and immediately discards any pending output transaction.
    /// The consumer must first prepare the destination, including recovery from
    /// partially emitted escape sequences; this does not roll back emitted bytes
    /// or automatically restore terminal contents. Also discards local-echo
    /// accounting and invalidates the host cursor observation.
    pub fn reset(&mut self) -> Result<(), Error> {
        match unsafe { tigt_presenter_reset(self.raw.as_ptr()) } {
            0 => Ok(()),
            other => Err(Error::from_status(other)),
        }
    }
}

impl Drop for Presenter<'_> {
    fn drop(&mut self) {
        // This allocation is exclusively owned. Destruction never closes the fd.
        unsafe { tigt_presenter_destroy(self.raw.as_ptr()) };
    }
}
