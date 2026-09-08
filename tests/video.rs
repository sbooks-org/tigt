// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

use tigt::video::{AdapterKind, Frame, VideoAdapter};
use tigt::{Error, TEXT_UNDERLINE, TextCell};

fn crtc(video: &mut VideoAdapter, port: u16, index: u8, value: u8) {
    video.write(port, index);
    video.write(port + 1, value);
}

fn cell(vram: &mut [u8], offset: usize, character: u8, attribute: u8) {
    vram[offset] = character;
    vram[offset + 1] = attribute;
}

fn text(frame: Frame<'_>) -> (&[TextCell], u16, u16) {
    match frame {
        Frame::Text {
            cells,
            columns,
            rows,
        } => (cells, columns, rows),
        Frame::Bitmap { .. } => panic!("expected text"),
    }
}

fn bitmap(frame: Frame<'_>) -> (&[u32], u16, u16) {
    match frame {
        Frame::Bitmap {
            pixels,
            width,
            height,
        } => (pixels, width, height),
        Frame::Text { .. } => panic!("expected bitmap"),
    }
}

#[test]
fn mda_start_wraps_at_four_kib_and_steps_eighty_cells_per_row() {
    let mut video = VideoAdapter::new(AdapterKind::Mda).unwrap();
    let mut vram = [0; 4096];
    cell(&mut vram, 4094, b'A', 0x07);
    cell(&mut vram, 0, 0xc4, 0x07);
    cell(&mut vram, 158, b'B', 0x0f);
    crtc(&mut video, 0x3b0, 1, 80);
    // Mirrored ports, masked register index and masked start high bits.
    crtc(&mut video, 0x3b6, 0x2c, 0xff);
    crtc(&mut video, 0x3b2, 0x0d, 0xff);
    video.write(0x3b8, 0x0a); // MDA remains text even with CGA's graphics bit.
    let (cells, columns, rows) = text(video.decode(&vram, true).unwrap());
    assert_eq!((columns, rows), (80, 25));
    assert_eq!(cells[0], TextCell::new('A', 0xaaaaaa, 0));
    assert_eq!(cells[1], TextCell::new('\u{2500}', 0xaaaaaa, 0));
    assert_eq!(cells[80], TextCell::new('B', 0xffffff, 0));
}

#[test]
fn cga_text_start_wraps_at_sixteen_kib_and_crtc_controls_row_stride() {
    let mut video = VideoAdapter::new(AdapterKind::Cga).unwrap();
    let mut vram = [0; 16384];
    cell(&mut vram, 16382, b'A', 0x07);
    cell(&mut vram, 0, b'B', 0x07);
    cell(&mut vram, 78, b'C', 0x07);
    cell(&mut vram, 158, b'D', 0x07);
    crtc(&mut video, 0x3d4, 0x0c, 0x1f);
    crtc(&mut video, 0x3d4, 0x0d, 0xff);
    crtc(&mut video, 0x3d4, 1, 40);
    video.write(0x3d8, 0x09); // Dot-clock bit must not override CRTC width.
    let (cells, columns, rows) = text(video.decode(&vram, true).unwrap());
    assert_eq!((columns, rows), (40, 25));
    assert_eq!(cells[0], TextCell::new('A', 0xaaaaaa, 0));
    assert_eq!(cells[1], TextCell::new('B', 0xaaaaaa, 0));
    assert_eq!(cells[40], TextCell::new('C', 0xaaaaaa, 0));

    crtc(&mut video, 0x3d4, 1, 80);
    video.write(0x3d8, 0x08);
    let (cells, columns, rows) = text(video.decode(&vram, true).unwrap());
    assert_eq!((columns, rows), (80, 25));
    assert_eq!(cells[80], TextCell::new('D', 0xaaaaaa, 0));
}

