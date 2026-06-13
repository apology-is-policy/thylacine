// kaua::widget -- the render trait + the v1.0 widget set.
//
// A widget is a pure value that paints itself into a `Buffer` over a `Rect`
// (the ratatui model: cheap, consumed on render). No terminal, no I/O -- fully
// host-testable. The v1.0 set is exactly what nora + ut + a SAK prompt need
// (KAUA.md section 3.3): Paragraph, Block, List, StatusLine. More widgets
// accrete later on the same trait.

use alloc::vec::Vec;

use crate::buffer::{Buffer, Cell};
use crate::rect::Rect;
use crate::style::{Color, Style};

/// Paint `self` into `buf` over `area`. Consumes the widget.
pub trait Widget {
    fn render(self, area: Rect, buf: &mut Buffer);
}

/// A run of text with a single style; the unit of `StatusLine` segments.
#[derive(Clone, Copy, Default)]
pub struct Span<'a> {
    pub text: &'a str,
    pub style: Style,
}

impl<'a> Span<'a> {
    /// Default-styled text.
    pub fn raw(text: &'a str) -> Self {
        Span {
            text,
            style: Style::new(),
        }
    }

    pub fn styled(text: &'a str, style: Style) -> Self {
        Span { text, style }
    }
}

/// A multi-line text block: each `\n`-separated logical line, one style, with
/// optional hard char-wrap + a vertical scroll offset (in visual lines). Word
/// wrap + a line index for O(window) scrolling are documented seams; v1.0 hard-
/// wraps at the area width and walks from the top (bounded by scroll + height).
pub struct Paragraph<'a> {
    text: &'a str,
    style: Style,
    wrap: bool,
    scroll: u16,
}

impl<'a> Paragraph<'a> {
    pub fn new(text: &'a str) -> Self {
        Paragraph {
            text,
            style: Style::new(),
            wrap: false,
            scroll: 0,
        }
    }

    pub fn style(mut self, style: Style) -> Self {
        self.style = style;
        self
    }

    pub fn wrap(mut self, on: bool) -> Self {
        self.wrap = on;
        self
    }

    /// Skip this many leading visual lines (the scroll position).
    pub fn scroll(mut self, lines: u16) -> Self {
        self.scroll = lines;
        self
    }
}

impl Widget for Paragraph<'_> {
    fn render(self, area: Rect, buf: &mut Buffer) {
        if area.is_empty() {
            return;
        }
        let w = area.width as usize;
        let scroll = self.scroll as u32;
        let mut produced: u32 = 0;
        let mut row: u16 = 0;
        for logical in self.text.split('\n') {
            // The visual sub-lines of this logical line (one, unless wrapping).
            let mut slices: Vec<&str> = Vec::new();
            if self.wrap && !logical.is_empty() {
                let mut count = 0usize;
                let mut start = 0usize;
                for (bi, _) in logical.char_indices() {
                    if count == w {
                        slices.push(&logical[start..bi]);
                        start = bi;
                        count = 0;
                    }
                    count += 1;
                }
                slices.push(&logical[start..]);
            } else {
                slices.push(logical);
            }
            for s in slices {
                if produced < scroll {
                    produced += 1;
                    continue;
                }
                if row >= area.height {
                    return;
                }
                buf.set_str(area.x, area.y + row, s, self.style);
                row += 1;
                produced += 1;
            }
        }
    }
}

/// An optional box-drawing border (the *Bonfire* `border` colour) + an optional
/// title on the top edge. `inner` is the content rect for the framed widget.
pub struct Block<'a> {
    title: Option<&'a str>,
    borders: bool,
    border_style: Style,
    title_style: Style,
}

impl<'a> Block<'a> {
    /// A bordered block (the common frame case); drop the border with
    /// `borders(false)`.
    pub fn new() -> Self {
        Block {
            title: None,
            borders: true,
            border_style: Style::new(),
            title_style: Style::new(),
        }
    }

    pub fn title(mut self, title: &'a str) -> Self {
        self.title = Some(title);
        self
    }

    pub fn borders(mut self, on: bool) -> Self {
        self.borders = on;
        self
    }

    pub fn border_style(mut self, style: Style) -> Self {
        self.border_style = style;
        self
    }

    pub fn title_style(mut self, style: Style) -> Self {
        self.title_style = style;
        self
    }

