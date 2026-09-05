---
id: chg-2026-08-03-nora-engine-sweep
type: chg
title: "nora's engine — and a census corrected twice, the second time by the batch that praised the first"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-nora-engine
  - sub-kaua
  - sub-utopia-parser
  - moc-userspace-shell-tui
established:
  - sub-nora-engine
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-03
---
Batch 43: nora's editor engine — text, editor, wrap, and the crate front door.
4 files, 5831 lines. Main unchanged at `d669299c` (already an ancestor; no
merge). L-1 absent on the THIRTY-FIRST check.

**#117 SPLIT THREE WAYS, FROM THE CODE RATHER THAN THE LINE COUNT.** nora's 13
files draw their own line and every module header states it: a pure engine, a
pure renderer, and exactly one file that touches a terminal. So — engine (text,
editor, wrap, lib; what a keystroke does), view (view, syntax, theme, diag,
debug, vartree; what reaches the screen plus the protocol-free display models),
host (main, lsp_host, dap_host; the process, the poll loop, the two child
sessions, and the I-27 console-discipline claim). That is utopia's cadence —
19.9k across three — rather than one 11.5k batch, and it is dependency-ordered so
each note's wikilinks resolve when written. `lib.rs` sits with the engine as the
crate front door. This is the engine.

**F1 -- THE CENSUS WAS CORRECTED, AND THE CORRECTION IS ALSO WRONG (#105, second
correction).** Batch 41 re-measured the stranded-test claim, found batch 38 had
checked two crates and asserted six, and published: kaua 92 / parley 73 /
libdriver 86 PASS; libutopia 385 / nora 238 / tapestryd 4 STRANDED — "251 run
today; 627 stranded, not 878."

nora's 238 run. All of them, in 0.02s.

Measured across all six, both ways:

    bare invocation          kaua PASS  parley PASS  libdriver PASS
                             nora FAIL  libutopia FAIL  tapestryd FAIL
    with --lib               kaua PASS  parley PASS  libdriver PASS
                             nora PASS  libutopia FAIL  tapestryd n/a

    -> 489 run today; 389 stranded.

**The six verdicts batch 41 recorded match the bare invocation's six results
exactly**, which is what identifies the mechanism: it ran every crate, with one
command. The crates document *different* commands. nora declares a binary target
whose `main.rs` imports `kaua::term` and `kaua::source` — the backend layers — so a
bare `cargo test` tries to compile the binary and fails on the import, while the
library beneath it is fine. Its `Cargo.toml` says so in three lines, including
the parenthetical reason (`libthyla-rs is aarch64-only, so drop backend + scope
--lib`). The fix was written in the file being censused, in the same field batch
41 praised kaua for having.

So the shape is not carelessness, and naming it precisely matters because it will
recur: **running the probe on every subject is completeness along one axis and
blindness along another.** It felt like the fix for batch 38's error — check them
all, not two — and it was, for coverage of subjects. The probe itself was never
re-derived per subject. Batch 41 wrote "an update lands where the work is, not
where the claim is" about this exact family; the sibling is that a uniform probe
measures uniformity, not the property.

The two crates that genuinely cannot host-test fail for a real and different
reason, now established rather than assumed: `libutopia` and `tapestryd` both
depend on libthyla-rs **unconditionally** (no `optional`, no feature), so the
host build reaches its aarch64 `_start` assembly and dies on `.type _start,
%function`. libutopia is additionally unconditionally `#![no_std]`. Their 389
tests are stranded and the fix for them is the two-part refactor the other four
crates already demonstrate.

Corrected on the Present plane in [[sub-kaua]], [[sub-utopia-parser]] and the
area MOC — whose cross-cutting note had generalized "the `#[test]` blocks in
these crates do not compile" across the whole area, true now of two children out
of six, and the single most misleading sentence in the vault for anyone reading a
coverage claim here.

