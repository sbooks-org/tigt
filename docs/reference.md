# tigt reference

The installed `include/tigt.h`, `include/tigt_video.h`, `include/tigt_presenter.h` and Rust API documentation are the authoritative declarations. This guide describes the contracts and their interactions.

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

At most one cell may have the cursor flag. It means a currently visible steady underline cursor; the producer resolves hardware blink timing. Direct frame producers also resolve text blink, palette mapping, display enable and hardware register interpretation. The optional register/VRAM adapter below performs that conversion for a deliberately small MDA/CGA subset.

Text colours use the same 16-colour approximation, but do not reinterpret the producer's attributes as hardware mode bits. Native snapshots retain the original resolved RGB and flags.

## Glass-TTY and adaptive presentation

This is an additive, output-only API, independent of the curses `Session` and register/VRAM decoder. Include `tigt_presenter.h`, or use `tigt::presenter::{Presenter, Config, Frame, Cursor, RefreshRate}`. Do not initialize curses for this path or let a live curses session write to its destination.

| C | Rust | Purpose |
|---|---|---|
| `tigt_presenter_create(&config,&presenter)` | `Presenter::new(fd.as_fd(), config)` | Allocate an opaque presenter with a borrowed output fd. |
| `tigt_presenter_present(presenter,&frame)` | `presenter.present(frame)` | Synchronously submit **every guest vsync**, including unchanged snapshots. |
| `tigt_presenter_present_nonblocking(presenter,&frame)` | `presenter.present_nonblocking(frame)` | Submit a vsync using at most one nonblocking write. |
| `tigt_presenter_resume(presenter)` | `presenter.resume()` | Continue the exact pending transaction without submitting another vsync. |
| `tigt_presenter_reset(presenter)` | `presenter.reset()` | Start an empty glass baseline without emitting anything. |
| `tigt_presenter_destroy(presenter)` | `Drop` | Free state; never close the output fd. |

The presenter configuration uses `TIGT_PRESENTER_ABI_VERSION`, not the curses ABI version. Mode is `TIGT_PRESENT_GLASS` / `Mode::Glass` or `TIGT_PRESENT_ADAPTIVE` / `Mode::Adaptive`. Encoding is `TIGT_ENCODING_LOCALE`, `TIGT_ENCODING_UTF8`, or `TIGT_ENCODING_ASCII` / `Encoding::{Locale,Utf8,Ascii}`. The C `reversible` field is 0 or 1; Rust uses `Reversibility::{OneWay,Reversible}`. This option controls adaptive fallback, not pure glass mode.

C callers retain ownership of the descriptor and serialize presenter operations and writes through every alias of that destination. Rust retains a `BorrowedFd` for the presenter's lifetime and uses mutable borrows for operations; it neither duplicates the fd nor takes ownership. The presenter is neither Send nor Sync. Submission retains neither the input slice nor its descriptor; Rust passes the existing `repr(C)` `TextCell` storage directly, without a Rust staging buffer. C retains its validated candidate image until output commits. Blocking descriptor writes in the synchronous API can block the calling thread. There are no input reads, termios changes, signal handlers, or alternate-screen entry/exit.

On macOS, writes temporarily enable no-SIGPIPE on the borrowed open-file description and restore its previous setting; this is another reason to serialize descriptor aliases. Other supported POSIX platforms suppress write-generated SIGPIPE on the calling thread without installing a signal handler.

### Snapshots, cursor and confirmation

Frames use the text geometry limits above (1..320 columns, 1..128 rows, at most 21440 visible cells), not a fixed 80×25 assumption: 40- and 20-column guests are supported. Stride is in cells and at least columns; the slice must reach the last visible cell, with no required padding after the last row. The logical cursor is an explicit zero-based `(cursor_column,cursor_row)` / `Cursor { column, row }` inside the guest frame. It is independent of `TIGT_TEXT_CURSOR`, cursor visibility, and blink phase. Glass output never renders, shows, hides or simulates a terminal cursor.

