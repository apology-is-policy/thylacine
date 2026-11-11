// The guest's display, as QEMU's `-display dbus,p2p=on` serves it: a private D-Bus connection, a listener
// QEMU calls, and the scanout mapped from a file descriptor (Listener.Unix.Map).
//
// QEMU's main loop calls the listener SYNCHRONOUSLY (the proxy's GetAll, ScanoutMap), so both connections
// live on one thread that never leaves block_on. A listener nobody drives stalls the whole VM.
use std::io::{BufRead, BufReader, Write};
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, OwnedFd};
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::Duration;
use zbus::Fd;

const CONSOLE: &str = "/org/qemu/Display1/Console_0";
const LISTENER: &str = "/org/qemu/Display1/Listener";
pub const PIXMAN_X8R8G8B8: u32 = 0x2002_0888;
pub const PIXMAN_A8R8G8B8: u32 = 0x2002_8888;
// Metal's largest 2D texture on Apple silicon: a scanout beyond it cannot be shown.
const MAX_SIDE: u32 = 16384;
// QEMU answers QMP and the display's first calls from its main loop; one that is hung never does.
const ATTACH_TIMEOUT: Duration = Duration::from_secs(10);
// virtio-gpu's pointer is 64 a side; this is what the window server takes without complaint.
const MAX_POINTER_SIDE: i32 = 256;

