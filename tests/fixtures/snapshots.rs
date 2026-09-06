// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

use super::Fixture;
use serde_json::{Value, json};
use std::{
    fs::{self, File},
    path::Path,
};
use tigt_gfxreader::terminal::Terminal;

const EXTENSIONS: [&str; 7] = ["png", "utf8", "ascii", "cp437", "ansi", "cells", "json"];
const PALETTE: [u32; 16] = [
    0, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa, 0x555555, 0x5555ff,
    0x55ff55, 0x55ffff, 0xff5555, 0xff55ff, 0xffff55, 0xffffff,
];

#[derive(Clone, Copy)]
struct Cell {
    character: char,
    glyph: u8,
    fg: u32,
    bg: u32,
    flags: u32,
}

fn scene() -> [Cell; 8] {
    [
        Cell {
            character: 'A',
            glyph: b'A',
            fg: 0x123456,
            bg: 0x654321,
            flags: 1,
        },
        Cell {
            character: 'é',
            glyph: 0x82,
            fg: 0x00aa00,
            bg: 0x0000aa,
            flags: 0,
        },
        Cell {
            character: '█',
            glyph: 0xdb,
            fg: 0xffffff,
            bg: 0,
            flags: 2,
        },
        Cell {
            character: ' ',
            glyph: b' ',
            fg: 0xffffff,
            bg: 0,
            flags: 0,
        },
        Cell {
            character: ' ',
            glyph: b' ',
            fg: 0,
            bg: 0xffffff,
            flags: 0,
        },
        Cell {
            character: 'B',
            glyph: b'B',
            fg: 0xaa0000,
            bg: 0x00aaaa,
            flags: 0,
        },
        Cell {
            character: '┌',
            glyph: 0xda,
            fg: 0x555555,
            bg: 0xaaaaaa,
            flags: 0,
        },
        Cell {
            character: ' ',
            glyph: b' ',
            fg: 0xffffff,
            bg: 0,
            flags: 0,
        },
    ]
}

fn rgb(color: u32) -> [u8; 3] {
    [(color >> 16) as u8, (color >> 8) as u8, color as u8]
}

fn nearest(color: u32) -> u8 {
    PALETTE
        .iter()
        .enumerate()
        .min_by_key(|(_, candidate)| {
            rgb(color)
                .into_iter()
                .zip(rgb(**candidate))
                .map(|(a, b)| (i32::from(a) - i32::from(b)).pow(2))
                .sum::<i32>()
        })
        .unwrap()
        .0 as u8
}

fn attributes(cells: &[Cell], columns: usize, rows: usize) -> Value {
    json!({
        "schema_version": 1, "kind": "text",
        "dimensions": {"width": columns, "height": rows, "pixel_width": 1},
        "overscan": {"color": 0x102030, "left": 1, "right": 2, "top": 3, "bottom": 4},
        "columns": columns, "rows": rows,
        "cells": cells.iter().map(|cell| json!({
            "codepoint": cell.character as u32, "foreground": cell.fg,
            "background": cell.bg, "flags": cell.flags,
            "legacy": {"glyph": cell.glyph, "attribute": nearest(cell.fg) | (nearest(cell.bg) << 4)},
        })).collect::<Vec<_>>()
    })
}

fn text_pixels(cells: &[Cell], columns: usize, height: usize, patterned: bool) -> Vec<u8> {
    let mut pixels = Vec::new();
    for row in cells.chunks_exact(columns) {
        for y in 0..height {
            for cell in row {
                let glyph = usize::from(cell.glyph);
                let bits = if patterned && glyph != 32 {
                    ((glyph * 37 + y * 19) ^ (glyph << (y % 3))) as u8
                } else {
                    0
                };
                for x in 0..8 {
                    let foreground =
                        bits & (0x80 >> x) != 0 || (cell.flags != 0 && y + 1 == height);
                    pixels.extend_from_slice(&rgb(if foreground { cell.fg } else { cell.bg }));
                }
            }
        }
    }
    pixels
}

