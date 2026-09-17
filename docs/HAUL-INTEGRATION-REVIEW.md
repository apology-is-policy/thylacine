# Haul completion integration — 2026-09-17

## Scope and authorization

The operator requested completion of Haul, authorized bringing its required
Imperium changes into main, waived Claude-specific settings gates, and asked
that the work remain single-agent. This record is a self-review and validation
record, **not an independent adversarial audit**.

The integration starts at main `0a34793f`. It selects code, tests and formal
models from aux commits `bccb297f`, `4c77db6e`, `871b8f92`, `6c4d61be`,
`0263a845`, `4bae9094`, `abdf70ae`, `8bcc2e3f`, `6758a1bd`, plus the restored
cross-user test arm from `b1b68eaa`. It preserves main's DMA_SEGMENTS syscall
112, native-top 113, Instrument prompt and Cargo members. It does not import
the unrelated aux audio or media-viewer implementation.

## Changes reviewed

- Trusted serial SAK episodes, corvus's distinct imperium key verifier and
  deferred authorization, propagating legate grants, the imperium command and
  fasces, and the reviewed namespace/session cleanup fixes required for logout.
- POST_SERVICE at bit 13 in the elevation-only, clearance and propagating
  masks; no inclusion in CAP_ALL and no new TCB role inheritance.
- Atomic registry reservation bounds: two live/reserving posts per scope,
  four cap-owned slots per registry; protected TCB names, cap-tombstone reuse,
  name revalidation and a non-wrapping generation check before enqueue.
- Haul's posted byte service, same-principal SRV_PEER gate, single remote
  session, refusal of later clients, and pump/process lifetime handling.
- Shell status expansion and regression tests; interactive PID framing; design,
  vault and agent operating instructions.

## Invariant review

**Authority.** WALK_CREATE is still the posting entry point. The post cap is
checked in the kernel before reservation and the scoped quota is computed under
the same lock as reservation, including in-flight posts. Ordinary forks strip
it. Propagating forks retain only the granted set intersected with parent and
requested caps. Existing console/PTY role gates are unchanged.

**Revocation.** The scope root's sweep and child publication serialize under
the process-table lock; a parent already marked for exit cannot publish a late
child. Root exit, expiry and abdication use the same scope termination path.
Territory references detach under the table lock and free outside it at exit,
so namespace teardown may sleep without retaining the process-table lock.
Session hangup and console restoration are checked by actual cross-user login.

**Registry lifetime.** Slots remain registry-owned; listeners cannot transfer
to another process. Only cap posters' tombstones with no active accepter can
be reused for a new cap name. A service reference stores its name rather than a slot pointer.
Openers revalidate that name and capture generation under the registry lock;
failed generation comparison before enqueue drops both connection references.
An already accepted connection owns identity by value and is independent of
later slot reuse. Existing TCB tombstones are not cap-postable.

**Relay lifetime.** A posted connection fd is used by both pumps; neither pump
closes it while the other might address its fd number. Completion is published
atomically, then the main loop exits the process to close shared resources.
The private path retains its distinct reply-writer close, which is required to
wake a synchronous attach when the remote server dies. Credentials are consumed
by the handshake before pumps start; incoming frames cannot race it. Extra
clients are closed rather than merged into an existing remote fid/tag space.

**Input and grammar.** Post mode accepts exactly name/address and rejects `-a`
even when it is `/`. The original private-command option boundary remains.
Trusted UI names the new bit. Errors remain queryable through `$errstr`.
The status-expansion fix uses a revision to distinguish a truly empty command
from an empty substitution whose failure must be preserved.

## Failures found and resolved

1. The first second-mount test waited for a printed builtin error and timed
   out. Verbose relay evidence plus an explicit stored-error read showed the
   connection was refused correctly; mount returns its error through `$errstr`.
   The test now checks status and error before proceeding.
2. That check exposed `eval_command` clearing status before argument expansion.
   `echo $status` and `exit $status` therefore lost the preceding failure.
   The fix preserves prior status during expansion; boot probes cover prior
   failure, empty expansion and empty successful/failed substitutions.
3. The Imperium harness accepted a partial background PID from a serial chunk
   (`24` instead of the complete PID), then matched it elsewhere in `ps`.
   Requiring the terminating newline fixes that framing error. The corrected
   full scenario passes, including background teardown and cross-user login.

## Verification

- Guest boot/unit suite: **1551/1551**, including the new post authority,
  scope quota, global partition, TCB tombstone, recycled-generation and concurrent
  accept-lifetime tests.
  Native shell builtin/substitution probes report all OK.
- Haul host library: **52 passed**, one explicitly ignored live fixture test;
  the live npxf interoperability test was run separately and passed.
- npxf KAT regeneration: **23 vectors** match the real npxf implementation.
- Fasces host library: **7 passed**. Corvus crypto host library: **17 passed**.
- Imperium TLC: clean **157839 distinct states**, and all four mutant runs
  produce the intended invariant violations. All eight existing corvus mutants
  also produce invariant counterexamples. These models abstract bytes and
  quota implementation; they are not proofs of the whole relay.
- Interactive checks: **7/7 passed**, with one attempt per case:
  `haul-post`, `haul-npxf`, `haul-hangup`, `ls-imperium`, `im1-sak-lever`,
  `im3-lex-curiata`, and `ls-bghome-stall`. The hardened `haul-post` output
  witnesses also passed in a separate run. These include real encrypted npxf
  reads, second-client refusal, unmount/repost, authenticated remote EOF during
  attach, live-scope revocation and cross-user login.
- Final SMP gate on the accept-lifetime fix: **40/40 boots passed**, with zero
  corruption, external kills, missed injections, timing or other failures.

| Build | CPUs | Passing boots |
|---|---:|---:|
| Default | 4 | 10/10 |
| Default | 8 | 10/10 |
| UBSan | 4 | 10/10 |
| UBSan | 8 | 10/10 |

Raw logs are retained in the integration worktree's `work/` directory:
`test-smp-final.log`, `test-final-interactive.log`, and
`test-haul-post-final-witness.log`. They are local evidence, not tracked source.

## Remaining limits

Credential custody through corvus remains the operator-deferred follow-up.
Haul continues to accept a protected token file or an explicitly selected
environment source. The earlier npxf stack-copy/secret-newtype review item and
a deterministic zero-write TCP back-pressure witness are not closed by these
small-file interoperability tests. The current SAK authorization is serial.
A posted service deliberately supports one mount, with two active posts per
scope and four cap-owned slots per registry.

The original main checkout's build artifacts are not replaced by worktree
artifacts. Rebuild in that checkout to run the integrated version.

Additional existing model regressions: the console-poll lost-wake mutant,
death-wake mutant, and all eleven handle/capability mutants each produced
invariant counterexamples. These are successful negative-control checks,
not failed production configurations.

## Self-review correction: accepter lifetime

`exits_code` tombstones services before it cascades termination to live peer
threads. A tombstone alone therefore does **not** prove there is no old
accepter. The implementation must pin an active accept operation under the
registry lock, reject a concurrent accept (the existing Rendez is single-waiter),
and prevent rebinding/recycling until that operation has unwound. Caller
stripes are validated under the same lock as acquiring the pin. Every sleep,
EOF and interrupted-return path releases the pin. This corrects the earlier
lifetime argument above; listener non-transferability is insufficient alone.
The regression exercises a real blocked accepter and concurrent refusal,
then tombstone wake and reuse after the pin is released. It passes in the
1551-test kernel suite and the seven interactive boot runs.
