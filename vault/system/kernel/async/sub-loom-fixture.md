---
id: sub-loom-fixture
type: sub
title: "the strict 9P fixture and the guest qualification of the native Loom adapters"
parent: moc-kernel-async
code:
  - usr/loom-fixture/src/main.rs
  - kernel/test/test_loom_native_probe.c
audit: light
guarded-by: [inv-i29, inv-i30]
validated-by: [prose]
design:
  - "docs/ASYNC-SERVICE-ABI.md \"Sub-chunks\" NA-4d"
created: 2026-10-18
updated: 2026-11-03
---
## Purpose

Both native Loom adapters -- the Rust [[sub-loom-native-adapter]] and its C
twin -- are client code over a `Kernel` trait, and their whole proof until NA-4d
was a *simulated* kernel (`sim.rs`, `sim_kernel.c`) the adapter authors also
wrote. The guest qualification closes that loop. `/bin/loom-fixture` is a strict
9P2000.L server in EL0; `/bin/loom-native-probe` and `/bin/loom-c-probe` drive
the two adapters against it on the real kernel ring; and
`kernel/test/test_loom_native_probe.c` boots all three under the private-ring
qualification lever and pins every outcome. A rule a sim gets wrong -- the hole
NA-4d exists to close -- surfaces as a probe that passes on the sim and fails in
the guest.

The fixture and the kernel test are owned here. The probes are HARNESS (they
exercise the adapter, they are not part of the system); the adapter they
exercise is [[sub-loom-native-adapter]], the kernel service they run against is
[[sub-kernel-loom]].

## Contract

- The fixture serves one registry it is handed on fd 0 (a `CWALKONLY` devsrv
  root), reads a control pipe on fd 1 (EOF = exit) and writes a notify pipe on
  fd 2. It posts itself as `fixture` with `t_walk_create(0, "fixture", T_OREAD,
  0)` then `t_srv_accept`, and serves every connection with `t_read`/`t_write`
  frames until the control pipe closes.
- **The tree.** A root directory; `data` (a multi-page pattern file, the
  multishot source); `sink` (writable, reads back what was written); `stall`
  (reads never answered -- the deadline and cancel-by-drop legs); `stats` (the
  server's own counters); `stream` (a per-fid cursor, because a multishot read
  repeats its offset); and a `dir` subdirectory (the non-root walk, and the
  walk-from-an-opened-fid leg).
- **Strict.** A message a correct client never sends -- a T-message before
  Tversion, an unknown fid, a walk from an opened fid, a reused live newfid, a
  read or write on an unopened fid, a tag already in use, `.`/`..`/NOFID/NOTAG,
  a trailing byte or an oversized count -- is answered Rlerror AND counts a
  violation.
- `stats` reports sessions accepted and ended, violations, stalled reads,
  flushes, clunks, getattrs, walks, opens, reads, writes and faults, so a probe
  check can tell a refusal the kernel made at admission from one the server
  answered.
- **Hostile services (AS-3).** Twelve more names serve the same tree with one
  fault at the first message of one type: `hold-version`, `-attach`, `-getattr`,
  `-walk`, `-open` hold it unanswered; `torn-version` and `torn-read` send part of
  the reply and stop; `garbled-version` and `garbled-read` answer an impossible
  frame length; `wrong-version` and `wrong-read` answer the wrong reply type;
  `hangup-read` ends the session. `unaccepted` and `unaccepted-drop` are posted and
  never accepted; `stats` carries a bit per service while its listener polls
  readable, the positive control that a CONNECT reached the backlog. An `E` on the
  control pipe asks for an `e` on the notify pipe at the next session end, for a
  probe whose full CQ rules out a stats read. A
  fault counts in `stats`, never as a violation: the kernel's client did nothing
  wrong, and the count is how a probe tells an operation that reached the named
  stage from one stopped earlier.
- **Exit codes.** The fixture exits 76 when it saw zero sessions (the default
  image, where private setup is refused), 77 when it saw at least one, every
  session ended and no violation, 2 when a session outlived its probe past the
  settle, else 1. On the default image's -EOPNOTSUPP
  setup refusal `loom-native-probe` exits 78, `loom-c-probe` 74 and
  `loom-death-probe` 72; with every leg passed on the qualification image, 79, 75
  and 73.

