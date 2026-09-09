//! The session's startup command (HALCYON.md 13.7, H-4c) -- rio's `-i initcmd`
//! idiom on the per-user compositor: once the first tile presents, the
//! compositor runs the user's `$home/lib/halcyon.rc` (a ut script, AS the
//! user, under the tile cap mask) if it exists; otherwise it restores the
//! image's `default` layout through the session tool (the first-launch
//! welcome, H-4d) if the image ships one; otherwise nothing. A user who wants
//! no welcome writes an rc -- an empty one will do. No marker state.
//!
//! Pure: what exists is injected, so the decision is host-tested.

use alloc::string::String;
use alloc::vec::Vec;

/// The rc's path under `$home`.
pub const RC_REL: &str = "/lib/halcyon.rc";
/// The device-tier layout the compositor restores when no rc exists.
pub const DEFAULT_LAYOUT: &str = "default";
/// Where that layout lives (HALCYON.md 13.7's device tier).
pub const DEVICE_DEFAULT_PATH: &str = "/lib/halcyon/layouts/default";

/// The argv (after argv[0]) for a tile's `kaua-term`: what this compositor
/// DECLARES to the terminal it spawns -- the render tier, the palette its
/// cells are born in, the geometry, then the hosted command.
///
/// Pure, and separate from the spawn, because a declaration that never
/// arrives is invisible: the tile just wears the wrong theme and nothing
/// fails. `kaua_term::cmdline::parse` is the other end, so the test below
/// drives what we BUILD through the parser the child actually RUNS.
pub fn tile_argv(
    tier: kaua_term::cmdline::Tier,
    palette: &vt::Palette,
    cols: u16,
    rows: u16,
    argv: &[String],
) -> Vec<String> {
    let mut out = Vec::new();
    out.push(String::from("--beacon"));
    out.push(String::from(tier.as_str()));
    out.push(String::from("--palette"));
    out.push(vt::palette_to_spec(palette));
    out.push(fmt_u16(cols));
    out.push(fmt_u16(rows));
    out.extend(argv.iter().cloned());
    out
}

fn fmt_u16(v: u16) -> String {
    use core::fmt::Write as _;
    let mut s = String::new();
    let _ = write!(s, "{}", v);
    s
}

/// The rc's full path for a session home (a trailing slash is trimmed).
pub fn rc_path(home: &str) -> String {
    let mut s = String::from(home.trim_end_matches('/'));
    s.push_str(RC_REL);
    s
}

/// What the compositor runs at session start.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Init {
    /// `ut --home <home> <rc>`.
    Rc { home: String, rc: String },
    /// `halcyon layout restore default`.
    DefaultLayout,
    /// Nothing: no rc, no device default.
    Nothing,
}

/// The decision. `rc_exists` is asked about the rc path (only when a home is
/// known -- a session without a home has no rc); `default_exists` is the
/// device default's presence.
pub fn decide(home: Option<&str>, rc_exists: impl Fn(&str) -> bool, default_exists: bool) -> Init {
    if let Some(h) = home {
        let rc = rc_path(h);
        if rc_exists(&rc) {
            return Init::Rc {
                home: String::from(h.trim_end_matches('/')),
                rc,
            };
        }
    }
    if default_exists {
        Init::DefaultLayout
    } else {
        Init::Nothing
    }
}

