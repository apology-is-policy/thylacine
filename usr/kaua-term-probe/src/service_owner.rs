//! Native schedules for the service thread owner, independent of graphics.
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use libthyla_rs::{
    err::{Error, Result},
    service_worker::{Control, ServiceWorker},
    *,
};
struct State {
    ticks: AtomicU32,
    fail: AtomicBool,
}
fn state() -> State {
    State {
        ticks: AtomicU32::new(0),
        fail: AtomicBool::new(false),
    }
}
fn serve(s: &State, c: &Control) -> Result<()> {
    c.ready()?;
    loop {
        c.drain_wake()?;
        if c.stopping() {
            return Ok(());
        }
        if s.fail.load(Ordering::Acquire) {
            return Err(Error::Io);
        }
        s.ticks.fetch_add(1, Ordering::Release);
        let mut p = TPollFd {
            fd: c.stop_fd(),
            events: T_POLLIN,
            revents: 0,
        };
        Error::from_syscall_return(unsafe { t_poll(&mut p, 1, 5) })?;
    }
}
fn refuse(_: &State, _: &Control) -> Result<()> {
    Err(Error::PermissionDenied)
}
fn exercise() -> core::result::Result<(), &'static str> {
    if ServiceWorker::new(state(), refuse).err() != Some(Error::PermissionDenied) {
        return Err("startup refusal");
    }
    for _ in 0..3 {
        let mut worker = ServiceWorker::new(state(), serve).map_err(|_| "startup")?;
        let before = worker.state().unwrap().ticks.load(Ordering::Acquire);
        // The UI thread waits on an unrelated, quiet notice. Service work
        // must continue without a UI pass or a rendering completion.
        let mut p = TPollFd {
            fd: worker.notice_fd(),
            events: T_POLLIN,
            revents: 0,
        };
        if unsafe { t_poll(&mut p, 1, 60) } != 0 {
            return Err("idle notice spinning");
        }
        if worker.state().unwrap().ticks.load(Ordering::Acquire) <= before {
            return Err("owner needs UI progress");
        }
        worker.state().unwrap().fail.store(true, Ordering::Release);
        if unsafe { t_poll(&mut p, 1, 1000) } <= 0 {
            return Err("failure notice");
        }
        if worker.check() != Err(Error::Io) {
            return Err("failed service remained ready");
        }
        worker.shutdown().map_err(|_| "failed owner join")?;
        worker.shutdown().map_err(|_| "idempotent join")?;
    }
    let mut worker = ServiceWorker::new(state(), serve).map_err(|_| "stop startup")?;
    // More wake hints than pipe capacity must coalesce, not block or fail.
    // The durable stopping bit must survive those hints and still join.
    for _ in 0..16384 {
        worker.wake().map_err(|_| "coalesced wake")?;
    }
    worker.shutdown().map_err(|_| "stop join")?;
    // Partial pipe construction may not leak any of the last three handles.
    let (source, _peer) = libthyla_rs::process::pipe().map_err(|_| "fixture pipe")?;
    let mut held = ::alloc::vec::Vec::new();
    while let Ok(f) = source.try_clone() {
        held.push(f);
    }
    if held.len() < 3 {
        return Err("fixture headroom");
    }
    for _ in 0..3 {
        held.pop();
    }
    if ServiceWorker::new(state(), serve).is_ok() {
        return Err("setup accepted incomplete pipes");
    }
    for _ in 0..3 {
        held.push(source.try_clone().map_err(|_| "setup leaked handle")?);
    }
    if source.try_clone().is_ok() {
        return Err("fixture drift");
    }
    Ok(())
}
pub fn run() -> i64 {
    match exercise() {
        Ok(()) => {
            t_putstr("service-owner-probe: PASS -- independent progress, failure, joined stop, rollback\n");
            0
        }
        Err(e) => {
            t_putstr("service-owner-probe: FAIL -- ");
            t_putstr(e);
            t_putstr("\n");
            1
        }
    }
}
