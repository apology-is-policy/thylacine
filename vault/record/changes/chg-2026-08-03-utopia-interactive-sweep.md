---
id: chg-2026-08-03-utopia-interactive-sweep
type: chg
title: "ut's interactive layer — a header denying what its own caller implements, and utopia closes"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-utopia-interactive
  - moc-userspace-shell-tui
established:
  - sub-utopia-interactive
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-03
---
Batch 40: the `ut` shell's interactive layer — line_editor, repl, the `ut`
binary, completion, palette, ansi, path, lib. 8 files, 5027 lines. **This closes
utopia** (#93: parser -> evaluator -> interactive; 26 files, 19864 lines, three
batches). Main advanced to `9b994f2d` (#131/#132, the Burrow charge-record);
merged clean before starting — the first sync this arc has needed in several
batches. L-1 absent on the TWENTY-EIGHTH check.

**THE SUBJECT IS WHERE A PERSON'S FINGERS MEET THE SYSTEM.** Three tiers,
deliberately separable: a line editor that is a pure state machine (bytes in,
`EditorAction` out, no syscall anywhere), an fd-agnostic REPL that writes its
rendering to an injected sink, and a thin `ut` binary that owns the actual
descriptors. `audit: light` — the kernel gates everything it calls — but the layer
owns the console's MODE, and that is not a capability question: nothing stops the
shell getting it wrong, and `panic = abort` means a crashed child's own cleanup
never runs, so the shell is the authoritative restorer of a console it does not
own.

**F1 -- THE MODULE HEADER DENIES A MECHANISM ITS ONLY CALLER IMPLEMENTS, NAMES,
AND CREDITS BY CHUNK (#109).** `repl.rs`'s scope-boundary section says:

> At v1.0 `/dev/cons` is a blocking-read-only Dev with NO `.poll` hook (cons.c),
> so the `ut` loop blocks in read(); the single-fd poll() there is the U-7 seam,
> not a load-bearing wait.

and reserves "the MULTI-FD poll() across the per-child + per-shell notes fds" for
U-7. Three claims, all false. `devcons`'s Dev vtable carries `.poll =
devcons_poll` (LS-8a, with the deferred poll-wake relay through `console_mgr`).
`ut` builds a `PollSet` over fd 0 AND the note fd, blocks in `poll()`, and
services note wakes at idle. **And `ut` says so, at that loop, by name:** *"LS-8c:
the multi-fd poll loop. LS-8a made /dev/cons pollable (a `.poll` hook + the
deferred poll-wake relay), so the shell now polls stdin (fd 0) AND its own note fd
together."*

**PATTERN, SEVENTEEN BATCHES — AND THE DISTANCE KEEPS COLLAPSING.** b37: the claim
is false and nothing reads it. b38: the claim is false and what reads it is a
second implementation, written because the first never worked. b39: the claim's
premise stays true while its conclusion dies. **b40: the claim is false and its
refutation is written in the direct caller, describing the same mechanism by
name.** The correction was not missing. Someone understood the change precisely,
in writing, while looking at exactly this subject — and wrote it at the CALL SITE
rather than at the claim. That is the generalization this arc has been circling:
knowledge is present in the tree and wrong in a specific place, because updates
land where the work happens, not where the claim lives.

There is a mechanism visible in the wording. The header labels itself *"Scope
boundary (binding -- do not smear later chunks in here)"* — it reads as a rule
about what may be ADDED, not as a description that must be MAINTAINED, so a
maintainer implementing LS-8c had every reason to leave it alone.

**F2 -- TWO LISTS DOCUMENTED AS THE SAME LIST, DIFFERING BY ONE DIRECTORY, WITH
TWO SYMPTOMS (#110).** `install_completion` scans `["/bin", "/goroot/bin"]` and
describes itself as *"matching `resolve_command`'s search list so a resolvable
command is a completable one"*. `resolve_command` searches THREE:
`["/bin/", "/", "/goroot/bin/"]`, the root entry deliberate and documented there
as *"the initrd root (where the boot-test shell runs)"*.

Two symptoms from one drift, because #115c reuses the SAME sorted vector for
command-line validity colouring — `refresh_command_index` hands `names` to both
`set_known_commands` and the completion source. A command reachable only via `/`
would fail to complete AND render **cinnabar**, the colour meaning "unresolvable",
while resolving and running fine.

Reported at the strength the evidence supports: currently LATENT. The post-pivot
session root holds only data files (`/thylacine-version`, optionally `/chase-w2` —
read off the build's populate block), and the shell that does run from a
root-level namespace is the bare-spawn boot check, which never installs
completion. It goes live the first time an executable is baked at the pool root.

**F3 -- THE CANDIDATE CAP IS APPLIED BEFORE THE SORT, AND THE SORT'S OWN COMMENT
STATES THE PROPERTY THE CAP UNDOES (#111).** `complete_path` iterates `read_dir`
in FS order, `break`s at 256, then sorts — under the comment *"read_dir order is
FS-defined; sort so the menu + LCP are deterministic."* The sort fixes the display
ORDER. It cannot fix the SET, because truncation already happened, and which 256
of N survive is exactly the FS-order dependence being disclaimed one line below.

Not cosmetic: `do_complete` computes `longest_common_prefix` over whatever set it
is handed and INSERTS it when it is longer than the typed word. Past 256 matches,
the surviving subset can share a longer prefix than the full set does — so Tab
extends the line to a prefix that excludes valid candidates, differently on
different runs. Command completion is unaffected (its vector is pre-sorted, so its
first 256 are deterministic), which is the detail that makes the path case easy to
miss.

**F4 -- A LIVE VISUAL DEFECT WHOSE DEFERRAL WAS JUSTIFIED BY WHAT THE TEST COULD
SEE (#112).** `render` documents that a shrinking multi-line buffer leaves the
trailing lines on screen, and names the fix: *"U-6 will track prev_render_lines +
emit \x1b[J."* U-6 landed — it is `repl.rs`, whose header calls itself U-6g — and
neither the tracking nor the escape exists anywhere in the crate (the only screen
clear is Ctrl-L's full-screen one, which happens to hide the artifact when a user
resorts to it).

The reason recorded for accepting it is the interesting part: *"For U-4b the boot
probe only checks emitted bytes (not screen state) so this is invisible."*
Invisible to the probe. Fully visible to a user editing a multi-line command.
**The deferral was scoped to the observer rather than the affected party**, so
nothing ever forced the issue — the only witness that could complain is one that
cannot see screens. A sibling of #105's shape (a property bought and never
delivered), arriving from the opposite direction: here the missing coverage is
what let a KNOWN defect stay.

**F5 -- CONSTRUCTION SNAPSHOTS, NOW OUTSIDE eval/ (extends #108).** `lib.rs` lists
`line_editor` under "Modules deferred to later U-* chunks" FOUR LINES above
`pub mod line_editor;`. `line_editor.rs` lists Tab completion as deferred fifty
lines above the `MenuShow` variant implementing it, and opens with the strategic
claim *"v1.0 has no PTY surface, so a pure-logic engine is the only thing that can
land before U-PTY"* — the PTY arc has landed, kernel seam and userspace server
both. `path.rs` promises richer abbreviation "at U-4", which landed without it.
#108 was scoped to the eval modules; it is crate-wide, front door included.

**F6 -- A RENAME THAT STOPPED AT ITS DEFINITION (#113).** The Bonfire migration
commit touched exactly ONE file: `palette.rs`, 175 lines, which now records that
Bonfire supersedes Pale Fire. Twelve "Pale Fire" descriptions survive across seven
files — including the shell's `Cargo.toml` description and the version banner in
`main.rs`. Every colour is right; six of the seven files that say what the palette
is CALLED say the superseded name.

**A HYPOTHESIS I CHECKED AND KILLED.** The tidy story was self-propagation: a
superseded name surviving in most sites becomes the name new code copies, and
#118's prompt-abbreviation comment (*"the canonical Pale Fire display"*) looked
like fresh code adopting the dead name. The timestamps say otherwise — #118 landed
at 11:11 and the Bonfire migration at 12:12 the SAME DAY, so #118 was accurate
when written. The two later Bonfire mentions in `line_editor.rs` are #115c test
comments, which used the new name correctly. So: a rename that stopped at its
definition, not one that regrew. Worth recording because the killed version was
the better story and the check took one `merge-base`.

**THE COUNTERWEIGHT IS THIS ARC'S b34 FINDING ANSWERED CORRECTLY.** b34's defect
was a reason written but not as a PRECONDITION, so the second consumer could not
inherit it. Here the ordering constraint — `open_notes` MUST precede the pts
session dance, because seating the shell as a pts's foreground group makes a `^C`
post `interrupt`, and a shell that is not yet self-managing is default-terminated
by its very first keystroke — is written at BOTH ends. `init_pts_session`'s doc
carries it as an explicit caller obligation ("NOTE: the caller must `open_notes`
FIRST"); `main.rs`'s call site carries the reason it obeys ("Hoisted ABOVE the
PTY-4b session dance"). **Neither side can lose it**, because each states it
independently — which is exactly what the b34 site lacked.

Three more, traced sound and worth recording: the ONE mode vocabulary
(`console_apply_default` and the LS-7 foreground restore share
`eval::console::PROMPT_MODE`, *"defining it once removes the drift hazard"* — a
deliberate anti-drift move in the crate this batch found six drifts in); the `!jc`
gate keeping a stray forwarded `--consctl-fd` from clobbering the pts dance's ctl;
and the R3-F1 pre-evaluation note drain (an idle `Ctrl-C` left queued would be
forwarded by the next command's interruptible wait and kill a just-spawned child)
— an audit finding turned into a documented invariant at the site.

Small, caveat-only: `ansi.rs` documents that non-CSI escapes would be over-counted
and rests on *"disciplined Utopia programs emit only CSI 24-bit-color SGR +
reset"* — while `repl.rs`'s menu strip emits `ESC 7` / `ESC 8`, two-byte non-CSI
escapes `visible_width` would count as two columns each. Harmless only because the
strip is written straight to the sink and never measured.

LEDGER. Corpus 834 -> **836**. Coverage 216 -> **224 owned of 421**, 51% ->
**53%**; unswept lines 72185 -> **67158** (-7.0%). `usr/utopia` 18/8 -> **26/0**,
5027 -> **0** — the first area this arc has taken to zero, and #93 closes with it.

And a note against the rule's own erosion: I drafted all four of those numbers
BEFORE running `render`, which is precisely what the last three batches' entries
forbid — and this time every one of them was right. That is not evidence the rule
is unnecessary; it is the exact result that would tempt someone to drop it. The
rule is about the ORDER of operations, not the hit rate, because a wrong ledger
number is indistinguishable from a right one until it is checked. Checked, then
kept.
