//! Connection capacity, not application authority. The native service adapter
//! supplies kernel peer identities and verifies live leaf ownership before
//! promotion. It must retire on disconnect/expiry, then release ONLY after the
//! readiness worker has relinquished the fd. No parsing, allocation or syscalls.
use libhalcyon::{interaction_body::MAX_CONTROLLERS, interaction_wire::MAX_RECORD};

pub const CONTROL_SLOTS: usize = MAX_CONTROLLERS as usize;
pub const MEDIA_SLOTS: usize = 2;
pub const HANDSHAKE_SLOTS: usize = 4;
pub const CONNECTION_SLOTS: usize = CONTROL_SLOTS + MEDIA_SLOTS + HANDSHAKE_SLOTS;
pub const HANDSHAKE_NS: u64 = 2_000_000_000;
pub const BUFFER_CEILING: usize = CONNECTION_SLOTS * 2 * MAX_RECORD;
pub const PAYLOAD_CEILING: usize = BUFFER_CEILING + crate::clipboard::PAYLOAD_CEILING;
/// Fixed native metadata and registered-buffer reserve, separate from both
/// protocol/clipboard payloads and the worker's explicit stack/guard mapping.
/// The native adapter asserts Conn <=8KiB, Link <=48KiB, Shared <=16KiB;
/// 38*8 +48 +16 =368KiB leaves144KiB for rings' user mappings and allocator slack.
pub const METADATA_RESERVE: usize = 512 * 1024;
pub const WORKING_RESERVE: usize = PAYLOAD_CEILING + METADATA_RESERVE;
const _: () = assert!(PAYLOAD_CEILING == 7 * 1024 * 1024 + 384 * 1024);

/// Millisecond poll deadline, rounded up so sub-ms deadlines do not busy-spin.
/// An absent deadline alone permits indefinite sleep; overdue work runs now.
pub fn poll_timeout(now: u64, deadline: Option<u64>) -> i32 {
    deadline.map_or(-1, |d| d.saturating_sub(now).div_ceil(1_000_000).min(i32::MAX as u64) as i32)
}

