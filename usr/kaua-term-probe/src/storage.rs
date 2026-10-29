//! Native storage generation checks through the real compositor protocol.
//! Owns two surfaces in the console's temporary graphical session; no peer
//! surface, process or kernel allocation limit is modified.
use alloc::format;
use libthyla_rs::fs::File;
use libthyla_rs::handle::Rights;
use libthyla_rs::io::Read;
use libthyla_rs::{t_open, t_pread, t_weft_map, t_write, T_OREAD, T_OWRITE};
use tapestry::{Event, EventRing, Rect, Surface, TapError, TEV_CONFIGURE, TEV_STORAGE};
type Result<T = ()> = core::result::Result<T, &'static str>;
fn check(ok: bool, why: &'static str) -> Result {
    if ok {
        Ok(())
    } else {
        Err(why)
    }
}
fn open(r: &EventRing, path: &str, mode: u32) -> Result<File> {
    let fd = unsafe { t_open(r.root(), path.as_ptr(), path.len(), mode) };
    check(fd >= 0, "open")?;
    Ok(unsafe { File::from_raw_fd(fd as i32, Rights::READ | Rights::WRITE) })
}
fn leaf(r: &EventRing, s: &Surface) -> Result<u32> {
    let mut text = alloc::string::String::new();
    open(r, "layout", T_OREAD as u32)?
        .read_to_string(&mut text)
        .map_err(|_| "layout")?;
    for l in text.lines() {
        let mut w = l.split_whitespace();
        let Some(id) = w.next() else { continue };
        if w.next() == Some("leaf") && w.next() == Some(format!("surface={}", s.id).as_str()) {
            return id.trim_end_matches('*').parse().map_err(|_| "leaf id");
        }
    }
    Err("leaf missing")
}
fn pane(r: &EventRing, id: u32, cmd: &str) -> Result {
    let f = open(r, &format!("pane/{id}/ctl"), T_OWRITE as u32)?;
    check(
        unsafe { t_write(f.as_raw_fd() as i64, cmd.as_ptr(), cmd.len()) } == cmd.len() as i64,
        "pane ctl",
    )
}
fn paint(s: &mut Surface) -> Result {
    if s.is_drawable() {
        s.pixels().fill(0xff183042);
        s.present(None).map_err(|_| "full paint")?;
    }
    Ok(())
}
fn configure(s: &mut Surface, e: &Event) -> Result {
    match s.handle_configure(e) {
        Ok(_) => paint(s),
        Err(TapError::Busy) => Ok(()),
        Err(_) => Err("configure"),
    }
}
// Read until the requested storage event, processing resize offers but leaving
// storage commands under explicit test control. Deadline uses guest monotonic
// time; no sleep is mistaken for acknowledgement of a transition.
fn offer(r: &EventRing, s: &mut Surface, kind: u16) -> Result<Event> {
    let end = libthyla_rs::time::monotonic_ns() + 5_000_000_000;
    loop {
        r.poll().map_err(|_| "ring poll")?;
        while let Some(e) = s.poll_event().map_err(|_| "event")? {
            if e.kind == TEV_STORAGE && e.code == kind {
                return Ok(e);
            }
            if e.kind == TEV_CONFIGURE {
                configure(s, &e)?;
            }
        }
        check(
            libthyla_rs::time::monotonic_ns() < end,
            "storage offer deadline",
        )?;
        let _ = libthyla_rs::time::sleep(core::time::Duration::from_millis(1));
    }
}
fn token(e: &Event) -> u64 {
    (e.rune as u64) << 32 | e.value as u64
}
fn raw_present(f: &File) -> i64 {
    let mut b = [0u8; 32];
    b[..4].copy_from_slice(&1u32.to_le_bytes());
    unsafe { t_write(f.as_raw_fd() as i64, b.as_ptr(), b.len()) }
}
fn geometry(f: &File) -> i64 {
    let mut b = [0u8; 128];
    unsafe { t_pread(f.as_raw_fd() as i64, b.as_mut_ptr(), b.len(), 0) }
}
fn run_inner() -> Result {
    let r = EventRing::connect_sqpoll().map_err(|_| "connect")?;
    r.global_ctl("session on").map_err(|_| "declare")?;
    let mut a = Surface::open_storage_claim_on(&r, 640, 400, 0).map_err(|_| "surface A")?;
    r.global_ctl("session on").map_err(|_| "redeclare")?;
    let al = leaf(&r, &a)?;
    pane(&r, al, "focus")?;
    paint(&mut a)?;
    pane(&r, al, "split h")?;
    let mut b = Surface::open_on(&r, 640, 400).map_err(|_| "legacy B")?;
    let bl = leaf(&r, &b)?;
    paint(&mut b)?;
    pane(&r, bl, "zoom")?;
    let stale = offer(&r, &mut a, 1)?;
    pane(&r, al, "focus")?;
    check(
        matches!(a.handle_storage(&stale), Err(TapError::Busy)),
        "stale suspend accepted",
    )?;
    check(a.is_drawable(), "stale suspend lost pixels")?;
    // Drain the unzoom resize before opening the retained handles. Repeating
    // hide/show later also covers CONFIGURE interleaving with storage events.
    pane(&r, bl, "focus")?;
    pane(&r, bl, "zoom")?;
    let hide = offer(&r, &mut a, 1)?;
    let oldw = open(&r, &format!("surface/{}/weave", a.id), T_OREAD as u32)?;
    let oldp = open(&r, &format!("surface/{}/present", a.id), T_OWRITE as u32)?;
    check(geometry(&oldw) > 0, "old geometry before suspend")?;
    a.handle_storage(&hide).map_err(|_| "suspend")?;
    check(!a.is_drawable(), "suspend retained client mapping")?;
    check(geometry(&oldw) < 0, "dormant old geometry accepted")?;
    check(raw_present(&oldp) == -9, "dormant old present not EBADF")?;
    pane(&r, al, "focus")?;
    let show = offer(&r, &mut a, 2)?;
    a.handle_storage(&show).map_err(|_| "resume")?;
    check(
        a.is_drawable() && !a.is_ready(),
        "resume published before paint",
    )?;
    check(
        geometry(&oldw) < 0 && raw_present(&oldp) == -9,
        "old generation revived",
    )?;
    check(
        a.present(Some(Rect {
            x: 0,
            y: 0,
            w: 1,
            h: 1,
        }))
        .is_err(),
        "partial first paint accepted",
    )?;
    check(a.present_hold(None).is_err(), "held first paint accepted")?;
    paint(&mut a)?;
    check(a.is_ready(), "full repaint not ready")?;
    check(
        a.surface_ctl(&format!("storage abort {}", token(&show)))
            .is_err(),
        "abort retired published image",
    )?;
    drop(oldw);
    drop(oldp);
    libthyla_rs::println!("STORAGE stale offers, fresh fids and first full paint PASS");
    // Simulate the client's post-allocation map/setup failure using the exact
    // public resume/abort sequence; this is not an injected kernel ENOMEM.
    pane(&r, bl, "focus")?;
    pane(&r, bl, "zoom")?;
    let hide = offer(&r, &mut a, 1)?;
    a.handle_storage(&hide).map_err(|_| "second suspend")?;
    pane(&r, al, "focus")?;
    let show = offer(&r, &mut a, 2)?;
    let t = token(&show);
    a.surface_ctl(&format!("storage resume {t}"))
        .map_err(|_| "raw resume")?;
    let abandoned = open(&r, &format!("surface/{}/weave", a.id), T_OREAD as u32)?;
    check(geometry(&abandoned) > 0, "allocated resume geometry")?;
    a.surface_ctl(&format!("storage abort {t}"))
        .map_err(|_| "raw abort")?;
    check(geometry(&abandoned) < 0, "aborted generation accepted")?;
    check(
        a.surface_ctl(&format!("storage resume {t}")).is_err(),
        "aborted token replay",
    )?;
    drop(abandoned);
    let end = libthyla_rs::time::monotonic_ns() + 100_000_000;
    while libthyla_rs::time::monotonic_ns() < end {
        r.poll().map_err(|_| "abort poll")?;
        while let Some(e) = a.poll_event().map_err(|_| "abort event")? {
            check(e.kind != TEV_STORAGE, "failed reveal retries on idle")?;
            if e.kind == TEV_CONFIGURE {
                configure(&mut a, &e)?;
            }
        }
        let _ = libthyla_rs::time::sleep(core::time::Duration::from_millis(1));
    }
    pane(&r, bl, "focus")?;
    pane(&r, bl, "zoom")?;
    pane(&r, al, "focus")?;
    let fresh = offer(&r, &mut a, 2)?;
    check(token(&fresh) != t, "retry reused token")?;
    a.handle_storage(&fresh).map_err(|_| "retry resume")?;
    paint(&mut a)?;
    check(a.is_ready() && leaf(&r, &a)? == al, "retry lost identity")?;
    libthyla_rs::println!("STORAGE abort, idle suppression and later reveal retry PASS");
    // Legacy surface remains usable after every opt-in generation retirement.
    pane(&r, bl, "focus")?;
    paint(&mut b)?;
    libthyla_rs::println!("STORAGE legacy sibling PASS");
    drop(a);
    drop(b);
    raw_generations(&r)?;
    Ok(())
}
pub fn run() -> i64 {
    match run_inner() {
        Ok(()) => {
            libthyla_rs::println!("storage-probe: PASS");
            0
        }
        Err(why) => {
            libthyla_rs::println!("storage-probe: FAIL -- {}", why);
            1
        }
    }
}

