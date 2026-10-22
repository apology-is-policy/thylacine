// stamp -- the time operands `touch` takes (POSIX -t and -d), as seconds and
// nanoseconds since the Unix epoch.
//
// There is no timezone database, so the "local time" POSIX names is UTC, the
// zone date(1) prints. A time before the epoch is refused, as is one past the
// kernel's signed 64-bit seconds.

/// A point in time: whole seconds since 1970-01-01T00:00:00Z plus nanoseconds.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct Stamp {
    pub secs: u64,
    pub nsecs: u32,
}

/// Days from 1970-01-01 to the proleptic Gregorian date `y-m-d` (Howard
/// Hinnant's days_from_civil, exact across the whole range).
pub fn days_from_civil(y: i64, m: u32, d: u32) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = (if y >= 0 { y } else { y - 399 }) / 400;
    let yoe = y - era * 400; // [0, 399]
    let mp = i64::from((m + 9) % 12); // March = 0
    let doy = (153 * mp + 2) / 5 + i64::from(d) - 1; // [0, 365]
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; // [0, 146096]
    era * 146097 + doe - 719468
}

/// The UTC calendar year containing `secs` (civil_from_days, year only).
pub fn year_of(secs: u64) -> i64 {
    let z = (secs / 86400) as i64 + 719468;
    let era = z / 146097; // z >= 0 for any u64 seconds that fit
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    yoe + era * 400 + if m <= 2 { 1 } else { 0 }
}

fn is_leap(y: i64) -> bool {
    (y % 4 == 0 && y % 100 != 0) || y % 400 == 0
}

fn days_in_month(y: i64, m: u32) -> u32 {
    match m {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        _ if is_leap(y) => 29,
        _ => 28,
    }
}

// Validate a broken-down UTC time and convert it. `ss` may be 60: POSIX lets a
// leap second name the second after :59.
fn to_stamp(y: i64, mo: u32, d: u32, h: u32, mi: u32, ss: u32, nsecs: u32) -> Result<Stamp, &'static str> {
    if !(1..=12).contains(&mo) || d == 0 || d > days_in_month(y, mo) {
        return Err("invalid date");
    }
    if h > 23 || mi > 59 || ss > 60 {
        return Err("invalid time of day");
    }
    let days = days_from_civil(y, mo, d);
    let secs = days
        .checked_mul(86400)
        .and_then(|s| s.checked_add(i64::from(h * 3600 + mi * 60 + ss)))
        .ok_or("time out of range")?;
    if secs < 0 {
        return Err("time before the epoch");
    }
    Ok(Stamp { secs: secs as u64, nsecs })
}

// `n` ASCII digits starting at `s[at]`, as a number.
fn digits(s: &[u8], at: usize, n: usize) -> Option<u32> {
    let run = s.get(at..at + n)?;
    let mut v: u32 = 0;
    for &c in run {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v * 10 + u32::from(c - b'0');
    }
    Some(v)
}

/// POSIX `touch -t [[CC]YY]MMDDhhmm[.SS]`. A missing century takes 19 for YY
/// 69..99 and 20 for 00..68; a missing year is `now_year`.
pub fn parse_t(s: &str, now_year: i64) -> Result<Stamp, &'static str> {
    const BAD: &str = "invalid date format (want [[CC]YY]MMDDhhmm[.SS])";
    let b = s.as_bytes();
    let (main, ss) = match s.find('.') {
        Some(dot) => {
            if b.len() != dot + 3 {
                return Err(BAD);
            }
            (&b[..dot], digits(b, dot + 1, 2).ok_or(BAD)?)
        }
        None => (b, 0),
    };
    let year_len = match main.len() {
        8 => 0,
        10 => 2,
        12 => 4,
        _ => return Err(BAD),
    };
    let y = match year_len {
        0 => now_year,
        2 => {
            let yy = i64::from(digits(main, 0, 2).ok_or(BAD)?);
            if yy >= 69 { 1900 + yy } else { 2000 + yy }
        }
        _ => i64::from(digits(main, 0, 4).ok_or(BAD)?),
    };
    let r = &main[year_len..];
    let mo = digits(r, 0, 2).ok_or(BAD)?;
    let d = digits(r, 2, 2).ok_or(BAD)?;
    let h = digits(r, 4, 2).ok_or(BAD)?;
    let mi = digits(r, 6, 2).ok_or(BAD)?;
    to_stamp(y, mo, d, h, mi, ss, 0)
}

