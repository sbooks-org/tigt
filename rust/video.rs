// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

//! Register/VRAM decoding without a terminal session or locale requirement.
//!
//! Only the MDA and CGA-compatible display view is modeled: text width and start
//! address, mode control, and CGA color selection. Timing, cursor registers, and
//! native PCjr extensions are ignored. The caller supplies the selected VRAM bank.

use crate::{Error, Session, TextCell, check_status, ffi};
use std::{ffi::c_void, mem::MaybeUninit, ptr::NonNull, slice};

/// The register and memory aperture interpreted by a decoder.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AdapterKind {
    Mda = 0,
    Cga = 1,
    /// A CGA-compatible view of the caller-selected 16 KiB PCjr bank.
    Pcjr = 2,
}

/// A contiguous decoded frame borrowed from its adapter, not from source VRAM.
///
/// Colors are `0x00RRGGBB`. Blink is already resolved and no cursor is added.
/// The adapter cannot be mutated or destroyed while this frame is in use.
#[derive(Debug)]
pub enum Frame<'a> {
    Text {
        cells: &'a [TextCell],
        columns: u16,
        rows: u16,
    },
    Bitmap {
        pixels: &'a [u32],
        width: u16,
        height: u16,
    },
}

/// Owns register state and a reusable decode buffer independently of a session.
///
/// New adapters start in 80-column text mode with video and blink disabled.
/// All access is serialized through mutable borrows.
pub struct VideoAdapter {
    raw: NonNull<c_void>,
}

impl VideoAdapter {
    pub fn new(kind: AdapterKind) -> Result<Self, Error> {
        // Every AdapterKind is valid; creation can fail only on allocation.
        let raw = unsafe { ffi::tigt_video_create(kind as u32) };
        Ok(Self {
            raw: NonNull::new(raw).ok_or(Error::System)?,
        })
    }

    /// Writes a hardware port. Unmodeled ports and register indices are ignored.
    pub fn write(&mut self, port: u16, value: u8) {
        unsafe { ffi::tigt_video_write(self.raw.as_ptr(), port, value) };
    }

    /// Decodes a complete aperture: at least 4 KiB for MDA or 16 KiB for CGA/PCjr.
    /// Extra bytes are ignored. Unsupported widths return [`Error::Argument`].
    ///
    /// Source VRAM is read only during this call; the returned slices borrow the
    /// adapter's reusable output buffer without copying it. `blink_on` selects
    /// the visible phase when hardware blink is enabled.
    pub fn decode(&mut self, vram: &[u8], blink_on: bool) -> Result<Frame<'_>, Error> {
        let mut frame = MaybeUninit::<ffi::VideoFrame>::uninit();
        // C checks the aperture length before reading and initializes frame on
        // success. Neither VRAM nor the output descriptor is retained by C.
        check_status(unsafe {
            ffi::tigt_video_decode(
                self.raw.as_ptr(),
                vram.as_ptr(),
                vram.len(),
                i32::from(blink_on),
                frame.as_mut_ptr(),
            )
        })?;
        let frame = unsafe { frame.assume_init() };
        let length = usize::from(frame.width) * usize::from(frame.height);
        // Successful decode supplies a non-null, aligned, initialized buffer
        // of width * height elements for the indicated kind. Its fixed maximum
        // dimensions fit isize. &mut self keeps it alive and prevents reuse.
        Ok(match frame.kind {
            0 => Frame::Text {
                cells: unsafe { slice::from_raw_parts(frame.cells, length) },
                columns: frame.width,
                rows: frame.height,
            },
            1 => Frame::Bitmap {
                pixels: unsafe { slice::from_raw_parts(frame.pixels, length) },
                width: frame.width,
                height: frame.height,
            },
            _ => unreachable!("tigt video decoder returned an invalid frame kind"),
        })
    }

    /// Decodes and submits to an active session, selecting MDA or generic display
    /// policy as appropriate. Bitmap pixels have width 1; overscan is unchanged.
    /// Session lifecycle and input callback errors propagate to the caller.
    pub fn present(&mut self, session: &Session, vram: &[u8], blink_on: bool) -> Result<(), Error> {
        session.input_status()?;
        check_status(unsafe {
            ffi::tigt_video_present(
                self.raw.as_ptr(),
                vram.as_ptr(),
                vram.len(),
                i32::from(blink_on),
            )
        })
    }
}

impl Drop for VideoAdapter {
    fn drop(&mut self) {
        unsafe { ffi::tigt_video_destroy(self.raw.as_ptr()) };
    }
}
