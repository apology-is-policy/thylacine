//! Real reservation and control-lane round trips on the declared session.
use core::sync::atomic::{AtomicBool, Ordering};
use libhalcyon::seat_control::{Op, Request, Snapshot};
use libthyla_rs::{
    err::{Error, Result},
    service_worker::{Control, ServiceWorker},
    *,
};
struct State {
    snapshot: Snapshot,
    done: AtomicBool,
    go: AtomicBool,
}
fn exchange(c: &mut tapestry::seat::Channel, q: Request) -> Result<Snapshot> {
    c.start(q).map_err(|_| Error::Io)?;
    let end = libthyla_rs::time::monotonic_ns() + 3_000_000_000;
    loop {
        c.pump().map_err(|_| Error::Io)?;
        if let Some(r) = c.take() {
            return r.result.map(|r| r.state).map_err(|_| Error::Io);
        }
        if libthyla_rs::time::monotonic_ns() >= end {
            return Err(Error::TimedOut);
        }
        let mut p = TPollFd {
            fd: c.poll_fd(),
            events: T_POLLIN,
            revents: 0,
        };
        Error::from_syscall_return(unsafe { t_poll(&mut p, 1, 10) })?;
    }
}
fn serve(s: &State, c: &Control) -> Result<()> {
    let mut lane = tapestry::seat::Channel::open().map_err(|_| Error::Io)?;
    let mut q = Request {
        op: Op::Join,
        request: 1,
        registration: s.snapshot.registration,
        generation: s.snapshot.generation,
        revision: s.snapshot.revision,
    };
    let mut current = exchange(&mut lane, q)?;
    if !current.enabled {
        return Err(Error::PermissionDenied);
    }
    c.ready()?;
    while !s.go.load(Ordering::Acquire) {
        if c.stopping() {
            break;
        }
        let mut p = TPollFd {
            fd: c.stop_fd(),
            events: T_POLLIN,
            revents: 0,
        };
        unsafe {
            t_poll(&mut p, 1, 10);
        }
    }
    q = Request {
        op: Op::State,
        request: 2,
        registration: q.registration,
        generation: 0,
        revision: current.revision,
    };
    // The bounded idle observation must eventually complete even unchanged.
    current = exchange(&mut lane, q)?;
    q = Request {
        op: Op::Retire,
        request: 3,
        registration: q.registration,
        generation: current.generation,
        revision: current.revision,
    };
    current = exchange(&mut lane, q)?;
    if current.enabled {
        return Err(Error::PermissionDenied);
    }
    s.done.store(true, Ordering::Release);
    while !c.stopping() {
        let mut p = TPollFd {
            fd: c.stop_fd(),
            events: T_POLLIN,
            revents: 0,
        };
        Error::from_syscall_return(unsafe { t_poll(&mut p, 1, 100) })?;
    }
    Ok(())
}
pub fn exercise(ring: &tapestry::EventRing) -> core::result::Result<(), &'static str> {
    let (_reservation, snapshot) = tapestry::seat::reserve(ring).map_err(|_| "seat reserve")?;
    let mut worker = ServiceWorker::new(
        State {
            snapshot,
            done: AtomicBool::new(false),
            go: AtomicBool::new(false),
        },
        serve,
    )
    .map_err(|_| "seat lane join")?;
    worker.state().unwrap().go.store(true, Ordering::Release);
    let end = libthyla_rs::time::monotonic_ns() + 4_000_000_000;
    while !worker.state().unwrap().done.load(Ordering::Acquire) {
        worker.check().map_err(|_| "seat lane failed")?;
        if libthyla_rs::time::monotonic_ns() >= end {
            return Err("seat lane did not progress");
        }
        let mut p = TPollFd {
            fd: worker.notice_fd(),
            events: T_POLLIN,
            revents: 0,
        };
        unsafe {
            t_poll(&mut p, 1, 20);
        }
    }
    worker.shutdown().map_err(|_| "seat owner join")?;
    t_putstr("ADMISSION independent seat owner PASS\n");
    Ok(())
}

