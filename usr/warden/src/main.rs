// /warden -- the Menagerie hardware broker (MENAGERIE.md sections 3-6), build-arc
// steps 5c + 5d. The warden is the TCB component that turns the discovery sources
// into capability-sandboxed driver Procs: it enumerates device nodes (the DTB
// source over /hw + the virtio-mmio bus source), matches each node's typed
// identity against a driver-manifest database, intersects the node's resources
// with the matched manifest's needs to compute the narrowed allowance (the
// auditable I-34 grant), and spawns the driver with exactly that allowance plus a
// descriptor of what it was granted.
//
// The warden binds on a node's IDENTITY, never the transport, and NEVER reads a
// device register itself (MENAGERIE section 3): a bus whose device type is only
// knowable at runtime is enumerated by ITS source. 5d-2 adds the virtio-mmio bus
// source -- a separate, capability-sandboxed Proc the warden spawns granted only
// the virtio-mmio bank; it reads each slot's DeviceID and reports typed
// `virtio:<id>` nodes back over a pipe, so the DeviceID-poke stays out of the TCB.
//
// What is not yet built: long-lived supervision + restart + DeviceRemoved revoke
// (5e); the netdev bind (5d-3 adds the `virtio:1` manifest + the grant-driven
// driver -- so a discovered virtio node is logged-but-unbound at 5d-2); the other
// bus sources (PCIe/USB) + a manifest-file database. v1.0 compiles the bind DB in.
//
// The warden holds CAP_HW_CREATE + the broad allowance (joey spawns it without a
// narrowing), so it can confer a narrowed allowance + CAP_HW_CREATE on each driver
// AND on the bus source. Runs in joey's THYLA_BOOT_PROBES ladder, PRE-pivot (so
// the driver + source binaries resolve in devramfs by absolute path). Exit 0 =
// every bound driver came up (or nothing matched); exit 1 = a bound driver failed.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::{String, ToString};
use alloc::vec;
use alloc::vec::Vec;

use libthyla_rs::fs::{File, OpenOptions};
use libthyla_rs::io::{slurp_capped, Read};
use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};
use libthyla_rs::process::{Child, Command, ExitStatus, Stdio};
use libthyla_rs::T_CAP_HW_CREATE;

use libdriver::driver::to_allowance;
use libdriver::{
    best_match, reconcile_reported_node, resolve, BoundResources, DeviceId, DeviceNode,
    DiscoverySource, DtbSource, Manifest,
};

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

/// Console-direct diagnostics (T_SYS_PUTS), visible in the boot log regardless of
/// fd wiring -- a boot probe loses fd-2 (eprintln) output.
macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

