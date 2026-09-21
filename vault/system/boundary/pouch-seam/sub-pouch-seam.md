---
id: sub-pouch-seam
type: sub
parent: moc-pouch-seam
title: "The syscall seam — the number table, the sentinel, the error decode, stdio"
code:
  - usr/lib/pouch/patches/0001-pouch-syscall-seam.patch
  - usr/lib/pouch/patches/0002-pouch-stdio-no-iovec.patch
  - usr/lib/pouch/patches/0008-pouch-hw-syscalls.patch
  - usr/lib/pouch/patches/0032-pouch-sysconf-nprocs.patch
  - usr/lib/pouch/patches/0034-pouch-sysconf-physpages.patch
  - usr/lib/pouch/patches/0035-pouch-stdio-read-refill.patch
  - usr/lib/pouch/patches/0036-pouch-tmpfile-delete-on-close.patch
  - usr/lib/pouch/patches/0037-pouch-unchecked-sentinel-wrappers.patch
  - usr/pouch-hello/pouch-hello-malloc.c
  - usr/pouch-hello/pouch-hello-fopen.c
audit: hard
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
design: ["docs/POUCH-DESIGN.md"]
created: 2026-08-01
updated: 2026-09-21
---
## Purpose

The one place a POSIX call becomes a Thylacine syscall. musl funnels
every OS operation through `__syscallN` / `__syscall_cp` with a number
from `arch/aarch64/bits/syscall.h.in`; pouch rewrites that table to
Thylacine numbers, guards the entries it has no answer for, and decodes
the kernel's flat-`-1` error convention into POSIX `errno`. Everything
else in the series is a lower-half file riding this seam.

## Contract

- **`bits/syscall.h.in`** — every `__NR_*` macro carries either a
  Thylacine syscall number or `0xFFFF`, the unimplemented sentinel. The
  file is reproducible after a musl re-vendor by the awk filter recorded
  in the 0001 header (a name→number map; everything unmapped becomes the
  sentinel).
- **`__syscall0..6`** (`arch/aarch64/syscall_arch.h`) — each begins
  `if (n == POUCH_SYSCALL_UNIMPL) return -ENOSYS;` BEFORE issuing `svc`.
- **`__syscall_cp`** (`src/thread/__syscall_cp.c`) — the same guard, at
  the C chokepoint every *cancellable* call resolves through.
- **`__syscall_ret`** (`src/internal/syscall_ret.c`) — `r == -1` →
  `errno = EIO`, return -1; `r` in `[-4095,-2]` → `errno = -r`; else pass
  through.
- **stdio** — `__stdio_write` / `__stdio_read` move bytes with
  `SYS_write` / `SYS_read` instead of `writev` / `readv`. `__stdio_read`
  issues exactly ONE `SYS_read` per call and, for a one-byte request on a
  buffered stream, leaves the byte it returns at `f->rpos[-1]` (0035).
- **`sysconf`'s machine figures come from `/ctl`, or are `-1`** —
  `_SC_NPROCESSORS_*` reads `/ctl/sched` "cpus: N" (0032, fail-soft to 1);
  `_SC_PHYS_PAGES` / `_SC_AVPHYS_PAGES` read `/ctl/memory` "total:" /
  "free:" (0034), and a miss is `-1` with `errno` untouched — never a
  guess.

## Mechanism

**The sentinel is the whole design.** A retargeted-to-`0xFFFF` call is
short-circuited in userspace: no trap is issued, so **P-1** holds
structurally (no foreign number can reach the kernel even by accident),
and the caller sees a clean `ENOSYS` rather than a Thylacine `-1`
meaning something else, so **P-3** holds too. One mechanism, two
invariants, zero runtime cost on the live path.

**Both syscall paths must be guarded.** The guards in `__syscallN` cover
only the non-cancellable path; musl's cancellable path runs a
hand-written `__syscall_cp_asm` that carries no guard, so a retargeted
cancellation-point call (`nanosleep`, `read`, `open`) on a
cancellation-enabled thread would have issued `svc` with `x8 = 0xFFFF`.
That was the seam round's **P0** ([[fnd-seam-r1-f1]]) and the fix is the
C chokepoint guard above.

