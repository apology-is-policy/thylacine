//! Human-readable byte sizes (`ls -h`, future `df`). 1024-based, integer-only
//! (no float in no_std): one decimal below 10 of a unit (1.1K, 9.9K), none at
//! or above (12K, 3.0M -> 3M when whole). Bytes under 1024 print plain.

use alloc::format;
use alloc::string::String;

/// Format `bytes` as a short human string: plain under 1 KiB, else `<n>[.<d>]U`
/// with `U` in `K M G T P` (powers of 1024).
pub fn human(bytes: u64) -> String {
    const UNITS: [&str; 6] = ["", "K", "M", "G", "T", "P"];
    let mut whole = bytes;
    let mut rem = 0u64;
    let mut idx = 0;
    while whole >= 1024 && idx + 1 < UNITS.len() {
        rem = whole % 1024;
        whole /= 1024;
        idx += 1;
    }
    if idx == 0 {
        return format!("{}", whole); // plain bytes
    }
    if whole < 10 {
        // One decimal, rounded; a carry (9.96 -> 10) drops the decimal.
        let tenths = (rem * 10 + 512) / 1024;
        if tenths >= 10 {
            return format!("{}{}", whole + 1, UNITS[idx]);
        }
        format!("{}.{}{}", whole, tenths, UNITS[idx])
    } else {
        format!("{}{}", whole, UNITS[idx])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn under_1k_is_plain() {
        assert_eq!(human(0), "0");
        assert_eq!(human(512), "512");
        assert_eq!(human(1023), "1023");
    }

    #[test]
    fn kib_with_one_decimal_below_ten() {
        assert_eq!(human(1024), "1.0K");
        assert_eq!(human(1126), "1.1K"); // 1.0996.. -> 1.1
        assert_eq!(human(8424), "8.2K"); // 8.226.. -> 8.2
    }

    #[test]
    fn ten_or_more_units_drop_the_decimal() {
        assert_eq!(human(10 * 1024), "10K");
        assert_eq!(human(12 * 1024 + 100), "12K");
    }

    #[test]
    fn rounds_up_across_a_unit() {
        // 1048064 = 1023.5 KiB -> rounds the decimal to .5 within M? No: it is
        // still < 1 MiB, so it shows as KiB: 1023.5 -> "1023" whole >= 10 path.
        assert_eq!(human(1023 * 1024), "1023K");
        // Just over 1 MiB.
        assert_eq!(human(1024 * 1024), "1.0M");
        // A decimal carry: 9.97M rounds to 10M.
        assert_eq!(human(10 * 1024 * 1024 - 1), "10M");
    }
}
