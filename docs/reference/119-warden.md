# 119 - warden: the Menagerie hardware broker

> As-built reference for `usr/warden` + `usr/menagerie-probe` (Menagerie
> build-arc step 5c). Design: `docs/MENAGERIE.md` §4-6, §16, §18. Invariant
> **I-34** (the hardware allowance). No new kernel ABI -- pure userspace over the
> frozen 5a `Command::allowance` + the libdriver crate (`118-libdriver.md`).

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
`menagerie-probe`).

---

## The engine

`usr/warden/src/main.rs`, `rs_main`:

1. **Parse the built-in bind database** (`BUILTIN_MANIFESTS`, a compiled-in
   `&[&str]` of §6 manifests). A malformed built-in is a build bug -> fail loud
   (exit 1). v1.x reads `/lib/driver/*.manifest`.
2. **Discover** (`discover_hw`): `fs::read_dir("/hw")`, and for each child
   *directory* read its `compatible`/`reg`/`interrupts` property files
   (`read_prop`) and decode them into a `NodeResources` via
   `libdriver::dtb::NodeResources::from_dtb`. Only nodes that expose a
   `compatible` are kept. Top-level only (every bindable device on the v1.0
   targets is a direct FDT-root child; nested-bus descent is a v1.x refinement).
3. **Match + grant** (`best_match` + `libdriver::resolve`): for each node, find
   the database manifest that binds it at the most-specific `compatible` (the
   earliest entry in the node's most-specific-first list), then `resolve` the
   grant (`BoundResources`). A per-manifest instance counter feeds `%instance`.
4. **Confer + spawn** (`bind_and_run`): encode the grant into the argv descriptor
   (`to_descriptor`) AND the kernel allowance (`to_allowance`) -- both from the
   one `BoundResources`, so the authority the kernel enforces and the resources
   the driver maps cannot drift -- then spawn the driver `/<name>` with the
   descriptor as argv[1], `CAP_HW_CREATE`, and the narrowed allowance.
5. **Supervise** (5c: reap): the boot-probe driver is a one-shot; the warden
   `wait()`s it and reads the lifecycle status. 5e replaces the reap with
   long-lived supervision + restart + `DeviceRemoved` revoke.

Exit 0 iff every bound driver came up (or nothing matched); exit 1 if a bound
driver failed to come up.

### The bind database (5c)

One manifest, the pl061 GPIO proof target (a single-instance, undriven,
unreserved QEMU-virt device with `reg` + `interrupts`):

```
driver "menagerie-probe" {
    abi   = 1
    binds = ["arm,pl061"]
    needs { mmio = "node:reg"  irq = "node:interrupts"  dma = "pool: 64 KiB" }
    serves  = "/dev/gpio/%instance"
    restart = on-crash
}
```

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

- **One-shot, not supervised (5c).** The warden reaps each bound driver. Long-
  lived supervision (restart policy from the manifest, back-off, give-up) +
  `DeviceRemoved` revoke + the device-gone CQE to a consumer is **5e**.
- **DTB source only.** PCIe/USB/SDIO discovery sources are MENAGERIE §3 seams
  (the step-6 per-(bus,dev,fn) PCI allowance axis, #159, is the first).
- **Compiled-in bind DB.** Manifest files under `/lib/driver` are a v1.x
  ergonomic; the `sig` authorization ladder (try-bind / ask-once / remember,
  MENAGERIE §9) is unbuilt.
- **No useful driver yet.** `menagerie-probe` proves the mechanics; the first
  real driver the warden binds NARROWED is the netdev retrofit (**5d**), which
  retires `usr/lib/netdev`'s hardcoded `virtio.rs:51` base.
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