fn png(path: &Path) -> (u32, u32, Vec<u8>) {
    let mut decoder = png::Decoder::new(File::open(path).unwrap())
        .read_info()
        .unwrap();
    let mut bytes = vec![0; decoder.output_buffer_size()];
    let frame = decoder.next_frame(&mut bytes).unwrap();
    assert_eq!(frame.color_type, png::ColorType::Rgb, "{}", path.display());
    assert_eq!(frame.bit_depth, png::BitDepth::Eight);
    bytes.truncate(frame.buffer_size());
    (frame.width, frame.height, bytes)
}

fn json_file(path: &Path) -> Value {
    serde_json::from_slice(&fs::read(path).unwrap()).unwrap()
}

fn check_ansi(path: &Path, expected: &[Cell], columns: usize) {
    let bytes = fs::read(path).unwrap();
    let lines: Vec<_> = bytes.split(|byte| *byte == b'\n').collect();
    assert_eq!(lines.len(), expected.len() / columns + 1);
    assert!(lines.last().unwrap().is_empty());
    for (line, expected) in lines.into_iter().zip(expected.chunks_exact(columns)) {
        let trimmed = expected
            .iter()
            .rposition(|cell| cell.character != ' ')
            .map_or(0, |index| index + 1);
        let expected = &expected[..trimmed];
        // Replay each portable text line independently; a terminal's ONLCR
        // translation, not this encoding, supplies carriage returns.
        let mut input = line.to_vec();
        input.push(b'Z'); // row reset must stop the previous cell's SGR leaking
        let terminal = Terminal::replay(&input);
        assert!(terminal.errors.is_empty(), "{:?}", terminal.errors);
        let (cells, _) = terminal.cells();
        for (actual, expected) in cells.iter().zip(expected) {
            assert_eq!(actual.character, expected.character);
            assert_eq!(
                actual.style.foreground.rgb(actual.style.bold),
                rgb(expected.fg)
            );
            assert_eq!(actual.style.background.rgb(false), rgb(expected.bg));
            assert_eq!(actual.style.underline, expected.flags != 0);
        }
        assert_eq!(cells[trimmed].character, 'Z');
        assert_eq!(cells[trimmed].style, Default::default());
    }
}

