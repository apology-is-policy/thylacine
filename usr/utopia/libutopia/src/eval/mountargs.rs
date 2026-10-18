// The argument grammar of the namespace built-ins, `mount` and `bind`. Pure --
// no syscalls, no Env -- so it builds without the `backend` feature and its
// tests run on the host; the built-ins that drive the kernel are in `builtin`.

use alloc::string::{String, ToString};

// The kernel's mount flags (territory.h). `builtin` asserts each equals its
// libthyla-rs constant, which this module cannot name without the backend.
pub const MREPL: u32 = 0x0001;
pub const MBEFORE: u32 = 0x0002;
pub const MAFTER: u32 = 0x0004;
pub const MCREATE: u32 = 0x0008;

/// What `mount`'s argument grammar resolved to.
#[derive(Debug, PartialEq, Eq)]
pub struct MountPlan {
    /// Bitmask of MREPL / MBEFORE / MAFTER / MCREATE.
    pub flags: u32,
    /// The service to attach, e.g. `/srv/ptyfs`.
    pub service: String,
    /// Where it lands in THIS shell's namespace.
    pub mountpoint: String,
    /// 9P attach name; `None` means the server's default root.
    pub aname: Option<String>,
}

/// What `bind`'s argument grammar resolved to.
#[derive(Debug, PartialEq, Eq)]
pub struct BindPlan {
    /// Bitmask of MREPL / MBEFORE / MAFTER / MCREATE.
    pub flags: u32,
    /// The file bound (Plan 9's `new`).
    pub new: String,
    /// The name it is bound over (Plan 9's `old`).
    pub old: String,
}

/// Plan 9's flags, shared by `mount` and `bind`: `-b` before, `-a` after, `-c`
/// permits creation in the union. `-b`/`-a` are the ORDERING (mutually
/// exclusive, default REPL); `-c` is orthogonal and ORs in. Returns the flags
/// and the index of the first operand.
fn parse_flags(cmd: &str, args: &[String]) -> Result<(u32, usize), String> {
    let mut flags: u32 = 0;
    let mut order: Option<char> = None;
    let mut i = 0usize;

    while i < args.len() {
        let a = args[i].as_str();
        // A lone "-" is an operand, not a flag; anything not starting with '-'
        // ends the option run. No `opts_done` latch: every path that would set
        // one breaks out of the loop in the same step, so the flag would be
        // written and never read (the compiler said so, and it was right --
        // vestigial structure copied from a parser that genuinely needed it).
        if !a.starts_with('-') || a.len() == 1 {
            break;
        }
        if a == "--" {
            i += 1;
            break;
        }
        // Clustered short flags, as `mount -bc` is the Plan 9 spelling.
        for c in a[1..].chars() {
            match c {
                // A repeated flag is the same request; only the other ordering
                // conflicts.
                'b' | 'a' => {
                    if order.is_some_and(|o| o != c) {
                        return Err(alloc::format!("{}: -b and -a are mutually exclusive", cmd));
                    }
                    order = Some(c);
                    flags |= if c == 'b' { MBEFORE } else { MAFTER };
                }
                'c' => flags |= MCREATE,
                _ => return Err(alloc::format!("{}: unknown option -{}", cmd, c)),
            }
        }
        i += 1;
    }
    if order.is_none() {
        flags |= MREPL; // the default, as in Plan 9
    }
    Ok((flags, i))
}

/// Parse `mount`'s arguments: `[-b|-a] [-c] service mountpoint [aname]`.
pub fn parse_mount(args: &[String]) -> Result<MountPlan, String> {
    let (flags, i) = parse_flags("mount", args)?;
    let rest = &args[i..];
    if rest.len() < 2 || rest.len() > 3 {
        return Err("usage: mount [-b|-a] [-c] service mountpoint [aname]".to_string());
    }
    Ok(MountPlan {
        flags,
        service: rest[0].clone(),
        mountpoint: rest[1].clone(),
        aname: rest.get(2).cloned(),
    })
}

