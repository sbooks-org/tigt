// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

//! Synchronous, output-only glass-TTY and adaptive text presentation.
//!
//! Unlike [`crate::Session`], this API does not use curses, read input, change
//! termios, install signal handlers, or enter the alternate screen. Submit every
//! guest vsync, including unchanged frames, with the independent logical cursor.
//! Serialize writes to the destination and its duplicates; never share it with a
//! live curses session. The borrowed descriptor must outlive the presenter.

use crate::TextCell;
use std::{
    ffi::{c_int, c_void},
    fmt,
    os::fd::{AsRawFd, BorrowedFd},
    ptr::{self, NonNull},
};

pub const ABI_VERSION: u32 = 1;
/// Presenter-only flag; not accepted by the curses text API.
pub const TEXT_BOLD: u32 = 1 << 2;
/// Presenter-only flag; glass-TTY treats reverse video as bold overprinting.
pub const TEXT_REVERSE: u32 = 1 << 3;

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
    /// Allow a guest clear followed by representable text to restore glass mode.
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

/// A snapshot borrowed only for the synchronous call to [`Presenter::present`].
///
/// `stride` counts cells, not bytes. Padding after the final row is unnecessary.
/// The wrapper checks geometry and slice bounds; C validates cell contents.
/// Colors and flags use the existing [`TextCell`] layout without a staging copy.
#[derive(Clone, Copy, Debug)]
pub struct Frame<'a> {
    pub cells: &'a [TextCell],
    pub columns: u16,
    pub rows: u16,
    pub stride: u16,
    pub cursor: Cursor,
    pub refresh_rate: RefreshRate,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    Glass,
    /// Unrepresentable update awaiting confirmation; continue every vsync.
    Pending,
    /// Adaptive presentation is using the normal-screen clipped region.
    Fullscreen,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Argument,
    /// Adaptive mode requires a terminal destination.
    Terminal,
    Busy,
    /// Allocation or I/O failure. I/O failures are terminal until reset.
    System,
    /// Confirmed glass-TTY failure (ABORT); sticky until an explicit reset.
    Unrepresentable,
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
            Self::System => f.write_str("a tigt presenter system operation failed"),
            Self::Unrepresentable => {
                f.write_str("guest text cannot be represented as glass-TTY output")
            }
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

unsafe extern "C" {
    fn tigt_presenter_create(config: *const RawConfig, output: *mut *mut c_void) -> c_int;
    fn tigt_presenter_present(presenter: *mut c_void, frame: *const RawFrame) -> c_int;
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
/// other handles must still be serialized by the application. Blocking output
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
    /// [`Error::Unrepresentable`]; adaptive mode instead returns Fullscreen.
    /// The wrapper supplies zero reserved frame hints. Use [`Self::notify`] for
    /// speculative, ordered output expectations. Only matching screen evidence
    /// can confirm their soft-wrap annotations.
    pub fn present(&mut self, frame: Frame<'_>) -> Result<Status, Error> {
        crate::validate_text(frame.cells, frame.columns, frame.rows, frame.stride)
            .map_err(|_| Error::Argument)?;
        if frame.cursor.column >= frame.columns || frame.cursor.row >= frame.rows {
            return Err(Error::Argument);
        }
        let raw_frame = RawFrame {
            cells: frame.cells.as_ptr(),
            columns: frame.columns,
            rows: frame.rows,
            stride: frame.stride,
            cursor_column: frame.cursor.column,
            cursor_row: frame.cursor.row,
            refresh_hz: frame.refresh_rate as u16,
            hints: 0,
        };
        // Bounds above cover every visible cell. repr(C) TextCell is shared
        // directly with C, which retains neither the slice nor this descriptor.
        match unsafe { tigt_presenter_present(self.raw.as_ptr(), &raw_frame) } {
            0 => Ok(Status::Glass),
            1 => Ok(Status::Pending),
            2 => Ok(Status::Fullscreen),
            other => Err(Error::from_status(other)),
        }
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
    /// Emits nothing. The consumer must first prepare the destination; this is
    /// not automatic recovery or restoration of terminal contents.
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
