---
id: abi-note-names
type: abi
kind: registry
stability: append-only
title: "The note-name registry — the deliverable set, the reserved prefixes, and struct note_record"
pinned-by:
  - "_Static_assert sizeof(struct note_record) == 32 == sizeof(struct Note) (kernel/include/thylacine/notes.h)"
  - "_Static_assert per name: sizeof(NAME) <= NOTE_NAME_MAX (12 asserts)"
  - "g_known_notes[] (kernel/notes.c) — the authoritative deliverable set"
mirrors:
  - "usr/lib/pouch/patches/0007-pouch-signals.patch: __pouch_sig_to_note, the open class"
  - "usr/lib/pouch/patches/0021-pouch-pty.patch: __pouch_sig_to_note, the tty: class"
created: 2026-08-02
updated: 2026-08-02
---
## The surface

A note is a name and a small argument delivered asynchronously to a Proc.
Names are strings, not numbers — the Plan 9 model — bounded by
`NOTE_NAME_MAX = 16` bytes including the NUL.

`g_known_notes[]` in `kernel/notes.c` is the authoritative set: a name not in
it is refused at post. Nine names are deliverable, in three classes that
differ by **who may post** and **what an uncaught one does**.

### Open class — anyone may post, catchable

| name | arg | default if uncaught |
|---|---|---|
| `interrupt` | 0 | **terminate** (LS-5) |
| `kill` | 0 | terminate — **non-catchable**, bypasses mask and handler |
| `pipe` | 0 | none (informational) |
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

### `snare:` class — kernel-post only, reserved

The fault family. Seven names are defined and `_Static_assert`ed to fit, and
**none is in `g_known_notes` at v1.0** — the fault path calls `exits()`
directly rather than routing through `notes_post`. They are a reserved
namespace with the delivery machinery not yet wired.

`snare:segv` (no VMA / W^X / permission) · `snare:bus` (VMA-covered, the
Burrow cannot satisfy) · `snare:align` · `snare:bti` · `snare:brk` ·
`snare:ill` (unknown sync EC) · `snare:fpe` (reserved; nothing emits it).

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
five tty names, and `NOTE_BIT_SNARE` would defer all seven snare names.
Per-kind masking within a family is a later extension. `NOTE_BIT_SNARE` has
no consumer today and is reserved so that the documented bit assignment is
honoured by a real symbol rather than a comment.

Setting mask bits outside `NOTE_MASK_SUPPORTED` **succeeds and does
nothing** — tolerated so the supported set can grow without an ABI break.
Posting an unsupported *name*, by contrast, is refused.

## Change protocol

Append-only. A new name means: add it to `g_known_notes` with its mask bit,
add a `sizeof(...) <= NOTE_NAME_MAX` assert, and decide its class — if it
belongs to a reserved prefix, confirm the post gate covers it.

A new *family* needs a new `NOTE_BIT_*` and a bump of
`NOTE_MASK_SUPPORTED`. Nothing derives that constant from the bits, so the
bump is manual; unlike `RIGHT_ALL` in [[abi-handle-rights]] the failure is
benign (an unmaskable note, not a rejected operation), because unknown mask
bits are tolerated rather than validated.

## Where the prose has drifted from the code

`sys_postnote_handler` describes "the v1.0 supported set" as
`"interrupt"/"kill"/"pipe"/"child_exit"` — four names, where the set is nine.
The sentence is stale as written but harmless in consequence: those four
*are* exactly the userspace-postable subset, since the other five are
prefix-gated. A reader taking it as the registry would be wrong; a reader
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