// The weave share is deliberately consume-once. Open a raw owned surface to
// retain ITS single mapped fid; opening a second weave fid of a libtapestry
// Surface cannot claim the already-consumed share a second time.
struct RawSurface {
    id: u32,
    ctl: File,
}
impl Drop for RawSurface {
    fn drop(&mut self) {
        let _ = write(&self.ctl, "destroy");
    }
}
fn write(f: &File, s: &str) -> Result {
    check(
        unsafe { t_write(f.as_raw_fd() as i64, s.as_ptr(), s.len()) } == s.len() as i64,
        "raw ctl",
    )
}
fn raw_offer(f: &File, kind: u16) -> Result<u64> {
    for _ in 0..128 {
        let mut b = [0u8; 24];
        let n = unsafe { libthyla_rs::t_read(f.as_raw_fd() as i64, b.as_mut_ptr(), b.len()) };
        check(n == 24, "raw event size")?;
        if u16::from_le_bytes([b[0], b[1]]) == TEV_STORAGE
            && u16::from_le_bytes([b[2], b[3]]) == kind
        {
            let lo = u32::from_le_bytes(b[4..8].try_into().unwrap());
            let hi = u32::from_le_bytes(b[8..12].try_into().unwrap());
            return Ok((hi as u64) << 32 | lo as u64);
        }
    }
    Err("raw offer flood")
}
fn raw_generations(r: &EventRing) -> Result {
    let mut ctl = open(r, "surface/new", libthyla_rs::T_ORDWR as u32)?;
    let mut id = alloc::string::String::new();
    ctl.read_to_string(&mut id).map_err(|_| "raw id")?;
    let raw = RawSurface {
        id: id.trim().parse().map_err(|_| "raw id parse")?,
        ctl,
    };
    write(&raw.ctl, "create 640 400")?;
    write(&raw.ctl, "storage 1")?;
    let mut text = alloc::string::String::new();
    open(r, "layout", T_OREAD as u32)?
        .read_to_string(&mut text)
        .map_err(|_| "raw layout")?;
    let al = text
        .lines()
        .find_map(|l| {
            let mut w = l.split_whitespace();
            let id = w.next()?;
            if w.next()? == "leaf" && w.next()? == format!("surface={}", raw.id) {
                id.trim_end_matches('*').parse::<u32>().ok()
            } else {
                None
            }
        })
        .ok_or("raw leaf")?;
    let events = open(r, &format!("surface/{}/event", raw.id), T_OREAD as u32)?;
    let old = open(r, &format!("surface/{}/weave", raw.id), T_OREAD as u32)?;
    let oldva = unsafe { t_weft_map(old.as_raw_fd() as u64, 0) };
    check(oldva > 0, "raw initial map")?;
    unsafe { (oldva as *mut u32).write_volatile(0xff123456) };
    pane(r, al, "split h")?;
    let mut b = Surface::open_on(r, 640, 400).map_err(|_| "raw sibling")?;
    let bl = leaf(r, &b)?;
    paint(&mut b)?;
    pane(r, bl, "zoom")?;
    let hide = raw_offer(&events, 1)?;
    write(&raw.ctl, &format!("storage suspend {hide}"))?;
    check(geometry(&old) < 0, "raw dormant geometry")?;
    check(
        unsafe { (oldva as *const u32).read_volatile() } == 0xff123456,
        "old pinned map freed",
    )?;
    pane(r, al, "focus")?;
    let show = raw_offer(&events, 2)?;
    write(&raw.ctl, &format!("storage resume {show}"))?;
    let current = open(r, &format!("surface/{}/weave", raw.id), T_OREAD as u32)?;
    let va = unsafe { t_weft_map(current.as_raw_fd() as u64, 0) };
    check(va > 0, "raw fresh map")?;
    unsafe {
        (va as *mut u32).write_volatile(0xffabcdef);
        (oldva as *mut u32).write_volatile(0xff654321)
    };
    check(
        unsafe { (va as *const u32).read_volatile() } == 0xffabcdef,
        "old map aliases fresh",
    )?;
    let p = open(r, &format!("surface/{}/present", raw.id), T_OWRITE as u32)?;
    check(raw_present(&p) == 32, "raw full present")?;
    drop(old);
    drop(p);
    libthyla_rs::println!("STORAGE retained old mapping remains pinned and cannot alias PASS");
    // Keep the test process's old mappings charged until its unchanged shared
    // mapping ceiling refuses a new generation. This uses ordinary owned fids,
    // not a test syscall, altered quota or privileged fault-injection switch.
    let mut retained = alloc::vec![current];
    let mut refused = false;
    for _ in 0..64 {
        pane(r, bl, "focus")?;
        pane(r, bl, "zoom")?;
        let hide = raw_offer(&events, 1)?;
        write(&raw.ctl, &format!("storage suspend {hide}"))?;
        pane(r, al, "focus")?;
        let show = raw_offer(&events, 2)?;
        write(&raw.ctl, &format!("storage resume {show}"))?;
        let next = open(r, &format!("surface/{}/weave", raw.id), T_OREAD as u32)?;
        let nextva = unsafe { t_weft_map(next.as_raw_fd() as u64, 0) };
        if nextva <= 0 {
            drop(next);
            write(&raw.ctl, &format!("storage abort {show}"))?;
            refused = true;
            break;
        }
        retained.push(next);
        let p = open(r, &format!("surface/{}/present", raw.id), T_OWRITE as u32)?;
        check(raw_present(&p) == 32, "pressure full present")?;
    }
    check(refused, "did not reach unchanged mapping ceiling")?;
    libthyla_rs::println!(
        "STORAGE real map refusal after {} retained generations",
        retained.len()
    );
    drop(retained);
    pane(r, bl, "focus")?;
    pane(r, bl, "zoom")?;
    pane(r, al, "focus")?;
    let show = raw_offer(&events, 2)?;
    write(&raw.ctl, &format!("storage resume {show}"))?;
    let fresh = open(r, &format!("surface/{}/weave", raw.id), T_OREAD as u32)?;
    check(
        unsafe { t_weft_map(fresh.as_raw_fd() as u64, 0) } > 0,
        "map retry after release",
    )?;
    let p = open(r, &format!("surface/{}/present", raw.id), T_OWRITE as u32)?;
    check(raw_present(&p) == 32, "pressure recovery paint")?;
    libthyla_rs::println!("STORAGE actual mapping refusal, abort and recovery PASS");
    Ok(())
}
