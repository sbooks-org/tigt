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
fn physical_repeats_preserve_shared_modifiers() {
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
            vec![(0x30, true)],
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
fn repeat_uses_held_mapping_and_cannot_resurrect_released_keys() {
    let mut keyboard = PcKeyboard::new(KeyboardModel::AtSet1);
    let key = InputKey::Left;
    let repeat = InputEvent::new(key, Modifiers::NONE, InputKind::Repeat);
    assert_eq!(physical(keyboard.handle(&repeat)), []);
    assert_eq!(
        physical(keyboard.handle(&InputEvent::new(key, Modifiers::SUPER, InputKind::Press))),
        [(0x4b, true)]
    );
    // Loss of the modifier in the repeat report must not change keypad Left
    // into enhanced Left or acquire another ownership reference.
    assert_eq!(physical(keyboard.handle(&repeat)), [(0x4b, true)]);
    assert_eq!(physical(keyboard.handle(&repeat)), [(0x4b, true)]);
    assert_eq!(
        physical(keyboard.handle(&InputEvent::new(
            InputKey::Modifier(ModifierKey::LeftSuper),
            Modifiers::NONE,
            InputKind::Release
        ))),
        [(0x4b, false)]
    );
    assert_eq!(physical(keyboard.handle(&repeat)), []);
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

#[test]
fn function_aliases_preserve_physical_and_wire_release_identity() {
    for model in [KeyboardModel::XtSet1, KeyboardModel::AtSet1] {
        for (number, modifiers, expected) in [
            (13, Modifiers::NONE, vec![0x2A, 0x37]),
            (13, Modifiers::SHIFT, vec![0x37]),
            (14, Modifiers::NONE, vec![0x46]),
            (15, Modifiers::NONE, vec![0x1D, 0x45]),
            (15, Modifiers::CONTROL, vec![0x1D, 0x46]),
        ] {
            let mut keyboard = PcKeyboard::new(model);
            let key = InputKey::Function(number);
            let make = keyboard.handle(&InputEvent::new(key, modifiers, InputKind::Press));
            assert_eq!(
                physical(make.clone()),
                expected
                    .iter()
                    .map(|&code| (u16::from(code), true))
                    .collect::<Vec<_>>(),
                "{model:?} {key:?} {modifiers:?}"
            );
            let make_bytes = make
                .iter()
                .flat_map(|event| match event {
                    PcEvent::Make(key) => key.make.bytes(),
                    PcEvent::Break(_) => panic!("unexpected break"),
                })
                .copied()
                .collect::<Vec<_>>();
            assert_eq!(make_bytes, expected);
            assert_eq!(
                physical(keyboard.handle(&InputEvent::new(
                    key,
                    Modifiers::NONE,
                    InputKind::Repeat
                ))),
                if number == 13 {
                    vec![(0x37, true)]
                } else {
                    vec![]
                }
            );
            // Release modifiers need not match the press: release the held chord,
            // not whichever alias those current modifiers would select.
            let release = keyboard.handle(&InputEvent::new(
                key,
                Modifiers::SHIFT | Modifiers::CONTROL,
                InputKind::Release,
            ));
            assert_eq!(
                physical(release.clone()),
                expected
                    .iter()
                    .map(|&code| (u16::from(code), false))
                    .collect::<Vec<_>>()
            );
            let break_bytes = release
                .iter()
                .flat_map(|event| match event {
                    PcEvent::Break(key) => key.break_sequence.bytes(),
                    PcEvent::Make(_) => panic!("unexpected make"),
                })
                .copied()
                .collect::<Vec<_>>();
            assert_eq!(
                break_bytes,
                expected.iter().map(|code| code | 0x80).collect::<Vec<_>>()
            );
            assert_eq!(keyboard.release_all(), []);
        }
    }
}

#[test]
fn shifted_f13_temporarily_suppresses_physical_shift_and_tracks_its_release() {
    for model in [KeyboardModel::XtSet1, KeyboardModel::AtSet1] {
        for (shift, code) in [
            (ModifierKey::LeftShift, 0x2A),
            (ModifierKey::RightShift, 0x36),
        ] {
            for release_shift_first in [false, true] {
                let mut keyboard = PcKeyboard::new(model);
                let shift = InputKey::Modifier(shift);
                assert_eq!(
                    physical(keyboard.handle(&InputEvent::new(
                        shift,
                        Modifiers::SHIFT,
                        InputKind::Press
                    ))),
                    [(code, true)]
                );
                assert_eq!(
                    physical(keyboard.handle(&InputEvent::new(
                        InputKey::Function(13),
                        Modifiers::SHIFT,
                        InputKind::Press,
                    ))),
                    [(code, false), (0x37, true)]
                );
                if release_shift_first {
                    assert_eq!(
                        keyboard.handle(&InputEvent::new(
                            shift,
                            Modifiers::NONE,
                            InputKind::Release
                        )),
                        []
                    );
                }
                assert_eq!(
                    physical(keyboard.handle(&InputEvent::new(
                        InputKey::Function(13),
                        Modifiers::NONE,
                        InputKind::Release,
                    ))),
                    if release_shift_first {
                        vec![(0x37, false)]
                    } else {
                        vec![(0x37, false), (code, true)]
                    }
                );
                if !release_shift_first {
                    assert_eq!(
                        physical(keyboard.handle(&InputEvent::new(
                            shift,
                            Modifiers::NONE,
                            InputKind::Release
                        ))),
                        [(code, false)]
                    );
                }
                assert_eq!(keyboard.release_all(), []);
            }
        }
    }
}

#[test]
fn alias_modifier_references_survive_chord_changes_and_other_held_keys() {
    for model in [KeyboardModel::XtSet1, KeyboardModel::AtSet1] {
        let mut keyboard = PcKeyboard::new(model);
        for (key, modifiers, kind, expected) in [
            (
                InputKey::Function(15),
                Modifiers::NONE,
                InputKind::Press,
                vec![(0x1D, true), (0x45, true)],
            ),
            (
                InputKey::Char('c'),
                Modifiers::CONTROL,
                InputKind::Press,
                vec![(0x2E, true)],
            ),
            (
                InputKey::Function(15),
                Modifiers::CONTROL,
                InputKind::Press,
                vec![(0x45, false), (0x46, true)],
            ),
            (
                InputKey::Function(14),
                Modifiers::NONE,
                InputKind::Press,
                vec![],
            ),
            (
                InputKey::Function(15),
                Modifiers::NONE,
                InputKind::Release,
                vec![],
            ),
            (
                InputKey::Function(14),
                Modifiers::NONE,
                InputKind::Release,
                vec![(0x46, false)],
            ),
            (
                InputKey::Char('c'),
                Modifiers::NONE,
                InputKind::Release,
                vec![(0x1D, false), (0x2E, false)],
            ),
        ] {
            assert_eq!(
                physical(keyboard.handle(&InputEvent::new(key, modifiers, kind))),
                expected,
                "{model:?} {key:?} {kind:?}"
            );
        }
        assert_eq!(keyboard.release_all(), []);
        assert_eq!(
            keyboard.handle(&InputEvent::new(
                InputKey::Char('\u{f746}'),
                Modifiers::NONE,
                InputKind::Press
            )),
            []
        );
        assert_eq!(
            physical(keyboard.handle(&InputEvent::new(
                InputKey::Function(1),
                Modifiers::NONE,
                InputKind::Press
            ))),
            [(0x3B, true)]
        );
    }
}

#[test]
fn enhanced_right_modifiers_keep_their_side_across_shared_chords() {
    for (right, left, modifiers, code) in [
        (
            ModifierKey::RightControl,
            ModifierKey::LeftControl,
            Modifiers::CONTROL,
            0x1D,
        ),
        (
            ModifierKey::RightAlt,
            ModifierKey::LeftAlt,
            Modifiers::ALT,
            0x38,
        ),
    ] {
        let mut keyboard = PcKeyboard::new(KeyboardModel::AtSet1);
        let right = InputKey::Modifier(right);
        let left = InputKey::Modifier(left);
        let press = InputEvent::new(right, modifiers, InputKind::Press);
        assert_eq!(pc_xt_keyboard::map_key(&press, KeyboardModel::XtSet1), []);
        assert!(matches!(
            keyboard.handle(&press).as_slice(),
            [PcEvent::Make(key)]
                if key.physical == 0x100 | u16::from(code)
                    && key.make.bytes() == [0xE0, code]
        ));
        assert_eq!(
            keyboard.handle(&InputEvent::new(right, modifiers, InputKind::Repeat)),
            []
        );
        assert_eq!(
            physical(keyboard.handle(&InputEvent::new(
                InputKey::Char('a'),
                modifiers,
                InputKind::Press
            ))),
            [(0x1E, true)]
        );
        // An explicit left-side press must not be folded into the right-side chord.
        for (kind, down) in [(InputKind::Press, true), (InputKind::Release, false)] {
            assert_eq!(
                physical(keyboard.handle(&InputEvent::new(left, modifiers, kind))),
                [(u16::from(code), down)]
            );
        }
        assert_eq!(
            keyboard.handle(&InputEvent::new(right, Modifiers::NONE, InputKind::Release)),
            []
        );
        let release = keyboard.handle(&InputEvent::new(
            InputKey::Char('a'),
            Modifiers::NONE,
            InputKind::Release,
        ));
        assert!(matches!(
            release.as_slice(),
            [PcEvent::Break(modifier), PcEvent::Break(key)]
                if modifier.physical == 0x100 | u16::from(code)
                    && modifier.break_sequence.bytes() == [0xE0, code | 0x80]
                    && key.physical == 0x1E
        ));
    }
}
