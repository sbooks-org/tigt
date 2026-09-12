# Terminal mouse support levels

Mouse support has two independent dimensions: **which events the terminal reports** and **how precisely it reports position**. Enabling a reporting mode requests that behavior; it does not establish that every terminal implements it.

Mouse input uses TIGT's shared terminal reader and does not require the optional PC keyboard mapper. For C/Rust setup and callback ownership rules, see [mouse input in the reference](reference.md#mouse-input).

## Event coverage

| Terminal support | What TIGT delivers |
|---|---|
| No mouse reporting | No mouse events. Keyboard and display operation remain independent. |
| X10 press-only clicks | Left/middle/right clicks with position. Explicit `MouseMode::X10` / `TIGT_MOUSE_X10` adds a marked synthetic release because the terminal reports no release. No reliable dragging or held-button timing. |
| Normal tracking — mode 1000 | Button presses/releases and wheel events, each carrying position. No independent motion. TIGT emits **Move immediately before the button or wheel event**. |
| Button-motion tracking — mode 1002 | Normal tracking plus motion while dragging. No hover motion with all buttons released. |
| All-motion tracking — mode 1003 | Button and wheel events plus movement without a button held. |
| Kitty pixel-mode leave extension | A genuine **Leave** event when the pointer exits the terminal window. Its coordinates are invalid. Focus loss is not treated as pointer departure. |

For ordinary live tracking, TIGT requests normal, drag and all-motion modes in that order. Unsupported requests can be ignored by the terminal. **TIGT does not separately negotiate or certify each tracking level.** X10 press-only tracking is an explicit choice, not an automatically detected fallback.

Every position-bearing Down, Up or Scroll event is immediately preceded by Move, even if the position has not changed. A terminal that reports only clicks therefore still establishes position before each click. This ordering does not reconstruct movement between clicks or missing physical release timing.

## Coordinate precision

| Reporting format | Precision and limitations |
|---|---|
| Legacy byte coordinates | Character-cell positions, limited to 223 rows/columns. Legacy normal-tracking releases do not identify the released button, so TIGT clears all known held buttons rather than guessing which member of a chord was released. |
| SGR cell coordinates — mode 1006 | Character-cell positions without the legacy byte-coordinate limit; identifies individual button releases. |
| SGR pixel coordinates — mode 1016 | Integer terminal-pixel positions, allowing finer movement within a character cell. Not physical sub-pixel reporting. |

Raw `x/y` are zero-based and carry explicit cell/pixel units and position validity. Legacy byte reports retain cell units even when pixel SGR is selected.

### Automatic and explicit selection

- Live `MouseMode::Auto` / `TIGT_MOUSE_AUTO` probes mode **1016** and selects pixels when recognized and settable; otherwise it falls back to cells. Probing requires stdin and stdout to refer to the same terminal.
- `MouseMode::Cells` / `TIGT_MOUSE_CELLS` explicitly selects cell reporting.
- `MouseMode::Pixels` / `TIGT_MOUSE_PIXELS` trusts the caller's selection of a supporting terminal; it does not guarantee support by itself.
- Standalone decoders do not probe or configure a terminal. Auto means cell decoding there.
- `session.mouse_mode()` / `tigt_get_mouse_mode()` reports the resolved live protocol, not a complete capability inventory. It returns Off while suspended or inactive.

TIGT disables conflicting mouse encodings while it owns reporting. Suspend/shutdown disable TIGT's modes and restore saved modes where the terminal supports DEC mode save/restore. Without that extension, TIGT's modes are left disabled.

## Displayed-frame coordinates

Both cell and pixel reports can map into fractional **displayed-frame coordinates**:

- Bitmap coordinates use logical source pixels: `width / pixel_width` by source height.
- Text coordinates use source text cells.
- Mapping uses the centre of the reported terminal cell or pixel. A fractional result preserves the mapping precision; it does not make a coarse cell report more precise than its source.
- Geometry belongs to the completed displayed frame, not merely the latest submitted frame.
- Coordinates are not clamped. The inside-frame flag distinguishes hits from clipped or outside-frame positions.
- With no completed frame, an in-progress redraw, or missing metrics needed to convert between cells and pixels, frame coordinates are invalid. Raw terminal coordinates remain available when the report supplies them.

Moving outside the displayed frame is distinct from leaving the terminal window. An outside-frame position is not converted into a window-leave event.

## Limits

- Wheel reporting consists of discrete horizontal/vertical steps, positive right/down. Continuous high-resolution wheel deltas are not available through the implemented protocols.
- Touch taps work only when the terminal translates them into mouse clicks. TIGT preserves those clicks but cannot identify them as touch.
- No native touch contact identity or multipoint-touch stream is supported or advertised.
- No leave event is fabricated for terminals without the Kitty extension. Keyboard focus loss and idle time do not imply pointer departure. Leave does not synthesize button releases.
- UTF-8 mouse encoding 1005 and urxvt encoding 1015 are not supported; TIGT disables these conflicting encodings during mouse ownership.

This is a protocol-level support description, not a tested terminal-emulator/version compatibility matrix. Terminal settings and intervening multiplexers can also affect which reports reach TIGT.

## Protocol references

- [Xterm mouse tracking and extended coordinate formats](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-Mouse-Tracking)
- [Kitty window-leave reporting extension](https://sw.kovidgoyal.net/kitty/misc-protocol/#reporting-when-the-mouse-leaves-the-window)
