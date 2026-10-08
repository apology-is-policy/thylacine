# XT-3a closed list (SCTLR_EL1 composition + the EC_WFX arm)

Surface: docs/AUDIT-TRIGGERS.md "Exception entry + EL0-entry trampolines"
(XT-3a addendum). Round r1: cross-family reviewer at max effort, on
`ab768299`, 2026-10-08. 0 P0 / 1 P1 / 1 P2 / 8 P3. Do not re-report these.

## Fixed

- F1 P1: ITD (7) and SED (8) were written 0, but they are RES1 where EL0
  has no AArch32 (Linux kvm/config.c AS_RES1; Apple cores, so HVF, whose
  reset value 0x30900180 sets both). Base is now 0x30D40998; the test
  constant moved with it.
- F2 P2: BT0 was documented as "the BTI enable". BTI is the GP bit of the
  text mappings; BT0/BT1 stop PACIxSP being a landing pad for a BR through a
  register other than x16/x17. Comments corrected (start.S, fault_test.c,
  hwfeat.h, test_hardening.c, dossiers, the audit row). BT1 is now set with
  BT0, gated on FEAT_BTI (Linux bti_enable). Census of the built kernel: 8
  `br xN` (N not 16/17), all intra-function jump tables landing on `bti j`.
- F3 P3: probe and joey wording claim only that the waits return (an nTWI
  trap is conditional on the WFI otherwise waiting).
- F4 P3: the test prints cpu and value on a mismatch, and checks the
  running CPU's live register against its record.
- F5 P3: the boot-entry dossier's before-list includes HVF's 0x30900180.
- F6 P3: stale INIT_SCTLR_EL1_MMU_OFF / 0x30D00800 references renamed
  (uaccess.S, hwfeat.h, diorama, VIVARIUM, AUX-ROADMAP). Historical records
  (phase1-status, handoffs, holotype audits, WINE-STUDY) keep their text.
- F7 P3: isb after the EL2 drop's HCR_EL2 write; the step-5 comment no
  longer overclaims. The E2H-RES1 CNTHCTL layout is a dossier seam (no
  target boots that way).
- F8 P3: every feature bit the write zeroes is in the start.S table with
  its reason (nAA, EnRCTX, IESB, MSCEn, DSSBS, EPAN, CP15BEN).
- F9 P3: debug_step.tla header and DEBUG-FS-DESIGN 5.5 describe a stepped
  instruction the kernel retires. TLC: clean cfg no error; both buggy cfgs
  violated.

## Deferred, tracked

- F10 P3 (second half): the EL0 timer event stream (CNTKCTL_EL1.EVNTEN).
  Task #15, XT-3c, before FEX's WFE spinlocks need it.

## Withdrawn by the reviewer (guarded)

- The tail re-arms SPSR.SS: no; only el0_stop_park's resume arm sets it.
- The arm skips the EL0-return tail: no; it returns through
  .Lel0_sync_return.
- The BTYPE clear is wrong: no; it is what the PE does.
- ELR, IL, WFIT semantics: correct.
- Something runs before the composed write: nothing on any path.
- Another whole-value writer: none in the tree.
- The record is read before the final write, or the publish order is wrong:
  no.
- A vacuous test on TCG: no; every platform reset value differs.
- A trap storm or tickless idle accounting: sound.
- Hypervisor trap priority: EL1 is checked first.
- Behaviour change for existing EL0 code: none.
- AArch32 EL0 reaching the arm: impossible.

## Found during verification (not the round's)

- Task #16: coreutil-smoke's "ps rich table" check failed once in three runs
  on one image. ps read /ctl/procs with a read-to-EOF loop; devctl
  re-renders per read, so the EOF-probing second read returned a newer
  rendering's tail, which parsed as a torn row. ps and cpubench now read
  once. Task #17 holds the systemic design question (snapshot per open
  versus the documented single-read contract) and /ctl/procs' silent
  truncation at 4 KiB.
