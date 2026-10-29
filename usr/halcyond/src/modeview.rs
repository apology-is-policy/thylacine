// Presentation only: application reports and keyboard ownership are resolved
// by the caller. An unknown application is never inferred to be in Insert.
use libhalcyon::theme::{Argb, Theme};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum DisplayMode {
    Insert,
    Normal,
    Visual,
    Command,
    #[default]
    Application,
}

impl DisplayMode {
    pub const ALL: [Self; 5] = [Self::Insert, Self::Normal, Self::Visual, Self::Command, Self::Application];

    pub fn label(self) -> &'static str {
        match self {
            Self::Insert => "INS",
            Self::Normal => "NOR",
            Self::Visual => "VIS",
            Self::Command => "CMD",
            Self::Application => "APP",
        }
    }

    pub fn transcript(inspecting: bool, selecting: bool) -> Self {
        match (inspecting, selecting) {
            (true, true) => Self::Visual,
            (true, false) => Self::Normal,
            _ => Self::Application,
        }
    }

    /// Nora's exported semantic roles, as opaque fills with contrasting ink.
    pub fn colors(self, theme: &Theme) -> (Argb, Argb) {
        let bg = match self {
            Self::Insert => theme.syntax.moss,
            Self::Normal => theme.ember,
            Self::Visual => theme.syntax.dusk,
            Self::Command => theme.syntax.sand,
            Self::Application => theme.fg_muted,
        } | 0xff00_0000;
        let brightness = 299 * ((bg >> 16) & 255)
            + 587 * ((bg >> 8) & 255)
            + 114 * (bg & 255);
        let ink = if brightness >= 128_000 { 0xff00_0000 } else { 0xffff_ffff };
        (bg, ink)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn transcript_does_not_guess_an_application_mode() {
        assert_eq!(DisplayMode::transcript(false, false), DisplayMode::Application);
        assert_eq!(DisplayMode::transcript(false, true), DisplayMode::Application);
        assert_eq!(DisplayMode::transcript(true, false), DisplayMode::Normal);
        assert_eq!(DisplayMode::transcript(true, true), DisplayMode::Visual);
    }
    #[test]
    fn chip_colors_follow_the_same_roles_as_nora() {
        let t = libhalcyon::theme::DAYLIGHT;
        for (mode, role) in [
            (DisplayMode::Insert, t.syntax.moss),
            (DisplayMode::Normal, t.ember),
            (DisplayMode::Visual, t.syntax.dusk),
            (DisplayMode::Command, t.syntax.sand),
            (DisplayMode::Application, t.fg_muted),
        ] {
            let (bg, ink) = mode.colors(&t);
            assert_eq!(bg, role | 0xff00_0000);
            assert!(ink == 0xff00_0000 || ink == 0xffff_ffff);
            assert_eq!(mode.label().len(), 3);
        }
    }

    #[test]
    fn mode_switches_keep_layout_and_paint_the_full_accent() {
        use crate::{layout, raster::GlyphSource, status::{self, StatusModel}};
        use libhalcyon::instrument::{Bundle, Profile};
        for profile in [Profile::Legacy, Profile::Instrument] {
            for scale in [100, 200] {
                for width in [800, 1280] {
                    let sheet = layout::sheet_for(&Bundle::builtin(profile), scale, width);
                    let height = status::bar_height(&sheet);
                    let mut gs = GlyphSource::new_vendored(64);
                    gs.set_scale(scale);
                    let mut fixed = None;
                    let mut images = alloc::vec::Vec::new();
                    for mode in DisplayMode::ALL {
                        let model = StatusModel { mode, ..StatusModel::empty() };
                        let (cart, slots) = status::status_list(&model, width, height, &sheet, &mut gs);
                        let geometry = (slots.mode, slots.clock, slots.cond, slots.ctx);
                        if let Some(previous) = fixed { assert_eq!(geometry, previous); }
                        fixed = Some(geometry);
                        assert!(slots.mode.0 >= 0 && slots.mode.1 > 0);
                        assert!(slots.mode.0 + slots.mode.1 <= width as i32);
                        let (fill, ink) = mode.colors(&sheet.theme);
                        let chip = cart.ops.iter().find_map(|op| match *op {
                            cartoon::Op::Rect { x, y, w, h, color }
                                if x == slots.mode.0 && w as i32 == slots.mode.1 && color == fill => Some((x, y, w, h)),
                            _ => None,
                        }).expect("an opaque filled chip at the reserved slot");
                        assert!(chip.1 >= 0 && chip.1 + chip.3 as i32 <= height as i32);
                        assert!(cart.ops.iter().any(|op| matches!(op,
                            cartoon::Op::Glyphs { color, count: 3, .. } if *color == ink)));
                        let mut px = alloc::vec![0u32; (width * height) as usize];
                        cartoon::execute(&cart, &gs.packer.store, &cartoon::BlobStore::new(), &mut px, width as usize, None);
                        assert_eq!(px[(chip.1 as u32 * width + chip.0 as u32) as usize], fill);
                        assert!(!images.iter().any(|prior| prior == &px), "each mode has a distinct rendered label/fill");
                        images.push(px);
                    }
                }
            }
        }
    }


    #[test]
    fn long_context_cannot_push_the_mode_off_a_supported_display() {
        use crate::{layout, raster::GlyphSource, status::{self, StatusModel}};
        use libhalcyon::instrument::{Bundle, Profile};
        for profile in [Profile::Legacy, Profile::Instrument] {
            let sheet = layout::sheet_for(&Bundle::builtin(profile), 200, 800);
            let mut gs = GlyphSource::new_vendored(64);
            gs.set_scale(200);
            let mut model = StatusModel::empty();
            model.mode = DisplayMode::Visual;
            model.host = Some("long-host-".repeat(20));
            model.notice = Some(("long notice ".repeat(20), true));
            let (_, slots) = status::status_list(&model, 800, status::bar_height(&sheet), &sheet, &mut gs);
            assert!(slots.mode.0 > 0 && slots.mode.0 + slots.mode.1 < 800);
        }
    }

}
