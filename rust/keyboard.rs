// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

//! Optional, allocation-free semantic event conversions. The mapper remains
//! owned and driven by the application; neither renderer nor decoder depends
//! on it. This module exists only with the `keyboard` Cargo feature.

use crate::{InputEvent, InputKey, InputKind, ModifierKey, Modifiers};

/// The exact optional mapper dependency, re-exported for version-consistent use.
pub use pc_xt_keyboard as mapper;

impl From<ModifierKey> for mapper::ModifierKey {
    fn from(key: ModifierKey) -> Self {
        match key {
            ModifierKey::LeftShift => Self::LeftShift,
            ModifierKey::RightShift => Self::RightShift,
            ModifierKey::LeftControl => Self::LeftControl,
            ModifierKey::RightControl => Self::RightControl,
            ModifierKey::LeftAlt => Self::LeftAlt,
            ModifierKey::RightAlt => Self::RightAlt,
            ModifierKey::LeftSuper => Self::LeftSuper,
            ModifierKey::RightSuper => Self::RightSuper,
            ModifierKey::Other => Self::Other,
        }
    }
}

impl From<InputKey> for mapper::InputKey {
    fn from(key: InputKey) -> Self {
        match key {
            InputKey::Char(value) => Self::Char(value),
            InputKey::Backspace => Self::Backspace,
            InputKey::Delete => Self::Delete,
            InputKey::Insert => Self::Insert,
            InputKey::Enter => Self::Enter,
            InputKey::Left => Self::Left,
            InputKey::Right => Self::Right,
            InputKey::Up => Self::Up,
            InputKey::Down => Self::Down,
            InputKey::Home => Self::Home,
            InputKey::End => Self::End,
            InputKey::PageUp => Self::PageUp,
            InputKey::PageDown => Self::PageDown,
            InputKey::PrintScreen => Self::PrintScreen,
            InputKey::Pause => Self::Pause,
            InputKey::ScrollLock => Self::ScrollLock,
            InputKey::NumLock => Self::NumLock,
            InputKey::KeypadBegin => Self::KeypadBegin,
            InputKey::Escape => Self::Escape,
            InputKey::Null => Self::Null,
            InputKey::Function(value) => Self::Function(value),
            InputKey::Modifier(value) => Self::Modifier(value.into()),
        }
    }
}

impl From<InputKind> for mapper::InputKind {
    fn from(kind: InputKind) -> Self {
        match kind {
            InputKind::Press => Self::Press,
            InputKind::Repeat => Self::Repeat,
            InputKind::Release => Self::Release,
        }
    }
}

impl From<Modifiers> for mapper::Modifiers {
    fn from(modifiers: Modifiers) -> Self {
        let mut result = Self::NONE;
        for (source, target) in [
            (Modifiers::SHIFT, Self::SHIFT),
            (Modifiers::CONTROL, Self::CONTROL),
            (Modifiers::ALT, Self::ALT),
            (Modifiers::SUPER, Self::SUPER),
        ] {
            if modifiers.contains(source) {
                result |= target;
            }
        }
        result
    }
}

impl From<InputEvent> for mapper::InputEvent {
    fn from(event: InputEvent) -> Self {
        Self::new(event.key.into(), event.modifiers.into(), event.kind.into())
    }
}

#[cfg(test)]
mod tests {
    use super::mapper::{PcEvent, PcKeyboard};
    use crate::InputDecoder;

    #[test]
    fn plain_and_control_input_generate_physical_key_transitions() {
        let mut keyboard = PcKeyboard::default();
        let mut keys = Vec::new();
        {
            let mut decoder = InputDecoder::new(|event| {
                for event in keyboard.handle(&event.into()) {
                    keys.push(match event {
                        PcEvent::Make(key) => (key.physical, true),
                        PcEvent::Break(key) => (key.physical, false),
                    });
                }
            })
            .unwrap();
            decoder.feed(b"a\x03").unwrap();
        }
        assert_eq!(
            &keys[..4],
            &[(0x1e, true), (0x1e, false), (0x1d, true), (0x2e, true)]
        );
        // Either release order is valid once the Ctrl+C make has been emitted.
        keys[4..].sort_unstable();
        assert_eq!(&keys[4..], &[(0x1d, false), (0x2e, false)]);
    }
}
