---
id: abi-note-names
type: abi
kind: registry
stability: append-only
title: "The note-name registry — the deliverable set, the reserved prefixes, and struct note_record"
pinned-by:
  - "_Static_assert sizeof(struct note_record) == 32 == sizeof(struct Note) (kernel/include/thylacine/notes.h)"
  - "_Static_assert per name: sizeof(NAME) <= NOTE_NAME_MAX (14 asserts)"
  - "g_known_notes[] (kernel/notes.c) — the authoritative deliverable set, with each name's death signal"
  - "g_snare_deaths[] (kernel/notes.c) — the snare family's death signals; NOTE_SIG_* == VIV_SIG* _Static_asserts"
  - "SNARE_ABORT_BRK_IMM 0xab06 (kernel/include/thylacine/notes.h) — the brk immediate that is snare:abort"
mirrors:
  - "usr/lib/pouch/patches/0007-pouch-signals.patch: __pouch_sig_to_note, the open class"
  - "usr/lib/pouch/patches/0021-pouch-pty.patch: __pouch_sig_to_note, the tty: class"
  - "usr/lib/pouch/patches/0011-pouch-abort.patch: abort() = brk #0xab06 (the SNARE_ABORT_BRK_IMM literal)"
created: 2026-08-02
updated: 2026-10-12
---
## The surface

A note is a name and a small argument delivered asynchronously to a Proc.
Names are strings, not numbers — the Plan 9 model — bounded by
`NOTE_NAME_MAX = 16` bytes including the NUL.

`g_known_notes[]` in `kernel/notes.c` is the authoritative set: a name not in
it is refused at post. Ten names are deliverable, in classes that differ by
**who may post** and **what an uncaught one does**: the open class, the `tty:`
class and `sak` (all below). The table also records, per name, the POSIX signal
a death by that note reports to a packed wait (gcpark-b; "The death signal"
below).

### Open class — anyone may post, catchable

