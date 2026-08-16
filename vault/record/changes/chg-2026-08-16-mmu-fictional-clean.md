---
id: chg-2026-08-16-mmu-fictional-clean
type: chg
title: "A safety argument that was fiction, and the system was correct anyway"
date: 2026-08-16
arc: arc-vault
commits: []
touched: [sub-kernel-mmu]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
The real-silicon bring-up rewrote a comment in the translation layer, and the
comment it replaced is the most instructive artefact I have swept.

## False twice over, about a mechanism that does not exist

The old text explained how a secondary processor safely reads page tables the
primary built: the table builder cleans them to the point of coherency, using a
named cache-maintenance instruction.

Both halves are false. **No such clean exists** anywhere on that path. And the
instruction it named operates to the point of *unification*, not coherency — so
it would not have accomplished what was claimed even if it had been there.

Nothing was ever broken.

The tables are built and mutated through **cacheable** mappings with no
maintenance at all, and that is sound because the translation-control register
configures the walks themselves as cacheable and inner-shareable. A secondary's
table walker therefore **participates in coherency** and observes the primary's
dirty lines directly. The real reason is a configuration setting the comment
never mentioned.

## Why this one is worse than an ordinary stale comment

A stale comment describes something that used to be true. This described
something that was **never** true, about a mechanism that was **never** there,
and it survived for the project's life.

**It survived precisely because it was inessential.** A fictional account of a
load-bearing mechanism gets falsified by the first failure; a fictional account
of an unnecessary one never does, because the property it claims to guarantee is
guaranteed by something else. **The comments most likely to be fiction are the
ones describing the safety you do not actually depend on** — and those are
exactly the ones an auditor reads and trusts, because checking them requires
knowing the real reason, which is the thing the comment was supposed to supply.

A reader auditing multiprocessor coherence here would have found it, believed the
tables were cleaned, and reasoned from a false premise to a true conclusion with
nothing available to signal the gap. The conclusion checks out. That is the whole
trap.

## What it says about safety arguments generally

**A safety argument can be entirely fictional while the system is correct — which
proves the argument was never what made it correct.**

That is a stronger statement than "the comment was wrong". It means the written
justification and the actual mechanism had come apart completely, and no amount
of testing could reveal it, because the system's behaviour is a function of the
mechanism and not of the justification.

The only thing that finds this is someone deciding to check whether a cited
mechanism exists. Here that happened because a genuinely non-coherent path on the
same bring-up sequence *did* bite — the secondary's own pre-translation writes,
which are a real coherence problem with a real protocol — and investigating the
real one led to re-reading the fictional one beside it.

## My own dossier escaped by silence, which is not the same as being right

This dossier said nothing about table-walk coherence. It did not repeat the
false claim.

Worth writing down rather than quietly taking the win: **an omission that happens
to avoid an error is not a correct treatment of the topic.** Had I covered
coherence at all, I would have covered it by reading that comment, and I would
have inherited the fiction — the same way the vault's own invariant note
inherited a stale scope earlier in this sweep by borrowing from a summary rather
than from the source.

Both dossier and prosecution list now carry the real account, and a rule aimed at
the class: when a mechanism is cited as the reason something is safe, check that
it exists and that it does what its name says. The previous claim failed both
tests.
