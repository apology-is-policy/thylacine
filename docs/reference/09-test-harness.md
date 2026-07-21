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

**Determinism — and a retracted "host timing" claim (#72).** The kernel is stable at idle: a no-input boot survives indefinitely (verified — a 75 s idle boot stays at the login prompt). This section previously concluded from that: "so an unexpected qemu exit *before* a terminal PASS/FAIL is a host-timing artifact — the TCG-under-oversubscription flake class, never a kernel fault." **That conclusion was false and unmeasured.** Idle-stability does rule out a *guest* fault, but the inference skipped the third possibility — that the harness's own relay died. It had, in every observed case: the VM was alive and `nc` was dead of SIGPIPE, and `lib.exp`'s login `eof` arm *asserted* the unverified cause ("qemu exited before login prompt") that this doc then recorded as settled. `lc_fail` now emits `vm-at-fail` (the VM's `ps` state — a dead child shows `Z*`, since Tcl lazy-reaps and `kill -0` would lie) and `bridge-at-fail` (the relay's exit record), which split the two causes that expect otherwise reports as one indistinguishable EOF.

The wrapper still retries each scenario up to `LS_CI_ATTEMPTS` (default 3) as belt-and-braces; a scenario fails only if ALL attempts fail, and a real regression fails every attempt deterministically. Each failed attempt is now preserved as `build/ls-ci-<name>.attempt<N>.{log,steps}` — previously the retry truncated the very transcript it was retrying over, so a "flake" claim could never be checked against its own evidence. **A retry is a tolerance, not a diagnosis.** Default accel is `THYLACINE_ACCEL=tcg` (portable; matches the LS-1/LS-2 proofs); `hvf` is the fast local override. Env: `LS_CI_BOOT_TIMEOUT` (default 180), `LS_CI_CMD_TIMEOUT` (default 30), `LS_CI_ATTEMPTS` (default 3).

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
