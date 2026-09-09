# Getting started

## Prerequisites

On Debian/Ubuntu:

```sh
sudo apt-get install build-essential cmake pkg-config libncurses-dev libpng-dev
```

On macOS, install the Xcode command-line tools and Homebrew's libpng/pkg-config:

```sh
xcode-select --install
brew install cmake libpng pkg-config
```

The macOS SDK supplies curses. Linux needs wide-character curses. Use a UTF-8 locale and a terminal with Unicode sextants and 256-colour support for best output. A current stable Rust toolchain is required for Rust consumers and the complete test suite.

## Clone

```sh
git clone git@github.com:sbooks-org/tigt.git
cd tigt
```

The repository and its analyzer development dependency are private. Your GitHub SSH identity must have access to both for the complete tests. The optional keyboard mapper is a separate Git dependency.

## C

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
./build/tigt-demo
```

Install to a chosen prefix, rather than modifying the system installation:

```sh
cmake --install build --prefix "$HOME/.local"
```

An external CMake consumer uses:

```cmake
find_package(tigt CONFIG REQUIRED)
add_executable(my-terminal main.c)
target_link_libraries(my-terminal PRIVATE tigt::tigt)
```

Set `CMAKE_PREFIX_PATH` to the installation prefix when configuring that consumer. Curses, threads and libpng are propagated by the imported target. `-DTIGT_BUILD_EXAMPLES=OFF` disables example executables.

A minimal text session:

```c
#include <tigt.h>
#include <unistd.h>

