# 04 — Extinction (kernel ELE) reference

When the kernel hits an unrecoverable condition, it calls `extinction(msg)` (or `extinction_with_addr(msg, addr)`), which prints `EXTINCTION: <msg>` to UART and halts forever. The boot's lineage is over; only a fresh boot continues. The TOOLING-side counterpart in `tools/test.sh` and the agentic loop watches for `^EXTINCTION:` on the UART stream.

The thematic name is intentional. The thylacine — *Thylacinus cynocephalus* — was declared extinct in 1936; the project carries the name because Plan 9's ideas suffered a similar fate. When a kernel boot dies, that boot's "lineage" is extinct; the metaphor is fitting.

Scope: `kernel/include/thylacine/extinction.h`, `kernel/extinction.c`. Caller integration appears as fault handlers and assert-points land at P1-F+.

Reference: `TOOLING.md §10` (kernel ABI contract), `CLAUDE.md` "Boot banner contract."

---

## Public API

```c
// Print "EXTINCTION: <msg>\n" to UART and halt forever.
void extinction(const char *msg) __attribute__((noreturn));

// Print "EXTINCTION: <msg> 0x<addr>\n" and halt.
void extinction_with_addr(const char *msg, uintptr_t addr) __attribute__((noreturn));

// Convenience: extinction(...) if the expression is false.
#define ASSERT_OR_DIE(expr, msg) \
    do { if (!(expr)) extinction(msg ": " #expr); } while (0)
```

All three never return. The `noreturn` attribute lets the compiler optimize callers (drop the implicit fall-through) and surfaces "unreachable code after extinction" warnings if anything follows.

---

## Implementation

```c
void extinction(const char *msg) {
    extinction_reentry_guard(msg);      // #214: per-CPU depth; >=2 parks
    if (!extinction_claim_console())    // one emitter per boot
        extinction_park_suppressed();
    extinction_claim_ring();            // the cons TX ring lock: bounded try, then HELD FOREVER
    uart_puts("\n");                    // start on a fresh line
    uart_puts("EXTINCTION: ");          // ABI prefix per TOOLING.md §10
    uart_puts(msg);
    uart_puts("\n");
    halls_dump((void *)0);              // the crash dump, under "HALLS:"
    extinction_report_suppressed();     // "peer-cpus-also-died: at least N"
    extinction_report_ring();           // "console-ring: held ..." | "NOT held (... may be torn)"
    _torpor();                          // arch/arm64/start.S WFI loop
}
```

### The console claim — why one CPU emits and the others go silent

The re-entrancy guard is deliberately **per-CPU**, so two CPUs extincting
concurrently both proceed past it. Each then emits the banner as four separate
unlocked `uart_puts` calls, and interleaved those can leave the prefix no longer
starting a line. That is not cosmetic: every consumer anchors the match
(`grep -q "^EXTINCTION:"` in `tools/test-fault.sh`; the multi-boot classifier
keys its corruption class on it), so a torn banner is **a real extinction the
harness cannot see** — fail-open on the channel the whole test discipline
trusts.

`extinction_claim_console` is a single `__atomic_exchange_n`. Three properties,
each deliberate:

- **A raw atomic, not a kernel spinlock.** This runs on a machine that is
  already dying, often from inside a fault handler; a primitive carrying
  lock-order assertions or debug bookkeeping could itself fault and turn a
  diagnosable crash into a silent one.
- **Try once, never spin.** The winner never releases — every path through
  `extinction()` ends in `_torpor()` — so a spin loop could only burn its bound
  and park anyway.
- **Losers park without emitting a byte.** Dropping a loser's banner beats
  garbling the winner's, because the two failure modes are not symmetric: a torn
  line can be classified as a clean boot, while a missing one leaves the guest
  visibly hung or timed out. The loud failure is the correct one to take.

