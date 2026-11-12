// The guest's display, as QEMU's `-display dbus,p2p=on` serves it: a private D-Bus connection, a listener
// QEMU calls, and the scanout mapped from a file descriptor (Listener.Unix.Map).
//
// QEMU's main loop calls the listener SYNCHRONOUSLY (the proxy's GetAll, ScanoutMap), so both connections
// live on one thread that never leaves block_on. A listener nobody drives stalls the whole VM.
use std::io::{BufRead, BufReader, Write};
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, OwnedFd};
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant};
use zbus::Fd;

const CONSOLE: &str = "/org/qemu/Display1/Console_0";
const LISTENER: &str = "/org/qemu/Display1/Listener";
// Metal's largest 2D texture on Apple silicon: a scanout beyond it cannot be shown.
const MAX_SIDE: u32 = 16384;
// QEMU answers QMP and the display's first calls from its main loop; one that is hung never does.
// The whole attach has this long, not each of its steps.
const ATTACH_TIMEOUT: Duration = Duration::from_secs(10);
// virtio-gpu's pointer is 64 a side. An image past this is not kept.
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

// Where the colours are in a pixel's four bytes. The byte that is left is alpha or nothing, and is
// not shown either way.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Order {
    Bgrx,
    Rgbx,
    Xrgb,
    Xbgr,
}

// The order of a pixman format, if it is one of the eight with a byte a channel: the ones a virtio
// GPU's guest can give a scanout. pixman names a format by its 32-bit word, high byte first, and
// this machine keeps the low byte first.
pub fn order(pixman_format: u32) -> Option<Order> {
    Some(match pixman_format {
        0x2002_0888 | 0x2002_8888 => Order::Bgrx, // x8r8g8b8, a8r8g8b8
        0x2003_0888 | 0x2003_8888 => Order::Rgbx, // x8b8g8r8, a8b8g8r8
        0x2008_0888 | 0x2008_8888 => Order::Xrgb, // b8g8r8x8, b8g8r8a8
        0x2009_0888 | 0x2009_8888 => Order::Xbgr, // r8g8b8x8, r8g8b8a8
        _ => return None,
    })
}

pub struct Scanout {
    pub pixels: Pixels,
    pub w: u32,
    pub h: u32,
    pub stride: u32,
    pub order: Order,
    pub serial: u64, // a new value for every scanout the guest sets
}

// The guest's pointer. It is not in the scanout: the guest hands QEMU an image and a hot spot, and
// whoever shows the screen draws it. Rows of B, G, R, A, the colours already multiplied by the alpha:
// Lictor rasterises it so (as a DRM cursor plane is), and QEMU passes the bytes on as they are.
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
    // The rows with no colour stronger than its alpha. Such a colour is not one an image of
    // colours multiplied by their alpha can hold, and what is drawn of it is not defined: it is
    // cut to the alpha.
    pub fn cut(&self) -> Vec<u8> {
        let mut out = self.bgra.clone();
        for px in out.chunks_exact_mut(4) {
            let a = px[3];
            for c in &mut px[..3] {
                *c = (*c).min(a);
            }
        }
        out
    }
    // The same as R, G, B, A.
    pub fn rgba(&self) -> Vec<u8> {
        let mut out = self.cut();
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
    pub pointer_at: (i32, i32),        // the pixel the guest has its pointer's hot spot on
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
            pointer_at: (0, 0),
        }
    }
}

pub struct Display {
    pub shared: Arc<Mutex<Shared>>,
    pub dirty: Arc<AtomicBool>,
    pub pointer_dirty: Arc<AtomicBool>,
    pub input: mpsc::Sender<Input>,
    synced: mpsc::Receiver<()>,
}
impl Display {
    // Wait until everything queued so far has been handed to QEMU, for at most `limit`. False if
    // it has not been, or never will be.
    pub fn drain(&self, limit: Duration) -> bool {
        while self.synced.try_recv().is_ok() {}
        self.input.send(Input::Sync).is_ok() && self.synced.recv_timeout(limit).is_ok()
    }
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
    // And with its first two.
    pub fn place_pointer(&self, x: i32, y: i32) {
        place_pointer(&self.shared, x, y);
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

// Where the guest has its pointer. It is no news to a view whose pointer is the Mac's; one that
// draws the guest's pointer itself looks here each frame.
fn place_pointer(shared: &Mutex<Shared>, x: i32, y: i32) {
    shared.lock().unwrap().pointer_at = (x, y);
}

// A length in points as whole pixels at `scale` pixels to a point. The drawable and the size the
// guest is told both come from here: a guest frame of exactly the drawable's size is shown a pixel
// to a pixel, and one a pixel off is not.
pub fn pixels(points: f64, scale: f64) -> u32 {
    (points * scale).round().max(1.0) as u32
}

// What the guest's display is told of the window that shows it: its size in pixels, and the size in
// millimetres of a screen that many points across at the 96 to the inch a guest takes for a scale
// of one. A guest that divides the one by the other finds the Mac's scale.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct UiInfo {
    pub width_mm: u16,
    pub height_mm: u16,
    pub width: u32,
    pub height: u32,
}
impl UiInfo {
    // For a view `w` x `h` points with `scale` pixels to a point. None for a view with no size,
    // or with one that is not a number: there is nothing true to tell, and the floors below
    // would make one pixel and one millimetre of it.
    pub fn of(w: f64, h: f64, scale: f64) -> Option<UiInfo> {
        let real = |v: f64| v.is_finite() && v > 0.0;
        if !real(w) || !real(h) || !real(scale) {
            return None;
        }
        let mm = |points: f64| (points * 25.4 / 96.0).round().clamp(1.0, u16::MAX as f64) as u16;
        Some(UiInfo {
            width_mm: mm(w),
            height_mm: mm(h),
            width: pixels(w, scale),
            height: pixels(h, scale),
        })
    }
}

// A pixel of the guest's screen, and the size the screen had when the pixel was found: a tablet
// reports a fraction of its surface, and which pixel that is depends on the size.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Place {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
}

// A button or a notch goes with the place it happened at, so that the guest is handed both as one
// report. A button let go of off the picture has no place, and nothing of a captured pointer has
// one: that pointer is where the guest's mouse has moved it to.
#[derive(Debug, PartialEq)]
pub enum Input {
    Key {
        qnum: u32,
        down: bool,
    },
    Abs(Place),
    // A captured pointer's motion, in pixels of the guest's screen.
    Rel {
        dx: i32,
        dy: i32,
    },
    Button {
        button: u32,
        down: bool,
        at: Option<Place>,
    },
    Wheel {
        up: bool,
        at: Option<Place>,
    },
    // Not sent: from here on the pointer is captured, or is not.
    Capture(bool),
    Size(UiInfo),
    Sync, // not sent: answered once everything before it has been
}

struct State {
    shared: Arc<Mutex<Shared>>,
    dirty: Arc<AtomicBool>,
    pointer_dirty: Arc<AtomicBool>,
    map: bool,
    echo: Option<mpsc::Sender<(i32, i32, i32)>>,
    unshowable: AtomicU64,
}

fn set_scanout(
    shared: &Mutex<Shared>,
    dirty: &AtomicBool,
    pixels: Pixels,
    (w, h, stride): (u32, u32, u32),
    order: Order,
) {
    let mut s = shared.lock().unwrap();
    s.serials += 1;
    let serial = s.serials;
    s.scanout = Some(Scanout {
        pixels,
        w,
        h,
        stride,
        order,
        serial,
    });
    s.damage = Some(Rect { x: 0, y: 0, w, h });
    dirty.store(true, Ordering::Release);
}