**Two ways to name a Thylacine number**, and which one a patch uses is
decided by whether Linux has the name at all:

- *Repoint* the macro in `bits/syscall.h.in` — when a Linux name exists
  and means the same thing (`__NR_read 9`), or exists and is being taken
  over (0010's `#undef __NR_fstat` / `#define __NR_fstat 50`, spelled
  loudly so a future reader sees the redefinition). Thylacine-only names
  (torpor, thread_spawn, the note family, the hw family, the tty family)
  are appended here too, so musl's build-time sed pass generates the
  `SYS_*` aliases the patched sources reference.
- *Define locally* as `SYS_thyla_*` in a pouch-private header (0019
  introduced the idiom for `SYS_stat`; 0024 and 0026 adopted it). The
  reason is mechanical: aarch64's LP64 table has no legacy `__NR_stat`
  to repoint, so there would be no generated alias — and churning
  `syscall.h.in` for every new call makes the re-vendor diff worse.

**The error decode's ordering is load-bearing**: `-1` also satisfies
`r > -4096UL`, so the flat-error test must precede the range test or
every Thylacine failure would report `errno = 1` (`EPERM`).

**The `_Static_assert` pins the sentinel's VALUE across two files**
(the `0xFFFF` literal in the table, the `POUCH_SYSCALL_UNIMPL` macro in
`syscall_arch.h`) — and explicitly does NOT witness that the guards
exist; that is the two source patches' job. Its witness is
`SYS_io_setup`: Linux async I/O, which POUCH-DESIGN §8.2 defers forever,
so it can never gain a number and quietly falsify the assert.

**stdio without iovec.** `__stdio_write` loops `SYS_write` over the two
spans musl would have passed to `writev` (the stream's pending buffer,
then the caller's new data), returning the count of *new* bytes on
error — musl's contract. A `cnt <= 0` return is treated as terminal,
which is a deliberate coupling to Thylacine's write semantics (a write
blocks until it makes progress and returns -1 on a dead peer; it never
0-returns for flow control) and avoids the unbounded spin musl's
writev loop would take on a 0.

**The read backend, and the sentence this dossier used to carry about it.**
Until 2026-09-21 this paragraph said `__stdio_read` "does one `SYS_read`
straight into the caller's buffer, dropping musl's readahead-into-`f->buf`
— throughput, not semantics." That was 0002's own claim, repeated here
unchecked, and it was wrong on both counts. `__toread()` parks a stream at
`rpos == rend == buf + buf_size`; upstream's read then REFILLS `f->buf` and
hands the requested byte back from it, so the byte just returned sits at
`f->rpos[-1]`. The scan helpers depend on exactly that: `shunget()` is a
bare `rpos--`, and `__shgetc()` stores the byte itself only when
`rpos <= buf` (the unbuffered case, into the UNGET area). With 0002 the
byte went straight to the caller, `rpos` stayed at the end of the buffer,
and every pushback stepped onto `buf[buf_size-1]` — a stale byte, usually
NUL. On every real `FILE` the scanf family failed at the first pushed-back
delimiter (`fscanf(f, "%7s %d %d")` over `"alpha 12 -7"` returned 1 with an
empty word; `sscanf` was never affected, a string pseudo-FILE has its own
read), and because the buffer never filled, every `getc`/`fgets`/`getline`
byte was its own syscall.

0035 has two arms and one `SYS_read` in each — never a second read after a
successful first, which would block a pipe or tty that had already
delivered what it had. The **buffered arm** (a buffered stream and
`len <= buf_size`) reads into `f->buf`, serves the caller from it, and
keeps the surplus as readahead: `rpos = buf + k`, `rend = buf + cnt`. For
the one-byte request (`__uflow`) this is upstream's own code — that arm
never used `readv` — and it is what leaves the returned byte at
`rpos[-1]`. For `1 < len <= buf_size` (`fread`'s remainder loop on a small
record) it is what upstream's `readv` bought: one syscall per buffer-full
instead of one per `fread` — TyrQuake reads its pak header and every demo
message length that way, and on a 9P-backed file each was a round trip.
The **direct arm** (`len > buf_size`, or any unbuffered stream) reads
straight into the caller, and needs no pushback slot because `fread` never
ungets, `ungetc` stores its byte itself, and an unbuffered stream takes
`__shgetc`'s `buf[-1]` path. Readahead survives exactly where upstream's
does: `fread`'s loop calls `__toread` (which drops the buffer) before
EVERY `f->read`, the first included — but it gets there only once the
buffer is exhausted, and a second pass happens only after a SHORT count,
which leaves no surplus. So nothing buffered is ever dropped. The two callers of
`f->read` are `__uflow` (len 1) and `fread` (len = remainder); there are
no others.

