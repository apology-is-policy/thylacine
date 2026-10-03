//! Typed HIN1 bodies. A valid request is only syntax, never clipboard authority.
//! IDs are compared against authenticated connection/controller state by the
//! broker. Decode borrows bounded input; it neither allocates nor mutates state.
use super::interaction_wire::{self as wire, Error, Header, Mode, Operation};
use alloc::vec::Vec;

pub const SCOPE_BYTES: usize = 32;
pub const MAX_CONTROLLERS: u16 = crate::layout::MAX_PANES as u16;
const _: () = assert!(crate::layout::MAX_PANES <= u16::MAX as usize);
pub const WRITE_SLOTS: u16 = 2;
pub const READ_SLOTS: u16 = 2;
pub const IDLE_MS: u32 = 30_000;
pub const LIFETIME_MS: u32 = 120_000;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Scope {
    pub session: u64,
    pub controller: u64,
    pub context: u64,
    pub epoch: u64,
}
impl Scope {
    fn valid(self) -> bool {
        self.session != 0 && self.controller != 0 && self.context != 0 && self.epoch != 0
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Request<'a> {
    Hello,
    Bind {
        session: u64,
        context: u64,
        epoch: u64,
    },
    Mode {
        scope: Scope,
        sequence: u64,
        mode: Mode,
        readonly: bool,
        label: &'a str,
    },
    Get {
        scope: Scope,
    },
    Read {
        transfer: u64,
        offset: u32,
        count: u32,
    },
    Begin {
        scope: Scope,
        length: u32,
    },
    Write {
        transfer: u64,
        offset: u32,
        data: &'a [u8],
    },
    Commit {
        scope: Scope,
        transfer: u64,
        expected: u64,
    },
    Cancel {
        transfer: u64,
    },
    Unbind {
        scope: Scope,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Response<'a> {
    Hello {
        session: u64,
    },
    Bound {
        controller: u64,
    },
    Mode,
    Clipboard {
        transfer: u64,
        generation: u64,
        length: u32,
    },
    Read {
        offset: u32,
        data: &'a [u8],
    },
    Begun {
        transfer: u64,
    },
    Written {
        count: u32,
    },
    Committed {
        generation: u64,
    },
    Cancelled,
    Unbound,
}

fn nonzero(n: u64) -> Result<u64, Error> {
    if n == 0 {
        Err(Error::Malformed)
    } else {
        Ok(n)
    }
}
fn text_length(n: u32) -> Result<(), Error> {
    if n as usize > wire::MAX_TEXT {
        Err(Error::TooLarge)
    } else {
        Ok(())
    }
}
fn chunk(offset: u32, n: usize, empty: bool) -> Result<(), Error> {
    if !empty && n == 0 {
        return Err(Error::Malformed);
    }
    if n > wire::MAX_CHUNK
        || offset as usize > wire::MAX_TEXT
        || n > wire::MAX_TEXT - offset as usize
    {
        return Err(Error::TooLarge);
    }
    Ok(())
}
fn label(text: &str) -> Result<(), Error> {
    if text.len() > wire::MAX_LABEL {
        return Err(Error::TooLarge);
    }
    if text.chars().any(char::is_control) {
        return Err(Error::Malformed);
    }
    Ok(())
}

struct Read<'a>(&'a [u8]);
impl<'a> Read<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8], Error> {
        let out = self.0.get(..n).ok_or(Error::Malformed)?;
        self.0 = &self.0[n..];
        Ok(out)
    }
    fn u8(&mut self) -> Result<u8, Error> {
        Ok(self.take(1)?[0])
    }
    fn u16(&mut self) -> Result<u16, Error> {
        Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap()))
    }
    fn u32(&mut self) -> Result<u32, Error> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }
    fn u64(&mut self) -> Result<u64, Error> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }
    fn id(&mut self) -> Result<u64, Error> {
        nonzero(self.u64()?)
    }
    fn scope(&mut self) -> Result<Scope, Error> {
        Ok(Scope {
            session: self.id()?,
            controller: self.id()?,
            context: self.id()?,
            epoch: self.id()?,
        })
    }
    fn zero16(&mut self) -> Result<(), Error> {
        if self.u16()? == 0 {
            Ok(())
        } else {
            Err(Error::Malformed)
        }
    }
    fn zero32(&mut self) -> Result<(), Error> {
        if self.u32()? == 0 {
            Ok(())
        } else {
            Err(Error::Malformed)
        }
    }
    fn end(self) -> Result<(), Error> {
        if self.0.is_empty() {
            Ok(())
        } else {
            Err(Error::Malformed)
        }
    }
}
fn put32(out: &mut Vec<u8>, n: u32) {
    out.extend_from_slice(&n.to_le_bytes());
}
fn put64(out: &mut Vec<u8>, n: u64) {
    out.extend_from_slice(&n.to_le_bytes());
}
fn put_scope(out: &mut Vec<u8>, s: Scope) {
    for n in [s.session, s.controller, s.context, s.epoch] {
        put64(out, n);
    }
}
fn envelope(id: u64, operation: Operation, response: bool, body: &[u8]) -> Result<Vec<u8>, Error> {
    let header = Header {
        request_id: id,
        operation,
        response,
        length: wire::HEADER_BYTES + body.len(),
    }
    .encode()?;
    let mut out = Vec::with_capacity(header.len() + body.len());
    out.extend_from_slice(&header);
    out.extend_from_slice(body);
    Ok(out)
}

