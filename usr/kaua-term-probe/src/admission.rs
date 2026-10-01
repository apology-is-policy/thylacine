//! Live compositor/PTY admission test. A sealed child temporarily owns the
//! console's graphical seat; it restores that seat by dropping its surfaces.
use alloc::format;
use libhalcyon::interaction_control::{Op, Request};
use libthyla_rs::fs::{File, OpenOptions};
use libthyla_rs::handle::Rights;
use libthyla_rs::io::Read;
use libthyla_rs::process::Command;
use libthyla_rs::pty_observer::BindingId;
use libthyla_rs::{
    t_getpid, t_open, t_setsid, t_tty_acquire, t_tty_set_fg, t_write, T_OREAD, T_OWRITE,
    T_SPAWN_PERM_SEAL,
};
use tapestry::{EventRing, Surface};
type Result<T = ()> = core::result::Result<T, &'static str>;
fn require(ok: bool, why: &'static str) -> Result {
    if ok {
        Ok(())
    } else {
        Err(why)
    }
}
struct Revoke(BindingId);
impl Drop for Revoke {
    fn drop(&mut self) {
        let _ = self.0.unbind();
    }
}
fn relative(root: i64, path: &str, mode: u32) -> Result<File> {
    let fd = unsafe { t_open(root, path.as_ptr(), path.len(), mode) };
    if fd < 0 {
        return Err("relative open");
    }
    Ok(unsafe { File::from_raw_fd(fd as i32, Rights::READ | Rights::WRITE) })
}
fn leaf(ring: &EventRing, surf: &Surface) -> Result<u32> {
    let mut file = relative(ring.root(), "layout", T_OREAD as u32)?;
    let mut text = alloc::string::String::new();
    file.read_to_string(&mut text).map_err(|_| "layout read")?;
    for line in text.lines() {
        let mut words = line.split_whitespace();
        let Some(id) = words.next() else { continue };
        if words.next() != Some("leaf") {
            continue;
        }
        if words.next() == Some(format!("surface={}", surf.id).as_str()) {
            return id.trim_end_matches('*').parse().map_err(|_| "leaf id");
        }
    }
    Err("surface has no leaf")
}
fn focus(ring: &EventRing, leaf: u32) -> Result {
    let fd = relative(ring.root(), &format!("pane/{}/ctl", leaf), T_OWRITE as u32)?;
    require(
        unsafe { t_write(fd.as_raw_fd() as i64, b"focus".as_ptr(), 5) } == 5,
        "focus verb",
    )
}
fn native() -> Result {
    // This probe is also its synthetic terminal's foreground process. Queue
    // carrier loss so closing our own master cannot terminate the test before
    // it reports the admission result (real Kaua has a separate application).
    let notes = libthyla_rs::notes::Notes::open_self().map_err(|_| "notes")?;
    let ring = EventRing::connect().map_err(|_| "connect")?;
    ring.global_ctl("session on").map_err(|_| "declare")?;
    let surf = Surface::fullscreen_on(&ring).map_err(|_| "surface")?;
    ring.global_ctl("session on").map_err(|_| "redeclare")?;
    let owner_leaf = leaf(&ring, &surf)?;
    focus(&ring, owner_leaf)?;
    let master = ptyhold::Master::mint().map_err(|_| "master")?;
    let _master = unsafe { File::from_raw_fd(master.mfd as i32, Rights::READ | Rights::WRITE) };
    let slave = OpenOptions::new()
        .read(true)
        .write(true)
        .open(format!("/dev/pts/{}", master.n))
        .map_err(|_| "slave")?;
    let fd = slave.as_raw_fd() as i64;
    require(unsafe { t_setsid() } > 0, "setsid")?;
    require(unsafe { t_tty_acquire(fd) } >= 0, "acquire")?;
    require(
        unsafe { t_tty_set_fg(fd, t_getpid() as u64) } >= 0,
        "foreground",
    )?;
    let peer = File::open("/srv/tapestry").map_err(|_| "observer peer")?;
    let binding =
        BindingId::bind(master.mfd as i32, peer.as_raw_fd()).map_err(|_| "bind kernel")?;
    let _revoke = Revoke(binding);
    drop(peer);
    let state = binding.state().map_err(|_| "binder state")?;
    let r = Request {
        op: Op::Bind,
        request: 1,
        leaf: owner_leaf,
        binder_pid: unsafe { t_getpid() } as u32,
        binding: binding.locator(),
        foreground: 0,
        subject: 0,
        controller: 0,
        context: 0,
        epoch: 0,
    };
    require(
        surf.interaction_control(Request {
            binder_pid: r.binder_pid + 1,
            ..r
        })
        .is_err(),
        "wrong host accepted",
    )?;
    surf.interaction_control(r)
        .map_err(|_| "register real host")?;
    require(
        surf.interaction_control(r).is_err(),
        "duplicate host accepted",
    )?;
    let p = Request {
        op: Op::Publish,
        request: 2,
        binder_pid: 0,
        foreground: state.foreground_epoch,
        subject: state.binder_stripes,
        controller: 1,
        context: 1,
        epoch: 1,
        ..r
    };
    surf.interaction_control(p)
        .map_err(|_| "publish real foreground")?;
    let q = Request {
        op: Op::Check,
        request: 3,
        ..p
    };
    let admitted = surf.interaction_control(q).map_err(|_| "focused check")?;
    require(admitted.foreground == p.foreground, "foreground receipt")?;
    require(
        surf.interaction_control(Request { context: 2, ..q })
            .is_err(),
        "wrong context accepted",
    )?;
    let other = Surface::open_on(&ring, 320, 200).map_err(|_| "other surface")?;
    let other_leaf = leaf(&ring, &other)?;
    focus(&ring, other_leaf)?;
    require(surf.interaction_control(q).is_err(), "background accepted")?;
    focus(&ring, owner_leaf)?;
    surf.interaction_control(q)
        .map_err(|_| "restored focus check")?;
    let p2 = Request {
        context: 2,
        epoch: 2,
        request: 4,
        ..p
    };
    surf.interaction_control(p2)
        .map_err(|_| "new context publish")?;
    require(surf.interaction_control(q).is_err(), "old context replay")?;
    surf.interaction_control(Request {
        op: Op::Check,
        request: 5,
        ..p2
    })
    .map_err(|_| "new context check")?;
    require(
        surf.interaction_control(Request {
            subject: u64::MAX,
            controller: 2,
            request: 6,
            ..p2
        })
        .is_err(),
        "nonexistent subject accepted",
    )?;
    require(
        surf.interaction_control(Request {
            op: Op::Check,
            request: 7,
            ..p2
        })
        .is_err(),
        "failed nomination retained old context",
    )?;
    let p3 = Request {
        controller: 3,
        request: 8,
        ..p2
    };
    surf.interaction_control(p3).map_err(|_| "renominate")?;
    surf.interaction_control(Request {
        op: Op::Unbind,
        request: 9,
        binder_pid: 0,
        ..r
    })
    .map_err(|_| "unbind")?;
    require(binding.state().is_err(), "observer unbind not retired")?;
    require(
        surf.interaction_control(Request {
            op: Op::Check,
            request: 10,
            ..p3
        })
        .is_err(),
        "retired binding accepted",
    )?;
    drop(_master);
    // dev9p close is asynchronous: observe the carrier notification, rather
    // than assuming the server has processed its clunk when close returns.
    let mut poll = libthyla_rs::poll::PollSet::new();
    poll.add(&notes, libthyla_rs::poll::PollEvents::READ);
    require(
        poll.poll(libthyla_rs::poll::PollTimeout::Millis(1000))
            .map_err(|_| "carrier poll")?
            .count()
            != 0,
        "carrier notification deadline",
    )?;
    require(
        notes
            .try_read()
            .map_err(|_| "carrier note")?
            .is_some_and(|n| n.name == "tty:hup"),
        "expected master-close carrier note",
    )?;
    Ok(())
}
pub fn run(arg: &[u8]) -> Option<i64> {
    let result = match arg {
        b"--admission" => {
            let mut command = Command::new("/bin/kaua-term-probe");
            command
                .arg("--admission-sealed-child")
                .perm(T_SPAWN_PERM_SEAL);
            command
                .spawn()
                .map_err(|_| "spawn sealed probe")
                .and_then(|mut c| {
                    require(
                        c.wait().map_err(|_| "wait sealed probe")?.success(),
                        "sealed probe exit",
                    )
                })
        }
        b"--admission-sealed-child" => native(),
        _ => return None,
    };
    Some(match result {
        Ok(()) => {
            if arg == b"--admission" {
                libthyla_rs::t_putstr("admission-probe: PASS -- real binding, foreground, focus, context and retirement\n");
            }
            0
        }
        Err(why) => {
            libthyla_rs::t_putstr(&format!("admission-probe: FAIL -- {}\n", why));
            1
        }
    })
}
