//! Guest proof of aggregation over real pipes and kernel-confirmed shutdown.
use alloc::vec::Vec;
use libthyla_rs::err::Error;
use libthyla_rs::fs::File;
use libthyla_rs::io::{Read, Write};
use libthyla_rs::poll::AsFd;
use libthyla_rs::poll_worker::PollWorker;
use libthyla_rs::{TPollFd, T_POLLHUP, T_POLLIN};

type Result<T = ()> = core::result::Result<T, &'static str>;
fn require(ok: bool, why: &'static str) -> Result {
    if ok {
        Ok(())
    } else {
        Err(why)
    }
}
fn wait(fd: i32, timeout: i32) -> Result<i16> {
    let mut p = TPollFd {
        fd,
        events: T_POLLIN,
        revents: 0,
    };
    let rc = unsafe { libthyla_rs::t_poll(&mut p, 1, timeout) };
    if rc < 0 {
        return Err("outer poll");
    }
    Ok(p.revents)
}
fn now() -> u64 {
    libthyla_rs::time::monotonic_ns() / 1_000_000
}
fn one_batch(worker: &mut PollWorker) -> Result<libthyla_rs::poll_worker::ReadyBatch> {
    require(
        wait(worker.as_raw_fd(), 2000)? & T_POLLIN != 0,
        "worker notice timeout",
    )?;
    worker.take_ready().map_err(|_| "take readiness")
}
fn pipes() -> Result<(File, File)> {
    libthyla_rs::process::pipe().map_err(|_| "pipe")
}
fn put(file: &mut File) -> Result {
    require(file.write(&[77]).map_err(|_| "write")? == 1, "short write")
}
fn take(file: &mut File) -> Result {
    let mut byte = [0u8];
    require(
        file.read(&mut byte).map_err(|_| "read")? == 1 && byte == [77],
        "pipe contents",
    )
}