impl<'a> Request<'a> {
    pub fn decode(bytes: &'a [u8]) -> Result<(u64, Self), Error> {
        let (h, body) = wire::frame(bytes)?;
        Self::decode_body(h, body)
    }

    /// Dispatch a completed Receiver without copying its header and body.
    pub fn decode_body(h: Header, body: &'a [u8]) -> Result<(u64, Self), Error> {
        if h.response
            || h.request_id == 0
            || h.length != wire::HEADER_BYTES + body.len()
            || h.length > wire::MAX_RECORD
        {
            return Err(Error::Malformed);
        }
        let mut r = Read(body);
        let request = match h.operation {
            Operation::Hello => Self::Hello,
            Operation::BindController => Self::Bind {
                session: r.id()?,
                context: r.id()?,
                epoch: r.id()?,
            },
            Operation::ReportMode => {
                let scope = r.scope()?;
                let sequence = r.id()?;
                let mode = Mode::decode(r.u8()?)?;
                let flags = r.u8()?;
                if flags & !1 != 0 {
                    return Err(Error::Malformed);
                }
                r.zero16()?;
                let n = r.u32()? as usize;
                if n > wire::MAX_LABEL {
                    return Err(Error::TooLarge);
                }
                let text = core::str::from_utf8(r.take(n)?).map_err(|_| Error::Malformed)?;
                label(text)?;
                Self::Mode {
                    scope,
                    sequence,
                    mode,
                    readonly: flags != 0,
                    label: text,
                }
            }
            Operation::GetClipboard => Self::Get { scope: r.scope()? },
            Operation::ReadClipboard => {
                let transfer = r.id()?;
                let offset = r.u32()?;
                let count = r.u32()?;
                chunk(offset, count as usize, false)?;
                Self::Read {
                    transfer,
                    offset,
                    count,
                }
            }
            Operation::BeginCopy => {
                let scope = r.scope()?;
                let length = r.u32()?;
                text_length(length)?;
                r.zero32()?;
                Self::Begin { scope, length }
            }
            Operation::WriteCopy => {
                let transfer = r.id()?;
                let offset = r.u32()?;
                let n = r.u32()? as usize;
                chunk(offset, n, false)?;
                Self::Write {
                    transfer,
                    offset,
                    data: r.take(n)?,
                }
            }
            Operation::CommitCopy => Self::Commit {
                scope: r.scope()?,
                transfer: r.id()?,
                expected: r.u64()?,
            },
            Operation::Cancel => Self::Cancel { transfer: r.id()? },
            Operation::UnbindController => Self::Unbind { scope: r.scope()? },
        };
        r.end()?;
        Ok((h.request_id, request))
    }

