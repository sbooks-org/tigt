// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

//! `cargo run --example keyboard --features keyboard`
//!
//! First decode and map input without a terminal session. Then display the
//! result in an output-only session for two seconds. The application owns the
//! mapper and its release policy; tigt neither creates nor drives a PC mapper.

use std::{fmt::Write, thread, time::Duration};
use tigt::keyboard::mapper::{PcEvent, PcKeyboard};
use tigt::{InputDecoder, Session, TextCell};

fn append_scan_bytes(bytes: &mut Vec<u8>, events: impl IntoIterator<Item = PcEvent>) {
    for event in events {
        let sequence = match event {
            PcEvent::Make(key) => key.make,
            PcEvent::Break(key) => key.break_sequence,
        };
        bytes.extend_from_slice(sequence.bytes());
    }
}

fn write_line(cells: &mut [TextCell], row: usize, text: &str) {
    for (column, character) in text.chars().take(80).enumerate() {
        cells[row * 80 + column] = TextCell::new(character, 0xc4c4c4, 0);
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut keyboard = PcKeyboard::default();
    let mut bytes = Vec::new();
    {
        let mut input = InputDecoder::new(|event| {
            append_scan_bytes(&mut bytes, keyboard.handle(&event.into()));
        })?;
        // Plain A, Ctrl+C, then Ctrl+Left: Ctrl+C is data, not an exit request.
        input.feed(b"A\x03\x1b[1;5D")?;
        input.flush()?;
    }
    // Release anything still held by an explicit transition in the batch.
    // Legacy input without release reporting is decoded as synthetic taps.
    append_scan_bytes(&mut bytes, keyboard.release_all());
    let mut hex = String::new();
    for byte in &bytes {
        write!(&mut hex, "{byte:02x} ")?;
    }

    let mut cells = [TextCell::new(' ', 0xc4c4c4, 0); 80 * 25];
    write_line(&mut cells, 0, "tigt + optional pc-xt-keyboard adapter");
    write_line(
        &mut cells,
        2,
        "Input decoded before opening the output-only terminal:",
    );
    write_line(
        &mut cells,
        3,
        "A, Ctrl+C, Ctrl+Left; release all at end of batch",
    );
    write_line(&mut cells, 5, "PC/XT Set 1 make/break bytes:");
    for (row, chunk) in hex.as_bytes().chunks(78).enumerate() {
        write_line(&mut cells, 6 + row, std::str::from_utf8(chunk)?);
    }
    let session = Session::new()?;
    session.present_text(&cells, 80, 25, 80)?;
    thread::sleep(Duration::from_secs(2));
    drop(session);
    println!("PC/XT scan bytes: {}", hex.trim_end());
    Ok(())
}
