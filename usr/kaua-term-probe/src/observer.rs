//! Positive EL0 ownership/notification proof against real ptyfs and Tapestry.

use libthyla_rs::err::Error;
use libthyla_rs::fs::File;
use libthyla_rs::handle::Rights;
use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};
use libthyla_rs::process::Command;
use libthyla_rs::pty_observer::BindingId;
use libthyla_rs::{t_putstr, t_set_dumpable, t_set_traceable, T_SPAWN_PERM_SEAL};

type Result = core::result::Result<(), &'static str>;

fn require(ok: bool, message: &'static str) -> Result {
    if ok {
        Ok(())
    } else {
        Err(message)
    }
}

fn child(argument: &str, sealed: bool) -> Result {
    let mut command = Command::new("/bin/kaua-term-probe");
    command.arg(argument);
    if sealed {
        command.perm(T_SPAWN_PERM_SEAL);
    }
    let mut child = command.spawn().map_err(|_| "spawn")?;
    require(child.wait().map_err(|_| "wait")?.success(), "child exit")
}

struct Revoke(BindingId);
impl Drop for Revoke {
    fn drop(&mut self) {
        let _ = self.0.unbind();
    }
}

fn native() -> Result {
    // These reads cannot create a seal: neither zero-setting syscall is used.
    require(unsafe { t_set_traceable(1) } < 0, "host trace seal absent")?;
    require(unsafe { t_set_dumpable(1) } < 0, "host dump seal absent")?;
    child("--observer-unsealed-child", false)?;

    let master = ptyhold::Master::mint().map_err(|_| "mint actual pts")?;
    // Master has no Drop; this File becomes the sole owner of its raw descriptor.
    let master = unsafe { File::from_raw_fd(master.mfd as i32, Rights::READ | Rights::WRITE) };
    let peer = File::open("/srv/tapestry").map_err(|_| "open actual observer")?;
    let binding = BindingId::bind(master.as_raw_fd(), peer.as_raw_fd()).map_err(|_| "BIND")?;
    let _revoke = Revoke(binding);
    drop(peer);
    let state = binding.state().map_err(|_| "STATE copyout")?;
    require(
        state.version == 1 && state.reserved == 0 && state.flags == 1,
        "STATE header",
    )?;
    require(
        state.binding_id == binding.locator()
            && state.pts_id != 0
            && state.foreground_epoch == 1
            && state.revision == 1
            && state.acknowledged_epoch == 0
            && state.subject_stripes == 0
            && state.binder_pid == unsafe { libthyla_rs::t_getpid() } as u32,
        "STATE identity or epoch",
    )?;
    require(
        binding.acknowledge(state.foreground_epoch, 0) == Err(Error::PermissionDenied),
        "binder cannot ACK",
    )?;
    require(
        binding.check(state.foreground_epoch, state.binder_stripes) == Err(Error::PermissionDenied),
        "binder cannot CHECK",
    )?;

    let mut watch = binding.watch().map_err(|_| "WATCH")?;
    require(
        binding.watch().err() == Some(Error::Busy),
        "duplicate WATCH refused",
    )?;
    let mut poll = PollSet::new();
    poll.add(&watch, PollEvents::READ);
    require(
        poll.poll(PollTimeout::Zero)
            .map_err(|_| "poll initial")?
            .count()
            == 1,
        "initial readiness",
    )?;
    require(
        watch.read().map_err(|_| "watch initial read")? == Some(state),
        "watch and STATE records agree",
    )?;
    require(
        watch.read() == Err(Error::WouldBlock),
        "unchanged watch would block",
    )?;
    require(
        poll.poll(PollTimeout::Zero)
            .map_err(|_| "poll consumed")?
            .count()
            == 0,
        "consumed watch not ready",
    )?;
    drop(poll);
    drop(watch);
    let mut watch = binding.watch().map_err(|_| "WATCH reopens after Drop")?;
    binding.unbind().map_err(|_| "UNBIND")?;
    let mut poll = PollSet::new();
    poll.add(&watch, PollEvents::READ);
    let mut ready = poll.poll(PollTimeout::Zero).map_err(|_| "poll retired")?;
    require(
        ready.next().is_some_and(|event| event.is_hup()),
        "retirement HUP",
    )?;
    require(
        watch.read().map_err(|_| "read retired")?.is_none(),
        "retirement EOF",
    )?;
    require(
        binding.state() == Err(Error::NotFound),
        "retired STATE refused",
    )?;
    Ok(())
}

pub fn run(argument: &[u8]) -> Option<i64> {
    let result = match argument {
        b"--observer" => child("--observer-sealed-child", true),
        b"--observer-sealed-child" => native(),
        b"--observer-unsealed-child" => require(
            unsafe { t_set_traceable(1) } == 0 && unsafe { t_set_dumpable(1) } == 0,
            "ordinary child inherited a seal",
        ),
        _ => return None,
    };
    Some(match result {
        Ok(()) => {
            if argument == b"--observer" {
                t_putstr("observer-probe: PASS -- native records, watch readiness, retirement, spawn seals\n");
            }
            0
        }
        Err(message) => {
            t_putstr("observer-probe: FAIL -- ");
            t_putstr(message);
            t_putstr("\n");
            1
        }
    })
}
