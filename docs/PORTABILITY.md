# PORTABILITY.md — the bare-metal portability arc ("Lazarus")

**Status: binding design -- SIGNED OFF + SEQUENCED 2026-06-05; atomics approach
amended 2026-06-06.** The section-9 forks are resolved (name "Lazarus" kept;
PAC/BTI/LSE -> runtime-conditional approved, incl. the boot-banner ABI; M1 =
W1 + W1.5 + W2 + W3 as a pre-Utopia arc, W4 post-v1.0). The **2026-06-06
amendment** (user-voted): W1 ships the inline LL/SC v8.0 floor (not
`-moutline-atomics` -- no kernel uses outline-atomics in-kernel) and a new
**W1.5** builds the Linux-style boot-time LSE alternatives-patcher to restore LSE
on capable cores at zero runtime cost (§4 + §4.5 + §9). M1 is the **first of two
pre-Utopia arcs** (Lazarus M1, then Loom) that land before Phase 7 (Utopia)
resumes at U-6d-b; per-workstream impl detail is authored at implementation time.
Authored 2026-05-30 after the HW-accel assessment mini-detour (see
`tools/run-vm.sh` commit `52d5663` for the empirical HVF findings).

**Working name: "Lazarus"** — the Lazarus-species motif (an extinct taxon
rediscovered alive; CLAUDE.md's naming sources). The thylacine leaves the
enclosure (QEMU emulation) for the wild (real silicon). *Held for signoff; rename
freely.*

---

## 1. Mission

Today Thylacine runs on exactly one substrate: QEMU `virt` under TCG emulation.
This arc makes the **same kernel binary** run on three:

1. **QEMU `virt` / TCG** — the current deterministic CI baseline. Unchanged.
2. **QEMU `virt` / HVF on Apple Silicon** — a near-native dev loop (the
   motivation: TCG boots take 16-26 s; HVF is the fast iteration win).
3. **Bare-metal ARM64 hardware** — first board **Raspberry Pi 400** (the
   lineage conviction that a real OS runs on real hardware).

**Method:** target the **ARMv8.0-A ISA floor** — the strict common subset of
Cortex-A72 (Pi 400), Cortex-A76 (Pi 5), and Apple M-series — and **light up
later features at runtime when the CPU implements them**. One binary, every
target, **no hardening loss on capable hardware**.

This arc was always partly committed: `ARCHITECTURE.md §4.4` names a first
bare-metal target and `§22` specs the `arch/arm64/<platform>/` structure (which
does not exist yet). Lazarus pulls that forward, makes it concrete, reconciles
the board choice, and adds the ISA-floor + GICv2 + software-RNG specifics the
assessment surfaced.

---

## 2. Why it is not "just RNDR" (the assessment, in one paragraph)

The hypothesis going in was "the only thing blocking acceleration is the RNDR
hard dependency." Ground truth (empirical, QEMU 10.0.2 + Apple Silicon)
falsified it. `kernel/random.c` is already graceful — RNDR absent means reads
return `-1` and **boot continues** (KASLR + the stack canary seed from the DTB
`rng-seed`/`cntpct`). Under HVF the kernel boots **natively** — LSE and PAC
execute fine, so the ISA was never the blocker — through almost all of GICv3
init. It dies in the GIC, two blockers: (1) a timing-sensitive `ESR.ISV=0`
data-abort syndrome on rapid back-to-back GICD MMIO that QEMU's HVF can't
emulate (a heisenbug — intervening MMIO hides it); and (2) the wall — the GICv3
**CPU-interface system registers** (`ICC_SRE_EL1` / `ICC_*`) are **undefined on
Apple cores** under HVF, because Apple Silicon uses AIC, not GICv3. Both are
solved by a **GICv2** path (MMIO CPU interface, no `ICC_*` sysregs) — which is
**also** what RPi 400 needs (BCM2711 = GIC-400 = GICv2). GICv2 is the convergent
investment.

---

## 3. Target matrix

| Target | CPU | ISA | LSE | PAC/BTI | RNDR | GIC | Accel | Role |
|---|---|---|---|---|---|---|---|---|
| QEMU virt (TCG) | `-cpu max` | v8.5+ | yes | yes | yes (emul) | v3 | tcg | CI baseline; deterministic; unchanged |
| QEMU virt (HVF) | Apple M-series | v8.6-ish | yes | yes | **no** | **v2** | hvf | the fast dev loop |
| RPi 400 | Cortex-A72 | **v8.0** | **no** | **no** | **no** | **v2** (GIC-400) | native | first bare-metal board |
| RPi 5 (secondary) | Cortex-A76 | v8.2 | yes | no | no | v2 (GIC-400) | native | covered by the v8.0 base |

The **v8.0 floor** is the binding compile target: it is the only baseline that
runs on every row. CI stays on `-cpu max` + GICv3 (full feature exercise,
deterministic); HVF + bare metal use GICv2.

---

## 4. W1 — ISA retarget to the v8.0 floor + the hardening-posture change

**Toolchain** (`cmake/Toolchain-aarch64-thylacine.cmake`):
- `-march=armv8-a` (drop `+lse+pauth+bti`), **keep** `-mno-outline-atomics`.
  Without `+lse` and with outline-atomics off, the compiler inlines **LL/SC**
  (`ldaxr`/`stlxr`) for every C11 `__atomic_*` op — which runs on every v8.0+
  core (A72 included). **LSE is the only true `#UD` wall on A72**; everything
  else degrades to NOPs.
  - **Why LL/SC inline and NOT `-moutline-atomics`** (this corrects the original
    flag wording; user-voted 2026-06-06 after the kernel-idiom research):
    outline-atomics turns every atomic into a `__aarch64_*` **function call**
    that runtime-branches on a `__aarch64_have_lse_atomics` global. That is a
    *userspace* portability technique (glibc / distro binaries that must run on
    any ARMv8 part) — **no serious kernel uses it in-kernel.** The Linux ARM64
    kernel explicitly passes `-mno-outline-atomics` and does its own boot-time
    LSE patching instead (Thylacine already matched Linux here). A kernel that
    detects its CPU at boot should not pay a call+branch on every atomic hot
    path. So the floor is plain inline LL/SC; LSE is restored — with **zero**
    steady-state runtime branch — by the **W1.5** boot-time patcher (§4.5).
- **Keep** `-mbranch-protection=pac-ret+bti`. PAC (`paciasp`/`autiasp`) and BTI
  markers are **HINT-space** (they do not need the `+pauth`/`+bti` arch
  extensions to emit): NOP on v8.0 (A72), **active** on v8.3/v8.5+ (M2). So one
  binary gets full PAC/BTI on capable hardware and harmless NOPs on A72 — **no
  hardening loss on the M2 dev/accel platform.**

**Runtime gating** (the v8.3/v8.5 sysregs that *do* trap on A72):
- The `APIAKey*` MSR programming + `SCTLR_EL1.{EnIA,EnIB,EnDA,EnDB}` → gate on
  `FEAT_PAuth` (`ID_AA64ISAR1_EL1`). `arch/arm64/start.S` (§ start.S:446-461;
  the `SCTLR.EnIA/BT0` writes are already RES0-safe per the author's comment,
  but the `APIAKey` MSRs are undefined on A72 and MUST be gated).
- `SCTLR_EL1.BT0` → gate on `FEAT_BTI` (cleanliness; already RES0-safe).
- `kernel/test/test_hardening.c` → soften the `TEST_ASSERT(pac)`/`TEST_ASSERT(bti)`
  and the `SCTLR.EnIA`/`BT0` asserts from "must be present" to "enabled iff the
  CPU implements it" (the `TEST_EXPECT_EQ`-vs-ID-register consistency checks stay).

**Hardening posture change (ARCH §28 + TOOLING §10 banner — signoff-bearing):**
P1-H currently reads "MMU+W^X+extinction+KASLR+vectors+IRQ+canaries+PAC+BTI+LSE
(required)." Under Lazarus, **PAC / BTI / LSE become runtime-conditional**
(best-effort: active on capable hardware, absent on A72 because the hardware
lacks them). **W^X (I-12), KASLR (I-16), vectors, IRQ-safety, stack canaries,
extinction stay unconditional on every target.** The boot banner already prints
a runtime-detected `features:` line; the static hardening line is reworded to
reflect "conditional where the CPU permits." The honest security delta: on A72,
the PAC/BTI exploit mitigations are unavailable — that is a property of that
silicon, not a regression on M2/QEMU. Documented, not hidden.

Scope: bounded (toolchain + start.S gating + one test file + the banner string +
the doc reconciliations). W1 is **independently sound** — the LL/SC floor runs on
every v8.0+ target with no further work — and lands before W1.5.

---

## 4.5 W1.5 — boot-time LSE alternatives-patching (restore LSE, zero runtime cost)

> **Implemented.** As-built: `docs/lazarus-status.md` (the W1.5 row) +
> `docs/reference/12-hardening.md` (the atomics-patching section). A few
> hundred patch sites (`g_alt_total`); `arch/arm64/alternatives.{c,h}` +
> `arch/arm64/atomic_lse.h` +
> `mmu_patch_text`. The deferred-detail choices below were resolved as: the
> scratch map is a single reserved vmalloc L3 slot (no per-patch allocation);
> the table is C-struct `alt_instr` (12 B, packed) with PC-relative offsets;
> `at s1e1r`+PAR_EL1 gives the site VA->PA. Audit-trigger rows landed
> (ARCH §25.4 + CLAUDE.md).

W1's floor inlines LL/SC for every atomic, which costs LSE-capable cores (M2,
A76, Pi 5) their faster single-instruction atomics. W1.5 restores LSE **with no
steady-state runtime branch and no function call** — the Linux ARM64 model:
compile the LL/SC baseline, then **rewrite the atomic sites to single LSE
instructions at boot** on cores that implement FEAT_LSE. One binary; A72 keeps
LL/SC; capable cores run native LSE; after the boot pass the code is exactly as
if compiled for that CPU.

