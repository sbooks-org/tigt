// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

//! tty-in-glass-tty: the same C renderer and incremental input decoder used by
//! native consumers, with owned sessions and checked Rust slices.
//!
//! [`Session`] owns the process-wide curses session on its creating thread and
//! restores terminal state on drop. It is neither Send nor Sync; do not operate
//! curses or the tigt C lifecycle independently while it is alive. Frames are
//! copied by C before submission returns, so their slices need not be retained.
//! Input decoding is independent of rendering and of any keyboard mapper.
//! Enable the optional `keyboard` feature for conversions to `pc-xt-keyboard`.

mod ffi;
mod input;
#[cfg(feature = "keyboard")]
pub mod keyboard;

pub use input::{InputDecoder, InputEvent, InputKey, InputKind, ModifierKey, Modifiers};

use input::{CallbackHandle, dispatch};
use std::{
    fmt,
    marker::PhantomData,
    ptr,
    rc::Rc,
    sync::atomic::{AtomicBool, Ordering},
};

pub const ABI_VERSION: u32 = 1;
pub const MDA_VRAM_SIZE: usize = 4096;
pub const CGA_VRAM_SIZE: usize = 16384;
pub const CRTC_SIZE: usize = 32;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// Invalid frame dimensions, stride, slice length, or C argument.
    Argument,
    /// The terminal could not be initialized or resumed.
    Terminal,
    /// A session is already owned, or the C operation cannot run in this state.
    Busy,
    /// An allocation, thread, or other system operation failed.
    System,
    /// The input handler panicked and has been permanently disabled.
    CallbackPanicked,
    /// The C decoder returned an event outside this binding's ABI.
    InvalidInputEvent,
    /// An unrecognized C status, retained for diagnosis.
    UnexpectedStatus(i32),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Argument => f.write_str("invalid tigt argument or frame buffer"),
            Self::Terminal => f.write_str("unable to initialize the terminal"),
            Self::Busy => f.write_str("the tigt session is unavailable or already owned"),
            Self::System => f.write_str("a tigt system operation failed"),
            Self::CallbackPanicked => f.write_str("the tigt input callback panicked"),
            Self::InvalidInputEvent => {
                f.write_str("the tigt decoder emitted an invalid input event")
            }
            Self::UnexpectedStatus(status) => write!(f, "unexpected tigt status {status}"),
        }
    }
}

impl std::error::Error for Error {}

fn check_status(status: i32) -> Result<(), Error> {
    match status {
        0 => Ok(()),
        -1 => Err(Error::Argument),
        -2 => Err(Error::Terminal),
        -3 => Err(Error::Busy),
        -4 => Err(Error::System),
        other => Err(Error::UnexpectedStatus(other)),
    }
}

static SESSION_OWNED: AtomicBool = AtomicBool::new(false);

struct SessionClaim;

impl SessionClaim {
    fn acquire() -> Result<Self, Error> {
        SESSION_OWNED
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .map(|_| Self)
            .map_err(|_| Error::Busy)
    }
}

impl Drop for SessionClaim {
    fn drop(&mut self) {
        SESSION_OWNED.store(false, Ordering::Release);
    }
}

type InputHandler = Box<dyn FnMut(InputEvent) + Send + 'static>;

/// Exclusive, owning-thread terminal session.
///
/// `new` is output-only; `with_input` calls a Send handler on C's input thread.
/// Input handlers cannot capture this non-Send session. They should send events
/// to the owning thread instead of invoking lifecycle operations. Ctrl+C and
/// Ctrl+Z arrive as semantic control characters: application policy decides
/// whether to quit or suspend. Signal dispositions remain application-owned.
///
/// Drop joins C's workers before releasing callback storage or the singleton
/// claim. As with all RAII resources, process abort/exit or `mem::forget` bypasses
/// normal restoration. A failed constructor never shuts down someone else's
/// C session.
pub struct Session {
    callback: Option<CallbackHandle<InputHandler>>,
    _claim: SessionClaim,
    _owner_thread: PhantomData<Rc<()>>,
}

impl Session {
    /// Opens an output-only session without creating an input worker.
    pub fn new() -> Result<Self, Error> {
        Self::start(SessionClaim::acquire()?, None)
    }

    /// Opens a session with a serialized input-thread callback.
    ///
    /// Panics are caught at the C boundary and permanently disable the callback.
    /// `input_status` and fallible session operations report the failure. With
    /// panic=abort the process still aborts; Rust panic hooks run normally.
    pub fn with_input<F>(handler: F) -> Result<Self, Error>
    where
        F: FnMut(InputEvent) + Send + 'static,
    {
        let claim = SessionClaim::acquire()?;
        Self::start(claim, Some(CallbackHandle::new(Box::new(handler))))
    }

    fn start(
        claim: SessionClaim,
        callback: Option<CallbackHandle<InputHandler>>,
    ) -> Result<Self, Error> {
        let (on_input, user) = match &callback {
            Some(state) => (
                Some(dispatch::<InputHandler> as ffi::InputCallback),
                state.user(),
            ),
            None => (None, ptr::null_mut()),
        };
        let config = ffi::Config {
            abi_version: ABI_VERSION,
            on_input,
            user,
        };
        // Config is copied by C; callback storage stays at its raw allocation.
        // Init failure stops callbacks before returning; claim then releases
        // automatically without calling shutdown on a foreign owner.
        check_status(unsafe { ffi::tigt_init(&config) })?;
        Ok(Self {
            callback,
            _claim: claim,
            _owner_thread: PhantomData,
        })
    }

