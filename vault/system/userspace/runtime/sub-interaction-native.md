---
id: sub-interaction-native
type: sub
title: "the interaction client's native adapter -- HIN1 over a private ring"
parent: moc-userspace-runtime
code:
  - usr/lib/interaction-native/Cargo.toml
  - usr/lib/interaction-native/src/lib.rs
  - usr/lib/interaction-native/src/qualification.rs
  - usr/lib/interaction-native/src/tests.rs
audit: hard
guarded-by: [inv-i29, inv-i30]
validated-by: [prose]
locks: []
hazards: []
abis: [abi-loom-service]
design:
  - "docs/ASYNC-SERVICE-LIFECYCLE.md \"AS-4 adoption design (November 3)\""
created: 2026-11-03
updated: 2026-11-03
---
## Purpose

`interaction-native` gives a UI host the shared clipboard and mode reporting without
a blocking call. It carries [[sub-libhalcyon]]'s `interaction_client`, which has no
I/O, over one private Loom ring ([[sub-loom-native-adapter]]). The client decides what
comes next and when; this crate connects, moves bytes and reports what happened.

A host keeps its one poll. It adds `poll_fd`, bounds the sleep by `next_deadline`, and
calls `pump`, which never sleeps. A server that stops at any step costs the host one
event, and typing carries on. Dropping the `Host` closes the ring, which ends the whole
private connection whatever the server is doing.

## Contract

- `parse_locator` accepts `/srv/<service>/<token>/interaction` and nothing else. The
  locator is not authority.
- `Host::new(kernel, registry_fd, locator)` makes the ring and the target. Without the
  private ring it answers -EOPNOTSUPP, and the host carries on with its local registers
  and tries no other path.
- `poll_fd` is for the host's poll set, POLLIN. It is None once the ring is closed or
  has failed, and the host must drop it then.
- `next_deadline` is the latest the host's poll may sleep until, if anything is owed.
- `pump(now)` does everything that can move without sleeping. Call it when `poll_fd`
  is ready or `next_deadline` has passed.
- `report_mode`, `copy`, `paste` and `cancel` pump before they return, so a host cannot
  start something and then sleep past it.
- `take_event` yields what the client decided: `Copied`, `CopyUnknown`, `CopyFailed`,
  `Pasted`, `PasteFailed`, `ModeRefused`.
- Only the thread that made the `Host` may use it, as with the ring.

## Mechanism

**Setup.** One ring of 8 entries and two registered buffers of `MAX_RECORD`, one per
lane. A lane's operations use the whole of its buffer, and the client issues one at a
time on a lane.

**Connecting.** `Step::Connect` submits the CONNECT and boxes a future that then walks
and opens each lane in turn: the token, then `interaction`, then LOPEN for reading and
writing. One WALK takes one component, so each lane's directory fid is dropped and
clunked in the background. Per-operation kernel deadlines stay unset, because the
client owns every deadline and acts on one by closing.

**A pump.** Up to 16 passes of: one `Ring::turn` at a past deadline, a poll of the
connect and of each lane's operation with a no-op waker, delivery to the client, then
every step the client has. The waker does nothing because every pass polls everything.
A pass that moved nothing ends the pump. The bound means a server answering as fast as
it is asked cannot hold the host's input. Each pass leaves its operations in flight and
its completions in the CQ, so the ring announces whatever remains.

**Closing.** `Step::Close` drops the connect, both lanes' operations and the
connection. A dropped operation aborts its scope, and the last holder of the scope does
so anyway, so no completion of anything started before can reach the client.

**A lent buffer.** An aborted operation keeps its buffer until its terminal is routed,
so a write on the next connection can find the ring refusing it with -EBUSY. That is
our own ring, not the server, and the client must not hear it as HIN1's `Busy`. Such a
write is parked and retried on later pumps. The terminal it waits for is a poll event.
If it never came, the client's deadline for that step would close the connection. Only
a write can meet this: a lane's first operation is always a write, and its buffer is
free again by the time the read that follows it starts.

**The SAK witness.** The feature `qualification` adds a module and changes nothing in
`Host`. `Stepping` wraps a ring's `Kernel` and holds its thread once, inside the ENTER
that crosses a numbered submission: before it reaches the kernel, or just after. Every
SQE goes through ENTER, so the numbers cover every operation with no list of phases to
go stale. `run` reports a mode, copies, then reads back through whatever connection is
left. What a copy reported bounds what the read may find: `Copied` the new text,
`CopyFailed` the old, `CopyUnknown` either, and with nothing held only `Copied` will do. A
connection that had formed when the hold began must fail at its next use, unless the
client has already seen it go, and the value is read again after recovery, because the
first read may have finished before the SAK landed. `kaua-term-probe --clipboard async-sak` supplies
the system calls and `ls-halcyon-clipboard-async` the key presses.

## Data structures

