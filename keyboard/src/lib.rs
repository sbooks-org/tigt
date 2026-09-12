mod ffi;
use std::{
    collections::HashMap,
    ops::{BitOr, BitOrAssign},
};

/// A key reported by an input transport, independent of terminal libraries.
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

/// A physical modifier identity reported by an input transport.
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

/// Press state reported by an input transport.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum InputKind {
    Press,
    Repeat,
    Release,
}

/// Active modifiers reported alongside an [`InputEvent`].
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct Modifiers(u8);

impl Modifiers {
    pub const NONE: Self = Self(0);
    pub const SHIFT: Self = Self(1 << 0);
    pub const CONTROL: Self = Self(1 << 1);
    pub const ALT: Self = Self(1 << 2);
    pub const SUPER: Self = Self(1 << 3);

    pub const fn contains(self, other: Self) -> bool {
        self.0 & other.0 == other.0
    }

    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }
}

impl BitOr for Modifiers {
    type Output = Self;

    fn bitor(self, rhs: Self) -> Self::Output {
        Self(self.0 | rhs.0)
    }
}

impl BitOrAssign for Modifiers {
    fn bitor_assign(&mut self, rhs: Self) {
        self.0 |= rhs.0;
    }
}

/// One input transition supplied by a terminal, window system, or other host adapter.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct InputEvent {
    pub key: InputKey,
    pub modifiers: Modifiers,
    pub kind: InputKind,
}

impl InputEvent {
    pub const fn new(key: InputKey, modifiers: Modifiers, kind: InputKind) -> Self {
        Self {
            key,
            modifiers,
            kind,
        }
    }
}

/// A bounded sequence of keyboard-controller bytes. Empty sequences are valid for keys,
/// such as PC/AT Pause, that have no distinct break sequence.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ScanSequence {
    bytes: [u8; 8],
    len: u8,
}

impl ScanSequence {
    pub const EMPTY: Self = Self {
        bytes: [0; 8],
        len: 0,
    };

    pub const fn one(byte: u8) -> Self {
        Self {
            bytes: [byte, 0, 0, 0, 0, 0, 0, 0],
            len: 1,
        }
    }

    const fn two(bytes: [u8; 2]) -> Self {
        Self {
            bytes: [bytes[0], bytes[1], 0, 0, 0, 0, 0, 0],
            len: 2,
        }
    }

    pub fn bytes(&self) -> &[u8] {
        &self.bytes[..self.len as usize]
    }

    const fn six(bytes: [u8; 6]) -> Self {
        Self {
            bytes: [
                bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], 0, 0,
            ],
            len: 6,
        }
    }

    const fn four(bytes: [u8; 4]) -> Self {
        Self {
            bytes: [bytes[0], bytes[1], bytes[2], bytes[3], 0, 0, 0, 0],
            len: 4,
        }
    }
}

/// A guest PC key with a physical identity and optional wire-protocol sequences.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PcKey {
    pub id: &'static str,
    /// PC make-position identity, not a scan byte or encoded sequence.
    ///
    /// Ordinary keys use `0x01..=0x7f`. AT Print Screen is `0x137` and AT Pause
    /// is `0x145`; XT Print Screen is `0x37` and XT Pause maps to Ctrl + Num Lock.
    /// AT navigation keys outside the Command layer use `0x100 | make_position`
    /// (for example Left is `0x14b`), distinct from their keypad counterparts.
    pub physical: u16,
    pub make: ScanSequence,
    pub break_sequence: ScanSequence,
}

/// A make or break transition emitted by [`PcKeyboard`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PcEvent {
    Make(PcKey),
    Break(PcKey),
}

/// Guest keyboard protocol to emit.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum KeyboardModel {
    #[default]
    XtSet1,
    AtSet1,
}

#[derive(Clone)]
struct HeldMapping {
    keys: Vec<PcKey>,
    release_with_command: bool,
}

