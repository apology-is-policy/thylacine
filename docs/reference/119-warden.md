# 119 - warden: the Menagerie hardware broker

> As-built reference for `usr/warden` + `usr/menagerie-probe` +
> `usr/virtio-mmio-source` (Menagerie build-arc steps 5c + 5d-2). Design:
> `docs/MENAGERIE.md` §3-6, §16, §18. Invariant **I-34** (the hardware allowance).
> No new kernel ABI -- pure userspace over the frozen 5a `Command::allowance` + the
> libdriver crate (`118-libdriver.md`).

---

## Purpose

The **warden** is the TCB component that turns the raw hardware-discovery sources
into capability-sandboxed driver Procs. It reads the device inventory, matches
each node against a driver-manifest database, intersects the node's resources
with the matched manifest's `needs` to compute the **narrowed allowance** (the
auditable I-34 grant), and spawns the driver with exactly that allowance plus a
descriptor of what it was granted. The warden reads the inventory for
*information*; the allowance it confers is the *authority*, and the kernel
enforces it (a driver that fabricated a PA outside its allowance is rejected by
the I-34 gate, not by the warden).

5c proves the loop end to end on QEMU-virt with the DTB discovery source (the
devhw `/hw` tree) and a one-entry built-in bind database (the pl061 GPIO ->
`menagerie-probe`). 5d-2 adds the **virtio-mmio bus source** (below): the warden
discovers the typed `virtio:<id>` nodes through a sandboxed enumerator, staying
hardware-free.

---

## The engine

`usr/warden/src/main.rs`, `rs_main`:

1. **Parse the built-in bind database** (`BUILTIN_MANIFESTS`, a compiled-in
   `&[&str]` of §6 manifests). A malformed built-in is a build bug -> fail loud
   (exit 1). v1.x reads `/lib/driver/*.manifest`.
2. **Discover via the sources** (MENAGERIE §3/§7): the **DTB source**
   (`libdriver::DtbSource`) enumerates `/hw` -> `Compatible` nodes (the static
   fabric); the **virtio-mmio bus source** (`run_virtio_mmio_source`, below)
   enumerates the bank -> typed `virtio:<id>` nodes. The raw `virtio,mmio`
   transport nodes are suppressed (`is_virtio_mmio`, claimed by the bus source).
   The warden binds on a node's typed IDENTITY (`DeviceId`), never the transport,
   and never reads a device register itself.
