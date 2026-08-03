---
id: moc-userspace-shell-tui
type: moc
title: "The shell and TUI stack — text in, effects out, and a screen in between"
parent: moc-userspace
created: 2026-08-03
updated: 2026-08-03
---
What a person actually touches: `ut`, the rc-shaped shell — its parser, its
evaluator, and the line editor in front of both — plus the console-TUI
substrate and the editor built on it. Orientation only; the facts live in the
`sub-*` dossiers.

## The organizing fact

**This area spans the widest range of risk in the tree, and the range is the
thing to keep in view.**

At one end is a parser that touches nothing outside its own arguments — no
syscall, no filesystem, no capability. At the other is the raw-mode handoff, in
which the shell puts the console into a state the user cannot type their way
out of, hands it to a child, and must restore it whatever happens to that
child. Between them sits an evaluator that turns text into spawns, pipes and
redirections.

So the dossiers here carry different `audit` levels for a real reason rather
than a filing convention, and reading them together is how the range stays
visible. In particular, two failure modes are worth not conflating:

- **A pure layer can still kill the shell.** The parser's one genuine hazard is
  stack depth: a `no_std` program has no guard page of its own, so a deep
  enough input faults, and a shell that dies takes the session with it. That is
  a liveness property with no invariant number, defended by hand-written
  counters.
- **A layer that is not a privilege boundary can still hold the console
  hostage.** Restoring cooked mode is not a capability question — nothing stops
  the shell from getting it wrong — and a native binary is `panic = abort`, so
  a crashed child's own cleanup never runs. The shell is the authoritative
  restorer of a console it does not own.

## Children

- [[sub-utopia-parser]] — text to AST: the rc-shaped grammar, the eight
  lexical surfaces, and the three separate recursion bounds that exist because
  the recursion has three shapes no single counter sees.

## Cross-cutting

- Everything here is native, so it stands on [[sub-libthyla-rs]] — the same
  ownership, error and allocation discipline, and the same fixed heap.
- The shell is also the thing that *starts* other programs, so its evaluator is
  the busiest consumer of the process and namespace surfaces the kernel plane
  describes.
- **The test story for this whole area is unusual and worth knowing before
  reading any coverage claim.** The `#[test]` blocks in these crates do not
  compile — the workspace pins a bare-metal target with no test harness — while
  a separate in-guest binary re-covers much of the same ground by driving the
  public entry points on every boot. So a dossier here that says a surface is
  tested means the second thing, not the first.
