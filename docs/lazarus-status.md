# Lazarus M1 — status

The **first of two pre-Utopia arcs** (Lazarus M1, then Loom). Binding design:
`docs/PORTABILITY.md` (signed off + sequenced 2026-06-05; atomics amended
2026-06-06). Roadmap registration: `ROADMAP.md §8.0a`.

## TL;DR

Lazarus makes the **same kernel binary** run on three substrates — QEMU `virt`
under TCG (the portable, full-feature compat reference), QEMU `virt` under HVF on
Apple Silicon (**the default dev loop -- the declared M1 end-state**), and
bare-metal ARM64 (first board: Raspberry Pi 400) — by targeting the **ARMv8.0-A
ISA floor** and lighting later features up at runtime. **M1 = W1 + W1.5 + W2 + W3
+ W3.5 -- COMPLETE (2026-06-06).** W3.5 fixed #890 (the userspace virtio-mmio
`isv` trip) and flipped the repo's QEMU launchers to HVF-by-default: HVF now runs
the boot/test loop on this host (auto-detected; `run-vm.sh` defaults to
`accel=hvf` + GICv2 + `-cpu host`, every other launcher inherits it), TCG is the
occasional compat check (`THYLACINE_ACCEL=tcg` / `make test-tcg`). Validated
full-green HVF: suite 721/721 + the 40-boot SMP soundness gate, 0 corruption.
**W4** (the actual Pi 400 board port = M2) stays post-v1.0 (`ROADMAP.md §12.1`).

## Landed sub-chunks

