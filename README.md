# tigt — tty-in-glass-tty

A terminal inside a terminal: render RGB bitmaps or resolved Unicode text cells, decode terminal input, and capture native frame snapshots for instrumentation.

The C library is the implementation. The Rust crate wraps that same library with checked slices, owned sessions and callback panic containment. They are two consumer APIs, not independently maintained rendering engines.

- [Getting started](docs/getting-started.md): prerequisites, CMake/Cargo builds and working consumers.
- [Reference](docs/reference.md): rendering, input, lifecycle, keyboard integration and error contracts.
- [Instrumentation](docs/instrumentation.md): signals, PNG/text dumps, files/FIFOs, analysis and C/Rust parity testing.

## Build and test

Requires macOS or Linux, a C compiler, CMake, curses, libpng and pkg-config. Rust builds and tests also require a current stable Rust toolchain. Optional ASCII graphics use libcaca (`libcaca-dev` on Debian/Ubuntu or `brew install libcaca`); it is required for `--all-features` tests but not default builds.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
cargo test --all-features
```

Run `./build/tigt-demo --graphics auto` or `cargo run --example demo -- --graphics auto` in a real terminal. Both demos accept `auto|blocks|sixel|ascii|iterm2`; explicit modes never silently fall back. Enable ASCII conversion with `-DTIGT_WITH_LIBCACA=ON` in CMake or `--features libcaca` in Cargo (off by default). Both stdin and stdout must be TTYs. Instrumentation outputs can be regular files or pipes; they do not replace the terminal used for rendering.

Sixel and iTerm2 target 80 terminal columns at a 4:3 display aspect, fitting the known pixel bounds. Sixel resamples on the host; iTerm2 normalizes logical widths 160/320/640 to a 320×200 RGB PNG for terminal-side enlargement: duplicate 160-wide pixels, preserve 320-wide pixels, and average adjacent 640-wide pairs. Without usable pixel-width metrics they fall back to a 640-pixel target width. Override the target with C `tigt_set_image_layout(columns, aspect_width, aspect_height)` or Rust `session.set_image_layout(...)`; native frame snapshots are unaffected. Identical resolved bitmap submissions do not redraw. See the [bitmap reference](docs/reference.md#bitmap-frames) for geometry, input ownership, and resize behavior.

## Analyzer

The independent [tigt-gfxreader](https://github.com/sbooks-org/tigt-gfxreader) project owns screen-image recognition, terminal transcript reconstruction and capture tooling. It is a development dependency of tigt, not a runtime dependency. tigt's C fixtures and cross-language rendering/instrumentation tests remain here under `tests/`.

Both repositories are currently private. Development tests require read access to tigt-gfxreader via GitHub SSH. Local Docker CI uses both local checkouts with no GitHub credentials in the container; see [instrumentation](docs/instrumentation.md#local-docker-ci). Building the installed C library does not need the analyzer repository.

## Scope

- One active process-wide curses session; POSIX terminals.
- Bitmap source frames: 320×200 or 640×200, with horizontal pixel duplication described explicitly. Logical snapshots can be 160×200, 320×200 or 640×200.
- Selectable Auto, Unicode blocks, sixel raster, iTerm2 inline PNG, or libcaca ASCII bitmap output, with exact RGB or custom indexed palettes. Theme substitutions require at most three source colours, all black/white or neutral grey129..254; sixel and iTerm2 always emit explicit RGB.
- Text frames: resolved single-column Unicode cells and RGB colours.
- Optional register/VRAM adapter: headless MDA/CGA standard text and basic CGA graphics decoding; PCjr exposes CGA compatibility only. C `tigt_video.h` and Rust `tigt::video` need no terminal session for tests.
- Overscan metadata is retained but not drawn.
- Optional `keyboard` feature integrates the separate [terminal-to-pc-keyboard](https://github.com/sbooks-org/terminal-to-pc-keyboard) mapper.
- Snapshot instrumentation is opt-in. No dump signal is installed by default.

MIT-0. Copyright (C) 2026 Simplebooks Foundation. Copyright (C) 2026 Josh Rodd.
