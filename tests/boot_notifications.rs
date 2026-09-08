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
        RefreshRate, Reversibility, TEXT_BOLD, TEXT_REVERSE,
    },
    video::{AdapterKind, Frame as VideoFrame, VideoAdapter},
};

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
                    decoder.write(port + 4, mode);
                    let VideoFrame::Text { cells, .. } =
                        decoder.decode_text(&vram, columns, rows, true).unwrap()
                    else {
                        unreachable!()
                    };
                    let mut cells: Vec<TextCell> = cells.to_vec();
                    let start = u16::from_be_bytes([crtc[12], crtc[13]]) as usize;
                    if mode & 8 != 0 {
                        for (index, cell) in cells.iter_mut().enumerate() {
                            let attr = vram[((start + index) * 2 + 1) & (vram.len() - 1)];
                            if attr & 8 != 0 {
                                cell.flags |= TEXT_BOLD;
                            }
                            if attr & 0x70 == 0x70 {
                                cell.flags |= TEXT_REVERSE;
                            }
                        }
                    }
                    let position = number(&event, "cursor");
                    let position = if (position as usize) < cells.len() {
                        position
                    } else {
                        0
                    };
                    let frame = Frame {
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
                    };
                    observe(&frame);
                    for _ in 0..event["repeat"].as_u64().unwrap_or(1) {
                        presenter
                            .present(frame)
                            .unwrap_or_else(|error| panic!("event {}: {error}", event["seq"]));
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
                assert_eq!(listing, first_listing, "{case}: incomplete listing {phase}");
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
