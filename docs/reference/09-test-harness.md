# 09 — In-kernel test harness (as-built reference)

A minimal in-kernel test runner for stable leaf APIs. Lives at `kernel/test/`. Each test is a `void(void)` function registered in a sentinel-terminated `g_tests[]` array; `test_run_all` walks the array, runs each test on a per-cpu CONTEXT pointer, prints PASS/FAIL on UART, and reports a summary count to the boot banner. `boot_main` extinctions if any test fails.

Landed alongside / between P1-F and P1-G. The harness covers stable leaf primitives (mix64 avalanche, DTB chosen-seed reading, phys allocator smoke, slub kmem smoke) and explicitly **does not** test internal data-structure invariants of evolving subsystems — those would need rewriting as the subsystems grow.

Scope: `kernel/test/test.{h,c}` (registry + runner), `kernel/test/test_kaslr.c`, `kernel/test/test_dtb.c`, `kernel/test/test_phys.c`, `kernel/test/test_slub.c`. The previously-inline alloc smoke + kmem smoke blocks in `kernel/main.c` move into `test_phys.c` and `test_slub.c` as named test cases.

Reference: `CLAUDE.md` "Regression testing" section (the 10000-iteration leak check at P1-I); `ARCHITECTURE.md §25.2` (TLA+ specs gate-tied per phase, complementary to runtime tests).

---

## Purpose

`tools/test.sh` is a single integration check: boots the kernel under QEMU and matches `Thylacine boot OK` for success, `EXTINCTION:` for failure. Everything in between is opaque to the host-side script.

The in-kernel harness adds **per-feature regression coverage** so a future refactor that breaks (say) `mix64`'s avalanche, or silently corrupts the DTB chosen walk, surfaces with a specific failing test rather than just "boot wedged somewhere." Each test is named for its targeted API (e.g., `kaslr.mix64_avalanche`) and reports its own PASS/FAIL line.

The harness is freestanding-friendly: no constructors, no linker sections, no host runtime, no malloc. New tests are added by editing `g_tests[]` and adding a function — explicit, predictable, easy to bisect.

**What we deliberately don't test (yet)**:

- **Internal data-structure invariants of evolving subsystems**: e.g., the buddy free-list shape, SLUB partial-list discipline, per-thread stack layout. These tests would need rewriting whenever the subsystem grows (P1-G adds GIC dispatch; Phase 2 adds per-CPU active slabs; etc.). We instead test the public smoke flows, which exercise those invariants implicitly.
- **Evolving APIs**: scheduler, territory, handle table, BURROW, 9P client. These get tests when their APIs stabilize at Phase 2/3/4 exit.
- **Concurrency / race conditions**: TSan + TLA+ specs at P1-I and Phase 2 spec gates.
- **Sanitizer-instrumented runs**: ASan + UBSan at P1-I.

---

## Public API

`kernel/test/test.h`:

```c
struct test_case {
    const char *name;       // human-readable identifier (e.g., "kaslr.mix64_avalanche")
    void (*fn)(void);       // test body; calls TEST_ASSERT / TEST_FAIL on failure
    bool failed;            // set by the harness post-run
    const char *fail_msg;
};

extern struct test_case g_tests[];   // sentinel-terminated (last entry has fn == NULL)

void test_run_all(void);             // walks g_tests; prints per-test PASS/FAIL
bool test_all_passed(void);          // true iff every test passed
unsigned test_total(void);           // count of tests that ran
unsigned test_passed(void);
unsigned test_failed(void);

void test_fail(const char *msg);     // call from inside a test on failure

#define TEST_ASSERT(cond, msg)       \
    do { if (!(cond)) { test_fail(msg); return; } } while (0)

#define TEST_EXPECT_EQ(a, b, msg) TEST_ASSERT((a) == (b), msg)
#define TEST_EXPECT_NE(a, b, msg) TEST_ASSERT((a) != (b), msg)
```