A geometry change can adopt new text directly when the previously committed screen is entirely blank: the presenter uses an empty baseline at the current output line, without inventing a clear or requiring another blank frame in the new mode. Both shrinking and growing geometry are supported this way. If the old screen contains text, a nonblank geometry change remains unrepresentable; an actually observed clear is still required before discarding that layout.

Submit a coherent VRAM/register snapshot taken at vsync, not cells accumulated across different raster scanlines. For raw text apertures, `tigt_video_decode_text` accepts the current CRTC geometry without changing the legacy decoder contract. A guest may update text before updating its hardware cursor; an unchanged guest cursor is not a new leftward-movement request, and does not rewind the emitted glass cursor.

Supply `refresh_hz` 50, 60 or 70 / `RefreshRate::{Hz50,Hz60,Hz70}` on every vsync; 70 Hz reserves a future VGA text cadence, **not VGA rendering support**. The caller supplies cadence; the presenter does not schedule guest vsyncs or sleep. Cursor-up alone is not an error: confirmation starts only when an actual text update cannot be represented at the emitted glass cursor. Recovery cancels the pending failure.

Confirmation is 100 ms measured in submitted vsync intervals after first detection: five at 50 Hz, six at 60 Hz, seven at 70 Hz. Including the initial bad observation, that is six, seven or eight consecutive bad snapshots respectively. Do not submit only changed frames or retry the same snapshot in a tight loop: both alter the effective confirmation time.

| C result | Rust result | Meaning |
|---|---|---|
| `TIGT_OK` | `Ok(Status::Glass)` | Representable glass state. |
| `TIGT_PRESENTER_PENDING` | `Ok(Status::Pending)` | Awaiting confirmation; continue submitting vsyncs. |
| `TIGT_PRESENTER_FULLSCREEN` | `Ok(Status::Fullscreen)` | Adaptive fallback is active. |
| `TIGT_ERROR_UNREPRESENTABLE` | `Err(presenter::Error::Unrepresentable)` | Confirmed pure-glass ABORT; consumer chooses recovery. |

Argument, terminal, busy and system failures propagate as the corresponding `presenter::Error` variants; unknown statuses preserve their numeric value. Unrepresentability and hard I/O failures are sticky until reset; ordinary backpressure is not. Reset emits nothing: the consumer must prepare the destination before starting a new empty glass baseline. Reset is not an implicit repair of previously emitted text.

### Nonblocking output and cancellation

Set `O_NONBLOCK` on the borrowed output descriptor before calling `present_nonblocking` or `resume`; these functions check the flag and reject a blocking descriptor without accepting a frame or writing. They never change the flag, poll, sleep, or start a background thread. Each call attempts at most **one** `write` syscall. A short write, `EAGAIN`/`EWOULDBLOCK`, or `EINTR` returns `TIGT_PRESENTER_WOULD_BLOCK` / `Ok(Progress::WouldBlock)`. This is a resumable result, not a sticky I/O failure. On platforms that ignore `O_NONBLOCK` for regular files, disk-file writes can still block in the kernel; the bounded nonblocking guarantee applies to pipes, sockets, and terminals that honor the flag.

After `WouldBlock`, service host cancellation or suspension, wait for writability if appropriate, and call `resume()` without supplying another frame. C retains the exact unwritten suffix and the original candidate image, cursor, presentation-mode transition, and notification decisions in **one bounded transaction**. The caller may immediately reuse the original frame storage. Resumption does not validate a new snapshot, advance vsync time, rerun confirmation, or consume notifications again. Only complete output commits the image and notification decisions. Completion returns the ordinary C status or Rust `Ok(Progress::Complete(Status::{Glass,Pending,Fullscreen}))`; `Pending` is guest representability confirmation, not output backpressure.

Until output completes, both submission variants, `notify`, and notification `cancel` return `BUSY` / `Error::Busy` without changing the pending transaction. Statistics remain readable. Calling `resume` with no pending transaction also returns `Busy`. There is no frame queue or coalescing: consumers needing lossless per-vsync output must stop guest advancement while draining, rather than accumulate unlimited frames or silently skip them. Do not turn resume attempts into extra guest vsync submissions.