**What gets patched.** Only the read-modify-write atomics benefit: `exchange`,
`compare_exchange`, `fetch_{add,sub,and,or,xor}` (u32 + u64) — each is an LL/SC
*loop* on v8.0 and a single `swp`/`cas`/`ldadd`/`ldclr`/`ldeor`/`ldset` on LSE.
Plain `load`/`store` are already single `ldar`/`stlr` on v8.0 and are left as
compiler builtins (no LSE benefit, nothing to patch). The RMW primitives are
re-authored (a small `arch/arm64/atomic_lse.h`) as an LL/SC default with the LSE
form recorded in an `.altinstructions`-style table; the spinlock + refcounts +
scheduler RMW sites route through them.

**The framework.** A Thylacine "alternatives" section + a boot-time patcher
(`arch/arm64/alternatives.{c,S}`). Each table entry records (orig_site,
lse_replacement, orig_len, alt_len, feature = FEAT_LSE). The patcher iterates the
table; for each entry, **iff `g_hw_features.atomic`**, it copies the single LSE
instruction over the LL/SC site and NOP-pads the tail to `orig_len` (the LL/SC
sequence is the longer of the two, so the replacement always fits). On an A72
(`!g_hw_features.atomic`) the pass is a no-op and every site stays LL/SC.

**Ordering.** The pass runs in `boot_main` after `hw_features_detect()` and after
the VM allocator is up (it maps a scratch page), and **before `smp_init()`**.
That makes it single-CPU: no other core can be executing a patched site while it
is being rewritten, and secondaries start later with cold I-caches, so they fetch
the already-patched bytes. The primary may have executed the LL/SC form before
the pass and the LSE form after — both are correct on an LSE-capable primary.

