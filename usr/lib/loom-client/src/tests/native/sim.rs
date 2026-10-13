//! A simulated kernel for the native adapter. ENTER, poll, REGISTER and close
//! follow kernel/loom_service_frontend.c, loom_service_decode.c and loom.c:
//! ENTER never sleeps and stops at a full CQ or a pending rejection, copies each
//! SQE before releasing sq_head and copies a WALK name before it returns; poll
//! reports POLLIN for a non-empty CQ and POLLHUP while closing. The service is
//! a strict 9P server with a flat file tree, answering on the next poll.
use crate::native::{Kernel, POLLHUP, POLLIN, POLLNVAL, Setup};
use crate::ring_records::{
    BufferRange, CQE_MORE, OP_CLUNK, OP_LOPEN, OP_READ, OP_WALK, OP_WRITE, SQE_MULTISHOT,
    Submission,
};
use crate::service_abi::{
    BufferReceipt, BufferReturn, CQE_SERVICE_BUFFER, Control, OP_SERVICE_CONNECT, PoolCreate,
    PoolMember, Reservation, SQE_BUFFER_SELECT, SlotRef, Snapshot, Target,
};
use std::{
    alloc::{Layout, alloc_zeroed, dealloc},
    cell::RefCell,
    collections::{HashMap, VecDeque},
    rc::Rc,
    sync::atomic::{AtomicU32, Ordering},
};

pub const EBADF: i64 = -9;
pub const EBUSY: i64 = -16;
pub const EINVAL: i64 = -22;
pub const ENOENT: i64 = -2;
pub const ENOSPC: i64 = -28;
pub const ENOTDIR: i64 = -20;
pub const ETIMEDOUT: i64 = -110;
pub const ECANCELED: i64 = -125;

pub const RESERVED: u32 = 1;
pub const READY: u32 = 5;
pub const ABORTING: u32 = 6;
pub const RETIRED: u32 = 7;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SlotKind {
    Target,
    Scope,
    Fid,
    Pool,
}
#[derive(Clone, Debug)]
pub struct Slot {
    pub kind: SlotKind,
    pub inc: u64,
    pub state: u32,
    /// A fid's scope slot; a scope's own.
    pub scope: usize,
    /// A fid's file, None for the root; a target's service name.
    pub name: Option<Vec<u8>>,
    pub open: bool,
}
#[derive(Clone, Debug)]
enum OpKind {
    Connect {
        target: usize,
    },
    Walk {
        dest: usize,
        name: Vec<u8>,
    },
    Open,
    Read {
        buf: u32,
        at: u64,
        len: u32,
    },
    Write {
        buf: u32,
        at: u64,
        len: u32,
    },
    Pooled {
        pool: usize,
        limit: u32,
        multi: bool,
    },
    Clunk,
}
#[derive(Clone, Debug)]
struct Op {
    user_data: u64,
    scope: usize,
    source: usize,
    kind: OpKind,
    offset: u64,
    deadline: u64,
}
struct Member {
    range: PoolMember,
    lease: Option<u64>,
}
struct Ring {
    fd: i32,
    va: usize,
    p: Setup,
    closing: bool,
    sq_head: u32,
    cq_tail: u32,
}
pub struct State {
    pub clock: u64,
    regions: HashMap<usize, Layout>,
    pub attach_fail_at: Option<usize>,
    attaches: usize,
    ring: Option<Ring>,
    pub slots: [Option<Slot>; 64],
    next_inc: u64,
    buffers: Vec<(usize, usize)>,
    pools: HashMap<usize, Vec<Member>>,
    ops: VecDeque<Op>,
    /// Completions waiting for CQ room, each with the scope of its operation
    /// (none for an SQE ENTER refused).
    deferred: VecDeque<(Option<usize>, u64, i32, u32, BufferReceipt)>,
    pub files: HashMap<Vec<u8>, Vec<u8>>,
    pub shots: HashMap<Vec<u8>, VecDeque<Vec<u8>>>,
    pub services: Vec<Vec<u8>>,
    pub registry_fd: i32,
    /// The server answers nothing while held.
    pub hold: bool,
    /// An abort's terminals wait for release_terminals: the kernel retires a
    /// scope in the visit that cancels its operations and posts their
    /// terminals on a later one.
    pub hold_terminals: bool,
    held: Vec<(usize, u64, i32)>,
    /// ENTER's refusals wait for release_rejections: the kernel precharges a
    /// refusal it cannot post into a full CQ and posts it on a later visit.
    pub hold_rejections: bool,
    rejected: Vec<(u64, i32)>,
    /// ENTER consumes at most this many SQEs per call.
    pub enter_limit: Option<u32>,
    pub polls: Vec<i32>,
    pub enters: usize,
    pub aborts: Vec<usize>,
    pub reaps: Vec<usize>,
    pub clunks: usize,
    pub clunk_sqes: usize,
    pub returns: usize,
    pub closed_fds: Vec<i32>,
    next_lease: u64,
    /// Bad sq_array indices seen; the kernel's `dropped`.
    pub dropped: u32,
    /// Overwrite the next CQE's user_data, to forge a stale completion.
    pub forge_next: Option<u64>,
}
#[derive(Clone)]
pub struct Sim(pub Rc<RefCell<State>>);
pub struct SimKernel(pub Sim);

