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

use libthyla_rs::fs::OpenOptions;
use libthyla_rs::io::slurp_capped;
use libthyla_rs::process::{Command, Stdio};
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

/// Spawn the driver for a computed grant -- the confer step. Encodes the grant
/// into the argv descriptor + the kernel allowance (both from one
/// `BoundResources`), spawns the driver narrowed, and (5c) reaps it as a
/// one-shot, returning whether it came up cleanly. 5e makes a long-lived driver
/// supervised rather than reaped.
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
    // 0/1/2 -- would fail before the kernel even resolves the binary. The driver
    // logs via the console-direct path, not fds; hand it /dev/null for the three
    // stdio slots so the spawn has real fds to install. (The post-pivot warden,
    // 5e, will give drivers a proper log sink.)
    let open_null = || OpenOptions::new().read(true).write(true).open("/dev/null");
    if let (Ok(i), Ok(o), Ok(e)) = (open_null(), open_null(), open_null()) {
        cmd.stdin(Stdio::File(i))
            .stdout(Stdio::File(o))
            .stderr(Stdio::File(e));
    }
    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => {
            say!("warden: spawn {} failed {:?}", m.name, e);
            return false;
        }
    };
    let pid = child.pid();
    match child.wait() {
        Ok(status) => {
            say!(
                "warden: {} pid={} exited code={:?}",
                m.name,
                pid,
                status.code()
            );
            status.success()
        }
        Err(e) => {
            say!("warden: wait {} (pid={}) failed {:?}", m.name, pid, e);
            false
        }
    }
}
