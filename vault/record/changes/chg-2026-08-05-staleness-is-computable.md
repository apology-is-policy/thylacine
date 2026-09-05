---
id: chg-2026-08-05-staleness-is-computable
type: chg
title: "Coverage was never currency — 45 of 112 dossiers describe code that moved"
date: 2026-08-05
arc: arc-vault
commits: []
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-05
---
Batch 54: `quaestor stale`, and the census it makes possible. Merging 88
upstream commits — the whole process-creation arc, the Linux-phenotype
gate, and two hygiene passes — turned a question this project had filed
and deferred into one it could no longer avoid.

**WHAT.** A new subcommand and MCP tool that asks, for every dossier,
whether the code it claims has changed since it was written. Both halves
are already in the frontmatter — `code:` names the files, `updated:` dates
the reading — so the check is a comparison; it just had never been made.
It is a WARN in the lint gate, never a FAIL, and it is churn-ordered.
Answer on first run: **45 of 112 dossiers**, with ~3394 lines moved under
the worst of them.

**WHY IT WAS OWED, AND WHY NOW.** The coverage ledger has always said in
its own body that it cannot answer this. Its number is a count of
*assertions*: a file is owned when some dossier names it. That was already
understood to measure claims rather than truth — but the gap was being
read one way (a false claim) and had a second, larger mouth (a claim that
was true and stopped being so). 84% owned was quietly doing duty as "the
vault is 84% current". It was not: the same tree was 84% owned and 40%
stale on the same afternoon.

The merge is what made it unavoidable rather than merely owed. A dossier
does not go stale gradually; it goes stale in a step, when a branch lands.

**THE BUG IN THE FIRST IMPLEMENTATION IS THE FINDING.** Version one
compared `updated:` against `git log -1 --format=%cI`, and reported the
resolver dossier as CURRENT. It is not — the resolver had gained four
gates the dossier does not mention, provably: two of the new functions
have zero occurrences in it and eight and four in the code.

The reason it passed is the whole lesson. Those commits are dated
2026-07-30. They were authored on a branch, and they arrived here on
2026-08-05, at the merge. The dossier was written on 2026-08-01 — inside
the gap. Against the commit's own date the dossier looks NEWER than a
change it could not possibly have seen.

**A commit's date is when it was written; on a branch that merges, that is
not when it became true here.** `--first-parent` attributes each change to
the merge that carried it in, which is the only date a reader of this
branch could have known. The fixture in `stale_test.go` reproduces exactly
that topology, and the revert probe fails at its own assertion, reporting
01-05 where 01-10 is correct.

**AND THE CENSUS BEFORE THE TOOL WAS WRONG THE OTHER WAY.** Sizing the
problem with a quick regex over frontmatter returned "23 of 112" — from a
sample of 70, because `code:` is written two ways and the regex matched
one. It was blind to 42 dossiers, 38% of the corpus, including the one
whose staleness prompted the whole exercise, and reported a confident
number anyway.

Two instrument errors in one sitting, same family, opposite mechanisms:
one read the wrong 63% of the corpus, the other read the right corpus
against the wrong clock. Both are the recorded shape — *a checker that
derives its reference from the data under test can only report internal
disagreement* — which is why the answer was to extend the tool that
already parses both forms rather than to keep scripting beside it.
[[gls-quaestor]] owns the reading; a second reading of one field is
precisely the drift both exist to catch, so `codeTarget` mirrors the
validator's stripping and a test pins them together.

**A THIRD, SMALLER ONE, CAUGHT BY ASKING WHAT THE NUMBER MEANT.** Bounding
the git walk by the oldest `updated:` (sound — a change older than every
dossier is stale for none) took it from 3.6s to 0.75s and silently moved
the summary from "453 code files checked" to "85". The dropped files *are*
checked; the answer is "not stale". Counting them below the lookup would
have redefined the total as "files that changed recently" while the label
still said checked.

**WHAT ELSE THE MERGE SURFACED.** Eight conflicts, every one main writing
to a reference doc this branch had already stubbed — the fifth time, and
the reason it keeps happening is structural rather than careless: absorption
retires a document that the other track is still maintaining. Resolved by
keeping the stubs, with the ledger of what main added recorded in the merge
commit rather than discarded. Two of them are more than upkeep: the
resolver's four new gates, and the byte-I/O and pipe error contracts.

`.mcp.json` was untracked here to match main, which removed it for holding
host-specific absolute paths. Left alone, this branch would have
re-introduced at merge time the file main had just deleted.

**AND THE MCP WAS QUIETER THAN THE GATE.** `vault_lint` over MCP skipped
`checkCodePaths`, so a `code:` entry naming a file that does not exist
failed the pre-commit hook and passed the tool — the same divergence task
#73 records on a different command. Closed; and the test fixture then
failed, because it had been claiming a file it never created and only the
quieter gate had been looking.

**WHAT THIS DOES NOT DO.** It flags dossiers; it does not re-read them. The
45 are a work order, not a repair, and the top of that list is where the
next batches go. Nor does either ledger judge whether a dossier that covers
a current file covers it *well* — that remains unmeasured and probably
unmeasurable.

LEDGER, read off the rendered view after the merge. **362 owned of 434
files (83%), ~17804 unswept lines** — main's arc added 8 files and 6510
unswept lines beneath the previous batch's closing numbers, so carrying
those forward would have been wrong before this batch moved anything. Read,
not predicted, for the fifth consecutive batch; and this time the reading
also demoted the headline it was about to be used for.
