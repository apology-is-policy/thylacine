//! The clipboard over the private ring (ASYNC-SERVICE-LIFECYCLE, AS-4c): the system
//! calls under interaction_native::qualification, driven by
//! tools/interactive/ls-halcyon-clipboard-async.exp.
use crate::clipboard_client::{current, Result};
use ::alloc::{format, string::String, vec::Vec};
use interaction_native::{
    parse_locator,
    qualification::{self, Moment, Stepping},
    Host,
};
use libthyla_rs::{env, fs::File, handle::Rights, loom::ThylaKernel, *};

const EOPNOTSUPP: i32 = -95;

fn registry() -> Result<File> {
    let path = b"/srv";
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_OPATH) };
    if fd < 0 {
        t_putstr(&format!("clipboard-probe: /srv errno {}\n", fd));
        return Err("the registry");
    }
    Ok(unsafe { File::from_raw_fd(fd as i32, Rights::READ) })
}

fn poll(fd: i32, timeout_ms: i32) {
    let mut p = TPollFd {
        fd,
        events: T_POLLIN,
        revents: 0,
    };
    // What a failed poll means, the pump after it learns from the ring.
    let _ = unsafe { t_poll(&mut p, 1, timeout_ms) };
}

/// Text no other run in a boot copies, so a run can tell its value from the last one's.
fn text(name: &str) -> Vec<u8> {
    let mut bytes = name.as_bytes().to_vec();
    bytes.push(b' ');
    bytes.extend((0..20000).map(|i| b'a' + (i % 26) as u8));
    bytes
}

/// The key that ends the hold reaches this process only after the trusted episode.
fn hold(name: String) -> impl Fn() {
    move || {
        t_putstr(&format!("clipboard-probe: SAK READY {}\n", name));
        let mut key = [0u8; 1];
        if unsafe { t_read(0, key.as_mut_ptr(), 1) } <= 0 {
            t_putstr(&format!("clipboard-probe: FAIL {} -- hold input\n", name));
            unsafe { t_exits(1) }
        }
    }
}

fn round_trip(at: Option<(u32, Moment)>, name: &str) -> Result<qualification::Report> {
    let before = current()?;
    let srv = registry()?;
    let value = env::var("HALCYON_INTERACTION").ok_or("no interaction locator")?;
    let locator = parse_locator(value.as_bytes()).ok_or("locator shape")?;
    let (kernel, progress) = Stepping::new(ThylaKernel, at, hold(String::from(name)));
    let host = match Host::new(kernel, srv.as_raw_fd(), locator) {
        Ok(host) => host,
        Err(EOPNOTSUPP) => {
            t_putstr("clipboard-probe: async UNAVAILABLE\n");
            return Err("no private ring in this image");
        }
        Err(errno) => {
            t_putstr(&format!("clipboard-probe: setup errno {}\n", errno));
            return Err("the private ring");
        }
    };
    let report = qualification::run(host, progress, poll, &before, text(name))?;
    // The ring has closed with its host, and the other path must agree.
    if current()? != report.value {
        return Err("the two paths read different values");
    }
    Ok(report)
}

pub fn test(mode: &str) -> Result<()> {
    if mode == "async-copy" {
        let report = round_trip(None, "async-copy")?;
        t_putstr(&format!(
            "clipboard-probe: async-copy OK submitted={}\n",
            report.submitted
        ));
        return Ok(());
    }
    if mode != "async-sak" {
        return Err("mode");
    }
    let moment = env::args().nth(3).unwrap_or(b"");
    let (moment, word) = match moment {
        b"before" => (Moment::Before, "before"),
        b"after" => (Moment::After, "after"),
        _ => return Err("moment"),
    };
    let at = env::args().nth(4).unwrap_or(b"");
    let at: u32 = core::str::from_utf8(at)
        .ok()
        .and_then(|n| n.parse().ok())
        .filter(|n| *n > 0)
        .ok_or("operation number")?;
    let name = format!("{}-{}", word, at);
    let report = round_trip(Some((at, moment)), &name)?;
    if report.held {
        t_putstr(&format!(
            "clipboard-probe: SAK DONE {} bound={} down={} copy={:?} lost={}\n",
            name, report.bound_at_hold as u8, report.down_after as u8, report.copy, report.lost
        ));
    } else {
        t_putstr(&format!(
            "clipboard-probe: SAK END {} submitted={}\n",
            name, report.submitted
        ));
    }
    Ok(())
}
