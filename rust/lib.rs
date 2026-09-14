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
//! Enable the optional `keyboard` feature for the in-tree PC/XT and PC/AT mapper.

mod ffi;
mod input;
#[cfg(feature = "keyboard")]
pub mod keyboard;
mod mouse;
pub mod presenter;
mod snapshot;
pub mod terminal;
pub mod video;

pub use input::{InputDecoder, InputEvent, InputKey, InputKind, ModifierKey, Modifiers};
pub use mouse::{MouseButton, MouseCoordinates, MouseDecoder, MouseEvent, MouseKind, MouseMode};
pub use snapshot::{SnapshotConfig, SnapshotFormat, SnapshotSignal, SnapshotStatus};

use input::{CallbackHandle, dispatch};
use std::{
    fmt,
    marker::PhantomData,
    ptr,
    rc::Rc,
    sync::atomic::{AtomicBool, Ordering},
};

pub const ABI_VERSION: u32 = 4;
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
    /// Guest text cannot be represented by the required glass-TTY output.
    Unrepresentable,
    /// Bitmap/full-screen output is forbidden while the process is backgrounded.
    Background,
    /// The requested host operation is not supported on this platform.
    Unsupported,
    /// An input or mouse handler panicked and has been permanently disabled.
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
            Self::Unrepresentable => {
                f.write_str("guest text cannot be represented as glass-TTY output")
            }
            Self::Background => {
                f.write_str("tigt graphics output requires the foreground terminal")
            }
            Self::Unsupported => f.write_str("the tigt host operation is unsupported"),
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
        -5 => Err(Error::Unrepresentable),
        -6 => Err(Error::Background),
        -7 => Err(Error::Unsupported),
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
type MouseHandler = Box<dyn FnMut(MouseEvent) + Send + 'static>;

/// Exclusive, owning-thread terminal session.
///
/// `new` is output-only; `with_input`, `with_mouse`, and `with_input_and_mouse`
/// call Send handlers on C's shared input worker. Keyboard and mouse callbacks
/// are serialized, never concurrent. Handlers cannot capture this non-Send
/// session and must not invoke lifecycle operations or reenter input decoding.
/// Send events to the owning thread instead. Raw Ctrl+C, Ctrl+backslash and
/// Ctrl+Z signal the host; Ctrl+T uses SIGINFO where available. Ctrl+V quotes
/// the next complete gesture for the guest. Signal cleanup/chaining is opt-in
/// through [`Self::install_signal_handlers`]; standalone decoders reserve no keys.
///
/// Drop joins C's workers before releasing callback storage or the singleton
/// claim. As with all RAII resources, process abort/exit or `mem::forget` bypasses
/// normal restoration. A failed constructor never shuts down someone else's
/// C session.
pub struct Session {
    callback: Option<CallbackHandle<InputHandler>>,
    mouse_callback: Option<CallbackHandle<MouseHandler>>,
    _claim: SessionClaim,
    signals_installed: bool,
    _owner_thread: PhantomData<Rc<()>>,
}

impl Session {
    /// Opens an output-only session without creating an input worker.
    pub fn new() -> Result<Self, Error> {
        Self::new_with_graphics(GraphicsMode::Auto)
    }

    /// Opens an output-only session with an explicit bitmap output policy.
    pub fn new_with_graphics(mode: GraphicsMode) -> Result<Self, Error> {
        Self::start(SessionClaim::acquire()?, None, None, mode, MouseMode::Off)
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
        Self::start(
            claim,
            Some(CallbackHandle::new(Box::new(handler))),
            None,
            mode,
            MouseMode::Off,
        )
    }

    /// Opens a mouse-only session; keyboard events are not delivered.
    ///
    /// Off returns [`Error::Argument`]; use [`Self::new`] for output-only.
    /// Callback serialization and panic handling match [`Self::with_input`].
    pub fn with_mouse<F>(mode: MouseMode, handler: F) -> Result<Self, Error>
    where
        F: FnMut(MouseEvent) + Send + 'static,
    {
        Self::with_mouse_and_graphics(GraphicsMode::Auto, mode, handler)
    }

    /// Opens a mouse-only session with a bitmap output policy.
    pub fn with_mouse_and_graphics<F>(
        graphics: GraphicsMode,
        mode: MouseMode,
        handler: F,
    ) -> Result<Self, Error>
    where
        F: FnMut(MouseEvent) + Send + 'static,
    {
        if mode == MouseMode::Off {
            return Err(Error::Argument);
        }
        Self::start(
            SessionClaim::acquire()?,
            None,
            Some(CallbackHandle::new(Box::new(handler))),
            graphics,
            mode,
        )
    }

    /// Opens a session with keyboard and mouse handlers on one shared worker.
    ///
    /// Callbacks are serialized and must not reenter decoding or call lifecycle
    /// operations. Each owns independent panic-contained state; [`Self::input_status`]
    /// reports failure from either. Off returns [`Error::Argument`]; use
    /// [`Self::with_input`] for keyboard-only.
    pub fn with_input_and_mouse<I, M>(
        mode: MouseMode,
        input_handler: I,
        mouse_handler: M,
    ) -> Result<Self, Error>
    where
        I: FnMut(InputEvent) + Send + 'static,
        M: FnMut(MouseEvent) + Send + 'static,
    {
        Self::with_input_and_mouse_and_graphics(
            GraphicsMode::Auto,
            mode,
            input_handler,
            mouse_handler,
        )
    }

