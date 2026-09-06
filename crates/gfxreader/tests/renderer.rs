// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

#![cfg(unix)]

use std::collections::BTreeSet;
use std::fs::{self, File};
use std::io::{Read, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::time::{Duration, Instant};
use tigt_gfxreader::terminal::{Terminal, COLS};
use tigt_gfxreader::{analyze, Options};

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
    b"\x03\x1a\x1b[99;5:1u\x1b[99;5:2u\x1b[99;5:3u\x1b[122;5:1u\x1b[122;5:2u\x1b[122;5:3uq";

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
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
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
        let child = Command::new(&self.binary)
            .args(arguments)
            .env("TERM", "xterm-256color")
            .env("LANG", locale)
            .env("LC_ALL", locale)
            .stdin(slave.try_clone().unwrap())
            .stdout(slave.try_clone().unwrap())
            .stderr(slave)
            .spawn()
            .expect("launching compiled C fixture in its own PTY");
        let mut child = ChildGuard(child);
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
    if (color == 15 && fg != 7 && bg != 7) || (color == 7 && fg != 15 && bg != 15) {
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
    let foreground = rendered_rgb(fg, fg, bg);
    let background = rendered_rgb(bg, fg, bg);
    let mut rgba = Vec::with_capacity(320 * 200 * 4);
    for y in 0..200 {
        for x in 0..320 {
            let code = (y / 8 * 40 + x / 8) % 128;
            let ink = font[FONT_OFFSET + code * 8 + y % 8] & (0x80 >> (x % 8)) != 0;
            rgba.extend_from_slice(if ink { &foreground } else { &background });
            rgba.push(255);
        }
    }
    rgba
}

fn check_frame(capture: &[u8], font: &[u8], fg: usize, bg: usize) {
    let analysis = analyze(capture, font, &options()).expect("valid font and dimensions");
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
        assert_eq!(glyph.status, "matched", "{fg}-{bg}: {glyph:?}");
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
    // The built CLI must produce the same exact report as the library. This
    // replaces comparing committed machine/ROM captures with private paths.
    let prefix = fixture.path("cli-result");
    let output = Command::new(env!("CARGO_BIN_EXE_tigt-gfxreader"))
        .args(["analyze", "--capture"])
        .arg(fixture.path("2-6.pty"))
        .arg("--rom")
        .arg(&rom)
        .args(["--font-offset", "0xfa6e", "--output"])
        .arg(&prefix)
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "CLI: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    let report: serde_json::Value =
        serde_json::from_slice(&fs::read(prefix.with_extension("json")).unwrap()).unwrap();
    let library = analyze(
        &fs::read(fixture.path("2-6.pty")).unwrap(),
        &font,
        &options(),
    )
    .unwrap();
    assert_eq!(report, serde_json::to_value(&library.report).unwrap());
    assert_eq!(
        fs::read_to_string(prefix.with_extension("txt")).unwrap(),
        library.report.decoded_text
    );
    let mut png = png::Decoder::new(File::open(prefix.with_extension("png")).unwrap())
        .read_info()
        .unwrap();
    let mut decoded = vec![0; png.output_buffer_size()];
    let frame = png.next_frame(&mut decoded).unwrap();
    assert_eq!((frame.width, frame.height), (320, 200));
    assert_eq!(&decoded[..frame.buffer_size()], library.rgba);
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
