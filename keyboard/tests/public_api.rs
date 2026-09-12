use pc_xt_keyboard::{
    InputEvent, InputKey, InputKind, KeyboardModel, ModifierKey, Modifiers, PcEvent, PcKeyboard,
};

#[test]
fn consumer_receives_transport_neutral_make_and_break_sequences() {
    let mut keyboard = PcKeyboard::new(KeyboardModel::XtSet1);
    let press = InputEvent::new(InputKey::Char('a'), Modifiers::NONE, InputKind::Press);
    let release = InputEvent::new(InputKey::Char('a'), Modifiers::NONE, InputKind::Release);

    assert!(matches!(
        keyboard.handle(&press).as_slice(),
        [PcEvent::Make(key)] if key.make.bytes() == [0x1E]
    ));
    assert!(matches!(
        keyboard.handle(&release).as_slice(),
        [PcEvent::Break(key)] if key.break_sequence.bytes() == [0x9E]
    ));
}

fn physical(events: Vec<PcEvent>) -> Vec<(u16, bool)> {
    events
        .into_iter()
        .map(|event| match event {
            PcEvent::Make(key) => (key.physical, true),
            PcEvent::Break(key) => (key.physical, false),
        })
        .collect()
}

#[test]
fn consumer_receives_physical_identity_for_both_models() {
    for (model, print, pause) in [
        (KeyboardModel::XtSet1, 0x37, vec![0x1D, 0x45]),
        (KeyboardModel::AtSet1, 0x137, vec![0x145]),
    ] {
        let mut keyboard = PcKeyboard::new(model);
        for (key, expected) in [
            (InputKey::Char('a'), vec![0x1E]),
            (InputKey::Char('\t'), vec![0x0F]),
            (InputKey::Function(19), vec![0x37]),
            (InputKey::PrintScreen, vec![print]),
            (InputKey::Pause, pause),
        ] {
            for (kind, down) in [(InputKind::Press, true), (InputKind::Release, false)] {
                assert_eq!(
                    physical(keyboard.handle(&InputEvent::new(key, Modifiers::NONE, kind))),
                    expected.iter().map(|&key| (key, down)).collect::<Vec<_>>(),
                    "{model:?} {key:?} {kind:?}"
                );
            }
        }
    }
}

#[test]
fn physical_transitions_preserve_shared_modifiers_and_ignore_repeat() {
    let mut keyboard = PcKeyboard::default();
    let shift = InputKey::Modifier(ModifierKey::LeftShift);
    for (key, modifiers, kind, expected) in [
        (
            shift,
            Modifiers::SHIFT,
            InputKind::Press,
            vec![(0x2A, true)],
        ),
        (
            InputKey::Char('A'),
            Modifiers::SHIFT,
            InputKind::Press,
            vec![(0x1E, true)],
        ),
        (
            InputKey::Char('B'),
            Modifiers::SHIFT,
            InputKind::Press,
            vec![(0x30, true)],
        ),
        (shift, Modifiers::NONE, InputKind::Release, vec![]),
        (
            InputKey::Char('a'),
            Modifiers::NONE,
            InputKind::Release,
            vec![(0x1E, false)],
        ),
        (
            InputKey::Char('B'),
            Modifiers::SHIFT,
            InputKind::Repeat,
            vec![],
        ),
        (
            InputKey::Char('b'),
            Modifiers::NONE,
            InputKind::Release,
            vec![(0x2A, false), (0x30, false)],
        ),
    ] {
        assert_eq!(
            physical(keyboard.handle(&InputEvent::new(key, modifiers, kind))),
            expected
        );
    }
}

#[test]
fn command_release_releases_enhanced_pause_even_without_wire_break_bytes() {
    let mut keyboard = PcKeyboard::new(KeyboardModel::AtSet1);
    let pause = InputEvent::new(
        InputKey::Char('\\'),
        Modifiers::SUPER | Modifiers::CONTROL | Modifiers::SHIFT | Modifiers::ALT,
        InputKind::Press,
    );
    assert_eq!(physical(keyboard.handle(&pause)), [(0x145, true)]);
    assert_eq!(
        physical(keyboard.handle(&InputEvent::new(
            InputKey::Modifier(ModifierKey::LeftSuper),
            Modifiers::NONE,
            InputKind::Release,
        ))),
        [(0x145, false)]
    );
    assert_eq!(
        physical(keyboard.handle(&InputEvent {
            kind: InputKind::Release,
            ..pause
        })),
        []
    );
}

#[test]
fn at_navigation_is_extended_but_xt_and_command_keypad_are_not() {
    for (model, modifiers, extended) in [
        (KeyboardModel::XtSet1, Modifiers::NONE, false),
        (KeyboardModel::AtSet1, Modifiers::NONE, true),
        (KeyboardModel::AtSet1, Modifiers::SUPER, false),
    ] {
        let mut keyboard = PcKeyboard::new(model);
        for (key, position) in [
            (InputKey::Home, 0x47),
            (InputKey::Up, 0x48),
            (InputKey::PageUp, 0x49),
            (InputKey::Left, 0x4B),
            (InputKey::Right, 0x4D),
            (InputKey::End, 0x4F),
            (InputKey::Down, 0x50),
            (InputKey::PageDown, 0x51),
            (InputKey::Insert, 0x52),
            (InputKey::Delete, 0x53),
        ] {
            let identity = u16::from(position) | if extended { 0x100 } else { 0 };
            let press = keyboard.handle(&InputEvent::new(key, modifiers, InputKind::Press));
            let [PcEvent::Make(mapped)] = press.as_slice() else {
                panic!("expected one navigation press");
            };
            assert_eq!(mapped.physical, identity);
            let expected_make = if extended {
                vec![0xE0, position]
            } else {
                vec![position]
            };
            assert_eq!(mapped.make.bytes(), expected_make);
            let release =
                keyboard.handle(&InputEvent::new(key, Modifiers::NONE, InputKind::Release));
            let [PcEvent::Break(mapped)] = release.as_slice() else {
                panic!("expected one navigation release");
            };
            assert_eq!(mapped.physical, identity);
            let expected_break = if extended {
                vec![0xE0, position | 0x80]
            } else {
                vec![position | 0x80]
            };
            assert_eq!(mapped.break_sequence.bytes(), expected_break);
        }
    }
}

#[test]
fn enhanced_navigation_and_command_keypad_have_independent_holds() {
    let mut keyboard = PcKeyboard::new(KeyboardModel::AtSet1);
    assert_eq!(
        physical(keyboard.handle(&InputEvent::new(
            InputKey::Insert,
            Modifiers::NONE,
            InputKind::Press,
        ))),
        [(0x152, true)]
    );
    assert_eq!(
        physical(keyboard.handle(&InputEvent::new(
            InputKey::Enter,
            Modifiers::SUPER,
            InputKind::Press,
        ))),
        [(0x52, true)]
    );
    assert_eq!(
        physical(keyboard.handle(&InputEvent::new(
            InputKey::Modifier(ModifierKey::LeftSuper),
            Modifiers::NONE,
            InputKind::Release,
        ))),
        [(0x52, false)]
    );
    assert_eq!(
        physical(keyboard.handle(&InputEvent::new(
            InputKey::Insert,
            Modifiers::NONE,
            InputKind::Release,
        ))),
        [(0x152, false)]
    );
}