    /// Opens a combined-input session with a bitmap output policy.
    ///
    /// Callback and mode rules match [`Self::with_input_and_mouse`].
    pub fn with_input_and_mouse_and_graphics<I, M>(
        graphics: GraphicsMode,
        mode: MouseMode,
        input_handler: I,
        mouse_handler: M,
    ) -> Result<Self, Error>
    where
        I: FnMut(InputEvent) + Send + 'static,
        M: FnMut(MouseEvent) + Send + 'static,
    {
        if mode == MouseMode::Off {
            return Err(Error::Argument);
        }
        Self::start(
            SessionClaim::acquire()?,
            Some(CallbackHandle::new(Box::new(input_handler))),
            Some(CallbackHandle::new(Box::new(mouse_handler))),
            graphics,
            mode,
        )
    }

    fn start(
        claim: SessionClaim,
        callback: Option<CallbackHandle<InputHandler>>,
        mouse_callback: Option<CallbackHandle<MouseHandler>>,
        mode: GraphicsMode,
        mouse_mode: MouseMode,
    ) -> Result<Self, Error> {
        let (on_input, user) = match &callback {
            Some(state) => (
                Some(dispatch::<InputHandler> as ffi::InputCallback),
                state.user(),
            ),
            None => (None, ptr::null_mut()),
        };
        let (on_mouse, mouse_user) = match &mouse_callback {
            Some(state) => (
                Some(mouse::dispatch::<MouseHandler> as ffi::MouseCallback),
                state.user(),
            ),
            None => (None, ptr::null_mut()),
        };
        let config = ffi::Config {
            abi_version: ABI_VERSION,
            on_input,
            user,
            graphics_mode: mode as u32,
            on_mouse,
            mouse_user,
            mouse_mode: mouse_mode as u32,
        };
        // Config is copied by C; callback storage stays at its raw allocation.
        // Init failure stops callbacks before returning; claim then releases
        // automatically without calling shutdown on a foreign owner.
        check_status(unsafe { ffi::tigt_init(&config) })?;
        Ok(Self {
            callback,
            mouse_callback,
            _claim: claim,
            _owner_thread: PhantomData,
            signals_installed: false,
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

    /// Returns the resolved live mouse protocol, never Auto.
    ///
    /// Reporting is Off while inactive; resume resolves Auto again.
    pub fn mouse_mode(&self) -> MouseMode {
        MouseMode::from_raw(unsafe { ffi::tigt_get_mouse_mode() })
            .expect("C returned an invalid mouse mode")
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

    /// Reports asynchronous keyboard- or mouse-handler failure without touching curses.
    /// A successful result is a snapshot, not a promise about future callbacks.
    pub fn input_status(&self) -> Result<(), Error> {
        self.callback
            .as_ref()
            .map_or(Ok(()), |callback| callback.status())?;
        self.mouse_callback
            .as_ref()
            .map_or(Ok(()), |callback| callback.status())
    }

    /// Reports both callback failures and persistent asynchronous terminal errors.
    /// Unlike a frame submission, this observes an accepted-frame/background
    /// failure even when the producer has no more frames to submit.
    pub fn status(&self) -> Result<(), Error> {
        self.input_status()?;
        check_status(unsafe { ffi::tigt_terminal_status() })
    }

    /// Services deferred lifecycle work on the owning thread, outside callbacks
    /// and signal handlers. Check [`Self::generation`] even when this returns an
    /// error: a transition must still invalidate external cursor/guest state.
    pub fn poll(&mut self) -> Result<terminal::Events, Error> {
        let result = terminal::Events::from_status(unsafe { ffi::tigt_terminal_poll() });
        self.input_status()?;
        result
    }

    pub fn generation(&self) -> u32 {
        unsafe { ffi::tigt_terminal_generation() }
    }

    pub fn is_foreground(&self) -> bool {
        unsafe { ffi::tigt_terminal_is_foreground() != 0 }
    }

    /// Opts into async-safe cleanup and chaining of supported signal dispositions.
    /// Serialize application signal registration with this call. Normal-context
    /// polling remains required; the raw handler never joins session workers.
    pub fn install_signal_handlers(&mut self) -> Result<(), Error> {
        check_status(unsafe { ffi::tigt_terminal_install_signal_handlers() })?;
        self.signals_installed = true;
        Ok(())
    }

    /// Installs chained handlers with explicitly supplied prior dispositions.
    ///
    /// Use this when platform readback omits flags such as Darwin's
    /// SA_RESETHAND. Unlisted signals retain discovered actions. Nonempty
    /// overrides return [`Error::Busy`] if handlers are already installed.
    ///
    /// # Safety
    ///
    /// Actions must contain valid platform flags and matching handler pointers.
    /// Handlers and their referenced state must remain valid while registered,
    /// including after uninstall restores them; they must obey signal-context
    /// restrictions and never unwind across C. Serialize disposition changes.
    /// See [`terminal::Terminal::install_signal_handlers_with_actions`].
    pub unsafe fn install_signal_handlers_with_actions(
        &mut self,
        actions: &[terminal::SignalAction],
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

    /// Stops terminal input/probes without suspending ongoing output.
    ///
    /// Restores the captured input baseline, including saved noecho, and disables
    /// keyboard/mouse reporting while retaining display ownership. Input stays
    /// disabled across terminal restoration; no output-generation change is
    /// caused by this operation. Call on the normal owner, never a callback or
    /// signal handler. This remains available after a callback panic.
    pub fn disable_input(&mut self) -> Result<(), Error> {
        check_status(unsafe { ffi::tigt_terminal_disable_input() })
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
    /// Background submission returns [`Error::Background`], never dropping,
    /// queueing or converting the bitmap to glass. A later background transition
    /// is a persistent failure observable through [`Self::poll`] / [`Self::status`].
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
        self.uninstall_signal_handlers();
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
