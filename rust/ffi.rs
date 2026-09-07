// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

use std::ffi::{c_char, c_int, c_void};

#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct InputKey {
    pub kind: u32,
    pub value: u32,
    pub character: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct InputEvent {
    pub key: InputKey,
    pub modifiers: u8,
    pub kind: u8,
}

pub(crate) type InputCallback = unsafe extern "C" fn(*const InputEvent, *mut c_void);

#[repr(C)]
pub(crate) struct Config {
    pub abi_version: u32,
    pub on_input: Option<InputCallback>,
    pub user: *mut c_void,
}

unsafe extern "C" {
    pub(crate) fn tigt_init(config: *const Config) -> c_int;
    pub(crate) fn tigt_resume() -> c_int;
    pub(crate) fn tigt_suspend();
    pub(crate) fn tigt_shutdown();
    pub(crate) fn tigt_present_bitmap(
        pixels: *const u32,
        width: u16,
        height: u16,
        stride: u16,
        pixel_width: u8,
    ) -> c_int;
    pub(crate) fn tigt_present_text(
        cells: *const crate::TextCell,
        columns: u16,
        rows: u16,
        stride: u16,
    ) -> c_int;
    pub(crate) fn tigt_cp437_codepoint(character: u8) -> u32;
    pub(crate) fn tigt_set_display_technology(technology: u32) -> c_int;
    pub(crate) fn tigt_set_overscan(overscan: *const crate::Overscan) -> c_int;
    pub(crate) fn tigt_get_overscan(overscan: *mut crate::Overscan) -> c_int;
    pub(crate) fn tigt_snapshot_write_fd(fd: c_int, format: u32) -> c_int;
    pub(crate) fn tigt_snapshot_configure(
        signal_number: c_int,
        format: u32,
        path: *const c_char,
    ) -> c_int;
    pub(crate) fn tigt_snapshot_set_font(font: *const u8, height: u16) -> c_int;
    pub(crate) fn tigt_snapshot_status(sequence: *mut u64, result: *mut c_int) -> c_int;
    pub(crate) fn tigt_input_create(
        callback: Option<InputCallback>,
        user: *mut c_void,
    ) -> *mut c_void;
    pub(crate) fn tigt_input_feed(input: *mut c_void, bytes: *const u8, length: usize);
    pub(crate) fn tigt_input_flush(input: *mut c_void);
    pub(crate) fn tigt_input_destroy(input: *mut c_void);
}
