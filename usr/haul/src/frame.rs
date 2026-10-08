// 9P framing for the pumps: the size prefix every message carries, and the
// msize the session negotiates, which bounds the reply direction. Pure and
// syscall-free, so the rules are tested here on the host rather than only
// exercised in a guest (lib.rs says why that needs the lib half).

use core::sync::atomic::{AtomicU32, Ordering};

/// The largest 9P message haul will relay in either direction.
///
/// A 9P message is `size[4]` INCLUDING those four bytes, so this bounds the
/// whole frame. It is haul's own ceiling, and the only bound it has before it
/// knows anything about the session: it holds for every frame going up, and
/// for replies until the Tversion has gone by (`ReplyBound` takes over then).
///
/// The number is chosen against what the kernel proposes. A direct mount
/// proposes `SYS_ATTACH_DEFAULT_MSIZE` (4 KiB, kernel/syscall.c) and a mount of
/// a posted service `SRVCONN_MSIZE` (32 KiB, kernel/include/thylacine/srvconn.h),
/// so 64 KiB is above both while keeping the worst case a hostile server can
/// drive to two 64 KiB buffers, one per pump.
///
/// It was 1 MiB, which was described as "far below a length a hostile peer could
/// use to make us allocate the heap". When the heap was a fixed 4 MiB, two of
/// those were a quarter of it, and on exhaustion the panic handler calls
/// `t_exits(1)` -- so a bound written to prevent a denial of service was set
/// where it could deliver one.
pub const MSG_MAX: u32 = 64 * 1024;

/// A 9P message is at minimum `size[4] type[1] tag[2]`. npxf's server refuses
/// anything shorter as a runt; matching it here means a malformed frame dies at
/// the transport instead of being handed to a parser on either side.
pub const MSG_MIN: u32 = 7;

// version(5): `size[4] type[1] tag[2] msize[4] version[s]`, both ways.
const TVERSION: u8 = 100;
const RVERSION: u8 = 101;

/// Decode the 9P `size[4]` little-endian prefix and validate it.
///
/// Returns the TOTAL frame length (header included), or None if the peer
/// claimed a size that cannot be a 9P message. Refusing here is what keeps a
/// bad length from becoming a giant allocation or an inward-handed lie.
pub fn frame_len(hdr: &[u8; 4]) -> Option<u32> {
    let size = u32::from_le_bytes(*hdr);
    if !(MSG_MIN..=MSG_MAX).contains(&size) {
        return None;
    }
    Some(size)
}

/// The msize field of a whole frame of type `ty`, or None for any other message
/// or for one too short to carry the field.
fn version_msize(frame: &[u8], ty: u8) -> Option<u32> {
    match frame {
        [_, _, _, _, t, _, _, a, b, c, d, ..] if *t == ty => {
            Some(u32::from_le_bytes([*a, *b, *c, *d]))
        }
        _ => None,
    }
}

/// The largest reply the kernel will take, as the session negotiates it.
///
/// The kernel caps its receive at the msize it proposed in its Tversion
/// (`recv_cap`, kernel/9p_client.c) and answers a larger reply by marking the
/// whole session dead, after which nothing reads the reply pipe and nothing
/// tells haul. The server's Rversion may only lower that msize, and the kernel
/// takes the lower value for what a reply may carry: an Rread or Rreaddir past
/// it kills the session the same way. Holding the server to the same number
/// here turns both into a refusal haul can report.
///
/// One field of the two version messages is all haul reads. That does not make
/// it a client: it is the transport learning the frame bound both ends have
/// agreed to obey.
pub struct ReplyBound(AtomicU32);

impl ReplyBound {
    pub const fn new() -> Self {
        ReplyBound(AtomicU32::new(MSG_MAX))
    }

    /// Every frame going up, BEFORE it is forwarded: a Tversion sets the bound
    /// to the msize it proposes, never above MSG_MAX.
    ///
    /// The order is what makes the bound hold for the Rversion itself. The
    /// server cannot answer a Tversion it has not received, and every hop
    /// between the two pumps -- the TCP stack, netd, the kernel's pipes -- is a
    /// lock or a device barrier, so a reply pump that asks after the reply has
    /// arrived sees this store. A second Tversion starts a new session
    /// (version(5)), so it replaces the bound rather than lowering it.
    pub fn up(&self, frame: &[u8]) {
        if let Some(msize) = version_msize(frame, TVERSION) {
            self.0.store(msize.min(MSG_MAX), Ordering::Release);
        }
    }

    /// Whether a reply of `len` bytes fits; the bound it broke, if not.
    ///
    /// Ask only once the reply has arrived. A pump that read the bound and
    /// then blocked for the reply would hold the value from before the
    /// Tversion went up -- MSG_MAX -- and wave through exactly the reply this
    /// exists to stop.
    pub fn fits(&self, len: usize) -> Result<(), u32> {
        let max = self.0.load(Ordering::Acquire);
        if len as u64 <= max as u64 {
            Ok(())
        } else {
            Err(max)
        }
    }

