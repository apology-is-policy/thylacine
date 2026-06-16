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

use alloc::vec;
use alloc::vec::Vec;

use libthyla_rs::fs::OpenOptions;
use libthyla_rs::process::{Command, Stdio};
use libthyla_rs::T_CAP_HW_CREATE;

use libdriver::driver::to_allowance;
use libdriver::{best_match, resolve, BoundResources, DiscoverySource, DtbSource, Manifest};

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

    // Discovery: the DTB source publishes the static fabric (MENAGERIE section 7).
    // The virtio-mmio bus source -- the typed virtio:<id> nodes -- is added in 5d-2.
    let nodes = DtbSource::new().enumerate();
    say!("warden: /hw discovered {} device nodes", nodes.len());

    // A per-manifest instance counter so each bound device of a driver gets a
    // distinct %instance in its served path.
    let mut instance = vec![0u32; db.len()];
    let mut bound = 0u32;
    let mut up = 0u32;

    for node in &nodes {
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
