// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

#![cfg(unix)]

use std::collections::BTreeSet;
use std::fs::{self, File};
use std::io::{Read, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::time::{Duration, Instant};
use tigt_gfxreader::terminal::{COLS, Terminal};
use tigt_gfxreader::{Options, analyze};

#[cfg(feature = "test-fixtures")]
#[path = "fixtures/snapshots.rs"]
mod snapshots;

const PAIRS: [(usize, usize); 13] = [
    (2, 6),
    (0, 7),
    (7, 0),
    (15, 0),
    (0, 15),
    (15, 15),
    (7, 7),
    (0, 0),
    (7, 15),
    (15, 7),
    (15, 2),
    (7, 2),
    (0, 2),
];
const XTERM: [usize; 16] = [
    16, 20, 40, 44, 160, 164, 136, 251, 239, 62, 83, 87, 167, 207, 227, 231,
];
const FONT_OFFSET: usize = 0xfa6e;
const CONTROLS: &[u8] =
    b"\x16\x03\x16\x1a\x16\x1b[99;5:1u\x1b[99;5:2u\x1b[99;5:3u\x16\x1b[122;5:1u\x1b[122;5:2u\x1b[122;5:3uq";

struct Fixture {
    directory: Option<tempfile::TempDir>,
    binary: PathBuf,
}

impl Drop for Fixture {
    fn drop(&mut self) {
        if std::thread::panicking() {
            if let Some(directory) = self.directory.take() {
                eprintln!(
                    "fixture binary/output retained at {}",
                    directory.keep().display()
                );
            }
        }
    }
}

impl Fixture {
    fn build(name: &str, renderer: bool, source_included: bool) -> Self {
        let directory = tempfile::Builder::new()
            .prefix("tigt-pty-")
            .tempdir()
            .unwrap();
        let fixture = Self {
            binary: directory.path().join(name),
            directory: Some(directory),
        };
        let root = Path::new(env!("CARGO_MANIFEST_DIR"));
        let rustc = Command::new(std::env::var_os("RUSTC").unwrap_or_else(|| "rustc".into()))
            .arg("-vV")
            .output()
            .expect("Rust compiler is required to identify the native C target");
        assert!(
            rustc.status.success(),
            "rustc -vV failed: {}",
            String::from_utf8_lossy(&rustc.stderr)
        );
        let version = String::from_utf8(rustc.stdout).unwrap();
        let host = version
            .lines()
            .find_map(|line| line.strip_prefix("host: "))
            .expect("rustc host triple");
        let mut build = cc::Build::new();
        build
            .host(host)
            .target(host)
            .opt_level(1)
            .debug(true)
            .cargo_metadata(false)
            .out_dir(fixture.directory.as_ref().unwrap().path())
            .std("c11")
            .define("_XOPEN_SOURCE", "700")
            .define("_XOPEN_SOURCE_EXTENDED", "1")
            .define("_DEFAULT_SOURCE", "1")
            .include(root.join("include"));
        let mut command = build
            .try_get_compiler()
            .expect("a working native C compiler is required")
            .to_command();
        command
            .arg("-UNDEBUG")
            .arg(root.join(format!("tests/{name}.c")))
            .arg(root.join("src/input.c"));
        if renderer && !source_included {
            command.arg(root.join("src/tigt.c"));
        }
        if renderer {
            command
                .arg(root.join("src/graphics.c"))
                .arg(root.join("src/snapshot.c"))
                .arg(root.join("src/video.c"))
                .arg(root.join("src/presenter.c"));
            command.arg(root.join("src/terminal.c"));
            let png = pkg_config::Config::new()
                .cargo_metadata(false)
                .probe("libpng")
                .expect("libpng development headers and library are required");
            for path in png.include_paths {
                command.arg("-I").arg(path);
            }
            for path in png.link_paths {
                command.arg("-L").arg(path);
            }
            for library in png.libs {
                command.arg(format!("-l{library}"));
            }
            if cfg!(feature = "libcaca") {
                let caca = pkg_config::Config::new()
                    .cargo_metadata(false)
                    .probe("caca")
                    .expect("the libcaca feature requires libcaca development headers and library");
                command.arg("-DTIGT_HAVE_LIBCACA=1");
                for path in caca.include_paths {
                    command.arg("-I").arg(path);
                }
                for (name, value) in caca.defines {
                    command.arg(match value {
                        Some(value) => format!("-D{name}={value}"),
                        None => format!("-D{name}"),
                    });
                }
                for path in caca.link_paths {
                    command.arg("-L").arg(path);
                }
                for path in caca.framework_paths {
                    command.arg("-F").arg(path);
                }
                for library in caca.libs {
                    command.arg(format!("-l{library}"));
                }
                for framework in caca.frameworks {
                    command.arg("-framework").arg(framework);
                }
            }
            let curses = ["ncursesw", "ncurses"].into_iter().find_map(|name| {
                pkg_config::Config::new()
                    .cargo_metadata(false)
                    .probe(name)
                    .ok()
            });
            if let Some(curses) = curses {
                for path in curses.include_paths {
                    command.arg("-I").arg(path);
                }
                for (name, value) in curses.defines {
                    command.arg(match value {
                        Some(value) => format!("-D{name}={value}"),
                        None => format!("-D{name}"),
                    });
                }
                for path in curses.link_paths {
                    command.arg("-L").arg(path);
                }
                for path in curses.framework_paths {
                    command.arg("-F").arg(path);
                }
                for library in curses.libs {
                    command.arg(format!("-l{library}"));
                }
                for framework in curses.frameworks {
                    command.arg("-framework").arg(framework);
                }
            } else {
                command.arg(if cfg!(target_os = "macos") {
                    "-lncurses"
                } else {
                    "-lncursesw"
                });
            }
            command.arg("-pthread");
        }
        command.arg("-o").arg(&fixture.binary);
        let output = command
            .output()
            .unwrap_or_else(|error| panic!("running {command:?}: {error}"));
        fs::write(fixture.path("compiler.stdout"), &output.stdout).unwrap();
        fs::write(fixture.path("compiler.stderr"), &output.stderr).unwrap();
        assert!(
            output.status.success(),
            "C fixture compilation failed: {command:?}\n{}",
            String::from_utf8_lossy(&output.stderr)
        );
        fixture
    }

    fn path(&self, name: &str) -> PathBuf {
        self.directory.as_ref().unwrap().path().join(name)
    }

    fn capture(
        &self,
        name: &str,
        arguments: &[&str],
        expected_frame: Option<&[u8]>,
        font: &[u8],
    ) -> Vec<u8> {
        let mut acknowledged = false;
        let bytes = self.capture_observing(name, arguments, |bytes, master| {
            if !acknowledged {
                if let Some(expected) = expected_frame {
                    if let Ok(analysis) = analyze(bytes, font, &options()) {
                        if analysis.report.success && analysis.rgba == expected {
                            master
                                .write_all(CONTROLS)
                                .map_err(|error| error.to_string())?;
                            acknowledged = true;
                        }
                    }
                }
            }
            Ok(())
        });
        assert!(
            expected_frame.is_none() || acknowledged,
            "{name}: frame was never acknowledged"
        );
        bytes
    }

    fn capture_observing(
        &self,
        name: &str,
        arguments: &[&str],
        observe: impl FnMut(&[u8], &mut File) -> Result<(), String>,
    ) -> Vec<u8> {
        self.capture_program(&self.binary, name, arguments, observe)
    }

    fn capture_program(
        &self,
        program: &Path,
        name: &str,
        arguments: &[&str],
        mut observe: impl FnMut(&[u8], &mut File) -> Result<(), String>,
    ) -> Vec<u8> {
        let (mut master_fd, mut slave_fd) = (-1, -1);
        let mut size = libc::winsize {
            ws_row: 80,
            ws_col: 320,
            ws_xpixel: 0,
            ws_ypixel: 0,
        };
        // openpty returns two owned descriptors. CLOEXEC keeps the parent's
        // master and surplus slave copies out of the child; Stdio duplicates
        // the slave onto exactly stdin/stdout/stderr before exec.
        assert_eq!(
            unsafe {
                libc::openpty(
                    &mut master_fd,
                    &mut slave_fd,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    &mut size,
                )
            },
            0,
            "openpty: {}",
            std::io::Error::last_os_error()
        );
        let mut master = unsafe { File::from_raw_fd(master_fd) };
        let slave = unsafe { File::from_raw_fd(slave_fd) };
        for fd in [master.as_raw_fd(), slave.as_raw_fd()] {
            assert_ne!(
                unsafe { libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC) },
                -1
            );
        }
        let locale = if cfg!(target_os = "macos") {
            "en_US.UTF-8"
        } else {
            "C.UTF-8"
        };
        let mut command = Command::new(program);
        command
            .args(arguments)
            .env("TERM", "xterm-256color")
            .env("LANG", locale)
            .env("LC_ALL", locale)
            .stdin(slave.try_clone().unwrap())
            .env_remove("TIGT_SNAPSHOT_PATH")
            .env_remove("TIGT_SNAPSHOT_FORMAT")
            .env_remove("TIGT_SNAPSHOT_SIGNAL")
            .stdout(slave.try_clone().unwrap())
            .stderr(slave);
        // A TTY fd alone is not foreground ownership. Give the subprocess its
        // own controlling terminal without touching the test runner's session.
        unsafe {
            command.pre_exec(|| {
                if libc::setsid() < 0 || libc::ioctl(0, libc::TIOCSCTTY as _, 0) < 0 {
                    return Err(std::io::Error::last_os_error());
                }
                Ok(())
            });
        }
        let child = command
            .spawn()
            .expect("launching compiled C fixture in its own PTY");
        let mut child = ChildGuard(child);
        // Command retains configured stdio handles. Linux cannot report PTY EOF
        // until the parent's slave copies are closed as well as the child's.
        drop(command);
        let mut bytes = Vec::new();
        let deadline = Instant::now() + Duration::from_secs(30);
        let result = (|| -> Result<ExitStatus, String> {
            let mut buffer = [0u8; 65536];
            loop {
                if Instant::now() >= deadline {
                    return Err(format!(
                        "{name}: timed out waiting for renderer output or orderly exit"
                    ));
                }
                let mut descriptor = libc::pollfd {
                    fd: master.as_raw_fd(),
                    events: libc::POLLIN,
                    revents: 0,
                };
                let count = unsafe { libc::poll(&mut descriptor, 1, 50) };
                if count < 0 {
                    let error = std::io::Error::last_os_error();
                    if error.kind() == std::io::ErrorKind::Interrupted {
                        continue;
                    }
                    return Err(format!("poll: {error}"));
                }
                if count != 0 {
                    match master.read(&mut buffer) {
                        Ok(0) => break,
                        Ok(count) => bytes.extend_from_slice(&buffer[..count]),
                        Err(error) if error.raw_os_error() == Some(libc::EIO) => break,
                        Err(error) if error.kind() == std::io::ErrorKind::Interrupted => continue,
                        Err(error) => return Err(format!("reading PTY: {error}")),
                    }
                    continue;
                }
                // Observe only after draining the PTY. A quiet pipe alone is
                // not success: consumers acknowledge exact frames.
                observe(&bytes, &mut master)?;
            }
            loop {
                if let Some(status) = child.0.try_wait().map_err(|error| error.to_string())? {
                    return Ok(status);
                }
                if Instant::now() >= deadline {
                    return Err(format!("{name}: child closed PTY but did not exit"));
                }
                std::thread::sleep(Duration::from_millis(5));
            }
        })();
        let output = self.path(&format!("{name}.pty"));
        fs::write(&output, &bytes).unwrap();
        let status =
            result.unwrap_or_else(|error| panic!("{error}; transcript: {}", output.display()));
        assert!(
            status.success(),
            "{name}: fixture exited {status}; transcript: {}\n{}",
            output.display(),
            String::from_utf8_lossy(&bytes)
        );
        bytes
    }
}

