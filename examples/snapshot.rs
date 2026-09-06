// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

//! Run in a terminal: `cargo run --example snapshot -- /tmp/tigt-snapshot.json 30`.
//! From another terminal, send `kill -USR1 PID` to this process. The JSON contains
//! the submitted Unicode, exact RGB, flags, borders and lossy legacy projection.
//! No font is needed for attributes; PNG text needs `set_snapshot_font` first.

use std::{
    fs::File,
    os::fd::AsFd,
    path::PathBuf,
    thread,
    time::{Duration, Instant},
};
use tigt::{
    Overscan, Session, SnapshotConfig, SnapshotFormat, SnapshotSignal, TEXT_UNDERLINE, TextCell,
};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut arguments = std::env::args_os().skip(1);
    let path = PathBuf::from(
        arguments
            .next()
            .unwrap_or_else(|| "tigt-snapshot.json".into()),
    );
    let seconds: u64 = arguments
        .next()
        .map(|value| value.to_string_lossy().parse())
        .transpose()?
        .unwrap_or(30);
    let session = Session::new()?;
    session.set_overscan(&Overscan {
        color: 0x203040,
        left: 8,
        right: 8,
        top: 4,
        bottom: 4,
    })?;
    let mut cells = vec![TextCell::new(' ', 0xffffff, 0x102030); 40 * 3];
    for (cell, character) in cells
        .iter_mut()
        .zip("tigt native snapshot instrumentation".chars())
    {
        cell.codepoint = character as u32;
        cell.flags = TEXT_UNDERLINE;
    }
    for (cell, character) in cells[40..]
        .iter_mut()
        .zip(format!("kill -USR1 {}", std::process::id()).chars())
    {
        cell.codepoint = character as u32;
    }
    session.present_text(&cells, 40, 3, 40)?;
    // A synchronous write borrows the descriptor; File still owns it afterward.
    let file = File::create(&path)?;
    session.snapshot_write_fd(file.as_fd(), SnapshotFormat::Attributes)?;
    drop(file);
    session.configure_snapshot(Some(SnapshotConfig {
        signal: SnapshotSignal::Usr1,
        format: SnapshotFormat::Attributes,
        path: &path,
    }))?;
    let start = Instant::now();
    let mut completed = 0;
    while start.elapsed() < Duration::from_secs(seconds) {
        for cell in &mut cells[80..] {
            cell.codepoint = ' ' as u32;
        }
        for (cell, character) in cells[80..]
            .iter_mut()
            .zip(format!("Elapsed: {} seconds", start.elapsed().as_secs()).chars())
        {
            cell.codepoint = character as u32;
        }
        session.present_text(&cells, 40, 3, 40)?;
        let status = session.snapshot_status()?;
        if status.sequence != completed {
            status.result?; // signal delivery is not proof that the write worked
            completed = status.sequence;
        }
        thread::sleep(Duration::from_millis(50));
    }
    session.configure_snapshot(None)?;
    let final_status = session.snapshot_status()?;
    final_status.result?;
    drop(session);
    println!(
        "Snapshot: {}; {} asynchronous requests completed",
        path.display(),
        final_status.sequence
    );
    Ok(())
}