`reset` or destruction immediately discards unwritten output without issuing any writes or committing speculative notification matches. Already emitted bytes cannot be undone. To suspend and later continue against a changed terminal, prepare a fresh destination baseline and reset before submitting the current screen. In particular, cancellation can split a UTF-8 character or ANSI sequence; terminate a pending escape sequence (for example, with ANSI CAN) before emitting restoration controls. Reset clears fullscreen state; adaptive fallback is then reconsidered normally. A caller can instead retain the pending transaction across a pause if the destination remains untouched.

To avoid emitting restoration controls for untouched glass output, query `tigt_presenter_fullscreen_output_started(presenter)` / `presenter.fullscreen_output_started()` after each submission or resumption, including errors, and before reset. It returns true when fullscreen is committed or a pending fullscreen transaction has emitted at least one byte. Thus it detects a partial first fullscreen entry even before `Complete(Fullscreen)`. It remains true during pending recovery to glass, then becomes false on successful recovery or reset. This is current state, not a historical latch; callers can publish or retain it according to their emergency-restoration policy. The query performs no I/O or allocation and still requires serialized presenter access. The C query returns zero for NULL.

The existing `present` method remains synchronous on blocking descriptors. If a descriptor is nonblocking and it encounters `EAGAIN`, C returns `TIGT_PRESENTER_WOULD_BLOCK` and Rust returns nonsticky `Error::WouldBlock`; the transaction is retained and can be finished with `resume` after ensuring `O_NONBLOCK`. Hard I/O errors remain terminal until reset, including errors after a partially successful write.

### Glass byte rules

Glass presentation reconstructs a stream from successive snapshots, not a sequence of complete screen dumps. Redirected output ignores the host terminal size.

| Guest operation | Output rule |
|---|---|
| New line | NL (`0x0a`) with implied carriage return, **not CRLF**. Never depend on host automatic wrapping at guest column boundaries. |
| Upward block scroll | Detect retained rows and emit only the necessary newlines/new text, without duplicating previous output. A partially observed cursor row may finish before scrolling if its prefix before the output cursor still matches exactly; its writable suffix is emitted before advancing. This does not require a soft-wrap notification and never reconstructs text that disappeared between snapshots. |
| Same-line rewrite/count-up | Bare CR (`0x0d`); avoid premature space blanking while replacement text is arriving. |
| Whole-screen clear at cursor `(0,0)` | FF (`0x0c`, never `0xff`). If the only previous text is one row beginning at `(0,0)`, clear that row with CR and spaces instead of FF. |
| Blank tab spacing | Compress using standard eight-column TAB stops. |
| Underlined character | Underscore, BS, character. |
| Bold character | Character, BS, repeated character. Reverse video uses the same bold overprint convention. |
| Character erasure | BS, SP, BS for each erased character. |
| Character replacement | BS, new character for each replaced character. |
| Leftward cursor movement beneath live text | Bare BS as needed, even when no glyph changed. |

Use `TIGT_TEXT_UNDERLINE` / `TEXT_UNDERLINE`, and the presenter-only `TIGT_PRESENT_BOLD`, `TIGT_PRESENT_REVERSE` / `presenter::{TEXT_BOLD,TEXT_REVERSE}` flags for these overprint effects. The extra flags are **not** accepted by `Session::present_text`. Glass output ignores color and other presentation attributes; cell cursor flags are not a substitute for the logical cursor.

Locale encoding follows normal `LC_ALL`, `LC_CTYPE`, then `LANG` precedence. UTF-8 locales use UTF-8; non-UTF-8 locales degrade to printable seven-bit ASCII, replacing unsupported glyphs with `?` rather than producing an ISO-8859-1 stream. Explicit UTF-8 and ASCII selections override locale choice. The structural BS/TAB/NL/FF/CR bytes above remain controls.

Cells contain Unicode, not raw CP437 bytes. Convert guest CP437 through `tigt_cp437_codepoint` / `cp437_codepoint`: source bytes `0x00` and `0xff` become spaces, never NUL or nonbreaking space. Do not pass raw byte values as Unicode scalars; the presenter rejects Unicode control characters, including U+0000.

