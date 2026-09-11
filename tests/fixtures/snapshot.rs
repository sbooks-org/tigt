// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

use std::{
    ffi::CString,
    fs::{File, OpenOptions},
    io::Write,
    os::{
        fd::{AsFd, AsRawFd, FromRawFd},
        unix::{ffi::OsStrExt, fs::OpenOptionsExt},
    },
    path::Path,
    sync::atomic::{AtomicI32, Ordering},
    thread,
    time::{Duration, Instant},
};
use tigt::{
    DisplayTechnology, Error, InputKey, InputKind, Overscan, Session, SnapshotConfig,
    SnapshotFormat, SnapshotSignal, TEXT_CURSOR, TEXT_UNDERLINE, TextCell,
};

const FORMATS: [(SnapshotFormat, &str); 7] = [
    (SnapshotFormat::Png, "png"),
    (SnapshotFormat::Utf8, "utf8"),
    (SnapshotFormat::Ascii, "ascii"),
    (SnapshotFormat::Cp437, "cp437"),
    (SnapshotFormat::Ansi, "ansi"),
    (SnapshotFormat::Cells, "cells"),
    (SnapshotFormat::Attributes, "json"),
];
static HANDLED: AtomicI32 = AtomicI32::new(0);
extern "C" fn app_handler(number: i32) {
    HANDLED.store(number, Ordering::Relaxed);
}

fn capture(
    session: &Session,
    directory: &Path,
    name: &str,
    format: SnapshotFormat,
) -> Result<(), Error> {
    let file = File::create(directory.join(name)).unwrap();
    let result = session.snapshot_write_fd(file.as_fd(), format);
    assert_ne!(unsafe { libc::fcntl(file.as_raw_fd(), libc::F_GETFD) }, -1);
    result
}

fn all_formats(session: &Session, directory: &Path, scene: &str, png: bool) {
    for (format, extension) in FORMATS.into_iter().skip(usize::from(!png)) {
        capture(session, directory, &format!("{scene}.{extension}"), format).unwrap();
    }
}

fn await_request(session: &Session, before: u64, expected: Result<(), Error>) {
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let status = session.snapshot_status().unwrap();
        if status.sequence != before {
            assert_eq!(status.sequence, before + 1);
            assert_eq!(status.result, expected);
            return;
        }
        assert!(
            Instant::now() < deadline,
            "snapshot worker failed to complete"
        );
        thread::sleep(Duration::from_millis(1));
    }
}

fn request(
    session: &Session,
    directory: &Path,
    name: &str,
    format: SnapshotFormat,
    expected: Result<(), Error>,
) {
    let path = directory.join(name);
    session
        .configure_snapshot(Some(SnapshotConfig {
            signal: SnapshotSignal::Usr1,
            format,
            path: &path,
        }))
        .unwrap();
    drop(path); // the safe configuration borrows only for the call
    let before = session.snapshot_status().unwrap().sequence;
    assert_eq!(unsafe { libc::raise(libc::SIGUSR1) }, 0);
    await_request(session, before, expected);
}

fn install_handler(number: i32, handler: usize) {
    let mut action: libc::sigaction = unsafe { std::mem::zeroed() };
    action.sa_sigaction = handler;
    unsafe {
        libc::sigemptyset(&mut action.sa_mask);
        assert_eq!(libc::sigaction(number, &action, std::ptr::null_mut()), 0);
    }
}

fn expect_handler(number: i32, handler: usize) {
    let mut action: libc::sigaction = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe { libc::sigaction(number, std::ptr::null(), &mut action) },
        0
    );
    assert_eq!(action.sa_sigaction, handler);
}