    /// Every reply, after `fits` passed it: an Rversion lowers the bound to the
    /// server's msize, and can never raise it -- the kernel negotiates down the
    /// same way (kernel/9p_session.c).
    pub fn down(&self, frame: &[u8]) {
        if let Some(msize) = version_msize(frame, RVERSION) {
            self.0.fetch_min(msize, Ordering::AcqRel);
        }
    }
}

impl Default for ReplyBound {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec::Vec;

    // A version message as the kernel's encoder lays it out: NOTAG, "9P2000.L".
    fn version(ty: u8, msize: u32) -> Vec<u8> {
        let v = b"9P2000.L";
        let size = (4 + 1 + 2 + 4 + 2 + v.len()) as u32;
        let mut m = Vec::new();
        m.extend_from_slice(&size.to_le_bytes());
        m.push(ty);
        m.extend_from_slice(&0xffffu16.to_le_bytes());
        m.extend_from_slice(&msize.to_le_bytes());
        m.extend_from_slice(&(v.len() as u16).to_le_bytes());
        m.extend_from_slice(v);
        m
    }

    #[test]
    fn frame_len_holds_the_runt_floor_and_the_ceiling() {
        assert_eq!(frame_len(&0u32.to_le_bytes()), None);
        assert_eq!(frame_len(&(MSG_MIN - 1).to_le_bytes()), None);
        assert_eq!(frame_len(&MSG_MIN.to_le_bytes()), Some(MSG_MIN));
        assert_eq!(frame_len(&MSG_MAX.to_le_bytes()), Some(MSG_MAX));
        assert_eq!(frame_len(&(MSG_MAX + 1).to_le_bytes()), None);
        assert_eq!(frame_len(&u32::MAX.to_le_bytes()), None);
    }

    #[test]
    fn the_kernels_tversion_sets_the_bound_to_its_proposal() {
        let b = ReplyBound::new();
        assert_eq!(b.fits(MSG_MAX as usize), Ok(()));
        assert_eq!(b.fits(MSG_MAX as usize + 1), Err(MSG_MAX));
        b.up(&version(TVERSION, 4096));
        assert_eq!(b.fits(4096), Ok(()));
        assert_eq!(b.fits(4097), Err(4096));
    }

    #[test]
    fn only_a_tversion_moves_the_bound_up() {
        let b = ReplyBound::new();
        // A Twalk (110), an Rversion going the wrong way, and a Tversion cut
        // off before its msize field: none of them is a proposal.
        let mut twalk = version(TVERSION, 4096);
        twalk[4] = 110;
        b.up(&twalk);
        b.up(&version(RVERSION, 4096));
        b.up(&version(TVERSION, 4096)[..10]);
        assert_eq!(b.fits(MSG_MAX as usize), Ok(()));
        // Eleven bytes is exactly enough to carry the field.
        b.up(&version(TVERSION, 4096)[..11]);
        assert_eq!(b.fits(4097), Err(4096));
    }

    #[test]
    fn a_proposal_above_the_ceiling_is_held_to_it() {
        let b = ReplyBound::new();
        b.up(&version(TVERSION, u32::MAX));
        assert_eq!(b.fits(MSG_MAX as usize), Ok(()));
        assert_eq!(b.fits(MSG_MAX as usize + 1), Err(MSG_MAX));
    }

    #[test]
    fn a_proposal_below_the_runt_floor_refuses_every_reply() {
        // The kernel never proposes one; if it did, it could not take even the
        // Rversion, and neither does haul.
        let b = ReplyBound::new();
        b.up(&version(TVERSION, 0));
        assert_eq!(b.fits(MSG_MIN as usize), Err(0));
    }

    #[test]
    fn the_servers_rversion_lowers_the_bound_and_never_raises_it() {
        let b = ReplyBound::new();
        b.up(&version(TVERSION, 32768));
        b.down(&version(RVERSION, 65536));
        assert_eq!(b.fits(32769), Err(32768));
        b.down(&version(RVERSION, 8192));
        assert_eq!(b.fits(8192), Ok(()));
        assert_eq!(b.fits(8193), Err(8192));
        // A Tversion seen going DOWN is not a server's msize.
        b.down(&version(TVERSION, 16));
        assert_eq!(b.fits(8192), Ok(()));
    }

    #[test]
    fn a_second_tversion_starts_over() {
        let b = ReplyBound::new();
        b.up(&version(TVERSION, 32768));
        b.down(&version(RVERSION, 8192));
        b.up(&version(TVERSION, 16384));
        assert_eq!(b.fits(16384), Ok(()));
        assert_eq!(b.fits(16385), Err(16384));
    }
}