struct ChildGuard(Child);
impl Drop for ChildGuard {
    fn drop(&mut self) {
        if !matches!(self.0.try_wait(), Ok(Some(_))) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
}

fn synthetic_font() -> Vec<u8> {
    let mut rom = vec![0u8; FONT_OFFSET + 128 * 8];
    for code in 0..128 {
        for row in 0..8 {
            rom[FONT_OFFSET + code * 8 + row] = match code {
                0 => 0,
                127 => 255,
                _ => ((code * 37 + row * 19) ^ (code << (row % 3))) as u8,
            };
        }
    }
    rom
}

fn options() -> Options {
    Options {
        width: 320,
        height: 200,
        font_offset: FONT_OFFSET,
        glyph_count: 128,
        region: None,
        expect: None,
    }
}

fn rendered_rgb(color: usize, fg: usize, bg: usize) -> [u8; 3] {
    if [fg, bg].iter().all(|color| matches!(color, 0 | 7 | 15))
        && !([fg, bg].contains(&7) && [fg, bg].contains(&15))
        && matches!(color, 7 | 15)
    {
        return [192; 3]; // Terminal theme foreground; bold is not an RGB promise.
    }
    let index = XTERM[color];
    if index >= 232 {
        return [8 + ((index - 232) * 10) as u8; 3];
    }
    let index = index - 16;
    let levels = [0, 95, 135, 175, 215, 255];
    [levels[index / 36], levels[index / 6 % 6], levels[index % 6]]
}

fn expected_pixels(font: &[u8], fg: usize, bg: usize) -> Vec<u8> {
    let source = |x: usize, y: usize| {
        let code = (y / 8 * 40 + x / 8) % 128;
        if font[FONT_OFFSET + code * 8 + y % 8] & (0x80 >> (x % 8)) != 0 {
            fg
        } else {
            bg
        }
    };
    let mut rgba = Vec::with_capacity(320 * 200 * 4);
    for y in 0..200 {
        for x in 0..320 {
            let color = source(x, y);
            let mut local_fg = fg;
            let mut local_bg = bg;
            if [fg, bg].contains(&7) && [fg, bg].contains(&15) {
                let first = source(x / 2 * 2, y / 3 * 3);
                if (0..6)
                    .all(|bit| source(x / 2 * 2 + bit % 2, (y / 3 * 3 + bit / 2).min(199)) == first)
                {
                    local_fg = first;
                    local_bg = first;
                }
            }
            rgba.extend_from_slice(&rendered_rgb(color, local_fg, local_bg));
            rgba.push(255);
        }
    }
    rgba
}

fn check_frame(capture: &[u8], font: &[u8], fg: usize, bg: usize) {
    let analysis = analyze(capture, font, &options()).expect("valid font and dimensions");
    if [fg, bg].contains(&7) && [fg, bg].contains(&15) {
        // A sextant cannot emphasize only its background. Solid cells use
        // defaults, mixed cells retain explicit contrast: more than two RGB
        // values per guest glyph is now valid, so compare the actual raster.
        assert_eq!(analysis.rgba, expected_pixels(font, fg, bg));
        return;
    }
    let report = &analysis.report;
    assert!(
        report.success && report.diagnostics.is_empty(),
        "{fg}-{bg}: {:?}",
        report.diagnostics
    );
    assert_eq!(report.missing_pixel_count, 0, "{fg}-{bg}");
    assert!(report.incomplete_scanlines.is_empty(), "{fg}-{bg}");
    assert_eq!(report.terminal_snapshot, "last_alternate_before_exit");
    assert_eq!(report.glyphs.len(), 1000, "{fg}-{bg}");
    let foreground = rendered_rgb(fg, fg, bg);
    let background = rendered_rgb(bg, fg, bg);
    let mut positions = BTreeSet::new();
    for glyph in &report.glyphs {
        positions.insert((glyph.row, glyph.column));
        let expected = (glyph.row * 40 + glyph.column) % 128;
        let rows = &font[FONT_OFFSET + expected * 8..FONT_OFFSET + expected * 8 + 8];
        let has_ink = rows.iter().any(|row| *row != 0);
        let has_background = rows.iter().any(|row| *row != 255);
        let mut colors = BTreeSet::new();
        if has_ink {
            colors.insert(foreground);
        }
        if has_background {
            colors.insert(background);
        }
        assert_eq!(
            glyph.colors,
            colors.into_iter().collect::<Vec<_>>(),
            "{fg}-{bg}: {glyph:?}"
        );
        if foreground != background {
            assert!(
                glyph
                    .alternatives
                    .iter()
                    .any(|candidate| candidate.code == expected
                        && candidate.foreground_rgb == has_ink.then_some(foreground)
                        && candidate.background_rgb == has_background.then_some(background)),
                "{fg}-{bg}: exact glyph polarity missing: {glyph:?}"
            );
        }
    }
    assert_eq!(
        positions,
        (0..25)
            .flat_map(|row| (0..40).map(move |column| (row, column)))
            .collect()
    );
    let expected = expected_pixels(font, fg, bg);
    if let Some(offset) = analysis
        .rgba
        .iter()
        .zip(&expected)
        .position(|(actual, wanted)| actual != wanted)
    {
        panic!(
            "{fg}-{bg}: pixel ({},{}) component {} differs: {} != {}",
            offset / 4 % 320,
            offset / 4 / 320,
            offset % 4,
            analysis.rgba[offset],
            expected[offset]
        );
    }
    assert_eq!(analysis.rgba.len(), expected.len());
}

#[test]
fn output_presenter_preserves_glass_bytes_and_adaptive_transitions() {
    let fixture = Fixture::build("presenter_fixture", true, false);
    let output = Command::new(&fixture.binary).output().unwrap();
    assert!(
        output.status.success(),
        "presenter fixture failed:\n{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn rust_presenter_resumes_owned_frame_after_backpressure() {
    use std::{
        io::{Read, Write},
        os::{fd::AsFd, unix::net::UnixStream},
    };
    use tigt::{
        TextCell,
        presenter::{
            Config, Cursor, Encoding, Error, Frame, Mode, Notification, Presenter, Progress,
            RefreshRate, Reversibility, Status,
        },
    };

    let (writer, mut reader) = UnixStream::pair().unwrap();
    writer.set_nonblocking(true).unwrap();
    reader.set_nonblocking(true).unwrap();
    let mut output = &writer;
    let mut padding = 0;
    loop {
        match output.write(&[b'#'; 4096]) {
            Ok(count) => {
                assert_ne!(count, 0);
                padding += count;
            }
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => break,
            Err(error) => panic!("filling destination: {error}"),
        }
    }
    let mut presenter = Presenter::new(
        writer.as_fd(),
        Config {
            mode: Mode::Glass,
            encoding: Encoding::Ascii,
            reversibility: Reversibility::OneWay,
        },
    )
    .unwrap();
    let mut cells = [TextCell {
        codepoint: b' ' as u32,
        foreground: 0,
        background: 0,
        flags: 0,
    }; 4];
    cells[0].codepoint = b'A' as u32;
    cells[1].codepoint = b'B' as u32;
    presenter
        .notify(Notification {
            operation_id: 1,
            text: &[b'A' as u32, b'B' as u32],
            boundaries: &[],
            columns: 4,
            rows: 1,
            start: Cursor { column: 0, row: 0 },
        })
        .unwrap();
    assert_eq!(
        presenter.present_nonblocking(Frame {
            cells: &cells,
            columns: 4,
            rows: 1,
            stride: 4,
            cursor: Cursor { column: 2, row: 0 },
            refresh_rate: RefreshRate::Hz60,
            hints: Default::default(),
        }),
        Ok(Progress::WouldBlock)
    );
    cells[0].codepoint = b'Z' as u32;
    assert_eq!(presenter.resume(), Ok(Progress::WouldBlock));
    assert_eq!(presenter.notification_stats().unwrap().consumed, 0);
    let mut discarded = vec![0; padding];
    reader.read_exact(&mut discarded).unwrap();
    assert!(discarded.iter().all(|byte| *byte == b'#'));
    assert_eq!(presenter.resume(), Ok(Progress::Complete(Status::Glass)));
    let mut actual = [0; 2];
    reader.read_exact(&mut actual).unwrap();
    assert_eq!(&actual, b"AB");
    assert_eq!(presenter.notification_stats().unwrap().consumed, 1);
    assert_eq!(presenter.resume(), Err(Error::Busy));
    assert_eq!(
        reader.read(&mut actual).unwrap_err().kind(),
        std::io::ErrorKind::WouldBlock
    );
}

#[test]
fn production_palette_masks_transitions_bounds_and_all_thirteen_font_color_pairs() {
    let fixture = Fixture::build("renderer_fixture", true, true);
    let font = synthetic_font();
    let rom = fixture.path("synthetic-font.bin");
    fs::write(&rom, &font).unwrap();
    fixture.capture("policy", &["--policy-tests"], None, &font);
    for (fg, bg) in PAIRS {
        let name = format!("{fg}-{bg}");
        let capture = fixture.capture(
            &name,
            &[
                "--rom",
                rom.to_str().unwrap(),
                &fg.to_string(),
                &bg.to_string(),
            ],
            None,
            &font,
        );
        check_frame(&capture, &font, fg, bg);
    }
    // Extraction leaves CLI coverage in the independent analyzer project.
    // This consumer verifies the API still decodes a real production capture.
    let analysis = analyze(
        &fs::read(fixture.path("2-6.pty")).unwrap(),
        &font,
        &options(),
    )
    .unwrap();
    assert!(analysis.report.success);
    assert_eq!(analysis.rgba, expected_pixels(&font, 2, 6));
}

#[test]
fn public_session_copies_frames_dispatches_controls_and_restores_terminal() {
    let fixture = Fixture::build("session_fixture", true, false);
    let font = synthetic_font();
    let expected = expected_pixels(&font, 7, 0);
    let capture = fixture.capture("session", &[], Some(&expected), &font);
    check_frame(&capture, &font, 7, 0);
    let output = Command::new(&fixture.binary)
        .arg("--no-terminal")
        .stdin(Stdio::null())
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "non-TTY initialization: {}",
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn terminal_lifecycle_preserves_job_control_signal_and_background_contracts() {
    let fixture = Fixture::build("lifecycle_fixture", true, false);
    let (mut raw_sent, mut cooked_sent) = (false, false);
    let capture = fixture.capture_observing("lifecycle", &[], |bytes, master| {
        if !raw_sent
            && bytes
                .windows(b"QUOTE-RAW-READY".len())
                .any(|w| w == b"QUOTE-RAW-READY")
        {
            master.write_all(
                b"\x16\x1b[99;5:1u\x1b[99;5:2u\x1b[99;5:3u\x16\x16\x16\x1a\x1b[200~\x03\x1b[201~q",
            ).map_err(|error| error.to_string())?;
            raw_sent = true;
        }
        if !cooked_sent
            && bytes
                .windows(b"QUOTE-COOKED-READY".len())
                .any(|w| w == b"QUOTE-COOKED-READY")
        {
            // The line discipline consumes each VLNEXT, retaining literal C/V.
            master
                .write_all(b"\x16\x03\x16\x16\n")
                .map_err(|error| error.to_string())?;
            cooked_sent = true;
        }
        Ok(())
    });
    assert!(
        raw_sent && cooked_sent,
        "both real PTY input paths must complete"
    );
    assert!(
        capture.windows(b"alive".len()).any(|w| w == b"alive"),
        "ending input must preserve real glass output"
    );
    let pipe_begin = capture
        .windows(b"PIPE-CLEANUP-BEGIN".len())
        .position(|w| w == b"PIPE-CLEANUP-BEGIN")
        .unwrap();
    let pipe_end = capture[pipe_begin..]
        .windows(b"PIPE-CLEANUP-END".len())
        .position(|w| w == b"PIPE-CLEANUP-END")
        .unwrap()
        + pipe_begin;
    let pipe_cleanup = &capture[pipe_begin..pipe_end];
    for restore in [b"\x1b[<u".as_slice(), b"\x1b[?2004r"] {
        assert_eq!(
            pipe_cleanup
                .windows(restore.len())
                .filter(|w| *w == restore)
                .count(),
            2,
            "custom and default SIGPIPE must each restore input protocols once through the surviving tty"
        );
    }
    let begin = capture
        .windows(b"GLASS-BG-BEGIN".len())
        .position(|w| w == b"GLASS-BG-BEGIN")
        .unwrap()
        + b"GLASS-BG-BEGIN".len();
    let end = capture[begin..]
        .windows(b"GLASS-BG-END".len())
        .position(|w| w == b"GLASS-BG-END")
        .unwrap()
        + begin;
    assert!(
        !capture[begin..end].contains(&0x1b),
        "background glass emitted terminal protocols"
    );
}

fn sixel_number(bytes: &[u8], cursor: &mut usize) -> Result<usize, String> {
    let start = *cursor;
    let mut number = 0usize;
    while let Some(digit) = bytes.get(*cursor).filter(|byte| byte.is_ascii_digit()) {
        number = number
            .checked_mul(10)
            .and_then(|number| number.checked_add((digit - b'0') as usize))
            .ok_or("overflowing sixel parameter")?;
        *cursor += 1;
    }
    if *cursor == start {
        return Err("missing sixel parameter".into());
    }
    Ok(number)
}

fn sixel_parameters<const N: usize>(
    bytes: &[u8],
    cursor: &mut usize,
) -> Result<[usize; N], String> {
    let mut values = [0; N];
    for (index, value) in values.iter_mut().enumerate() {
        if index != 0 {
            if bytes.get(*cursor) != Some(&b';') {
                return Err("missing sixel parameter separator".into());
            }
            *cursor += 1;
        }
        *value = sixel_number(bytes, cursor)?;
    }
    Ok(values)
}

fn next_sixel<'a>(bytes: &'a [u8], cursor: &mut usize) -> Option<&'a [u8]> {
    while let Some(start) = bytes[*cursor..]
        .windows(2)
        .position(|pair| pair == b"\x1bP")
        .map(|offset| *cursor + offset + 2)
    {
        let Some(end) = bytes[start..]
            .windows(2)
            .position(|pair| pair == b"\x1b\\")
            .map(|offset| start + offset)
        else {
            // Keep an incomplete DCS until its terminating ST arrives.
            *cursor = start - 2;
            return None;
        };
        *cursor = end + 2;
        if let Some(header) = bytes[start..end].iter().position(|byte| *byte == b'q') {
            return Some(&bytes[start + header + 1..end]);
        }
    }
    None
}

fn check_sixel_raster(
    bytes: &[u8],
    expected_width: usize,
    expected_height: usize,
    scene: usize,
) -> Result<(), String> {
    if bytes.first() != Some(&b'"') {
        return Err("sixel frame has no raster attributes".into());
    }
    let mut cursor = 1;
    let [pan, pad, width, height] = sixel_parameters(bytes, &mut cursor)?;
    if (pan, pad) != (1, 1) || (width, height) != (expected_width, expected_height) {
        return Err(format!(
            "sixel raster {pan}:{pad} {width}x{height}, expected square pixels {expected_width}x{expected_height}"
        ));
    }
    // Independently decode actual palette definitions, runs and bitplanes.
    // The sentinel catches holes: raster attributes alone are not a frame.
    let mut pixels = vec![u32::MAX; width * height];
    let mut palette = [u32::MAX; 256];
    let (mut x, mut y, mut color) = (0usize, 0usize, 0usize);
    while cursor < bytes.len() {
        let command = bytes[cursor];
        cursor += 1;
        match command {
            b'#' => {
                color = sixel_number(bytes, &mut cursor)?;
                if color >= palette.len() {
                    return Err("sixel palette index exceeds 255".into());
                }
                if bytes.get(cursor) == Some(&b';') {
                    cursor += 1;
                    let [space, red, green, blue] = sixel_parameters(bytes, &mut cursor)?;
                    if space != 2 || red > 100 || green > 100 || blue > 100 {
                        return Err("invalid sixel RGB definition".into());
                    }
                    palette[color] = ((red * 255 / 100) << 16
                        | (green * 255 / 100) << 8
                        | blue * 255 / 100) as u32;
                }
            }
            b'$' => x = 0,
            b'-' => {
                x = 0;
                y += 6;
            }
            b'!' | b'?'..=b'~' => {
                let (count, mask) = if command == b'!' {
                    let count = sixel_number(bytes, &mut cursor)?;
                    let mask = *bytes.get(cursor).ok_or("missing repeated sixel")?;
                    cursor += 1;
                    (count, mask)
                } else {
                    (1, command)
                };
                if !(b'?'..=b'~').contains(&mask)
                    || count == 0
                    || count > width.saturating_sub(x)
                    || palette[color] == u32::MAX
                {
                    return Err("invalid sixel run or undefined color".into());
                }
                for bit in 0..6 {
                    if (mask - b'?') & (1 << bit) == 0 {
                        continue;
                    }
                    if y + bit >= height {
                        return Err("sixel paints beyond declared raster height".into());
                    }
                    pixels[(y + bit) * width + x..(y + bit) * width + x + count]
                        .fill(palette[color]);
                }
                x += count;
            }
            _ => return Err(format!("unexpected sixel command {command:#x}")),
        }
    }
    let colors = [0xff0000, 0x00ff00, 0x0000ff, 0xffffff];
    for (index, actual) in pixels.into_iter().enumerate() {
        let (x, y) = (index % width, index / width);
        let quadrant = usize::from(y * 2 >= height) * 2 + usize::from(x * 2 >= width);
        let expected = colors[quadrant ^ scene];
        if actual != expected {
            return Err(format!(
                "sixel pixel ({x},{y}) = {actual:#x}, expected {expected:#x}"
            ));
        }
    }
    Ok(())
}

fn capture_sixel(fixture: &Fixture, scenario: &str, expected: &[(usize, usize, usize)]) {
    let owns_input = scenario.starts_with("query-");
    let mut queued_input = false;
    let mut queried = [false; 2];
    let (mut cursor, mut stage) = (0, 0);
    let capture =
        fixture.capture_observing(scenario, &["--sixel-tests", scenario], |bytes, master| {
            if !owns_input
                && !queued_input
                && bytes
                    .windows(b"SIXEL APPLICATION INPUT".len())
                    .any(|part| part == b"SIXEL APPLICATION INPUT")
            {
                master
                    .write_all(b"owned\n")
                    .map_err(|error| error.to_string())?;
                queued_input = true;
            }
            if owns_input {
                for (index, query) in [b"\x1b[16t", b"\x1b[14t"].into_iter().enumerate() {
                    if !queried[index] && bytes.windows(query.len()).any(|part| part == query) {
                        queried[index] = true;
                        let reply: &[u8] = match (index, scenario) {
                            (0, "query-cell") => b"\x1b[6;24;12t",
                            (0, _) => b"",
                            // Deliberately distinct ratios establish cell-report
                            // precedence over a whole-window report.
                            (1, "query-cell") => b"\x1b[4;1600;3200t",
                            _ => b"\x1b[4;1920;3840t",
                        };
                        master.write_all(reply).map_err(|error| error.to_string())?;
                    }
                }
            }
            if stage < expected.len() {
                if let Some(raster) = next_sixel(bytes, &mut cursor) {
                    let (width, height, scene) = expected[stage];
                    check_sixel_raster(raster, width, height, scene)
                        .map_err(|error| format!("{scenario} stage {stage}: {error}"))?;
                    master
                        .write_all(if owns_input { b"n" } else { b"n\n" })
                        .map_err(|error| error.to_string())?;
                    stage += 1;
                }
            }
            Ok(())
        });
    assert_eq!(
        stage,
        expected.len(),
        "{scenario}: missing complete sixel frame"
    );
    if owns_input {
        assert_eq!(
            queried, [true; 2],
            "explicit SIXEL must query pixel metrics"
        );
    } else {
        assert!(queued_input, "application-owned input was never queued");
        for query in [
            b"\x1b[16t".as_slice(),
            b"\x1b[14t",
            b"\x1b[?2;1;0S",
            b"\x1b[c",
        ] {
            assert!(
                !capture.windows(query.len()).any(|part| part == query),
                "output-only session emitted probe {query:?}"
            );
        }
    }
}

fn next_iterm2<'a>(bytes: &'a [u8], cursor: &mut usize) -> Option<&'a [u8]> {
    let prefix = b"\x1b]1337;File=";
    let start = *cursor
        + bytes[*cursor..]
            .windows(prefix.len())
            .position(|p| p == prefix)?;
    let payload = start + prefix.len();
    let end = payload + bytes[payload..].windows(2).position(|p| p == b"\x1b\\")?;
    *cursor = end + 2;
    Some(&bytes[payload..end])
}