fn check_signal_ownership(session: &Session, directory: &Path) {
    let path = directory.join("owned.json");
    let config = SnapshotConfig {
        signal: SnapshotSignal::Usr2,
        format: SnapshotFormat::Attributes,
        path: &path,
    };
    install_handler(libc::SIGUSR2, app_handler as *const () as usize);
    assert_eq!(session.configure_snapshot(Some(config)), Err(Error::Busy));
    assert_eq!(unsafe { libc::raise(libc::SIGUSR2) }, 0);
    assert_eq!(HANDLED.load(Ordering::Relaxed), libc::SIGUSR2);
    expect_handler(libc::SIGUSR2, app_handler as *const () as usize);
    install_handler(libc::SIGUSR2, libc::SIG_IGN);
    assert_eq!(session.configure_snapshot(Some(config)), Err(Error::Busy));
    expect_handler(libc::SIGUSR2, libc::SIG_IGN);
    install_handler(libc::SIGUSR2, libc::SIG_DFL);
    session.configure_snapshot(Some(config)).unwrap();
    let before = session.snapshot_status().unwrap().sequence;
    assert_eq!(unsafe { libc::raise(libc::SIGUSR2) }, 0);
    await_request(session, before, Ok(()));
    session.configure_snapshot(None).unwrap();
    expect_handler(libc::SIGUSR2, libc::SIG_DFL);
    session
        .configure_snapshot(Some(SnapshotConfig {
            signal: SnapshotSignal::Usr1,
            ..config
        }))
        .unwrap();
    install_handler(libc::SIGUSR1, app_handler as *const () as usize);
    session.configure_snapshot(None).unwrap();
    expect_handler(libc::SIGUSR1, app_handler as *const () as usize);
    assert_eq!(unsafe { libc::raise(libc::SIGUSR1) }, 0);
    assert_eq!(HANDLED.load(Ordering::Relaxed), libc::SIGUSR1);
    install_handler(libc::SIGUSR1, libc::SIG_DFL);
}

fn text_scene(session: &Session, directory: &Path) {
    let mut cells = [
        TextCell {
            flags: TEXT_UNDERLINE,
            ..TextCell::new('A', 0x123456, 0x654321)
        },
        TextCell::new('é', 0x00aa00, 0x0000aa),
        TextCell {
            flags: TEXT_CURSOR,
            ..TextCell::new('█', 0xffffff, 0)
        },
        TextCell::new(' ', 0xffffff, 0),
        TextCell {
            codepoint: u32::MAX,
            foreground: u32::MAX,
            background: u32::MAX,
            flags: u32::MAX,
        },
        TextCell {
            codepoint: u32::MAX,
            foreground: u32::MAX,
            background: u32::MAX,
            flags: u32::MAX,
        },
        TextCell::new(' ', 0, 0xffffff),
        TextCell::new('B', 0xaa0000, 0x00aaaa),
        TextCell::new('┌', 0x555555, 0xaaaaaa),
        TextCell::new(' ', 0xffffff, 0),
    ];
    assert_eq!(session.present_text(&[], 0, 0, 0), Err(Error::Argument));
    assert_eq!(
        session.present_text(&cells[..9], 4, 2, 6),
        Err(Error::Argument)
    );
    session.present_text(&cells, 4, 2, 6).unwrap();
    cells.fill(TextCell::new('X', 0, 0));
    assert_eq!(
        capture(session, directory, "missing-font.png", SnapshotFormat::Png),
        Err(Error::Argument)
    );
    let mut font = vec![0; 256 * 32];
    for (length, height) in [(0, 0), (256, 0), (256, 33), (255, 1), (257, 1), (8191, 32)] {
        assert_eq!(
            session.set_snapshot_font(&font[..length], height),
            Err(Error::Argument)
        );
    }
    session.set_snapshot_font(&font[..256], 1).unwrap();
    capture(session, directory, "height1.png", SnapshotFormat::Png).unwrap();
    session.set_snapshot_font(&font, 32).unwrap();
    capture(session, directory, "height32.png", SnapshotFormat::Png).unwrap();
    for glyph in 0..256 {
        for row in 0..8 {
            font[glyph * 8 + row] = if glyph == 32 {
                0
            } else {
                ((glyph * 37 + row * 19) ^ (glyph << (row % 3))) as u8
            };
        }
    }
    session.set_snapshot_font(&font[..256 * 8], 8).unwrap();
    font.fill(0);
    let before = session.snapshot_status().unwrap();
    all_formats(session, directory, "text", true);
    assert_eq!(session.snapshot_status().unwrap(), before);
    for (format, extension) in FORMATS {
        request(
            session,
            directory,
            &format!("signal.{extension}"),
            format,
            Ok(()),
        );
    }
    check_signal_ownership(session, directory);
    session.clear_snapshot_font().unwrap();
    assert_eq!(
        capture(session, directory, "cleared-font.png", SnapshotFormat::Png),
        Err(Error::Argument)
    );
    session.set_snapshot_font(&font[..256 * 8], 8).unwrap();
    session
        .present_text(&[TextCell::new(' ', 0x123456, 0x654321); 6], 3, 2, 3)
        .unwrap();
    all_formats(session, directory, "blank", true);
    session
        .present_text(&[TextCell::new('λ', 0x123456, 0x654321)], 1, 1, 1)
        .unwrap();
    all_formats(session, directory, "unmapped", false);
    assert_eq!(
        capture(session, directory, "unmapped.png", SnapshotFormat::Png),
        Err(Error::Argument)
    );
    request(
        session,
        directory,
        "unmapped-signal.png",
        SnapshotFormat::Png,
        Err(Error::Argument),
    );
}

