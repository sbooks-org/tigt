# tigt reference

The installed `include/tigt.h` and Rust API documentation are the authoritative declarations. This guide describes the contracts and their interactions.

## Lifecycle and ownership

| C | Rust | Purpose |
|---|---|---|
| `tigt_init(&config)` | `Session::new()` / `Session::with_input(callback)` | Acquire the process-wide terminal session. |
| `tigt_suspend()` | `session.suspend()` | Restore shell terminal modes; retain the session and latest frame. |
| `tigt_resume()` | `session.resume()` | Re-enter curses and redraw. |
| `tigt_shutdown()` | `Drop` | Stop/join workers, restore terminal state, release session resources. |

`config.abi_version` must equal `TIGT_ABI_VERSION`. A NULL input callback selects output-only mode. Both standard input and standard output must be terminals even in output-only mode. Lifecycle calls are serialized on the owning thread. Frame producers must not race lifecycle operations. Do not mix raw C lifecycle calls with a live Rust `Session`.

Input callbacks run on a worker thread in a live session. They must return promptly, must not call lifecycle operations or recursively operate/destroy their decoder, and must not unwind across C. Rust contains unwinding callback panics and reports them on `input_status()` and fallible session operations; `panic=abort` still aborts.

The application owns SIGINT/SIGTSTP policy. Normal initialization does not reserve a screenshot signal. Snapshot configuration explicitly opts into SIGUSR1 or SIGUSR2. Snapshot configuration and its signal ownership survive suspension; shutdown or explicit disabling restores the prior disposition.

## Bitmap frames

`tigt_present_bitmap(pixels, width, height, stride, pixel_width)` / `Session::present_bitmap` copy their input. Width is 320 or 640; height is 200; stride is at least width; pixel_width is 1 or 2. The Rust slice must cover the last visible pixel, not padding after the final row. Pixel high bytes are ignored; RGB channels occupy bits16..23,8..15,0..7.

The renderer maps bitmap pixels into Unicode sextant cells. It uses an adaptive black/white terminal-default policy: when only normal or only intense white is present, it may use normal or bold terminal foreground respectively. Frames containing both use explicit colour distinctions. Colours are quantized to the existing 16-colour PC display palette, then approximated using terminal capabilities. Native PNG snapshots bypass that terminal approximation.

A 160×200 logical image is supplied as a horizontally duplicated 320×200 source with pixel_width2. Overscan is separate metadata, not part of the active bitmap.

## Text frames

`tigt_present_text(cells, columns, rows, stride)` / `Session::present_text` copy resolved cells. `tigt_text_cell` / `TextCell` contains:

- `codepoint`: a Unicode scalar.
- `foreground`, `background`: explicit `0x00RRGGBB`, high bytes zero.
- `flags`: `TIGT_TEXT_UNDERLINE` / `TEXT_UNDERLINE` and `TIGT_TEXT_CURSOR` / `TEXT_CURSOR`.

Columns1..320, rows1..128, at most21440 visible cells; stride is in cells and at least columns. Each codepoint must occupy exactly one column according to `wcwidth` in the active locale. Controls, wide characters and standalone combining marks are rejected. CP437 graphical control characters should be mapped through `tigt_cp437_codepoint` / `cp437_codepoint`, not submitted as control scalars.

At most one cell may have the cursor flag. It means a currently visible steady underline cursor; the producer resolves hardware blink timing. The producer also resolves text blink, palette mapping, display enable and any hardware register interpretation. tigt has no CGA/MDA VRAM decoder.

Text colours use the same 16-colour approximation, but do not reinterpret the producer's attributes as hardware mode bits. Native snapshots retain the original resolved RGB and flags.

## Display technology

`tigt_set_display_technology(technology)` / `Session::set_display_technology(DisplayTechnology)` select an optional terminal presentation policy. C accepts `TIGT_DISPLAY_GENERIC=0` or `TIGT_DISPLAY_MDA=1`; Rust exposes `DisplayTechnology::{Generic,Mda}`. This additive API does not change the ABI version or any frame/configuration layout.

Generic is the new-session default and honors the resolved cursor immediately. MDA hides only the initial terminal cursor until an accepted text submission contains visible output: distinct resolved foreground/background RGB plus a nonblank, non-whitespace glyph or an underline. Whitespace, nonbreaking spaces and the empty braille pattern are blank; an underline can make a blank visible. Cursor flags alone and equal-colour POST RAM fills do not count.

The latch observes every accepted submission, not just frames sampled by the renderer. After first output, clearing the display does not hide subsequent cursors. Bitmap submissions neither release nor reset the latch. Native snapshots always retain the exact source cursor flags, coordinates and attributes.

