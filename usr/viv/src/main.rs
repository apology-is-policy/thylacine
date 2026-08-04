// /bin/viv -- the vivarium container runner (VIVARIUM V-7).
//
// The RUNTIME half of the OCI split (docs/VIVARIUM.md section 7.2, the runc
// factoring): `viv run <bundle>` consumes a pre-assembled bundle -- a
// directory holding rootfs/ plus config.json -- assembles the container
// territory, and spawns the entrypoint through the #58 namespace-exec path.
// Image acquisition (`viv pull`) is the separately-owned v1.x sibling; the
// v1.0 bundles are host-baked into the pool at /vivarium/* by tools/build.sh.
//
// The assembly order is forced by capability mechanics and is the part worth
// reading twice:
//
//   1. parse the manifest; spawn the PER-CONTAINER diorama (--vivarium <us>,
//      posting /srv/viv-dio) and mount it over /dio in OUR territory;
//   2. set our own /env to exactly the manifest's set (the child env is the
//      kernel Env CLONE at spawn -- "inherits nothing the manifest does not
//      name");
//   3. pre-open every capability the container world needs as an fd: the
//      rootfs, the /dio/proc + /dio/sys subtrees, the trivial /dev leaves,
//      /env, /net when granted, and the diorama's /proc/<pid>/ctl kill
//      channel -- fds survive chroot, paths do not;
//   4. chroot to the rootfs (viv itself enters the container world; nothing
//      it still needs is path-reachable), then mount the held fds over the
//      rootfs's anchor paths, chdir, and spawn the entrypoint;
//   5. wait by-pid, then kill the diorama through the held ctl fd (its path
//      is gone -- and the container /proc would not show it anyway: the
//      diorama is deliberately not a member of its own view) and reap it.
//
// viv holds NO capability beyond the invoker's: chroot/mount/chdir are
// per-territory ops, the container principal is the invoker's (no
// CAP_SET_IDENTITY anywhere), no hardware allowance is conferred, and the
// only spawn perm it passes on is MAY_POST_SERVICE to its own diorama --
// which is also the one perm viv itself must be spawned with.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

mod json;

use libthyla_rs::{
    t_chdir, t_chroot, t_close, t_fstat, t_getpid, t_mount, t_open, t_pipe, t_poll, t_putstr,
    t_read, t_spawn_full_argv, t_unlink, t_wait_pid_for, t_walk_create, t_write, TPollFd,
    TSpawnArgs, T_MREPL, T_OPATH, T_OREAD, T_OWRITE, T_POLLIN, T_SPAWN_PERM_MAY_POST_SERVICE,
    T_SPAWN_PHENO_LINUX, T_WAIT_WNOHANG, T_WALK_OPEN_FROM_ROOT,
};

// Manifest bounds (fail closed past any of them; the kernel's own spawn/env
// bounds sit behind these, so nothing here relies on downstream rejection).
const CONFIG_MAX: usize = 64 * 1024;
const ARGS_MAX: usize = 64;
const ENV_MAX: usize = 64;
const PATH_MAX: usize = 512;
const ENV_NAME_MAX: usize = 128;
const ENV_VALUE_MAX: usize = 3900;

// The ptyfs slave qid contract (PTS_FLAG | n<<8 | filekind) -- the is-a-pts
// discriminator for the /dev/tty bind, mirrored from usr/ptyfs/src/server.rs.
const PTS_QID_FLAG: u64 = 1 << 40;

struct Manifest {
    root_path: String,
    args: Vec<String>,
    env: Vec<(String, String)>,
    cwd: String,
    net_granted: bool,
    // VIVARIUM section 12.1 rule 1: the CONTAINER declares the phenotype, and
    // this manifest annotation is the only thing in the system that can. The
    // ELF byte is a hint that may never decide (the Q3 resolution).
    pheno_linux: bool,
    sigpipe_selftest: bool,
}

fn say(msg: &str) {
    t_putstr("viv: ");
    t_putstr(msg);
    t_putstr("\n");
}

