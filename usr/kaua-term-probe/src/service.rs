//! Real SrvConn backpressure proof, using the exact Halcyon stream pump and
//! native adapter. Requires an explicitly conferred POST_SERVICE provincia.
use crate::serviceio::NativeEndpoint;
use crate::servicewire::{Handler, Interest, Stream};
use ::alloc::{format, vec, vec::Vec};
use libthyla_rs::{fs::File, handle::Rights, poll::AsFd, poll_worker::PollWorker, *};

type Result<T = ()> = core::result::Result<T, &'static str>;
fn check(ok: bool, why: &'static str) -> Result {
    if ok {
        Ok(())
    } else {
        Err(why)
    }
}
fn file(fd: i64, step: &'static str) -> Result<File> {
    if fd < 0 {
        t_putstr(&format!("service-probe: setup {} returned {}\n", step, fd));
        return Err(step);
    }
    Ok(unsafe { File::from_raw_fd(fd as i32, Rights::READ | Rights::WRITE) })
}
fn nonblock(f: &File) -> Result {
    check(
        unsafe { t_set_nonblock(f.as_raw_fd() as i64, true) } == 0,
        "set nonblocking",
    )
}
struct Response {
    bytes: Vec<u8>,
    seen: usize,
}
impl Handler for Response {
    fn dispatch(&mut self, frame: &[u8]) -> core::result::Result<(), ()> {
        if frame.len() != 7 || frame[4] as usize != self.seen {
            return Err(());
        }
        self.bytes.fill(frame[4]);
        let len = self.bytes.len() as u32;
        self.bytes[..4].copy_from_slice(&len.to_le_bytes());
        self.seen += 1;
        Ok(())
    }
    fn reply(&self) -> &[u8] {
        &self.bytes
    }
}
fn drive(s: &mut Stream, fd: i64, h: &mut Response) -> Result {
    check(
        s.service(&mut NativeEndpoint(fd), h, u64::MAX),
        "stream closed",
    )
}
fn requests(f: &File, n: u8) -> Result {
    let mut bytes = Vec::new();
    for i in 0..n {
        bytes.extend_from_slice(&[7, 0, 0, 0, i, 0, 0]);
    }
    check(
        unsafe { t_write(f.as_raw_fd() as i64, bytes.as_ptr(), bytes.len()) } == bytes.len() as i64,
        "request write",
    )
}
fn read_some(f: &File, out: &mut Vec<u8>, max: usize) -> Result<usize> {
    let mut buf = [0u8; 4096];
    let n = unsafe { t_read(f.as_raw_fd() as i64, buf.as_mut_ptr(), max.min(buf.len())) };
    if n == -11 {
        return Ok(0);
    }
    if n <= 0 {
        return Err("unexpected reply EOF/error");
    }
    out.extend_from_slice(&buf[..n as usize]);
    Ok(n as usize)
}
fn poll(worker: &PollWorker, timeout: i32) -> Result<i64> {
    let mut fd = TPollFd {
        fd: worker.as_raw_fd(),
        events: T_POLLIN,
        revents: 0,
    };
    let n = unsafe { t_poll(&mut fd, 1, timeout) };
    if n < 0 {
        Err("worker poll")
    } else {
        Ok(n)
    }
}
fn native() -> Result {
    let root = file(
        unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) },
        "open srv root",
    )?;
    let name = format!("hi1-wire-{}", unsafe { t_getpid() });
    let listener = file(
        unsafe {
            t_walk_create(
                root.as_raw_fd() as i64,
                name.as_ptr(),
                name.len(),
                T_OREAD,
                T_WALK_CREATE_DMSRVBYTE,
            )
        },
        "post byte service",
    )?;
    let path = format!("/srv/{}", name);
    let mut pairs = Vec::new();
    for _ in 0..2 {
        let client = file(
            unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_ORDWR) },
            "open byte client",
        )?;
        let server = file(
            unsafe { t_srv_accept(listener.as_raw_fd() as i64) },
            "accept client",
        )?;
        nonblock(&client)?;
        nonblock(&server)?;
        pairs.push((client, server));
    }
    let (other_client, other_server) = pairs.pop().unwrap();
    let (client, server) = pairs.pop().unwrap();
    let mut stream = Stream::new();
    let mut reply = Response {
        bytes: vec![0; 32768],
        seen: 0,
    };
    requests(&client, 5)?;
    // Two complete replies fill the 64 KiB default SrvConn ring. The third
    // must remain retained. Each turn has its own bounded work deadline.
    let limit = time::monotonic_ns() + 2_000_000_000;
    while reply.seen < 3 && time::monotonic_ns() < limit {
        drive(&mut stream, server.as_raw_fd() as i64, &mut reply)?;
    }
    check(
        reply.seen == 3 && stream.interest() == Interest::Write,
        "full ring did not retain third reply",
    )?;
    check(!stream.runnable(), "blocked reply is spuriously runnable")?;
    let mut worker = PollWorker::new(2).map_err(|_| "worker start")?;
    check(server.try_clone().is_err(), "srv alias contract changed")?;
    let listener_id = worker
        .register_owned(listener, T_POLLIN)
        .map_err(|_| "own listener")?;
    let blocked = worker
        .register_owned(server, T_POLLOUT)
        .map_err(|_| "watch blocked writer")?;
    check(poll(&worker, 20)? == 0, "full reply ring writable")?;
    // Same UI thread services another real connection while the first client
    // refuses to read. No helper thread can hide a blocking pump here.
    requests(&other_client, 1)?;
    let mut other_stream = Stream::new();
    let mut other_reply = Response {
        bytes: vec![0; 7],
        seen: 0,
    };
    drive(
        &mut other_stream,
        other_server.as_raw_fd() as i64,
        &mut other_reply,
    )?;
    let mut other_out = Vec::new();
    check(
        read_some(&other_client, &mut other_out, 7)? == 7 && other_out == [7, 0, 0, 0, 0, 0, 0],
        "other peer starved",
    )?;
    let mut out = Vec::new();
    check(
        read_some(&client, &mut out, 17)? == 17,
        "open short-write credit",
    )?;
    check(poll(&worker, 1000)? == 1, "drain failed to wake writer")?;
    check(
        worker
            .take_ready()
            .map_err(|_| "take write ready")?
            .iter()
            .any(|r| r.watch == blocked),
        "missing write notice",
    )?;
    worker
        .with_fd(blocked, |fd| drive(&mut stream, fd as i64, &mut reply))
        .map_err(|_| "borrow owned writer")??;
    check(
        reply.seen == 3 && stream.interest() == Interest::Write,
        "short write lost retained reply",
    )?;
    worker
        .rearm(blocked, T_POLLOUT)
        .map_err(|_| "rearm blocked writer")?;
    check(
        poll(&worker, 20)? == 0,
        "short write did not consume exact credit",
    )?;
    let limit = time::monotonic_ns() + 3_000_000_000;
    while out.len() < 5 * 32768 && time::monotonic_ns() < limit {
        read_some(&client, &mut out, 4096)?;
        worker
            .with_fd(blocked, |fd| drive(&mut stream, fd as i64, &mut reply))
            .map_err(|_| "borrow owned writer")??;
    }
    check(
        out.len() == 5 * 32768 && reply.seen == 5,
        "reply drain incomplete or dispatch repeated",
    )?;
    for (i, frame) in out.chunks_exact(32768).enumerate() {
        check(
            frame[..4] == 32768u32.to_le_bytes() && frame[4..].iter().all(|b| *b == i as u8),
            "reply byte loss/reorder",
        )?;
    }
    check(
        !stream.runnable() && stream.interest() == Interest::Read,
        "drained stream still runnable",
    )?;
    worker.remove(blocked).map_err(|_| "remove writer")?;
    check(
        worker.with_fd(blocked, |_| ()).is_err(),
        "retired IO token accepted",
    )?;
    worker.remove(listener_id).map_err(|_| "remove listener")?;
    worker.shutdown().map_err(|_| "join worker")?;
    let mut byte = 0;
    check(
        unsafe { t_read(client.as_raw_fd() as i64, &mut byte, 1) } == 0,
        "owned server not closed at join",
    )?;
    // Handles close through their owners; the service itself is tombstoned
    // only when this poster process exits. No system service is changed.
    Ok(())
}
pub fn run() -> i64 {
    match (|| {
        let mut cmd = process::Command::new("/bin/kaua-term-probe");
        cmd.arg("--service-transport");
        let mut child = cmd.spawn().map_err(|_| "spawn transport child")?;
        check(
            child.wait().map_err(|_| "join transport child")?.success(),
            "transport child exit",
        )?;
        media()
    })() {
        Ok(()) => {
            t_putstr("service-probe: PASS -- real SrvConn stall, peer progress, short write, ordered replies, worker wake, actual media adapter\n");
            0
        }
        Err(e) => {
            t_putstr("service-probe: FAIL -- ");
            t_putstr(e);
            t_putstr("\n");
            1
        }
    }
}