In UTF-8 mode, wide characters and standalone combining marks are rejected rather than corrupting single-cell cursor accounting. ASCII mode projects non-ASCII scalars to `?`; it does not implement additional character sets.

### Adaptive normal-screen region

Adaptive creation rejects a non-TTY destination with `TERMINAL`; choose pure glass for files and pipes. Adaptive starts in glass mode and switches only after confirmed unrepresentability. Full-screen fallback occupies the **bottom guest-sized region of the normal host screen**, clipping to smaller host windows. It never uses the alternate screen, and preceding output remains in scrollback.

`OneWay` retains fallback until explicit reset. `Reversible` can return only after a guest clear followed by text that meets glass requirements. The transition clears/prepares the region, scrolls upward by the guest row count (25 for a 25-row guest), and resumes glass output; a cursor move alone does not trigger reversal.

### Speculative output notifications

#### Observed soft-wrap notifications

A guest row transition does not necessarily end a logical output line. An observer of DOS INT 21h console-output calls, BIOS INT 10h video-write calls, or equivalent guest output activity can know that a text write will reach the guest's right edge and cause an automatic wrap. The observer need not intercept, replace, suppress, or modify the call. It supplies semantic context to tigt alongside the normal screen observations; tigt remains independent of DOS/BIOS instrumentation.

The intended notification means: **“Expect a guest line wrap at this output boundary; the following text continues the same logical line, rather than starting a true new line.”** It annotates the write-induced wrap, not an arbitrary cursor movement or an entire frame.

The integration contract is:

- The consumer identifies an actual console/video text write and the boundary at which it will wrap, using the relevant guest cursor, geometry, and output-call semantics. An INT 21h call whose output is redirected away from the console is not evidence of a screen wrap.
- The consumer associates the notification with the corresponding output operation and resulting vsync update, making it available before tigt presents that update. Ordering must distinguish multiple wraps or explicit newlines between two snapshots; an unqualified “this frame wrapped” flag is insufficient.
- The notification is consumed only for its matching wrap. It must not suppress a later explicit newline, apply to unrelated output, or remain armed after the predicted wrap does not occur. Nested observations of the same output, such as a DOS call reaching BIOS, must not double-count it.
- For a matched soft wrap, tigt tracks the physical guest row transition, including any resulting screen scroll, but continues the glass-TTY logical line without emitting NL solely for that transition. An explicit newline remains a newline, even when adjacent to a wrap. This does not rely on host automatic wrapping at the guest's column boundary.
- Screen snapshots remain the source of displayed text. The observer annotates boundaries; it does not supply a replacement text stream. Without a matching notification, tigt must not infer a soft wrap merely because the previous row reached the right edge.

For example, if a write produces `ABCDE` followed by `FG` across the right edge of a five-column guest, an observed soft-wrap boundary permits glass output `ABCDEFG`, not `ABCDE\nFG`. A subsequent explicit newline still emits NL. Merely seeing those two rows in a snapshot cannot establish this distinction.

#### Queue API and matching

Call `tigt_presenter_notify(presenter, &notification)` / `presenter.notify(notification)` before presenting the corresponding screen update. The notification contains a nonzero `operation_id`, guest geometry and starting cursor, a slice of decoded display scalars, and ordered boundary annotations. Each boundary occurs **after** `text_offset` scalars; its kind is `TIGT_BOUNDARY_SOFT_WRAP` / `BoundaryKind::SoftWrap` or `TIGT_BOUNDARY_NEWLINE` / `BoundaryKind::Newline`. Equal offsets permit consecutive boundaries, such as an explicit newline immediately after a wrap. Soft-wrap boundaries must fall at the guest right edge.

Payloads are copied before `notify` returns. Scalars describe guest display characters before locale-dependent output encoding; use CP437 decoding where appropriate. They are not raw DOS strings with embedded control bytes: expand tabs to cells and anchor separate writes following CR, backspace, or cursor movement. The observer supplies DOS/BIOS interpretation, not tigt.