/// Stateful, transport-neutral PC keyboard mapper.
///
/// It translates [`InputEvent`] values into physical key transitions, with Set 1
/// sequences available for wire-protocol consumers, and reference-counts shared modifiers.
pub struct PcKeyboard {
    model: KeyboardModel,
    held: HashMap<String, HeldMapping>,
    counts: HashMap<&'static str, usize>,
}

impl PcKeyboard {
    pub fn new(model: KeyboardModel) -> Self {
        Self {
            model,
            held: HashMap::new(),
            counts: HashMap::new(),
        }
    }

    pub const fn model(&self) -> KeyboardModel {
        self.model
    }

    /// Processes one transport-neutral input event.
    pub fn handle(&mut self, input: &InputEvent) -> Vec<PcEvent> {
        match input.kind {
            InputKind::Press => {
                let source = source_id(input.key);
                let mapping = map_key(input, self.model);
                let release_with_command = is_command_layer_mapping(input, &mapping);
                self.press(source, mapping, release_with_command)
            }
            InputKind::Release => {
                let mut events = self.release_source(&source_id(input.key));
                if is_command_key(input.key) {
                    events.extend(self.release_command_mappings());
                }
                events
            }
            InputKind::Repeat => Vec::new(),
        }
    }

    /// Releases a source key when an input transport supplies a synthetic timeout.
    pub fn release_source(&mut self, source: &str) -> Vec<PcEvent> {
        let Some(held) = self.held.remove(source) else {
            return Vec::new();
        };

        let mut events = Vec::new();
        for key in held.keys {
            let count = self
                .counts
                .get_mut(key.id)
                .expect("held PC mapping must have a reference count");
            *count -= 1;
            if *count == 0 {
                self.counts.remove(key.id);
                events.push(PcEvent::Break(key));
            }
        }
        events
    }

    /// Releases all key mappings dependent on the Command layer.
    pub fn release_command_mappings(&mut self) -> Vec<PcEvent> {
        let sources = self
            .held
            .iter()
            .filter(|(_, held)| held.release_with_command)
            .map(|(source, _)| source.clone())
            .collect::<Vec<_>>();
        sources
            .into_iter()
            .flat_map(|source| self.release_source(&source))
            .collect()
    }

    /// Releases every key still held by the mapper.
    pub fn release_all(&mut self) -> Vec<PcEvent> {
        let sources = self.held.keys().cloned().collect::<Vec<_>>();
        sources
            .into_iter()
            .flat_map(|source| self.release_source(&source))
            .collect()
    }

    fn press(
        &mut self,
        source: String,
        mapping: Vec<PcKey>,
        release_with_command: bool,
    ) -> Vec<PcEvent> {
        let mut events = self.release_source(&source);
        for key in &mapping {
            let count = self.counts.entry(key.id).or_default();
            if *count == 0 {
                events.push(PcEvent::Make(*key));
            }
            *count += 1;
        }
        self.held.insert(
            source,
            HeldMapping {
                keys: mapping,
                release_with_command,
            },
        );
        events
    }
}

impl Default for PcKeyboard {
    fn default() -> Self {
        Self::new(KeyboardModel::XtSet1)
    }
}

