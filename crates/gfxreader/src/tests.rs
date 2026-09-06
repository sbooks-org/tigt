// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

use super::*;
use terminal::{Color, Terminal, COLS};

fn options(width: usize, region: Region) -> Options {
    Options {
        width,
        height: 200,
        font_offset: 0,
        glyph_count: 128,
        region: Some(region),
        expect: None,
    }
}

fn region(width: usize, height: usize) -> Region {
    Region {
        x: 0,
        y: 0,
        width,
        height,
    }
}

// Independent test font includes a duplicate H before the ASCII H. The analyzer
// must retain all alternatives and select the expected ASCII character.
fn font() -> Vec<u8> {
    let mut font = vec![0; 128 * 8];
    for (code, rows) in [
        (1, [0x81, 0x81, 0x81, 0xff, 0x81, 0x81, 0x81, 0]),
        (72, [0x81, 0x81, 0x81, 0xff, 0x81, 0x81, 0x81, 0]),
        (69, [0xff, 0x80, 0x80, 0xfc, 0x80, 0x80, 0xff, 0]),
        (76, [0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xff, 0]),
        (79, [0x7e, 0x81, 0x81, 0x81, 0x81, 0x81, 0x7e, 0]),
    ] {
        font[code * 8..code * 8 + 8].copy_from_slice(&rows);
    }
    font
}

fn encoded_mask(mask: u8) -> char {
    match mask {
        0 => ' ',
        21 => '▌',
        42 => '▐',
        63 => '█',
        1..=20 => char::from_u32(0x1fb00 + u32::from(mask) - 1).unwrap(),
        22..=41 => char::from_u32(0x1fb14 + u32::from(mask) - 22).unwrap(),
        _ => char::from_u32(0x1fb28 + u32::from(mask) - 43).unwrap(),
    }
}

fn encode(pixels: &[bool], width: usize, height: usize, scale: usize) -> String {
    let mut capture = String::from("boot text\r\n\x1b[?1049h\x1b[38;5;15;48;5;0m");
    for row in 0..height.div_ceil(3) {
        capture.push_str(&format!("\x1b[{};1H", row + 1));
        for column in 0..width * scale / 2 {
            let mut mask = 0;
            for dy in 0..3 {
                for dx in 0..2 {
                    let y = row * 3 + dy;
                    let x = (column * 2 + dx) / scale;
                    if y < height && pixels[y * width + x] {
                        mask |= 1 << (dy * 2 + dx);
                    }
                }
            }
            capture.push(encoded_mask(mask));
        }
    }
    capture
}

#[test]
fn ansi_replay_repaints_moves_erases_and_preserves_alternate_before_exit() {
    let terminal = Terminal::replay("old\x1b[?1049h\x1b[2J\x1b[HXXX\x1b[1;1H\x1b[31;44;1m▌\x1b[2C\x1b[38;5;1m▐\x1b[2;2H█\x1b[1D\x1b[K\x1b[?1049lrestored".as_bytes());
    assert!(terminal.errors.is_empty(), "{:?}", terminal.errors);
    let (cells, source) = terminal.cells();
    assert_eq!(source, "last_alternate_before_exit");
    assert_eq!(cells[0].character, '▌');
    assert_eq!(cells[0].style.colors(), ([255, 0, 0], [0, 0, 128]));
    assert_eq!(cells[3].character, '▐');
    assert_eq!(cells[3].style.colors().0, [128, 0, 0]); // Indexed is not ANSI bold-brightened.
    assert_eq!(cells[COLS + 1].character, ' ');
    assert!(cells[COLS + 1].written);
    assert_eq!(cells[COLS + 1].style.colors().1, [0, 0, 128]);
}

#[test]
fn explicit_erasure_defines_pixels_but_initial_blanks_remain_unknown() {
    let options = options(320, region(8, 8));
    let absent = analyze(b"\x1b[?1049h", &font(), &options).unwrap();
    assert!(!absent.report.success);
    assert_eq!(absent.report.missing_pixel_count, 64);
    assert_eq!(absent.rgba, vec![0; 64 * 4]);

    let erased = analyze(b"\x1b[?1049h\x1b[48;2;12;34;56m\x1b[2J", &font(), &options).unwrap();
    assert!(erased.report.success, "{:?}", erased.report.diagnostics);
    assert_eq!(erased.report.missing_pixel_count, 0);
    assert_eq!(erased.rgba, [12, 34, 56, 255].repeat(64));

    // An erase beginning inside the region must not invent earlier coverage.
    let partial = analyze(b"\x1b[3;3H\x1b[J", &font(), &options).unwrap();
    assert!(!partial.report.success);
    assert_eq!(partial.report.missing_pixel_count, 56);
    for y in 0..8 {
        for x in 0..8 {
            let alpha = partial.rgba[(y * 8 + x) * 4 + 3];
            assert_eq!(alpha, if y >= 6 && x >= 4 { 255 } else { 0 });
        }
    }
}

