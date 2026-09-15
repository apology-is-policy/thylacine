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
    /// A dash-leading word sits where the command belongs. Ambiguous by
    /// construction -- an option written late, or a command that really is named
    /// with a dash -- so haul refuses rather than picking one silently.
    DashAfterOperands,
}

/// Parse `argv[1..]` (the caller drops argv[0]).
///
/// Option parsing stops as soon as ADDR AND MOUNTPOINT are in hand -- those are
/// the only two positionals haul owns, and the third word is already the child's
/// argv[0]. `--` ends it earlier, for a command whose own name starts with a
/// dash.
///
/// The boundary was at three, which looked equivalent and is not: with three,
/// the command WORD itself is still read as an option, so
/// `haul -t /real h!1 /m -t /attacker` matches `-t` at the command position,
/// swallows `/attacker` as haul's token path, OVERRIDES the operator's `-t`, and
/// leaves `cmd` empty -- haul silently parks instead of running anything. Two is
/// the count that makes "everything after the operands is the child's" true of
/// the command name as well as its arguments.
///
/// BUT MOVING THE BOUNDARY ONLY MOVED THE SILENCE, and that is why the refusal
/// below exists. At two, `haul h!1 /m -t /cfg.token` -- an operator writing the
/// flag after the operands, which every getopt-shaped tool accepts -- makes `-t`
/// the COMMAND: the token is silently dropped, haul mounts IN THE CLEAR, prints
/// PLAIN 9P, and finally fails to exec `-t`. Neither boundary can be right for
/// both argvs, because both are guessing at intent. So a dash-leading word at
/// the command position is refused outright and the operator is pointed at `--`,
/// which says which one they meant. Loud on both, silent on neither.
pub fn plan(argv: &[&str]) -> Result<Plan, Bad> {
    let mut aname = String::from("/");
    let mut token: Option<TokenSource> = None;
    let mut verbose = false;
    let mut positional: Vec<String> = Vec::new();
    let mut opts_done = false;

    let mut i = 0usize;
    while i < argv.len() {
        let a = argv[i];
        if opts_done || positional.len() >= 2 {
            // The command word itself may not start with a dash unless `--`
            // said so. A lone "-" is a conventional stdin operand, not a flag.
            if !opts_done && positional.len() == 2 && a.starts_with('-') && a.len() > 1 {
                // `--` HERE is the operator reaching for the exact escape hatch
                // the refusal points them at, so refusing it would make the
                // error message a lie. Consume it and take the rest verbatim.
                if a == "--" {
                    opts_done = true;
                    i += 1;
                    continue;
                }
                return Err(Bad::DashAfterOperands);
            }
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

    /// THE ROUND-2 FINDING, AS AMENDED BY ROUND 3 -- and the amendment is the
    /// interesting part. Round 2 caught that with the boundary at three
    /// positionals, `-t` here silently replaced the operator's token AND emptied
    /// `cmd`. Moving the boundary to two fixed that argv and broke its mirror:
    /// `haul h!1 /m -t /cfg.token`, an option written late, then silently became
    /// a COMMAND -- token dropped, mount in the clear.
    ///
    /// Both argvs are the same shape, so no boundary can read both correctly;
    /// each choice only picks which one fails silently. Refusing serves both,
    /// and `--` lets the operator say which they meant.
    #[test]
    fn a_dash_word_at_the_command_position_is_refused_not_guessed() {
        assert_eq!(
            plan(&["-t", "/real", "h!1", "/m", "-t", "/attacker"]),
            Err(Bad::DashAfterOperands)
        );
        assert_eq!(plan(&["h!1", "/m", "-t", "/cfg.token"]), Err(Bad::DashAfterOperands));
    }

    /// The escape hatch the refusal points at has to actually work for a command
    /// that spells one of haul's own options -- otherwise the refusal is a wall.
    #[test]
    fn a_double_dash_admits_a_command_named_like_an_option() {
        let p = ok(&["-t", "/real", "--", "h!1", "/m", "-t", "/attacker"]);
        assert_eq!(p.token, Some(TokenSource::File(String::from("/real"))));
        assert_eq!(p.cmd, vec!["-t", "/attacker"]);
    }

    /// A lone "-" is a conventional operand (stdin), not a flag, so it stays a
    /// command name. Guards the `a.len() > 1` half of the refusal.
    #[test]
    fn a_lone_dash_is_a_command_not_an_option() {
        let p = ok(&["h!1", "/m", "-"]);
        assert_eq!(p.cmd, vec!["-"]);
    }

    /// SELF-FOUND while auditing the refusal above, before it ever ran: `--`
    /// written AFTER the operands is the operator reaching for the exact escape
    /// hatch the refusal's message points them at, and the first draft refused
    /// it -- which would have made the error message a lie. It is consumed, not
    /// pushed, so it never reaches the child.
    #[test]
    fn a_double_dash_after_the_operands_is_consumed_not_refused() {
        let p = ok(&["h!1", "/m", "--", "-weird", "-t", "x"]);
        assert_eq!(p.addr, "h!1");
        assert_eq!(p.mountpoint, "/m");
        assert_eq!(p.cmd, vec!["-weird", "-t", "x"]);
    }

    /// The refusal is anchored at the command POSITION, not "any dash after the
    /// operands" -- the child's own flags must still reach it untouched.
    #[test]
    fn the_childs_own_flags_still_reach_it() {
        let p = ok(&["h!1", "/m", "/bin/echo", "-n", "-t", "x"]);
        assert_eq!(p.cmd, vec!["/bin/echo", "-n", "-t", "x"]);
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
