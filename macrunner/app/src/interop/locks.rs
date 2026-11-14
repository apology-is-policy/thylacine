// POSIX record locks on the shared folders' files, arbitrated among the guest's lock owners. The
// kernel names each owner in its Tlock (client_id and proc_id) and leaves the arbitration to the
// server; every guest owner is this one Mac process, so the host's own fcntl locks could not tell
// two guest owners apart. One table serves every channel, since two sessions may lock one file.
use std::collections::HashMap;
use std::sync::Mutex;

use ninep::{
    P9_LOCK_BLOCKED, P9_LOCK_SUCCESS, P9_LOCK_TYPE_RDLCK, P9_LOCK_TYPE_UNLCK, P9_LOCK_TYPE_WRLCK,
};

/// A host file: its device and inode.
pub type FileId = (u64, u64);

/// Who holds a lock: the session, and the owner the kernel named in it. Two sessions never share
/// an owner.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Owner {
    pub session: u64,
    pub client: Vec<u8>,
    pub proc_id: u32,
}

#[derive(Clone, Debug)]
struct Lock {
    owner: Owner,
    write: bool,
    start: u64,
    /// Exclusive; u64::MAX reaches past any offset (a length of 0 is "to the end, and beyond").
    end: u64,
    /// The fid the lock came through: its clunk lets the lock go, as closing a descriptor does.
    fid: u32,
}

/// A conflicting lock, as Rgetlock reports it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Holder {
    pub write: bool,
    pub start: u64,
    pub length: u64,
    pub proc_id: u32,
    pub client: Vec<u8>,
}

#[derive(Default)]
pub struct Locks {
    files: Mutex<HashMap<FileId, Vec<Lock>>>,
}

fn range(start: u64, length: u64) -> (u64, u64) {
    let end = if length == 0 {
        u64::MAX
    } else {
        start.saturating_add(length)
    };
    (start, end)
}

fn overlaps(a: (u64, u64), b: (u64, u64)) -> bool {
    a.0 < b.1 && b.0 < a.1
}

// Take `owner`'s locks out of [start, end), keeping the parts outside it.
fn carve(locks: &mut Vec<Lock>, owner: &Owner, start: u64, end: u64) {
    let mut kept = Vec::with_capacity(locks.len() + 1);
    for l in locks.drain(..) {
        if l.owner != *owner || !overlaps((l.start, l.end), (start, end)) {
            kept.push(l);
            continue;
        }
        if l.start < start {
            kept.push(Lock {
                end: start,
                ..l.clone()
            });
        }
        if l.end > end {
            kept.push(Lock { start: end, ..l });
        }
    }
    *locks = kept;
}