fn decode_base64(bytes: &[u8]) -> Result<Vec<u8>, String> {
    if bytes.len() % 4 != 0 {
        return Err("incomplete image base64".into());
    }
    let mut decoded = Vec::with_capacity(bytes.len() / 4 * 3);
    for group in bytes.chunks_exact(4) {
        let mut value = 0u32;
        for byte in group {
            let digit = match byte {
                b'A'..=b'Z' => byte - b'A',
                b'a'..=b'z' => byte - b'a' + 26,
                b'0'..=b'9' => byte - b'0' + 52,
                b'+' => 62,
                b'/' => 63,
                b'=' => 0,
                _ => return Err("invalid image base64".into()),
            };
            value = value << 6 | u32::from(digit);
        }
        decoded.push((value >> 16) as u8);
        if group[2] != b'=' {
            decoded.push((value >> 8) as u8);
        }
        if group[3] != b'=' {
            decoded.push(value as u8);
        }
    }
    Ok(decoded)
}

fn check_iterm2_image(bytes: &[u8], stage: usize) -> Result<(), String> {
    let separator = bytes
        .iter()
        .position(|byte| *byte == b':')
        .ok_or("missing PNG payload")?;
    let fields: std::collections::BTreeMap<_, _> = std::str::from_utf8(&bytes[..separator])
        .map_err(|e| e.to_string())?
        .split(';')
        .filter_map(|field| field.split_once('='))
        .collect();
    let (width, height) = match stage {
        0 | 1 => ("1280px", "960px"),
        2 => ("640px", "360px"),
        _ => ("800px", "450px"),
    };
    for (key, value) in [
        ("inline", "1"),
        ("width", width),
        ("height", height),
        ("preserveAspectRatio", "0"),
        ("doNotMoveCursor", "1"),
    ] {
        if fields.get(key) != Some(&value) {
            return Err(format!(
                "iTerm2 stage {stage}: {key} = {:?}, expected {value}",
                fields.get(key)
            ));
        }
    }
    let encoded = decode_base64(&bytes[separator + 1..])?;
    let mut reader = png::Decoder::new(encoded.as_slice())
        .read_info()
        .map_err(|e| e.to_string())?;
    let mut rgb = vec![0; reader.output_buffer_size()];
    let frame = reader.next_frame(&mut rgb).map_err(|e| e.to_string())?;
    if frame.width != 320
        || frame.height != 200
        || frame.color_type != png::ColorType::Rgb
        || frame.bit_depth != png::BitDepth::Eight
    {
        return Err(format!(
            "iTerm2 PNG did not normalize to a 320x200 RGB image: {frame:?}"
        ));
    }
    let colors = [0x123456u32, 0xabcdef, 0x102030, 0xfedcba];
    for (index, actual) in rgb[..frame.buffer_size()].chunks_exact(3).enumerate() {
        let x = index % 320;
        let y = index / 320;
        let mut expected = colors[usize::from(x >= 160) + 2 * usize::from(y >= 100)];
        if (index == 0 && (1..=5).contains(&stage)) || (stage == 6 && index < 2) {
            expected ^= 0x010101;
        }
        if stage >= 7 && y == 0 {
            expected = if stage == 7 { 0x808080 } else { 0 };
        }
        if stage >= 7 && y == 1 && x == 0 {
            expected = if stage == 7 { 0x5e7799 } else { 0x112233 };
        }
        if actual != &expected.to_be_bytes()[1..] {
            return Err(format!(
                "iTerm2 stage {stage}: pixel {x},{y} = {actual:?}, expected {expected:#x}"
            ));
        }
    }
    Ok(())
}