**W^X / I-12 — the soundness crux.** The patcher self-modifies `.text` but
**never makes an executable page writable.** It writes through a *transient
RW-not-X alias* of the target page (a fresh scratch VA mapped `RW + PXN/UXN`,
torn down immediately after); the canonical `.text` mapping stays `RO + X` and
the P3-Bb direct-map alias stays `RO + XN` throughout. So **no page is ever
simultaneously writable and executable — I-12 holds at mapping granularity, never
violated, not even momentarily.** After the writes, the pass does the
ARM-ARM-B2-mandated instruction-cache maintenance on each modified line — `dc
cvau` (clean to PoU) → `dsb ish` → `ic ivau` (I-cache invalidate to PoU) → `dsb
ish` → `isb` — so the primary's fetch stream sees the new instructions. The pass
runs IRQ-masked.

**Correctness + fallback.** LL/SC is the always-correct default: an unpatched
site (A72, or any entry the pass skips) is correct by construction, so a patcher
bug fails *safe* (slower, never wrong). The pass is idempotent (run once; a
re-run would re-copy identical bytes). The per-op LL/SC ↔ LSE equivalence is a
local argument (same acquire/release memory-order semantics, same operand
register, same width).

**Audit-bearing.** Self-modifying `.text` + the I-12 interaction. A focused round
prosecutes: the RW-not-X-alias bound (no W&X window at any instant), the
cache-maintenance completeness (every modified line; the point-of-unification
reasoning), the per-op LL/SC ↔ LSE equivalence (memory ordering + width), the
NOP-pad fit (`orig_len ≥ alt_len`), idempotency, and the strictly-pre-`smp_init`
ordering. The `ARCHITECTURE.md §25.4` + `CLAUDE.md` audit-trigger rows land **with
the W1.5 impl** (the W2/W3 pattern; §10). **No new TLA+ spec** — the pass is
sequential boot-time code, its soundness a per-op equivalence + a bounded-window
argument, validated by prose + the audit per the 2026-05-23 spec-to-code
broadening.

