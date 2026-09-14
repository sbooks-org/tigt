// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

use crate::{Error, ffi};
use std::{
    cell::UnsafeCell,
    ffi::c_void,
    marker::PhantomData,
    ops::{BitOr, BitOrAssign},
    panic::{AssertUnwindSafe, catch_unwind},
    ptr::NonNull,
    rc::Rc,
    sync::atomic::{AtomicU8, Ordering},
};

/// A transport-neutral semantic key, not a PC scan code.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum InputKey {
    Char(char),
    Backspace,
    Delete,
    Insert,
    Enter,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
    PrintScreen,
    Pause,
    ScrollLock,
    NumLock,
    KeypadBegin,
    Escape,
    Null,
    Function(u8),
    Modifier(ModifierKey),
}

/// A physical modifier reported by the input transport.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ModifierKey {
    LeftShift,
    RightShift,
    LeftControl,
    RightControl,
    LeftAlt,
    RightAlt,
    LeftSuper,
    RightSuper,
    Other,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum InputKind {
    Press,
    Repeat,
    Release,
}

/// Modifiers active during a semantic input event.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct Modifiers(u8);

impl Modifiers {
    pub const NONE: Self = Self(0);
    pub const SHIFT: Self = Self(1 << 0);
    pub const CONTROL: Self = Self(1 << 1);
    pub const ALT: Self = Self(1 << 2);
    pub const SUPER: Self = Self(1 << 3);

    pub const fn from_bits(bits: u8) -> Option<Self> {
        if bits & !0x0f == 0 {
            Some(Self(bits))
        } else {
            None
        }
    }

    pub const fn bits(self) -> u8 {
        self.0
    }

    pub const fn contains(self, other: Self) -> bool {
        self.0 & other.0 == other.0
    }

    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }
}

impl BitOr for Modifiers {
    type Output = Self;

    fn bitor(self, rhs: Self) -> Self {
        Self(self.0 | rhs.0)
    }
}

