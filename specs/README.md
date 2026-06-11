# Specs

TLA+ formal specifications for Thylacine OS load-bearing invariants.

Per `docs/ARCHITECTURE.md §25` and `docs/NOVEL.md` Angle #8: nine specs gate-tied to phases. The spec is the source of truth; the implementation is an implementation of the spec. If they disagree, the spec wins.

---

## Setup

Install OpenJDK (`/opt/homebrew/opt/openjdk/bin` on macOS via `brew install openjdk`; `apt-get install default-jdk` on Linux).

Download TLA+ tools:

```bash
curl -sL -o /tmp/tla2tools.jar \
  https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar
```

---

## Run all specs

```bash
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
for s in *.tla; do
    echo "== $s =="
    java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
        -config "${s%.tla}.cfg" "$s" 2>&1 | tail -3
done
```

---

## The spec inventory (as-built; RW-10 reconcile 2026-06-11)

The Phase-0 plan gate-tied nine specs; the committed inventory is **17
modules** (the authoritative table: `docs/ARCHITECTURE.md §25.2`). Three of
the planned nine were dropped per the 2026-05-23 spec-to-code suspension:
`futex.tla` (torpor is prose-validated), `notes.tla` (prose + audit + tests),
`pty.tla` (the PTY mechanism is unbuilt — LS-8, task #952).

| Spec | Landed | Pins |
|---|---|---|
| `scheduler.tla` | P2 | Wait/wake atomicity, IPI ordering, steal no-double-enqueue, eventual-progress liveness |
| `territory.tla` | P2 | Bind cycle-freedom, isolation, mount-refcount consistency |
| `handles.tla` | P2 | Rights ceiling, transfer-only-via-9P, hw non-transferability, caps ceiling |
| `burrow.tla` | P2/P3 | Dual refcount + mapping lifecycle |
| `9p_client.tla` | P4/P5 | Tag uniqueness, fid lifecycle, flow control |
| `poll.tla` | P5 | Missed-wakeup-freedom across N fds |
| `pipe.tla` | P5 | Pipe two-direction wait/wake |
| `tsleep.tla` | P5 | Deadline-bounded Rendez sleep |
| `corvus.tla` | P5 | Key-agent session/identity/elevation protocol |
| `sched_ctxsw.tla` | P5 | Uniform-EL1h kernel (I-21) |
| `sched_oncpu.tla` | deep-smp-review | Diagnostic — reproduces the #860 class |
| `sched_alpha.tla` | deep-smp-review | The SMP-redesign gating model |
| `asid.tla` | RW-1 | ASID generation-rollover safety (I-31) |
| `death_wake.tla` | RW-2 | Death-wake cascade (I-9 generalized + I-24) |
| `loom.tla` | Loom-1 | Completion integrity (I-29), submit pin (I-30) |
| `loom_multishot.tla` | Loom-5 | I-29 generalized to a CQE stream |
| `loom_order.tla` | Loom-5 | LINK/DRAIN ordering + cancel completeness |

---

## Structure

Every spec has:

- `<name>.tla` — the model.
- `<name>.cfg` — the primary config; TLC should find no violations.
- `<name>_buggy.cfg` (optional but recommended) — demonstrates a specific bug at the spec level. TLC should produce a counterexample.

Two configs per spec is the **executable-documentation pattern**: the primary config says "this is how it should work," the buggy config says "this is the specific way it could fail." When a runtime bug is caught that a spec could have caught, add the retroactive spec AND the buggy-config counterexample — future refactors are bound by the test.

---

## Spec-to-code mapping

`SPEC-TO-CODE.md` (in this directory; one section per spec) maps each TLA+ action to a source location:

```markdown
## scheduler.tla

- `EpochEnter` ↔ `kernel/sched.c:sched_enter()` lines 145-189
- `EpochExit` ↔ `kernel/sched.c:sched_exit()` lines 192-220
- `IPIRecv` ↔ `arch/arm64/ipi.c:ipi_handle()` lines 78-103
...
```

CI verifies the mapping is current — file must exist, function must exist, line range must match. Stale mapping = failing CI.

---

## CI integration

CI runs TLC on every PR touching specified files. Failing TLC blocks merge.

(Phase 1 deliverable: CI workflow that detects which specs need to run based on changed files.)

---

## When to write a spec

Per `CLAUDE.md` "Spec-first policy":

If a feature touches a load-bearing invariant — concurrency, commit ordering, territory operations, handle transfer, BURROW lifecycle, 9P pipelining, scheduler IPI, futex atomicity, poll wait/wake, note delivery, PTY semantics, capability checks, anything in `ARCHITECTURE.md §28` Invariants list — the TLA+ model comes BEFORE the implementation.

Pure computation, test helpers, config parsing, CLI glue: skip the spec.

If you cannot articulate the invariant formally, you don't understand it well enough to implement it.

---

## Status

All 17 modules in the inventory table above are committed, each with clean
cfg(s) + buggy-cfg counterexamples (71 buggy cfgs total). `make specs` runs
every module's default clean cfg and fails on any TLC failure; the buggy-cfg
counterexample runs remain a manual per-surface pre-commit discipline (the
tiered automated gate is a tracked task — RW-10 F3). The canonical
action↔source mapping lives in `SPEC-TO-CODE.md`.
