// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

//! Run in a terminal with `cargo run --example demo -- --graphics auto`.
//! The animation ends after five seconds; Escape or Ctrl+C also quits.
//! Quit policy belongs here, not C.

use std::{
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};
use tigt::{GraphicsMode, InputKey, InputKind, Modifiers, Session};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut graphics = GraphicsMode::Auto;
    let mut args = std::env::args().skip(1);
    while let Some(argument) = args.next() {
        match argument.as_str() {
            "--graphics" => {
                graphics = match args.next().as_deref() {
                    Some("auto") => GraphicsMode::Auto,
                    Some("blocks") => GraphicsMode::Blocks,
                    Some("sixel") => GraphicsMode::Sixel,
                    Some("ascii") => GraphicsMode::Ascii,
                    Some("iterm2") => GraphicsMode::Iterm2,
                    _ => return Err("--graphics requires auto|blocks|sixel|ascii|iterm2".into()),
                };
            }
            "--help" => {
                println!("usage: demo [--graphics auto|blocks|sixel|ascii|iterm2]");
                return Ok(());
            }
            _ => return Err(format!("unknown argument: {argument}").into()),
        }
    }
    let (send, receive) = mpsc::channel();
    let session = Session::with_input_and_graphics(graphics, move |event| {
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
