// ring -- the event set's slot bookkeeping, syscall-free: everything an
// `EventRing` decides that a host test can drive without a compositor (a
// synthetic completion, a table rebuild, a full queue). `lib.rs`'s
// `RingCore` owns the Loom + the session and calls in here; the routing
// invariants live here, under test: a completion lands only in the slot AND
// generation it was armed for; a retiring slot frees on ANY completion and
// is never re-armed; EOF or an error ends a stream for good; a full queue
// stops arming; the table index is the slot index.

use alloc::vec::Vec;

use crate::{
    Event, PopFirst, TapError, EV_CAP, EV_REGION, MAX_RING_SURFACES, SLOT_QUEUE_CAP, TEVENT_LEN,
    UD_EVENT,
};

pub(crate) struct Slot {
    pub(crate) used: bool,
    /// The surface dropped while a read was in flight: the slot stays taken
    /// until that read's completion (the EOF after `destroy`) frees it.
    pub(crate) retiring: bool,
    pub(crate) event_fd: i32,
    pub(crate) armed: bool,
    /// Latched when the stream ends -- EOF (the surface retired server-side)
    /// or an error (the session's death, a refused tag): no further read is
    /// armed. Without the latch a poll caller would re-arm through the end
    /// forever, and an errored read's inline error CQE satisfies every
    /// blocking wait at once -- a spin.
    pub(crate) closed: bool,
    /// Bumped per join: the tag a completion must carry to be this
    /// surface's.
    pub(crate) gen: u32,
    pub(crate) pending: Vec<Event>,
}

pub(crate) fn new_slots() -> Vec<Slot> {
    let mut slots = Vec::with_capacity(MAX_RING_SURFACES);
    for _ in 0..MAX_RING_SURFACES {
        slots.push(Slot {
            used: false,
            retiring: false,
            event_fd: -1,
            armed: false,
            closed: false,
            gen: 0,
            pending: Vec::new(),
        });
    }
    slots
}

/// The completion tag: the slot in bits 40.., its generation in bits
/// 8..40, the op class in the low byte.
pub(crate) fn ud(slot: usize, gen: u32) -> u64 {
    ((slot as u64) << 40) | ((gen as u64) << 8) | UD_EVENT
}

/// Take a free slot for `event_fd` (the caller then rebuilds the table).
pub(crate) fn join(slots: &mut [Slot], event_fd: i32) -> Result<u16, TapError> {
    let i = slots.iter().position(|s| !s.used).ok_or(TapError::Full)?;
    let s = &mut slots[i];
    s.used = true;
    s.retiring = false;
    s.event_fd = event_fd;
    s.armed = false;
    s.closed = false;
    s.gen = s.gen.wrapping_add(1);
    s.pending.clear();
    Ok(i as u16)
}

/// A surface is going: the slot frees now, or when the read in flight on
/// it completes. True when the slot was live (the caller rebuilds the
/// table).
pub(crate) fn leave(slots: &mut [Slot], slot: u16) -> bool {
    let i = slot as usize;
    if i >= slots.len() || !slots[i].used {
        return false;
    }
    let s = &mut slots[i];
    s.pending.clear();
    if s.armed {
        s.retiring = true;
    } else {
        s.used = false;
    }
    true
}

/// The registered-handle table: index == slot index -- a live slot's event
/// fid, `placeholder` everywhere else (free, retiring, or left).
pub(crate) fn table(slots: &[Slot], placeholder: i32) -> Vec<i32> {
    slots
        .iter()
        .map(|s| {
            if s.used && !s.retiring {
                s.event_fd
            } else {
                placeholder
            }
        })
        .collect()
}

/// Whether a read belongs on this slot now: live, idle, its stream open,
/// its queue below the cap (a full queue leaves its events with the
/// compositor, whose own cap then retires a consumer that never polls).
pub(crate) fn arm_wanted(s: &Slot) -> bool {
    s.used && !s.retiring && !s.armed && !s.closed && s.pending.len() < SLOT_QUEUE_CAP
}

pub(crate) fn any_armed(slots: &[Slot]) -> bool {
    slots.iter().any(|s| s.armed)
}

