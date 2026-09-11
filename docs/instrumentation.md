# Instrumentation and snapshots

There are two complementary observation paths:

1. **Native snapshot:** tigt copies the most recently submitted frame and writes its original pixels or resolved text. This is the producer-side reference, independent of terminal colour approximation.
2. **Terminal observation:** tigt-gfxreader captures/replays actual terminal output and reconstructs what the terminal consumer sees. Use this to test the renderer rather than merely verifying submitted data.

Comparing both finds rendering, palette, stride, clipping and encoding errors. Both can supply images and recognized text/attribute metadata to a computer vision model; neither project requires a vision service or sends captures over the network.

## Opt-in signal capture

No screenshot signal is installed by default. Configure SIGUSR1 or SIGUSR2 explicitly through the API, or set these before initialization:

| Environment variable | Meaning |
|---|---|
| `TIGT_SNAPSHOT_PATH` | Output filename or an existing named FIFO. Enables instrumentation. |
| `TIGT_SNAPSHOT_FORMAT` | `png`, `utf8`, `ascii`, `cp437`, `ansi`, `cells`, or `attributes`. |
| `TIGT_SNAPSHOT_SIGNAL` | `USR1` or `USR2`; defaults to `USR1`. |

Set the format explicitly so the output type is unambiguous. For a bitmap-rendering application:

```sh
TIGT_SNAPSHOT_PATH=/tmp/frame.png \
TIGT_SNAPSHOT_FORMAT=png \
TIGT_SNAPSHOT_SIGNAL=USR1 \
./build/tigt-demo
```

From a second terminal, signal the **actual renderer process PID**, not a shell, Cargo wrapper or every process with a matching name:

```sh
kill -USR1 PID
```

Use `ps -p PID -o pid=,command=` to verify the target. The signal handler only records a request. Encoding and filesystem operations happen outside signal context. Standard signals may coalesce; repeated requests are not a frame queue.

A configured application signal handler is not silently replaced. Disable capture with `tigt_snapshot_configure(0,0,NULL)` or `session.configure_snapshot(None)`. Shutdown restores the prior signal disposition. Configuration survives suspend/resume; no terminal frame is available for capture while the session is inactive.

### Explicit C configuration

```c
int rc = tigt_snapshot_configure(SIGUSR1, TIGT_SNAPSHOT_ATTRIBUTES,
                                 "/tmp/tigt-frame.json");
```

Include `<signal.h>` and check `rc`. The path is copied. Monitor completion with:

```c
uint64_t sequence;
int result;
int rc = tigt_snapshot_status(&sequence, &result);
```

A changed sequence identifies a completed asynchronous request; inspect both the accessor result and the operation result. A missing frame, unavailable FIFO reader, blocked output or I/O failure is not a successful screenshot.

### Explicit Rust configuration

```rust
use std::path::Path;
use tigt::{SnapshotConfig, SnapshotFormat, SnapshotSignal};

session.configure_snapshot(Some(SnapshotConfig {
    signal: SnapshotSignal::Usr1,
    format: SnapshotFormat::Attributes,
    path: Path::new("/tmp/tigt-frame.json"),
}))?;
```

`session.snapshot_status()` returns a sequence and a `Result` for the completed operation. The examples provide complete runnable sessions:

```sh
./build/tigt-snapshot /tmp/tigt-frame.json
cargo run --example snapshot -- /tmp/tigt-frame.json
```

## Files and pipes

For signal-triggered output, a normal pathname writes a file; an existing FIFO writes to its reader. Keep output separate from curses stdout. Signal delivery to a FIFO is bounded and nonblocking: no reader or a slow/full pipe is reported as an error, not an indefinite shutdown hang.

Prepare a one-capture reader before requesting PNG output:

```sh
mkfifo /tmp/tigt-frame.fifo
cat /tmp/tigt-frame.fifo > /tmp/frame.png
```

Run the reader in a separate terminal. Configure the application's snapshot path to `/tmp/tigt-frame.fifo`, then send its configured signal. One writer open/close supplies one snapshot; the reader sees EOF afterwards. For repeated captures, reopen the reader for each request. Use separate output streams or an external framing protocol if combining several snapshots; concatenated arbitrary binary formats are not implicitly framed.

A synchronous snapshot can also target any suitable open file descriptor:

```c
int rc = tigt_snapshot_write_fd(output_fd, TIGT_SNAPSHOT_PNG);
```

```rust
use std::os::fd::AsFd;
session.snapshot_write_fd(file.as_fd(), SnapshotFormat::Png)?;
```

The descriptor is borrowed, not closed by tigt. The synchronous caller owns descriptor lifetime and flow control; do not call it from a signal handler. Check errors and discard incomplete output. Merely observing that a file exists is not proof that a PNG is complete.

Serialize writes and descriptor configuration, including through duplicates of the same open file, until the synchronous call returns. On macOS, avoiding process-wide `SIGPIPE` delivery requires temporarily setting the open file's no-SIGPIPE flag; tigt saves and restores that flag. It never installs or changes the application's `SIGPIPE` disposition. On other supported POSIX systems, suppression is confined to the writing thread.

## Output formats

| Format | Contents |
|---|---|
| `png` | Logical source RGB bitmap, or text rasterized with a caller-supplied font. |
| `utf8` | Plain Unicode text; preferred plain-text format. |
| `ascii` | Plain 7-bit ASCII; unsupported characters become `?`. |
| `cp437` | CP437 display-glyph bytes; unsupported Unicode becomes `?`. |
| `ansi` | UTF-8 with RGB/underline SGR attributes, resets and newline-delimited rows. No cursor addressing. |
| `cells` | Raw row-major CP437 glyph byte followed by a projected attribute byte for every cell. |
| `attributes` | Versioned JSON retaining dimensions, resolved codepoints, RGB colours, flags and overscan metadata. |