/// The argv for a decision, or None for nothing to run. The rc runs under
/// `ut --home <home> <script>`: flags first, then the script operand (ut's
/// D2 script mode), so `$home` is set for it exactly as for a tile's shell.
pub fn argv(init: &Init) -> Option<Vec<String>> {
    match init {
        Init::Rc { home, rc } => Some(alloc::vec![
            String::from("/bin/ut"),
            String::from("--home"),
            home.clone(),
            rc.clone(),
        ]),
        Init::DefaultLayout => Some(alloc::vec![
            String::from("/bin/halcyon"),
            String::from("layout"),
            String::from("restore"),
            String::from(DEFAULT_LAYOUT),
        ]),
        Init::Nothing => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use kaua_term::cmdline::{self, Tier};

    // THE SEAM, both ends, in one test: the args this compositor builds,
    // parsed by the parser its child runs. A unit test of either half alone
    // would pass with the two halves disagreeing -- which is the failure that
    // matters, because a tile wearing the wrong palette renders fine.
    #[test]
    fn the_args_we_build_declare_our_theme_to_the_parser_the_child_runs() {
        let pal = libhalcyon::theme::daylight_palette();
        let hosted = [String::from("/bin/ut")];
        let args = tile_argv(Tier::Rich, &pal, 100, 40, &hosted);
        let owned: Vec<&[u8]> = args.iter().map(|s| s.as_bytes()).collect();
        let parsed = cmdline::parse(&owned).expect("our own args must parse");
        assert_eq!(parsed.tier, Tier::Rich);
        assert_eq!(
            parsed.palette,
            Some(pal),
            "the child is born in OUR theme, not a constant of its own"
        );
        assert_eq!((parsed.cols, parsed.rows), (100, 40));
        assert_eq!(parsed.argv, hosted.to_vec());
    }

    // The sabotage the test above must catch: drop the declaration and the
    // child falls back to a palette that is not ours. Pinned so a future edit
    // that removes the flag cannot leave the suite green.
    #[test]
    fn an_undeclared_tile_would_not_wear_our_theme() {
        let pal = libhalcyon::theme::daylight_palette();
        let bare = [String::from("100"), String::from("40")];
        let owned: Vec<&[u8]> = bare.iter().map(|s| s.as_bytes()).collect();
        let parsed = cmdline::parse(&owned).unwrap();
        assert_eq!(parsed.palette, None);
        let fallback = vt::BONFIRE;
        assert_ne!(
            fallback, pal,
            "the fallback must be VISIBLY not our theme, so a plumbing break shows"
        );
    }

    // Every hosted argv survives verbatim, including one that looks like a
    // flag -- the positional parser stops taking flags at the dimensions, so
    // a program named `--palette` is a program, not a second declaration.
    #[test]
    fn a_hosted_argv_that_looks_like_a_flag_is_still_the_program() {
        let pal = libhalcyon::theme::daylight_palette();
        let hosted = [String::from("--palette"), String::from("-x")];
        let args = tile_argv(Tier::None, &pal, 8, 2, &hosted);
        let owned: Vec<&[u8]> = args.iter().map(|s| s.as_bytes()).collect();
        let parsed = cmdline::parse(&owned).unwrap();
        assert_eq!(parsed.palette, Some(pal), "ours, not the hosted one");
        assert_eq!(parsed.argv, hosted.to_vec());
    }

    #[test]
    fn the_rc_wins_over_the_device_default() {
        let d = decide(
            Some("/home/cora/"),
            |p| p == "/home/cora/lib/halcyon.rc",
            true,
        );
        assert_eq!(
            d,
            Init::Rc {
                home: String::from("/home/cora"),
                rc: String::from("/home/cora/lib/halcyon.rc")
            }
        );
        assert_eq!(
            argv(&d).unwrap(),
            [
                "/bin/ut",
                "--home",
                "/home/cora",
                "/home/cora/lib/halcyon.rc"
            ]
        );
    }

    #[test]
    fn no_rc_means_the_device_default_when_the_image_ships_one() {
        let d = decide(Some("/home/cora"), |_| false, true);
        assert_eq!(d, Init::DefaultLayout);
        assert_eq!(
            argv(&d).unwrap(),
            ["/bin/halcyon", "layout", "restore", "default"]
        );
    }

    #[test]
    fn nothing_when_neither_exists_or_no_home_and_no_default() {
        assert_eq!(decide(Some("/home/cora"), |_| false, false), Init::Nothing);
        assert_eq!(decide(None, |_| true, false), Init::Nothing);
        assert_eq!(decide(None, |_| true, true), Init::DefaultLayout);
        assert_eq!(argv(&Init::Nothing), None);
    }
}