/// Monotone connection identity; never an fd or reusable array index.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Connection(u64);
impl Connection {
    pub fn id(self) -> u64 {
        self.0
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Full,
    PeerBusy,
    LeafBusy,
    Stale,
    Expired,
    Exhausted,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Class {
    Handshake { deadline: u64 },
    Control { leaf: u64 },
    Media,
}
#[derive(Clone, Copy)]
struct Entry {
    id: Connection,
    peer: u64,
    class: Class,
    retiring: bool,
}

/// Single service-owner fixed metadata. Retiring entries retain their class quota,
/// leaf exclusion and peer handshake exclusion until worker reclamation.
pub struct Pool {
    entries: [Option<Entry>; CONNECTION_SLOTS],
    next: u64,
}
impl Default for Pool {
    fn default() -> Self {
        Self::new()
    }
}
impl Pool {
    pub const fn new() -> Self {
        Self {
            entries: [None; CONNECTION_SLOTS],
            next: 1,
        }
    }

    /// `peer` is a kernel-observed process identity, never a request field.
    /// Expired entries still require explicit retirement/reclamation by owner.
    pub fn accept(&mut self, peer: u64, now: u64) -> Result<Connection, Error> {
        let mut count = 0;
        for e in self.entries.iter().flatten() {
            if matches!(e.class, Class::Handshake { .. }) {
                if e.peer == peer {
                    return Err(Error::PeerBusy);
                }
                count += 1;
            }
        }
        if count == HANDSHAKE_SLOTS {
            return Err(Error::Full);
        }
        let deadline = now.checked_add(HANDSHAKE_NS).ok_or(Error::Exhausted)?;
        let next = self.next.checked_add(1).ok_or(Error::Exhausted)?;
        let slot = self
            .entries
            .iter_mut()
            .find(|e| e.is_none())
            .ok_or(Error::Full)?;
        let id = Connection(self.next);
        *slot = Some(Entry {
            id,
            peer,
            class: Class::Handshake { deadline },
            retiring: false,
        });
        self.next = next;
        Ok(id)
    }

    fn index(&self, id: Connection) -> Result<usize, Error> {
        self.entries
            .iter()
            .position(|e| e.is_some_and(|e| e.id == id))
            .ok_or(Error::Stale)
    }

    fn handshake(&self, id: Connection, now: u64) -> Result<usize, Error> {
        let index = self.index(id)?;
        let e = self.entries[index].unwrap();
        if e.retiring {
            return Err(Error::Stale);
        }
        match e.class {
            Class::Handshake { deadline } if now < deadline => Ok(index),
            Class::Handshake { .. } => Err(Error::Expired),
            _ => Err(Error::Stale),
        }
    }

    /// Only after authenticated live leaf ownership has been verified. The leaf
    /// is the session's monotone pane identity, not a reusable layout slot.
    /// Success reserves capacity; it does NOT confer clipboard/focus authority.
    pub fn promote_control(&mut self, id: Connection, leaf: u64, now: u64) -> Result<(), Error> {
        let index = self.handshake(id, now)?;
        let mut count = 0;
        for e in self.entries.iter().flatten() {
            if let Class::Control { leaf: held } = e.class {
                if held == leaf {
                    return Err(Error::LeafBusy);
                }
                count += 1;
            }
        }
        if count == CONTROL_SLOTS {
            return Err(Error::Full);
        }
        self.entries[index].as_mut().unwrap().class = Class::Control { leaf };
        Ok(())
    }

    /// Only after existing route/principal validation. Media cannot borrow the
    /// controller reserve. Refusal leaves the original handshake unchanged.
    pub fn promote_media(&mut self, id: Connection, now: u64) -> Result<(), Error> {
        let index = self.handshake(id, now)?;
        if self
            .entries
            .iter()
            .flatten()
            .filter(|e| e.class == Class::Media)
            .count()
            == MEDIA_SLOTS
        {
            return Err(Error::Full);
        }
        self.entries[index].as_mut().unwrap().class = Class::Media;
        Ok(())
    }

    /// Next timer deadline; no periodic scan/wakeup is needed while idle.
    pub fn deadline(&self) -> Option<u64> {
        self.entries
            .iter()
            .flatten()
            .filter_map(|e| match e.class {
                Class::Handshake { deadline } if !e.retiring => Some(deadline),
                _ => None,
            })
            .min()
    }

    /// One expired connection at a time, bounded by four; caller retires its
    /// watch and closes after reclamation. No premature capacity publication.
    pub fn expire_one(&mut self, now: u64) -> Option<Connection> {
        let e = self.entries.iter_mut().flatten().find(|e| {
            !e.retiring && matches!(e.class, Class::Handshake { deadline } if now >= deadline)
        })?;
        e.retiring = true;
        Some(e.id)
    }

    pub fn retire(&mut self, id: Connection) -> Result<(), Error> {
        let index = self.index(id)?;
        self.entries[index].as_mut().unwrap().retiring = true;
        Ok(())
    }

    /// Owner asserts that watch reclamation/descriptor close has completed.
    /// Reject release of a live entry; stale completions cannot free a new peer.
    pub fn reclaimed(&mut self, id: Connection) -> Result<(), Error> {
        let index = self.index(id)?;
        if !self.entries[index].unwrap().retiring {
            return Err(Error::Stale);
        }
        self.entries[index] = None;
        Ok(())
    }
}
const _: () = assert!(core::mem::size_of::<Pool>() <= 4096);

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn deadline_wakes_without_descriptor_activity_and_does_not_spin() {
        assert_eq!(poll_timeout(10, None), -1);
        assert_eq!(poll_timeout(10, Some(10)), 0);
        assert_eq!(poll_timeout(10, Some(9)), 0);
        assert_eq!(poll_timeout(10, Some(11)), 1);
        assert_eq!(poll_timeout(0, Some(30_000_000_000)), 30_000);
        assert_eq!(poll_timeout(0, Some(u64::MAX)), i32::MAX);
    }
    #[test]
    fn all_reserves_coexist_and_refusals_preserve_handshake() {
        let mut p = Pool::new();
        for leaf in 1..=CONTROL_SLOTS as u64 {
            let id = p.accept(leaf, 0).unwrap();
            p.promote_control(id, leaf, 0).unwrap();
        }
        for peer in 100..102 {
            let id = p.accept(peer, 0).unwrap();
            p.promote_media(id, 0).unwrap();
        }
        let ids = [200, 201, 202, 203].map(|peer| p.accept(peer, 0).unwrap());
        assert_eq!(p.accept(204, 0), Err(Error::Full));
        assert_eq!(p.promote_media(ids[0], 0), Err(Error::Full));
        assert_eq!(p.promote_control(ids[0], 100, 0), Err(Error::Full));
        assert_eq!(p.deadline(), Some(HANDSHAKE_NS));
        assert_eq!(p.accept(200, 0), Err(Error::PeerBusy));
    }

    #[test]
    fn media_full_does_not_consume_control_reserve() {
        let mut p = Pool::new();
        for peer in 1..=2 {
            let id = p.accept(peer, 0).unwrap();
            p.promote_media(id, 0).unwrap();
        }
        let id = p.accept(3, 0).unwrap();
        assert_eq!(p.promote_media(id, 0), Err(Error::Full));
        p.promote_control(id, 77, 0).unwrap();
        assert_eq!(p.deadline(), None);
    }

    #[test]
    fn retiring_controller_reserves_leaf_and_stale_release_cannot_evict() {
        let mut p = Pool::new();
        let first = p.accept(1, 0).unwrap();
        p.promote_control(first, 9, 0).unwrap();
        assert_eq!(p.reclaimed(first), Err(Error::Stale));
        p.retire(first).unwrap();
        let second = p.accept(1, 0).unwrap();
        assert_eq!(p.promote_control(second, 9, 0), Err(Error::LeafBusy));
        p.reclaimed(first).unwrap();
        p.promote_control(second, 9, 0).unwrap();
        assert_eq!(p.reclaimed(first), Err(Error::Stale));
        assert_eq!(p.promote_media(first, 0), Err(Error::Stale));
        assert_ne!(first.id(), second.id());
        let third = p.accept(1, 0).unwrap();
        assert_eq!(p.promote_control(third, 9, 0), Err(Error::LeafBusy));
    }

    #[test]
    fn expiry_boundary_and_deferred_release() {
        let mut p = Pool::new();
        let ids = [1, 2, 3, 4].map(|peer| p.accept(peer, 7).unwrap());
        assert_eq!(p.expire_one(HANDSHAKE_NS + 6), None);
        assert_eq!(
            p.promote_control(ids[0], 8, HANDSHAKE_NS + 7),
            Err(Error::Expired)
        );
        for id in ids {
            assert_eq!(p.expire_one(HANDSHAKE_NS + 7), Some(id));
        }
        assert_eq!(p.deadline(), None);
        assert_eq!(p.expire_one(u64::MAX), None);
        assert_eq!(p.accept(5, 0), Err(Error::Full));
        assert_eq!(p.accept(1, 0), Err(Error::PeerBusy));
        assert_eq!(p.promote_media(ids[0], 0), Err(Error::Stale));
        p.reclaimed(ids[0]).unwrap();
        p.accept(1, 0).unwrap();
    }

    #[test]
    fn retiring_media_keeps_quota_and_promoted_connections_cannot_change_class() {
        let mut p = Pool::new();
        let a = p.accept(1, 0).unwrap();
        let b = p.accept(2, 0).unwrap();
        p.promote_media(a, 0).unwrap();
        p.promote_media(b, 0).unwrap();
        assert_eq!(p.promote_control(a, 8, 0), Err(Error::Stale));
        p.retire(a).unwrap();
        let c = p.accept(3, 0).unwrap();
        assert_eq!(p.promote_media(c, 0), Err(Error::Full));
        p.reclaimed(a).unwrap();
        p.promote_media(c, 0).unwrap();
        assert_eq!(p.deadline(), None);
    }

    #[test]
    fn promotion_releases_handshake_quota_for_same_peer() {
        let mut p = Pool::new();
        let a = p.accept(1, 10).unwrap();
        assert_eq!(p.accept(1, 11), Err(Error::PeerBusy));
        p.promote_control(a, 1, 12).unwrap();
        let b = p.accept(1, 13).unwrap();
        assert_eq!(p.deadline(), Some(13 + HANDSHAKE_NS));
        p.promote_control(b, 2, 14).unwrap();
        assert_eq!(p.deadline(), None);
    }

    #[test]
    fn monotone_ids_and_deadlines_never_wrap() {
        let mut p = Pool::new();
        assert_eq!(p.accept(1, u64::MAX), Err(Error::Exhausted));
        assert_eq!(p.next, 1);
        p.next = u64::MAX;
        assert_eq!(p.accept(1, 0), Err(Error::Exhausted));
        assert!(p.entries.iter().all(Option::is_none));
    }
}