fn bitmap_scenes(session: &Session, directory: &Path) {
    for width in [320u16, 640] {
        let stride = width + 5;
        let mut pixels = vec![0; 199 * usize::from(stride) + usize::from(width)];
        for pixel_width in [1u8, 2] {
            pixels.fill(0xdeadbeef);
            for y in 0..200 {
                for x in 0..usize::from(width) {
                    pixels[y * usize::from(stride) + x] = 0xab000000
                        | (((x & 255) as u32) << 16)
                        | ((y as u32) << 8)
                        | ((x * 3 + y * 5) & 255) as u32;
                }
            }
            assert_eq!(
                session.present_bitmap(
                    &pixels[..pixels.len() - 1],
                    width,
                    200,
                    stride,
                    pixel_width
                ),
                Err(Error::Argument)
            );
            session
                .present_bitmap(&pixels, width, 200, stride, pixel_width)
                .unwrap();
            pixels.fill(0);
            capture(
                session,
                directory,
                &format!("bitmap-{width}-{pixel_width}.png"),
                SnapshotFormat::Png,
            )
            .unwrap();
            capture(
                session,
                directory,
                &format!("bitmap-{width}-{pixel_width}.json"),
                SnapshotFormat::Attributes,
            )
            .unwrap();
            for (format, _) in &FORMATS[1..6] {
                assert_eq!(
                    capture(session, directory, "bitmap-invalid", *format),
                    Err(Error::Argument)
                );
            }
        }
    }
    request(
        session,
        directory,
        "bitmap-signal.png",
        SnapshotFormat::Png,
        Ok(()),
    );
    request(
        session,
        directory,
        "missing-parent/output.png",
        SnapshotFormat::Png,
        Err(Error::System),
    );
    let mut descriptors = [-1; 2];
    assert_eq!(unsafe { libc::pipe(descriptors.as_mut_ptr()) }, 0);
    let reader = unsafe { File::from_raw_fd(descriptors[0]) };
    let writer = unsafe { File::from_raw_fd(descriptors[1]) };
    drop(reader);
    assert_eq!(
        session.snapshot_write_fd(writer.as_fd(), SnapshotFormat::Png),
        Err(Error::System)
    );
    assert_ne!(
        unsafe { libc::fcntl(writer.as_raw_fd(), libc::F_GETFD) },
        -1
    );
    drop(writer);
    let readonly = File::open(directory.join("bitmap-640-2.png")).unwrap();
    assert_eq!(
        session.snapshot_write_fd(readonly.as_fd(), SnapshotFormat::Png),
        Err(Error::System)
    );
}

fn fifo_failures_and_shutdown(session: Session, directory: &Path) {
    let path = directory.join("capture.fifo");
    let c_path = CString::new(path.as_os_str().as_bytes()).unwrap();
    assert_eq!(unsafe { libc::mkfifo(c_path.as_ptr(), 0o600) }, 0);
    request(
        &session,
        directory,
        "capture.fifo",
        SnapshotFormat::Png,
        Err(Error::System),
    );
    let reader = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NONBLOCK)
        .open(&path)
        .unwrap();
    let mut writer = OpenOptions::new()
        .write(true)
        .custom_flags(libc::O_NONBLOCK)
        .open(&path)
        .unwrap();
    loop {
        match writer.write(&[0; 4096]) {
            Ok(count) => assert!(count > 0),
            Err(error) => {
                assert_eq!(error.kind(), std::io::ErrorKind::WouldBlock);
                break;
            }
        }
    }
    let started = Instant::now();
    request(
        &session,
        directory,
        "capture.fifo",
        SnapshotFormat::Png,
        Err(Error::System),
    );
    assert!(
        started.elapsed() < Duration::from_secs(2),
        "full FIFO write was not bounded"
    );
    assert_eq!(unsafe { libc::raise(libc::SIGUSR1) }, 0);
    let start = Instant::now();
    drop(session);
    assert!(
        start.elapsed() < Duration::from_secs(2),
        "shutdown blocked on a full FIFO"
    );
    expect_handler(libc::SIGUSR1, libc::SIG_DFL);
    drop(writer);
    drop(reader);
    std::fs::remove_file(path).unwrap();
}

