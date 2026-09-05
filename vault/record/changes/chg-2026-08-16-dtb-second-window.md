---
id: chg-2026-08-16-dtb-second-window
type: chg
title: "A literal inside a filter reads as validation and was a selection"
date: 2026-08-16
arc: arc-vault
commits: ["971639cb"]
touched: [sub-kernel-dtb]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
A small change with a shape worth naming: a hardcoded value that was correct for
every case the system had ever seen, invisible as a decision, and found only when
a device arrived that the value structurally excluded.

## The window that could not hold the device

The host bridge's ranges property encodes an address-space code in the high word
of each entry. The walker matched one code — the 32-bit memory window — because
every device the tree had ever placed fits inside the roughly 750 MiB that code
names.

A graphics device configured with host memory presents a **multi-gigabyte**
region. Not "larger than convenient": the 32-bit window **cannot** hold it, by
arithmetic. The 64-bit window, half a terabyte wide on the reference board, is
the only arena that can, and nothing was looking at it.

## Why the constant was invisible

It appeared as a literal comparison inside a filter — skip this entry unless its
code equals the expected one.

**That shape reads as validating an entry, not as choosing among several.** A
reader auditing the walker sees a well-formed check against a specification
value, confirms the value is right, and moves on. The question that would have
found it — *which of the legal codes is this, and who needs the others?* — is not
prompted by anything on the line, because a correct equality test against a
correct constant looks finished.

**A filter and a validator are written identically and mean opposite things.** A
validator says the other values are wrong; a filter says the other values are
someone else's. Only the second implies a missing caller.

The general form worth carrying: **a constant that has always been right is
indistinguishable from a constant that is the only possibility** — and the
difference only shows up when something arrives that needs the other branch. Up
to that moment, every observation confirms both readings equally.

## The generalization, and the reason it was not a copy

The walker was **parameterized** on the code rather than duplicated, with the two
entry points as one-line wrappers.

That is the right call for a specific reason, not a stylistic one: the parsing is
the part of this file that took several attempts to get right — property order
within a node is not guaranteed, so these lookups accumulate per depth and emit
at the closing token rather than acting on first sight, and the naive version is
recorded in the source as the bug this replaced.

**A parallel copy would have duplicated the fragile half and diverged on the
trivial half.** The two windows differ by one integer; the walking they share is
the part with a history.

## The comment that gained a caller and kept its contract

A later round had to correct the shared walker's own comment, which still said it
returns the 32-bit window. True of one wrapper. False of the function the
sentence now sits above.

**A function that gains a parameter gains a new contract, and the sentence above
it keeps the old one.** The old text is not stale in the ordinary sense — it
describes real behaviour, accurately, for one of the callers — which is exactly
why it survives a read: it is *true*, just no longer *complete*, and truth is
what a reader checks for.

That is the fifth instance in this sweep of a correction landing at one site and
not its sibling, and the second where the surviving text is true-but-partial
rather than simply wrong. The partial ones are harder: **checking a claim
confirms it, and nothing about confirming it reveals what it stopped covering.**