**F2 -- A DOC COMMENT MOVED TO THE WRONG FUNCTION BY INSERTION (task #126).**
`TextBuffer::find_all`'s rustdoc opens: "Find `pat` starting just after the
cursor, wrapping to the top. Returns the match-start position." That is `find`'s
contract. `find_all` returns *every* match, buffer-wide, and neither starts at
the cursor nor wraps. `find` itself has no doc comment at all.

The mechanism is visible in the layout: `find_all` was inserted between `find`'s
doc comment and `find`'s signature, so the comment silently re-parented. Same
family as the query.rs finding one batch ago (#118) and a different mechanism —
that one updated the prose it was reading and not the prose the reader receives;
this one moved correct prose onto the wrong symbol. Both end in rustdoc
publishing a false contract.

**F3 -- THE WRAP PASS IS WRITTEN TWICE AND THE SECOND COPY IS DEAD (task #127).**
`find`'s loop runs `0..=n` over n lines, so its final iteration revisits the
cursor's own line with the start offset at 0 — that *is* the wrap pass. Directly
below sits an explicit `if off == n` block doing the identical search over the
identical line, reachable only when the loop body just failed to find the same
thing. Harmless, and it is also the block a reader checks when verifying that
wrapping works, so the loop bound that actually does the work reads as
incidental. Tightening the bound to `0..n` later would leave the wrap silently
resting on the redundant copy.

**F4 -- THE DISARM LIVES AT THE CONSUMER, SO IT FIRES ONLY WHEN THE CONSUMER RUNS
(task #124).** A cross-file go-to-definition parks its target in `pending_jump`
and raises `Request::Open`; `open_buffer` `take`s it, annotated "so an ordinary
later `:e` can never inherit a stale jump". It delivers that on success and on
both soft failures — a missing file and a `NotFound` still open an empty buffer,
consuming the jump. The binary's hard read-error path reports the error and
`return`s *without* calling `open_buffer`, leaving the jump armed; the next
successful `:e`, of any file, moves the cursor to a line meant for a different
one. Four sites total for the field, and no disarm on the one path that skips the
consumer.

**F5 -- THE DEFENSE-IN-DEPTH WAS APPLIED TO THE THREE-LINE FUNCTION AND NOT THE
FIFTY-LINE ONE (task #125).** `insert()` has no read-only gate: Enter,
Backspace, Delete, Tab and the catch-all `Char(c)` all mutate unconditionally.
The property holds today — all seven assignments of `Mode::Insert` are gated
(`i`/`a`/`o`/`A` behind `if editable`, and `change()` from Visual) — and the set
is enumerable, which is what makes "correct today" safe to write. What makes it
worth recording is that `change()` re-checks `readonly` at its top *even though*
its only caller has just checked it. The author's instinct was already
belt-and-braces; it reached the small function and not the whole typing path.

**THE COUNTERWEIGHTS ARE THE STRONGEST OF THE ARC SO FAR, AND ONE IS A TEST
DESIGNED AGAINST A FUTURE AUTHOR.** `rev_bumps_on_every_content_mutation_only`
enumerates all eight mutators, each from a *fresh* buffer so one cannot mask
another, asserts a battery of cursor moves does not bump, and handles the
asymmetric case (a no-op undo must not bump; a real one must). Its comment names
its own purpose: "a mutator added later without a bump silently costs the LSP
client a document sync (fail-soft, but wrong), and this is the thing that catches
it." A test written for the person who has not arrived yet.

Three more. The byte/char conversion tests round-trip every valid column on a
multi-byte string and open by naming why the bug hides — "ASCII: the two
coordinates coincide, which is exactly why a byte-vs-char mixup survives every
ASCII test and breaks on the first accented character." The three async request
axes are separated with the reason stated at the type, and the reason is a
soundness one: a server that never answers can never wedge a save. And
`load_active` clears each per-position transient with a per-field justification
rather than one blanket comment — diagnostics because they would paint another
file's errors on these lines, hover and completion because they describe a
position in the file just left.

LEDGER, read off the rendered view. Corpus 840 -> **842**. Coverage 244 ->
**248 owned of 421**, 57% -> **58%**; unswept lines 59217 -> **53386** (-9.9%).
`usr/nora` 0/13 -> **4/9**, 5660 lines remaining across the view and host
batches.

The unswept delta is exactly 5831 — the four files' line count, to the line. Not
a check I set out to make; it fell out of reading the number instead of
predicting it, which is the same rule that caught last batch's 58-vs-57.