    pub fn encode(self, id: u64) -> Result<Vec<u8>, Error> {
        let mut b = Vec::new();
        let op = match self {
            Self::Hello => Operation::Hello,
            Self::Bind {
                session,
                context,
                epoch,
            } => {
                for n in [session, context, epoch] {
                    put64(&mut b, nonzero(n)?);
                }
                Operation::BindController
            }
            Self::Mode {
                scope,
                sequence,
                mode,
                readonly,
                label: text,
            } => {
                if !scope.valid() {
                    return Err(Error::Malformed);
                }
                label(text)?;
                put_scope(&mut b, scope);
                put64(&mut b, nonzero(sequence)?);
                b.extend_from_slice(&[mode as u8, readonly as u8, 0, 0]);
                put32(&mut b, text.len() as u32);
                b.extend_from_slice(text.as_bytes());
                Operation::ReportMode
            }
            Self::Get { scope } | Self::Unbind { scope } => {
                if !scope.valid() {
                    return Err(Error::Malformed);
                }
                put_scope(&mut b, scope);
                if matches!(self, Self::Get { .. }) {
                    Operation::GetClipboard
                } else {
                    Operation::UnbindController
                }
            }
            Self::Read {
                transfer,
                offset,
                count,
            } => {
                chunk(offset, count as usize, false)?;
                put64(&mut b, nonzero(transfer)?);
                put32(&mut b, offset);
                put32(&mut b, count);
                Operation::ReadClipboard
            }
            Self::Begin { scope, length } => {
                if !scope.valid() {
                    return Err(Error::Malformed);
                }
                text_length(length)?;
                put_scope(&mut b, scope);
                put32(&mut b, length);
                put32(&mut b, 0);
                Operation::BeginCopy
            }
            Self::Write {
                transfer,
                offset,
                data,
            } => {
                chunk(offset, data.len(), false)?;
                put64(&mut b, nonzero(transfer)?);
                put32(&mut b, offset);
                put32(&mut b, data.len() as u32);
                b.extend_from_slice(data);
                Operation::WriteCopy
            }
            Self::Commit {
                scope,
                transfer,
                expected,
            } => {
                if !scope.valid() {
                    return Err(Error::Malformed);
                }
                put_scope(&mut b, scope);
                put64(&mut b, nonzero(transfer)?);
                put64(&mut b, expected);
                Operation::CommitCopy
            }
            Self::Cancel { transfer } => {
                put64(&mut b, nonzero(transfer)?);
                Operation::Cancel
            }
        };
        envelope(id, op, false, &b)
    }
}

impl<'a> Response<'a> {
    pub fn decode(bytes: &'a [u8], operation: Operation, id: u64) -> Result<Self, Error> {
        let (h, body) = wire::frame(bytes)?;
        if !h.response || h.operation != operation || h.request_id != id {
            return Err(Error::Malformed);
        }
        let mut r = Read(body);
        let response = match operation {
            Operation::Hello => {
                let session = r.id()?;
                for expected in [
                    wire::MAX_TEXT,
                    wire::MAX_CHUNK,
                    wire::MAX_LABEL,
                    wire::MAX_RECORD,
                ] {
                    if r.u32()? as usize != expected {
                        return Err(Error::Unsupported);
                    }
                }
                for expected in [WRITE_SLOTS, READ_SLOTS, MAX_CONTROLLERS] {
                    if r.u16()? != expected {
                        return Err(Error::Unsupported);
                    }
                }
                r.zero16()?;
                if r.u32()? != IDLE_MS || r.u32()? != LIFETIME_MS {
                    return Err(Error::Unsupported);
                }
                Self::Hello { session }
            }
            Operation::BindController => Self::Bound {
                controller: r.id()?,
            },
            Operation::ReportMode => Self::Mode,
            Operation::GetClipboard => {
                let transfer = r.id()?;
                let generation = r.u64()?;
                let length = r.u32()?;
                text_length(length)?;
                r.zero32()?;
                Self::Clipboard {
                    transfer,
                    generation,
                    length,
                }
            }
            Operation::ReadClipboard => {
                let offset = r.u32()?;
                let n = r.u32()? as usize;
                chunk(offset, n, true)?;
                Self::Read {
                    offset,
                    data: r.take(n)?,
                }
            }
            Operation::BeginCopy => Self::Begun { transfer: r.id()? },
            Operation::WriteCopy => {
                let count = r.u32()?;
                chunk(0, count as usize, false)?;
                r.zero32()?;
                Self::Written { count }
            }
            Operation::CommitCopy => Self::Committed {
                generation: r.id()?,
            },
            Operation::Cancel => Self::Cancelled,
            Operation::UnbindController => Self::Unbound,
        };
        r.end()?;
        Ok(response)
    }

