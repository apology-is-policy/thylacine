//! Ordered compositor ownership changes and decisions; never drawing events.
use crate::interaction_control::{Op, Reply, Request};

pub const RECORD_BYTES: usize = 80;
pub const CAPACITY: usize = 64;
pub const SELECT: [u8; 16] = *b"HIO1\x01\0\0\0\0\0\0\0\0\0\0\0";
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Body {
    Ready(u64),
    FocusLost {
        leaf: u32,
        binding: u64,
        epoch: u64,
    },
    Terminal {
        leaf: u32,
        binding: u64,
        foreground: u64,
        subject: u64,
    },
    Retired {
        leaf: u32,
        binding: u64,
    },
    Reset,
    Decision {
        leaf: u32,
        binding: u64,
        request: u64,
        op: Op,
        result: Result<Reply, u32>,
    },
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Record {
    pub sequence: u64,
    pub body: Body,
}
fn word(b: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(b[at..at + 8].try_into().unwrap())
}
impl Record {
    pub fn encode(self) -> [u8; RECORD_BYTES] {
        let (kind, request, leaf, binding, words) = match self.body {
            Body::Ready(id) => (1, 0, 0, 0, [id, 0, 0, 0, 0]),
            Body::FocusLost {
                leaf,
                binding,
                epoch,
            } => (2, 0, leaf, binding, [epoch, 0, 0, 0, 0]),
            Body::Terminal {
                leaf,
                binding,
                foreground,
                subject,
            } => (3, 0, leaf, binding, [foreground, subject, 0, 0, 0]),
            Body::Retired { leaf, binding } => (4, 0, leaf, binding, [0; 5]),
            Body::Reset => (5, 0, 0, 0, [0; 5]),
            Body::Decision {
                leaf,
                binding,
                request,
                op,
                result,
            } => (
                6,
                request,
                leaf,
                binding,
                match result {
                    Ok(r) => [op as u64, 0, r.focus, r.seat, r.foreground],
                    Err(e) => [op as u64, e as u64, 0, 0, 0],
                },
            ),
        };
        let mut b = [0; RECORD_BYTES];
        b[..4].copy_from_slice(b"HIO1");
        b[4] = 1;
        b[6] = kind;
        b[8..16].copy_from_slice(&self.sequence.to_le_bytes());
        b[16..24].copy_from_slice(&request.to_le_bytes());
        b[24..28].copy_from_slice(&leaf.to_le_bytes());
        b[32..40].copy_from_slice(&binding.to_le_bytes());
        for (i, w) in words.into_iter().enumerate() {
            b[40 + i * 8..48 + i * 8].copy_from_slice(&w.to_le_bytes());
        }
        b
    }
    pub fn decode(b: &[u8]) -> Option<Self> {
        if b.len() != RECORD_BYTES
            || b[..4] != *b"HIO1"
            || b[4..6] != [1, 0]
            || b[7] != 0
            || b[28..32] != [0; 4]
        {
            return None;
        }
        let sequence = word(b, 8);
        if sequence == 0 || sequence == u64::MAX {
            return None;
        }
        let request = word(b, 16);
        let leaf = u32::from_le_bytes(b[24..28].try_into().unwrap());
        let binding = word(b, 32);
        let w = [
            word(b, 40),
            word(b, 48),
            word(b, 56),
            word(b, 64),
            word(b, 72),
        ];
        let route = leaf != 0 && binding != 0 && binding <= i64::MAX as u64;
        let body = match b[6] {
            1 if request == 0 && leaf == 0 && binding == 0 && w[0] != 0 && w[1..] == [0; 4] => {
                Body::Ready(w[0])
            }
            2 if request == 0 && route && w[0] != 0 && w[0] != u64::MAX && w[1..] == [0; 4] => {
                Body::FocusLost {
                    leaf,
                    binding,
                    epoch: w[0],
                }
            }
            3 if request == 0 && route && w[0] != 0 && w[2..] == [0; 3] => Body::Terminal {
                leaf,
                binding,
                foreground: w[0],
                subject: w[1],
            },
            4 if request == 0 && route && w == [0; 5] => Body::Retired { leaf, binding },
            5 if request == 0 && leaf == 0 && binding == 0 && w == [0; 5] => Body::Reset,
            6 if request != 0 && route => {
                let op = match w[0] {
                    1 => Op::Bind,
                    2 => Op::Publish,
                    3 => Op::Check,
                    4 => Op::Unbind,
                    _ => return None,
                };
                let result = if w[1] == 0 {
                    if w[2] == 0 || w[2] == u64::MAX || (op == Op::Unbind) != (w[4] == 0) {
                        return None;
                    }
                    Ok(Reply {
                        op,
                        request,
                        focus: w[2],
                        seat: w[3],
                        foreground: w[4],
                    })
                } else {
                    if w[1] > 4095 || w[2..] != [0; 3] {
                        return None;
                    }
                    Err(w[1] as u32)
                };
                Body::Decision {
                    leaf,
                    binding,
                    request,
                    op,
                    result,
                }
            }
            _ => return None,
        };
        Some(Self { sequence, body })
    }
}
impl Body {
    pub fn decision(r: Request, result: Result<Reply, u32>) -> Self {
        Self::Decision {
            leaf: r.leaf,
            binding: r.binding,
            request: r.request,
            op: r.op,
            result,
        }
    }
}
/// Overflow poisons the whole history; it never hides an earlier revocation.
pub struct Journal {
    entries: [Option<Record>; CAPACITY],
    head: usize,
    len: usize,
    next: u64,
    failed: bool,
}
const _: () = assert!(core::mem::size_of::<Journal>() <= 8 * 1024);
impl Journal {
    pub fn new(identity: u64) -> Option<Self> {
        if identity == 0 {
            return None;
        }
        let mut j = Self {
            entries: [None; CAPACITY],
            head: 0,
            len: 0,
            next: 1,
            failed: false,
        };
        j.push(Body::Ready(identity)).ok()?;
        Some(j)
    }
    pub fn poison(&mut self) {
        self.failed = true;
        self.entries.fill(None);
        self.len = 0;
    }
    pub fn failed(&self) -> bool {
        self.failed
    }
    pub fn push(&mut self, body: Body) -> Result<(), ()> {
        if self.failed {
            return Err(());
        }
        let r = Record {
            sequence: self.next,
            body,
        };
        if self.len == CAPACITY || self.next == u64::MAX || Record::decode(&r.encode()) != Some(r) {
            self.poison();
            return Err(());
        }
        self.entries[(self.head + self.len) % CAPACITY] = Some(r);
        self.next += 1;
        self.len += 1;
        Ok(())
    }
    pub fn pop(&mut self) -> Result<Option<Record>, ()> {
        if self.failed {
            return Err(());
        }
        if self.len == 0 {
            return Ok(None);
        }
        let r = self.entries[self.head].take();
        self.head = (self.head + 1) % CAPACITY;
        self.len -= 1;
        Ok(r)
    }
}
/// Only exact contiguous records from a fresh stream may reach the owner.
pub struct Receiver {
    next: u64,
    identity: Option<u64>,
    failed: bool,
}
impl Default for Receiver {
    fn default() -> Self {
        Self::new()
    }
}
impl Receiver {
    pub const fn new() -> Self {
        Self {
            next: 1,
            identity: None,
            failed: false,
        }
    }
    pub fn identity(&self) -> Option<u64> {
        if self.failed {
            None
        } else {
            self.identity
        }
    }
    pub fn accept(&mut self, bytes: &[u8]) -> Result<Record, ()> {
        let r = Record::decode(bytes);
        if self.failed
            || r.is_none_or(|r| {
                r.sequence != self.next || matches!(r.body, Body::Ready(_)) != (self.next == 1)
            })
        {
            self.failed = true;
            return Err(());
        }
        let r = r.unwrap();
        if let Body::Ready(id) = r.body {
            self.identity = Some(id);
        }
        self.next += 1;
        Ok(r)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn lost(epoch: u64) -> Body {
        Body::FocusLost {
            leaf: 2,
            binding: 3,
            epoch,
        }
    }
    #[test]
    fn initial_normal_seat_generation_zero_is_valid() {
        let body = Body::Decision {
            leaf: 3,
            binding: 588,
            request: 101,
            op: Op::Publish,
            result: Ok(Reply {
                op: Op::Publish,
                request: 101,
                focus: 16,
                seat: 0,
                foreground: 3,
            }),
        };
        let mut journal = Journal::new(1).unwrap();
        assert_eq!(journal.push(body), Ok(()));
        journal.pop().unwrap();
        assert_eq!(journal.pop().unwrap().unwrap().body, body);
    }
    #[test]
    fn independent_little_endian_fixture_and_malformed_records() {
        let r = Record {
            sequence: 7,
            body: lost(11),
        };
        let mut fixture = [0u8; 80];
        fixture[..4].copy_from_slice(b"HIO1");
        fixture[4] = 1;
        fixture[6] = 2;
        fixture[8] = 7;
        fixture[24] = 2;
        fixture[32] = 3;
        fixture[40] = 11;
        assert_eq!(r.encode(), fixture);
        assert_eq!(Record::decode(&fixture), Some(r));
        for at in [4, 5, 7, 16, 28, 48, 56, 64, 72] {
            let mut bad = fixture;
            bad[at] = 255;
            assert_eq!(Record::decode(&bad), None, "byte {at}");
        }
        for n in 0..80 {
            assert_eq!(Record::decode(&fixture[..n]), None);
        }
        fixture[40] = 0;
        assert_eq!(Record::decode(&fixture), None);
    }
    #[test]
    fn every_body_roundtrips_and_reserved_fields_stay_zero() {
        for body in [
            Body::Ready(2),
            lost(4),
            Body::Terminal {
                leaf: 2,
                binding: 3,
                foreground: 4,
                subject: 0,
            },
            Body::Retired {
                leaf: 2,
                binding: 3,
            },
            Body::Reset,
            Body::Decision {
                leaf: 2,
                binding: 3,
                request: 5,
                op: Op::Check,
                result: Ok(Reply {
                    op: Op::Check,
                    request: 5,
                    focus: 6,
                    seat: 7,
                    foreground: 8,
                }),
            },
            Body::Decision {
                leaf: 2,
                binding: 3,
                request: 5,
                op: Op::Check,
                result: Err(1),
            },
        ] {
            let r = Record { sequence: 1, body };
            assert_eq!(Record::decode(&r.encode()), Some(r));
            let mut b = r.encode();
            b[28] = 1;
            assert_eq!(Record::decode(&b), None);
        }
    }
    #[test]
    fn overflow_discards_history_and_never_recovers_in_place() {
        let mut j = Journal::new(1).unwrap();
        for n in 1..CAPACITY {
            j.push(lost(n as u64)).unwrap();
        }
        assert_eq!(j.push(lost(99)), Err(()));
        assert!(j.failed());
        assert_eq!(j.pop(), Err(()));
        assert_eq!(j.push(Body::Reset), Err(()));
    }
    #[test]
    fn ordered_round_trips_survive_ring_wrap_without_coalescing() {
        let mut j = Journal::new(1).unwrap();
        let mut receiver = Receiver::new();
        receiver
            .accept(&j.pop().unwrap().unwrap().encode())
            .unwrap();
        for n in 1..200 {
            j.push(lost(n)).unwrap();
            j.push(lost(n + 1)).unwrap();
            assert_eq!(
                receiver
                    .accept(&j.pop().unwrap().unwrap().encode())
                    .unwrap()
                    .body,
                lost(n)
            );
            assert_eq!(
                receiver
                    .accept(&j.pop().unwrap().unwrap().encode())
                    .unwrap()
                    .body,
                lost(n + 1)
            );
        }
    }
    #[test]
    fn duplicate_gap_wrong_ready_and_exhaustion_poison() {
        for seq in [1, 3] {
            let mut r = Receiver::new();
            r.accept(
                &Record {
                    sequence: 1,
                    body: Body::Ready(1),
                }
                .encode(),
            )
            .unwrap();
            assert!(r
                .accept(
                    &Record {
                        sequence: seq,
                        body: lost(5)
                    }
                    .encode()
                )
                .is_err());
            assert!(r
                .accept(
                    &Record {
                        sequence: 2,
                        body: lost(5)
                    }
                    .encode()
                )
                .is_err());
            assert_eq!(r.identity(), None);
        }
        let mut r = Receiver::new();
        assert!(r
            .accept(
                &Record {
                    sequence: 1,
                    body: lost(5)
                }
                .encode()
            )
            .is_err());
        let mut j = Journal::new(1).unwrap();
        j.next = u64::MAX;
        assert_eq!(j.push(lost(1)), Err(()));
        assert_eq!(j.pop(), Err(()));
    }
}