fn open_from_root(path: &str, omode: u32) -> i64 {
    unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), omode) }
}

/// Read a whole file, bounded. None on open failure, over-bound, or any read
/// error.
fn read_file_bounded(path: &str, cap: usize) -> Option<Vec<u8>> {
    let fd = open_from_root(path, T_OREAD);
    if fd < 0 {
        return None;
    }
    let mut out: Vec<u8> = Vec::new();
    let mut buf = [0u8; 4096];
    loop {
        let n = unsafe { t_read(fd, buf.as_mut_ptr(), buf.len()) };
        if n < 0 {
            let _ = unsafe { t_close(fd) };
            return None;
        }
        if n == 0 {
            break;
        }
        out.extend_from_slice(&buf[..n as usize]);
        if out.len() > cap {
            let _ = unsafe { t_close(fd) };
            return None;
        }
    }
    let _ = unsafe { t_close(fd) };
    Some(out)
}

fn sleep_ms(pipe_rd: i64, ms: i32) {
    let mut pf = TPollFd { fd: pipe_rd as i32, events: T_POLLIN, revents: 0 };
    let _ = unsafe { t_poll(&mut pf as *mut TPollFd, 1, ms) };
}

/// Raw argv spawn -- NOT process::Command, deliberately: Command always endows
/// the parent's fds 0/1/2 on the child, and `viv` itself is routinely FD-LESS
/// (joey spawns its boot daemons with no fds; output rides SYS_PUTS), so the
/// endowment's handle lookup would fail the whole spawn. `with_stdio` is the
/// BORN-WITH-STDIO fact captured at startup (see rs_main) -- not probed here:
/// viv's own transient opens recycle low fd numbers, so a late fstat(0) can
/// see e.g. the diorama ctl fd at slot 0 and mis-endow a half-empty trio
/// (which fails the whole spawn at the kernel's fd bump). caps 0 always.
fn spawn_raw(name: &str, args: &[String], perm_flags: u32, with_stdio: bool,
             pheno_flags: u32, fd0_override: i64) -> i64 {
    let mut argv_buf: Vec<u8> = Vec::new();
    argv_buf.extend_from_slice(name.as_bytes());
    argv_buf.push(0);
    let mut argc: u32 = 1;
    for a in args {
        argv_buf.extend_from_slice(a.as_bytes());
        argv_buf.push(0);
        argc += 1;
    }
    // fd0_override endows exactly ONE fd, landing at the child's fd 0, and
    // takes precedence over stdio: the sigpipe selftest wants a known-dead
    // write end there and nothing else.
    let have_stdio = with_stdio && fd0_override < 0;
    let fd_list: [u32; 3] = [0, 1, 2];
    let fd_one: [u32; 1] = [if fd0_override < 0 { 0 } else { fd0_override as u32 }];
    let (fd_va, fd_n): (u64, u32) = if fd0_override >= 0 {
        (fd_one.as_ptr() as u64, 1)
    } else if have_stdio {
        (fd_list.as_ptr() as u64, 3)
    } else {
        (0, 0)
    };
    let req = TSpawnArgs {
        name_va: name.as_ptr() as u64,
        argv_data_va: argv_buf.as_ptr() as u64,
        fd_list_va: fd_va,
        name_len: name.len() as u32,
        argv_data_len: argv_buf.len() as u32,
        argc,
        fd_count: fd_n,
        perm_flags,
        _pad_envp: 0,
        cap_mask: 0,
        principal_id: 0,
        primary_gid: 0,
        supp_gids_va: 0,
        supp_gid_count: 0,
        identity_flags: 0,
        allowance_va: 0,
        allowance_flags: 0,
        pheno_flags,
    };
    unsafe { t_spawn_full_argv(&req as *const _) }
}

