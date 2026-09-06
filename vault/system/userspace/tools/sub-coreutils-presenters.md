---
id: sub-coreutils-presenters
type: sub
title: "The presenters — sixteen tools, one console probe"
parent: moc-userspace-tools
code:
  - usr/coreutils/src/bin/ls.rs
  - usr/coreutils/src/bin/pelt.rs
  - usr/coreutils/src/bin/ns.rs
  - usr/coreutils/src/bin/qid.rs
  - usr/coreutils/src/bin/realm.rs
  - usr/coreutils/src/bin/stat.rs
  - usr/coreutils/src/bin/ps.rs
  - usr/coreutils/src/bin/grep.rs
  - usr/coreutils/src/bin/nc.rs
  - usr/coreutils/src/bin/con.rs
  - usr/coreutils/src/bin/dial.rs
  - usr/coreutils/src/bin/ping.rs
  - usr/coreutils/src/bin/netstat.rs
  - usr/coreutils/src/bin/nslookup.rs
  - usr/coreutils/src/bin/ipconfig.rs
  - usr/coreutils/src/bin/tcpproxy.rs
audit: none
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: []
created: 2026-08-04
updated: 2026-09-06
---
## Purpose

The sixteen coreutils binaries that link the colour modules: the namespace
introspection tools that make Thylacine's own structure visible, the process
presenter (`ps`, added by H-1c-2), and the network clients that frame a
result.

Two groups by subject, one group by consequence. What unites them is that
their output is meant for a person's eyes, which is what earns them colour
under the crate's rule. The terminal question — *is stdout a person's
console or a pipe?* — that none of them could answer when this dossier was
first written is now answered for all sixteen: H-1c-2 (`8922ccd7`) built
the shared probe, so `--color=auto` finally means auto. See Caveats for the
before/after.

## Contract

Same shape as the filters: one binary per file, own argument loop, own
exit status. The difference is the colour flag. Each accepts
`--color[=WHEN]`, resolves it once at startup, and threads the resulting
boolean through every formatting call.

**Every one of the sixteen now defaults `--color=auto`** — the H-1c-2
unification. That collapsed the asymmetry this dossier once described (the
introspection tools defaulting ON, `grep` defaulting OFF): with a working
gate, `auto` colours a console and stays clean in a pipe, which is the
behaviour every tool wanted, so every tool defaults to it. `grep`'s old
default-OFF was a workaround for a gate that always said "yes"; it is now
`auto` like the rest, and its output is byte-clean the moment it is piped.

Four of them — `ls`, `stat`, `grep`, `ps` — additionally accept
`--beacon[=WHEN]` (default `auto`) and gain a second, structured
realization at the Rich tier (Mechanism). The remaining twelve emit SGR
colour only.

## Mechanism

**The namespace tools present a vocabulary the filesystem does not have.**
A `graft` is an entry the directory reader calls a directory and `fstat`
cannot cross — a live kernel namespace mounted into the tree. The failure
*is* the signal, so what would otherwise render as an error row becomes a
first-class kind with its own colour, its own classify suffix, and its own
realm column.

That inference has a cost worth stating: any other cause of a stat failure
on a directory — a permission denial, a transport error, a race with a
removal — also renders as a graft. The classification is
"directory that could not be stat'd", presented as "live namespace mount",
and the two are not the same set. There is a positive source (the mount
list, which `ns` reads from the process filesystem) and the listing tools
do not consult it.

**`ns` reads the kernel's own rendering** rather than deriving anything:
one line per mount, mountpoint and source, where a source with no
namespace name appears as a device specifier. The realm column is derived
from that device character — precise, available now, and requiring no new
kernel surface. With colour off it passes the kernel text through
untouched, which is the right escape hatch for a tool whose subject is
already text.

**`pelt` walks the tree and stops at every graft.** That is its reason to
exist: a general tree walker that descended into a live kernel namespace
would try to walk it as if it were disk. Marked and never entered.

**The network clients frame results in the shared card renderer**, so a
ping summary and a long listing share one visual language, and they share
the connection plumbing — dial-string resolution and the byte pumps — from
the library rather than each reimplementing back-pressure.

**`ps` presents the kernel's process table.** It reads `/ctl/procs` in one
atomic slurp — the kernel renders the whole table under `g_proc_table_lock`,
so there is no readdir race to lose a row to — and offers three
realizations of the same eight columns (PID PPID NAME STATE THREADS PAGES
CHILDREN CPU). Colour off *and* beacon off: the kernel text passes through
**verbatim**, byte-clean and parseable, raw `CPU_NS` intact — the same
pass-through discipline `ns` uses. Colour on: a boxed listing, CPU
humanized (ns -> ms/s) and STATE coloured against the kernel's own
vocabulary (ALIVE green, ZOMBIE ember, STOPPED gold). It parses defensively
— the NAME column is rejoined from the middle fields so a spaced name
cannot shear the numeric columns, and **any** row it cannot parse (kernel
format drift) drops the whole render back to the verbatim text rather than
draw a partial table.

**Four tools gain a Beacon Rich realization** (`docs/BEACON.md`). At the
Rich tier — resolved by the shared `beacon_gate` from the `BEACON` env
export, stdout's Dev class, and the tool's `--beacon` flag — the same plain
bytes go out wrapped in semantic frames a renderer can act on: `ps` emits a
`table` with `obj type=pid` on the PID cells; `grep` wraps each match in
`em class=strong` and tags the filename prefix `obj type=path`; `ls` and
`stat` frame their listings likewise. **SGR colour and Rich are mutually
exclusive** — a tool forces its colour gate off when the resolved tier is
Rich, because the renderer's stylesheet owns typography there. The gate
itself lives in `[[sub-coreutils-lib]]`; what belongs here is that these
four presenters now have a second face aimed at Halcyon's verb menu, not
just at a person's eyes.