impl Sim {
    pub fn new() -> Self {
        Sim(Rc::new(RefCell::new(State {
            clock: 1_000_000_000,
            regions: HashMap::new(),
            attach_fail_at: None,
            attaches: 0,
            ring: None,
            slots: std::array::from_fn(|_| None),
            next_inc: 1,
            buffers: Vec::new(),
            pools: HashMap::new(),
            ops: VecDeque::new(),
            deferred: VecDeque::new(),
            files: HashMap::new(),
            shots: HashMap::new(),
            services: vec![b"clip".to_vec()],
            registry_fd: 5,
            hold: false,
            hold_terminals: false,
            held: Vec::new(),
            hold_rejections: false,
            rejected: Vec::new(),
            enter_limit: None,
            polls: Vec::new(),
            enters: 0,
            aborts: Vec::new(),
            reaps: Vec::new(),
            clunks: 0,
            clunk_sqes: 0,
            returns: 0,
            closed_fds: Vec::new(),
            next_lease: 1,
            dropped: 0,
            forge_next: None,
        })))
    }
    pub fn kernel(&self) -> SimKernel {
        SimKernel(self.clone())
    }
    pub fn s(&self) -> std::cell::RefMut<'_, State> {
        self.0.borrow_mut()
    }
    pub fn regions(&self) -> usize {
        self.0.borrow().regions.len()
    }
    pub fn live_slots(&self) -> usize {
        self.0.borrow().slots.iter().flatten().count()
    }
    pub fn pending_ops(&self) -> usize {
        self.0.borrow().ops.len()
    }
    /// One visit of the kernel's engine, which runs beside the adapter: it
    /// retires every aborted scope with nothing in flight.
    pub fn engine_visit(&self) {
        self.0.borrow_mut().settle();
    }
    /// Post the refusals hold_rejections kept back, and stop holding.
    pub fn release_rejections(&self) {
        let mut s = self.0.borrow_mut();
        s.hold_rejections = false;
        for (user_data, result) in std::mem::take(&mut s.rejected) {
            s.post(None, user_data, result, 0, zero());
        }
    }
    /// Post the terminals hold_terminals kept back, and stop holding.
    pub fn release_terminals(&self) {
        let mut s = self.0.borrow_mut();
        s.hold_terminals = false;
        for (scope, user_data, result) in std::mem::take(&mut s.held) {
            s.post(Some(scope), user_data, result, 0, zero());
        }
    }
    /// Another thread closes the ring's descriptor: the number is gone, so
    /// ENTER answers -EBADF and poll POLLNVAL.
    pub fn close_under(&self) {
        let mut s = self.0.borrow_mut();
        let fd = s.ring().fd;
        s.closed_fds.push(fd);
        s.ring.as_mut().unwrap().closing = true;
        s.abort_all();
    }
    /// The ring latches closing while the descriptor still resolves: a close
    /// racing ENTER (-ECANCELED) or poll (POLLHUP).
    pub fn latch_under(&self) {
        let mut s = self.0.borrow_mut();
        s.ring.as_mut().unwrap().closing = true;
        s.abort_all();
    }
    pub fn hdr_word(&self, i: usize) -> u32 {
        let s = self.0.borrow();
        s.word(i).load(Ordering::Acquire)
    }
    pub fn poke_hdr(&self, i: usize, v: u32) {
        let s = self.0.borrow();
        s.word(i).store(v, Ordering::Release);
    }
}

fn shape(s: &Submission, pools: bool) -> i64 {
    let no_buffer = s.len == 0 && s.buf_idx_or_off == 0 && s.args[0] == 0;
    let fixed = s.buf_idx_or_off < 64 && s.args[0] <= u64::MAX - s.len as u64;
    let dest = s.offset < 64 && s.args[2] != 0 && s.offset != s.handle_idx as u64;
    if s.reserved != 0 || s.handle_idx >= 64 || s.args[1] == 0 {
        return EINVAL;
    }
    let pooled = s.opcode == OP_READ && s.flags as u32 & SQE_BUFFER_SELECT != 0;
    let allowed = if pooled {
        SQE_BUFFER_SELECT as u8 | SQE_MULTISHOT
    } else {
        0
    };
    if s.flags & !allowed != 0 {
        return EINVAL;
    }
    match s.opcode as u32 {
        OP_SERVICE_CONNECT if !no_buffer || !dest => EINVAL,
        OP_SERVICE_CONNECT => 0,
        o if o == OP_WALK as u32 => {
            if !dest || s.len > 255 || if s.len != 0 { !fixed } else { !no_buffer } {
                EINVAL
            } else {
                0
            }
        }
        o if o == OP_LOPEN as u32 => {
            if s.offset > 2 || s.args[2] != 0 || !no_buffer {
                EINVAL
            } else {
                0
            }
        }
        o if o == OP_READ as u32 && pooled => {
            if !pools {
                -95
            } else if s.len == 0 || s.buf_idx_or_off >= 64 || s.args[2] == 0 || s.args[0] != 0 {
                EINVAL
            } else {
                0
            }
        }
        o if o == OP_READ as u32 || o == OP_WRITE as u32 => {
            if s.args[2] != 0 || !fixed {
                EINVAL
            } else {
                0
            }
        }
        o if o == OP_CLUNK as u32 => {
            if s.offset != 0 || s.args[2] != 0 || !no_buffer {
                EINVAL
            } else {
                0
            }
        }
        _ => -95,
    }
}

