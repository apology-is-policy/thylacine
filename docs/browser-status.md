# Boosty -- browser arc status (the B-arc; `docs/BROWSER-DESIGN.md`)

The authoritative pickup guide for the web-browser arc. The design document is
the plan and the research; this tracks what has LANDED and what is next. The
arc sits outside ROADMAP's eight phases and must never put the v1.0 release
candidate at risk (ROADMAP section 11; BROWSER-DESIGN section 10, risk 7).

## TL;DR

**Ratified 2026-09-21. The browser is named Boosty (the operator's cat). B-0 is in progress on branch `browser-b0`: JavaScriptCore already RUNS on the device (JIT off).** The operator voted: **WebKit
first, then Servo; no stage 0 (no NetSurf, no `webfs`); the Rust `std` port
runs in parallel, owned by the aux track; effort `xhigh` throughout.** The
first implementation chunk is **B-0: JavaScriptCore alone** (`JSCOnly`, no
JIT) cross-built for `aarch64-thylacine` -- WebKit's own first step for a new
OS, and the cheap way to *measure* the anonymous-memory gaps (P1) and answer
the JIT question (B-2) instead of guessing.

## Landed chunks

| Commit | What | Witness |
|---|---|---|
| `c09141da` | `docs/BROWSER-DESIGN.md` PROPOSED: six research lanes, the tree's measured starting line, the platform tranche, the JIT mapping, the capability-graph design | docs only |
| `8cd50a2d` | RATIFIED: the vote recorded (`dec-2026-09-21-browser-engine-order`), this status doc, the NOVEL.md candidate, the track-R brief for aux (`docs/handoffs/041`) | docs only |
| `af293efc` | scripture: first-party `std` Rust on Pouch is the THIRD userspace substrate (ARCH 3.5) | docs only |
| `25df504f` | scripture + spec: the mount-table shed at pivot/chroot (#80, ARCH 9.6.10); `specs/territory_shed.tla` + 5 buggy cfgs | `specs/check-territory-shed.sh` |
| `b8b27f1d` | scripture + spec: poll re-registers every pass, owns death/stop, crosses a preemption point; `specs/poll_cpu.tla` NEW; ARCH 8.1's as-built note | `specs/check-poll.sh` 16/16 |
| `2a737959` | territory + stalk: the shed, the `..` floor, the dissolved-union degrade, the COPEN strip | suite + `symlink-probe` union-a/b/c 34/0 |
| `1f14b6c5` | poll: the re-arm, the loop's own death/stop, `sched_preempt_point`, the irqsave list ops, the two-endpoint SrvConn poll | suite; `sched.preempt_point_takes_a_pending_irq` |
| `92a9ae94` | pouch 0033-0042 + the provers + the census header + `check-patch-hunks.py` | `pouch-hello-*` legs |
| `b70e1bfd` | the WebKit port: JSCOnly JIT-off, the CMake platform/toolchain files, `CHUNK_WEBKIT`, ICU | `ls-jsc.exp` (6 legs) |
| `5c08c7fd` | journal: addenda 3-7 | docs only |
| `3df67cb0` | `vault/record/`: 43 notes -- 10 audit rounds, 27 findings, 5 changes, 1 decision, 1 arc | `quaestor lint` 0 fail |
| `7c1dc314` | scripture (#98): a poll samples remote readiness with a SNAPSHOT and arms only before it parks -- NET-DESIGN 12.2 amendment, ARCH 23.3, `dec-2026-09-28-poll-sample-arm-split` (three operator votes) | docs only |
| `866c2959` | spec (#98): `net_poll.tla` rewritten for the split (level `ready`, timed + zero-timeout poller, snapshot, settle, hung server, collector); `poll.tla` gains remote fds, the settle, the arm, sample-only passes; `specs/check-net-poll.sh` NEW | `specs/check-net-poll.sh` 15/15, `specs/check-poll.sh` 14/14, on SPEC-POLICY's TLC |
| `5caa79ae` | the userspace 9P codec becomes its own crate, `usr/lib/ninep`, re-exported as `libthyla_rs::ninep`, so that its tests run on the host (libthyla-rs cannot be host-built); 8 tests of the invariants its dossier claimed | `tools/test-rust.sh` ninep 8/8, each test seen red with its invariant broken; the 11 consumer crates build for aarch64 |
| `6a57d37a` (NP-3b) | `ninep::ready_answer`, the one decision both ready-file servers call: a SNAPSHOT (offset bit 16) answered at once even at 0, an ARM answered on arrival if already ready, any other bit refused EINVAL, the reply cut to the count (ptyfs's no longer overruns a short read); `P9_POLL_*` in `9p_wire.h` | ninep 15/15 on the host, 5 sabotages red; joey net-6b + pty-probe read both wires on the device: green, and each red (the boot extincts at the refusal check) with its server's old file |
| `f14cf395` (NP-4a) | spec (#98): a readiness read the kernel cannot send is a shortage, not an answer -- an unsent arm bounds the park by a 10 ms retry timer whose expiry is a wake, never the call's timeout; `net_poll.tla` gains `ARM_MAY_FAIL`, `PollerArmFails`, `RetryTick`; `poll.tla` lets any arm fail and gains `RetryWake`; NET-DESIGN 12.2 + ARCH 23.3 | `specs/check-net-poll.sh` 19/19 and `specs/check-poll.sh` 19/19 as claimed (first recorded as 22/22, a miscount: the script runs 19 cfgs, then and now), the older counts unchanged; 3 new reds by their named property; both new liveness properties shown able to fail |
| `76204779` (NP-4b) | the async 9P submit reports a shortage as the retryable `-P9_E_AGAIN` and keeps the session: a full tag pool is checked before the build, and a full send ring takes the op back whole (`p9_session_retract_unsent`: the tag, and the fid a never-sent Tclunk unbound) -- before, one full ring latched the whole shared session dead | `9p_client.async_send_eagain_keeps_session_alive` + `async_full_tag_pool_is_eagain`, both seen red first; suite 1734/1734 |
| `24e0ac0d` (NP-4c) | the kernel half of #98: a poll samples a remote readiness file with a SNAPSHOT its server answers at once, and decides only after every snapshot of the pass is in (a fixed 1 s fail-safe per snapshot, counted and printed as `poll: FAILSAFE`, which `tools/test.sh` fails on); the deferred read is only the pre-park ARM, a wake; an unsendable snapshot is resent every 1 ms, an unsendable arm bounds the park by the 10 ms retry timer; three Dev slots (`poll_snapshot`, `poll_snapshot_release`, `poll_arm`) replace dev9p's `.poll`, and `dev_register` refuses a partial set; the cache and `VIV_PPOLL_PROBE_MS` are deleted. Fixed in the chunk: the stranded-arm collector unlinked under its lock and flushed after it, so a close in between had its Tclunk refused and leaked the server's slot (since #294; `net_poll_teardown` `BUGGY_SPLIT_GC`) | suite 1742/1742; ten `dev9p.poll_*` tests, each seen red on its named assert (15 sabotages); on the device, answering from nothing reddens viv-pheno-probe L113; `specs/check-net-poll.sh` 20/20; NP-1..NP-4c audited by Fable 5.1, round 1 clean (0 P0 / 0 P1 / 0 P2 / 2 P3); rebased onto aux-3 `eb9a74ea` and run again at `a402cb70`: suite 1750/1750, `ci-smp-gate` all five rows (ubsan-smp8 25/25 and default-smp1 25/25; default-smp4, default-smp8 and ubsan-smp4 10/10 each), the PTY and network legs 14/14 on the CI image |
| `5771d099` (closer) | a dying thread's Tclunk goes to a pool of closer threads (FID-LIFECYCLE section 9, `dec-2026-09-28-tclunk-closer`, two operator votes): a clunk that cannot be sent on a live session returns `-P9_E_AGAIN` with its fid still bound, and `dev9p_clunk_fid` hands it to `p9_attached_defer_clunk`; one closer per session, a spare spawned by the closer that takes work or by a hand-off that finds none idle, retirees reaped; slot reservation keeps the take-back from failing; flush(5) on both sides (a Tclunk is never flushed; a flushed or abandoned walk's late reply binds its fid and the orphan sink hands it to the closer; Stratum 20a1a27 sends an executed op's reply before its Rflush); `tools/test.sh` fails on the refusal line | suite 1774/1774; 22 new or rewritten kernel tests, each seen red at its assertion (6 sabotage boots); `net_poll_teardown.tla` 2 clean + 3 red cfgs, liveness fails without WF on `CloserSend`; `9p_client.tla` clean (197 states) + 5 buggy cfgs violated; Fable 5.1 audit, 3 rounds (r1 0/0/0/6 P3 + 2 self-found, r2 0/0/0/3 P3, r3 0/0/0/1 P3), all fixed; SMP gate 5 rows x 10/10, 0 corruption; Stratum `test_9p_pool` 20/20 (red on the old pool), ctest 73/73 |
| `d73e68da` (VIV-EINTR) | a caught signal interrupts only the calls on `signal(7)`'s list, and one caught note unwinds one thread (ARCH 8.8.3; the operator's vote of 2026-09-29; scripture `54be3162`): a per-Thread `note_interruptible`, set per Linux call by the vivarium dispatcher (always / on a slow fd / never) and cleared at the dispatch exit and around a page-in; a claim the claimant holds until its EL0-return tail, which loops past the notes it discards; `child_exit`, a caught `tty:susp` and `tty:cont` now wake a caught sleeper (a lost wakeup since item 11); `accept` and `connect` are interruptible only in their waits, an interrupted handshake leaves the row `CONNECTING`, and a retry or the socket's next use (send, recv, read, write and their vector and positioned forms, `SO_ERROR`) finishes it; `SO_ERROR` reads netd's `status` without a guest fd; `ETIMEDOUT` reaches the guest as itself | suite 1797/1797 + V-1b (probe L301-L310); 23 new kernel tests, each seen red at its own assertion (sabotage boots); variant A (the old kernel) reddens L309a, B reddens L305; 8/8 boots; `viv-run` + `r5f9-ash` on the CI image; Fable 5.1 audit, 3 rounds (r1 0/0/2/2, r2 0/1/0/2 + a self-found P2, r3 0/0/0/4), every P1 and P2 fixed; each P3 fixed, owned in OPEN-BUGS, or closed with its reason; landed with NP-5, one gate run on the landed tree: suite 1800/1800, `ci-smp-gate` all five rows 10/10 (default-smp1, -smp4, -smp8; ubsan-smp4, -smp8), 0 corruption, every cfg of the specs whose modelled code it touched (scheduler, tsleep, death_wake, reader_frame, pty_stop) clean or red on its named property, 15/15 CI-image legs |
| `979250a7` (NP-5) | the vivarium polls a /net socket through a readiness Spoor cached in its socktab (`ready[]` beside the rows; each row owns one reference, and a socket's dups share one Spoor, so a connection costs netd one readiness fid per Proc), never through a transient guest fd closed by number (wrong since N-3 admitted peer threads); the poll core takes pre-resolved Spoors (`sys_poll_for_proc_spoors`, `handle_snapshot_spoor`); the cache is released with the rows -- close, dup onto, both execve sweeps and EXIT, not reap. NP-5b: connect, accept and UDP sendto/recvmsg open their /net files as private Spoors (`sys_resolve_kpath_for_proc`). NP-5c: `socket(SOCK_NONBLOCK)` and `F_SETFL` write netd's `nonblock` verb (a nonblocking read of an empty socket BLOCKED since 2026-08-31, and recvmsg answered a closed peer EAGAIN); recvmsg's 0 is 0. netd `MAX_FIDS = MAX_SLOTS * 4`, derived. Rebased onto VIV-EINTR: connect's private `data` Spoor is resolved inside VIV-EINTR's interruptible handshake (an `EINTR` still leaves the row `CONNECTING`), and `SO_ERROR`'s status read resolves through the same core | suite 1800/1800 on the landed tree (1777/1777 on NP-5 alone); `vivarium.socktab_ready_cache` (incl. the per-connection sharing), `vivarium.socktab_ready_release_paths`, `poll.pre_resolved_spoor`, each seen red; viv-pheno-probe L278-L300 on the device, each group seen red before the boot that proved it (main's kernel at L282 and L283; the per-row cache at L300; sabotages at L288, L296, L297, L298, and a hang at each half of L290); `ci-smp-gate` all five rows 10/10 (default-smp1, -smp4, -smp8; ubsan-smp4, -smp8), 0 corruption; `specs/check-poll.sh` 19/19 and `specs/check-net-poll.sh` 20/20, specs unchanged; on a CI-image bake of the landed tree, NP-4c's 14 PTY and network legs (haul-npxf and haul-post against a real npxf server) and VIV-EINTR's `r5f9-ash`: 15/15 PASS; Fable 5.1 audit, 2 rounds (r1 0/0/1/4 P3: the P2 and the tests P3 fixed, three P3 tracked to the socket object; r2 0/0/0/1 P3, documented, its derived witness queued) |
| `2c44c3bd` (flush(5)) | an interrupted 9P call honours flush(5). A caught signal no longer abandons the op. The call sends `Tflush` and waits, killable only, for the first answer: a reply that beats the `Rflush` completes the call with its result (a read keeps its bytes; a write that completed reports its count), and only an `Rflush` that comes first returns `EINTR`. A death in that wait still abandons the op (#845). A `Tflush` whose op's reply lands while it waits for ring space is taken back unsent, a tag drainer parks while another op's tag is owed, and a living owner's flushed op still counts as live on its fid (`owner_waits`). Scripture `6fcbdd86`: ARCH 8.8.3 and 21.10, and the I-10 cell. The session gains `dispatch_flushed_rmsg` (applies a reply without freeing its tag) and `flush_retract`. Two PRE-EXISTING #349 defects are fixed with it: neither `p9_client_reader_pump_once` departure signalled send progress, so a sender parked while the SQPOLL or dev9p-poll pump held the reader role slept on after it left (every freed tag and every pump departure now signal); and the reader-role handoff could designate an op whose thread sleeps on the send list, where the designation never reaches it (an op is now `sending` until `client_wait`, and the handoff skips it). Audit: r1 (Fable 5.1) 0/0/1/2, r2 (Opus 5.5 fallback) 0/1/1/2 -- its P1 was a regression in r1's fix (a take-back evaluated inside `CLIENT_UNLOCK_RET`, after the unlock; the helper now asserts the lock) --, r3 (Opus 5.5 fallback) 0/0/1/5 in the arc plus 1 pre-existing P2 owned in OPEN-BUGS (its P2 was a regression in r2's fix: hand-built test rpcs read an uninitialised `sending`), r4 (Opus 5.5 fallback) 0/0/0/4, a clean close (F4, pre-existing since #845: a Tflush's root-fid placeholder refused a root-fid setattr during any flush; F1, a stale stop snapshot in the owed check, is owned with the 9P waiters-and-stops family in OPEN-BUGS). Closes the OPEN-BUGS P1 enqueued 2026-09-29 ~16:05Z | suite 1816/1816 on `e68fb165` (a full build; V-1b PASS, whose L305 interrupts a netd-backed read); `9p_client.note_flush_{honours_late_read,rflush_first_cancels,death_abandons,reader_honours_walk,full_pool_own_reply,pump_wakes_parked_flush,reader_rflush_first,reply_beats_unsent_flush,handoff_skips_staging,staging_waits_for_owed_tag}` + `9p_client.{handoff_skips_send_parked,async_clunk_drain_waits_for_owed_tag}` + `9p_session.{flushed_reply_honoured_for_waiting_owner,flush_retract_restores_live_op,flush_owner_waits_keeps_fid_live,flush_names_no_fid}`: all 10 client flush tests RED on the pre-fix client, and each mechanism RED under its own sabotage (B..U; removing the death-in-flush-wait arm turns `death_abandons` RED); `ci-smp-gate` all five rows 10/10 (default-smp1, -smp4, -smp8; ubsan-smp4, -smp8), 0 corruption; `9p_client.tla` clean + 5 buggy cfgs as claimed (the spec does not model Tflush); on a CI-image bake, 15 legs, one attempt each, all PASS: NP-4c's 14 PTY and network legs (haul-npxf and haul-post against a real npxf server on the host) and VIV-EINTR's r5f9-ash; audit 4 rounds, 0/1/3/13 (r4 clean) |
| *(pending)* (waiters-stops) | a stopped 9P waiter parks inside the client and re-elects on resume, and a Loom ENTER waits for the reader role (DEBUG-FS 5c.6's waiters-and-stops amendment, LOOM.md 8.6 item 2; scripture `6be7af12`; no vote, since ARCH 21.10's rules stand). Every client sleep sets `stop_unwinds`, so a Ctrl-Z or a debugger stop unwinds the wait and the caller's loop parks the thread in `client_debug_stop_park`, which brackets the park with `rpc->stop_parked` under `c->lock`; the reader handoff and the tag-owed check read that flag instead of the Proc's stop flags, which a resume-then-re-stop flips while the thread never runs (`p9_rpc.owner` is gone). A handoff that leaves the role free with nobody designated wakes the client's new role-waiter list, as does the session's death, and an ENTER whose pump finds the role held hooks it beside its CQ hook. Before: a waiter stopped while the reader left re-slept on resume with its reply unread; a drainer could wait out a second stop; the handoff could designate a thread still parked; an ENTER behind another process's synchronous reader slept until unrelated traffic read its reply | WITNESSES-PENDING |

## B-0 (built, audited over ten rounds, gated; landed as nine commits)

The working history is the branch `browser-b0` (46 commits, most of them labelled WIP and ungated as
they went). It was landed as nine coherent commits so that `main` stays bisectable -- the landed tree
is byte-identical to the branch's, checked with `git diff`. The per-step hashes cited in
`docs/JOURNAL.md` and in the `vault/record/` notes name working-branch commits.

The arc's record plane is `vault/record/`: one `chg` per change, one `adt` per audit round, one `fnd`
per finding, and `dec-2026-09-22-point-now-model-next` for the one decision that was the operator's.

**JavaScriptCore runs on Thylacine** (2026-09-21, HVF, JIT off = asm LLInt + IPInt).
WebKit `webkitgtk-2.54.0`, `PORT=JSCOnly`, static, against ICU 78.3; a 60 MB `ET_EXEC`
with no `PT_DYNAMIC` and no W+X segment. Measured on the device:

| Check | Result |
|---|---|
| `print("hello-" + 6*7)` | `hello-42` |
| `new Intl.NumberFormat("de-DE").format(1234567.891)` | `1.234.567,891` (ICU + its data) |
| runaway recursion in `try/catch` | `RangeError`, graceful |
| `a.push()` x 400,000 (ints, then objects); `new Array(300000)` filled | all reach the end |
| `new Uint8Array(N << 20)`, N = 1..256 | all succeed, last byte written |
| `typeof WebAssembly`, `WebAssembly.validate(header)` | `object`, `true` |
| `fib(30)` on the interpreter | 832040 in 71-72 ms |
| ICU's own `genrb` tool (same toolchain) | runs, prints its ICU 78.3 usage |

**All of WTF compiled against Pouch unpatched except five files**; all of JavaScriptCore
except two. The whole WebKit-side delta is `usr/ports/webkit/patches/0001` (10 files, +60/-3):
an `OS(THYLACINE)` guard; `cacheFlush` -> `SYS_ICACHE_SYNC`, fatal on failure; the ELF
branches of `InlineASM.h` and offlineasm's `globaladdr`; an upstream bit-rot fix in
`MemoryFootprintGeneric.cpp`; the Wasm fault handler compiled out where no machine context
exists (upstream never builds Wasm-on + no-machine-context); `OptionsJSCOnly` API tests off;
and three tolerances listed as findings F3-F5 below.

**Three Pouch libc bugs found and fixed on this branch** (each a libc-only change, no kernel),
and a fourth found and deliberately NOT fixed here:
- `0033` -- `pthread_getattr_np()` told every program its main-thread stack is ONE PAGE
  (upstream probes with `mremap`, which the seam ENOSYSes). Measured `size=4096`; real is
  1 MiB. JSC threw a stack overflow on its first call and could not print it. Pinned by a
  two-sided check in `pouch-hello-threads`. **The aux Rust-`std` track would have hit this
  too** (std's main-thread guard).
- `0034` -- `sysconf(_SC_PHYS_PAGES)` returned UNINITIALISED STACK (upstream never checks its
  ENOSYSed `sysinfo`). JSC caps its heap at 2 x RAM, so every allocation over 8 KB failed
  ("Out of memory" at array element 1003). Now reads `/ctl/memory`; -1 on a miss (errno
  untouched; a figure that runs to the end of the read buffer is a miss too). Pinned by
  `pouch-hello-malloc`, which re-parses `/ctl/memory` itself and demands equality.
- `0035` -- found BY the 0034 pin, not by the engine: **`fscanf`/`scanf` have been broken on
  every real `FILE` since patch 0002**, and every `getc` was one syscall. 0002's read backend
  never refilled `f->buf`, so musl's "the byte just returned sits at `rpos[-1]`" invariant was
  gone and every scan pushback re-read `buf[1023]`. Raw `read`, `fgetc` and `fread` over the
  same file were all correct, which is what cleared the kernel. Restores upstream's own
  no-`readv` refill arm; `pouch-hello-fopen` gains a `scan` leg, measured RED on the unfixed
  libc first. Behaviour change: C stdio input now reads ahead, like every other libc.
- **OPEN, not fixed here: `getuid`/`geteuid`/`getgid`/`getegid`/`getppid` return the raw ENOSYS
  sentinel, `0xFFFFFFDA`.** The kernel has `SYS_GETUID`/`SYS_GETGID`; only `getpid` was ever
  wired. stratumd CONSUMES the value (its admin uid, the keyslot token gate, dataset-root
  ownership), so the fix changes the storage daemon's security behaviour and is its own chunk
  on the A-3 identity surface. Found by sweeping the class all four belong to: a syscall parked
  at the sentinel whose libc caller cannot report failure, so the program gets a wrong value.
  Every C library in B-3 and the Rust `std` crate tail will call these.

### The measured platform findings -- the list for the operator conversation

The operator, 2026-09-21: *"When you reach the need to design the kernel chunks -- mprotect,
dlopen etc., let's talk about it."* **DESIGNED 2026-09-23 -- see "The B-1
decisions" below.** The table is B-0's measurement, kept as the record: what the engine hit, in
order, and what the probe did instead.

| # | What JavaScriptCore needs | What Pouch/Thylacine does today | What the probe did | Kind |
|---|---|---|---|---|
| F1 | main-thread stack bounds | reported one page | fixed (`0033`) | libc, done |
| F2 | machine RAM size | returned stack garbage | fixed (`0034`) | libc, done |
| F3 | **aligned address-space reservation** (`tryReserveUncommittedAligned`: map size+align, then `munmap` the two slack ends) | partial `munmap` refused (whole-mapping detach only) -> `CRASH()` -> **B-1a' the range form; B-1b's aligned-reserve leg** | keeps the slack mapped; harmless because lazy reservations commit nothing, but the VA is never returned | **kernel/Pouch design** (an alignment request on attach, or partial detach) |
| F4 | **decommit** (`madvise(MADV_DONTNEED/FREE)` on the GC's freed blocks) | `madvise` = ENOSYS; WTF ignores the error, so memory is simply never returned -> **B-1b: `SYS_BURROW_DECOMMIT`** | nothing (RSS only grows) | Pouch wiring onto the existing `SYS_BURROW_DECOMMIT` -- no kernel change, but it is the P1 conversation |
| F5 | **guard pages** (`mmap(MAP_FIXED, PROT_NONE)` over the ends of a reservation) | `MAP_FIXED` refused; WTF ignores the result -> **B-1b: discard + reprotect over one's own mapping** | silently none | **kernel design** (reservation holes vs an I-12 wording change -- BROWSER-DESIGN O-1) |
| F6 | **reservations over 256 MiB** (structure heap asks 4 GiB, halves until it fits) | `BURROW_ATTACH_MAX` = 256 MiB per mapping | JSC settled at 128 MiB by itself | policy; fine for JSC, will bind Gigacage / Wasm fast memory |
| F7 | **per-thread async signal + machine context** (`SIGUSR1` for GC thread suspend, signal-based VM traps, the sampling profiler, Wasm fast-memory faults) | `sigaction` admits SIGINT/TERM/PIPE/CHLD only; no `pthread_kill`; no `ucontext` registers | polling VM traps; suspend made fatal; fault handler off. Costs nothing with one JS thread and no JIT; **matters for B-2 (JIT) and for multi-threaded JS** | kernel/Pouch design (notes are per-Proc, fd-first) |
| F8 | a main-thread stack of several MiB (JSC asks 5) | **B-1b: 8 MiB with `AT_STACK_BASE` / `AT_STACK_SIZE`**; was a fixed 1 MiB reservation; SPARSE since LINEAGE L-4a (`exec_map_user_stack` is `burrow_create_anon_lazy`) -- I first wrote "eager" here from `exec.h`'s comment, which was stale and is corrected in the same change | JSC clamps to what it gets | raising it costs nothing at exec: a constant in `exec.h`, its mirror in pouch 0033, the prover's pin, and a higher I-32 ceiling for runaway recursion. Better still: pass the extent in auxv so libc DERIVES it instead of mirroring |
| F9 | `dlopen` | none (static only) | not needed by JSC | the operator named it; WebKit proper does not need it either (no plugins) |

### The B-1 decisions (2026-09-23; `dec-2026-09-23-memory-surface-and-loader`)

The conversation the operator asked for, held at max effort on Fable 5.1. Each vote was by blocking
question; the research behind them -- Plan 9, Fuchsia, Genode, seL4, OpenBSD, Mach, and WebKit's own
source read line by line -- is ARCH 6.5 "The permission ceiling", "Range detach", "Capacity" and
"Dynamic loading".

| # | Decision | Vote |
|---|---|---|
| 1 | the shape of the permission surface | **ceiling-bounded `burrow_protect`** (the Fuchsia / Mach shape): a mint-time `prot_max` per VMA; X never a target; a range within one mapping |
| 2 | a capability on the call, like `CAP_JIT`? | **no -- the gate is structural.** Attenuation and a bounded re-grant create no authority; a cap here would be ambient (every pthread guard needs it) or would break every threaded program |
| 3 | the one-way seal | **yes, same chunk** (`PROTECT_SEAL`; Mach `set_maximum`, not OpenBSD `mimmutable`) |
| 4 | F8 / F9 | **dlopen designed now** (rows 5-6); the stack to 8 MiB with its extent in auxv (my reading of "as well", stated to the operator and not contradicted) |
| 5 | the loader model | **L-A, dynamic Pouch**: `libc.so` is the loader; D-4's PT_INTERP rewrite lifted to native execs; `burrow_map_file` |
| 6 | what the sysroot ships | **static by default; `.so` only for runtime-loaded objects and `libc.so`** |
| 7 | the memory bar | **production-comparable, both substrates**: never refused while free memory exists; relinquished memory returns and the footprint shrinks |
| 8 | the I-32 default | **physical RAM minus a boot-sized TCB reserve**; the cap is the confinement mechanism |
| 9 | the native allocator | **`dlmalloc-rs`** over a Thylacine platform trait (alloc = lazy attach, free = detach, free_part = decommit) |
| 10 | the capacity chunk | **range detach + the charged sparse `filepages`**, both in B-1a' |
| 11 | the sequence | scripture -> **B-1a** permissions -> **B-1a'** capacity -> **B-1b** Pouch -> **B-1c** dlmalloc + witness -> **B-1d** dlopen, before B-3 |
| 12 | B-1c's large blocks (2026-09-24, `dec-2026-09-24-native-heap-large-blocks`) | **direct-map at 256 KiB and above** (C dlmalloc's own threshold, which the Rust port dropped): a block that size gets its own lazy reservation and is detached on free. Designing B-1c also showed the literal row-9 mapping leaks a VMA per trim-and-release cycle unless the platform owns its reservations, so it does (scripture's own mapping, made sound; no vote) |
| 13 | the manual's bounds test (2026-09-24) | **peak use under dlmalloc**: MANUAL-DESIGN 8.1 measures the reader under the guest's allocator, and `HEAP_BYTES` becomes its working-set bound |
| 14 | B-1d's loader shape (2026-09-24, `dec-2026-09-24-b1d-loader-shape`) | four votes, each on the recommended option: **PIE only where the loader places code** (a static program stays non-PIE `ET_EXEC`; `-pie` = a dynamic PIE, `-shared` = a `.so`, `-static-pie` refused); **`burrow_map_file` gains `addr`** as its sixth argument, read only under `BURROW_MAP_FIXED`; **PT_INTERP `/lib/libc.so`**, with `/lib` bound from the initrd; **`fdlopen` plus endowed directory handles** as the handle form (FreeBSD's `fdlopen(3)` + rtld's `LD_LIBRARY_PATH_FDS`), built at B-6 |
| 15 | `/lib` after the pivot (2026-09-24, `dec-2026-09-24-union-covered-directory`) | **Plan 9 unions**: an `MBEFORE` / `MAFTER` mount at a directory with nothing mounted on it keeps the covered directory as a member (`cmount`'s "add the old node"), so joey binds the initrd's `lib/` `MBEFORE` the disk's `/lib` and both stay visible. A file point stays a plain mount (Plan 9's `Emount` refusal is the operator's call). Rejected: an initrd-owned loader directory, the pool carrying `libc.so`, deferring to B-6. Lands as **B-1d-u**, before B-1d |
| 16 | the initrd's `lib/` (2026-09-25, `dec-2026-09-25-devramfs-directories`) | **devramfs serves directories**: `mkcpio.py` recurses and emits directory entries, and devramfs builds a static tree (each entry keeps its parent; walk, `..`, readdir and stat per directory). The initrd had been flat, so the staged `lib/` never reached it and the device witness soft-skipped while `test.sh` went green. The prover now fails closed whenever the image ships it. Rejected: one synthetic `lib/` level, binding the whole initrd root `MAFTER` the disk's `/lib` |
| 17 | the initrd's programs (2026-09-25, `dec-2026-09-25-initrd-bin-directory`) | **Programs under the initrd's `bin/`**: the root keeps only the six mount points, `bin/` and `lib/`; joey binds the initrd's `bin/` at `/bin` and resolves its pre-pivot bare names through a working directory of `/bin`, so `/bin/<name>` is one path on both sides of the pivot. Found when the tree's load refused the native `env`: the initrd root was both the pre-pivot root (with an `env` mount point since G15, 2026-06-23) and `/bin`, so `/bin/env` had been the `/env` directory for three months. Rejected: dropping `env` until a later chunk, retiring `env`, keeping the shadowing |

Measured against the bar before the vote (the reason 7-10 exist): four refusals with free memory --
the fixed 4 MiB native heap (`alloc.rs:77`), the 256 MiB default budget against a 2 GiB VM
(`proc.h:114`), the 256 MiB / 1 GiB per-reservation caps anchored to the flat uncharged `filepages`
array (`syscall.h:3203/3217`), and eager attach's contiguity (rings only; not a bar issue) -- and three
paths that keep relinquished pages: `mallocng`'s `MADV_FREE` inside a retained group (`free.c:124`,
ENOSYS today), WebKit's own decommit (same), and the native allocator (never trims). Decommit itself
already frees and uncharges (`burrow.c:1246-1266`); whole-group `munmap` already works.

What the vote did NOT cover and stays open: F7 (a thread-directed async note + register capture --
its own notes design, needed by B-2 and by multi-threaded JS); the fixed 1024-handle table (#355,
flagged, not built); PIE for Pouch binaries (decided 2026-09-24, row 14); OOM victim selection (deliberately not
built -- the reserve is the production property that matters).

Also learned, not engine findings: **the host has 8 GiB of RAM and ~15 GiB of free disk** --
JSC builds locally at `-j5`; all of WebKit will not, and belongs on the GCP builder (the Clade
precedent). `ut` does not reset `$errstr` after a success, which cost this run an hour (see
the journal). A 60 MB binary fetched with the native `curl` into an encrypted home execs fine.

### Where B-0 stands (2026-09-22)

The working branch is `browser-b0`; the landed history is the nine commits in the table above.
`docs/JOURNAL.md` (2026-09-21 and its addenda 1-7) carries the story; `vault/record/` carries the
per-round evidence; this is the ledger.

- **Ten audit rounds.** Pouch/libc 1-4, the shed 1-3, the B-0 self round, poll 5-7. Every P0/P1/P2 is
  fixed or tracked with a queue item; nothing was closed silently. Rounds 4 onward ran on Opus 5 (the
  Fable fallback with the same-family preamble, per CLAUDE.md: a round that finishes is closed, and a
  round is never skipped for want of Fable). Round 7's three P2s were all failures of VERIFICATION
  rather than of the mechanism, which is the part worth carrying forward:
  a spec property entailed by the behaviour it was written to exclude (`fnd-b0poll-r7-f2`), tests that
  could not witness the window they were named for (`fnd-b0poll-r7-f3`), and a latency claim resting
  on an unbounded walk (`fnd-b0poll-r7-f1`, now `seam-poll-hooks-per-list`).
- **Every new test rode a RED-before** on a kernel with its own fix reverted
  (`work/b0/red3/kernel-sab2.py`). Three of those sabotages PASSED at some point and each was a real
  finding, not a formality: the deadline sabotage (the test asserted the result, not the wake), the
  c2s-drain sabotage (same shape), and `noisb` -- which is kept in the script as a labelled
  NON-discriminating control, because the `isb` widens the unmask window and cannot be witnessed here.

- **DECIDED 2026-09-23 (was owed as a conversation): the F3-F9 kernel design** -- eleven votes in
  "The B-1 decisions" above; the scripture is ARCH 6.5. Still owed as conversations: the
  small-integer socket fd redesign; unlink-while-open; and F7 (per-thread signals).
- **DECIDED 2026-09-22 ("point now, model next"), was owed:** syscalls run IRQ-masked end to end, so a
  noise-driven wait held its CPU's interrupts. poll now crosses a PREEMPTION POINT each re-loop
  (`sched_preempt_point`; ARCH 23.3, `specs/poll_cpu.tla` checks the CPU-level claim). Scheduled next,
  BEFORE the F3-F9 kernel work and it deletes the point: build ARCH 8.1 as written -- syscall bodies
  with interrupts ON, still non-preemptible. The Phase-0 deferral that was never executed (ROADMAP's
  "Kernel preemption" item never reached a status doc).
- **OPEN from poll round 7 (F1), tracked here because nothing else owns it:** nothing caps hooks on one
  `poll_waiter_list` (64 per call x `PROC_THREAD_MAX` x Procs), so a producer's `poll_waiter_list_wake`
  walk and the poller's per-pass unregister walks are O(attacker-scaled) and IRQ-masked -- and the
  producer's walk crosses no preemption point at all. The point bounds the NUMBER of masked spans, not
  the length of one. Fixes: the per-endpoint/event-keyed lists of round-4 F8, a per-walk wake cap with
  the remainder deferred, and an I-32 axis capping hooks per list.
- test262 / a real benchmark on `jsc`: not started.

## ARCH 8.1 -- the kernel chunk that precedes F3-F9 (BUILT 2026-09-22, branch `arch81`)

Not a browser chunk, but sequenced here because the operator put it before the
browser arc's kernel work and because B-0's audit is what surfaced the defect
it repairs. Seventeen commits off `main` @`ca1c7030`; tip `ce802b56`.

**Syscall bodies now run with interrupts ON and are still non-preemptible** --
ARCH 8.1's line as written, which Phase 0 deferred and P3-Ec accidentally built
as "interrupts off" instead. `Thread.in_syscall` gates `preempt_check_irq`;
`syscall_dispatch` is a wrapper that unmasks for the body and re-masks
unconditionally before the EL0-return tail, so the unmask cannot leak into the
KERNEL_EXIT eret window (#713).

What it means for the browser arc: **poll's preemption point is deleted**
(`b7132455`), and with it `specs/poll_cpu.tla`. `pipe_block_locked` and
`chan_role_acquire`, which had the same masked-loop shape and no point, are
covered without needing one. The B-0 residue item "any other masked loop"
(r5 F1's tail) is closed by construction rather than by a sweep.

Measured, and one number is a finding in its own right: the Linux-phenotype
syscall path was at **86% of the 16 KiB kernel stack** before this chunk,
because `viv_tier2`'s switch unioned getdents64's 4.6 KiB of staging into every
phenotype syscall's frame. Fixed (`e7c83ec6`); worst case is now 75.5% WITH the
new IRQ frame.

**Audit round 1 closed 2026-09-22: 0 P0 / 1 P1 / 1 P2 / 6 P3, all fixed**
(`dc77a4b0` + `ce802b56`). An **OPUS FALLBACK** round -- Fable was out of
credits and the rule is never to skip a round for want of it -- so the
family-diversity axis was forfeited and context independence was not; note the
tier when weighing it. The prime target (a lock-free read-modify-write on state
an IRQ handler also writes -- the class the chunk's lock sweep did NOT cover)
came back **clean**.

The P1 was a false claim in ARCH 8.12 itself: it said five latent single-CPU
hangs were fixed unlooked-for, and `loom_free`'s spin on a KTHREAD's exit flag
is not one of them -- `in_syscall` refuses the switch that would run the
kthread. Servicing an interrupt is not scheduling a thread. Scripture
corrected; the defect was left **OPEN, pre-existing, and tracked**, and is now
**CLOSED** (below). The P2 was an
unprivileged masked-window DoS in `/ctl/kstack`, an instrument this chunk had
added two commits earlier -- now `CAP_HOSTOWNER`-gated with a budgeted scan.

| bar | state |
|---|---|
| suite @tip `ce802b56` | **1616/1616 PASS** |
| poll spec gate | ALL CFGS AS CLAIMED (4 clean + 7 buggy) |
| `syscall_irqs` gate | 8 cfgs on their named verdicts, clean at 18 states; the tail-check sabotage now caught (it was NOT, before the close) |
| SMP gate @`dd0e9ce1` | **PASS -- 40/40 boots, 0 corruption** (default/ubsan x smp4/smp8) |
| SMP gate @close `40261a8c` | **PASS -- 40/40 boots, 0 corruption** (default/ubsan x smp4/smp8). This gate matters more than usual here: #713 was 3-13% of boots and NEVER at `-smp 1`, so the HVF suite is structurally blind to the hazard this chunk sits closest to. |
| `tools/test-fault.sh` | **8 PASS / 0 FAIL of 8** -- all three kernel-stack GUARD variants fire (`kstack_overflow`, `secondary_stack_guard`, `bootcpu_idle_guard`), plus `recursive_kernel_fault` and `el1_sync_runaway`, the nested-exception cases this chunk makes more reachable. Added to this chunk's bar MID-RUN: the chunk deepens the kernel stack and this is the only runtime witness that an overflow FAULTS into a no-access guard rather than corrupting its neighbour. A bar that omits the one gate aimed at the hazard the change creates is a bar that verifies around it. |
| kstack runtime witness | **peak=10448 of 16384 = 63.8%** on a default boot, now printed every boot |
| interactive fleet @`40261a8c` | **PASS -- 55/77, 0 FAIL** (22 SKIP = absent optional host artifacts + the documented ci-vs-halcyon image split, neither a guest result nor coverage). The ~15 scenarios that parse boot output all pass, which is the check that mattered: this chunk adds a `boot-kstack:` line before the banner. |

### The P1's own close: the loom join, and the gate row it bought (2026-09-22)

Fixed on the operator's ratified sequencing (the loom fix first, then identity).
`loom_free` now BLOCKS on a new `Loom.sqpoll_join` Rendez that the kthread's
terminal wakes after its `state=EXITING` + `sqpoll_exited` release stores, in
the same masked window -- so a joiner that observes the flag observes EXITING,
which is what `thread_free`'s not-RUNNING gate needs.

**It was not latent.** `usr/loom-smoke`, which joey spawns on every boot, has
carried an EL0 SQPOLL consumer since `15796866` (2026-09-03), so every `-smp 1`
boot for 19 days hung at its exit. One-variable control:

| | pre-fix `-smp 1` | post-fix `-smp 1` |
|---|---|---|
| last log line | `loom-smoke: PASS`, then silence | `joey: /loom-smoke reaped status=0` |
| boot banner | **never** (120 s) | present |
| suite | never completed | **1616/1616**, 5/5 boots |

**Nothing could see it**, and that bought the durable part: every boot gate ran
four or eight CPUs, so the one configuration where a thread spinning on another
THREAD's write cannot be rescued by a peer was the one configuration nothing
booted. `default-smp1` is now `ci-smp-gate.sh`'s first row. A peer CPU is a
rescue mechanism as well as extra concurrency, and a hazard a rescue mechanism
hides is one the matrix can no longer observe.

## Remaining work (in order; BROWSER-DESIGN section 9)

| Phase | What | Exit |
|---|---|---|
| **B-0** | `JSCOnly` cross-build, JIT off (asm LLInt + IPInt). Source lives OUTSIDE this repo (a `webkit-thylacine` fork beside `llvm-thylacine`); the repo carries the build wiring and patches. | `jsc` runs on the device; the measured list of P1 gaps |
| **B-1** | P1, the anonymous-memory surface. **Scripture LANDED 2026-09-23** (O-1 resolved; ARCH 6.5); five gated chunks, **all LANDED by 2026-09-25**: | |
| B-1a | **LANDED 2026-09-23 `839c1745`; holotype audit r1 (Fable 5.1) 0 P0 / 1 P1 / 1 P2 / 4 P3, closed at `3fc95125` (SMP gate 5 rows x 10/10 clean) -- F1 the FILE page-in's admission spanned an unlock (a protect to none mid-page-in yielded a readable PTE on a guard), F2 four quadratic list passes + the per-page uninstall under a non-preemptible lock; both fixed with regressions, F5 (an over-charge after a D-3b window inside a touched lazy mapping) OWNED by B-1a'. The pouch (musl) substrate's `mprotect` was still ENOSYS -- closed at B-1b.** permissions: `burrow_reserve` (124) + `burrow_protect` (125) + `PROTECT_SEAL`; the phenotype `mprotect` row + exact `PROT_NONE`/`PROT_READ` mints; `cow.tla` extended FIRST behind `ALLOW_PROTECT` (bugs 4-6; additive by measurement, `cow_protect` 10636 states). Built beyond the letter: multi-mapping ranges all-or-nothing + a merge pass (ARCH 6.5 amended as built); the fork's per-Burrow clone dedupe; the eager-ANON fork share keyed on the ceiling; the lazy-piece detach uncharge. 1632/1632 kernel tests; `/protect-probe` + `/protect-guard-child` at boot. Audit-bearing. | probes with REDs; the `burrow_protect(X)` deny path; SMP gate; audit closed |
| B-1a' | **LANDED 2026-09-23 `387ffcd8`** capacity: the range detach (`vma_detach_range_in`; native 38 and the phenotype `munmap` are both the range form; holes permitted, an empty range answers 0); the charged sparse pagemap (`kernel/pagemap.{c,h}`) replaces the flat `filepages` array and lifts `BURROW_RESERVE_MAX` to the window; the I-32 default = the user pool = RAM minus max(256 MiB, RAM/8); the >256 MiB detach refusal is gone; the pool is PHYSICAL (charged at allocation, returned at free -- page tables and pagemap nodes included; a fork costs the pool only its node mirror); hardware page tables are charged to the space and reclaimed as they empty (audit r1 F1); `/proc/<pid>/status` reports `tables:` and `file:`; the pool reclaims idle images' pages before it refuses and a mapped FILE page is charged to its holder per leaf (audit r2 F8); the COW break replaces its leaf in place and a fork keeps the parent's tables (r2 F9). `specs/capacity.tla` (ChargeConserved / NoOrphan; `check-capacity.sh`). Audit-bearing. | a 4 GiB reservation round-trips (`detach.four_gib_reservation_round_trips`; `/capacity-probe` sees the census rise and fall from EL0); the reserve holds under a memory bomb (`capacity.pool_refuses_users_keeps_tcb`; the round-1 attack -- one page per 2 MiB, decommitted -- refused within one touch of the room, tables counted, everything returned: `capacity.memory_bomb_leaves_the_reserve`); SMP gate; holotype r1 (Fable 5.1) 0 P0 / 2 P1 / 0 P2 / 5 P3, all fixed; r2 (Fable 5.1, on the close) 0 P0 / 0 P1 / 2 P2 / 2 P3, all fixed; r3 (Fable 5.1, on the round-2 fixes) 0 P0 / 2 P1 / 0 P2 / 3 P3, all fixed; r4 (Fable 5.1, on the round-3 fixes) 0 P0 / 1 P1 / 0 P2 / 4 P3, all fixed -- F17: an EL0 alignment / external abort was answered as handled and re-faulted forever, a `snare:bus` death now (`/bus-probe-child`) |
| B-1b | **LANDED 2026-09-24 `037f511d`** Pouch: `mmap` mints at the prot asked for over `SYS_BURROW_RESERVE` (PROT_NONE reserves; X EACCES); `mprotect` -> `SYS_BURROW_PROTECT` (four arguments; Linux's normalisation); `madvise` DONTNEED / FREE -> `SYS_BURROW_DECOMMIT`; `MAP_FIXED` over one's own anonymous mapping = discard + reprotect (never a creation); partial `munmap` = the range detach; mallocng's `USE_MADV_FREE` on; the pthread guard real; the main stack 8 MiB with `AT_STACK_BASE` / `AT_STACK_SIZE` in the auxv (libc derives, never mirrors); the phenotype `madvise` row (233) pulled forward (VIVARIUM 6.28). `__NR_mmap` is back at the sentinel, so musl's one raw caller, `__init_tls`, maps through `__mmap` (0046, required by the parking; the kernel's Clade CL-4 arm had served its six-argument call). Patches 0044 / 0045 / 0046. Audit-bearing. | `/pouch-hello-mem` (twelve legs; the mallocng witness measured 4263 -> 2339 -> 11 pages full / half-freed / all-freed, identical at both smp counts), `/pouch-hello-guard` (a write into a worker's guard dies of `snare:segv`), three legs on `/pouch-hello-threads` (8 MiB, a 4 MiB frame, every worker's `---p` guard row), pheno-probe L23j-L23t; six sabotages red by name (the AT row); 1667/1667 both smp |
| B-1c | **LANDED 2026-09-24 `96b51346`** native: libthyla-rs's `ThylaAlloc` is thyla-heap (`usr/lib/thyla-heap`) -- dlmalloc 0.2.14 (vendored from rust-src) over lazy reservations the platform owns: segments carved at a bump pointer in the latest reservation (256 MiB, doubled per live reservation to 32 TiB, asked again at half when refused), `free_part` = decommit + bump rollback, `free` = detach of a whole reservation, a carve never filling its reservation so none can merge; a block of 256 KiB or more, or aligned to 256 KiB or more, is a reservation of its own, detached on free (the operator's vote). The fixed heaps, `ThylaAllocN` and `slurp`'s 2 MiB cap are gone; the six filters that do not need their whole input (cmp, grep, cut, uniq, wc, tail) stream it (`coreutils::stream`), because a lazy heap's exhaustion is a fault kill = exit 1 = cmp's "differ" (HT09.R4-F2); a line is bounded at `LINE_MAX` (64 MiB), nothing is collected from one (grep's matches and cut's fields go out as found; cat's transforms stream with no line held), and a filter whose reader leaves stops, silent, with its status (#54). The manual's bound is the reader's peak on this heap (worst 5504 KiB of 8192; MANUAL-DESIGN 8.1). `/heap-probe` (joey, every boot): 64 MiB of small blocks +16432 pages, 13 left after free with no trim asked; a trim returns the 243 pages the frees kept; a 32 MiB block returns every page under a live small one; a reservation switch releases the emptied first; churn 1070 vs 48 ns/page/cycle either side of the 2 MiB trim threshold (B-1b's F4). Device sabotages RED by name: decommit + detach answering without the syscall; the trim a no-op (211 pages against <= 32); each half of the gone-reader policy (five checks); round 2's fixes reverted (the banners and `-` of head and tail, `tail -n 0`, grep -c's verdict with no reader, ns's raw write): exactly those eight of 93 checks; round 3's reap-first capture restored (the boot hangs at the first capture check) and its other fixes reverted (six of 101); round 4's `tail -n -N`, `--` and head's overflowing count reverted (four of 105). Holotype r1 0 P0 / 1 P1 / 1 P2 / 10 P3, r2 0 / 0 / 1 / 8, r3 0 / 0 / 1 / 9 and r4 0 / 0 / 0 / 9, clean (Opus 5.5 fallback, all four, merged with the parallel self-audits), every P0-P2 fixed. | the page count rises past 4 MiB and FALLS after free, sabotaged once |
| B-1d | **LANDED 2026-09-25: B-1d-u `3b52d769`, B-1d `dfdd6344`** dlopen: `SYS_BURROW_MAP_FILE` (126) over D-3's arms, `addr` read under `BURROW_MAP_FIXED`, every executable window vouched (EACCES); PT_INTERP for every phenotype, one level, resolved in the Proc's namespace; `libc.so` is musl's loader (a second configure through the fork clang; 0047 file maps, 0048 RELRO as a protect reduction); the fork driver's `-shared` / `-pie`, `-static-pie` refused; the initrd serves directories (row 16), keeps its programs in `bin/` and the loader in `lib/` (row 17), and joey mounts `lib/` MBEFORE the disk's `/lib` (B-1d-u's covered member, row 15). Holotype r1 clean, r2 dirty (4 P0-class breakers of the `bin/` move, then 0/0/1/5), r3 clean (0/0/1/6 with the self-audit; five EXTINCTION rewordings reverted, the operator's call; `CoveredIsItsPoint` given its buggy cfg), all Opus-on-Opus (Fable 429); 18 sabotage legs; test-fault 8/8; full smp matrix 50/50; TLC 27 cfgs (`specs/check-territory.sh`). | a Pouch `.so` loaded on the device; the two deny paths -- **MET**: `/pouch-hello-dlopen` on every boot (loader, MNOEXEC EACCES, confined ENOENT with a control each side of the pivot, load, segments, RELRO) |
| B-1d-v | **LANDED** 46d943c5 (three votes after B-1d's landing, a fourth after audit round 1): `SYS_MOUNT` refuses what Plan 9's `cmount` refuses, `ENOTDIR` for a source/point type mismatch under any flag and for any mount but `MREPL` at a point that is not a directory (`dec-2026-09-25-mrepl-only-at-a-file`, which replaced `dec-2026-09-25-sys-mount-emount`); joey's five extinction bodies name `bin/joey`, the prefix and the tool-matched bodies alone being ABI (`dec-2026-09-25-extinction-bodies`); the loader's refusal of `LD_*` made policy, static programs as found (`dec-2026-09-25-musl-secure-loader`, docs only). Audited Opus on Opus (Fable out of credits); round 2 found every `/srv/<name>` node keyed at the registry root, fixed with a `qid.path` per post. | both refusals in kernel tests and on the device; alloc-smoke's file-over-`/srv` leg a refusal check; `devsrv.service_keys_distinct`; `territory.tla` re-run |
| **B-2** | The JIT: JSC's separated WX heap on `SYS_JIT_CREATE` + `SYS_ICACHE_SYNC`; `CAP_JIT` clearance. Audit-bearing (I-42/I-12). | benchmark with JIT tiers; deny-path probe (no `CAP_JIT` -> interpreter, never RWX) |
| **B-3** | P3: ICU, FreeType, HarfBuzz, sqlite, libxml2, png/jpeg/webp, libpsl, curl, OpenSSL; fontconfig decision (O-4). | each library's tests under Pouch |
| **B-4** | P2: its own design document (O-3: generalise the Weft share gate vs Mycelium), the primitive, then WebKit's `Platform/IPC` + `SharedMemory` backend. Audit-bearing (I-4). **O-3 leaning (operator, 2026-09-29): generalise the Weft share gate** -- not yet a signed decision; the design document must also show how a connection endpoint crosses between processes (the Weft share carries memory only), and the vote comes with it. | two-process message + shared-bitmap witness |
| **B-5** | WebCore + WebKit2 headless, `PORT=Thylacine` modelled on PlayStation; P5 (EGL) resolved here; O-2 (which WebKit line to track) measured here. | `WKPagePaint` renders a local page to a PNG on the device |
| **B-6** | The chrome on Tapestry; P6; the constructed namespaces with deny-path probes. | a TLS page in a tile; content Proc proven unable to open `/net` |
| **B-7** | Hardening, fuzz posture, the owed invariant ENFORCED, the Operator's Manual section. | arc close |
| **R** | **aux**: Rust `std` for Thylacine, the crate tail, then Servo. Brief: `docs/handoffs/041-rust-std-track-to-aux.md`. aux's design `docs/RUST-STD-DESIGN.md` was RATIFIED by the operator 2026-09-21 (on `aux-3` @`4cce758d`, not yet on `main`): target `aarch64-unknown-thylacine`, family unix over Pouch; and **O-5 decided: `std`-on-Pouch is a sanctioned THIRD substrate for new first-party programs, not ports only** -- an ARCHITECTURE 3.5 amendment that main owes. R-0 (the target JSON, the `libc` module, the `std` arms) is in progress; it consumes Pouch 0033/0034/0035 from `main`. | a `std` hello built by cargo and run on the device |

## Exit criteria status

- [ ] A page loads over TLS from the network and renders in a Halcyon tile.
- [ ] JavaScript runs with a JIT under strict W^X (no RWX page ever exists).
- [ ] A content Proc cannot name `/net`, `/srv`, `/proc` or `/dev` (deny-path probe).
- [ ] The owed section-28 invariant is allocated, then ENFORCED.
- [ ] The Operator's Manual has a browser section.

## Trip hazards

- **Effort is `xhigh`, by the operator's vote, for the whole arc.** Say so in
  every audit-bearing commit body; do not re-ask.
- **The permission surface is `burrow_protect` under a mint-time ceiling (ARCH
  6.5, ratified 2026-09-23; BUILT at B-1a as `SYS_BURROW_PROTECT` 125, with
  `SYS_BURROW_RESERVE` 124), not `mprotect`.** X is never a target of it and no
  capability gates it -- do not add either. The native detach is the
  range form since B-1a': one `burrow_detach(vaddr, length)` removes every
  mapping and piece under the range, holes included; an empty range answers 0.
- **`SYS_WEFT_SHARE` is gated to the driver tier on purpose** (Weft-7 F1,
  `kernel/syscall.c`). Read that audit before proposing to lift it.
- **Thylacine links statically BY DEFAULT; `dlopen` arrives with B-1d as an
  opt-in dynamic link against `libc.so`** (ARCH 6.5 "Dynamic loading"). WebKit
  is LGPLv2: keep the build reproducible from published source so LGPL
  section 6 is met.
- **Swift is entering WebKit** (`ENABLE_BACK_FORWARD_LIST_SWIFT`; OFF when
  cross-compiling). Pin it OFF explicitly and watch for it becoming mandatory.
- **WebKit cannot be built on thyla-pi** (1.5-2 GB per unified job). Host only.
- **Never claim a research fact from memory.** The design's section 13 marks
  what was verified in source; five items are flagged unverified, and the
  first of them (every JSC writer uses `performJITMemcpy`) is B-2's first job.

## References

`docs/BROWSER-DESIGN.md`; `vault/record/decisions/dec-2026-09-21-browser-engine-order.md`;
`docs/NOVEL.md` ("The browser as a capability graph", "Mycelium", "JIT-as-a-capability");
`docs/JIT-ON-WX-DESIGN.md`; `docs/LLVM-DESIGN.md` (the C++ runtime, CL-2/CL-3);
`docs/POUCH-DESIGN.md` section 8 (memory); `usr/lib/thylajit/thyla_jit.h`.
