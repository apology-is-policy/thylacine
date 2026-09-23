# Spec-first policy

> Moved verbatim from `CLAUDE.md` on 2026-09-23 (the CLAUDE.md trim). CLAUDE.md keeps the three-line summary.
> Section headings keep their original levels.

## Spec-first policy (applies to every invariant-bearing feature)

**If a feature touches a load-bearing invariant — concurrency, commit ordering, namespace operations, handle transfer, VMO lifecycle, 9P pipelining, scheduler IPI, futex atomicity, poll wait/wake, note delivery, PTY semantics, capability checks, anything in the §28 Invariants list in ARCHITECTURE.md — the TLA+ model comes BEFORE the implementation.** Write the spec, let TLC chew on it, let invariant violations surface at the spec level where they cost minutes, not at runtime where they cost commits.

Concrete pattern:

1. Propose the feature in prose (problem + shape).
2. Model the mechanism in TLA+ — state, actions, invariants. TLC with small bounds.
3. Iterate until TLC is green under the invariants the implementation must uphold. If a bug shows up, fix the DESIGN before writing code.
4. Where a spec captures a specific bug, also write a `{spec}_buggy.cfg` that fails the invariant under the buggy assumption. Executable documentation of "this is the bug, this is the fix."
5. Implement against the model. Cross-reference each impl step to the corresponding spec action in comments. Keep `specs/SPEC-TO-CODE.md` current.
6. When the impl surfaces a new mechanism the spec didn't cover, extend the spec FIRST, then update the impl.

