# 09 — Test harness [ABSORBED INTO THE VAULT]

Absorbed at the substrate sweep (`chg-2026-08-01-substrate-sweep`). Its
content now lives, code-verified and current, in:

    vault/system/substrate/sub-substrate-gates.md         (test.sh, the
                                                           multi-boot
                                                           classifier, the
                                                           v8.0 floor guard)
    vault/system/substrate/sub-substrate-interactive.md   (LS-CI: the PTY
                                                           harness and its
                                                           fault taxonomy)
    vault/abis/abi-boot-banner.md                         (the two ABI
                                                           strings both
                                                           halves key on)

The in-kernel runner itself (`kernel/test/`) is guest code and belongs with
the kernel; it lands with that sweep.

**What this file got WRONG by the time it was absorbed** — and the way it
was wrong is a mode the earlier sweeps had not seen.

This was a JANUS document. Its spine was frozen at P1-F while two excellent,
current sections were grafted onto it: the #77/#92 `TEST_YIELD_UNTIL`
treatment and the whole LS-CI half. The document was edited repeatedly, with
care, in two places — and the edits never met.

So it stated the suite size twice, in two of its own sections, as **4** and
as **1233**:

| Where | Claim | Truth at absorption |
|---|---|---|
| "Tests catalog (current)" | four tests, listed by name | `g_tests[]` holds **1237**, across 121 files |
| the banner example | `tests: 4/4 PASS`, `phase: P1-F` | — |
| "Implementation > Runner" | "Single-threaded by design at v1.0 (NCPUS = 1 still)"; "When SMP arrives at Phase 2" | SMP landed long ago; the section grafted directly ABOVE it is entirely about smp4/smp8 peer-thread races |
| "Not yet implemented" | the 10000-iteration leak check, tests for scheduler / territory / handle table / 9P client / syscalls | all exist; `phys.leak_10k` sits four lines below `phys.alloc_smoke` |
| "Error paths" | "Boot timeout (10s by default in tools/test.sh)" | 90 s, or 300 s with the GOROOT baked |
| the #92 section | `1232/1233 FAIL` | the only roughly-right number in the file |

`docs/REFERENCE.md`'s index row was a third, independent count — "6 leaf-API
tests at v1.0" — and listed as still-pending the very leak check that sits
in the registry it indexes. Three published numbers for one quantity, in
three places that cross-reference each other, none of them right.

The self-similar micro-instance, in the newest section: "**Four**
portability facts are load-bearing", followed by **six** numbered items. The
list grew; its header did not.

None of this made the new sections wrong — they are the best writing in the
file, and one of them opens by explicitly RETRACTING an unmeasured claim of
its own (#72's "host timing"). The habit of self-correction was live the
whole time. It simply never looked up.

Binding design (unchanged): `docs/TOOLING.md` (the harness contract),
`docs/LIFE-SUPPORT.md` ("LS-CI").