impl State {
    fn ring(&self) -> &Ring {
        self.ring.as_ref().expect("a ring")
    }
    fn base(&self) -> *mut u8 {
        self.ring().va as *mut u8
    }
    fn word(&self, i: usize) -> &AtomicU32 {
        let r = self.ring();
        unsafe {
            &*(r.va as *mut u8)
                .add(r.p.hdr_off as usize + 4 * i)
                .cast::<AtomicU32>()
        }
    }
    fn cq_room(&self) -> bool {
        let r = self.ring();
        r.cq_tail.wrapping_sub(self.word(4).load(Ordering::Acquire)) < r.p.cq_entries
    }
    fn post(
        &mut self,
        scope: Option<usize>,
        user_data: u64,
        result: i32,
        flags: u32,
        receipt: BufferReceipt,
    ) {
        self.deferred
            .push_back((scope, user_data, result, flags, receipt));
        self.flush();
    }
    /// Post deferred completions in order while the CQ has room.
    fn flush(&mut self) {
        while self.cq_room() {
            let Some((_, user_data, result, flags, receipt)) = self.deferred.pop_front() else {
                break;
            };
            let user_data = self.forge_next.take().unwrap_or(user_data);
            let base = self.base();
            let r = self.ring.as_mut().unwrap();
            let at = (r.cq_tail & (r.p.cq_entries - 1)) as usize;
            unsafe {
                let cqe = base.add(r.p.cqe_off as usize + at * 16);
                cqe.cast::<u64>().write(user_data);
                cqe.add(8).cast::<i32>().write(result);
                cqe.add(12).cast::<u32>().write(flags);
                base.add(r.p.receipt[0] as usize + at * 32)
                    .cast::<BufferReceipt>()
                    .write(receipt);
            }
            r.cq_tail = r.cq_tail.wrapping_add(1);
            let tail = r.cq_tail;
            self.word(5).store(tail, Ordering::Release);
        }
    }
    fn mint(&mut self, slot: Slot) -> Option<SlotRef> {
        let i = self.slots.iter().position(Option::is_none)?;
        let inc = self.next_inc;
        self.next_inc += 1;
        self.slots[i] = Some(Slot { inc, ..slot });
        Some(SlotRef {
            slot: i as u32,
            reserved: 0,
            incarnation: inc,
        })
    }
    fn live(&self, r: SlotRef) -> Option<&Slot> {
        self.slots
            .get(r.slot as usize)?
            .as_ref()
            .filter(|s| s.inc == r.incarnation && r.reserved == 0)
    }
    fn cancel_scope(&mut self, scope: usize, result: i64) {
        let (gone, kept): (Vec<Op>, Vec<Op>) = self.ops.drain(..).partition(|o| o.scope == scope);
        self.ops = kept.into();
        for op in gone {
            if self.hold_terminals {
                self.held.push((scope, op.user_data, result as i32));
            } else {
                self.post(Some(scope), op.user_data, result as i32, 0, zero());
            }
        }
        if let Some(s) = self.slots[scope].as_mut() {
            if s.state != RETIRED && s.state != RESERVED {
                s.state = ABORTING;
            }
        }
    }
    fn abort_all(&mut self) {
        for i in 0..64 {
            if self.slots[i]
                .as_ref()
                .is_some_and(|s| s.kind == SlotKind::Scope)
            {
                self.cancel_scope(i, ECANCELED);
            }
        }
    }
    /// Retire aborted scopes with nothing in flight. A fid never retires with
    /// its scope: REAP retires it (loom_fid_reap_retired).
    fn settle(&mut self) {
        for i in 0..64 {
            let busy = self.ops.iter().any(|o| o.scope == i);
            if let Some(s) = self.slots[i].as_mut() {
                if s.kind == SlotKind::Scope && s.state == ABORTING && !busy {
                    s.state = RETIRED;
                }
            }
        }
    }
    /// loom_private_service_reap: a target or pool once unborrowed; a scope once
    /// unused or RETIRED and no fid pins it; a fid once unused, clunked, or of a
    /// RETIRED scope, and unborrowed.
    fn reapable(&self, i: usize, sl: &Slot) -> bool {
        let borrowed = self.ops.iter().any(|o| {
            o.source == i || o.scope == i || matches!(o.kind, OpKind::Connect { target } if target == i)
                || matches!(o.kind, OpKind::Pooled { pool, .. } if pool == i)
                // loom_fid_claim_walk pins the destination until the WALK ends.
                || matches!(o.kind, OpKind::Walk { dest, .. } if dest == i)
        });
        if borrowed {
            return false;
        }
        match sl.kind {
            SlotKind::Target => true,
            SlotKind::Pool => self.pools[&i].iter().all(|m| m.lease.is_none()),
            SlotKind::Scope => {
                let pinned = self.slots.iter().enumerate().any(|(j, f)| {
                    j != i
                        && f.as_ref()
                            .is_some_and(|f| f.kind == SlotKind::Fid && f.scope == i)
                });
                !pinned && (sl.state == RESERVED || sl.state == RETIRED)
            }
            SlotKind::Fid => {
                sl.state == RESERVED
                    || sl.state == RETIRED
                    || self.slots[sl.scope]
                        .as_ref()
                        .is_some_and(|p| p.state == RETIRED)
            }
        }
    }
    /// The server's turn: deadlines first, then every pending answer.
    pub fn step(&mut self) {
        let now = self.clock;
        let expired: Vec<usize> = self
            .ops
            .iter()
            .filter(|o| o.deadline != 0 && o.deadline <= now)
            .map(|o| o.scope)
            .collect();
        for scope in expired {
            // Any operation timeout terminally aborts its whole scope.
            let (gone, kept): (Vec<Op>, Vec<Op>) =
                self.ops.drain(..).partition(|o| o.scope == scope);
            self.ops = kept.into();
            for op in gone {
                let r = if op.deadline != 0 && op.deadline <= now {
                    ETIMEDOUT
                } else {
                    ECANCELED
                };
                self.post(Some(scope), op.user_data, r as i32, 0, zero());
            }
            if let Some(s) = self.slots[scope].as_mut() {
                s.state = ABORTING;
            }
        }
        if !self.hold {
            let n = self.ops.len();
            for _ in 0..n {
                let op = self.ops.pop_front().unwrap();
                if let Some(op) = self.answer(op) {
                    self.ops.push_back(op);
                }
            }
        }
        self.settle();
        self.flush();
    }
    fn buffer(&self, index: u32, at: u64, len: u32) -> Option<*mut u8> {
        let &(va, n) = self.buffers.get(index as usize)?;
        if at.checked_add(len as u64)? > n as u64 {
            return None;
        }
        Some((va + at as usize) as *mut u8)
    }
    /// Answer one operation; Some means it is still running.
    fn answer(&mut self, op: Op) -> Option<Op> {
        let file = self.slots[op.source].as_ref().and_then(|s| s.name.clone());
        let rc: i64 = match &op.kind {
            OpKind::Connect { target } => {
                let name = self.slots[*target]
                    .as_ref()
                    .and_then(|t| t.name.clone())
                    .unwrap();
                let scope = self.slots[op.scope].as_mut().unwrap();
                if self.services.contains(&name) {
                    scope.state = READY;
                    op.scope as i64
                } else {
                    scope.state = RETIRED;
                    ENOENT
                }
            }
            OpKind::Walk { dest, name } => {
                if file.is_some() && !name.is_empty() {
                    ENOTDIR
                } else if name.is_empty() || self.files.contains_key(name) {
                    let d = self.slots[*dest].as_mut().unwrap();
                    d.state = READY;
                    d.name = if name.is_empty() {
                        file
                    } else {
                        Some(name.clone())
                    };
                    *dest as i64
                } else {
                    ENOENT
                }
            }
            OpKind::Open => {
                self.slots[op.source].as_mut().unwrap().open = true;
                0
            }
            OpKind::Read { buf, at, len } => {
                let data = self
                    .files
                    .get(file.as_ref().unwrap())
                    .cloned()
                    .unwrap_or_default();
                let start = (op.offset as usize).min(data.len());
                let n = (data.len() - start).min(*len as usize);
                let dst = self.buffer(*buf, *at, *len).unwrap();
                unsafe { std::ptr::copy_nonoverlapping(data[start..].as_ptr(), dst, n) };
                n as i64
            }
            OpKind::Write { buf, at, len } => {
                let src = self.buffer(*buf, *at, *len).unwrap();
                let bytes = unsafe { std::slice::from_raw_parts(src, *len as usize) }.to_vec();
                let f = self.files.entry(file.clone().unwrap()).or_default();
                let end = op.offset as usize + bytes.len();
                if f.len() < end {
                    f.resize(end, 0);
                }
                f[op.offset as usize..end].copy_from_slice(&bytes);
                *len as i64
            }
            OpKind::Pooled { pool, limit, multi } => {
                let (pool, limit, multi) = (*pool, *limit, *multi);
                let name = file.clone().unwrap();
                loop {
                    let next = if multi {
                        self.shots.get_mut(&name).and_then(|q| q.pop_front())
                    } else {
                        let data = self.files.get(&name).cloned().unwrap_or_default();
                        let start = (op.offset as usize).min(data.len());
                        let n = (data.len() - start).min(limit as usize);
                        if n == 0 {
                            None
                        } else {
                            Some(data[start..start + n].to_vec())
                        }
                    };
                    let Some(shot) = next else {
                        self.post(Some(op.scope), op.user_data, 0, 0, zero());
                        return None;
                    };
                    assert!(shot.len() as u32 <= limit, "a scripted shot fits its limit");
                    let inc = self.slots[pool].as_ref().unwrap().inc;
                    let lease = self.next_lease;
                    let members = self.pools.get_mut(&pool).unwrap();
                    let Some(m) = members
                        .iter()
                        .position(|m| m.lease.is_none() && m.range.length >= shot.len() as u64)
                    else {
                        // No free member: the stream waits for a RETURN.
                        if multi {
                            self.shots.get_mut(&name).unwrap().push_front(shot);
                        }
                        return Some(op);
                    };
                    members[m].lease = Some(lease);
                    let range = members[m].range;
                    self.next_lease += 1;
                    let dst = self
                        .buffer(range.buffer_index, range.offset, shot.len() as u32)
                        .unwrap();
                    unsafe { std::ptr::copy_nonoverlapping(shot.as_ptr(), dst, shot.len()) };
                    let receipt = BufferReceipt {
                        pool: SlotRef {
                            slot: pool as u32,
                            reserved: 0,
                            incarnation: inc,
                        },
                        member: m as u32,
                        reserved: 0,
                        lease,
                    };
                    let more = if multi { CQE_MORE } else { 0 };
                    self.post(
                        Some(op.scope),
                        op.user_data,
                        shot.len() as i32,
                        CQE_SERVICE_BUFFER | more,
                        receipt,
                    );
                    if !multi {
                        return None;
                    }
                }
            }
            OpKind::Clunk => {
                self.clunks += 1;
                self.slots[op.source].as_mut().unwrap().state = RETIRED;
                0
            }
        };
        self.post(Some(op.scope), op.user_data, rc as i32, 0, zero());
        None
    }
    /// ENTER's admission of one copied SQE: shape, then the slots it names.
    fn admit(&mut self, s: Submission) -> i64 {
        let rc = shape(&s, true);
        if rc != 0 {
            return rc;
        }
        let source = SlotRef {
            slot: s.handle_idx,
            reserved: 0,
            incarnation: s.args[1],
        };
        let Some(src) = self.live(source).cloned() else {
            return ENOENT;
        };
        let dest = SlotRef {
            slot: s.offset as u32,
            reserved: 0,
            incarnation: s.args[2],
        };
        let (scope, kind) = match s.opcode as u32 {
            OP_SERVICE_CONNECT => {
                let Some(d) = self.live(dest) else {
                    return ENOENT;
                };
                if src.kind != SlotKind::Target || d.kind != SlotKind::Scope || d.state != RESERVED
                {
                    return ENOENT;
                }
                (
                    dest.slot as usize,
                    OpKind::Connect {
                        target: source.slot as usize,
                    },
                )
            }
            o => {
                if o == OP_CLUNK as u32 {
                    self.clunk_sqes += 1;
                }
                let scope = match src.kind {
                    SlotKind::Scope => source.slot as usize,
                    SlotKind::Fid => src.scope,
                    _ => return ENOENT,
                };
                if src.state != READY || self.slots[scope].as_ref().unwrap().state != READY {
                    return ECANCELED;
                }
                let kind = if o == OP_WALK as u32 {
                    let Some(d) = self.live(dest) else {
                        return ENOENT;
                    };
                    if d.kind != SlotKind::Fid || d.state != RESERVED || d.scope != scope {
                        return ENOENT;
                    }
                    let name = if s.len == 0 {
                        Vec::new()
                    } else {
                        let Some(p) = self.buffer(s.buf_idx_or_off, s.args[0], s.len) else {
                            return EINVAL;
                        };
                        // Copied here, inside ENTER, as loom_private_io_prepare does.
                        unsafe { std::slice::from_raw_parts(p, s.len as usize) }.to_vec()
                    };
                    if name.iter().any(|&b| b == 0 || b == b'/') || name == b"." || name == b".." {
                        return EINVAL;
                    }
                    OpKind::Walk {
                        dest: dest.slot as usize,
                        name,
                    }
                } else if o == OP_LOPEN as u32 {
                    OpKind::Open
                } else if o == OP_CLUNK as u32 {
                    OpKind::Clunk
                } else if s.flags as u32 & SQE_BUFFER_SELECT != 0 {
                    let pool = SlotRef {
                        slot: s.buf_idx_or_off,
                        reserved: 0,
                        incarnation: s.args[2],
                    };
                    if self.live(pool).is_none_or(|p| p.kind != SlotKind::Pool) {
                        return ENOENT;
                    }
                    OpKind::Pooled {
                        pool: pool.slot as usize,
                        limit: s.len,
                        multi: s.flags & SQE_MULTISHOT != 0,
                    }
                } else {
                    if self.buffer(s.buf_idx_or_off, s.args[0], s.len).is_none() {
                        return EINVAL;
                    }
                    let (buf, at, len) = (s.buf_idx_or_off, s.args[0], s.len);
                    if o == OP_READ as u32 {
                        OpKind::Read { buf, at, len }
                    } else {
                        OpKind::Write { buf, at, len }
                    }
                };
                (scope, kind)
            }
        };
        self.ops.push_back(Op {
            user_data: s.user_data,
            scope,
            source: source.slot as usize,
            kind,
            offset: s.offset,
            deadline: s.args[3],
        });
        0
    }
}
fn zero() -> BufferReceipt {
    BufferReceipt {
        pool: SlotRef {
            slot: 0,
            reserved: 0,
            incarnation: 0,
        },
        member: 0,
        reserved: 0,
        lease: 0,
    }
}
fn header_ok(size: u32, want: u32, version: u16, flags: u16) -> bool {
    size == want && version == 1 && flags == 0
}

