# 13 — Phase 1 verification infrastructure [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-verification-doc-absorb`).
The Phase-1 verification cadence that maps each `ROADMAP §4.2` exit criterion to a
regression gate: the 10 000-iteration alloc/free leak checks, boot-time
measurement, multi-boot KASLR variability, the UBSan trapping build, and the
deliberate-fault matrix. Its content lives, code-verified and current, in:

- **the deliberate-fault matrix** (`canary_smash`/`wxe_violation`/`bti_fault`,
  `pac_mismatch` deferred) — the audit-critical proof that the hardening
  protections *fire under attack* rather than merely compiling in, **including the
  load-bearing anti-DCE patterns** (the `asm("":"+r"(p))` launder without which
  clang elides the OOB writes and the canary never arms; the `volatile` fp without
  which clang's `bl` devirtualization bypasses BTI) and the #244 lesson (a silent
  provoker reads identically to a passing boot — `test-fault.sh` in no gate is how
  `recursive_kernel_fault` emitting nothing hid for a month):

      vault/system/kernel/boot/sub-kernel-boot-sequence.md   (audit: hard — folded here; owns fault_test.c + test-fault.sh)

- **the boot banner + the KASLR variability gate** — the banner is the tooling
  ABI; `verify-kaslr.sh` is I-16's runtime witness, parsing `KASLR offset 0xN`
  from that banner and PASSing iff >=70% of N boots' offsets are distinct with no
  extinction:

      vault/system/boundary/registries/abi-boot-banner.md   (the parsed line + why a reword breaks the gate)

- **the SMP / UBSan / boot gate harness** — `test.sh` (one boot reached the
  banner), the sanitizer builds, and the classifier that decides what a failure
  MEANS:

      vault/system/substrate/sub-substrate-gates.md

- **the boot-time measurement + the in-kernel leak suite** — `_boot_start_cntpct`
  captured at `.Lel1_main` (kernel-boot time, not firmware-entry), the ~37 ms /
  ~65 ms-UBSan readout vs the 500 ms VISION budget, and the `phys.leak_10k` /
  `slub.leak_10k` single-pointer-cycle tests (the `magazines_drain_all`
  drain-before-compare subtlety) — run by the boot sequence's in-kernel suite:

      vault/system/kernel/boot/sub-kernel-boot-sequence.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **One genuine gap, now folded — the deliberate-fault matrix's anti-DCE detail.**
  `sub-substrate-gates` covers the SMP-*classifier* (the "assumed-vs-known to fire"
  lesson is the classifier's arms, #143/#234/#212), and `sub-kernel-boot-sequence`
  only *referenced* the seven provokers — neither carried the load-bearing
  compiler-defeating detail nor owned `fault_test.c`. Both are now in
  `sub-kernel-boot-sequence` (`chg-2026-09-07-verification-doc-absorb`), which
  claims `fault_test.c` + `test-fault.sh`.
- **Historical / stale content, correctly superseded.** The doc's `THYLACINE_FAULT_TEST`
  variant list was three-of-eight (it says so itself — the authority is
  `test-fault.sh::ALL_VARIANTS`); the `*(pending)*` P1-I-D landing hash was never
  filled; KASAN and a boot-time gate stayed deferred; the "TLA+ specs — Phase 2
  onward" note predates the 34-module spec inventory. These are P1-I history, not
  live surfaces.
- **Residual UNOWNED files (pre-existing, content covered-by-reference).**
  `tools/verify-kaslr.sh`, `kernel/test/test_phys.c`, and `test_slub.c` remain
  unclaimed by any dossier's code list — a pre-existing attribution gap, not this
  absorption's regression. Their *content* is covered (the KASLR gate at
  `abi-boot-banner` + I-16; the leak suite at `sub-kernel-boot-sequence`); a light
  follow-up claim (KASLR gate → the gates/KASLR home; the leak tests → the
  allocator dossier) is owed. Zero code change.