A loser increments a counter the winner reports *after* `halls_dump`, so the ABI
line stays first and pristine. That note deliberately contains neither
`EXTINCTION` nor `EXTINCTION:` and starts no line at column 0 — some consumers
match the bare token, and a peer note shaped like a banner would be counted as a
second extinction by exactly the tooling this mechanism exists to keep honest.

**Scope — three tearing sources, and the names are confusingly close.** The
console word closes *extinction vs extinction*. The second — **extinction vs a
peer's normal console write** (the vault's `seam-extinction-line-unserialized`)
— is closed by the ring claim below (2026-08-18). The third, `IPI_HALT`, would
subsume both by stopping peers before printing; it is a commented-out
reservation today, and it is also what would cover the residual the ring claim
leaves (next section).

### The ring claim — why the winner takes the console TX ring lock and never lets go

Every steady-state console producer pushes its unit into the TX ring under
`g_cons_tx.lock` (`cons_tx_push_bulk`: `SYS_PUTS` through the writer role, the
echo, `cons_diag_line`), and every ring→FIFO drain pops under the same lock (the
TX IRQ arm on cpu0, `cons_tx_kick` on the pusher's own CPU, `cons_tx_flush`).
So a CPU that **holds** that lock owns the wire: nothing else can put a byte in
the FIFO between the winner's `"\n"` and its `"EXTINCTION: ...\n"`. The
predecessor (`cons_tx_flush_for_dump`) took the lock by ONE trylock, drained,
and **released** it — a peer mid-push made it skip, and even when it flushed,
the release let the next push land inside the banner. The one case it declined
to handle was precisely the tear.

`cons_tx_claim_for_dump` (kernel/cons.c) is the winner's claim of that lock:

- **The ring lock, not the writer role** the seam prescribed. The role
  (`g_cons_tx.writing`) serializes whole `cons_output_write` calls, but the
  DRAIN never consults it (main#144): bytes a peer already pushed would still
  pop into the FIFO from cpu0's TX IRQ or a peer's kick. The ring lock covers
  the push *and* the pop, and a healthy peer holds it only for one bounded
  push or one FIFO-depth drain (microseconds), where the role is held across a
  whole write, room-waits included. A prescribed remedy is a hypothesis; this
  one was checked against the drain path and refined.
- **A bounded spin, not try-once** — the opposite of the console word, on
  purpose. The word's holder is a dying peer that never releases, so a spin
  there could only burn its bound. The ring's holder is a *healthy* peer that
  WILL release, so a try-once fails in exactly the case it exists for. Bound:
  20 ms wall clock (the #67 shape) with a `1<<20` iteration backstop for a
  frozen or pre-init timer. Past the bound the winner emits anyway — torn beats
  silent, the same asymmetry the console word chose — and reports the miss
  after the dump (`console-ring: NOT held ...`).
- **Raw, never counted** (`spin_trylock_raw`, the second legitimate raw user
  after `sched()`'s handoff): the counted variants read `current_thread()`
  through TPIDR_EL1, state a crash may have destroyed — the
  `recursive_kernel_fault` fault variant runs with TPIDR = `0xdead...`, and the
  predecessor's counted `spin_trylock` faulted on `t->magic` inside the dump
  (measured on the base tree, below), turning the expected banner into the
  `el1-sync recursion` one.
- **IRQs masked first, never restored.** With the ring lock held on this CPU,
  its own TX IRQ arm (`cons_tx_drain_from_irq` → `spin_lock_irqsave`) would
  self-deadlock — a silent hang in place of the dump. The caller parks in
  `_torpor`, so nothing is owed back.
- **Held forever on success.** Peers that reach for the ring after this spin
  IRQ-masked on a dead machine — the intended effect (a poor man's `IPI_HALT`
  for anyone touching the console). Hence the same interface split as the
  console word: `cons_tx_claim_core` runs on a *caller-supplied* lock so a test
  can exercise the bound and the return-holding contract, and nothing exports a
  claim of the LIVE ring — a test that took it would silence the console for
  every test after it (`cons.ring_unclaimed_on_clean_boot` guards this).
- **The flush on success is the full bounded ring** (the healthy
  `cons_tx_flush` loop shape through the non-spinning `uart_tx_try_putc`, no
  TXIM re-eval, no nested lock): with the lock held forever, whatever the
  pre-crash ring still holds when the flush stops is lost, where the
  predecessor's one-FIFO flush let the rest trickle out after the dump.
  Bounded: each pass empties the ring, or moves ≥1 byte after a bounded FIFO
  wait, or finds the FIFO still full after that wait (a stalled host consumer —
  nobody is reading) and stops.
- **The owner re-entering is answered "held" at once** (per-CPU owner record):
  the classic seed — `extinction` → `halls_dump` faults → … →
  `el1_sync_runaway` on the same CPU — must not burn the bound on a lock it
  already holds and then report a miss it did not have.

**The other emitter of the ABI line.** `arch/arm64/exception.c::el1_sync_runaway`
(the #214 EL1h-sync recursion guard's terminal banner) prints `"EXTINCTION: el1-sync
recursion ..."` without going through `extinction()`. Until 2026-08-18 it was
enrolled in NEITHER serializer — the console-word claim (2026-08-16) never
named it, and neither did the vault's `abi-boot-banner` mirror set (`quaestor
owner` flagged it as matching the literal outside the set). It now takes both:
`extinction_console_claim_or_own()` (claim, or confirm this CPU already owns
the word — the runaway is reached from a chain that may have claimed it at
depth 1; a PEER holding it means a peer is dumping, so it parks silent like any
loser, counted) and then `cons_tx_claim_for_dump()`, whose miss it reports
after its own banner on the same terms `extinction()` uses. It was found by the
compile, not by the census: the runaway path called the deleted
`cons_tx_flush_for_dump`. **A rename is a census that cannot lie.**

**And it is exercised by NO test — stated because the gap is easy to miss and
this chunk just added three calls to it (main#246).** In a healthy kernel the
#806 guard extincts at the *second* kernel fault, so `g_el1_sync_depth` never
reaches 3; reaching the runaway requires the extinction/Halls path itself to
fault — which is exactly what the base tree was doing (above), and what this
fix removed. So the fix deleted the only thing that was reaching this path, and
everything on it is static-audited only. "No current path drives it" is the
latent-P1 trap, not a safety argument. A variant that injects a fault *inside*
`halls_dump` would drive it; it belongs with main#245 (wiring `test-fault.sh`
into a gate at all).

**Residual, named.** The ring lock reaches every writer that goes THROUGH the
ring. Kernel diagnostics that still write the UART directly (`uart_puts` at
steady state — `sched.c`'s runnable-dump, `syscall.c`'s vivarium
unserved-syscall / `viv-trace` lines, `exec.c`'s exec-failure line,
`9p_client.c`'s ownerless-frame line) sit outside it and can still land inside
the banner from a peer CPU. The cons.h contract already says such callers must
use `cons_diag_line`; converting them is the enqueued follow-up (main#243);
`IPI_HALT` would cover them structurally.

`uart_puts` uses the runtime `pl011_base` (per `docs/reference/01-boot.md`). At P1-C the base is DTB-driven; if DTB parsing failed the fallback `0x09000000` (QEMU virt) is used. Either way, the message reaches the host UART tty before the kernel halts — `tools/test.sh` will pattern-match it.

`extinction_with_addr` is the same with an additional hex print of the address (uses `uart_puthex64` from `arch/arm64/uart.c`).

`ASSERT_OR_DIE(expr, msg)` is a macro so the failed expression's source text appears in the message — `#expr` stringifies it. Example:

```c
ASSERT_OR_DIE(handle->refcount > 0, "handle in clean-up path");
// On failure prints: "EXTINCTION: handle in clean-up path: handle->refcount > 0"
```

---

## TOOLING ABI contract

The literal string `"EXTINCTION: "` (12 bytes: 11 ASCII + space) is the agentic-loop's catastrophic-failure-detection signal. Per `TOOLING.md §10`:

> Any output matching `/^EXTINCTION:/` on the UART stream triggers the agent to: record the message, restore the last good snapshot, and report to the human before retrying.

This contract is shared between:

- `kernel/extinction.c` — emits the prefix.
- `tools/test.sh` — `EXTINCTION_MARKER="EXTINCTION:"`; pattern-match.
- `TOOLING.md §10` — documents the contract.
- `CLAUDE.md` "Boot banner contract" — references the same.
- Full agent-side handling lives in `TOOLING.md §10` itself. (This line previously promised a future `tools/agent-protocol.md`; it was planned in Phase 1, never written, and the promise was retired rather than kept — main#244.)

Changing the prefix requires coordinated updates across all five surfaces in the same commit. Don't.

---

## Callers (current and planned)

| Caller | Status | Why |
|---|---|---|
| (none) | P1-C | The infrastructure is in place but no caller has called `extinction()` yet. The first deliberate caller will be the fault handler at P1-F. |
| Page-fault handler | P1-F | Synchronous abort with cause = translation fault on a kernel address → `extinction_with_addr("kernel page fault", ESR_EL1)`. |
| W^X violation | P1-F | Catches a kernel write to RX or execute from RW. `extinction_with_addr("W^X violation", PTE)`. |
| Stack overflow | P1-C-extras | Page fault on the boot-stack guard page → `extinction("kernel stack overflow")`. |
| Unhandled IRQ at boot | P1-F | Spurious interrupt before GIC init completes. |
| Unrecoverable DTB parse failure | (open) | Arguably should be a fatal extinction rather than a degraded boot; deferred until we have a use case. |

---

## Tests

`tools/test-fault.sh` boots with a fault compiled in and asserts the expected
message — seven variants, matched as full strings and anchored at line start.

Two in-suite legs cover the console claim:

- `extinction.claim_word_exactly_one_winner` — the exactly-one-winner
  bookkeeping, on a **local** word. **It is sequential and the property is a
  race**, so a non-atomic `if (*w) return 0; *w = 1; return 1;` passes it
  identically: it pins the bookkeeping, not the atomicity (which comes from
  `__atomic_exchange_n`, a compiler intrinsic). The uncovered regression is
  concurrent, and covering it honestly needs a multi-CPU fault-injection arm
  with a **forced** interleaving — without forcing it the pre-fix build garbles
  only sometimes, and a discriminator that fails only sometimes is not a
  regression test. Tracked, not skipped quietly.
- `extinction.console_unclaimed_on_clean_boot` — guards the hazard the claim
  itself introduces. Nothing ever releases the console, so anything claiming it
  spuriously silences every later extinction banner in the boot: the same
  fail-open arriving from the other side. This is why the interface exports the
  claim core to be run on a *caller-supplied* word and exports no way to claim
  the live one.

Both were sabotage-verified: removing the exclusion fails the first on "a second
claimant must lose"; pre-claiming the console at boot fails the second. Suite
1367/1367 → 1365/1367 under sabotage.

Three in-suite legs cover the ring claim (kernel/test/test_cons.c; the same
core-on-a-caller-supplied-lock split as the console word):

- `cons.ring_claim_core_returns_holding` — a free lock is claimed and the
  claimant HOLDS it (a counted `spin_trylock` second taker fails); released, it
  is takeable again. A core that reported success without acquiring passes a
  bare "returned true" check and tears exactly as before.
- `cons.ring_claim_core_bounded_when_held` — a held lock is NOT claimed and
  the miss returns within its bound (measured < 5 s; the load-bearing half is
  that it returns at all — an unbounded spin hangs the suite, loud).
- `cons.ring_unclaimed_on_clean_boot` — the live ring has not been claimed by
  suite time (a claim would be a dead console), plus a live-lock liveness probe
  through `cons_test_tx_ring_peek`.

Sequential, like the word's tests: they pin the bookkeeping (held means held,
missed means returned), not the race. **Sabotage-verified, both arms in one
run** (1427/1427 → 1424/1427, each failure naming its own assertion): S1 made
`cons_tx_claim_core` `return true` without acquiring — the first two legs fail
on "returns HOLDING: a second taker must fail" and "a held lock must NOT be
claimed"; S2 set `g_cons_tx_claimed_for_dump` inside `cons_tx_push_bulk` (so
the flag reads true by suite time without actually wedging the console) — the
third fails on "the live ring must be unclaimed on a healthy boot". S2 pins
that the leg reads the real flag, not that a genuine claim is survivable;
a genuine boot-time claim would hang the console, which is the hazard, not a
test.

`tools/test-fault.sh` (all seven variants) is the E2E witness that the banner
still arrives intact through the new path — and it is more than a witness here:
see the next section.

### What the base tree was doing — `recursive_kernel_fault` printed nothing at all

Measured on `f525cea3` with the change stashed, one variable at a time:

| tree | result |
|---|---|
| base `f525cea3` | **TIMEOUT (60 s)** — the guest's last line is `fault-test: invoking recursive_kernel_fault...` |
| this change (raw try-spin) | PASS — `EXTINCTION: recursive kernel fault (handler re-entered) 0xdead000000000000` |
| this change, counted `spin_trylock` restored | TIMEOUT, symptom byte-identical to base |

The variant installs `TPIDR_EL1 = 0xdead000000000000` on purpose — a wild
`current_thread()` is the whole point of the #806 regression. `extinction()`
flushes the console ring **before** the banner (deliberately: causal order), and
that flush took the lock with the **counted** `spin_trylock` → `spin_preempt_inc`
→ `current_thread()->magic` → **fault, inside the extinction path**. The nested
EL1-sync faults climb `g_el1_sync_depth` to 3 → `el1_sync_runaway` → which
called the *same* flush → faults again → depth 4 → the `depth > MAX` arm parks
**silently**. So the one fault variant whose entire premise is a destroyed
`current_thread()` could not print its own banner, and emitted **zero bytes**
rather than a wrong message.

Broken since `ed56f21f` (#75 P1-F, 2026-07-20) met `ce7bd352` (#360's counted
spinlocks, 2026-07-04) — about a month, unobserved, because **`test-fault.sh` is
wired into no gate** (grep-proven over the Makefile, `ci-smp-gate.sh`,
`test.sh`, `test-interactive.sh` and `.github`: it is manual-only). Filed as
main#244 (the defect, closed here) and main#245 (the ungated harness, open).

The generalizable rule, and why `spin_trylock_raw` exists: **a dying-machine
path may not call a primitive that reads state the crash may have destroyed.**
#360 retrofitted the `current_thread()` deref under *every* existing
`spin_trylock` caller, including one on the extinction path, without anyone
re-asking whether that caller could survive it.

---

## Naming rationale

The function was originally named `panic()` (per the priming docs and standard kernel terminology). At P1-C close it was renamed to `extinction()` for thematic alignment with the project's identity:

- Thylacine = the extinct apex marsupial.
- Plan 9 = the "extinct" OS family the project resurrects.
- Kernel die = the boot's lineage extinct.

Other thematic possibilities considered and held for future signoff:

- `_torpor` (the WFI loop) → could become `_torpor` (the marsupial deep-sleep state). Not blocked; held for explicit signoff.
- The audit prosecutor agent → "tracker" or "hunter" instead of "prosecutor"? Stratum uses "prosecutor"; preserving cross-project continuity is more valuable than thematic novelty.

---

## See also

- `docs/reference/01-boot.md` — boot path; `_torpor` is the underlying halt loop.
- `docs/reference/03-mmu.md` — MMU + W^X; future fault path will use `pte_violates_wxe(pte)` + `extinction_with_addr(...)`.
- `docs/TOOLING.md §10` — kernel ABI contract.
- `CLAUDE.md` — operational framework; "Boot banner contract" references the EXTINCTION prefix.
