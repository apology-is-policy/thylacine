//! Bounded ctl-stream framing. A connection has one in-flight transaction; an
//! invalid length poisons it instead of trying to resynchronize attacker bytes.
use alloc::vec::Vec;
use crate::wire::MAX_PACKET;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error { Poisoned, Length, Busy, Sequence }
#[derive(Default)]
pub struct Transaction {
    incoming: Vec<u8>,
    wanted: Option<usize>,
    complete: bool,
    poisoned: bool,
    last: u64,
}
impl Transaction {
    pub fn push(&mut self, bytes: &[u8]) -> Result<Option<(u64, Vec<u8>)>, Error> {
        if self.poisoned { return Err(Error::Poisoned); }
        if self.complete { return Err(Error::Busy); }
        let result = self.append(bytes);
        if result.is_err() { self.poisoned = true; self.incoming.clear(); }
        result
    }
    fn append(&mut self, mut bytes: &[u8]) -> Result<Option<(u64, Vec<u8>)>, Error> {
        if self.incoming.len() < 4 {
            let n = bytes.len().min(4 - self.incoming.len());
            self.incoming.extend_from_slice(&bytes[..n]); bytes = &bytes[n..];
            if self.incoming.len() < 4 { return Ok(None); }
            let n = u32::from_le_bytes(self.incoming[..4].try_into().unwrap()) as usize;
            if !(12..=MAX_PACKET).contains(&n) { return Err(Error::Length); }
            self.wanted = Some(n);
        }
        let wanted = self.wanted.ok_or(Error::Length)?;
        if bytes.len() > wanted - self.incoming.len() { return Err(Error::Length); }
        self.incoming.extend_from_slice(bytes);
        if self.incoming.len() != wanted { return Ok(None); }
        let sequence = u64::from_le_bytes(self.incoming[4..12].try_into().unwrap());
        if sequence == 0 || sequence != self.last.checked_add(1).ok_or(Error::Sequence)? {
            return Err(Error::Sequence);
        }
        self.last = sequence;
        self.complete = true;
        let payload = self.incoming[12..].to_vec();
        self.incoming.clear();
        Ok(Some((sequence, payload)))
    }
    /// A full reply was consumed. Flushing or abandoning a read cannot silently
    /// reopen the transaction and execute a duplicate request.
    pub fn replied(&mut self) -> Result<(), Error> {
        if self.poisoned { return Err(Error::Poisoned); }
        if !self.complete { return Err(Error::Sequence); }
        self.complete = false; self.wanted = None; Ok(())
    }
}
pub fn encode(sequence: u64, bytes: &[u8]) -> Result<Vec<u8>, Error> {
    let n = bytes.len().checked_add(12).ok_or(Error::Length)?;
    if n > MAX_PACKET || sequence == 0 { return Err(Error::Length); }
    let mut out = Vec::with_capacity(n);
    out.extend_from_slice(&(n as u32).to_le_bytes());
    out.extend_from_slice(&sequence.to_le_bytes());
    out.extend_from_slice(bytes); Ok(out)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn fragmentation_executes_once_and_replay_poisoning_is_sticky() {
        let encoded = encode(1, b"request").unwrap();
        for cut in 0..encoded.len() {
            let mut t = Transaction::default();
            assert_eq!(t.push(&encoded[..cut]), Ok(None));
            assert_eq!(t.push(&encoded[cut..]), Ok(Some((1, b"request".to_vec()))));
            assert_eq!(t.push(&encoded), Err(Error::Busy));
            t.replied().unwrap();
            assert_eq!(t.push(&encoded), Err(Error::Sequence));
            assert_eq!(t.push(&encode(2, b"new").unwrap()), Err(Error::Poisoned));
        }
    }
    #[test]
    fn lengths_and_coalesced_commands_cannot_cross_transactions() {
        for n in [0u32, 11, MAX_PACKET as u32 + 1, u32::MAX] {
            let mut t = Transaction::default();
            assert_eq!(t.push(&n.to_le_bytes()), Err(Error::Length));
            assert_eq!(t.push(&encode(1, b"ok").unwrap()), Err(Error::Poisoned));
        }
        let mut both = encode(1, b"first").unwrap();
        both.extend_from_slice(&encode(2, b"second").unwrap());
        assert_eq!(Transaction::default().push(&both), Err(Error::Length));
    }
}