#[test]
fn cga_blink_bit_switches_between_bright_background_and_hidden_foreground() {
    let mut video = VideoAdapter::new(AdapterKind::Cga).unwrap();
    let mut vram = [0; 16384];
    cell(&mut vram, 0, b'X', 0x9e);
    video.write(0x3d8, 0x08);
    assert_eq!(
        text(video.decode(&vram, false).unwrap()).0[0],
        TextCell::new('X', 0xffff55, 0x5555ff)
    );
    video.write(0x3d8, 0x28);
    assert_eq!(
        text(video.decode(&vram, true).unwrap()).0[0],
        TextCell::new('X', 0xffff55, 0x0000aa)
    );
    assert_eq!(
        text(video.decode(&vram, false).unwrap()).0[0],
        TextCell::new('X', 0x0000aa, 0x0000aa)
    );
    video.write(0x3d8, 0x08);
    assert_eq!(
        text(video.decode(&vram, false).unwrap()).0[0],
        TextCell::new('X', 0xffff55, 0x5555ff)
    );
}

#[test]
fn mda_resolves_monochrome_attributes_and_blink_without_cga_backgrounds() {
    let mut video = VideoAdapter::new(AdapterKind::Mda).unwrap();
    let mut vram = [0; 4096];
    let attributes = [0x07, 0x0f, 0x01, 0x09, 0x70, 0x78, 0x08, 0x80, 0x81, 0xf0];
    for (index, attribute) in attributes.into_iter().enumerate() {
        cell(&mut vram, index * 2, b'X', attribute);
    }
    video.write(0x3b8, 0x08);
    let cells = text(video.decode(&vram, false).unwrap()).0;
    let expected = [
        (0xaaaaaa, 0, 0),
        (0xffffff, 0, 0),
        (0xaaaaaa, 0, TEXT_UNDERLINE),
        (0xffffff, 0, TEXT_UNDERLINE),
        (0, 0xffffff, 0),
        (0xaaaaaa, 0xffffff, 0),
        (0, 0, 0),
        (0, 0, 0),
        (0xaaaaaa, 0, TEXT_UNDERLINE),
        (0, 0xffffff, 0),
    ];
    for (cell, (foreground, background, flags)) in cells.iter().zip(expected) {
        assert_eq!(
            *cell,
            TextCell {
                codepoint: 'X' as u32,
                foreground,
                background,
                flags,
            }
        );
    }
    video.write(0x3b8, 0x28);
    let cells = text(video.decode(&vram, false).unwrap()).0;
    assert_eq!(cells[0], TextCell::new('X', 0xaaaaaa, 0));
    assert_eq!(
        cells[8],
        TextCell {
            codepoint: 'X' as u32,
            foreground: 0,
            background: 0,
            flags: TEXT_UNDERLINE,
        }
    );
    assert_eq!(cells[9], TextCell::new('X', 0xffffff, 0xffffff));
}

#[test]
fn cga_two_bit_pixels_use_banked_scanlines_wrapped_start_and_palette_precedence() {
    let mut video = VideoAdapter::new(AdapterKind::Cga).unwrap();
    let mut vram = [0; 16384];
    vram[8190] = 0x1b; // Left to right: 0, 1, 2, 3.
    vram[8191] = 0xe4;
    vram[0] = 0x40; // Third byte wraps within the even bank.
    vram[16382] = 0x80; // First odd scanline.
    vram[78] = 0xc0; // Next even scanline advances 80 bytes.
    crtc(&mut video, 0x3d4, 1, 40);
    crtc(&mut video, 0x3d4, 0x0c, 0x0f);
    crtc(&mut video, 0x3d4, 0x0d, 0xff);
    video.write(0x3d8, 0x0a);
    video.write(0x3d9, 0x01);
    let (pixels, width, height) = bitmap(video.decode(&vram, true).unwrap());
    assert_eq!((width, height), (320, 200));
    assert_eq!(
        &pixels[..9],
        &[
            0x0000aa, 0x00aa00, 0xaa0000, 0xaa5500, 0xaa5500, 0xaa0000, 0x00aa00, 0x0000aa,
            0x00aa00
        ]
    );
    assert_eq!(pixels[320], 0xaa0000);
    assert_eq!(pixels[640], 0xaa5500);

    video.write(0x3d9, 0x31); // High-intensity palette 1, blue color zero.
    assert_eq!(
        &bitmap(video.decode(&vram, true).unwrap()).0[..4],
        &[0x0000aa, 0x55ffff, 0xff55ff, 0xffffff]
    );
    video.write(0x3d8, 0x0e); // Mode bit 2 overrides palette select, not intensity.
    assert_eq!(
        &bitmap(video.decode(&vram, true).unwrap()).0[..4],
        &[0x0000aa, 0x55ffff, 0xff5555, 0xffffff]
    );
    video.write(0x3d9, 0x01); // Neither palette select nor intensity.
    assert_eq!(
        &bitmap(video.decode(&vram, true).unwrap()).0[..4],
        &[0x0000aa, 0x00aaaa, 0xaa0000, 0xaaaaaa]
    );
}