Notifications emit nothing. Matching is exact, position-anchored and incremental: operations can span several vsyncs, and one vsync can confirm several operations. Unchanged frames are not mismatches, but unchanged cells cannot prove that identical text was rewritten. A wrap needs matching preceding text and following text or an observed cursor crossing. Confirmed wraps retain the guest-to-transcript column mapping for subsequent edits and tab spacing.

On mismatch, bounded lookahead permits resynchronization only with unambiguous matching evidence. Skipped predictions cannot confirm earlier boundaries. Uncertain observations use ordinary snapshot presentation, never predicted text. Matching and discard decisions commit with successful presentation; retries do not consume the queue twice. Output already emitted cannot be retroactively reinterpreted by a late notification.

The queue holds at most 32 operations, with at most 2048 scalars and 128 boundaries per operation. Exceeding capacity or payload limits discards outstanding predictions and the incoming operation, returning `TIGT_PRESENTER_NOTIFY_DROPPED` / `NotifyStatus::Dropped`; it does not fail presentation or alter confirmed output. Successful acceptance or deduplication returns `TIGT_OK` / `NotifyStatus::Accepted`.

Predictions expire after 2000 ms of guest vsync time, independently of the 100 ms unrepresentability interval. Reset, geometry changes, observed clears and full-screen entry invalidate outstanding speculation. The last 64 accepted operation IDs are deduplicated, including completed or cancelled operations; duplicate notifications do not refresh age. Reset clears this ID history. Choose one observation layer, or correlate IDs, to avoid counting DOS output and its nested BIOS writes twice.

`tigt_presenter_cancel(presenter, operation_id)` / `presenter.cancel(operation_id)` withdraws the remaining prediction idempotently. `tigt_presenter_get_notification_stats` / `presenter.notification_stats()` reports consumed, discarded and expired operation counts plus current queue length. Counters are cumulative across reset and count operations, not characters; resynchronization past an unseen operation prefix counts that operation as discarded.

The frame's reserved `hints` field remains **zero**; the queue is a separate API. Serialize notifications, cancellation and frame submissions on the presenter owner; drain pending output before accepting further notifications or cancellation. Continue submitting every vsync normally, not extra frames per interrupt call.

Predictions cannot recover text written and overwritten, or scrolled entirely away, between snapshots. Such output requires a stronger confirmed-output source; this API deliberately does not invent it from observed call arguments.

#### Captured boot, scrolling and clear regressions

`tests/boot_notifications.rs` replays PC DOS 1.00 and 2.10 on IBM 5150 and XT machines with CGA. The boot-only fixtures remain intact; the additional `*-scroll` captures continue from boot through four `DIR` commands, `CLS`, and another four `DIR` commands. The fixtures retain ordered INT 10h/INT 21h observations, registers and relevant buffers, plus coherent VRAM/CRTC snapshots. Lossless VRAM patches and repeat counts preserve every vsync and interrupt boundary. BIOS teletype (INT 10h/AH=0Eh) is the notification source; retained DOS calls and nested BIOS operations are not counted again.

The scrolling JSONL files use lossless gzip containers to keep the permanent fixtures small; decompression preserves the original event order, interrupt boundaries and frame repeats. Provenance retains both the uncompressed trace hashes and the stored gzip hashes. Glass goldens retain their exact control bytes, including sampled carriage returns and the DOS 2.10 clear marker.

Boot-only expected glass bytes were derived through the pre-notification presenter. Scrolling goldens are independently generated by the real presenter with notifications disabled, then reviewed against the captured guest screens; notified replay must match those snapshot-only bytes exactly. Neither oracle manufactures output from interrupt arguments. The regressions require eight complete directory transcripts, including rows no longer visible after scrolling, and observable upward row shifts before and after `CLS`.

The captured DOS versions behave differently: PC DOS 1.00 rejects `CLS` with `Bad command or file name` and leaves the listing visible; PC DOS 2.10 clears the listing and returns to a lone `A>` on the second screen row. Tests check these outcomes in decoded snapshots as well as checking output after the command. The clear is not inferred from an interrupt call.