/// The compiled-in bind database (v1.0). Each entry is a section-6 manifest; the
/// driver's binary is `<manifest.name>` (resolved in the namespace by name). The
/// pl061 GPIO -> `menagerie-probe` proves the I-34 grant on a trivial device; the
/// `virtio:1` -> `netdev-driver` bind (5d-3) is the first useful driver, bound
/// through the virtio-mmio bus source's typed identity. v1.x reads
/// `/lib/driver/*.manifest`.
const BUILTIN_MANIFESTS: &[&str] = &[
    r#"
driver "menagerie-probe" {
    abi   = 1
    binds = ["arm,pl061"]
    needs {
        mmio = "node:reg"
        irq  = "node:interrupts"
        dma  = "pool: 64 KiB"
    }
    serves  = "/dev/gpio/%instance"
    restart = on-crash
}
"#,
    r#"
driver "netdev-driver" {
    abi   = 1
    binds = ["virtio:1"]
    needs {
        mmio = "node:reg"
        irq  = "node:interrupts"
        dma  = "pool: 64 KiB"
    }
    serves  = "/dev/net/%instance"
    restart = on-crash
}
"#,
];

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // Parse the built-in bind database. A malformed built-in is a build bug, not
    // a runtime condition -- fail loud.
    let mut db: Vec<Manifest> = Vec::with_capacity(BUILTIN_MANIFESTS.len());
    for text in BUILTIN_MANIFESTS {
        match Manifest::parse(text) {
            Ok(m) => db.push(m),
            Err(e) => {
                say!("warden: FAIL -- built-in manifest parse error {:?}", e);
                return 1;
            }
        }
    }

    // Discovery. The DTB source publishes the static fabric (MENAGERIE section 7);
    // the virtio-mmio bus source enumerates the bank and re-emits typed virtio:<id>
    // nodes. The warden suppresses the raw `virtio,mmio` transport nodes (claimed
    // by the source) and binds the typed children by id.
    let dtb_nodes = DtbSource::new().enumerate();
    say!("warden: /hw discovered {} device nodes", dtb_nodes.len());

    let mut discovered: Vec<DeviceNode> = Vec::new();
    let mut virtio_slots: Vec<&DeviceNode> = Vec::new();
    for n in &dtb_nodes {
        if is_virtio_mmio(n) {
            virtio_slots.push(n);
        } else {
            discovered.push(n.clone());
        }
    }
    if !virtio_slots.is_empty() {
        match bank_window(&virtio_slots) {
            Some(bank) => {
                say!(
                    "warden: virtio-mmio bank {:#x}/{:#x} ({} slots) -> spawning bus source",
                    bank.0,
                    bank.1,
                    virtio_slots.len()
                );
                // The warden's OWN trusted view of the slots -- the source supplies
                // identity, the warden supplies resources (reconcile_reported_node).
                let trusted: Vec<DeviceNode> =
                    virtio_slots.iter().map(|n| (*n).clone()).collect();
                let typed = run_virtio_mmio_source(bank, &trusted);
                say!(
                    "warden: virtio-mmio source reported {} typed node(s)",
                    typed.len()
                );
                for n in &typed {
                    let id = n.ids.first().map(|i| i.as_string()).unwrap_or_default();
                    say!("warden: discovered {} ({})", id, n.label);
                }
                discovered.extend(typed);
            }
            None => say!("warden: virtio-mmio slots present but no reg window; skipping source"),
        }
    }

    // A per-manifest instance counter so each bound device of a driver gets a
    // distinct %instance in its served path.
    let mut instance = vec![0u32; db.len()];
    let mut bound = 0u32;
    let mut up = 0u32;

    for node in &discovered {
        let Some(idx) = best_match(&db, node) else {
            continue;
        };
        let inst = instance[idx];
        let grant = match resolve(&db[idx], node, inst) {
            Ok(g) => g,
            Err(e) => {
                say!(
                    "warden: resolve {} ({}) failed {:?}",
                    db[idx].name,
                    node.label,
                    e
                );
                continue;
            }
        };
        instance[idx] += 1;
        bound += 1;
        if bind_and_run(&db[idx], &grant, &node.label) {
            up += 1;
        }
    }

    say!("warden: {} bound, {} up", bound, up);
    if up != bound {
        1
    } else {
        0
    }
}

const PAGE: u64 = 0x1000;
const VIRTIO_MMIO_COMPATIBLE: &str = "virtio,mmio";
const VIRTIO_MMIO_SOURCE_BIN: &str = "/virtio-mmio-source";

/// Is this a raw virtio-mmio transport node (a `virtio,mmio` DTB slot)? Such nodes
/// are claimed by the virtio-mmio bus source, which re-emits typed `virtio:<id>`
/// children, so the warden suppresses the raw nodes from its bind set.
fn is_virtio_mmio(node: &DeviceNode) -> bool {
    node.ids
        .iter()
        .any(|id| matches!(id, DeviceId::Compatible(c) if c == VIRTIO_MMIO_COMPATIBLE))
}

/// The page-aligned MMIO window spanning every virtio-mmio slot -- the bank the
/// source is granted (one window: the ~32 slots cannot fit as separate allowance
/// windows, and the source needs all of them to read each slot's DeviceID).
/// Returns `None` if no slot exposes a reg window.
fn bank_window(slots: &[&DeviceNode]) -> Option<(u64, u64)> {
    let mut lo = u64::MAX;
    let mut hi = 0u64;
    for s in slots {
        for (base, size) in &s.resources.reg {
            lo = lo.min(*base);
            hi = hi.max(base.saturating_add(*size));
        }
    }
    if lo == u64::MAX || hi <= lo {
        return None;
    }
    let lo = lo & !(PAGE - 1); // round the base down to a page
    let hi = hi.saturating_add(PAGE - 1) & !(PAGE - 1); // round the end up to a page
    Some((lo, hi - lo))
}

