//! haul's argument grammar, as a pure function.
//!
//! Split out of `main.rs` so it can be TESTED. The grammar has one rule that is
//! easy to get wrong and impossible to notice at a glance -- where haul's own
//! options stop and the child command's begin -- and getting it wrong lets an
//! argument written for the child reconfigure haul's security posture. A rule
//! like that should not live only in a binary whose tests nothing runs.
//!
//! Resolving a token SOURCE into token BYTES needs syscalls, so it stays in
//! `main.rs`; this decides only which source was named.

use alloc::string::String;
use alloc::vec::Vec;

/// Where the token is to be read from. Resolved by the caller.
#[derive(Debug, PartialEq, Eq)]
pub enum TokenSource {
    File(String),
    Env(String),
}

#[derive(Debug, PartialEq, Eq)]
pub struct Plan {
    pub addr: String,
    pub mountpoint: String,
    pub aname: String,
    pub token: Option<TokenSource>,
    pub verbose: bool,
    /// The command to run with the mount visible, argv[0] first. Empty = park.
    pub cmd: Vec<String>,
}

#[derive(Debug, PartialEq, Eq)]
pub enum Bad {
    /// `-h`, or too few operands: the caller prints usage.
    WantsUsage,
    MissingValue(&'static str),
    UnknownOption,
}

/// Parse `argv[1..]` (the caller drops argv[0]).
///
/// Option parsing stops as soon as the command word is in hand -- addr,
/// mountpoint, command are the three positionals haul owns, and everything
/// after the third belongs to the child. `--` ends it earlier, for a command
/// whose own name starts with a dash.
pub fn plan(argv: &[&str]) -> Result<Plan, Bad> {
    let mut aname = String::from("/");
    let mut token: Option<TokenSource> = None;
    let mut verbose = false;
    let mut positional: Vec<String> = Vec::new();
    let mut opts_done = false;

    let mut i = 0usize;
    while i < argv.len() {
        let a = argv[i];
        if opts_done || positional.len() >= 3 {
            positional.push(String::from(a));
            i += 1;
            continue;
        }
        match a {
            "--" => opts_done = true,
            "-a" => {
                i += 1;
                match argv.get(i) {
                    Some(v) => aname = String::from(*v),
                    None => return Err(Bad::MissingValue("-a wants a tree name")),
                }
            }
            "-t" | "--token-file" => {
                i += 1;
                match argv.get(i) {
                    Some(v) => token = Some(TokenSource::File(String::from(*v))),
                    None => return Err(Bad::MissingValue("-t wants a file")),
                }
            }
            "--token-env" => {
                i += 1;
                match argv.get(i) {
                    Some(v) => token = Some(TokenSource::Env(String::from(*v))),
                    None => return Err(Bad::MissingValue("--token-env wants a variable name")),
                }
            }
            "-v" | "--verbose" => verbose = true,
            "-h" | "--help" => return Err(Bad::WantsUsage),
            _ if a.starts_with('-') && a.len() > 1 => return Err(Bad::UnknownOption),
            _ => positional.push(String::from(a)),
        }
        i += 1;
    }

    if positional.len() < 2 {
        return Err(Bad::WantsUsage);
    }
    Ok(Plan {
        addr: positional[0].clone(),
        mountpoint: positional[1].clone(),
        aname,
        token,
        verbose,
        cmd: positional[2..].to_vec(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    fn ok(argv: &[&str]) -> Plan {
        plan(argv).expect("should parse")
    }

    #[test]
    fn the_two_operand_form_parks() {
        let p = ok(&["10.0.2.2!5640", "/n/host"]);
        assert_eq!(p.addr, "10.0.2.2!5640");
        assert_eq!(p.mountpoint, "/n/host");
        assert_eq!(p.aname, "/");
        assert_eq!(p.token, None);
        assert!(!p.verbose);
        assert!(p.cmd.is_empty());
    }

    #[test]
    fn options_precede_the_operands() {
        let p = ok(&["-v", "-a", "tree", "-t", "/k", "h!1", "/m"]);
        assert!(p.verbose);
        assert_eq!(p.aname, "tree");
        assert_eq!(p.token, Some(TokenSource::File(String::from("/k"))));
        assert_eq!(p.addr, "h!1");
        assert_eq!(p.mountpoint, "/m");
    }

    /// THE FINDING. Everything after the command word is the child's argv, so a
    /// flag there must reach the child verbatim -- never haul's parser.
    #[test]
    fn the_childs_flags_are_not_hauls() {
        let p = ok(&["h!1", "/m", "/bin/ls", "-l", "/m"]);
        assert_eq!(p.cmd, vec!["/bin/ls", "-l", "/m"]);
        assert!(!p.verbose, "-l must not be read as anything of haul's");
    }

    /// The sharp end of the same finding: a child argument that HAPPENS to spell
    /// one of haul's options must not reconfigure haul. `-t` picks the token,
    /// so reading it here would let a command line aimed at the child choose the
    /// credential the mount authenticates with.
    #[test]
    fn a_childs_dash_t_does_not_choose_hauls_token() {
        let p = ok(&["-t", "/real", "h!1", "/m", "/bin/foo", "-t", "/attacker"]);
        assert_eq!(p.token, Some(TokenSource::File(String::from("/real"))));
        assert_eq!(p.cmd, vec!["/bin/foo", "-t", "/attacker"]);
    }

    /// And `-v`, which is the same bug with a quieter consequence: the child's
    /// flag would be swallowed rather than passed on.
    #[test]
    fn a_childs_dash_v_reaches_the_child() {
        let p = ok(&["h!1", "/m", "/bin/foo", "-v"]);
        assert!(!p.verbose);
        assert_eq!(p.cmd, vec!["/bin/foo", "-v"]);
    }

    #[test]
    fn a_double_dash_ends_options_early() {
        let p = ok(&["-v", "--", "h!1", "/m", "-weird-command", "-x"]);
        assert!(p.verbose);
        assert_eq!(p.addr, "h!1");
        assert_eq!(p.cmd, vec!["-weird-command", "-x"]);
    }

    #[test]
    fn an_unknown_option_before_the_operands_is_refused() {
        assert_eq!(plan(&["-z", "h!1", "/m"]), Err(Bad::UnknownOption));
    }

    #[test]
    fn a_missing_option_value_is_named() {
        assert_eq!(plan(&["h!1", "/m", "x", "-a"]).map(|p| p.cmd.len()), Ok(2));
        assert!(matches!(plan(&["-a"]), Err(Bad::MissingValue(_))));
        assert!(matches!(plan(&["-t"]), Err(Bad::MissingValue(_))));
        assert!(matches!(plan(&["--token-env"]), Err(Bad::MissingValue(_))));
    }

    #[test]
    fn too_few_operands_wants_usage() {
        assert_eq!(plan(&[]), Err(Bad::WantsUsage));
        assert_eq!(plan(&["h!1"]), Err(Bad::WantsUsage));
        assert_eq!(plan(&["-h"]), Err(Bad::WantsUsage));
    }

    /// A lone `-` is a conventional operand (stdin), not an option.
    #[test]
    fn a_bare_dash_is_an_operand() {
        let p = ok(&["h!1", "/m", "-"]);
        assert_eq!(p.cmd, vec!["-"]);
    }

    #[test]
    fn the_env_source_is_distinct_from_the_file_source() {
        let p = ok(&["--token-env", "NPXF_TOKEN", "h!1", "/m"]);
        assert_eq!(p.token, Some(TokenSource::Env(String::from("NPXF_TOKEN"))));
    }
}