Set the hint before submitting that technology's frame, and serialize hints with submissions when ordering matters. An actual technology change resets the latch and redraws the retained frame even without another submission; a repeated identical hint does nothing. The hint and latch survive suspend/resume; shutdown/init reset them to Generic. Setting requires an active session (`BUSY` otherwise); unsupported C values return `ARGUMENT` without altering state.

## Overscan

`tigt_overscan` / `Overscan` has `color` and `left`, `right`, `top`, `bottom` dimensions in native backing pixels before host vertical line doubling. `tigt_set_overscan` / `Session::set_overscan` copy metadata; `tigt_get_overscan` / `Session::overscan` retrieve it.

It is not drawn. Submitting a bitmap or text frame does not reset it. Serialize metadata with frame submission when ordering matters. New sessions start with zero metadata; suspension retains it. Attributes snapshots include this metadata for instrumentation.

## Input decoding

The input decoder is independent of a curses session and of the keyboard mapper:

- C: `tigt_input_create(callback,user)`, `tigt_input_feed`, `tigt_input_flush`, `tigt_input_destroy`.
- Rust: `InputDecoder`, `feed` and `flush`.

Feed arbitrary stream fragments; UTF-8 and escape sequences can span calls. Flush resolves a pending bare Escape; the embedding application chooses when to do that. Live sessions perform their own input polling/escape timeout. No key is reserved: Ctrl+C and Ctrl+Z are semantic input events.

Events distinguish press, repeat and release; key identity includes Unicode characters, function keys, navigation, editing, lock and modifier keys. Modifier bits cover Shift, Control, Alt and Super. The decoder understands supported traditional terminal sequences and Kitty keyboard events. Legacy terminal taps cannot reconstruct physical key-release timing that the transport never reported. Invalid or unsupported input is not a promise of arbitrary terminal-protocol compatibility.

C decoder creation rejects a NULL callback. Feed/flush/destroy must be externally serialized. A decoder callback executes synchronously during explicit feed/flush; a live-session callback executes on the input worker.

## PC keyboard integration

C consumers may include `tigt_keyboard.h` alongside the separate mapper's `pc_xt_keyboard.h`. `tigt_keyboard_handle` converts one semantic event to mapper input and returns the generated scan bytes. Link the mapper separately; the core tigt library does not require it.

Rust enables `keyboard` to expose event conversions and the mapper re-export. The mapper's PC policy remains separate from terminal transport decoding. Applications deliver the resulting bytes to their emulator or other consumer.

## Snapshots

See [Instrumentation](instrumentation.md) for complete recipes and output semantics.

| C | Rust |
|---|---|
| `tigt_snapshot_write_fd(fd,format)` | `session.snapshot_write_fd(fd.as_fd(), format)` |
| `tigt_snapshot_configure(signal,format,path)` | `session.configure_snapshot(Some(config))` |
| `tigt_snapshot_configure(0,0,NULL)` | `session.configure_snapshot(None)` |
| `tigt_snapshot_set_font(bytes,height)` | `session.set_snapshot_font(bytes,height)` |
| `tigt_snapshot_set_font(NULL,0)` | `session.clear_snapshot_font()` |
| `tigt_snapshot_status(&sequence,&result)` | `session.snapshot_status()` |

Formats: PNG, UTF8, ASCII, CP437, ANSI, CELLS, ATTRIBUTES. Signals: SIGUSR1 or SIGUSR2 only, explicit opt-in. A non-default application signal disposition is not replaced. Snapshot capture takes an owned consistent frame copy, so a producer can submit a new frame while the previous snapshot is encoded.

## Errors and limits

C errors are `TIGT_ERROR_ARGUMENT`, `TIGT_ERROR_TERMINAL`, `TIGT_ERROR_BUSY` and `TIGT_ERROR_SYSTEM`; success is `TIGT_OK`. Rust exposes corresponding `Error` values and callback-panic reporting. Argument errors include unsupported dimensions/formats/flags, invalid text cells and insufficient font data. Busy includes conflicting ownership and missing/inactive frame state. Terminal errors describe unavailable terminal initialization; system errors describe OS/allocation/I/O failures.

Signal delivery is asynchronous; successful configuration does not mean a capture succeeded. Inspect snapshot completion status, and validate/decode output before declaring an instrumentation step successful. Standard signals can coalesce; this is a request for a current frame, not a lossless frame-recording protocol.

Do not expect native dumps to preserve absent historical information: text cells contain resolved Unicode/RGB, not original guest VRAM attribute bytes. The legacy byte-pair export is a documented projection, not an inverse hardware emulator.