    /// Reports asynchronous input-handler failure without touching curses.
    /// A successful result is a snapshot, not a promise about future callbacks.
    pub fn input_status(&self) -> Result<(), Error> {
        self.callback
            .as_ref()
            .map_or(Ok(()), |callback| callback.status())
    }

    /// Restores shell state and stops worker callbacks, retaining ownership.
    /// This remains available even after a callback panic.
    pub fn suspend(&mut self) {
        unsafe { ffi::tigt_suspend() };
    }

    /// Re-enters the terminal after suspension; failure is returned, not fatal.
    pub fn resume(&mut self) -> Result<(), Error> {
        self.input_status()?;
        check_status(unsafe { ffi::tigt_resume() })
    }

    /// Copies a 320x200 or 640x200 RGB frame. Colors are `0x00RRGGBB`;
    /// stride is in pixels and `pixel_width` is 1 or 2 backing pixels per
    /// logical terminal pixel. Padding after the last row is not required.
    pub fn present_bitmap(
        &self,
        pixels: &[u32],
        width: u16,
        height: u16,
        stride: u16,
        pixel_width: u8,
    ) -> Result<(), Error> {
        self.input_status()?;
        if !matches!(width, 320 | 640) || height != 200 || !matches!(pixel_width, 1 | 2) {
            return Err(Error::Argument);
        }
        validate_pixels(pixels, width as usize, height as usize, stride as usize, 0)?;
        // Dimensions and every read row are within pixels; C copies them now.
        check_status(unsafe {
            ffi::tigt_present_bitmap(pixels.as_ptr(), width, height, stride, pixel_width)
        })
    }

    /// Copies hardware-format MDA VRAM and CRTC registers. Slices must contain
    /// at least MDA_VRAM_SIZE and CRTC_SIZE bytes respectively.
    pub fn present_mda(&self, vram: &[u8], crtc: &[u8], mode: u8) -> Result<(), Error> {
        self.input_status()?;
        validate_text(vram, crtc, MDA_VRAM_SIZE)?;
        check_status(unsafe { ffi::tigt_present_mda(vram.as_ptr(), crtc.as_ptr(), mode) })
    }

    /// Copies CGA text state or an already-decoded RGB graphics frame.
    ///
    /// VRAM and CRTC slices must always contain CGA_VRAM_SIZE and CRTC_SIZE
    /// bytes. With mode bit 1 set and Some(pixels), C displays 640x200 pixels
    /// beginning at source_y, with stride >= 640; mode bit 4 chooses single
    /// rather than doubled logical pixels. Otherwise pixels/source_y/stride
    /// are unused and C displays the hardware text/blank transition state.
    pub fn present_cga(
        &self,
        vram: &[u8],
        crtc: &[u8],
        mode: u8,
        source_y: usize,
        pixels: Option<&[u32]>,
        stride: u16,
    ) -> Result<(), Error> {
        self.input_status()?;
        validate_text(vram, crtc, CGA_VRAM_SIZE)?;
        let pixels = if let Some(pixels) = pixels.filter(|_| mode & 2 != 0) {
            let offset = validate_pixels(pixels, 640, 200, stride as usize, source_y)?;
            // Slice first and pass source_y=0, avoiding C int-offset overflow.
            pixels[offset..].as_ptr()
        } else {
            ptr::null()
        };
        check_status(unsafe {
            ffi::tigt_present_cga(vram.as_ptr(), crtc.as_ptr(), mode, 0, pixels, stride)
        })
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        // Session is !Send/!Sync: lifecycle and submissions cannot race in safe
        // Rust. Shutdown joins both C workers before fields are destroyed.
        unsafe { ffi::tigt_shutdown() };
    }
}

fn validate_text(vram: &[u8], crtc: &[u8], vram_size: usize) -> Result<(), Error> {
    if vram.len() < vram_size || crtc.len() < CRTC_SIZE {
        Err(Error::Argument)
    } else {
        Ok(())
    }
}

fn validate_pixels(
    pixels: &[u32],
    width: usize,
    height: usize,
    stride: usize,
    source_y: usize,
) -> Result<usize, Error> {
    if stride < width || width == 0 || height == 0 {
        return Err(Error::Argument);
    }
    let offset = source_y.checked_mul(stride).ok_or(Error::Argument)?;
    let end = (height - 1)
        .checked_mul(stride)
        .and_then(|rows| offset.checked_add(rows))
        .and_then(|last| last.checked_add(width))
        .ok_or(Error::Argument)?;
    if pixels.len() < end {
        Err(Error::Argument)
    } else {
        Ok(offset)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn padded_frame_requires_last_row_pixels_but_not_last_row_padding() {
        let pixels = vec![0; 3 * 672 + 640];
        assert_eq!(validate_pixels(&pixels, 640, 2, 672, 2), Ok(1344));
        assert_eq!(
            validate_pixels(&pixels[..pixels.len() - 1], 640, 2, 672, 2),
            Err(Error::Argument)
        );
    }

    #[test]
    fn huge_source_offset_cannot_wrap_to_a_small_valid_buffer() {
        assert_eq!(
            validate_pixels(&[0; 640], 640, 200, 640, usize::MAX),
            Err(Error::Argument)
        );
    }
}