/// Spawn the virtio-mmio bus source granted ONLY the bank, read its typed
/// `DeviceNode` records off the pipe (bounded, to EOF), reap it, reconcile each
/// reported node against the warden's `trusted` DTB view, and return the typed
/// nodes. The source -- not the warden -- reads the slot DeviceID registers, so
/// the warden never touches a device register (MENAGERIE section 3); but the
/// source is non-TCB, so the warden does NOT trust its reported resources: it uses
/// the source only for the slot identity and rebuilds reg/INTID from its own
/// trusted view (`reconcile_reported_node`), so a source can never fabricate a
/// resource or inflate a driver's allowance. A source failure is non-fatal: the
/// warden returns an empty set and binds whatever else it found.
fn run_virtio_mmio_source(bank: (u64, u64), trusted: &[DeviceNode]) -> Vec<DeviceNode> {
    let mut out_nodes = Vec::new();

    // The source's grant: the bank MMIO window, no IRQ, no DMA. Both the kernel
    // allowance (the authority) and the argv descriptor (the information) come from
    // this one BoundResources, so they cannot drift -- the driver-bind discipline.
    let source_grant = BoundResources {
        instance: 0,
        compatible: "virtio-mmio-bank".to_string(),
        serves: String::new(),
        mmio: vec![bank],
        irq: Vec::new(),
        dma_max: 0,
    };
    let desc = match source_grant.to_descriptor() {
        Ok(d) => d,
        Err(e) => {
            say!("warden: virtio-mmio-source descriptor encode failed {:?}", e);
            return out_nodes;
        }
    };
    let allow = to_allowance(&source_grant);

    let mut cmd = Command::new(VIRTIO_MMIO_SOURCE_BIN);
    cmd.arg(desc).caps(T_CAP_HW_CREATE).allowance(allow);
    // The source has no stdio of its own (the boot-probe-no-fds gap; see
    // bind_and_run): /dev/null for stdin + stderr, a PIPE for stdout -- the warden
    // reads its node records off the pipe.
    let open_null = || OpenOptions::new().read(true).write(true).open("/dev/null");
    let (Ok(nin), Ok(nerr)) = (open_null(), open_null()) else {
        say!("warden: /dev/null open failed; skipping virtio-mmio source");
        return out_nodes;
    };
    cmd.stdin(Stdio::File(nin))
        .stdout(Stdio::Piped)
        .stderr(Stdio::File(nerr));
    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => {
            say!("warden: spawn virtio-mmio-source failed {:?}", e);
            return out_nodes;
        }
    };

    // Read the records to EOF (the source closes stdout on exit) BEFORE reaping --
    // the bounded read drains the pipe concurrently, so the small (<= ~32 records)
    // output never deadlocks on a full pipe; the 64 KiB cap stops a runaway/hostile
    // source from OOMing the warden (the TCB) -- the source is non-TCB.
    let buf = match child.stdout.take() {
        Some(mut so) => slurp_capped(&mut so, 64 * 1024).unwrap_or_default(),
        None => Vec::new(),
    };
    let _ = child.wait();

    // Parse each newline-delimited record into a typed DeviceNode. parse_record is
    // strict + bounds the resource counts -- the discovery-source trust boundary.
    match core::str::from_utf8(&buf) {
        Ok(text) => {
            for line in text.lines() {
                if line.is_empty() {
                    continue;
                }
                match DeviceNode::parse_record(line) {
                    Ok(n) => match reconcile_reported_node(&n, trusted) {
                        Some(node) => out_nodes.push(node),
                        None => say!(
                            "warden: virtio-mmio-source reported out-of-domain node {:?}; rejected",
                            n.ids.first().map(|i| i.as_string())
                        ),
                    },
                    Err(e) => say!("warden: bad virtio-mmio-source record {:?} ({})", e, line),
                }
            }
        }
        Err(_) => say!("warden: virtio-mmio-source emitted non-UTF8 records"),
    }
    out_nodes
}

/// The maximum length of a driver's readiness line (`READY`); a longer line
/// without a newline is a garbled/hostile signal -- the driver is non-TCB.
const READY_LINE_MAX: usize = 64;

