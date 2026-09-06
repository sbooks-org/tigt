// SPDX-License-Identifier: MIT-0
// Copyright (C) 2026 Simplebooks Foundation
// Copyright (C) 2026 Josh Rodd

use serde::Serialize;
use unicode_width::UnicodeWidthChar;
use vte::{Params, Perform};

pub const COLS: usize = 320;
pub const ROWS: usize = 80;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd, Serialize)]
#[serde(tag = "kind", content = "value", rename_all = "snake_case")]
pub enum Color {
    DefaultForeground,
    DefaultBackground,
    Ansi(u8),
    Indexed(u8),
    Rgb([u8; 3]),
}

impl Color {
    pub fn rgb(self, bold: bool) -> [u8; 3] {
        const BASE: [[u8; 3]; 16] = [
            [0, 0, 0],
            [128, 0, 0],
            [0, 128, 0],
            [128, 128, 0],
            [0, 0, 128],
            [128, 0, 128],
            [0, 128, 128],
            [192, 192, 192],
            [128, 128, 128],
            [255, 0, 0],
            [0, 255, 0],
            [255, 255, 0],
            [0, 0, 255],
            [255, 0, 255],
            [0, 255, 255],
            [255, 255, 255],
        ];
        let index = match self {
            Self::DefaultForeground => return [192; 3],
            Self::DefaultBackground => return [0; 3],
            Self::Rgb(rgb) => return rgb,
            Self::Ansi(n) => n + if bold && n < 8 { 8 } else { 0 },
            Self::Indexed(n) => n,
        };
        match index {
            0..=15 => BASE[index as usize],
            16..=231 => {
                let n = index - 16;
                let component = |n| if n == 0 { 0 } else { 55 + 40 * n };
                [component(n / 36), component((n / 6) % 6), component(n % 6)]
            }
            _ => [8 + (index - 232) * 10; 3],
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize)]
pub struct Style {
    pub foreground: Color,
    pub background: Color,
    pub bold: bool,
    pub underline: bool,
    pub blink: bool,
    pub inverse: bool,
    pub invisible: bool,
}

impl Default for Style {
    fn default() -> Self {
        Self {
            foreground: Color::DefaultForeground,
            background: Color::DefaultBackground,
            bold: false,
            underline: false,
            blink: false,
            inverse: false,
            invisible: false,
        }
    }
}

impl Style {
    pub fn colors(self) -> ([u8; 3], [u8; 3]) {
        let mut fg = self.foreground.rgb(self.bold);
        let mut bg = self.background.rgb(false);
        if self.inverse {
            std::mem::swap(&mut fg, &mut bg);
        }
        if self.invisible {
            fg = bg;
        }
        (fg, bg)
    }
}

#[derive(Clone, Debug)]
pub struct Cell {
    pub character: char,
    pub style: Style,
    pub written: bool,
    pub combined: bool,
}

impl Cell {
    fn blank(style: Style) -> Self {
        Self {
            character: ' ',
            style,
            written: false,
            combined: false,
        }
    }
}

#[derive(Clone)]
struct Screen {
    cells: Vec<Cell>,
    x: usize,
    y: usize,
    saved: (usize, usize, Style),
    wrap: bool,
    top: usize,
    bottom: usize,
}

impl Screen {
    fn new() -> Self {
        Self {
            cells: vec![Cell::blank(Style::default()); COLS * ROWS],
            x: 0,
            y: 0,
            saved: (0, 0, Style::default()),
            wrap: false,
            top: 0,
            bottom: ROWS - 1,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Cursor {
    pub column: usize,
    pub row: usize,
    pub visible: bool,
    pub blinking: bool,
}

pub struct Terminal {
    screen: Screen,
    primary: Option<Screen>,
    last_alternate: Option<Screen>,
    style: Style,
    autowrap: bool,
    origin: bool,
    cursor_visible: bool,
    cursor_blinking: bool,
    pub errors: Vec<String>,
}

impl Terminal {
    pub fn replay(bytes: &[u8]) -> Self {
        let mut terminal = Self {
            screen: Screen::new(),
            primary: None,
            last_alternate: None,
            style: Style::default(),
            autowrap: true,
            origin: false,
            cursor_visible: true,
            cursor_blinking: true,
            errors: Vec::new(),
        };
        let mut parser = vte::Parser::new();
        parser.advance(&mut terminal, bytes);
        terminal
    }

    pub fn cells(&self) -> (&[Cell], &'static str) {
        // A normal terminal shutdown restores the primary buffer. Preserve the graphics
        // buffer rather than mistaking the restored shell prompt for guest video.
        if self.primary.is_none() {
            if let Some(screen) = &self.last_alternate {
                return (&screen.cells, "last_alternate_before_exit");
            }
        }
        (
            &self.screen.cells,
            if self.primary.is_some() {
                "active_alternate"
            } else {
                "primary"
            },
        )
    }

    /// Current terminal cursor, including native visibility and blink control.
    pub fn cursor(&self) -> Cursor {
        Cursor {
            column: self.screen.x,
            row: self.screen.y,
            visible: self.cursor_visible,
            blinking: self.cursor_blinking,
        }
    }

    fn scroll_up(&mut self, count: usize) {
        let start = self.screen.top * COLS;
        let end = (self.screen.bottom + 1) * COLS;
        let n = count.min(self.screen.bottom - self.screen.top + 1) * COLS;
        self.screen.cells[start..end].rotate_left(n);
        self.erase(end - n, end);
    }

    fn scroll_down(&mut self, count: usize) {
        let start = self.screen.top * COLS;
        let end = (self.screen.bottom + 1) * COLS;
        let n = count.min(self.screen.bottom - self.screen.top + 1) * COLS;
        self.screen.cells[start..end].rotate_right(n);
        self.erase(start, start + n);
    }

    fn newline(&mut self) {
        self.screen.wrap = false;
        if self.screen.y == self.screen.bottom {
            self.scroll_up(1);
        } else {
            self.screen.y = (self.screen.y + 1).min(ROWS - 1);
        }
    }

    fn erase(&mut self, start: usize, end: usize) {
        // Erasure paints known background-colored spaces. Only the initial
        // screen (which may predate the capture) has unknown coverage.
        self.screen.cells[start..end].fill(Cell {
            written: true,
            ..Cell::blank(self.style)
        });
    }

    fn sgr(&mut self, params: &[Vec<u16>]) {
        let mut i = 0;
        while i < params.len() {
            let p = params[i][0];
            match p {
                0 => self.style = Style::default(),
                1 => self.style.bold = true,
                22 => self.style.bold = false,
                4 | 21 => self.style.underline = true,
                24 => self.style.underline = false,
                5 | 6 => self.style.blink = true,
                25 => self.style.blink = false,
                7 => self.style.inverse = true,
                27 => self.style.inverse = false,
                8 => self.style.invisible = true,
                28 => self.style.invisible = false,
                30..=37 => self.style.foreground = Color::Ansi((p - 30) as u8),
                40..=47 => self.style.background = Color::Ansi((p - 40) as u8),
                90..=97 => self.style.foreground = Color::Ansi((p - 90 + 8) as u8),
                100..=107 => self.style.background = Color::Ansi((p - 100 + 8) as u8),
                39 => self.style.foreground = Color::DefaultForeground,
                49 => self.style.background = Color::DefaultBackground,
                38 | 48 => {
                    let colon = params[i].len() > 1;
                    let values: Vec<u16> = if colon {
                        params[i][1..].to_vec()
                    } else {
                        params[i + 1..].iter().map(|p| p[0]).collect()
                    };
                    let color = match values.as_slice() {
                        [5, n, ..] if *n <= 255 => {
                            if !colon {
                                i += 2;
                            }
                            Some(Color::Indexed(*n as u8))
                        }
                        [2, rest @ ..] => {
                            // ISO colon RGB permits an empty color-space field.
                            let rgb = if colon && rest.len() == 4 {
                                &rest[1..]
                            } else {
                                rest
                            };
                            if rgb.len() >= 3 && rgb[..3].iter().all(|n| *n <= 255) {
                                if !colon {
                                    i += 4;
                                }
                                Some(Color::Rgb([rgb[0] as u8, rgb[1] as u8, rgb[2] as u8]))
                            } else {
                                None
                            }
                        }
                        _ => None,
                    };
                    if let Some(color) = color {
                        if p == 38 {
                            self.style.foreground = color;
                        } else {
                            self.style.background = color;
                        }
                    } else {
                        self.errors.push("malformed extended SGR color".into());
                        break;
                    }
                }
                // These attributes do not change sextant coverage or its flat colors.
                2 | 3 | 9 | 23 | 26 | 29 | 53 | 55 => {}
                _ => self.errors.push(format!("unsupported SGR attribute {p}")),
            }
            i += 1;
        }
    }
}

impl Perform for Terminal {
    fn print(&mut self, c: char) {
        let width = c.width().unwrap_or(0);
        if width == 0 {
            let x = if self.screen.wrap {
                self.screen.x
            } else {
                self.screen.x.saturating_sub(1)
            };
            self.screen.cells[self.screen.y * COLS + x].combined = true;
            return;
        }
        if self.screen.wrap && self.autowrap {
            self.screen.x = 0;
            self.newline();
        }
        if width == 2 && self.screen.x == COLS - 1 && self.autowrap {
            self.screen.x = 0;
            self.newline();
        }
        let position = self.screen.y * COLS + self.screen.x;
        self.screen.cells[position] = Cell {
            character: c,
            style: self.style,
            written: true,
            combined: false,
        };
        if width == 2 && self.screen.x + 1 < COLS {
            self.screen.cells[position + 1] = Cell {
                character: '\0',
                style: self.style,
                written: true,
                combined: false,
            };
        }
        self.screen.wrap = self.screen.x + width >= COLS;
        self.screen.x = (self.screen.x + width).min(COLS - 1);
    }

    fn execute(&mut self, byte: u8) {
        match byte {
            b'\r' => {
                self.screen.x = 0;
                self.screen.wrap = false;
            }
            b'\n' | 0x0b | 0x0c => self.newline(),
            8 => {
                self.screen.x = self.screen.x.saturating_sub(1);
                self.screen.wrap = false;
            }
            b'\t' => {
                self.screen.x = ((self.screen.x / 8 + 1) * 8).min(COLS - 1);
                self.screen.wrap = false;
            }
            _ => {}
        }
    }

    fn esc_dispatch(&mut self, intermediates: &[u8], ignore: bool, byte: u8) {
        if ignore {
            self.errors.push("ignored malformed ESC sequence".into());
            return;
        }
        if !intermediates.is_empty() {
            return;
        } // Character-set designation; graphics are Unicode.
        match byte {
            b'7' => self.screen.saved = (self.screen.x, self.screen.y, self.style),
            b'8' => {
                (self.screen.x, self.screen.y, self.style) = self.screen.saved;
                self.screen.wrap = false;
            }
            b'D' => self.newline(),
            b'E' => {
                self.screen.x = 0;
                self.newline();
            }
            b'M' => {
                if self.screen.y == self.screen.top {
                    self.scroll_down(1);
                } else {
                    self.screen.y = self.screen.y.saturating_sub(1);
                }
                self.screen.wrap = false;
            }
            b'c' => {
                self.screen = Screen::new();
                self.style = Style::default();
                self.autowrap = true;
                self.origin = false;
            }
            b'=' | b'>' => {}
            _ => self.errors.push(format!("unsupported ESC 0x{byte:02x}")),
        }
    }

    fn csi_dispatch(&mut self, params: &Params, intermediates: &[u8], ignore: bool, action: char) {
        if ignore {
            self.errors.push("ignored malformed CSI sequence".into());
            return;
        }
        let p: Vec<Vec<u16>> = params.iter().map(|p| p.to_vec()).collect();
        let arg = |i: usize, default: usize| {
            p.get(i)
                .and_then(|p| p.first())
                .copied()
                .filter(|n| *n != 0)
                .map(usize::from)
                .unwrap_or(default)
        };
        let first = p.first().map(|p| p[0]).unwrap_or(0);
        if intermediates == b"?" && matches!(action, 'h' | 'l') {
            for parameter in &p {
                let set = action == 'h';
                match parameter[0] {
                    47 | 1047 | 1049 => {
                        if set && self.primary.is_none() {
                            self.primary = Some(std::mem::replace(&mut self.screen, Screen::new()));
                            self.last_alternate = None;
                        } else if !set {
                            if let Some(primary) = self.primary.take() {
                                self.last_alternate =
                                    Some(std::mem::replace(&mut self.screen, primary));
                            }
                        }
                    }
                    7 => self.autowrap = set,
                    6 => {
                        self.origin = set;
                        self.screen.x = 0;
                        self.screen.y = if set { self.screen.top } else { 0 };
                    }
                    1048 => {
                        if set {
                            self.screen.saved = (self.screen.x, self.screen.y, self.style);
                        } else {
                            (self.screen.x, self.screen.y, self.style) = self.screen.saved;
                        }
                    }
                    12 => self.cursor_blinking = set,
                    25 => self.cursor_visible = set,
                    1 | 1000..=1007 | 1015 | 2004 | 2026 => {}
                    n => self
                        .errors
                        .push(format!("unsupported private terminal mode {n}")),
                }
            }
            return;
        }
        if !intermediates.is_empty() {
            if action == 'q' && intermediates == b" " {
                self.cursor_blinking = matches!(first, 0 | 1 | 3 | 5);
                return;
            } // Cursor shape.
              // Kitty keyboard protocol controls alter input encoding, not the screen.
            if action == 'u' && matches!(intermediates, b">" | b"=" | b"<" | b"?") {
                return;
            }
            self.errors.push(format!(
                "unsupported CSI intermediates {intermediates:?} action {action}"
            ));
            return;
        }
        if !matches!(action, 'm' | 's') {
            self.screen.wrap = false;
        }
        match action {
            'm' => self.sgr(&p),
            'A' => {
                self.screen.y = self.screen.y.saturating_sub(arg(0, 1)).max(if self.origin {
                    self.screen.top
                } else {
                    0
                })
            }
            'B' | 'e' => {
                self.screen.y = (self.screen.y + arg(0, 1)).min(if self.origin {
                    self.screen.bottom
                } else {
                    ROWS - 1
                })
            }
            'C' | 'a' => self.screen.x = (self.screen.x + arg(0, 1)).min(COLS - 1),
            'D' => self.screen.x = self.screen.x.saturating_sub(arg(0, 1)),
            'E' => {
                self.screen.y = (self.screen.y + arg(0, 1)).min(ROWS - 1);
                self.screen.x = 0;
            }
            'F' => {
                self.screen.y = self.screen.y.saturating_sub(arg(0, 1));
                self.screen.x = 0;
            }
            'G' | '`' => self.screen.x = arg(0, 1).saturating_sub(1).min(COLS - 1),
            'd' => {
                self.screen.y = (arg(0, 1) - 1 + if self.origin { self.screen.top } else { 0 }).min(
                    if self.origin {
                        self.screen.bottom
                    } else {
                        ROWS - 1
                    },
                )
            }
            'H' | 'f' => {
                self.screen.y = (arg(0, 1) - 1 + if self.origin { self.screen.top } else { 0 })
                    .min(if self.origin {
                        self.screen.bottom
                    } else {
                        ROWS - 1
                    });
                self.screen.x = (arg(1, 1) - 1).min(COLS - 1);
            }
            'J' => {
                let pos = self.screen.y * COLS + self.screen.x;
                match first {
                    0 => self.erase(pos, ROWS * COLS),
                    1 => self.erase(0, pos + 1),
                    2 => self.erase(0, ROWS * COLS),
                    3 => {}
                    _ => self
                        .errors
                        .push(format!("invalid erase display mode {first}")),
                }
            }
            'K' => {
                let start = self.screen.y * COLS;
                match first {
                    0 => self.erase(start + self.screen.x, start + COLS),
                    1 => self.erase(start, start + self.screen.x + 1),
                    2 => self.erase(start, start + COLS),
                    _ => self.errors.push(format!("invalid erase line mode {first}")),
                }
            }
            'X' => {
                let start = self.screen.y * COLS + self.screen.x;
                self.erase(start, start + arg(0, 1).min(COLS - self.screen.x));
            }
            '@' | 'P' => {
                let start = self.screen.y * COLS + self.screen.x;
                let end = (self.screen.y + 1) * COLS;
                let n = arg(0, 1).min(end - start);
                if action == '@' {
                    self.screen.cells[start..end].rotate_right(n);
                    self.erase(start, start + n);
                } else {
                    self.screen.cells[start..end].rotate_left(n);
                    self.erase(end - n, end);
                }
            }
            'S' => self.scroll_up(arg(0, 1)),
            'T' => self.scroll_down(arg(0, 1)),
            'L' | 'M' => {
                if self.screen.y >= self.screen.top && self.screen.y <= self.screen.bottom {
                    let old_top = self.screen.top;
                    self.screen.top = self.screen.y;
                    if action == 'L' {
                        self.scroll_down(arg(0, 1));
                    } else {
                        self.scroll_up(arg(0, 1));
                    }
                    self.screen.top = old_top;
                }
            }
            'r' => {
                let top = arg(0, 1) - 1;
                let bottom = arg(1, ROWS) - 1;
                if top < bottom && bottom < ROWS {
                    self.screen.top = top;
                    self.screen.bottom = bottom;
                    self.screen.x = 0;
                    self.screen.y = if self.origin { top } else { 0 };
                } else {
                    self.errors
                        .push(format!("invalid scroll region {}..{}", top + 1, bottom + 1));
                }
            }
            's' => self.screen.saved = (self.screen.x, self.screen.y, self.style),
            'u' => (self.screen.x, self.screen.y, self.style) = self.screen.saved,
            'n' | 'c' | 't' => {} // Queries/window reports do not paint cells.
            'h' | 'l' if first == 4 => {
                if action == 'h' {
                    self.errors.push("insert mode is unsupported".into());
                }
            }
            _ => self
                .errors
                .push(format!("unsupported CSI action {action} parameters {p:?}")),
        }
    }
}