#[test]
fn bitmap_images_idle_until_rgb_or_presentation_changes() {
    let fixture = Fixture::build("session_fixture", true, false);
    for backend in ["sixel", "iterm2"] {
        let progress = fixture.path(&format!("{backend}-progress"));
        let (mut cursor, mut frames) = (0, 0);
        let (mut idle_since, mut text_since) = (None, None);
        let (mut idle_released, mut text_released) = (false, false);
        let capture = fixture.capture_observing(
            &format!("{backend}-updates"),
            &["--image-tests", backend, progress.to_str().unwrap()],
            |bytes, master| {
                while let Some(image) = if backend == "sixel" {
                    next_sixel(bytes, &mut cursor)
                } else {
                    next_iterm2(bytes, &mut cursor)
                } {
                    if frames >= 9 || frames == 1 && !idle_released || frames == 5 && !text_released
                    {
                        return Err(format!("{backend}: redundant image after {frames} frames"));
                    }
                    if backend == "iterm2" {
                        check_iterm2_image(image, frames)?;
                    }
                    frames += 1;
                    master.write_all(b"n\n").map_err(|e| e.to_string())?;
                }
                let stage = fs::read_to_string(&progress)
                    .ok()
                    .and_then(|s| s.trim().parse::<u8>().ok());
                if !idle_released && stage == Some(1) {
                    let since = idle_since.get_or_insert_with(Instant::now);
                    if since.elapsed() >= Duration::from_millis(120) {
                        if frames != 1 {
                            return Err("initial image missing".into());
                        }
                        master.write_all(b"n\n").map_err(|e| e.to_string())?;
                        idle_released = true;
                    }
                }
                if !text_released && stage == Some(2) {
                    let since = text_since.get_or_insert_with(Instant::now);
                    if since.elapsed() >= Duration::from_millis(120) {
                        let text_output = &bytes[cursor..];
                        let last_clear = text_output.windows(4).rposition(|p| p == b"\x1b[2J");
                        let text = text_output.iter().position(|byte| *byte == b'X');
                        if frames != 5 || !matches!((last_clear, text), (Some(clear), Some(text)) if clear < text)
                        {
                            return Err("image cleanup must precede replacement text".into());
                        }
                        master.write_all(b"n\n").map_err(|e| e.to_string())?;
                        text_released = true;
                    }
                }
                Ok(())
            },
        );
        assert_eq!(
            frames, 9,
            "{backend}: missed RGB/geometry/layout/resize/resume/kind invalidation"
        );
        // Curses may write directly to the fd while image controls use stdio.
        // Both suspend and shutdown must clear the image before restoring the
        // shell, never flush a delayed clear into the normal screen afterward.
        let (mut alternate, mut image_visible, mut exits) = (false, false, 0);
        for offset in 0..capture.len() {
            let output = &capture[offset..];
            if output.starts_with(b"\x1b[?1049h") {
                alternate = true;
            } else if output.starts_with(b"\x1b[?1049l") {
                assert!(
                    !image_visible,
                    "{backend}: image not cleared before terminal restoration"
                );
                alternate = false;
                exits += 1;
            } else if output.starts_with(b"\x1b[2J") {
                assert!(
                    alternate,
                    "{backend}: image cleanup cleared the restored shell"
                );
                image_visible = false;
            } else if output.starts_with(b"\x1bP") || output.starts_with(b"\x1b]1337;File=") {
                image_visible = true;
            }
        }
        assert_eq!(exits, 2, "{backend}: exercise both suspend and shutdown");
        for query in [
            b"\x1b[16t".as_slice(),
            b"\x1b[14t",
            b"\x1b[?2;1;0S",
            b"\x1b[c",
        ] {
            assert!(
                !capture.windows(query.len()).any(|p| p == query),
                "output-only input ownership"
            );
        }
        if backend == "iterm2" {
            assert!(
                !capture.windows(8).any(|p| p == b"?80;1070"),
                "iTerm2 changed sixel modes"
            );
        }
    }
}

