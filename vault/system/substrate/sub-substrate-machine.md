---
id: sub-substrate-machine
type: sub
parent: moc-substrate
title: "The machine — run-vm.sh, the QEMU virt board, and the three accelerators"
code:
  - tools/run-vm.sh
audit: none
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
abis: [abi-boot-banner]
design: ["docs/TOOLING.md", "docs/PORTABILITY.md"]
created: 2026-08-01
updated: 2026-08-16
---
## Purpose

The single canonical QEMU invocation. Every other harness shells out to it,
so the board Thylacine develops against is defined in exactly one file —
"direct `qemu-system-aarch64` invocations diverge and accumulate
inconsistencies" (TOOLING.md §3).

## Contract

- `-machine virt` with the accel-appropriate GIC version; `-smp 4` and
  `-m 2048` by default.
- Boots `thylacine.bin` (the flat image), never the ELF.
- Wires the full device complement: two virtio-blk (pool + scratch disk),
  two NICs (mmio + PCI), keyboard/tablet/mouse, two GPUs, two RNGs, the 9P
  host share, and a QMP control socket.
- Prints `==> qemu: accel=<a> cpu=<c> gic=v<n> smp=<n>` — the line
  downstream harnesses read back rather than re-deriving.
- Every device group is disable-able by env (`THYLACINE_NO_{NET,INPUT,GPU,QMP,SHARE}`).

## Mechanism

**The kernel must be loaded as a flat binary, and that is a QEMU
requirement, not a preference.** QEMU's `load_aarch64_image()` detects the
`ARM\x64` magic at offset 0x38 and treats the file as a Linux Image
(`is_linux=1`), which is what makes it load the DTB and pass its address in
`x0`. An ELF load takes the other branch (`is_linux=0`) and **skips the DTB
entirely** — the guest then boots with `x0 = 0` and no hardware view at all.
The ELF exists only for the debugger.

**Slot allocation is reverse-order, and the tree exploits it deliberately.**
QEMU's virt machine assigns virtio-mmio slots by reading the device list
back-to-front, so the FIRST `-device` on the command line lands in the
highest slot (31). Two virtio-blk devices are wired, and the collision is
resolved by scan direction rather than by configuration:

| Device | Slot | Claimed by | Scan direction |
|---|---|---|---|
| `pool.img` (Stratum) | 31 | stratumd's `bdev_thylacine.c` | HIGH → LOW |
| `disk.img` (scratch) | 30 | `virtio-blk-probe` / `virtio-blk-rw` | LOW → HIGH |

Two devices, two directions, no negotiation and no collision. It works
because each consumer stops at its first match from opposite ends.

**`-device` ORDER is load-bearing for the PCI functions, for a reason that
is not about slots at all.** qemu-virt's PCI INTx has only four shared
lines, assigned `(slot + pin) % 4`, and `KObj_IRQ` is exclusive per INTID —
so the two IRQ-CLAIMING functions (the PCI NIC → netd, the GPU →
tapestryd) must land on distinct lines. Inserting the relative mouse before
the GPU shifted the GPU one PCI slot onto the NIC's line (intid 35 → 36)
and its `SYS_IRQ_CREATE` failed on exclusivity. The input functions are
poll-mode and never claim an IRQ, so the mouse can share any line — which
is why it expands LAST, after gpu and rng.

**The G-1 co-page rule is why persistent drivers are on PCI.** QEMU-virt
packs every populated MMIO slot into ONE 4 KiB page (stride 0x200), and
userspace MMIO claims are page-granular and exclusive. So that page belongs
temporally to the transient probes and then permanently to stratumd
(virtio-blk); a second persistent claimant starves the disk — boot-fatal,
measured at G-1. PCI BARs are per-function with no co-residency, so every
resident driver (tapestryd's GPU + keyboard + tablet, netd's NIC) takes a
PCI function while the one-shot kernel-test probes keep the MMIO devices.
Hence the deliberate `gpu0`/`gpu-mmio0` and `kbd-pci0`/`kbd0` pairs.

**The accel matrix is three coherent configurations, not a knob.**

