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

The keyboard example demonstrates conversion to PC scan codes. The snapshot example demonstrates instrumentation; see its printed process information and [the instrumentation guide](instrumentation.md).

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

## Verify a development checkout

```sh
cargo test --all-features
cargo test --no-default-features
cargo fmt --all --check
```

`--all-features` includes the development-only `test-fixtures` consumer used for C/Rust parity. The comprehensive tests compile C fixtures, run real PTYs, replay rendered output with tigt-gfxreader, decode PNGs and compare native C/Rust snapshot outputs. The development fixture binary is not built by ordinary library consumers.