    /// True iff a border is actually drawn (requested AND the area is at least
    /// 2x2 -- a border needs room for two edges).
    fn drawn(&self, area: Rect) -> bool {
        self.borders && area.width >= 2 && area.height >= 2
    }

    /// The content area inside the border (the area itself when no border).
    pub fn inner(&self, area: Rect) -> Rect {
        if self.drawn(area) {
            Rect::new(area.x + 1, area.y + 1, area.width - 2, area.height - 2)
        } else {
            area
        }
    }
}

impl Default for Block<'_> {
    fn default() -> Self {
        Block::new()
    }
}

impl Widget for Block<'_> {
    fn render(self, area: Rect, buf: &mut Buffer) {
        if area.is_empty() {
            return;
        }
        let bs = self.border_style;
        if self.drawn(area) {
            let x0 = area.x;
            let y0 = area.y;
            let x1 = area.right() - 1;
            let y1 = area.bottom() - 1;
            buf.set_cell(x0, y0, Cell::new('\u{250C}', bs)); // |--
            buf.set_cell(x1, y0, Cell::new('\u{2510}', bs)); // --|
            buf.set_cell(x0, y1, Cell::new('\u{2514}', bs)); // L
            buf.set_cell(x1, y1, Cell::new('\u{2518}', bs)); // -|
            for x in (x0 + 1)..x1 {
                buf.set_cell(x, y0, Cell::new('\u{2500}', bs)); // -
                buf.set_cell(x, y1, Cell::new('\u{2500}', bs));
            }
            for y in (y0 + 1)..y1 {
                buf.set_cell(x0, y, Cell::new('\u{2502}', bs)); // |
                buf.set_cell(x1, y, Cell::new('\u{2502}', bs));
            }
        }
        if let Some(t) = self.title {
            // Inset past the left corner; clip before the right corner.
            let inset = if self.drawn(area) { 1 } else { 0 };
            let tx = area.x + inset;
            let max = area.width.saturating_sub(2 * inset);
            let mut cx = tx;
            let mut left = max;
            for ch in t.chars() {
                if left == 0 {
                    break;
                }
                buf.set_cell(cx, area.y, Cell::new(ch, self.title_style));
                cx = cx.saturating_add(1);
                left -= 1;
            }
        }
    }
}

/// A vertical list with an optional selected row (styled `selected_style`, a
/// full-row highlight) + a scroll `offset`. Completion menus, file pickers,
/// `jobs`.
pub struct List<'a> {
    items: &'a [&'a str],
    selected: Option<usize>,
    style: Style,
    selected_style: Style,
    offset: usize,
}

impl<'a> List<'a> {
    pub fn new(items: &'a [&'a str]) -> Self {
        List {
            items,
            selected: None,
            style: Style::new(),
            selected_style: Style::new(),
            offset: 0,
        }
    }

    pub fn select(mut self, sel: Option<usize>) -> Self {
        self.selected = sel;
        self
    }

    pub fn style(mut self, style: Style) -> Self {
        self.style = style;
        self
    }

    pub fn selected_style(mut self, style: Style) -> Self {
        self.selected_style = style;
        self
    }

    /// First item index to show (the scroll position).
    pub fn offset(mut self, offset: usize) -> Self {
        self.offset = offset;
        self
    }
}

impl Widget for List<'_> {
    fn render(self, area: Rect, buf: &mut Buffer) {
        if area.is_empty() {
            return;
        }
        let rows = area.height as usize;
        for r in 0..rows {
            let idx = self.offset + r;
            if idx >= self.items.len() {
                break;
            }
            let y = area.y + r as u16;
            let selected = self.selected == Some(idx);
            let style = if selected {
                self.selected_style
            } else {
                self.style
            };
            if selected {
                for x in area.x..area.right() {
                    buf.set_cell(x, y, Cell::new(' ', style));
                }
            }
            buf.set_str(area.x, y, self.items[idx], style);
        }
    }
}

/// A one-row left/center/right segmented bar (mode | filename | position).
/// Renders only the first row of `area`. On a narrow line later segments win:
/// left, then right, then center (so the centre stays visible).
pub struct StatusLine<'a> {
    left: Span<'a>,
    center: Span<'a>,
    right: Span<'a>,
    fill: Style,
}

