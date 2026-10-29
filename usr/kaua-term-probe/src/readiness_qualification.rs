//! Deterministic schedules over the actual native worker and real kernel pipes.
//! Opt-in binary only. The RAII gate is declared after the worker so any test
//! error releases its paused thread before PollWorker drops and joins it.
use alloc::vec::Vec;
use libthyla_rs::err::Error;
use libthyla_rs::fs::File;
use libthyla_rs::io::{Read, Write};
use libthyla_rs::poll::AsFd;
use libthyla_rs::poll_worker::qualification::{self as q, Fault, Gate, Point};
use libthyla_rs::poll_worker::{PollWorker, WatchId};
use libthyla_rs::{TPollFd, T_POLLERR, T_POLLHUP, T_POLLIN};
type Result<T = ()> = core::result::Result<T, &'static str>;
fn need(ok: bool, why: &'static str) -> Result {
    if ok {
        Ok(())
    } else {
        Err(why)
    }
}
fn pipe() -> Result<(File, File)> {
    libthyla_rs::process::pipe().map_err(|_| "pipe")
}
fn put(f: &mut File) -> Result {
    need(f.write(&[51]).map_err(|_| "write")? == 1, "short write")
}
fn take(f: &mut File) -> Result {
    let mut b = [0];
    need(
        f.read(&mut b).map_err(|_| "read")? == 1 && b == [51],
        "contents",
    )
}
fn poll(fd: i32, event: i16, ms: i32) -> Result<i16> {
    let mut p = TPollFd {
        fd,
        events: event,
        revents: 0,
    };
    need(
        unsafe { libthyla_rs::t_poll(&mut p, 1, ms) } >= 0,
        "poll error",
    )?;
    Ok(p.revents)
}
fn ready(w: &mut PollWorker, id: WatchId) -> Result {
    need(
        poll(w.as_raw_fd(), T_POLLIN, 2000)? & T_POLLIN != 0,
        "notice timeout",
    )?;
    let batch = w.take_ready().map_err(|_| "take ready")?;
    need(
        batch.iter().count() == 1 && batch.iter().next().unwrap().watch == id,
        "wrong ready batch",
    )
}
fn fd(w: &mut PollWorker, id: WatchId) -> Result<i32> {
    // Used only to choose the instrumentation boundary, never for subsequent I/O.
    w.with_fd(id, |f| f).map_err(|_| "watch fd")
}
fn schedules() -> Result {
    // Snapshot an old arm, produce its result, then drain/rearm BEFORE the
    // worker publishes that result. Matching by generation alone is unsound.
    let mut w = PollWorker::new(2).map_err(|_| "worker")?;
    let (mut r, mut p) = pipe()?;
    let id = w.register(&r, T_POLLIN).map_err(|_| "register")?;
    let g = Gate::arm(Point::AfterPoll, fd(&mut w, id)?).map_err(|_| "arm stale gate")?;
    put(&mut p)?;
    g.arrived().map_err(|_| "stale result rendezvous")?;
    take(&mut r)?;
    w.rearm(id, T_POLLIN).map_err(|_| "rearm stale")?;
    g.release().map_err(|_| "release stale")?;
    need(
        poll(w.as_raw_fd(), T_POLLIN, 30)? == 0,
        "old poll result acknowledged new arm",
    )?;
    put(&mut p)?;
    ready(&mut w, id)?;
    // The source remains ready but its delivered arm must stay disarmed.
    need(
        poll(w.as_raw_fd(), T_POLLIN, 30)? == 0,
        "disarmed level peer spins",
    )?;
    take(&mut r)?;
    w.shutdown().map_err(|_| "schedule join")?;

    // Data arrives while the worker is between snapshot and poll. At the same
    // boundary the owner re-arms and clears any pending notice. The old ticket
    // is discarded, and the command latch must drive a fresh poll and delivery.
    let mut w = PollWorker::new(1).map_err(|_| "before worker")?;
    let (r, mut p) = pipe()?;
    let g = Gate::arm(Point::BeforePoll, r.as_raw_fd()).map_err(|_| "arm before gate")?;
    let id = w
        .register_owned(r, T_POLLIN)
        .map_err(|_| "owned before register")?;
    g.arrived().map_err(|_| "before poll rendezvous")?;
    need(
        w.take_ready()
            .map_err(|_| "drain before notice")?
            .iter()
            .count()
            == 0,
        "premature notice",
    )?;
    w.rearm(id, T_POLLIN).map_err(|_| "before rearm")?;
    put(&mut p)?;
    g.release().map_err(|_| "release before")?;
    ready(&mut w, id)?;
    w.shutdown().map_err(|_| "before join")?;

    // Remove a descriptor while its poll result is parked. It is logically
    // invalid immediately, physically pinned until that result is retired.
    let mut w = PollWorker::new(1).map_err(|_| "retire worker")?;
    let (r, mut p) = pipe()?;
    let id = w
        .register_owned(r, T_POLLIN)
        .map_err(|_| "owned retire register")?;
    let g = Gate::arm(Point::AfterPoll, fd(&mut w, id)?).map_err(|_| "arm retire gate")?;
    put(&mut p)?;
    g.arrived().map_err(|_| "retire rendezvous")?;
    w.remove(id).map_err(|_| "remove paused")?;
    need(
        w.free_slots().map_err(|_| "retiring capacity")? == 0,
        "retiring capacity published early",
    )?;
    need(
        w.with_fd(id, |_| ()) == Err(Error::NotFound),
        "retired borrowed fd accepted",
    )?;
    let (replacement, _peer) = pipe()?;
    need(
        w.register(&replacement, T_POLLIN) == Err(Error::Busy),
        "slot reused before old poll retired",
    )?;
    need(
        poll(p.as_raw_fd(), T_POLLHUP, 0)? == 0,
        "descriptor closed during old poll",
    )?;
    g.release().map_err(|_| "release retire")?;
    need(
        poll(p.as_raw_fd(), T_POLLHUP, 2000)? & T_POLLERR != 0,
        "retired fd not closed",
    )?;
    need(
        w.take_ready().map_err(|_| "drain retired")?.iter().count() == 0,
        "retired poll result escaped",
    )?;
    need(
        w.free_slots().map_err(|_| "reclaimed capacity")? == 1,
        "reclaimed capacity missing",
    )?;
    let new = w
        .register(&replacement, T_POLLIN)
        .map_err(|_| "reuse retired slot")?;
    need(
        new != id && w.rearm(id, T_POLLIN) == Err(Error::NotFound),
        "retired identity reused",
    )?;
    w.shutdown().map_err(|_| "retire join")?;
    Ok(())
}
fn free_handles(source: &File, held: &mut Vec<File>) -> usize {
    while let Ok(f) = source.try_clone() {
        held.push(f);
    }
    let n = held.len();
    held.clear();
    n
}
fn rollback() -> Result {
    let (source, _peer) = pipe()?;
    let mut held = Vec::new();
    let baseline = free_handles(&source, &mut held);
    for fault in [
        Fault::CommandPipe,
        Fault::NoticePipe,
        Fault::Context,
        Fault::Reserve,
        Fault::Protect,
        Fault::Spawn,
        Fault::Startup,
    ] {
        q::fail_next(fault);
        need(
            PollWorker::new(1).err() == Some(Error::NoMemory),
            "injected constructor error changed",
        )?;
        need(q::fault_consumed(), "constructor fault not reached")?;
        need(
            free_handles(&source, &mut held) == baseline,
            "constructor rollback leaked handles",
        )?;
        need(
            q::live_contexts() == 0,
            "constructor rollback leaked context",
        )?;
        if matches!(fault, Fault::Protect | Fault::Spawn | Fault::Startup) {
            let base = q::last_stack();
            need(
                base != 0
                    && unsafe {
                        libthyla_rs::t_burrow_protect(
                            base,
                            4096,
                            libthyla_rs::T_BURROW_PROT_READ,
                            0,
                        )
                    } < 0,
                "constructor rollback retained stack mapping",
            )?;
        }
    }
    let mut w = PollWorker::new(1).map_err(|_| "duplicate fault worker")?;
    let baseline = free_handles(&source, &mut held);
    q::fail_next(Fault::Duplicate);
    need(
        w.register(&source, T_POLLIN) == Err(Error::NoMemory),
        "duplicate fault lost",
    )?;
    need(
        free_handles(&source, &mut held) == baseline,
        "duplicate rollback leaked handles",
    )?;
    let id = w
        .register(&source, T_POLLIN)
        .map_err(|_| "duplicate failure consumed capacity")?;
    w.remove(id).map_err(|_| "remove after duplicate failure")?;
    w.shutdown().map_err(|_| "rollback join")?;
    need(q::live_contexts() == 0, "joined context retained")?;
    Ok(())
}
pub fn run() -> i64 {
    match schedules().and_then(|_| rollback()) {
        Ok(()) => {
            libthyla_rs::t_putstr("readiness-qualification: PASS -- controlled rearm, wake, retirement and eight rollback boundaries\n");
            0
        }
        Err(why) => {
            libthyla_rs::t_putstr("readiness-qualification: FAIL -- ");
            libthyla_rs::t_putstr(why);
            libthyla_rs::t_putstr("\n");
            1
        }
    }
}