**Deferred detail.** The exact op/size/order table, the `atomic_lse.h` macro
shape, and the mmu scratch-map helper are authored at W1.5 implementation time
(the `POUCH-DESIGN.md` deferred-per-chunk-detail pattern). W1.5 is its own
sub-chunk with its own audit; it lands after W1 and before W2.

---

## 5. W2 — the GICv2 driver (the convergent enabler)

Today `arch/arm64/gic.c` autodetects v2/v3 and **extincts** on v2 ("v2 path
deferred (no test target)"). W2 implements the real GICv2 path.

**GICv2 vs GICv3 deltas the driver must handle:**
- **CPU interface is MMIO (GICC), not `ICC_*` sysregs.** Ack = `GICC_IAR` read;
  EOI = `GICC_EOIR` write; priority mask = `GICC_PMR`; enable = `GICC_CTLR`.
  This is precisely what sidesteps the HVF-on-Apple wall (§2 blocker 2).
- **SPI routing via `GICD_ITARGETSR`** (8-bit per-INTID CPU bitmask) vs v3's
  64-bit `GICD_IROUTER`.
- **No redistributors** (v3-only); SGI/PPI config is in the distributor banked
  region.
- **IPI via `GICD_SGIR`** (write target + SGI id) vs v3's `ICC_SGI1R_EL1`
  sysreg. `kernel/smp.c`'s IPI send path gets a v2 variant.

**HVF MMIO timing (blocker 1):** the assessment showed rapid back-to-back GICD
MMIO under HVF/M2 intermittently yields `ISV=0` → QEMU asserts. The GICv2 driver
must be **HVF-MMIO-safe** — candidate mitigation is a barrier (`dsb`) between
consecutive GIC MMIO accesses (interspersing MMIO empirically hid the assert).
**To be validated empirically when W2 lands** — it is the one residual risk on
the HVF-on-M2 deliverable.

**Audit-bearing** (scheduler / IPI surface; I-18 IPI ordering). Follows the
CLAUDE.md audit-trigger discipline for `kernel/smp.c` + the new gic v2 path.

### Implemented (Lazarus W2)

The GICv2 path is built and the v2 detection no longer extincts. As-built:

- **GICC MMIO CPU interface** (`gic_init_v2` maps DTB reg[1]; `gic_acknowledge`
  reads `GICC_IAR`, `gic_eoi` writes `GICC_EOIR`). The GICv2 SGI subtlety:
  `GICC_IAR` carries the **source CPUID** in bits[12:10] for an SGI and
  `GICC_EOIR` must echo it back (ARM IHI 0048B §4.4.5), so the raw IAR is saved
  per-CPU (`g_v2_eoi_token[]`) between ack + EOI. There is exactly one in-flight
  IAR per CPU (handlers run PSTATE.I-masked -- no nesting), so the slot is
  single-writer with no cross-CPU race.
- **SGI/PPI live in the distributor's banked low region** (no redistributor);
  `gic_cpu_config_v2` + `cpu_iface_init_v2` run on each CPU for its own bank.
  `gic_enable_irq`/`gic_disable_irq` route both SGI/PPI and SPI through
  `GICD_ISENABLER(intid/32)`.
- **IPI via `GICD_SGIR`** (TargetListFilter=0, `1 << target` in the
  CPUTargetList byte; v2 caps at 8 CPUs). `kernel/smp.c`'s `smp_resched_others`
  per-target loop is unchanged.
- **HVF-MMIO mitigation**: `v2_w32`/`v2_r32`/`v2_w8` issue a `dsb sy` after every
  GIC access, pacing the trap rate so QEMU/HVF does not see the `isv=0`
  data-abort. **Empirically validated under HVF on M-series**: `gic_init`,
  the timer, IRQ delivery, and the full kernel boot run clean under
  `-accel hvf -machine virt,gic-version=2`.
- **I-5 reservation**: `kobj_mmio_reserve_kernel_ranges` now reserves the GICC
  region for both `arm,cortex-a15-gic` (QEMU virt) and `arm,gic-400` (Pi 4), so
  a CAP_HW_CREATE driver cannot claim the kernel's IRQ ack/EOI registers.

**Virtual-timer switch pulled in (the HVF dev loop needs it too; task #889).**
Reaching `timer_init` under HVF surfaced a second blocker: the EL1 **physical**
timer (CNTP_*) is hypervisor-reserved on Apple Silicon, so an EL1 guest
`CNTP_TVAL_EL0` write reflects as an `EC=0` undefined-instruction exception. The
fix is the architecturally-correct EL1-guest choice -- the **virtual timer**
(`CNTV_CTL/TVAL_EL0`, `CNTVCT_EL0`, INTID 27 / PPI 11), which works on every
substrate (TCG, HVF, and a direct-EL1 bare-metal boot where CNTVOFF=0 makes the
virtual counter equal the physical). One timebase everywhere; `timer.c` reads
`CNTVCT_EL0` and EL0 gets `CNTKCTL_EL1.EL0VCTEN`. (Bare metal W4 inherits this:
the virtual timer is correct there too.)

**Validation**: default (v3, TCG) 716/716; GICv2 (TCG, smp4) 716/716; the SMP
soundness gate (default + UBSan x smp4/smp8) clean; HVF+GICv2 boots the kernel +
runs deep into the userspace suite on real M-series silicon.

**Owed follow-up (#890) -- CLOSED @0124a8f (W3.5):** under HVF the *userspace*
virtio-mmio drivers tripped the same `isv` assert on their back-to-back register
sequence -- a different layer (userspace driver codegen, not the kernel GIC).
Fixed by routing every userspace device-MMIO access through an ISV-safe
single-instruction `ldr`/`str` primitive (see section 8). The HVF boot now
reaches `Thylacine boot OK` (721/721, 0 isv); it was the last gap to a
100%-green HVF boot.

---

## 6. W3 — the software-RNG fallback (the owed seam)

`kernel/random.c` is RNDR-only. On RNDR-less targets (M2, A72) `SYS_GETRANDOM`
+ `/dev/random` return `-1`, so userspace crypto (corvus, libsodium) cannot get
entropy. ROADMAP already specs "RNDR + chacha20 stir"; W3 implements it.

- A **chacha20 CSPRNG** keyed by a mixed seed from: a kernel **virtio-rng**
  driver (the device is *already attached* in `run-vm.sh` and currently
  unconsumed — real host entropy on QEMU/HVF), `cntpct` jitter, the DTB
  `rng-seed`, and on bare metal the **BCM2711 HW RNG** (`/soc/rng`).
- **RNDR, when present, stirs the chacha20 state** (defense-in-depth) rather
  than being the sole source — so the *same* path works on every target.
- `kern_random_bytes` / `kern_random_seeded` / `SYS_GETRANDOM` keep their
  contracts; callers (corvus C-15) are unchanged.

**Audit-bearing** (CSPRNG quality; the secret-on-disk consumers). The seed
sources + the reseed cadence get a focused round.

### Implemented (Lazarus W3)

A ChaCha20 forward-secure CSPRNG (the OpenBSD arc4random construction) replaces
the RNDR-only baseline. The *same* path runs on every substrate; RNDR is one
stir source, not the gate. As-built:

- **The cipher** (`kernel/chacha20.{c,h}`): a pure, allocation-free ChaCha20
  keystream primitive (DJB reference; the spec name is kept like virtio / 9P).
  Pinned by a unit test against the canonical zero-key/nonce/counter vector, so
  the primitive is proven correct independent of any RNG behavior.
- **The CSPRNG** (`kernel/random.c`): a ChaCha20 context re-keys on every
  keystream-buffer drain (fast key erasure -> backtracking resistance; the
  rekey material is zeroed + never served), serves the buffer tail (each byte
  zeroed in place), and pulls a fresh strong stir every ~1 MiB. `g_rng_seeded`
  is the readiness gate -- false until a strong source has ever contributed,
  monotonic, `kern_random_bytes` returns -1 while false (the prior fail-closed
  contract; `SYS_GETRANDOM` is unchanged).
- **Seed sources**: DTB `/chosen` `kaslr-seed` + `rng-seed` (host boot entropy),
  CNTPCT jitter, RNDR when present (stir only), and the kernel **virtio-rng**
  driver (the strong/real source). The boot path seeds the pool early
  (`devrandom_init`, DTB + cntpct) then strong-reseeds from virtio-rng right
  after `virtio_init` (`kernel/main.c`).
- **The kernel virtio-rng driver** (`random_virtio_pull`): the first kernel
  consumer of the P4-F virtio substrate to do a real virtqueue transfer (find
  device-id RNG, negotiate features=0, create queue 0, DRIVER_OK, submit one
  device-writable descriptor, notify, bounded-poll the used ring, copy, then
  reset + destroy to dormant). Serialized by `g_rng_dev_lock`, run OUTSIDE the
  chacha lock. An **all-zero pull is rejected** (treated as a coherency/failure
  signal -> the pool keeps its prior DTB seed), so a non-coherent DMA transport
  fails *safe* (weaker-but-seeded), never silently to zero entropy.
- **I-5**: the kernel now drives the virtio-rng MMIO slot transiently
  (reset-to-dormant between pulls). It is not page-reserved -- the slot shares a
  4 KiB page with up to 7 sibling slots userspace drives -- so it inherits the
  v1.0 virtio-mmio trust posture (kproc-only CAP_HW_CREATE; the kobj_mmio
  overlap check; no userspace driver targets device-id RNG), the same residual
  virtio-blk carries. Documented in `kernel/mmio_handle.c`.

**DMA coherency**: the entropy buffer is Normal cacheable; a `dsb` orders the
used-ring observation before the buffer read -- correct for dma-coherent
virtio-mmio (ARM virt). A non-coherent bare-metal transport (W4) would add a
cache-invalidate; until then the all-zero guard makes a coherency miss fail safe
rather than silently weak.

**Validation**: default (TCG) 721/721 + UBSan 721/721 (+ `chacha20.block_vector`
against the RFC vector + `kern_random.virtio_reseed` driving the device); boot
log shows `random: virtio-rng reseed OK (40 bytes mixed)`; the SMP soundness gate
(default + UBSan x smp4/smp8) clean / 0 corruption; 0 EXTINCTION. **HVF**: the
kernel virtio-rng pull is confirmed working under HVF on M-series (`primed...
awaiting` -> `reseed OK`, since `-cpu host` has no RNDR) and every kernel virtio
test passes; the full-green HVF boot is gated only by the pre-existing #890
(the userspace virtio-blk-probe's `isv` trip, a different layer).

---

## 7. W4 — RPi 400 board bring-up (the culminating milestone; post-v1.0-sized)

The actual platform port. Lands `arch/arm64/rpi400/` (per ARCH §22's
`<platform>/` model; the first concrete instance). **Large + uncertain; does NOT
gate M1.** Components:

- **Boot protocol**: RPi firmware (`start4.elf` + `config.txt` + `armstub8`)
  loads the kernel at EL2; the kernel does the **EL2→EL1 drop** (QEMU virt enters
  at EL1 directly; bare metal does not). `arch/arm64/start.S` grows the EL2 path.
- **UART**: PL011 + the BCM mini-UART (DTB-driven; the existing `uart.c` path
  should mostly carry over — different base, same PL011 programming model).
- **Mailbox**: the BCM2711 property channel (framebuffer alloc, clock/voltage,
  board info). New driver.
- **Storage — the load-bearing gap**: Thylacine has **only** a virtio-blk path
  (stratumd's `bdev_thylacine`). Real hardware has **no virtio** — it is the
  **EMMC2/SD controller**. W4 needs a new block backend (an SD/EMMC `stm_bdev`
  arm, parallel to `bdev_thylacine`) **OR** USB mass storage **OR** network
  boot. **Storage approach is an open W4-design fork** (§9).
- **USB**: DWC2/xHCI for keyboard input.
- **Framebuffer/HDMI**: via the mailbox (feeds Halcyon, post-v1.0).

W4 stays near its planned post-v1.0 slot (ARCH §4.4). M1 is fully exercisable
under QEMU (TCG + HVF) without any of W4.

---

## 8. Milestones + sequencing

- **M1 = W1 + W1.5 + W2 + W3.** The same binary boots **QEMU-TCG** (gic-version=3
  or 2) **AND HVF-on-M2** (gic-version=2, sidestepping the `ICC_*` wall) **AND is
  A72-ISA-ready**. W1 is the LL/SC v8.0 floor (independently sound); W1.5 patches
  LSE back in at boot on capable cores (§4.5). Delivers the faster-iteration win +
  the bare-metal groundwork. Bounded; fully testable under QEMU.
- **M2 = W4.** Actual RPi 400 hardware boot. The culminating, post-v1.0-sized
  platform port.

- **M1's declared END-STATE -- HVF-on-this-host BY DEFAULT (user-decided
  2026-06-06).** The arc is prolonged past W3 to the goal it was always reaching
  for: **the default dev/test loop runs under HVF on the development host (Apple
  Silicon)**, with TCG (`-cpu max` + GICv3) demoted to an *occasional compat
  reference*. TCG is not retired -- it is the only path that exercises RNDR,
  GICv3, and the full `-cpu max` ISA, so it stays the portable, deterministic,
  full-feature check (run before pushes / on GIC / RNDR / atomics-touching
  changes, and on any non-Apple host). **W3.5 -- LANDED 2026-06-06:**
  - **#890 -- the hard gate (FIXED @0124a8f).** The HVF boot now reaches
    `Thylacine boot OK`. The userspace virtio-mmio accessors were emitting
    ISV=0 load/store forms -- pre-indexed writeback (`str wzr,[x13,#0x6c]!`, the
    VirtIO RESET write) + unscaled `stur`/`ldur` -- once LLVM folded the
    `#[inline(always)]` helpers into a register-dense caller; HVF can only
    emulate an MMIO access when `ESR_EL1.ISV=1`. Routed every device-MMIO access
    through a new single-instruction base-only `ldr`/`str` primitive in
    `libthyla_rs::hardware` (the kernel's out-of-line accessors were already
    ISV=1, which is why kernel virtio worked under HVF while userspace tripped).
    721/721 under HVF, 0 isv, 0 EXTINCTION; TCG regression 721/721.
  - **the default flip (DONE).** `tools/run-vm.sh` auto-detects HVF on a capable
    host (Apple Silicon + `kern.hv_support` + qemu-built-with-hvf) and defaults
    to `accel=hvf` + `gic-version=2` + `-cpu host`; every other repo launcher
    (`test.sh` / `ci-smp-gate.sh` / `test-cross-reboot.sh` / `test-fault.sh` /
    `verify-kaslr.sh`) delegates to `run-vm.sh` and inherits the default with no
    change of their own. `THYLACINE_ACCEL=tcg` (+ `make test-tcg` / `make
    run-tcg`) forces the full-emulation compat reference; a non-Apple host
    auto-falls-back to TCG.
  - **full-green HVF validated.** The suite (721/721) + the SMP multi-boot
    soundness gate -- default+UBSan x smp4/smp8, N=10, 40 boots -- run **0
    corruption** under HVF's real-core concurrency, a stronger #860-class race
    test than TCG's round-robin. The gate leans on the multi-boot + soft-warn
    discipline (#865), not single-boot determinism. **M1 is COMPLETE.**
  Deliberately **host-specific** (HVF needs Apple Silicon + macOS) -- the right
  trade for a solo-on-this-host project; TCG remains the fallback for any other
  host / CI.

**Implementation sequencing (user-decided 2026-05-30; re-sequenced 2026-06-05):**
the identity / access / privilege convergence detour (A-2..A-5c) is **complete**.
Lazarus **M1** is now sequenced as the **first of two pre-Utopia arcs** -- Lazarus
M1, then Loom (the io_uring-inverted 9P ring transport) -- both landing before
Phase 7 (Utopia) resumes at U-6d-b. Rationale (user): Utopia brings userspace
apps, and Loom's fast IO should exist before the apps that consume it; Lazarus M1
is the natural first impl (scripture mostly written, the near-term dev-loop win).
**M2 (W4) stays in its post-v1.0 slot** (ROADMAP section 12.1).

---

## 9. Forks — RESOLVED (user signoff 2026-06-05)

- **Arc name**: **"Lazarus" kept** (signed off). The Lazarus-species motif — an
  extinct taxon found alive again; the thylacine leaving the QEMU enclosure for
  real silicon.
- **Hardening posture (W1)**: **PAC / BTI / LSE → runtime-conditional APPROVED**,
  including the TOOLING §10 boot-banner ABI rewording and the ARCH §28 posture
  edit (both land *with* W1, not in the scripture commit). W^X (I-12), KASLR
  (I-16), stack canaries, vectors, IRQ-safety, and extinction stay
  **unconditional** on every target. The A72/Pi400 PAC/BTI gap is a property of
  that silicon, documented not hidden.
- **Atomics lowering (W1 + W1.5) — amended 2026-06-06** (supersedes the original
  `-moutline-atomics` wording in §4). After grounding in the kernel (200+ C11
  `__atomic_*` sites, no compiler-rt linked) + the kernel-idiom research:
  outline-atomics is a *userspace* technique (a call+branch per atomic) that no
  serious kernel uses in-kernel — Linux explicitly passes `-mno-outline-atomics`
  and patches LSE at boot instead. **Resolved (user-voted 2026-06-06):** W1 ships
  the inline **LL/SC v8.0 floor**; **W1.5 builds the Linux-style boot-time LSE
  alternatives-patcher** (§4.5) to restore LSE on capable cores with zero runtime
  cost. The "build the patcher now" path was chosen over "floor now, patcher
  later" per the deeper-build directive ("we don't care about cost or scope, only
  the system").
- **M1 scope + roadmap slot**: **M1 = W1 + W1.5 + W2 + W3 lands now as a
  pre-Utopia arc** (ROADMAP §8.0a); **W4 stays post-v1.0** (ROADMAP §12.1). M1 is
  fully testable under QEMU (TCG + HVF).
- **Board reconciliation**: v8.0 floor + **Pi 400 first** (the board-agnostic
  v8.0 baseline → widest compatibility; both Pis are GIC-400/GICv2). ARCH §4.4
  reconciled at W1 / W4.
- **HVF-on-M2 commitment level**: a target with **one known residual risk**
  (blocker 1, the GICv2-MMIO `ISV=0` timing heisenbug); framed as "expected to
  work, validated empirically at W2," not a hard guarantee.
- **W4 storage** (SD-EMMC driver vs USB-MSC vs netboot): deferred to W4 design.
- **CI baseline (REVERSED 2026-06-06; EXECUTED via W3.5):** the original
  decision -- "stays `-cpu max` + GICv3; HVF + bare metal as additional
  matrices" -- is superseded by the M1 end-state above (§8). **HVF-on-this-host
  (`-cpu host` + GICv2) is now the DEFAULT dev/test loop** (the W3.5 default
  flip landed 2026-06-06: `run-vm.sh` auto-detects HVF, every launcher inherits
  it); **TCG (`-cpu max` + GICv3) is demoted to the occasional compat
  reference** (the unique exerciser of RNDR / GICv3 / full-`-cpu max` ISA, and
  the only host-portable path -- `THYLACINE_ACCEL=tcg` / `make test-tcg`). The
  W3.5 workstream (§8) executed the reversal in full: #890 fixed -> the default
  flip -> full HVF validation (suite + 40-boot SMP gate, 0 corruption).

---

## 10. Scripture cross-references / reconciliations (this commit)

- **New**: this doc (`docs/PORTABILITY.md`) — the binding arc design.
- **`ARCHITECTURE.md`**: §4.4 (first bare-metal board: Pi 5 → reconciled to the
  v8.0 floor + Pi 400 first; Pi 5 secondary), §22 (the `<platform>/` model — this
  arc is its first concrete instance), §28 (the hardening posture: PAC/BTI/LSE →
  runtime-conditional; W^X/KASLR/canaries/vectors/IRQ/extinction stay
  unconditional). Pointer notes added here; the full §28 invariant-table edit
  lands with W1.
- **`ROADMAP.md`**: **registered 2026-06-05** — Lazarus M1 is a scheduled
  pre-Utopia arc (§8.0a, the first of two, ahead of Loom); the bare-metal §12.1
  sprint is reconciled to its W4 (Pi 400 first).
- **`TOOLING.md` §10**: the boot-banner hardening line becomes
  runtime-feature-reflective (lands with W1). The `run-vm.sh`
  `THYLACINE_ACCEL`/`THYLACINE_CPU`/`THYLACINE_GIC` toggles already landed
  (`52d5663`).
- **`CLAUDE.md`** + **`ARCHITECTURE.md` §25.4**: the audit-trigger surface table
  gains the boot-time LSE-patcher (W1.5), the GICv2 driver (W2), and the
  software-RNG (W3) rows — each in the PR that lands that workstream (the W1.5
  patcher is audit-bearing: self-modifying `.text` vs W^X / I-12, §4.5).

No code in this commit. Per-workstream detailed design (especially W2's GICv2
register map + W4's board layout) is authored at implementation time, the way
`POUCH-DESIGN.md` deferred per-chunk detail.