The boundary is `>`, and the first draft had `>=`. A host model of the
read state machine (the audit's differential model, extended with the new
arm: random short reads, buffer sizes 0/1/2/3/7/16/64/1024, mixed getc /
fread / ungetc / scan / peek) failed 1889 of 2000 trials at
`buf_size == 1` and none at any other size: a one-byte request on a
one-byte buffer (`setvbuf` with `UNGET + 1` bytes) took the direct arm and
lost the pushback slot — 0002's defect, reintroduced at one size. With
`>`: 0 of 32000, against 26496 of 32000 for the 0002 backend on the same
trials. The model is a copy of the function, so it proves the design, not
the file; the file is held by `pouch-hello-fopen`'s scan leg, which runs
the same text through the default buffer and through `setvbuf` buffers of
9 and 10 bytes (stream buffers of ONE and TWO).

**What libc tells a program about the machine.** Three patches in this
dossier and one in [[sub-pouch-thread]] (0033) are the same defect: a
syscall parked at the sentinel whose libc caller *cannot or does not report
failure*, so the program is told a LIE rather than an error.
`sysconf(_SC_NPROCESSORS_ONLN)` seeded its affinity set with `{1}` and
ignored the ENOSYS, so every pouch program saw one CPU (0032).
`sysconf(_SC_PHYS_PAGES)` called `__lsysinfo(&si)` unchecked and computed
from the uninitialised struct, so it returned stack residue (0034);
JavaScriptCore caps its heap from that figure and refused every allocation
past ~1000 array elements. `pthread_getattr_np` probed the main stack with
`mremap` expecting `ENOMEM` and got `ENOSYS`, so it reported one page
(0033). The sentinel is honest only where the caller propagates the error;
where musl treats a call as cannot-fail, the sentinel produces a wrong
*value*.

0034's parser is deliberately strict where 0032's is soft. The key is
matched at a line start only (`free:` cannot match inside another word), a
figure must be followed by a terminator inside the buffer (a number that
runs to the end of the read may be a truncated prefix, so it is a miss),
overflow clamps to `LONG_MAX`, and every miss is `-1` with the caller's
`errno` restored — POSIX's spelling of "indeterminate". The figure is the
MACHINE's memory; a Proc's real ceiling is its I-32 page budget, which is
smaller, and an engine that sizes its heap from RAM meets the budget first
and must take the clean `ENOMEM` it gets there.

## Data structures

None. The seam is macros, four inline functions, and one decode.

## Concurrency

None of its own. The guards are pure; `__syscall_ret` writes only
`errno` (TLS).

## Invariants enforced

pouch's own four (POUCH-DESIGN §11) — no §28 invariant binds this
surface, which is recorded honestly rather than papered over:

- **P-1** structurally, by the sentinel short-circuit on both syscall
  paths.
- **P-3** structurally for un-retargeted calls (`ENOSYS`); by
  construction for retargeted ones (each lower-half patch owns its own
  errno fidelity).