| | accel | `-cpu` | GIC | Why |
|---|---|---|---|---|
| default on Apple Silicon | `hvf` | `host` | v2 | HVF's emulated GICv3 distributor MMIO trips an `isv` data-abort assert; the GICv2 MMIO CPU interface is the HVF enabler (Lazarus W2) |
| Linux on ARM silicon | `kvm` | `host` | `host` | the guest GIC is the in-kernel one matching the actual silicon |
| fallback / compat | `tcg` | `max` | v3 | full ISA incl. RNDR; v3 is QEMU-virt's modern default |

**The third row's GIC entry is a different *kind* of choice from the other
two.** Both emulated arms name a version; the hardware arm names *the host's*,
delegating the question to the silicon rather than answering it. That works
because the guest autodetects version 2 versus 3 from the device tree anyway, so
neither an emulated pin nor a hardware passthrough is baked into the kernel.

A consequence of that delegation shows up in the announce line, which prints the
GIC field with a literal `v` prefix and therefore reads `gic=vhost` on the
hardware arm. Checked rather than assumed: **no consumer parses that field** —
every downstream reader extracts the accel token alone — so it is cosmetic. The
contract above is accurate that harnesses read this line back, and imprecise
about which part.

Auto-detection probes the host (`kern.hv_support`) AND the qemu build, so
the launcher still works on a non-Apple box. The kernel autodetects v2-vs-v3
from the DTB, so neither choice is baked into the guest. Under HVF the guest
sees Apple cores, which have LSE+PAC+BTI but **not** FEAT_RNG/RNDR — which
is precisely why the kernel CSPRNG seeds from virtio-rng (Lazarus W3): one
software path on every target.

**The `nowatchpoint` token, and why its ABSENCE is the safe default.** QEMU
TCG programs `DBGWVR`/`DBGWCR` but never raises EC 0x34; a guest thread that
touches a watched page then spins inside the emulator's retry of that one
instruction, takes no timer IRQ, never reaches the EL0-return tail, and
therefore **cannot be killed** — under TCG's round-robin that wedged vCPU
starves the guest and the boot never finishes. The kernel is correct (the
same encoding fires on real silicon under HVF), so the only safe move is to
not arm a watchpoint on that substrate. It is advertised by appending
`thylacine.nowatchpoint` to `/chosen/bootargs`, which the guest reads back
through the `/hw` FDT mount — **no kernel cmdline parser required**. The
polarity is the design: absence means "watchpoints work", so every
substrate that is not TCG keeps the hard assertion, and `test.sh` enforces
the fire on any accel that can deliver it. A new substrate cannot silently
inherit the exemption.

**That polarity has since been tested by a real event rather than argued.** A
third accelerator arrived — hardware virtualization on ARM silicon — and did not
inherit the exemption, because the exemption is keyed to the one substrate that
needs it rather than to a list of substrates that do not. The default held
without anyone revisiting it, which is the whole return on choosing absence as
the safe state.

**The token array is appended to, never assigned, and the comment says why.** A
second boot token arrived later, and had the array been rebuilt rather than
extended, that arrival would have silently dropped the watchpoint token and
re-wedged the original defect under emulation. Nothing would have failed
loudly — the guest would simply have started arming watchpoints again on the
substrate that cannot deliver them.

**A second thing arriving is how the first thing gets silently voided**, and it
is worth noting the defence was written *before* the second token existed. That
is unusual: most instances of this class in the tree are recorded after the
collision.

**The second token opts a boot out of the build storm, and the reasoning is
about matched budgets rather than about the storm.** The compile-heavy pre-login
gate costs most of five minutes per boot under emulation, against an interactive
harness's five-minute login budget — so a disk image minted for the compiler gate
made every interactive scenario fail by timeout **with a completely healthy
guest**. Opting out *removes* the mismatch rather than detecting it, which is
what lets one image serve both gates.

The proof is not weakened, and the argument for that is the part worth keeping:
the storm still runs unconditionally on the ordinary boot, which is where the
charter it proves is actually tested. It merely stops being repeated dozens of
times by a harness that is not testing it. **Coverage is about the claim being
exercised somewhere, not about it being exercised everywhere** — and a gate that
re-runs an unrelated proof on every scenario is paying for it in the budget of
the thing it *is* testing.

