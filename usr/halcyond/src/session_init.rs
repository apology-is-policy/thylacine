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
