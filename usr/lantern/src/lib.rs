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

/// Hide the caret while presenting, and show it again on the way out.
///
/// A blinking bar under the last line is a desk affordance; on a projected
/// slide it is a distraction with nothing to mark.
///
/// It works in a Halcyon tile, and the seam that carries it is pinned by
/// `halcyond::tile::tests::dectcem_travels_the_whole_seam_to_the_caret_predicate`:
/// `vt` records DEC private `?25` into `cursor_visible` (distinct from SGR 25,
/// which is blink-off -- the `?` is what carries the meaning), kaua-term's
/// Producer emits a cursor record for a visibility-only change, the wire
/// round-trips the flag, and `Tile::paints_caret` reads it. Nothing lantern
/// emits resets it: only RIS (`ESC c`) sets `cursor_visible` back to true.
///
/// This comment previously recorded the opposite as MEASURED. It was not: the
/// caret was seen in a capture from a build that did not yet emit the escape,
/// and that observation was carried forward instead of re-taken. The test above
/// exists because a screenshot cannot settle this question in either direction
/// -- the caret BLINKS, so a frame showing none may be a frame caught mid-step.
///
/// `SHOW_CARET` is emitted on every exit path, and is idempotent with ut's
/// post-reap `RESTORE_SCREEN`, which re-emits the same show-cursor escape -- so a
/// crash that skips lantern's own cleanup is covered by the backstop.
pub const HIDE_CARET: &[u8] = b"\x1b[?25l";
pub const SHOW_CARET: &[u8] = b"\x1b[?25h";

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

    /// The tokens `tools/interactive/lantern.exp` matches, pinned against the
    /// SHIPPED slides rather than against a fixture.
    ///
    /// A gate token the renderer splits across a line break turns a real pass
    /// into a TIMEOUT, which reads as a hang rather than as a regression -- the
    /// most expensive way for a gate to fail. So the tokens are asserted here,
    /// where a break is a failing test with a name. Both postures are covered
    /// because they differ in exactly the way that matters: down a pipe there is
    /// no width and nothing wraps, while a presented deck has a console width
    /// and its prose IS wrapped. The gate uses body prose only in the first and
    /// headings and footers in the second, and this is what makes that split
    /// safe instead of merely intended.
    #[test]
    fn slide_tokens_render_contiguously() {
        const SLIDES: [(&str, &str); 3] = [
            (include_str!("../deck/01-title.md"), "Beacon slides"),
            (include_str!("../deck/02-how.md"), "How it works"),
            (include_str!("../deck/03-keys.md"), "Keys"),
        ];

        // The shipped manifest names exactly these three, in this order -- which
        // is what makes the gate's "3 / 3" footer token correct.
        let d = crate::deck::parse(include_str!("../deck/slides.toml")).expect("the manifest");
        assert_eq!(d.slides, ["01-title.md", "02-how.md", "03-keys.md"]);
        assert_eq!(d.slides.len(), SLIDES.len());
        assert_eq!(d.title.as_deref(), Some("Beacon slides"));

        let rendered = |src: &str, width: Option<usize>| -> alloc::string::String {
            let mut v: Vec<u8> = Vec::new();
            manual::render::render(src, beacon::Tier::None, width, &mut |c| {
                v.extend_from_slice(c)
            });
            alloc::string::String::from_utf8(v).expect("the plain tier is UTF-8")
        };

        for (src, heading) in SLIDES {
            assert_eq!(
                manual::format::check(None, src, &mut |_, _| {}),
                0,
                "a shipped slide must pass the section checker"
            );
            // A heading survives EVERY width, including one narrower than the
            // heading itself: the renderer does not wrap headings at all.
            // Measured, not assumed -- asserting it at a comfortable 40 columns
            // proved nothing, because 40 already exceeds every heading here, so
            // that assertion could not have failed for the reason it named.
            for width in [None, Some(40), Some(5)] {
                assert!(
                    rendered(src, width).contains(heading),
                    "heading {:?} split at width {:?}",
                    heading,
                    width
                );
            }
        }

        // The positive control the heading assertions need, one variable away: a
        // BODY token at the same narrow width IS broken up. Without it, every
        // "survives at width N" above is equally satisfied by a renderer that
        // ignores `width` entirely -- and then the gate's posture split would
        // rest on nothing.
        let body = "renderer already knows how to draw one";
        assert!(
            rendered(SLIDES[0].0, None).contains(body),
            "the control's own premise: the token is contiguous unwrapped"
        );
        assert!(
            !rendered(SLIDES[0].0, Some(5)).contains(body),
            "width is inert: a body token survived a 5-column wrap, so the \
             heading assertions above prove nothing about wrapping"
        );

        // The body tokens leg (b) matches -- the cat posture only, where the
        // absence of a width is what keeps them contiguous.
        for (src, token) in [
            (SLIDES[0].0, "renderer already knows how to draw one"),
            (SLIDES[1].0, "raw character grid"),
            (SLIDES[2].0, "keeping the slide on screen"),
        ] {
            assert!(
                rendered(src, None).contains(token),
                "body token {:?} is not contiguous down a pipe",
                token
            );
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