int main(void)
{
    const tigt_config config = { TIGT_ABI_VERSION, NULL, NULL };
    const tigt_text_cell cells[] = {
        { 'H', 0xffffff, 0, 0 },
        { 'i', 0xffffff, 0, 0 }
    };
    int result = tigt_init(&config);
    if (result != TIGT_OK)
        return 1;
    result = tigt_present_text(cells, 2, 1, 2);
    sleep(2);
    tigt_shutdown();
    return result != TIGT_OK;
}
```

The sleep only keeps this small example visible; applications run their own event loop. Always call shutdown on normal exit. Ctrl+C/Ctrl+Z policy belongs to the application, not the decoder.

## Rust

For a sibling local consumer, use:

```toml
[dependencies]
tigt = { path = "../tigt" }
```

A session owns and restores terminal state:

```rust
use tigt::{Session, TextCell};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let session = Session::new()?;
    let cells = [
        TextCell::new('H', 0xffffff, 0),
        TextCell::new('i', 0xffffff, 0),
    ];
    session.present_text(&cells, 2, 1, 2)?;
    std::thread::sleep(std::time::Duration::from_secs(2));
    Ok(())
}
```

`Session` is neither Send nor Sync. Keep it on its owning thread. C copies submitted frames, so the input arrays need not outlive the call. `Drop` joins workers and restores the terminal. Aborting the process or forgetting the session bypasses normal restoration.

Existing runnable examples:

```sh
cargo run --example demo
cargo run --example keyboard --features keyboard
cargo run --example snapshot -- /tmp/tigt-snapshot.json
```

The keyboard example demonstrates physical PC key press/release events, decoded independently of a display session. The snapshot example demonstrates instrumentation; see its printed process information and [the instrumentation guide](instrumentation.md).

## Glass-TTY output without curses

Use the additive presenter when you want a reconstructed stdout transcript rather than a curses session:

```sh
cargo run --example presenter
cargo run --example presenter > transcript.txt
```

This example calls the real presenter with a simulated 20-column guest at 60 Hz, including unchanged vsyncs. It borrows stdout, does not read stdin or change terminal input modes, and never uses the alternate screen. No `Session` or `tigt_init` is needed.

The Rust setup is:

```rust
use std::os::fd::AsFd;
use tigt::{TextCell, presenter::{
    Config, Cursor, Encoding, Frame, Mode, Presenter, RefreshRate, Reversibility,
}};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let output = std::io::stdout();
    let mut presenter = Presenter::new(output.as_fd(), Config {
        mode: Mode::Glass,
        encoding: Encoding::Locale,
        reversibility: Reversibility::OneWay,
    })?;
    let mut cells = [TextCell::new(' ', 0xffffff, 0); 40];
    cells[0] = TextCell::new('H', 0xffffff, 0);
    cells[1] = TextCell::new('i', 0xffffff, 0);
    let status = presenter.present(Frame {
        cells: &cells,
        columns: 20,
        rows: 2,
        stride: 20,
        cursor: Cursor { column: 2, row: 0 },
        refresh_rate: RefreshRate::Hz60,
    })?;
    // In an emulator, repeat present at EVERY guest vsync, including unchanged
    // frames. Handle Pending, Fullscreen, and errors as described below.
    let _ = status;
    Ok(())
}
```

Keep the fd owner alive until the presenter is dropped. Rust enforces that borrow; drop never closes the fd. Serialize other writers to the same destination and never share it with a live curses session. Frame slices need only survive `present`, which is synchronous and does not allocate a Rust staging copy.

For responsive output to a pipe or terminal, set `O_NONBLOCK` yourself and use `present_nonblocking(frame)`. Handle `Progress::WouldBlock` by servicing host control, waiting for writability as needed, and calling `resume()` until `Progress::Complete(status)`; never resubmit the pending frame. These calls attempt at most one write each and never wait. The presenter retains one bounded transaction, including the original frame and exact unwritten bytes; further frames and notifications return `Error::Busy` until completion. Reset/drop cancel unwritten output immediately, but do not undo emitted bytes or repair a partially emitted escape sequence. Regular-file writes may still block because POSIX permits ignoring `O_NONBLOCK` on files. See [nonblocking output and cancellation](reference.md#nonblocking-output-and-cancellation) for the full lifecycle contract.

For a C consumer, include `<tigt_presenter.h>`, fill `tigt_presenter_config` with `TIGT_PRESENTER_ABI_VERSION`, the borrowed fd, mode, encoding and reversible flag, then call `tigt_presenter_create(&config, &presenter)`. Submit `tigt_presenter_frame` with the separate logical cursor, refresh cadence and `hints = 0` on every vsync; finish with `tigt_presenter_destroy(presenter)`.

`Mode::Glass` works on a TTY or redirected fd and ignores host dimensions. The separate logical cursor is required even when the guest cursor is hidden: glass output does not draw or manipulate a visible cursor. Locale mode follows `LC_ALL`, `LC_CTYPE`, then `LANG`; UTF-8 locales produce UTF-8 and other locales degrade to ASCII. Convert raw CP437 with `cp437_codepoint`, including its space mapping for guest bytes `0x00` and `0xff`.

Choose `Mode::Adaptive` only for a TTY destination. It starts in glass mode, then falls back to a clipped bottom guest-sized region on the **normal screen**, never the alternate screen. `Reversibility::OneWay` keeps that fallback; `Reversible` allows a guest clear followed by representable text to restore glass output after preparing/scrolling the region. A non-TTY adaptive destination is an error, not an automatic switch to glass.

Handle `Status::Pending` by continuing normal vsync submissions. Unrepresentable text must persist for 100 ms: 5/6/7 elapsed intervals at 50/60/70 Hz after the first bad snapshot (6/7/8 bad observations total). Recovery cancels confirmation; cursor-up alone is not failure. Confirmed failure yields `Error::Unrepresentable` in pure glass or `Status::Fullscreen` in adaptive mode. Pure-glass failure is sticky until `reset`; the consumer decides recovery and must prepare its destination before resetting. The 70 Hz option reserves future cadence only, not VGA support.

The [presenter reference](reference.md#glass-tty-and-adaptive-presentation) specifies exact BS/CR/NL/FF, overprinting, scrolling and clear rules. The frame's `hints` field stays zero. Vertical tabs, ISO-8859-1, cooked input and echo validation remain deferred.

An observer of INT 21h console output, INT 10h video writes, or equivalent activity can now submit speculative output through `Presenter::notify` / `tigt_presenter_notify`, without intercepting or changing the guest call. Supply decoded display scalars, starting guest cursor and geometry, a nonzero operation ID, and boundaries after particular scalar offsets. For example, `ABCDE` followed by `FG` in a five-column guest is text `ABCDEFG` with a soft-wrap boundary at offset 5. Once matching screen evidence confirms that boundary, glass output continues the logical line without inserting NL. An explicit newline remains a separate boundary.

The queue copies the payload and emits nothing until screen observations provide evidence. It supports partial matching and conservative resynchronization, bounded capacity, expiry, cancellation and counters. Handle `NotifyStatus::Dropped` as lost speculation, not failed output; continue normal vsync submissions. See the [queue contract](reference.md#queue-api-and-matching) for limits, ownership and mismatches. Notifications do not recover text that disappeared between snapshots.

Run `cargo run --example notifications` to exercise the real queue with a simulated 20-column guest. The observed row wrap in `Hello, wrapped glass-TTY!` does not split the stdout line; a later explicit newline still terminates it.

## RGB bitmaps

Pass pixels in `0x00RRGGBB` order. The high byte is ignored for bitmap input. Width is the backing-buffer width; stride is pixels between rows. Height is 200. `pixel_width` is 1 or 2 backing pixels per logical pixel.

Examples of logical geometry:

| Source width | pixel_width | Logical PNG width |
|---:|---:|---:|
| 320 | 2 | 160 |
| 320 | 1 | 320 |
| 640 | 2 | 320 |
| 640 | 1 | 640 |

For duplicated input, repeat each logical pixel twice horizontally; snapshots select the first sample in each group. The terminal renderer approximates colours and spatial detail, while native PNG snapshots retain source RGB samples.

## Headless emulator tests

Feed port writes and your emulator's VRAM directly to the optional video adapter. No `tigt_init`, `Session`, terminal, font ROM or locale setup is required to decode a frame.

An ordinary C consumer can test a 40-column CGA display with a nonzero start address:

```c
#include <tigt_video.h>
#include <assert.h>

