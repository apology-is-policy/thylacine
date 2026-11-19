//! The ordinary compositor's client. It has no display/input hardware handles.
use alloc::{vec, vec::Vec};
use libdriver::Error;
use libthyla_rs::{t_open, t_close, t_read, t_write, T_WALK_OPEN_FROM_ROOT, T_OREAD, T_ORDWR};
use crate::{framing, gpu_api::{Request, Stats}, wire::{Reader, Wire, MAX_PACKET}};
pub struct Client { fd: i64, sequence: u64, failed: bool, stats: Stats }
impl Client {
    pub fn connect() -> Result<Self, Error> {
        let fd = open(b"ctl", T_ORDWR)?;
        Ok(Self { fd, sequence: 0, failed: false, stats: Stats::default() })
    }
    pub fn failed(&self) -> bool { self.failed }
    /// The broker's state as of the last reply, an error reply included.
    pub fn stats(&self) -> Stats { self.stats }
    pub fn call<R: Wire>(&mut self, request: Request) -> Result<R, Error> {
        if self.failed { return Err(Error::Hardware); }
        match self.exchange(request) {
            Ok(result) => {
                let bytes = result?;
                let mut r = match Reader::new(&bytes) {
                    Ok(r) => r, Err(_) => { self.failed = true; return Err(Error::BadField); }
                };
                let value = R::get(&mut r).and_then(|v| { r.finish()?; Ok(v) });
                match value {
                    Ok(v) => Ok(v),
                    Err(_) => { self.failed = true; Err(Error::BadField) }
                }
            }
            Err(e) => { self.failed = true; Err(e) }
        }
    }
    fn exchange(&mut self, request: Request) -> Result<Result<Vec<u8>, Error>, Error> {
        let sequence = self.sequence.checked_add(1).ok_or(Error::Hardware)?;
        let mut payload = Vec::new(); request.put(&mut payload);
        let packet = framing::encode(sequence, &payload).map_err(|_| Error::BadField)?;
        let mut off = 0;
        while off < packet.len() {
            let n = (packet.len() - off).min(4096);
            let written = unsafe { t_write(self.fd, packet[off..].as_ptr(), n) };
            if written <= 0 || written as usize > n { return Err(Error::Hardware); }
            off += written as usize;
        }
        // The broker answers a read with as much of the reply as it asks for, so a
        // reply that fits one read costs one round trip, its prefix included.
        let mut reply = vec![0u8; READ_MAX];
        let mut got = 0;
        while got < 4 { got += self.read_some(&mut reply[got..])?; }
        let total = u32::from_le_bytes([reply[0], reply[1], reply[2], reply[3]]) as usize;
        if !(12..=MAX_PACKET).contains(&total) || got > total { return Err(Error::BadField); }
        reply.resize(total, 0);
        self.read_exact(&mut reply[got..])?;
        let mut r = Reader::new(&reply[4..]).map_err(|_| Error::BadField)?;
        if u64::get(&mut r).map_err(|_| Error::BadField)? != sequence { return Err(Error::BadField); }
        let stats = Stats::get(&mut r).map_err(|_| Error::BadField)?;
        let result = <Result<Vec<u8>, Error>>::get(&mut r).map_err(|_| Error::BadField)?;
        r.finish().map_err(|_| Error::BadField)?;
        self.sequence = sequence;
        self.stats = stats;
        Ok(result)
    }
    fn read_exact(&self, mut bytes: &mut [u8]) -> Result<(), Error> {
        while !bytes.is_empty() {
            let n = self.read_some(bytes)?;
            bytes = &mut bytes[n..];
        }
        Ok(())
    }
    /// One read into the front of `bytes`: at least one byte, at most READ_MAX.
    fn read_some(&self, bytes: &mut [u8]) -> Result<usize, Error> {
        let count = bytes.len().min(READ_MAX);
        let n = unsafe { t_read(self.fd, bytes.as_mut_ptr(), count) };
        if n <= 0 || n as usize > count { return Err(Error::Hardware); }
        Ok(n as usize)
    }
}
const READ_MAX: usize = 4096;
/// One file of the broker's tree, on a connection of its own.
pub fn open(name: &[u8], mode: u32) -> Result<i64, Error> {
    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/lictor".as_ptr(), 11, T_OREAD) };
    if root < 0 { let _ = libthyla_rs::t_putstr(&alloc::format!("lictor client: connect root {}\n", root)); return Err(Error::Hardware); }
    let fd = unsafe { t_open(root, name.as_ptr(), name.len(), mode) };
    unsafe { t_close(root); }
    if fd < 0 {
        let _ = libthyla_rs::t_putstr(&alloc::format!("lictor client: open {} {}\n", core::str::from_utf8(name).unwrap_or("?"), fd));
        return Err(Error::Hardware);
    }
    Ok(fd)
}
impl Drop for Client { fn drop(&mut self) { unsafe { t_close(self.fd); } } }