/// POSIX `touch -d YYYY-MM-DDThh:mm:SS[.frac][Z]`: the `T` may be a space, the
/// decimal mark a period or a comma, and the year longer than four digits. A
/// fraction past nanoseconds is truncated.
pub fn parse_d(s: &str) -> Result<Stamp, &'static str> {
    const BAD: &str = "invalid date format (want YYYY-MM-DDThh:mm:SS[.frac][Z])";
    let b = s.as_bytes();
    let ylen = b.iter().take_while(|c| c.is_ascii_digit()).count();
    if !(4..=9).contains(&ylen) {
        return Err(BAD);
    }
    let y = i64::from(digits(b, 0, ylen).ok_or(BAD)?);
    let r = &b[ylen..];
    // -MM-DDThh:mm:SS is 15 bytes at fixed offsets.
    if r.len() < 15
        || r[0] != b'-'
        || r[3] != b'-'
        || !(r[6] == b'T' || r[6] == b' ')
        || r[9] != b':'
        || r[12] != b':'
    {
        return Err(BAD);
    }
    let mo = digits(r, 1, 2).ok_or(BAD)?;
    let d = digits(r, 4, 2).ok_or(BAD)?;
    let h = digits(r, 7, 2).ok_or(BAD)?;
    let mi = digits(r, 10, 2).ok_or(BAD)?;
    let ss = digits(r, 13, 2).ok_or(BAD)?;
    let mut rest = &r[15..];
    let mut nsecs: u32 = 0;
    if let Some((&mark, frac)) = rest.split_first() {
        if mark == b'.' || mark == b',' {
            let n = frac.iter().take_while(|c| c.is_ascii_digit()).count();
            if n == 0 {
                return Err(BAD);
            }
            let mut scale = 100_000_000;
            for &c in &frac[..n.min(9)] {
                nsecs += u32::from(c - b'0') * scale;
                scale /= 10;
            }
            rest = &frac[n..];
        }
    }
    match rest {
        b"" | b"Z" => to_stamp(y, mo, d, h, mi, ss, nsecs),
        _ => Err(BAD),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn civil_round_trips_known_days() {
        assert_eq!(days_from_civil(1970, 1, 1), 0);
        assert_eq!(days_from_civil(2000, 3, 1), 11017);
        assert_eq!(days_from_civil(1969, 12, 31), -1);
        // 2026-10-22 = 20748 days.
        assert_eq!(days_from_civil(2026, 10, 22), 20748);
        for &(y, m, d) in &[(1970, 1, 1), (2000, 2, 29), (2024, 12, 31), (2100, 3, 1)] {
            let secs = (days_from_civil(y, m, d) * 86400) as u64;
            assert_eq!(year_of(secs), y);
            assert_eq!(year_of(secs + 86399), y);
        }
    }

    #[test]
    fn t_forms() {
        let want = Stamp { secs: 1_761_134_400, nsecs: 0 }; // 2025-10-22T12:00:00Z
        assert_eq!(parse_t("202510221200", 1999), Ok(want));
        assert_eq!(parse_t("2510221200", 1999), Ok(want));
        assert_eq!(parse_t("10221200", 2025), Ok(want));
        assert_eq!(parse_t("202510221200.30", 0), Ok(Stamp { secs: want.secs + 30, nsecs: 0 }));
        // A leap second names the next second.
        assert_eq!(parse_t("202510221200.60", 0), Ok(Stamp { secs: want.secs + 60, nsecs: 0 }));
        // The two-digit year pivot.
        assert_eq!(parse_t("6901010000", 0), Err("time before the epoch"));
        assert_eq!(parse_t("7001010000", 0), Ok(Stamp { secs: 0, nsecs: 0 }));
        assert_eq!(parse_t("6801010000", 0).map(|s| year_of(s.secs)), Ok(2068));
    }

    #[test]
    fn t_refusals() {
        for bad in [
            "", "1022120", "102212000", "20251022120", "2025102212000", "202510221200.", "202510221200.3",
            "202510221200.300", "2025102212x0", "202513221200", "202502291200", "202510321200", "202510222400",
            "202510221260", "202510221200.61", "196912312359", "202510001200", "210002291200",
        ] {
            assert!(parse_t(bad, 2025).is_err(), "accepted {:?}", bad);
        }
        assert!(parse_t("202402291200", 0).is_ok()); // a leap day
        assert!(parse_t("200002291200", 0).is_ok()); // a leap century
    }

    #[test]
    fn d_forms() {
        let base = 1_761_134_400;
        assert_eq!(parse_d("2025-10-22T12:00:00"), Ok(Stamp { secs: base, nsecs: 0 }));
        assert_eq!(parse_d("2025-10-22 12:00:00Z"), Ok(Stamp { secs: base, nsecs: 0 }));
        assert_eq!(parse_d("2025-10-22T12:00:01.5"), Ok(Stamp { secs: base + 1, nsecs: 500_000_000 }));
        assert_eq!(parse_d("2025-10-22T12:00:01,123456789Z"), Ok(Stamp { secs: base + 1, nsecs: 123_456_789 }));
        // Past nanoseconds truncates, never rounds into the next second.
        assert_eq!(parse_d("2025-10-22T12:00:01.9999999999"), Ok(Stamp { secs: base + 1, nsecs: 999_999_999 }));
        assert_eq!(parse_d("1970-01-01T00:00:00Z"), Ok(Stamp { secs: 0, nsecs: 0 }));
        assert_eq!(parse_d("10000-01-01T00:00:00").map(|s| year_of(s.secs)), Ok(10000));
    }

    #[test]
    fn d_refusals() {
        for bad in [
            "", "2025-10-22", "2025-10-22T12:00", "2025-10-22T12:00:00.", "2025-10-22T12:00:00.5x",
            "2025-10-22T12:00:00+01:00", "2025-10-22t12:00:00", "2025/10/22T12:00:00", "025-10-22T12:00:00",
            "2025-1-22T12:00:00", "2025-02-30T00:00:00", "2025-10-22T25:00:00", "1969-12-31T23:59:59Z",
            "2025-10-22T12:00:00ZZ", "2025-10-00T00:00:00", "2025-00-10T00:00:00",
        ] {
            assert!(parse_d(bad).is_err(), "accepted {:?}", bad);
        }
    }
}