fn single(model: KeyboardModel, id: &'static str, make_code: u8) -> PcKey {
    match (model, id) {
        (KeyboardModel::AtSet1, "PRINT") => PcKey {
            id,
            physical: 0x137,
            make: ScanSequence::four([0xE0, 0x2A, 0xE0, 0x37]),
            break_sequence: ScanSequence::four([0xE0, 0xB7, 0xE0, 0xAA]),
        },
        (KeyboardModel::AtSet1, "PAUSE") => PcKey {
            id,
            physical: 0x145,
            make: ScanSequence::six([0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5]),
            break_sequence: ScanSequence::EMPTY,
        },
        _ => PcKey {
            id,
            physical: u16::from(make_code),
            make: ScanSequence::one(make_code),
            break_sequence: ScanSequence::one(make_code | 0x80),
        },
    }
}
/// Maps a transport-neutral input event into guest PC keys.
pub fn map_key(input: &InputEvent, model: KeyboardModel) -> Vec<PcKey> {
    use InputKey::*;
    use ModifierKey::*;

    let single = |id, make_code| vec![crate::single(model, id, make_code)];
    let command = input.modifiers.contains(Modifiers::SUPER);
    if let Some(mapping) = command_mapping(input, model) {
        return mapping;
    }
    let navigation = |id, keypad_id, make_code| {
        if model == KeyboardModel::AtSet1 && !command {
            vec![PcKey {
                id,
                physical: 0x100 | u16::from(make_code),
                make: ScanSequence::two([0xE0, make_code]),
                break_sequence: ScanSequence::two([0xE0, make_code | 0x80]),
            }]
        } else {
            single(keypad_id, make_code)
        }
    };

    let mut mapping = match input.key {
        Modifier(LeftShift) => single("L_SHIFT", 0x2A),
        Modifier(RightShift) => single("R_SHIFT", 0x36),
        Modifier(LeftControl) => single("CTRL", 0x1D),
        Modifier(LeftAlt) => single("ALT", 0x38),
        Modifier(RightControl | RightAlt | Other | LeftSuper | RightSuper) => Vec::new(),
        Escape => single("ESC", 0x01),
        Null | Char('\0') => vec![
            crate::single(model, "CTRL", 0x1D),
            crate::single(model, "SPACE", 0x39),
        ],
        Backspace | Char('\u{8}') if command => single("SCROLL", 0x46),
        Backspace | Char('\u{8}') => single("BACKSPACE", 0x0E),
        Delete => navigation("DELETE", "KP_DEL", 0x53),
        Insert => navigation("INSERT", "KP_INS", 0x52),
        Enter | Char('\r' | '\n') if command => single("KP_INS", 0x52),
        Enter | Char('\r' | '\n') => single("ENTER", 0x1C),
        Left => navigation("LEFT", "KP_4", 0x4B),
        Right => navigation("RIGHT", "KP_6", 0x4D),
        Up => navigation("UP", "KP_8", 0x48),
        Down => navigation("DOWN", "KP_2", 0x50),
        Home => navigation("HOME", "KP_7", 0x47),
        End => navigation("END", "KP_1", 0x4F),
        PageUp => navigation("PAGE_UP", "KP_9", 0x49),
        PageDown => navigation("PAGE_DOWN", "KP_3", 0x51),
        PrintScreen => single("PRINT", 0x37),
        Pause if model == KeyboardModel::AtSet1 => single("PAUSE", 0),
        Pause => vec![
            crate::single(model, "CTRL", 0x1D),
            crate::single(model, "NUMLOCK", 0x45),
        ],
        ScrollLock => single("SCROLL", 0x46),
        NumLock => single("NUMLOCK", 0x45),
        KeypadBegin => single("KP_5", 0x4C),
        Function(number @ 1..=10) => single(
            match number {
                1 => "F1",
                2 => "F2",
                3 => "F3",
                4 => "F4",
                5 => "F5",
                6 => "F6",
                7 => "F7",
                8 => "F8",
                9 => "F9",
                10 => "F10",
                _ => unreachable!(),
            },
            0x3A + number,
        ),
        Function(13) => single("KP_INS", 0x52),
        Function(14) => single("KP_5", 0x4C),
        Function(15) => single("KP_DEL", 0x53),
        Function(16) => single("CAPS", 0x3A),
        Function(17) => single("KP_MINUS", 0x4A),
        Function(18) => single("KP_PLUS", 0x4E),
        Function(19) => single("KP_STAR", 0x37),
        Function(20) => single("SCROLL", 0x46),
        Function(21) => single("NUMLOCK", 0x45),
        Char(character) => map_char(character, command, model),
        _ => Vec::new(),
    };

    if !matches!(input.key, Modifier(_)) {
        prepend_simulated_modifiers(input, &mut mapping, model);
    }
    mapping
}

