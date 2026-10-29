//! Fixed compositor control records; only the declared session may submit them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u16)]
pub enum Op {
    Bind = 1,
    Publish = 2,
    Check = 3,
    Unbind = 4,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Request {
    pub op: Op,
    pub request: u64,
    pub leaf: u32,
    pub binder_pid: u32,
    pub binding: u64,
    pub foreground: u64,
    pub subject: u64,
    pub controller: u64,
    pub context: u64,
    pub epoch: u64,
}
pub const REQUEST_BYTES: usize = 80;
pub const REPLY_BYTES: usize = 40;
fn u32at(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}
fn u64at(b: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(b[at..at + 8].try_into().unwrap())
}
impl Request {
    pub fn decode(b: &[u8]) -> Option<Self> {
        if b.len() != REQUEST_BYTES || &b[..4] != b"HIA1" || b[4..6] != [1, 0] || u64at(b, 72) != 0
        {
            return None;
        }
        let op = match u16::from_le_bytes(b[6..8].try_into().unwrap()) {
            1 => Op::Bind,
            2 => Op::Publish,
            3 => Op::Check,
            4 => Op::Unbind,
            _ => return None,
        };
        let r = Self {
            op,
            request: u64at(b, 8),
            leaf: u32at(b, 16),
            binder_pid: u32at(b, 20),
            binding: u64at(b, 24),
            foreground: u64at(b, 32),
            subject: u64at(b, 40),
            controller: u64at(b, 48),
            context: u64at(b, 56),
            epoch: u64at(b, 64),
        };
        if r.request == 0 || r.leaf == 0 || r.binding == 0 || r.binding > i64::MAX as u64 {
            return None;
        }
        match op {
            Op::Bind | Op::Unbind => {
                if (op == Op::Bind) != (r.binder_pid != 0)
                    || r.foreground | r.subject | r.controller | r.context | r.epoch != 0
                {
                    return None;
                }
            }
            Op::Publish | Op::Check => {
                if r.binder_pid != 0
                    || [r.foreground, r.subject, r.controller, r.context, r.epoch].contains(&0)
                {
                    return None;
                }
            }
        }
        Some(r)
    }
    pub fn encode(self) -> [u8; REQUEST_BYTES] {
        let mut b = [0; REQUEST_BYTES];
        b[..4].copy_from_slice(b"HIA1");
        b[4] = 1;
        b[6..8].copy_from_slice(&(self.op as u16).to_le_bytes());
        b[8..16].copy_from_slice(&self.request.to_le_bytes());
        b[16..20].copy_from_slice(&self.leaf.to_le_bytes());
        b[20..24].copy_from_slice(&self.binder_pid.to_le_bytes());
        for (i, n) in [
            self.binding,
            self.foreground,
            self.subject,
            self.controller,
            self.context,
            self.epoch,
        ]
        .iter()
        .enumerate()
        {
            b[24 + i * 8..32 + i * 8].copy_from_slice(&n.to_le_bytes());
        }
        b
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Reply {
    pub op: Op,
    pub request: u64,
    pub focus: u64,
    pub seat: u64,
    pub foreground: u64,
}
impl Reply {
    pub fn encode(self) -> [u8; REPLY_BYTES] {
        let mut b = [0; REPLY_BYTES];
        b[..4].copy_from_slice(b"HIA1");
        b[4] = 1;
        b[6..8].copy_from_slice(&(self.op as u16).to_le_bytes());
        for (i, n) in [self.request, self.focus, self.seat, self.foreground]
            .iter()
            .enumerate()
        {
            b[8 + i * 8..16 + i * 8].copy_from_slice(&n.to_le_bytes());
        }
        b
    }
    pub fn decode(b: &[u8], r: Request) -> Option<Self> {
        if b.len() != REPLY_BYTES
            || b[..4] != *b"HIA1"
            || b[4..6] != [1, 0]
            || b[6..8] != (r.op as u16).to_le_bytes()
            || u64at(b, 8) != r.request
        {
            return None;
        }
        Some(Self {
            op: r.op,
            request: r.request,
            focus: u64at(b, 16),
            seat: u64at(b, 24),
            foreground: u64at(b, 32),
        })
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn exact_control_shape_and_reserved_bits() {
        let r = Request {
            op: Op::Bind,
            request: 1,
            leaf: 2,
            binder_pid: 3,
            binding: 4,
            foreground: 0,
            subject: 0,
            controller: 0,
            context: 0,
            epoch: 0,
        };
        let bytes = r.encode();
        assert_eq!(&bytes[..8], b"HIA1\x01\0\x01\0");
        assert_eq!(Request::decode(&bytes), Some(r));
        for n in 0..80 {
            assert!(Request::decode(&bytes[..n]).is_none());
        }
        for at in [0, 4, 6, 32, 40, 48, 56, 64, 72] {
            let mut b = bytes;
            b[at] = 255;
            assert!(Request::decode(&b).is_none());
        }
        let p = Request {
            op: Op::Publish,
            binder_pid: 0,
            foreground: 5,
            subject: 6,
            controller: 7,
            context: 8,
            epoch: 9,
            ..r
        };
        assert_eq!(Request::decode(&p.encode()), Some(p));
        for at in [8, 16, 24, 32, 40, 48, 56, 64] {
            let mut b = p.encode();
            b[at..at + 8].fill(0);
            assert!(Request::decode(&b).is_none());
        }
        let reply = Reply {
            op: p.op,
            request: p.request,
            focus: 11,
            seat: 12,
            foreground: 5,
        };
        assert_eq!(Reply::decode(&reply.encode(), p), Some(reply));
        assert!(Reply::decode(&reply.encode(), Request { request: 2, ..p }).is_none());
    }
}
