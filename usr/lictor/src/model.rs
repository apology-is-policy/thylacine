//! Corvus's bounded semantic frame. No ANSI, paths, fonts or executable layout.
use alloc::vec::Vec;
use crate::wire::{Malformed, Reader, Wire};
const MAGIC: u32 = 0x5255_434c; // LCUR
pub const CAPS: u64 = ((1 << 14) - 1) & !((1 << 7) - 1);
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum State { Empty, Pending, Verifying, Denied, Locked, Expired, Gone, Cancelled, Success, Failed }
impl Wire for State {
    fn put(&self, out: &mut Vec<u8>) { (*self as u8).put(out); }
    fn get(r: &mut Reader<'_>) -> Result<Self, Malformed> {
        Ok(match u8::get(r)? {
            0 => Self::Empty, 1 => Self::Pending, 2 => Self::Verifying,
            3 => Self::Denied, 4 => Self::Locked, 5 => Self::Expired,
            6 => Self::Gone, 7 => Self::Cancelled, 8 => Self::Success,
            9 => Self::Failed, _ => return Err(Malformed),
        })
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Model {
    pub state: State,
    pub pid: u32,
    pub principal: u32,
    pub stripes: u64,
    pub caps: u64,
    pub term_ns: u64,
    pub request_deadline_ns: u64,
    pub propagating: bool,
    pub user: Vec<u8>,
    pub level: Vec<u8>,
    pub notice: Vec<u8>,
}
impl Default for Model {
    fn default() -> Self {
        Self { state: State::Empty, pid: 0, principal: 0, stripes: 0, caps: 0,
            term_ns: 0, request_deadline_ns: 0, propagating: false,
            user: Vec::new(), level: Vec::new(), notice: Vec::new() }
    }
}
impl Model {
    fn valid(&self) -> bool {
        let printable = |bytes: &[u8]| bytes.iter().all(|b| (0x20..=0x7e).contains(b));
        if self.user.len() > 32 || self.level.len() > 32 || self.notice.len() > 128
            || !printable(&self.user) || !printable(&self.level) || !printable(&self.notice)
            || self.caps & !CAPS != 0 { return false; }
        if matches!(self.state, State::Pending | State::Verifying | State::Success) {
            if self.user.is_empty() || self.level.is_empty() || self.pid == 0
                || self.stripes == 0 || self.caps == 0 { return false; }
        }
        true
    }
    pub fn encode(&self) -> Result<Vec<u8>, Malformed> {
        if !self.valid() { return Err(Malformed); }
        let mut out = Vec::new();
        MAGIC.put(&mut out); 1u16.put(&mut out); self.state.put(&mut out);
        self.propagating.put(&mut out); self.pid.put(&mut out); self.principal.put(&mut out);
        self.stripes.put(&mut out); self.caps.put(&mut out); self.term_ns.put(&mut out);
        self.request_deadline_ns.put(&mut out); self.user.put(&mut out);
        self.level.put(&mut out); self.notice.put(&mut out);
        if out.len() > 512 { return Err(Malformed); }
        Ok(out)
    }
    pub fn decode(bytes: &[u8]) -> Result<Self, Malformed> {
        if bytes.len() > 512 { return Err(Malformed); }
        let mut r = Reader::new(bytes)?;
        if u32::get(&mut r)? != MAGIC || u16::get(&mut r)? != 1 { return Err(Malformed); }
        let state = State::get(&mut r)?;
        let propagating = bool::get(&mut r)?;
        let m = Self { state, propagating, pid: u32::get(&mut r)?, principal: u32::get(&mut r)?,
            stripes: u64::get(&mut r)?, caps: u64::get(&mut r)?, term_ns: u64::get(&mut r)?,
            request_deadline_ns: u64::get(&mut r)?, user: Vec::get(&mut r)?,
            level: Vec::get(&mut r)?, notice: Vec::get(&mut r)? };
        r.finish()?;
        if m.valid() { Ok(m) } else { Err(Malformed) }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn hostile_content_never_becomes_a_partial_authorization() {
        let good = Model { state: State::Pending, user: b"michael".to_vec(), level: b"imperium".to_vec(),
            pid: 31, stripes: 500, caps: 1 << 13, ..Model::default() };
        let encoded = good.encode().unwrap();
        assert_eq!(Model::decode(&encoded), Ok(good.clone()));
        for n in 0..encoded.len() { assert!(Model::decode(&encoded[..n]).is_err()); }
        let mut bad = good.clone(); bad.caps |= 1 << 63; assert!(bad.encode().is_err());
        let mut bad = good.clone(); bad.user.push(0x1b); assert!(bad.encode().is_err());
        let mut bad = good.clone(); bad.user = alloc::vec![b'a'; 33]; assert!(bad.encode().is_err());
        let mut bad = encoded.clone(); bad.push(0); assert!(Model::decode(&bad).is_err());
        let mut bad = encoded; bad[7] = 2; assert!(Model::decode(&bad).is_err());
    }
}
