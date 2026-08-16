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
    extinction_flush_console();         // #75: drain the cons ring first
    uart_puts("\n");                    // start on a fresh line
    uart_puts("EXTINCTION: ");          // ABI prefix per TOOLING.md §10
    uart_puts(msg);
    uart_puts("\n");
    halls_dump((void *)0);              // the crash dump, under "HALLS:"
    extinction_report_suppressed();     // "peer-cpus-also-died: at least N"
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

**Scope — this closes one of three tearing sources, and the names are
confusingly close.** What is fixed is *extinction vs extinction*. Still open:
**extinction vs a peer's normal console write**, which is the vault's
`seam-extinction-line-unserialized` — the crash emitter holds no console writer
role, and its pre-emit ring flush is a bounded try-lock that *skips* when a peer
holds the ring, so a peer mid-write is precisely the case it declines to handle.
The prescribed lift there is a **try**-acquire of the writer role (never a park).
The third, `IPI_HALT`, would subsume both by stopping peers before printing; it
is a commented-out reservation today.

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
