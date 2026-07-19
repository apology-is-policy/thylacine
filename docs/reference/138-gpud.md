# 138 — gpud: the resident virtio-gpu driver (G-1)

As-built reference for `usr/gpud`, the Tapestry build arc's stage-0
device-owning half (TAPESTRY.md §18.9 G-1; landed on `gfx-1`). The number 137
is reserved by main's `137-gopls.md` (Go 8d); this doc takes 138 to avoid a
merge collision.

## Purpose

A warden-manifested, `lifecycle = persistent` virtio-gpu driver that brings
the GPU up at boot, presents the P4-L 4-quadrant test pattern on scanout 0,
and holds the device — and therefore the display — up for the life of the
box, ticking an 8×8 heartbeat block through partial-rect
`TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`, each IRQ-completed.

Why resident (the G-0 finding): a scanout lives exactly as long as its
driving Proc. At driver reap the RW-7 proc-death quiesce resets the dying
driver's virtio devices (DMA soundness — no in-flight device write into
freed KObj_DMA pages), and a virtio-gpu reset destroys the host-side
resources and disables the scanout. The one-shot `/virtio-gpu` probe's
pattern therefore dies at its reap; gpud's persists, and
`tools/screendump.sh -v` verifies it at any time after READY.

## The transport decision — PCI, not the virtio-mmio slot

The G-1 charter said "promote usr/virtio-gpu" (the MMIO probe); the first
build did exactly that and **measured** the structural blocker: QEMU-virt
packs all six populated virtio-mmio slots into ONE 4-KiB page (`0xa003000`;
slot stride 0x200), and a userspace MMIO claim is page-granular + exclusive
(I-5). The boot survives on TEMPORAL sequencing — the transient probes and
the netdev-driver ARP proof each release the page before the next claimant,
and stratumd then holds it permanently (its virtio-blk slots). A resident
MMIO gpud starved netdev-driver (soft) and stratumd's disk claim
(`stratumd: run failed (rc=-207)` — boot-fatal). A second persistent
claimant on a shared MMIO page is structurally impossible.

The fix is the move netd already made for the NIC (#140): **virtio-gpu-pci**
— per-function BARs, no co-residency. `tools/run-vm.sh` wires BOTH devices:

- `-device virtio-gpu-pci,id=gpu0,disable-legacy=on` — gpud's device (and
  `tools/screendump.sh`'s default `-d gpu0`).
- `-device virtio-gpu-device,id=gpu-mmio0` — the one-shot kernel-test
  probe's device (P4-L; its scanout legitimately blanks at the probe's
  reap).

Consequence for G-3 scripture: tapestryd's manifest absorbs the GPU as
**`virtio-pci:16`** (not the `virtio:16` MMIO id §18.7 sketched — same
identity model, transport-shifted). Side effect: the GPU function takes bdf
0.2.0, shifting rng-pci/9p-pci down one slot; nothing binds those by bdf.

## Implementation

`usr/gpud/src/main.rs`, self-contained (~700 lines), two lineages:

- The command machinery — single-page ring layout (controlq + the
  configured-but-unused cursorq + request/response regions), the
  2-descriptor command/response chain, the request builders, the
  `submit_and_wait` IRQ discipline (`virtio_rmb` after the `used.idx`
  observation; bounded non-queue-wake retry) — is the audited
  P4-L/P4-L-scanout probe's (`usr/virtio-gpu`), verbatim where possible.
- The PCI transport — `PciDev::claim(16, BAR_WINDOW_VA)` + the common-cfg
  handshake (VERSION_1 only; MSI-X parked at NO_VECTOR — INTx), per-queue
  setup + the notify-doorbell bound check (the pci-3 F2 guard), the
  read-to-clear ISR — mirrors the audited netdev `VirtioNetPci` (pci-2/3).
  All device access rides the ISV-safe `libthyla_rs::hardware` accessors
  (#890).

`probe()` = claim → regions → `Irq`/`Dma` (RAII) → prewarm → init handshake
→ the 7-command bring-up (GET_DISPLAY_INFO, CREATE_2D, fill + heartbeat
paint, ATTACH_BACKING, SET_SCANOUT, full TRANSFER, full FLUSH). `serve()` =
the `READY` line to stdout (the warden's readiness pipe — console output
never goes there), then the render loop: sleep `TICK_MS` (500 ms) → toggle
the heartbeat → partial-rect TRANSFER + FLUSH.

The heartbeat block sits at rows/cols 60..68 — compile-time-asserted clear
of the four `-v` quadrant sample points, so `screendump -v` is
tick-phase-independent while two dumps ≥ half a period apart differ (the
liveness witness; proven at pixel level: block(64,64) yellow ↔ black across
a tick, neighbors stable).

## The warden manifest + the crash-probe re-home

```
driver "gpud" {
    binds = ["virtio-pci:16"]
    needs { pci = "node"  irq = "node:interrupts"  dma = "pool: 64 KiB" }
    restart = on-crash    lifecycle = persistent
}
```

crash-probe (the supervision demo) re-homed off `virtio:16` to the
warden-synthetic **`restart-test`** node (TAPESTRY.md §18.12 F15): the
warden appends a resource-less `DeviceNode` to its discovered set, and the
demo's grant is `mmio=0 irq=0 dma=0` — the restart ladder exercised with
zero hardware footprint. This returned `virtio:16` to the kernel-test
probe's exclusive (transient) use and restored netdev-driver's ARP proof
(`24/24 PASS` again — it runs before stratumd claims the shared page).

## The pattern-persists gate

`tools/test.sh` (and therefore every `ci-smp-gate` boot, which composes it)
runs `tools/screendump.sh -v` after the boot banner + grace, before
declaring PASS — bounded retry (20 × 0.5 s), failing the boot as
`gpu-gate` with a gpud/warden log slice. Skips: `THYLACINE_NO_GPU=1`,
`THYLACINE_NO_QMP=1`, `THYLACINE_GPU_GATE=0`, missing python3/socket. Since
gpud signals READY before the warden bind loop completes and joey waits
that loop out before the banner, the first try succeeds on a healthy boot.

## Known caveats

- **No restarter after the warden exits.** The boot-probe warden runs its
  bind loop and exits; a gpud crash after that leaves a quiesced (blank)
  display until reboot. Shared posture with netd (the same persistent
  model); the resident-warden lift is the v1.x seam.
- **cursorq is configured but unused** (the device requires both queues
  live; cursor work arrives with tapestryd at G-3).
- gpud serves no namespace files at G-1 (`serves` is decor;
  MAY_POST_SERVICE conferred by the persistent lifecycle but unused).
- At G-3 tapestryd absorbs the device and this scaffold retires.

## Tests + gates

Suite 1170/1170 + boot OK (the ecosystem restored: netdev-driver ARP
24/24, stratumd serving, crash-probe laddering on the synthetic node);
the pattern-persists gate green inside `tools/test.sh`; liveness proven by
differing cross-tick dumps; the reduced SMP amplifier per the G-1 close.
Kernel byte-unchanged.