fn command_mapping(input: &InputEvent, model: KeyboardModel) -> Option<Vec<PcKey>> {
    use InputKey::*;

    if !input.modifiers.contains(Modifiers::SUPER) {
        return None;
    }

    let single = |id, make_code| vec![crate::single(model, id, make_code)];
    let control_shift =
        input.modifiers.contains(Modifiers::CONTROL) && input.modifiers.contains(Modifiers::SHIFT);
    let control_pair = |id, make_code| {
        vec![
            crate::single(model, "CTRL", 0x1D),
            crate::single(model, id, make_code),
        ]
    };

    match input.key {
        Backspace | Char('\u{8}') if control_shift => Some(control_pair("SCROLL", 0x46)),
        Backspace | Char('\u{8}') => Some(single("SCROLL", 0x46)),
        Char('\\' | '|') if control_shift && model == KeyboardModel::AtSet1 => {
            Some(single("PAUSE", 0))
        }
        Char('\\' | '|') if control_shift => Some(control_pair("NUMLOCK", 0x45)),
        Char('\\' | '|') => Some(single("NUMLOCK", 0x45)),
        Char('5' | '%') if control_shift && input.modifiers.contains(Modifiers::ALT) => {
            Some(single("KP_5", 0x4C))
        }
        Char('-' | '_') if control_shift && input.modifiers.contains(Modifiers::ALT) => {
            Some(single("KP_MINUS", 0x4A))
        }
        Char('=' | '+') if control_shift && input.modifiers.contains(Modifiers::ALT) => {
            Some(single("KP_PLUS", 0x4E))
        }
        Char('`' | '~')
            if control_shift
                && input.modifiers.contains(Modifiers::ALT)
                && model == KeyboardModel::AtSet1 =>
        {
            Some(single("SYSRQ", 0x54))
        }
        Char('*') => Some(single("PRINT", 0x37)),
        Char('8') if input.modifiers.contains(Modifiers::SHIFT) => Some(single("PRINT", 0x37)),
        _ => None,
    }
}

/// A stable physical-key identifier for matching press and release events.
pub fn source_id(key: InputKey) -> String {
    match key {
        InputKey::Backspace | InputKey::Char('\u{8}') => "BACKSPACE".into(),
        InputKey::Null | InputKey::Char('\0') => "SPACE".into(),
        InputKey::Enter | InputKey::Char('\r' | '\n') => "RETURN".into(),
        InputKey::Char(character) => physical_char_id(character).into(),
        InputKey::Delete => "FORWARD_DELETE".into(),
        InputKey::Insert => "INSERT".into(),
        InputKey::Escape => "ESC".into(),
        InputKey::Pause => "PAUSE".into(),
        InputKey::ScrollLock => "SCROLLLOCK".into(),
        InputKey::NumLock => "NUMLOCK".into(),
        InputKey::PrintScreen => "PRINT".into(),
        InputKey::KeypadBegin => "KEYPAD_BEGIN".into(),
        InputKey::Function(number) => format!("F{number}"),
        InputKey::Modifier(modifier) => modifier_id(modifier).into(),
        other => format!("{other:?}"),
    }
}

pub fn is_command_layer_mapping(input: &InputEvent, mapping: &[PcKey]) -> bool {
    input.modifiers.contains(Modifiers::SUPER)
        && !mapping.is_empty()
        && !matches!(input.key, InputKey::Modifier(_))
}

pub fn is_command_key(key: InputKey) -> bool {
    matches!(
        key,
        InputKey::Modifier(ModifierKey::LeftSuper | ModifierKey::RightSuper)
    )
}

