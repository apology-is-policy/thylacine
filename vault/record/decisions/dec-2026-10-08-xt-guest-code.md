---
id: dec-2026-10-08-xt-guest-code
type: dec
title: "XT F10: guest code generation is a per-objtype policy in the host-owned objtype table, strict or permissive"
date: 2026-10-08
status: standing
decided-by: user-vote
affects: [sub-kernel-vivarium, sub-kernel-exec, sub-kernel-caps]
created: 2026-10-08
---
## Fork

Under translation no guest byte is ever host-executable, so I-12 holds at the
page level. But the property I-12's provenance half and I-42 protect, that
code comes only from vouched files or from an explicit code-emission
authority, would hold for every program except translated ones unless the
translator enforces it. Proposed I-48(e) makes guest memory guest-executable
only through exec's vouching or code authority. The open question is who holds
the code authority for anonymous or written guest memory: x86 JITs (Mono-built
Unity titles, Java), UPX-packed executables, DRM, self-modifying 386 code, and
32-bit Windows programs built without `/NXCOMPAT`, which Windows' default
OptIn policy runs with DEP off.

## Research

- **Linux.** A binary without `PT_GNU_STACK` gets `READ_IMPLIES_EXEC`, so
  the binary decides. Thylacine rejects that: an ELF byte may corroborate
  but never decide (I-43).
- **Windows.** A system-wide DEP policy (OptIn, OptOut, AlwaysOn, AlwaysOff),
  set by the administrator, decides with the binary's `NX_COMPAT` flag as the
  request [R].
- **DOSBox-X here.** Its guests self-modify freely because the emulator is a
  sandbox: a DOS guest makes no Thylacine syscalls. A translated guest is not
  sandboxed (P6), so it does not get the same pass.
- **The tree.** The corvus `jit` clearance is re-authenticated and
  deliberately non-propagating (`usr/corvus/src/main.rs:1285-1301`).

## Options

1. **G1 strict:** `CAP_JIT` only, exactly as for a native JIT, so a prompt per
   process.
2. **G2:** a guest-code policy per objtype entry, `strict` or `permissive`, in
   the host-owned objtype table.
3. **G3:** a per-session grant. The launcher takes the clearance once and the
   territory inherits it, which makes the clearance propagate.

## The call

G2 (operator, 2026-10-08, AskUserQuestion), with the proposed defaults:

- `strict` for `(linux, amd64)`;
- `permissive` for `(linux, 386)` and both Windows entries.

Under `strict`, anonymous or written guest memory becomes guest-executable
only with `CAP_JIT`. Under `permissive`, the guest's own requests are honoured
without it. File-backed guest code always passes the vouching rule, and memory
the guest never made executable is never translated (NX emulation), whatever
the policy.

## Rationale

Guest code runs with exactly its Proc's authority (P6), so `permissive` costs
exploit mitigation, not authority: the posture Linux and Windows give these
programs natively. The decision sits where the entitlement does, in one
auditable host-owned file, the operator's act once per entry, not a prompt per
process. Accepted residue, stated: under `permissive`, an x86 JIT needs no
clearance that a native JIT would. G1 would prompt for most 32-bit Windows
games and every Mono-built Unity title. G3 would change the clearance's
deliberate non-propagation.