unsafe impl Kernel for SimKernel {
    fn setup(&self, entries: u32, p: &mut Setup) -> i64 {
        let mut s = self.0.s();
        if p.flags != 12 || entries == 0 || entries > 4096 || !entries.is_power_of_two() {
            return EINVAL;
        }
        let cq = entries * 2;
        let up = |n: u32| (n + 63) & !63;
        let sq_array_off = 64;
        let sqe_off = up(sq_array_off + entries * 4);
        let cqe_off = up(sqe_off + entries * 64);
        let receipt_off = up(cqe_off + cq * 16);
        let size = (receipt_off + cq * 32 + 4095) & !4095;
        let layout = Layout::from_size_align(size as usize, 4096).unwrap();
        let va = unsafe { alloc_zeroed(layout) } as usize;
        s.regions.insert(va, layout);
        *p = Setup {
            flags: 12,
            sq_entries: entries,
            cq_entries: cq,
            ring_size: size,
            ring_va: va as u64,
            hdr_off: 0,
            sq_array_off,
            sqe_off,
            cqe_off,
            sq_array_size: entries * 4,
            sqe_size: entries * 64,
            cqe_size: cq * 16,
            reserved: 0,
            receipt: [receipt_off as u64, cq as u64 * 32, 32, 0],
        };
        s.ring = Some(Ring {
            fd: 7,
            va,
            p: *p,
            closing: false,
            sq_head: 0,
            cq_tail: 0,
        });
        for (i, v) in [(2, entries - 1), (3, entries), (6, cq - 1), (7, cq)] {
            s.word(i).store(v, Ordering::Release);
        }
        7
    }
    unsafe fn register(&self, fd: i32, op: u32, arg: *mut u8, nargs: u32) -> i64 {
        let mut s = self.0.s();
        if s.ring.as_ref().is_none_or(|r| r.fd != fd) || s.closed_fds.contains(&fd) {
            return EBADF;
        }
        if op == 1 {
            if nargs > 64 || !s.pools.is_empty() {
                return if nargs > 64 { EINVAL } else { EBUSY };
            }
            let table =
                unsafe { std::slice::from_raw_parts(arg.cast::<BufferRange>(), nargs as usize) };
            let mut fresh = Vec::new();
            for b in table {
                let inside = s.regions.iter().any(|(&va, l)| {
                    b.va as usize >= va && b.va as usize + b.len as usize <= va + l.size()
                });
                if !inside || b.va as usize == s.ring().va {
                    return EINVAL;
                }
                fresh.push((b.va as usize, b.len as usize));
            }
            s.buffers = fresh;
            return 0;
        }
        if nargs != 1 {
            return EINVAL;
        }
        let closing = s.ring().closing;
        match op {
            2 => {
                let t = unsafe { &mut *arg.cast::<Target>() };
                if !header_ok(t.size, 288, t.version, t.flags)
                    || t.name_len == 0
                    || t.name_len > 255
                {
                    return EINVAL;
                }
                if closing {
                    return ECANCELED;
                }
                if t.registry_fd != s.registry_fd {
                    return EBADF;
                }
                let name = t.name[..t.name_len as usize].to_vec();
                if name.iter().any(|&b| b == 0 || b == b'/') {
                    return EINVAL;
                }
                let Some(r) = s.mint(Slot {
                    kind: SlotKind::Target,
                    inc: 0,
                    state: READY,
                    scope: 0,
                    name: Some(name),
                    open: false,
                }) else {
                    return ENOSPC;
                };
                t.result = r;
                0
            }
            3 => {
                let rec = unsafe { &mut *arg.cast::<Reservation>() };
                if !header_ok(rec.size, 48, rec.version, rec.flags) || rec.reserved != 0 {
                    return EINVAL;
                }
                if closing {
                    return ECANCELED;
                }
                let slot = match rec.kind {
                    2 if rec.scope == zero().pool => Slot {
                        kind: SlotKind::Scope,
                        inc: 0,
                        state: RESERVED,
                        scope: 0,
                        name: None,
                        open: false,
                    },
                    3 => {
                        let Some(sc) = s.live(rec.scope) else {
                            return ENOENT;
                        };
                        if sc.kind != SlotKind::Scope || sc.state != READY {
                            return ENOENT;
                        }
                        Slot {
                            kind: SlotKind::Fid,
                            inc: 0,
                            state: RESERVED,
                            scope: rec.scope.slot as usize,
                            name: None,
                            open: false,
                        }
                    }
                    _ => return EINVAL,
                };
                let Some(r) = s.mint(slot) else { return ENOSPC };
                if rec.kind == 2 {
                    s.slots[r.slot as usize].as_mut().unwrap().scope = r.slot as usize;
                }
                rec.result = r;
                0
            }
            4 => {
                let c = unsafe { &*arg.cast::<Control>() };
                let Some(sl) = s.live(c.object).cloned() else {
                    return ENOENT;
                };
                if sl.kind != SlotKind::Scope {
                    return EINVAL;
                }
                s.aborts.push(c.object.slot as usize);
                if sl.state == ABORTING || sl.state == RETIRED {
                    return 1;
                }
                if sl.state == RESERVED {
                    s.slots[c.object.slot as usize].as_mut().unwrap().state = RETIRED;
                    let i = c.object.slot as usize;
                    s.cancel_scope(i, ECANCELED);
                    return 0;
                }
                let i = c.object.slot as usize;
                s.cancel_scope(i, ECANCELED);
                0
            }
            5 => {
                let q = unsafe { &mut *arg.cast::<Snapshot>() };
                let Some(sl) = s.live(q.object).cloned() else {
                    return ENOENT;
                };
                q.state = sl.state;
                if sl.kind == SlotKind::Scope {
                    // loom_private_service_query: operations not yet terminal,
                    // and terminals not yet posted.
                    let i = q.object.slot as usize;
                    q.active_ops = s.ops.iter().filter(|o| o.scope == i).count() as u32;
                    let deferred = s
                        .deferred
                        .iter()
                        .filter(|d| d.0 == Some(i) && d.3 & CQE_MORE == 0)
                        .count();
                    let held = s.held.iter().filter(|h| h.0 == i).count();
                    q.pending_terminals = (deferred + held) as u32;
                }
                q.kind = match sl.kind {
                    SlotKind::Target => 1,
                    SlotKind::Scope => 2,
                    SlotKind::Fid => 3,
                    SlotKind::Pool => 4,
                };
                0
            }
            6 => {
                let c = unsafe { &*arg.cast::<Control>() };
                let Some(sl) = s.live(c.object).cloned() else {
                    return ENOENT;
                };
                let i = c.object.slot as usize;
                if !s.reapable(i, &sl) {
                    return EBUSY;
                }
                s.slots[i] = None;
                s.pools.remove(&i);
                s.reaps.push(i);
                0
            }
            7 => {
                let p = unsafe { &mut *arg.cast::<PoolCreate>() };
                if !header_ok(p.size, 1568, p.version, p.flags) || p.count == 0 || p.count > 64 {
                    return EINVAL;
                }
                if closing {
                    return ECANCELED;
                }
                let members: Vec<Member> = p.members[..p.count as usize]
                    .iter()
                    .map(|&range| Member { range, lease: None })
                    .collect();
                for m in &members {
                    if s.buffer(m.range.buffer_index, m.range.offset, m.range.length as u32)
                        .is_none()
                    {
                        return EINVAL;
                    }
                }
                let Some(r) = s.mint(Slot {
                    kind: SlotKind::Pool,
                    inc: 0,
                    state: READY,
                    scope: 0,
                    name: None,
                    open: false,
                }) else {
                    return ENOSPC;
                };
                s.pools.insert(r.slot as usize, members);
                p.result = r;
                0
            }
            8 => {
                let b = unsafe { &*arg.cast::<BufferReturn>() };
                if !header_ok(b.size, 40, b.version, b.flags) {
                    return EINVAL;
                }
                let r = b.receipt;
                if s.live(r.pool).is_none() {
                    return ENOENT;
                }
                let Some(m) = s
                    .pools
                    .get_mut(&(r.pool.slot as usize))
                    .and_then(|p| p.get_mut(r.member as usize))
                else {
                    return EINVAL;
                };
                if m.lease != Some(r.lease) {
                    return EINVAL;
                }
                m.lease = None;
                s.returns += 1;
                0
            }
            _ => EINVAL,
        }
    }
    fn enter(&self, fd: i32, to_submit: u32, min_complete: u32, flags: u32) -> i64 {
        let mut s = self.0.s();
        if s.ring.as_ref().is_none_or(|r| r.fd != fd) || s.closed_fds.contains(&fd) {
            return EBADF;
        }
        if min_complete != 0 || flags != 2 {
            return EINVAL;
        }
        if s.ring().closing {
            return ECANCELED;
        }
        s.enters += 1;
        s.flush();
        let entries = s.ring().p.sq_entries;
        let budget = to_submit
            .min(entries)
            .min(s.enter_limit.unwrap_or(u32::MAX));
        let mut consumed = 0;
        for _ in 0..budget {
            let tail = s.word(1).load(Ordering::Acquire);
            if !s.cq_room() || !s.deferred.is_empty() || !s.rejected.is_empty() || tail == s.ring().sq_head {
                break;
            }
            let p = s.ring().p;
            let head = s.ring().sq_head;
            let index = unsafe {
                (*s.base()
                    .add(p.sq_array_off as usize + 4 * (head & (entries - 1)) as usize)
                    .cast::<AtomicU32>())
                .load(Ordering::Acquire)
            };
            let copy = (index < entries).then(|| unsafe {
                s.base()
                    .add(p.sqe_off as usize + 64 * index as usize)
                    .cast::<Submission>()
                    .read()
            });
            if copy.is_none() {
                s.dropped += 1;
                let d = s.dropped;
                s.word(9).store(d, Ordering::Release);
            }
            s.ring.as_mut().unwrap().sq_head = head.wrapping_add(1);
            s.word(0).store(head.wrapping_add(1), Ordering::Release);
            consumed += 1;
            if let Some(copy) = copy {
                let rc = s.admit(copy);
                if rc < 0 {
                    if s.hold_rejections {
                        s.rejected.push((copy.user_data, rc as i32));
                    } else {
                        s.post(None, copy.user_data, rc as i32, 0, zero());
                    }
                }
            }
        }
        consumed
    }
    fn poll(&self, fd: i32, events: i16, timeout_ms: i32) -> i64 {
        let mut s = self.0.s();
        assert_eq!(events, POLLIN);
        s.polls.push(timeout_ms);
        if s.ring.as_ref().is_none_or(|r| r.fd != fd) || s.closed_fds.contains(&fd) {
            return POLLNVAL as i64;
        }
        if s.ring().closing {
            return POLLHUP as i64;
        }
        let ready =
            |s: &State| s.word(5).load(Ordering::Acquire) != s.word(4).load(Ordering::Acquire);
        if ready(&s) {
            return POLLIN as i64;
        }
        s.step();
        if ready(&s) {
            return POLLIN as i64;
        }
        // Sleep until the timeout or the earliest operation deadline, whose
        // expiry the kernel posts as a completion.
        let due = s
            .ops
            .iter()
            .filter(|o| o.deadline != 0)
            .map(|o| o.deadline)
            .min();
        let until = (timeout_ms >= 0).then(|| s.clock + timeout_ms as u64 * 1_000_000);
        let wake = match (until, due) {
            (Some(a), Some(b)) => a.min(b),
            (a, b) => a
                .or(b)
                .expect("poll would block forever: nothing in flight can complete"),
        };
        s.clock = s.clock.max(wake);
        s.step();
        if ready(&s) { POLLIN as i64 } else { 0 }
    }
    fn close(&self, fd: i32) -> i64 {
        let mut s = self.0.s();
        assert!(
            !s.closed_fds.contains(&fd),
            "a second close of fd {fd}, which may name another file now"
        );
        if s.ring.as_ref().is_none_or(|r| r.fd != fd) {
            return EBADF;
        }
        s.closed_fds.push(fd);
        s.ring.as_mut().unwrap().closing = true;
        s.abort_all();
        0
    }
    fn attach(&self, len: usize) -> i64 {
        let mut s = self.0.s();
        s.attaches += 1;
        if s.attach_fail_at == Some(s.attaches) {
            return -12;
        }
        assert!(
            len > 0 && len % 4096 == 0,
            "the adapter attaches whole pages"
        );
        let layout = Layout::from_size_align(len, 4096).unwrap();
        let va = unsafe { alloc_zeroed(layout) } as usize;
        s.regions.insert(va, layout);
        va as i64
    }
    fn detach(&self, va: usize, len: usize) -> i64 {
        let mut s = self.0.s();
        let layout = s
            .regions
            .remove(&va)
            .expect("detach names an attached region");
        assert_eq!(layout.size(), len, "detach names the whole region");
        unsafe { dealloc(va as *mut u8, layout) };
        0
    }
    fn now(&self) -> u64 {
        self.0.0.borrow().clock
    }
}
