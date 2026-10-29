//! Independent cancellation control owner. It never borrows Comp, surfaces,
//! GPU queues or the normal 9P parser, so a parked present cannot block it.
use ::alloc::vec::Vec;
use libhalcyon::seat_control::{
    Coordinator, Error as ControlError, Op, Reply, Request, PARTICIPANTS,
};
use libthyla_rs::{
    err::{Error, Result},
    fs::File,
    ninep as p9,
    service_worker::{Control, ServiceWorker},
    sync::Mutex,
    *,
};
use lictor::{gpu_api::Request as SeatRequest, rpc_client::Client};
const MAX_CONNS: usize = PARTICIPANTS + 2;
const MSIZE: usize = 4096;
const MAX_FIDS: usize = 4;
pub struct Shared {
    pub table: Mutex<Coordinator>,
}
pub type Owner = ServiceWorker<Shared>;
pub fn start() -> Result<Owner> {
    ServiceWorker::new(
        Shared {
            table: Mutex::new(Coordinator::new()),
        },
        run,
    )
}
pub(crate) fn errno(e: ControlError) -> u32 {
    match e {
        ControlError::Denied => p9::E_PERM,
        ControlError::Gone => p9::E_NOENT,
        ControlError::Busy => p9::E_BUSY,
        ControlError::Exhausted => p9::E_NOMEM,
        ControlError::Invalid => p9::E_INVAL,
    }
}
#[derive(Clone, Copy)]
struct Fid {
    id: u32,
    path: u64,
    open: bool,
}
#[derive(Clone, Copy)]
struct Transaction {
    request: Request,
    result: Option<core::result::Result<Reply, u32>>,
}
struct Conn {
    file: File,
    lane: u64,
    stripes: u64,
    registration: Option<u64>,
    lost: bool,
    created: u64,
    waiting_since: u64,
    input: Vec<u8>,
    output: Vec<u8>,
    sent: usize,
    msize: usize,
    fids: [Option<Fid>; MAX_FIDS],
    versioned: bool,
    ctl_fid: Option<u32>,
    transaction: Option<Transaction>,
    parked: Option<(u16, u32, u64)>,
}
fn qid(path: u64) -> p9::Qid {
    p9::Qid {
        kind: if path == 0 {
            p9::P9_QTDIR
        } else {
            p9::P9_QTFILE
        },
        version: 0,
        path,
    }
}
impl Conn {
    fn new(file: File, lane: u64, stripes: u64) -> Result<Self> {
        let mut input = Vec::new();
        input
            .try_reserve_exact(MSIZE)
            .map_err(|_| Error::NoMemory)?;
        let mut output = Vec::new();
        output
            .try_reserve_exact(MSIZE)
            .map_err(|_| Error::NoMemory)?;
        Ok(Self {
            file,
            lane,
            stripes,
            registration: None,
            lost: false,
            created: libthyla_rs::time::monotonic_ns(),
            waiting_since: 0,
            input,
            output,
            sent: 0,
            msize: MSIZE,
            fids: [None; MAX_FIDS],
            versioned: false,
            ctl_fid: None,
            transaction: None,
            parked: None,
        })
    }
    fn fd(&self) -> i32 {
        self.file.as_raw_fd()
    }
    fn events(&self) -> i16 {
        if self.lost {
            0
        } else if self.output.is_empty() {
            T_POLLIN
        } else {
            T_POLLOUT
        }
    }
    fn lost(&mut self, shared: &Shared) {
        self.lost = true;
        shared.table.lock().disconnect(self.lane);
        self.input.clear();
        self.output.clear();
        self.parked = None;
    }
    fn io(&mut self, ready: i16, shared: &Shared) {
        if self.lost {
            return;
        }
        if ready & (T_POLLERR | T_POLLHUP | T_POLLNVAL) != 0 {
            self.lost(shared);
            return;
        }
        if ready & T_POLLOUT != 0 && !self.output.is_empty() {
            let bytes = &self.output[self.sent..];
            let n = unsafe { t_write(self.fd() as i64, bytes.as_ptr(), bytes.len()) };
            if n == -11 {
                return;
            }
            if n <= 0 || n as usize > bytes.len() {
                self.lost(shared);
                return;
            }
            self.sent += n as usize;
            if self.sent == self.output.len() {
                self.output.clear();
                self.sent = 0;
            }
        }
        if ready & T_POLLIN != 0 && self.output.is_empty() {
            let room = MSIZE - self.input.len();
            if room == 0 {
                return;
            }
            let mut bytes = [0; MSIZE];
            let n = unsafe { t_read(self.fd() as i64, bytes.as_mut_ptr(), room) };
            if n == -11 {
                return;
            }
            if n <= 0 || n as usize > room {
                self.lost(shared);
                return;
            }
            self.input.extend_from_slice(&bytes[..n as usize]);
        }
    }
    fn fid(&self, id: u32) -> core::result::Result<Fid, ()> {
        self.fids
            .iter()
            .flatten()
            .find(|f| f.id == id)
            .copied()
            .ok_or(())
    }
    fn install(&mut self, id: u32, path: u64) -> core::result::Result<(), ()> {
        if self.fids.iter().flatten().any(|f| f.id == id) {
            return Err(());
        }
        let free = self.fids.iter_mut().find(|f| f.is_none()).ok_or(())?;
        *free = Some(Fid {
            id,
            path,
            open: false,
        });
        Ok(())
    }
    fn decide(&mut self, q: Request, shared: &Shared) -> core::result::Result<(), u32> {
        if let Some(old) = self.transaction {
            if old.request == q {
                return old.result.map_or(Ok(()), |r| r.map(|_| ()));
            }
            if q.request <= old.request.request {
                return Err(p9::E_INVAL);
            }
            if old.result.is_none() || self.parked.is_some() {
                return Err(p9::E_BUSY);
            }
        }
        let mut table = shared.table.lock();
        let result = match q.op {
            Op::Join
                if self.registration.is_none() || self.registration == Some(q.registration) =>
            {
                table
                    .join(
                        q.registration,
                        self.stripes,
                        self.lane,
                        q.generation,
                        q.revision,
                    )
                    .map(|s| {
                        self.registration = Some(q.registration);
                        Some(s)
                    })
            }
            Op::State if self.registration == Some(q.registration) => table
                .state(q.registration, self.stripes, self.lane)
                .map(|s| (s.revision != q.revision).then_some(s)),
            Op::Cancelled if self.registration == Some(q.registration) => table
                .cancelled(
                    q.registration,
                    self.stripes,
                    self.lane,
                    q.generation,
                    q.revision,
                )
                .map(Some),
            Op::Retire if self.registration == Some(q.registration) => table
                .retire(
                    q.registration,
                    self.stripes,
                    self.lane,
                    q.generation,
                    q.revision,
                )
                .map(Some),
            _ => Err(ControlError::Denied),
        };
        let result = match result {
            Ok(Some(state)) => Some(Ok(Reply {
                op: q.op,
                request: q.request,
                state,
            })),
            Ok(None) => None,
            Err(e) => Some(Err(errno(e))),
        };
        self.waiting_since = libthyla_rs::time::monotonic_ns();
        self.transaction = Some(Transaction { request: q, result });
        result.map_or(Ok(()), |r| r.map(|_| ()))
    }
    fn poll_state(&mut self, shared: &Shared) {
        if let Some(t) = self.transaction.as_mut() {
            if t.result.is_none() {
                t.result =
                    match shared
                        .table
                        .lock()
                        .state(t.request.registration, self.stripes, self.lane)
                    {
                        Ok(state)
                            if state.revision != t.request.revision
                                || libthyla_rs::time::monotonic_ns()
                                    .saturating_sub(self.waiting_since)
                                    >= 250_000_000 =>
                        {
                            Some(Ok(Reply {
                                op: t.request.op,
                                request: t.request.request,
                                state,
                            }))
                        }
                        Ok(_) => None,
                        Err(e) => Some(Err(errno(e))),
                    };
            }
        }
    }
    fn read_reply(
        &self,
        tag: u16,
        count: u32,
        offset: u64,
        out: &mut [u8],
    ) -> core::result::Result<Option<usize>, ()> {
        let t = self.transaction.ok_or(())?;
        let Some(result) = t.result else {
            return Ok(None);
        };
        match result {
            Err(e) => p9::build_rlerror(out, tag, e).map(Some),
            Ok(r) => {
                let bytes = r.encode();
                let at = usize::try_from(offset).map_err(|_| ())?.min(bytes.len());
                let n = (count as usize).min(bytes.len() - at).min(self.msize - 11);
                p9::build_rread(out, tag, &bytes[at..at + n]).map(Some)
            }
        }
    }
    fn advance(&mut self, shared: &Shared) {
        if self.lost || !self.output.is_empty() {
            return;
        }
        self.poll_state(shared);
        let mut out = [0; MSIZE];
        if let Some((tag, count, offset)) = self.parked {
            match self.read_reply(tag, count, offset, &mut out) {
                Ok(Some(n)) => {
                    self.output.extend_from_slice(&out[..n]);
                    self.parked = None;
                    return;
                }
                Ok(None) => {}
                Err(()) => {
                    self.lost(shared);
                    return;
                }
            }
        }
        if self.input.len() < 7 {
            return;
        }
        let h = match p9::peek_header(&self.input) {
            Ok(h) => h,
            Err(_) => {
                self.lost(shared);
                return;
            }
        };
        let len = h.size as usize;
        if len < 7 || len > self.msize {
            self.lost(shared);
            return;
        }
        if self.input.len() < len {
            return;
        }
        // Move the buffer, not its bytes: dispatch may mutate connection state.
        let mut input = core::mem::take(&mut self.input);
        let result = self.dispatch(h.mtype, h.tag, &input[..len], &mut out, shared);
        input.drain(..len);
        self.input = input;
        match result {
            Ok(Some(n)) => self.output.extend_from_slice(&out[..n]),
            Ok(None) => {}
            Err(()) => self.lost(shared),
        }
    }
    fn dispatch(
        &mut self,
        ty: u8,
        tag: u16,
        msg: &[u8],
        out: &mut [u8],
        shared: &Shared,
    ) -> core::result::Result<Option<usize>, ()> {
        if !self.versioned && ty != p9::P9_TVERSION {
            return Err(());
        }
        Ok(Some(match ty {
            p9::P9_TVERSION => {
                let a = p9::parse_tversion(msg)?;
                if self.versioned || a.version != b"9P2000.L" || a.msize < 256 {
                    return Err(());
                }
                self.msize = (a.msize as usize).min(MSIZE);
                self.versioned = true;
                p9::build_rversion(out, tag, self.msize as u32, b"9P2000.L")?
            }
            p9::P9_TATTACH => {
                let a = p9::parse_tattach(msg)?;
                if a.afid != p9::P9_NOFID || !a.aname.is_empty() {
                    return Err(());
                }
                self.install(a.fid, 0)?;
                p9::build_rattach(out, tag, &qid(0))?
            }
            p9::P9_TWALK => {
                let a = p9::parse_twalk(msg)?;
                let f = self.fid(a.fid)?;
                if f.open {
                    return Err(());
                }
                let mut path = f.path;
                let mut qids = [qid(0); 16];
                if a.nwname as usize > qids.len() {
                    return Err(());
                }
                for (i, name) in a.names[..a.nwname as usize].iter().enumerate() {
                    path = match (path, *name) {
                        (0, b"ctl") => 1,
                        (_, b".") => path,
                        (1, b"..") => 0,
                        _ => return Err(()),
                    };
                    qids[i] = qid(path);
                }
                if a.newfid == a.fid {
                    self.fids
                        .iter_mut()
                        .flatten()
                        .find(|f| f.id == a.fid)
                        .ok_or(())?
                        .path = path;
                } else {
                    self.install(a.newfid, path)?;
                }
                p9::build_rwalk(out, tag, &qids[..a.nwname as usize])?
            }
            p9::P9_TLOPEN => {
                let a = p9::parse_tlopen(msg)?;
                let f = self.fid(a.fid)?;
                if f.open || a.flags != if f.path == 1 { 2 } else { 0 } {
                    return Err(());
                }
                if f.path == 1 {
                    if self.ctl_fid.is_some() {
                        return Err(());
                    }
                    self.ctl_fid = Some(a.fid);
                }
                self.fids
                    .iter_mut()
                    .flatten()
                    .find(|f| f.id == a.fid)
                    .ok_or(())?
                    .open = true;
                p9::build_rlopen(out, tag, &qid(f.path), (self.msize - 24) as u32)?
            }
            p9::P9_TWRITE => {
                let a = p9::parse_twrite(msg)?;
                let f = self.fid(a.fid)?;
                if !f.open || f.path != 1 || a.offset != 0 || self.ctl_fid != Some(a.fid) {
                    return Err(());
                }
                let Some(q) = Request::decode(a.data) else {
                    return p9::build_rlerror(out, tag, p9::E_INVAL).map(Some);
                };
                match self.decide(q, shared) {
                    Ok(()) => p9::build_rwrite(out, tag, a.data.len() as u32)?,
                    Err(e) => p9::build_rlerror(out, tag, e)?,
                }
            }
            p9::P9_TREAD => {
                let a = p9::parse_tread(msg)?;
                let f = self.fid(a.fid)?;
                if !f.open || f.path != 1 || self.parked.is_some() {
                    return Err(());
                }
                match self.read_reply(tag, a.count, a.offset, out)? {
                    Some(n) => n,
                    None => {
                        self.parked = Some((tag, a.count, a.offset));
                        return Ok(None);
                    }
                }
            }
            p9::P9_TFLUSH => {
                let a = p9::parse_tflush(msg)?;
                if self.parked.is_some_and(|p| p.0 == a.oldtag) {
                    self.parked = None;
                    if let Some(t) = self.transaction.as_mut() {
                        if t.result.is_none() {
                            t.result =
                                Some(Err(4 /* EINTR: retain request history after flush */));
                        }
                    }
                }
                p9::build_rflush(out, tag)?
            }
            p9::P9_TCLUNK => {
                let a = p9::parse_tclunk(msg)?;
                let i = self
                    .fids
                    .iter()
                    .position(|f| f.is_some_and(|f| f.id == a.fid))
                    .ok_or(())?;
                if self.fids[i].unwrap().path == 1 && self.parked.is_some() {
                    return Err(());
                }
                self.fids[i] = None;
                p9::build_rclunk(out, tag)?
            }
            p9::P9_TGETATTR => {
                let f = self.fid(p9::parse_tgetattr(msg)?)?;
                p9::build_rgetattr(
                    out,
                    tag,
                    0x7ff,
                    &qid(f.path),
                    if f.path == 0 { 0o040555 } else { 0o100666 },
                    0,
                    0,
                    1,
                    0,
                )?
            }
            _ => p9::build_rlerror(out, tag, p9::E_NOSYS)?,
        }))
    }
}
fn run(shared: &Shared, control: &Control) -> Result<()> {
    let mut seat = Client::connect().map_err(|_| Error::Io)?;
    let (_, state): (_, (u64, u32)) = seat.call(SeatRequest::SeatState).map_err(|_| Error::Io)?;
    shared
        .table
        .lock()
        .observe(state.0, state.1)
        .map_err(|_| Error::Io)?;
    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
    if root < 0 {
        return Err(Error::Io);
    }
    let listener = unsafe { t_walk_create(root, b"tapestry-interaction".as_ptr(), 20, T_OREAD, 0) };
    unsafe {
        t_close(root);
    }
    if listener < 0 {
        return Err(Error::Io);
    }
    let listener = unsafe { File::from_raw_fd(listener as i32, libthyla_rs::handle::Rights::READ) };
    let result = serve(shared, control, &listener, &mut seat);
    // A posted resident cannot be left with a live poster and a dead server.
    // Whole-process failure also revokes Tapestry's designated seat identity.
    if result.is_err() {
        // Console output itself parks in EXCLUSIVE. Fail before any logging.
        unsafe {
            t_exit_group(1);
        }
    }
    result
}
fn serve(shared: &Shared, control: &Control, listener: &File, seat: &mut Client) -> Result<()> {
    let mut conns = Vec::new();
    conns
        .try_reserve_exact(MAX_CONNS)
        .map_err(|_| Error::NoMemory)?;
    let mut next_lane = 1u64;
    let mut acknowledged = None;
    control.ready()?;
    loop {
        if control.stopping() {
            return Ok(());
        }
        let (_, state): (_, (u64, u32)) =
            seat.call(SeatRequest::SeatState).map_err(|_| Error::Io)?;
        shared
            .table
            .lock()
            .observe(state.0, state.1)
            .map_err(|_| Error::Io)?;
        if state.1 != 1 {
            acknowledged = None;
        }
        // Fresh kernel peer death, not HUP, releases a retained obligation.
        let mut i = conns.len();
        while i > 0 {
            i -= 1;
            let c: &mut Conn = &mut conns[i];
            let mut peer = TSrvPeerInfo::default();
            if unsafe { t_srv_peer(c.fd() as i64, &mut peer) } != 0 {
                return Err(Error::Io);
            }
            if peer.stripes != c.stripes {
                return Err(Error::Io);
            }
            if peer.alive == 0 {
                shared
                    .table
                    .lock()
                    .peer_dead(c.stripes)
                    .map_err(|_| Error::Io)?;
                conns.remove(i);
            } else if c.lost && shared.table.lock().lane_retired(c.lane) {
                conns.remove(i);
            } else if c.registration.is_none()
                && (c.lost
                    || libthyla_rs::time::monotonic_ns().saturating_sub(c.created) >= 5_000_000_000)
            {
                conns.remove(i);
            }
        }
        for c in &mut conns {
            c.advance(shared);
        }
        let aggregate = shared.table.lock().aggregate();
        if let Some(generation) = aggregate {
            if acknowledged != Some(generation) {
                let (_, accepted): (_, bool) = seat
                    .call(SeatRequest::SeatQuiesced { generation })
                    .map_err(|_| Error::Io)?;
                if accepted {
                    acknowledged = Some(generation);
                }
            }
        }
        let mut poll = [TPollFd::default(); MAX_CONNS + 2];
        poll[0] = TPollFd {
            fd: control.stop_fd(),
            events: T_POLLIN,
            revents: 0,
        };
        poll[1] = TPollFd {
            fd: listener.as_raw_fd(),
            events: T_POLLIN,
            revents: 0,
        };
        let mut map = [0usize; MAX_CONNS];
        let mut count = 2;
        for (i, c) in conns.iter().enumerate() {
            if !c.lost {
                map[count - 2] = i;
                poll[count] = TPollFd {
                    fd: c.fd(),
                    events: c.events(),
                    revents: 0,
                };
                count += 1;
            }
        }
        Error::from_syscall_return(unsafe { t_poll(poll.as_mut_ptr(), count, 10) })?;
        for n in 2..count {
            conns[map[n - 2]].io(poll[n].revents, shared);
        }
        if poll[1].revents & (T_POLLERR | T_POLLHUP | T_POLLNVAL) != 0 { return Err(Error::Io); }
        if poll[1].revents & T_POLLIN != 0 {
            let fd = unsafe { t_srv_accept(listener.as_raw_fd() as i64) };
            if fd >= 0 {
                let file = unsafe {
                    File::from_raw_fd(
                        fd as i32,
                        libthyla_rs::handle::Rights::READ | libthyla_rs::handle::Rights::WRITE,
                    )
                };
                let mut peer = TSrvPeerInfo::default();
                if conns.len() < MAX_CONNS
                    && conns.iter().filter(|c| c.registration.is_none()).count() < 2
                    && unsafe { t_set_nonblock(fd, true) } == 0
                    && unsafe { t_srv_peer(fd, &mut peer) } == 0
                    && peer.alive != 0
                    && peer.stripes != 0
                    && shared.table.lock().awaiting_lane(peer.stripes)
                    && !conns
                        .iter()
                        .any(|c| c.stripes == peer.stripes && c.registration.is_none())
                {
                    let lane = next_lane;
                    next_lane = next_lane.checked_add(1).ok_or(Error::Io)?;
                    conns.push(Conn::new(file, lane, peer.stripes)?);
                }
            } else if fd != -11 { return Err(Error::Io); }
        }
    }
}