    /// A fixed prefix and a borrowed payload. No second snapshot allocation.
    pub fn encoded(self, id: u64) -> Result<EncodedResponse<'a>, Error> {
        let mut b = Prefix { bytes: [0; 64], len: wire::HEADER_BYTES };
        let mut payload: &[u8] = &[];
        let op = match self {
            Self::Hello { session } => {
                b.u64(nonzero(session)?);
                for n in [
                    wire::MAX_TEXT,
                    wire::MAX_CHUNK,
                    wire::MAX_LABEL,
                    wire::MAX_RECORD,
                ] {
                    b.u32(n as u32);
                }
                for n in [WRITE_SLOTS, READ_SLOTS, MAX_CONTROLLERS, 0] {
                    b.u16(n);
                }
                b.u32(IDLE_MS);
                b.u32(LIFETIME_MS);
                Operation::Hello
            }
            Self::Bound { controller } => {
                b.u64(nonzero(controller)?);
                Operation::BindController
            }
            Self::Mode => Operation::ReportMode,
            Self::Clipboard {
                transfer,
                generation,
                length,
            } => {
                text_length(length)?;
                b.u64(nonzero(transfer)?);
                b.u64(generation);
                b.u32(length);
                b.u32(0);
                Operation::GetClipboard
            }
            Self::Read { offset, data } => {
                chunk(offset, data.len(), true)?;
                b.u32(offset);
                b.u32(data.len() as u32);
                payload = data;
                Operation::ReadClipboard
            }
            Self::Begun { transfer } => {
                b.u64(nonzero(transfer)?);
                Operation::BeginCopy
            }
            Self::Written { count } => {
                chunk(0, count as usize, false)?;
                b.u32(count);
                b.u32(0);
                Operation::WriteCopy
            }
            Self::Committed { generation } => {
                b.u64(nonzero(generation)?);
                Operation::CommitCopy
            }
            Self::Cancelled => Operation::Cancel,
            Self::Unbound => Operation::UnbindController,
        };
        let header = Header { request_id: id, operation: op, response: true,
            length: b.len + payload.len() }.encode()?;
        b.bytes[..wire::HEADER_BYTES].copy_from_slice(&header);
        Ok(EncodedResponse { prefix: b.bytes, used: b.len, payload })
    }
    pub fn encode(self, id: u64) -> Result<Vec<u8>, Error> {
        let encoded = self.encoded(id)?;
        let mut out = alloc::vec![0; encoded.len()];
        encoded.copy_range(0, &mut out);
        Ok(out)
    }

}

