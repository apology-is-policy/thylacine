# 31 — Kernel-internal trivial Devs (P4-B)

The first wave of real Devs. Four single-file leaves implementing the vtable from `30-dev-spoor.md`: `cons` (PL011 UART forward), `null` (bit bucket), `zero` (zero source), and `random` (ARM64 RNDR-backed CSPRNG). All registered by `dev_init()` after devnone; bestiary count goes from 1 (P4-A: devnone) to 5 (P4-B: + cons + null + zero + random).

Per ARCH §9.4 these surfaces are bound contracts: their POSIX-like semantics (writes silently consumed for null, reads produce zeroes for zero, etc.) are part of the v1.0 ABI.

---

## Purpose

P4-B closes the ROADMAP §6.2 exit criteria for the trivial-Dev set:

- "Spoor lifecycle: 10,000 open/read/close cycles on `/dev/null` without leak" — `trivial_devs.devnull_10k_no_leak` runs the full 10K iter count against the now-warm "spoor" SLUB cache.
- "`cat /dev/random` produces non-zero bytes" — `random.read_produces_nonzero` reads 16 bytes via RNDR + verifies at least one byte is non-zero (failure rate 2^-128).

The cons / null / zero / random Devs are also the substrate for the future userspace shell + every program that does line-oriented I/O — `printf("hello\n")` going to stdout becomes (Phase 5+) a 9P write to a Spoor pointing at devcons.

---

## Public API — `<thylacine/dev.h>`

```c
extern struct Dev devcons;        // dc='c'  — UART console (write + blocking RX, A-4c-1)
extern struct Dev devnull;        // dc='0'  — bit bucket
extern struct Dev devzero;        // dc='z'  — produces zeroes
extern struct Dev devrandom;      // dc='r'  — CSPRNG (RNDR-backed)

// Shared helpers — leaf-file Dev plumbing (used by cons/null/zero/random
// and any future kernel-internal leaf Dev with the same lifecycle).
struct Spoor *dev_simple_attach(struct Dev *d, u8 qtype);
struct Spoor *dev_simple_open(struct Spoor *c, int omode);
void          dev_simple_close(struct Spoor *c);
```

Each Dev's vtable is a single-file leaf with `dc` + `name` + 16 ops. Most ops follow the same `dev_simple_*` plumbing; the per-Dev semantics live in `read` / `write` (and, for random, `init` for RNDR detection).

### Per-Dev semantics

| Dev | `read` | `write` | `init` | Notes |
|---|---|---|---|---|
| `cons` ('c') | blocks on the RX ring; returns >= 1 byte (A-4c-1) | forwards to `uart_putc` | no-op | RX landed at A-4c-1: the PL011 RX IRQ (`arch/arm64/uart.c::uart_rx_handler`, SPI 33) fills a bounded ring via `cons_rx_input`; `devcons_read` blocks on a single `Rendez` (single-reader busy-guard; a 2nd concurrent reader gets -1). Ctrl-C (0x03) is cooked-consumed -> a deferred `interrupt` note to the console owner (posted by the `console_mgr` kthread, since the IRQ handler is wakeup-only); a serial BREAK is the A-4c-2 SAK (discarded at A-4c-1). See `cons.h` + IDENTITY-DESIGN.md section 9.8. |
| `null` ('0') | returns 0 (EOF) | returns n (silently consume) | no-op | POSIX `/dev/null` semantics. |
| `zero` ('z') | fills buf with 0; returns n | returns n (silently consume) | no-op | POSIX `/dev/zero` semantics. |
| `random` ('r') | RNDR-backed; returns n on full success, < n on RNDR-retry-exhausted, -1 if RNDR absent | returns n (silently consume; future versions stir into chacha20 state) | detects FEAT_RNG via ID_AA64ISAR0_EL1.RNDR field; sets `g_rndr_available`; prints to boot banner | RNDR / RNDRRS per ARM v8.5+. |

### `dev_simple_open` / `dev_simple_close` — invariants

