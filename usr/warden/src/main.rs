// /warden -- the Menagerie hardware broker (MENAGERIE.md sections 4-6), build-arc
// step 5c. The warden is the TCB component that turns the raw discovery sources
// into capability-sandboxed driver Procs: it reads the device inventory, matches
// each node against a driver-manifest database, intersects the node's resources
// with the matched manifest's needs to compute the narrowed allowance (the
// auditable I-34 grant), and spawns the driver with exactly that allowance plus a
// descriptor of what it was granted.
//
// 5c proves the loop end to end on QEMU-virt with the DTB discovery source (the
// devhw /hw tree) and a one-entry built-in bind database (the pl061 GPIO ->
// menagerie-probe). The engine is general; what 5c does not yet build:
//   - long-lived supervision + restart + DeviceRemoved revoke (5e). 5c reaps each
//     bound driver and reads its lifecycle status -- the probe is a one-shot.
//   - the other discovery sources (PCIe/USB) + the manifest-file database
//     (/lib/driver/*.manifest). v1.0 compiles the bind DB in.
//   - retrofitting the real virtio drivers to be warden-bound (5d).
//
// The warden holds CAP_HW_CREATE + the broad allowance (joey spawns it without a
// narrowing), so it can confer a narrowed allowance + CAP_HW_CREATE on each
// driver. It reads /hw for *information* only; the allowance it confers is the
// *authority*, and the kernel enforces it.
//
// Runs in joey's THYLA_BOOT_PROBES ladder, PRE-pivot (so /menagerie-probe
// resolves in devramfs by bare name). Exit 0 = every bound driver came up (or
// nothing matched); exit 1 = a bound driver failed to come up.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::ToString;
use alloc::vec;
use alloc::vec::Vec;

use libthyla_rs::fs;
use libthyla_rs::fs::OpenOptions;
use libthyla_rs::io::Read;
use libthyla_rs::process::{Command, Stdio};
use libthyla_rs::T_CAP_HW_CREATE;

use libdriver::driver::to_allowance;
use libdriver::dtb::{ARM64_ADDR_CELLS, ARM64_SIZE_CELLS, GIC_INTERRUPT_CELLS};
use libdriver::{resolve, BoundResources, Manifest, NodeResources};

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
/// driver's binary is `<manifest.name>` (resolved in the namespace by name). 5c
/// ships one driver -- the pl061 GPIO proof. v1.x reads `/lib/driver/*.manifest`.
const BUILTIN_MANIFESTS: &[&str] = &[r#"
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
"#];

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

    let nodes = discover_hw();
    say!("warden: /hw discovered {} device nodes", nodes.len());

    // A per-manifest instance counter so each bound device of a driver gets a
    // distinct %instance in its served path.
    let mut instance = vec![0u32; db.len()];
    let mut bound = 0u32;
    let mut up = 0u32;

    for (name, node) in &nodes {
        let Some(idx) = best_match(&db, node) else {
            continue;
        };
        let inst = instance[idx];
        let grant = match resolve(&db[idx], node, inst) {
            Ok(g) => g,
            Err(e) => {
                say!("warden: resolve {} ({}) failed {:?}", db[idx].name, name, e);
                continue;
            }
        };
        instance[idx] += 1;
        bound += 1;
        if bind_and_run(&db[idx], &grant, name) {
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

/// Enumerate `/hw` and build a `NodeResources` for each device node (a child
/// directory that exposes a `compatible`). Top-level only -- every bindable
/// device on the v1.0 targets (QEMU-virt, RPi4/5) is a direct child of the FDT
/// root; descending nested buses is a v1.x refinement.
fn discover_hw() -> Vec<(alloc::string::String, NodeResources)> {
    let mut out = Vec::new();
    let rd = match fs::read_dir("/hw") {
        Ok(r) => r,
        Err(e) => {
            say!("warden: read_dir(/hw) failed {:?}", e);
            return out;
        }
    };
    for ent in rd {
        let ent = match ent {
            Ok(e) => e,
            Err(_) => continue,
        };
        if !ent.is_dir() {
            continue; // skip the root-level property files (compatible, #*-cells)
        }
        let name = ent.file_name();
        let node = read_node(name);
        if !node.compatible.is_empty() {
            out.push((name.to_string(), node));
        }
    }
    out
}

/// Read a node's `compatible`/`reg`/`interrupts` property files and decode them.
fn read_node(name: &str) -> NodeResources {
    NodeResources::from_dtb(
        &read_prop(name, "compatible"),
        &read_prop(name, "reg"),
        &read_prop(name, "interrupts"),
        ARM64_ADDR_CELLS,
        ARM64_SIZE_CELLS,
        GIC_INTERRUPT_CELLS,
    )
}

/// Read one `/hw/<node>/<prop>` property file into bytes. A missing property
/// (the node does not have it) yields an empty buffer -- the decoder treats that
/// as an absent axis.
fn read_prop(node: &str, prop: &str) -> Vec<u8> {
    let path = format!("/hw/{}/{}", node, prop);
    let mut f = match fs::File::open(&path) {
        Ok(f) => f,
        Err(_) => return Vec::new(),
    };
    let mut buf = Vec::new();
    if f.read_to_end(&mut buf).is_err() {
        return Vec::new();
    }
    buf
}

/// Find the database manifest that binds this node at the most-specific
/// `compatible` (the earliest entry in the node's most-specific-first list).
/// Returns the manifest index, or `None` if no manifest binds the node.
fn best_match(db: &[Manifest], node: &NodeResources) -> Option<usize> {
    let mut best: Option<(usize, usize)> = None; // (db index, node-compatible position)
    for (i, m) in db.iter().enumerate() {
        for (pos, c) in node.compatible.iter().enumerate() {
            if m.binds.iter().any(|b| b == c) {
                let better = match best {
                    None => true,
                    Some((_, bp)) => pos < bp,
                };
                if better {
                    best = Some((i, pos));
                }
                break; // the node's first hit is this manifest's most-specific match
            }
        }
    }
    best.map(|(i, _)| i)
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
