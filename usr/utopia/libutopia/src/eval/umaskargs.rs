// The argument grammar of the `umask` built-in. Pure -- no syscalls, no Env --
// so it builds without the `backend` feature and its tests run on the host; the
// built-in that exchanges the kernel's mask is in `builtin`.

use alloc::string::{String, ToString};

/// The bits a creation mask can hold: the nine permission bits.
pub const MASK_BITS: u32 = 0o777;

/// Parse `umask`'s arguments: none reads the mask (`None`), one sets it. A mode
/// is one to four octal digits of value at most 0777; symbolic modes are not
/// taken.
pub fn parse_umask(args: &[String]) -> Result<Option<u32>, String> {
    match args {
        [] => Ok(None),
        [mode] => parse_mask(mode).map(Some),
        _ => Err("usage: umask [mode]".to_string()),
    }
}

fn parse_mask(mode: &str) -> Result<u32, String> {
    let bad = || alloc::format!("umask: bad mask {}", mode);
    if mode.is_empty() || mode.len() > 4 || !mode.bytes().all(|b| (b'0'..=b'7').contains(&b)) {
        return Err(bad());
    }
    let mask = mode.bytes().fold(0u32, |m, b| m * 8 + u32::from(b - b'0'));
    if mask > MASK_BITS {
        return Err(bad());
    }
    Ok(mask)
}

/// The mask as `umask` prints it: four octal digits.
pub fn format_mask(mask: u32) -> String {
    alloc::format!("{:04o}", mask)
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec::Vec;

    fn v(args: &[&str]) -> Vec<String> {
        args.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn no_operand_reads() {
        assert_eq!(parse_umask(&[]), Ok(None));
    }

    #[test]
    fn one_to_four_octal_digits_set() {
        for (arg, mask) in [
            ("0", 0),
            ("7", 0o7),
            ("22", 0o22),
            ("022", 0o22),
            ("0022", 0o22),
            ("0000", 0),
            ("777", 0o777),
            ("0777", 0o777),
        ] {
            assert_eq!(parse_umask(&v(&[arg])), Ok(Some(mask)), "{}", arg);
        }
    }

    #[test]
    fn a_bad_mask_is_named() {
        for arg in [
            "", "8", "08", "1000", "7777", "00022", "00777", "-1", "+7", "0x1f", " 22", "22 ",
            "u=rwx", "a-w", "-S", "--",
        ] {
            assert_eq!(
                parse_umask(&v(&[arg])),
                Err(alloc::format!("umask: bad mask {}", arg)),
                "{:?}",
                arg
            );
        }
    }

    #[test]
    fn two_operands_are_usage() {
        let usage = Err("usage: umask [mode]".to_string());
        assert_eq!(parse_umask(&v(&["022", "077"])), usage);
        assert_eq!(parse_umask(&v(&["-S", "022"])), usage);
    }

    #[test]
    fn prints_four_digits() {
        assert_eq!(format_mask(0o22), "0022");
        assert_eq!(format_mask(0), "0000");
        assert_eq!(format_mask(0o7), "0007");
        assert_eq!(format_mask(0o777), "0777");
    }

    #[test]
    fn every_mask_reads_back() {
        for m in 0..=MASK_BITS {
            assert_eq!(parse_umask(&v(&[&format_mask(m)])), Ok(Some(m)));
        }
    }
}
