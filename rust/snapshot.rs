// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

use crate::{Error, Session, check_status, ffi};
use std::{
    ffi::CString,
    os::{
        fd::{AsRawFd, BorrowedFd},
        unix::ffi::OsStrExt,
    },
    path::Path,
    ptr,
};

/// Encoding of the last submitted frame, never a terminal-screen approximation.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnapshotFormat {
    /// Original RGB samples; text needs a caller-supplied CP437 bitmap font.
    Png = 0,
    /// Unicode text, with trailing spaces removed from each row.
    Utf8 = 1,
    /// Text projected to ASCII, replacing unsupported characters with `?`.
    Ascii = 2,
    /// Text projected to CP437, replacing unsupported characters with `?`.
    Cp437 = 3,
    /// Truecolor SGR text with underline, trimmed trailing spaces and row resets.
    Ansi = 4,
    /// Lossy CP437/nearest-IBM16 glyph-attribute byte pairs in row-major order.
    Cells = 5,
    /// Versioned, lossless JSON attributes plus the explicit legacy projection.
    Attributes = 6,
}

/// Only the two application-request signals supported by C can be selected.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnapshotSignal {
    Usr1,
    Usr2,
}

impl SnapshotSignal {
    fn number(self) -> i32 {
        match self {
            Self::Usr1 => libc::SIGUSR1,
            Self::Usr2 => libc::SIGUSR2,
        }
    }
}

/// An opt-in capture destination. C copies the path before returning.
///
/// Destinations are regular files or named FIFOs. FIFOs are nonblocking and
/// bounded; no reader or insufficient capacity is reported in snapshot status.
/// Existing application signal handlers are not replaced.
#[derive(Clone, Copy, Debug)]
pub struct SnapshotConfig<'a> {
    pub signal: SnapshotSignal,
    pub format: SnapshotFormat,
    pub path: &'a Path,
}

/// Last asynchronous completion, including failed requests.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SnapshotStatus {
    /// Zero means no asynchronous request has completed in this session.
    pub sequence: u64,
    pub result: Result<(), Error>,
}

impl Session {
    /// Synchronously writes the last submitted frame to a borrowed descriptor.
    ///
    /// The descriptor is never closed or retained. The caller controls its
    /// position and blocking mode; ordinary blocking descriptor writes may block.
    /// Serialize access to this descriptor and its duplicates until return.
    /// On macOS, SIGPIPE suppression temporarily changes the shared open-file
    /// description and is restored afterwards, without changing signal disposition.
    /// No frame returns [`Error::Busy`]. Text PNG without a font, unmappable PNG
    /// glyphs, and text-only encodings of a bitmap return [`Error::Argument`].
    pub fn snapshot_write_fd(
        &self,
        fd: BorrowedFd<'_>,
        format: SnapshotFormat,
    ) -> Result<(), Error> {
        self.input_status()?;
        check_status(unsafe { ffi::tigt_snapshot_write_fd(fd.as_raw_fd(), format as u32) })
    }

    /// Enables signal-triggered snapshots, or disables them with `None`.
    ///
    /// The signal handler only requests work; I/O runs outside signal context.
    /// Inspect [`Self::snapshot_status`] to distinguish completion from failure.
    /// Disabling and session drop restore the disposition owned by this feature.
    /// Suspension preserves configuration; requests while suspended report
    /// [`Error::Busy`], and resume makes subsequent requests available again.
    /// Disabling remains available even after an input callback has panicked.
    pub fn configure_snapshot(&self, config: Option<SnapshotConfig<'_>>) -> Result<(), Error> {
        if let Some(config) = config {
            self.input_status()?;
            let path =
                CString::new(config.path.as_os_str().as_bytes()).map_err(|_| Error::Argument)?;
            check_status(unsafe {
                ffi::tigt_snapshot_configure(
                    config.signal.number(),
                    config.format as u32,
                    path.as_ptr(),
                )
            })
        } else {
            check_status(unsafe { ffi::tigt_snapshot_configure(0, 0, ptr::null()) })
        }
    }

    /// Copies 256 width-eight CP437 glyphs, each `height` bytes high.
    ///
    /// Height must be 1..=32 and the slice must contain exactly `256 * height`
    /// bytes. Bit 7 is the left pixel. No font or ROM is bundled with tigt.
    pub fn set_snapshot_font(&self, font: &[u8], height: u16) -> Result<(), Error> {
        self.input_status()?;
        if !(1..=32).contains(&height) || font.len() != 256 * usize::from(height) {
            return Err(Error::Argument);
        }
        check_status(unsafe { ffi::tigt_snapshot_set_font(font.as_ptr(), height) })
    }

    /// Removes the copied font. Bitmap PNG snapshots do not need a font.
    pub fn clear_snapshot_font(&self) -> Result<(), Error> {
        self.input_status()?;
        check_status(unsafe { ffi::tigt_snapshot_set_font(ptr::null(), 0) })
    }

    /// Reads the last asynchronous completion without consuming it.
    ///
    /// The outer result reports failure to query status; the inner result is the
    /// actual snapshot result. A successful read does not imply successful I/O.
    pub fn snapshot_status(&self) -> Result<SnapshotStatus, Error> {
        let mut sequence = 0;
        let mut result = 0;
        check_status(unsafe { ffi::tigt_snapshot_status(&mut sequence, &mut result) })?;
        Ok(SnapshotStatus {
            sequence,
            result: check_status(result),
        })
    }
}