`tests/fixtures/boot-notifications/provenance.json` records source-media and trace hashes, machine BIOS, event counts and oracle provenance. No guest disk or ROM binaries are included. Date/time input and all nine commands were ordinary terminal input; captures ended with a verified host stop at the stable prompt, not guest poweroff. Synthetic exact-byte regressions separately exercise confirmed soft wraps, ambiguity, expiration, cancellation and transaction boundaries that these captures do not necessarily trigger.

#### Other deferred work

Vertical tabs and additional character sets, specifically ISO-8859-1, remain unimplemented TODOs.

Input is outside this presenter. Future cooked-input and guest-echo validation are design intent only: this API does not implement raw input, backspace/arrow input handling, echo checking, or echo errors. The existing independent input decoder does not change that boundary.

## Display technology

`tigt_set_display_technology(technology)` / `Session::set_display_technology(DisplayTechnology)` select an optional terminal presentation policy. C accepts `TIGT_DISPLAY_GENERIC=0` or `TIGT_DISPLAY_MDA=1`; Rust exposes `DisplayTechnology::{Generic,Mda}`. This additive API does not change the ABI version or any frame/configuration layout.

Generic is the new-session default and honors the resolved cursor immediately. MDA hides only the initial terminal cursor until an accepted text submission contains visible output: distinct resolved foreground/background RGB plus a nonblank, non-whitespace glyph or an underline. Whitespace, nonbreaking spaces and the empty braille pattern are blank; an underline can make a blank visible. Cursor flags alone and equal-colour POST RAM fills do not count.

The latch observes every accepted submission, not just frames sampled by the renderer. After first output, clearing the display does not hide subsequent cursors. Bitmap submissions neither release nor reset the latch. Native snapshots always retain the exact source cursor flags, coordinates and attributes.

Set the hint before submitting that technology's frame, and serialize hints with submissions when ordering matters. An actual technology change resets the latch and redraws the retained frame even without another submission; a repeated identical hint does nothing. The hint and latch survive suspend/resume; shutdown/init reset them to Generic. Setting requires an active session (`BUSY` otherwise); unsupported C values return `ARGUMENT` without altering state.

## Register/VRAM adapter

Include `tigt_video.h` in C, or use `tigt::video::{AdapterKind, VideoAdapter, Frame}` in Rust. The decoder is independent of terminal sessions: tests can inspect resolved cells or RGB pixels without curses initialization, a TTY, a font ROM or a locale. It is available with no Cargo features enabled.

| C | Rust | Purpose |
|---|---|---|
| `tigt_video_create(adapter)` | `VideoAdapter::new(kind)` | Own register state and reusable frame storage. |
| `tigt_video_write(video,port,value)` | `adapter.write(port,value)` | Feed relevant byte-sized output-port writes. |
| `tigt_video_decode(video,vram,length,blink_on,&frame)` | `adapter.decode(vram,blink_on)` | Decode synchronously, without a terminal. |
| `tigt_video_decode_text(video,vram,length,columns,rows,blink_on,&frame)` | `adapter.decode_text(vram,columns,rows,blink_on)` | Decode text with caller-supplied geometry, without changing register state. |
| `tigt_video_present(video,vram,length,blink_on)` | `adapter.present(&session,vram,blink_on)` | Decode, select display technology and submit to the existing renderer. |
| `tigt_video_destroy(video)` | `Drop` | Release state and frame storage. |

Adapters are `TIGT_VIDEO_MDA`, `TIGT_VIDEO_CGA`, `TIGT_VIDEO_PCJR` / `AdapterKind::{Mda,Cga,Pcjr}`. Each decoder is externally serialized in C; Rust uses mutable borrows. Presenting requires an active session and follows its lifecycle rules. Presentation does not change overscan metadata.

### Supported registers and modes