pub struct Region {
    base: *mut u8, // what mmap gave, and what munmap takes back
    mapped: usize,
    lead: usize, // from `base` to the first byte that was asked for
    len: usize,
}
unsafe impl Send for Region {}
unsafe impl Sync for Region {}
impl Region {
    // `len` bytes of a file QEMU shares, read-only, starting `offset` bytes in. The file must hold
    // them: a mapping past the end of its object is granted and then faults on the first read. The
    // offset is wherever the guest put its frame in the resource, so it need not be on a page; a
    // mapping must start on one.
    fn map(fd: BorrowedFd<'_>, offset: u64, len: usize) -> std::io::Result<Region> {
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::fstat(fd.as_raw_fd(), &mut st) } != 0 {
            return Err(std::io::Error::last_os_error());
        }
        let end = offset.checked_add(len as u64);
        if end.is_none_or(|end| st.st_size < 0 || (st.st_size as u64) < end) {
            return Err(std::io::Error::other(format!(
                "the shared file holds {} bytes, the scanout needs {len} at {offset}",
                st.st_size
            )));
        }
        let page = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
        if page <= 0 {
            return Err(std::io::Error::last_os_error());
        }
        let lead = (offset % page as u64) as usize;
        let mapped = len + lead;
        let p = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                mapped,
                libc::PROT_READ,
                libc::MAP_SHARED,
                fd.as_raw_fd(),
                (offset - lead as u64) as libc::off_t,
            )
        };
        if p == libc::MAP_FAILED {
            return Err(std::io::Error::last_os_error());
        }
        Ok(Region {
            base: p as *mut u8,
            mapped,
            lead,
            len,
        })
    }
    pub fn as_ptr(&self) -> *const u8 {
        // SAFETY: `lead` is inside the mapping, which is `lead + len` bytes.
        unsafe { self.base.add(self.lead) }
    }
    pub fn len(&self) -> usize {
        self.len
    }
}
impl Drop for Region {
    fn drop(&mut self) {
        unsafe { libc::munmap(self.base as *mut libc::c_void, self.mapped) };
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Rect {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
}
impl Rect {
    pub fn union(self, o: Rect) -> Rect {
        let x0 = self.x.min(o.x);
        let y0 = self.y.min(o.y);
        let x1 = (self.x + self.w).max(o.x + o.w);
        let y1 = (self.y + self.h).max(o.y + o.h);
        Rect {
            x: x0,
            y: y0,
            w: x1 - x0,
            h: y1 - y0,
        }
    }
    // The part of a guest-supplied rectangle that lies inside a w x h surface.
    pub fn clamp(x: i32, y: i32, w: i32, h: i32, sw: u32, sh: u32) -> Option<Rect> {
        let x0 = (x.max(0) as i64).min(sw as i64);
        let y0 = (y.max(0) as i64).min(sh as i64);
        let x1 = (x as i64 + w.max(0) as i64).clamp(x0, sw as i64);
        let y1 = (y as i64 + h.max(0) as i64).clamp(y0, sh as i64);
        if x1 == x0 || y1 == y0 {
            return None;
        }
        Some(Rect {
            x: x0 as u32,
            y: y0 as u32,
            w: (x1 - x0) as u32,
            h: (y1 - y0) as u32,
        })
    }
}

pub enum Pixels {
    Mapped(Arc<Region>),
    Owned(Vec<u8>),
}

pub struct Scanout {
    pub pixels: Pixels,
    pub w: u32,
    pub h: u32,
    pub stride: u32,
    pub serial: u64, // a new value for every scanout the guest sets
}

// The guest's pointer. It is not in the scanout: the guest hands QEMU an image and a hot spot, and
// whoever shows the screen draws it. Rows of B, G, R, A, with the colours not multiplied by the alpha.
#[derive(Debug, PartialEq)]
pub struct Pointer {
    pub w: u32,
    pub h: u32,
    pub hot_x: u32,
    pub hot_y: u32,
    pub bgra: Vec<u8>,
}
impl Pointer {
    // The image if it is one that can be shown. A hot spot outside it is brought to its edge.
    pub fn new(width: i32, height: i32, hot_x: i32, hot_y: i32, bgra: Vec<u8>) -> Option<Pointer> {
        if !(1..=MAX_POINTER_SIDE).contains(&width) || !(1..=MAX_POINTER_SIDE).contains(&height) {
            return None;
        }
        if bgra.len() != width as usize * height as usize * 4 {
            return None;
        }
        Some(Pointer {
            w: width as u32,
            h: height as u32,
            hot_x: hot_x.clamp(0, width - 1) as u32,
            hot_y: hot_y.clamp(0, height - 1) as u32,
            bgra,
        })
    }
    // The same rows as R, G, B, A.
    pub fn rgba(&self) -> Vec<u8> {
        let mut out = self.bgra.clone();
        for px in out.chunks_exact_mut(4) {
            px.swap(0, 2);
        }
        out
    }
}

pub struct Shared {
    pub scanout: Option<Scanout>,
    pub damage: Option<Rect>,
    pub serials: u64,
    pub update_calls: u64,
    pub pointer: Option<Arc<Pointer>>, // None until the guest defines one
    pub pointer_on: bool,              // the guest shows its pointer
}
impl Default for Shared {
    fn default() -> Shared {
        Shared {
            scanout: None,
            damage: None,
            serials: 0,
            update_calls: 0,
            pointer: None,
            pointer_on: true,
        }
    }
}

pub struct Display {
    pub shared: Arc<Mutex<Shared>>,
    pub dirty: Arc<AtomicBool>,
    pub pointer_dirty: Arc<AtomicBool>,
    pub input: mpsc::Sender<Input>,
}
impl Display {
    // What Listener.CursorDefine does with its arguments.
    pub fn define_pointer(&self, width: i32, height: i32, hot_x: i32, hot_y: i32, bgra: Vec<u8>) {
        define_pointer(
            &self.shared,
            &self.pointer_dirty,
            Pointer::new(width, height, hot_x, hot_y, bgra),
        );
    }
    // What Listener.MouseSet does with its last argument.
    pub fn show_pointer(&self, on: bool) {
        show_pointer(&self.shared, &self.pointer_dirty, on);
    }
}

// An image that cannot be shown leaves the pointer the guest had.
fn define_pointer(shared: &Mutex<Shared>, dirty: &AtomicBool, pointer: Option<Pointer>) {
    let Some(pointer) = pointer else { return };
    shared.lock().unwrap().pointer = Some(Arc::new(pointer));
    dirty.store(true, Ordering::Release);
}

// One call per pointer move, so it costs a comparison unless the guest changed its mind.
fn show_pointer(shared: &Mutex<Shared>, dirty: &AtomicBool, on: bool) {
    let mut s = shared.lock().unwrap();
    if s.pointer_on != on {
        s.pointer_on = on;
        dirty.store(true, Ordering::Release);
    }
}

#[derive(Debug, PartialEq)]
pub enum Input {
    Key { qnum: u32, down: bool },
    Abs { x: u32, y: u32 },
    Button { button: u32, down: bool },
    Wheel { up: bool },
}

struct State {
    shared: Arc<Mutex<Shared>>,
    dirty: Arc<AtomicBool>,
    pointer_dirty: Arc<AtomicBool>,
    map: bool,
}
impl State {
    fn set(&self, pixels: Pixels, w: u32, h: u32, stride: u32) {
        let mut s = self.shared.lock().unwrap();
        s.serials += 1;
        let serial = s.serials;
        s.scanout = Some(Scanout {
            pixels,
            w,
            h,
            stride,
            serial,
        });
        s.damage = Some(Rect { x: 0, y: 0, w, h });
        self.dirty.store(true, Ordering::Release);
    }
    fn damage(&self, x: i32, y: i32, w: i32, h: i32) {
        let mut s = self.shared.lock().unwrap();
        s.update_calls += 1;
        let Some(sc) = &s.scanout else { return };
        let Some(r) = Rect::clamp(x, y, w, h, sc.w, sc.h) else {
            return;
        };
        s.damage = Some(match s.damage {
            Some(d) => d.union(r),
            None => r,
        });
        self.dirty.store(true, Ordering::Release);
    }
}

// The arithmetic on frame sizes below is done in usize and assumes it cannot wrap.
const _: () = assert!(usize::BITS == 64);

// The bytes a width x height scanout needs when its rows are `stride` apart: up to the last pixel of
// the last row, and not the padding after it, which a frame placed at the end of its resource does
// not have. An error if it is not a scanout that can be shown; everything past this point may assume
// a frame of at least one pixel and at most MAX_SIDE a side, with rows a whole number of pixels apart.
fn geometry(width: u32, height: u32, stride: u32) -> zbus::fdo::Result<usize> {
    if width == 0 || height == 0 || width > MAX_SIDE || height > MAX_SIDE {
        return Err(zbus::fdo::Error::InvalidArgs(format!(
            "a scanout of {width}x{height}"
        )));
    }
    if stride < width * 4 || !stride.is_multiple_of(4) || stride > i32::MAX as u32 {
        return Err(zbus::fdo::Error::InvalidArgs(format!(
            "a stride of {stride} for {width} pixels"
        )));
    }
    Ok((height as usize - 1) * stride as usize + width as usize * 4)
}

fn supported(format: u32) -> zbus::fdo::Result<()> {
    if format == PIXMAN_X8R8G8B8 || format == PIXMAN_A8R8G8B8 {
        return Ok(());
    }
    Err(zbus::fdo::Error::NotSupported(format!(
        "pixman format {format:#010x}"
    )))
}

struct Listener(Arc<State>);
// spawn = false: a call is finished before the next is looked at. A scanout and the updates to it
// only mean something in the order QEMU sent them.
#[zbus::interface(name = "org.qemu.Display1.Listener", spawn = false)]
impl Listener {
    fn scanout(
        &mut self,
        width: u32,
        height: u32,
        stride: u32,
        pixman_format: u32,
        data: Vec<u8>,
    ) -> zbus::fdo::Result<()> {
        supported(pixman_format)?;
        if data.len() < geometry(width, height, stride)? {
            return Err(zbus::fdo::Error::InvalidArgs(
                "scanout shorter than its geometry".into(),
            ));
        }
        self.0.set(Pixels::Owned(data), width, height, stride);
        Ok(())
    }
    #[allow(clippy::too_many_arguments)]
    fn update(
        &mut self,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        stride: u32,
        pixman_format: u32,
        data: Vec<u8>,
    ) -> zbus::fdo::Result<()> {
        supported(pixman_format)?;
        {
            let mut s = self.0.shared.lock().unwrap();
            let Some(sc) = &mut s.scanout else {
                return Ok(());
            };
            let (sw, sh, sstride) = (sc.w, sc.h, sc.stride as usize);
            let Pixels::Owned(buf) = &mut sc.pixels else {
                return Ok(());
            };
            let Some(r) = Rect::clamp(x, y, width, height, sw, sh) else {
                return Ok(());
            };
            // The rectangle QEMU sent starts at (x, y); rows are `stride` apart in `data`.
            let (dx, dy) = (
                (r.x as i64 - x as i64) as usize,
                (r.y as i64 - y as i64) as usize,
            );
            for row in 0..r.h as usize {
                let src = (dy + row) * stride as usize + dx * 4;
                let dst = (r.y as usize + row) * sstride + r.x as usize * 4;
                let n = r.w as usize * 4;
                if src + n > data.len() || dst + n > buf.len() {
                    break;
                }
                buf[dst..dst + n].copy_from_slice(&data[src..src + n]);
            }
        }
        self.0.damage(x, y, width, height);
        Ok(())
    }
    fn disable(&mut self) {
        let mut s = self.0.shared.lock().unwrap();
        s.scanout = None;
        s.damage = None;
        self.0.dirty.store(true, Ordering::Release);
    }
    // The position is the guest repeating the one it was sent, so only whether it shows its pointer
    // is news. QEMU says so once when the listener registers, before any guest has spoken.
    fn mouse_set(&mut self, _x: i32, _y: i32, on: i32) {
        show_pointer(&self.0.shared, &self.0.pointer_dirty, on != 0);
    }
    fn cursor_define(&mut self, width: i32, height: i32, hot_x: i32, hot_y: i32, data: Vec<u8>) {
        define_pointer(
            &self.0.shared,
            &self.0.pointer_dirty,
            Pointer::new(width, height, hot_x, hot_y, data),
        );
    }
    #[zbus(property)]
    fn interfaces(&self) -> Vec<String> {
        // QEMU shares the scanout's memory only with a listener that says it can map it.
        if self.0.map {
            vec!["org.qemu.Display1.Listener.Unix.Map".into()]
        } else {
            Vec::new()
        }
    }
}

struct MapListener(Arc<State>);
#[zbus::interface(name = "org.qemu.Display1.Listener.Unix.Map", spawn = false)]
impl MapListener {
    // An error here makes QEMU fall back to sending pixels in the messages (Listener.Scanout/Update).
    fn scanout_map(
        &mut self,
        handle: Fd<'_>,
        offset: u32,
        width: u32,
        height: u32,
        stride: u32,
        pixman_format: u32,
    ) -> zbus::fdo::Result<()> {
        supported(pixman_format)?;
        let len = geometry(width, height, stride)?;
        let region = Region::map(handle.as_fd(), offset as u64, len).map_err(|e| {
            eprintln!(
                "display: the scanout's memory was not mapped ({e}); QEMU sends pixels instead"
            );
            zbus::fdo::Error::Failed(e.to_string())
        })?;
        self.0
            .set(Pixels::Mapped(Arc::new(region)), width, height, stride);
        Ok(())
    }
    fn update_map(&mut self, x: i32, y: i32, width: i32, height: i32) {
        self.0.damage(x, y, width, height);
    }
}

fn send_with_fd(sock: &UnixStream, data: &[u8], fd: i32) -> std::io::Result<()> {
    let mut iov = libc::iovec {
        iov_base: data.as_ptr() as *mut libc::c_void,
        iov_len: data.len(),
    };
    let space = unsafe { libc::CMSG_SPACE(std::mem::size_of::<i32>() as u32) } as usize;
    let mut cbuf = vec![0u8; space];
    let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
    msg.msg_iov = &mut iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf.as_mut_ptr() as *mut libc::c_void;
    msg.msg_controllen = space as _;
    unsafe {
        let c = libc::CMSG_FIRSTHDR(&msg);
        (*c).cmsg_level = libc::SOL_SOCKET;
        (*c).cmsg_type = libc::SCM_RIGHTS;
        (*c).cmsg_len = libc::CMSG_LEN(std::mem::size_of::<i32>() as u32) as _;
        std::ptr::copy_nonoverlapping(
            &fd as *const i32 as *const u8,
            libc::CMSG_DATA(c),
            std::mem::size_of::<i32>(),
        );
        if libc::sendmsg(sock.as_raw_fd(), &msg, 0) < 0 {
            return Err(std::io::Error::last_os_error());
        }
    }
    Ok(())
}

// One QMP command; events are skipped. An error reply is an Err.
fn qmp_cmd(
    w: &UnixStream,
    r: &mut BufReader<UnixStream>,
    json: &str,
    fd: Option<i32>,
) -> std::io::Result<()> {
    let line = format!("{json}\n");
    match fd {
        Some(fd) => send_with_fd(w, line.as_bytes(), fd)?,
        None => (&*w).write_all(line.as_bytes())?,
    }
    loop {
        let mut reply = String::new();
        if r.read_line(&mut reply)? == 0 {
            return Err(std::io::Error::other("QMP closed"));
        }
        if reply.contains("\"event\"") {
            continue;
        }
        if reply.contains("\"error\"") {
            return Err(std::io::Error::other(format!("{json} -> {}", reply.trim())));
        }
        return Ok(());
    }
}

// Hand QEMU one end of a socket pair over QMP and make it a display client; returns our end.
fn attach(qmp_path: &str) -> std::io::Result<UnixStream> {
    attach_qmp(qmp_path).map_err(|e| {
        let why = match e.kind() {
            std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut => {
                format!("no answer in {} s", ATTACH_TIMEOUT.as_secs())
            }
            _ => e.to_string(),
        };
        std::io::Error::other(format!("QEMU's monitor at {qmp_path}: {why}"))
    })
}

fn attach_qmp(qmp_path: &str) -> std::io::Result<UnixStream> {
    let w = UnixStream::connect(qmp_path)?;
    w.set_read_timeout(Some(ATTACH_TIMEOUT))?;
    w.set_write_timeout(Some(ATTACH_TIMEOUT))?;
    let mut r = BufReader::new(w.try_clone()?);
    let mut greeting = String::new();
    r.read_line(&mut greeting)?;
    qmp_cmd(&w, &mut r, r#"{"execute":"qmp_capabilities"}"#, None)?;
    let (ours, theirs) = UnixStream::pair()?;
    qmp_cmd(
        &w,
        &mut r,
        r#"{"execute":"getfd","arguments":{"fdname":"thylacine-display"}}"#,
        Some(theirs.as_raw_fd()),
    )?;
    qmp_cmd(
        &w,
        &mut r,
        r#"{"execute":"add_client","arguments":{"protocol":"@dbus-display","fdname":"thylacine-display"}}"#,
        None,
    )?;
    Ok(ours)
}

async fn serve(
    sock: UnixStream,
    state: Arc<State>,
) -> zbus::Result<(zbus::Connection, zbus::Connection)> {
    let conn = zbus::connection::Builder::unix_stream(sock)
        .p2p()
        .build()
        .await?;
    let (l0, l1) = UnixStream::pair()?;
    let l0: OwnedFd = l0.into();
    conn.call_method(
        Some("org.qemu"),
        CONSOLE,
        Some("org.qemu.Display1.Console"),
        "RegisterListener",
        &(Fd::from(l0.as_fd()),),
    )
    .await?;
    drop(l0);
    let lconn = zbus::connection::Builder::unix_stream(l1)
        .p2p()
        .serve_at(LISTENER, Listener(state.clone()))
        .serve_at(LISTENER, MapListener(state))
        .build()
        .await?;
    Ok((conn, lconn))
}

// The next event to send, given `first` and whatever is already queued behind it. Of a run of pointer
// positions only the last is worth a call: the guest wants where the pointer is, not where it was while
// QEMU was busy. An event of another kind ends the run and is handed back for the next turn.
fn next_input(first: Input, mut more: impl FnMut() -> Option<Input>) -> (Input, Option<Input>) {
    let mut ev = first;
    while matches!(ev, Input::Abs { .. }) {
        match more() {
            Some(next @ Input::Abs { .. }) => ev = next,
            other => return (ev, other),
        }
    }
    (ev, None)
}

fn send_input(conn: &zbus::Connection, ev: Input) -> zbus::Result<()> {
    macro_rules! call {
        ($iface:expr, $method:expr, $body:expr) => {
            zbus::block_on(conn.call_method(
                Some("org.qemu"),
                CONSOLE,
                Some($iface),
                $method,
                $body,
            ))
            .map(|_| ())
        };
    }
    match ev {
        Input::Key { qnum, down } => call!(
            "org.qemu.Display1.Keyboard",
            if down { "Press" } else { "Release" },
            &(qnum,)
        ),
        Input::Abs { x, y } => call!("org.qemu.Display1.Mouse", "SetAbsPosition", &(x, y)),
        Input::Button { button, down } => call!(
            "org.qemu.Display1.Mouse",
            if down { "Press" } else { "Release" },
            &(button,)
        ),
        Input::Wheel { up } => {
            let b = if up { 3u32 } else { 4u32 };
            call!("org.qemu.Display1.Mouse", "Press", &(b,))?;
            call!("org.qemu.Display1.Mouse", "Release", &(b,))
        }
    }
}

pub fn connect(qmp_path: &str) -> Result<Display, Box<dyn std::error::Error>> {
    let sock = attach(qmp_path)?;
    let shared: Arc<Mutex<Shared>> = Arc::default();
    let dirty = Arc::new(AtomicBool::new(false));
    let pointer_dirty = Arc::new(AtomicBool::new(false));
    // THYLACINE_NO_MAP=1 makes QEMU send pixels in its messages instead: the path a host without a
    // shareable surface takes, kept reachable so that it can be tried.
    let map = std::env::var_os("THYLACINE_NO_MAP").is_none_or(|v| v != "1");
    let state = Arc::new(State {
        shared: shared.clone(),
        dirty: dirty.clone(),
        pointer_dirty: pointer_dirty.clone(),
        map,
    });
    let (tx, rx) = mpsc::channel::<Result<zbus::Connection, String>>();
    std::thread::Builder::new()
        .name("display".into())
        .spawn(move || {
            zbus::block_on(async move {
                match serve(sock, state).await {
                    Ok((conn, lconn)) => {
                        tx.send(Ok(conn)).ok();
                        let _keep = lconn;
                        std::future::pending::<()>().await
                    }
                    Err(e) => {
                        tx.send(Err(e.to_string())).ok();
                    }
                }
            })
        })?;
    let conn = match rx.recv_timeout(ATTACH_TIMEOUT) {
        Ok(r) => r?,
        Err(_) => return Err("QEMU did not answer on its display connection".into()),
    };
    // Input has a thread of its own so that the window's thread never waits for QEMU.
    let (itx, irx) = mpsc::channel::<Input>();
    std::thread::Builder::new()
        .name("input".into())
        .spawn(move || {
            let mut held_back = None;
            let mut refused = 0u64;
            while let Some(first) = held_back.take().or_else(|| irx.recv().ok()) {
                let (ev, rest) = next_input(first, || irx.try_recv().ok());
                held_back = rest;
                match send_input(&conn, ev) {
                    Ok(()) => {}
                    Err(zbus::Error::InputOutput(e)) => {
                        eprintln!("input: QEMU is gone ({e}); nothing more is sent");
                        break;
                    }
                    // QEMU still answers but will not take the event: another client took its
                    // console, or the position was outside a screen that has just changed.
                    Err(e) => {
                        refused += 1;
                        if refused.is_power_of_two() {
                            eprintln!("input: QEMU refused an event ({e}); {refused} so far");
                        }
                    }
                }
            }
        })?;
    Ok(Display {
        shared,
        dirty,
        pointer_dirty,
        input: itx,
    })
}

#[cfg(test)]
mod tests {
    use super::{
        define_pointer, geometry, next_input, show_pointer, Input, Pointer, Rect, Region, Shared,
        MAX_POINTER_SIDE, MAX_SIDE,
    };
    use std::os::fd::AsFd;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Mutex;

    #[test]
    fn a_pointer_is_an_image_that_can_be_shown() {
        let px = |n: i32| vec![0u8; n as usize * 4];
        let p = Pointer::new(64, 64, 3, 5, px(64 * 64)).unwrap();
        assert_eq!((p.w, p.h, p.hot_x, p.hot_y), (64, 64, 3, 5));
        assert!(Pointer::new(MAX_POINTER_SIDE, 1, 0, 0, px(MAX_POINTER_SIDE)).is_some());
        assert!(Pointer::new(0, 64, 0, 0, Vec::new()).is_none());
        assert!(Pointer::new(64, 0, 0, 0, Vec::new()).is_none());
        assert!(Pointer::new(-1, 64, 0, 0, px(64)).is_none());
        assert!(Pointer::new(MAX_POINTER_SIDE + 1, 1, 0, 0, px(MAX_POINTER_SIDE + 1)).is_none());
        assert!(Pointer::new(i32::MAX, i32::MAX, 0, 0, px(4)).is_none());
        // The bytes are the image, no fewer and no more.
        assert!(Pointer::new(2, 2, 0, 0, px(3)).is_none());
        assert!(Pointer::new(2, 2, 0, 0, px(5)).is_none());
        // A hot spot outside the image is brought to its edge.
        let p = Pointer::new(4, 2, 9, -3, px(8)).unwrap();
        assert_eq!((p.hot_x, p.hot_y), (3, 0));
        let p = Pointer::new(4, 2, i32::MIN, i32::MAX, px(8)).unwrap();
        assert_eq!((p.hot_x, p.hot_y), (0, 1));
    }

    #[test]
    fn a_pointer_keeps_its_alpha_and_exchanges_red_and_blue() {
        let p = Pointer::new(2, 1, 0, 0, vec![1, 2, 3, 4, 250, 251, 252, 0]).unwrap();
        assert_eq!(p.rgba(), vec![3, 2, 1, 4, 252, 251, 250, 0]);
    }

    #[test]
    fn a_pointer_that_cannot_be_shown_leaves_the_last_one() {
        let shared = Mutex::new(Shared::default());
        let dirty = AtomicBool::new(false);
        assert!(shared.lock().unwrap().pointer_on);
        define_pointer(&shared, &dirty, Pointer::new(1, 1, 0, 0, vec![9, 8, 7, 6]));
        assert!(dirty.swap(false, Ordering::AcqRel));
        define_pointer(&shared, &dirty, Pointer::new(0, 0, 0, 0, Vec::new()));
        assert!(!dirty.load(Ordering::Acquire));
        assert_eq!(
            shared.lock().unwrap().pointer.as_ref().unwrap().bgra,
            [9, 8, 7, 6]
        );
    }

    #[test]
    fn hiding_the_pointer_is_news_once() {
        let shared = Mutex::new(Shared::default());
        let dirty = AtomicBool::new(false);
        show_pointer(&shared, &dirty, true);
        assert!(!dirty.load(Ordering::Acquire));
        show_pointer(&shared, &dirty, false);
        assert!(dirty.swap(false, Ordering::AcqRel));
        assert!(!shared.lock().unwrap().pointer_on);
        show_pointer(&shared, &dirty, false);
        assert!(!dirty.load(Ordering::Acquire));
        show_pointer(&shared, &dirty, true);
        assert!(dirty.load(Ordering::Acquire));
        assert!(shared.lock().unwrap().pointer_on);
    }

    #[test]
    fn of_a_run_of_positions_the_last_is_sent() {
        let abs = |x| Input::Abs { x, y: 7 };
        let key = Input::Key {
            qnum: 30,
            down: true,
        };
        let mut q: std::collections::VecDeque<Input> = [abs(2), abs(3), key, abs(4), abs(5)].into();
        // Three positions, then a key: the last position goes out and the key is handed back.
        let (ev, rest) = next_input(abs(1), || q.pop_front());
        assert_eq!(ev, abs(3));
        assert_eq!(
            rest,
            Some(Input::Key {
                qnum: 30,
                down: true
            })
        );
        // The key is sent as it is, and nothing behind it is looked at.
        let (ev, rest2) = next_input(rest.unwrap(), || q.pop_front());
        assert_eq!(
            ev,
            Input::Key {
                qnum: 30,
                down: true
            }
        );
        assert_eq!(rest2, None);
        assert_eq!(q.len(), 2);
        // A run that ends with the queue: the last position, nothing handed back.
        let first = q.pop_front().unwrap();
        let (ev, rest) = next_input(first, || q.pop_front());
        assert_eq!(ev, abs(5));
        assert_eq!(rest, None);
        // A single position with nothing behind it is itself.
        assert_eq!(next_input(abs(9), || None), (abs(9), None));
    }

    #[test]
    fn a_scanout_has_a_size_that_can_be_shown() {
        assert_eq!(geometry(640, 480, 2560).ok(), Some(480 * 2560));
        // Rows further apart than they are wide: the last row has no padding to count.
        assert_eq!(geometry(640, 480, 4096).ok(), Some(479 * 4096 + 2560));
        assert_eq!(geometry(1, 1, 4096).ok(), Some(4));
        // A stride is a whole number of pixels, and one QEMU itself would allow.
        assert!(geometry(640, 480, 2562).is_err());
        assert!(geometry(640, 480, 0x8000_0000).is_err());
        assert_eq!(geometry(1, 2, 0x7fff_fffc).ok(), Some(0x7fff_fffc + 4));
        assert_eq!(
            geometry(MAX_SIDE, MAX_SIDE, MAX_SIDE * 4).ok(),
            Some(MAX_SIDE as usize * MAX_SIDE as usize * 4)
        );
        assert!(geometry(0, 480, 2560).is_err());
        assert!(geometry(640, 0, 2560).is_err());
        assert!(geometry(640, 480, 2559).is_err());
        assert!(geometry(MAX_SIDE + 1, 480, u32::MAX).is_err());
        assert!(geometry(640, MAX_SIDE + 1, 2560).is_err());
        assert!(geometry(u32::MAX, u32::MAX, u32::MAX).is_err());
    }

    #[test]
    fn a_mapping_stays_inside_the_shared_file() {
        let path = std::env::temp_dir().join(format!("thylacine-mac-map-{}", std::process::id()));
        let f = std::fs::File::create_new(&path).unwrap();
        std::fs::remove_file(&path).unwrap();
        f.set_len(8192).unwrap();
        let r = Region::map(f.as_fd(), 0, 8192).unwrap();
        assert_eq!(r.len(), 8192);
        assert!(Region::map(f.as_fd(), 0, 4096).is_ok());
        assert!(Region::map(f.as_fd(), 0, 8193).is_err());
        assert!(Region::map(f.as_fd(), 4096, 8192).is_err());
        assert!(Region::map(f.as_fd(), u64::MAX, 4096).is_err());
        // The last byte of the file is in; one more is not.
        assert!(Region::map(f.as_fd(), 5, 8187).is_ok());
        assert!(Region::map(f.as_fd(), 5, 8188).is_err());
    }

    #[test]
    fn a_mapping_starts_where_the_frame_does() {
        use std::io::Write;
        let page = unsafe { libc::sysconf(libc::_SC_PAGESIZE) } as usize;
        let path = std::env::temp_dir().join(format!("thylacine-mac-lead-{}", std::process::id()));
        let mut f = std::fs::File::create_new(&path).unwrap();
        std::fs::remove_file(&path).unwrap();
        let bytes: Vec<u8> = (0..3 * page).map(|i| (i % 251) as u8).collect();
        f.write_all(&bytes).unwrap();
        // Offsets a guest's frame can have: inside the first page, on a page, just past one.
        for offset in [0, 4, 4096, page - 4, page, page + 4, 2 * page + 12] {
            let len = page / 2 + 8;
            let r = Region::map(f.as_fd(), offset as u64, len).unwrap();
            assert_eq!(r.len(), len);
            let got = unsafe { std::slice::from_raw_parts(r.as_ptr(), r.len()) };
            assert_eq!(got, &bytes[offset..offset + len], "offset {offset}");
        }
    }
    #[test]
    fn a_rectangle_is_cut_to_the_surface() {
        assert_eq!(
            Rect::clamp(0, 0, 10, 10, 640, 480),
            Some(Rect {
                x: 0,
                y: 0,
                w: 10,
                h: 10
            })
        );
        assert_eq!(
            Rect::clamp(-5, -5, 10, 10, 640, 480),
            Some(Rect {
                x: 0,
                y: 0,
                w: 5,
                h: 5
            })
        );
        assert_eq!(
            Rect::clamp(635, 475, 10, 10, 640, 480),
            Some(Rect {
                x: 635,
                y: 475,
                w: 5,
                h: 5
            })
        );
        assert_eq!(Rect::clamp(640, 0, 10, 10, 640, 480), None);
        assert_eq!(Rect::clamp(0, 0, 0, 10, 640, 480), None);
        assert_eq!(
            Rect::clamp(i32::MAX, i32::MAX, i32::MAX, i32::MAX, 640, 480),
            None
        );
        assert_eq!(
            Rect::clamp(i32::MIN, i32::MIN, i32::MAX, i32::MAX, 640, 480),
            None
        );
        assert_eq!(
            Rect::clamp(-1, -1, i32::MAX, i32::MAX, 640, 480),
            Some(Rect {
                x: 0,
                y: 0,
                w: 640,
                h: 480
            })
        );
    }
    #[test]
    fn a_union_covers_both() {
        let a = Rect {
            x: 10,
            y: 10,
            w: 5,
            h: 5,
        };
        let b = Rect {
            x: 0,
            y: 12,
            w: 2,
            h: 20,
        };
        assert_eq!(
            a.union(b),
            Rect {
                x: 0,
                y: 10,
                w: 15,
                h: 22
            }
        );
    }
}