struct Episode {
    snapshot: Snapshot,
    stall: bool,
    done: AtomicBool,
    presenting: AtomicBool,
    returned: core::sync::atomic::AtomicU32,
}
fn episode_owner(s: &Episode, c: &Control) -> Result<()> {
    let mut lane = tapestry::seat::Channel::open().map_err(|_| Error::Io)?;
    let mut id = 1u64;
    let mut current = exchange(
        &mut lane,
        Request {
            op: Op::Join,
            request: id,
            registration: s.snapshot.registration,
            generation: s.snapshot.generation,
            revision: s.snapshot.revision,
        },
    )?;
    if !current.enabled {
        return Err(Error::PermissionDenied);
    }
    c.ready()?;
    let mut saw_quiescing = false;
    let mut saw_exclusive = false;
    let mut saw_failed = false;
    let mut suspended_returned = None;
    let deadline = libthyla_rs::time::monotonic_ns() + 45_000_000_000;
    loop {
        if c.stopping() || libthyla_rs::time::monotonic_ns() >= deadline {
            return Err(Error::TimedOut);
        }
        id = id.checked_add(1).ok_or(Error::Io)?;
        current = exchange(
            &mut lane,
            Request {
                op: Op::State,
                request: id,
                registration: current.registration,
                generation: 0,
                revision: current.revision,
            },
        )?;
        if current.phase == 1 && !saw_quiescing {
            saw_quiescing = true;
            // Give the ordinary present enough time to reach a real parked
            // response. This is a probe delay, never a production timeout ACK.
            let mut p = TPollFd {
                fd: c.stop_fd(),
                events: T_POLLIN,
                revents: 0,
            };
            unsafe {
                t_poll(&mut p, 1, 200);
            }
            let before = s.returned.load(Ordering::Acquire);
            if !s.presenting.load(Ordering::Acquire) {
                return Err(Error::Io);
            }
            unsafe {
                t_poll(&mut p, 1, 200);
            }
            if !s.presenting.load(Ordering::Acquire) || s.returned.load(Ordering::Acquire) != before
            {
                return Err(Error::Io);
            }
            suspended_returned = Some(before);
            t_putstr("SEAT normal present remains pending\n");
            if !s.stall {
                // This participant has no public clipboard endpoint or app
                // replies. Its local disabled state is therefore immediate.
                id += 1;
                current = exchange(
                    &mut lane,
                    Request {
                        op: Op::Cancelled,
                        request: id,
                        registration: current.registration,
                        generation: current.generation,
                        revision: current.revision,
                    },
                )?;
                if current.enabled {
                    return Err(Error::PermissionDenied);
                }
            } else {
                t_putstr("SEAT deliberately withholding cancellation\n");
            }
        }
        if current.phase == 2 {
            if s.stall || !saw_quiescing {
                return Err(Error::PermissionDenied);
            }

            if !s.presenting.load(Ordering::Acquire)
                || Some(s.returned.load(Ordering::Acquire)) != suspended_returned
            {
                return Err(Error::Io);
            }
            saw_exclusive = true;
        }
        if current.phase == 4 {
            saw_failed = true;
            if !s.stall {
                return Err(Error::Io);
            }
        }
        if current.phase == 0 && saw_quiescing {
            if (s.stall && (!saw_failed || saw_exclusive)) || (!s.stall && !saw_exclusive) {
                return Err(Error::PermissionDenied);
            }
            id += 1;
            let r = exchange(
                &mut lane,
                Request {
                    op: Op::Retire,
                    request: id,
                    registration: current.registration,
                    generation: current.generation,
                    revision: current.revision,
                },
            )?;
            if r.enabled {
                return Err(Error::PermissionDenied);
            }
            if !s.stall {
                t_putstr("SEAT exact cancellation acknowledged\n");
                t_putstr("SEAT trusted input opened with present still pending\n");
            }
            s.done.store(true, Ordering::Release);
            while !c.stopping() {
                let mut p = TPollFd {
                    fd: c.stop_fd(),
                    events: T_POLLIN,
                    revents: 0,
                };
                unsafe {
                    t_poll(&mut p, 1, 100);
                }
            }
            return Ok(());
        }
    }
}
fn episode(stall: bool) -> core::result::Result<(), &'static str> {
    let ring = tapestry::EventRing::connect().map_err(|_| "episode connect")?;
    ring.global_ctl("session on")
        .map_err(|_| "episode declare")?;
    let mut surface = tapestry::Surface::fullscreen_on(&ring).map_err(|_| "episode surface")?;
    surface.pixels().fill(0xff203040);
    surface.present(None).map_err(|_| "first present")?;
    let (_reservation, snapshot) = tapestry::seat::reserve(&ring).map_err(|_| "episode reserve")?;
    let mut worker = ServiceWorker::new(
        Episode {
            snapshot,
            stall,
            done: AtomicBool::new(false),
            presenting: AtomicBool::new(false),
            returned: core::sync::atomic::AtomicU32::new(0),
        },
        episode_owner,
    )
    .map_err(|_| "episode worker")?;
    ring.global_ctl("test-seat-park-next-present")
        .map_err(|_| "arm parked GPU RPC")?;
    t_putstr(if stall {
        "SEAT stalled participant ready\n"
    } else {
        "SEAT healthy participant ready\n"
    });
    while !worker.state().unwrap().done.load(Ordering::Acquire) {
        worker.check().map_err(|_| "episode owner failed")?;
        let s = worker.state().unwrap();
        s.presenting.store(true, Ordering::Release);
        surface.present(None).map_err(|_| "present resume")?;
        s.returned.fetch_add(1, Ordering::AcqRel);
        s.presenting.store(false, Ordering::Release);
        let mut p = TPollFd {
            fd: worker.notice_fd(),
            events: T_POLLIN,
            revents: 0,
        };
        unsafe {
            t_poll(&mut p, 1, 10);
        }
    }
    worker.shutdown().map_err(|_| "episode joined exit")?;
    // A second draw/response proves the original slot recycle gate survived.
    surface.pixels().fill(0xff405060);
    surface.present(None).map_err(|_| "final present")?;
    t_putstr(if stall {
        "seat-episode: PASS stalled refusal and restored presentation\n"
    } else {
        "seat-episode: PASS independent cancellation and restored presentation\n"
    });
    Ok(())
}
pub fn run(arg: &[u8]) -> Option<i64> {
    let (stall, child) = match arg {
        b"--seat-healthy" => (false, false),
        b"--seat-stalled" => (true, false),
        b"--seat-healthy-child" => (false, true),
        b"--seat-stalled-child" => (true, true),
        _ => return None,
    };
    let r = if child {
        episode(stall)
    } else {
        let mut cmd = libthyla_rs::process::Command::new("/bin/kaua-term-probe");
        cmd.arg(if stall {
            "--seat-stalled-child"
        } else {
            "--seat-healthy-child"
        })
        .perm(T_SPAWN_PERM_SEAL);
        cmd.spawn()
            .map_err(|_| "episode child spawn")
            .and_then(|mut c| {
                c.wait().map_err(|_| "episode child wait").and_then(|s| {
                    if s.success() {
                        Ok(())
                    } else {
                        Err("episode child exit")
                    }
                })
            })
    };
    Some(match r {
        Ok(()) => 0,
        Err(e) => {
            t_putstr("seat-episode: FAIL -- ");
            t_putstr(e);
            t_putstr("\n");
            1
        }
    })
}
