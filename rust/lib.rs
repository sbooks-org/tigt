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
pub mod presenter;
mod snapshot;
pub mod video;

pub use input::{InputDecoder, InputEvent, InputKey, InputKind, ModifierKey, Modifiers};
pub use snapshot::{SnapshotConfig, SnapshotFormat, SnapshotSignal, SnapshotStatus};

use input::{CallbackHandle, dispatch};
use std::{
    fmt,
    marker::PhantomData,
    ptr,
    rc::Rc,
    sync::atomic::{AtomicBool, Ordering},
};

pub const ABI_VERSION: u32 = 3;
pub const TEXT_MAX_COLUMNS: u16 = 320;
pub const TEXT_MAX_ROWS: u16 = 128;
pub const TEXT_MAX_CELLS: usize = 21440;
pub const TEXT_UNDERLINE: u32 = 1;
pub const TEXT_CURSOR: u32 = 2;

/// Bitmap output policy, selected at initialization and resolved again on resume.
///
/// Explicit modes never silently fall back; unavailable modes return
/// [`Error::Terminal`]. ASCII requires the optional `libcaca` build feature.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum GraphicsMode {
    #[default]
    Auto = 0,
    Blocks = 1,
    Sixel = 2,
    Ascii = 3,
    /// Aspect-normalized 320x200 PNG; requires iTerm2 image support.
    Iterm2 = 4,
}

impl GraphicsMode {
    fn from_raw(value: u32) -> Self {
        match value {
            0 => Self::Auto,
            1 => Self::Blocks,
            2 => Self::Sixel,
            3 => Self::Ascii,
            4 => Self::Iterm2,
            _ => unreachable!("C returned an invalid graphics mode"),
        }
    }
}

/// Terminal presentation policy; this does not decode hardware video memory.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum DisplayTechnology {
    /// Honor the submitted cursor immediately.
    #[default]
    Generic = 0,
    /// Hide the initial cursor until visibly nonblank text has been submitted.
    Mda = 1,
}

/// A resolved single-column Unicode cell. Colors are `0x00RRGGBB`.
///
/// The renderer rejects controls, invalid scalars, and characters whose width
/// is not one in the active terminal locale, as well as unknown flag bits.
/// [`TEXT_CURSOR`] requests a currently visible, steady cursor; the producer
/// resolves blink phases. At most one cell per frame may carry this flag.
/// [`DisplayTechnology::Mda`] can suppress its initial terminal presentation;
/// native snapshots retain the original flag.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TextCell {
    pub codepoint: u32,
    pub foreground: u32,
    pub background: u32,
    pub flags: u32,
}

impl TextCell {
    pub const fn new(character: char, foreground: u32, background: u32) -> Self {
        Self {
            codepoint: character as u32,
            foreground,
            background,
            flags: 0,
        }
    }
}

/// Stored border metadata, not currently rendered. Dimensions are native
/// backing pixels, before any host vertical line doubling.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Overscan {
    pub color: u32,
    pub left: u16,
    pub right: u16,
    pub top: u16,
    pub bottom: u16,
}

