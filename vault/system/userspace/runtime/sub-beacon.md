---
id: sub-beacon
type: sub
title: "beacon -- the semantic output markup, host-tested at every tier"
parent: moc-userspace-runtime
code:
  - usr/lib/beacon/src/lib.rs
  - usr/lib/beacon/src/wire.rs
  - usr/lib/beacon/src/sink.rs
  - usr/lib/beacon/src/verbs.rs
  - usr/lib/beacon/src/boxd.rs
  - usr/lib/beacon/src/color.rs
  - usr/lib/beacon/src/palette.rs
  - usr/lib/beacon/verbs.default
  - usr/lib/beacon/Cargo.toml
audit: light
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/BEACON.md"]
created: 2026-09-05
updated: 2026-09-05
---
## Purpose

Programs describe their output once -- text runs, emphasis by class, typed
objects, tables, the shell's transcript zones -- and this crate realizes that
description at whatever the sink can render: `rich` becomes OSC 1936 frames
wrapping the plain payload, `cells` becomes the Bonfire box+SGR visual
language the coreutils already spoke, `none` is plain bytes. The H-1 crate,
binding-specified by docs/BEACON.md.

It is the semantic-output layer for the whole tree, not a coreutils annex.
The consumers are the coreutils (51 bins), the shell, and halcyond's
renderer + context menu; the crate began by absorbing the coreutils colour
language verbatim and grew the frame + verb layers above it. That breadth is
why it lives beside [[sub-libtapestry]] in runtime rather than in the tools
cluster with [[sub-coreutils-lib]].

The one discipline underneath all of it: the plain payload is *always* in the
stream, and presentation is only ever additive. That is what lets a program
emit rich output to a terminal and clean bytes down a pipe from the same call
site, and it is enforced structurally rather than by remembering to.

## Contract

`Tier` (None/Cells/Rich) is what a renderer advertises, transported renderer
-> consctl -> the shell's `BEACON` env export -> children. `BeaconMode`
(Auto/Always/Never) is the per-tool `--beacon=WHEN` override mirroring
`--color`. `effective_tier(env_tier, dc_of_stdout, flag)` is the two-condition
emission gate and the crate's front door -- a pure function, so the caller
supplies the syscall-derived fd class and the crate needs no libthyla-rs
dependency.

`Sink` is the emit API: plain runs, `em(class)` emphasis, `obj(type, ref)`
typed presentations, `zone_open`/`zone_close` for the shell's transcript
zones, and `Table`/`Cell` for listings. `wire::{open, close, point, parse,
strip}` is the raw frame grammar. `verbs::{parse, rules_for, quote, expand}`
is the presentation-verb rules engine. `boxd`/`color`/`palette` are the cells
tier, used directly by the bins.

## Mechanism

**The strip property is the crate's soul (BEACON.md 12.8 P1).** Annotated
text is ordinary stream bytes *between* frames -- never inside them -- so
`wire::strip()` on a rich stream yields byte-exactly the `none`-tier
emission. This is not a nicety: it is why `richtool | plaintool` is safe, and
it is the property the whole design is arranged to preserve.

**The emission gate fails closed.** `effective_tier` emits above None only
when the flag permits AND (for Auto) the stdout Dev class is an interactive
terminal AND the advertised tier renders. "Interactive terminal" is the console
(`DC_CONSOLE`, `'c'`) OR a pts slave (`DC_PTS`, `'t'` -- H-4d): a program
writing into a session tile through a pseudoterminal is as much on a terminal as
one on `/dev/cons`, so the Auto arm admits both classes. A failed
`SYS_FD_DEVCLASS` probe reads as not-a-terminal (the caller passes `None`), never
as one, so a closed or pre-H-1 fd gets plain bytes -- the safe direction. An explicit `Always`
trusts the advertisement over the fd, but an *absent* advertisement still
yields None: there is no renderer to read frames, so emitting them would be
pure corruption.