int main(void)
{
    uint8_t vram[16384] = {0};
    vram[2] = 'O'; vram[3] = 0x0f;
    vram[4] = 'K'; vram[5] = 0x0f;
    tigt_video *video = tigt_video_create(TIGT_VIDEO_CGA);
    assert(video != NULL);
    tigt_video_write(video, 0x3d4, 1);
    tigt_video_write(video, 0x3d5, 40);
    tigt_video_write(video, 0x3d4, 13);
    tigt_video_write(video, 0x3d5, 1);  /* start address is in words */
    tigt_video_write(video, 0x3d8, 0x28);
    tigt_video_frame frame;
    assert(tigt_video_decode(video, vram, sizeof(vram), 1, &frame) == TIGT_OK);
    assert(frame.kind == TIGT_VIDEO_TEXT && frame.width == 40 && frame.height == 25);
    assert(frame.cells[0].codepoint == 'O' && frame.cells[1].codepoint == 'K');
    tigt_video_destroy(video);
    return 0;
}
```

The same operation from Rust:

```rust
use tigt::video::{AdapterKind, Frame, VideoAdapter};

fn main() -> Result<(), tigt::Error> {
    let mut video = VideoAdapter::new(AdapterKind::Cga)?;
    let mut vram = [0u8; 16384];
    vram[2..6].copy_from_slice(&[b'O', 0x0f, b'K', 0x0f]);
    for (port, value) in [(0x3d4, 1), (0x3d5, 40),
                         (0x3d4, 13), (0x3d5, 1), (0x3d8, 0x28)] {
        video.write(port, value);
    }
    let Frame::Text { cells, columns, rows } = video.decode(&vram, true)?
        else { panic!("expected text"); };
    assert_eq!((columns, rows), (40, 25));
    assert_eq!((cells[0].codepoint, cells[1].codepoint), ('O' as u32, 'K' as u32));
    Ok(())
}
```

The returned frame borrows adapter-owned reusable storage, not VRAM. Use `tigt_video_present` or `video.present(&session, &vram, blink_on)` when terminal output is wanted; these submit through the existing renderer and snapshots. The caller controls blink phase, making assertions deterministic. The adapter has no cursor-register support.

MDA accepts an entire 4 KiB aperture and 80×25 text. CGA accepts 16 KiB, 40/80×25 text, and standard 320×200 or 640×200 graphics. PCjr uses the same CGA-compatible ports and the caller-selected 16 KiB bank, not native PCjr extensions. See the [register/VRAM reference](reference.md#registervram-adapter) for addressing, palette and ownership details.

## Verify a development checkout

```sh
cargo test --all-features
cargo test --no-default-features
cargo fmt --all --check
```

`--all-features` includes the development-only `test-fixtures` consumer used for C/Rust parity. The comprehensive tests compile C fixtures, run real PTYs, replay rendered output with tigt-gfxreader, decode PNGs and compare native C/Rust snapshot outputs. The development fixture binary is not built by ordinary library consumers.
