// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

//! Run in a terminal with `cargo run --example demo`. The animation ends after
//! five seconds; Escape or Ctrl+C also quits. Quit policy belongs here, not C.

use std::{
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};
use tigt::{InputKey, InputKind, Modifiers, Session};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let (send, receive) = mpsc::channel();
    let session = Session::with_input(move |event| {
        let _ = send.send(event);
    })?;
    let mut pixels = vec![0_u32; 320 * 200];
    let start = Instant::now();
    let mut frame = 0_u32;
    while start.elapsed() < Duration::from_secs(5) {
        let quit = receive.try_iter().any(|event| {
            event.kind != InputKind::Release
                && (event.key == InputKey::Escape
                    || (event.key == InputKey::Char('c')
                        && event.modifiers.contains(Modifiers::CONTROL)))
        });
        if quit {
            break;
        }
        for (index, pixel) in pixels.iter_mut().enumerate() {
            let x = (index % 320) as u32;
            let y = (index / 320) as u32;
            let red = (x + frame * 3) % 256;
            let green = y * 255 / 199;
            let blue = ((x / 40 + y / 25 + frame / 8) & 1) * 255;
            *pixel = (red << 16) | (green << 8) | blue;
        }
        session.present_bitmap(&pixels, 320, 200, 320, 2)?;
        frame += 1;
        thread::sleep(Duration::from_millis(16));
    }
    session.input_status()?;
    drop(session);
    println!("tigt: rendered {frame} frames and restored the terminal");
    Ok(())
}
