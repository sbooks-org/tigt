// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

use std::{os::fd::AsFd, thread, time::Duration};
use tigt::{
    TextCell,
    presenter::{
        Boundary, BoundaryKind, Config, Cursor, Encoding, Frame, Mode, Notification, NotifyStatus,
        Presenter, RefreshRate, Reversibility,
    },
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
    let mut cells = [TextCell::new(' ', 0xaaaaaa, 0); 60];
    presenter.present(Frame {
        cells: &cells,
        columns: 20,
        rows: 3,
        stride: 20,
        cursor: Cursor { column: 0, row: 0 },
        refresh_rate: RefreshRate::Hz60,
        hints: 0,
    })?;
    let text: Vec<u32> = "Hello, wrapped glass-TTY!".chars().map(u32::from).collect();
    let boundaries = [
        Boundary {
            text_offset: 20,
            kind: BoundaryKind::SoftWrap,
        },
        Boundary {
            text_offset: text.len(),
            kind: BoundaryKind::Newline,
        },
    ];
    // In a VM this comes from an observer, before the output's next snapshot.
    // notify copies the data and does not print any of it.
    assert_eq!(
        presenter.notify(Notification {
            operation_id: 1,
            text: &text,
            boundaries: &boundaries,
            columns: 20,
            rows: 3,
            start: Cursor { column: 0, row: 0 },
        })?,
        NotifyStatus::Accepted
    );
    for count in 1..=text.len() + 1 {
        let cursor = if count <= text.len() {
            cells[count - 1].codepoint = text[count - 1];
            Cursor {
                column: (count % 20) as u16,
                row: (count / 20) as u16,
            }
        } else {
            Cursor { column: 0, row: 2 }
        };
        presenter.present(Frame {
            cells: &cells,
            columns: 20,
            rows: 3,
            stride: 20,
            cursor,
            refresh_rate: RefreshRate::Hz60,
            hints: 0,
        })?;
        thread::sleep(Duration::from_secs_f64(1.0 / 60.0));
    }
    Ok(())
}
