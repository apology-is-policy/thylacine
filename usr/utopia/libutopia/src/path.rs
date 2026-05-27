// libutopia::path -- path display helpers.
//
// Per UTOPIA-VISUAL.md section 3.1: the prompt path renders with
// HOME-relative abbreviation (`~` for HOME, `~/foo` for HOME/foo).
// Programs that emit paths in the Pale Fire prompt use this helper
// rather than rolling their own.
//
// At U-3 (skeleton) the helpers are conservative -- exact-HOME and
// HOME-prefix only. Richer behaviour (bound-namespace abbreviation per
// UTOPIA-VISUAL.md section 3.1, width-budget truncation for the
// terminal-width-derived middle-ellipsis case) lands at U-4 (the line
// editor) when cursor accounting needs precise width.

use alloc::string::String;

/// Abbreviate `path` against `home`. Returns the original path if
/// `home` is empty or not a prefix.
///
/// Cases per UTOPIA-VISUAL.md section 3.1:
///
///   - `path == home`           -> `"~"`
///   - `path == home + "/..."`  -> `"~/..."`
///   - otherwise                -> `path` unchanged
///
/// Both inputs are byte-precise. UTF-8 is the convention but the
/// comparison is byte-wise, so whatever encoding the caller stored is
/// preserved end-to-end.
pub fn abbreviate_home(path: &str, home: &str) -> String {
    if home.is_empty() {
        return String::from(path);
    }
    if path == home {
        return String::from("~");
    }
    // Match home + '/' so a partial-component prefix
    // ("/home/joey" vs "/home/joey-other") doesn't false-positive.
    let mut prefix = String::from(home);
    if !prefix.ends_with('/') {
        prefix.push('/');
    }
    if let Some(rest) = path.strip_prefix(prefix.as_str()) {
        let mut out = String::from("~/");
        out.push_str(rest);
        out
    } else {
        String::from(path)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exact_home_yields_tilde() {
        assert_eq!(abbreviate_home("/home/joey", "/home/joey"), "~");
    }

    #[test]
    fn home_prefix_yields_tilde_relative() {
        assert_eq!(abbreviate_home("/home/joey/src/thylacine", "/home/joey"),
                   "~/src/thylacine");
    }

    #[test]
    fn non_home_path_unchanged() {
        assert_eq!(abbreviate_home("/etc/utopia/utopia.rc", "/home/joey"),
                   "/etc/utopia/utopia.rc");
    }

    #[test]
    fn empty_home_yields_unchanged() {
        assert_eq!(abbreviate_home("/etc/utopia/utopia.rc", ""),
                   "/etc/utopia/utopia.rc");
    }

    #[test]
    fn home_with_trailing_slash_handled() {
        assert_eq!(abbreviate_home("/home/joey/x", "/home/joey/"), "~/x");
    }

    #[test]
    fn partial_component_not_matched() {
        // "/home/joey-other" must NOT match the "/home/joey" home --
        // the helper rejects partial-component prefixes.
        assert_eq!(abbreviate_home("/home/joey-other/x", "/home/joey"),
                   "/home/joey-other/x");
    }
}