/// Read one newline-terminated readiness line off a driver's stdout pipe.
/// Blocks until '\n', EOF, or the cap. Returns the line (without the trailing
/// '\n') on success; `None` on EOF before a newline (the driver exited without
/// signalling) or an over-long line. Reads byte-by-byte so it stops exactly at
/// the newline and never blocks waiting past it -- a long-lived driver keeps
/// its stdout open after the line.
fn read_ready_line(f: &mut File) -> Option<String> {
    let mut buf: Vec<u8> = Vec::new();
    let mut b = [0u8; 1];
    loop {
        match f.read(&mut b) {
            Ok(0) => return None, // EOF before a newline
            Ok(_) => {
                if b[0] == b'\n' {
                    return core::str::from_utf8(&buf).ok().map(|s| s.to_string());
                }
                if buf.len() >= READY_LINE_MAX {
                    return None; // over-long -> garbled
                }
                buf.push(b[0]);
            }
            Err(_) => return None, // read error
        }
    }
}

/// The outcome of waiting for a freshly-spawned driver to declare itself.
enum Readiness {
    /// The driver wrote a readiness line ("READY" = up) and is still alive.
    Signalled(String),
    /// The driver exited on its own (a one-shot proof, or a bring-up crash) and
    /// has been reaped; its status is attached.
    Exited(ExitStatus),
    /// The driver neither signalled nor exited within the bound -- misbehaving.
    Timeout,
    /// The child could not be tracked for readiness (no stdout pipe, or it is
    /// no longer ours).
    Untracked,
}

/// Milliseconds per readiness poll -- the cadence at which the warden re-checks
/// whether a still-silent driver has exited (the pipe cannot report that; see
/// `await_readiness`).
const READY_POLL_MS: u32 = 100;
/// Readiness give-up bound (`READY_POLL_MS * READY_WAIT_TRIES` total, ~10s). A
/// driver that neither signals nor exits within this is torn down.
const READY_WAIT_TRIES: u32 = 100;

/// Wait for a freshly-spawned driver to either signal readiness on its stdout
/// pipe ("READY") or exit.
///
/// A driver's stdout pipe does NOT EOF when the driver exits: a libdriver
/// driver exits via SYS_EXIT_GROUP, and a single-thread Proc defers its
/// handle-table close -- including the pipe write end -- to REAP, not exit (the
/// #926 asymmetry). So the warden cannot block reading the pipe to detect an
/// exit; it would deadlock (it holds the only reader and cannot reap while
/// blocked in the read). Instead it detects an exit with `try_wait` (off the
/// pipe) and polls the pipe only for the readiness DATA, bounded by a give-up.
fn await_readiness(child: &mut Child) -> Readiness {
    let Some(mut pipe) = child.stdout.take() else {
        return Readiness::Untracked;
    };
    let mut ps = PollSet::with_capacity(1);
    ps.add(&pipe, PollEvents::READ);
    for _ in 0..READY_WAIT_TRIES {
        // Catch an exit independent of the pipe (it will not EOF until reap).
        match child.try_wait() {
            Ok(Some(status)) => return Readiness::Exited(status),
            Ok(None) => {}
            Err(_) => return Readiness::Untracked,
        }
        // Poll the pipe for the readiness line (data), bounded.
        let mut readable = false;
        if let Ok(results) = ps.poll(PollTimeout::Millis(READY_POLL_MS)) {
            for ev in results {
                if ev.fd == pipe.as_raw_fd() && (ev.is_readable() || ev.is_hup()) {
                    readable = true;
                }
            }
        }
        if readable {
            // The driver writes "READY\n" atomically (one small write_all, well
            // under PIPE_BUF), so a readable pipe carries the whole line --
            // read_ready_line stops at the '\n' and does not block.
            if let Some(line) = read_ready_line(&mut pipe) {
                return Readiness::Signalled(line);
            }
            // EOF / garbled: keep looping; try_wait catches the eventual exit.
        }
    }
    Readiness::Timeout
}