impl BitOrAssign for Modifiers {
    fn bitor_assign(&mut self, rhs: Self) {
        self.0 |= rhs.0;
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct InputEvent {
    pub key: InputKey,
    pub modifiers: Modifiers,
    pub kind: InputKind,
    /// Literal bracketed-paste payload; never interpret it as a host control gesture.
    pub is_paste: bool,
}

impl InputEvent {
    pub const fn new(key: InputKey, modifiers: Modifiers, kind: InputKind) -> Self {
        Self {
            key,
            modifiers,
            kind,
            is_paste: false,
        }
    }

    fn from_raw(raw: &ffi::InputEvent) -> Option<Self> {
        if raw.flags & !1 != 0 {
            return None;
        }
        let key = match raw.key.kind {
            0 => InputKey::Char(char::from_u32(raw.key.character)?),
            1 => InputKey::Backspace,
            2 => InputKey::Delete,
            3 => InputKey::Insert,
            4 => InputKey::Enter,
            5 => InputKey::Left,
            6 => InputKey::Right,
            7 => InputKey::Up,
            8 => InputKey::Down,
            9 => InputKey::Home,
            10 => InputKey::End,
            11 => InputKey::PageUp,
            12 => InputKey::PageDown,
            13 => InputKey::PrintScreen,
            14 => InputKey::Pause,
            15 => InputKey::ScrollLock,
            16 => InputKey::NumLock,
            17 => InputKey::KeypadBegin,
            18 => InputKey::Escape,
            19 => InputKey::Null,
            20 => InputKey::Function(u8::try_from(raw.key.value).ok()?),
            21 => InputKey::Modifier(match raw.key.value {
                0 => ModifierKey::LeftShift,
                1 => ModifierKey::RightShift,
                2 => ModifierKey::LeftControl,
                3 => ModifierKey::RightControl,
                4 => ModifierKey::LeftAlt,
                5 => ModifierKey::RightAlt,
                6 => ModifierKey::LeftSuper,
                7 => ModifierKey::RightSuper,
                8 => ModifierKey::Other,
                _ => return None,
            }),
            _ => return None,
        };
        let kind = match raw.kind {
            0 => InputKind::Press,
            1 => InputKind::Repeat,
            2 => InputKind::Release,
            _ => return None,
        };
        Some(Self {
            key,
            modifiers: Modifiers::from_bits(raw.modifiers)?,
            kind,
            is_paste: raw.flags & 1 != 0,
        })
    }

    pub(crate) fn to_raw(self) -> ffi::InputEvent {
        let (kind, value, character) = match self.key {
            InputKey::Char(character) => (0, 0, character as u32),
            InputKey::Backspace => (1, 0, 0),
            InputKey::Delete => (2, 0, 0),
            InputKey::Insert => (3, 0, 0),
            InputKey::Enter => (4, 0, 0),
            InputKey::Left => (5, 0, 0),
            InputKey::Right => (6, 0, 0),
            InputKey::Up => (7, 0, 0),
            InputKey::Down => (8, 0, 0),
            InputKey::Home => (9, 0, 0),
            InputKey::End => (10, 0, 0),
            InputKey::PageUp => (11, 0, 0),
            InputKey::PageDown => (12, 0, 0),
            InputKey::PrintScreen => (13, 0, 0),
            InputKey::Pause => (14, 0, 0),
            InputKey::ScrollLock => (15, 0, 0),
            InputKey::NumLock => (16, 0, 0),
            InputKey::KeypadBegin => (17, 0, 0),
            InputKey::Escape => (18, 0, 0),
            InputKey::Null => (19, 0, 0),
            InputKey::Function(value) => (20, u32::from(value), 0),
            InputKey::Modifier(key) => (
                21,
                match key {
                    ModifierKey::LeftShift => 0,
                    ModifierKey::RightShift => 1,
                    ModifierKey::LeftControl => 2,
                    ModifierKey::RightControl => 3,
                    ModifierKey::LeftAlt => 4,
                    ModifierKey::RightAlt => 5,
                    ModifierKey::LeftSuper => 6,
                    ModifierKey::RightSuper => 7,
                    ModifierKey::Other => 8,
                },
                0,
            ),
        };
        ffi::InputEvent {
            key: ffi::InputKey {
                kind,
                value,
                character,
            },
            modifiers: self.modifiers.bits(),
            kind: match self.kind {
                InputKind::Press => 0,
                InputKind::Repeat => 1,
                InputKind::Release => 2,
            },
            flags: u8::from(self.is_paste),
        }
    }
}

struct Callback<F> {
    handler: UnsafeCell<F>,
    failure: AtomicU8,
}

impl<F> Callback<F> {
    pub(crate) fn new(handler: F) -> Self {
        Self {
            handler: UnsafeCell::new(handler),
            failure: AtomicU8::new(0),
        }
    }

    pub(crate) fn status(&self) -> Result<(), Error> {
        match self.failure.load(Ordering::Acquire) {
            0 => Ok(()),
            1 => Err(Error::CallbackPanicked),
            _ => Err(Error::InvalidInputEvent),
        }
    }
}

pub(crate) struct CallbackHandle<F> {
    raw: NonNull<Callback<F>>,
}

impl<F> CallbackHandle<F> {
    pub(crate) fn new(handler: F) -> Self {
        // Keep ownership as a raw pointer while C can call us. Moving a Rust
        // Box must not retag the pointee while the input thread accesses it.
        let raw = Box::into_raw(Box::new(Callback::new(handler)));
        Self {
            raw: NonNull::new(raw).expect("Box allocation returned null"),
        }
    }

    pub(crate) fn user(&self) -> *mut c_void {
        self.raw.as_ptr().cast()
    }

    pub(crate) fn status(&self) -> Result<(), Error> {
        unsafe { self.raw.as_ref() }.status()
    }
}

impl<F> Drop for CallbackHandle<F> {
    fn drop(&mut self) {
        // Owners stop C callbacks before dropping this allocation.
        unsafe { drop(Box::from_raw(self.raw.as_ptr())) };
    }
}

pub(crate) unsafe extern "C" fn dispatch<F: FnMut(InputEvent)>(
    event: *const ffi::InputEvent,
    user: *mut c_void,
) {
    let event = unsafe { event.as_ref() }.and_then(InputEvent::from_raw);
    unsafe { dispatch_event::<F, InputEvent>(event, user) };
}

/// Dispatches a checked event using the shared callback ownership convention.
///
/// The owner retains its stable allocation until C stops callbacks. C invokes
/// callbacks serially; only dispatch accesses the handler. The owner reads
/// failure concurrently through the atomic. Live-session handlers require Send.
pub(crate) unsafe fn dispatch_event<F: FnMut(E), E>(event: Option<E>, user: *mut c_void) {
    let state = unsafe { &*user.cast::<Callback<F>>() };
    if state.failure.load(Ordering::Acquire) != 0 {
        return;
    }
    let Some(event) = event else {
        state.failure.store(2, Ordering::Release);
        return;
    };
    let result = catch_unwind(AssertUnwindSafe(|| {
        unsafe { (&mut *state.handler.get())(event) };
    }));
    if let Err(payload) = result {
        state.failure.store(1, Ordering::Release);
        // A panic payload can itself panic when dropped. Do not run its
        // destructor at the C boundary. At most one payload leaks per handler.
        std::mem::forget(payload);
    }
}

/// An incremental decoder that works without opening a terminal session.
///
/// The handler runs synchronously during `feed` and `flush`, and may borrow
/// local data. Input buffering belongs to C, so split UTF-8 and escape sequences
/// can be fed in separate chunks. `flush` resolves a pending bare Escape after
/// the application's chosen timeout. This standalone decoder reserves no control
/// keys; live-session host policy is separate.
///
/// A panicking handler is disabled and subsequent operations return
/// [`Error::CallbackPanicked`]; unwinding never crosses C (with panic=abort,
/// Rust still aborts). This object is neither Send nor Sync.
/// Handlers must not reenter or destroy their decoder or invoke C lifecycle
/// operations.
///
/// ```
/// use tigt::{InputDecoder, InputKey};
/// let mut keys = Vec::new();
/// {
///     let mut decoder = InputDecoder::new(|event| {
///         if event.kind == tigt::InputKind::Press {
///             keys.push(event.key);
///         }
///     })?;
///     decoder.feed(b"a")?;
/// }
/// assert_eq!(keys, vec![InputKey::Char('a')]);
/// # Ok::<(), tigt::Error>(())
/// ```
pub struct InputDecoder<F: FnMut(InputEvent)> {
    raw: NonNull<c_void>,
    callback: CallbackHandle<F>,
    _owner_thread: PhantomData<Rc<()>>,
}

impl<F: FnMut(InputEvent)> InputDecoder<F> {
    pub fn new(handler: F) -> Result<Self, Error> {
        let callback = CallbackHandle::new(handler);
        let user = callback.user();
        // C stores user but does not invoke it until feed/flush.
        let raw = unsafe { ffi::tigt_input_create(Some(dispatch::<F>), user) };
        let raw = NonNull::new(raw).ok_or(Error::System)?;
        Ok(Self {
            raw,
            callback,
            _owner_thread: PhantomData,
        })
    }

    pub fn feed(&mut self, bytes: &[u8]) -> Result<(), Error> {
        self.callback.status()?;
        // C only reads bytes during this call; &mut self serializes decoding.
        unsafe { ffi::tigt_input_feed(self.raw.as_ptr(), bytes.as_ptr(), bytes.len()) };
        self.callback.status()
    }

    pub fn flush(&mut self) -> Result<(), Error> {
        self.callback.status()?;
        unsafe { ffi::tigt_input_flush(self.raw.as_ptr()) };
        self.callback.status()
    }
}

impl<F: FnMut(InputEvent)> Drop for InputDecoder<F> {
    fn drop(&mut self) {
        // Destroy before callback storage is dropped. Destroy emits no events.
        unsafe { ffi::tigt_input_destroy(self.raw.as_ptr()) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bracketed_paste_preserves_literal_controls_and_help_in_rust_events() {
        let mut events = Vec::new();
        {
            let mut decoder = InputDecoder::new(|event| events.push(event)).unwrap();
            decoder.feed(b"\x1b[200").unwrap();
            decoder
                .feed("~\u{f746}\x03\x1b[201~\u{f746}".as_bytes())
                .unwrap();
        }
        let presses: Vec<_> = events
            .into_iter()
            .filter(|event| event.kind == InputKind::Press)
            .map(|event| (event.key, event.is_paste))
            .collect();
        assert_eq!(
            presses,
            [
                (InputKey::Char('\u{f746}'), true),
                (InputKey::Char('\x03'), true),
                (InputKey::Insert, false),
            ]
        );
    }

    #[test]
    fn fragmented_utf8_and_control_taps_reach_borrowed_callback() {
        let mut events = Vec::new();
        {
            let mut decoder = InputDecoder::new(|event| events.push(event)).unwrap();
            decoder.feed(b"\xf0\x9d").unwrap();
            decoder.feed(b"\x84\x9e\x03").unwrap();
        }
        assert_eq!(
            events,
            [
                InputEvent::new(
                    InputKey::Char('\u{1d11e}'),
                    Modifiers::NONE,
                    InputKind::Press
                ),
                InputEvent::new(
                    InputKey::Char('\u{1d11e}'),
                    Modifiers::NONE,
                    InputKind::Release
                ),
                InputEvent::new(InputKey::Char('c'), Modifiers::CONTROL, InputKind::Press),
                InputEvent::new(InputKey::Char('c'), Modifiers::CONTROL, InputKind::Release),
            ]
        );
    }

    #[test]
    fn callback_panic_is_contained_and_further_delivery_is_disabled() {
        let mut calls = 0;
        {
            let mut decoder = InputDecoder::new(|_| {
                calls += 1;
                panic!("callback failure");
            })
            .unwrap();
            assert_eq!(decoder.feed(b"ab"), Err(Error::CallbackPanicked));
            assert_eq!(decoder.feed(b"c"), Err(Error::CallbackPanicked));
            assert_eq!(decoder.flush(), Err(Error::CallbackPanicked));
        }
        assert_eq!(calls, 1);
    }
}