The opt-out is compared against the string `1` rather than tested for
non-emptiness, because the obvious emptiness test is true for the string `0` —
so the documented way to turn the feature off would have been inert on arrival.

**The vnc display drops the MMIO GPU.** A display backend binds QemuConsole
0, and `gpu-mmio0` (probe-only, driverless in a resident boot) would squat
it — the VNC client must land on `gpu0`'s head. `cocoa` keeps the canonical
device set because its View menu switches consoles interactively.

## Data structures

None. The script's state is bash arrays expanded with the
`${arr[@]+"${arr[@]}"}` idiom, which is required for empty-array expansion
under `set -u`.

## Concurrency

One VM per invocation. Concurrency lives in the callers: two worktrees can
run VMs simultaneously, which is why every reap in this area must be scoped
to its own build dir ([[sub-substrate-interactive]], #59).

## Invariants enforced

None of §28 — this is host tooling. It does however *establish the
conditions* several kernel invariants are verified under: I-5's hardware
exclusivity is what the INTx ordering rule serves, and the GICv2 selection
is what makes the I-18 IPI path reachable under HVF at all.

## Error paths

Missing `thylacine.bin` → refuse with a build hint. Unknown
`THYLACINE_DISPLAY` → exit 2. A missing pool/disk/ramfs is a *soft* absence:
the flag group is simply omitted and the guest degrades (no
`/srv/stratum-fs`), which is why run-vm announces which pool it is booting.

## Performance

HVF boots far faster than TCG; the 90 s default timeout in `test.sh` is
sized for the slower TCG compat run. Idle cost under HVF was the subject of
#299/#890 (the tickless work) — a never-stopped tick showed 332% idle.

## Prosecution

- The two-scan-direction pool/disk split depends on QEMU's reverse-order
  slot assignment. A QEMU change here silently swaps which device stratumd
  mounts.
- Any new `-device` inserted before `gpu_flags` re-shuffles PCI slots and
  can collide two IRQ-claiming functions onto one INTx line. Add at the end.
- A new accel or board must either deliver EL0 watchpoints or set
  `thylacine.nowatchpoint`; inheriting the exemption by accident is the
  failure the absence-default is designed to prevent.
- `-cpu host` under HVF means the gate's CPU is the *dev machine's* CPU —
  LSE is present, so this path is structurally blind to an ARMv8.0 floor
  regression. That is [[gate-v80-floor]]'s job. **The hardware-virtualization arm
  inherits the same blindness for the same reason**, and adds a second: its GIC
  is whatever the silicon has, so a version-specific defect is only exercised on
  whatever board happens to be in the loop.
- **The boot-token array must keep being appended to.** Rebuilding it drops
  whichever token was added first, silently, and the guest resumes the behaviour
  the dropped token exists to prevent.
- **A new accelerator must be reasoned about against the watchpoint exemption
  explicitly**, even though the absence-default means doing nothing is usually
  right. Doing nothing is right *because* the exemption is keyed to the one
  substrate that needs it; a future token keyed to a list of substrates that do
  not would invert that.

## Seams

[[seam-70-tcg-watchpoint]] · [[seam-791-smp1-joey]].

## Caveats

- `--snapshot` is parsed but unimplemented: it prints "not yet implemented"
  and continues. TOOLING.md §6 describes the snapshot workflow as a
  Phase-5+ deliverable; the flag is a placeholder, not a feature.
- The header comment still describes P1-A ("kernel boots, prints banner,
  hangs. No disk image yet... uncomment as the corresponding subsystems
  land") and predicts graphical flags "at Phase 8". Both landed long ago and
  the flags below the comment are live — read the code, not the preamble.
- The pool-size line is a *coarse tell* (64M bootstrap / 2560M goroot /
  3072M clade / 5120M both), explicitly **not** a verification. The
  verification is at the bake chokepoint ([[sub-substrate-build]], #101).

## Provenance

[[chg-2026-08-01-substrate-sweep]] records the sweep;
[[chg-2026-08-16-machine-third-accel]] the hardware-virtualization arm and the
two boot tokens. The board accreted
across the whole project; the load-bearing corrections are noted inline
above (G-1 co-page, the INTx ordering, Lazarus W2/W3, task #70).
