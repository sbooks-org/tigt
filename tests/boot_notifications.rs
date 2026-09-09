// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

use serde_json::Value;
use std::{
    io::{Read, Seek, SeekFrom},
    os::fd::AsFd,
};
use tigt::{
    TextCell, cp437_codepoint,
    presenter::{
        Boundary, BoundaryKind, Config, Cursor, Encoding, Frame, Mode, Notification, Presenter,
        RefreshRate, Reversibility, TEXT_BOLD, TEXT_REVERSE, VIDEO_DISABLED, VIDEO_MEMORY_CHANGED,
    },
    video::{AdapterKind, Frame as VideoFrame, VideoAdapter},
};
use tigt_gfxreader::terminal::{COLS, Terminal};

fn hex(value: &str) -> Vec<u8> {
    assert_eq!(value.len() % 2, 0);
    value
        .as_bytes()
        .chunks_exact(2)
        .map(|pair| u8::from_str_radix(std::str::from_utf8(pair).unwrap(), 16).unwrap())
        .collect()
}

fn number(event: &Value, key: &str) -> u16 {
    event[key].as_u64().unwrap().try_into().unwrap()
}

// Replay the observer, not DOS call arguments as output. BIOS teletype is the
// single notification source; raw INT21 records remain in the fixture so its
// nested INT10 writes are not double-counted. Frames alone provide the text.
fn replay(trace: &str, notifications: bool, mut observe: impl FnMut(&Frame<'_>)) -> Vec<u8> {
    let mut destination = tempfile::tempfile().unwrap();
    {
        let mut presenter = Presenter::new(
            destination.as_fd(),
            Config {
                mode: Mode::Glass,
                encoding: Encoding::Ascii,
                reversibility: Reversibility::OneWay,
            },
        )
        .unwrap();
        let mut decoders = [
            VideoAdapter::new(AdapterKind::Cga).unwrap(),
            VideoAdapter::new(AdapterKind::Mda).unwrap(),
        ];
        let mut vram = vec![0; 16384];
        let mut submitted_vram: Option<Vec<u8>> = None;
        for line in trace.lines() {
            let event: Value = serde_json::from_str(line).unwrap();
            match event["kind"].as_str().unwrap() {
                "int" => {
                    if !notifications {
                        continue;
                    }
                    if number(&event, "vector") != 0x10 {
                        continue;
                    }
                    let regs = event["regs"].as_array().unwrap();
                    let ax = regs[0].as_u64().unwrap() as u16;
                    if ax >> 8 != 0x0e || matches!(ax as u8, 7 | 8 | 10 | 13) {
                        continue;
                    }
                    let bda = hex(event["bda_hex"].as_str().unwrap());
                    if bda.len() < 30 {
                        continue;
                    }
                    let columns = u16::from_le_bytes([bda[1], bda[2]]);
                    let page = (regs[1].as_u64().unwrap() >> 8) as usize;
                    if page >= 8 || page != bda[25] as usize || columns == 0 || columns > 320 {
                        continue;
                    }
                    let start = Cursor {
                        column: bda[7 + page * 2] as u16,
                        row: bda[8 + page * 2] as u16,
                    };
                    if start.column >= columns || start.row >= 25 {
                        continue;
                    }
                    let text = [u32::from(cp437_codepoint(ax as u8))];
                    let boundary = [Boundary {
                        text_offset: 1,
                        kind: BoundaryKind::SoftWrap,
                    }];
                    presenter
                        .notify(Notification {
                            operation_id: event["seq"].as_u64().unwrap(),
                            text: &text,
                            boundaries: if start.column + 1 == columns {
                                &boundary
                            } else {
                                &[]
                            },
                            columns,
                            rows: 25,
                            start,
                        })
                        .unwrap();
                }
                "frame" => {
                    if let Some(full) = event["vram_hex"].as_str() {
                        vram = hex(full);
                    }
                    let monochrome = number(&event, "mono") != 0;
                    if let Some(patches) = event["patches"].as_array() {
                        for patch in patches {
                            let offset = patch[0].as_u64().unwrap() as usize;
                            let bytes = hex(patch[1].as_str().unwrap());
                            vram[offset..offset + bytes.len()].copy_from_slice(&bytes);
                        }
                    }
                    let columns = number(&event, "columns");
                    let rows = number(&event, "rows");
                    if columns == 0 || rows == 0 {
                        continue;
                    }
                    let crtc = hex(event["crtc_hex"].as_str().unwrap());
                    let decoder = &mut decoders[usize::from(monochrome)];
                    let port = if monochrome { 0x3b4 } else { 0x3d4 };
                    for reg in [12, 13] {
                        decoder.write(port, reg);
                        decoder.write(port + 1, crtc[reg as usize]);
                    }
                    let mode = number(&event, "mode") as u8;
                    if !monochrome && mode & 2 != 0 {
                        continue;
                    }
                    decoder.write(port + 4, mode | 8);
                    let VideoFrame::Text { cells, .. } =
                        decoder.decode_text(&vram, columns, rows, true).unwrap()
                    else {
                        unreachable!()
                    };
                    let mut cells: Vec<TextCell> = cells.to_vec();
                    let start = u16::from_be_bytes([crtc[12], crtc[13]]) as usize;
                    for (index, cell) in cells.iter_mut().enumerate() {
                        let attr = vram[((start + index) * 2 + 1) & (vram.len() - 1)];
                        if attr & 8 != 0 {
                            cell.flags |= TEXT_BOLD;
                        }
                        if attr & 0x70 == 0x70 {
                            cell.flags |= TEXT_REVERSE;
                        }
                    }
                    let position = number(&event, "cursor");
                    let position = if (position as usize) < cells.len() {
                        position
                    } else {
                        0
                    };
                    let hints = if mode & 8 == 0 { VIDEO_DISABLED } else { 0 }
                        | if submitted_vram.as_deref() != Some(vram.as_slice()) {
                            VIDEO_MEMORY_CHANGED
                        } else {
                            0
                        };
                    submitted_vram = Some(vram.clone());
                    let mut frame = Frame {
                        cells: &cells,
                        columns,
                        rows,
                        stride: columns,
                        cursor: Cursor {
                            column: position % columns,
                            row: position / columns,
                        },
                        refresh_rate: if monochrome {
                            RefreshRate::Hz50
                        } else {
                            RefreshRate::Hz60
                        },
                        hints,
                    };
                    observe(&frame);
                    for _ in 0..event["repeat"].as_u64().unwrap_or(1) {
                        presenter
                            .present(frame)
                            .unwrap_or_else(|error| panic!("event {}: {error}", event["seq"]));
                        frame.hints &= !VIDEO_MEMORY_CHANGED;
                    }
                }
                other => panic!("unknown trace event {other}"),
            }
        }
    }
    destination.seek(SeekFrom::Start(0)).unwrap();
    let mut bytes = Vec::new();
    destination.read_to_end(&mut bytes).unwrap();
    bytes
}

fn assert_bytes(actual: &[u8], expected: &[u8], context: &str) {
    if actual == expected {
        return;
    }
    let offset = actual
        .iter()
        .zip(expected)
        .position(|(actual, expected)| actual != expected)
        .unwrap_or(actual.len().min(expected.len()));
    let start = offset.saturating_sub(24);
    panic!(
        "{context}: first difference at byte {offset}; actual {} bytes, expected {} bytes; actual {:?}, expected {:?}",
        actual.len(),
        expected.len(),
        String::from_utf8_lossy(&actual[start..actual.len().min(offset + 48)]),
        String::from_utf8_lossy(&expected[start..expected.len().min(offset + 48)]),
    );
}

#[test]
fn real_dos_boots_reconcile_observed_calls_without_inventing_output() {
    let root =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/boot-notifications");
    for case in ["dos100-xt", "dos100-5150", "dos210-xt", "dos210-5150"] {
        let trace = std::fs::read_to_string(root.join(format!("{case}.jsonl"))).unwrap();
        let expected = std::fs::read(root.join(format!("{case}.stdout"))).unwrap();
        assert_bytes(&replay(&trace, true, |_| {}), &expected, case);
    }
}

#[test]
fn real_cga_disabled_scroll_preserves_history_until_the_copy_finishes() {
    // IBM XT 8088/4.77 MHz, DOS 2.10 DIR: seq 5781 copies through row 18
    // while rows 19..24 are still old; mode 0x25 has video enable clear.
    // Seq 5799 reenables after the copy and starts the next directory row.
    let compressed = std::fs::File::open(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("tests/fixtures/boot-notifications/cga8088-disabled-scroll.jsonl.gz"),
    )
    .unwrap();
    let mut trace = String::new();
    flate2::read::GzDecoder::new(compressed)
        .read_to_string(&mut trace)
        .unwrap();
    let baseline = trace.lines().next().unwrap();
    let mut expected = replay(baseline, false, |_| {});
    expected.extend_from_slice(b"\nBAC");
    let actual = replay(&trace, false, |_| {});
    assert_bytes(&actual, &expected, "disabled copy must not blank or replay DIR history");
}

#[test]
fn real_mda_scroll_holds_a_copied_prefix_after_finishing_the_source_line() {
    // V20/10 MHz DIR: seq 2634 has video enabled, copies only the first three
    // rows, and finishes SYS.COM in the still-unmoved bottom source row.
    let compressed = std::fs::File::open(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("tests/fixtures/boot-notifications/mda-v20-partial-scroll.jsonl.gz"),
    )
    .unwrap();
    let mut trace = String::new();
    flate2::read::GzDecoder::new(compressed)
        .read_to_string(&mut trace)
        .unwrap();
    let frames: Vec<_> = trace.lines().collect();
    let coherent = format!("{}\n{}\n", frames[0], frames[2]);
    let expected = replay(&coherent, false, |_| {});
    let actual = replay(&trace, false, |_| {});
    assert_bytes(&actual, &expected, "partial copy must not alter the completed DIR transcript");
    let transcript = std::str::from_utf8(&actual).unwrap();
    assert_eq!(transcript.matches("SYS").count(), 2); // ANSI.SYS and SYS.COM.
    assert!(transcript.trim_end().ends_with("DISKCOPY COM"));
}

#[test]
fn real_mda_successive_directory_scrolls_preserve_every_file_once() {
    // Three actual DIR commands at 10 MHz exposed cursor-lagged BASIC/BASICA
    // appends. The old policy rejected seq 17053, then repeatedly fell back
    // and repainted retained listings in the interactive capture.
    let compressed = std::fs::File::open(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("tests/fixtures/boot-notifications/mda-v20-repeated-dir.jsonl.gz"),
    )
    .unwrap();
    let mut trace = String::new();
    flate2::read::GzDecoder::new(compressed)
        .read_to_string(&mut trace)
        .unwrap();
    let actual = replay(&trace, false, |_| {});
    let transcript = std::str::from_utf8(&actual).unwrap().replace("\r\n", "\n");
    assert!(!transcript.contains('\x0c'), "DIR must not clear retained history");
    assert!(transcript.ends_with("A>"));
    let listings: Vec<Vec<Vec<String>>> = transcript
        .split("A>DIR\n")
        .skip(1)
        .map(|listing| {
            let listing = listing
                .split_once("A>")
                .map_or(listing, |(body, _)| body)
                .trim()
                .replace('\n', "\r\n");
            // A terminal applies the cursor-lag backspaces, rather than
            // interpreting overwritten digits as additional file-size text.
            let terminal = Terminal::replay(listing.as_bytes());
            assert!(terminal.errors.is_empty(), "{:?}", terminal.errors);
            terminal.cells().0
                .chunks_exact(COLS)
                .map(|row| {
                    row.iter()
                        .map(|cell| cell.character)
                        .collect::<String>()
                        .split_whitespace()
                        .map(str::to_owned)
                        .collect()
                })
                .collect()
        })
        .collect();
    assert_eq!(listings.len(), 3);
    let first = &listings[0];
    assert_eq!(
        first
            .iter()
            .filter(|fields| {
                fields.get(1).is_some_and(|extension| matches!(extension.as_str(), "COM" | "SYS" | "EXE" | "BAS"))
            })
            .count(),
        39,
    );
    for listing in &listings {
        assert_eq!(listing, first, "a DIR lost or replayed retained rows");
        for expected in [
            "MORE COM 384 10-20-83 12:00p",
            "BASIC COM 16256 10-20-83 12:00p",
            "BASICA COM 26112 10-20-83 12:00p",
        ] {
            assert_eq!(
                listing.iter().filter(|fields| fields.iter().map(String::as_str).eq(expected.split_whitespace())).count(),
                1,
                "cursor-lagged directory field changed: {expected}",
            );
        }
    }
}

#[derive(Default)]
struct ScreenEvidence {
    recent_rows: std::collections::VecDeque<Vec<String>>,
    saw_dir: bool,
    saw_cls: bool,
    cleared_after_cls: bool,
    unsupported_cls: bool,
    scrolls: [usize; 2],
}

impl ScreenEvidence {
    fn observe(&mut self, frame: &Frame<'_>) {
        let rows: Vec<String> = frame
            .cells
            .chunks_exact(usize::from(frame.stride))
            .map(|row| {
                row[..usize::from(frame.columns)]
                    .iter()
                    .map(|cell| char::from_u32(cell.codepoint).unwrap())
                    .collect::<String>()
                    .trim_end()
                    .to_owned()
            })
            .collect();
        self.saw_dir |= rows.iter().any(|row| row.eq_ignore_ascii_case("A>DIR"));
        self.saw_cls |= rows.iter().any(|row| row.eq_ignore_ascii_case("A>CLS"));
        if self.saw_cls {
            self.cleared_after_cls |= rows[1] == "A>"
                && rows
                    .iter()
                    .enumerate()
                    .all(|(index, row)| index == 1 || row.is_empty())
                && frame.cursor == Cursor { column: 2, row: 1 };
            self.unsupported_cls |= rows.iter().any(|row| row == "Bad command or file name");
        }
        // BIOS copies a scroll over several vsyncs. Compare a short window,
        // matching 22 stable rows while excluding the two rows being written.
        if self.saw_dir
            && self.recent_rows.iter().any(|previous| {
                previous.len() == rows.len()
                    && previous[0] != rows[0]
                    && previous[1..rows.len() - 2] == rows[..rows.len() - 3]
            })
        {
            self.scrolls[usize::from(self.saw_cls)] += 1;
            self.recent_rows.clear();
        }
        if self.recent_rows.len() == 12 {
            self.recent_rows.pop_front();
        }
        self.recent_rows.push_back(rows);
    }
}

#[test]
fn real_dos_directory_history_survives_scrolls_and_the_observed_cls_outcome() {
    let root =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/boot-notifications");
    for case in ["dos100-xt", "dos100-5150", "dos210-xt", "dos210-5150"] {
        let compressed = std::fs::File::open(root.join(format!("{case}-scroll.jsonl.gz"))).unwrap();
        let mut trace = String::new();
        flate2::read::GzDecoder::new(compressed)
            .read_to_string(&mut trace)
            .unwrap();
        let expected = std::fs::read(root.join(format!("{case}-scroll.stdout"))).unwrap();
        let mut screens = ScreenEvidence::default();
        let snapshot_only = replay(&trace, false, |frame| screens.observe(frame));
        assert_bytes(
            &snapshot_only,
            &expected,
            &format!("{case}: independent snapshot oracle"),
        );
        let notified = replay(&trace, true, |_| {});
        assert_bytes(
            &notified,
            &snapshot_only,
            &format!("{case}: notifications changed visible output"),
        );

        // Exact control bytes are checked above. Compare logical listing rows
        // independently of whether a sampled CR needed an explicit host CR.
        let transcript = std::str::from_utf8(&notified)
            .unwrap()
            .replace("\r\n", "\n");
        let (before_cls, after_cls) = transcript.split_once("A>CLS\n").unwrap();
        assert!(!after_cls.contains("A>CLS"), "{case}: duplicate CLS");
        assert!(
            transcript.ends_with("A>"),
            "{case}: final command did not return"
        );
        let first_listing = before_cls.split("A>DIR\n").nth(1).unwrap().trim();
        let date = if case.starts_with("dos100") {
            "08-04-81"
        } else {
            "10-20-83"
        };
        // Both real disks list 38 files. COMMAND is above the final viewport;
        // COMM is the final file. Requiring the entire first listing eight
        // times catches dropped history, duplicated scroll rows and truncation.
        assert_eq!(
            first_listing
                .lines()
                .filter(|line| line.contains(date))
                .count(),
            38,
            "{case}"
        );
        assert!(first_listing.contains("COMMAND"), "{case}");
        assert!(
            first_listing
                .lines()
                .any(|line| line.split_whitespace().next() == Some("COMM")),
            "{case}"
        );
        for (phase, output) in [("before CLS", before_cls), ("after CLS", after_cls)] {
            let listings: Vec<_> = output.split("A>DIR\n").skip(1).collect();
            assert_eq!(listings.len(), 4, "{case}: {phase}");
            for listing in listings {
                let listing = listing
                    .split_once("A>")
                    .map_or(listing, |(body, _)| body)
                    .trim();
                // A sampled blank cursor advance may use spaces rather than a
                // later TAB. Compare complete ordered directory fields here;
                // the independent byte oracle above checks the control stream.
                assert_eq!(
                    listing.lines().map(|line| line.split_whitespace().collect::<Vec<_>>()).collect::<Vec<_>>(),
                    first_listing.lines().map(|line| line.split_whitespace().collect::<Vec<_>>()).collect::<Vec<_>>(),
                    "{case}: incomplete listing {phase}"
                );
            }
        }
        assert!(screens.scrolls[0] > 0, "{case}: no observed pre-CLS scroll");
        assert!(
            screens.scrolls[1] > 0,
            "{case}: no observed post-CLS scroll"
        );
        if case.starts_with("dos100") {
            assert!(
                screens.unsupported_cls,
                "{case}: missing observed CLS error"
            );
            assert!(!screens.cleared_after_cls, "{case}: fabricated clear");
            assert!(
                after_cls.starts_with("Bad command or file name\n"),
                "{case}"
            );
        } else {
            assert!(screens.cleared_after_cls, "{case}: missing observed clear");
            assert!(
                after_cls.starts_with("\x0c\n"),
                "{case}: missing glass clear marker"
            );
            assert!(!screens.unsupported_cls, "{case}: unexpected CLS error");
            assert!(!after_cls.contains("Bad command or file name"), "{case}");
            assert_eq!(
                transcript
                    .matches("38 File(s)     91136 bytes free")
                    .count(),
                8,
                "{case}"
            );
        }
    }
}