- `dev_simple_open` is idempotent — re-opening an already-open Spoor just updates `omode`. Plan 9 idiom; matches the `open / close` pair's contract.
- `dev_simple_close` clears `COPEN`. Per-Dev close hooks that release aux resources should call `dev_simple_close` last (or first; order doesn't matter at v1.0 since aux is dev-private).

---

## Implementation

| File | LOC | Scope |
|---|---|---|
| `kernel/null.c` | ~100 | devnull vtable; reads return 0; writes consume. |
| `kernel/zero.c` | ~100 | devzero vtable; reads fill with zeroes; writes consume. |
| `kernel/random.c` | ~190 | devrandom vtable + RNDR detection + retry loop. |
| `kernel/cons.c` | ~240 | devcons vtable; writes forward to `uart_putc`; A-4c-1: RX ring + blocking `devcons_read` + `cons_rx_input` (IRQ-context, wakeup-only) + the `console_mgr` kthread (deferred Ctrl-C `interrupt` post). |
| `arch/arm64/uart.c` | +RX | A-4c-1: `uart_rx_init` (IMSC.RXIM unmask) + `uart_rx_handler` (RX-FIFO drain + `DR.BE` BREAK split -> `cons_rx_input`); `UART_INTID_PL011 = 33` (QEMU-virt SPI fallback) in uart.h. |
| `kernel/dev.c` | +50 | `dev_simple_attach / dev_simple_open / dev_simple_close` helpers + bestiary registration of all four in `dev_init`. |

### RNDR detail (random.c)

ARM64 FEAT_RNG (introduced v8.5; mandatory v9.0+) provides two sysreg-encoded random-number sources:

- `RNDR` (S3_3_C2_C4_0): on-die DRBG output; standard random.
- `RNDRRS` (S3_3_C2_C4_1): reseeded random (force fresh entropy draw); slower.

Both set `PSTATE.NZCV` based on success:
- Success: `NZCV = 0b0000` (Z=0; result valid).
- Failure: `NZCV = 0b0100` (Z=1; retry).

The capture pattern uses `cset` immediately after the `mrs`, with `"cc"` clobber so the compiler treats the flags-register as written:

```c
__asm__ __volatile__(
    "mrs %0, S3_3_C2_C4_0\n"   // RNDR — sets NZ from result
    "cset %w1, ne\n"             // ok = 1 if Z==0 (success)
    : "=r"(val), "=r"(ok)
    :
    : "cc", "memory"
);
```

Per ARM ARM, retry budget is implementation-defined; we cap at 10 attempts. Most reads succeed on the first attempt; transient failure is rare on QEMU virt with `-cpu max`.

`devrandom_init` reads `ID_AA64ISAR0_EL1.RNDR` (bits[63:60]); a value `>= 1` indicates FEAT_RNG support. Boot banner reflects detection:

```
  random: RNDR available (FEAT_RNG)
```

If RNDR is absent, `devrandom.read` returns -1 and the banner says `RNDR ABSENT — /dev/random reads fail`. The ROADMAP §6.2 exit criterion is conditional on FEAT_RNG; in absence, the test skips with a soft note rather than failing.

### chacha20 stir — held to a future sub-chunk

ROADMAP §6.1 specifies `/dev/random` as "ARM RNDR + chacha20 stir". P4-B lands the RNDR baseline. The chacha20 stir is held to a future hardening sub-chunk for two reasons:

1. The exit criterion (`cat /dev/random produces non-zero bytes`) is satisfied by raw RNDR — a hardware DRBG. The stir is defense-in-depth, not correctness.
2. Implementing chacha20 cleanly in C99 freestanding with `_Static_assert`-pinned constants takes ~80 LOC + a careful audit pass. Better to land it as a focused chunk where the diff stays focused.

When the stir chunk lands, the `devrandom_read` API does not change — entropy from each RNDR draw mixes into a chacha20 state, and the reader sees an output stream indistinguishable from raw RNDR for non-cryptanalytic use.

### Bestiary registration

`dev_init()` registers all five Devs in deterministic order:

```c
spoor_init();
dev_register(&devnone);     // dc='-'
dev_register(&devcons);     // dc='c'
dev_register(&devnull);     // dc='0'
dev_register(&devzero);     // dc='z'
dev_register(&devrandom);   // dc='r'
```

Then walks the bestiary, calling each non-NULL `init()`. devrandom's init is the only one that does real work at P4-B — it detects RNDR. devcons / devnull / devzero have no-op init.

### `dev_simple_*` helpers — placement

The helpers live in `kernel/dev.c` next to `dev_register / dev_lookup_*`. Linking via `<thylacine/dev.h>` declarations. Per-Dev source files (`null.c` etc) call them inline; they never compose against the helpers' internal state (the helpers are pure functions over the Spoor passed in).

---

## Spec cross-reference

P4-B is impl-only — no new TLA+ module. The Spoor lifecycle pattern is covered structurally by `burrow.tla`'s refcount discipline (mirrored in `kernel/spoor.c::spoor_unref`); the per-Dev read/write semantics are state-free pure functions over byte buffers and don't fit a TLA+ refinement.

When `specs/9p_client.tla` lands at Phase 4+ (cumulative ROADMAP §7), Spoor cross-session invariants will be modeled there — currently the P4-A/B Devs serve only kernel-internal callers.

---

## Tests

`kernel/test/test_trivial_devs.c` — 12 tests.

### Bestiary

- `trivial_devs.bestiary_smoke` — `dev_count() >= 5`; `dev_lookup_by_dc` and `dev_lookup_by_name` find each of cons / null / zero / random.

### null

- `null.attach_open_close` — full lifecycle; verifies QTFILE qtype on attach; COPEN flag transitions on open/close.
- `null.read_returns_eof` — read of 16 bytes returns 0.
- `null.write_consumes` — write of n bytes returns n; negative n rejected.

### zero

- `zero.read_fills_zeroes` — pre-fills buffer with 0xAB; reads 32 bytes; verifies every byte is zero. NULL buf rejected; n=0 returns 0.
- `zero.write_consumes` — write returns n.

### random

- `random.rndr_available` — sanity check: read returns either 8 (RNDR up) or -1 (RNDR absent). Other values are bugs.
- `random.read_produces_nonzero_bytes` — ROADMAP §6.2 exit criterion: 16-byte read; assert at least one byte non-zero.
- `random.read_varies_across_calls` — two consecutive 16-byte reads must differ (probabilistic; 2^-128 false-fail rate).

### cons

- `cons.write_advances` — write of marker string returns n; n=0 returns 0; NULL/negative rejected.
- `cons.ring_fill_drain` — pushed data bytes drain in FIFO order (A-4c-1).
- `cons.ring_overflow_drop` — a full ring drops the excess; no corruption / overflow.
- `cons.ctrlc_consumed` — Ctrl-C (0x03) sets intr-pending and is NOT enqueued as ring data.
- `cons.break_discarded` — a serial BREAK entry is discarded (A-4c-1; the SAK is A-4c-2).
- `cons.read_busy_guard` — a 2nd concurrent reader returns -1 (single-reader guard), not data.
- `cons.read_bad_args` — NULL buf / n<0 -> -1; n==0 -> 0 (no block).
- `cons.console_owner_intr` — `proc_console_post_interrupt` posts `interrupt` to the live owner; a NULL or zombie owner is a no-op.

The A-4c-1 cons tests drive `cons_rx_input` synthetically (the harness cannot inject UART RX -- IDENTITY-DESIGN.md section 9.8 test note); the real PL011 RX IRQ wiring is validated by boot survival + the interactive `Ctrl-A b` BREAK path.

### Lifecycle stress (ROADMAP §6.2)

- `trivial_devs.devnull_10k_no_leak` — 10000 attach/open/read/write/clunk cycles on devnull. Verifies allocated == freed delta; uses the warm "spoor" SLUB cache from prior tests in the run, comfortably within boot budget. **Closes the ROADMAP §6.2 exit criterion** that P4-A's 1000-iter-on-devnone version held open.

The `random.*` tests skip gracefully when RNDR is unavailable (e.g., a future port to a CPU without FEAT_RNG); the test framework still marks them PASS in that environment.

---

## Status

| Component | State |
|---|---|
| `kernel/null.c` + devnull Dev (dc='0') | Landed (P4-B) |
| `kernel/zero.c` + devzero Dev (dc='z') | Landed (P4-B) |
| `kernel/random.c` + devrandom Dev (dc='r'; RNDR-backed) | Landed (P4-B) |
| `kernel/cons.c` + devcons Dev (dc='c'; UART forward) | Landed (P4-B); blocking RX + Ctrl-C + console-owner added (A-4c-1) |
| `dev_simple_attach / open / close` helpers | Landed (P4-B) |
| Bestiary registration in `dev_init` | Landed (P4-B; 5 Devs total) |
| In-kernel tests | 11 covering bestiary registration, per-Dev contracts, lifecycle stress (10K no-leak on /dev/null). The A-4c-1 console RX tests (8 `cons.*`) live in `kernel/test/test_cons.c`. |
| ROADMAP §6.2 "Spoor lifecycle: 10K cycles on /dev/null" | Closed (P4-B). |
| ROADMAP §6.2 "cat /dev/random produces non-zero bytes" | Closed (P4-B; conditional on FEAT_RNG, which QEMU virt -cpu max provides). |
| chacha20 stir for devrandom | Held to a future hardening sub-chunk (defense-in-depth; API unchanged). |
| /dev/consctl (console mode control) | Held to Phase 5 (PTY + termios surface). |
| /dev/urandom (POSIX-compat alias) | Held to a future sub-chunk (trivial; alias devrandom or implement with weaker mixing). |
| UART RX (devcons.read) | Held to a later P4 sub-chunk after irqfwd (P4-G+). |

---

## Known caveats / footguns

### `devcons.read` returns 0 at v1.0

The console reader is degenerate at v1.0. Programs that block on stdin via 9P-mounted /dev/cons will get immediate EOF. The Phase 5+ PTY infrastructure + Phase 4 userspace input drivers will deliver keystrokes via different namespaces; kernel-side `cons.read` may stay degenerate permanently if the userspace input path covers all interactive shell scenarios.

### `devrandom` is RNDR-only at v1.0

The chacha20 stir per ROADMAP §6.1 is held. Direct RNDR is a hardware DRBG (NIST SP 800-90B-class output) — sound for v1.0's ROADMAP §6.2 exit criterion. The hardening sub-chunk that adds the stir does not change the API; current callers continue to work without modification.

### `devrandom.read` may short-read under transient RNDR failure

The retry budget is 10 attempts per 64-bit word. If RNDR fails 10x in a row on a chunk, `read` returns the partial fill (or -1 if no progress). Callers that need exact-N entropy should retry on short returns. Tests like `random.read_produces_nonzero` cope by skipping when `read` returns -1.

### dc characters: `'-' / 'c' / '0' / 'z' / 'r'`

The Plan 9 device-character space is one byte. We've used 5 so far. Future Phase 4 sub-chunks will add (per ROADMAP §6.1):
- `'p'` for /proc (devproc)
- `'C'` for /ctl (devctl) — uppercase to avoid collision with cons
- `'m'` or another letter for /ramfs (TBD; 'r' is taken)

When P4-E lands ramfs and 'r' is already taken, ramfs will need a different dc. Easiest: 'm' (memfs / ramfs in-memory). A small trip hazard.

### `dev_simple_open` is idempotent

Calling open twice on the same Spoor doesn't error — it just re-records the omode. This matches Plan 9 but may surprise POSIX-trained callers. Tests cover the single-open path; the syscall surface (Phase 5+) may add stricter checking via `EALREADY` or similar.

### Static cast in `devrandom_read` masks PSTATE coupling

The `cset %w1, ne` capture relies on `mrs S3_3_C2_C4_0` being the immediately-preceding instruction setting NZCV. The `"cc"` clobber tells the compiler the flags register is dirty after the asm. Adding any flag-touching instruction between the `mrs` and `cset` (e.g., a `cmp` for retry counter manipulation) would break the capture. Currently safe because the asm block is opaque to the compiler.

---

## References

- `docs/ARCHITECTURE.md` §9.4 — device territory layout (the canonical /dev/ structure).
- `docs/ROADMAP.md` §6.1 — Phase 4 deliverables (kernel-internal Devs); §6.2 — exit criteria.
- `docs/reference/30-dev-spoor.md` — Dev vtable + Spoor lifecycle (the substrate P4-B implements against).
- ARM ARM (DDI 0487): FEAT_RNG (RNDR / RNDRRS encoding + flag semantics).