/// Spawn the driver for a computed grant -- the confer step. Encodes the grant
/// into the argv descriptor + the kernel allowance (both from one
/// `BoundResources`), spawns the driver narrowed, then reads its readiness
/// line: a long-lived service (`READY`) is brought up and then torn down via
/// DeviceRemoved (revoke + group-terminate) to prove the teardown lifecycle; a
/// one-shot proof (EOF, no line) is reaped for its exit code. Returns whether
/// the driver came up cleanly.
fn bind_and_run(m: &Manifest, grant: &BoundResources, node_name: &str) -> bool {
    let desc = match grant.to_descriptor() {
        Ok(d) => d,
        Err(e) => {
            say!("warden: descriptor encode for {} failed {:?}", m.name, e);
            return false;
        }
    };
    let allow = to_allowance(grant);
    say!(
        "warden: bind {} ({}) -> {} inst={} [mmio={} irq={} dma={:#x}]",
        grant.compatible,
        node_name,
        m.name,
        grant.instance,
        grant.mmio.len(),
        grant.irq.len(),
        grant.dma_max
    );

    // The boot-probe warden runs PRE-pivot, where the driver binaries live at the
    // devramfs root; spawn by ABSOLUTE path (resolved from root_spoor, the same
    // base the warden's /hw reads use) rather than by bare name, so resolution
    // does not depend on the per-Proc cwd. The post-pivot warden (5e) resolves
    // /bin/<name>.
    let bin = alloc::format!("/{}", m.name);

    let mut cmd = Command::new(bin.as_str());
    cmd.arg(desc).caps(T_CAP_HW_CREATE).allowance(allow);
    // The boot-probe warden has no stdio fds of its own (joey spawns it without
    // any), so Command's default Stdio::Inherit -- which bumps the parent's fd
    // 0/1/2 -- would fail before the kernel even resolves the binary. stdin +
    // stderr are /dev/null; stdout is a PIPE: a driver writes one readiness line
    // ("READY") to it once it is up (the section-5 "serves its file" readiness
    // analog), so the warden can tell a long-lived service from a one-shot
    // proof. All other driver diagnostics go to the console (t_putstr), not fds.
    let open_null = || OpenOptions::new().read(true).write(true).open("/dev/null");
    let (Ok(nin), Ok(nerr)) = (open_null(), open_null()) else {
        say!("warden: /dev/null open failed; cannot spawn {}", m.name);
        return false;
    };
    cmd.stdin(Stdio::File(nin))
        .stdout(Stdio::Piped)
        .stderr(Stdio::File(nerr));

    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => {
            say!("warden: spawn {} failed {:?}", m.name, e);
            return false;
        }
    };
    let pid = child.pid();

    // Wait for the driver to declare itself: "READY" (a long-lived service still
    // holding its device) or an exit (a one-shot proof, or a bring-up crash).
    // We must NOT block reading the pipe to EOF -- a driver's fds close at reap,
    // not exit (#926), so a silent exit would deadlock the reader.
    // await_readiness detects the exit via try_wait and polls only for the data.
    match await_readiness(&mut child) {
        Readiness::Exited(status) => {
            // One-shot proof completed (or a bring-up crash); already reaped.
            say!(
                "warden: {} pid={} exited code={:?}",
                m.name,
                pid,
                status.code()
            );
            status.success()
        }
        Readiness::Signalled(line) => {
            // A long-lived service still holding its device. Demonstrate
            // DeviceRemoved: a forced group-terminate that revokes the allowance
            // FIRST (atomic, #160) then cascades the death-wake. The driver,
            // blocked in a death-interruptible wait, unwinds cleanly; the reap
            // frees the slot's exclusive MMIO/IRQ/DMA claims (so a later
            // claimant -- stratumd's virtio-blk post-pivot -- finds the bank
            // free). Only "READY" is a clean bring-up; any other line is a
            // misbehaving driver, still torn down.
            let up = line == "READY";
            if up {
                say!(
                    "warden: {} pid={} up (READY) -> DeviceRemoved (revoke + terminate)",
                    m.name,
                    pid
                );
            } else {
                say!(
                    "warden: {} pid={} signalled {:?} (expected READY) -> terminating",
                    m.name,
                    pid,
                    line
                );
            }
            let _ = child.kill();
            match child.wait() {
                Ok(status) => {
                    say!(
                        "warden: {} pid={} torn down (status={:?})",
                        m.name,
                        pid,
                        status.code()
                    );
                    up
                }
                Err(e) => {
                    say!(
                        "warden: reap {} (pid={}) after teardown failed {:?}",
                        m.name,
                        pid,
                        e
                    );
                    false
                }
            }
        }
        Readiness::Timeout => {
            say!(
                "warden: {} pid={} gave no readiness/exit signal -> terminating",
                m.name,
                pid
            );
            let _ = child.kill();
            let _ = child.wait();
            false
        }
        Readiness::Untracked => {
            // Unreachable while every driver is spawned with a piped stdout, but
            // kill-then-reap defensively: a bare wait() on a long-lived driver
            // we cannot observe would block the warden forever.
            say!(
                "warden: {} pid={} could not be tracked for readiness -> terminating",
                m.name,
                pid
            );
            let _ = child.kill();
            let _ = child.wait();
            false
        }
    }
}
