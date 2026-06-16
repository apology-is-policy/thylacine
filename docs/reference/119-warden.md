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
4. **Confer + run** (`run_once`): encode the grant into the argv descriptor
   (`to_descriptor`) AND the kernel allowance (`to_allowance`) -- both from the
   one `BoundResources`, so the authority the kernel enforces and the resources
   the driver maps cannot drift -- then spawn the driver `/<name>` with the
   descriptor as argv[1], `CAP_HW_CREATE`, and the narrowed allowance, and watch
   it declare itself (`await_readiness`). Returns one `RunOutcome` (Served /
   Exited / HardFail).
5. **Supervise** (`supervise`, 5e): loop `run_once` under the pure
   `libdriver::supervise::next_step` decision (host-tested) -- restart a crash
   with back-off, bounded, per the manifest's `restart` policy, until the device
   settles. A long-lived service that signals `READY` is brought up and then torn
   down via **DeviceRemoved** -- `Child::kill` writes `killgrp` to
   `/proc/<pid>/ctl`, which revokes the driver's allowance FIRST (atomic, #160)
   then group-terminates it; the driver, blocked in a death-interruptible
   `Irq::wait`, unwinds cleanly and the reap frees the slot's exclusive claims. A
   one-shot proof (menagerie-probe) is reaped for its exit code. A driver that
   crashes is restarted with back-off and given up on after the bound (5e-2,
   below). The **device-gone terminal CQE to a consumer** is 5e-3.

Each bound device settles to one of three dispositions: **Up** (came up, or
served then removed), **GaveUp** (crashed + restarts exhausted -- a SOFT
per-device failure), or **Failed** (a structural failure -- the warden could not
spawn it). Exit 1 iff any device is `Failed`; a `GaveUp` device does NOT fail the
boot (the device is unavailable, the system is fine).

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
  - **The read is bounded, not byte-blocking (5e-4 audit F1).** Each poll-readable
    event does ONE bounded `read(up to READY_LINE_MAX)` -- which returns the
    *available* bytes without blocking for a full buffer -- and feeds the chunk
    into a persistent accumulator via the pure `libdriver::readyline::feed_ready_line`
    (scan for '\n', capped at `READY_LINE_MAX`). The original read the pipe ONE
    BYTE AT A TIME with *blocking* reads; a non-TCB driver that wrote a PARTIAL
    line (`"READ"`, no newline) and then held would stall the warden on the next
    byte FOREVER, escaping the give-up budget (which lives in the poll loop, not
    the inner read) -- a boot-availability DoS on the TCB warden by a misbehaving
    driver (the untrusted-3rd-party-driver model the framework exists to sandbox).
    A garbled (over-long) or EOF'd pipe now stops being read; the loop only
    watches `try_wait` for the exit, still bounded -> Timeout -> kill.
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

### Bounded restart-on-crash supervision (5e-2)

A driver that crashes during bring-up must not wedge the boot ladder, nor be
restarted forever. `supervise` (the impure loop) drives the **pure**
`libdriver::supervise::next_step` decision: given one run's `RunOutcome`, the
manifest's `restart` policy, and the restarts already spent, decide *restart with
back-off* or *settle*. The split mirrors `manifest`/`resource`/`source` -- the
state machine (with the subtle edges: crash-vs-clean, the give-up bound, the
policy cross-product) is host-tested (`supervise::tests`), and the warden owns
only the mechanism (spawn / readiness / reap / `time::sleep`).

The mapping `run_once` -> `RunOutcome`:

- `READY` then torn down -> **Served** (a clean bring-up; never a restart
  candidate -- the removal was deliberate).
- exited on its own -> **Exited(code)**; crash iff `code != Some(0)`.
- a garbled readiness line, or a hung driver (`Timeout`) -> treated as a crash
  (`Exited(None)`) so the supervisor restarts it per policy.
- could not spawn / track -> **HardFail** (structural -> `Failed`, no restart).

`next_step` then settles or restarts. The back-off is exponential from
`BACKOFF_BASE_MS` (50ms), doubling, capped at `BACKOFF_MAX_MS` (500ms); the
give-up bound is `RESTART_LIMIT` (3 restarts -> 4 spawns total). A crash that
exhausts the restarts settles **GaveUp** -- a SOFT per-device failure, logged but
not boot-failing; only a `HardFail` (e.g. a missing binary) is **Failed** (HARD).

The proof is the `crash-probe` driver, bound to the undriven `virtio:16` (GPU id).
Its `probe` always returns `Err` (`EXIT_PROBE`) -- *before any hardware claim*, so
its page-rounded allowance is never a live exclusive claim and cannot contend with
the netdev driver that shares the rounded MMIO page (the #140 co-residency
over-grant). At boot the warden restarts it three times (50/100/200ms) then gives
up; the boot stays green (`3 bound, 2 up, 1 gave up, 0 failed`) and netdev still
comes up on the next bind.

**v1.0 exit-code SEAM.** The kernel collapses every non-"ok" exit to status `1`
(`sys_exits_handler`; the structured 64-bit exit_status is a v1.x lift per
`docs/ERRORS.md`), so the warden only ever observes `Some(0)` (clean) or `Some(1)`
(crashed) -- the supervisor distinguishes clean-vs-crashed, **not** specific exit
codes. A finer policy (e.g. do-not-restart `EXIT_BIND`, a warden bug, vs restart
`EXIT_PROBE`, a device-init failure) needs the structured status and lands with
it. `next_step` already accepts arbitrary codes, so only the warden's `RunOutcome`
mapping changes when it arrives.

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
  QEMU-virt bytes; `resolve` + the descriptor codec; the `supervise` state machine
  -- the restart-vs-settle decision across every policy + the back-off schedule)
  -- `118-libdriver.md`.
- **Live end-to-end** is the boot-probe proof above: discovery (45 `/hw` nodes +
  the 6 typed virtio slots), match + grant + spawn narrowed for three drivers,
  menagerie-probe's positive map + negative reject (Up), netdev's 24-ARP +
  `READY` + DeviceRemoved teardown (Up), crash-probe's 3 restarts (50/100/200ms) +
  give-up (GaveUp), the tally `3 bound, 2 up, 1 gave up, 0 failed`,
  `Thylacine boot OK`, 0 EXTINCTION. Gated additionally by the SMP gate
  (default + UBSan x smp4/smp8).

---

## Status / known caveats

- **Long-lived serve + DeviceRemoved (5e-1) + bounded supervision (5e-2).** A
  long-lived service is brought up and then torn down (revoke + group-terminate)
  to prove the teardown lifecycle; a one-shot proof is reaped for its exit code; a
  crashing driver is restarted with back-off, bounded, then given up on (SOFT).
  Still owed: the **device-gone terminal CQE to a consumer** (5e-3). The
  persistent post-pivot warden (drivers kept up, not torn down at boot) is the
  deployment shape; the boot-probe warden demonstrates the full lifecycle in
  bounded form. v1.0 SEAM: the supervisor distinguishes clean-vs-crashed only
  (not specific exit codes -- the kernel collapses non-"ok" to 1; the structured
  status is a v1.x lift, see the 5e-2 section).
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