#[test]
fn public_sixel_layout_resamples_both_modes_and_redraws_retained_frames() {
    let fixture = Fixture::build("session_fixture", true, false);
    capture_sixel(
        &fixture,
        "geometry",
        &[
            (960, 720, 0),   // 80 cells at 12 pixels per cell, not native 320x200.
            (960, 720, 1),   // 640-dot mode occupies exactly the same rectangle.
            (480, 270, 1),   // Active layout-only change redraws retained pixels.
            (480, 270, 1),   // Invalid setters are atomic; resume preserves layout.
            (1920, 1920, 1), // Suspended setter accepts maximum columns/aspect.
            (960, 720, 1),   // Suspended update survives resume.
            (480, 360, 1),   // Pixel-only resize without a new submission.
            (321, 241, 1),   // Height fit rounds down, including partial sixel band.
            (480, 360, 1),   // Width fit when fewer than 80 cells are available.
            (4096, 3072, 1), // Huge pixel metrics respect the encoder width limit.
            (3072, 4096, 1), // Portrait layout respects the encoder height limit.
            (819, 1, 1),     // Extreme valid aspect cannot round height to zero.
            (960, 720, 0),   // A new session restores all default layout values.
        ],
    );
}

#[test]
fn public_sixel_output_only_falls_back_without_probes_or_consuming_input() {
    let fixture = Fixture::build("session_fixture", true, false);
    capture_sixel(&fixture, "fallback", &[(640, 480, 0)]);
}

