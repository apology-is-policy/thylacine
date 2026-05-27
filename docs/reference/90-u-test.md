# 90 — /u-test (libthyla-rs uplift integration smoke)

**Status**: Landed at U-2-test, closing the U-2 arc (commit `*(pending)*`).

## Purpose

`/u-test` is the **cumulative integration smoke** for the libthyla-rs uplift. Where `/alloc-smoke` isolates each U-2X module's surface in its own block (one section per landed sub-chunk; FAIL paths exercised individually), `/u-test` runs **composed cross-module flows** — the patterns a future Utopia builtin will reach for.

The two binaries complement:
- `alloc-smoke` proves each module standalone.
- `u-test` proves they compose under realistic usage.

Both run at boot via joey-spawned orchestration; u-test runs immediately after alloc-smoke. If either binary returns non-zero, the boot fails loudly with a tagged FAIL log line.

## Flow inventory

Six composed flows, each printing `u-test: <flow> OK` on success; the final `u-test: all OK` precedes a clean `exits(0)`:

| Flow | Composed modules | What it validates |
|------|-----------------|--------------------|
| 1. alloc + fs + io | `t::alloc` (heap), `t::handle` (RAII), `t::fs::File`, `t::io::Read::read_to_end` | Open `/system.key`, slurp into a heap-backed `Vec<u8>`, validate exact size (3656 B) + that bytes are non-zero. |
| 2. process + pipe | `t::process::pipe`, `t::io::{Read, Write}`, `t::process::Command`, `Stdio::Piped`, `Child::wait`, `ExitStatus::success` | (a) pipe() round-trip: write a payload via the writer + read it back via the reader + drop the writer + read EOF. (b) Spawn `/hello-rs` with all three stdio ends Piped + drop them on the parent side + wait + verify exit clean. |
| 3. notes + poll + time | `t::notes::Notes::open_self`, `Notes::try_read`, `notes::send`, `t::poll::PollSet`, `PollEvents::READ`, `PollTimeout::Millis`, `core::time::Duration` | Open self-notes, drain any synthetic posts from flow 2's reap, poll with a 5 ms timeout (expect empty), self-send `"interrupt"`, poll again (expect one READ-ready event), drain via `notes_fd.read()` + verify the note name. |
| 4. thread + torpor + time | `t::thread::spawn_raw`, `set_tid_address`, `exit_self`, `join_tid`, `t::torpor::wait`, `wake_one`, `WaitResult`, `t::time::sleep` | Spawn a child thread; child wakes a shared `AtomicU32` via `torpor::wake_one` + exits via `exit_self`; parent waits on the atomic via `torpor::wait` (Duration timeout), then joins via `thread::join_tid` on the clear-child-tid word. |
| 5. ninep codec | `t::ninep` pack/parse primitives | Hand-build a Tversion frame via the pack primitives + back-patch the header size, peek_header to verify, parse_tversion to verify fields, build an Rversion response, peek_header on the response. |
| 6. hardware + cap | `t::hardware::{Mmio, Irq, Dma}`, `t::err::Error::InvalidArgument` | Without CAP_HW_CREATE, every constructor returns `Err(Error::InvalidArgument)`. Validates the error-mapping helper + the wrappers' early-return discipline. |

## Implementation

`usr/u-test/src/main.rs` (~340 LOC, U-2-test).

### Layout

A single `rs_main` that calls each flow in sequence; each flow returns `Result<(), i64>` where `Err(rc)` bails immediately with `return rc`. The first FAIL prints a tagged diagnostic + the binary exits non-zero; the orchestrating joey treats that as a boot failure.

### Spawn discipline

Spawned by joey via `t_spawn("u-test")` immediately after the `/alloc-smoke` block. Required caps inherited from joey via the standard spawn — `CAP_HW_CREATE` is NOT inherited (the negative paths in flow 6 depend on that absence).

### Why hello-rs in flow 2 doesn't validate stdout content

`/hello-rs` writes its banner via `SYS_PUTS` (console-direct), not via `fd 1`. The parent's piped stdout therefore stays empty; a `read_to_end` on it blocks forever. Flow 2 sidesteps this by dropping every pipe end on the parent side after spawn — same pattern alloc-smoke uses. Stdout-content validation belongs in a future Utopia-builtin test once a fd-1-writing binary exists (e.g., a native `echo` from coreutils piped into a captured fd).