// The largest fixed body is Hello's 40 bytes. Inline metadata is counted by
// the owning connection, while the payload remains in the admitted read slot.
struct Prefix { bytes: [u8; 64], len: usize }
impl Prefix {
    fn put(&mut self, bytes: &[u8]) {
        self.bytes[self.len..self.len + bytes.len()].copy_from_slice(bytes);
        self.len += bytes.len();
    }
    fn u16(&mut self, n: u16) { self.put(&n.to_le_bytes()); }
    fn u32(&mut self, n: u32) { self.put(&n.to_le_bytes()); }
    fn u64(&mut self, n: u64) { self.put(&n.to_le_bytes()); }
}
pub struct EncodedResponse<'a> { prefix: [u8; 64], used: usize, payload: &'a [u8] }
impl EncodedResponse<'_> {
    pub fn len(&self) -> usize { self.used + self.payload.len() }
    pub fn is_empty(&self) -> bool { false }
    /// Copy only the requested slice straight into an existing 9P reply buffer.
    /// Out-of-range offsets are EOF; no addition can wrap on a hostile offset.
    pub fn copy_range(&self, offset: usize, out: &mut [u8]) -> usize {
        if offset >= self.len() { return 0; }
        let count = out.len().min(self.len() - offset);
        let first = if offset < self.used { count.min(self.used - offset) } else { 0 };
        out[..first].copy_from_slice(&self.prefix[offset.min(self.used)..offset.min(self.used) + first]);
        if first < count {
            let start = (offset + first) - self.used;
            out[first..count].copy_from_slice(&self.payload[start..start + count - first]);
        }
        count
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn borrowed_response_ranges_match_wire_fixture_without_copying_payload() {
        let payload = [b'a', b'b', b'c', b'd'];
        let view = Response::Read { offset: 7, data: &payload }.encoded(9).unwrap();
        assert_eq!(view.payload.as_ptr(), payload.as_ptr());
        let expected = [b'H', b'I', b'N', b'1', 1, 0, 5, 0,
            36, 0, 0, 0, 1, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0,
            7, 0, 0, 0, 4, 0, 0, 0, b'a', b'b', b'c', b'd'];
        for offset in 0..=expected.len() + 1 {
            for count in 0..=expected.len() + 1 {
                let mut out = alloc::vec![0xa5; count];
                let n = view.copy_range(offset, &mut out);
                let want = expected.get(offset..).unwrap_or(&[]);
                assert_eq!(n, count.min(want.len()));
                assert_eq!(&out[..n], &want[..n]);
                assert!(out[n..].iter().all(|b| *b == 0xa5));
            }
        }
        assert_eq!(view.copy_range(usize::MAX, &mut [0; 8]), 0);
        let hello = Response::Hello { session: 1 }.encoded(1).unwrap();
        assert_eq!(hello.len(), 64);
        let mut bytes = [0; 64]; assert_eq!(hello.copy_range(0, &mut bytes), 64);
        assert_eq!(Response::decode(&bytes, Operation::Hello, 1), Ok(Response::Hello { session: 1 }));
        assert_eq!(Response::Read { offset: 0, data: &payload }.encoded(0).err(), Some(Error::Malformed));
        assert_eq!(Response::Read { offset: wire::MAX_TEXT as u32, data: &payload }.encoded(1).err(), Some(Error::TooLarge));
    }
    const S: Scope = Scope {
        session: 1,
        controller: 2,
        context: 3,
        epoch: 4,
    };
    fn requests() -> [Request<'static>; 10] {
        [
            Request::Hello,
            Request::Bind {
                session: 1,
                context: 3,
                epoch: 4,
            },
            Request::Mode {
                scope: S,
                sequence: 5,
                mode: Mode::Visual,
                readonly: true,
                label: "Nora",
            },
            Request::Get { scope: S },
            Request::Read {
                transfer: 6,
                offset: 7,
                count: 3,
            },
            Request::Begin {
                scope: S,
                length: 3,
            },
            Request::Write {
                transfer: 6,
                offset: 7,
                data: b"abc",
            },
            Request::Commit {
                scope: S,
                transfer: 6,
                expected: 0,
            },
            Request::Cancel { transfer: 6 },
            Request::Unbind { scope: S },
        ]
    }
    #[test]
    fn every_body_roundtrips_and_refuses_truncation_trailing_or_wrong_direction() {
        for q in requests() {
            let b = q.encode(9).unwrap();
            assert_eq!(Request::decode(&b), Ok((9, q)));
            for end in 0..b.len() {
                assert!(Request::decode(&b[..end]).is_err());
            }
            // Adjusting the envelope must not make a truncated/trailing BODY legal.
            for end in 24..b.len() {
                let mut cut = b[..end].to_vec();
                cut[8..12].copy_from_slice(&(end as u32).to_le_bytes());
                assert!(Request::decode(&cut).is_err());
            }
            let mut extra = b.clone();
            extra.push(0);
            let n = extra.len() as u32;
            extra[8..12].copy_from_slice(&n.to_le_bytes());
            assert!(Request::decode(&extra).is_err());
            let mut reply = b;
            reply[12] = 1;
            assert!(Request::decode(&reply).is_err());
        }
    }
    #[test]
    fn successful_responses_are_bound_to_the_exact_operation_and_request() {
        let replies = [
            Response::Hello { session: 1 },
            Response::Bound { controller: 2 },
            Response::Mode,
            Response::Clipboard {
                transfer: 6,
                generation: 0,
                length: 3,
            },
            Response::Read {
                offset: 7,
                data: b"abc",
            },
            Response::Begun { transfer: 6 },
            Response::Written { count: 3 },
            Response::Committed { generation: 1 },
            Response::Cancelled,
            Response::Unbound,
        ];
        for (q, r) in requests().into_iter().zip(replies) {
            let operation = wire::frame(&q.encode(9).unwrap()).unwrap().0.operation;
            let bytes = r.encode(9).unwrap();
            assert_eq!(Response::decode(&bytes, operation, 9), Ok(r));
            assert!(Response::decode(&bytes, operation, 10).is_err());
            let wrong = if operation == Operation::Hello {
                Operation::Cancel
            } else {
                Operation::Hello
            };
            assert!(Response::decode(&bytes, wrong, 9).is_err());
            for end in 24..bytes.len() {
                let mut cut = bytes[..end].to_vec();
                cut[8..12].copy_from_slice(&(end as u32).to_le_bytes());
                assert!(Response::decode(&cut, operation, 9).is_err());
            }
        }
    }
    #[test]
    fn independent_c_rust_wire_fixtures() {
        let fixtures = include_str!("../fixtures/interaction-v1.hex");
        let replies = [
            Response::Hello { session: 1 },
            Response::Bound { controller: 2 },
            Response::Mode,
            Response::Clipboard {
                transfer: 6,
                generation: 0,
                length: 3,
            },
            Response::Read {
                offset: 7,
                data: b"abc",
            },
            Response::Begun { transfer: 6 },
            Response::Written { count: 3 },
            Response::Committed { generation: 1 },
            Response::Cancelled,
            Response::Unbound,
        ];
        assert_eq!(fixtures.lines().count(), 20);
        for (i, line) in fixtures.lines().enumerate() {
            let (name, hex) = line.split_once(' ').unwrap();
            let operation = i % 10;
            let expected: Vec<u8> = hex
                .as_bytes()
                .chunks_exact(2)
                .map(|pair| u8::from_str_radix(core::str::from_utf8(pair).unwrap(), 16).unwrap())
                .collect();
            let actual = if i < 10 {
                requests()[operation].encode(9)
            } else {
                replies[operation].encode(9)
            }
            .unwrap();
            assert_eq!(actual, expected, "{}", name);
        }
    }
    #[test]
    fn refuses_reserved_bits_bad_text_zero_id_and_overflow_before_allocation() {
        let mode = requests()[2].encode(9).unwrap();
        for (offset, value) in [(64, 0), (65, 2), (66, 1), (67, 1), (68, 65), (72, 0xff)] {
            let mut b = mode.clone();
            b[offset] = value;
            assert!(Request::decode(&b).is_err());
        }
        for off in [24, 32, 40, 48, 56] {
            let mut b = mode.clone();
            b[off..off + 8].fill(0);
            assert!(Request::decode(&b).is_err());
        }
        assert!(Request::Mode {
            scope: S,
            sequence: 1,
            mode: Mode::Insert,
            readonly: false,
            label: "a\nb"
        }
        .encode(1)
        .is_err());
        assert!(Request::Read {
            transfer: 1,
            offset: u32::MAX,
            count: 1
        }
        .encode(1)
        .is_err());
        assert!(Request::Read {
            transfer: 1,
            offset: 0,
            count: 0
        }
        .encode(1)
        .is_err());
        assert!(Request::Begin {
            scope: S,
            length: wire::MAX_TEXT as u32 + 1
        }
        .encode(1)
        .is_err());
        assert!(Request::Write {
            transfer: 1,
            offset: 0,
            data: &[]
        }
        .encode(1)
        .is_err());
        assert!(Request::Begin {
            scope: S,
            length: 0
        }
        .encode(1)
        .is_ok());
        assert!(Response::Read {
            offset: wire::MAX_TEXT as u32,
            data: &[]
        }
        .encode(1)
        .is_ok());
        let mut begin = requests()[5].encode(9).unwrap();
        begin[60] = 1;
        assert!(Request::decode(&begin).is_err());
    }
}