#[test]
fn cga_one_bit_pixels_are_msb_first_and_ignore_upper_palette_bits() {
    let mut video = VideoAdapter::new(AdapterKind::Cga).unwrap();
    let mut vram = [0; 16384];
    vram[0] = 0x81;
    vram[8192] = 0x40;
    crtc(&mut video, 0x3d4, 1, 40);
    video.write(0x3d8, 0x1e);
    video.write(0x3d9, 0x36);
    let (pixels, width, height) = bitmap(video.decode(&vram, true).unwrap());
    assert_eq!((width, height), (640, 200));
    assert_eq!(&pixels[..8], &[0xaa5500, 0, 0, 0, 0, 0, 0, 0xaa5500]);
    assert_eq!(&pixels[640..648], &[0, 0xaa5500, 0, 0, 0, 0, 0, 0]);
    video.write(0x3d9, 0x06);
    assert_eq!(
        &bitmap(video.decode(&vram, true).unwrap()).0[..8],
        &[0xaa5500, 0, 0, 0, 0, 0, 0, 0xaa5500]
    );
}

#[test]
fn pcjr_uses_supplied_cga_bank_and_ignores_native_extensions() {
    let mut video = VideoAdapter::new(AdapterKind::Pcjr).unwrap();
    let mut vram = [0; 32768];
    cell(&mut vram, 0, b'A', 0x1f);
    cell(&mut vram, 16384, b'B', 0x2e);
    crtc(&mut video, 0x3d6, 1, 40);
    video.write(0x3d8, 0x08);
    video.write(0x3da, 0x00);
    video.write(0x3da, 0x1f);
    video.write(0x3df, 0x3f);
    crtc(&mut video, 0x3d0, 0x0e, 0xff); // Cursor registers are out of scope.
    let (cells, columns, rows) = text(video.decode(&vram, true).unwrap());
    assert_eq!((columns, rows), (40, 25));
    assert_eq!(cells[0], TextCell::new('A', 0xffffff, 0x0000aa));
    assert_eq!(
        text(video.decode(&vram[16384..], true).unwrap()).0[0],
        TextCell::new('B', 0xffff55, 0x00aa00)
    );

    vram[0] = 0x1b;
    vram[8192] = 0xc0;
    video.write(0x3d8, 0x0a);
    video.write(0x3d9, 0x20);
    video.write(0x3da, 0x00);
    video.write(0x3da, 0x1f);
    video.write(0x3df, 0x3f);
    let (pixels, width, height) = bitmap(video.decode(&vram, true).unwrap());
    assert_eq!((width, height), (320, 200));
    assert_eq!(&pixels[..4], &[0, 0x00aaaa, 0xaa00aa, 0xaaaaaa]);
    assert_eq!(pixels[320], 0xaaaaaa);
}

#[test]
fn disabled_video_blanks_decoded_text_and_pixels_and_can_be_reenabled() {
    let mut video = VideoAdapter::new(AdapterKind::Cga).unwrap();
    let vram = [0xff; 16384];
    video.write(0x3d8, 0x20);
    assert!(
        text(video.decode(&vram, true).unwrap())
            .0
            .iter()
            .all(|cell| *cell == TextCell::new(' ', 0, 0))
    );
    crtc(&mut video, 0x3d4, 1, 40);
    video.write(0x3d8, 0x02);
    video.write(0x3d9, 0x3f);
    assert!(
        bitmap(video.decode(&vram, true).unwrap())
            .0
            .iter()
            .all(|pixel| *pixel == 0)
    );
    video.write(0x3d8, 0x0a);
    assert!(
        bitmap(video.decode(&vram, true).unwrap())
            .0
            .iter()
            .all(|pixel| *pixel == 0xffffff)
    );
}