3. **Match + grant** (`libdriver::best_match` + `resolve`): for each discovered
   node, find the database manifest that binds its most-specific identity (the
   earliest in the node's most-specific-first `ids` list), then `resolve` the
   grant (`BoundResources`). A per-manifest instance counter feeds `%instance`.
4. **Confer + spawn** (`bind_and_run`): encode the grant into the argv descriptor
   (`to_descriptor`) AND the kernel allowance (`to_allowance`) -- both from the
   one `BoundResources`, so the authority the kernel enforces and the resources
   the driver maps cannot drift -- then spawn the driver `/<name>` with the
   descriptor as argv[1], `CAP_HW_CREATE`, and the narrowed allowance.
5. **Supervise + tear down** (`bind_and_run` + `await_readiness`, 5e-1): the
   driver is spawned with a piped stdout; the warden waits for it to either
   signal readiness (`READY`, a long-lived service still holding its device) or
   exit (a one-shot proof). A long-lived service is then torn down via
   **DeviceRemoved** -- `Child::kill` writes `killgrp` to `/proc/<pid>/ctl`,
   which revokes the driver's allowance FIRST (atomic, #160) then
   group-terminates it; the driver, blocked in a death-interruptible
   `Irq::wait`, unwinds cleanly and the reap frees the slot's exclusive claims.
   A one-shot proof (menagerie-probe) is reaped for its exit code. **Bounded
   restart-on-crash** (the manifest's `restart` policy + back-off + give-up) is
   5e-2; the **device-gone terminal CQE to a consumer** is 5e-3.

Exit 0 iff every bound driver came up (or nothing matched); exit 1 if a bound
driver failed to come up.

### Long-lived serve + the DeviceRemoved teardown (5e-1)

A real driver is a long-lived service: `netdev-driver` runs the net-1 24-ARP
proof, signals `READY`, quiesces its device, and then blocks in `Irq::wait`
indefinitely -- it never exits on its own. The warden brings it up and then
demonstrates **DeviceRemoved** by group-terminating it. Three things make this
sound:

- **Readiness without EOF (`await_readiness`).** The warden cannot learn a
  driver came up by reading its stdout pipe to EOF: a libdriver driver exits via
  `SYS_EXIT_GROUP`, and a single-thread Proc defers its handle-table close --
  including the pipe write end -- to *reap*, not exit (the #926 asymmetry). So a
  silent exit never EOFs the pipe while the warden holds the only reader and has
  not reaped, and a blocking read would deadlock. `await_readiness` instead
  detects an exit with `try_wait` (off the pipe) and polls the pipe only for the
  `READY` data line, bounded by a give-up (~10s). A long-lived service signals
  `READY` (data); a one-shot proof or a bring-up crash is caught by `try_wait`.
- **The teardown is the I-25/#160 mechanism.** `Child::kill` ->
  `/proc/<pid>/ctl` `killgrp` -> `proc_group_terminate`, which revokes the
  allowance as its first step (atomic with the death-wake). The warden is
  authorized as the driver's owner (both `PRINCIPAL_SYSTEM`); `/proc` is reached
  through the boot namespace the warden inherits. The freed slot is observable:
  stratumd claims the (page-shared) virtio-blk slot post-pivot.
- **The teardown is DMA-safe.** The driver quiesces its device *before* it
  blocks, so a forced group-terminate (which does NOT run `Drop`) leaves no live
  queue to DMA into the pages the reap frees. A driver torn down *mid-DMA* with a
  still-live device is the MENAGERIE section-10 surprise-removal hazard; its full
  fencing (an IOMMU, or a cooperative quiesce-on-remove) is owed to net-2 / real
  hardware. The data path itself -- serving `/dev/net/0` -- is also net-2; 5e
  proves the LIFECYCLE.

### The virtio-mmio bus source (5d-2)

QEMU-virt exposes ~32 identical `virtio,mmio` transport slots; the DTB cannot say
which device is in which slot -- only each slot's runtime `DeviceID` register can.
The warden does **not** read that register (it stays hardware-free); instead it
spawns a separate, capability-sandboxed enumerator, `/virtio-mmio-source`
(`usr/virtio-mmio-source`), granted an allowance narrowed to **only** the
virtio-mmio bank (`bank_window` = the page-aligned union of the slots' `reg`; one
MMIO window, no IRQ). The source maps the bank, reads each slot's `DeviceID`, and
writes one typed `DeviceNode::to_record` line per *populated* slot to its stdout --
a **pipe** the warden reads (`Stdio::Piped` -> `child.stdout`). The warden reads to
EOF then reaps, and `DeviceNode::parse_record`s each line; the read is **bounded**
(`slurp_capped`, 64 KiB -- a runaway/hostile source cannot OOM the warden, the TCB)
and the parse is strict + count-bounded.

The source is **non-TCB**, so the warden does not trust what it reports. It uses
the source only for the slot IDENTITY and reconciles each reported node against its
OWN trusted DTB view (`reconcile_reported_node`): match the reported node to a
trusted slot by reg base, then rebuild its reg/INTID from that trusted slot. So a
compromised source can at most mis-identify a real slot (the driver's device
re-validation catches a wrong device) or name a non-existent slot (rejected) -- it
can **never** fabricate a reg/INTID to inflate a driver's conferred allowance (the
5d-4 trust-boundary fix; the I-34 "grant exactly that slot" property is enforced,
not merely trusted). The source releases the bank before exiting, so the warden can
later grant an individual slot to a driver (the exclusive MMIO claim must be free).

This realizes the §3 "the warden never reads a device register" property: the
`DeviceID`-poke is isolated in the sandboxed source, and the warden binds the
typed `virtio:<id>` children by id (e.g. `virtio:1` -> netdev, 5d-3). It is the
template every self-enumerating-bus source (PCIe/USB) follows. Proven at boot: the
source reports the 6 populated QEMU-virt slots (`virtio:1` net, `virtio:2` blk x2,
`virtio:4` rng, `virtio:16` gpu, `virtio:18` input), the warden logs each typed
node, and -- because the bank is released -- stratumd still claims its virtio-blk
device post-pivot.

### The bind database (5c + 5d-3)

Two manifests: the pl061 GPIO (a single-instance, undriven, unreserved QEMU-virt
device) -> `menagerie-probe`, proving the I-34 grant on a trivial device; and the
`virtio:1` net device -> `netdev-driver` (5d-3), the first *useful* driver, bound
through the bus source's typed identity:

```
driver "menagerie-probe" {
    abi   = 1
    binds = ["arm,pl061"]
    needs { mmio = "node:reg"  irq = "node:interrupts"  dma = "pool: 64 KiB" }
    serves  = "/dev/gpio/%instance"
    restart = on-crash
}
driver "netdev-driver" {
    abi   = 1
    binds = ["virtio:1"]
    needs { mmio = "node:reg"  irq = "node:interrupts"  dma = "pool: 64 KiB" }
    serves  = "/dev/net/%instance"
    restart = on-crash
}
```

At boot the warden binds both (`2 bound, 2 up`): pl061 -> `menagerie-probe`
(grant + allowance enforced); `virtio:1` -> `netdev-driver`, which maps the
granted slot via `VirtioNet::open_slot` and runs the 24-ARP net-1 proof. A
virtio-mmio slot's `reg` is **sub-page** (0x200), so `libdriver::to_allowance`
page-rounds the MMIO grant out to the slot's 4 KiB page (MMIO is mapped
page-granular -- a sub-page allowance is unmappable); the descriptor keeps the
exact sub-page window so the driver still learns its precise slot address. The
page-rounded grant spans the shared net/blk page -- the documented #140 / net-2
co-residency over-grant, released when the one-shot driver exits.

`resolve` against the pl061 node yields `mmio = [(0x9030000, 0x1000)]`,
`irq = [39]` (SPI 7 + 32), `dma_max = 65536`.

### The spawn -- the boot-probe stdio subtlety

The boot-probe warden has **no stdio fds of its own** (joey spawns it via
`t_spawn_with_caps`, which passes none). `Command`'s default `Stdio::Inherit`
bumps the parent's fd 0/1/2 -- which would fail in the SYS_SPAWN_FULL_ARGV
handler *before* the kernel resolves the binary, returning a bare `-1`. So the
warden hands each driver `/dev/null` (opened read+write) for the three stdio
slots; the driver logs via the console-direct path (`t_putstr`), not fds. (The
post-pivot warden, 5e, will give drivers a proper log sink.) This is the first
pre-pivot `Command::spawn` in the tree -- login uses it post-pivot, where it
*has* console fds to inherit.

---

## `menagerie-probe` -- the first live `impl Driver`

`usr/menagerie-probe/src/main.rs` is a real `libdriver::Driver` whose `main` is
`run::<ProbeDriver>()`. It proves the loop with one positive and one negative:

- **POSITIVE**: `map_mmio(res, 0, ..)` maps the granted pl061 window. The grant
  came from the node's own `reg`, the I-34 gate admits exactly that window, and
  pl061 is undriven/unreserved -> the map succeeds. This is the descriptor
  round-trip (the driver received the exact window the warden granted) plus the
  allowance permitting the grant.
- **NEGATIVE**: an `Mmio::new` for a PA *outside* the grant (`0xDEAD_0000`) must
  be rejected -- the conferred allowance is narrowed, so the kernel gate denies
  it. A success here would mean the allowance was not enforced (the bug guarded
  against).

It holds no long-lived device; `serve` returns immediately so the warden can reap
it and read the lifecycle status.

---

## Boot-probe placement

joey spawns the warden in the `THYLA_BOOT_PROBES` ladder (`usr/joey/joey.c`),
PRE-pivot (so `/menagerie-probe` resolves in devramfs by absolute path), PRE-
stratumd, with `CAP_HW_CREATE` (which the warden, broad-allowance, confers a
narrowed slice of onto each driver). joey reaps it and fails the boot if the
warden exits non-zero. The observed boot output:

```
warden: /hw discovered 45 device nodes
warden: bind arm,pl061 (pl061@9030000) -> menagerie-probe inst=0 [mmio=1 irq=1 dma=0x10000]
menagerie-probe: mapped granted MMIO 0x9030000/0x1000 OK (4096 bytes)
menagerie-probe: out-of-grant MMIO create rejected OK (allowance enforced)
warden: menagerie-probe pid=N exited code=Some(0)
warden: 1 bound, 1 up
joey: warden ok (5c Menagerie bind-loop: discover -> grant -> spawn narrowed)
```

---

## Tests / proof

- **Pure logic** is host-tested in libdriver (the `dtb` decode against the real
  QEMU-virt bytes; `resolve` + the descriptor codec) -- `118-libdriver.md`.
- **Live end-to-end** is the boot-probe proof above: discovery (45 `/hw` nodes),
  match (pl061), grant (`mmio=1 irq=1 dma=0x10000`), spawn narrowed, the driver's
  positive map + negative reject, reap (`1 bound, 1 up`), `joey: warden ok`,
  `Thylacine boot OK`, 0 EXTINCTION. Gated additionally by the SMP gate
  (default + UBSan x smp4/smp8).

---

## Status / known caveats

- **Long-lived serve + DeviceRemoved (5e-1).** A long-lived service is brought
  up and then torn down (revoke + group-terminate) to prove the teardown
  lifecycle; a one-shot proof is reaped for its exit code. Still owed: **bounded
  restart-on-crash** (the manifest `restart` policy + back-off + give-up, 5e-2)
  and the **device-gone terminal CQE to a consumer** (5e-3). The persistent
  post-pivot warden (drivers kept up, not torn down at boot) is the deployment
  shape; the boot-probe warden demonstrates the full lifecycle in bounded form.
- **DTB + virtio-mmio sources (5d-2).** The DTB source + the virtio-mmio bus
  source are built; PCIe/USB/SDIO sources are MENAGERIE §3/§7 seams that plug into
  the same `DiscoverySource` slot (the step-6 per-(bus,dev,fn) PCI allowance axis,
  #159, is the first). The virtio-mmio source enumerates once at boot; the live
  `DeviceRemoved` hotplug stream is 5e.
- **Compiled-in bind DB.** Manifest files under `/lib/driver` are a v1.x
  ergonomic; the `sig` authorization ladder (try-bind / ask-once / remember,
  MENAGERIE §9) is unbuilt.
- **No useful driver yet (through 5d-2).** `menagerie-probe` proves the mechanics
  and the virtio-mmio source proves the discovery channel; the first real driver
  the warden binds NARROWED (by `virtio:1`) is the netdev retrofit (**5d-3**),
  which retires `usr/lib/netdev`'s hardcoded `virtio.rs:51` base. At 5d-2 the
  discovered `virtio:<id>` nodes are logged-but-unbound (no virtio manifest yet).
- **The boot probe is `THYLA_BOOT_PROBES`-gated** (off in `--production`), like
  the netdev probes. The persistent warden is a 5e+ service.

---

## Cross-references

- `docs/MENAGERIE.md` §4 (allowance / I-34), §5 (the warden), §6 (driver +
  manifest), §16 + §18 (the build sequence).
- `docs/reference/118-libdriver.md` -- the framework crate the warden + the
  driver are built on.
- `docs/reference/117-allowance.md` -- the kernel I-34 gate + the 5a
  confer-at-spawn ABI.
- `docs/reference/116-devhw.md` (or the devhw section) -- the `/hw` DTB tree the
  warden reads.