fn exercise() -> Result {
    require(
        PollWorker::new(0).err() == Some(Error::InvalidArgument),
        "zero capacity",
    )?;
    require(
        PollWorker::new(64).err() == Some(Error::InvalidArgument),
        "poll overflow capacity",
    )?;
    let mut maximum = PollWorker::new(63).map_err(|_| "maximum poll capacity")?;
    maximum.shutdown().map_err(|_| "maximum worker join")?;
    let mut worker = PollWorker::new(39).map_err(|_| "start 39-slot worker")?;
    let mut pairs = Vec::new();
    let mut ids = Vec::new();
    for _ in 0..39 {
        let (read, mut write) = pipes()?;
        // Readiness can already exist before the registration is installed.
        put(&mut write)?;
        ids.push(worker.register(&read, T_POLLIN).map_err(|_| "register")?);
        pairs.push((read, write));
    }
    require(
        worker.register(&pairs[0].0, T_POLLIN) == Err(Error::Busy),
        "full slots",
    )?;
    let mut seen = 0u64;
    let deadline = now() + 5000;
    while seen.count_ones() < 39 {
        require(now() < deadline, "all peers progress deadline")?;
        for event in one_batch(&mut worker)?.iter() {
            let i = ids
                .iter()
                .position(|id| *id == event.watch)
                .ok_or("unexpected watch id")?;
            require(event.events & T_POLLIN != 0, "read event")?;
            require(seen & (1u64 << i) == 0, "duplicate notice without rearm")?;
            seen |= 1u64 << i;
        }
    }
    // Every source remains level-ready. Delivery disarmed all of them.
    require(
        wait(worker.as_raw_fd(), 50)? == 0,
        "ready peers spin notices",
    )?;
    for (read, _) in &mut pairs {
        take(read)?;
    }
    for &id in &ids {
        worker.rearm(id, T_POLLIN).map_err(|_| "rearm empty")?;
    }
    require(wait(worker.as_raw_fd(), 50)? == 0, "empty peers signal")?;
    // A single busy peer cannot prevent an independently rearmed peer waking.
    for i in [0usize, 38] {
        put(&mut pairs[i].1)?;
    }
    let mut seen = 0u64;
    let deadline = now() + 5000;
    while seen != (1 | (1u64 << 38)) {
        require(now() < deadline, "rearmed deadline")?;
        for event in one_batch(&mut worker)?.iter() {
            let i = ids
                .iter()
                .position(|id| *id == event.watch)
                .ok_or("rearmed id")?;
            require(i == 0 || i == 38, "unready peer delivered")?;
            seen |= 1u64 << i;
        }
    }
    worker.shutdown().map_err(|_| "join full worker")?;
    require(
        worker.as_raw_fd() == -1,
        "joined worker still owns notification",
    )?;
    worker.shutdown().map_err(|_| "idempotent shutdown")?;
    drop(pairs);

    let mut worker = PollWorker::new(1).map_err(|_| "start reuse worker")?;
    let mut other = PollWorker::new(1).map_err(|_| "start second worker")?;
    let (identity_read, _identity_write) = pipes()?;
    let foreign = other
        .register(&identity_read, T_POLLIN)
        .map_err(|_| "foreign registration")?;
    let own = worker
        .register(&identity_read, T_POLLIN)
        .map_err(|_| "own registration")?;
    require(
        foreign != own && worker.remove(foreign) == Err(Error::NotFound),
        "foreign worker token accepted",
    )?;
    worker.remove(own).map_err(|_| "remove own identity")?;
    let _ = one_batch(&mut worker)?;
    other.shutdown().map_err(|_| "second worker join")?;
    for _ in 0..64 {
        let (read, mut write) = pipes()?;
        let old = worker
            .register(&read, T_POLLIN)
            .map_err(|_| "reuse register")?;
        drop(read); // only the worker's duplicate now pins the original endpoint
        let (replacement, mut unrelated) = pipes()?;
        put(&mut unrelated)?; // may reuse the UI's old raw fd; not its open object
        require(
            wait(worker.as_raw_fd(), 10)? == 0,
            "fd reuse retargeted watch",
        )?;
        put(&mut write)?;
        let batch = one_batch(&mut worker)?;
        require(
            batch.iter().count() == 1 && batch.iter().next().unwrap().watch == old,
            "duplicate did not retain source",
        )?;
        worker.remove(old).map_err(|_| "remove")?;
        require(
            worker.rearm(old, T_POLLIN) == Err(Error::NotFound),
            "retired arm accepted",
        )?;
        // The duplicate is closed only once the worker leaves its old poll.
        let mut hup = TPollFd {
            fd: write.as_raw_fd(),
            events: T_POLLHUP,
            revents: 0,
        };
        require(
            unsafe { libthyla_rs::t_poll(&mut hup, 1, 2000) } > 0,
            "retired duplicate retained",
        )?;
        let new = worker
            .register(&replacement, T_POLLIN)
            .map_err(|_| "reclaimed capacity")?;
        require(new != old, "registration generation reused")?;
        let deadline = now() + 2000;
        loop {
            require(now() < deadline, "replacement notice deadline")?;
            let batch = one_batch(&mut worker)?;
            if batch.iter().any(|e| e.watch == new) {
                break;
            }
        }
        worker.remove(new).map_err(|_| "remove replacement")?;
        // Observe actual reclamation before the next single-slot registration.
        drop(replacement);
        let mut hup = TPollFd {
            fd: unrelated.as_raw_fd(),
            events: T_POLLHUP,
            revents: 0,
        };
        require(
            unsafe { libthyla_rs::t_poll(&mut hup, 1, 2000) } > 0,
            "replacement still pinned",
        )?;
        let _ = worker.take_ready().map_err(|_| "drain reclaimed notice")?;
    }
    worker.shutdown().map_err(|_| "join reuse worker")?;

    // Constructor rollback: leave only three fd slots, so its second pipe
    // cannot be created. Exactly those three slots must remain available.
    let (source, _peer) = pipes()?;
    let mut held = Vec::new();
    while let Ok(fd) = source.try_clone() {
        held.push(fd);
    }
    require(held.len() >= 3, "fd exhaustion fixture")?;
    for _ in 0..3 {
        held.pop();
    }
    require(
        PollWorker::new(1).is_err(),
        "partial pipe setup unexpectedly succeeded",
    )?;
    for _ in 0..3 {
        held.push(source.try_clone().map_err(|_| "constructor leaked fd")?);
    }
    require(source.try_clone().is_err(), "exhaustion fixture drift")?;
    drop(held);
    Ok(())
}

pub fn run() -> i64 {
    match exercise() {
        Ok(()) => {
            libthyla_rs::t_putstr("readiness-probe: PASS -- capacity, one-shot wake, fd reuse, rearm, join, rollback\n");
            0
        }
        Err(why) => {
            libthyla_rs::t_putstr("readiness-probe: FAIL -- ");
            libthyla_rs::t_putstr(why);
            libthyla_rs::t_putstr("\n");
            1
        }
    }
}