#[test]
fn erase_modes_paint_only_the_addressed_cells() {
    let cursor = COLS + 2;
    for (command, start, end) in [
        ("J", cursor, COLS * terminal::ROWS),
        ("1J", 0, cursor + 1),
        ("2J", 0, COLS * terminal::ROWS),
        ("K", cursor, COLS * 2),
        ("1K", COLS, cursor + 1),
        ("2K", COLS, COLS * 2),
        ("2X", cursor, cursor + 2),
    ] {
        let capture = format!("\x1b[2;3H\x1b[48;2;12;34;56m\x1b[{command}");
        let terminal = Terminal::replay(capture.as_bytes());
        assert!(terminal.errors.is_empty(), "{:?}", terminal.errors);
        let (cells, _) = terminal.cells();
        for (index, cell) in cells.iter().enumerate() {
            assert_eq!(
                cell.written,
                (start..end).contains(&index),
                "{command}: {index}"
            );
            if cell.written {
                assert_eq!(cell.character, ' ');
                assert_eq!(cell.style.colors().1, [12, 34, 56]);
            }
        }
    }
}

#[test]
fn sextants_boundaries_inverse_conceal_and_rgb_are_exact() {
    for (character, mask) in [
        (' ', 0),
        ('\u{1fb00}', 1),
        ('\u{1fb13}', 20),
        ('▌', 21),
        ('\u{1fb14}', 22),
        ('\u{1fb27}', 41),
        ('▐', 42),
        ('\u{1fb28}', 43),
        ('\u{1fb3b}', 62),
        ('█', 63),
    ] {
        assert_eq!(sextant_mask(character), Some(mask));
    }
    assert_eq!(sextant_mask('A'), None);
    let terminal = Terminal::replay(
        "\x1b[0;7m▌\x1b[0;38;2;1;2;3;48;5;200;7m▐\x1b[8m█\x1b[0;38:2::4:5:6m█".as_bytes(),
    );
    assert!(terminal.errors.is_empty(), "{:?}", terminal.errors);
    let (cells, _) = terminal.cells();
    assert_eq!(cells[0].style.colors(), ([0, 0, 0], [192, 192, 192]));
    assert_eq!(cells[1].style.foreground, Color::Rgb([1, 2, 3]));
    assert_eq!(cells[1].style.colors(), ([255, 0, 215], [1, 2, 3]));
    assert_eq!(cells[2].style.colors(), ([1, 2, 3], [1, 2, 3]));
    assert_eq!(cells[3].style.colors().0, [4, 5, 6]);
}

#[test]
fn color_polarity_does_not_change_rom_match() {
    let rom = [[0x80, 0x40, 0x20, 0x10, 8, 4, 2, 1]];
    for invert in [false, true] {
        let mut tile = [Some([0; 3]); 64];
        for y in 0..8 {
            for x in 0..8 {
                tile[y * 8 + x] = Some(if (x == y) ^ invert { [255; 3] } else { [0; 3] });
            }
        }
        let glyph = classify(0, 0, &tile, &rom);
        assert_eq!(glyph.status, "matched");
        assert_eq!(
            glyph.alternatives[0].foreground_rgb,
            Some(if invert { [0; 3] } else { [255; 3] })
        );
    }
    let blank = classify(0, 0, &[Some([7; 3]); 64], &[[0; 8], [255; 8], [0; 8]]);
    assert_eq!(
        blank
            .alternatives
            .iter()
            .map(|m| m.code)
            .collect::<Vec<_>>(),
        [0, 1, 2]
    );
}