/// Reap `pid` by-pid, returning its exit status (or -1 on a wait error).
fn wait_status(pid: i64) -> i64 {
    let mut st: i32 = 0;
    let got = unsafe { t_wait_pid_for(pid as i32, 0, &mut st as *mut i32) };
    if got != pid {
        return -1;
    }
    st as i64
}

/// Has `pid` already exited? REAPS it when so -- the caller must not reap
/// again (a by-pid wait for a non-child returns -1 rather than blocking, so a
/// double reap is harmless, but writing `kill` to a reaped pid's /proc path is
/// not something to do on purpose).
fn child_exited(pid: i64) -> bool {
    let mut st: i32 = 0;
    unsafe { t_wait_pid_for(pid as i32, T_WAIT_WNOHANG, &mut st as *mut i32) == pid }
}

fn extract_manifest(doc: &json::Json) -> Result<Manifest, &'static str> {
    let root = doc.get("root").ok_or("manifest: no root")?;
    let root_path = root
        .get("path")
        .and_then(|p| p.as_str())
        .ok_or("manifest: no root.path")?;
    if root_path.is_empty() || root_path.len() > PATH_MAX {
        return Err("manifest: root.path bounds");
    }
    // root.readonly is parsed for shape but not acted on at v1.0: there is no
    // read-only bind flag, so the FS permission model (a SYSTEM-owned bake vs
    // a non-SYSTEM invoker) is the enforcement. Documented, not silent.
    let _ = root.get("readonly").map(|v| v.as_bool());

    let process = doc.get("process").ok_or("manifest: no process")?;
    let raw_args = process
        .get("args")
        .and_then(|a| a.as_arr())
        .ok_or("manifest: no process.args")?;
    if raw_args.is_empty() || raw_args.len() > ARGS_MAX {
        return Err("manifest: process.args bounds");
    }
    let mut args: Vec<String> = Vec::new();
    for a in raw_args {
        let s = a.as_str().ok_or("manifest: non-string arg")?;
        if s.is_empty() || s.len() > PATH_MAX {
            return Err("manifest: arg bounds");
        }
        args.push(s.to_string());
    }

    let mut env: Vec<(String, String)> = Vec::new();
    if let Some(raw_env) = process.get("env") {
        let raw_env = raw_env.as_arr().ok_or("manifest: process.env not an array")?;
        if raw_env.len() > ENV_MAX {
            return Err("manifest: process.env bounds");
        }
        for e in raw_env {
            let s = e.as_str().ok_or("manifest: non-string env entry")?;
            let eq = s.find('=').ok_or("manifest: env entry without '='")?;
            let (name, value) = (&s[..eq], &s[eq + 1..]);
            if name.is_empty()
                || name.len() > ENV_NAME_MAX
                || value.len() > ENV_VALUE_MAX
                || !name.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_')
            {
                return Err("manifest: env entry bounds");
            }
            env.push((name.to_string(), value.to_string()));
        }
    }

    let cwd = match process.get("cwd") {
        Some(c) => {
            let s = c.as_str().ok_or("manifest: process.cwd not a string")?;
            if !s.starts_with('/') || s.len() > PATH_MAX {
                return Err("manifest: process.cwd bounds");
            }
            s.to_string()
        }
        None => String::from("/"),
    };

    let net_granted = doc
        .get("annotations")
        .and_then(|a| a.get("org.thylacine.net"))
        .and_then(|v| v.as_str())
        == Some("granted");

    // The phenotype declaration. Absent / anything but "linux" -> native, so a
    // bundle that predates V-1b, or one written by a tool that knows nothing
    // about phenotypes, gets the safe default (section 12.1 rule 3's spirit:
    // never inferred into a non-default ABI, only declared into one).
    let pheno_linux = doc
        .get("annotations")
        .and_then(|a| a.get("org.thylacine.phenotype"))
        .and_then(|v| v.as_str())
        == Some("linux");

    // A BUNDLE-SCOPED test facility (VIVARIUM.md section 6.22): hand the
    // entrypoint fd 0 as the write end of a pipe with NO READER, so its own
    // `write()` makes the kernel post `pipe` -- a SIGPIPE the guest inflicts on
    // ITSELF, synchronously, with no second Proc timing anything. It is the
    // only way a v1.0 Linux guest can cause a catchable signal at all: `kill`
    // and `tkill` are not table rows, and `clone` -- which IS one since LINEAGE
    // L-3d -- admits only the fork and vfork shapes, never CLONE_THREAD. So a
    // guest can reach neither another Proc nor a peer thread of its own (#158;
    // the reason used to be stated as "clone is not a table row", which the
    // clone row's landing quietly falsified without breaking anything).
    //
    // Scoped to the bundle that asks, rather than an argv flag, so every other
    // container's fd endowment is byte-unchanged. It confers no authority --
    // viv is the entrypoint's parent and already holds a kill channel on it,
    // which is strictly stronger than handing it a dead pipe.
    let sigpipe_selftest = doc
        .get("annotations")
        .and_then(|a| a.get("org.thylacine.sigpipe-selftest"))
        .and_then(|v| v.as_str())
        == Some("yes");

    Ok(Manifest {
        root_path: root_path.to_string(),
        args,
        env,
        cwd,
        net_granted,
        pheno_linux,
        sigpipe_selftest,
    })
}