## Mechanism

`test_loom_native_probe.c` (`loom.native_probe_rust`, `loom.native_probe_c`)
builds a registry with `srv_registry_create` + `devsrv_attach_registry` +
`CWALKONLY`, opens two pipes, and `rfork`s the fixture and one probe with fds
0..2 installed in the thunk (the fixture additionally
`proc_mark_may_post_service`, which exec does not clear). It waits on the
monotonic private-ring counters (`loom_private_created`/`_retired`), pins both
exit statuses against `LOOM_PRIVATE_QUALIFICATION` (never against what ran), and
checks rings made == retired, connections freed, and the registry destroyed
after its own unref. The qualification image expects two rings and twenty-six
connections per adapter probe, and twelve rings and ten connections for the death
probe, all released; the default image expects none.

Past the NA-4d legs, each adapter probe drives the AS-3 list against the hostile
services: a hold at each handshake and setup stage, ended by a deadline once and
otherwise by a drop, after turns show the operation still pending; a garbled, wrong or torn reply at the handshake and at a
READ; a service that never accepts; a stream parked with both its pool's leases held
while a sibling scope stalls and is cancelled, after which a WALK shows its session
still answers and the stream finishes; a cancel with the CQ full, whose session ends while the CQ
is still full and which posts nothing past it; and the pool and request bounds an
honest program can reach. Those bounds are the adapters' own gates, reached before
the kernel is asked: a pool over a live member answers `-EINVAL`, a second fixed
operation on one buffer `-EBUSY`, and the read past the book's four `-ENOSPC`. The
kernel's own answers, and quota refusals that need a resource floor, stay with the
kernel fixtures
(`loom_private_protocol_fault` and the charge tests).

`/bin/loom-death-probe` (`loom.native_probe_death`) is the whole-userspace half
the kernel's close-boundary schedules left open. Its driver spawns itself per
case; each victim holds a stalled READ (or a CONNECT the server holds, or one
nobody accepts, or a stream with a lease held), then exits, exits from a sibling
thread, is killed, closes its ring from a sibling thread, or execs, from the main
thread and from a sibling. The driver bounds the victim's end and reads the
session's end at the server. A sibling acts only once the main thread has said it
is entering its turn, and that turn has one long deadline, so a wake the kernel
loses shows as a victim the driver's 5 s bound finds still alive. The driver reads
the packed wait word, so a kill must read signal 9, not any nonzero status. Exec
latches a private ring and also closes it. A descriptor that survived would answer
a new image's ENTER `-EACCES`, refused for that image before the kernel looks at the
latch, so the new image requires `-EBADF` from every number below the scan's end,
and makes a ring of its own. The victim found its own ring with the same scan before
the exec. Rust only: the paths under test are
the kernel's, and an adapter's teardown does not run on any of them.

The retire witness is a `Q`/`Z` handshake: after the ring retires the probe
writes `Q` on the control pipe, and the fixture, once its connection set is
empty, writes `Z` on the notify pipe -- so a lingering session is observable
rather than assumed absent (closing audit r1 F6 / self S1, which only `close()`
had witnessed). `asked` is sticky, so there is no race: `Z` is sent once `conns`
empties whether or not the ask was read first.

**The interaction node (AS-4d).** `dir/interaction` speaks enough HIN1 to serve one
clipboard, so that a real client can run against a server that can be stopped. The
session's own service cannot be: typing goes through it. With `--srv` the fixture posts
in /srv itself. It opens /srv because a walk-only handle does not transfer, and it posts
only `fixture` and `hold-copy`, because a registry holds sixteen services. `hold-copy`
takes a copy's first write, and from the read for that write's reply it answers nothing
more on that connection, whichever fid asks, so the session ends only when the client
ends it. An 'H' on the control pipe asks for an 'h' on the notify pipe at the next such
stop, and each 'Q' asks for its own 'Z'. The node checks no scope and holds no authority,
which stay [[sub-halcyond]]'s. `kaua-term-probe --stopped-nora` drives it, by way of
`ls-nora-stopped-copy`, which has not yet run.

## Data structures