Plain-text formats trim trailing spaces and end rows with LF. ANSI output also trims trailing blanks; therefore it is convenient for display but not a lossless serialization of coloured blank cells. Pair plain text with the attributes JSON when exact attributes matter. The cell-pair dump is untrimmed: its length is exactly `columns × rows × 2` bytes. Treat pairs as bytes; when read as little-endian 16-bit words, the glyph is the low byte and the projected attribute is the high byte.

The legacy attribute projection chooses nearest IBM16 foreground/background colours and packs `foreground | background << 4`. It cannot recover original hardware blink bits, original palette indexes after redefinition, or arbitrary RGB colours. Underline and visible-cursor flags remain available in JSON. CP437 graphical controls are display glyph bytes, not instructions to print directly to a host terminal. Use the Unicode or ANSI output for display.

Bitmap frames do not magically become text inside tigt. Their PNG and metadata exports work directly; text-only formats report an unsupported-argument error. Recognize bitmap text with tigt-gfxreader and an appropriate font.

### Text PNG font

For deterministic text PNGs, supply a 256-glyph font: width8 pixels, height1..32, one byte per glyph scanline, MSB left, glyph-major CP437 order. C copies `256 × height` bytes:

```c
int rc = tigt_snapshot_set_font(font_bytes, glyph_height);
```

Rust checks the slice length with `session.set_snapshot_font(&font_bytes,height)`. Clear it with `tigt_snapshot_set_font(NULL,0)` / `session.clear_snapshot_font()`.

A text PNG without a configured font, or containing a Unicode character outside the available CP437 mapping, reports an argument error rather than drawing a fabricated glyph. The plain Unicode and lossless metadata exports do not need a font. Use a legally available font corresponding to the system being tested; neither project bundles IBM ROMs.

## Analysis and vision handoff

Install the independent tool from its private repository:

```sh
cargo install --git ssh://git@github.com/sbooks-org/tigt-gfxreader.git --locked
tigt-gfxreader --help
```

Its [usage guide](https://github.com/sbooks-org/tigt-gfxreader) documents direct PNG analysis, terminal transcript/stdin reconstruction, and bounded PTY child capture. Supply the actual screen font for recognition. Keep the PNG, decoded text and JSON report together when handing a result to a vision model. Include ambiguity/unknown-glyph information rather than presenting nearest matches as established text.

Native snapshots and terminal reconstructions intentionally need not have identical RGB. Blocks/ASCII can use terminal-default foreground/background only with at most three distinct source bitmap colours, all black, white, or neutral grey with equal channels in 129..254; classification includes every source pixel before sampling. Sixel uses explicit source RGB (integer-percentage channel precision, at most 256 colours), never theme substitutions. Native dumps preserve exact submitted/resolved RGB and are independent of the selected graphics mode. Compare against the documented terminal palette policy, not an assumption of lossless display transport.

Display technology can also intentionally change cursor presentation. With `TIGT_DISPLAY_MDA` / `DisplayTechnology::Mda`, initial cursor wandering is hidden in terminal output until the first visibly nonblank text submission. Equal foreground/background RAM fills and un-underlined blanks do not release suppression; an underlined blank with distinct colours does. A cursor flag alone is not first output. Every accepted text submission is observed, including frames too short-lived for the renderer to sample. Later clears and suspend/resume preserve the released latch; only a real technology change or a new session resets it.

Native PNG, ANSI and attributes snapshots keep the original cursor flags and attributes even while the terminal cursor is suppressed. Use attributes captures for source cursor coordinates and terminal replay for what the user actually saw; do not interpret that initial difference as source corruption. Repeating the same technology hint before every frame is safe and does not reset the latch.

## Test strategy

```sh
cargo test --all-features
cargo test --no-default-features
```

The `--all-features` build requires the libcaca development package (`libcaca-dev` on Debian/Ubuntu, `libcaca` on Homebrew); default builds do not depend on it.

The comprehensive suite runs real C and Rust consumers in PTYs and compares their native dumps, decoded PNG pixels and structured attributes. Tests also exercise actual terminal replay, signal configuration/restoration, file/FIFO behavior, copied/padded frames, mode transitions, and error paths. The analyzer has an independent unit/CLI suite, so neither project relies on a circular runtime dependency.

Use explicit child PIDs and finite timeouts in CI. Do not use blanket process-name signals. Recognized text and attribute reports are evidence to evaluate alongside the image, not a replacement for checking parser errors or child exit status.

## Local Docker CI

With the two repositories checked out alongside one another, run from tigt:

```sh
docker build -t tigt-ci ci
docker run --rm \
  -v "$PWD:/source/tigt:ro" \
  -v "$(dirname "$PWD")/tigt-gfxreader:/source/tigt-gfxreader:ro" \
  tigt-ci
```

The Linux container copies sources into its own writable workspace, builds and tests both projects, compares C/Rust snapshots, and checks an external installed CMake consumer. It mounts no SSH agent or token. Cargo's pinned analyzer revision is fetched from the local analyzer Git repository; make sure that checkout contains the pinned commit. Crates.io and the public keyboard dependency still require network access.

Host source trees are read-only and host build products are not reused. Container files disappear on `--rm`. The manually triggered GitHub workflow runs this same image; hosted runs additionally require an `ANALYZER_READ_TOKEN` secret with contents-read access to the private analyzer repository. Hosted CI is optional, not a prerequisite for running this local suite.