#[test]
fn explicit_sixel_queries_metrics_and_refits_after_grid_resize() {
    let fixture = Fixture::build("session_fixture", true, false);
    capture_sixel(
        &fixture,
        "query-cell",
        &[(960, 720, 0), (320, 240, 0), (480, 360, 0)],
    );
    capture_sixel(&fixture, "query-window", &[(960, 720, 0), (320, 240, 0)]);
}

fn resolved_text_matches(terminal: &Terminal, stage: usize) -> bool {
    let (cells, source) = terminal.cells();
    if source != "active_alternate" {
        return false;
    }
    let glyphs = ['A', 'B', 'C', 'D', '☺', 'é', '─', '█'];
    let colors = if stage == 0 {
        ([0, 215, 0], [0, 0, 215])
    } else {
        ([215, 0, 0], [0, 215, 215])
    };
    let decorated = stage != 2;
    for (index, cell) in cells.iter().enumerate() {
        let row = index / COLS;
        let column = index % COLS;
        if row < 2 && column < 4 {
            if cell.character != glyphs[row * 4 + column]
                || cell.style.colors() != colors
                || cell.style.underline != (decorated && row == 0 && column == 0)
                || cell.style.blink
            {
                return false;
            }
        } else if cell.character != ' ' || cell.style.colors().1 != [0; 3] {
            // Metadata must not introduce border cells or shift the content.
            // Returning from bitmap must also erase its larger footprint.
            return false;
        }
    }
    let cursor = terminal.cursor();
    cursor.visible == decorated
        && (!decorated || (cursor.column == 1 && cursor.row == 0 && !cursor.blinking))
}

#[test]
fn public_resolved_text_preserves_rgb_flags_transitions_and_nonvisual_overscan() {
    let fixture = Fixture::build("session_fixture", true, false);
    let font = synthetic_font();
    let blue = expected_pixels(&font, 1, 1);
    let mut stage = 0;
    let mut visible_since = None;
    fixture.capture_observing("resolved-text", &["--text-tests"], |bytes, master| {
        if stage == 5 {
            return Ok(());
        }
        let terminal = Terminal::replay(bytes);
        if !terminal.errors.is_empty() {
            return Err(format!("text stage {stage}: {:?}", terminal.errors));
        }
        let matches = if stage == 3 {
            !terminal.cursor().visible
                && analyze(bytes, &font, &options())
                    .is_ok_and(|analysis| analysis.report.success && analysis.rgba == blue)
        } else {
            resolved_text_matches(&terminal, stage)
        };
        if !matches {
            if visible_since.is_some() {
                return Err("resolved native cursor/text changed without a new frame".into());
            }
            return Ok(());
        }
        if stage == 0 {
            // A submitted visible native cursor must remain steady, not acquire
            // the old library-generated blink phases while its frame is idle.
            let since = visible_since.get_or_insert_with(Instant::now);
            if since.elapsed() < Duration::from_millis(750) {
                return Ok(());
            }
        }
        master.write_all(b"n").map_err(|error| error.to_string())?;
        stage += 1;
        visible_since = None;
        Ok(())
    });
    assert_eq!(
        stage, 5,
        "not all resolved text/bitmap frames were observed"
    );
}

fn display_frame(stage: usize) -> [tigt::TextCell; 8] {
    let source_stage = match stage {
        1 => 0,
        12 | 13 => 11,
        _ => stage,
    };
    let mut cells = [tigt::TextCell::new(' ', 0xaaaaaa, 0); 8];
    cells[0] = tigt::TextCell::new(char::from(b'a' + source_stage as u8), 0, 0);
    match source_stage {
        2 => cells[4..].fill(tigt::TextCell::new('A', 0, 0)),
        3 => {
            for (cell, character) in cells[4..]
                .iter_mut()
                .zip(['\u{a0}', '\u{2002}', '\u{202f}', '\u{2800}'])
            {
                cell.codepoint = character as u32;
            }
        }
        5 => cells[4].codepoint = 'X' as u32,
        10 | 11 => {
            cells[4].flags = tigt::TEXT_UNDERLINE;
            if source_stage == 10 {
                cells[4].foreground = 0;
            }
        }
        _ => {}
    }
    cells[4 + source_stage % 4].flags |= tigt::TEXT_CURSOR;
    cells
}