impl Locks {
    fn conflict<'a>(
        locks: &'a [Lock],
        owner: &Owner,
        write: bool,
        r: (u64, u64),
    ) -> Option<&'a Lock> {
        locks
            .iter()
            .find(|l| l.owner != *owner && (write || l.write) && overlaps((l.start, l.end), r))
    }

    /// A Tlock: P9_LOCK_SUCCESS, or P9_LOCK_BLOCKED when another owner's lock is in the way (the
    /// kernel tries again for a waiting lock). An unknown type is refused by the caller.
    pub fn lock(
        &self,
        file: FileId,
        owner: &Owner,
        kind: u8,
        start: u64,
        length: u64,
        fid: u32,
    ) -> u8 {
        let r = range(start, length);
        let mut files = self.files.lock().unwrap_or_else(|e| e.into_inner());
        let locks = files.entry(file).or_default();
        match kind {
            P9_LOCK_TYPE_UNLCK => carve(locks, owner, r.0, r.1),
            P9_LOCK_TYPE_RDLCK | P9_LOCK_TYPE_WRLCK => {
                let write = kind == P9_LOCK_TYPE_WRLCK;
                if Self::conflict(locks, owner, write, r).is_some() {
                    return P9_LOCK_BLOCKED;
                }
                carve(locks, owner, r.0, r.1);
                locks.push(Lock {
                    owner: owner.clone(),
                    write,
                    start: r.0,
                    end: r.1,
                    fid,
                });
            }
            _ => {}
        }
        if locks.is_empty() {
            files.remove(&file);
        }
        P9_LOCK_SUCCESS
    }

    /// A Tgetlock: the first other owner's lock that a lock of `kind` would meet, or none.
    pub fn test(
        &self,
        file: FileId,
        owner: &Owner,
        kind: u8,
        start: u64,
        length: u64,
    ) -> Option<Holder> {
        if kind == P9_LOCK_TYPE_UNLCK {
            return None;
        }
        let files = self.files.lock().unwrap_or_else(|e| e.into_inner());
        let locks = files.get(&file)?;
        let l = Self::conflict(
            locks,
            owner,
            kind == P9_LOCK_TYPE_WRLCK,
            range(start, length),
        )?;
        Some(Holder {
            write: l.write,
            start: l.start,
            length: if l.end == u64::MAX {
                0
            } else {
                l.end - l.start
            },
            proc_id: l.owner.proc_id,
            client: l.owner.client.clone(),
        })
    }

    /// Every lock a session took through `fid` goes (the fid's clunk).
    pub fn release_fid(&self, session: u64, fid: u32) {
        self.release(|l| l.owner.session == session && l.fid == fid);
    }

    /// Every lock of a session goes (its channel closed, or its Tversion began it again).
    pub fn release_session(&self, session: u64) {
        self.release(|l| l.owner.session == session);
    }

    fn release(&self, gone: impl Fn(&Lock) -> bool) {
        let mut files = self.files.lock().unwrap_or_else(|e| e.into_inner());
        files.retain(|_, locks| {
            locks.retain(|l| !gone(l));
            !locks.is_empty()
        });
    }

    #[cfg(test)]
    fn held(&self, file: FileId) -> Vec<(u64, u64, bool, u32)> {
        let files = self.files.lock().unwrap();
        let mut v: Vec<_> = files
            .get(&file)
            .map(|ls| {
                ls.iter()
                    .map(|l| (l.start, l.end, l.write, l.owner.proc_id))
                    .collect()
            })
            .unwrap_or_default();
        v.sort();
        v
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const F: FileId = (1, 2);

    fn owner(session: u64, proc_id: u32) -> Owner {
        Owner {
            session,
            client: b"thylacine".to_vec(),
            proc_id,
        }
    }

    #[test]
    fn readers_share_and_a_writer_waits_for_them() {
        let t = Locks::default();
        let (a, b, c) = (owner(1, 10), owner(1, 11), owner(1, 12));
        assert_eq!(
            t.lock(F, &a, P9_LOCK_TYPE_RDLCK, 0, 100, 1),
            P9_LOCK_SUCCESS
        );
        assert_eq!(
            t.lock(F, &b, P9_LOCK_TYPE_RDLCK, 50, 100, 2),
            P9_LOCK_SUCCESS
        );
        assert_eq!(t.lock(F, &c, P9_LOCK_TYPE_WRLCK, 99, 1, 3), P9_LOCK_BLOCKED);
        // Past both readers the writer has its range.
        assert_eq!(
            t.lock(F, &c, P9_LOCK_TYPE_WRLCK, 150, 0, 3),
            P9_LOCK_SUCCESS
        );
        assert_eq!(
            t.lock(F, &a, P9_LOCK_TYPE_RDLCK, 1000, 1, 1),
            P9_LOCK_BLOCKED,
            "to the end means beyond"
        );
    }

    #[test]
    fn an_owner_never_conflicts_with_itself_and_its_new_lock_replaces_the_old() {
        let t = Locks::default();
        let a = owner(1, 10);
        assert_eq!(
            t.lock(F, &a, P9_LOCK_TYPE_RDLCK, 0, 100, 1),
            P9_LOCK_SUCCESS
        );
        assert_eq!(
            t.lock(F, &a, P9_LOCK_TYPE_WRLCK, 20, 10, 1),
            P9_LOCK_SUCCESS
        );
        assert_eq!(
            t.held(F),
            vec![(0, 20, false, 10), (20, 30, true, 10), (30, 100, false, 10)]
        );
    }

    #[test]
    fn an_unlock_takes_out_a_middle_and_keeps_the_ends() {
        let t = Locks::default();
        let (a, b) = (owner(1, 10), owner(1, 11));
        t.lock(F, &a, P9_LOCK_TYPE_WRLCK, 0, 100, 1);
        assert_eq!(
            t.lock(F, &a, P9_LOCK_TYPE_UNLCK, 40, 20, 1),
            P9_LOCK_SUCCESS
        );
        assert_eq!(t.held(F), vec![(0, 40, true, 10), (60, 100, true, 10)]);
        assert_eq!(
            t.lock(F, &b, P9_LOCK_TYPE_WRLCK, 40, 20, 2),
            P9_LOCK_SUCCESS
        );
        assert_eq!(t.lock(F, &b, P9_LOCK_TYPE_WRLCK, 39, 1, 2), P9_LOCK_BLOCKED);
        // Another owner's unlock touches nothing of a's.
        t.lock(F, &b, P9_LOCK_TYPE_UNLCK, 0, 0, 2);
        assert_eq!(t.held(F), vec![(0, 40, true, 10), (60, 100, true, 10)]);
    }

    #[test]
    fn the_same_owner_in_two_sessions_is_two_owners() {
        let t = Locks::default();
        assert_eq!(
            t.lock(F, &owner(1, 10), P9_LOCK_TYPE_WRLCK, 0, 0, 1),
            P9_LOCK_SUCCESS
        );
        assert_eq!(
            t.lock(F, &owner(2, 10), P9_LOCK_TYPE_RDLCK, 0, 1, 1),
            P9_LOCK_BLOCKED
        );
    }

    #[test]
    fn test_names_the_lock_in_the_way_and_none_when_the_way_is_clear() {
        let t = Locks::default();
        let (a, b) = (owner(1, 10), owner(1, 11));
        t.lock(F, &a, P9_LOCK_TYPE_WRLCK, 10, 0, 1);
        assert_eq!(
            t.test(F, &b, P9_LOCK_TYPE_RDLCK, 0, 20),
            Some(Holder {
                write: true,
                start: 10,
                length: 0,
                proc_id: 10,
                client: b"thylacine".to_vec()
            })
        );
        assert_eq!(t.test(F, &b, P9_LOCK_TYPE_RDLCK, 0, 10), None);
        assert_eq!(
            t.test(F, &a, P9_LOCK_TYPE_WRLCK, 0, 0),
            None,
            "its own lock is never in its way"
        );
        assert_eq!(t.test((9, 9), &b, P9_LOCK_TYPE_WRLCK, 0, 0), None);
    }

    #[test]
    fn a_clunk_lets_its_fids_locks_go_and_a_session_end_lets_all_go() {
        let t = Locks::default();
        let (a, b) = (owner(1, 10), owner(2, 20));
        t.lock(F, &a, P9_LOCK_TYPE_WRLCK, 0, 10, 1);
        t.lock(F, &a, P9_LOCK_TYPE_WRLCK, 20, 10, 2);
        t.lock(F, &b, P9_LOCK_TYPE_RDLCK, 40, 10, 1);
        t.release_fid(1, 1);
        assert_eq!(t.held(F), vec![(20, 30, true, 10), (40, 50, false, 20)]);
        t.release_session(1);
        assert_eq!(t.held(F), vec![(40, 50, false, 20)]);
        t.release_session(2);
        assert!(
            t.files.lock().unwrap().is_empty(),
            "a file with no lock keeps no entry"
        );
    }
}