| Commit | Sub-chunk | What | Tests |
|---|---|---|---|
| `29c0492` | scripture | Atomics amendment: LL/SC floor (drop `-moutline-atomics`) + new W1.5 boot-time LSE-patcher design. `PORTABILITY.md §4` + new §4.5 + `ROADMAP.md §8.0a`. | (docs only) |
| `9a43a9d` | **W1** | v8.0 ISA floor + hardening posture. Toolchain `-march=armv8-a` (drop `+lse+pauth+bti`; keep `-mno-outline-atomics` + `pac-ret+bti`) → inline LL/SC atomics, runs on every v8.0 core. `start.S` `.arch_extension pauth` + FEAT_PAuth/FEAT_BTI runtime gating of the PAC-key MSRs + SCTLR enables (kept leaf). `halls_strip_pac` `xpaci` gated on a direct `ID_AA64ISAR1_EL1` read (latent UNDEF-on-v8.0 bug surfaced + fixed). `test_hardening` softened to enabled-iff-CPU (F25 anti-performative preserved). Runtime-reflective boot banner. ARCH §15.5/§24.4 + TOOLING §10 + CLAUDE banner + ref 01-boot/12-hardening reconciled. | 714/714 default; SMP gate 40/40 (default+UBSan × smp4/smp8) 0 corruption; 0 EXTINCTION |
| `173ffe5` | **W1.5** | Boot-time LSE alternatives-patcher. Restores single-instruction LSE on FEAT_LSE cores at zero steady-state cost (the Linux `apply_alternatives` model). New `arch/arm64/alternatives.{c,h}` (the `ALTERNATIVE()` macro + the `apply_alternatives()` boot pass) + `arch/arm64/atomic_lse.h` (LL/SC-default patchable RMW primitives) + `mmu_patch_text` (writes `.text` via a transient RW-not-X alias → W^X / I-12 never violated) + the `.altinstructions`/`.altinstr_replacement` `.rodata` sections (reloc-free, KASLR-independent). Routes the spinlock test-and-set + the Spoor/SrvConn refcounts + the scheduler steal-rotate. `apply_alternatives` runs single-CPU, full-DAIF-masked, before `smp_init`. Routes every spinlock/refcount/scheduler RMW site (`g_alt_total`, a few hundred inlined); on `-cpu max` all patched to LSE. Audit-bearing (ARCH §25.4 + CLAUDE.md rows added). | 716/716 default (+`alternatives.patch_applied` + `alternatives.atomics_correct`); SMP gate (default+UBSan × smp4/smp8) 0 corruption; 0 EXTINCTION |
| `f19e985` + `c76f4ac` | **W3** | ChaCha20 forward-secure kernel CSPRNG (the OpenBSD arc4random construction) replacing the RNDR-only baseline -- the same path runs on RNDR-less HVF/A72, fixing the entropy break W2's HVF dev loop exposed. New `kernel/chacha20.{c,h}` (pure cipher, RFC-8439-vector-pinned) + `kernel/random.c` (rekey-on-drain fast-key-erasure + ~1 MiB strong-reseed cadence + DTB/CNTPCT/RNDR sources + the kernel **virtio-rng driver** `random_virtio_pull` -- the first kernel virtqueue data transfer). Boot seeds early (DTB+cntpct) then strong-reseeds from virtio-rng after `virtio_init`; `g_rng_seeded` gates on an UNOBSERVED source (RNDR/virtio), DTB domain-separated from KASLR; an all-zero pull is rejected (non-coherent DMA fails safe). Audit-bearing (CSPRNG quality + seeds + cadence); ARCH §25.4 + CLAUDE.md rows. **Audit: R1 0 P0 / 2 P1 / 3 P2 / 3 P3 all fixed -> DIRTY close -> R2 on the fixes CLEAN 0 P0 / 0 P1 / 0 P2 / 2 P3** (2 Opus prosecutors + self-audit; the 2 R2 P3 were doc-drift, fixed; `audit_w3_closed_list.md`). | 721/721 default + UBSan (0 runtime-err); the SMP gate (default+UBSan × smp4/smp8) 0 corruption (default-smp8 PASS 10/10 in isolation; a gate-vs-agent host-oversub timeout was #894, not W3); **HVF: the kernel virtio-rng pull confirmed working** (`reseed OK`; full-green HVF boot gated by the pre-existing #890 userspace virtio-blk-probe `isv` trip, a different layer); 0 EXTINCTION. boot: `random: virtio-rng reseed OK (40 bytes mixed)` |
| `cba064b` | **W2** | GICv2 driver + EL1 virtual-timer switch (the HVF-on-Apple enabler). Real GICv2 path in `arch/arm64/gic.c` (GICC MMIO CPU interface; `gic_acknowledge`/`gic_eoi` with the per-CPU CPUID-preserving `g_v2_eoi_token`; `GICD_SGIR` IPI; banked SGI/PPI distributor; `v2_w32`/`v2_r32`/`v2_w8` dsb-after-each HVF mitigation) + the GICC I-5 reservation (both v2 compats). The MMIO CPU interface sidesteps the GICv3-distributor `isv` assertion -- **empirically validated under HVF on M-series**. Pulled in the virtual-timer switch (#889): under HVF the EL1 physical timer is hypervisor-reserved (CNTP_TVAL write → EC=0 undef), so `timer.c` moves to CNTV_*/CNTVCT + INTID 27 (the EL1-guest timer, correct on TCG/HVF/bare-metal). Audit-bearing (I-18); ARCH §25.4 + CLAUDE.md rows added. **Audit: CLEAN 0 P0/0 P1/2 P2/2 P3, all fixed** (one Opus prosecutor + self-audit; F1/F2 = two v2 distributor RMW/write functions bypassed the dsb mitigation -> routed through the v2 accessors; F3 stale comments; F4 dead-but-documented EOI fallback; `audit_w2_closed_list.md`). | 716/716 default + GICv2-TCG 716/716 + SMP gate (default+UBSan × smp4/smp8) 0 corruption + the HVF+GICv2 kernel boot; 0 EXTINCTION. Owed: #890 (userspace virtio-mmio `isv` under HVF) |
| `0124a8f` + `372d3d5` | **W3.5** | HVF-on-this-host BY DEFAULT -- the M1 end-state. Two parts. **(a) #890 fix (`0124a8f`):** the userspace virtio-mmio drivers tripped QEMU's `assert(isv)` under HVF -- their `#[inline(always)]` `read_volatile`/`write_volatile` helpers folded into a register-dense caller and LLVM emitted ISV=0 forms (pre-indexed writeback `str wzr,[x13,#0x6c]!` = the RESET write, + unscaled `stur`/`ldur`); a hypervisor needs `ESR_EL1.ISV=1` to decode an emulated MMIO access. New ISV-safe `mmio_read8/16/32/64` + `mmio_write*` in `libthyla_rs::hardware` (single-instruction base-only `ldr`/`str` via inline asm; offset pre-applied in Rust); the typed `Mmio` + all 7 native virtio-* driver helpers delegate (the kernel's out-of-line accessors were already ISV=1). objdump confirms every `rs_main` MMIO access is now base-only `[xN]`. **(b) the default flip:** `run-vm.sh` auto-detects HVF on a capable host (Darwin + `kern.hv_support` + qemu-built-with-hvf) -> `accel=hvf` + GICv2 + `-cpu host`; every other launcher delegates to it and inherits the default. `THYLACINE_ACCEL=tcg` (+ `make test-tcg` / `run-tcg`) forces the compat reference; non-Apple auto-falls-back to TCG. NOT audit-bearing (codegen-shape fix, identical semantics; the MMIO accessor is not on the §25.4 trigger list). | **HVF**: suite 721/721 + 0 isv + 0 EXTINCTION (virtio-blk-probe PASS) + the 40-boot SMP soundness gate (default+UBSan × smp4/smp8, N=10) **0 corruption** under real-core concurrency. **TCG regression**: 721/721 + 0 EXTINCTION. boot resolves `accel=hvf cpu=host gic=v2`. |

## Remaining work

**M1 is COMPLETE (W1 + W1.5 + W2 + W3 + W3.5).** The only Lazarus item left is
the post-v1.0 board port:

- **W4 — RPi 400 board bring-up** (post-v1.0; `ROADMAP.md §12.1`). EL2→EL1 drop,
  BCM mailbox, a new SD/EMMC block backend (the load-bearing gap), USB input,
  mailbox framebuffer. `arch/arm64/rpi400/`. Storage-backend fork open
  (`PORTABILITY.md §9`).

## Build + verify

```bash
tools/build.sh kernel                 # the v8.0-floor kernel
tools/test.sh                         # boot + in-kernel suite (DEFAULT: HVF + GICv2 on a capable host; W3.5)
tools/ci-smp-gate.sh                  # SMP soundness gate (default+UBSan x smp4/smp8, N>=10; HVF by default)
make test-tcg                         # the same suite, forced to full-emulation TCG (-cpu max + GICv3) compat
```

Since W3.5 (`run-vm.sh` auto-detect, 2026-06-06) **HVF on this host is the
default** for every launcher (`-cpu host` + GICv2); a non-Apple host auto-falls
back to TCG. The **TCG compat reference** (`-cpu max` + GICv3 -- the unique
exerciser of RNDR / GICv3 / full-ISA) is one flag away: `THYLACINE_ACCEL=tcg
tools/test.sh` or `make test-tcg`. The legacy explicit toggles still work on
`run-vm.sh`: `THYLACINE_ACCEL` / `THYLACINE_CPU` / `THYLACINE_GIC`.

## Trip hazards

- **PAC/BTI/LSE are runtime-conditional** since W1 — not "always on." The boot
  banner's `hardening:` line lists the unconditional set; the `features:` line
  carries the live per-CPU truth. The agentic-loop tooling matches only
  `Thylacine boot OK` / `EXTINCTION:` (TOOLING §10), so the reworded banner lines
  are ABI-safe.
- **`.arch_extension pauth`** in `start.S` + the inline asm in `halls.c` lets the
  FEAT_PAuth instructions (`AP*KEY*` MSRs, `xpaci`) assemble under
  `-march=armv8-a`; their **execution** is separately runtime-gated. A new file
  that hand-writes a v8.1+/v8.3+ instruction must do the same (assemble-gate +
  execute-gate), or the build fails under the v8.0 floor (`xpaci` was exactly
  this — a latent UNDEF-on-A72 bug W1 caught).
- **The atomics are inline LL/SC**, not LSE and not outline-atomics. W1.5 patches
  LSE back in. Do NOT re-add `+lse` to `-march` (it would `#UD` on A72) or
  `-moutline-atomics` (the userspace call+branch form no kernel uses).
- **W2 is audit-bearing** (I-18 IPI ordering) and **W3 is audit-bearing** (CSPRNG
  quality). Each gets a focused round + an audit-trigger row.

## References

- `docs/PORTABILITY.md` — the binding arc design (W1 §4, W1.5 §4.5, W2 §5, W3 §6,
  W4 §7, forks §9).
- `docs/ROADMAP.md §8.0a` — pre-Utopia arc registration; §12.1 (W4).
- `docs/ARCHITECTURE.md` §4.4 (board), §15.5 (hardening posture), §22 (the
  `<platform>/` model), §24.4 (LSE atomics + the W1.5 patcher), §25.4
  (audit-trigger surfaces — W1.5/W2/W3 rows land with each).
- `docs/reference/12-hardening.md` — the as-built hardening reference.