- **P-4** by per-patch review, and by nothing else. This section, the MOC
  and the series header all said "against the UPPER/LOWER/SEAM inventory in
  `docs/reference/78-pouch.md`" — that file is an absorbed stub and never
  held one; the inventory exists nowhere (audit B-0 r2 F7g). The as-built
  boundary is therefore DERIVED rather than kept:
  `grep -h '^+++ b/' usr/lib/pouch/patches/*.patch | cut -d/ -f2-3 | sort | uniq -c`.
  A patch that adds a directory to that list is a P-4 review event, argued
  in its header. "stdio" is not wholly upper half: its fd-facing BACKENDS
  (`__stdio_read/write/close/seek`, the openers, `tmpfile`) are where libc
  meets the kernel, and 0002 / 0023 / 0035 / 0036 / 0038 patch them; the
  formatting and buffering core above them is untouched. By that
  derivation the B-0 series entered exactly TWO new directories:
  `src/legacy` (0037) and `include/sys` (0039 — the series' first PUBLIC
  header, which is a different kind of event: the change lands in every
  port's objects, not in libc.a). This sentence first listed six and had
  four of them wrong — `src/conf` was 0032's, `src/misc` 0021's,
  `src/unistd` 0006's, `src/select` 0005's — typed from memory under the
  very paragraph that says to derive (audit r3 F8). First-touch per
  directory: `for d in ...; do grep -l "^+++ b/$d/" *.patch | head -1; done`.

## Error paths

`-ENOSYS` (sentinel, both paths). `EIO` for every flat kernel `-1` —
design-sanctioned imprecision, not a bug: Thylacine's convention carries
no errno, so a lower-half wrapper that can determine something better
does so itself before reaching the decode. Explicit `-errno` in
`[-4095,-2]` passes through, which is how the stalk-resolved calls
(`SYS_open` and friends, since ER-1) deliver real `ENOENT` / `EACCES`.

## Performance

The guard is one compare on a register already loaded. The stdio
rewrite costs one extra syscall per flush when the stream has both a
pending buffer and new data (musl's single `writev` became two
`SYS_write`s). Character-at-a-time input is one `SYS_read` per BUFSIZ
since 0035 (it was one per BYTE from 0002 until then), and a run of small
`fread`s is one per buffer-full; only an `fread` LARGER than the buffer
goes direct and leaves the buffer empty, so a `getc` that follows one pays
a refill upstream would have folded into the `readv`. Each
`sysconf` machine query is an open + read + close on `/ctl` — callers
cache it, and nothing hot asks.

## Prosecution

- **The seam-check list must grow with the series.** `build_sysroot`
  greps the *generated* `bits/syscall.h` for each expected number plus
  sentinel representatives — the only defense against a re-vendor
  silently losing an entry (which would degrade a working call to
  `ENOSYS`). Two separate audit rounds found the same defect — the list
  not extended for the round's new numbers ([[fnd-threads9b-r1-f5]],
  [[fnd-signals13b-r1-f1]]) — which makes it a *lineage*, not a
  coincidence: any patch adding a number must add it to the check.
- The guard must stay on BOTH paths; a new syscall wrapper that hand-rolls
  `svc` (as `__pouch_pipe` legitimately does for its two-register return —
  [[sub-pouch-process]]) is outside the guard by construction and must
  carry a real number.
- The `-1`-before-range ordering in the decode.
- `.rej` files after the apply loop abort the build ([[fnd-seam-r1-f6]]);
  `patch -t` alone would silently skip an already-applied patch.