/// Route one completion: `user_data` names the slot + generation it was
/// armed for; `result` is the read's byte count, 0 at EOF, negative on an
/// error; the bytes are in the slot's region of `staging`.
pub(crate) fn route(slots: &mut [Slot], staging: &[u8], user_data: u64, result: i32) {
    if user_data & 0xff != UD_EVENT {
        return;
    }
    let i = (user_data >> 40) as usize;
    let gen = ((user_data >> 8) & 0xffff_ffff) as u32;
    if i >= slots.len() {
        return;
    }
    let s = &mut slots[i];
    if !s.used || s.gen != gen {
        return; // a completion for a slot that has moved on
    }
    s.armed = false;
    if s.retiring {
        // The dropped surface's last read completed (the EOF after its
        // `destroy`, or an error): the slot may be re-minted now.
        s.used = false;
        s.retiring = false;
        return;
    }
    if result <= 0 {
        s.closed = true;
        return;
    }
    let region = (i as u64 * EV_REGION) as usize;
    let n = (result as usize).min(EV_CAP);
    let end = region + n;
    if end > staging.len() {
        return;
    }
    let d = staging;
    let mut off = region;
    while off + TEVENT_LEN <= end {
        let g16 = |o: usize| u16::from_le_bytes([d[o], d[o + 1]]);
        let g32 = |o: usize| u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]);
        let g64 = |o: usize| {
            let mut b = [0u8; 8];
            b.copy_from_slice(&d[o..o + 8]);
            u64::from_le_bytes(b)
        };
        s.pending.push(Event {
            kind: g16(off),
            code: g16(off + 2),
            value: g32(off + 4),
            rune: g32(off + 8),
            mods: g16(off + 12),
            flags: g16(off + 14),
            tick: g64(off + 16),
        });
        off += TEVENT_LEN;
    }
}