fn environment_opt_in(directory: &Path) {
    for number in [1, 2] {
        // This fixture has one application thread, and the prior Session drop
        // has joined every C worker before changing process environment.
        unsafe {
            std::env::set_var(
                "TIGT_SNAPSHOT_PATH",
                directory.join(format!("environment{number}.utf8")),
            );
            std::env::set_var("TIGT_SNAPSHOT_FORMAT", "utf8");
            if number == 1 {
                std::env::remove_var("TIGT_SNAPSHOT_SIGNAL");
            } else {
                std::env::set_var("TIGT_SNAPSHOT_SIGNAL", "USR2");
            }
        }
        let session = Session::new().unwrap();
        session
            .present_text(&[TextCell::new('E', 0xffffff, 0)], 1, 1, 1)
            .unwrap();
        let before = session.snapshot_status().unwrap().sequence;
        assert_eq!(
            unsafe {
                libc::raise(if number == 1 {
                    libc::SIGUSR1
                } else {
                    libc::SIGUSR2
                })
            },
            0
        );
        await_request(&session, before, Ok(()));
        drop(session);
        expect_handler(libc::SIGUSR1, libc::SIG_DFL);
        expect_handler(libc::SIGUSR2, libc::SIG_DFL);
    }
    unsafe {
        std::env::set_var("TIGT_SNAPSHOT_FORMAT", "not-a-format");
    }
    assert!(matches!(Session::new(), Err(Error::Argument)));
    expect_handler(libc::SIGUSR2, libc::SIG_DFL);
    unsafe {
        std::env::remove_var("TIGT_SNAPSHOT_PATH");
        std::env::remove_var("TIGT_SNAPSHOT_FORMAT");
        std::env::remove_var("TIGT_SNAPSHOT_SIGNAL");
    }
}

fn display_technology(directory: &Path) {
    let (sender, receiver) = std::sync::mpsc::channel();
    let start = || {
        let sender = sender.clone();
        Session::with_input_and_graphics(tigt::GraphicsMode::Blocks, move |event| {
            if event.key == InputKey::Char('n') && event.kind == InputKind::Press {
                sender.send(()).unwrap();
            }
        })
        .unwrap()
    };
    let mut session = start();
    for stage in 0..17 {
        if stage == 4 || stage == 7 {
            session.suspend();
            assert_eq!(
                session.set_display_technology(DisplayTechnology::Generic),
                Err(Error::Busy)
            );
            session.resume().unwrap();
        }
        if stage == 15 || stage == 16 {
            drop(session);
            session = start();
        }
        if stage == 9 || stage == 13 {
            session
                .set_display_technology(DisplayTechnology::Generic)
                .unwrap();
        }
        if stage != 0 && stage != 15 {
            session
                .set_display_technology(DisplayTechnology::Mda)
                .unwrap();
        }
        if stage == 2 {
            let mut rejected = [TextCell::new('X', 0xaaaaaa, 0); 2];
            rejected[0].flags = TEXT_CURSOR;
            rejected[1].flags = TEXT_CURSOR;
            assert_eq!(
                session.present_text(&rejected, 2, 1, 2),
                Err(Error::Argument)
            );
        }
        if stage == 3 || stage == 8 {
            session
                .present_bitmap(&vec![0; 640 * 200], 640, 200, 640, 2)
                .unwrap();
        }
        if stage != 1 && stage != 12 && stage != 13 {
            let mut cells = [TextCell::new(' ', 0xaaaaaa, 0); 10];
            // Unique but invisible: observation can acknowledge the exact
            // rendered stage without releasing the first-visible-output latch.
            cells[0] = TextCell::new(char::from(b'a' + stage), 0, 0);
            cells[4] = TextCell::new('X', 0xaaaaaa, 0);
            cells[5] = cells[4]; // visible stride padding is not part of the frame
            if stage == 2 {
                cells[6..10].fill(TextCell::new('A', 0, 0));
            }
            if stage == 3 {
                for (cell, character) in cells[6..10]
                    .iter_mut()
                    .zip(['\u{a0}', '\u{2002}', '\u{202f}', '\u{2800}'])
                {
                    cell.codepoint = character as u32;
                }
            }
            if stage == 5 {
                cells[6].codepoint = 'X' as u32;
            }
            if stage == 10 || stage == 11 {
                cells[6].flags = TEXT_UNDERLINE;
                if stage == 10 {
                    cells[6].foreground = 0;
                }
            }
            cells[6 + usize::from(stage % 4)].flags |= TEXT_CURSOR;
            session.present_text(&cells, 4, 2, 6).unwrap();
            cells.fill(TextCell::new(' ', 0, 0));
        }
        capture(
            &session,
            directory,
            &format!("display-{stage}.json"),
            SnapshotFormat::Attributes,
        )
        .unwrap();
        receiver.recv_timeout(Duration::from_secs(20)).unwrap();
    }
}

