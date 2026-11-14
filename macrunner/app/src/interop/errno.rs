// A macOS errno as the Linux value 9P2000.L carries in Rlerror (4.4: the two numberings differ;
// ENOTEMPTY is 66 on macOS and 39 on Linux).

/// The Linux errno for a macOS one. A value with no Linux counterpart reads EIO.
pub fn linux(mac: i32) -> u32 {
    match mac {
        // EPERM through ERANGE share their values, except 11: EDEADLK on macOS, EAGAIN on Linux.
        1..=10 | 12..=34 => mac as u32,
        libc::EDEADLK => 35,
        libc::EAGAIN => 11,
        libc::EINPROGRESS => 115,
        libc::EALREADY => 114,
        libc::ENOTSOCK => 88,
        libc::EMSGSIZE => 90,
        libc::ENOTSUP | libc::EOPNOTSUPP => 95,
        libc::EADDRINUSE => 98,
        libc::ENETDOWN => 100,
        libc::ENETUNREACH => 101,
        libc::ECONNABORTED => 103,
        libc::ECONNRESET => 104,
        libc::ENOBUFS => 105,
        libc::ENOTCONN => 107,
        libc::ETIMEDOUT => 110,
        libc::ECONNREFUSED => 111,
        libc::ELOOP => 40,
        libc::ENAMETOOLONG => 36,
        libc::EHOSTDOWN => 112,
        libc::EHOSTUNREACH => 113,
        libc::ENOTEMPTY => 39,
        libc::EUSERS => 87,
        libc::EDQUOT => 122,
        libc::ESTALE => 116,
        libc::EREMOTE => 66,
        libc::ENOLCK => 37,
        libc::ENOSYS => 38,
        libc::EOVERFLOW => 75,
        libc::ECANCELED => 125,
        libc::EILSEQ => 84,
        // A missing extended attribute, and macOS's own ENODATA: Linux's ENODATA.
        libc::ENOATTR | libc::ENODATA => 61,
        libc::EBADMSG => 74,
        libc::EPROTO => 71,
        // A file of a type the call does not take (macOS's own errno): Linux has EINVAL for it.
        libc::EFTYPE => 22,
        _ => 5,
    }
}

/// The calling thread's errno, as Linux's value.
pub fn last() -> u32 {
    linux(
        std::io::Error::last_os_error()
            .raw_os_error()
            .unwrap_or(libc::EIO),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_values_that_differ_are_translated() {
        assert_eq!(linux(libc::ENOTEMPTY), 39);
        assert_eq!(linux(libc::ELOOP), 40);
        assert_eq!(linux(libc::ENAMETOOLONG), 36);
        assert_eq!(linux(libc::EAGAIN), 11);
        assert_eq!(linux(libc::EDEADLK), 35);
        assert_eq!(linux(libc::EDQUOT), 122);
        assert_eq!(linux(libc::ENOTSUP), 95);
        assert_eq!(linux(libc::EOPNOTSUPP), 95);
    }

    #[test]
    fn the_values_that_agree_pass_through() {
        for (mac, want) in [
            (libc::EPERM, 1),
            (libc::ENOENT, 2),
            (libc::EIO, 5),
            (libc::EBADF, 9),
            (libc::EACCES, 13),
            (libc::EEXIST, 17),
            (libc::EXDEV, 18),
            (libc::ENOTDIR, 20),
            (libc::EISDIR, 21),
            (libc::EINVAL, 22),
            (libc::EFBIG, 27),
            (libc::ENOSPC, 28),
            (libc::EROFS, 30),
            (libc::EMLINK, 31),
        ] {
            assert_eq!(linux(mac), want, "macOS {mac}");
        }
    }

    #[test]
    fn a_value_with_no_linux_counterpart_reads_eio() {
        assert_eq!(linux(libc::EAUTH), 5);
        assert_eq!(linux(0), 5);
        assert_eq!(linux(-1), 5);
        assert_eq!(linux(10_000), 5);
    }
}
