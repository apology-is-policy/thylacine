//! Native ordered HIA WRITE whose real 9P server withholds Rwrite.
//! A raw byte Spoor is deliberately not used: Loom requires a dev9p fid.
use ::alloc::{format, vec::Vec};
use core::sync::atomic::{AtomicBool, Ordering};
use libhalcyon::{
    interaction_control::{Op, Request},
    interaction_events::{Body, Record, SELECT},
};
use libthyla_rs::{
    err::Error,
    fs::File,
    handle::Rights,
    ninep as p9,
    service_worker::{Control, ServiceWorker},
    time, *,
};
use tapestry::ordered::Channel;
type Result<T = ()> = core::result::Result<T, &'static str>;
fn check(b: bool, s: &'static str) -> Result {
    if b {
        Ok(())
    } else {
        Err(s)
    }
}
fn file(fd: i64) -> core::result::Result<File, Error> {
    Error::from_syscall_return(fd)?;
    Ok(unsafe { File::from_raw_fd(fd as i32, Rights::READ | Rights::WRITE) })
}
struct Server {
    listener: File,
    held: AtomicBool,
    late: AtomicBool,
}
fn serve(s: &Server, c: &Control) -> core::result::Result<(), Error> {
    c.ready()?;
    let mut conn: Option<File> = None;
    let mut input = Vec::new();
    let mut selected = false;
    let mut held = None;
    let mut announced = false;
    loop {
        c.drain_wake()?;
        if c.stopping() {
            return Ok(());
        }
        let fd = conn
            .as_ref()
            .map_or(s.listener.as_raw_fd(), |f: &File| f.as_raw_fd());
        let mut p = [
            TPollFd {
                fd: c.stop_fd(),
                events: T_POLLIN,
                revents: 0,
            },
            TPollFd {
                fd,
                events: T_POLLIN,
                revents: 0,
            },
        ];
        Error::from_syscall_return(unsafe { t_poll(p.as_mut_ptr(), 2, -1) })?;
        if p[1].revents == 0 {
            continue;
        }
        if conn.is_none() {
            let f = file(unsafe { t_srv_accept(s.listener.as_raw_fd() as i64) })?;
            Error::from_syscall_return(unsafe { t_set_nonblock(f.as_raw_fd() as i64, true) })?;
            conn = Some(f);
            continue;
        }
        let mut buf = [0; 512];
        let n = unsafe { t_read(fd as i64, buf.as_mut_ptr(), buf.len()) };
        if n == 0 {
            return Ok(());
        }
        if n == -11 {
            continue;
        }
        Error::from_syscall_return(n)?;
        input.extend_from_slice(&buf[..n as usize]);
        loop {
            if input.len() < 4 {
                break;
            }
            let size = u32::from_le_bytes(input[..4].try_into().unwrap()) as usize;
            if !(7..=1024).contains(&size) {
                return Err(Error::Io);
            }
            if input.len() < size {
                break;
            }
            let b = &input[..size];
            let h = p9::peek_header(b).map_err(|_| Error::Io)?;
            let tag = h.tag;
            let mut out = [0; 256];
            let qid = p9::Qid {
                kind: p9::P9_QTFILE,
                version: 0,
                path: 1,
            };
            let n = match h.mtype {
                p9::P9_TVERSION => p9::build_rversion(&mut out, tag, 8192, b"9P2000.L"),
                p9::P9_TATTACH => p9::build_rattach(&mut out, tag, &qid),
                p9::P9_TLOPEN => p9::build_rlopen(&mut out, tag, &qid, 0),
                p9::P9_TGETATTR => p9::build_rgetattr(
                    &mut out,
                    tag,
                    p9::P9_GETATTR_MODE | p9::P9_GETATTR_UID | p9::P9_GETATTR_GID,
                    &qid,
                    0o100666,
                    1000,
                    1000,
                    1,
                    0,
                ),
                p9::P9_TWRITE => {
                    let a = p9::parse_twrite(b).map_err(|_| Error::Io)?;
                    if a.data == SELECT {
                        selected = true;
                        announced = false;
                        p9::build_rwrite(&mut out, tag, a.count)
                    } else {
                        if Request::decode(a.data).is_none() || held.is_some() {
                            return Err(Error::Io);
                        }
                        held = Some(tag);
                        s.held.store(true, Ordering::Release);
                        Ok(0)
                    }
                }
                p9::P9_TREAD => {
                    if selected && !announced {
                        announced = true;
                        p9::build_rread(
                            &mut out,
                            tag,
                            &Record {
                                sequence: 1,
                                body: Body::Ready(3),
                            }
                            .encode(),
                        )
                    } else {
                        Ok(0)
                    }
                }
                p9::P9_TFLUSH => {
                    let a = p9::parse_tflush(b).map_err(|_| Error::Io)?;
                    if held == Some(a.oldtag) {
                        // The old response may precede Rflush, but it must no
                        // longer have a live callback in the retired ring.
                        let n = p9::build_rwrite(&mut out, a.oldtag, 80).map_err(|_| Error::Io)?;
                        if unsafe { t_write(fd as i64, out.as_ptr(), n) } != n as i64 {
                            return Err(Error::Io);
                        }
                        held = None;
                        s.late.store(true, Ordering::Release);
                    }
                    p9::build_rflush(&mut out, tag)
                }
                p9::P9_TCLUNK => p9::build_rclunk(&mut out, tag),
                _ => p9::build_rlerror(&mut out, tag, p9::E_NOSYS),
            }
            .map_err(|_| Error::Io)?;
            if n != 0 && unsafe { t_write(fd as i64, out.as_ptr(), n) } != n as i64 {
                return Err(Error::Io);
            }
            input.drain(..size);
        }
    }
}
fn poll(fd: i32, ms: i32) -> Result<i64> {
    let mut p = TPollFd {
        fd,
        events: T_POLLIN,
        revents: 0,
    };
    let n = unsafe { t_poll(&mut p, 1, ms) };
    if n < 0 {
        Err("deadline poll")
    } else {
        Ok(n)
    }
}
pub fn run() -> Result {
    let root = file(unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) })
        .map_err(|_| "deadline root")?;
    let name = format!("hi1-deadline-{}", unsafe { t_getpid() });
    let listener = file(unsafe {
        t_walk_create(
            root.as_raw_fd() as i64,
            name.as_ptr(),
            name.len(),
            T_OREAD,
            0,
        )
    })
    .map_err(|_| "deadline post")?;
    let mut server = ServiceWorker::new(
        Server {
            listener,
            held: AtomicBool::new(false),
            late: AtomicBool::new(false),
        },
        serve,
    )
    .map_err(|_| "deadline server")?;
    let path = format!("/srv/{}", name);
    let ctl = file(unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_ORDWR) })
        .map_err(|_| "deadline ctl")?;
    let retained = ctl.try_clone().map_err(|_| "deadline retain transport")?;
    let mut lane = Some(Channel::from_file(ctl).map_err(|_| "deadline channel")?);
    let until = time::monotonic_ns() + 5_000_000_000;
    loop {
        let c = lane.as_mut().unwrap();
        c.pump().map_err(|_| "deadline ready pump")?;
        if let Some(r) = c.take() {
            check(r.body == Body::Ready(3), "deadline ready bytes")?;
            break;
        }
        check(time::monotonic_ns() < until, "deadline ready timeout")?;
        poll(c.poll_fd(), 10)?;
    }
    let mut owner = crate::interaction::Interaction::new(3, 1000).map_err(|_| "deadline owner")?;
    owner.seat(Some(1));
    let start = time::monotonic_ns();
    let q = owner
        .control(
            Op::Bind,
            crate::controllers::RouteKey {
                leaf: 1,
                incarnation: 1,
            },
            1,
            1,
            start / 1_000_000,
        )
        .map_err(|_| "deadline request")?;
    lane.as_mut()
        .unwrap()
        .start(q)
        .map_err(|_| "deadline start")?;
    let mut wakes = 0;
    loop {
        let now = time::monotonic_ns();
        let c = lane.as_mut().unwrap();
        c.pump().map_err(|_| "deadline blocked pump")?;
        check(c.take().is_none(), "deadline unexpected receipt")?;
        if let Some(done) = owner.expire(now / 1_000_000) {
            check(
                matches!(
                    done,
                    crate::interaction::Completion::Control {
                        result: Err(libhalcyon::interaction_wire::Failure::Timeout),
                        ..
                    }
                ),
                "deadline completion",
            )?;
            break;
        }
        let timeout =
            crate::servicepool::poll_timeout(now, owner.deadline().map(|d| d * 1_000_000));
        check(timeout >= 0, "deadline became infinite")?;
        poll(c.poll_fd(), timeout)?;
        wakes += 1;
        check(wakes < 10, "deadline busy spin")?;
    }
    check(
        server.state().unwrap().held.load(Ordering::Acquire),
        "deadline WRITE never reached peer",
    )?;
    let elapsed = time::monotonic_ns() - start;
    check(
        elapsed >= 29_000_000_000 && elapsed < 35_000_000_000,
        "deadline elapsed bound",
    )?;
    check(
        crate::session_seat::retire_overdue(
            &mut owner,
            &mut lane,
            time::monotonic_ns() / 1_000_000,
        ) == Err(Error::TimedOut),
        "deadline close helper",
    )?;
    check(lane.is_none() && !owner.busy(), "deadline retained flight")?;
    // Loom closes by joining SQPOLL and abandoning RPC callbacks under the
    // client lock; it sends Tflush without awaiting Rflush. Keep the 9P session alive
    // and deliver the old Rwrite after teardown. A fresh ring must see only its
    // own selection response, never an old callback into recycled storage.
    check(
        time::monotonic_ns() - start < 35_000_000_000,
        "deadline join bound",
    )?;
    let mut fresh = Channel::from_file(retained).map_err(|_| "deadline fresh ring")?;
    let until = time::monotonic_ns() + 5_000_000_000;
    loop {
        fresh.pump().map_err(|_| "late reply corrupted new ring")?;
        if let Some(r) = fresh.take() {
            check(r.body == Body::Ready(3), "deadline fresh record")?;
            break;
        }
        check(time::monotonic_ns() < until, "deadline fresh timeout")?;
        poll(fresh.poll_fd(), 10)?;
    }
    check(
        server.state().unwrap().late.load(Ordering::Acquire),
        "deadline no late Rwrite",
    )?;
    drop(fresh);
    server.shutdown().map_err(|_| "deadline server join")?;
    t_putstr("service-probe: overdue WRITE retired, ring joined and late Rwrite isolated\n");
    Ok(())
}