/// Set our own /env to exactly the manifest's set: the entrypoint inherits the
/// kernel Env clone at spawn. Runs pre-chroot (the /env device mount is only
/// path-reachable there) and AFTER the diorama spawn, so only the entrypoint
/// sees the manifest environment.
fn set_own_env(env: &[(String, String)]) -> Result<(), &'static str> {
    let edir = open_from_root("/env", T_OPATH);
    if edir < 0 {
        return Err("open /env");
    }
    // Clear the inherited set. Collect first: unlink-while-enumerating would
    // race the readdir cursor. An unenumerable /env FAILS the run -- carrying
    // the invoker's environment into the container would break "inherits
    // nothing the manifest does not name".
    let mut names: Vec<String> = Vec::new();
    match libthyla_rs::fs::read_dir("/env") {
        Ok(rd) => {
            for ent in rd.flatten() {
                names.push(ent.file_name().to_string());
            }
        }
        Err(_) => {
            let _ = unsafe { t_close(edir) };
            return Err("enumerate /env");
        }
    }
    for n in &names {
        let _ = unsafe { t_unlink(edir, n.as_ptr(), n.len(), 0) };
    }
    for (name, value) in env {
        let v = unsafe { t_walk_create(edir, name.as_ptr(), name.len(), T_OWRITE, 0o644) };
        if v < 0 {
            let _ = unsafe { t_close(edir) };
            return Err("create /env entry");
        }
        let w = unsafe { t_write(v, value.as_ptr(), value.len()) };
        let _ = unsafe { t_close(v) };
        if w < 0 || w as usize != value.len() {
            let _ = unsafe { t_close(edir) };
            return Err("write /env entry");
        }
    }
    let _ = unsafe { t_close(edir) };
    Ok(())
}

/// The pts-slave decode for the /dev/tty bind: when fd 0 is a ptyfs SLAVE,
/// return an O_PATH fd of its /dev/pts/<n> path (the bind source; the OPEN
/// fd 0 itself cannot be a source -- crossing clone-walks the source, and a
/// 9P fid that has been opened cannot be walked). None when stdin is not a
/// pts (the boot gate: console-inherited stdio) -- the manifest table's
/// "omitted when viv has none".
fn tty_bind_source() -> Option<i64> {
    let mut st = [0u8; 88];
    if unsafe { t_fstat(0, st.as_mut_ptr()) } != 0 {
        return None;
    }
    let qid = u64::from_le_bytes(st[8..16].try_into().ok()?);
    if qid & PTS_QID_FLAG == 0 {
        return None;
    }
    let n = (qid >> 8) & 0xFFFF_FFFF;
    let path = format!("/dev/pts/{}", n);
    let fd = open_from_root(&path, T_OPATH);
    if fd < 0 {
        return None;
    }
    Some(fd)
}

