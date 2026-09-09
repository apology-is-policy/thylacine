// Slice 1b lever (I-47, HALCYON.md 14.7 staging): the boot-time inline-image
// WITNESS. The pure half -- the bootarg token test + the witness raster --
// lives in the lib so it is host-tested; main (the bin) reads
// /hw/chosen/bootargs and calls Transcript::inject_image. Gated by the
// `thylacine.viewtest` bootarg (run-vm.sh THYLACINE_VIEWTEST=1) so a real-
// hardware boot PROVES the Item::Image render path (layout letterbox +
// cartoon Op::Image) through the GPU/scanout, before the out-of-band channel
// (slice 3) or the decoder (slice 2) exist. Retired when the `view` program
// lands the real path.

use alloc::vec::Vec;

/// Whole-word `thylacine.viewtest` present in the FDT bootargs (the
/// `libhalcyon::scale::declared_scale` discipline: a token at the start or
/// after a separator, ending at one -- `xthylacine.viewtest` and
/// `thylacine.viewtestx` are NOT it; the FDT property's trailing NUL is a
/// separator).
pub fn declared(bootargs: &[u8]) -> bool {
    const KEY: &[u8] = b"thylacine.viewtest";
    let is_sep = |b: u8| b == b' ' || b == b'\n' || b == b'\t' || b == 0;
    let mut i = 0;
    while i + KEY.len() <= bootargs.len() {
        let at_word = i == 0 || is_sep(bootargs[i - 1]);
        let ends = i + KEY.len() == bootargs.len() || is_sep(bootargs[i + KEY.len()]);
        if at_word && ends && &bootargs[i..i + KEY.len()] == KEY {
            return true;
        }
        i += 1;
    }
    false
}

#[inline]
fn argb(r: u8, g: u8, b: u8) -> u32 {
    0xFF00_0000 | ((r as u32) << 16) | ((g as u32) << 8) | b as u32
}

/// The witness raster (`w`, `h`, ARGB `w`-tight): a bordered card sized to sit
/// comfortably inside a typical console pane at NATIVE size (width-fit shows it
/// exact when it fits), centred with letterbox side bars. Three bands make the
/// blit legible on the real scanout: primary color bars (a clean palette), a
/// luminance gradient (smooth tone), and diagonal thylacine stripes (fine
/// detail). A 3px border makes the letterbox centring unmistakable. (The
/// resampler is exercised by the host layout test at a narrow width; a real
/// boot proves the native-size blit + present.)
pub fn raster() -> (u32, u32, Vec<u32>) {
    const W: u32 = 720;
    const H: u32 = 480;
    let (wi, hi) = (W as usize, H as usize);
    let mut px = alloc::vec![0u32; wi * hi];

    let bars = [
        argb(0xFF, 0xFF, 0xFF), // white
        argb(0xE6, 0xC8, 0x4B), // amber
        argb(0x4B, 0xC8, 0xE6), // cyan
        argb(0x5A, 0xC8, 0x5A), // green
        argb(0xC8, 0x5A, 0xC8), // magenta
        argb(0xD8, 0x4B, 0x4B), // red
        argb(0x4B, 0x5A, 0xD8), // blue
    ];
    let band = hi / 3;
    for y in 0..hi {
        for x in 0..wi {
            let c = if y < band {
                bars[(x * bars.len()) / wi]
            } else if y < 2 * band {
                let v = ((x * 255) / (wi - 1)) as u8;
                argb(v, v, v)
            } else if ((x + y) / 14) % 2 == 0 {
                argb(0x2A, 0x24, 0x1E) // thylacine dark
            } else {
                argb(0xC9, 0xA9, 0x76) // tan
            };
            px[y * wi + x] = c;
        }
    }
    let border = argb(0x1A, 0x16, 0x12);
    for y in 0..hi {
        for x in 0..wi {
            if x < 3 || x >= wi - 3 || y < 3 || y >= hi - 3 {
                px[y * wi + x] = border;
            }
        }
    }
    (W, H, px)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn declared_whole_word_only() {
        assert!(declared(b"thylacine.viewtest"));
        assert!(declared(b"a=b thylacine.viewtest c=d"));
        assert!(declared(b"thylacine.viewtest\0"), "the FDT NUL ends the token");
        assert!(declared(b"thylacine.nowatchpoint thylacine.viewtest\n"));
        assert!(!declared(b""));
        assert!(!declared(b"thylacine.scale=200"));
        assert!(!declared(b"xthylacine.viewtest"), "not a word boundary before");
        assert!(!declared(b"thylacine.viewtestx"), "not a word boundary after");
    }

    #[test]
    fn raster_is_wtight_opaque_and_bordered() {
        let (w, h, px) = raster();
        assert_eq!(px.len(), (w as usize) * (h as usize), "w-tight, h rows");
        assert!(px.iter().all(|&c| c >> 24 == 0xFF), "every pixel opaque");
        // The corners are the border (the letterbox-centring witness).
        let border = argb(0x1A, 0x16, 0x12);
        assert_eq!(px[0], border, "top-left border");
        assert_eq!(px[(w as usize) - 1], border, "top-right border");
    }
}