fn prepend_simulated_modifiers(input: &InputEvent, mapping: &mut Vec<PcKey>, model: KeyboardModel) {
    if mapping.is_empty() {
        return;
    }

    let mut modifiers = Vec::with_capacity(3);
    let shift = input.modifiers.contains(Modifiers::SHIFT) || key_requires_shift(input.key);
    for (active, id, make_code) in [
        (shift, "L_SHIFT", 0x2A),
        (input.modifiers.contains(Modifiers::CONTROL), "CTRL", 0x1D),
        (input.modifiers.contains(Modifiers::ALT), "ALT", 0x38),
    ] {
        if active && !mapping.iter().any(|key| key.id == id) {
            modifiers.push(crate::single(model, id, make_code));
        }
    }
    modifiers.append(mapping);
    *mapping = modifiers;
}

fn key_requires_shift(key: InputKey) -> bool {
    matches!(
        key,
        InputKey::Char(
            'A'..='Z'
                | '!'
                | '@'
                | '#'
                | '$'
                | '%'
                | '^'
                | '&'
                | '*'
                | '('
                | ')'
                | '_'
                | '+'
                | '{'
                | '}'
                | '|'
                | ':'
                | '"'
                | '<'
                | '>'
                | '?'
                | '~'
        )
    )
}

fn map_char(character: char, command: bool, model: KeyboardModel) -> Vec<PcKey> {
    let single = |id, make_code| vec![crate::single(model, id, make_code)];
    let character = character.to_ascii_lowercase();

    if command {
        return match character {
            '5' => single("KP_5", 0x4C),
            '.' | '>' => single("KP_DEL", 0x53),
            '-' | '_' => single("KP_MINUS", 0x4A),
            '=' | '+' => single("KP_PLUS", 0x4E),
            '8' | '*' => single("KP_STAR", 0x37),
            '\\' | '|' => single("NUMLOCK", 0x45),
            'c' => vec![
                crate::single(model, "CTRL", 0x1D),
                crate::single(model, "C", 0x2E),
            ],
            'z' => vec![
                crate::single(model, "CTRL", 0x1D),
                crate::single(model, "Z", 0x2C),
            ],
            _ => Vec::new(),
        };
    }

    let (id, make_code) = match character {
        '\t' => ("TAB", 0x0F),
        'a' => ("A", 0x1E),
        'b' => ("B", 0x30),
        'c' => ("C", 0x2E),
        'd' => ("D", 0x20),
        'e' => ("E", 0x12),
        'f' => ("F", 0x21),
        'g' => ("G", 0x22),
        'h' => ("H", 0x23),
        'i' => ("I", 0x17),
        'j' => ("J", 0x24),
        'k' => ("K", 0x25),
        'l' => ("L", 0x26),
        'm' => ("M", 0x32),
        'n' => ("N", 0x31),
        'o' => ("O", 0x18),
        'p' => ("P", 0x19),
        'q' => ("Q", 0x10),
        'r' => ("R", 0x13),
        's' => ("S", 0x1F),
        't' => ("T", 0x14),
        'u' => ("U", 0x16),
        'v' => ("V", 0x2F),
        'w' => ("W", 0x11),
        'x' => ("X", 0x2D),
        'y' => ("Y", 0x15),
        'z' => ("Z", 0x2C),
        '1' | '!' => ("1", 0x02),
        '2' | '@' => ("2", 0x03),
        '3' | '#' => ("3", 0x04),
        '4' | '$' => ("4", 0x05),
        '5' | '%' => ("5", 0x06),
        '6' | '^' => ("6", 0x07),
        '7' | '&' => ("7", 0x08),
        '8' | '*' => ("8", 0x09),
        '9' | '(' => ("9", 0x0A),
        '0' | ')' => ("0", 0x0B),
        '-' | '_' => ("-", 0x0C),
        '=' | '+' => ("=", 0x0D),
        '[' | '{' => ("[", 0x1A),
        ']' | '}' => ("]", 0x1B),
        '\\' | '|' => ("\\", 0x2B),
        ';' | ':' => (";", 0x27),
        '\'' | '"' => ("'", 0x28),
        ',' | '<' => (",", 0x33),
        '.' | '>' => (".", 0x34),
        '/' | '?' => ("/", 0x35),
        '`' | '~' => ("`", 0x29),
        ' ' => ("SPACE", 0x39),
        _ => return Vec::new(),
    };
    single(id, make_code)
}

