---
id: adt-81-r1
type: adt
title: "#81 June (O_PATH byte-I/O block, CWALKONLY) focused round"
date: 2026-06-12
scope: [sub-kernel-stalk]
reviewer: fable
model-start: "claude-fable-5"
model-end: "claude-fable-5"
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 1}
findings: []
round-of: chg-2026-06-12-81-cwalkonly
created: 2026-08-01
---
## Scope

The CWALKONLY flag + the three content-I/O gates (commits `fe25495d`
impl + `94bb0a81` close). Fable formal round (MODEL start==end — the
"read the pool master key" framing did not trip the filter) + a
concurrent self-audit. All findings live on surfaces whose dossiers
pend their sweeps (syscall dispatch, loom, spoor), so none is minted as
a note yet — this body is the do-not-re-report record.

## Convergence

**F1 [P2, FIXED]** — the handler `len==0` fast-path bypassed the gate:
the CWALKONLY check lived only in the inner
`sys_read_for_proc`/`sys_write_for_proc`, but the dispatch handlers
have their OWN inline `len==0 → return 0` short-circuit that never
calls the inner helper; the unit test "passed" only because it called
the gated inner (a lying test). The lesson pair with the self-audit's
SA-1 (the impl-test catch: `spoor_clone` propagated the flag to a
CREATED child, denying its own write — fixed by clearing on clone):
*a flag checked at one I/O chokepoint must be checked at EVERY
chokepoint and must not propagate where it shouldn't.* The
two-prosecutor value stated plainly: the self-audit tested the gated
inner; Fable traced the real dispatch. **F2 [P3, FIXED]** — the Loom
payload content opcodes (READ/WRITE/READDIR) carried no CWALKONLY
check; non-exploitable at v1.0 (rested on the SERVER's Tlopen
enforcement) but gated in-kernel as defense-in-depth; metadata +
mutation opcodes deliberately stay allowed (the fstat-equivalent and
create-from-O_PATH-base classes). Fable independently confirmed the
entire self-audit SOUND set: set-site completeness (two O_PATH
creators, flag set before handle_alloc), handle_dup shares the SAME
Spoor (not a clone-bypass), the exec slurp is kernel-internal,
fstat/lseek/wstat by design, no other byte path. Verified on the close
SHA: 847/847 + the joey probe (len>0 AND len==0 both denied) + the SMP
gate on both SHAs (the flag is set-once-pre-publish — SMP-inert).
