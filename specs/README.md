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

## The nine specs

| # | Spec | Phase | Invariants |
|---|---|---|---|
| 1 | `scheduler.tla` | 2 | EEVDF correctness, IPI ordering, wakeup atomicity, work-stealing fairness |
| 2 | `namespace.tla` | 2 | bind/mount semantics, cycle-freedom, isolation between processes |
| 3 | `handles.tla` | 2 | Rights monotonicity, transfer-via-9P invariant, hardware-handle non-transferability |
| 4 | `vmo.tla` | 3 | Refcount + mapping lifecycle, no-use-after-free |
| 5 | `9p_client.tla` | 4 | Tag uniqueness per session, fid lifecycle, out-of-order completion correctness, flow control |
| 6 | `poll.tla` | 5 | Wait/wake state machine, missed-wakeup-freedom across N fds |
| 7 | `futex.tla` | 5 | FUTEX_WAIT / FUTEX_WAKE atomicity (no wakeup lost between value check and sleep) |
| 8 | `notes.tla` | 5 | Note delivery ordering, signal mask correctness, async safety |
| 9 | `pty.tla` | 5 | Master/slave atomicity, termios state transitions |

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

If a feature touches a load-bearing invariant — concurrency, commit ordering, namespace operations, handle transfer, VMO lifecycle, 9P pipelining, scheduler IPI, futex atomicity, poll wait/wake, note delivery, PTY semantics, capability checks, anything in `ARCHITECTURE.md §28` Invariants list — the TLA+ model comes BEFORE the implementation.

Pure computation, test helpers, config parsing, CLI glue: skip the spec.

If you cannot articulate the invariant formally, you don't understand it well enough to implement it.

---

## Status

Phase 0 complete; no specs written yet. All 9 land per phase per the table above:

- Phase 2 (next): `scheduler.tla`, `namespace.tla`, `handles.tla`.
- Phase 3: `vmo.tla`.
- Phase 4: `9p_client.tla`.
- Phase 5: `poll.tla`, `futex.tla`, `notes.tla`, `pty.tla`.
