---
id: sub-coreutils-presenters
type: sub
title: "The presenters — fifteen tools, and fifteen copies of one stub"
parent: moc-userspace-tools
code:
  - usr/coreutils/src/bin/ls.rs
  - usr/coreutils/src/bin/pelt.rs
  - usr/coreutils/src/bin/ns.rs
  - usr/coreutils/src/bin/qid.rs
  - usr/coreutils/src/bin/realm.rs
  - usr/coreutils/src/bin/stat.rs
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
updated: 2026-08-04
---
## Purpose

The fifteen coreutils binaries that link the colour modules: the namespace
introspection tools that make Thylacine's own structure visible, and the
network clients that frame a result.

Two groups by subject, one group by consequence. What unites them is that
their output is meant for a person's eyes, which is what earns them colour
under the crate's rule — and what makes the terminal question, which none
of them answers, matter.

## Contract

Same shape as the filters: one binary per file, own argument loop, own
exit status. The difference is the colour flag. Each accepts
`--color[=WHEN]`, resolves it once at startup, and threads the resulting
boolean through every formatting call.

The introspection tools default to colour ON — the listing *is* the
product, and a box with a realm column is the point. `grep` defaults to
OFF, matching the convention, because its output is routinely piped.

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

## Performance

Listing cost is dominated by the per-entry stat. The box-fitting pass
walks the rows twice — once to measure, once to draw — which is free at
directory scale.

## Prosecution

- **The colour gate must be resolved once and threaded.** Fifteen tools
  resolve it at startup and pass a boolean down; a tool that re-derived it
  mid-run could emit a half-coloured line.
- **A new presenter must take the shared probe, not write its own.** The
  count of hand-written probes is currently fifteen and should never reach
  sixteen (task #156).
- **`grep`'s default must stay off.** It is the one tool in this group
  whose output is ordinarily a payload; the whole reason it can live here
  safely is that colour is opt-in.

## Seams

There is no name service for user identities, so an owner column shows a
number for anyone but the system principal.

The graft classification cannot distinguish a mount from any other
stat failure (above). Consulting the mount list would fix it and would
cost a read per listing.

## Caveats

- **`--color=auto` does not mean auto. It means always, in all fifteen.**
  Every one of these files defines its own
  `fn stdout_is_console() -> bool { true }` — fifteen identical stubs. The
  library deliberately delegated the probe to the binary, because
  answering it needs a syscall and the pure modules have none; all fifteen
  callers then wrote the same constant.

  The blocking reason both `ls` and `grep` cite is a device-class syscall
  that is reserved and was never built. But the mechanism shipped under a
  different name: the console gained a stat contract with its own
  identifying bit, deliberately disjoint from the pseudoterminal's, and
  **the shell already performs exactly this probe** — stat the descriptor,
  check the character-device mode, test the bit. Thirty lines away, in a
  crate these tools already resemble.

  The consequence lands precisely on the users who asked for it. Defaults
  are unaffected — the presenters colour anyway, `grep` does not — so the
  only people affected are those who explicitly wrote `--color=auto` to
  get pipe-safety, and they get colour (task #156).

- **`grep` gates colour two different ways in one file, and one of them
  cannot be seen from the function that relies on it.** The
  match-only path checks the flag around each escape write. The
  whole-line path calls an emitter that writes escapes *unconditionally*
  and is kept correct by the caller passing an empty span list when
  colour is off.

  That is sound today — verified — and it is a legitimate pattern: gate
  the data rather than the emission. But the emitter's signature cannot
  express the precondition, so a second caller that computed spans without
  consulting the flag would colour a payload with nothing to stop it. In a
  crate whose top-level rule is about exactly that, it is the one place
  where the rule depends on a convention rather than on structure.

- **No tests.** These link the runtime unconditionally, so the host
  harness cannot build them. The interactive scenarios exercise a handful
  on a live console each boot; the flag matrices, the graft
  classification, and every network error path are unpinned.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