The committed spec inventory is **36 modules** (`ls specs/*.tla | grep -v TTrace`
-- re-derive it rather than trusting this number; it was stale at 28 until
LINEAGE L-4 measured it, and the ARCH table was stale by the same six rows, so
the two agreed with each other instead of with the tree). The authoritative table lives
in `ARCHITECTURE.md §25.2`): `scheduler` / `territory` / `handles` / `burrow`
/ `9p_client` / `poll` / `pipe` / `tsleep` / `corvus` / `sched_ctxsw` /
`sched_oncpu` / `sched_alpha` / `asid` / `death_wake` / `loom` /
`loom_multishot` / `loom_order` / `cons_poll` / `loom_devgone` / `allowance` /
`net_poll` / `net_poll_teardown` / `weft` / `weft_readiness` /
`sched_tickless` / `sched_rebalance` / `fs_cache` / `debug_stop` / `imperium` / `territory_shed`, each with clean
cfg(s) + buggy-cfg counterexamples (146 buggy cfgs as of 2026-09-21 -- re-derive
with `ls specs/*buggy*.cfg | wc -l` rather than trusting this; it read 100 for
long enough to be off by 21). Three of the Phase-0 planned nine
(`futex.tla`, `notes.tla`, `pty.tla`) were dropped per the 2026-05-23
suspension — torpor + notes are prose-validated; PTY is unbuilt (LS-8, #952).

Features that clearly benefit: scheduler IPI, territory bind/mount, handle transfer, BURROW lifecycle, 9P pipelining, poll wait/wake, futex wait/wake, note delivery, PTY master/slave atomicity.

Features that usually don't (pure computation, test helpers, config parsing, CLI glue): skip the spec; just write + test. Use judgment.

**If you cannot articulate the invariant formally, you don't understand it well enough to implement it.**

### Spec-to-code FULLY suspended until further notice (user-authorized, broadened 2026-05-23)

**This supersedes the 2026-05-21 clean-cfg-only suspension.** The spec-first policy is now **fully suspended** for new sub-chunks: no `specs/*.tla` module is written for an invariant-bearing feature; the invariant is validated by **careful prose reasoning** in the impl's file header + commit message + reference doc, and rigor is provided by the audit round + the runtime test suite. Per the user's 2026-05-23 direction: "let's suspend spec-to-code until further notice, just validate the model by thinking."

The 2026-05-21 record (clean-cfg-only suspension; spec-first design still binding) is preserved as the predecessor; the broadening was triggered at sub-chunk 8 (`pouch-wait-addr`) — the `torpor` wait-on-address primitive — where the I-9-specialized no-lost-wakeup invariant is validated by walking the WAIT/WAKE interleavings with lock-acquire as the serializing event, not by a TLA+ module.

Why broaden: spec-first design served as a thinking aid — the discipline of articulating the invariant in formal syntax. The user has signalled trust that we can validate models by careful prose reasoning. The corvus precedent (a CSPRNG-token verification chunk whose spec wasn't load-bearing in retrospect) was the 2026-05-21 narrow lift; sub-chunk 8 is the explicit broadening.

What stays binding:
- **Buggy-cfg counterexamples on EXISTING specs**: any impl change that touches a mechanism modelled in `specs/` must re-run the relevant buggy cfgs (`scheduler.tla`, `namespace.tla`, `handles.tla`, `vmo.tla`, `9p_client.tla`, `pipe.tla`, `poll.tla`, `corvus.tla`, `burrow.tla`, `tsleep.tla`, ...). They terminate fast and remain pre-commit gates for invariant-detection regressions on already-spec'd subsystems.
- **Audit-trigger surfaces** (CLAUDE.md §"Audit-triggering changes") are unchanged; the formal-audit discipline is now the load-bearing rigor pass for new invariant-bearing work — it does not get suspended.
- **The 21 enumerated invariants** in `ARCHITECTURE.md §28` remain proof obligations; the suspension affects how we verify them, not whether they must hold. Whatever invariant a new sub-chunk introduces must be articulated (in prose) and audited.
- **The audit round + runtime test suite are the rigor floor** for new sub-chunks.

What gets deferred:
- TLA+ modules for new features. Sub-chunk 8 is the worked example — no `specs/futex.tla` written; the no-lost-wakeup model validated by reasoning in `kernel/torpor.c` + `kernel/include/thylacine/torpor.h` + the audit.
- Clean-cfg TLC runs (suspended since 2026-05-21).
- Coverage claims of the form "spec re-verified clean GREEN" per chunk.

When to re-enable: at user direction. The natural re-enabling points: (a) an invariant-bearing feature that genuinely benefits from machine-checked exploration; (b) when wall-clock budgets allow returning the spec-first DESIGN discipline as a thinking aid.

**RE-ENABLED, surface-by-surface (six instances of re-enabling point (a); the
verbatim records moved to `specs/SPEC-TO-CODE.md` "Spec-first re-enablement
record" 2026-08-05):** the SMP scheduler/thread-lifecycle (`sched_oncpu` +
`sched_alpha`, 2026-06-05), the ASID generation-rollover (`asid`, 2026-06-10),
the death-wake cascade (`death_wake`, 2026-06-10), the hardware allowance
(`allowance`, I-34, 2026-06-15), the capability network dataplane (`weft`,
I-37, 2026-06-20), and the debugger stop/continue/step machine (`debug_stop`,
I-39, 2026-07-14). Later re-enablements are recorded per-row in
`docs/AUDIT-TRIGGERS.md`. Re-enabled for THOSE surfaces only; the broader
suspension stands elsewhere.

Cross-link: `memory/feedback_spec_to_code_suspended.md` (project-wide policy record; updated 2026-05-23 to reflect the broadening).

### TLA+ setup

Install OpenJDK (`/opt/homebrew/opt/openjdk/bin` on macOS; `apt-get install default-jdk` on Linux).

Download TLA+ tools:

```bash
curl -sL -o /tmp/tla2tools.jar \
  https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar
```

Run every spec in `specs/`:

```bash
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs
for s in $(ls *.tla | sed 's/\.tla$//'); do
    echo "== $s =="
    java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
        -config "$s.cfg" "$s.tla" 2>&1 | tail -3
done
```

Pre-commit for invariant-bearing features: spec clean + buggy-config counterexample confirmed + all tests pass.

---