#[test]
fn invalid_apertures_and_widths_fail_without_poisoning_later_decodes() {
    let mut mda = VideoAdapter::new(AdapterKind::Mda).unwrap();
    let mut mono_vram = [0; 4096];
    cell(&mut mono_vram, 0, b'M', 0x07);
    mda.write(0x3b8, 0x08);
    assert!(matches!(
        mda.decode(&mono_vram[..4095], true),
        Err(Error::Argument)
    ));
    crtc(&mut mda, 0x3b4, 1, 40);
    assert!(matches!(mda.decode(&mono_vram, true), Err(Error::Argument)));
    crtc(&mut mda, 0x3b4, 1, 80);
    assert_eq!(
        text(mda.decode(&mono_vram, true).unwrap()).0[0],
        TextCell::new('M', 0xaaaaaa, 0)
    );

    let mut cga = VideoAdapter::new(AdapterKind::Cga).unwrap();
    let mut color_vram = [0; 16384];
    color_vram[0] = 0x40;
    cga.write(0x3d8, 0x08);
    crtc(&mut cga, 0x3d4, 1, 41);
    assert!(matches!(
        cga.decode(&color_vram, true),
        Err(Error::Argument)
    ));
    cga.write(0x3d9, 0);
    cga.write(0x3d8, 0x0a);
    crtc(&mut cga, 0x3d4, 1, 40);
    assert!(matches!(cga.decode(&[], true), Err(Error::Argument)));
    assert!(matches!(
        cga.decode(&color_vram[..16383], true),
        Err(Error::Argument)
    ));
    crtc(&mut cga, 0x3d4, 1, 80);
    assert!(matches!(
        cga.decode(&color_vram, true),
        Err(Error::Argument)
    ));
    crtc(&mut cga, 0x3d4, 1, 40);
    assert_eq!(
        &bitmap(cga.decode(&color_vram, true).unwrap()).0[..4],
        &[0x00aa00, 0, 0, 0]
    );
}

#[test]
fn decoded_frame_does_not_borrow_or_retain_source_vram() {
    let mut video = VideoAdapter::new(AdapterKind::Cga).unwrap();
    video.write(0x3d8, 0x08);
    let frame = {
        let mut vram = [0; 16384];
        cell(&mut vram, 0, b'X', 0x07);
        let frame = video.decode(&vram, true).unwrap();
        vram.fill(0);
        frame
    };
    assert_eq!(text(frame).0[0], TextCell::new('X', 0xaaaaaa, 0));
}

#[test]
fn explicit_text_geometry_wraps_apertures_and_uses_twenty_cell_rows() {
    for (kind, base, aperture) in [
        (AdapterKind::Mda, 0x3b0, 4096),
        (AdapterKind::Cga, 0x3d0, 16384),
        (AdapterKind::Pcjr, 0x3d0, 16384),
    ] {
        let mut video = VideoAdapter::new(kind).unwrap();
        let mut vram = vec![0; aperture + 2];
        cell(&mut vram, aperture - 2, b'A', 0x07);
        cell(&mut vram, 0, 0xc4, 0x07);
        cell(&mut vram, 38, b'B', 0x0f);
        cell(&mut vram, 116, b'Z', 0x07);
        cell(&mut vram, 158, b'C', 0x07);
        cell(&mut vram, aperture, b'!', 0x07); // Not part of the aperture.
        let start = (aperture / 2 - 1) as u16;
        crtc(&mut video, base, 0x0c, (start >> 8) as u8);
        crtc(&mut video, base, 0x0d, start as u8);
        video.write(base + 8, 0x08);

        let (cells, columns, rows) = text(video.decode_text(&vram, 20, 3, true).unwrap());
        assert_eq!((columns, rows), (20, 3));
        assert_eq!(cells.len(), 60);
        assert_eq!(cells[0], TextCell::new('A', 0xaaaaaa, 0));
        assert_eq!(cells[1], TextCell::new('\u{2500}', 0xaaaaaa, 0));
        assert_eq!(cells[20], TextCell::new('B', 0xffffff, 0));
        assert_eq!(cells[59], TextCell::new('Z', 0xaaaaaa, 0));

        // Explicit dimensions must not alter the old API's register geometry.
        let (cells, columns, rows) = text(video.decode(&vram, true).unwrap());
        assert_eq!((columns, rows), (80, 25));
        assert_eq!(cells[80], TextCell::new('C', 0xaaaaaa, 0));
        crtc(&mut video, base, 1, 20);
        assert!(matches!(video.decode(&vram, true), Err(Error::Argument)));
        assert_eq!(
            text(video.decode_text(&vram, 20, 3, true).unwrap()).0[20],
            TextCell::new('B', 0xffffff, 0)
        );
        assert!(matches!(video.decode(&vram, true), Err(Error::Argument)));
    }
}

