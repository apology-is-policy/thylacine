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
    uart_puts("\n");                  // start on a fresh line
    uart_puts("EXTINCTION: ");        // ABI prefix per TOOLING.md §10
    uart_puts(msg);
    uart_puts("\n");
    _hang();                          // arch/arm64/start.S WFI loop
}
```

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
- (Future) `tools/agent-protocol.md` — full agent-side handling.

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

P1-C boot test (`tools/test.sh`) exercises the success path (boot banner observed). The extinction path isn't exercised yet — no caller fires `extinction()` at P1-C. P1-F's fault-handler tests will deliberately trigger faults and assert the agentic loop sees `EXTINCTION: <expected>` on stdout.

---

## Naming rationale

The function was originally named `panic()` (per the priming docs and standard kernel terminology). At P1-C close it was renamed to `extinction()` for thematic alignment with the project's identity:

- Thylacine = the extinct apex marsupial.
- Plan 9 = the "extinct" OS family the project resurrects.
- Kernel die = the boot's lineage extinct.

Other thematic possibilities considered and held for future signoff:

- `_hang` (the WFI loop) → could become `_torpor` (the marsupial deep-sleep state). Not blocked; held for explicit signoff.
- The audit prosecutor agent → "tracker" or "hunter" instead of "prosecutor"? Stratum uses "prosecutor"; preserving cross-project continuity is more valuable than thematic novelty.

---

## See also

- `docs/reference/01-boot.md` — boot path; `_hang` is the underlying halt loop.
- `docs/reference/03-mmu.md` — MMU + W^X; future fault path will use `pte_violates_wxe(pte)` + `extinction_with_addr(...)`.
- `docs/TOOLING.md §10` — kernel ABI contract.
- `CLAUDE.md` — operational framework; "Boot banner contract" references the EXTINCTION prefix.
