// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

use crate::{
    Error, Modifiers, ffi,
    input::{CallbackHandle, dispatch_event},
};
use std::{
    ffi::c_void,
    marker::PhantomData,
    ptr::{self, NonNull},
    rc::Rc,
};

/// Mouse reporting policy. Standalone decoding never enables terminal modes.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum MouseMode {
    #[default]
    Off = 0,
    /// Live sessions probe pixel reporting, falling back to cell SGR.
    /// Standalone decoders interpret SGR coordinates as cells without probing.
    Auto = 1,
    Cells = 2,
    /// Caller-selected pixel reporting; the terminal must support it.
    Pixels = 3,
    /// Legacy press-only clicks; each press receives a synthetic release.
    X10 = 4,
}

impl MouseMode {
    pub(crate) fn from_raw(value: u32) -> Option<Self> {
        match value {
            0 => Some(Self::Off),
            1 => Some(Self::Auto),
            2 => Some(Self::Cells),
            3 => Some(Self::Pixels),
            4 => Some(Self::X10),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum MouseKind {
    Move,
    Down,
    Up,
    Scroll,
    /// A real Kitty pixel-mode leave report, not keyboard focus loss.
    Leave,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum MouseButton {
    #[default]
    None,
    Left,
    Middle,
    Right,
}

impl MouseButton {
    /// This button's bit in [`MouseEvent::buttons`]; None has no bit.
    pub const fn mask(self) -> u32 {
        match self {
            Self::None => 0,
            Self::Left => 1 << 0,
            Self::Middle => 1 << 1,
            Self::Right => 1 << 2,
        }
    }
}

/// Units of the raw, zero-based terminal coordinates.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum MouseCoordinates {
    Cells,
    Pixels,
}

/// A mouse transition, retaining raw coordinates and explicit validity.
///
/// MOVE precedes every position-bearing DOWN, UP, or SCROLL. Frame coordinates
/// are fractional logical bitmap pixels or text cells in the displayed frame,
/// not the latest submitted frame. Coarse cell reports map from cell centres.
/// Coordinates are not clamped; unknown geometry leaves `frame_valid` false.
/// Raw terminal pixels do not promise physical sub-pixel precision. Standalone
/// decoders leave all frame fields unset. Mouse-emulated taps remain clicks;
/// these protocols do not identify touch contacts or multitouch.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MouseEvent {
    pub kind: MouseKind,
    pub button: MouseButton,
    /// Held-button mask, using [`MouseButton::mask`].
    pub buttons: u32,
    pub modifiers: Modifiers,
    pub coordinates: MouseCoordinates,
    pub position_valid: bool,
    pub frame_valid: bool,
    pub inside_frame: bool,
    /// True for synthetic X10 releases.
    pub synthetic: bool,
    /// Raw zero-based terminal position; valid only when `position_valid`.
    pub x: i32,
    /// Raw zero-based terminal position; valid only when `position_valid`.
    pub y: i32,
    /// Wheel steps, positive right.
    pub scroll_x: i32,
    /// Wheel steps, positive down.
    pub scroll_y: i32,
    /// Displayed source-frame position; valid only when `frame_valid`.
    pub frame_x: f64,
    /// Displayed source-frame position; valid only when `frame_valid`.
    pub frame_y: f64,
}

impl MouseEvent {
    fn from_raw(raw: &ffi::MouseEvent) -> Option<Self> {
        let kind = match raw.kind {
            0 => MouseKind::Move,
            1 => MouseKind::Down,
            2 => MouseKind::Up,
            3 => MouseKind::Scroll,
            4 => MouseKind::Leave,
            _ => return None,
        };
        let button = match raw.button {
            0 => MouseButton::None,
            1 => MouseButton::Left,
            2 => MouseButton::Middle,
            3 => MouseButton::Right,
            _ => return None,
        };
        let coordinates = match raw.coordinates {
            0 => MouseCoordinates::Cells,
            1 => MouseCoordinates::Pixels,
            _ => return None,
        };
        if raw.buttons & !0x07 != 0 || raw.flags & !0x0f != 0 {
            return None;
        }
        Some(Self {
            kind,
            button,
            buttons: raw.buttons,
            modifiers: Modifiers::from_bits(u8::try_from(raw.modifiers).ok()?)?,
            coordinates,
            position_valid: raw.flags & (1 << 0) != 0,
            frame_valid: raw.flags & (1 << 1) != 0,
            inside_frame: raw.flags & (1 << 2) != 0,
            synthetic: raw.flags & (1 << 3) != 0,
            x: raw.x,
            y: raw.y,
            scroll_x: raw.scroll_x,
            scroll_y: raw.scroll_y,
            frame_x: raw.frame_x,
            frame_y: raw.frame_y,
        })
    }
}

pub(crate) unsafe extern "C" fn dispatch<F: FnMut(MouseEvent)>(
    event: *const ffi::MouseEvent,
    user: *mut c_void,
) {
    let event = unsafe { event.as_ref() }.and_then(MouseEvent::from_raw);
    unsafe { dispatch_event::<F, MouseEvent>(event, user) };
}

/// An incremental mouse decoder without terminal ownership or frame geometry.
///
/// The handler runs synchronously during `feed` and `flush` and may borrow local
/// data; it need not be Send. Split escape sequences may span chunks. Keyboard
/// events are discarded. `flush` resolves pending Escape ambiguity. The caller
/// selects the protocol and owns any terminal setup; Auto means cell SGR here.
///
/// A panicking handler is permanently disabled and operations report
/// [`Error::CallbackPanicked`]. Invalid C events report [`Error::InvalidInputEvent`].
/// Unwinding never crosses C; panic=abort still aborts. Handlers must not reenter
/// or destroy their decoder or invoke C lifecycle operations. This object is
/// neither Send nor Sync, and its callback storage outlives the C decoder.
pub struct MouseDecoder<F: FnMut(MouseEvent)> {
    raw: NonNull<c_void>,
    callback: CallbackHandle<F>,
    _owner_thread: PhantomData<Rc<()>>,
}

impl<F: FnMut(MouseEvent)> MouseDecoder<F> {
    /// Creates a decoder for a reporting protocol; Off returns [`Error::Argument`].
    pub fn new(mode: MouseMode, handler: F) -> Result<Self, Error> {
        if mode == MouseMode::Off {
            return Err(Error::Argument);
        }
        let callback = CallbackHandle::new(handler);
        // C retains the stable callback allocation and invokes it only on feed/flush.
        let raw = unsafe {
            ffi::tigt_input_create_with_mouse(
                None,
                ptr::null_mut(),
                Some(dispatch::<F>),
                callback.user(),
                mode as u32,
            )
        };
        let raw = NonNull::new(raw).ok_or(Error::System)?;
        Ok(Self {
            raw,
            callback,
            _owner_thread: PhantomData,
        })
    }

    pub fn feed(&mut self, bytes: &[u8]) -> Result<(), Error> {
        self.callback.status()?;
        // The slice is borrowed only for this call; &mut self serializes decoding.
        unsafe { ffi::tigt_input_feed(self.raw.as_ptr(), bytes.as_ptr(), bytes.len()) };
        self.callback.status()
    }

    pub fn flush(&mut self) -> Result<(), Error> {
        self.callback.status()?;
        unsafe { ffi::tigt_input_flush(self.raw.as_ptr()) };
        self.callback.status()
    }
}

impl<F: FnMut(MouseEvent)> Drop for MouseDecoder<F> {
    fn drop(&mut self) {
        // Stop C before dropping the callback allocation. Destroy emits no events.
        unsafe { ffi::tigt_input_destroy(self.raw.as_ptr()) };
    }
}
