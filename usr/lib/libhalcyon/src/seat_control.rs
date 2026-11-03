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
/// Which refused requests a participant re-samples from, since the seat can move
/// before a request reaches the coordinator. A re-sample restores no authority.
/// A Join refused twice at one snapshot raced nothing, so retrying would spin.
#[derive(Clone, Copy, Debug, Default)]
struct Refusals {
    join: Option<(u64, u64)>,
}
impl Refusals {
    fn stale(&mut self, q: &Request) -> bool {
        match q.op {
            Op::Cancelled | Op::Retire => true,
            Op::Join => self.join.replace((q.generation, q.revision)) != Some((q.generation, q.revision)),
            _ => false,
        }
    }
}
/// What the lane's owner does with one completed exchange, in this order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Step {
    /// Drop local authority, then finish the application work it covered.
    pub revoke: bool,
    /// The generation normal authority is restored at.
    pub normal: Option<u64>,
    pub send: Option<Op>,
}
/// How long a seat away from normal stops counting against a participant's own
/// bounds. The kernel's deadlines total 100 s, but it gives FAILED none, and a held
/// key keeps it there, so this is what returns a session that cannot start to login.
pub const PARKED_CEILING_MS: u64 = 120_000;
/// One lane owner's seat decisions. The owner does the I/O, so every arm is
/// reachable here without a transport or a guest.
#[derive(Clone, Copy, Debug)]
pub struct Participant {
    state: Snapshot,
    joined: bool,
    may_owe: bool,
    stopping: bool,
    retired: bool,
    acknowledged: Option<(u64, u64)>,
    refusals: Refusals,
    away_since: Option<u64>,
}
impl Participant {
    pub fn new(state: Snapshot, now_ms: u64) -> Self {
        Self {
            state,
            joined: false,
            may_owe: false,
            stopping: false,
            retired: false,
            acknowledged: None,
            refusals: Refusals::default(),
            away_since: (state.phase != NORMAL).then_some(now_ms),
        }
    }
    pub fn state(&self) -> Snapshot {
        self.state
    }
    pub fn joined(&self) -> bool {
        self.joined
    }
    /// The table may hold this member's cancellation, which only the process's death
    /// can then answer. It serves a Join before the reply can be read here, so this
    /// holds from the send, and only a refusal of a first Join clears it.
    pub fn may_owe(&self) -> bool {
        self.may_owe
    }
    pub fn retired(&self) -> bool {
        self.retired
    }
    pub fn stop(&mut self) {
        self.stopping = true;
    }
    /// The compositor serves nothing while the seat is away from normal, so that
    /// time is not this member's to account for, up to the ceiling.
    pub fn parked(&self, now_ms: u64) -> bool {
        !self.stopping && self.away_since.is_some_and(|t| now_ms.saturating_sub(t) < PARKED_CEILING_MS)
    }
    pub fn normal(&self) -> Option<u64> {
        (self.joined && !self.stopping && !self.retired && self.state.enabled && self.state.phase == NORMAL)
            .then_some(self.state.generation)
    }
    pub fn request(&mut self, op: Op, request: u64) -> Request {
        self.may_owe |= op == Op::Join;
        Request {
            op,
            request,
            registration: self.state.registration,
            generation: if op == Op::State { 0 } else { self.state.generation },
            revision: self.state.revision,
        }
    }
    /// `reply` is None when the table refused the request. Err ends the session's
    /// participation: Denied for a refusal with nothing to sample from, Invalid for
    /// a Join answered with a seat that is not normal.
    pub fn complete(&mut self, q: &Request, reply: Option<Snapshot>, now_ms: u64) -> Result<Step, Error> {
        let Some(state) = reply else {
            if q.op == Op::Join && !self.joined {
                self.may_owe = false;
            }
            if !self.refusals.stale(q) {
                return Err(Error::Denied);
            }
            self.state.revision = 0;
            return Ok(Step { revoke: true, normal: None, send: Some(Op::State) });
        };
        self.state = state;
        // From the first reply away, so fresh replies through a long episode do not restart it.
        self.away_since = (state.phase != NORMAL).then(|| self.away_since.unwrap_or(now_ms));
        if q.op == Op::Retire {
            self.retired = true;
            return Ok(Step { revoke: false, normal: None, send: None });
        }
        if q.op == Op::Join {
            if !state.enabled || state.phase != NORMAL {
                return Err(Error::Invalid);
            }
            self.joined = true;
        }
        if self.stopping {
            // A lane that never joined holds no record to retire.
            self.retired = !self.joined;
            let send = self.joined.then_some(Op::Retire);
            return Ok(Step { revoke: true, normal: None, send });
        }
        if state.phase != NORMAL {
            let identity = (state.generation, state.revision);
            if q.op == Op::Cancelled {
                self.acknowledged = Some(identity);
            }
            // Only Join makes a member owe a cancellation, and the table refuses
            // one from a lane it has not pinned.
            let owes = self.joined && state.phase == QUIESCING && self.acknowledged != Some(identity);
            return Ok(Step { revoke: true, normal: None, send: Some(if owes { Op::Cancelled } else { Op::State }) });
        }
        if !state.enabled {
            self.acknowledged = None;
            return Ok(Step { revoke: true, normal: None, send: Some(Op::Join) });
        }
        Ok(Step { revoke: false, normal: self.normal(), send: Some(Op::State) })
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
    /// `state`, also for a reserving peer's own record before its lane is pinned:
    /// a Join that lost a race needs a fresh pair, and the connection that reserved
    /// parks with the compositor. Pins nothing and certifies nothing.
    pub fn sample(&self, id: u64, stripes: u64, lane: u64) -> Result<Snapshot, Error> {
        self.live()?;
        let i = self.index(id)?;
        if self.members[i].unwrap().lane.is_none() {
            return if lane != 0 && self.members[i].unwrap().peer.stripes == stripes {
                Ok(self.snapshot_at(i))
            } else {
                Err(Error::Denied)
            };
        }
        self.state(id, stripes, lane)
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
    pub fn parked(&self) -> bool {
        self.phase != NORMAL
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
    fn q(op: Op, generation: u64, revision: u64) -> Request {
        Request { op, request: 1, registration: 1, generation, revision }
    }
    // One exchange against the real table, the way the coordinator routes it.
    fn exchange(c: &mut Coordinator, p: &mut Participant, op: Op, n: u64) -> Result<Step, Error> {
        let q = p.request(op, 1);
        let s = p.state();
        let reply = match op {
            Op::Join => c.join(s.registration, n, n, q.generation, q.revision),
            Op::State => c.sample(s.registration, n, n),
            Op::Cancelled => c.cancelled(s.registration, n, n, q.generation, q.revision),
            Op::Retire => c.retire(s.registration, n, n, q.generation, q.revision),
            _ => unreachable!(),
        };
        p.complete(&q, reply.ok(), 0)
    }
    #[test]
    fn a_joined_participant_resamples_a_refusal_once_per_snapshot() {
        let mut r = Refusals::default();
        assert!(r.stale(&q(Op::Cancelled, 1, 1)));
        assert!(r.stale(&q(Op::Retire, 1, 1)));
        // A Join lost a race; the same snapshot twice raced nothing; then the seat moved.
        assert!(r.stale(&q(Op::Join, 1, 5)));
        assert!(!r.stale(&q(Op::Join, 1, 5)));
        assert!(r.stale(&q(Op::Join, 2, 6)));
        // A refused State has nothing left to sample with.
        assert!(!r.stale(&q(Op::State, 2, 6)));
    }
    #[test]
    fn a_join_may_owe_from_its_send_until_a_first_join_is_refused() {
        let mut c = table();
        let reserved = c.reserve(peer(1)).unwrap();
        let mut p = Participant::new(reserved, 0);
        assert!(!p.may_owe());
        // The table can serve a Join before its reply is read.
        let q = p.request(Op::Join, 1);
        assert!(p.may_owe());
        // A refused first Join pinned nothing, and neither does a sample.
        p.complete(&q, None, 0).unwrap();
        assert!(!p.may_owe());
        let s = p.request(Op::State, 2);
        assert!(!p.may_owe());
        p.complete(&s, Some(reserved), 0).unwrap();
        let q = p.request(Op::Join, 3);
        assert!(p.may_owe());
        let revision = reserved.revision + 1;
        p.complete(&q, Some(Snapshot { enabled: true, revision, ..reserved }), 0).unwrap();
        assert!(p.joined() && p.may_owe());
        // A member that joined still owes, whatever becomes of a later Join.
        let q = p.request(Op::Join, 4);
        p.complete(&q, None, 0).unwrap();
        assert!(p.joined() && p.may_owe());
        // A served Join the owner rejects still pinned the member.
        let mut p = Participant::new(reserved, 0);
        let q = p.request(Op::Join, 1);
        assert_eq!(p.complete(&q, Some(reserved), 0), Err(Error::Invalid));
        assert!(!p.joined() && p.may_owe());
    }
    #[test]
    fn a_parked_seat_stops_counting_at_the_ceiling_however_fresh_the_replies() {
        let mut c = table();
        let reserved = c.reserve(peer(1)).unwrap();
        assert!(!Participant::new(reserved, 0).parked(0));
        let mut p = Participant::new(Snapshot { phase: QUIESCING, ..reserved }, 7);
        // A reservation can already read a seat that is away.
        assert!(p.parked(7));
        let mut p2 = Participant::new(reserved, 0);
        let s = p2.request(Op::State, 1);
        p2.complete(&s, Some(Snapshot { phase: FAILED, ..reserved }), 100).unwrap();
        assert!(p2.parked(100 + PARKED_CEILING_MS - 1));
        // A held key keeps FAILED live with replies still flowing.
        p2.complete(&s, Some(Snapshot { phase: FAILED, ..reserved }), 100 + PARKED_CEILING_MS - 1).unwrap();
        assert!(!p2.parked(100 + PARKED_CEILING_MS));
        // A wedged coordinator sends none: the same ceiling, from the last snapshot.
        assert!(p.parked(7 + PARKED_CEILING_MS - 1));
        assert!(!p.parked(7 + PARKED_CEILING_MS));
        // Back at normal, and the next episode has its full time.
        p2.complete(&s, Some(reserved), 200_000).unwrap();
        assert!(!p2.parked(200_000));
        p2.complete(&s, Some(Snapshot { phase: QUIESCING, ..reserved }), 300_000).unwrap();
        assert!(p2.parked(300_000 + PARKED_CEILING_MS - 1));
        // Stopping owes no wait for anyone.
        p.stop();
        assert!(!p.parked(7));
    }
    #[test]
    fn a_first_join_that_lost_to_a_chord_samples_and_joins_after_the_episode() {
        let mut c = table();
        let reserved = c.reserve(peer(1)).unwrap();
        let mut p = Participant::new(reserved, 0);
        c.observe(1, QUIESCING).unwrap();
        // The owner has joined nothing, so it owes nothing and holds up no one.
        assert_eq!(c.aggregate(), Some(1));
        let refused = exchange(&mut c, &mut p, Op::Join, 1).unwrap();
        assert_eq!(refused, Step { revoke: true, normal: None, send: Some(Op::State) });
        assert_eq!(p.normal(), None);
        let parked = exchange(&mut c, &mut p, Op::State, 1).unwrap();
        assert_eq!(parked.send, Some(Op::State), "an unpinned lane cannot send Cancelled");
        assert!(p.parked(0));
        c.observe(1, NORMAL).unwrap();
        let back = exchange(&mut c, &mut p, Op::State, 1).unwrap();
        assert_eq!(back.send, Some(Op::Join));
        assert_eq!(p.normal(), None, "a sample confers nothing");
        let joined = exchange(&mut c, &mut p, Op::Join, 1).unwrap();
        assert!(p.joined());
        assert_eq!(joined, Step { revoke: false, normal: Some(1), send: Some(Op::State) });
    }
    #[test]
    fn a_join_refused_twice_at_one_snapshot_is_fatal() {
        let mut c = table();
        let reserved = c.reserve(peer(1)).unwrap();
        let mut p = Participant::new(reserved, 0);
        let q = p.request(Op::Join, 1);
        assert_eq!(p.complete(&q, None, 0).unwrap().send, Some(Op::State));
        assert_eq!(p.complete(&q, None, 0), Err(Error::Denied));
    }
    #[test]
    fn a_refused_state_is_fatal_since_nothing_is_left_to_sample_with() {
        let mut c = table();
        let reserved = c.reserve(peer(1)).unwrap();
        let mut p = Participant::new(reserved, 0);
        let q = p.request(Op::State, 1);
        assert_eq!(p.complete(&q, None, 0), Err(Error::Denied));
    }
    #[test]
    fn a_join_answered_with_a_seat_away_from_normal_is_fatal() {
        let mut c = table();
        let reserved = c.reserve(peer(1)).unwrap();
        let mut p = Participant::new(reserved, 0);
        let q = p.request(Op::Join, 1);
        let reply = Snapshot { phase: QUIESCING, enabled: true, ..reserved };
        assert_eq!(p.complete(&q, Some(reply), 0), Err(Error::Invalid));
        let reply = Snapshot { enabled: false, ..reserved };
        assert_eq!(p.complete(&q, Some(reply), 0), Err(Error::Invalid));
    }
    #[test]
    fn a_sample_confers_nothing_even_if_the_reply_claims_enabled() {
        let mut c = table();
        let reserved = c.reserve(peer(1)).unwrap();
        let mut p = Participant::new(reserved, 0);
        let q = p.request(Op::State, 1);
        let step = p.complete(&q, Some(Snapshot { enabled: true, ..reserved }), 0).unwrap();
        assert_eq!(step.normal, None);
        assert_eq!(p.normal(), None);
    }
    #[test]
    fn a_joined_participant_cancels_once_then_rejoins_at_the_new_generation() {
        let mut c = table();
        let reserved = c.reserve(peer(1)).unwrap();
        let mut p = Participant::new(reserved, 0);
        exchange(&mut c, &mut p, Op::Join, 1).unwrap();
        c.observe(1, QUIESCING).unwrap();
        assert_eq!(c.aggregate(), None);
        let owed = exchange(&mut c, &mut p, Op::State, 1).unwrap();
        assert_eq!(owed, Step { revoke: true, normal: None, send: Some(Op::Cancelled) });
        let paid = exchange(&mut c, &mut p, Op::Cancelled, 1).unwrap();
        assert_eq!(paid.send, Some(Op::State), "the same snapshot is not cancelled twice");
        assert_eq!(c.aggregate(), Some(1));
        c.observe(1, FAILED).unwrap();
        c.observe(2, NORMAL).unwrap();
        let back = exchange(&mut c, &mut p, Op::State, 1).unwrap();
        assert_eq!(back, Step { revoke: true, normal: None, send: Some(Op::Join) });
        assert_eq!(exchange(&mut c, &mut p, Op::Join, 1).unwrap().normal, Some(2));
    }
    #[test]
    fn a_refused_rejoin_revokes_and_the_resample_still_owes_its_cancellation() {
        let mut c = table();
        let reserved = c.reserve(peer(1)).unwrap();
        let mut p = Participant::new(reserved, 0);
        exchange(&mut c, &mut p, Op::Join, 1).unwrap();
        c.observe(0, FAILED).unwrap();
        c.observe(1, NORMAL).unwrap();
        assert_eq!(exchange(&mut c, &mut p, Op::State, 1).unwrap().send, Some(Op::Join));
        c.observe(2, QUIESCING).unwrap();
        let refused = exchange(&mut c, &mut p, Op::Join, 1).unwrap();
        assert_eq!(refused, Step { revoke: true, normal: None, send: Some(Op::State) });
        let owed = exchange(&mut c, &mut p, Op::State, 1).unwrap();
        assert_eq!(owed.send, Some(Op::Cancelled));
        assert_eq!(c.aggregate(), None);
    }
    #[test]
    fn a_stopping_participant_retires_only_what_it_joined() {
        let mut c = table();
        let reserved = c.reserve(peer(1)).unwrap();
        let mut p = Participant::new(reserved, 0);
        c.observe(1, QUIESCING).unwrap();
        exchange(&mut c, &mut p, Op::Join, 1).unwrap();
        p.stop();
        assert!(!p.parked(0));
        let done = exchange(&mut c, &mut p, Op::State, 1).unwrap();
        assert_eq!(done, Step { revoke: true, normal: None, send: None });
        assert!(p.retired());

        let mut c = table();
        let reserved = c.reserve(peer(1)).unwrap();
        let mut p = Participant::new(reserved, 0);
        exchange(&mut c, &mut p, Op::Join, 1).unwrap();
        p.stop();
        assert_eq!(p.normal(), None);
        let retire = exchange(&mut c, &mut p, Op::State, 1).unwrap();
        assert_eq!(retire, Step { revoke: true, normal: None, send: Some(Op::Retire) });
        assert!(!p.retired());
        assert_eq!(exchange(&mut c, &mut p, Op::Retire, 1).unwrap().send, None);
        assert!(p.retired());
    }
    #[test]
    fn a_sample_is_refused_to_another_peer_and_pins_nothing() {
        let mut c = table();
        let s = c.reserve(peer(1)).unwrap();
        assert_eq!(c.sample(s.registration, 2, 2), Err(Error::Denied));
        assert_eq!(c.sample(s.registration, 1, 0), Err(Error::Denied));
        assert_eq!(c.state(s.registration, 1, 1), Err(Error::Denied), "the plain read still needs a pin");
        assert!(c.sample(s.registration, 1, 1).is_ok());
        let sampled = c.sample(s.registration, 1, 1).unwrap();
        assert!(!sampled.enabled);
        // A different lane of the same peer can still be the one that joins.
        c.join(s.registration, 1, 2, sampled.generation, sampled.revision).unwrap();
        assert_eq!(c.sample(s.registration, 1, 1), Err(Error::Denied), "pinned now");
        assert!(c.sample(s.registration, 1, 2).is_ok());
    }
    #[test]
    fn a_chord_under_a_rejoin_is_refused_and_the_resample_still_owes_its_cancellation() {
        let mut c = table();
        let s = join(&mut c, 1);
        c.observe(s.generation, FAILED).unwrap();
        c.observe(s.generation + 1, NORMAL).unwrap();
        let sampled = c.state(s.registration, 1, 1).unwrap();
        assert!(!sampled.enabled);
        c.observe(s.generation + 2, QUIESCING).unwrap();
        assert_eq!(c.join(s.registration, 1, 1, sampled.generation, sampled.revision), Err(Error::Denied));
        assert_eq!(c.aggregate(), None);
        let fresh = c.state(s.registration, 1, 1).unwrap();
        assert_eq!(fresh.phase, QUIESCING);
        c.cancelled(s.registration, 1, 1, fresh.generation, fresh.revision).unwrap();
        assert_eq!(c.aggregate(), Some(s.generation + 2));
    }
    #[test]
    fn a_recovery_at_a_new_generation_rejoins() {
        let mut c = table();
        let s = join(&mut c, 1);
        c.observe(s.generation, FAILED).unwrap();
        c.observe(s.generation + 1, NORMAL).unwrap();
        let fresh = c.state(s.registration, 1, 1).unwrap();
        assert!(!fresh.enabled);
        assert!(c.join(s.registration, 1, 1, fresh.generation, fresh.revision).unwrap().enabled);
    }
    #[test]
    fn a_sample_that_missed_failed_still_owes_cancellation() {
        let mut c = table();
        let a = join(&mut c, 1);
        c.observe(1, QUIESCING).unwrap();
        c.observe(2, NORMAL).unwrap();
        c.observe(3, QUIESCING).unwrap();
        assert_eq!(c.aggregate(), None);
        cancel(&mut c, a, 1);
        assert_eq!(c.aggregate(), Some(3));
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
        c.observe(2, NORMAL).unwrap();
        assert!(!c.admitted(peer(1), a.registration));
        c.observe(3, QUIESCING).unwrap();
        assert_eq!(c.aggregate(), None);
        cancel(&mut c, a, 1);
        assert_eq!(c.aggregate(), Some(3));
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