/// Maps a hardware CP437 byte to Unicode, including its control-area glyphs.
pub fn cp437_codepoint(character: u8) -> char {
    char::from_u32(unsafe { ffi::tigt_cp437_codepoint(character) })
        .expect("tigt CP437 table contains Unicode scalars")
}

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
        Self::new_with_graphics(GraphicsMode::Auto)
    }

    /// Opens an output-only session with an explicit bitmap output policy.
    pub fn new_with_graphics(mode: GraphicsMode) -> Result<Self, Error> {
        Self::start(SessionClaim::acquire()?, None, mode)
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
        Self::with_input_and_graphics(GraphicsMode::Auto, handler)
    }

    /// Opens a session with a bitmap policy and serialized input-thread callback.
    ///
    /// Callback panic handling is the same as [`Self::with_input`].
    pub fn with_input_and_graphics<F>(mode: GraphicsMode, handler: F) -> Result<Self, Error>
    where
        F: FnMut(InputEvent) + Send + 'static,
    {
        let claim = SessionClaim::acquire()?;
        Self::start(claim, Some(CallbackHandle::new(Box::new(handler))), mode)
    }

    fn start(
        claim: SessionClaim,
        callback: Option<CallbackHandle<InputHandler>>,
        mode: GraphicsMode,
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
            graphics_mode: mode as u32,
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

    /// Returns the configured policy, unchanged by suspend/resume.
    pub fn requested_graphics_mode(&self) -> GraphicsMode {
        GraphicsMode::from_raw(unsafe { ffi::tigt_get_requested_graphics_mode() })
    }

    /// Returns the resolved bitmap output mode. Resume resolves the mode again.
    pub fn graphics_mode(&self) -> GraphicsMode {
        GraphicsMode::from_raw(unsafe { ffi::tigt_get_graphics_mode() })
    }

    /// Sets sixel/iTerm2 target width in terminal columns and display aspect ratio.
    ///
    /// Defaults to 80 columns at 4:3, independently of the source resolution.
    /// Output fits the terminal's known pixel bounds and a 4096-pixel limit:
    /// sixel uses nearest-neighbor resampling, iTerm2 scales in the terminal.
    /// Without usable pixel metrics, the target width
    /// is 640 pixels rather than an assumed character-cell measurement.
    ///
    /// `columns` must be 1..=320 and both aspect components must be nonzero.
    /// Invalid values leave the layout unchanged. Changes redraw the latest
    /// frame without another submission, survive suspend/resume, and do not
    /// affect native snapshots or the blocks/ASCII backends.
    pub fn set_image_layout(
        &self,
        columns: u16,
        aspect_width: u16,
        aspect_height: u16,
    ) -> Result<(), Error> {
        self.input_status()?;
        check_status(unsafe { ffi::tigt_set_image_layout(columns, aspect_width, aspect_height) })
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

    /// Copies a 320x200 or 640x200 RGB frame. The high color byte is ignored;
    /// stride is in pixels and `pixel_width` is 1 or 2 backing pixels per
    /// logical terminal pixel. Padding after the last row is not required.
    /// Native snapshots retain exact RGB. Sixel preserves up to 256 colours
    /// within its percentage-channel precision; larger sets are quantized.
    /// iTerm2 sends 320x200 RGB8: 640-wide logical pixels are averaged in pairs
    /// (per channel, rounded up), 160-wide pixels duplicated, 320-wide unchanged.
    /// Further display scaling belongs to the terminal; native snapshots are unaffected.
    /// Identical resolved RGB/geometry does not redraw; padding and high bytes
    /// are ignored. Layout, resize, resume and text/bitmap transitions still redraw.
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
        validate_frame_length(pixels, width as usize, height as usize, stride as usize)?;
        // Dimensions and every read row are within pixels; C copies them now.
        check_status(unsafe {
            ffi::tigt_present_bitmap(pixels.as_ptr(), width, height, stride, pixel_width)
        })
    }

    /// Copies an indexed bitmap, resolving its palette to exact native RGB.
    ///
    /// Dimensions and stride follow [`Self::present_bitmap`]. `None` selects
    /// standard IBM16 colours and requires indices in 0..16. An explicit
    /// palette must contain 1..=256 `0x00RRGGBB` colours with zero high bytes.
    /// Every visible source index must be in range; stride padding is ignored.
    /// Invalid input leaves the stored frame unchanged. Native snapshots retain
    /// resolved RGB, not palette indices.
    pub fn present_indexed_bitmap(
        &self,
        indices: &[u8],
        width: u16,
        height: u16,
        stride: u16,
        pixel_width: u8,
        palette: Option<&[u32]>,
    ) -> Result<(), Error> {
        self.input_status()?;
        if !matches!(width, 320 | 640) || height != 200 || !matches!(pixel_width, 1 | 2) {
            return Err(Error::Argument);
        }
        validate_frame_length(indices, width as usize, height as usize, stride as usize)?;
        let (palette, palette_size) = match palette {
            Some(colors) if (1..=256).contains(&colors.len()) => {
                (colors.as_ptr(), colors.len() as u16)
            }
            Some(_) => return Err(Error::Argument),
            None => (ptr::null(), 0),
        };
        // C checks palette RGB and all source indices before copying the frame.
        check_status(unsafe {
            ffi::tigt_present_indexed_bitmap(
                indices.as_ptr(),
                width,
                height,
                stride,
                pixel_width,
                palette,
                palette_size,
            )
        })
    }

    /// Copies a resolved text frame without allocating a staging buffer.
    ///
    /// Stride is in cells; padding after the last row is not required. Dimensions
    /// must be nonzero and within [`TEXT_MAX_COLUMNS`], [`TEXT_MAX_ROWS`], and
    /// [`TEXT_MAX_CELLS`]. RGB is quantized to the renderer's 16-color palette.
    pub fn present_text(
        &self,
        cells: &[TextCell],
        columns: u16,
        rows: u16,
        stride: u16,
    ) -> Result<(), Error> {
        self.input_status()?;
        validate_text(cells, columns, rows, stride)?;
        check_status(unsafe { ffi::tigt_present_text(cells.as_ptr(), columns, rows, stride) })
    }

    /// Selects the display policy for an active session, before its next frame.
    ///
    /// MDA hides the initial terminal cursor until an accepted text submission
    /// contains distinct resolved foreground/background RGB and a nonblank,
    /// non-whitespace glyph or underline. Cursor flags alone do not release it.
    /// Every submission counts, even if the renderer skips that frame. Clearing
    /// after output, bitmap frames and repeated hints do not rearm suppression.
    /// Native snapshots are never modified.
    ///
    /// An actual change resets the latch and redraws the retained frame.
    /// Suspend/resume preserve both policy and latch; new sessions use Generic.
    /// Like frame submission, this returns [`Error::Busy`] while suspended.
    pub fn set_display_technology(&self, technology: DisplayTechnology) -> Result<(), Error> {
        self.input_status()?;
        check_status(unsafe { ffi::tigt_set_display_technology(technology as u32) })
    }

    /// Copies metadata independently of the current frame. The caller must
    /// serialize this with submissions when they represent one logical update.
    /// Suspension preserves metadata; a new session starts with zero borders.
    pub fn set_overscan(&self, overscan: &Overscan) -> Result<(), Error> {
        self.input_status()?;
        check_status(unsafe { ffi::tigt_set_overscan(overscan) })
    }

    /// Reads the stored metadata. Like submissions, this requires an active
    /// session; reading while suspended returns [`Error::Busy`].
    pub fn overscan(&self) -> Result<Overscan, Error> {
        self.input_status()?;
        let mut overscan = Overscan::default();
        check_status(unsafe { ffi::tigt_get_overscan(&mut overscan) })?;
        Ok(overscan)
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        // Session is !Send/!Sync: lifecycle and submissions cannot race in safe
        // Rust. Shutdown joins both C workers before fields are destroyed.
        unsafe { ffi::tigt_shutdown() };
    }
}