use libthyla_rs::service_worker::qualification::{self as sq, Fault as SFault};
/// The executor exits the posting Proc on a published failure. The parent
/// witnesses the registry after process teardown; this is NOT Warden/UI recovery.
pub fn failure_server() -> i64 {
    fn serve() -> Result<i64> {
        let mode = libthyla_rs::env::args().nth(2).ok_or("failure mode")?;
        let name = libthyla_rs::env::args().nth(3).ok_or("failure name")?;
        let name = core::str::from_utf8(name).map_err(|_| "failure name utf8")?;
        if mode == b"register" {
            sq::fail_next(SFault::Published);
        }
        let mut server = match crate::paneplace::PanePlaceServer::post(name) {
            Err(crate::paneplace::PostError::Published(Error::NoMemory)) if mode == b"register" => {
                return Ok(42)
            }
            Ok(_s) if mode == b"repost" => return Ok(42),
            Ok(s) if mode == b"poll" => s,
            _ => return Err("failure post disposition"),
        };
        sq::fail_next(SFault::Service);
        need(
            unsafe { libthyla_rs::t_write(1, b"R".as_ptr(), 1) } == 1,
            "failure server ready",
        )?;
        let end = libthyla_rs::time::monotonic_ns() + 3_000_000_000;
        loop {
            match server.service() {
                Err(Error::NoMemory) => return Ok(42),
                Err(_) => return Err("wrong service failure"),
                Ok(()) => {}
            }
            need(
                libthyla_rs::time::monotonic_ns() < end,
                "service failure not delivered",
            )?;
            let mut fds = Vec::new();
            server.push_fds(&mut fds);
            need(
                unsafe { libthyla_rs::t_poll(fds.as_mut_ptr(), fds.len(), 100) } >= 0,
                "failure server poll",
            )?;
        }
    }
    match serve() {
        Ok(code) => code,
        Err(why) => {
            let message = alloc::format!("readiness-failure-child: {}\n", why);
            unsafe {
                libthyla_rs::t_write(2, message.as_ptr(), message.len());
            }
            1
        }
    }
}

