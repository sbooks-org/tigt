// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

use clap::{Parser, Subcommand};
use std::fs::{self, File};
use std::io::BufWriter;
use std::path::{Path, PathBuf};
use tigt_gfxreader::{analyze, Options, Region};

#[derive(Parser)]
#[command(
    version,
    about = "Replay terminal sextant graphics and verify 8x8 glyphs against a ROM font"
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Reconstruct graphics from a raw PTY transcript (320-column, 80-row terminal).
    Analyze {
        #[arg(long)]
        capture: PathBuf,
        #[arg(long)]
        rom: PathBuf,
        /// Byte offset to the ROM's 8-byte-per-glyph, MSB-left font; decimal or 0xHEX.
        #[arg(long, value_parser = parse_integer)]
        font_offset: usize,
        #[arg(long, default_value_t = 128)]
        glyph_count: usize,
        #[arg(long, default_value_t = 320)]
        width: usize,
        #[arg(long, default_value_t = 200)]
        height: usize,
        /// Append .png, .json and .txt to this path. PNG is cropped to --region.
        #[arg(long)]
        output: PathBuf,
        /// Guest pixel coordinates x,y,w,h, all multiples of 8. Default: entire screen.
        #[arg(long)]
        region: Option<Region>,
        /// Require printable ASCII substring in a glyph row, considering all ROM alternatives.
        #[arg(long)]
        expect: Option<String>,
    },
}

fn parse_integer(value: &str) -> Result<usize, String> {
    let result = if let Some(hex) = value
        .strip_prefix("0x")
        .or_else(|| value.strip_prefix("0X"))
    {
        usize::from_str_radix(hex, 16)
    } else {
        value.parse()
    };
    result.map_err(|_| format!("invalid nonnegative integer {value:?} (use decimal or 0xHEX)"))
}

fn output_path(prefix: &Path, extension: &str) -> PathBuf {
    let mut name = prefix.as_os_str().to_os_string();
    name.push(extension);
    PathBuf::from(name)
}

fn run(cli: Cli) -> Result<bool, Box<dyn std::error::Error>> {
    let Command::Analyze {
        capture,
        rom,
        font_offset,
        glyph_count,
        width,
        height,
        output,
        region,
        expect,
    } = cli.command;
    let capture_bytes = fs::read(&capture)
        .map_err(|error| format!("reading capture {}: {error}", capture.display()))?;
    let rom_bytes =
        fs::read(&rom).map_err(|error| format!("reading ROM {}: {error}", rom.display()))?;
    let analysis = analyze(
        &capture_bytes,
        &rom_bytes,
        &Options {
            width,
            height,
            font_offset,
            glyph_count,
            region,
            expect,
        },
    )?;
    if let Some(parent) = output.parent().filter(|p| !p.as_os_str().is_empty()) {
        fs::create_dir_all(parent)?;
    }
    let png_path = output_path(&output, ".png");
    let json_path = output_path(&output, ".json");
    let text_path = output_path(&output, ".txt");
    let mut encoder = png::Encoder::new(
        BufWriter::new(File::create(&png_path)?),
        analysis.report.region.width as u32,
        analysis.report.region.height as u32,
    );
    encoder.set_color(png::ColorType::Rgba);
    encoder.set_depth(png::BitDepth::Eight);
    let mut writer = encoder.write_header()?;
    writer.write_image_data(&analysis.rgba)?;
    writer.finish()?;
    fs::write(&json_path, serde_json::to_vec_pretty(&analysis.report)?)?;
    fs::write(&text_path, &analysis.report.decoded_text)?;
    print!("{}", analysis.report.decoded_text);
    for diagnostic in analysis.report.diagnostics.iter().take(20) {
        let position = diagnostic
            .guest_position
            .map(|[x, y]| format!(" at guest ({x},{y})"))
            .unwrap_or_default();
        let terminal = diagnostic
            .terminal_position
            .map(|[x, y]| format!(" [terminal ({x},{y})]"))
            .unwrap_or_default();
        eprintln!(
            "{}{}{}: {}",
            diagnostic.kind, position, terminal, diagnostic.message
        );
    }
    if analysis.report.diagnostics.len() > 20 {
        eprintln!(
            "{} additional diagnostics in {}",
            analysis.report.diagnostics.len() - 20,
            json_path.display()
        );
    }
    if !analysis.report.incomplete_scanlines.is_empty() {
        eprintln!(
            "incomplete guest scanlines: {:?} ({} unobserved/invalid pixels)",
            analysis.report.incomplete_scanlines, analysis.report.missing_pixel_count
        );
    }
    eprintln!(
        "{}: {} glyphs; {} diagnostics; snapshot={}; outputs: {}, {}, {}",
        if analysis.report.success {
            "PASS"
        } else {
            "FAIL"
        },
        analysis.report.glyphs.len(),
        analysis.report.diagnostics.len(),
        analysis.report.terminal_snapshot,
        png_path.display(),
        json_path.display(),
        text_path.display()
    );
    Ok(analysis.report.success)
}

fn main() -> std::process::ExitCode {
    match run(Cli::parse()) {
        Ok(true) => std::process::ExitCode::SUCCESS,
        Ok(false) => std::process::ExitCode::from(1),
        Err(error) => {
            eprintln!("tigt-gfxreader: {error}");
            std::process::ExitCode::from(2)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cli_writes_bitmap_and_reports_mismatch_with_generated_rom() {
        let directory = tempfile::tempdir().unwrap();
        let capture = directory.path().join("capture.pty");
        let rom = directory.path().join("font.rom");
        let prefix = directory.path().join("result");
        // Independent blank ROM: one set sextant pixel must be rejected.
        fs::write(&rom, [0u8; 8]).unwrap();
        fs::write(&capture, "\x1b[H    \x1b[2;1H    \x1b[3;1H    ").unwrap();
        let args = [
            "tigt-gfxreader",
            "analyze",
            "--capture",
            capture.to_str().unwrap(),
            "--rom",
            rom.to_str().unwrap(),
            "--font-offset",
            "0x0",
            "--glyph-count",
            "1",
            "--region",
            "0,0,8,8",
            "--output",
            prefix.to_str().unwrap(),
        ];
        assert!(run(Cli::try_parse_from(args).unwrap()).unwrap());
        let decoder = png::Decoder::new(File::open(output_path(&prefix, ".png")).unwrap());
        let reader = decoder.read_info().unwrap();
        assert_eq!((reader.info().width, reader.info().height), (8, 8));
        let report: serde_json::Value =
            serde_json::from_slice(&fs::read(output_path(&prefix, ".json")).unwrap()).unwrap();
        assert_eq!(report["success"], true);
        assert!(output_path(&prefix, ".txt").is_file());
        fs::write(&capture, "\x1b[H\u{1fb00}   \x1b[2;1H    \x1b[3;1H    ").unwrap();
        assert!(!run(Cli::try_parse_from(args).unwrap()).unwrap());
        let report: serde_json::Value =
            serde_json::from_slice(&fs::read(output_path(&prefix, ".json")).unwrap()).unwrap();
        assert_eq!(report["glyphs"][0]["nearest"]["distance"], 1);
        assert_eq!(
            report["glyphs"][0]["nearest"]["error_pixels"],
            serde_json::json!([[0, 0]])
        );
    }
}
