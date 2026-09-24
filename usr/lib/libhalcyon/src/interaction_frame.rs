//! One bounded HIN1 record assembled from contiguous transaction-file writes.
//! The caller accounts `reserved_bytes()` across ALL fids and supplies the
//! remaining per-connection allowance (including this receiver's reservation).
//! Partial bytes cannot be dispatched. Any error poisons the receiver until
//! reset; no later suffix can repair an invalid operation into acceptance.
use super::interaction_wire::{Error, Header, HEADER_BYTES};
use alloc::vec::Vec;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ReceiveError {
    Wire(Error),
    Offset,
    Budget,
    Allocation,
    Poisoned,
}

pub struct Receiver {
    prefix: [u8; HEADER_BYTES],
    prefix_len: usize,
    header: Option<Header>,
    body: Vec<u8>,
    poisoned: bool,
}
impl Default for Receiver {
    fn default() -> Self {
        Self {
            prefix: [0; HEADER_BYTES],
            prefix_len: 0,
            header: None,
            body: Vec::new(),
            poisoned: false,
        }
    }
}
impl Receiver {
    pub fn reserved_bytes(&self) -> usize {
        HEADER_BYTES + self.body.capacity()
    }
    pub fn received_bytes(&self) -> usize {
        self.prefix_len + self.body.len()
    }
    /// Header/body are exposed only after the entire declared record arrives.
    /// This is syntax, not admission: the typed body and authenticated context
    /// must still be checked. Replay handling belongs to the transaction owner.
    pub fn complete(&self) -> Option<(Header, &[u8])> {
        let h = self.header?;
        if self.poisoned || self.received_bytes() != h.length {
            return None;
        }
        Some((h, &self.body))
    }
    pub fn reset(&mut self) {
        *self = Self::default();
    }
    pub fn push(
        &mut self,
        offset: u64,
        bytes: &[u8],
        allowance: usize,
    ) -> Result<bool, ReceiveError> {
        if self.poisoned {
            return Err(ReceiveError::Poisoned);
        }
        let result = self.append(offset, bytes, allowance);
        if result.is_err() {
            self.poisoned = true;
        }
        result
    }
    fn append(
        &mut self,
        offset: u64,
        mut bytes: &[u8],
        allowance: usize,
    ) -> Result<bool, ReceiveError> {
        if offset != self.received_bytes() as u64 || self.complete().is_some() || bytes.is_empty() {
            return Err(ReceiveError::Offset);
        }
        if allowance < self.reserved_bytes() {
            return Err(ReceiveError::Budget);
        }
        if self.prefix_len < HEADER_BYTES {
            let n = bytes.len().min(HEADER_BYTES - self.prefix_len);
            self.prefix[self.prefix_len..self.prefix_len + n].copy_from_slice(&bytes[..n]);
            self.prefix_len += n;
            bytes = &bytes[n..];
            if self.prefix_len < HEADER_BYTES {
                return Ok(false);
            }
            let h = Header::decode(&self.prefix).map_err(ReceiveError::Wire)?;
            if h.length > allowance {
                return Err(ReceiveError::Budget);
            }
            let n = h.length - HEADER_BYTES;
            // Refuse a surplus before allocating the declared body.
            if bytes.len() > n {
                return Err(ReceiveError::Wire(Error::Malformed));
            }
            self.body
                .try_reserve_exact(n)
                .map_err(|_| ReceiveError::Allocation)?;
            if self.reserved_bytes() > allowance {
                self.body = Vec::new();
                return Err(ReceiveError::Budget);
            }
            self.header = Some(h);
        }
        let h = self.header.ok_or(ReceiveError::Wire(Error::Malformed))?;
        if bytes.len() > h.length - self.received_bytes() {
            return Err(ReceiveError::Wire(Error::Malformed));
        }
        self.body.extend_from_slice(bytes);
        Ok(self.complete().is_some())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::interaction_body::{Request, Scope};
    use crate::interaction_wire::MAX_RECORD;
    fn record() -> Vec<u8> {
        Request::Begin {
            scope: Scope {
                session: 1,
                controller: 2,
                context: 3,
                epoch: 4,
            },
            length: 12,
        }
        .encode(1)
        .unwrap()
    }
    #[test]
    fn every_split_and_byte_at_a_time_withhold_partial_records() {
        let b = record();
        for split in 1..b.len() {
            let mut r = Receiver::default();
            assert_eq!(r.push(0, &b[..split], MAX_RECORD), Ok(false));
            assert!(r.complete().is_none());
            assert_eq!(r.push(split as u64, &b[split..], MAX_RECORD), Ok(true));
            assert_eq!(r.complete().unwrap().1, &b[HEADER_BYTES..]);
            let (header, body) = r.complete().unwrap();
            assert_eq!(Request::decode_body(header, body), Request::decode(&b));
            assert_eq!(r.reserved_bytes(), b.len());
        }
        let mut r = Receiver::default();
        for (i, byte) in b.iter().enumerate() {
            assert_eq!(r.push(i as u64, &[*byte], MAX_RECORD), Ok(i + 1 == b.len()));
        }
        assert!(r.complete().is_some());
    }
    #[test]
    fn errors_poison_until_reset_and_cannot_be_repaired_by_a_suffix() {
        let b = record();
        let mut r = Receiver::default();
        assert_eq!(r.push(1, &b, MAX_RECORD), Err(ReceiveError::Offset));
        assert_eq!(r.push(0, &b, MAX_RECORD), Err(ReceiveError::Poisoned));
        r.reset();
        assert_eq!(r.push(0, &b, MAX_RECORD), Ok(true));
        assert_eq!(
            r.push(b.len() as u64, &[0], MAX_RECORD),
            Err(ReceiveError::Offset)
        );
        assert!(r.complete().is_none());
        r.reset();
        let mut bad = b.clone();
        bad[0] = 0;
        assert_eq!(
            r.push(0, &bad, MAX_RECORD),
            Err(ReceiveError::Wire(Error::Malformed))
        );
        assert_eq!(r.reserved_bytes(), HEADER_BYTES);
    }
    #[test]
    fn declared_extent_and_connection_allowance_bound_allocation() {
        let b = record();
        let mut r = Receiver::default();
        assert_eq!(r.push(0, &b[..24], b.len() - 1), Err(ReceiveError::Budget));
        assert_eq!(r.reserved_bytes(), HEADER_BYTES);
        assert!(r.complete().is_none());
        r.reset();
        let mut extra = b.clone();
        extra.push(0);
        assert_eq!(
            r.push(0, &extra, MAX_RECORD),
            Err(ReceiveError::Wire(Error::Malformed))
        );
        assert_eq!(r.reserved_bytes(), HEADER_BYTES);
        r.reset();
        assert_eq!(r.push(0, &b[..24], b.len()), Ok(false));
        assert_eq!(r.push(24, &b[24..], b.len() - 1), Err(ReceiveError::Budget));
        r.reset();
        let hello = Request::Hello.encode(1).unwrap();
        assert_eq!(r.push(0, &hello, HEADER_BYTES), Ok(true));
        assert_eq!(r.complete().unwrap().1.len(), 0);
    }
}