pub fn media_failure() -> i64 {
    fn exercise() -> Result {
        use alloc::format;
        use libthyla_rs::process::{Child, Command, Stdio};
        struct Reap(Child);
        impl Drop for Reap {
            fn drop(&mut self) {
                let _ = self.0.kill();
                let _ = self.0.wait();
            }
        }
        let name = format!("no-post-{}", unsafe { libthyla_rs::t_getpid() });
        sq::fail_next(SFault::Startup);
        need(
            matches!(
                crate::paneplace::PanePlaceServer::post(&name),
                Err(crate::paneplace::PostError::Unavailable)
            ),
            "pre-post failure published",
        )?;
        let path = format!("/srv/halcyon-{}", name);
        need(File::open(&path).is_err(), "pre-post name exists")?;
        for mode in ["register", "poll"] {
            let name = format!("failure-{}-{}", unsafe { libthyla_rs::t_getpid() }, mode);
            let path = format!("/srv/halcyon-{}", name);
            let mut cmd = Command::new("/bin/kaua-term-probe");
            cmd.arg("--readiness-failure-server")
                .arg(mode)
                .arg(&name)
                .stdout(Stdio::Piped);
            let mut child = Reap(cmd.spawn().map_err(|_| "spawn failure server")?);
            if mode == "poll" {
                let out = child.0.stdout.as_mut().ok_or("failure stdout")?;
                need(
                    poll(out.as_raw_fd(), T_POLLIN, 2000)? & T_POLLIN != 0,
                    "failure server not ready",
                )?;
                let mut b = [0];
                need(
                    out.read(&mut b).map_err(|_| "read failure ready")? == 1 && b == [b'R'],
                    "failure ready byte",
                )?;
                // Dialing wakes the real listener. The worker fails instead of
                // delivering its result; poster exit must release the dialer.
                let _ = File::open(&path);
            }
            let status = child.0.wait().map_err(|_| "wait failure server")?;
            need(
                status.code() == Some(1),
                "posted failure did not reach process exit",
            )?;
            need(
                File::open(&path).is_err(),
                "exited poster retained registry entry",
            )?;
            // The registry entry follows the poster PROCESS, not the local
            // server value. Repost in a short-lived child so the next case
            // does not retain a dead listener in this tight 16-slot fixture.
            let mut cmd = Command::new("/bin/kaua-term-probe");
            cmd.arg("--readiness-failure-server")
                .arg("repost")
                .arg(&name);
            let mut replacement = Reap(cmd.spawn().map_err(|_| "spawn repost server")?);
            need(
                replacement
                    .0
                    .wait()
                    .map_err(|_| "wait repost server")?
                    .code()
                    == Some(42),
                "service cannot repost after poster exit",
            )?;
            need(
                File::open(&path).is_err(),
                "repost exit retained registry entry",
            )?;
        }
        Ok(())
    }
    match exercise() {
        Ok(()) => {
            libthyla_rs::t_putstr("service-readiness-failure: PASS -- before-post rollback, published failure, process exit and repost\n");
            0
        }
        Err(why) => {
            libthyla_rs::t_putstr("service-readiness-failure: FAIL -- ");
            libthyla_rs::t_putstr(why);
            libthyla_rs::t_putstr("\n");
            1
        }
    }
}