`Host` holds the `Client`, the boxed connect, the `Connection` (the scope and a fid per
lane), one `Op` per lane, the token, the target and the ring. `Op` is `Idle`,
`Parked`, `Write` or `Read`. A parked write keeps its own copy of the bytes, since a step's bytes are valid only until the next call into the client.

## Concurrency

None. It runs on the creating thread, like the ring it owns, and holds no lock.

## Invariants enforced

None of its own. The ring's I-29 and I-30 rest with [[sub-loom-native-adapter]]. What
it must not break is that a refusal the server chose and a failure of our own are told
apart, which the client relies on to decide between an answer and a lost connection.

## Error paths

| Where | What happens |
|---|---|
| `Ring::setup` refused | `Host::new` returns the errno. |
| `Target::connect` refused at submission | `connect_failed`: the job fails `Lost`, and the client backs off. |
| A walk or open fails | The same, from the boxed future. |
| A write refused at submission with -EBUSY | Parked. |
| Any other refusal at submission, -ENOSPC included | `failed(lane, -EIO)`. -EIO is outside the table of HIN1 failures, so the client sees a lost connection, never a refusal. |
| A completion's negative result | Passed to `failed` as it stands. On the write that finishes a record, a value in the table is the server's refusal. On a reply's read, or outside the table, it loses the connection. |
| The ring closed or failed | Every future fails, `poll_fd` is None, and the next use reports `Lost`. The ring is not rebuilt. |

## Performance

At least one ENTER per pass of a pump, since each pass turns the ring and that is how it
learns the ring is still there. No poll. A paste of the full 1 MiB clipboard is 64 Read records. Each copy or
paste allocates its text once, and a parked write copies its record.

## Prosecution

Host tests (`tests.rs`, 28) drive the adapter against [[sub-loom-native-adapter]]'s
simulated kernel and a scripted server speaking real HIN1 records. Every wait goes
through one poll loop, as a host's does, and the loop asserts that only its own poll
reached the kernel. The sweep `a_server_that_stops_at_any_operation_...` stops the
server at each of 40 operation counts: each stop must cost one event in at most the 120
s job lifetime, then a new copy must succeed over a new connection.

The module `sak` runs the qualification against a simulated SAK, which ends the
connection the way the server's close does, at every submission, at both moments, with
each operation in flight and with its completion unrouted. Its negatives are a server
that keeps the connection, stale bytes, a closed ring, and failures with no SAK to blame.
`a_number_names_one_phase` pins what each number means, and the scenario quotes it.

The tests run the session's real message size, 8192, so a record longer than one message
goes in parts and the number table matches a guest's. Round 1 of the audit found that the
sim had no such limit and that its scripts refused on the read, where no real server does.

Sabotage, run 2026-11-03 on the Pi against a private copy: named mutants across this
crate, `Ring`'s turn at a past deadline and the client core, each predicted to redden a
named test. The first sweep showed four real gaps, each now a test: a close with nothing
in flight, a refused read, a stale generation at commit, and a `Gone` commit. It also
showed code nothing could reach, which went: a parked read, parking on -ENOSPC, the
client's copy of `Header::decode`'s length checks, and a state check in `poll_fd` that
was redundant because a failure already closes the descriptor.

The tests found one real defect while they were being written. `Ring::turn` at a past
deadline issued no poll and so never learnt that the ring had closed; a host would have
spun on POLLHUP. See [[sub-loom-native-adapter]].

## Seams

- **Up:** AS-4c's qualification mode in `kaua-term-probe`, not yet booted, and AS-4d's
  Nora, not written. A program supplies `ThylaKernel` and opens `/srv` itself.
- **Down:** the ring's futures and `Ring::poll_fd`.
- **Sideways:** `ut` uses the C adapter instead (AS-4e), which has no `poll_fd` yet.

## Caveats

1. **No guest has run this.** The route `<token>/interaction` and the private CONNECT
   against halcyond are first exercised by `ls-halcyon-clipboard-async`, which has not
   run. A host test cannot prove them, since the sim is a model of the kernel and the
   server is a script.
2. **The death of an idle connection is learnt at its next use.** Nothing is in flight
   then, so nothing tells the client. If that use is a copy or paste, that job reports
   `Lost` and the one after it reconnects; a mode report heals the link without a
   visible failure. SAK closes the control connection, so this is the ordinary case after
   a trusted-path episode. AS-4f's list has to judge whether it is acceptable.
3. **A closed or failed ring is not rebuilt.** Both mean something broke the adapter's
   contract, such as another thread closing the descriptor. The host keeps its local
   registers from then on.
4. **A descriptor number reused by another ring is not seen.** ENTER admits any Loom, so
   if something closes this ring's descriptor behind the adapter's back and a new ring
   takes the number, the host polls that ring. It takes a broken contract and a second
   ring in the same process, so nothing guards it.
5. **A target outlives no re-post.** The name is resolved at `Host::new`. If the service
   is posted again under the same name, every later connect answers -ENOENT, so the job
   reports `Lost` for the host's life. A session's service lasts as long as the session
   the host runs in.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