## Data structures

Per-tool flag structs and a vector of rows. The card renderer's row type
carries plain and coloured text separately, because the plain form sizes
the box and the coloured form prints.

## Concurrency

None. Single-threaded; the network tools multiplex with poll.

## Invariants enforced

None directly. The network clients compose with the daemon's ownership of
the interface — they reach the network only through the granted filesystem
tree and touch no hardware. The introspection tools read what their
namespace shows them, which is itself the containment.

`ns` is the sharpest illustration: it displays another process's mount
list, and the authority for that is the kernel's check on the process
filesystem, not anything here.

## Error paths

Standard for the suite: named diagnostics on stderr, two for usage, one
for failure. The network tools distinguish an unresolvable host from a
malformed dial string, which is the distinction a user needs.

`grep` continues across an unreadable operand and reports it, returning
non-zero at the end.

`ps` reports a `/ctl/procs` open/read failure on stderr and exits non-zero;
a row it cannot parse is *not* an error but a trigger to emit the kernel
text verbatim (Mechanism) — the exit stays zero, because the data was
delivered, just not styled.

## Performance

Listing cost is dominated by the per-entry stat. The box-fitting pass
walks the rows twice — once to measure, once to draw — which is free at
directory scale.

## Prosecution

- **The colour gate must be resolved once and threaded.** All sixteen
  resolve it at startup and pass a boolean down; a tool that re-derived it
  mid-run could emit a half-coloured line.
- **A new presenter must call the shared probe, never re-derive the
  Dev-class check.** Task #156 — "the count of hand-written probes should
  never reach sixteen" — was closed the right way: instead of `ps` adding a
  sixteenth stub, H-1c-2 built the one shared probe
  (`libthyla_rs::stdout_is_terminal`, over `SYS_FD_DEVCLASS`), the fifteen
  `stdout_is_console` stubs collapsed to one-line wrappers delegating to it,
  and `ps` calls it directly. There is now exactly one probe *body* in the
  suite; a new presenter that wrote its own would reopen the divergence #156
  guarded against.
- **`grep`'s default is `auto`, and that is only safe because the gate
  works.** `grep` is the one tool here whose output is ordinarily a payload;
  its old default-OFF was a workaround for a gate that always answered "yes".
  With a real gate, `auto` resolves OFF in a pipe on its own, so the default
  moved to `auto` like the rest — but the safety now *depends on* the probe
  correctly classifying a pipe. A regression that made `stdout_is_terminal`
  return true for a pipe would colour a `grep` payload by default; the pipe
  path is the one to guard.

## Seams

There is no name service for user identities, so an owner column shows a
number for anyone but the system principal.

The graft classification cannot distinguish a mount from any other
stat failure (above). Consulting the mount list would fix it and would
cost a read per listing.

## Caveats

- **`--color=auto` now means auto — RESOLVED by H-1c-2 (`8922ccd7`).**
  When this dossier was written, every one of the (then fifteen) files
  defined its own `fn stdout_is_console() -> bool { true }` — identical
  stubs — so `auto` meant *always*, and the only people affected were those
  who wrote `--color=auto` to get pipe-safety and got colour anyway. The
  library had deliberately delegated the probe to the binary because
  answering it needs a syscall the pure modules cannot make; the callers
  then all wrote the same constant.

  The fix landed exactly where this caveat predicted it would: the blocking
  device-class syscall, once "reserved and never built", shipped as
  `SYS_FD_DEVCLASS` (the H-1 fd-class introspection). The console's own Dev
  class is `'c'`, disjoint from the pseudoterminal's, and the probe the
  shell already performed is now a library function,
  `libthyla_rs::stdout_is_terminal()`. Each `{ true }` stub became a
  one-line wrapper delegating to it; `ps` calls it directly. A pipe, a file,
  or a closed fd now resolves colour-off. The prediction's other half held
  too — the shared probe was built rather than a sixteenth stub added
  (Prosecution, task #156 closed).

- **`grep` still gates styling two different ways in one file, and one of
  them cannot be seen from the function that relies on it.** The H-1c-2
  rewrite kept the shape: the prefix path (`emit_prefix`) checks the `on`
  flag directly around each escape write, while the match path (`emit_line`)
  is gated by whether it was *handed* any spans — the caller computes spans
  only when `(colour || rich) && !invert`, and an empty span list yields the
  plain line. `emit_line` now serves both realizations off that one data
  gate: non-empty spans wrap in SGR bold-ember when colouring, or in an
  `em class=strong` frame at the Rich tier.

  That is sound today — verified — and it is a legitimate pattern: gate the
  data rather than the emission. But `emit_line`'s signature still cannot
  express the precondition, so a second caller that computed spans without
  consulting the flag would style a payload with nothing to stop it. In a
  crate whose top-level rule is about exactly that, it is the one place where
  the rule depends on a convention rather than on structure — and the Rich
  path now rides the same convention.

- **No unit tests.** These link the runtime unconditionally, so the host
  harness cannot build them — the structural reason is unchanged. The
  interactive scenarios exercise a handful on a live console each boot, and
  H-1c-2 added one witness that matters: `ls-halcyon.exp` asserts `ps`
  frames its output at the Rich tier while `ls` (there) does not
  ("ps framed, ls never did; the gate was innocent"), which pins the beacon
  side of the new probe. The colour flag matrices — in particular the
  `auto`-resolves-off-in-a-pipe guarantee that the whole H-1c-2 change turns
  on — the graft classification, and every network error path remain
  unpinned.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