fn physical_char_id(character: char) -> &'static str {
    match character {
        ' ' => "SPACE",
        '0' => "0",
        '1' => "1",
        '2' => "2",
        '3' => "3",
        '4' => "4",
        '5' => "5",
        '6' => "6",
        '7' => "7",
        '8' => "8",
        '9' => "9",
        '`' | '~' => "`",
        '-' | '_' => "-",
        '=' | '+' => "=",
        '[' | '{' => "[",
        ']' | '}' => "]",
        '\\' | '|' => "\\",
        ';' | ':' => ";",
        '\'' | '"' => "'",
        ',' | '<' => ",",
        '.' | '>' => ".",
        '/' | '?' => "/",
        _ if character.is_ascii_alphabetic() => match character.to_ascii_uppercase() {
            'A' => "A",
            'B' => "B",
            'C' => "C",
            'D' => "D",
            'E' => "E",
            'F' => "F",
            'G' => "G",
            'H' => "H",
            'I' => "I",
            'J' => "J",
            'K' => "K",
            'L' => "L",
            'M' => "M",
            'N' => "N",
            'O' => "O",
            'P' => "P",
            'Q' => "Q",
            'R' => "R",
            'S' => "S",
            'T' => "T",
            'U' => "U",
            'V' => "V",
            'W' => "W",
            'X' => "X",
            'Y' => "Y",
            'Z' => "Z",
            _ => unreachable!(),
        },
        _ => "OTHER_CHARACTER",
    }
}

