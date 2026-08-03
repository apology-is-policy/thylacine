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
- [[sub-utopia-eval]] — AST to effects: the three-way command resolution, the
  two foreground wait paths (console forwarding vs pts process groups), and the
  single recursion counter that answers the parser's question the other way,
  because a shell's recursion shapes compose rather than stay separate.
- [[sub-utopia-interactive]] — bytes to a line and back to a screen: the editor
  state machine, the fd-agnostic REPL, and the startup order in which a shell
  must become self-managing *before* it becomes a signal target.
- [[sub-kaua]] — the substrate a full-screen app paints on: a cell diff, a total
  VT parser, and a capability story preserved by omission (fd 0 and fd 1, never
  the line discipline). The other half of the raw-mode handoff `ut` performs.
- [[sub-parley]] — the editor's dialogue with its language and debug servers: a
  JSON codec, Content-Length framing, two protocol grammars and two pure
  clients, with only the transport touching a process. The one area here whose
  untrusted input arrives from another program rather than from a keyboard.
- [[sub-nora-engine]] — what a keystroke does: the char-addressed text buffer,
  the modal state machine, and the soft-wrap arithmetic the scroller and the
  renderer share. The layer that can neither act nor fail — every effect is
  raised as a request for someone else to perform, and every out-of-range input
  clamps rather than erroring.

## Cross-cutting

- Everything here is native, so it stands on [[sub-libthyla-rs]] — the same
  ownership, error and allocation discipline, and the same fixed heap.
- The shell is also the thing that *starts* other programs, so its evaluator is
  the busiest consumer of the process and namespace surfaces the kernel plane
  describes.
- **The test story here is split down the middle, and knowing which half a
  dossier is in matters before reading any coverage claim.** The workspace pins
  a bare-metal target with no test harness, so a crate's `#[test]` blocks run on
  the host only if the crate gates `no_std` on `cfg(test)` and makes libthyla-rs
  optional. kaua, parley and nora do — their suites run, and a coverage claim in
  those dossiers means assertions that execute. The `libutopia` crate does not
  (it depends on libthyla-rs unconditionally), so the parser and evaluator
  dossiers describe a *dead* suite, and their real coverage is a separate
  in-guest binary driving the public entry points on every boot. Two figures
  have been published for this and both were wrong; the measured split is 489
  running against 389 stranded.