- MDA: mirrored CRTC index/data ports `3B0h..3B7h`, mode control `3B8h`.
- CGA and the PCjr CGA-compatible view: mirrored index/data ports `3D0h..3D7h`, mode control `3D8h`, color select `3D9h`.
- Only CRTC `01h` (displayed row width) and `0Ch/0Dh` (14-bit word start address) are tracked. Other writes are ignored. There is no register-read API, cursor emulation, timing, scrolling logic beyond start-address interpretation, or overscan.
- `decode` and `present` text is always 25 rows: MDA requires 80 columns; CGA/PCjr accepts 40 or 80, selected by CRTC `01h`. Mode bit 0's dot-clock effect is outside this decoder. Nonstandard widths return `ARGUMENT`.
- Mode bit 3 controls video enable; disabled output is black. CGA/PCjr mode bit 1 selects graphics, and bit 4 selects 640×200 1bpp instead of 320×200 2bpp. Both standard graphics modes require CRTC `01h = 40`, use 80 bytes per scanline and MSB-left pixels.
- Mode bit 5 enables text blink: bit 7 of a CGA attribute then selects blinking instead of a bright background. `blink_on` is the caller-selected visible phase; no clock or blink timer is created. MDA resolves normal/intense, blank, underline and reverse-video attributes; disabling MDA blink does not create CGA colors.
- CGA 320×200 color selection: low four bits choose pixel 0, bit 4 selects intensity, bit 5 chooses green/red/brown versus cyan/magenta/white. Mode bit 2 overrides the latter choice with cyan/red/white. In 640×200 mode, the low four bits choose the foreground against black. No composite artifact colors are decoded.
- PCjr offers this **CGA-compatible interface only**, not native gate-array or paging registers, programmable palettes, or additional video modes. The emulator supplies its selected 16 KiB bank.

`tigt_video_decode_text` / `VideoAdapter::decode_text` is a separate text-only helper for callers that already know the displayed CRTC geometry. It accepts 1–320 columns and 1–128 rows, with at most 21440 cells, overriding the tracked width and standard 25 rows for this call only. The returned text frame has exactly these dimensions and contiguous rows with stride equal to columns. It uses the same borrowed full aperture, owned reusable output buffer, word start-address wrapping, CP437/attribute decoding, video enable and caller-supplied blink phase as `decode`; no timing or cursor emulation is added. CGA/PCjr graphics mode, invalid dimensions and incomplete apertures return `ARGUMENT` without changing register state, the output descriptor or previously decoded storage. Allocation failure returns `SYSTEM` with the same transactional behavior. The existing `decode`/`present` geometry restrictions are unchanged.

### Memory and frame ownership

Pass the complete aperture beginning at its video-memory base: at least 4096 bytes for MDA, or 16384 for CGA/PCjr. The decoder does not own or intercept guest memory writes. Extra input bytes are ignored; the input is never retained. Text addresses wrap within that aperture. CGA graphics use odd/even 8 KiB banks, with start address and row offsets wrapping within each bank.

`tigt_video_frame` is tagged `TIGT_VIDEO_TEXT` or `TIGT_VIDEO_BITMAP`. Width/height are cells for text, pixels for bitmaps. Its active `cells` or `pixels` pointer is contiguous; the other is NULL. Rust returns corresponding `Frame::Text` or `Frame::Bitmap` slices. Graphics are compact 320×200 or 640×200 RGB frames, presented with `pixel_width = 1`; there is no horizontal staging expansion.

Decoded storage belongs to the adapter and is valid until its next decode/present or destruction. Rust enforces that lifetime. Input VRAM can be changed or released immediately after decoding. Storage grows only when necessary and is reused; there is no per-frame allocation once capacity is sufficient. Argument/allocation errors leave the C output descriptor unchanged.