/// Parse `bind`'s arguments: `[-b|-a] [-c] new old`, Plan 9's order -- the
/// file first, then the name it covers.
pub fn parse_bind(args: &[String]) -> Result<BindPlan, String> {
    let (flags, i) = parse_flags("bind", args)?;
    let rest = &args[i..];
    if rest.len() != 2 {
        return Err("usage: bind [-b|-a] [-c] new old".to_string());
    }
    Ok(BindPlan { flags, new: rest[0].clone(), old: rest[1].clone() })
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec::Vec;

    fn v(args: &[&str]) -> Vec<String> {
        args.iter().map(|s| s.to_string()).collect()
    }

    fn bind_flags(args: &[&str]) -> u32 {
        parse_bind(&v(args)).expect("parses").flags
    }

    #[test]
    fn bind_defaults_to_replace() {
        let p = parse_bind(&v(&["/tmp/x", "/n/x"])).unwrap();
        assert_eq!(p, BindPlan { flags: MREPL, new: "/tmp/x".into(), old: "/n/x".into() });
    }

    #[test]
    fn bind_ordering_and_create() {
        assert_eq!(bind_flags(&["-b", "a", "b"]), MBEFORE);
        assert_eq!(bind_flags(&["-a", "a", "b"]), MAFTER);
        assert_eq!(bind_flags(&["-c", "a", "b"]), MREPL | MCREATE);
        assert_eq!(bind_flags(&["-bc", "a", "b"]), MBEFORE | MCREATE);
        assert_eq!(bind_flags(&["-cb", "a", "b"]), MBEFORE | MCREATE);
        assert_eq!(bind_flags(&["-a", "-c", "a", "b"]), MAFTER | MCREATE);
        assert_eq!(bind_flags(&["-bb", "-b", "a", "b"]), MBEFORE);
    }

    #[test]
    fn bind_refuses_both_orderings() {
        for args in [&["-ab", "a", "b"][..], &["-b", "-a", "a", "b"][..], &["-bcb", "-a", "a", "b"][..]] {
            let e = parse_bind(&v(args)).unwrap_err();
            assert_eq!(e, "bind: -b and -a are mutually exclusive", "{:?}", args);
        }
    }

    #[test]
    fn bind_names_an_unknown_option() {
        assert_eq!(parse_bind(&v(&["-r", "a", "b"])).unwrap_err(), "bind: unknown option -r");
        assert_eq!(parse_bind(&v(&["-bq", "a", "b"])).unwrap_err(), "bind: unknown option -q");
    }

    #[test]
    fn bind_needs_exactly_two_operands() {
        let usage = "usage: bind [-b|-a] [-c] new old";
        for args in [&[][..], &["a"][..], &["-b", "a"][..], &["a", "b", "c"][..]] {
            assert_eq!(parse_bind(&v(args)).unwrap_err(), usage, "{:?}", args);
        }
    }

    #[test]
    fn options_end_at_double_dash_and_lone_dash() {
        let p = parse_bind(&v(&["-b", "--", "-c", "old"])).unwrap();
        assert_eq!((p.flags, p.new.as_str(), p.old.as_str()), (MBEFORE, "-c", "old"));
        let p = parse_bind(&v(&["-", "old"])).unwrap();
        assert_eq!((p.flags, p.new.as_str()), (MREPL, "-"));
        // The first operand ends the option run: a later "-b" is an operand.
        assert_eq!(parse_bind(&v(&["new", "-b"])).unwrap().old, "-b");
    }

    #[test]
    fn mount_takes_two_or_three_operands() {
        let p = parse_mount(&v(&["-bc", "/srv/x", "/n/x", "sub"])).unwrap();
        assert_eq!(
            p,
            MountPlan {
                flags: MBEFORE | MCREATE,
                service: "/srv/x".into(),
                mountpoint: "/n/x".into(),
                aname: Some("sub".into()),
            }
        );
        assert_eq!(parse_mount(&v(&["/srv/x", "/n/x"])).unwrap().aname, None);
        let usage = "usage: mount [-b|-a] [-c] service mountpoint [aname]";
        for args in [&["/srv/x"][..], &["a", "b", "c", "d"][..]] {
            assert_eq!(parse_mount(&v(args)).unwrap_err(), usage, "{:?}", args);
        }
        assert_eq!(parse_mount(&v(&["-ba", "a", "b"])).unwrap_err(),
                   "mount: -b and -a are mutually exclusive");
    }
}
