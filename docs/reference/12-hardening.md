# 12 — Hardening enablement (P1-H) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-hardening-doc-absorb`).
The kernel's compile-time + runtime hardening posture (ARCH §24): stack canaries,
PAC return-address signing, BTI indirect-branch guards, LSE atomics, stack-clash
protection, NX stack, and the runtime hardware-feature detection the banner
reports. Its content lives, code-verified and current, in:

- the **stack canary + the hardware-feature detection + the hardening banner
  line** — `__stack_chk_guard` (the link-time-magic → KASLR-seeded-runtime-cookie
  lifecycle + the one-cookie-per-frame barrier + `__stack_chk_fail` →
  `extinction`, `kernel/canary.c`, **folded here at this absorption** — it was
  previously unowned), `hw_features_detect` (the ID-register reduction, the
  Linux-shaped published word), and the `hardening:`/`features:` banner lines:

      vault/system/kernel/boot/sub-kernel-boot-sequence.md

- the **PAC keys + BTI + SCTLR enable** (`arch/arm64/start.S` — the PAC key
  install, `SCTLR_EL1` enable bits, the BLR-not-BR long-branch into TTBR1):

      vault/system/kernel/boot/sub-kernel-boot-entry.md

- the **W^X + PTE_GP for BTI on kernel text** (`arch/arm64/mmu.{h,c}` — the PTE
  encoders, the `pte_violates_wxe` W^X predicate, I-12):

      vault/system/kernel/memory/sub-kernel-mmu.md

- the **LSE atomics alternatives-patcher** (the boot-time `ALTERNATIVE()` LSE
  patching):

      vault/system/kernel/boot/sub-kernel-alternatives.md

- **KASLR** (the slide the canary cookie derives from; I-16):

      vault/system/kernel/boot/sub-kernel-kaslr.md

- the **banner ABI** — the `hardening:` / `features:` / `kernel base:` lines the
  tooling parses:

      vault/system/boundary/registries/abi-boot-banner.md   (a PIN)

The hardening *witnesses* (`tools/test-fault.sh`'s seven provokers proving the
canary / W^X / BTI / stack guards actually fire, `tools/verify-kaslr.sh`) are
gate tooling, not a dossier's subject.

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The stack canary (`kernel/canary.c`) was unowned — folded at absorption.**
  Every other hardening surface had a dossier (hwfeat → boot-sequence, PAC/BTI
  start.S → boot-entry, W^X/PTE_GP → mmu, LSE → alternatives, KASLR → kaslr), but
  the canary — its link-time-magic → runtime-cookie lifecycle, the ordering
  barrier that keeps one cookie per frame, and `__stack_chk_fail` → `extinction`
  — was in no dossier. Now folded into `sub-kernel-boot-sequence` (the sibling
  boot-time hardening-init + banner owner), which now claims `canary.c`.
- **The content is distributed** across the six dossiers above; the banner lines
  are informational (`abi-boot-banner`), the enforcement is I-12 (mmu) and I-16
  (kaslr).
