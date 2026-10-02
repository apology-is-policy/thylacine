//! HSR1 reservation and HSC1 cancellation control. Encoding is not authority:
//! the server supplies exact declared-connection and kernel peer identities.
//! No payload bytes or hardware capabilities cross this protocol.
pub const REQUEST_BYTES: usize = 40;
pub const REPLY_BYTES: usize = 56;
pub const PARTICIPANTS: usize = 8;
pub const NORMAL: u32 = 0;
pub const QUIESCING: u32 = 1;
pub const FAILED: u32 = 4;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Op {
    Reserve,
    Withdraw,
    Join,
    State,
    Cancelled,
    Retire,
}
impl Op {
    fn wire(self) -> ([u8; 4], u16) {
        match self {
            Self::Reserve => (*b"HSR1", 1),
            Self::Withdraw => (*b"HSR1", 2),
            Self::Join => (*b"HSC1", 1),
            Self::State => (*b"HSC1", 2),
            Self::Cancelled => (*b"HSC1", 3),
            Self::Retire => (*b"HSC1", 4),
        }
    }
    fn decode(magic: &[u8], value: u16) -> Option<Self> {
        Some(match (magic, value) {
            (b"HSR1", 1) => Self::Reserve,
            (b"HSR1", 2) => Self::Withdraw,
            (b"HSC1", 1) => Self::Join,
            (b"HSC1", 2) => Self::State,
            (b"HSC1", 3) => Self::Cancelled,
            (b"HSC1", 4) => Self::Retire,
            _ => return None,
        })
    }
}
fn u64at(b: &[u8], i: usize) -> u64 {
    u64::from_le_bytes(b[i..i + 8].try_into().unwrap())
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Request {
    pub op: Op,
    pub request: u64,
    pub registration: u64,
    pub generation: u64,
    pub revision: u64,
}
impl Request {
    pub fn encode(self) -> [u8; REQUEST_BYTES] {
        let mut b = [0; REQUEST_BYTES];
        let (magic, op) = self.op.wire();
        b[..4].copy_from_slice(&magic);
        b[4..6].copy_from_slice(&1u16.to_le_bytes());
        b[6..8].copy_from_slice(&op.to_le_bytes());
        for (i, n) in [
            self.request,
            self.registration,
            self.generation,
            self.revision,
        ]
        .iter()
        .enumerate()
        {
            b[8 + i * 8..16 + i * 8].copy_from_slice(&n.to_le_bytes());
        }
        b
    }
    pub fn decode(b: &[u8]) -> Option<Self> {
        if b.len() != REQUEST_BYTES || b[4..6] != [1, 0] {
            return None;
        }
        let r = Self {
            op: Op::decode(&b[..4], u16::from_le_bytes(b[6..8].try_into().unwrap()))?,
            request: u64at(b, 8),
            registration: u64at(b, 16),
            generation: u64at(b, 24),
            revision: u64at(b, 32),
        };
        if r.request == 0 {
            return None;
        }
        let valid = match r.op {
            Op::Reserve => r.registration | r.generation | r.revision == 0,
            Op::Withdraw => r.registration != 0 && r.generation | r.revision == 0,
            Op::State => r.registration != 0 && r.generation == 0,
            Op::Join | Op::Cancelled | Op::Retire => r.registration != 0 && r.revision != 0,
        };
        valid.then_some(r)
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Snapshot {
    pub registration: u64,
    pub generation: u64,
    pub revision: u64,
    pub phase: u32,
    pub enabled: bool,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Reply {
    pub op: Op,
    pub request: u64,
    pub state: Snapshot,
}
impl Reply {
    pub fn encode(self) -> [u8; REPLY_BYTES] {
        let mut b = [0; REPLY_BYTES];
        let (magic, op) = self.op.wire();
        b[..4].copy_from_slice(&magic);
        b[4] = 1;
        b[6..8].copy_from_slice(&op.to_le_bytes());
        for (i, n) in [
            self.request,
            self.state.registration,
            self.state.generation,
            self.state.revision,
        ]
        .iter()
        .enumerate()
        {
            b[8 + i * 8..16 + i * 8].copy_from_slice(&n.to_le_bytes());
        }
        b[40..44].copy_from_slice(&self.state.phase.to_le_bytes());
        b[44] = self.state.enabled as u8;
        b
    }
    pub fn decode(b: &[u8], q: Request) -> Option<Self> {
        if b.len() != REPLY_BYTES || b[4..6] != [1, 0] || b[48..56] != [0; 8] {
            return None;
        }
        let op = Op::decode(&b[..4], u16::from_le_bytes(b[6..8].try_into().unwrap()))?;
        let state = Snapshot {
            registration: u64at(b, 16),
            generation: u64at(b, 24),
            revision: u64at(b, 32),
            phase: u32::from_le_bytes(b[40..44].try_into().unwrap()),
            enabled: b[44] == 1,
        };
        if op != q.op
            || u64at(b, 8) != q.request
            || state.registration == 0
            || state.revision == 0
            || state.phase > FAILED
            || b[44] > 1
            || b[45..48] != [0; 3]
            || (q.op != Op::Reserve && state.registration != q.registration)
            || (state.enabled && state.phase != NORMAL)
        {
            return None;
        }
        Some(Self {
            op,
            request: q.request,
            state,
        })
    }
}

/// These identities are never decoded from a peer's message.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Peer {
    pub stripes: u64,
    pub declaration: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Denied,
    Gone,
    Busy,
    Exhausted,
    Invalid,
}
#[derive(Clone, Copy, Debug)]
struct Member {
    peer: Peer,
    id: u64,
    lane: Option<u64>,
    revision: u64,
    // An enabled service remains potentially active until its owner confirms
    // cancellation. Seat notification, EOF and phase changes do NOT clear it.
    potential: bool,
    enabled: bool,
    lost: bool,
    retired: bool,
    declared: bool,
}
/// Single serialized owner (or short caller-held lock). No I/O or allocation.
/// Private incarnations never wrap; loss/exhaustion cannot become an empty
/// successful barrier. Independent graphics progress is not required here.
pub struct Coordinator {
    members: [Option<Member>; PARTICIPANTS],
    next: u64,
    clock: u64,
    generation: u64,
    phase: u32,
    sampled: bool,
    broken: bool,
}
impl Default for Coordinator {
    fn default() -> Self {
        Self::new()
    }
}
impl Coordinator {
    pub const fn new() -> Self {
        Self {
            members: [None; PARTICIPANTS],
            next: 1,
            clock: 0,
            generation: 0,
            phase: FAILED,
            sampled: false,
            broken: false,
        }
    }
    fn tick(&mut self) -> Result<u64, Error> {
        match self.clock.checked_add(1) {
            Some(n) => {
                self.clock = n;
                Ok(n)
            }
            None => {
                self.broken = true;
                Err(Error::Exhausted)
            }
        }
    }
    fn live(&self) -> Result<(), Error> {
        if self.broken || !self.sampled {
            Err(Error::Denied)
        } else {
            Ok(())
        }
    }
    fn index(&self, id: u64) -> Result<usize, Error> {
        self.members
            .iter()
            .position(|m| m.is_some_and(|m| m.id == id))
            .ok_or(Error::Gone)
    }
    fn lane(&self, id: u64, stripes: u64, lane: u64) -> Result<usize, Error> {
        self.live()?;
        let i = self.index(id)?;
        let m = self.members[i].unwrap();
        if lane == 0 || m.peer.stripes != stripes || m.lane != Some(lane) || m.lost || m.retired {
            Err(Error::Denied)
        } else {
            Ok(i)
        }
    }
    fn snapshot_at(&self, i: usize) -> Snapshot {
        let m = self.members[i].unwrap();
        Snapshot {
            registration: m.id,
            generation: self.generation,
            revision: m.revision,
            phase: self.phase,
            enabled: m.enabled && self.phase == NORMAL && !self.broken,
        }
    }
    /// Called only with a fresh sample from the designated trusted seat owner.
    pub fn observe(&mut self, generation: u64, phase: u32) -> Result<(), Error> {
        if self.broken {
            return Err(Error::Denied);
        }
        if phase > FAILED || (self.sampled && generation < self.generation) {
            self.broken = true;
            return Err(Error::Invalid);
        }
        if self.sampled && self.generation == generation && self.phase == phase {
            return Ok(());
        }
        let revision = self.tick()?;
        self.sampled = true;
        self.generation = generation;
        self.phase = phase;
        for m in self.members.iter_mut().flatten() {
            m.revision = revision;
            m.enabled = false;
        }
        // potential intentionally survives even a failed episode/restoration.
        // A live service that missed cancellation still owes it next time.
        Ok(())
    }
    pub fn reserve(&mut self, peer: Peer) -> Result<Snapshot, Error> {
        self.live()?;
        if self.phase != NORMAL || peer.stripes == 0 || peer.declaration == 0 {
            return Err(Error::Denied);
        }
        if self.members.iter().flatten().any(|m| m.peer == peer) {
            return Err(Error::Busy);
        }
        let i = self
            .members
            .iter()
            .position(Option::is_none)
            .ok_or(Error::Busy)?;
        let next = match self.next.checked_add(1) {
            Some(n) => n,
            None => {
                self.broken = true;
                return Err(Error::Exhausted);
            }
        };
        let revision = self.tick()?;
        let id = self.next;
        self.next = next;
        self.members[i] = Some(Member {
            peer,
            id,
            lane: None,
            revision,
            potential: false,
            enabled: false,
            lost: false,
            retired: false,
            declared: true,
        });
        Ok(self.snapshot_at(i))
    }
    /// Withdraw is main-connection authenticated, and cannot remove a live or
    /// disconnected participant merely because its normal UI connection died.
    pub fn withdraw(&mut self, peer: Peer, id: u64) -> Result<(), Error> {
        self.live()?;
        let i = self.index(id)?;
        let m = self.members[i].unwrap();
        if m.peer != peer
            || m.potential
            || (m.lane.is_some() && !m.retired)
            || (m.lost && !m.retired)
        {
            return Err(Error::Denied);
        }
        self.tick()?;
        self.members[i] = None;
        Ok(())
    }
    /// The coordinator pins the physical control connection's incarnation.
    /// A second connection from even the same process cannot replace it.
    pub fn join(
        &mut self,
        id: u64,
        stripes: u64,
        lane: u64,
        generation: u64,
        revision: u64,
    ) -> Result<Snapshot, Error> {
        self.live()?;
        let i = self.index(id)?;
        let m = self.members[i].unwrap();
        if self.phase != NORMAL
            || generation != self.generation
            || revision != m.revision
            || lane == 0
            || stripes != m.peer.stripes
            || m.lost
            || m.retired
            || !m.declared
            || m.lane.is_some_and(|old| old != lane)
        {
            return Err(Error::Denied);
        }
        let revision = self.tick()?;
        let m = self.members[i].as_mut().unwrap();
        m.lane = Some(lane);
        m.revision = revision;
        m.potential = true;
        m.enabled = true;
        Ok(self.snapshot_at(i))
    }
    pub fn state(&self, id: u64, stripes: u64, lane: u64) -> Result<Snapshot, Error> {
        let i = self.lane(id, stripes, lane)?;
        Ok(self.snapshot_at(i))
    }
    pub fn cancelled(
        &mut self,
        id: u64,
        stripes: u64,
        lane: u64,
        generation: u64,
        revision: u64,
    ) -> Result<Snapshot, Error> {
        let i = self.lane(id, stripes, lane)?;
        if self.phase != QUIESCING
            || generation != self.generation
            || revision != self.members[i].unwrap().revision
        {
            return Err(Error::Denied);
        }
        // No revision bump: another participant's ACK cannot stale this one.
        let m = self.members[i].as_mut().unwrap();
        m.potential = false;
        m.enabled = false;
        Ok(self.snapshot_at(i))
    }
    pub fn retire(
        &mut self,
        id: u64,
        stripes: u64,
        lane: u64,
        generation: u64,
        revision: u64,
    ) -> Result<Snapshot, Error> {
        let i = self.lane(id, stripes, lane)?;
        if generation != self.generation || revision != self.members[i].unwrap().revision {
            return Err(Error::Denied);
        }
        let revision = self.tick()?;
        let m = self.members[i].as_mut().unwrap();
        m.potential = false;
        m.enabled = false;
        m.retired = true;
        m.revision = revision;
        Ok(self.snapshot_at(i))
    }
    /// Admission is limited to a kernel peer that already reserved on its
    /// authenticated normal connection. A token alone never admits a lane.
    pub fn awaiting_lane(&self, stripes: u64) -> bool {
        !self.broken
            && self.members.iter().flatten().any(|m| {
                m.peer.stripes == stripes && m.declared && !m.retired && !m.lost && m.lane.is_none()
            })
    }
    pub fn reservation(&self, peer: Peer, id: u64) -> Result<Snapshot, Error> {
        self.live()?;
        let i = self.index(id)?;
        if self.members[i].unwrap().peer != peer {
            return Err(Error::Denied);
        }
        Ok(self.snapshot_at(i))
    }
    /// Losing the normal declaration revokes admission, but is not proof
    /// that the independent service executor has cancelled its operations.
    pub fn declaration_gone(&mut self, declaration: u64) -> Result<(), Error> {
        let revision = self.tick()?;
        for m in &mut self.members {
            if let Some(v) = m {
                if v.peer.declaration == declaration {
                    if v.lane.is_none() || v.retired {
                        *m = None;
                    } else {
                        v.declared = false;
                        v.enabled = false;
                        v.revision = revision;
                    }
                }
            }
        }
        Ok(())
    }
    /// A gracefully retired or withdrawn lane can release its FD. HUP on a
    /// live/cancelled-but-not-retired lane still pins process-death evidence.
    pub fn lane_retired(&self, lane: u64) -> bool {
        !self
            .members
            .iter()
            .flatten()
            .any(|m| m.lane == Some(lane) && !m.retired)
    }
    /// EOF never certifies cancellation, nor allows another connection to join.
    pub fn disconnect(&mut self, lane: u64) {
        if lane == 0 {
            return;
        }
        for m in self.members.iter_mut().flatten() {
            if m.lane == Some(lane) {
                m.lost = true;
                m.enabled = false;
            }
        }
    }
    /// Only a fresh kernel-confirmed process death may call this. A raw
    /// connection HUP, PID guess or caller-supplied identity is insufficient.
    pub fn peer_dead(&mut self, stripes: u64) -> Result<(), Error> {
        self.live()?;
        if stripes == 0 {
            return Err(Error::Invalid);
        }
        if self
            .members
            .iter()
            .flatten()
            .any(|m| m.peer.stripes == stripes)
        {
            self.tick()?;
            for m in &mut self.members {
                if m.is_some_and(|m| m.peer.stripes == stripes) {
                    *m = None;
                }
            }
        }
        Ok(())
    }
    /// Even zero members needs a fresh QUIESCING sample. Calling this does
    /// not open trusted input; Lictor independently checks hardware and keys.
    pub fn aggregate(&self) -> Option<u64> {
        if self.broken
            || !self.sampled
            || self.phase != QUIESCING
            || self.members.iter().flatten().any(|m| m.potential)
        {
            None
        } else {
            Some(self.generation)
        }
    }
    pub fn admitted(&self, peer: Peer, id: u64) -> bool {
        !self.broken
            && self.sampled
            && self.phase == NORMAL
            && self
                .members
                .iter()
                .flatten()
                .any(|m| m.id == id && m.peer == peer && m.enabled && !m.lost && !m.retired)
    }
}
const _: () = assert!(core::mem::size_of::<Coordinator>() <= 1024);

#[cfg(test)]
mod tests {
    use super::*;
    fn peer(n: u64) -> Peer {
        Peer {
            stripes: n,
            declaration: n + 100,
        }
    }
    fn table() -> Coordinator {
        let mut c = Coordinator::new();
        c.observe(0, NORMAL).unwrap();
        c
    }
    fn join(c: &mut Coordinator, n: u64) -> Snapshot {
        let s = c.reserve(peer(n)).unwrap();
        c.join(s.registration, n, n, s.generation, s.revision)
            .unwrap()
    }
    fn cancel(c: &mut Coordinator, s: Snapshot, n: u64) -> Snapshot {
        let s = c.state(s.registration, n, n).unwrap();
        c.cancelled(s.registration, n, n, s.generation, s.revision)
            .unwrap()
    }
    #[test]
    fn declaration_loss_keeps_live_obligation_and_prevents_rejoin() {
        let mut c = Coordinator::new();
        c.observe(0, NORMAL).unwrap();
        let peer = Peer {
            stripes: 40,
            declaration: 20,
        };
        let r = c.reserve(peer).unwrap();
        assert!(c.awaiting_lane(40));
        let r = c
            .join(r.registration, 40, 7, r.generation, r.revision)
            .unwrap();
        assert!(!c.awaiting_lane(40));
        assert!(!c.lane_retired(7));
        c.declaration_gone(20).unwrap();
        let now = c.state(r.registration, 40, 7).unwrap();
        assert!(!now.enabled);
        assert!(c
            .join(r.registration, 40, 7, now.generation, now.revision)
            .is_err());
        c.observe(1, QUIESCING).unwrap();
        assert_eq!(c.aggregate(), None);
        let now = c.state(r.registration, 40, 7).unwrap();
        c.cancelled(r.registration, 40, 7, 1, now.revision).unwrap();
        assert_eq!(c.aggregate(), Some(1));
        assert!(!c.lane_retired(7));
        c.retire(r.registration, 40, 7, 1, now.revision).unwrap();
        assert!(c.lane_retired(7));
        c.declaration_gone(20).unwrap();
        assert!(c.reservation(peer, r.registration).is_err());
    }
    #[test]
    fn unjoined_declaration_teardown_reclaims_its_reservation() {
        let mut c = Coordinator::new();
        c.observe(0, NORMAL).unwrap();
        let p = Peer {
            stripes: 40,
            declaration: 20,
        };
        let r = c.reserve(p).unwrap();
        c.declaration_gone(20).unwrap();
        assert!(!c.awaiting_lane(40));
        assert!(c
            .join(r.registration, 40, 7, r.generation, r.revision)
            .is_err());
        assert!(c.reserve(p).is_ok());
    }
    #[test]
    fn wire_rejects_aliases_trailing_and_unknown_fields() {
        for op in [
            Op::Reserve,
            Op::Withdraw,
            Op::Join,
            Op::State,
            Op::Cancelled,
            Op::Retire,
        ] {
            let q = Request {
                op,
                request: 1,
                registration: if op == Op::Reserve { 0 } else { 8 },
                generation: 0,
                revision: if matches!(op, Op::Join | Op::Cancelled | Op::Retire) {
                    2
                } else {
                    0
                },
            };
            let b = q.encode();
            assert_eq!(Request::decode(&b), Some(q));
            for at in [4, 5, 7] {
                let mut b = b;
                b[at] = 255;
                assert!(Request::decode(&b).is_none());
            }
            let mut b = b;
            b[8..16].fill(0);
            assert!(Request::decode(&b).is_none());
            assert!(Request::decode(&[0; 41]).is_none());
            let r = Reply {
                op,
                request: 1,
                state: Snapshot {
                    registration: 8,
                    generation: 0,
                    revision: 2,
                    phase: NORMAL,
                    enabled: false,
                },
            };
            assert_eq!(Reply::decode(&r.encode(), q), Some(r));
            for at in [5, 7, 8, 44, 45, 48, 55] {
                let mut b = r.encode();
                b[at] = 255;
                assert!(Reply::decode(&b, q).is_none(), "{at}");
            }
            let mut r = r;
            r.state.phase = QUIESCING;
            r.state.enabled = true;
            assert!(Reply::decode(&r.encode(), q).is_none());
        }
    }
    #[test]
    fn empty_is_not_ready_until_fresh_quiescence() {
        let mut c = Coordinator::new();
        assert_eq!(c.aggregate(), None);
        assert!(c.reserve(peer(1)).is_err());
        c.observe(0, NORMAL).unwrap();
        assert_eq!(c.aggregate(), None);
        c.observe(1, QUIESCING).unwrap();
        assert_eq!(c.aggregate(), Some(1));
        c.observe(1, 2).unwrap();
        assert_eq!(c.aggregate(), None);
    }
    #[test]
    fn every_visible_and_hidden_member_must_cancel() {
        let mut c = table();
        let a = join(&mut c, 1);
        let b = join(&mut c, 2);
        c.observe(1, QUIESCING).unwrap();
        assert_eq!(c.aggregate(), None);
        cancel(&mut c, a, 1);
        assert_eq!(c.aggregate(), None);
        cancel(&mut c, b, 2);
        assert_eq!(c.aggregate(), Some(1));
    }
    #[test]
    fn notification_and_eof_do_not_discharge_obligation() {
        let mut c = table();
        let a = join(&mut c, 1);
        c.observe(1, QUIESCING).unwrap();
        assert!(!c.admitted(peer(1), a.registration));
        c.disconnect(1);
        assert_eq!(c.aggregate(), None);
        assert!(c.withdraw(peer(1), a.registration).is_err());
        assert!(c.join(a.registration, 1, 2, 1, a.revision).is_err());
        c.peer_dead(2).unwrap();
        assert_eq!(c.aggregate(), None);
        c.peer_dead(1).unwrap();
        assert_eq!(c.aggregate(), Some(1));
    }
    #[test]
    fn exact_peer_lane_generation_and_revision_required() {
        let mut c = table();
        let a = join(&mut c, 1);
        c.observe(1, QUIESCING).unwrap();
        let s = c.state(a.registration, 1, 1).unwrap();
        for (id, p, l, g, r) in [
            (s.registration + 1, 1, 1, 1, s.revision),
            (s.registration, 2, 1, 1, s.revision),
            (s.registration, 1, 2, 1, s.revision),
            (s.registration, 1, 1, 0, s.revision),
            (s.registration, 1, 1, 1, s.revision - 1),
        ] {
            assert!(c.cancelled(id, p, l, g, r).is_err());
            assert_eq!(c.aggregate(), None);
        }
        cancel(&mut c, a, 1);
        cancel(&mut c, a, 1);
        assert_eq!(c.aggregate(), Some(1));
    }
    #[test]
    fn registration_races_cannot_enable_after_snapshot() {
        let mut c = table();
        let a = c.reserve(peer(1)).unwrap();
        c.observe(1, QUIESCING).unwrap();
        assert!(c.reserve(peer(2)).is_err());
        assert!(c
            .join(a.registration, 1, 1, a.generation, a.revision)
            .is_err());
        assert_eq!(c.aggregate(), Some(1));
        c.observe(1, NORMAL).unwrap();
        assert!(c
            .join(a.registration, 1, 1, a.generation, a.revision)
            .is_err());
    }
    #[test]
    fn restoration_requires_fresh_join_and_old_ack_cannot_recur() {
        let mut c = table();
        let a = join(&mut c, 1);
        c.observe(1, QUIESCING).unwrap();
        let ack = cancel(&mut c, a, 1);
        c.observe(1, 2).unwrap();
        c.observe(1, 3).unwrap();
        c.observe(1, NORMAL).unwrap();
        assert!(!c.admitted(peer(1), a.registration));
        let now = c.state(a.registration, 1, 1).unwrap();
        c.join(a.registration, 1, 1, now.generation, now.revision)
            .unwrap();
        assert!(c.admitted(peer(1), a.registration));
        c.observe(2, QUIESCING).unwrap();
        assert!(c
            .cancelled(a.registration, 1, 1, ack.generation, ack.revision)
            .is_err());
        assert_eq!(c.aggregate(), None);
        cancel(&mut c, a, 1);
        assert_eq!(c.aggregate(), Some(2));
    }
    #[test]
    fn timed_out_live_owner_still_owes_cancellation() {
        let mut c = table();
        let a = join(&mut c, 1);
        c.observe(1, QUIESCING).unwrap();
        c.observe(1, FAILED).unwrap();
        c.observe(1, NORMAL).unwrap();
        assert!(!c.admitted(peer(1), a.registration));
        c.observe(2, QUIESCING).unwrap();
        assert_eq!(c.aggregate(), None);
        cancel(&mut c, a, 1);
        assert_eq!(c.aggregate(), Some(2));
    }
    #[test]
    fn orderly_retirement_and_unenabled_withdrawal() {
        let mut c = table();
        let a = c.reserve(peer(1)).unwrap();
        c.withdraw(peer(1), a.registration).unwrap();
        let b = join(&mut c, 1);
        assert_ne!(a.registration, b.registration);
        assert!(c.withdraw(peer(1), b.registration).is_err());
        let s = c.state(b.registration, 1, 1).unwrap();
        c.retire(b.registration, 1, 1, s.generation, s.revision)
            .unwrap();
        assert!(!c.admitted(peer(1), b.registration));
        c.withdraw(peer(1), b.registration).unwrap();
        c.observe(1, QUIESCING).unwrap();
        assert_eq!(c.aggregate(), Some(1));
    }
    #[test]
    fn bounded_capacity_and_no_identity_recycling() {
        let mut c = table();
        for n in 1..=PARTICIPANTS as u64 {
            join(&mut c, n);
        }
        assert_eq!(c.reserve(peer(1)), Err(Error::Busy));
        assert_eq!(c.reserve(peer(20)), Err(Error::Busy));
        c.peer_dead(1).unwrap();
        let s = join(&mut c, 20);
        assert!(s.registration > PARTICIPANTS as u64);
        let mut c = table();
        c.next = u64::MAX;
        assert_eq!(c.reserve(peer(1)), Err(Error::Exhausted));
        assert!(c.members.iter().all(Option::is_none));
    }
    #[test]
    fn revision_exhaustion_and_bad_samples_fail_closed() {
        let mut c = table();
        join(&mut c, 1);
        c.clock = u64::MAX;
        assert_eq!(c.observe(1, QUIESCING), Err(Error::Exhausted));
        assert_eq!(c.aggregate(), None);
        let mut c = table();
        c.observe(2, QUIESCING).unwrap();
        assert_eq!(c.observe(1, NORMAL), Err(Error::Invalid));
        assert_eq!(c.aggregate(), None);
        assert!(c.reserve(peer(1)).is_err());
    }
}