- **A sentinel is only honest where the caller can fail.** Before parking
  a name at `0xFFFF` — and at every re-vendor — read each libc function
  that returns `__syscall(SYS_x)` raw or ignores its result and then
  consumes an out-buffer. Three were found by CONSUMERS rather than by
  review (0032, 0033, 0034); the first sweep then found the open identity
  calls below in minutes. The sweep
  method has TWO halves, and the first version of this paragraph had only
  the first, which is why the audit found five more (0037): (a) list the
  `0xFFFF` names in the PATCHED `bits/syscall.h.in`, then grep the patched
  `src/` for statement-position `__syscall(` and for `return __syscall(` on
  those names; (b) for every libc WRAPPER over such a name (`getrlimit`,
  `sysinfo`, `uname`, `prlimit`, ...), find every libc-INTERNAL caller that
  ignores the wrapper's return and then reads the out-struct. Half (a)
  cannot see half (b)'s sites — there is no `__syscall(` on the line — and
  0034's own bug was a (b). 0037 fixes the five found: `sysconf`'s rlimit
  arm (`_SC_OPEN_MAX` / `_SC_CHILD_MAX` were uninitialised stack with errno
  clobbered; now -1, errno as it was), `getloadavg` (reported SUCCESS with
  garbage samples — GNU make's `-l` consumes it; now -1), `ulimit`,
  `getdomainname`, and `getdtablesize`, which has no error channel and so
  states the kernel's handle-table size. That last is a mirror of
  `PROC_HANDLE_MAX`, and the prover does not compare it with the number
  typed again: it opens `/ctl/memory` until the kernel refuses and requires
  `getdtablesize()` to equal the last fd issued plus one.
- **`tmpfile()` is delete-on-close (0036), because an open file does not
  outlive its last name on this root filesystem.** Upstream creates the
  file, issues a raw `SYS_unlinkat` and ignores the result; that number is
  a sentinel, so NO unlink happened and every `tmpfile()` left
  `/tmp/tmpfile_XXXXXX` on the persistent root (measured: `during=1 nlink=1
  after_close=1`, then 2 on the second run). The audit's suggested fix —
  the public `unlink()` — was tried first and the boot went RED:
  `raw read=-1 errno=2` at EOF of the now genuinely unlinked file.
  `dev9p_read` only forwards it; the refusal is Stratum's and is by design
  (its fid model's IOReject gate, `specs/fid.tla`;
  `verify_fresh_snapshot` in `src/9p/server.c`). The data bytes had come
  from the Larder's own-write pages, so the first WIRE read was the EOF
  probe — which is why a short read-back passed and only the scan leg
  failed. The prover's "the fid survives the unlink" leg had been green
  since 0024 because no unlink was ever issued: one false green hid both
  defects. Now the name lives as long as the stream: `fclose()` removes it
  through `f->close` (close first, then unlink), and streams still open at
  a normal `exit()` are swept by `__stdio_exit` through a weak hook. The
  prover writes three pages, reads them back to a clean EOF over the wire,
  and COUNTS `tmpfile_*` names before / WHILE OPEN (the positive control:
  before + 1, or the counter is blind) / after `fclose` / after a
  self-respawned child — which returns 42, so the parent knows the arm ran
  — exits with a tmpfile still open.
  **The locking was wrong in the first version, twice (audit r2 F3 + F4).**
  It held `tmp_lock` across close + unlink — four 9P round trips, each a
  note-delivery point — so a handler calling `exit()` there met its own
  lock in a threaded program and, single-threaded, found the node unlisted
  with the name still on disk. And its exit sweep took the lock and kept
  it, so a thread that had just `open()`ed blocked for ever holding a name
  the sweep never saw, under a comment saying that could not happen. Now
  the lock is never held across a syscall: `fclose()` closes and unlinks
  while the node is STILL LISTED and unlists last; the sweep sets an
  `exiting` flag, drops the lock, and walks a list that is frozen from
  then on (`fclose()` leaves its node, `tmpfile()` removes its own new file
  instead of listing it). `tmpfile.o` also references
  `__stdio_exit_needed` itself — otherwise only `__toread.o` /
  `__towrite.o` pull it, and a program using `tmpfile()` through `fileno()`
  alone exited through the dummy (r2 F5; verified by `llvm-nm`, which is
  the only witness: any prover that prints links `__towrite.o`). It is
  best effort by nature — `_exit`, `abort`, a kill, or an `exit()` landing
  between `open()` returning and the listing leave the name — and the
  backstop is a boot-time `/tmp` sweep, OWED.
  **Couplings for whoever wires the missing calls** (r2 F8): `fork` — a
  child would inherit the list and its `exit()` would unlink the PARENT's
  live files (needs a child-side reset + `tmp_lock` in the atfork set);
  `dup` — make's `os_anontmp` is `tmpfile()` → `dup(fileno)` → `fclose`,
  which would delete the file under the fd make keeps; the name is
  re-resolved at `fclose()`, so a pivot or a bind over `/tmp` in between
  unlinks in the wrong place; and `getdtablesize()` answers 1024 while
  `sysconf(_SC_OPEN_MAX)` answers -1 — deliberately: the first has no error
  channel and a close-all loop needs a bound, the second has one and the
  kernel exposes no limit to read.
- `__stdio_read` must keep BOTH properties: one `SYS_read` per call, and
  the returned byte at `rpos[-1]` after every buffered-arm read — at EVERY
  buffer size, including one byte. `pouch-hello-fopen`'s `scan` leg pins
  the second three pushbacks deep, at three buffer sizes, plus a small
  `fread` followed by a scan, and checks VALUES, so a scan that "succeeds"
  on the wrong bytes still fails; it was measured RED on the 0002 backend
  (`n=1 w1=[]`) before 0035 was applied.
- The stdio backends are fd CONSUMERS that issue raw syscalls, so the
  socket tag reaches them ([[sub-pouch-net]], 0038).
- The `sysconf` memory figures are pinned from the device side by
  `pouch-hello-malloc`, which re-reads `/ctl/memory` with a DIFFERENT
  parser (stdio) and requires `_SC_PHYS_PAGES` to equal the kernel's total
  exactly. A range check alone would pass on plausible garbage, which is
  what the arm used to return. `_SC_AVPHYS_PAGES` moves, so it is
  BRACKETED: the kernel's `free` is read before and after the libc call and
  libc's figure must sit between the two readings give or take `total/16`,
  and never above `total - reserved` (`phys_free_pages` can never exceed
  the initial free count). The first version asked only `0 < avail <=
  phys`, which a libc that matched the `reserved:` key would have passed.
  NOT pinned, and owed: the honest `-1` when `/ctl` is absent — it needs a
  prover spawned into a namespace without `/ctl`, which no pouch program
  can build for itself. (That pin is how the 0035 defect was found:
  its `fscanf` returned 0 while raw `read`, `fgetc` and `fread` over the
  same file were all correct.)

## Seams

[[seam-pouch-errno-channel]] — the flat-`-1` → `EIO` collapse, and every
per-call errno approximation built on top of it.

## Caveats

- **OPEN (found 2026-09-21): the identity calls return the raw sentinel.**
  `getuid` / `geteuid` / `getgid` / `getegid` / `getppid` are
  `return __syscall(SYS_getuid)`-shaped and their numbers are `0xFFFF`, so
  each returns `(uid_t)-38` = `0xFFFFFFDA`. The kernel HAS `SYS_GETUID` = 73
  (principal_id) and `SYS_GETGID` = 74 (primary_gid); CL-1a wired only
  `getpid`. It is NOT a libc one-liner, because stratumd consumes the value:
  `stm_ctl_set_admin_uid(ctl, geteuid())`, the unauthenticated-peer fallback
  `peer_uid = getuid()`, `stm_fs_init_dataset_root(..., geteuid(),
  getegid())`, and the keyslot token gate `tok_st.st_uid != geteuid()`.
  Changing libc's answer changes the storage daemon's security behaviour, so
  it is its own chunk on the A-3 identity surface. CONFIRMED ON THE DEVICE
  2026-09-21: `uid=4294967258`, `ppid=-38`. Of the other parked names only
  `uname` fails VISIBLY (`-1`/`ENOSYS`): `umask()` returns `0xFFFFFFFF` as the
  "previous mask" from an API that cannot fail and never sets the mask, and
  `times()` returns `(clock_t)-38` -- not the documented `-1` -- with `*tms`
  untouched. The audit rounds' full list of this class (r1 F1 + F8, r2 F1)
  is in `memory/audit_pouch_0033_0035_closed_list.md`. Round 2 found a
  SIXTH out-struct reader round 1's list had waved through: `ualarm()`
  returned the `it_old` a failed `setitimer` never wrote (`alarm()` beside
  it is saved only by upstream's `old = { 0 }`). It now answers
  `(unsigned)-1` / `ENOSYS`, in 0037. That round swept the whole patched
  `src/` — a named-wrapper pass plus a generic pass over the 213
  always-failing wrappers — and those two files were the only out-struct
  readers left.
- **stdio input reads ahead since 0035**, as on every other libc, and
  everything that follows from that is now true here. A program that mixes
  `FILE` reads with raw reads of the same fd, or hands the fd to a child
  mid-stream, sees standard POSIX readahead rather than the accidental
  byte-exact positioning 0002 gave it. `fflush(stdin)`, `fclose` and `exit`
  on a NON-seekable stream discard what was read ahead. An update stream
  that reads then writes without the `fseek`/`fflush` ISO C requires writes
  at the readahead offset. Bytes read through a `FILE` persist in its
  buffer: Stratum's `janusd` reads a passphrase with `fgets` and wipes only
  its own copy (`src/janus/janusd.c`) — true on every libc, new here, and
  OPEN on the Stratum side (`setvbuf(_IONBF)` before the read, or wipe and
  close the stream).
- **OPEN, and bigger than `tmpfile()`: unlink-while-open loses the file.**
  POSIX postpones removal until the last reference closes; Stratum rejects
  I/O on the fid at once (IOReject, deliberate). Every ported program that
  uses the private-scratch idiom (`open`, `unlink`, keep using the fd) and
  every VIVARIUM guest that does is affected. A filesystem-semantics
  question for the operator, not a libc patch; Stratum's anonymous inodes
  (`stm_fs_create_anon`, its `O_TMPFILE`) are the natural substrate, and the
  kernel has no create flag for them. Also owed: a boot-time `/tmp` sweep —
  a process that dies without `exit()` leaves its `tmpfile_*` names.
- **OPEN: tty type-ahead is swallowed, and 0035 is what makes it visible.**
  A canonical-mode read must return at most one line; neither tty layer
  bounds it (`kernel/cons.c`'s read and ptyfs' `ring_drain` both drain past
  the newline). 0002's byte-at-a-time reads hid that. A BUFSIZ read now
  takes the whole type-ahead into the first reader's `FILE`, so a line
  typed ahead for the NEXT program is consumed by this one. The defect is
  in the tty layers (both audit surfaces), tracked as its own item.
- `usr/pouch-hello/` is otherwise unclaimed — run `quaestor owner
  usr/pouch-hello/*` for the count rather than trusting one typed here
  (it was wrong two rounds running). [[sub-pouch-thread]] claims
  `pouch-hello-threads.c` and [[sub-pouch-net]] `pouch-hello-sockets.c`;
  this one claims the two provers whose new legs pin mechanisms described
  here, not the directory. joey matches each prover on a LEG CENSUS
  (`<name>: legs=a,b,c: exit 0`), not on `exit 0` alone, so a stale binary
  — the bake traps that skip a populate — cannot pass for a new one.
- **`docs/REFERENCE.md`'s pouch row (absorbed) says "seven patches" and
  "Ten pouch binaries"** — both long stale (count the series with
  `grep -vc '^#\|^$' usr/lib/pouch/patches/series`, never from a number
  typed here: this caveat said 31 while the series was 38). The row was written at sub-chunk 14 and never
  re-counted.
- **`78-pouch.md` (absorbed) carried a caveat asserting the opposite of
  the patch it documents**: "`exit` and `exit_group` both terminate the
  whole process. Both map to `SYS_EXITS`." Since #809 the table maps
  `__NR_exit → 0` (`SYS_EXITS`) and `__NR_exit_group → 60`
  (`SYS_EXIT_GROUP`), and 0001's own header says so in as many words.
- The same doc's "Terminal detection always reports 'not a tty'" caveat
  was retired by 0021 and #55c ([[sub-pouch-tty]]) — the section
  documenting the working `isatty` sits 400 lines below the caveat
  denying it.
- 0011 / 0012 / 0013 (the termination overrides, [[sub-pouch-process]])
  are documented in `86-pouch-stratumd-boot.md` and were never mentioned
  in the pouch reference at all.

## Provenance

[[chg-2026-05-22-p6-syscall-seam]] (0001 + 0002 + the build wiring;
[[adt-seam-r1]] 1 P0) → [[chg-2026-05-25-16b-beta-hw-openat]] (0008, the
hw numbers for Stratum's in-process virtio-blk driver) → the number table
has grown at nearly every subsequent pouch landing.

[[chg-2026-08-15-build-targets]] **dropped `tools/build.sh` from this
dossier's `code:` list.** The claim dated from the original landing ("0001 +
0002 + the build wiring") and had never been paid for: the whole file
mentioned the build script exactly once, in the `code:` line claiming it.
Meanwhile [[sub-substrate-build]] describes the sysroot rebuild, the patch
series application and the staleness checks in full, so nothing was lost.
What it cost while it stood was a false signal — an 841-line churn figure
attributed to a dossier that described none of it, competing for a place at
the head of the sweep queue. Same narrowing as the batch-35 pass, for the
same reason: **traversal is not a sweep, and neither is being built by
something.**
