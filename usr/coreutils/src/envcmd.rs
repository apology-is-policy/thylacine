// envcmd -- the operands `env` takes: `[-i] [NAME=VALUE]... [COMMAND [ARG]...]`.
//
// POSIX's rules: options come first (`-i`, its obsolescent spelling `-`, and
// `--` to end them); every operand after them that contains `=` assigns the
// variable named by the bytes before the first `=`; the first operand without
// one is the command, and everything after it is the command's own.

use alloc::vec::Vec;

/// What one `env` invocation asks for.
#[derive(Debug, PartialEq, Eq)]
pub struct Invocation<'a> {
    /// `-i`: start from an empty environment.
    pub clear: bool,
    /// `NAME=VALUE` operands in order; a later one for the same name wins.
    pub assigns: Vec<(&'a [u8], &'a [u8])>,
    /// The command and its arguments; empty means print the environment.
    pub command: &'a [&'a [u8]],
}

#[derive(Debug, PartialEq, Eq)]
pub enum ParseError<'a> {
    /// `--help` before any operand.
    Help,
    /// An option `env` does not take.
    BadOption(&'a [u8]),
    /// A `NAME=VALUE` whose NAME is empty or holds a `/`, which `/env` cannot
    /// name.
    BadName(&'a [u8]),
}

pub fn parse<'a>(ops: &'a [&'a [u8]]) -> Result<Invocation<'a>, ParseError<'a>> {
    let mut clear = false;
    let mut i = 0;
    while i < ops.len() {
        let op = ops[i];
        match op {
            b"--" => {
                i += 1;
                break;
            }
            b"--help" => return Err(ParseError::Help),
            b"-" | b"-i" => clear = true,
            _ if op.len() > 1 && op[0] == b'-' && op.iter().skip(1).all(|&c| c == b'i') => {
                clear = true
            }
            _ if op.first() == Some(&b'-') => return Err(ParseError::BadOption(op)),
            _ => break,
        }
        i += 1;
    }
    let mut assigns = Vec::new();
    while i < ops.len() {
        let op = ops[i];
        let Some(eq) = op.iter().position(|&c| c == b'=') else { break };
        let name = &op[..eq];
        if name.is_empty() || name.contains(&b'/') {
            return Err(ParseError::BadName(op));
        }
        assigns.push((name, &op[eq + 1..]));
        i += 1;
    }
    Ok(Invocation { clear, assigns, command: &ops[i..] })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn p<'a>(ops: &'a [&'a [u8]]) -> Result<Invocation<'a>, ParseError<'a>> {
        parse(ops)
    }

    #[test]
    fn bare_prints() {
        let inv = p(&[]).unwrap();
        assert!(!inv.clear && inv.assigns.is_empty() && inv.command.is_empty());
    }

    #[test]
    fn assignments_then_command() {
        let ops: &[&[u8]] = &[b"A=1", b"B=x=y", b"C=", b"fc-list", b"D=2", b"-i"];
        let inv = p(ops).unwrap();
        assert!(!inv.clear);
        assert_eq!(inv.assigns, [(&b"A"[..], &b"1"[..]), (b"B", b"x=y"), (b"C", b"")]);
        assert_eq!(inv.command, &ops[3..]);
    }

    #[test]
    fn clear_forms() {
        for o in [&b"-i"[..], b"-", b"-ii"] {
            let ops: &[&[u8]] = &[o, b"A=1"];
            let inv = p(ops).unwrap();
            assert!(inv.clear, "{:?}", o);
            assert_eq!(inv.assigns.len(), 1);
            assert!(inv.command.is_empty());
        }
    }

    #[test]
    fn double_dash_ends_options() {
        let ops: &[&[u8]] = &[b"-i", b"--", b"-i"];
        let inv = p(ops).unwrap();
        assert!(inv.clear);
        assert_eq!(inv.command, &ops[2..]);
    }

    #[test]
    fn options_only_before_operands() {
        let ops: &[&[u8]] = &[b"A=1", b"-i"];
        let inv = p(ops).unwrap();
        assert!(!inv.clear);
        assert_eq!(inv.command, &ops[1..]);
    }

    #[test]
    fn refusals() {
        assert_eq!(p(&[b"-u", b"A"]), Err(ParseError::BadOption(b"-u")));
        assert_eq!(p(&[b"-ix"]), Err(ParseError::BadOption(b"-ix")));
        assert_eq!(p(&[b"--help"]), Err(ParseError::Help));
        assert_eq!(p(&[b"=v"]), Err(ParseError::BadName(b"=v")));
        assert_eq!(p(&[b"a/b=v"]), Err(ParseError::BadName(b"a/b=v")));
    }

    #[test]
    fn help_after_the_command_is_the_commands() {
        let ops: &[&[u8]] = &[b"ls", b"--help"];
        assert_eq!(p(ops).unwrap().command, ops);
    }
}
