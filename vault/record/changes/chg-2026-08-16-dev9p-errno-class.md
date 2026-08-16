---
id: chg-2026-08-16-dev9p-errno-class
type: chg
title: "The channel existed and the value was being discarded"
date: 2026-08-16
arc: arc-vault
commits: ["07b79f0b"]
touched: [sub-kernel-ninep-dev9p]
established: []
closed: []
opened: [seam-fstat-errno-flattened-above-the-leaf]
depth: rich
created: 2026-08-16
---
Two commits since the dossier, and they are the same lesson at two addresses: a
statement about the world that was true when written and quietly stopped being
true. One was a comment naming a single server; one was a return value being
thrown away. **The dossier had already recorded the first correction and then
committed the error itself, three paragraphs later.**

## The fix that needed no mechanism

The name operations collapsed every failure to a bare sentinel, which the
boundary lines render as a generic I/O error. So "you cannot remove a directory",
"that directory is not empty", "no such file" and "you lack permission" reached
every caller as one indistinguishable noise. It is why the C library's remove()
had to be rewritten onto a stat-dispatch: its classic form branches on the errno
that never arrived.

The enqueued plan for this was **wrong**, and the commit says so plainly: it
prescribed "the same side-channel as the create path". It is not the same at all.
The create slot returns a pointer — there is nowhere in a pointer to put a cause,
which is exactly why that path needed a field bolted onto the private state. The
name slots return an integer, and the client below them already returned the
server's negative errno.

**The channel existed the whole time. The value was simply being discarded.** The
fix is to return it, plus one bounding helper on the way out. No new state.

That is worth keeping as a diagnostic habit rather than as a fact about two
functions: **before building a mechanism to carry a value, check whether the
carrier is already there and the value is being dropped.** A prescription
inherited from a neighbouring fix arrives with the neighbour's *shape* attached,
and the shape is the part least likely to transfer.

## What crosses a boundary for free

The load-bearing boundary fact, and the one I would have got wrong: **a SERVER
errno crosses by value.** The codes that motivated the whole change have no name
in this project's registry at all, and do not need one, because both boundary
lines map numerically. Zero registry appends were required.

The rule that falls out: **only errnos the kernel ORIGINATES must be named.** A
registry entry for a code the server supplies is ABI surface bought for nothing.

## One value cannot cross, and it is handled three ways in one file

A server's permission error is code 1, and negated it **is** the flat
generic-failure sentinel. The client's mapper rejects only a zero code and
anything above the window's top, so code 1 passes straight through.

Three paths in this one file decided about that collision independently:

- the name operations **fold** it onto the access-denied code — a deliberate
  small lie, chosen because reporting a permission problem as an I/O error is a
  much larger one;
- the write path **returns it raw** and documents the collision as a known
  residual owed to the rollout;
- the create accessor's passthrough window **silently excludes it**, so it
  becomes the flat sentinel again.

Nothing is broken. But a permission-denied create and a permission-denied unlink
now answer the same caller differently about the same class of denial, and the
difference is invisible unless you read all three. Recorded as a table in the
dossier for exactly that reason: **a per-path decision that was never taken as
one decision reads as arbitrary to whoever arrives next.**

## The comment that claims an immunity it does not have

Chasing that collision led to the native-stat arm, whose comment makes two
claims. Both are false, and they fail in opposite directions.

It says the integrity invariant bounds a server's code into a window that
excludes the sentinel, so this path can never see it. The mapper's clamp refutes
that — and the clamp's *own* comment names the window one value narrower than the
code it implements, which is very likely where the belief came from. A mistake
was made once, in a comment, and then read back as a fact by a second author.

It says both named callers propagate the value. **The resolver does; the stat
syscall does not** — its inner helper collapses every non-zero return to the flat
sentinel. And the project's own error-rollout document lists that syscall in the
still-owed half. So the comment tells the reader most likely to arrive here — the
one scheduled to do that exact chunk — that it is already done.

**The strongest evidence sits one frame below.** The resolver has a dedicated
converter for this return, and that converter guards *explicitly* against the
value the leaf's comment says cannot arrive. Two frames disagree in writing about
whether a case is reachable, and **the one that handles it is the one that knows.**
A guard is a load-bearing statement about reachability; when it contradicts a
neighbouring assertion of impossibility, the guard wins.

Filed as [[seam-fstat-errno-flattened-above-the-leaf]]. Comment-only — the
behaviour matches the rollout's own staging exactly.

## I nearly filed a stronger finding that was false

My first reading was that the leaf's propagated errno is **dead** — computed
carefully and read by nobody. That would have been a much better finding, and it
is wrong.

The resolver consumes it, deliberately, as the design document says it should.
The staging is real: the leaf got its errno in one stage so the resolver could
use it, and the syscall's own propagation is a separate scheduled item. What I
mistook for accidental waste is a half-finished plan proceeding in order.

I found this out by checking, before filing, whether the value had a consumer —
and the check took two commands. **The narrower finding that survives is worth
more than the dramatic one that would not have**, and the difference between them
was entirely the willingness to look for the reader before declaring the writer
pointless.

## The error the dossier had already been told about

The other commit is comment-only: the capability latch had described the
non-supporting servers by naming **one**, and the correction states them as a
class. The reasoning is precise about the harm — a reader working out *which
sessions get cached* arrives at this latch, and a single named example invites
the conclusion that the others are cached. They are not, and this latch is
exactly what keeps them out.

**The dossier already carried the corrected form.** It said "the majority of
servers — every native userspace 9P server — answer ENOSYS", which is right.

And then, three paragraphs below, in the sentence about the cacheability gate, it
said the latch's absence "is what keeps netd's stream files uncached."

One named instance. In the sentence about cache admission. Which is the precise
error, in the precise place, that the commit was written to fix.

**Knowing a rule stated abstractly does not prevent applying its exception
concretely.** I had the general form in the paragraph where the general form was
the subject, and reached for the vivid single example the moment the subject
changed to something else — because one server is easier to picture than a class,
and the sentence was *about* something else, so the naming felt incidental.

It is not incidental. It is the sentence a reader consults to answer the question,
which makes it the one place the class statement was actually load-bearing. Both
now name the class, say the list only grows, and say why a new native server is
non-supporting **by default**.

## Two smaller self-corrections, same shape

I twice this batch nearly recorded a tool defect that was a defect in my reading
of the tool.

The staleness checker did not flag the loom dossier after a merge, and I assumed
day-granularity had swallowed it. It had not: the tool reports a **three-way**
verdict and puts same-day pairs in an explicit "unverifiable either way" bucket,
which it printed, in a section I had filtered out of my own command.

Then the registrar refused two closure flags in a row and I began drafting the
extension. The flag I needed exists; the abridged usage line I read omits it, and
the full one four lines further down lists it.

**Both times the tool had already answered and I had truncated the answer.**
Once by piping into a filter, once by reading the short help. The instrument was
more careful than the reading of it — which is the same relation this whole batch
is about, one layer up.
