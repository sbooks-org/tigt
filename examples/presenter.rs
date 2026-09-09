// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

use std::{os::fd::AsFd, thread, time::Duration};
use tigt::{
    TextCell,
    presenter::{Config, Cursor, Encoding, Frame, Mode, Presenter, RefreshRate, Reversibility},
};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let output = std::io::stdout();
    let mut presenter = Presenter::new(
        output.as_fd(),
        Config {
            mode: Mode::Glass,
            encoding: Encoding::Locale,
            reversibility: Reversibility::OneWay,
        },
    )?;
    let mut cells = [TextCell::new(' ', 0xffffff, 0); 40];
    let message = b"Hello, glass-TTY!";
    // Simulate a 20-column, two-row guest at 60 Hz. A real emulator calls once
    // per guest vsync rather than sleeping. Keep submitting unchanged frames.
    for tick in 0..=message.len() + 6 {
        if (1..=message.len()).contains(&tick) {
            cells[tick - 1].codepoint = u32::from(message[tick - 1]);
        }
        let cursor = if tick <= message.len() {
            Cursor {
                column: tick as u16,
                row: 0,
            }
        } else {
            Cursor { column: 0, row: 1 }
        };
        presenter.present(Frame {
            cells: &cells,
            columns: 20,
            rows: 2,
            stride: 20,
            cursor,
            refresh_rate: RefreshRate::Hz60,
            hints: 0,
        })?;
        thread::sleep(Duration::from_secs_f64(1.0 / 60.0));
    }
    Ok(())
}
