//! The ordinary compositor's client. It has no display/input hardware handles.
use alloc::{vec, vec::Vec};
use libdriver::Error;
use libthyla_rs::{t_open, t_close, t_read, t_write, T_WALK_OPEN_FROM_ROOT, T_OREAD, T_ORDWR};
use crate::{framing, gpu_api::{Request, Stats}, wire::{Reader, Wire, MAX_PACKET}};
pub struct Client { fd: i64, sequence: u64, failed: bool }
impl Client {
    pub fn connect() -> Result<Self, Error> {
        let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/lictor".as_ptr(), 11, T_OREAD) };
        if root < 0 { let _ = libthyla_rs::t_putstr(&alloc::format!("lictor client: connect root {}\n", root)); return Err(Error::Hardware); }
        let fd = unsafe { t_open(root, b"ctl".as_ptr(), 3, T_ORDWR) };
        unsafe { t_close(root); }
        if fd < 0 { let _ = libthyla_rs::t_putstr(&alloc::format!("lictor client: open ctl {}\n", fd)); return Err(Error::Hardware); }
        Ok(Self { fd, sequence: 0, failed: false })
    }
    pub fn failed(&self) -> bool { self.failed }
    pub fn call<R: Wire>(&mut self, request: Request) -> Result<(Stats, R), Error> {
        if self.failed { return Err(Error::Hardware); }
        match self.exchange(request) {
            Ok((stats, result)) => {
                let bytes = result?;
                let mut r = match Reader::new(&bytes) {
                    Ok(r) => r, Err(_) => { self.failed = true; return Err(Error::BadField); }
                };
                let value = R::get(&mut r).and_then(|v| { r.finish()?; Ok(v) });
                match value {
                    Ok(v) => Ok((stats, v)),
                    Err(_) => { self.failed = true; Err(Error::BadField) }
                }
            }
            Err(e) => { self.failed = true; Err(e) }
        }
    }
    fn exchange(&mut self, request: Request) -> Result<(Stats, Result<Vec<u8>, Error>), Error> {
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
        let mut prefix = [0u8; 4]; self.read_exact(&mut prefix)?;
        let total = u32::from_le_bytes(prefix) as usize;
        if !(12..=MAX_PACKET).contains(&total) { return Err(Error::BadField); }
        let mut response = vec![0u8; total - 4]; self.read_exact(&mut response)?;
        let mut r = Reader::new(&response).map_err(|_| Error::BadField)?;
        if u64::get(&mut r).map_err(|_| Error::BadField)? != sequence { return Err(Error::BadField); }
        let stats = Stats::get(&mut r).map_err(|_| Error::BadField)?;
        let result = <Result<Vec<u8>, Error>>::get(&mut r).map_err(|_| Error::BadField)?;
        r.finish().map_err(|_| Error::BadField)?;
        self.sequence = sequence;
        Ok((stats, result))
    }
    fn read_exact(&self, mut bytes: &mut [u8]) -> Result<(), Error> {
        while !bytes.is_empty() {
            let count = bytes.len().min(4096);
            let n = unsafe { t_read(self.fd, bytes.as_mut_ptr(), count) };
            if n <= 0 || n as usize > count { return Err(Error::Hardware); }
            bytes = &mut bytes[n as usize..];
        }
        Ok(())
    }
}
impl Drop for Client { fn drop(&mut self) { unsafe { t_close(self.fd); } } }
