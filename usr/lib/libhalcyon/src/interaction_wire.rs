//! HIN1 envelope only; decoding a frame does not validate its operation body or authorize it.
use core::convert::TryInto;

pub const HEADER_BYTES: usize = 24;
pub const MAX_RECORD: usize = 32 * 1024;
pub const MAX_TEXT: usize = 1024 * 1024;
pub const MAX_CHUNK: usize = 16 * 1024;
pub const MAX_LABEL: usize = 64;
pub const MAX_QUERY: usize = 4096;
pub const VERSION: u16 = 1;
pub const RESPONSE: u32 = 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u16)]
pub enum Operation {
    Hello = 1,
    BindController = 2,
    ReportMode = 3,
    GetClipboard = 4,
    ReadClipboard = 5,
    BeginCopy = 6,
    WriteCopy = 7,
    CommitCopy = 8,
    Cancel = 9,
    UnbindController = 10,
}
impl Operation {
    fn decode(n: u16) -> Result<Self, Error> {
        Ok(match n {
            1 => Self::Hello,
            2 => Self::BindController,
            3 => Self::ReportMode,
            4 => Self::GetClipboard,
            5 => Self::ReadClipboard,
            6 => Self::BeginCopy,
            7 => Self::WriteCopy,
            8 => Self::CommitCopy,
            9 => Self::Cancel,
            10 => Self::UnbindController,
            _ => return Err(Error::Unsupported),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Mode {
    Insert = 1,
    Normal = 2,
    Visual = 3,
    Command = 4,
    Application = 5,
}
impl Mode {
    pub fn decode(n: u8) -> Result<Self, Error> {
        Ok(match n {
            1 => Self::Insert,
            2 => Self::Normal,
            3 => Self::Visual,
            4 => Self::Command,
            5 => Self::Application,
            _ => return Err(Error::Unsupported),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Malformed,
    Unsupported,
    TooLarge,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Header {
    pub operation: Operation,
    pub response: bool,
    pub request_id: u64,
    pub length: usize,
}
impl Header {
    pub fn decode(bytes: &[u8]) -> Result<Self, Error> {
        if bytes.len() != HEADER_BYTES || bytes[..4] != *b"HIN1" {
            return Err(Error::Malformed);
        }
        if u16::from_le_bytes(bytes[4..6].try_into().unwrap()) != VERSION {
            return Err(Error::Unsupported);
        }
        let operation = Operation::decode(u16::from_le_bytes(bytes[6..8].try_into().unwrap()))?;
        let length = u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as usize;
        let flags = u32::from_le_bytes(bytes[12..16].try_into().unwrap());
        let request_id = u64::from_le_bytes(bytes[16..24].try_into().unwrap());
        if length < HEADER_BYTES || flags & !RESPONSE != 0 || request_id == 0 {
            return Err(Error::Malformed);
        }
        if length > MAX_RECORD {
            return Err(Error::TooLarge);
        }
        Ok(Self {
            operation,
            response: flags == RESPONSE,
            request_id,
            length,
        })
    }

    pub fn encode(self) -> Result<[u8; HEADER_BYTES], Error> {
        if self.length < HEADER_BYTES || self.request_id == 0 {
            return Err(Error::Malformed);
        }
        if self.length > MAX_RECORD {
            return Err(Error::TooLarge);
        }
        let mut out = [0; HEADER_BYTES];
        out[..4].copy_from_slice(b"HIN1");
        out[4..6].copy_from_slice(&VERSION.to_le_bytes());
        out[6..8].copy_from_slice(&(self.operation as u16).to_le_bytes());
        out[8..12].copy_from_slice(&(self.length as u32).to_le_bytes());
        out[12..16].copy_from_slice(&(if self.response { RESPONSE } else { 0 }).to_le_bytes());
        out[16..24].copy_from_slice(&self.request_id.to_le_bytes());
        Ok(out)
    }
}

/// Borrow an exact envelope; callers must additionally validate the typed body and direction.
pub fn frame(bytes: &[u8]) -> Result<(Header, &[u8]), Error> {
    let header = Header::decode(bytes.get(..HEADER_BYTES).ok_or(Error::Malformed)?)?;
    if bytes.len() != header.length {
        return Err(Error::Malformed);
    }
    Ok((header, &bytes[HEADER_BYTES..]))
}

/// Export adapters normalize newlines before this canonical clipboard check.
pub fn clipboard_text(bytes: &[u8]) -> Result<&str, Error> {
    if bytes.len() > MAX_TEXT {
        return Err(Error::TooLarge);
    }
    let text = core::str::from_utf8(bytes).map_err(|_| Error::Malformed)?;
    if text
        .chars()
        .any(|c| c.is_control() && c != '\t' && c != '\n')
    {
        return Err(Error::Malformed);
    }
    Ok(text)
}

#[cfg(test)]
mod tests {
    use super::*;
    const HELLO: [u8; 24] = [
        0x48, 0x49, 0x4e, 0x31, 1, 0, 1, 0, 24, 0, 0, 0, 0, 0, 0, 0, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01,
    ];
    #[test]
    fn fixed_fixture_and_exact_extent() {
        let expected = Header {
            operation: Operation::Hello,
            response: false,
            request_id: 0x0102030405060708,
            length: 24,
        };
        assert_eq!(expected.encode(), Ok(HELLO));
        assert_eq!(frame(&HELLO), Ok((expected, &[][..])));
        for end in 0..24 {
            assert!(frame(&HELLO[..end]).is_err());
        }
        let mut trailing = HELLO.to_vec();
        trailing.push(0);
        assert_eq!(frame(&trailing), Err(Error::Malformed));
    }
    #[test]
    fn bounds_and_unknown_envelope_fields_refuse() {
        for (offset, value) in [(0, 0), (4, 2), (6, 0), (6, 11), (12, 2), (15, 128)] {
            let mut bad = HELLO;
            bad[offset] = value;
            assert!(frame(&bad).is_err());
        }
        let mut bad = HELLO;
        bad[16..24].fill(0);
        assert_eq!(frame(&bad), Err(Error::Malformed));
        for length in [0u32, 23, 32769, u32::MAX] {
            let mut bad = HELLO;
            bad[8..12].copy_from_slice(&length.to_le_bytes());
            assert!(Header::decode(&bad).is_err());
        }
        let mut reply = Header::decode(&HELLO).unwrap();
        reply.response = true;
        reply.length = MAX_RECORD;
        assert_eq!(Header::decode(&reply.encode().unwrap()), Ok(reply));
    }
    #[test]
    fn clipboard_is_canonical_text_without_terminal_controls() {
        for text in [
            "",
            "one\ttwo\nthree",
            "žluťoučký",
            "👩\u{200d}💻",
            "a\u{301}",
        ] {
            assert_eq!(clipboard_text(text.as_bytes()), Ok(text));
        }
        for bytes in [
            &b"\x1b[31m"[..],
            &b"a\0b"[..],
            &b"a\rb"[..],
            &b"\x7f"[..],
            &b"\xff"[..],
            "\u{0085}".as_bytes(),
        ] {
            assert_eq!(clipboard_text(bytes), Err(Error::Malformed));
        }
        let mut full = alloc::vec![b'x'; MAX_TEXT];
        assert!(clipboard_text(&full).is_ok());
        full.push(b'x');
        assert_eq!(clipboard_text(&full), Err(Error::TooLarge));
    }
}