fn check_display_consumer(fixture: &Fixture, program: &Path, name: &str) {
    // These are observable policy transitions, not a timing-dependent sequence
    // of sleeps: each new frame has an invisible marker and is acknowledged.
    let cursor_visible = [
        true, false, false, false, false, true, true, true, true, false, false, true, true, false,
        false, true, false,
    ];
    let directory = fixture.path(name);
    fs::create_dir(&directory).unwrap();
    let mut stage = 0;
    fixture.capture_program(
        program,
        name,
        &["--display-tests", directory.to_str().unwrap()],
        |bytes, master| {
            if stage == cursor_visible.len() {
                return Ok(());
            }
            let Ok(snapshot) = fs::read(directory.join(format!("display-{stage}.json"))) else {
                return Ok(());
            };
            let Ok(snapshot) = serde_json::from_slice::<serde_json::Value>(&snapshot) else {
                // The producer can still be writing this stage's snapshot.
                return Ok(());
            };
            let expected = display_frame(stage);
            let native = snapshot["cells"]
                .as_array()
                .ok_or("missing native text cells")?;
            if native.len() != expected.len()
                || native.iter().zip(expected).any(|(actual, expected)| {
                    actual["codepoint"] != expected.codepoint
                        || actual["foreground"] != expected.foreground
                        || actual["background"] != expected.background
                        || actual["flags"] != expected.flags
                })
            {
                return Err(format!(
                    "stage {stage}: display policy modified native snapshot"
                ));
            }
            let terminal = Terminal::replay(bytes);
            if !terminal.errors.is_empty() {
                return Err(format!("display stage {stage}: {:?}", terminal.errors));
            }
            let (cells, source) = terminal.cells();
            if source != "active_alternate" {
                return Ok(());
            }
            for (index, expected) in expected.iter().enumerate() {
                let actual = &cells[index / 4 * COLS + index % 4];
                if actual.character as u32 != expected.codepoint
                    || actual.style.underline != (expected.flags & tigt::TEXT_UNDERLINE != 0)
                    || actual.style.blink
                    || actual.style.colors().1 != [0; 3]
                    // Plain-space foreground is not visible; curses can erase
                    // those cells without emitting their foreground style.
                    || (expected.codepoint != ' ' as u32
                        && (actual.style.colors().0 == actual.style.colors().1)
                            != (expected.foreground == expected.background))
                {
                    return Ok(());
                }
            }
            let cursor = terminal.cursor();
            let cursor_index = expected
                .iter()
                .position(|cell| cell.flags & tigt::TEXT_CURSOR != 0)
                .unwrap();
            if cursor.visible != cursor_visible[stage]
                || (cursor.visible
                    && (cursor.blinking || cursor.row != 1 || cursor.column != cursor_index % 4))
            {
                return Ok(());
            }
            master.write_all(b"n").map_err(|error| error.to_string())?;
            stage += 1;
            Ok(())
        },
    );
    assert_eq!(
        stage,
        cursor_visible.len(),
        "display transitions were not observed"
    );
}

#[test]
fn c_display_technology_suppresses_only_initial_mda_cursor_and_preserves_native_frames() {
    let fixture = Fixture::build("session_fixture", true, false);
    check_display_consumer(&fixture, &fixture.binary, "c-display-technology");
}

#[cfg(feature = "test-fixtures")]
#[test]
fn rust_display_technology_suppresses_only_initial_mda_cursor_and_preserves_native_frames() {
    let directory = tempfile::Builder::new()
        .prefix("tigt-display-")
        .tempdir()
        .unwrap();
    let fixture = Fixture {
        directory: Some(directory),
        binary: PathBuf::from(env!("CARGO_BIN_EXE_tigt-snapshot-fixture")),
    };
    check_display_consumer(&fixture, &fixture.binary, "rust-display-technology");
}

#[test]
fn mda_first_output_releases_cursor_even_when_that_frame_is_never_rendered() {
    let fixture = Fixture::build("renderer_fixture", true, true);
    let mut acknowledged = false;
    fixture.capture_observing(
        "unsampled-output",
        &["--unsampled-output"],
        |bytes, master| {
            if acknowledged {
                return Ok(());
            }
            let terminal = Terminal::replay(bytes);
            if !terminal.errors.is_empty() {
                return Err(format!("unsampled output: {:?}", terminal.errors));
            }
            let (cells, source) = terminal.cells();
            let cursor = terminal.cursor();
            if source == "active_alternate"
                && cells.iter().all(|cell| cell.character == ' ')
                && cursor.visible
                && !cursor.blinking
                && cursor.row == 0
                && cursor.column == 3
            {
                master
                    .write_all(b"n\n")
                    .map_err(|error| error.to_string())?;
                acknowledged = true;
            }
            Ok(())
        },
    );
    assert!(
        acknowledged,
        "the unrendered first-output frame did not release the cursor"
    );
}

