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
use tigt::{CRTC_SIZE, InputDecoder, MDA_VRAM_SIZE, Session};

fn append_scan_bytes(bytes: &mut Vec<u8>, events: impl IntoIterator<Item = PcEvent>) {
    for event in events {
        let sequence = match event {
            PcEvent::Make(key) => key.make,
            PcEvent::Break(key) => key.break_sequence,
        };
        bytes.extend_from_slice(sequence.bytes());
    }
}

fn write_line(vram: &mut [u8], row: usize, text: &str) {
    for (column, byte) in text.bytes().take(80).enumerate() {
        let offset = (row * 80 + column) * 2;
        vram[offset] = byte;
        vram[offset + 1] = 0x07;
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

    let mut vram = [0_u8; MDA_VRAM_SIZE];
    for cell in vram.chunks_exact_mut(2) {
        cell.copy_from_slice(&[b' ', 0x07]);
    }
    let mut crtc = [0_u8; CRTC_SIZE];
    crtc[1] = 80;
    crtc[6] = 25;
    crtc[9] = 13;
    crtc[10] = 0x20; // Disable cursor for this static demonstration.
    write_line(&mut vram, 0, "tigt + optional pc-xt-keyboard adapter");
    write_line(
        &mut vram,
        2,
        "Input decoded before opening the output-only terminal:",
    );
    write_line(
        &mut vram,
        3,
        "A, Ctrl+C, Ctrl+Left; release all at end of batch",
    );
    write_line(&mut vram, 5, "PC/XT Set 1 make/break bytes:");
    for (row, chunk) in hex.as_bytes().chunks(78).enumerate() {
        write_line(&mut vram, 6 + row, std::str::from_utf8(chunk)?);
    }
    let session = Session::new()?;
    session.present_mda(&vram, &crtc, 0x08)?;
    thread::sleep(Duration::from_secs(2));
    drop(session);
    println!("PC/XT scan bytes: {}", hex.trim_end());
    Ok(())
}