#[test]
fn c_and_safe_rust_snapshot_consumers_match_native_pixels_encodings_and_signals() {
    let fixture = Fixture::build("snapshot_fixture", true, false);
    let c = fixture.path("c");
    let rust = fixture.path("rust");
    fs::create_dir(&c).unwrap();
    fs::create_dir(&rust).unwrap();
    fixture.capture_program(
        &fixture.binary,
        "snapshot-c",
        &[c.to_str().unwrap()],
        |_, _| Ok(()),
    );
    fixture.capture_program(
        Path::new(env!("CARGO_BIN_EXE_tigt-snapshot-fixture")),
        "snapshot-rust",
        &[rust.to_str().unwrap()],
        |_, _| Ok(()),
    );

    // Both actual public API consumers must agree, but agreement alone is not
    // an oracle: below every successful output is also checked against the scene.
    for name in ["text", "signal", "blank", "unmapped"] {
        for extension in EXTENSIONS {
            if name == "unmapped" && extension == "png" {
                continue;
            }
            let name = format!("{name}.{extension}");
            match extension {
                "png" => assert_eq!(png(&c.join(&name)), png(&rust.join(&name)), "{name}"),
                "json" => assert_eq!(
                    json_file(&c.join(&name)),
                    json_file(&rust.join(&name)),
                    "{name}"
                ),
                _ => assert_eq!(
                    fs::read(c.join(&name)).unwrap(),
                    fs::read(rust.join(&name)).unwrap(),
                    "{name}"
                ),
            }
        }
    }
    let cells = scene();
    let blank = [Cell {
        character: ' ',
        glyph: b' ',
        fg: 0x123456,
        bg: 0x654321,
        flags: 0,
    }; 6];
    let unmapped = [Cell {
        character: 'λ',
        glyph: b'?',
        fg: 0x123456,
        bg: 0x654321,
        flags: 0,
    }];
    for directory in [&c, &rust] {
        for name in ["text", "signal"] {
            assert_eq!(
                fs::read(directory.join(format!("{name}.utf8"))).unwrap(),
                "Aé█\n B┌\n".as_bytes()
            );
            assert_eq!(
                fs::read(directory.join(format!("{name}.ascii"))).unwrap(),
                b"A??\n B?\n"
            );
            assert_eq!(
                fs::read(directory.join(format!("{name}.cp437"))).unwrap(),
                b"A\x82\xdb\n B\xda\n"
            );
            let pairs: Vec<_> = cells
                .iter()
                .flat_map(|cell| [cell.glyph, nearest(cell.fg) | (nearest(cell.bg) << 4)])
                .collect();
            assert_eq!(
                fs::read(directory.join(format!("{name}.cells"))).unwrap(),
                pairs
            );
            assert_eq!(
                json_file(&directory.join(format!("{name}.json"))),
                attributes(&cells, 4, 2)
            );
            assert_eq!(
                png(&directory.join(format!("{name}.png"))),
                (32, 16, text_pixels(&cells, 4, 8, true))
            );
            check_ansi(&directory.join(format!("{name}.ansi")), &cells, 4);
        }
        for height in [1, 32] {
            assert_eq!(
                png(&directory.join(format!("height{height}.png"))),
                (32, 2 * height as u32, text_pixels(&cells, 4, height, false))
            );
        }
        for extension in ["utf8", "ascii", "cp437"] {
            assert_eq!(
                fs::read(directory.join(format!("blank.{extension}"))).unwrap(),
                b"\n\n"
            );
        }
        assert_eq!(
            fs::read(directory.join("blank.cells")).unwrap(),
            [b' ', nearest(blank[0].fg) | (nearest(blank[0].bg) << 4)].repeat(6)
        );
        assert_eq!(
            json_file(&directory.join("blank.json")),
            attributes(&blank, 3, 2)
        );
        assert_eq!(
            png(&directory.join("blank.png")),
            (24, 16, text_pixels(&blank, 3, 8, false))
        );
        check_ansi(&directory.join("blank.ansi"), &blank, 3);
        assert_eq!(
            fs::read(directory.join("unmapped.utf8")).unwrap(),
            "λ\n".as_bytes()
        );
        for extension in ["ascii", "cp437"] {
            assert_eq!(
                fs::read(directory.join(format!("unmapped.{extension}"))).unwrap(),
                b"?\n"
            );
        }
        assert_eq!(
            fs::read(directory.join("unmapped.cells")).unwrap(),
            [
                b'?',
                nearest(unmapped[0].fg) | (nearest(unmapped[0].bg) << 4)
            ]
        );
        assert_eq!(
            json_file(&directory.join("unmapped.json")),
            attributes(&unmapped, 1, 1)
        );
        check_ansi(&directory.join("unmapped.ansi"), &unmapped, 1);
        assert_eq!(
            fs::read(directory.join("resumed.utf8")).unwrap(),
            "λ\n".as_bytes()
        );
        for number in [1, 2] {
            assert_eq!(
                fs::read(directory.join(format!("environment{number}.utf8"))).unwrap(),
                b"E\n"
            );
        }
        for width in [320, 640] {
            for pixel_width in [1, 2] {
                let mut pixels = Vec::new();
                for y in 0..200 {
                    for logical_x in 0..width / pixel_width {
                        let x = logical_x * pixel_width;
                        pixels.extend_from_slice(&[
                            (x & 255) as u8,
                            y as u8,
                            ((x * 3 + y * 5) & 255) as u8,
                        ]);
                    }
                }
                let actual = png(&directory.join(format!("bitmap-{width}-{pixel_width}.png")));
                assert_eq!(actual, (width / pixel_width, 200, pixels));
                assert_eq!(
                    json_file(&directory.join(format!("bitmap-{width}-{pixel_width}.json"))),
                    json!({
                        "schema_version": 1, "kind": "bitmap", "dimensions": {"width": width / pixel_width, "height": 200, "pixel_width": pixel_width},
                        "overscan": {"color": 0x102030, "left": 1, "right": 2, "top": 3, "bottom": 4}
                    })
                );
            }
        }
        assert_eq!(
            png(&directory.join("bitmap-signal.png")),
            png(&directory.join("bitmap-640-2.png"))
        );
    }
}