/// The next queued event for `slot`; `Err(Closed)` once its stream has
/// ended and the backlog is drained.
pub(crate) fn take_event(slots: &mut [Slot], slot: u16) -> Result<Option<Event>, TapError> {
    let s = match slots.get_mut(slot as usize) {
        Some(s) => s,
        None => return Err(TapError::Closed),
    };
    if s.closed && s.pending.is_empty() {
        return Err(TapError::Closed);
    }
    Ok(s.pending.pop_first())
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    /// A staging buffer with `count` records (kind = 1 + k, tick = k) at
    /// `slot`'s region; returns the byte count the kernel would report.
    fn stage(staging: &mut [u8], slot: usize, count: usize) -> i32 {
        let base = slot * EV_REGION as usize;
        for k in 0..count {
            let o = base + k * TEVENT_LEN;
            staging[o..o + 2].copy_from_slice(&((1 + k) as u16).to_le_bytes());
            staging[o + 2..o + 4].copy_from_slice(&0x2au16.to_le_bytes());
            staging[o + 4..o + 8].copy_from_slice(&1u32.to_le_bytes());
            staging[o + 8..o + 12].copy_from_slice(&0u32.to_le_bytes());
            staging[o + 12..o + 14].copy_from_slice(&0u16.to_le_bytes());
            staging[o + 14..o + 16].copy_from_slice(&0u16.to_le_bytes());
            staging[o + 16..o + 24].copy_from_slice(&(k as u64).to_le_bytes());
        }
        (count * TEVENT_LEN) as i32
    }

    fn buf() -> Vec<u8> {
        vec![0u8; MAX_RING_SURFACES * EV_REGION as usize]
    }

    /// Arm as `arm_all` would (the Loom submit is the only part left out).
    fn arm(slots: &mut [Slot], i: usize) {
        assert!(arm_wanted(&slots[i]));
        slots[i].armed = true;
    }

    #[test]
    fn an_error_completion_ends_the_stream_and_is_never_re_armed() {
        // The H-3c-2 round F1: a dead compositor (or a refused tag) posts
        // an error; before the fix the slot was re-armed forever and every
        // blocking wait returned at once.
        let mut s = new_slots();
        let st = buf();
        let i = join(&mut s, 10).unwrap() as usize;
        arm(&mut s, i);
        let t = ud(i, s[i].gen);
        route(&mut s, &st, t, -19);
        assert!(s[i].closed);
        assert!(!s[i].armed);
        assert!(!arm_wanted(&s[i]));
        assert!(!any_armed(&s));
        assert!(matches!(
            take_event(&mut s, i as u16),
            Err(TapError::Closed)
        ));
    }

    #[test]
    fn eof_ends_the_stream_after_the_backlog_drains() {
        let mut s = new_slots();
        let mut st = buf();
        let i = join(&mut s, 10).unwrap() as usize;
        arm(&mut s, i);
        let n = stage(&mut st, i, 2);
        let t = ud(i, s[i].gen);
        route(&mut s, &st, t, n);
        arm(&mut s, i);
        let t = ud(i, s[i].gen);
        route(&mut s, &st, t, 0);
        assert!(s[i].closed);
        let a = take_event(&mut s, i as u16).unwrap().unwrap();
        let b = take_event(&mut s, i as u16).unwrap().unwrap();
        assert_eq!((a.kind, a.tick), (1, 0));
        assert_eq!((b.kind, b.tick), (2, 1));
        assert!(matches!(
            take_event(&mut s, i as u16),
            Err(TapError::Closed)
        ));
    }

    #[test]
    fn a_completion_for_an_older_generation_is_dropped() {
        let mut s = new_slots();
        let mut st = buf();
        let i = join(&mut s, 10).unwrap() as usize;
        let old = s[i].gen;
        assert!(leave(&mut s, i as u16)); // unarmed: freed at once
        assert!(!s[i].used);
        let j = join(&mut s, 11).unwrap() as usize;
        assert_eq!(i, j);
        assert_eq!(s[i].gen, old.wrapping_add(1));
        let n = stage(&mut st, i, 1);
        route(&mut s, &st, ud(i, old), n);
        assert!(s[i].pending.is_empty());
        let t = ud(i, s[i].gen);
        route(&mut s, &st, t, n);
        assert_eq!(s[i].pending.len(), 1);
    }

    #[test]
    fn a_retiring_slot_frees_on_any_completion_and_drops_its_data() {
        let mut s = new_slots();
        let mut st = buf();
        let i = join(&mut s, 10).unwrap() as usize;
        arm(&mut s, i);
        assert!(leave(&mut s, i as u16));
        assert!(s[i].used && s[i].retiring);
        assert!(!arm_wanted(&s[i]));
        assert!(any_armed(&s)); // the read in flight still counts
        assert!(matches!(join(&mut s, 12), Ok(1))); // the slot is not reused yet
        let n = stage(&mut st, i, 3);
        let t = ud(i, s[i].gen);
        route(&mut s, &st, t, n);
        assert!(!s[i].used && !s[i].retiring && !s[i].armed);
        assert!(s[i].pending.is_empty());
        assert!(matches!(join(&mut s, 13), Ok(0))); // now it is
    }

    #[test]
    fn the_table_index_is_the_slot_index() {
        // The H-3c-2 round F3 / SA-4: a rebuild never moves a live slot, so
        // an SQE the kernel has not consumed yet can never be re-bound to
        // another surface's fid; a departed slot's index names the
        // placeholder.
        let mut s = new_slots();
        let ph = 7;
        assert_eq!(join(&mut s, 10).unwrap(), 0);
        assert_eq!(join(&mut s, 11).unwrap(), 1);
        assert_eq!(join(&mut s, 12).unwrap(), 2);
        let t = table(&s, ph);
        assert_eq!(t.len(), MAX_RING_SURFACES);
        assert_eq!(&t[..4], &[10, 11, 12, ph]);
        assert!(leave(&mut s, 1)); // unarmed: freed
        assert_eq!(&table(&s, ph)[..4], &[10, ph, 12, ph]);
        arm(&mut s, 0);
        assert!(leave(&mut s, 0)); // armed: retiring, off the table
        assert!(s[0].used);
        assert_eq!(&table(&s, ph)[..4], &[ph, ph, 12, ph]);
        assert_eq!(join(&mut s, 13).unwrap(), 1); // the free slot, never the retiring one
        assert_eq!(&table(&s, ph)[..4], &[ph, 13, 12, ph]);
    }

    #[test]
    fn a_full_queue_stops_arming_until_it_drains() {
        // The H-3c-2 round F4: a surface its owner never polls must not
        // grow without bound client-side; the compositor's own cap takes
        // over once the ring stops reading for it.
        let mut s = new_slots();
        let mut st = buf();
        let i = join(&mut s, 10).unwrap() as usize;
        let n = stage(&mut st, i, 4);
        while s[i].pending.len() < SLOT_QUEUE_CAP {
            arm(&mut s, i);
            let t = ud(i, s[i].gen);
            route(&mut s, &st, t, n);
        }
        assert_eq!(s[i].pending.len(), SLOT_QUEUE_CAP);
        assert!(!arm_wanted(&s[i]));
        assert!(!s[i].closed);
        take_event(&mut s, i as u16).unwrap().unwrap();
        assert!(arm_wanted(&s[i]));
    }

    #[test]
    fn route_takes_whole_records_only_and_clamps_the_count() {
        let mut s = new_slots();
        let mut st = buf();
        let i = join(&mut s, 10).unwrap() as usize;
        let n = stage(&mut st, i, 2);
        arm(&mut s, i);
        let t = ud(i, s[i].gen);
        route(&mut s, &st, t, n + 5); // a partial third record
        assert_eq!(s[i].pending.len(), 2);
        s[i].pending.clear();
        stage(&mut st, i, 4);
        arm(&mut s, i);
        let t = ud(i, s[i].gen);
        route(&mut s, &st, t, 1000); // past EV_CAP: clamped to 4
        assert_eq!(s[i].pending.len(), EV_CAP / TEVENT_LEN);
        assert_eq!(s[i].pending[3].tick, 3);
    }

    #[test]
    fn a_foreign_tag_or_slot_is_ignored() {
        let mut s = new_slots();
        let mut st = buf();
        let i = join(&mut s, 10).unwrap() as usize;
        let n = stage(&mut st, i, 1);
        arm(&mut s, i);
        let g = s[i].gen;
        route(&mut s, &st, ud(i, g) & !0xff, n); // not an event op
        route(&mut s, &st, ud(MAX_RING_SURFACES, g), n); // no such slot
        route(&mut s, &st, ud(i + 1, 0), n); // an unused slot
        assert!(s[i].pending.is_empty() && s[i].armed);
    }

    #[test]
    fn join_refuses_past_the_bound() {
        let mut s = new_slots();
        for k in 0..MAX_RING_SURFACES {
            assert_eq!(join(&mut s, k as i32).unwrap() as usize, k);
        }
        assert!(matches!(join(&mut s, 99), Err(TapError::Full)));
    }
}
