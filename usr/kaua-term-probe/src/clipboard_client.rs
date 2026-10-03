//! Blocking qualification client, deliberately separate from future UI clients.
//! Uses a fresh native /srv connection from this real foreground process.
use ::alloc::{vec, vec::Vec};
use libhalcyon::{
    interaction_body::{Request, Response, Scope},
    interaction_wire::{Header, Mode, Operation, MAX_RECORD},
};
use libthyla_rs::{fs::File, handle::Rights, *};
type Result<T> = core::result::Result<T, &'static str>;
struct Client {
    _root: File,
    file: File,
    next: u64,
    last_errno: i64,
    scope: Scope,
}
fn file(fd: i64) -> Result<File> {
    if fd < 0 {
        t_putstr(&::alloc::format!("clipboard-probe: open errno {}\n", fd));
        return Err("open");
    }
    Ok(unsafe { File::from_raw_fd(fd as i32, Rights::READ | Rights::WRITE) })
}
impl Client {
    fn connect() -> Result<Self> {
        let addr = env::var("HALCYON_INTERACTION").ok_or("no interaction locator")?;
        if !addr.starts_with("/srv/") {
            return Err("bad locator");
        }
        let split = addr[5..].find('/').ok_or("locator shape")? + 5;
        let root = file(unsafe { t_open(T_WALK_OPEN_FROM_ROOT, addr.as_ptr(), split, T_OREAD) })?;
        let path = &addr[split + 1..];
        let child =
            file(unsafe { t_open(root.as_raw_fd() as i64, path.as_ptr(), path.len(), T_ORDWR) })?;
        let mut c = Self {
            _root: root,
            file: child,
            next: 1,
            last_errno: 0,
            scope: Scope {
                session: 1,
                controller: 1,
                context: 1,
                epoch: 1,
            },
        };
        let b = c.call(Request::Hello)?;
        let Response::Hello { session } =
            Response::decode(&b, Operation::Hello, 1).map_err(|_| "hello reply")?
        else {
            return Err("hello body");
        };
        // Foreground handoff and its observer notification are ordered on a
        // separate channel. A stale pre-authority nomination may be Gone. Only
        // registration retries; each attempt has a new ID and a strict bound.
        // Never apply this policy to CommitCopy (its outcome may be unknown).
        for attempt in 0..8 {
            let id = c.next;
            match c.call(Request::Bind {
                session,
                context: 1,
                epoch: 1,
            }) {
                Ok(b) => {
                    let Response::Bound { controller } =
                        Response::decode(&b, Operation::BindController, id)
                            .map_err(|_| "bind reply")?
                    else {
                        return Err("bind body");
                    };
                    c.scope = Scope {
                        session,
                        controller,
                        context: 1,
                        epoch: 1,
                    };
                    return Ok(c);
                }
                Err(_) if attempt < 7 && matches!(c.last_errno, -2 | -16) => {
                    time::sleep(time::Duration::from_millis(20))
                        .map_err(|_| "registration wait")?;
                }
                Err(e) => return Err(e),
            }
        }
        Err("registration deadline")
    }
    fn call(&mut self, q: Request<'_>) -> Result<Vec<u8>> {
        let id = self.next;
        self.next += 1;
        self.last_errno = 0;
        let bytes = q.encode(id).map_err(|_| "encode")?;
        // Split the HIN1 envelope/body independently of 9P frame boundaries.
        for (i, part) in bytes.chunks(2573).enumerate() {
            let n = unsafe {
                t_pwrite(
                    self.file.as_raw_fd() as i64,
                    part.as_ptr(),
                    part.len(),
                    (i * 2573) as i64,
                )
            };
            if n != part.len() as i64 {
                self.last_errno = n;
                t_putstr(&::alloc::format!(
                    "clipboard-probe: request {} write result {}\n",
                    id,
                    n
                ));
                return Err("request write");
            }
        }
        let mut bytes = vec![0; 24];
        let mut got = 0;
        while got < bytes.len() {
            let n = unsafe {
                t_pread(
                    self.file.as_raw_fd() as i64,
                    bytes[got..].as_mut_ptr(),
                    bytes.len() - got,
                    got as i64,
                )
            };
            if n <= 0 {
                return Err("reply header");
            }
            got += n as usize;
        }
        let h = Header::decode(&bytes).map_err(|_| "reply envelope")?;
        if h.request_id != id || !h.response || h.length > MAX_RECORD {
            return Err("reply identity");
        }
        bytes.resize(h.length, 0);
        while got < bytes.len() {
            let count = (bytes.len() - got).min(3001);
            let n = unsafe {
                t_pread(
                    self.file.as_raw_fd() as i64,
                    bytes[got..].as_mut_ptr(),
                    count,
                    got as i64,
                )
            };
            if n <= 0 || n as usize > count {
                return Err("reply fragment");
            }
            got += n as usize;
        }
        Ok(bytes)
    }
    fn get(&mut self) -> Result<(u64, u64, usize)> {
        let id = self.next;
        let b = self.call(Request::Get { scope: self.scope })?;
        match Response::decode(&b, Operation::GetClipboard, id).map_err(|_| "get reply")? {
            Response::Clipboard {
                transfer,
                generation,
                length,
            } => Ok((transfer, generation, length as usize)),
            _ => Err("get body"),
        }
    }
    fn read(&mut self, transfer: u64, offset: usize, count: u32) -> Result<Vec<u8>> {
        let id = self.next;
        let b = self.call(Request::Read {
            transfer,
            offset: offset as u32,
            count,
        })?;
        match Response::decode(&b, Operation::ReadClipboard, id).map_err(|_| "read reply")? {
            Response::Read { offset: got, data } if got == offset as u32 => Ok(data.to_vec()),
            _ => Err("read body"),
        }
    }
}
fn payload() -> Vec<u8> {
    (0..20000).map(|i| b'a' + (i % 26) as u8).collect()
}
fn test(mode: &str) -> Result<()> {
    if !matches!(mode, "copy" | "read" | "hold") {
        return Err("mode");
    }
    let mut c = Client::connect()?;
    c.call(Request::Mode {
        scope: c.scope,
        sequence: 1,
        mode: Mode::Normal,
        readonly: false,
        label: "clipboard qualification",
    })?;
    let expected = payload();
    if mode == "copy" {
        let (old, generation, _) = c.get()?;
        c.call(Request::Cancel { transfer: old })?;
        let id = c.next;
        let b = c.call(Request::Begin {
            scope: c.scope,
            length: expected.len() as u32,
        })?;
        let Response::Begun { transfer } =
            Response::decode(&b, Operation::BeginCopy, id).map_err(|_| "begin reply")?
        else {
            return Err("begin body");
        };
        for (i, part) in expected.chunks(16384).enumerate() {
            c.call(Request::Write {
                transfer,
                offset: (i * 16384) as u32,
                data: part,
            })?;
        }
        let id = c.next;
        let b = c.call(Request::Commit {
            scope: c.scope,
            transfer,
            expected: generation,
        })?;
        if Response::decode(&b, Operation::CommitCopy, id).map_err(|_| "commit reply")?
            != (Response::Committed {
                generation: generation + 1,
            })
        {
            return Err("commit generation");
        }
    }
    let (transfer, _, length) = c.get()?;
    if length != expected.len() {
        return Err("snapshot length");
    }
    let mut bytes = Vec::new();
    while bytes.len() < length {
        let part = c.read(transfer, bytes.len(), 9000)?;
        if part.is_empty() {
            return Err("early snapshot EOF");
        }
        bytes.extend_from_slice(&part);
    }
    if bytes != expected {
        return Err("snapshot bytes");
    }
    if mode == "hold" {
        t_putstr("clipboard-probe: SAK READY\n");
        let mut key = [0u8; 1];
        if unsafe { t_read(0, key.as_mut_ptr(), 1) } <= 0 {
            return Err("hold input");
        }
        let mut output = [0u8; 64];
        if unsafe {
            t_pread(
                c.file.as_raw_fd() as i64,
                output.as_mut_ptr(),
                output.len(),
                0,
            )
        } >= 0
        {
            return Err("SAK retained old connection");
        }
        drop(c);
        let mut c = Client::connect()?;
        let (transfer, _, length) = c.get()?;
        if length != expected.len() || c.read(transfer, 0, 100)? != expected[..100] {
            return Err("post-SAK reconnect");
        }
        c.call(Request::Cancel { transfer })?;
        c.call(Request::Unbind { scope: c.scope })?;
    } else {
        c.call(Request::Cancel { transfer })?;
        c.call(Request::Unbind { scope: c.scope })?;
    }
    Ok(())
}
pub fn run() -> i64 {
    let arg = env::args().nth(2).unwrap_or(b"copy");
    let mode = core::str::from_utf8(arg).unwrap_or("invalid");
    match test(mode) {
        Ok(()) => {
            t_putstr(&::alloc::format!("clipboard-probe: {} PASS\n", mode));
            0
        }
        Err(e) => {
            t_putstr(&::alloc::format!(
                "clipboard-probe: FAIL {} -- {}\n",
                mode,
                e
            ));
            1
        }
    }
}