Creation starts with 80-column text, start address zero, video/blink disabled, color zero and CRTC index zero. Invalid C adapter kinds return NULL with `errno = EINVAL`; allocation failure returns NULL/`ENOMEM` or `SYSTEM` during decode. See [Getting started](getting-started.md#headless-emulator-tests) for a terminal-free consumer.

## Overscan

`tigt_overscan` / `Overscan` has `color` and `left`, `right`, `top`, `bottom` dimensions in native backing pixels before host vertical line doubling. `tigt_set_overscan` / `Session::set_overscan` copy metadata; `tigt_get_overscan` / `Session::overscan` retrieve it.

It is not drawn. Submitting a bitmap or text frame does not reset it. Serialize metadata with frame submission when ordering matters. New sessions start with zero metadata; suspension retains it. Attributes snapshots include this metadata for instrumentation.

## Input decoding

The input decoder is independent of a curses session and of the keyboard mapper:

- C: `tigt_input_create(callback,user)`, `tigt_input_feed`, `tigt_input_flush`, `tigt_input_destroy`.
- Rust: `InputDecoder`, `feed` and `flush`.

Feed arbitrary stream fragments; UTF-8 and escape sequences can span calls. Flush resolves a pending bare Escape; the embedding application chooses when to do that. Live sessions perform their own input polling/escape timeout. No key is reserved: Ctrl+C and Ctrl+Z are semantic input events.

Events distinguish press, repeat and release; key identity includes Unicode characters, function keys, navigation, editing, lock and modifier keys. Modifier bits cover Shift, Control, Alt and Super. The decoder understands supported traditional terminal sequences and Kitty keyboard events, including fragmented SS3 (`ESC O`) arrows, Home/End, keypad Begin, and F1–F4. SS3 R is F3; CSI R remains a cursor-position report, not a key. Legacy terminal taps cannot reconstruct physical key-release timing that the transport never reported. Invalid or unsupported input is not a promise of arbitrary terminal-protocol compatibility.

C decoder creation rejects a NULL callback. Feed/flush/destroy must be externally serialized. A decoder callback executes synchronously during explicit feed/flush; a live-session callback executes on the input worker.

## PC keyboard integration

C consumers may include `tigt_keyboard.h` alongside the separate mapper's `pc_xt_keyboard.h`. `tigt_keyboard_handle` converts a semantic input event into `pc_xt_keyboard_v1_key_event` records: a physical PC key identity and a separate down/up flag. Supply `PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS` slots; capacity and return count are in events, not bytes. Link the mapper separately; the core tigt library does not require it.

Rust enables `keyboard` to expose event conversions and the mapper re-export. `PcEvent::Make(key)` / `Break(key)` expose `key.physical` before wire encoding. Ordinary identities use PC make-position numbering; enhanced Print Screen and Pause are distinct identities, not E0/E1 byte streams. Feed an emulator's existing host-key interface and let its emulated keyboard choose guest scan-code encoding. The mapper's separate byte API remains available to consumers that actually need wire bytes.

Input decoding, keyboard mapping and monitor decoding have independent state. A live terminal session coordinates terminal ownership and input callbacks, but display technology never selects keyboard profile. Applications own the mapper and release policy; tigt does not instantiate a PC keyboard. Do not submit one input to both mapper output APIs: both consume the same held-key state.

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

Core/session C errors are `TIGT_ERROR_ARGUMENT`, `TIGT_ERROR_TERMINAL`, `TIGT_ERROR_BUSY` and `TIGT_ERROR_SYSTEM`; success is `TIGT_OK`. Rust exposes corresponding `Error` values and callback-panic reporting. Argument errors include unsupported dimensions/formats/flags, invalid text cells and insufficient font data. Busy includes conflicting ownership and missing/inactive frame state. Terminal errors describe unavailable terminal initialization; system errors describe OS/allocation/I/O failures. The independent presenter adds the statuses and unrepresentability error [described above](#snapshots-cursor-and-confirmation), exposed through `presenter::{Status,Error}`.

Signal delivery is asynchronous; successful configuration does not mean a capture succeeded. Inspect snapshot completion status, and validate/decode output before declaring an instrumentation step successful. Standard signals can coalesce; this is a request for a current frame, not a lossless frame-recording protocol.

Do not expect native dumps to preserve absent historical information: text cells contain resolved Unicode/RGB, not original guest VRAM attribute bytes. The legacy byte-pair export is a documented projection, not an inverse hardware emulator.