impl<'a> StatusLine<'a> {
    pub fn new() -> Self {
        StatusLine {
            left: Span::default(),
            center: Span::default(),
            right: Span::default(),
            fill: Style::new(),
        }
    }

    pub fn left(mut self, span: Span<'a>) -> Self {
        self.left = span;
        self
    }

    pub fn center(mut self, span: Span<'a>) -> Self {
        self.center = span;
        self
    }

    pub fn right(mut self, span: Span<'a>) -> Self {
        self.right = span;
        self
    }

    /// The bar background under the segments.
    pub fn fill(mut self, style: Style) -> Self {
        self.fill = style;
        self
    }
}

impl Default for StatusLine<'_> {
    fn default() -> Self {
        StatusLine::new()
    }
}

impl Widget for StatusLine<'_> {
    fn render(self, area: Rect, buf: &mut Buffer) {
        if area.is_empty() {
            return;
        }
        let y = area.y;
        for x in area.x..area.right() {
            buf.set_cell(x, y, Cell::new(' ', self.fill));
        }
        let right_w = self.right.text.chars().count() as u16;
        let center_w = self.center.text.chars().count() as u16;
        let rx = area.right().saturating_sub(right_w).max(area.x);
        let cx = area.x + area.width.saturating_sub(center_w) / 2;
        place_span(buf, area.x, y, area.right(), self.left, self.fill);
        place_span(buf, rx, y, area.right(), self.right, self.fill);
        place_span(buf, cx, y, area.right(), self.center, self.fill);
    }
}