The TEST_ASSERT macro short-circuits the current test on failure (returns from the test_case's fn). The runner then sees `failed = true` on that test_case and continues to the next. No setjmp/longjmp; no heap; no exceptions.

---

## Peer-thread observation under SMP — `TEST_YIELD_UNTIL` (#77, #92)

A threaded test creates a helper, `ready()`s it, and needs to observe it reach some
point. The obvious spelling is one `sched()` then an assert on the helper's progress
counter — which encodes an assumption that stopped being true when SMP landed:
**`select_target_cpu` can place the woken peer on another CPU, so it is RUNNABLE but
not yet dispatched when we resume, and the assert reads the pre-wake value.** The
wake was delivered; only the observation was early. I-9 is not at issue.

Two independent witnesses, both 1-in-10, both with the peer proven woken by the
harness's own runnable-dump: `srvconn.teardown_wakes_blocked` on ubsan-smp8
(2026-07-20, RUNNABLE on cpu=4) and `srvconn.role_park_second_reader` on
default-smp4 (2026-07-28, `state=2` on cpu=1). Each such site is a potential
spurious gate RED, which is corrosive out of proportion to its rate: it trains
dismissal of red results.

```c
#define TEST_YIELD_BUDGET_NS (2ull * 1000 * 1000 * 1000)   // 2 s

TEST_YIELD_UNTIL(cond)        // bounded wait; FAILS loudly on exhaustion
TEST_YIELD_UNTIL_SOFT(cond)   // bounded wait; falls through (see below)

extern unsigned g_test_yield_calls;   // invocations
extern unsigned g_test_yield_spun;    // ...that actually had to wait
extern unsigned g_test_yield_deep;    // ...that needed more than one yield
```

Three design points, each of which is a way the naive version goes wrong:

1. **The budget is wall-clock, not an iteration count.** When the run tree is empty
   `sched()` returns immediately, so an N-iteration spin can burn its whole budget in
   microseconds *while the peer is still legitimately running on another CPU* — the
   bounded spin then manufactures the very failure it was added to prevent. Time is
   what is actually being waited on. (The pre-#92 helpers were iteration-bounded at
   10000 and 4.)

2. **Exhaustion fails, naming the stringified condition.** Falling through to the
   downstream assert is how a *wrong* observable stays invisible: the wait does
   nothing, the downstream assert passes on its own, and the site is silently racy
   again with no signal that the guard never guarded.

3. **`_SOFT` exists for exactly one situation** — the wait sits inside a section
   owning global state that must be released (a held console TX role, a stalled UART,
   an armed echo capture). `TEST_ASSERT` *returns from the test function*, so failing
   there skips the release and strands that state for the rest of the boot: a held
   role parks every later console writer; a stalled UART silences the console
   entirely, swallowing the rest of the suite. A rare spurious FAIL is worth catching;
   a dead console is not. `_SOFT` is **not** the general soft option — everywhere else
   the loud form is required, for reason (2).

**Choosing the observable is the actual work, and it is not mechanical.** A wait on
the wrong thing yields a test that spins and asserts nothing. The shapes:

| Shape | Site | Observable |
|---|---|---|
| peer blocked | `sched()` → assert `state == SLEEPING` | `counter >= N && state == SLEEPING` — sound because a not-yet-run thread is RUNNABLE, never SLEEPING |
| peer resumed | `sched()` → assert a result | `counter >= N+1`, or `result != <sentinel>` |
| system drained | `sched()` → assert `sched_runnable_count() == 0` | the same predicate |
| flag consumed | `sched()` → assert a flag cleared | the same predicate — **but a cleared flag is not a completed act**; see below |

**A cleared flag means the act STARTED, not that it FINISHED.** This row is the one
that misleads, so state its precondition rather than the shape: waiting on
`!pending` is a sound observable only where the code that clears the flag
provably goes on to complete the act, with no path that clears and bails and no
window between the clear and the effect the test then reads. That is a property
of the *specific consumer*, and it must be re-established at every site — never
inherited from this table. Where it does not hold, the wait exits early and the
assert reads pre-act state, which is the original race with a guard bolted on
top: strictly worse than the bare `sched()`, because it now looks handled.

The same distinction is why `burrow_handle_count() == 0` is not "the pages were
freed" and why `#130`/`#131` both went wrong by predicting an event from state
sampled beforehand. When the act's completion is what you need, make the
operation *report* it (`burrow_unref_freed`) rather than inferring it from a
flag that merely precedes it.

**Two shapes that `TEST_YIELD_UNTIL` cannot fix, and must not be applied to:**

- **Transient-state asserts.** `wake(); TEST_EXPECT_EQ(peer->state, THREAD_RUNNABLE)`
  races the same dispatch window, but RUNNABLE is *transient* — a spin-until-RUNNABLE
  can wait forever on a peer that already ran. The fix is to assert the non-transient
  property instead: `state != THREAD_SLEEPING`. That is deterministic, because
  `wakeup()` sets RUNNABLE under the rendez lock before the waker returns, and no
  helper in these tests re-sleeps (each either `sched()`-yields → RUNNABLE, or
  `test_kthread_park_terminal`s → EXITING). This is not a weakening: `test_cons.c`'s
  own comment already stated the property as *"a lost wakeup would leave it
  SLEEPING"* — the assert was stricter than its stated intent, and the extra
  strictness was the racy part. A *fresh* thread not yet `ready()`d is a different
  claim and stays `== RUNNABLE`: no CPU can dispatch it, so it is deterministic.

- **Negative asserts** ("the peer has NOT progressed"). `test_poll.c`'s #103 test
  asserts the IRQ producer did not wake the poller — but `cons_rx_input` wakes
  `console_mgr`, which a peer CPU may dispatch inside that window, where it runs the
  deferred relay for an entirely good reason. Waiting is meaningless here; the fix is
  to assert an **implication** with a deliberate read order. `cons_service_deferred`
  is the pending flag's only consumer and always walks the hook list, so "cleared"
  implies "relay ran" implies "poller woken", which makes two one-directional forms
  hold: read the flag first for *armed* (`pending || woken`), read the state first
  for *deferred* (`sleeping || !pending`). The biconditional is racy in **both**
  orders; only the implications hold. Note what that argument rests on — the
  sole-consumer-always-completes property of `cons_service_deferred`, not on
  anything general about flags. Copying the *form* to a site without that
  property gets you the fallacy above; re-derive it, do not port it.

**The waits measure themselves.** Exhaustion is loud, but the opposite failure is
silent: a condition already true on entry exits at once, so the guard is a no-op and
the site is racy again with nothing to show for it — indistinguishable from a healthy
run by any pass/fail signal. `test_run_all` therefore emits

```
    [test] yield-waits: 135 invoked, 92 actually waited, 3 needed >1 yield, 0 child-Proc expiries
```

`spun` proves the guards are live rather than short-circuiting. `deep` is the sharp
one: the loop tests its condition *before* the first `sched()`, so `spun` counts
anything taking even one yield — exactly what the bare `sched()` it replaced already
did. Only a wait needing a **second** yield did work the old form structurally could
not. Before `#134`, `deep` was 0 on an unloaded host at smp4 and smp8; the
witnesses were 1-in-10 *under gate load*, so absence there is expected and is
**not** evidence the race is absent — only that this run did not exercise it.

Since `#134` it reads ~3, and that number must not be over-read. The sites it
converted include child-Proc thunks that wait on a release gate the parent test
holds deliberately for the duration of a handshake — those need many yields *by
design*, so a non-zero `deep` now means "some bounded wait genuinely waited more
than once", not "the peer-not-yet-dispatched race was observed". The original
reading of `deep` still applies only to the test-thread sites it was written for.

**A third shape, which needs a different macro rather than exclusion: a wait
inside a child-Proc entry thunk (`TEST_YIELD_UNTIL_PROC`, #134).** Roughly a
third of the suite's hand-rolled waits do not run on the test thread at all —
they sit in `*_entry` / `*_thunk` functions that `rfork` runs in another Proc.
`TEST_ASSERT` is wrong there in two independent ways, each worse than the
unbounded spin:

- It expands to `test_fail(msg); return;`, and returning from a Proc entry lands
  on `thread_trampoline`'s `1: wfe / b 1b` (`arch/arm64/context.S`) — an entry
  that returns without `exits()` is a kernel-thread bug and the trampoline halts
  it permanently. The thunk's Proc then never exits, so the parent's `wait_pid`
  never returns either: one stuck handshake becomes two parked threads, where the
  `sched()` spin at least kept yielding.
- `test_fail` writes `current_test->failed` / `->fail_msg`, which belong to the
  runner on the boot thread. A child Proc writing them races the runner, and a
  child that expires after the runner moved on reddens whatever test is running
  *then*.

So the thunk form terminates its own Proc instead: it records the condition in a
global and `exits("test-wait-timeout")`. The runner reports it on the right
thread and fails the **suite** (`test_all_passed`), deliberately *without*
blaming `current_test`. That last part was measured, not assumed — sabotaging one
wait failed its test, which returned early and so never released its own spinner
thunks, which expired 2 s later while the runner was two tests further on. The
first draft blamed `current_test` and reddened an innocent bystander: the
`#136`-F2 shape, a diagnostic reaching into state it does not own. A wrong
accusation is worse than none, because the genuine failure is already red on its
own; the named condition identifies the owner exactly for a human reader.

A fourth form exists for one local reason: `test_proc.c`'s two orphan-reparent
tests use `OTI_CHECK` (`test_fail(); goto fail;`) because they own an explicit
`fail:` block that restores the test-init pointer and releases the gates so
helper thunks exit rather than spin. A bare `TEST_YIELD_UNTIL` would `return`
past that cleanup, so those sites use `OTI_YIELD_UNTIL`, which routes an expiry
through the same cleanup every other failure in those tests uses. The general
lesson is that the macro has to match the enclosing function's *failure
convention*, not just its wait shape.

**On censusing this.** The `#134` census found 27 sites where the task that filed
it had found 13, and three whole files it never named. Three successive censuses
each missed a different subset, all for the same reason — the filter was built
from the shape its author already had in mind. `#92`'s caught single-line waits
and missed multi-line conditions; `#134`'s caught multi-line and missed
single-line; the replacement then treated `< N` as evidence of a bound, silently
dropping every `while (g_torpor_done < 2u) sched();` — a *peer progress counter*,
not an iteration bound. The discriminator that survives: a bound is a deadline
(`timer_now_ns`) or a counter the loop itself increments (`spins++ <`), never any
`<`. A census needs a positive control containing one specimen of every shape it
claims to catch **and** negatives it must not flag, run before the census is
believed.

Fail-closed proven by revert-probe: one condition made unsatisfiable →
`1232/1233 FAIL` in 3 s, message naming the condition, suite completing normally.
Note a *broader* sabotage (deleting the real `wakeup(&r->read_rendez)` in
`kernel/pipe.c`) does **not** probe this: it hangs the boot earlier, at
`userspace.attach_probe_round_trip`, which blocks in a real kernel `sleep()` —
correctly untimed — before any guarded site is reached. A revert-probe has to be
confined to the sites under test or it measures something else.

---

## Implementation

### Registry (`kernel/test/test.c`)

A single `struct test_case g_tests[]` array, sentinel-terminated:

```c
struct test_case g_tests[] = {
    { "kaslr.mix64_avalanche",         test_kaslr_mix64_avalanche,         false, NULL },
    { "dtb.chosen_kaslr_seed_present", test_dtb_chosen_kaslr_seed_present, false, NULL },
    { "phys.alloc_smoke",              test_phys_alloc_smoke,              false, NULL },
    { "slub.kmem_smoke",               test_slub_kmem_smoke,               false, NULL },
    { NULL, NULL, false, NULL },          // sentinel
};
```

Adding a test:

1. Write `void test_<name>(void)` in some `kernel/test/test_<subsystem>.c` file.
2. Add `void test_<name>(void);` forward declaration in `test.c`.
3. Add a `g_tests[]` entry before the sentinel.

No constructors, no linker section magic, no auto-discovery. The explicit registration matches Linux's older "linker-table-of-tests" pattern at half the complexity.

### Runner (`test_run_all`)

```c
static struct test_case *current_test;

void test_fail(const char *msg) {
    if (current_test) {
        current_test->failed = true;
        current_test->fail_msg = msg;
    }
}

void test_run_all(void) {
    for (int i = 0; g_tests[i].fn != NULL; i++) {
        current_test = &g_tests[i];
        current_test->failed = false;
        current_test->fail_msg = NULL;

        uart_puts("    [test] ");
        uart_puts(current_test->name);
        uart_puts(" ... ");

        current_test->fn();

        if (current_test->failed) {
            uart_puts("FAIL: ");
            uart_puts(current_test->fail_msg ? current_test->fail_msg : "(no message)");
            uart_puts("\n");
        } else {
            uart_puts("PASS\n");
        }
    }
    current_test = NULL;
}
```

Single-threaded by design at v1.0 (NCPUS = 1 still). When SMP arrives at Phase 2, `current_test` becomes per-CPU or the runner serializes — the contract stays: one test runs at a time on one CPU.

### Leaked global state (`cons_test_release_owned_state`, #130-R2 F2)

Between `current_test->fn()` and the verdict, the runner releases console/UART
state the test left armed, and **fails the test that left it**:

| Bit | State | Cost if leaked |
|---|---|---|
| `CONS_TEST_OWNED_ECHO_CAPTURE` | `cons_test_echo_capture(true)` | `cons_emit`/`cons_emit_wait` divert into a 128-byte buffer and return, so every later `/dev/cons` write is swallowed — the login prompt, the shell, the LS-CI transcript. **Silent**: kernel diagnostics take `cons_diag_byte`, which ignores capture, so the suite keeps printing PASS over a dead userspace console. |
| `CONS_TEST_OWNED_TX_ROLE` | `cons_test_tx_role_hold()` | `cons_tx_role_acquire` parks contenders **untimed** — every later console writer parks forever and the boot hangs. |
| `CONS_TEST_OWNED_MGR_HOLD` | `cons_test_mgr_hold(true)` | `console_mgr` stops servicing deferred work; poll wakes strand. |
| `CONS_TEST_OWNED_READER_BUSY` | `cons_test_set_reader_busy(true)` | the single-reader guard refuses every later `devcons_read`. |
| *(runner-local bit 4)* | `uart_test_tx_stall(true)` | console silent, and every later writer eats the #67 20 ms deadline per byte. |

Each is armed by one call and released by another with the test body in between,
and `TEST_ASSERT` is `test_fail(); return;` — so **one failing assert inside such
a window skips the release**, and the leaked state then destroys the diagnosis of
everything after it. That is the failure this harness exists to prevent, arriving
by a different door than #74/#85/#87.

`TEST_YIELD_UNTIL_SOFT` was the per-site answer, and it only covers the *wait*;
an ordinary assert in the window is the far commoner case, and per-site
discipline cannot cover a site that does not exist yet. Hence the backstop.
Reporting is the point — a silent auto-repair would hide the leak it repaired —
so the runner prints `LEAKED-STATE(<names>)` and reddens the test even when its
own assertions all passed.

Verified by A/B, because a backstop that never fires proves nothing. With a
deliberate failure inside the held-role window: **without** the backstop the boot
hangs at the very next test (`cons.tx_room_wait_and_deadline`, which needs the
role) and the suite never completes; **with** it, the run prints
`LEAKED-STATE(echo-capture,tx-role)`, finishes 1245/1246, and that next test
passes.

### Boot integration

`boot_main` calls `test_run_all` after `slub_init` and `exception_init`:

```c
uart_puts("  tests:\n");
test_run_all();
uart_puts("  tests: ");
uart_putdec(test_passed());
uart_puts("/");
uart_putdec(test_total());
if (test_all_passed()) {
    uart_puts(" PASS\n");
} else {
    uart_puts(" FAIL\n");
    extinction("kernel test suite failed");
}
```

The boot banner now ends with:

```
  tests:
    [test] kaslr.mix64_avalanche ... PASS
    [test] dtb.chosen_kaslr_seed_present ... PASS
    [test] phys.alloc_smoke ... PASS
    [test] slub.kmem_smoke ... PASS
  tests: 4/4 PASS
  phase: P1-F
Thylacine boot OK
```

`tools/test.sh` continues to gate on `Thylacine boot OK` — but if any test fails, the kernel `extinction`s before reaching that line, so test failures surface as integration failures the host script already detects.

---

## Tests catalog (current)

### `kaslr.mix64_avalanche`

Pure-function test of the SipHash-style mix function in `arch/arm64/kaslr.c`. The function is exposed for testing via `kaslr_test_mix64(u64)` (a thin wrapper around the static `mix64`).

Checks:

- `mix64(0) == 0` — all bits clear, no propagation.
- `mix64(1) != 0` — single-bit input must propagate.
- `mix64(1) != 1` — output must not equal input.
- `popcount(mix64(1) ^ mix64(2)) > 16` — avalanche: differing single-bit inputs produce wildly different outputs.
- `mix64(x) == mix64(x)` — pure function (deterministic).

Stable: `mix64`'s implementation hasn't changed since P1-C-extras Part B and is unlikely to. Even if it does, the avalanche property is the canonical correctness check — drift here would mean a regression.

### `dtb.chosen_kaslr_seed_present`

Black-box check of the DTB parser against the live boot DTB. Verifies:

- `dtb_is_ready()` is true post-`phys_init`.
- At least one of `/chosen/kaslr-seed` or `/chosen/rng-seed` is non-zero (otherwise our entropy chain fell back to cntpct, which the banner would already flag).
- `dtb_get_total_size()` returns a sensible value (≥ 200 bytes; < 4 GiB).

Stable: the DTB parser's public API (`dtb_is_ready`, `dtb_get_chosen_kaslr_seed`, `dtb_get_chosen_rng_seed`, `dtb_get_total_size`) has been stable since P1-B / P1-C-extras Part B. The walker internals could change without affecting this test.

### `phys.alloc_smoke`

Refactored from the inline alloc smoke that lived in `boot_main` from P1-D through P1-F. Exercises:

- 256 × `alloc_pages(0, KP_ZERO)` then `free_pages` — magazine[0] refill/drain.
- `alloc_pages(9, KP_ZERO)` → `free_pages` — magazine[1] (2 MiB).
- `alloc_pages(10, 0)` → `free_pages` — non-magazine order; buddy direct.
- `magazines_drain_all()`, then assert `phys_free_pages() == baseline`.

Stable for the public API surface (`alloc_pages` / `free_pages` / `magazines_drain_all` / `phys_free_pages`). Internal buddy / magazine implementation can evolve under the test as long as the public contract holds.

### `slub.kmem_smoke`

Refactored from the inline kmem smoke that lived in `boot_main` post-P1-E. Exercises:

- 1500 × `kmalloc(8) / kfree` — kmalloc-8 cache forced through 3 slab pages.
- Mixed-size kmalloc round-trips at 16, 64, 128, 512, 2048 bytes.
- `kzalloc(8192) / kfree` — bypasses slab, hits `alloc_pages` directly.
- `kmem_cache_create / alloc / free / destroy` round-trip on a custom cache.
- `magazines_drain_all()`, assert `phys_free_pages() == baseline`.

The 1500-element pointer array is `static` (12 KiB) inside `test_slub.c` so it lives in BSS rather than crowding the boot stack.

Stable for the public SLUB API (`kmalloc` / `kfree` / `kzalloc` / `kmem_cache_*`). Internal slab implementation can evolve.

---

## Spec cross-reference

No formal TLA+ spec for the harness itself. The harness is structurally trivial (linear iteration over a static array; no concurrency at v1.0). Future SMP runs of `test_run_all` (Phase 2) will need a per-CPU current-test pointer or a serializing runner — covered by `scheduler.tla` if test_run_all interacts with the scheduler.

The tests themselves are gated on the public APIs of subsystems whose invariants ARE spec-bound:

- `phys.alloc_smoke` exercises buddy + magazines (no spec at v1.0; candidate for `phys.tla` post-v1.0).
- `slub.kmem_smoke` exercises SLUB (no spec at v1.0; candidate for `slub.tla` post-v1.0).

The runtime test catches regressions; the future spec proves correctness in the small. Both layers of defense complement each other.

---

## Tests of the harness itself

There aren't any. The harness is structurally simple enough that visual review + the fact that it runs successfully on every boot is sufficient v1.0 confidence. P1-I introduces a deliberate-failure test that registers a failing test_case and verifies the runner reports FAIL correctly (the inverse of the boot-time PASS check).

---

## Error paths

| Condition | Behavior |
|---|---|
| Test calls `TEST_ASSERT(false, msg)` | `test_fail(msg)` sets `current_test->failed = true` + `fail_msg = msg`; macro `return`s from the test fn. Runner prints `FAIL: <msg>`; continues to next test. |
| Test calls `extinction()` directly | Boot terminates immediately at the extinction point. Runner doesn't get a chance to summarize. (Tests should prefer TEST_ASSERT.) |
| Test infinite-loops | Boot timeout (10s by default in `tools/test.sh`) catches it. |
| Test corrupts kernel state (e.g., kfrees an object it shouldn't) | Subsequent tests may fail due to drift. The drift is caught by the per-test smoke checks (`phys_free_pages() == baseline`). |
| `test_run_all` called twice | Each invocation re-runs every test from scratch. Counters reset per call. |

---

## Performance characteristics

P1-F-test landing measurements (informal, from the boot output):

| Metric | Estimate | Notes |
|---|---|---|
| `test_run_all` total cost | < 100 ms | Dominated by the 1500-iteration kmalloc path in `test_slub.c`. |
| Per-test reporting cost | ~5 µs / test | UART writes for the prefix + name. |
| Kernel ELF size delta | +9 KB | Harness + 4 tests (mostly the test bodies). |
| Boot-time delta | < 100 ms | The actual test work (mostly already in old smoke tests; refactored into the harness with no behavior change). |

The harness adds boot time but keeps the kernel honest. Phase 2's gate on Phase 1 exit (P1-I) requires a clean test-suite run, so this is paid forward.

---

## Status

**Implemented**:

- `kernel/test/test.{h,c}` — registry + runner.
- `kernel/test/test_kaslr.c` — mix64 avalanche.
- `kernel/test/test_dtb.c` — chosen seed presence.
- `kernel/test/test_phys.c` — refactored alloc smoke.
- `kernel/test/test_slub.c` — refactored kmem smoke.
- `arch/arm64/kaslr.{h,c}` — `kaslr_test_mix64` exposed.
- `kernel/main.c` — replaced inline smokes with `test_run_all` call; banner reports `tests: N/N PASS|FAIL`.
- `kernel/CMakeLists.txt` — wires `kernel/test/*.c` into the build.

**Not yet implemented**:

- Host-side test target (compile leaf modules with `-fsanitize=address,undefined` for x86_64 host runs). P1-I deliverable.
- 10000-iteration alloc/free leak check (per ROADMAP §4.2 exit criterion). P1-I.
- TLA+ spec runs gated to test-target builds. P1-I.
- Tests for evolving subsystems: scheduler (Phase 2), territory (Phase 2), handle table (Phase 2), 9P client (Phase 4), POSIX surface (Phase 5), syscalls (Phase 5).
- Deliberate-failure test (verifies the runner reports FAIL correctly). P1-I.

**Landed**: cross-cutting harness addition between P1-F and P1-G; commit `c3f9196`.

---

## Caveats

### Boot-time only

`test_run_all` is called once from `boot_main`. There's no way to re-run tests at v1.0 (no `/proc/sys/kernel/test/run` knob, no signal handler). Phase 2's `/ctl/` territory + the kernel `Dev` infrastructure can expose a re-run knob if needed.

### Tests share the live kernel state

All tests run against the same kernel: same allocator, same DTB parser, same struct page array. A test that corrupts global state pollutes subsequent tests. The smoke-test pattern (capture baseline, perform work, assert baseline restored) catches most drift, but a test that leaks 1 KiB of memory and 1 byte of `g_zone0` accounting wouldn't be caught locally — it'd show up only as a drift in a later test.

For regression coverage of leaf APIs this is fine. For invariant-bearing tests (Phase 2+ scheduler races, territory cycles), the spec-first methodology kicks in; runtime tests are a complement, not the primary defense.

### Stack budget

The 1500-element pointer array in `test_slub.c` is `static` (lives in BSS) so it doesn't crowd the 16 KiB boot stack. Future tests with large stack frames should use `static` for the same reason. Phase 2's per-thread stacks make this a non-issue.

### TEST_ASSERT short-circuits the current test only

A failing TEST_ASSERT returns from the current test_case's fn. Subsequent tests still run. This is the right behavior for regression coverage (we want to see all failing tests in one boot), but a test with multi-step setup can leave global state inconsistent if it bails midway. If this becomes a problem (it hasn't yet at v1.0), tests can wrap their bodies in a do/while + label for cleanup, or use a "test fixture" pattern with explicit teardown.

### `kaslr_test_mix64` is a public symbol now

To test the static `mix64` from outside `kaslr.c`, we expose `kaslr_test_mix64` as a public wrapper. This is conventional (Linux uses `mod_internal_for_testing`-style suffixes). The wrapper carries no production callers; its only job is the test. Future debug introspection (`/ctl/security/entropy_chain_test`) might also surface it.

### Test names use dotted notation

`<subsystem>.<test_name>` (`kaslr.mix64_avalanche`, `phys.alloc_smoke`). The subsystem prefix groups related tests in the runner output and lets a future filter (`test_run_one("kaslr.*")`) target a subset. No filtering at v1.0; the convention is just there for forward compatibility.

---

## Interactive E2E harness (LS-CI) — host/PTY, distinct from the in-kernel harness above

The harness above runs *inside* the kernel at boot. It structurally cannot exercise the **interactive console**: CI feeds QEMU a piped stdin, which hits EOF and closes the `mon:stdio` chardev, so no keystroke is ever delivered. That blind spot let two interactive regressions ship silently — LS-1 (the UART was never master-enabled for RX) and LS-2 (external command stdout/stderr were dropped). **LS-CI** closes it: a host-side `expect`/PTY harness that drives a *real* terminal into the live console.

Layout (added by LS-CI, closes #945):

- `tools/test-interactive.sh` — the wrapper. Optional gate: SKIPs (exit 0) if `expect` is absent. Builds the kernel/ramfs/pool if missing, then runs each `tools/interactive/*.exp` scenario (or one named on the CLI). `make test-interactive`.
- `tools/interactive/lib.exp` — the reusable helper library: `lc_boot` (spawn the VM), `lc_login user pass`, `lc_send line`, `lc_expect pat phase`, `lc_run_expect cmd expected`, `lc_quit`, plus `lc_step`/`lc_pass`/`lc_fail`.
- `tools/interactive/ls-ci.exp` — the headline scenario: login as `michael` (proves LS-1 — reaching the shell banner means every keystroke was received), then assert LS-2 three ways: `echo` stdout (`exec_external`), `echo | tr a-z A-Z` upper-cased stdout (`spawn_pipeline_elements`), `cat /missing` -> `cat:` stderr.

**Four portability facts are load-bearing** (encoded in `lib.exp` + the wrapper; honor them in every new scenario):

1. **Run `expect` under `script(1)`.** macOS expect 5.45 corrupts its own std channels inside `spawn` when its stdout is not a tty (a `>file` redirect OR a pipe) — it aborts with `Tcl_RegisterChannel: duplicate channel names` (SIGABRT) or breaks `puts` with `bad file number`. The wrapper runs `script -q "$transcript" expect -f "$scen" < /dev/null`, which gives expect a controlling PTY, captures the session to the transcript, and propagates the exit code.
2. **`global spawn_id` in any proc that `spawn`s.** `spawn` writes `spawn_id` in the *current* scope; without the `global` declaration in `lc_boot`, the spawn is proc-local and every later proc's `expect`/`send` finds no open spawn and reports a spurious immediate EOF.
3. **Match command OUTPUT, never typed input.** The `ut` line editor redraws the prompt on every keystroke via cursor positioning (`ESC[K` + the colored `/ ⊢ ` + a cursor-forward) and does NOT emit the typed line as plain contiguous bytes. So the typed command is unmatchable; only the command's output (clean text on its own line after Enter) is. Prefer an output token that cannot appear in the typed line anyway — a `tr a-z A-Z` upper-cased token, or a `cat:` stderr prefix.

4. **The serial relay is `serial-bridge.py`, never `nc`** (#72). On Darwin the console rides a UNIX socket bridged into expect (the #66 fix); the relay must survive a full boot-output burst. BSD `nc -U` does not: measured over N=10 single-attempt boots it lost **5 of 10**, each time with the VM still ALIVE (`stat=R+/S+`) and `bridge exit=141` — SIGPIPE. `tools/interactive/serial-bridge.py` is immune by construction (SIGPIPE is Python-default-ignored → catchable `BrokenPipeError`; it **spools** serial→stdout and never blocks on the reader — the #78 rework, point 6; a bounded `select()` park re-checks both endpoints level-triggered, also covering the #66 lost-wakeup class; and stdin EOF does *not* end the relay, where BSD nc exits).
5. **Raise `match_max` before the boot burst** (#72). expect's default match buffer is **2000 bytes**; the boot emits ~110 KB, forcing ~55 discard-and-rescan cycles, and under that churn expect closes its read end mid-stream — the relay then dies as a *consequence* (`reason=stdout-broken`, socket still healthy). Swapping the relay alone left this at 2/10; `match_max 200000` took it to **0/10**. Two distinct causes wearing one symptom, which is why the `reason=` field is load-bearing: `stdout-broken` (reader closed) vs `socket-eof` (guest gone) are the difference between chasing the relay and chasing expect.
6. **The relay spools; it does not back-press the guest** (#78). The original relay wrote stdout *blocking*, on the theory that a full pipe back-pressures the socket read and drops nothing. That reasoning was wrong: under a slow expect reader the back-pressure does not prevent drops — it *causes* them, silently, at the guest. A blocked stdout write stops the relay from draining QEMU's serial socket → QEMU's send buffer fills → the guest UART TX ring fills → the guest drops the remainder of its console write on the kernel **#75 TX deadline** (`kernel/cons.c:518-542`), silently losing whatever token expect is waiting for. The relay now drains the socket aggressively into an in-process spool and writes it out **non-blocking**, so the guest is never stalled. Proven by `tools/interactive/test-serial-bridge.py` (a host-only differential, no QEMU): against a paused reader the blocking relay stalls at ~80 KB, the spool relay accepts a full 4 MB burst. **Residual (tracked, separate):** `reason=stdout-broken` with the guest `R+` still recurs during small post-login output when the host is heavily loaded (e.g. a second QEMU running concurrently) — that is macOS **expect 5.45 closing its channel spuriously**, not the relay and not the guest (the old relay reproduces it identically). `match_max` narrowed but did not eradicate it; run interactive gates with the host otherwise idle.

7. **The console socket is pre-widened by us, because a stalled reader SUSPENDS the guest** (#125). Point 6's "so the guest is never stalled" is true only while the relay is *running*: it cannot drain a socket while it is off-CPU, and the socket holds only **8192 bytes** on macOS against ~117–198 KiB of output per boot. Past that, QEMU's serial write blocks and **QEMU stops executing the guest entirely** — measured with `tools/stall-amplify.sh`, sampling host CPU outside QMP (QMP is served by the same stalled QEMU, so it cannot be the instrument): SIGSTOP the relay → qemu 100% → **2.4%** within ~2 s, held for the whole freeze, then 167% catching up. The guest is *suspended*, and from inside that is indistinguishable from a guest hang — so **a guest that "stopped making progress" in an LS-CI log is not evidence of a guest defect until the consumer is exonerated.** The obvious fix is vacuous: capacity is governed by the **writer's** `SO_SNDBUF`, and the relay is the *reader* — `setsockopt(SO_RCVBUF)` on the relay measurably changes nothing (8192 either way). What works is owning the listener: an accepted connection inherits `SO_SNDBUF` from the listening socket, so `tools/interactive/serial-listen.py` creates it, sets the option before `listen()`, and `exec`s through to the VM, which takes it via `-chardev socket,fd=N,server=on,wait=on,mux=on`. It **wraps** `run-vm.sh` rather than editing it, so the canonical launcher stays byte-identical for `test.sh`, the SMP gate and manual boots. A/B through a real boot with nobody reading for 60 s: **44221 B / no login** (QEMU's listener — and byte-identical to its own 12 s figure, i.e. a hard stop, not slowness) vs **128183 B / login reached** (ours). Regression: property `[4]` of `test-serial-bridge.py` measures the capacity directly (8388608 vs an 8192 control) in ~2 s without a VM. `mon:` is what gave every scenario its `Ctrl-A x` shutdown; the `mux=on` + `-mon` form preserves it — verify *positively* via `EOF clean` in the steps file, since a dead monitor still PASSes (expect times out, the wrapper reaps QEMU).

**Determinism — and a retracted "host timing" claim (#72).** The kernel is stable at idle: a no-input boot survives indefinitely (verified — a 75 s idle boot stays at the login prompt). This section previously concluded from that: "so an unexpected qemu exit *before* a terminal PASS/FAIL is a host-timing artifact — the TCG-under-oversubscription flake class, never a kernel fault." **That conclusion was false and unmeasured.** Idle-stability does rule out a *guest* fault, but the inference skipped the third possibility — that the harness's own relay died. It had, in every observed case: the VM was alive and `nc` was dead of SIGPIPE, and `lib.exp`'s login `eof` arm *asserted* the unverified cause ("qemu exited before login prompt") that this doc then recorded as settled. `lc_fail` now emits `vm-at-fail` (the VM's `ps` state — a dead child shows `Z*`, since Tcl lazy-reaps and `kill -0` would lie) and `bridge-at-fail` (the relay's exit record), which split the two causes that expect otherwise reports as one indistinguishable EOF.

The wrapper still retries each scenario up to `LS_CI_ATTEMPTS` (default 3) as belt-and-braces; a scenario fails only if ALL attempts fail, and a real regression fails every attempt deterministically. Each failed attempt is now preserved as `build/ls-ci-<name>.attempt<N>.{log,steps}` — previously the retry truncated the very transcript it was retrying over, so a "flake" claim could never be checked against its own evidence. **A retry is a tolerance, not a diagnosis.** Default accel is `THYLACINE_ACCEL=tcg` (portable; matches the LS-1/LS-2 proofs); `hvf` is the fast local override. Env: `LS_CI_BOOT_TIMEOUT` (default 180), `LS_CI_CMD_TIMEOUT` (default 30), `LS_CI_ATTEMPTS` (default 3).

**The failure-time state probe (`::lc_fail_probe`, 2026-08-17).** A capture can show *that* an assertion missed and still not say *why*: pty-4's burned retry (echo stops at `sleep `, no `Stopped`, guest alive) fits input truncation, output loss and a lost `^Z` byte equally, and no amount of sensitivity on the same capture separates them — only a second axis does, and the guest can be *asked*. A scenario sets `::lc_fail_probe` to a script; `lc_fail` runs it once, before the kill, only while the VM is alive. `lc_probe_capture bytes secs what` sends (or just listens) and logs every byte the guest answers within `secs` of quiet — control bytes escaped — as one greppable `fail-probe:` step in the `.log` and `.steps` files. It records, asserts nothing, never calls `lc_fail` (cleared before it runs; a bare `expect` inside), and is bounded by the quiet timeout and a chunk cap. `pty-4.exp` arms it around the stop leg (listen / CR / `jobs` / Ctrl-C / `jobs` / an uppercase-pipe liveness token) with the reading table in the file; proven under a sabotaged assertion (`[jobs] -> [1]+ Stopped sleep 30`, `DIAG-ALIVE`) and silent on the passing path.

Not audit-bearing (host test tooling; no kernel surface). Binding design: `docs/LIFE-SUPPORT.md` "LS-CI".

---

## See also

- `docs/reference/00-overview.md` — system-wide layer cake.
- `docs/reference/01-boot.md` — `test_run_all` slot in the boot sequence.
- `docs/reference/04-extinction.md` — the ELE primitive that gates the boot on test pass.
- `docs/reference/05-kaslr.md` — `kaslr_test_mix64` is a P1-F-test addition there.
- `docs/reference/06-allocator.md` + `07-slub.md` — the public APIs the smoke tests exercise.
- `CLAUDE.md` "Regression testing" — the audit-finding-to-test pipeline that future tests will fill.
- `ARCHITECTURE.md §25.2` — TLA+ spec catalog (complementary to runtime tests).
