---
id: chg-2026-08-03-nora-view-sweep
type: chg
title: "nora's renderer — and a sanitizer applied to the input that needed it least"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-nora-view
  - moc-userspace-shell-tui
established:
  - sub-nora-view
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-03
---
Batch 44: nora's renderer and its five display models — view, syntax, theme,
diag, debug, vartree. 6 files, 3374 lines. Main unchanged at `d669299c`
(already an ancestor; no merge). L-1 absent on the THIRTY-SECOND check.

**THE TEST CLAIM WAS VERIFIED RATHER THAN CARRIED.** The handoff said 83 tests
run here, split 48/19/0/7/3/6. Counted: 83, split exactly that way, and nora's
238 pass on the host in 0.01s. Last batch the handoff's own figure was wrong on
this exact axis, so the check was cheap insurance and it came back clean — which
is the outcome that makes the habit worth keeping, not the one that makes it feel
justified.

**F1 -- THE SANITIZER IS APPLIED TO THE ONE INPUT THAT DID NOT NEED IT (task
#130).** `view.rs` has two drawing helpers, and both map every control character
to a space. They are used for the buffer text, the command line, and the debug
status line — the user's own content.

The twelve kaua-widget call sites do not sanitize. Those are: hover, completion,
the console scrollback, the variables tree, the call stack, the goroutine list,
and the status line's centre slot. Traced end to end: kaua's eleven widget
`render` impls contain **zero** `is_control` checks, they write through a buffer
primitive that passes characters unchanged, and the terminal encoder emits each
cell's character as raw UTF-8. The encoder emits a cursor move only when the pen
mismatches and a style code only when the style changes — so a run of adjacent
same-styled cells emits its characters **back-to-back with nothing between
them**, and an escape sequence written across consecutive cells arrives at the
terminal intact.

The reachable producers are, easiest first: a debuggee's own stdout, which lands
in the Console tile (a Go program under `:debug` printing a clear-screen
sequence); a debuggee variable's value, formatted into the Variables tree; and a
language server's hover text, diagnostic message and completion labels.

Consequence is display integrity, not privilege — nora runs as the user and
holds nothing they do not. What makes it worth filing is the direction of the
gap. The mitigation exists, in this file, on the path carrying the user's own
text, where a control character is the user's own data. It is absent on every
path carrying text from another program. The sibling dossier had already named
this area as "the one whose untrusted input arrives from another program rather
than from a keyboard"; the defense went to the keyboard.

**F2 -- THE HEADER NAMES THE WRONG COORDINATE SIX LINES BEFORE WARNING ABOUT
EXACTLY THAT MISTAKE (task #131).** `diag.rs` opens by saying the engine knows
"byte columns C..E", and describes the binary as converting the server's
character offsets to byte offsets. Eleven lines later the same header says
columns are CHARACTER columns, that a producer holding byte offsets must
convert, and that getting it wrong "is invisible on ASCII and lands the cursor
off by (bytes - chars) on the first non-ASCII line, which is why the coordinate
is named here rather than left to each caller."

Ground truth is the second paragraph: the field docs say character column, and
the real conversion is two steps ending in characters
(`byte_to_char_col(src, char_to_byte(src, ..., enc))`). The opening sentence
names the intermediate step as the destination — the pre-fix convention, left
standing when the warning was added beneath it.

A producer who reads the first paragraph and stops builds precisely the bug the
second paragraph exists to prevent, and it will pass every ASCII test they write.

**F3 -- "NEVER A WRONG ROW" IS FALSE FOR A HASH WITH NO INDEX (task #132).**
The kernel-backtrace parser documents that a line without the `#<i>` shape is
skipped, "never a wrong row". Stripping leading digits from a digit-less
remainder is a no-op, so `#foo bar` yields a frame named `foo bar`. Unreachable
today — the sole producer is the kernel's own backtrace file with a fixed format
— and the three tests cover the hash-with-no-symbol case but not the
hash-with-no-index one. Filed because the claim is explicit, a wrong row in a
call stack is a user-visible lie, and the fix is one condition.

**F4 -- THE TRACKED PALETTE RESIDUE IS 4.5x LARGER THAN TRACKED, AND THE CODE
WAS ALREADY FIXED (task #113, corrected).** The handoff sent me to check
`theme.rs` against the Bonfire rename. It is clean: Bonfire values throughout,
and one mention of the retired name in a header note recording the correction
and citing the exact hexes the visual scripture flags as residue. That is the
model for what a corrected file should say.

The surviving residue is entirely in documentation, and measured at **55
occurrences across 13 files** against the "twelve across seven" the tracking
item claimed. One of the 55 is legitimate (the supersession record); the rest
sampled are live descriptions, including a boot checklist still quoting the
retired hex values as what the prompt renders.

The probe behind the original twelve was never stated. That is the *third*
count in this arc found wrong for the same reason — 878, then 627, now twelve —
and the pattern is now specific enough to name: **a published count with an
unstated probe is a claim about the probe, not about the tree.** Corrected on the
task rather than in a dossier, since the residue is not in this batch's code.

**THE COUNTERWEIGHTS ARE MOSTLY ABOUT CONSEQUENCE RATHER THAN BEHAVIOR.** Four
comments in this batch explain what would go wrong instead of what the code
does. The completion popup windows the candidate list before formatting any row,
and says what the other order silently costs — the highlight drops off the end
once the selection passes the last formatted row. The gutter recolors the line
number for a diagnostic rather than adding a marker column, and states the
cross-module reason: the gutter width is shared with the wrapped renderer, so a
width change would reflow every visual row. The diagnostic counter excludes
hints because "the counter exists to answer 'is my code broken', and folding
hints in would keep it permanently lit." And the geometry exports carry their
consequence — a width mismatch desyncs the wrapped cursor — at all three sites
rather than at one.

Two more. The Call Stack separates a *frame* index from a *row* index so
navigation can never land on the visual-only kernel divider, with a test named
for exactly that. And the one place a model function carries a false
precondition — a by-reference lookup whose documentation claims references are
unique, which fails for the zero every leaf carries — is guarded at its single
caller by a comment naming the exact collision. The guard is in the right place
and says why; recorded because it and the claim live in different files.

**THE GEOMETRY QUESTION THE HANDOFF FLAGGED RESOLVES CLEAN.** The renderer and
the engine's scroller share one wrap module — no private copy on either side —
and the width they wrap at comes from one exported function whose two internal
steps are themselves shared with the renderer through a common helper. The
binary closes the loop each frame in the right order. Three mechanisms, all
commented with the consequence rather than the mechanism.

**ONE DEPENDENCY FACT, TWO VISIBLE COSTS.** The shell keyword list is copied
into the highlighter rather than imported, and the test that guards it pins this
copy against a literal in the same file — so the parser gaining a keyword fires
nothing. Verified in sync today (sixteen words, identical sets). What makes it
the right call is that the cross-crate test is *impossible* for the same reason
the copy exists: the parser's crate depends on the aarch64-only runtime
unconditionally, which is exactly what strands its own 385 tests (F1 of last
batch). The header says all of this and uses the accurate verb — the test
*fixes* the set, it does not *check* it — and names the refactor that would make
the match a compile-time guarantee.

LEDGER, read off the rendered view. Corpus 842 -> **844**. Coverage 248 ->
**254 owned of 421**, 58% -> **60%**; unswept lines 53386 -> **50012** (-6.3%).
`usr/nora` 4/9 -> **10/3**, 2286 lines remaining in the process half.

The unswept delta is exactly 3374 — the six files' line count, to the line.

A process note against myself, since this batch spent its length on unstated
probes. These figures were **written before the render and verified after**,
which is backwards: the standing rule is to read them off the rendered view
precisely because a prediction that happens to be right is indistinguishable
from one that is wrong until you check. They matched — corpus 844, 254 of 421 at
60%, 50012 unswept, nora 10/3 — so nothing here is inaccurate. The order was.
Recorded because "I guessed right this time" is the reasoning that erodes the
rule, and the same batch found three counts published without their probe.