// The real per-user media adapter is compiled from its production source.
// Two child clients use the kernel 9P client, not a fake protocol handler.
struct Children(Vec<Option<process::Child>>);
impl Drop for Children {
    fn drop(&mut self) {
        for c in self.0.iter_mut().flatten() {
            let _ = c.kill();
            let _ = c.wait();
        }
    }
}
fn media() -> Result {
    let user = format!("probe-{}", unsafe { t_getpid() });
    let mut server =
        crate::paneplace::PanePlaceServer::post(&user).map_err(|_| "post actual media adapter")?;
    let notes = notes::Notes::open_self().map_err(|_| "child notifications")?;
    for _wave in 0..2 {
        let mut children = Children(Vec::new());
        for id in 1..=2u32 {
            server.register(id as u128, id);
            let mut cmd = process::Command::new("/bin/kaua-term-probe");
            cmd.arg("--service-media-client")
                .arg(server.place_address(id as u128))
                .arg(format!("{}", id));
            children
                .0
                .push(Some(cmd.spawn().map_err(|_| "spawn actual media client")?));
        }
        let mut seen = [false; 2];
        let deadline = time::monotonic_ns() + 10_000_000_000;
        loop {
            server
                .service()
                .map_err(|_| "actual media readiness failed")?;
            for image in server.take_completed() {
                check((1..=2).contains(&image.leaf), "wrong media leaf")?;
                let i = image.leaf as usize - 1;
                check(
                    !seen[i] && image.id == image.leaf as u128 && image.w == 256 && image.h == 256,
                    "media duplicate/id/dimensions",
                )?;
                check(
                    image.argb.len() == 65536
                        && image.argb.iter().all(|p| *p == 0xff123400 + image.leaf),
                    "actual media bytes differ",
                )?;
                seen[i] = true;
            }
            for slot in &mut children.0 {
                if let Some(c) = slot {
                    if let Some(status) = c.try_wait().map_err(|_| "reap media client")? {
                        *slot = None;
                        check(status.success(), "media client exit")?;
                    }
                }
            }
            if children.0.iter().all(Option::is_none) {
                break;
            }
            let now = time::monotonic_ns();
            check(now < deadline, "actual media adapter deadline")?;
            let mut fds = vec![TPollFd {
                fd: notes.as_raw_fd(),
                events: T_POLLIN,
                revents: 0,
            }];
            server.push_fds(&mut fds);
            check(fds.len() == 2, "media adapter exceeds one UI descriptor")?;
            let timeout = if server.runnable() {
                0
            } else {
                ((deadline - now) / 1_000_000).min(i32::MAX as u64) as i32 + 1
            };
            check(
                unsafe { t_poll(fds.as_mut_ptr(), fds.len(), timeout) } >= 0,
                "media poll",
            )?;
            while notes
                .try_read()
                .map_err(|_| "read child notification")?
                .is_some()
            {}
        }
        check(seen == [true, true], "missing routed image")?;
        // Retire the completed clients through real notifications. Once quiet,
        // the service itself supplies no timer wake; the next wave must reuse its
        // bounded slots, not progressively lose capacity.
        let end = time::monotonic_ns() + 2_000_000_000;
        loop {
            server.service().map_err(|_| "media cleanup readiness")?;
            let mut fds = Vec::new();
            server.push_fds(&mut fds);
            check(fds.len() == 1, "quiet media descriptor count")?;
            let rc = unsafe { t_poll(fds.as_mut_ptr(), fds.len(), 50) };
            check(rc >= 0, "quiet media poll")?;
            if rc == 0 {
                break;
            }
            check(time::monotonic_ns() < end, "quiet media keeps waking")?;
        }
    }
    Ok(())
}
pub fn media_client() -> i64 {
    fn upload() -> Result {
        use libthyla_rs::io::Write;
        let path = env::args().nth(2).ok_or("media path")?;
        let id = env::args().nth(3).ok_or("media id")?;
        let id = core::str::from_utf8(id)
            .map_err(|_| "id utf8")?
            .parse::<u32>()
            .map_err(|_| "id parse")?;
        let path = core::str::from_utf8(path).map_err(|_| "path utf8")?;
        // Match view's actual /srv protocol: open the service root first to
        // instantiate its kernel 9P client, then walk the place path relative
        // to that root. A flat namespace walk cannot cross an unopened service.
        let split = path[5..].find('/').ok_or("service path shape")? + 5;
        let root = file(
            unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), split, T_OREAD) },
            "open media service root",
        )?;
        let sub = &path[split + 1..];
        let mut f = file(
            unsafe { t_open(root.as_raw_fd() as i64, sub.as_ptr(), sub.len(), T_OWRITE) },
            "open media place",
        )?;
        let mut header = inlinewire::PlaceHeader::argb(256, 256);
        header.id = id as u128;
        f.write_all(&header.pack())
            .map_err(|_| "write media header")?;
        let pixel = (0xff123400u32 + id).to_le_bytes();
        let mut chunk = [0u8; 4096];
        for p in chunk.chunks_exact_mut(4) {
            p.copy_from_slice(&pixel);
        }
        for _ in 0..64 {
            f.write_all(&chunk).map_err(|_| "write media pixels")?;
        }
        Ok(())
    }
    match upload() {
        Ok(()) => 0,
        Err(e) => {
            t_putstr("service-probe: media client: ");
            t_putstr(e);
            t_putstr("\n");
            1
        }
    }
}

pub fn transport_child() -> i64 {
    match native() {
        Ok(()) => {
            t_putstr("service-probe: transport checks passed\n");
            0
        }
        Err(e) => {
            t_putstr("service-probe: FAIL -- ");
            t_putstr(e);
            t_putstr("\n");
            1
        }
    }
}
