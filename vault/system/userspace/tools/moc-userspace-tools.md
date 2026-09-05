---
id: moc-userspace-tools
type: moc
title: "The leaf commands — what a person runs, and what it holds afterwards"
parent: moc-userspace
created: 2026-08-04
updated: 2026-08-04
---
The programs a person invokes that finish: the fifty-one coreutils and
their shared library, the process monitor, the standalone network clients,
and the two single-file programs that exist to drive a kernel mechanism.
Orientation only; the facts live in the `sub-*` dossiers.

## The organizing fact

**They are leaves. Each does one bounded job and holds nothing
afterwards** — no session, no console role, no device, no capability, no
peer that outlives the process.

That is what separates them from the shell and editor stack, which also
face the user. Those programs *mediate*: the shell stands between a person
and everything else, the editor between a person and a file, the renderer
between a person and the console. The programs here consume the interfaces
every other area builds and mediate nothing. Their invariant sections say
"composes with" throughout and "enforces" nowhere — which is a real
finding about the tree's shape, not an absence of content: the least
privileged code in the system is also the code a person touches most.

The consequence for reading them is that the interesting questions are not
about soundness. A defect here corrupts the invoking user's own files at
the invoking user's own authority, which for `rm` is exactly right. The
interesting question is **which disciplines propagated across fifty-odd
independent implementations of the same shape**, because this is by far
the tree's largest sample of one problem solved many times, and it answers
that question both ways at once:

- **The colour discipline propagated perfectly.** The rule is that colour
  belongs on presentation and diagnostics, never on a payload another
  program reads. Measured across all fifty-one binaries: exactly the
  fifteen presenters link the colour modules and exactly zero of the
  thirty-six filters do. The partition is *exact*, and it holds because it
  is structural — a program that does not name the module cannot emit an
  escape byte. This is the "authority by absence" pattern the runtime
  libraries use for capabilities, applied to output cleanliness, and it is
  stronger than a check: a gate can be forgotten at one call site, a
  missing import cannot.

- **The terminal probe did not.** The same library that carries the colour
  rule delegates "is stdout a terminal" to each binary, because answering
  it needs a syscall the pure modules do not have. All fifteen callers
  wrote the same stub returning true. So `--color=auto` — the one flag
  whose entire purpose is to enforce the rule above — means "always", in
  every tool, and the fix now costs fifteen edits (task #156).

The pair is the lesson: the same library, the same authors, one rule
enforced by construction and one delegated by convention, with exactly the
outcomes those two choices predict.

## Children

- [[sub-coreutils-lib]] — the nine shared modules: the palette and its
  gate, box furniture, size formatting, metadata presentation, the card
  renderer, help plumbing, and the network pumps. Where the colour rule is
  stated and where the terminal probe was delegated away.
- [[sub-coreutils-filters]] — the thirty-six payload tools, kept
  byte-clean by not linking the module. Also where the command-search
  mirror drifted.
- [[sub-coreutils-presenters]] — the fifteen colour-linked tools: the
  namespace introspection commands that make Thylacine's own structure
  visible, and the network clients that frame a result. Carries the
  fifteen-stub finding.
- [[sub-prowl]] — the process monitor: a full-screen console application
  over the kernel's live telemetry. The clearest case in the tree of a
  program that is right about everything it can see and cannot see what it
  is not told.
- [[sub-net-clients]] — the five standalone network crates, each with a
  deterministic boot self-test that gates it and a live path that
  deliberately does not.
- [[sub-mechanism-drivers]] — the session host and the ring stress
  program: two one-file programs whose execution *is* the assertion.

## Cross-cutting

- **Four answers to "how is this proved", in one area.** The pure library
  half has fifteen tests that run. The network crates have deterministic
  boot self-tests. The two mechanism drivers are proved by running for
  real under the multi-processor matrix. Everything else — fifty-one
  binaries and the entire process monitor — has nothing that executes
  unattended beyond a handful of console scenarios. The sweep has been
  finding this divergence crate by crate since batch 51; this area shows
  all of it at once.

- **The wall is one line of crate configuration, and it is the same wall
  every time.** A binary that links the runtime unconditionally cannot be
  built for the host, so its tests cannot compile. Crates that gate the
  bare-metal attribute on not-being-under-test have running suites; crates
  that do not, do not. [[sub-prowl]] is the sharpest case: its sampling
  layer is deliberately pure — terminal-free, clock-free, elapsed time
  passed in — which is every structural choice a testable module makes,
  and its manifest then states plainly that nothing runs.

- **Three mirrors of one list, and two of them dropped the same entry.**
  The shell resolves a bare command against three directories; the
  environment variable that `which` reads is seeded with two, and the
  shell's own completion index also omits the third. Each copy documents
  itself as mirroring the others, which is what made the drift invisible —
  everyone who read any copy was told it matched (task #159, with #110).
  Same family as the argument bounds duplicated in seven places.

- **A kernel-side limit that no client can see.** The process table
  renderer stops when its buffer fills and discards the flag that records
  it, so [[sub-prowl]] presents a partial list as a complete one and has
  no way to know better (task #158). The only area finding whose fix is
  not in this area.

- Everything here is native, so it stands on [[sub-libthyla-rs]] — the
  same ownership, error and allocation discipline, and the same fixed
  heap. The network programs additionally reach the network only through
  the tree the network daemon serves, so they touch no hardware.

- The shell that launches most of them is [[moc-userspace-shell-tui]];
  read the two together for the mediator/leaf contrast the organizing fact
  above rests on.