fn modifier_id(modifier: ModifierKey) -> &'static str {
    match modifier {
        ModifierKey::LeftShift => "L_SHIFT",
        ModifierKey::RightShift => "R_SHIFT",
        ModifierKey::LeftControl => "L_CTRL",
        ModifierKey::RightControl => "R_CTRL",
        ModifierKey::LeftAlt => "L_OPT",
        ModifierKey::RightAlt => "R_OPT",
        ModifierKey::LeftSuper => "L_CMD",
        ModifierKey::RightSuper => "R_CMD",
        ModifierKey::Other => "OTHER_MODIFIER",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn press(key: InputKey, modifiers: Modifiers) -> InputEvent {
        InputEvent::new(key, modifiers, InputKind::Press)
    }

    fn make_bytes(input: InputEvent, model: KeyboardModel) -> Vec<u8> {
        map_key(&input, model)
            .iter()
            .flat_map(|key| key.make.bytes())
            .copied()
            .collect()
    }

    #[test]
    fn maps_standard_and_navigation_keys() {
        assert_eq!(
            make_bytes(
                press(InputKey::Char('a'), Modifiers::NONE),
                KeyboardModel::XtSet1
            ),
            vec![0x1E]
        );
        assert_eq!(
            make_bytes(press(InputKey::Up, Modifiers::NONE), KeyboardModel::XtSet1),
            vec![0x48]
        );
        assert_eq!(
            make_bytes(
                press(InputKey::Delete, Modifiers::NONE),
                KeyboardModel::XtSet1
            ),
            vec![0x53]
        );
    }

    #[test]
    fn maps_modifier_chords_and_legacy_control_space() {
        assert_eq!(
            make_bytes(
                press(InputKey::Char('A'), Modifiers::NONE),
                KeyboardModel::XtSet1
            ),
            vec![0x2A, 0x1E]
        );
        assert_eq!(
            make_bytes(
                press(InputKey::Null, Modifiers::NONE),
                KeyboardModel::XtSet1
            ),
            vec![0x1D, 0x39]
        );
        assert_eq!(
            make_bytes(
                press(InputKey::Char(' '), Modifiers::SHIFT),
                KeyboardModel::XtSet1
            ),
            vec![0x2A, 0x39]
        );
    }

    #[test]
    fn maps_command_layer_without_normal_number_duplication() {
        assert_eq!(
            make_bytes(
                press(InputKey::Char('5'), Modifiers::SUPER),
                KeyboardModel::XtSet1
            ),
            vec![0x4C]
        );
        assert_eq!(
            make_bytes(
                press(InputKey::Enter, Modifiers::SUPER),
                KeyboardModel::XtSet1
            ),
            vec![0x52]
        );
    }

    #[test]
    fn maps_xt_command_layer_lock_print_and_control_break_chords() {
        let command = Modifiers::SUPER;
        let full_chord = Modifiers::SUPER | Modifiers::ALT | Modifiers::SHIFT | Modifiers::CONTROL;

        assert_eq!(
            make_bytes(press(InputKey::Char('*'), command), KeyboardModel::XtSet1),
            vec![0x37]
        );
        assert_eq!(
            make_bytes(press(InputKey::Char('\\'), command), KeyboardModel::XtSet1),
            vec![0x45]
        );
        assert_eq!(
            make_bytes(press(InputKey::Backspace, command), KeyboardModel::XtSet1),
            vec![0x46]
        );
        assert_eq!(
            make_bytes(
                press(InputKey::Char('\\'), full_chord),
                KeyboardModel::XtSet1
            ),
            vec![0x1D, 0x45]
        );
        assert_eq!(
            make_bytes(
                press(InputKey::Backspace, full_chord),
                KeyboardModel::XtSet1
            ),
            vec![0x1D, 0x46]
        );
        assert_eq!(
            make_bytes(
                press(InputKey::Char('5'), full_chord),
                KeyboardModel::XtSet1
            ),
            vec![0x4C]
        );
        assert_eq!(
            make_bytes(
                press(InputKey::Char('-'), full_chord),
                KeyboardModel::XtSet1
            ),
            vec![0x4A]
        );
        assert_eq!(
            make_bytes(
                press(InputKey::Char('='), full_chord),
                KeyboardModel::XtSet1
            ),
            vec![0x4E]
        );
    }

    #[test]
    fn emits_complete_at_sequences_for_special_keys() {
        let full_chord = Modifiers::SUPER | Modifiers::ALT | Modifiers::SHIFT | Modifiers::CONTROL;
        let print = map_key(
            &press(InputKey::PrintScreen, Modifiers::NONE),
            KeyboardModel::AtSet1,
        )[0];
        let pause = map_key(
            &press(InputKey::Char('\\'), full_chord),
            KeyboardModel::AtSet1,
        )[0];
        let sysrq = map_key(
            &press(InputKey::Char('`'), full_chord),
            KeyboardModel::AtSet1,
        )[0];

        assert_eq!(print.make.bytes(), &[0xE0, 0x2A, 0xE0, 0x37]);
        assert_eq!(print.break_sequence.bytes(), &[0xE0, 0xB7, 0xE0, 0xAA]);
        assert_eq!(pause.make.bytes(), &[0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5]);
        assert!(pause.break_sequence.bytes().is_empty());
        assert_eq!(sysrq.make.bytes(), &[0x54]);
    }

    #[test]
    fn normalizes_release_identities_and_command_fallback() {
        assert_eq!(source_id(InputKey::Enter), source_id(InputKey::Char('\r')));
        assert_eq!(source_id(InputKey::Char('`')), "`");
        assert_eq!(source_id(InputKey::Char('~')), "`");

        let mut keyboard = PcKeyboard::default();
        assert!(matches!(
            keyboard.handle(&press(InputKey::Enter, Modifiers::SUPER)).as_slice(),
            [PcEvent::Make(key)] if key.make.bytes() == [0x52]
        ));
        assert!(matches!(
            keyboard.handle(&InputEvent::new(InputKey::Modifier(ModifierKey::LeftSuper), Modifiers::NONE, InputKind::Release)).as_slice(),
            [PcEvent::Break(key)] if key.break_sequence.bytes() == [0xD2]
        ));
    }
}