/// Kill + reap the per-container diorama through the pre-opened ctl fd (its
/// /proc path is unreachable post-chroot; the ctl_fd < 0 fallback opens the
/// path, which only works pre-chroot). Best-effort by design: a diorama that
/// already died is a wait_pid reap either way.
fn reap_diorama(dio_pid: i64, ctl_fd: i64) {
    if ctl_fd >= 0 {
        let _ = unsafe { t_write(ctl_fd, b"kill".as_ptr(), 4) };
        let _ = unsafe { t_close(ctl_fd) };
    } else {
        let path = format!("/proc/{}/ctl", dio_pid);
        let fd = open_from_root(&path, T_OWRITE);
        if fd >= 0 {
            let _ = unsafe { t_write(fd, b"kill".as_ptr(), 4) };
            let _ = unsafe { t_close(fd) };
        }
    }
    let _ = wait_status(dio_pid);
}

fn run(bundle: &str, stdio_born: bool) -> Result<i64, String> {
    // --- the manifest ------------------------------------------------------
    let cfg_path = format!("{}/config.json", bundle);
    let cfg =
        read_file_bounded(&cfg_path, CONFIG_MAX).ok_or_else(|| format!("read {}", cfg_path))?;
    let doc = json::parse(&cfg).map_err(|e| format!("parse {}: {}", cfg_path, e))?;
    let m = extract_manifest(&doc).map_err(|e| e.to_string())?;

    let rootfs_path = if m.root_path.starts_with('/') {
        m.root_path.clone()
    } else {
        format!("{}/{}", bundle, m.root_path)
    };

    // --- the per-container diorama ----------------------------------------
    let my_pid = unsafe { t_getpid() };
    let dio_args = [String::from("--vivarium"), format!("{}", my_pid)];
    let dio_pid =
        spawn_raw("/bin/diorama", &dio_args, T_SPAWN_PERM_MAY_POST_SERVICE as u32,
                  stdio_born, /*pheno_flags=*/0, /*fd0_override=*/-1);
    if dio_pid <= 0 {
        return Err(String::from("spawn /bin/diorama"));
    }

    // From here on every failure kills + reaps the diorama -- a leaked
    // half-container daemon would hold the fixed /srv/viv-dio name against
    // the next run.
    let (pr, pw) = unsafe { t_pipe() };
    let mut dio_root: i64 = -1;
    let mut dio_died = false;
    if pr >= 0 {
        for _ in 0..50 {
            dio_root = open_from_root("/srv/viv-dio", T_OREAD);
            if dio_root >= 0 {
                break;
            }
            // A dead diorama can never post, so stop the moment our own child
            // is gone instead of burning the whole 5s. #101: the likely reason
            // it is gone is that a concurrent `viv run` already held the fixed
            // name, so its post failed -- worth telling apart from a timeout.
            if child_exited(dio_pid) {
                dio_died = true;
                break;
            }
            sleep_ms(pr, 100);
        }
        let _ = unsafe { t_close(pr) };
        let _ = unsafe { t_close(pw) };
    }
    if dio_root < 0 {
        if dio_died {
            // Already reaped by child_exited -- do NOT reap_diorama again.
            return Err(String::from(
                "the container diorama exited before posting /srv/viv-dio. \
                 Usually that means another `viv run` is already using it: \
                 the name is fixed, so containers cannot run concurrently \
                 yet (a known limit, not a fault in this bundle). \
                 Otherwise check the diorama selftest line on the console.",
            ));
        }
        reap_diorama(dio_pid, -1);
        return Err(String::from("/srv/viv-dio never came up (diorama selftest?)"));
    }

    // The diorama kill channel, pre-opened while its /proc path still
    // resolves. The write is I-26 owner-gated at the write site; viv IS the
    // owner (same principal, and the parent besides). An unopenable ctl on a
    // LIVE diorama is fatal HERE, pre-chroot, where the path-based kill
    // still works -- post-chroot it is the only kill channel, and proceeding
    // without one risks a wait() on an unkillable child.
    let dio_ctl_path = format!("/proc/{}/ctl", dio_pid);
    let dio_ctl = open_from_root(&dio_ctl_path, T_OWRITE);
    if dio_ctl < 0 {
        reap_diorama(dio_pid, -1);
        return Err(String::from("open the diorama ctl channel"));
    }

    macro_rules! fail {
        ($msg:expr) => {{
            reap_diorama(dio_pid, dio_ctl);
            return Err(String::from($msg));
        }};
    }

    // Mount the diorama over our own /dio (the mount point joey creates on
    // the pivoted root for exactly this per-client use) and take O_PATH fds
    // of the two subtrees the container recipe binds.
    if unsafe { t_mount(b"/dio".as_ptr(), 4, dio_root, T_MREPL) } != 0 {
        let _ = unsafe { t_close(dio_root) };
        fail!("mount diorama at /dio");
    }
    let _ = unsafe { t_close(dio_root) };
    let dio_proc = open_from_root("/dio/proc", T_OPATH);
    let dio_sys = open_from_root("/dio/sys", T_OPATH);
    if dio_proc < 0 || dio_sys < 0 {
        fail!("open /dio/{proc,sys}");
    }

    // --- the manifest environment (pre-chroot; see set_own_env) ------------
    if let Err(e) = set_own_env(&m.env) {
        fail!(e);
    }

    // --- pre-open the container world's capabilities -----------------------
    let rootfs = open_from_root(&rootfs_path, T_OPATH);
    if rootfs < 0 {
        fail!("open bundle rootfs");
    }
    // Session path -> container anchor, 1:1 (the recipe's "trivial devdev
    // leaves"; the container never sees cons/consctl or the renderer pair).
    let dev_leaves: [&str; 5] =
        ["/dev/null", "/dev/zero", "/dev/full", "/dev/random", "/dev/urandom"];
    let mut leaf_fds: [i64; 5] = [-1; 5];
    for (i, path) in dev_leaves.iter().enumerate() {
        leaf_fds[i] = open_from_root(path, T_OPATH);
        if leaf_fds[i] < 0 {
            fail!("open a /dev leaf");
        }
    }
    let env_dev = open_from_root("/env", T_OPATH);
    if env_dev < 0 {
        fail!("open /env for the bind");
    }
    let net_root = if m.net_granted {
        let fd = open_from_root("/net", T_OPATH);
        if fd < 0 {
            fail!("manifest grants /net but /net is unreachable");
        }
        fd
    } else {
        -1
    };
    let tty = tty_bind_source();

    // --- enter the container world -----------------------------------------
    if unsafe { t_chroot(rootfs) } != 0 {
        fail!("chroot to the bundle rootfs");
    }
    let _ = unsafe { t_close(rootfs) };

    // The recipe mounts (docs/VIVARIUM.md section 7.2). Every target is a
    // bake-provided anchor inside the rootfs; a missing anchor is a bundle
    // defect and fails the run closed.
    let mut binds: Vec<(&str, i64)> = Vec::new();
    binds.push(("/proc", dio_proc));
    binds.push(("/sys", dio_sys));
    binds.push(("/env", env_dev));
    for (i, path) in dev_leaves.iter().enumerate() {
        binds.push((path, leaf_fds[i]));
    }
    if net_root >= 0 {
        binds.push(("/net", net_root));
    }
    if let Some(t) = tty {
        binds.push(("/dev/tty", t));
    }
    for (path, fd) in &binds {
        if unsafe { t_mount(path.as_ptr(), path.len(), *fd, T_MREPL) } != 0 {
            reap_diorama(dio_pid, dio_ctl);
            return Err(format!("recipe mount {} failed (missing rootfs anchor?)", path));
        }
    }
    for (_, fd) in &binds {
        let _ = unsafe { t_close(*fd) };
    }

    if unsafe { t_chdir(m.cwd.as_ptr(), m.cwd.len()) } != 0 {
        fail!("chdir to process.cwd");
    }

    // --- the entrypoint ----------------------------------------------------
    // The declaration lands HERE and only here: the diorama above is a native
    // Thylacine server that happens to serve a Linux-shaped world, so it spawns
    // native. Only the container's own entrypoint carries the manifest's
    // phenotype -- and its descendants inherit it via rfork (rule 2).
    let pheno = if m.pheno_linux { T_SPAWN_PHENO_LINUX } else { 0 };

    // The sigpipe selftest fd, if the bundle asked for one. The READ end is
    // closed BEFORE the spawn, so the pipe is already reader-less when the
    // child's first write lands -- there is no window in which the write could
    // succeed instead.
    let mut sp_w: i64 = -1;
    if m.sigpipe_selftest {
        let (r, w) = unsafe { t_pipe() };
        if r >= 0 && w >= 0 {
            let _ = unsafe { t_close(r) };
            sp_w = w;
        }
    }
    let child_pid = spawn_raw(&m.args[0], &m.args[1..], 0, stdio_born, pheno, sp_w);
    if sp_w >= 0 {
        let _ = unsafe { t_close(sp_w) };
    }
    if child_pid <= 0 {
        // Name the failure class for the operator: OEXEC (3) runs the same
        // leaf perm gate + Dev open the spawn-time resolve runs, so a failed
        // spawn with a passing OEXEC open points past resolution.
        let oexec = open_from_root(&m.args[0], 3);
        if oexec >= 0 {
            let _ = unsafe { t_close(oexec) };
        }
        reap_diorama(dio_pid, dio_ctl);
        return Err(format!(
            "spawn the entrypoint rc={} ({}: oexec-open {})",
            child_pid,
            m.args[0],
            if oexec >= 0 { "passes" } else { "fails" }
        ));
    }
    let status = wait_status(child_pid);

    reap_diorama(dio_pid, dio_ctl);

    if status < 0 {
        return Err(String::from("wait for the entrypoint"));
    }
    // A NON-ZERO container is the case an operator has to diagnose, and until
    // now `viv` reported it by exiting with the same code and saying nothing --
    // so "the container failed" and "viv failed" looked identical from outside,
    // and neither said whether the container even had somewhere to write.
    //
    // stdio_born is the specific fact worth naming: it is inherited from how
    // the CALLER spawned viv (a live 0/1/2 trio or not), it silently decides
    // whether the container gets any output at all, and when it is false a
    // perfectly healthy container looks like one that never ran.
    if status != 0 {
        say(&format!(
            "container exited {} (stdio_born={} pheno={} entry={} pid={})",
            status, stdio_born, pheno, m.args[0], child_pid
        ));
    }
    Ok(status)
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // Captured before ANY open: was viv BORN with stdio (fds 0/1/2 all live)?
    // The interactive case inherits a real trio; a joey-spawned boot viv is
    // fd-less. Probed later this would be corrupted by viv's own transient
    // opens recycling low fd numbers.
    let stdio_born = {
        let mut st = [0u8; 88];
        (0..3).all(|fd| unsafe { t_fstat(fd, st.as_mut_ptr()) } == 0)
    };

    if let Err(e) = json::selftest() {
        say("json selftest FAIL:");
        say(e);
        return 1;
    }

    let args = libthyla_rs::env::args();
    let (cmd, bundle) = (args.get_str(1), args.get_str(2));
    if cmd != Some("run") || bundle.is_none() || args.len() != 3 {
        say("usage: viv run <bundle-dir>");
        return 2;
    }
    let bundle = bundle.unwrap();
    if !bundle.starts_with('/') || bundle.len() > PATH_MAX {
        say("bundle path must be absolute");
        return 2;
    }

    match run(bundle, stdio_born) {
        Ok(code) => code,
        Err(e) => {
            say(&e);
            1
        }
    }
}