#[test]
fn explicit_text_geometry_enforces_bounds_and_recovers_from_invalid_input() {
    let mut video = VideoAdapter::new(AdapterKind::Cga).unwrap();
    let mut vram = [0; 16384];
    cell(&mut vram, 2, b'X', 0x1f);
    crtc(&mut video, 0x3d4, 0x0d, 1);
    video.write(0x3d8, 0x08);
    // Maximum width and cell count; output wraps more than once through VRAM.
    let last_address = ((1 + 21439) * 2) & 0x3fff;
    cell(&mut vram, last_address, b'Z', 0x07);
    let (cells, columns, rows) = text(video.decode_text(&vram, 320, 67, true).unwrap());
    assert_eq!((columns, rows), (320, 67));
    assert_eq!(cells.len(), 21440);
    assert_eq!(cells[21439], TextCell::new('Z', 0xaaaaaa, 0));

    for (columns, rows) in [(0, 3), (20, 0), (321, 1), (1, 129), (168, 128)] {
        assert!(matches!(
            video.decode_text(&vram, columns, rows, true),
            Err(Error::Argument)
        ));
    }
    assert!(matches!(
        video.decode_text(&vram[..16383], 20, 3, true),
        Err(Error::Argument)
    ));
    video.write(0x3d8, 0x0a);
    assert!(matches!(
        video.decode_text(&vram, 20, 3, true),
        Err(Error::Argument)
    ));
    // The rejected text call must not switch graphics off or change CRTC width.
    crtc(&mut video, 0x3d4, 1, 40);
    vram[2] = 0x40;
    assert_eq!(
        &bitmap(video.decode(&vram, true).unwrap()).0[..4],
        &[0x00aa00, 0, 0, 0]
    );
    cell(&mut vram, 2, b'X', 0x1f);
    video.write(0x3d8, 0x08);
    let (cells, columns, rows) = text(video.decode_text(&vram, 1, 128, true).unwrap());
    assert_eq!((columns, rows), (1, 128));
    assert_eq!(cells[0], TextCell::new('X', 0xffffff, 0x0000aa));

    let mut mda = VideoAdapter::new(AdapterKind::Mda).unwrap();
    mda.write(0x3b8, 0x0a); // MDA has no graphics-mode bit.
    assert!(matches!(
        mda.decode_text(&vram[..4095], 20, 3, true),
        Err(Error::Argument)
    ));
    assert_eq!(
        text(mda.decode_text(&vram[..4096], 20, 3, true).unwrap()).0[1],
        TextCell::new('X', 0xffffff, 0)
    );
}

#[test]
fn explicit_text_frame_owns_decoded_cells_and_resolves_enable_and_blink() {
    let mut video = VideoAdapter::new(AdapterKind::Cga).unwrap();
    video.write(0x3d8, 0x28);
    let frame = {
        let mut vram = [0; 16384];
        cell(&mut vram, 0, b'X', 0x9e);
        let frame = video.decode_text(&vram, 20, 3, false).unwrap();
        vram.fill(0);
        frame
    };
    assert_eq!(text(frame).0[0], TextCell::new('X', 0x0000aa, 0x0000aa));

    let mut vram = [0; 16384];
    cell(&mut vram, 0, b'X', 0x9e);
    assert_eq!(
        text(video.decode_text(&vram, 20, 3, true).unwrap()).0[0],
        TextCell::new('X', 0xffff55, 0x0000aa)
    );
    video.write(0x3d8, 0x20);
    assert!(
        text(video.decode_text(&vram, 20, 3, true).unwrap())
            .0
            .iter()
            .all(|cell| *cell == TextCell::new(' ', 0, 0))
    );
    video.write(0x3d8, 0x08);
    assert_eq!(
        text(video.decode_text(&vram, 20, 3, false).unwrap()).0[0],
        TextCell::new('X', 0xffff55, 0x5555ff)
    );
}