/// Write `span` from `(x, y)` up to `max_x`, painting its fg/attr over the bar
/// `fill` background (a span may override the bg explicitly).
fn place_span(buf: &mut Buffer, x: u16, y: u16, max_x: u16, span: Span<'_>, fill: Style) {
    let mut cx = x;
    for ch in span.text.chars() {
        if cx >= max_x {
            break;
        }
        let bg = if matches!(span.style.bg, Color::Reset) {
            fill.bg
        } else {
            span.style.bg
        };
        let st = Style {
            fg: span.style.fg,
            bg,
            attr: span.style.attr,
        };
        buf.set_cell(cx, y, Cell::new(ch, st));
        cx = cx.saturating_add(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::style::Color;

    fn sym(b: &Buffer, x: u16, y: u16) -> char {
        b.get(x, y).unwrap().symbol
    }

    #[test]
    fn block_draws_border_and_title() {
        let mut b = Buffer::empty(Rect::new(0, 0, 6, 3));
        Block::new()
            .title("hi")
            .render(Rect::new(0, 0, 6, 3), &mut b);
        assert_eq!(sym(&b, 0, 0), '\u{250C}');
        assert_eq!(sym(&b, 5, 0), '\u{2510}');
        assert_eq!(sym(&b, 0, 2), '\u{2514}');
        assert_eq!(sym(&b, 5, 2), '\u{2518}');
        assert_eq!(sym(&b, 0, 1), '\u{2502}');
        // Title overwrites the top edge, inset past the corner.
        assert_eq!(sym(&b, 1, 0), 'h');
        assert_eq!(sym(&b, 2, 0), 'i');
        assert_eq!(
            Block::new().inner(Rect::new(0, 0, 6, 3)),
            Rect::new(1, 1, 4, 1)
        );
    }

    #[test]
    fn block_without_border_has_full_inner() {
        let b = Block::new().borders(false);
        assert_eq!(b.inner(Rect::new(2, 2, 5, 5)), Rect::new(2, 2, 5, 5));
    }

    #[test]
    fn block_too_small_for_border_skips_it() {
        // 1-wide area: no border drawn, inner == area (no negative shrink).
        let b = Block::new();
        assert_eq!(b.inner(Rect::new(0, 0, 1, 3)), Rect::new(0, 0, 1, 3));
    }

    #[test]
    fn paragraph_clips_without_wrap() {
        let mut b = Buffer::empty(Rect::new(0, 0, 5, 1));
        Paragraph::new("hello world").render(Rect::new(0, 0, 5, 1), &mut b);
        assert_eq!(sym(&b, 0, 0), 'h');
        assert_eq!(sym(&b, 4, 0), 'o'); // "hello" then clipped
    }

    #[test]
    fn paragraph_hard_wraps() {
        let mut b = Buffer::empty(Rect::new(0, 0, 2, 3));
        Paragraph::new("hello")
            .wrap(true)
            .render(Rect::new(0, 0, 2, 3), &mut b);
        assert_eq!((sym(&b, 0, 0), sym(&b, 1, 0)), ('h', 'e'));
        assert_eq!((sym(&b, 0, 1), sym(&b, 1, 1)), ('l', 'l'));
        assert_eq!(sym(&b, 0, 2), 'o');
    }

    #[test]
    fn paragraph_scrolls_logical_lines() {
        let mut b = Buffer::empty(Rect::new(0, 0, 1, 2));
        Paragraph::new("a\nb\nc")
            .scroll(1)
            .render(Rect::new(0, 0, 1, 2), &mut b);
        assert_eq!(sym(&b, 0, 0), 'b');
        assert_eq!(sym(&b, 0, 1), 'c');
    }

    #[test]
    fn paragraph_keeps_blank_lines() {
        let mut b = Buffer::empty(Rect::new(0, 0, 1, 3));
        Paragraph::new("x\n\ny").render(Rect::new(0, 0, 1, 3), &mut b);
        assert_eq!(sym(&b, 0, 0), 'x');
        assert_eq!(sym(&b, 0, 1), ' '); // the blank line still occupies a row
        assert_eq!(sym(&b, 0, 2), 'y');
    }

    #[test]
    fn list_highlights_the_selected_row() {
        let items = ["one", "two", "three"];
        let mut b = Buffer::empty(Rect::new(0, 0, 5, 3));
        let ember = Style::new().bg(Color::Rgb(224, 120, 64));
        List::new(&items)
            .select(Some(1))
            .selected_style(ember)
            .render(Rect::new(0, 0, 5, 3), &mut b);
        assert_eq!(sym(&b, 0, 1), 't');
        // The whole selected row carries the highlight bg, text cells included.
        assert_eq!(b.get(0, 1).unwrap().style.bg, Color::Rgb(224, 120, 64));
        assert_eq!(b.get(4, 1).unwrap().style.bg, Color::Rgb(224, 120, 64));
        // A non-selected row keeps the default bg.
        assert_eq!(b.get(0, 0).unwrap().style.bg, Color::Reset);
    }

    #[test]
    fn list_offset_scrolls() {
        let items = ["one", "two", "three"];
        let mut b = Buffer::empty(Rect::new(0, 0, 5, 2));
        List::new(&items)
            .offset(1)
            .render(Rect::new(0, 0, 5, 2), &mut b);
        assert_eq!(sym(&b, 0, 0), 't'); // "two"
        assert_eq!(sym(&b, 0, 1), 't'); // "three"
        assert_eq!(sym(&b, 4, 1), 'e'); // "three" fills the 5-wide row
    }

    #[test]
    fn statusline_places_three_segments() {
        let mut b = Buffer::empty(Rect::new(0, 0, 7, 1));
        StatusLine::new()
            .left(Span::raw("L"))
            .center(Span::raw("C"))
            .right(Span::raw("R"))
            .render(Rect::new(0, 0, 7, 1), &mut b);
        assert_eq!(sym(&b, 0, 0), 'L');
        assert_eq!(sym(&b, 3, 0), 'C'); // (7-1)/2 = 3
        assert_eq!(sym(&b, 6, 0), 'R'); // flush right
        assert_eq!(sym(&b, 1, 0), ' '); // gap is the fill
    }

    #[test]
    fn statusline_paints_the_fill_background() {
        let mut b = Buffer::empty(Rect::new(0, 0, 4, 1));
        let bar = Style::new().bg(Color::Rgb(40, 36, 32));
        StatusLine::new()
            .fill(bar)
            .render(Rect::new(0, 0, 4, 1), &mut b);
        assert_eq!(b.get(0, 0).unwrap().style.bg, Color::Rgb(40, 36, 32));
        assert_eq!(b.get(3, 0).unwrap().style.bg, Color::Rgb(40, 36, 32));
    }

    #[test]
    fn span_constructors() {
        let s = Span::styled("x", Style::new().fg(Color::Rgb(1, 2, 3)));
        assert_eq!(s.text, "x");
        assert_eq!(s.style.fg, Color::Rgb(1, 2, 3));
        assert_eq!(Span::raw("y").style, Style::new());
    }
}
