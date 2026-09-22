//! lantern -- the Beacon deck presenter, pure half (docs/LANTERN-DESIGN.md).
//!
//! A deck is a DIRECTORY holding a `slides.toml` manifest and the Markdown
//! slides it names, in order. A slide is an Operator's-Manual section: the
//! `manual` crate's format (MANUAL-DESIGN.md 3), its checker and its Beacon
//! realization, used unchanged. The tree therefore carries one Markdown
//! dialect, not a second one private to slides -- two dialects is how a
//! format rots, and the manual's subset already accepts everything a textual
//! slide needs (a title, headings, lists, tables, code fences, emphasis).
//!
//! What lantern adds is only what a deck needs beyond a document: the ORDER,
//! the key -> action map, the clear that puts one slide on screen at a time,
//! and the output cooking that clear implies. No I/O lives here; the binary
//! supplies the files, the tier and the keystrokes.
//!
//! A manifest carries CONTENT and ORDER, never display authority. It cannot
//! set the scale, the theme or the font: those belong to the compositor and
//! reach it through its own gated verbs, so a deck file someone mails you
//! cannot reach for them.

#![no_std]

extern crate alloc;

pub mod deck;
pub mod nav;

/// What lantern writes before each slide: reset the pen, home the cursor,
/// erase the display.
///
/// In a Halcyon tile this is a GRID operation and nothing more -- kaua-term's
/// VT digests it, `vt::Screen::erase_display(2)` blanks every cell in place
/// (no scroll-off, so the transcript does not accumulate the slides already
/// shown) and every blanked cell carries `span: 0`, so the Beacon span tags of
/// the previous slide go with its text. The tile stays in `ScreenMode::Normal`,
/// which is the mode that lays the document out richly. On a serial console it
/// is an ordinary clear. Down a pipe it is never written at all (`Show::Cat`).
///
/// It is deliberately NOT the alt-screen: entering that is what flips a tile
/// to painting its raw mono grid, which would discard the rich rendering the
/// whole facility exists to get.
pub const CLEAR: &[u8] = b"\x1b[0m\x1b[H\x1b[2J";

/// Write `chunk` with every LF cooked to CR-LF.
///
/// The raw-mode dance ut runs for a full-screen child sets `-onlcr` (the
/// kernel stops translating output line endings) on the argument that such a
/// child owns every byte it emits. lantern IS such a child -- it needs `-isig`
/// so a keystroke is a keystroke -- but it emits a DOCUMENT, whose lines end
/// in a bare LF. So it does the translation the discipline stopped doing,
/// which is the honest reading of owning your own bytes.
///
/// Stateless per byte, so a chunk boundary anywhere gives the same result as
/// one whole write -- `manual::render` streams in `CHUNK`-sized pieces and
/// splits wherever it must. Safe across Beacon frames because a frame never
/// carries an LF: `manual`'s checker rejects a control character in section
/// text and its renderer sanitizes every value it did not produce, so LF
/// appears only BETWEEN frames, as the document's own line separator. The
/// `lf_never_appears_inside_a_frame` test pins that rather than trusting it.
pub fn cook(chunk: &[u8], out: &mut dyn FnMut(&[u8])) {
    let mut start = 0;
    for (i, &b) in chunk.iter().enumerate() {
        if b == b'\n' {
            out(&chunk[start..i]);
            out(b"\r\n");
            start = i + 1;
        }
    }
    if start < chunk.len() {
        out(&chunk[start..]);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec::Vec;

    fn cooked(s: &[u8]) -> Vec<u8> {
        let mut v = Vec::new();
        cook(s, &mut |c| v.extend_from_slice(c));
        v
    }

    #[test]
    fn cook_translates_every_lf_and_nothing_else() {
        assert_eq!(cooked(b"a\nb"), b"a\r\nb");
        assert_eq!(cooked(b"\n"), b"\r\n");
        assert_eq!(cooked(b"\n\n"), b"\r\n\r\n");
        assert_eq!(cooked(b""), b"");
        assert_eq!(cooked(b"no newline"), b"no newline");
        // An LF that already follows a CR is still cooked -- the input never
        // carries CR (the checker rejects it), so this cannot double up in
        // practice, and asserting the simple rule keeps the function pure.
        assert_eq!(cooked(b"a\r\nb"), b"a\r\r\nb");
    }

    #[test]
    fn cook_is_chunk_boundary_independent() {
        let src = b"one\ntwo\nthree\n\nfour";
        let whole = cooked(src);
        for split in 0..=src.len() {
            let mut v = Vec::new();
            cook(&src[..split], &mut |c| v.extend_from_slice(c));
            cook(&src[split..], &mut |c| v.extend_from_slice(c));
            assert_eq!(v, whole, "split at {}", split);
        }
    }

    /// The claim `cook` rests on: a Beacon frame never carries an LF, so a
    /// blanket LF -> CR-LF translation cannot corrupt one. Rendered at the
    /// rich tier from a section exercising every construct the format has, and
    /// checked by walking the OSC state -- not by trusting the renderer.
    #[test]
    fn lf_never_appears_inside_a_frame() {
        let src = "\
# A slide's title

A paragraph with *emphasis*, **strong**, and a `code span`.

## A heading

- a bulleted item
- another item

1. a numbered item
2. another

| Column | Other |
| --- | ---: |
| cell | 42 |

```
a code block line
and another
```
";
        // The section must pass its own checker first, or this proves nothing
        // about a real slide.
        let problems = manual::format::check(None, src, &mut |_, _| {});
        assert_eq!(problems, 0, "the fixture must be a valid section");

        let mut out: Vec<u8> = Vec::new();
        manual::render::render(src, beacon::Tier::Rich, None, &mut |c| {
            out.extend_from_slice(c)
        });
        assert!(
            out.windows(2).any(|w| w == b"\x1b]"),
            "the fixture must actually emit frames at the rich tier"
        );
        assert!(out.contains(&b'\n'), "and must actually emit line endings");

        // Walk the stream: an OSC opens at ESC ] and closes at ST (ESC \) or
        // BEL. No LF may fall between.
        let mut in_osc = false;
        let mut i = 0;
        while i < out.len() {
            let b = out[i];
            if !in_osc && b == 0x1b && out.get(i + 1) == Some(&b']') {
                in_osc = true;
                i += 2;
                continue;
            }
            if in_osc {
                if b == 0x07 || (b == 0x1b && out.get(i + 1) == Some(&b'\\')) {
                    in_osc = false;
                    i += if b == 0x07 { 1 } else { 2 };
                    continue;
                }
                assert_ne!(b, b'\n', "an LF inside a frame at byte {}", i);
            }
            i += 1;
        }
        assert!(!in_osc, "the stream ends inside a frame");
    }
}
