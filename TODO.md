# TODO

## Correct iTerm2 image aspect without losing source detail

- [ ] Remove the lossy 320×200 presentation normalization and verify native-resolution images display at the intended 4:3 aspect ratio in an actual terminal.

The current iTerm2 encoder averages adjacent columns of 640-wide logical images and duplicates columns of 160-wide images to produce a 320×200 PNG. The averaging visibly degrades CGA high-resolution graphics (`SCREEN 2`). It is not an acceptable aspect-ratio correction.

Preserve native logical resolutions (160×200, 320×200, and 640×200) and request the same 4:3 destination rectangle for all three. Their pixel width:height ratios are respectively 5:3, 5:6, and 5:12. Native snapshots must remain unchanged.

The encoder already sends explicit pixel-valued `width` and `height` with `preserveAspectRatio=0`. The [iTerm2 image protocol](https://iterm2.com/documentation-images.html) defines this as permitting nonuniform scaling. [WezTerm 20240203-110809-5046fc22's image handler](https://github.com/wezterm/wezterm/blob/20240203-110809-5046fc22/term/src/terminalstate/iterm.rs) honors both dimensions when that flag is zero. The incorrect displayed proportions reported before normalization remain unexplained; investigate the actual terminal rendering path rather than applying another source-resolution workaround.

Verification must inspect the terminal-displayed geometry, not just the decoded PNG or emitted sizing fields. Check all three logical widths, retain distinguishable adjacent columns in 640-wide images, and confirm that circles have the intended proportions. Preserve the existing layout/resize behavior, idle-frame suppression, keyboard ownership, and image cleanup on text return, suspend, and shutdown. Keep sixel behavior unchanged.