#[test]
fn corruption_reports_nearest_glyph_exact_pixel_and_three_colors_reject() {
    let mut tile = [Some([0; 3]); 64];
    tile[19] = Some([255; 3]);
    let glyph = classify(16, 24, &tile, &[[0; 8]]);
    assert_eq!(glyph.status, "rom_mismatch");
    let nearest = glyph.nearest.unwrap();
    assert_eq!(nearest.distance, 1);
    assert_eq!(nearest.error_pixels, [[19, 26]]);
    tile[20] = Some([1, 2, 3]);
    assert_eq!(classify(0, 0, &tile, &[[0; 8]]).status, "too_many_colors");
}

#[test]
fn expected_text_resolves_duplicate_rom_candidates() {
    let font = font();
    let mut pixels = vec![false; 40 * 8];
    for (column, code) in b"HELLO".iter().enumerate() {
        for y in 0..8 {
            for x in 0..8 {
                pixels[y * 40 + column * 8 + x] =
                    font[usize::from(*code) * 8 + y] & (0x80 >> x) != 0;
            }
        }
    }
    let mut options = options(320, region(40, 8));
    options.expect = Some("HELLO".into());
    let result = analyze(encode(&pixels, 40, 8, 1).as_bytes(), &font, &options).unwrap();
    assert!(result.report.success, "{:?}", result.report.diagnostics);
    assert_eq!(result.report.decoded_text, "HELLO\n");
    assert!(result.report.glyphs[0]
        .alternatives
        .iter()
        .any(|m| m.code == 1));
    assert_eq!(result.report.glyphs[0].selected_code, Some(72));
    options.expect = Some("WORLD".into());
    let result = analyze(encode(&pixels, 40, 8, 1).as_bytes(), &font, &options).unwrap();
    assert_eq!(result.report.expected_found, Some(false));
    assert!(!result.report.success);
}

#[test]
fn all_supported_dimensions_include_final_two_scanlines() {
    for width in [160, 320, 640] {
        let pixels = vec![false; width * 200];
        let capture = encode(&pixels, width, 200, if width == 160 { 2 } else { 1 });
        let mut options = options(width, region(width, 200));
        let result = analyze(capture.as_bytes(), &font(), &options).unwrap();
        assert!(
            result.report.success,
            "width {width}: {:?}",
            result.report.diagnostics
        );
        assert_eq!(result.rgba.len(), width * 200 * 4);
        assert_eq!(result.report.glyphs.len(), width / 8 * 25);
        let final_row = capture.rfind("\x1b[67;1H").unwrap();
        let result = analyze(&capture.as_bytes()[..final_row], &font(), &options).unwrap();
        assert!(!result.report.success);
        assert_eq!(result.report.incomplete_scanlines, [198, 199]);
        assert_eq!(result.report.missing_pixel_count, width * 2);
        assert_eq!(&result.rgba[result.rgba.len() - 4..], &[0, 0, 0, 0]);
        options.width = 480;
        assert!(analyze(capture.as_bytes(), &font(), &options).is_err());
    }
}

#[test]
fn doubled_pixels_and_nonsextant_unicode_are_not_silently_accepted() {
    let options = options(160, region(8, 8));
    let capture = encode(&[false; 64], 8, 8, 2);
    let capture = format!("{capture}\x1b[H▌");
    let result = analyze(capture.as_bytes(), &font(), &options).unwrap();
    assert!(result
        .report
        .diagnostics
        .iter()
        .any(|d| d.kind == "unequal_horizontal_pair"));
    let result = analyze(format!("{capture}\x1b[HA").as_bytes(), &font(), &options).unwrap();
    assert!(result
        .report
        .diagnostics
        .iter()
        .any(|d| d.kind == "invalid_sextant"));
    let result = analyze(
        format!("{capture}\x1b[H█\u{0301}").as_bytes(),
        &font(),
        &options,
    )
    .unwrap();
    assert!(result
        .report
        .diagnostics
        .iter()
        .any(|d| d.kind == "invalid_sextant"));
}

#[test]
fn font_range_and_crop_alignment_are_checked_before_replay() {
    let mut options = options(320, region(8, 8));
    assert!(analyze(b"", &[0; 1023], &options).is_err());
    options.font_offset = usize::MAX;
    assert!(options.validate(1024).is_err());
    options.font_offset = 0;
    options.region = Some(Region {
        x: 1,
        y: 0,
        width: 8,
        height: 8,
    });
    assert!(options.validate(1024).is_err());
    options.region = Some(Region {
        x: 320,
        y: 0,
        width: 8,
        height: 8,
    });
    assert!(options.validate(1024).is_err());
}
