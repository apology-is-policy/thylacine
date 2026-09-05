---
id: chg-2026-08-06-ledger-correction
type: chg
title: "Correction: batch 55's pre-sweep baseline was 17829, and proximity beat provenance"
date: 2026-08-06
arc: arc-vault
commits: []
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-06
---
**Corrects [[chg-2026-08-06-process-creation-arc]]**, which states the
batch's pre-sweep unswept-lines figure as **17804**. It was **17829**.

**THE FACT.** 17804 is batch 54's *closing* number. This batch's own
render — run at the top of the session, immediately after merging main's
five commits, with the output on screen — reported 362 owned of 434 files
and **~17829 unswept**. The 25-line gap is
`usr/ports/sdl2/thylacine/SDL_thylacineopengl.c`, which one of those
commits grew and which nothing owns. The closing figure, 11837, is
correct; only the baseline is wrong, so the batch moved **5992** lines,
not 5967.

**WHERE IT LANDED IS THE POINT.** The sentence two paragraphs below it
reads "LEDGER read off the rendered view after the merge, for the sixth
consecutive batch." That claim is true of the *process* — the render was
run, post-merge, before anything was swept — and false of the *number*
that got written down. This arc has a standing rule that ledger figures
are read rather than predicted, and the rule was followed right up to the
transcription.

**THE MECHANISM, which is not the one this arc keeps recording.** The
established failures here are instrument failures: a detector deriving its
reference from the data under test, a gate that cannot parse its own log,
a regex blind to 38% of a corpus. All of those *measure* and measure
wrongly. This one measured correctly and then **never consulted the
measurement**. The number was transcribed from the previous batch's chg
note — same directory, same filename shape, same section heading, one
file over — while the true figure sat two tool calls up in the session.
**Proximity beat provenance.** The nearer text won over the authoritative
one, and nothing in the process distinguishes them, because both are
"a number in the right format in a plausible place".

The generalisation worth keeping: *a figure copied from a document that
looks like the one you are writing is not a reading, however recently the
real reading happened.* The defence is not more care at the moment of
typing — it is to re-derive at the checkpoint, which is what caught this.

**AND THE CORRECTION ITSELF BROKE A RULE, WHICH IS HOW IT ENDED UP HERE.**
The first attempt edited the original note in place. `quaestor lint`
refused the commit: **R3, the Record plane is append-only; correct via a
superseding note.** Exactly right, and worth recording that the gate fired
on a well-intentioned fix rather than on carelessness — the whole reason
the plane split exists is that a history you may quietly rewrite is not a
history. So the original stands as written, with its wrong number intact,
and this note is where the truth lives.

**THE KNOWN COST, already tracked.** A `chg` cannot gain a
`superseded-by` edge, so this correction is discoverable only from *this*
side — a reader who finds the original and stops there sees 17804 with no
signal that anything follows. That is task #58, and this is now a live
instance of it rather than a hypothetical one.
