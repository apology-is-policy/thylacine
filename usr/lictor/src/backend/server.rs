//! Small, bounded 9P broker tree: ctl plus per-resource host-ring mappings.
//! Authentication happens at accept using the kernel's live peer snapshot.
use alloc::{vec, vec::Vec};
use libthyla_rs::{ninep as p9, t_close, t_read, t_write, t_weft_share, t_weft_unshare, T_POLLIN, T_POLLOUT, T_POLLHUP};
use crate::{framing::{self, Transaction}, gpu_api::{Request, Stats}, wire::{Reader, Wire}};
use super::device::Device;
const MSIZE: usize = 8192;
const MAX_FIDS: usize = 16;
const RING_BASE: u64 = 1 << 32;
#[derive(Clone, Copy)]
struct Fid { id: u32, path: u64, open: bool, share: u64 }
fn qid(path: u64) -> p9::Qid {
    p9::Qid { kind: if path == 0 || path == 2 { p9::P9_QTDIR } else { p9::P9_QTFILE }, version: 0, path }
}
pub struct Conn {
    pub fd: i64,
    incoming: Vec<u8>,
    outgoing: Vec<u8>,
    sent: usize,
    fids: Vec<Fid>,
    transaction: Transaction,
    pub pending: Option<(u64, Request)>,
    reply: Vec<u8>,
    read_at: usize,
    parked: Option<(u16, u32)>,
    alive: bool,
    msize: usize,
}
impl Conn {
    pub fn new(fd: i64) -> Self {
        Self { fd, incoming: Vec::new(), outgoing: Vec::new(), sent: 0, fids: Vec::new(),
            transaction: Transaction::default(), pending: None, reply: Vec::new(), read_at: 0,
            parked: None, alive: true, msize: MSIZE }
    }
    pub fn events(&self) -> i16 { if self.outgoing.is_empty() { T_POLLIN } else { T_POLLOUT } }
    pub fn alive(&self) -> bool { self.alive }
    pub fn io(&mut self, revents: i16) {
        if revents & T_POLLHUP != 0 { self.alive = false; return; }
        if revents & T_POLLOUT != 0 && !self.outgoing.is_empty() {
            let bytes = &self.outgoing[self.sent..];
            let n = unsafe { t_write(self.fd, bytes.as_ptr(), bytes.len().min(4096)) };
            if n == -11 { return; }
            if n <= 0 || n as usize > bytes.len().min(4096) { self.alive = false; return; }
            self.sent += n as usize;
            if self.sent == self.outgoing.len() { self.outgoing.clear(); self.sent = 0; }
        }
        if revents & T_POLLIN != 0 && self.outgoing.is_empty() {
            let mut bytes = [0u8; 4096];
            let n = unsafe { t_read(self.fd, bytes.as_mut_ptr(), bytes.len()) };
            if n == -11 { return; }
            if n <= 0 || n as usize > bytes.len() || self.incoming.len() + n as usize > MSIZE * 2 {
                self.alive = false; return;
            }
            self.incoming.extend_from_slice(&bytes[..n as usize]);
        }
    }
    fn fid(&self, id: u32) -> Result<Fid, ()> { self.fids.iter().find(|f| f.id == id).copied().ok_or(()) }
    fn install(&mut self, id: u32, path: u64) -> Result<(), ()> {
        if self.fids.len() >= MAX_FIDS || self.fids.iter().any(|f| f.id == id) { return Err(()); }
        self.fids.push(Fid { id, path, open: false, share: 0 }); Ok(())
    }
    pub fn finish(&mut self, sequence: u64, stats: Stats, value: Result<Vec<u8>, libdriver::Error>) {
        let mut bytes = Vec::new(); stats.put(&mut bytes); value.put(&mut bytes);
        match framing::encode(sequence, &bytes) {
            Ok(reply) => { self.reply = reply; self.read_at = 0; }
            Err(_) => self.alive = false,
        }
    }
    fn read_reply(&mut self, tag: u16, count: u32, out: &mut [u8]) -> Result<usize, ()> {
        let available = self.reply.len().checked_sub(self.read_at).ok_or(())?;
        let n = available.min(count as usize).min(self.msize - 11);
        let result = p9::build_rread(out, tag, &self.reply[self.read_at..self.read_at + n])?;
        self.read_at += n;
        if self.read_at == self.reply.len() {
            self.reply.clear(); self.read_at = 0;
            self.transaction.replied().map_err(|_| ())?;
        }
        Ok(result)
    }
    /// Complete at most one 9P request per pass, so the hardware input pump
    /// continues even when a normal client continuously sends work.
    pub fn advance(&mut self, device: &Device) {
        if !self.alive || !self.outgoing.is_empty() { return; }
        // Runs every pass of a 100 Hz loop for every connection: an idle one
        // must cost nothing, so the reply buffer is sized only once needed.
        let ready = !self.reply.is_empty() && self.parked.is_some();
        if !ready && self.incoming.len() < 7 { return; }
        let mut out = vec![0u8; MSIZE];
        if ready {
            let (tag, count) = self.parked.take().unwrap();
            match self.read_reply(tag, count, &mut out) {
                Ok(n) => { out.truncate(n); self.outgoing = out; }
                Err(_) => self.alive = false,
            }
            return;
        }
        if self.incoming.len() < 7 { return; }
        let head = match p9::peek_header(&self.incoming) { Ok(h) => h, Err(_) => { self.alive = false; return; } };
        let len = head.size as usize;
        if len < 7 || len > self.msize { self.alive = false; return; }
        if self.incoming.len() < len { return; }
        let message = self.incoming[..len].to_vec();
        self.incoming.drain(..len);
        match self.dispatch(head.mtype, head.tag, &message, &mut out, device) {
            Ok(Some(n)) => { out.truncate(n); self.outgoing = out; }
            Ok(None) => {},
            Err(_) => {
                super::diagnostic(&alloc::format!("lictor: broker rejected 9P type={} tag={}\n", head.mtype, head.tag));
                if let Ok(n) = p9::build_rlerror(&mut out, head.tag, p9::E_INVAL) {
                    out.truncate(n); self.outgoing = out;
                } else { self.alive = false; }
            }
        }
    }
    fn dispatch(&mut self, ty: u8, tag: u16, msg: &[u8], out: &mut [u8], device: &Device) -> Result<Option<usize>, ()> {
        Ok(Some(match ty {
            p9::P9_TVERSION => {
                let a = p9::parse_tversion(msg)?;
                if a.version != b"9P2000.L" || a.msize < 256 || !self.fids.is_empty() { return Err(()); }
                self.msize = (a.msize as usize).min(MSIZE);
                p9::build_rversion(out, tag, self.msize as u32, b"9P2000.L")?
            }
            p9::P9_TATTACH => {
                let a = p9::parse_tattach(msg)?;
                if a.afid != p9::P9_NOFID || !a.aname.is_empty() { return Err(()); }
                self.install(a.fid, 0)?; p9::build_rattach(out, tag, &qid(0))?
            }
            p9::P9_TWALK => {
                let a = p9::parse_twalk(msg)?;
                let mut path = self.fid(a.fid)?.path;
                if self.fid(a.fid)?.open { return Err(()); }
                let mut walked = Vec::new();
                for name in &a.names[..a.nwname as usize] {
                    path = match (path, *name) {
                        (0, b"ctl") => 1, (0, b"rings") => 2,
                        (_, b".") => path, (2, b"..") => 0,
                        (2, name) => {
                            let id = core::str::from_utf8(name).ok().and_then(|s| s.parse::<u32>().ok()).ok_or(())?;
                            if device.ring(id).is_none() { return Err(()); }
                            RING_BASE + id as u64
                        }
                        _ => return Err(()),
                    };
                    walked.push(qid(path));
                }
                if a.fid == a.newfid { self.fids.iter_mut().find(|f| f.id == a.fid).ok_or(())?.path = path; }
                else { self.install(a.newfid, path)?; }
                p9::build_rwalk(out, tag, &walked)?
            }
            p9::P9_TLOPEN => {
                let a = p9::parse_tlopen(msg)?;
                let f = self.fids.iter_mut().find(|f| f.id == a.fid).ok_or(())?;
                if f.open || (a.flags & 3 != 0 && f.path != 1) { return Err(()); }
                f.open = true; p9::build_rlopen(out, tag, &qid(f.path), (self.msize - 24) as u32)?
            }
            p9::P9_TWRITE => {
                let a = p9::parse_twrite(msg)?;
                let f = self.fid(a.fid)?;
                if f.path != 1 || !f.open { return Err(()); }
                if let Some((sequence, bytes)) = self.transaction.push(a.data).map_err(|_| ())? {
                    let mut r = Reader::new(&bytes).map_err(|_| ())?;
                    let request = Request::get(&mut r).map_err(|_| ())?;
                    r.finish().map_err(|_| ())?;
                    self.pending = Some((sequence, request));
                }
                p9::build_rwrite(out, tag, a.data.len() as u32)?
            }
            p9::P9_TREAD => {
                let a = p9::parse_tread(msg)?;
                let f = self.fid(a.fid)?;
                if f.path != 1 || !f.open || self.parked.is_some() { return Err(()); }
                if !self.reply.is_empty() { self.read_reply(tag, a.count, out)? }
                else if self.pending.is_some() { self.parked = Some((tag, a.count)); return Ok(None); }
                else { return Err(()); }
            }
            p9::P9_TFLUSH => {
                let a = p9::parse_tflush(msg)?;
                if self.parked.is_some_and(|(tag, _)| tag == a.oldtag) { self.parked = None; }
                p9::build_rflush(out, tag)?
            }
            p9::P9_TCLUNK => {
                let a = p9::parse_tclunk(msg)?;
                let i = self.fids.iter().position(|f| f.id == a.fid).ok_or(())?;
                let fid = self.fids.swap_remove(i);
                if fid.share != 0 { unsafe { t_weft_unshare(fid.share); } }
                p9::build_rclunk(out, tag)?
            }
            p9::P9_TGETATTR => {
                let f = self.fid(p9::parse_tgetattr(msg)?)?;
                let directory = f.path == 0 || f.path == 2;
                p9::build_rgetattr(out, tag, 0x7ff, &qid(f.path), if directory { 0o040555 } else if f.path == 1 { 0o100666 } else { 0o100444 }, 0, 0, 1, 0)?
            }
            p9::P9_TWEFT => {
                let f = self.fid(p9::parse_tweft(msg)?)?;
                if !f.open || f.path < RING_BASE { return Err(()); }
                let ring = device.ring((f.path - RING_BASE) as u32).ok_or(())?;
                if f.share != 0 { unsafe { t_weft_unshare(f.share); } }
                let share = unsafe { t_weft_share(ring.va, ring.size) };
                if share <= 0 { return Err(()); }
                self.fids.iter_mut().find(|entry| entry.id == f.id).ok_or(())?.share = share as u64;
                p9::build_rweft(out, tag, share as u64, ring.size as u32, 0)?
            }
            _ => p9::build_rlerror(out, tag, p9::E_NOSYS)?,
        }))
    }
}
impl Drop for Conn { fn drop(&mut self) {
    for fid in &self.fids { if fid.share != 0 { unsafe { t_weft_unshare(fid.share); } } }
    unsafe { t_close(self.fd); }
} }