fn validate_text(cells: &[TextCell], columns: u16, rows: u16, stride: u16) -> Result<(), Error> {
    if columns > TEXT_MAX_COLUMNS
        || rows > TEXT_MAX_ROWS
        || usize::from(columns) * usize::from(rows) > TEXT_MAX_CELLS
    {
        return Err(Error::Argument);
    }
    validate_frame_length(cells, columns as usize, rows as usize, stride as usize)
}

fn validate_frame_length<T>(
    elements: &[T],
    width: usize,
    height: usize,
    stride: usize,
) -> Result<(), Error> {
    if stride < width || width == 0 || height == 0 {
        return Err(Error::Argument);
    }
    let end = (height - 1)
        .checked_mul(stride)
        .and_then(|last| last.checked_add(width))
        .ok_or(Error::Argument)?;
    if elements.len() < end {
        Err(Error::Argument)
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn padded_frame_requires_last_row_pixels_but_not_last_row_padding() {
        let pixels = vec![0; 672 + 640];
        assert_eq!(validate_frame_length(&pixels, 640, 2, 672), Ok(()));
        assert_eq!(
            validate_frame_length(&pixels[..pixels.len() - 1], 640, 2, 672),
            Err(Error::Argument)
        );
    }

    #[test]
    fn text_slices_enforce_stride_and_frame_capacity() {
        let cell = TextCell::new('A', 0xffffff, 0);
        let cells = vec![cell; TEXT_MAX_CELLS];
        assert_eq!(validate_text(&cells, 320, 67, 320), Ok(()));
        assert_eq!(validate_text(&cells, 167, 128, 167), Ok(()));
        for (columns, rows, stride) in [
            (0, 1, 1),
            (1, 0, 1),
            (321, 1, 321),
            (1, 129, 1),
            (320, 68, 320),
            (168, 128, 168),
            (80, 25, 79),
        ] {
            assert_eq!(
                validate_text(&cells, columns, rows, stride),
                Err(Error::Argument)
            );
        }
        assert_eq!(validate_text(&cells[..84], 4, 2, 80), Ok(()));
        assert_eq!(validate_text(&cells[..83], 4, 2, 80), Err(Error::Argument));
        assert_eq!(validate_text(&[], 1, 1, 1), Err(Error::Argument));
    }

    #[test]
    fn cp437_preserves_control_glyphs_and_extended_characters() {
        assert_eq!(cp437_codepoint(1), '☺');
        assert_eq!(cp437_codepoint(0x7f), '⌂');
        assert_eq!(cp437_codepoint(0x82), 'é');
        assert_eq!(cp437_codepoint(0xdb), '█');
    }
}