impl State {
    fn set(&self, pixels: Pixels, size: (u32, u32, u32), order: Order) {
        set_scanout(&self.shared, &self.dirty, pixels, size, order);
    }
    // A scanout that cannot be shown is shown as nothing, not as the frame before it: the guest's
    // screen is no longer that frame, and the pointer would be placed by its size.
    fn unshowable(&self, what: &str) {
        let n = self.unshowable.fetch_add(1, Ordering::Relaxed) + 1;
        if n.is_power_of_two() {
            eprintln!("display: {what} cannot be shown ({n} so far); the window is blank until the guest sets one that can");
        }
        let mut s = self.shared.lock().unwrap();
        s.scanout = None;
        s.damage = None;
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
fn geometry(width: u32, height: u32, stride: u32) -> Result<usize, String> {
    if width == 0 || height == 0 || width > MAX_SIDE || height > MAX_SIDE {
        return Err(format!("a scanout of {width}x{height}"));
    }
    if stride < width * 4 || !stride.is_multiple_of(4) || stride > i32::MAX as u32 {
        return Err(format!("a stride of {stride} for {width} pixels"));
    }
    Ok((height as usize - 1) * stride as usize + width as usize * 4)
}

// The byte order and the length of a scanout that can be shown, or what it is that cannot be.
fn showable(
    width: u32,
    height: u32,
    stride: u32,
    pixman_format: u32,
) -> Result<(Order, usize), String> {
    let order = order(pixman_format)
        .ok_or_else(|| format!("a scanout in pixman format {pixman_format:#010x}"))?;
    Ok((order, geometry(width, height, stride)?))
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
        match showable(width, height, stride, pixman_format) {
            Ok((order, len)) if data.len() >= len => {
                self.0
                    .set(Pixels::Owned(data), (width, height, stride), order);
            }
            Ok(_) => self.0.unshowable("a scanout shorter than its size"),
            Err(what) => self.0.unshowable(&what),
        }
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
        {
            let mut s = self.0.shared.lock().unwrap();
            let Some(sc) = &mut s.scanout else {
                return Ok(());
            };
            // Pixels in another order than the scanout's are not pixels of it.
            if order(pixman_format) != Some(sc.order) {
                return Ok(());
            }
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
    fn mouse_set(&mut self, x: i32, y: i32, on: i32) {
        if let Some(echo) = &self.0.echo {
            echo.send((x, y, on)).ok();
        }
        show_pointer(&self.0.shared, &self.0.pointer_dirty, on != 0);
        place_pointer(&self.0.shared, x, y);
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
    // An error here makes QEMU send pixels in the messages (Listener.Scanout/Update) instead, and
    // not for this scanout only: it never offers its memory to this listener again. So the only
    // error is a mapping that could not be made. A scanout that cannot be shown is taken, and
    // shown as nothing.
    fn scanout_map(
        &mut self,
        handle: Fd<'_>,
        offset: u32,
        width: u32,
        height: u32,
        stride: u32,
        pixman_format: u32,
    ) -> zbus::fdo::Result<()> {
        let (order, len) = match showable(width, height, stride, pixman_format) {
            Ok(shown) => shown,
            Err(what) => {
                self.0.unshowable(&what);
                return Ok(());
            }
        };
        let region = Region::map(handle.as_fd(), offset as u64, len).map_err(|e| {
            eprintln!(
                "display: the scanout's memory was not mapped ({e}); QEMU sends pixels instead"
            );
            zbus::fdo::Error::Failed(e.to_string())
        })?;
        let pixels = Pixels::Mapped(Arc::new(region));
        self.0.set(pixels, (width, height, stride), order);
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

// Give the socket what is left until `deadline` for its next read and write. The reader is a clone
// of the same socket, so it has the same limits.
fn limit(w: &UnixStream, deadline: Instant) -> std::io::Result<()> {
    let left = deadline.saturating_duration_since(Instant::now());
    if left.is_zero() {
        return Err(std::io::ErrorKind::TimedOut.into());
    }
    w.set_read_timeout(Some(left))?;
    w.set_write_timeout(Some(left))
}

// Why a monitor command was not carried out.
#[derive(Debug)]
enum QmpError {
    Io(std::io::Error), // the connection: QEMU is gone, or gave no answer in time
    Refused(String),    // QEMU's own answer, in its words
}
impl From<std::io::Error> for QmpError {
    fn from(e: std::io::Error) -> QmpError {
        QmpError::Io(e)
    }
}

// What a line from the monitor says of the command before it: nothing if it is not an answer (an
// event, which QEMU sends whenever it has one), or else done, or refused with QEMU's reason. A
// command sent without an id is answered by an object with one member, `return` or `error`.
fn answer(line: &str) -> Option<Result<(), String>> {
    if line.starts_with("{\"return\"") {
        return Some(Ok(()));
    }
    if !line.starts_with("{\"error\"") {
        return None;
    }
    // The reason is the text of "desc"; a quote inside it is written with a backslash before it.
    let why = line.split_once("\"desc\": \"").map(|(_, rest)| {
        let mut end = rest.len();
        let mut escaped = false;
        for (i, c) in rest.char_indices() {
            match c {
                '"' if !escaped => {
                    end = i;
                    break;
                }
                '\\' => escaped = !escaped,
                _ => escaped = false,
            }
        }
        rest[..end].to_string()
    });
    Some(Err(why.unwrap_or_else(|| line.trim().to_string())))
}

// A monitor connection. QEMU's monitor serves one client at a time and answers its commands in
// the order they came.
#[derive(Debug)]
struct Monitor {
    w: UnixStream,
    r: BufReader<UnixStream>,
}
impl Monitor {
    fn cmd(
        &mut self,
        json: &str,
        fd: Option<i32>,
        deadline: Option<Instant>,
    ) -> Result<(), QmpError> {
        self.ask(json, fd, deadline).map(|_| ())
    }

    // One command and the line that answers it. With no deadline it waits for as long as QEMU
    // takes.
    fn ask(
        &mut self,
        json: &str,
        fd: Option<i32>,
        deadline: Option<Instant>,
    ) -> Result<String, QmpError> {
        let line = format!("{json}\n");
        if let Some(deadline) = deadline {
            limit(&self.w, deadline)?;
        }
        match fd {
            Some(fd) => send_with_fd(&self.w, line.as_bytes(), fd)?,
            None => (&self.w).write_all(line.as_bytes())?,
        }
        loop {
            let mut reply = String::new();
            if let Some(deadline) = deadline {
                limit(&self.w, deadline)?;
            }
            if self.r.read_line(&mut reply)? == 0 {
                return Err(std::io::Error::other("QMP closed").into());
            }
            match answer(&reply) {
                None => continue,
                Some(Ok(())) => return Ok(reply),
                Some(Err(why)) => return Err(QmpError::Refused(why)),
            }
        }
    }
}

// A tablet's axis runs from 0 to this whatever the screen's size (QEMU's INPUT_EVENT_ABS_MAX).
const AXIS_MAX: u64 = 0x7fff;
// The display device the pointer's events are addressed to. A tablet bound to it (`display=gpu0`
// on the tablet, tools/run-vm.sh) takes all of an event; with none bound a place goes to the
// tablet and a button to the pointer device that came up last, whose report QEMU hands over first.
const SCREEN: &str = "gpu0";
// The tablet as tools/run-vm.sh names it. Another launcher's tablet is not looked for: one that
// is not found is taken for one bound to nothing.
const TABLET: &str = "tab-pci0";
// QEMU's names for the buttons, by the numbers the view counts them in (InputButton's order).
const BUTTONS: [&str; 7] = [
    "left",
    "middle",
    "right",
    "wheel-up",
    "wheel-down",
    "side",
    "extra",
];

// The axis value for pixel `p` of `n`. The guest finds the pixel again as value * (n - 1) /
// AXIS_MAX rounded down (usr/tapestryd/src/main.rs, the tablet's drain), so this is the least
// value that division brings back to `p`: exact for every pixel while n - 1 is at most AXIS_MAX.
fn axis(p: u32, n: u32) -> u64 {
    if n < 2 {
        return 0;
    }
    let last = n as u64 - 1;
    ((p as u64).min(last) * AXIS_MAX).div_ceil(last)
}
const _: () = assert!(MAX_SIDE as u64 - 1 <= AXIS_MAX);

// One input-send-event command: a place, a button going down or up, or both. A tablet bound to
// the display is handed all of it as one report: its driver cannot see a press before its place.
fn pointer_event(to_screen: bool, at: Option<Place>, button: Option<(&str, bool)>) -> String {
    let mut events = Vec::new();
    if let Some(p) = at {
        for (name, value) in [("x", axis(p.x, p.w)), ("y", axis(p.y, p.h))] {
            events.push(format!(
                r#"{{"type":"abs","data":{{"axis":"{name}","value":{value}}}}}"#
            ));
        }
    }
    if let Some((name, down)) = button {
        events.push(button_event(name, down));
    }
    send_event(to_screen, &events)
}

fn button_event(name: &str, down: bool) -> String {
    format!(r#"{{"type":"btn","data":{{"down":{down},"button":"{name}"}}}}"#)
}

// The command for `events`, addressed to the screen or to no display.
fn send_event(to_screen: bool, events: &[String]) -> String {
    let to = if to_screen {
        format!(r#""device":"{SCREEN}","head":0,"#)
    } else {
        String::new()
    };
    format!(
        r#"{{"execute":"input-send-event","arguments":{{{to}"events":[{}]}}}}"#,
        events.join(",")
    )
}

// One input-send-event command for the guest's mouse: motion, or a button going down or up. It is
// addressed to no display, because a tablet bound to the display takes a button addressed to it.
// With no address QEMU hands motion to the mouse, the one device that takes any, and a button to
// the pointer device bound to no display whose driver came up last: the mouse again, in a guest
// of ours.
fn mouse_event(by: (i32, i32), button: Option<(&str, bool)>) -> String {
    let mut events = Vec::new();
    for (name, value) in [("x", by.0), ("y", by.1)] {
        // QEMU passes a zero on to the guest as an event.
        if value != 0 {
            events.push(format!(
                r#"{{"type":"rel","data":{{"axis":"{name}","value":{value}}}}}"#
            ));
        }
    }
    if let Some((name, down)) = button {
        events.push(button_event(name, down));
    }
    send_event(false, &events)
}

// The pointer's way to the guest. QEMU's display connection gives a console one pointer mode: it
// takes a position only while the console's pointer device is an absolute one (the device bound
// to the console, or with none bound the one whose driver came up last, which in a guest of ours
// is the mouse), and relative motion only while it is not. The monitor's input-send-event takes
// either at any time: a position goes to the guest's tablet, and a captured pointer's motion to
// its mouse.
//
// QEMU refuses an event while its machine is stopped. A refused place, motion or press is nothing
// to the guest: the next place stands for it, and the release of a press QEMU did not take is not
// sent. A refused release is another matter, because the guest holds the button until it is told.
// It is kept, and it goes before anything that happened after it.
#[derive(Debug)]
struct Pointing {
    monitor: Monitor,
    to_screen: bool,
    // A tablet is bound to the screen, which makes one report of a button and its place.
    bound: bool,
    // The pointer is captured: a button that goes down, and a notch, are the mouse's.
    captured: bool,
    // The buttons the guest holds, each on the device it went down on: QEMU took the press and
    // has not taken the release. The release goes to that device, however the pointer is sent
    // by then.
    down: [Option<Device>; BUTTONS.len()],
    // The releases QEMU refused, oldest first, each with the place it had.
    owed: Vec<(usize, Option<Place>)>,
    // The place QEMU last took, while the guest's pointer is still there.
    last: Option<Place>,
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum Device {
    Tablet,
    Mouse,
}

impl Pointing {
    fn new(monitor: Monitor, to_screen: bool, bound: bool) -> Pointing {
        Pointing {
            monitor,
            to_screen,
            bound,
            captured: false,
            down: [None; BUTTONS.len()],
            owed: Vec::new(),
            last: None,
        }
    }

    fn cmd(&mut self, at: Option<Place>, button: Option<(usize, bool)>) -> Result<(), QmpError> {
        let named = button.map(|(b, down)| (BUTTONS[b], down));
        self.monitor
            .cmd(&pointer_event(self.to_screen, at, named), None, None)?;
        if at.is_some() {
            self.last = at;
        }
        Ok(())
    }

    fn mouse(&mut self, by: (i32, i32), button: Option<(usize, bool)>) -> Result<(), QmpError> {
        let named = button.map(|(b, down)| (BUTTONS[b], down));
        self.monitor.cmd(&mouse_event(by, named), None, None)?;
        if by != (0, 0) {
            // The guest's pointer has left the place it was last sent.
            self.last = None;
        }
        Ok(())
    }

    // A button or a notch with its place. With no tablet bound to the screen QEMU hands the place
    // to the tablet and the button to another device, the button first. So there a place the guest
    // was not sent yet goes ahead in a command of its own: the guest has that command's round trip
    // to take the place in, which makes the button first unlikely and not impossible. The place
    // goes with the button all the same, for a tablet that is bound and was not found to be.
    fn with_place(&mut self, at: Option<Place>, button: (usize, bool)) -> Result<(), QmpError> {
        if !self.bound && at.is_some() && at != self.last {
            self.cmd(at, None)?;
        }
        self.cmd(at, Some(button))
    }

    // A button or a notch on one of the two devices. The mouse has no places.
    fn on(
        &mut self,
        device: Device,
        at: Option<Place>,
        button: (usize, bool),
    ) -> Result<(), QmpError> {
        match device {
            Device::Tablet => self.with_place(at, button),
            Device::Mouse => self.mouse((0, 0), Some(button)),
        }
    }

    fn device(&self) -> Device {
        if self.captured {
            Device::Mouse
        } else {
            Device::Tablet
        }
    }

    // Send the releases QEMU refused. The first one it refuses again ends the try: it refuses for
    // as long as its machine is stopped.
    fn settle(&mut self) -> Result<(), QmpError> {
        while let Some(&(b, at)) = self.owed.first() {
            if let Some(device) = self.down[b] {
                self.on(device, at, (b, false))?;
            }
            self.owed.remove(0);
            self.down[b] = None;
        }
        Ok(())
    }

    fn place(&mut self, at: Place) -> Result<(), QmpError> {
        self.settle()?;
        self.cmd(Some(at), None)
    }

    fn motion(&mut self, dx: i32, dy: i32) -> Result<(), QmpError> {
        self.settle()?;
        if (dx, dy) == (0, 0) {
            return Ok(());
        }
        self.mouse((dx, dy), None)
    }

    // A release of a button the guest holds is owed from here until QEMU takes it. A press of one
    // it holds, and a release of one it does not, are not sent.
    fn button(&mut self, b: usize, down: bool, at: Option<Place>) -> Result<(), QmpError> {
        if !down && self.down[b].is_some() && !self.owed.iter().any(|&(o, _)| o == b) {
            self.owed.push((b, at));
        }
        self.settle()?;
        if down && self.down[b].is_none() {
            let device = self.device();
            self.on(device, at, (b, true))?;
            self.down[b] = Some(device);
        }
        Ok(())
    }

    // A notch is the wheel's button going down. Its coming up is no part of a notch, and a virtio
    // tablet and a virtio mouse both make a key of it that no driver expects.
    fn notch(&mut self, up: bool, at: Option<Place>) -> Result<(), QmpError> {
        self.settle()?;
        let device = self.device();
        self.on(device, at, (if up { 3 } else { 4 }, true))
    }

    // The pointer's events are sent from here. Any other is handed back, for the display
    // connection, once the releases the guest is owed have been tried: they go before what
    // happened after them. It is handed back whether or not QEMU took them this time. A key QEMU
    // does not hand a stopped machine it drops without a word, and one kept back here would be
    // lost to a machine that had just been continued.
    fn take(&mut self, ev: Input) -> Result<Option<Input>, QmpError> {
        match ev {
            Input::Abs(at) => self.place(at)?,
            Input::Rel { dx, dy } => self.motion(dx, dy)?,
            Input::Button { button, down, at } => {
                if button as usize >= BUTTONS.len() {
                    return Err(QmpError::Refused(format!("there is no button {button}")));
                }
                self.button(button as usize, down, at)?
            }
            Input::Wheel { up, at } => self.notch(up, at)?,
            Input::Capture(on) => self.captured = on,
            other => {
                return match self.settle() {
                    Err(QmpError::Io(e)) => Err(QmpError::Io(e)),
                    _ => Ok(Some(other)),
                }
            }
        }
        Ok(None)
    }
}

// Whether the launcher's tablet is bound to the screen: if not, what QEMU answered instead.
fn tablet_bound(monitor: &mut Monitor, deadline: Instant) -> Result<Result<(), String>, QmpError> {
    for (property, bound) in [("display", format!("\"{SCREEN}\"")), ("head", "0".into())] {
        let get = format!(
            r#"{{"execute":"qom-get","arguments":{{"path":"/machine/peripheral/{TABLET}","property":"{property}"}}}}"#
        );
        match monitor.ask(&get, None, Some(deadline)) {
            Ok(line) => {
                let line = line.trim_end();
                let value = line
                    .strip_prefix("{\"return\": ")
                    .and_then(|v| v.strip_suffix('}'))
                    .unwrap_or(line);
                if value != bound {
                    return Ok(Err(format!("its {property} is {value}")));
                }
            }
            Err(QmpError::Refused(why)) => return Ok(Err(why)),
            Err(e) => return Err(e),
        }
    }
    Ok(Ok(()))
}

// Hand QEMU one end of a socket pair over QMP and make it a display client; returns our end, and
// the monitor, which is kept for the pointer.
fn attach(qmp_path: &str, deadline: Instant) -> std::io::Result<(UnixStream, Pointing)> {
    attach_qmp(qmp_path, deadline).map_err(|e| {
        let why = match e {
            QmpError::Io(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                ) =>
            {
                format!(
                    "no answer in {} s: it is not answering, or another client holds this monitor (it serves one at a time)",
                    ATTACH_TIMEOUT.as_secs()
                )
            }
            QmpError::Io(e) => e.to_string(),
            QmpError::Refused(why) => why,
        };
        std::io::Error::other(format!("QEMU's monitor at {qmp_path}: {why}"))
    })
}

fn attach_qmp(qmp_path: &str, deadline: Instant) -> Result<(UnixStream, Pointing), QmpError> {
    let w = UnixStream::connect(qmp_path)?;
    let mut monitor = Monitor {
        r: BufReader::new(w.try_clone()?),
        w,
    };
    let mut greeting = String::new();
    limit(&monitor.w, deadline)?;
    monitor.r.read_line(&mut greeting)?;
    // A refusal here is the end of the attach, and is told with the command it answers.
    let mut ask = |json: &str, fd: Option<i32>| {
        monitor.cmd(json, fd, Some(deadline)).map_err(|e| match e {
            QmpError::Refused(why) => QmpError::Refused(format!("{json} -> {why}")),
            e => e,
        })
    };
    ask(r#"{"execute":"qmp_capabilities"}"#, None)?;
    let (ours, theirs) = UnixStream::pair()?;
    ask(
        r#"{"execute":"getfd","arguments":{"fdname":"thylacine-display"}}"#,
        Some(theirs.as_raw_fd()),
    )?;
    ask(
        r#"{"execute":"add_client","arguments":{"protocol":"@dbus-display","fdname":"thylacine-display"}}"#,
        None,
    )?;
    // Whether the screen is a display of this QEMU: an event of nothing, addressed as the
    // pointer's will be. QEMU looks for the display before it looks at whether its machine is
    // running, so a stopped machine's refusal says the display is there.
    let to_screen = match monitor.cmd(&pointer_event(true, None, None), None, Some(deadline)) {
        Ok(()) => true,
        Err(QmpError::Refused(why)) if why == "VM not running" => true,
        Err(QmpError::Refused(why)) => {
            eprintln!("input: {SCREEN} is not a display of this QEMU ({why}); the pointer's events go to whichever device QEMU picks for each");
            false
        }
        Err(e) => return Err(e),
    };
    let bound = to_screen
        && match tablet_bound(&mut monitor, deadline)? {
            Ok(()) => true,
            Err(why) => {
                eprintln!("input: the tablet {TABLET} is not known to be bound to {SCREEN} ({why}); a button's place is sent ahead of it, and a guest may still see the button first");
                false
            }
        };
    // From here on a command waits for its answer for as long as QEMU takes.
    monitor.w.set_read_timeout(None)?;
    monitor.w.set_write_timeout(None)?;
    Ok((ours, Pointing::new(monitor, to_screen, bound)))
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
// QEMU was busy. A run of a captured pointer's motion is one motion, the sum, because the guest wants
// all of it. An event of another kind ends the run and is handed back for the next turn.
fn next_input(first: Input, mut more: impl FnMut() -> Option<Input>) -> (Input, Option<Input>) {
    let mut ev = first;
    loop {
        if !matches!(ev, Input::Abs(_) | Input::Rel { .. }) {
            return (ev, None);
        }
        ev = match (ev, more()) {
            (Input::Abs(_), Some(next @ Input::Abs(_))) => next,
            (Input::Rel { dx, dy }, Some(Input::Rel { dx: x, dy: y })) => Input::Rel {
                dx: dx.saturating_add(x),
                dy: dy.saturating_add(y),
            },
            (ev, other) => return (ev, other),
        };
    }
}

// Why an event did not reach the guest: QEMU is gone, or it answered that it will not take it.
#[derive(Debug)]
enum Unsent {
    Gone(String),
    Refused(String),
}
impl From<zbus::Error> for Unsent {
    fn from(e: zbus::Error) -> Unsent {
        match e {
            zbus::Error::InputOutput(e) => Unsent::Gone(e.to_string()),
            e => Unsent::Refused(e.to_string()),
        }
    }
}
impl From<QmpError> for Unsent {
    fn from(e: QmpError) -> Unsent {
        match e {
            QmpError::Io(e) => Unsent::Gone(e.to_string()),
            QmpError::Refused(why) => Unsent::Refused(why),
        }
    }
}

// Keys and the window's size go over the display connection; the pointer goes over the monitor.
fn send_input(conn: &zbus::Connection, pointing: &mut Pointing, ev: Input) -> Result<(), Unsent> {
    macro_rules! call {
        ($iface:expr, $method:expr, $body:expr) => {
            zbus::block_on(conn.call_method(
                Some("org.qemu"),
                CONSOLE,
                Some($iface),
                $method,
                $body,
            ))
            .map(|_| ())?
        };
    }
    let Some(ev) = pointing.take(ev)? else {
        return Ok(());
    };
    match ev {
        Input::Key { qnum, down } => call!(
            "org.qemu.Display1.Keyboard",
            if down { "Press" } else { "Release" },
            &(qnum,)
        ),
        // QEMU keeps it for the guest's next question about its display, and raises the device's
        // display event. A console whose device takes no size refuses the call.
        Input::Size(u) => call!(
            "org.qemu.Display1.Console",
            "SetUIInfo",
            &(u.width_mm, u.height_mm, 0i32, 0i32, u.width, u.height)
        ),
        // A Sync has nothing of its own to send, and the pointer's events did not come back.
        _ => {}
    }
    Ok(())
}

// What the input thread does: the events in order, a run of positions as its last one, until the
// window is gone or QEMU is. A Sync is answered once everything before it has been tried.
fn pump(
    irx: mpsc::Receiver<Input>,
    stx: mpsc::Sender<()>,
    mut send: impl FnMut(Input) -> Result<(), Unsent>,
) {
    let mut held_back = None;
    let mut refused = 0u64;
    while let Some(first) = held_back.take().or_else(|| irx.recv().ok()) {
        let (ev, rest) = next_input(first, || irx.try_recv().ok());
        held_back = rest;
        let sync = ev == Input::Sync;
        match send(ev) {
            Ok(()) => {}
            Err(Unsent::Gone(e)) => {
                eprintln!("input: QEMU is gone ({e}); nothing more is sent");
                break;
            }
            // QEMU still answers but will not take the event: another client took its console,
            // the machine is stopped, or the guest has no device for it.
            Err(Unsent::Refused(e)) => {
                refused += 1;
                if refused.is_power_of_two() {
                    eprintln!("input: QEMU refused an event ({e}); {refused} so far");
                }
            }
        }
        if sync {
            stx.send(()).ok();
        }
    }
}

pub fn connect(qmp_path: &str) -> Result<Display, Box<dyn std::error::Error>> {
    let deadline = Instant::now() + ATTACH_TIMEOUT;
    let (sock, mut pointing) = attach(qmp_path, deadline)?;
    let shared: Arc<Mutex<Shared>> = Arc::default();
    let dirty = Arc::new(AtomicBool::new(false));
    let pointer_dirty = Arc::new(AtomicBool::new(false));
    // THYLACINE_NO_MAP=1 makes QEMU send pixels in its messages instead: the path a host without a
    // shareable surface takes, kept reachable so that it can be tried.
    let map = std::env::var_os("THYLACINE_NO_MAP").is_none_or(|v| v != "1");
    // THYLACINE_ECHO_POINTER=1 prints each place the guest says its pointer is at, for a test
    // that wants to see where an event landed. A thread of its own prints them: QEMU waits for
    // the display thread, which must not wait for whoever reads the app's output.
    let echo = match std::env::var_os("THYLACINE_ECHO_POINTER") {
        Some(v) if v == "1" => {
            let (tx, said) = mpsc::channel::<(i32, i32, i32)>();
            std::thread::Builder::new()
                .name("echo".into())
                .spawn(move || {
                    for (x, y, on) in said {
                        eprintln!("pointer: the guest has its pointer at {x},{y}, shown {on}");
                    }
                })?;
            Some(tx)
        }
        _ => None,
    };
    let state = Arc::new(State {
        shared: shared.clone(),
        dirty: dirty.clone(),
        pointer_dirty: pointer_dirty.clone(),
        map,
        echo,
        unshowable: AtomicU64::new(0),
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
    let conn = match rx.recv_timeout(deadline.saturating_duration_since(Instant::now())) {
        Ok(r) => r?,
        Err(_) => return Err("QEMU did not answer on its display connection".into()),
    };
    // Input has a thread of its own so that the window's thread never waits for QEMU.
    let (itx, irx) = mpsc::channel::<Input>();
    let (stx, synced) = mpsc::channel::<()>();
    std::thread::Builder::new()
        .name("input".into())
        .spawn(move || pump(irx, stx, |ev| send_input(&conn, &mut pointing, ev)))?;
    Ok(Display {
        shared,
        dirty,
        pointer_dirty,
        input: itx,
        synced,
    })
}

// A display with no QEMU behind it, and a scanout set as the listener sets one: for tests.
#[cfg(test)]
impl Display {
    pub fn detached() -> (Display, mpsc::Receiver<Input>) {
        let (input, sent) = mpsc::channel();
        let (_, synced) = mpsc::channel();
        let display = Display {
            shared: Arc::default(),
            dirty: Arc::new(AtomicBool::new(false)),
            pointer_dirty: Arc::new(AtomicBool::new(false)),
            input,
            synced,
        };
        (display, sent)
    }
    pub fn set_owned(&self, w: u32, h: u32, order: Order, bytes: Vec<u8>) {
        assert_eq!(bytes.len(), w as usize * h as usize * 4);
        set_scanout(
            &self.shared,
            &self.dirty,
            Pixels::Owned(bytes),
            (w, h, w * 4),
            order,
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::net::UnixListener;

    const X8R8G8B8: u32 = 0x2002_0888;

    fn state(map: bool) -> (Arc<State>, Arc<Mutex<Shared>>) {
        let shared: Arc<Mutex<Shared>> = Arc::default();
        let state = Arc::new(State {
            shared: shared.clone(),
            dirty: Arc::new(AtomicBool::new(false)),
            pointer_dirty: Arc::new(AtomicBool::new(false)),
            map,
            echo: None,
            unshowable: AtomicU64::new(0),
        });
        (state, shared)
    }

    fn showing(shared: &Mutex<Shared>) -> Option<(u32, u32, Order)> {
        let s = shared.lock().unwrap();
        s.scanout.as_ref().map(|sc| (sc.w, sc.h, sc.order))
    }

    #[test]
    fn the_eight_formats_of_a_guest_have_four_orders() {
        // pixman's PIXMAN_FORMAT(32, type, a, 8, 8, 8), and its numbers for the four types.
        let code = |ty: u32, a: u32| 32 << 24 | ty << 16 | a << 12 | 0x888;
        let (argb, abgr, bgra, rgba) = (2, 3, 8, 9);
        assert_eq!(code(argb, 0), X8R8G8B8);
        for a in [0, 8] {
            assert_eq!(order(code(argb, a)), Some(Order::Bgrx));
            assert_eq!(order(code(abgr, a)), Some(Order::Rgbx));
            assert_eq!(order(code(bgra, a)), Some(Order::Xrgb));
            assert_eq!(order(code(rgba, a)), Some(Order::Xbgr));
        }
        // r5g6b5, r8g8b8, a2r10g10b10, nothing.
        for other in [0x1002_0565, 0x1802_0888, 0x2002_2aaa, 0] {
            assert_eq!(order(other), None, "{other:#010x}");
        }
    }

    #[test]
    fn a_mapped_scanout_that_cannot_be_shown_is_taken_and_shows_nothing() {
        let (state, shared) = state(true);
        let mut map = MapListener(state.clone());
        let path = std::env::temp_dir().join(format!("thylacine-mac-take-{}", std::process::id()));
        let f = std::fs::File::create_new(&path).unwrap();
        std::fs::remove_file(&path).unwrap();
        f.set_len(8192).unwrap();
        let fd = || Fd::from(f.as_fd());
        assert!(map.scanout_map(fd(), 0, 16, 16, 64, X8R8G8B8).is_ok());
        assert_eq!(showing(&shared), Some((16, 16, Order::Bgrx)));
        // A format with no byte a channel. Answered as taken, so that QEMU goes on sharing its
        // memory; and the frame before it is not left standing.
        assert!(map.scanout_map(fd(), 0, 16, 16, 64, 0x1002_0565).is_ok());
        assert_eq!(showing(&shared), None);
        assert!(state.dirty.swap(false, Ordering::AcqRel));
        // The colours in another order are a scanout like any other.
        assert!(map.scanout_map(fd(), 0, 16, 16, 64, 0x2003_8888).is_ok());
        assert_eq!(showing(&shared), Some((16, 16, Order::Rgbx)));
        // A side past what a texture can have.
        let wide = MAX_SIDE + 1;
        assert!(map
            .scanout_map(fd(), 0, wide, 1, wide * 4, X8R8G8B8)
            .is_ok());
        assert_eq!(showing(&shared), None);
        // Memory that is not there is the one refusal. QEMU then sends that scanout in a message,
        // and until it does the one before stands.
        assert!(map.scanout_map(fd(), 0, 16, 16, 64, X8R8G8B8).is_ok());
        assert!(map.scanout_map(fd(), 0, 64, 64, 256, X8R8G8B8).is_err());
        assert_eq!(showing(&shared), Some((16, 16, Order::Bgrx)));
    }

    #[test]
    fn pixels_in_a_message_are_held_to_the_same() {
        let (state, shared) = state(false);
        let mut l = Listener(state);
        assert!(l.scanout(2, 2, 8, X8R8G8B8, vec![7; 16]).is_ok());
        assert_eq!(showing(&shared), Some((2, 2, Order::Bgrx)));
        // An update in the scanout's order is copied in; one in another order is not its pixels.
        assert!(l.update(0, 0, 1, 1, 4, X8R8G8B8, vec![1, 2, 3, 4]).is_ok());
        assert!(l.update(1, 1, 1, 1, 4, 0x2003_0888, vec![9; 4]).is_ok());
        {
            let s = shared.lock().unwrap();
            let Pixels::Owned(buf) = &s.scanout.as_ref().unwrap().pixels else {
                panic!("a scanout sent in a message is owned");
            };
            assert_eq!(buf[..4], [1, 2, 3, 4]);
            assert_eq!(buf[12..], [7; 4]);
        }
        // Fewer bytes than its size, or a size that is none: nothing is shown.
        assert!(l.scanout(2, 2, 8, X8R8G8B8, vec![7; 15]).is_ok());
        assert_eq!(showing(&shared), None);
        assert!(l.scanout(2, 2, 8, X8R8G8B8, vec![7; 16]).is_ok());
        assert_eq!(showing(&shared), Some((2, 2, Order::Bgrx)));
        assert!(l.scanout(0, 2, 8, X8R8G8B8, Vec::new()).is_ok());
        assert_eq!(showing(&shared), None);
    }

    // Lines as QEMU 10.0.2 and 11.1.3 wrote them to a monitor (the two agree), carriage return
    // and all.
    const DONE: &str = "{\"return\": {}}\r\n";
    const STOPPED: &str =
        "{\"timestamp\": {\"seconds\": 1794471987, \"microseconds\": 21641}, \"event\": \"STOP\"}\r\n";
    const NOT_RUNNING: &str =
        "{\"error\": {\"class\": \"GenericError\", \"desc\": \"VM not running\"}}\r\n";
    const RESUMED: &str =
        "{\"timestamp\": {\"seconds\": 1794479739, \"microseconds\": 687903}, \"event\": \"RESUME\"}\r\n";
    const NO_SCREEN: &str =
        "{\"error\": {\"class\": \"DeviceNotFound\", \"desc\": \"Device 'gpu0' not found\"}}\r\n";
    const NOT_A_SCREEN: &str = "{\"error\": {\"class\": \"GenericError\", \"desc\": \"Device gpu0 (head 0) is not bound to a QemuConsole\"}}\r\n";
    const NO_TABLET: &str = "{\"error\": {\"class\": \"DeviceNotFound\", \"desc\": \"Device '/machine/peripheral/tab-pci0' not found\"}}\r\n";
    const ON_SCREEN: &str = "{\"return\": \"gpu0\"}\r\n";
    const ON_NOTHING: &str = "{\"return\": \"\"}\r\n";
    const HEAD_0: &str = "{\"return\": 0}\r\n";
    // Not seen from QEMU: made after HEAD_0.
    const HEAD_1: &str = "{\"return\": 1}\r\n";

    // What a QEMU answers the attach's questions: an event of nothing addressed to the screen,
    // and what its tablet's display and head are.
    #[derive(Clone, Copy)]
    struct Machine {
        probe: &'static str,
        display: &'static str,
        head: &'static str,
    }
    // The launcher's: the screen is a display, and the tablet is bound to it.
    const OURS: Machine = Machine {
        probe: DONE,
        display: ON_SCREEN,
        head: HEAD_0,
    };

    // A monitor that greets at once and answers every line after `delay`, as that machine's
    // would. It hands back the commands it was sent.
    fn monitor(
        name: &str,
        delay: Duration,
        machine: Machine,
    ) -> (String, std::thread::JoinHandle<Vec<String>>) {
        let path = std::env::temp_dir().join(format!("tm-{name}-{}", std::process::id()));
        let _ = std::fs::remove_file(&path);
        let listener = UnixListener::bind(&path).unwrap();
        let served = std::thread::spawn(move || {
            let (s, _) = listener.accept().unwrap();
            let mut r = BufReader::new(s.try_clone().unwrap());
            (&s).write_all(b"{\"QMP\": {}}\r\n").unwrap();
            let mut seen = Vec::new();
            let mut line = String::new();
            while r.read_line(&mut line).is_ok_and(|n| n > 0) {
                let answer = if line.contains("input-send-event") {
                    machine.probe
                } else if line.contains("\"property\":\"display\"") {
                    machine.display
                } else if line.contains("\"property\":\"head\"") {
                    machine.head
                } else {
                    DONE
                };
                seen.push(line.trim_end().to_string());
                line.clear();
                std::thread::sleep(delay);
                if (&s).write_all(answer.as_bytes()).is_err() {
                    break;
                }
            }
            seen
        });
        (path.to_str().unwrap().to_string(), served)
    }

    #[test]
    fn the_whole_attach_has_one_deadline() {
        let step = Duration::from_millis(150);
        // Six answers, each `step` late, fit in five seconds.
        let (path, served) = monitor("fits", step, OURS);
        let begun = Instant::now();
        let (_display, pointing) = attach_qmp(&path, begun + Duration::from_secs(5)).unwrap();
        assert!(begun.elapsed() >= 6 * step);
        assert!(pointing.to_screen && pointing.bound);
        // The monitor is kept, and after the attach it is waited on for as long as it takes.
        assert_eq!(pointing.monitor.w.read_timeout().unwrap(), None);
        assert_eq!(pointing.monitor.w.write_timeout().unwrap(), None);
        drop(pointing);
        let seen = served.join().unwrap();
        std::fs::remove_file(&path).unwrap();
        let asked: Vec<&str> = seen
            .iter()
            .map(|c| c.split('"').nth(3).unwrap_or(""))
            .collect();
        assert_eq!(
            asked,
            [
                "qmp_capabilities",
                "getfd",
                "add_client",
                "input-send-event",
                "qom-get",
                "qom-get"
            ]
        );
        // The question about the screen is an event of nothing, addressed as the pointer's are.
        assert_eq!(
            seen[3],
            r#"{"execute":"input-send-event","arguments":{"device":"gpu0","head":0,"events":[]}}"#
        );
        assert_eq!(
            seen[4],
            r#"{"execute":"qom-get","arguments":{"path":"/machine/peripheral/tab-pci0","property":"display"}}"#
        );
        // They do not fit in two steps, though each of them alone does.
        let (path, served) = monitor("late", step, OURS);
        match attach_qmp(&path, Instant::now() + 2 * step).unwrap_err() {
            QmpError::Io(e) => assert!(
                matches!(
                    e.kind(),
                    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                ),
                "{e}"
            ),
            e => panic!("{e:?}"),
        }
        served.join().unwrap();
        std::fs::remove_file(&path).unwrap();
    }

    #[test]
    fn the_attach_finds_out_what_the_pointers_events_will_reach() {
        let m = |probe, display, head| Machine {
            probe,
            display,
            head,
        };
        // (the machine, whether the screen is a display of it, whether the tablet is bound to
        // that, how many questions it took)
        for (name, machine, to_screen, bound, asked) in [
            ("ours", OURS, true, true, 6),
            // A stopped machine refuses the event after it has found the display.
            ("stopped", m(NOT_RUNNING, ON_SCREEN, HEAD_0), true, true, 6),
            ("unbound", m(DONE, ON_NOTHING, HEAD_0), true, false, 5),
            ("head", m(DONE, ON_SCREEN, HEAD_1), true, false, 6),
            ("anothers", m(DONE, NO_TABLET, NO_TABLET), true, false, 5),
            // With no screen to be bound to, the tablet is not asked about.
            ("bare", m(NO_SCREEN, NO_TABLET, NO_TABLET), false, false, 4),
            ("rng", m(NOT_A_SCREEN, ON_NOTHING, HEAD_0), false, false, 4),
        ] {
            let (path, served) = monitor(name, Duration::ZERO, machine);
            let deadline = Instant::now() + Duration::from_secs(5);
            let (_display, pointing) = attach_qmp(&path, deadline).unwrap();
            assert_eq!(
                (pointing.to_screen, pointing.bound),
                (to_screen, bound),
                "{name}"
            );
            drop(pointing);
            assert_eq!(served.join().unwrap().len(), asked, "{name}");
            std::fs::remove_file(&path).unwrap();
        }
    }

    #[test]
    fn a_drain_waits_for_what_was_queued() {
        let (input, irx) = mpsc::channel();
        let (stx, synced) = mpsc::channel();
        let display = Display {
            shared: Arc::default(),
            dirty: Arc::new(AtomicBool::new(false)),
            pointer_dirty: Arc::new(AtomicBool::new(false)),
            input,
            synced,
        };
        let sent = Arc::new(Mutex::new(Vec::new()));
        let record = sent.clone();
        let gone = Arc::new(AtomicBool::new(false));
        let is_gone = gone.clone();
        let syncs = Arc::new(AtomicU64::new(0));
        let synced_with = syncs.clone();
        let pumping = std::thread::spawn(move || {
            pump(irx, stx, |ev| {
                if is_gone.load(Ordering::Acquire) {
                    return Err(Unsent::Gone("gone".into()));
                }
                // The sender is handed a Sync too: it has releases of its own to try by then.
                if ev == Input::Sync {
                    synced_with.fetch_add(1, Ordering::AcqRel);
                    return Ok(());
                }
                // QEMU taking its time.
                std::thread::sleep(Duration::from_millis(40));
                // An event QEMU will not take is counted and the next one is tried.
                if matches!(ev, Input::Abs(_)) {
                    return Err(Unsent::Refused("VM not running".into()));
                }
                record.lock().unwrap().push(ev);
                Ok(())
            })
        });
        let at = Place {
            x: 1,
            y: 2,
            w: 640,
            h: 480,
        };
        let up = Input::Button {
            button: 0,
            down: false,
            at: None,
        };
        display.input.send(Input::Abs(at)).unwrap();
        display
            .input
            .send(Input::Key {
                qnum: 30,
                down: false,
            })
            .unwrap();
        display.input.send(up).unwrap();
        assert!(display.drain(Duration::from_secs(5)));
        assert_eq!(sent.lock().unwrap().len(), 2);
        assert_eq!(syncs.load(Ordering::Acquire), 1);
        // With QEMU gone there is nothing to wait for, and the wait says so at once.
        gone.store(true, Ordering::Release);
        display
            .input
            .send(Input::Wheel {
                up: true,
                at: Some(at),
            })
            .unwrap();
        let begun = Instant::now();
        assert!(!display.drain(Duration::from_secs(5)));
        assert!(begun.elapsed() < Duration::from_secs(4));
        pumping.join().unwrap();
        assert_eq!(sent.lock().unwrap().len(), 2);
    }

    #[test]
    fn the_guest_is_told_pixels_and_the_size_of_as_many_points() {
        // 1280 x 800 points is 338.67 x 211.67 mm at 96 to the inch, whatever the pixels are.
        let retina = UiInfo::of(1280.0, 800.0, 2.0).unwrap();
        assert_eq!(
            (
                retina.width,
                retina.height,
                retina.width_mm,
                retina.height_mm
            ),
            (2560, 1600, 339, 212)
        );
        let plain = UiInfo::of(1280.0, 800.0, 1.0).unwrap();
        assert_eq!(
            (plain.width, plain.height, plain.width_mm, plain.height_mm),
            (1280, 800, 339, 212)
        );
        // A half pixel goes up, as the drawable's does.
        assert_eq!(UiInfo::of(100.25, 50.0, 2.0).unwrap().width, 201);
        assert_eq!(pixels(100.25, 2.0), 201);
        assert_eq!(pixels(0.1, 1.0), 1);
        // No size, and a size that is not a number, are not told. The arithmetic alone would
        // tell one pixel for nothing.
        assert_eq!(pixels(0.0, 2.0), 1);
        for (w, h, scale) in [
            (0.0, 800.0, 2.0),
            (1280.0, 0.0, 2.0),
            (1280.0, 800.0, 0.0),
            (-1.0, 800.0, 2.0),
            (f64::NAN, 800.0, 2.0),
            (1280.0, f64::INFINITY, 2.0),
        ] {
            assert_eq!(UiInfo::of(w, h, scale), None, "{w}x{h} at {scale}");
        }
        // A sliver still has a millimetre, and a width past any screen still fits its field.
        let odd = UiInfo::of(1.0, 1e9, 1.0).unwrap();
        assert_eq!((odd.width_mm, odd.height_mm), (1, u16::MAX));
    }

    // The base block of the EDID QEMU makes for a size it was told, as far as a size is read
    // from it (hw/display/edid-generate.c, the same in 10.1 and 11.1): whole centimetres
    // always, and the millimetres in a first detailed timing unless the mode is too large for
    // one at the 75 Hz QEMU takes when it is told no rate. Whether it had a detailed timing.
    fn qemu_edid(u: UiInfo) -> ([u8; 128], bool) {
        let (x, y) = (u.width as u64, u.height as u64);
        let clock = 75_000 * (x + x * 35 / 100) * (y + y * 35 / 1000) / 10_000_000;
        let large = x >= 4096 || y >= 4096 || clock >= 65536;
        let mut e = [0u8; 128];
        e[..8].copy_from_slice(&[0, 255, 255, 255, 255, 255, 255, 0]);
        e[21] = (u.width_mm / 10) as u8;
        e[22] = (u.height_mm / 10) as u8;
        if !large {
            e[54] = clock as u8;
            e[55] = (clock >> 8) as u8;
            e[66] = u.width_mm as u8;
            e[67] = u.height_mm as u8;
            e[68] = ((u.width_mm & 0xf00) >> 4) as u8 | ((u.height_mm & 0xf00) >> 8) as u8;
        }
        e[127] = 0u8.wrapping_sub(e[..127].iter().fold(0u8, |a, &b| a.wrapping_add(b)));
        (e, !large)
    }

    // The scale a guest takes for a view: its own reading of that block and its own rule.
    fn guest_scale_for(w: f64, h: f64, scale: f64) -> (Option<u16>, bool) {
        use crate::guest_scale::{parse_edid_mm, scale_pct};
        let u = UiInfo::of(w, h, scale).unwrap();
        let (edid, timing) = qemu_edid(u);
        let pct = parse_edid_mm(&edid).map(|(mw, mh)| scale_pct(u.width, u.height, mw, mh));
        (pct, timing)
    }

    #[test]
    fn a_guest_takes_the_macs_scale_from_what_it_is_told() {
        // The window as it opens, at its least, full screen on a 13-inch Air below the camera,
        // and on the larger screens, where QEMU's block has centimetres only.
        for (w, h, timing_at_1, timing_at_2) in [
            (1280.0, 800.0, true, true),
            (640.0, 400.0, true, true),
            (1470.0, 924.0, true, true),
            (1512.0, 982.0, true, true),
            (1728.0, 1117.0, true, false),
            (2560.0, 1440.0, true, false),
            (3008.0, 1692.0, true, false),
            (3840.0, 2160.0, false, false),
            (4200.0, 700.0, false, false),
        ] {
            assert_eq!(
                guest_scale_for(w, h, 1.0),
                (Some(100), timing_at_1),
                "{w}x{h} at 1"
            );
            assert_eq!(
                guest_scale_for(w, h, 2.0),
                (Some(200), timing_at_2),
                "{w}x{h} at 2"
            );
        }
        // And the sizes between: every pair of sides from 22 points, where a millimetre more or
        // less stops moving the answer, to the two metres past which a guest takes an EDID's size
        // for garbage. Half points among the small ones. A side under a centimetre (38 points)
        // is tried only against one of no more than 640: see below.
        let mut sides: Vec<f64> = vec![];
        let mut side = 22.0;
        while side <= 7500.0 {
            sides.push(side);
            side += match side {
                s if s < 100.0 => 0.5,
                s if s < 640.0 => 1.5,
                _ => 13.5,
            };
        }
        let (mut tried, mut no_size) = (0u32, 0u32);
        for scale in [1.0, 2.0] {
            let want = Some((100.0 * scale) as u16);
            for &w in &sides {
                for &h in &sides {
                    if w.min(h) < 38.0 && w.max(h) > 640.0 {
                        continue;
                    }
                    tried += 1;
                    match guest_scale_for(w, h, scale).0 {
                        got if got == want => {}
                        // A size told as 16 by 9 or 16 by 10, in millimetres, in centimetres or
                        // in tens of them, is one a guest reads as an aspect ratio written where
                        // the size goes: no size, and its scale is 100 then. The window is none
                        // of these views: it is at least 640 by 400 points, and the others are
                        // 160 cm wide, which no display is in points.
                        None => {
                            assert!(
                                (w < 640.0 && h < 400.0) || (w > 6000.0 && h > 3400.0),
                                "{w}x{h} at {scale}: a view the window can be is told as no size"
                            );
                            no_size += 1;
                        }
                        got => panic!("{w}x{h} at {scale}: {got:?}, want {want:?}"),
                    }
                }
            }
        }
        assert!(tried > 2_000_000, "{tried}");
        assert_eq!(no_size, 266, "the views told as an aspect ratio");
        // Under 22 points a side is a few millimetres, and rounding them is too much of it: 21
        // points are 5.56 mm, told as 6, and 42 pixels over 6 mm are not twice 96 to the inch.
        assert_eq!(guest_scale_for(21.0, 800.0, 2.0).0, Some(175));
        // A strip: under a centimetre one way and too long the other for a detailed timing.
        // QEMU's block then has whole centimetres only, none of them, and a guest reads no size
        // from it and keeps the scale it has. So does one past two metres. Neither is a window:
        // the least is 640x400 points.
        assert_eq!(guest_scale_for(22.0, 4096.0, 1.0), (None, false));
        assert_eq!(guest_scale_for(7600.0, 800.0, 1.0).0, None);
    }

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
    fn a_pointer_exchanges_red_and_blue_and_holds_no_colour_past_its_alpha() {
        let bgra = vec![1, 2, 3, 4, 250, 251, 252, 0, 40, 100, 200, 128];
        let p = Pointer::new(3, 1, 0, 0, bgra).unwrap();
        assert_eq!(p.rgba(), vec![3, 2, 1, 4, 0, 0, 0, 0, 128, 100, 40, 128]);
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
        let abs = |x| {
            Input::Abs(Place {
                x,
                y: 7,
                w: 640,
                h: 480,
            })
        };
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
    fn of_a_run_of_motion_the_sum_is_sent() {
        let rel = |dx, dy| Input::Rel { dx, dy };
        let press = || Input::Button {
            button: 0,
            down: true,
            at: None,
        };
        let mut q: std::collections::VecDeque<Input> = [
            rel(2, -1),
            rel(-5, 4),
            press(),
            rel(1, 1),
            abs_at(3),
            rel(7, 7),
        ]
        .into();
        // Three motions, then a press: all of the motion goes out as one, and the press is handed back.
        assert_eq!(
            next_input(rel(10, 10), || q.pop_front()),
            (rel(7, 13), Some(press()))
        );
        // Motion and a position are two kinds: neither run takes the other in.
        assert_eq!(
            next_input(q.pop_front().unwrap(), || q.pop_front()),
            (rel(1, 1), Some(abs_at(3)))
        );
        assert_eq!(
            next_input(abs_at(3), || q.pop_front()),
            (abs_at(3), Some(rel(7, 7)))
        );
        assert!(q.is_empty());
        // More motion than a number holds stays at the most it holds.
        let mut q: std::collections::VecDeque<Input> = [rel(i32::MAX, i32::MIN), rel(-1, 1)].into();
        assert_eq!(
            next_input(rel(1, -1), || q.pop_front()),
            (rel(i32::MAX - 1, i32::MIN + 1), None)
        );
    }

    fn abs_at(x: u32) -> Input {
        Input::Abs(Place {
            x,
            y: 7,
            w: 640,
            h: 480,
        })
    }

    // The pixel the guest makes of an axis value on a screen `n` across, with the tablet's 32767
    // for `mx`: the compositor's own arithmetic, which the test below finds in its source.
    fn guest_pixel_of(value: u64, n: u32) -> u64 {
        value.min(32767) * n.saturating_sub(1) as u64 / 32767
    }

    // `axis` is exact only against the arithmetic above. If the compositor comes to make a pixel of
    // an axis value another way, this fails, and `axis` and `guest_pixel_of` follow it.
    #[test]
    fn the_compositor_still_makes_a_pixel_of_an_axis_value_this_way() {
        let compositor = include_str!("../../../usr/tapestryd/src/main.rs");
        for line in [
            "(ax.min(mx) as u64 * dw.saturating_sub(1) as u64 / mx.max(1) as u64) as u32;",
            "(ay.min(my) as u64 * dh.saturating_sub(1) as u64 / my.max(1) as u64) as u32;",
        ] {
            assert_eq!(compositor.matches(line).count(), 1, "{line}");
        }
    }

    #[test]
    fn every_pixel_of_every_screen_comes_back_from_its_axis_value() {
        for n in 1..=MAX_SIDE {
            let mut before = 0;
            for p in 0..n {
                let v = axis(p, n);
                assert!(v <= AXIS_MAX && v >= before, "{p} of {n}: {v}");
                assert_eq!(guest_pixel_of(v, n), p as u64, "{p} of {n}: {v}");
                // The least such value: one less is the pixel before.
                if p > 0 {
                    assert_eq!(guest_pixel_of(v - 1, n), p as u64 - 1, "{p} of {n}: {v}");
                }
                before = v;
            }
        }
        // What QEMU's display connection made of a pixel, x * 0x7fff / n, lands a pixel short
        // nearly everywhere: this is why the value is not left to it.
        assert_eq!(guest_pixel_of(10 * 0x7fff / 1280, 1280), 9);
        assert_eq!(guest_pixel_of(1279 * 0x7fff / 1280, 1280), 1277);
        assert_eq!((axis(10, 1280), axis(1279, 1280)), (257, 32767));
        // A pixel past the screen, which a size that changed since could make of it, is the last.
        assert_eq!(axis(700, 640), 32767);
        assert_eq!((axis(0, 1), axis(5, 1), axis(5, 0)), (0, 0, 0));
    }

    #[test]
    fn a_place_and_its_button_are_one_command() {
        let at = Place {
            x: 10,
            y: 479,
            w: 1280,
            h: 480,
        };
        // The command that was tried by hand against QEMU 10.0.2 and 11.1.3 with a tablet bound
        // to gpu0: the tablet reported `abs 0 257; abs 1 32767; key 0x111 1; syn` and the mouse
        // nothing.
        assert_eq!(
            pointer_event(true, Some(at), Some(("right", true))),
            r#"{"execute":"input-send-event","arguments":{"device":"gpu0","head":0,"events":[{"type":"abs","data":{"axis":"x","value":257}},{"type":"abs","data":{"axis":"y","value":32767}},{"type":"btn","data":{"down":true,"button":"right"}}]}}"#
        );
        assert_eq!(
            pointer_event(false, Some(at), None),
            r#"{"execute":"input-send-event","arguments":{"events":[{"type":"abs","data":{"axis":"x","value":257}},{"type":"abs","data":{"axis":"y","value":32767}}]}}"#
        );
        // A button let go of off the picture: no place.
        assert_eq!(
            pointer_event(true, None, Some(("left", false))),
            r#"{"execute":"input-send-event","arguments":{"device":"gpu0","head":0,"events":[{"type":"btn","data":{"down":false,"button":"left"}}]}}"#
        );
        // The view's numbers are InputButton's: 0 left, 1 middle, 2 right, 5 and 6 the side ones.
        assert_eq!(
            [0, 1, 2, 5, 6].map(|b| BUTTONS[b]),
            ["left", "middle", "right", "side", "extra"]
        );
    }

    #[test]
    fn an_answer_is_told_from_an_event_and_a_refusal_from_both() {
        assert_eq!(answer(DONE), Some(Ok(())));
        assert_eq!(answer("{\"return\": \"virtio-gpu-pci\"}\r\n"), Some(Ok(())));
        assert_eq!(answer(STOPPED), None);
        assert_eq!(answer(NOT_RUNNING), Some(Err("VM not running".into())));
        assert_eq!(
            answer("{\"error\": {\"class\": \"DeviceNotFound\", \"desc\": \"Device '/machine/peripheral/gpu1' not found\"}}\r\n"),
            Some(Err("Device '/machine/peripheral/gpu1' not found".into()))
        );
        // An answer that only mentions the other words is still what its first member says.
        assert_eq!(
            answer("{\"return\": \"unknown command: 'error' \\\"event\\\"\\r\\n\"}\r\n"),
            Some(Ok(()))
        );
        // Not seen from QEMU, made from JSON's rules: a quote inside the reason, and a reason
        // that ends in a backslash.
        assert_eq!(
            answer(
                "{\"error\": {\"class\": \"GenericError\", \"desc\": \"no \\\"x\\\" here\"}}\r\n"
            ),
            Some(Err("no \\\"x\\\" here".into()))
        );
        assert_eq!(
            answer("{\"error\": {\"class\": \"GenericError\", \"desc\": \"ends \\\\\"}}\r\n"),
            Some(Err("ends \\\\".into()))
        );
        // An error with no reason to be found is given whole.
        assert_eq!(
            answer("{\"error\": {}}\r\n"),
            Some(Err("{\"error\": {}}".into()))
        );
    }

    // A monitor whose other end is played by `script`: for each command read, the lines to answer
    // with; when the script is over the connection closes.
    fn scripted(script: Vec<String>) -> (Monitor, std::thread::JoinHandle<Vec<String>>) {
        let (ours, theirs) = UnixStream::pair().unwrap();
        let played = std::thread::spawn(move || {
            // A test that sends fewer commands than its script answers fails here: it is not
            // left waiting for one.
            theirs
                .set_read_timeout(Some(Duration::from_secs(10)))
                .unwrap();
            let mut r = BufReader::new(theirs.try_clone().unwrap());
            let mut seen = Vec::new();
            for lines in script {
                let mut cmd = String::new();
                if r.read_line(&mut cmd).unwrap() == 0 {
                    break;
                }
                seen.push(cmd.trim_end().to_string());
                (&theirs).write_all(lines.as_bytes()).unwrap();
            }
            seen
        });
        let monitor = Monitor {
            r: BufReader::new(ours.try_clone().unwrap()),
            w: ours,
        };
        (monitor, played)
    }

    #[test]
    fn the_pointer_is_refused_or_qemu_is_gone_and_the_two_are_told_apart() {
        let (monitor, played) = scripted(vec![
            format!("{STOPPED}{DONE}"),
            NOT_RUNNING.into(),
            DONE.into(),
        ]);
        let mut pointing = Pointing::new(monitor, true, true);
        let at = Place {
            x: 0,
            y: 0,
            w: 640,
            h: 480,
        };
        // An event that came first is passed over, and the answer behind it is the command's.
        assert!(pointing.place(at).is_ok());
        // A refusal is QEMU's reason, and the connection is as good as before.
        match pointing.button(0, true, Some(at)) {
            Err(QmpError::Refused(why)) => assert_eq!(why, "VM not running"),
            other => panic!("{other:?}"),
        }
        assert!(pointing.notch(true, Some(at)).is_ok());
        // QEMU gone: the script is over and its end is closed.
        let seen = played.join().unwrap();
        match pointing.place(at).map_err(Unsent::from) {
            Err(Unsent::Gone(_)) => {}
            other => panic!("{other:?}"),
        }
        assert_eq!(seen.len(), 3);
        assert_eq!(seen[0], pointer_event(true, Some(at), None));
        assert_eq!(
            seen[2],
            pointer_event(true, Some(at), Some(("wheel-up", true)))
        );
        assert_eq!((BUTTONS[3], BUTTONS[4]), ("wheel-up", "wheel-down"));
    }

    fn at(x: u32) -> Place {
        Place {
            x,
            y: 7,
            w: 640,
            h: 480,
        }
    }

    fn refused(r: Result<(), QmpError>) {
        match r {
            Err(QmpError::Refused(why)) => assert_eq!(why, "VM not running"),
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn a_release_qemu_refused_is_owed_and_goes_before_what_happened_after_it() {
        let (monitor, played) = scripted(vec![
            DONE.into(),                       // left goes down
            format!("{STOPPED}{NOT_RUNNING}"), // and comes up while the machine is stopped
            NOT_RUNNING.into(),                // the release again, for a press
            NOT_RUNNING.into(),                // and for a second release
            NOT_RUNNING.into(),                // and for a key
            format!("{RESUMED}{DONE}"),        // and for a move, once the machine runs
            DONE.into(),                       // that move
            DONE.into(),                       // right goes down
        ]);
        let mut pointing = Pointing::new(monitor, true, true);
        assert!(pointing.button(0, true, Some(at(1))).is_ok());
        refused(pointing.button(0, false, Some(at(2))));
        // Nothing of the pointer's goes ahead of the release: not a press of the same button,
        refused(pointing.button(0, true, Some(at(3))));
        // and its release, which the guest is owed no second time, is the first one tried again.
        refused(pointing.button(0, false, None));
        // A key is handed back to be sent whether or not QEMU took the release.
        let key = Input::Key {
            qnum: 30,
            down: false,
        };
        assert_eq!(
            pointing
                .take(Input::Key {
                    qnum: 30,
                    down: false
                })
                .unwrap(),
            Some(key)
        );
        assert!(pointing.take(Input::Abs(at(4))).unwrap().is_none());
        // The guest was told left is up, and a release that comes now is no news to it.
        assert!(pointing.button(0, false, Some(at(4))).is_ok());
        assert!(pointing.button(2, true, Some(at(4))).is_ok());
        let up = pointer_event(true, Some(at(2)), Some(("left", false)));
        assert_eq!(
            played.join().unwrap(),
            [
                pointer_event(true, Some(at(1)), Some(("left", true))),
                up.clone(),
                up.clone(),
                up.clone(),
                up.clone(),
                up,
                pointer_event(true, Some(at(4)), None),
                pointer_event(true, Some(at(4)), Some(("right", true))),
            ]
        );
    }

    #[test]
    fn a_release_that_is_owed_is_tried_when_the_app_waits_for_its_input_to_be_sent() {
        let (monitor, played) = scripted(vec![
            DONE.into(),
            NOT_RUNNING.into(),
            NOT_RUNNING.into(),
            DONE.into(),
        ]);
        let mut pointing = Pointing::new(monitor, true, true);
        assert!(pointing.button(1, true, Some(at(1))).is_ok());
        refused(pointing.button(1, false, None));
        // QEMU refuses it again, and the wait is answered all the same.
        assert_eq!(pointing.take(Input::Sync).unwrap(), Some(Input::Sync));
        assert_eq!(pointing.take(Input::Sync).unwrap(), Some(Input::Sync));
        // With nothing owed a Sync sends nothing: the script is over, and this does not find out.
        assert_eq!(pointing.take(Input::Sync).unwrap(), Some(Input::Sync));
        let up = pointer_event(true, None, Some(("middle", false)));
        assert_eq!(
            played.join().unwrap(),
            [
                pointer_event(true, Some(at(1)), Some(("middle", true))),
                up.clone(),
                up.clone(),
                up
            ]
        );
    }

    #[test]
    fn qemu_gone_is_found_out_by_a_release_that_is_owed() {
        let (monitor, played) = scripted(vec![DONE.into(), NOT_RUNNING.into()]);
        let mut pointing = Pointing::new(monitor, true, true);
        assert!(pointing.button(0, true, None).is_ok());
        refused(pointing.button(0, false, None));
        // The script is over and its end is closed. A refusal does not keep a key back; QEMU
        // gone is told whatever the event is.
        played.join().unwrap();
        let key = Input::Key {
            qnum: 30,
            down: true,
        };
        match pointing.take(key) {
            Err(QmpError::Io(_)) => {}
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn the_release_of_a_press_qemu_refused_is_not_sent() {
        let (monitor, played) = scripted(vec![NOT_RUNNING.into(), DONE.into()]);
        let mut pointing = Pointing::new(monitor, true, true);
        refused(pointing.button(2, true, Some(at(1))));
        assert!(pointing.button(2, false, Some(at(1))).is_ok());
        assert!(pointing.place(at(2)).is_ok());
        assert_eq!(
            played.join().unwrap(),
            [
                pointer_event(true, Some(at(1)), Some(("right", true))),
                pointer_event(true, Some(at(2)), None),
            ]
        );
    }

    #[test]
    fn with_no_tablet_bound_a_place_the_guest_was_not_sent_goes_ahead_of_its_button() {
        let mut script = vec![DONE.to_string(); 8];
        script[7] = NOT_RUNNING.into();
        let (monitor, played) = scripted(script);
        let mut pointing = Pointing::new(monitor, true, false);
        assert!(pointing.button(2, true, Some(at(1))).is_ok());
        // The guest has that place by now, and the release is one command.
        assert!(pointing.button(2, false, Some(at(1))).is_ok());
        assert!(pointing.place(at(2)).is_ok());
        assert!(pointing.notch(true, Some(at(2))).is_ok());
        assert!(pointing.notch(false, Some(at(3))).is_ok());
        // A place QEMU refuses takes its button with it,
        refused(pointing.button(0, true, Some(at(4))));
        // and the guest was not told of that press.
        assert!(pointing.button(0, false, Some(at(4))).is_ok());
        assert_eq!(
            played.join().unwrap(),
            [
                pointer_event(true, Some(at(1)), None),
                pointer_event(true, Some(at(1)), Some(("right", true))),
                pointer_event(true, Some(at(1)), Some(("right", false))),
                pointer_event(true, Some(at(2)), None),
                pointer_event(true, Some(at(2)), Some(("wheel-up", true))),
                pointer_event(true, Some(at(3)), None),
                pointer_event(true, Some(at(3)), Some(("wheel-down", true))),
                pointer_event(true, Some(at(4)), None),
            ]
        );
    }

    #[test]
    fn motion_and_a_button_for_the_mouse_are_addressed_to_no_display() {
        // The commands that were tried by hand against QEMU 10.0.2 and 11.1.3, the tablet bound to
        // gpu0 and bound to nothing, a driver for each device (aux-gate/macrunner/scale/rel/
        // probe.out). The mouse reported `rel 0x0 5; rel 0x1 -3; syn` for the first of these and
        // `key 0x110 1; syn` for the second; the tablet nothing.
        assert_eq!(
            mouse_event((5, -3), None),
            r#"{"execute":"input-send-event","arguments":{"events":[{"type":"rel","data":{"axis":"x","value":5}},{"type":"rel","data":{"axis":"y","value":-3}}]}}"#
        );
        assert_eq!(
            mouse_event((0, 0), Some(("left", true))),
            r#"{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":true,"button":"left"}}]}}"#
        );
        // QEMU passes a zero on as an event: an axis that did not move is left out.
        assert_eq!(
            mouse_event((0, 9), None),
            r#"{"execute":"input-send-event","arguments":{"events":[{"type":"rel","data":{"axis":"y","value":9}}]}}"#
        );
        // A notch for the mouse: it reported `rel 0x8 1; syn`.
        assert_eq!(
            mouse_event((0, 0), Some(("wheel-up", true))),
            r#"{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":true,"button":"wheel-up"}}]}}"#
        );
    }

    #[test]
    fn a_captured_pointer_is_the_mouses_and_a_button_comes_up_where_it_went_down() {
        let (monitor, played) = scripted(vec![DONE.to_string(); 9]);
        let mut pointing = Pointing::new(monitor, true, true);
        let sent = |p: &mut Pointing, ev: Input| assert_eq!(p.take(ev).unwrap(), None);
        let button = |button, down, at| Input::Button { button, down, at };
        // Left goes down on the tablet, and the pointer is captured while it is held.
        sent(&mut pointing, button(0, true, Some(at(1))));
        sent(&mut pointing, Input::Capture(true));
        sent(&mut pointing, Input::Rel { dx: 5, dy: -3 });
        // No motion is nothing to send.
        sent(&mut pointing, Input::Rel { dx: 0, dy: 0 });
        sent(&mut pointing, button(2, true, None));
        sent(
            &mut pointing,
            Input::Wheel {
                up: false,
                at: None,
            },
        );
        // Left comes up on the tablet, where the guest holds it,
        sent(&mut pointing, button(0, false, None));
        sent(&mut pointing, Input::Capture(false));
        // and right on the mouse, though the pointer is the tablet's again and the release has a place.
        sent(&mut pointing, Input::Abs(at(2)));
        sent(&mut pointing, button(2, false, Some(at(2))));
        sent(&mut pointing, button(1, true, Some(at(2))));
        sent(
            &mut pointing,
            Input::Wheel {
                up: true,
                at: Some(at(2)),
            },
        );
        assert_eq!(
            played.join().unwrap(),
            [
                pointer_event(true, Some(at(1)), Some(("left", true))),
                mouse_event((5, -3), None),
                mouse_event((0, 0), Some(("right", true))),
                mouse_event((0, 0), Some(("wheel-down", true))),
                pointer_event(true, None, Some(("left", false))),
                pointer_event(true, Some(at(2)), None),
                mouse_event((0, 0), Some(("right", false))),
                pointer_event(true, Some(at(2)), Some(("middle", true))),
                pointer_event(true, Some(at(2)), Some(("wheel-up", true))),
            ]
        );
    }

    #[test]
    fn a_release_the_mouse_is_owed_goes_to_the_mouse_before_the_tablets_next_event() {
        let (monitor, played) = scripted(vec![
            DONE.into(),                // right goes down, captured
            NOT_RUNNING.into(),         // and comes up while the machine is stopped
            NOT_RUNNING.into(),         // motion finds the release still refused
            format!("{RESUMED}{DONE}"), // a position, uncaptured: the release first
            DONE.into(),                // that position
        ]);
        let mut pointing = Pointing::new(monitor, true, true);
        assert_eq!(pointing.take(Input::Capture(true)).unwrap(), None);
        assert!(pointing.button(2, true, None).is_ok());
        refused(pointing.button(2, false, None));
        refused(pointing.motion(4, 4));
        assert_eq!(pointing.take(Input::Capture(false)).unwrap(), None);
        assert!(pointing.place(at(6)).is_ok());
        let up = mouse_event((0, 0), Some(("right", false)));
        assert_eq!(
            played.join().unwrap(),
            [
                mouse_event((0, 0), Some(("right", true))),
                up.clone(),
                up.clone(),
                up,
                pointer_event(true, Some(at(6)), None),
            ]
        );
    }

    #[test]
    fn a_notch_waits_behind_a_release_that_is_owed() {
        let (monitor, played) = scripted(vec![
            DONE.into(),                // left goes down
            NOT_RUNNING.into(),         // and comes up while the machine is stopped
            NOT_RUNNING.into(),         // a notch finds the release still refused, and is not sent
            format!("{RESUMED}{DONE}"), // the next notch: the release first
            DONE.into(),                // then that notch
        ]);
        let mut pointing = Pointing::new(monitor, true, true);
        assert!(pointing.button(0, true, Some(at(1))).is_ok());
        refused(pointing.button(0, false, Some(at(1))));
        refused(pointing.notch(true, Some(at(2))));
        assert!(pointing.notch(false, Some(at(3))).is_ok());
        let up = pointer_event(true, Some(at(1)), Some(("left", false)));
        assert_eq!(
            played.join().unwrap(),
            [
                pointer_event(true, Some(at(1)), Some(("left", true))),
                up.clone(),
                up.clone(),
                up,
                pointer_event(true, Some(at(3)), Some(("wheel-down", true))),
            ]
        );
    }

    #[test]
    fn two_releases_that_are_owed_are_both_sent_the_older_first() {
        let (monitor, played) = scripted(vec![
            DONE.into(),                // left goes down
            DONE.into(),                // right goes down
            NOT_RUNNING.into(),         // left comes up while the machine is stopped
            NOT_RUNNING.into(),         // right too: the older release is tried first
            format!("{RESUMED}{DONE}"), // a place: left's release first,
            DONE.into(),                // then right's,
            DONE.into(),                // then the place
        ]);
        let mut pointing = Pointing::new(monitor, true, true);
        assert!(pointing.button(0, true, Some(at(1))).is_ok());
        assert!(pointing.button(2, true, Some(at(1))).is_ok());
        refused(pointing.button(0, false, Some(at(2))));
        refused(pointing.button(2, false, Some(at(3))));
        assert!(pointing.place(at(4)).is_ok());
        let left = pointer_event(true, Some(at(2)), Some(("left", false)));
        assert_eq!(
            played.join().unwrap(),
            [
                pointer_event(true, Some(at(1)), Some(("left", true))),
                pointer_event(true, Some(at(1)), Some(("right", true))),
                left.clone(),
                left.clone(),
                left,
                pointer_event(true, Some(at(3)), Some(("right", false))),
                pointer_event(true, Some(at(4)), None),
            ]
        );
    }

    #[test]
    fn a_run_of_motion_ends_at_a_capture_and_what_follows_is_the_tablets() {
        let (monitor, played) = scripted(vec![DONE.to_string(); 2]);
        let mut pointing = Pointing::new(monitor, true, true);
        assert_eq!(pointing.take(Input::Capture(true)).unwrap(), None);
        let mut q: std::collections::VecDeque<Input> = [
            Input::Rel { dx: 2, dy: 3 },
            Input::Rel { dx: 1, dy: 1 },
            Input::Capture(false),
            Input::Button {
                button: 0,
                down: true,
                at: Some(at(1)),
            },
        ]
        .into();
        // As the pump takes them: what ends a run is kept for the next turn.
        let mut kept = None;
        while let Some(first) = kept.take().or_else(|| q.pop_front()) {
            let (ev, rest) = next_input(first, || q.pop_front());
            kept = rest;
            assert_eq!(pointing.take(ev).unwrap(), None);
        }
        // The motion as one, on the mouse; then the press on the tablet, with its place: the
        // pointer was given back between them.
        assert_eq!(
            played.join().unwrap(),
            [
                mouse_event((3, 4), None),
                pointer_event(true, Some(at(1)), Some(("left", true))),
            ]
        );
    }

    #[test]
    fn after_motion_a_place_the_guest_was_sent_before_goes_ahead_of_its_button_again() {
        let (monitor, played) = scripted(vec![DONE.to_string(); 5]);
        // No tablet bound to the screen: a place the guest does not have goes ahead of its button.
        let mut pointing = Pointing::new(monitor, true, false);
        assert!(pointing.place(at(1)).is_ok());
        // The guest has that place,
        assert!(pointing.notch(true, Some(at(1))).is_ok());
        // until its mouse moves the pointer off it.
        assert!(pointing.motion(3, 0).is_ok());
        assert!(pointing.notch(true, Some(at(1))).is_ok());
        assert_eq!(
            played.join().unwrap(),
            [
                pointer_event(true, Some(at(1)), None),
                pointer_event(true, Some(at(1)), Some(("wheel-up", true))),
                mouse_event((3, 0), None),
                pointer_event(true, Some(at(1)), None),
                pointer_event(true, Some(at(1)), Some(("wheel-up", true))),
            ]
        );
    }

    #[test]
    fn where_the_guest_has_its_pointer_is_kept() {
        let (state, shared) = state(true);
        assert_eq!(shared.lock().unwrap().pointer_at, (0, 0));
        Listener(state).mouse_set(600, 108, 1);
        assert_eq!(shared.lock().unwrap().pointer_at, (600, 108));
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