fn main() {
    if std::env::args_os().nth(1).as_deref() == Some(std::ffi::OsStr::new("--display-tests")) {
        let directory = std::env::args_os()
            .nth(2)
            .expect("display output directory");
        display_technology(Path::new(&directory));
        return;
    }
    let directory = std::env::args_os()
        .nth(1)
        .expect("snapshot fixture OUTPUT_DIRECTORY");
    let directory = Path::new(&directory);
    let mut session = Session::new().unwrap();
    expect_handler(libc::SIGUSR1, libc::SIG_DFL);
    expect_handler(libc::SIGUSR2, libc::SIG_DFL);
    assert_eq!(session.snapshot_status().unwrap().sequence, 0);
    assert_eq!(
        capture(&session, directory, "no-frame", SnapshotFormat::Utf8),
        Err(Error::Busy)
    );
    request(
        &session,
        directory,
        "no-frame-signal",
        SnapshotFormat::Utf8,
        Err(Error::Busy),
    );
    session.configure_snapshot(None).unwrap();
    expect_handler(libc::SIGUSR1, libc::SIG_DFL);
    let invalid_path = Path::new(std::ffi::OsStr::from_bytes(b"bad\0path"));
    assert_eq!(
        session.configure_snapshot(Some(SnapshotConfig {
            signal: SnapshotSignal::Usr1,
            format: SnapshotFormat::Utf8,
            path: invalid_path
        })),
        Err(Error::Argument)
    );
    session
        .set_overscan(&Overscan {
            color: 0x102030,
            left: 1,
            right: 2,
            top: 3,
            bottom: 4,
        })
        .unwrap();
    text_scene(&session, directory);
    let resumed_path = directory.join("resumed.utf8");
    session
        .configure_snapshot(Some(SnapshotConfig {
            signal: SnapshotSignal::Usr1,
            format: SnapshotFormat::Utf8,
            path: &resumed_path,
        }))
        .unwrap();
    session.suspend();
    let before = session.snapshot_status().unwrap().sequence;
    assert_eq!(unsafe { libc::raise(libc::SIGUSR1) }, 0);
    await_request(&session, before, Err(Error::Busy));
    session.resume().unwrap();
    let before = session.snapshot_status().unwrap().sequence;
    assert_eq!(unsafe { libc::raise(libc::SIGUSR1) }, 0);
    await_request(&session, before, Ok(()));
    bitmap_scenes(&session, directory);
    fifo_failures_and_shutdown(session, directory);
    let session = Session::new().unwrap();
    assert_eq!(session.snapshot_status().unwrap().sequence, 0);
    assert_eq!(
        capture(&session, directory, "new-session", SnapshotFormat::Png),
        Err(Error::Busy)
    );
    session
        .present_text(&[TextCell::new('A', 0xffffff, 0)], 1, 1, 1)
        .unwrap();
    assert_eq!(
        capture(
            &session,
            directory,
            "new-session-font.png",
            SnapshotFormat::Png
        ),
        Err(Error::Argument)
    );
    drop(session);
    environment_opt_in(directory);
    println!("Rust snapshot fixture completed");
}