## Data structures

No special types — u-test composes existing module surfaces. Two `static AtomicU32`s in flow 4 (`SHARED` for the wake target; `TID_WORD` for the clear-child-tid join target).

## Spec cross-reference

No formal `specs/*.tla` module for u-test. Per the spec-to-code FULLY suspended broadening, validation is the composed-flow exercise + the audit + the runtime test suite.

## Tests

`u-test` itself IS the runtime regression-check binary. Six flows × one `OK` log line each + a final `all OK`. Runs on every boot via joey.

Boot-log signature (after the alloc-smoke block):

```
u-test: starting (U-2-test; U-2 uplift arc integration smoke)
u-test: alloc + fs + io OK
u-test: process + pipe OK
u-test: notes + poll + time OK
u-test: thread + torpor + time OK
u-test: ninep codec OK
u-test: hardware + cap OK
u-test: all OK
joey: /u-test reaped status=0; U-2 arc integration verified
```

If any flow fails, the binary prints `u-test: <flow>: <what> FAILED\n` + exits 1; joey then prints `joey: /u-test orchestration FAILED\n` + returns 1 from its main, which extincts the boot.

## Error paths

Each flow's FAIL branch is annotated with the failing primitive (e.g., `u-test: flow_notes_poll_timeout: second poll wrong-shape FAILED\n`). The exit code is always 1 on any FAIL; the binary does NOT continue past a FAIL.

## Performance characteristics

u-test runs in ~50-200 ms on QEMU TCG (dominated by flow 4's thread spawn + join handshake; the synchronous primitive flows are sub-millisecond). Not on the boot-time critical path at v1.0 (boot wallclock is 14-24 s bimodal); becomes load-bearing once the broader boot path tightens past Phase 7.

## Status

- **U-2-test LANDED** — closes the U-2 uplift arc. Library half of Phase 7 (the libthyla-rs uplift) is complete.
- **Next**: U-3 (Utopia workspace skeleton + `ut` shell skeleton + `tools/build.sh utopia` cross-compile wiring).

## Naming rationale

`u-test` — the "U" prefix mirrors the U-* arc naming (every sub-chunk in the libthyla-rs uplift is `U-2X`). Short to type at the joey-spawn site + immediately recognizable as a Utopia-arc artifact. `utopia-test` was considered + rejected as longer-without-payoff.

## Known caveats / footguns

- **Drains synthetic child_exit at flow 3 start**. The hello-rs spawn in flow 2 leaves a synthetic `child_exit` note in the per-Proc queue; flow 3 drains it before its own timeout probe. If a future flow inserts a new spawn between flow 2 and flow 3, that drain may need to be more aggressive (currently `try_read`-until-None).
- **No teardown for ThyaAlloc heap**. The Proc-end SYS_EXITS releases everything; no explicit cleanup needed.
- **Flow 4's static `TID_WORD` is reset at flow entry**. If the binary ever runs `flow_thread_torpor` twice in a single Proc (it doesn't today), the reset matters — `0xC0FFEE` is restored before each invocation.
- **Flow 6's hardware checks assume CAP_HW_CREATE is NOT held**. If a future joey grants it to u-test (e.g., for a positive-path validation), flow 6 will need to flip from "expect Err(InvalidArgument)" to "expect Ok" — and pick valid PA/INTID targets.
- **u-test is NOT a substitute for the per-module alloc-smoke checks**. They complement; both are required at boot. The audit-trigger inventory in CLAUDE.md should be checked against both binaries when a new U-2X module lands.

## References

- `docs/UTOPIA-SHELL-DESIGN.md §15` + §19 — libthyla-rs uplift design + U-* arc roadmap.
- `docs/phase7-status.md` — landed-chunks table.
- `usr/alloc-smoke/src/main.rs` — companion per-module isolation tests.
- `usr/u-test/src/main.rs` — this binary.
- `usr/joey/joey.c` — orchestration wiring (search for `u-test`).
- `tools/build.sh::usr_rs_bins` — cpio packaging.