**The wire parser is structural, not semantic.** It recognizes the `ESC ]
1936 ; v1 ; op ... ST` framing (ST is `ESC \`, and BEL is accepted per VT
convention), enforces the byte caps (frame 2048, value 1024, 8 args) and the
nesting-depth cap (8; the depth cap is parser-enforced per 12.1 rule 3, audit
H-1 F3), decodes args, and passes *everything else* through as payload --
foreign OSC (aurora's 7770 config channel), SGR, arbitrary escapes are all
just text to this layer. A paired op opened past depth 8 is discarded as
malformed together with its matching close and any point op inside the
suppressed region, but the payload bytes between them still flow. Which op may
legally contain which is the tree-building consumer's concern, not the
parser's.

**The Sink enforces P1 by construction.** Every method writes the plain
payload at every tier and adds frames only at Rich, so any program built on
the Sink inherits the strip property for free. Two deviations from the 12.5
sketch are deliberate and recorded in the source: zones are explicit
open/close calls rather than a lexical scope guard (the shell's prompt zone
opens in `draw_prompt` and closes in a different call, the accept arm); and
`em`/`obj` at the Cells tier are payload-only, because the cells look is the
bins' existing box+SGR language and object *identity* is a Rich-only concept
-- SGR never appears inside Rich beacon-structured output, where the
renderer's stylesheet owns typography.

**verbs is the one rules engine three surfaces share** (the transcript's
context menu, the tag bar, acme-style selection execution): "text + type ->
verb", a plumber-style rules file of `type label command-template` lines.
Two security properties are load-bearing:

- **Anti-clickjack quoting.** `{}` in a template is replaced by the *resolved*
  ref, single-quoted the way ut's lexer reads it (rc's rule: `''` is the one
  escape), so the command the user picked acts on exactly the ref the menu
  displayed -- not a re-parsed or substituted one.
- **The #880 internal-strip.** A template beginning `#` is an internal action
  a renderer interprets itself (a test lever), admitted only when the caller
  passes `allow_internal`; a production build drops such rules on sight, so a
  session-supplied rules file can never smuggle an internal action into a
  shell context.

The parse is bounded throughout (type 16, label 32, template 256, 256 rules)
because the rules file may one day arrive from a session tier.

The shipped `verbs.default` gained the H-4c **layout** verbs —
`restore`/`save`/`delete` on an `obj type=layout` whose ref is the saved
layout's name (`halcyon layout {}`) — so `halcyon layout list` presents each
saved layout as a menu-actionable object with **no renderer code**: `layout` is
a new value of the existing `type` key (BEACON.md 12.2), handled by string in
halcyond, not a new frame op.

**The cells tier is the coreutils colour language, relocated verbatim**
(2026-09-01, BEACON.md 12.5): `boxd` (box furniture computed on plain text,
coloured at emit), `color` (the `col(code, on)` single-path gate), `palette`
(the Bonfire ANSI map). The colour discipline is inherited wholesale from
COREUTILS-THYLACINE-DESIGN.md -- presentation and diagnostics may be styled;
a data payload a pipe consumes stays byte-clean at *every* tier.

## Data structures

`Tier` and `BeaconMode` are the two config enums. `wire` carries `Op` (the
frame opcodes), `Arg` (a decoded key=value), and `Event` (the parse output:
open/close/point/payload). `sink` carries `Zone` (Prompt/Command/Output,
shell-only), `Em` (Emph/Strong/Dim/Code -- emphasis by class, never by face),
`ObjType` (Path/Pid/Url/Commit/User/Layout -- a presentation canonically named
by its ref: a cleaned absolute 9P path for Path, a saved layout's NAME for
Layout), the `Sink<'a>` emitter over a `dyn Out` byte shim, and `Cell`/`Table`
for listings. `verbs` carries `Rule`.
The cells modules carry the palette + card-row types relocated from coreutils.

## Concurrency

None. A pure `no_std` + `alloc` library with zero dependencies; every consumer
drives it single-threaded and owns its own `Sink`.

## Invariants enforced

None of the numbered system invariants -- no syscall, no capability, no
handle. Its own load-bearing rules:

- **P1 (the strip property)**: `strip(rich) == none`. Pipe-safety depends on
  it and the Sink guarantees it by construction.
- **The gate fails closed**: no frames onto a pipe, a non-console fd, or a
  failed probe.
- **The colour/payload separation**, inherited from the coreutils discipline
  ([[sub-coreutils-lib]]): styling on presentation + diagnostics, never on a
  consumed payload, at any tier.

**Audit-trigger participation:** `verbs.rs` is a component of the H-3c "obj
verb menu" audit-trigger surface (docs/AUDIT-TRIGGERS.md), whose hard gate --
the menu grab, the compositor-owned dismiss, the click-to-focus authority --
lives in [[sub-tapestryd]]. beacon's contribution to that surface is the
rules engine's two security properties above (anti-clickjack quoting, the
#880 internal-strip); the crate itself carries no capability, so it is
classified `light` and the gate is prosecuted where the authority is.

## Error paths

Everything degrades toward plain bytes, which is the safe direction for an
output layer:

- An over-cap or over-depth frame is discarded as malformed; its payload
  still flows (frames are never payload).
- `wire::parse` on a truncated or foreign stream yields payload events, never
  a fault.
- `verbs::quote` returns `None` on a ref it cannot safely single-quote, so
  `expand` refuses rather than emit an unsafe command line.
- `Tier::parse` / `BeaconMode::parse_when` return `None` on an unknown word;
  an absent `BEACON` value anywhere along the transport chain is None.
- A failed fd-class probe is `None` -> `effective_tier` returns None -> plain.

## Performance

Irrelevant at this layer; pure functions over output buffers with no
allocation beyond the emitted bytes. The box-fitting pass in the cells tier
measures a listing twice (widest-row then draw), which is free for directory
listings -- the same characteristic documented for [[sub-coreutils-lib]].

## Prosecution

- **`strip()` must remain the exact inverse of the framing.** A single byte
  emitted *inside* a frame rather than between frames breaks P1 and corrupts
  every downstream pipe -- the property is tested in `wire` and must stay so.
- **The gate must fail closed.** A failed probe or an absent advertisement
  must yield None; the load-bearing pair is that a pipe never gets frames
  whatever the env says.
- **The wire byte + depth caps must hold.** The depth cap is parser-enforced
  (H-1 F3); an unbounded nest is a DoS + stack hazard for the consumer's
  tree builder, which trusts the parser to have bounded it.
- **`verbs::quote` must single-quote the resolved ref, rc-style.** The
  command must act on exactly the ref the menu displayed; anything else is
  the clickjack the quoting exists to prevent.
- **`#`-internal rules must be dropped unless `allow_internal`.** A production
  renderer executing an internal test lever as a shell command is the #880
  class.
- **No styling on a consumed payload, at any tier.** The cells tier's colour
  gate is a single formatting path precisely so a "clean output" mode cannot
  rot into a second, drifting branch.

## Seams

- Object identity (`em`/`obj`) is a Rich concept; the Cells tier renders those
  runs as plain payload with the bins' box+SGR look, no per-object SGR.
- Nesting *legality* (which op may contain which) is deferred to the
  tree-building consumer; the parser bounds depth but does not police
  structure.
- The 256-colour / 16-colour terminal degrade is unbuilt -- the palette emits
  truecolour only, inherited from the coreutils cells tier.

## Caveats

- **Host-tested at every layer** (`cargo test -p beacon --target
  aarch64-apple-darwin`): the width math, the SGR wrapping, the frame grammar
  (including the P1 strip round-trip and the depth-cap discard), and the
  emission gate. The gate's syscall-derived input -- the fd's Dev class -- is
  passed in by the caller, and the libthyla-rs wrappers that produce it
  (`t_fd_devclass` / `stdout_is_terminal`) are what is *not* host-testable and
  live outside this crate; that split is exactly what keeps beacon testable.

- **`boxd`/`color`/`palette` were relocated verbatim from the coreutils crate**
  (2026-09-01), so their behaviour and their caveats are the ones already
  documented for [[sub-coreutils-lib]] and [[sub-coreutils-presenters]] --
  the single-path colour gate, the box-width-on-plain-text rule, and the
  terminal-probe delegation the callers each supplied. Nothing about that
  discipline changed in the move; only its home did.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