| name | arg | default if uncaught |
|---|---|---|
| `interrupt` | 0 | **terminate** (LS-5) |
| `kill` | 0 | terminate — **non-catchable**, bypasses mask and handler |
| `pipe` | 0 | **terminate** (SIGPIPE's default; a write to a reader-less pipe posts it) |
| `child_exit` | packed pid + status | none (informational) |

`kill` is the standing exception to nearly every rule below: it is scanned
for first regardless of FIFO position, ignores the per-Thread mask, ignores
`in_handler`, and is invisible to the fd-read path — a Proc reading
`/dev/notes` must not be able to consume its own kill.

### `tty:` class — kernel-post only, catchable

Posted by the terminal seam and the controlling-terminal paths. A target may
catch or mask them like any note (shells install handlers for these
routinely).

| name | POSIX analogue | default if uncaught |
|---|---|---|
| `tty:winch` | SIGWINCH | none (informational) |
| `tty:susp` | SIGTSTP | **stop** — consumed at post time, never queued |
| `tty:cont` | SIGCONT | none; the resume is a kernel stop-clear, not a disposition |
| `tty:quit` | SIGQUIT | terminate |
| `tty:hup` | SIGHUP | terminate |

`tty:susp` is the odd one: an *uncaught* susp never enters the queue at all.
The stop happens at post time, so nothing stays pending across it. Only a
*caught* susp is queued.

### `sak` — kernel-post only, catchable, ignored by default

The secure-attention event (IM-1, 2026-09-07; docs/ERRORS.md has its row):
posted only to the trusted Proc (corvus) when a serial BREAK opens a trusted
episode. An exact-name gate in `notes_post` keeps userspace posters out, and an
uncaught `sak` never terminates its target. Mask bit `NOTE_BIT_SAK` 6.

### `snare:` class — kernel-post only, reserved

The fault family. Eight names are defined and `_Static_assert`ed to fit, and
**none is in `g_known_notes`** — the fault path ends the Proc directly
(`exits_note()`) rather than routing through `notes_post`. They are a reserved
namespace with the delivery machinery not yet wired.

`snare:segv` (no VMA / W^X / permission) · `snare:bus` (VMA-covered, the
Burrow cannot satisfy) · `snare:align` · `snare:bti` · `snare:brk` (any EL0
`brk` but one) · `snare:ill` (unknown sync EC) · `snare:fpe` (reserved; nothing
emits it) · `snare:abort` (gcpark-b, 2026-10-12: the EL0 `brk #0xab06` that
Pouch's `abort()` executes; the immediate `SNARE_ABORT_BRK_IMM` is ABI with the
name).

## The prefix gates, and why only one is load-bearing today

`notes_post` refuses a non-synthetic (userspace) post whose name begins with
`snare:` or `tty:`. The two gates have different standing:

- The **`tty:` gate is load-bearing right now**, because those names *are*
  in the deliverable set. Without it a userspace Proc could post `tty:cont`
  to a debug-stopped child and resume it — an I-39 leak. The gate is the
  only thing preventing it.
- The **`snare:` gate is future-proofing.** Those names are not in the set,
  so the set-membership check would refuse them anyway. It exists so that
  adding `snare:*` for kernel-synthetic delivery does not silently open
  userspace fault-forgery — a Proc faking a `snare:segv` in a sibling's
  queue to fool its `/dev/notes` consumer.

The reserved-prefix rule is therefore: **a prefix class is closed to
userspace at the post gate, not at the name table**, so a name can be added
to the table without re-deciding who may post it.

## Authority: two cross-Proc paths, two different models

Worth stating plainly because the natural assumption is that they are
unified, and they are not.

- **`SYS_POSTNOTE`** to another Proc requires the caller be the target's
  **parent**. Not owner-identity, not a capability — parenthood. A
  non-parent gets `-1` whatever the name and whatever it holds.
- **`/proc/<pid>/ctl`** uses the I-26 two-axis gate: owner-identity on the
  `0600` ctl file, OR `CAP_HOSTOWNER`, OR `CAP_KILL`.

So a supervisor holding `CAP_KILL` kills through the filesystem, never
through `SYS_POSTNOTE`. Additionally, `SYS_POSTNOTE` refuses any target not
`ALIVE` — a post to a zombie has no consumer and would return success
misleadingly; `wait_pid` is that channel.

A multi-thread target receiving `kill` is cascade-terminated via
`proc_group_terminate` rather than refused (the earlier `kill` → `EIO`
behaviour is gone).

## The wire record

```c
struct note_record {           // 32 bytes, ABI-pinned
    char name[16];             // NUL-terminated within
    u32  arg;                  // child_exit packs pid+status; others 0
    u32  sender_pid;           // 0 for kernel-synthetic
    u64  timestamp_ns;         // monotonic at post
};
```

Pinned byte-for-byte identical to the in-kernel `struct Note`, which is what
lets `devnotes_read` `memcpy` one record per `read()` under the queue lock
with no field marshalling. One record per read at v1.0 — vectored reads are
a later extension.

`NOTE_NAME_MAX = 16` is chosen against the record, not the other way round:
Plan 9 used 128, and 16 is what keeps the record at 32 bytes while fitting
the longest current name (`child_exit`, 11 + NUL). The assert deliberately
uses `<=`, so a name of exactly 15 characters plus NUL is legal — the
padding NUL coincides with the source NUL.

## The mask bits

`NOTE_BIT_*` indexes the per-Thread mask (set = defer). Per-Thread, not
per-Proc, so a multi-thread Proc can route different notes to different
threads.

`INTERRUPT` 0 · `KILL` 1 · `PIPE` 2 · `CHILD_EXIT` 3 · `SNARE` 4 · `TTY` 5,
with `NOTE_MASK_SUPPORTED = 0x3f`.

Two bits cover **families**, one bit each: masking `NOTE_BIT_TTY` defers all
five tty names, and `NOTE_BIT_SNARE` would defer all eight snare names.
Per-kind masking within a family is a later extension. `NOTE_BIT_SNARE` has
no consumer today and is reserved so that the documented bit assignment is
honoured by a real symbol rather than a comment.

Setting mask bits outside `NOTE_MASK_SUPPORTED` **succeeds and does
nothing** — tolerated so the supported set can grow without an ABI break.
Posting an unsupported *name*, by contrast, is refused.

## The death signal (gcpark-b, 2026-10-12)

A Proc that dies of a note reports that note's POSIX signal to a wait that asks
for the packed word (`WAIT_UNTRACED`, `WAIT_CONTINUED` or `WAIT_PACKED`; a raw
wait reads 1). The mapping is ABI, append-only like the names, and the numbers
are Linux's aarch64 ones so both boundary-lines pass them through:

| death | signal |
|---|---|
| `interrupt` | SIGINT 2 |
| `kill` | SIGKILL 9 |
| `pipe` | SIGPIPE 13 |
| `tty:quit` | SIGQUIT 3 |
| `tty:hup` | SIGHUP 1 |
| `snare:segv` | SIGSEGV 11 |
| `snare:bus`, `snare:align` | SIGBUS 7 |
| `snare:ill`, `snare:bti` | SIGILL 4 |
| `snare:brk` | SIGTRAP 5 |
| `snare:fpe` | SIGFPE 8 |
| `snare:abort` | SIGABRT 6 |

`child_exit`, `tty:winch`, `tty:susp`, `tty:cont` and `sak` cannot kill and
carry no signal. docs/ERRORS.md "Exit-status semantics" is the binding table;
`notes.death_signal_table` pins every row and the rule "a known note kills iff
it records a signal".

## Change protocol

Append-only. A new name means: add it to `g_known_notes` with its mask bit and
its death signal (nonzero exactly when its default terminates), add a
`sizeof(...) <= NOTE_NAME_MAX` assert, and decide its class — if it belongs to
a reserved prefix, confirm the post gate covers it. A new `snare:` name goes in
`g_snare_deaths` with its signal.

A new *family* needs a new `NOTE_BIT_*` and a bump of
`NOTE_MASK_SUPPORTED`. Nothing derives that constant from the bits, so the
bump is manual; unlike `RIGHT_ALL` in [[abi-handle-rights]] the failure is
benign (an unmaskable note, not a rejected operation), because unknown mask
bits are tolerated rather than validated.

## Where the prose has drifted from the code

`sys_postnote_handler` describes "the v1.0 supported set" as
`"interrupt"/"kill"/"pipe"/"child_exit"` — four names, where the set is ten.
The sentence is stale as written but harmless in consequence: those four
*are* exactly the userspace-postable subset, since the other six are
gated (the `tty:` prefix and `sak`'s exact name). A reader taking it as the registry would be wrong; a reader
taking it as "what this path accepts" would be right.

## The POSIX mapping, and what it loses

`__pouch_sig_to_note` is the mirror, split across two boundary-line patches:
the open class in `0007-pouch-signals.patch`, the tty class in
`0021-pouch-pty.patch`.

| signal | note | note |
|---|---|---|
| SIGINT | `interrupt` | catchable |
| SIGTERM | `interrupt` | **shares the note with SIGINT** |
| SIGPIPE | `pipe` | pouch masks it at startup |
| SIGCHLD | `child_exit` | default ignore |
| SIGKILL | `kill` | non-catchable; `sigaction()` returns `EINVAL` |
| SIGQUIT | `tty:quit` | |
| SIGTSTP | `tty:susp` | |
| SIGCONT | `tty:cont` | |
| SIGWINCH | `tty:winch` | |
| SIGHUP | `tty:hup` | |

SIGABRT has no note. Pouch's `abort()` is the `snare:abort` trap (0011), which
a parent reads as SIGABRT; a self-directed `raise(SIGABRT)` answers EINVAL
today and the program runs on -- an owned gap (OPEN-BUGS 2026-10-12: route it
through abort()'s trap).

The mapping is **many-to-one at SIGINT/SIGTERM**: both land on `interrupt`,
so a handler cannot tell a Ctrl-C from a polite termination request, and
installing different dispositions for the two is not expressible. That is a
v1.0 boundary loss, not a registry property — the fix is a distinct note
name, not a change here.

The tty family is **receive-only** from pouch: nothing on that side may
originate one, which is the userspace face of the `tty:` post gate above.
`tty:susp`'s kernel default is a stop, and the pouch side deliberately does
not request that arm — changing it would be an ABI-semantics change needing
its own signoff.

## Queue behaviour worth knowing at this boundary

Depth is 16 per Proc. At or above 12 entries a **kernel-synthetic** poster
of an already-queued same-name note coalesces into it (last arg wins,
position preserved). Userspace posters never coalesce and see `EAGAIN`.

The accepted consequence: a console Ctrl-C is a synthetic `interrupt`, so if
a target's queue already holds 16 entries *none of which is an interrupt*,
there is no same-name slot to coalesce into — the interrupt is dropped and
the terminate latch is never armed. That Ctrl-C is lost. The precondition
(16 queued unconsumed notes) is unreachable for a typical foreground
program.

Ordering is post-order per source, with two documented relaxations: `kill`
jumps the queue, and a note re-enqueued at the head after a user-stack push
failure can reverse cross-name order against mask-deferred entries.

## Prosecution

- Adding a name under a reserved prefix without confirming the post gate
  opens userspace forgery of that class.
- Removing the `tty:` gate is an I-39 leak, immediately: a parent could
  `tty:cont` a debug-stopped child.
- Growing `struct Note` breaks the `memcpy` equivalence with
  `struct note_record` and the 32-byte read ABI. Both asserts must be
  bumped deliberately.
- A name of 16+ bytes fails its assert; do not raise `NOTE_NAME_MAX`
  without re-deriving the record size.

## Referenced by

[[sub-pouch-signal]] · [[sub-kernel-proc]] · [[sub-kernel-death]] ·
[[inv-i27]] · [[inv-i39]] · [[moc-boundary]].
