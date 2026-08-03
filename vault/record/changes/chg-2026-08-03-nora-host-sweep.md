---
id: chg-2026-08-03-nora-host-sweep
type: chg
title: "nora's process half, and two dossiers the merge made wrong"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-nora-host
  - sub-kernel-cons
  - sub-kernel-devctl
  - moc-userspace-shell-tui
established:
  - sub-nora-host
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-03
---
Batch 45: nora's process half — main, lsp_host, dap_host. 3 files, 2286 lines.
**nora is COMPLETE**, 13 of 13, across three dossiers. L-1 absent on the
THIRTY-THIRD check.

**THE SYNC MERGE MADE TWO PRESENT-PLANE DOSSIERS WRONG, AND FIXING THEM WAS PART
OF THIS BATCH.** Main had moved for the first time in six batches, landing the
console's receive-admission rework. It touches two files the vault owns, and the
dossiers describing them were written before it — so merging it into this branch
is what introduced the falsehood. Leaving it for a later batch would have meant
the vault knowingly carrying a wrong table that this session put there.

Measured against the code rather than the commit message:

- The receive ring is **512**, not the 256 the table gave twice, and it is sized
  by a static assertion to hold one worst-case cooked flush — because the obvious
  per-byte admission check would have refused a maximal line *forever*, trading a
  bounded drop for an unbounded wedge.
- The cooked arm **back-pressures**; the table said it "drops the overflow". Both
  producers now learn the answer: the serial drain masks receive and leaves the
  bytes in hardware, the graphical keyboard feed returns a short count.
- `/ctl` has **seven** leaves; the dossier said six. The console's counters were
  added by the instrumentation work two commits earlier and the leaf list had not
  caught up.

Three mechanisms from that rework were worth capturing rather than merely
correcting, because each states a consequence the code alone does not:
**a refusal changes nothing** (the line stays assembled, the terminator is not
consumed, and the echo moved *inside* the accepted branch, since echoing a
refused byte shows a character the console did not take and then shows it twice
on re-offer); **a one-byte holdback covers the pre-check race** (the room check
is lockless, so a peer can take the room before the under-lock push, and by then
the byte is out of the data register — without the holdback "the fix would merely
NARROW the loss window"); and **a full ring never suppresses the trusted path**
(a serial break is recognized before any admission logic and ungated by the mode
flags, so the secure-attention trigger cannot be starved by filling the ring).
Both dossiers had their `updated:` bumped by hand, since lint does not yet catch
a body that changes without one.

**F1 -- THE FORMATTER'S SAFETY ARGUMENT RESTS ON A PREMISE THE SAME FILE
INVERTED (task #133).** `gofmt_source`'s comment closes: a broken-pipe note from
a died-early formatter "is dropped by the kernel ... nora is not self-managing
(no notes fd)". nora opens a note queue at startup, and the comment eleven lines
above that call says in capitals that doing so **makes nora SELF-MANAGING** — the
queue arrived so the loop could wake on a console resize.

Four hundred lines apart, in one file, and the later one is right. Behaviourally
benign: the note is queued and then discarded by the queue's own drain arm, so
nothing misbehaves. What is wrong is the *reasoning*, and this comment is a
safety argument rather than a description — it exists to explain why writing to a
possibly-dead child's pipe is safe here. The next author extending that handling
inherits a premise that no longer describes the program.

**F2 -- THE BYTE-VERSUS-CHARACTER STALENESS IS A PATTERN, NOT A SLIP (task #131,
widened).** Last batch found the display model's header saying "byte columns"
eleven lines before warning that they are character columns. The diagnostic
conversion in this batch does the same thing at four lines: its rustdoc says LSP
offsets "become BYTE columns against the actual line text", and the comment
inside the function says "TWO conversions, and both are load-bearing ... every
position in nora is a CHARACTER column."

Two files, same wrong claim, each with its own correction immediately beneath.
That is one era's convention surviving in the prose after the code moved, rather
than one careless sentence. All four crossing sites were checked and every one
does both steps — outbound at the cursor, inbound at a jump, and both ends of a
diagnostic span. The task now names both sites.

**F3 -- the #124 arm, confirmed from the consuming side.** The open request's
hard read-error branch reports and returns early, skipping the buffer-open call
that disarms the pending jump. Found last batch from the engine; this batch
matched it to the one caller that can skip the consumer.

**THE COUNTERWEIGHTS ARE ABOUT ABSTENTION AND ABOUT NAMING THE HAZARD.** The
trusted-path contribution is a *refusal*: nora acquires the screen and never
opens the console control file, so it can never change the line discipline out
from under the shell and never becomes console-attached. Verified rather than
trusted — the control file's name appears exactly once in the whole crate, in the
comment saying nora does not touch it, and the libthyla-rs surfaces it imports
are only allocation, arguments, errors, files, I/O, notes, poll and process.

Both children carry the same shutdown, and it states *why* rather than what: an
orphaned language server holds the workspace, an orphaned debugger holds its
debuggee and its stop, so the kill and wait are unconditional. The formatter
argues its own deadlock-freedom from the child's implementation (it reads all of
stdin before emitting) plus a stderr cap chosen to sit under the kernel's pipe
buffer, pipes all three stdio slots so the child can never paint on the
alt-screen or steal a keystroke, and guards the result so a pathological output
cannot clobber the buffer. And two conversions decline instead of guessing: a
diagnostic past the end of the buffer is dropped rather than clamped, "where it
would mark innocent code", and a jump into an unread file states its own error
bound.

**THE TESTS ARE ABSENT BY DESIGN, AND THAT WAS MEASURED.** All 238 of nora's host
tests are in the other nine files; these three have zero, counted rather than
assumed. The split is the crate's stated architecture — protocol in the client
library, terminal here, a host-tested engine between. Worth naming anyway: the
file that owns the console, spawns children and performs every write is covered
by a booted VM or not at all.

**AND THE DEBUGGEE-OUTPUT PRODUCER WAS TRACED END TO END.** Last batch found the
renderer emitting foreign text unsanitized and named a program under the debugger
as the easiest trigger. The chain is now complete through this layer: the
debuggee's output arrives as a protocol event, is trimmed and clipped **for
length and not for content**, is stored in the scrollback, and is drawn by a
widget that emits characters verbatim. This is where such text enters the editor,
so it is the natural place to sanitize, even though the omission is shared.

LEDGER, read off the rendered view **before** being written here — last batch it
was written first and merely happened to match. Corpus 844 -> **846**. Coverage
254 -> **257 owned of 421**, 60% -> **61%**; unswept lines 50012 -> **48060**.
`usr/nora` 10/3 -> **13/0**.

**And this time the unswept delta is NOT the batch's line count, which is the
check earning its keep by disagreeing.** The three files are 2286 lines but the
figure fell by only 1952. The gap is the merge: it added 334 net lines in files
nothing owns — the serial driver (+123 and +7) and the compositor's front end
(+204) — so 50012 + 334 - 2286 = 48060 exactly. Two batches running the delta
matched the swept line count and that was worth noting; here it does not, and the
discrepancy is itself a measurement — of how much unswept surface main grew while
this branch was working.