#[test]
fn public_incremental_parser_preserves_events_without_reserving_controls() {
    let fixture = Fixture::build("input_fixture", false, false);
    let output = Command::new(&fixture.binary).output().unwrap();
    assert!(
        output.status.success(),
        "parser fixture: {}",
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn register_adapter_presents_text_and_cga_graphics_through_public_c_api() {
    let fixture = Fixture::build("video_fixture", true, false);
    let green = [0, 215, 0, 255].repeat(320 * 200);
    let red = rendered_rgb(4, 4, 0);
    let mut stripes = Vec::with_capacity(640 * 200 * 4);
    for _ in 0..200 {
        for x in 0..640 {
            stripes.extend_from_slice(if x % 8 == 0 { &red } else { &[0; 3] });
            stripes.push(255);
        }
    }
    let mut stage = 0;
    fixture.capture_observing("register-video", &[], |bytes, master| {
        if stage == 3 {
            return Ok(());
        }
        let terminal = Terminal::replay(bytes);
        if !terminal.errors.is_empty() {
            return Err(format!(
                "register video stage {stage}: {:?}",
                terminal.errors
            ));
        }
        let matches = if stage == 0 {
            let (cells, source) = terminal.cells();
            source == "active_alternate"
                && cells[0].character == 'H'
                && cells[1].character == 'i'
                && cells[COLS].character == 'R'
                && cells[0].style.colors() == ([255, 255, 95], [0, 0, 215])
                && !terminal.cursor().visible
        } else {
            let mut dimensions = options();
            if stage == 2 {
                dimensions.width = 640;
            }
            let expected = if stage == 1 { &green } else { &stripes };
            tigt_gfxreader::reconstruct(bytes, &dimensions).is_ok_and(|analysis| {
                analysis.report.success && analysis.rgba.as_slice() == expected.as_slice()
            })
        };
        if matches {
            master.write_all(b"n").map_err(|error| error.to_string())?;
            stage += 1;
        }
        Ok(())
    });
    assert_eq!(
        stage, 3,
        "text and both CGA graphics modes must reach the terminal"
    );
}

#[test]
fn mouse_decoder_preserves_fragmentation_order_and_real_leave_events() {
    let fixture = Fixture::build("mouse_fixture", false, false);
    let output = Command::new(&fixture.binary).output().unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn live_mouse_maps_completed_frames_and_restores_shared_terminal_modes() {
    let fixture = Fixture::build("mouse_session_fixture", true, false);
    // Distinct contracts: probe fallback, pixel precision, unknown metrics,
    // raster/cell conversion, press-only terminals, and libcaca's full viewport.
    let mut cases = vec![
        ("cell-fallback", 1, 1, false, true, false),
        ("pixel-combined", 1, 1, true, true, true),
        ("unknown-metrics", 1, 3, true, false, false),
        ("sixel-pixels", 2, 1, true, true, false),
        ("iterm2-cells", 4, 2, false, true, false),
        ("x10-clicks", 1, 4, false, false, false),
    ];
    if cfg!(feature = "libcaca") {
        cases.push(("ascii-pixels", 3, 1, true, true, false));
    }
    for (name, graphics, mode, pixels, metrics, combined) in cases {
        let progress = fixture.path(&format!("{name}.progress"));
        let (mut answered, mut sent) = (0, 0);
        let capture = fixture.capture_observing(
            name,
            &[
                &graphics.to_string(),
                &mode.to_string(),
                progress.to_str().unwrap(),
                if combined { "combined" } else { "mouse-only" },
            ],
            |bytes, master| {
                let queries = bytes
                    .windows(5)
                    .filter(|window| *window == b"\x1b[16t")
                    .count();
                while answered < queries {
                    if metrics {
                        master
                            .write_all(b"\x1b[6;16;8t")
                            .map_err(|e| e.to_string())?;
                    }
                    if mode == 1 {
                        master
                            .write_all(if pixels {
                                b"\x1b[?1016;2$y"
                            } else {
                                b"\x1b[?1016;0$y"
                            })
                            .map_err(|e| e.to_string())?;
                    }
                    answered += 1;
                }
                if let Ok(state) = fs::read_to_string(&progress) {
                    let values: Vec<u32> = state
                        .split_whitespace()
                        .filter_map(|s| s.parse().ok())
                        .collect();
                    if values.len() == 2 && values[0] == sent {
                        assert_eq!(
                            values[1],
                            if mode == 4 {
                                4
                            } else if pixels {
                                3
                            } else {
                                2
                            }
                        );
                        // The shared reader must discard this key in mouse-only sessions.
                        master.write_all(b"k").map_err(|e| e.to_string())?;
                        if mode == 4 {
                            master
                                .write_all(b"\x1b[M\x20\x22\x22\x1b[M\x61\x22\x22")
                                .map_err(|e| e.to_string())?;
                        } else {
                            let (x, y) = if pixels { (9, 25) } else { (2, 2) };
                            let mut packet =
                                format!("\x1b[<35;{x};{y}M\x1b[<0;{x};{y}M\x1b[<0;{x};{y}m");
                            packet.push_str(if pixels {
                                "\x1b[<35;1277;1065M\x1b[<35;1289;1081M\x1b[<291;-1;-1M"
                            } else {
                                "\x1b[<35;160;67M\x1b[<35;161;68M"
                            });
                            packet.push_str(&format!("\x1b[<65;{x};{y}M"));
                            master
                                .write_all(packet.as_bytes())
                                .map_err(|e| e.to_string())?;
                        }
                        sent += 1;
                    }
                }
                Ok(())
            },
        );
        assert_eq!(sent, 2, "{name}: both presentations must be exercised");
        let output = String::from_utf8_lossy(&capture);
        let records: Vec<Vec<f64>> = output
            .lines()
            .filter_map(|line| {
                line.strip_prefix("@mouse ").map(|line| {
                    line.split_whitespace()
                        .map(|v| v.parse().unwrap())
                        .collect()
                })
            })
            .collect();
        for stage in 0..2 {
            let events: Vec<_> = records
                .iter()
                .filter(|e| e[0] == f64::from(stage))
                .collect();
            assert_eq!(
                events.len(),
                if mode == 4 {
                    6
                } else if pixels {
                    10
                } else {
                    9
                },
                "{name}"
            );
            for (index, event) in events.iter().enumerate() {
                let kind = event[2] as u32;
                let flags = event[3] as u32;
                if kind == 4 {
                    assert!(pixels);
                    assert_eq!(flags & 7, 0, "leave coordinates must be invalid");
                    continue;
                }
                assert_ne!(flags & 1, 0);
                if (1..=3).contains(&kind) {
                    assert!(index > 0);
                    let previous = events[index - 1];
                    assert_eq!(
                        previous[2], 0.0,
                        "motion must immediately precede transition"
                    );
                    assert_eq!(&previous[6..10], &event[6..10]);
                }
                if pixels && !metrics {
                    assert_eq!(
                        flags & 6,
                        0,
                        "unknown pixel/cell geometry must not be guessed"
                    );
                    continue;
                }
                assert_ne!(flags & 2, 0, "{name}: completed frame coordinates missing");
                let (mut x, mut y) = (event[6] + 0.5, event[7] + 0.5);
                if pixels && (stage == 1 || graphics == 1 || graphics == 3) {
                    x /= 8.0;
                    y /= 16.0;
                } else if !pixels && stage == 0 && matches!(graphics, 2 | 4) {
                    x *= 8.0;
                    y *= 16.0;
                }
                if stage == 0 {
                    match graphics {
                        1 => {
                            x *= 2.0;
                            y *= 3.0;
                        }
                        2 | 4 => {
                            x *= 320.0 / 640.0;
                            y *= 200.0 / 480.0;
                        }
                        3 => {
                            y *= 200.0 / 67.0;
                        } // capped by the renderer's 21440-cell budget
                        _ => unreachable!(),
                    }
                }
                assert!(
                    (event[8] - x).abs() < 1e-9,
                    "{name}: {event:?}, expected x={x}"
                );
                assert!(
                    (event[9] - y).abs() < 1e-9,
                    "{name}: {event:?}, expected y={y}"
                );
                let (width, height) = if stage == 0 {
                    (320.0, 200.0)
                } else {
                    (40.0, 25.0)
                };
                assert_eq!(
                    flags & 4 != 0,
                    x < width && y < height,
                    "{name}: clipped hit test"
                );
                if mode == 4 && kind == 2 {
                    assert_ne!(flags & 8, 0, "X10 release must be marked synthetic");
                }
            }
            assert_eq!(
                events.last().unwrap()[11],
                1.0,
                "wheel direction must survive"
            );
        }
        assert_eq!(
            capture.windows(4).any(|w| w == b"\x1b[>u"),
            combined,
            "mouse-only sessions must not enable keyboard reporting"
        );
        // Model a caller's inherited tracking/encoding modes, including the
        // modes TIGT temporarily disables to select an unambiguous encoding.
        let tracked = [9, 1000, 1002, 1003, 1005, 1006, 1015, 1016];
        let mut active = BTreeSet::from([1000, 1005]);
        let mut saved = active.clone();
        for part in output.split("\x1b[?").skip(1) {
            let end = part
                .find(|c: char| !c.is_ascii_digit() && c != ';')
                .unwrap_or(part.len());
            let params = part[..end].split(';').filter_map(|s| s.parse::<u32>().ok());
            for parameter in params.filter(|p| tracked.contains(p)) {
                match part.as_bytes().get(end) {
                    Some(b'h') => {
                        active.insert(parameter);
                    }
                    Some(b'l') => {
                        active.remove(&parameter);
                    }
                    Some(b's') => {
                        if active.contains(&parameter) {
                            saved.insert(parameter);
                        } else {
                            saved.remove(&parameter);
                        }
                    }
                    Some(b'r') => {
                        if saved.contains(&parameter) {
                            active.insert(parameter);
                        } else {
                            active.remove(&parameter);
                        }
                    }
                    _ => {}
                }
            }
        }
        assert_eq!(
            active,
            BTreeSet::from([1000, 1005]),
            "{name}: inherited mouse modes lost"
        );
    }
}
