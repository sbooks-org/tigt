// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

use std::{env, path::PathBuf};

fn main() {
    let target_os = env::var("CARGO_CFG_TARGET_OS").expect("Cargo must set the target OS");
    assert!(
        matches!(target_os.as_str(), "macos" | "linux"),
        "tigt currently supports macOS and Linux"
    );

    let curses = ["ncursesw", "ncurses"].into_iter().find_map(|name| {
        pkg_config::Config::new()
            .cargo_metadata(false)
            .probe(name)
            .ok()
            .map(|library| (name, library))
    });
    let png = pkg_config::Config::new()
        .cargo_metadata(false)
        .probe("libpng")
        .expect("tigt snapshots require the libpng development package and pkg-config metadata");
    let mut build = cc::Build::new();
    build
        .files([
            "src/tigt.c",
            "src/input.c",
            "src/snapshot.c",
            "src/video.c",
            "src/presenter.c",
        ])
        .include("include")
        .std("c11")
        .define("_XOPEN_SOURCE", "700")
        .define("_DEFAULT_SOURCE", None)
        .flag_if_supported("-pthread");
    for path in &png.include_paths {
        build.include(path);
    }
    for (name, value) in &png.defines {
        build.define(name, value.as_deref());
    }
    if let Some((_, library)) = &curses {
        for path in &library.include_paths {
            build.include(path);
        }
        for (name, value) in &library.defines {
            // tigt selects XSI/POSIX.1-2008 above; ncurses may advertise an older level.
            if name != "_XOPEN_SOURCE" {
                build.define(name, value.as_deref());
            }
        }
    }
    build.compile("tigt");

    // Emit native dependencies after the static tigt archive for Unix linker ordering.
    pkg_config::Config::new()
        .probe("libpng")
        .expect("the selected libpng pkg-config package disappeared");
    if let Some((name, _)) = curses {
        pkg_config::Config::new()
            .probe(name)
            .expect("the selected curses pkg-config package disappeared");
    } else {
        // macOS's SDK bundles wide-character support in ncurses. Linux's
        // wide-character development library uses the ncursesw name.
        let library = if target_os == "macos" {
            "ncurses"
        } else {
            "ncursesw"
        };
        println!("cargo:rustc-link-lib={library}");
    }
    println!("cargo:rustc-link-lib=pthread");
    let include = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap()).join("include");
    println!("cargo:include={}", include.display());
    for path in [
        "src/tigt.c",
        "src/input.c",
        "src/snapshot.c",
        "src/snapshot.h",
        "src/video.c",
        "src/presenter.c",
        "include/tigt_presenter.h",
        "src/palette.h",
        "include/tigt.h",
        "include/tigt_video.h",
        "build.rs",
    ] {
        println!("cargo:rerun-if-changed={path}");
    }
}