The fixture is `no_std` over lib/ninep's codecs and libhalcyon's HIN1 bodies: a fixed
node table, a per-session fid map, per-fid `stream` cursors and HIN1 records, one
clipboard, and the `stats` counters. Each round
allocates its poll set, control sampled last so that a pass seeing a session's hang-up
also sees any 'E' written before it. The kernel test holds the two probe blobs in
`alloc_pages` of an order sized from the ELF, and the registry and pipe
descriptors on its own stack frame.

## Concurrency

The fixture is a single-threaded EL0 server: it owns every connection and serves
them in one poll loop, so it needs no lock. The kernel test drives two `rfork`'d
children (the fixture and one probe) over the two pipes; the test thread waits on
the monotonic ring counters and reaps both.

## Invariants enforced

Nothing here *enforces* an invariant -- the fixture and the probes *witness* the
kernel's. The qualification exercises [[inv-i29]] (the full-CQ leg: a backlog
that stays exactly `cq_entries`, the first time that overfill witness ran red)
and [[inv-i30]] (the submit-time pin, through the walk-from-an-opened-fid leg,
refused at admission before the server is ever contacted) against the real ring,
where the sims could only model them. The `guarded-by` edge records which
invariants the qualification is *about*, not which it implements.

## Error paths

Every leg that expects a refusal pins the errno, and -- where the kernel and the
server would answer the same errno (a walk from an opened fid is -EBADF on both
sides) -- also pins the `WALKS`/`GETATTRS`/`VIOLATIONS` deltas at zero, so a
kernel submit-refusal is told apart from a server Rlerror. A fixture that sees
any violation exits 1 and fails the kernel test's fixture-status pin first (the
test checks the fixture status before the probe status).

## Performance

Not a performance surface. The fixture serves one program's connections; the
kernel test runs one per probe, three in all, in one boot.

## Prosecution

- **The fixture is the oracle the sims are checked against.** A sim more lenient
  than the kernel makes a host test green that the guest fails; the qualification
  is what finds it. Prosecute: does every probe leg that claims a *kernel*
  refusal actually prove no server contact (the counter deltas), or could a
  server-side Rlerror with the same errno pass it?
- **The guest sabotage.** `work/oct15-na4d/guest-sabotage.py` turns the kernel
  or an adapter red one mutant at a time (27 mutants, October 18); each must turn
  exactly `loom.native_probe_rust` and/or `_c` red at its own named check with
  its predicted `got` value. Three mutants (`k6`, `a4`, `a4c`) were mis-designed
  -- masked by legitimate redundancy (the triple CQ-overfill guard; the
  driver-close session end, not the fd-syscall cleanup) -- and were re-targeted,
  not the code; the JOURNAL carries the diagnosis. A mutant that fails to
  discriminate is a finding about the probe or the mutant, never waved away.
- **The host sabotage.** `work/oct15-na4d/host-sabotage.py` (118 cases) turns
  each adapter and sim assertion red on the host; the two kernel-module host
  tests (`tools/test-loom-service-requests.py`, `tools/test-loom-service-fids.py`)
  turn each kernel assertion red. The FID-module host test had never compiled
  before this close (its `walk-opened` mutant deleted the sole use of a
  parameter): a committed test is not a passing test until something runs it.

## Seams

- [[sub-loom-native-adapter]] is the adapter this qualifies; [[sub-kernel-loom]]
  is the kernel service it runs against.
- **AS-3's list** (ASYNC-SERVICE-LIFECYCLE.md section 9) is the map from a
  required case to its leg or kernel schedule; a whole-boot SMP qualification of
  these legs is owed and is not run while the full gate is on hold.
- **AS-4** (the clipboard client) is the first real guest consumer of the
  adapter; the fixture is its stand-in until then.

## Caveats

- The probes are short-lived, so a leaked kernel ring descriptor (a `close_fd`
  that no-ops) is reclaimed at process exit and the kernel test still sees rings
  retired == made; the harness does not witness *prompt* descriptor release.
  Acceptable for a qualification harness -- the adapter's own close is prompt (on
  the last mapping or lease drop), and the kernel-descriptor leak is an adapter
  concern, owned by [[sub-loom-native-adapter]].
- `audit: light`: the fixture is a test server, not a production authority
  surface. The surface it qualifies -- both adapters and the kernel service --
  is `audit: hard` and owned by [[sub-loom-native-adapter]] and
  [[sub-kernel-loom]].

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
