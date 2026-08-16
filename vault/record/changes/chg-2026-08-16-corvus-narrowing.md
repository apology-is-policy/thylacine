---
id: chg-2026-08-16-corvus-narrowing
type: chg
title: "corvus re-swept: the AUTH gate was a narrowing, and the expired premise had three dependents"
date: 2026-08-16
arc: arc-vault
commits: []
touched: [sub-corvus]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
322 lines across two commits, and both are about the same thing from
opposite ends: a bound that was correct when written, whose justification
expired, and which nobody noticed because the justification was never the
part anyone re-read.

## The event: a capability class was structurally unreachable

No post-login program could acquire `CAP_JIT`. The chain is short and every
link was individually sound: `CAP_JIT` is elevation-only, so a program must
redeem a clearance itself; the only path to a clearance was
`CLEARANCE_ACTIVATE`, which is token-gated; the only source of a token is
`AUTH`; and `AUTH` refuses while a session is bound. Once `login` existed and
held the session, the loop closed. **Not a refused login — a whole capability
class walled off, in a system whose eligibility table said the user was
allowed it.** Every user-launched GL program, silently.

## The finding: this note had the mechanism right and the character wrong

The Concurrency section said the AUTH gate "is what actually prevents one
client overwriting another's session", and presented it as the load-bearing
design. #139 settled what it actually is by reading the model against the
code.

`corvus.tla`'s `AuthSuccess` says `~(\E s \in sessions : s.owner_proc = p)` —
no session owned by **this** Proc. The model permits concurrent sessions from
different Procs. The implementation refuses AUTH from *any* Proc while *any*
session exists. The gate is **strictly stricter than the model**, and the
comment claiming spec authority for it quoted the formula correctly while
paraphrasing away `s.owner_proc = p` — the exact clause the strictness hides
in. A comment can be right about its citation and wrong about what the
citation says.

The narrowing was sound for the world it was written in: joey the only
console-attached Proc, one connection per peer. Login and user programs
arrived later; the bound stayed. **A correct bound whose reason had expired.**

## The part that is mine, and it is a layering failure

This note already carried the caveat that the single-slot justification cites
a kernel cap that no longer exists — and then rested the safety argument on
"a mechanism the comment does not mention: AUTH's refusal while a session is
bound."

I never checked what the AUTH refusal's *own* justification was. It was the
same expired cap, one layer down. So the caveat diagnosed layer 1 correctly
and then stood on layer 2 without looking at its footing.

**An expired premise does not sit in one comment. It is inherited by the
argument written to replace it**, where it is harder to see because it now
appears as a conclusion rather than a citation.

The repair pattern rhymes with it: #139 fixed the third dependent —
`handle_auth`'s comment, the one that had just caused the outage — and left
the two in `main.rs` untouched. Correct as a priority call, incomplete as a
sweep, and both survivors still teach the next reader the same false thing.

## A bound I asserted that does not exist

The caveat said "the live bound is a global 64". Measured: `srvconn_create`
is a plain `kmalloc` of a linked-list node with **no cap of any kind** — no
per-Proc limit, no global limit, no fixed table. corvus's own `MAX_CONNS = 8`
is the only ceiling on the path, and it is corvus's own.

Where the 64 came from is unrecoverable. The nearest 64 in the srvconn header
is `SRVCONN_PATH_MAX`, a *path length*. The shape outlives the number:
**a stale constant does not stay in the note that quotes it — it gets reused
as a premise next door**, where it names no constant. A name-grep misses it
because the name is absent; a value-grep drowns because `64` is everywhere.
This is the [[chg-2026-08-15-handle-posix-fds]] lesson arriving one hop
further out — there a dossier quoted a stale ceiling, here a *different*
dossier derived a claim from one.

The census was also scoped narrower than it read. "Eight other sites" was
true of source + specs + reference docs; tree-wide the phantom cap appears
**19 times across 12 files**. Two of the nineteen are in `main.rs`, both
justifying the same decision.

## Three verbs are specified and unbuilt, and the wire cannot say so

`CORVUS-DESIGN.md` §6.4's verb table has eighteen rows. Fifteen have
handlers. Verbs 2 (`CHANGE_PASSPHRASE`), 6 (`USER_DELETE`), 9 (`ROTATE_KEY`)
carry byte-level payload formats in the same table, in the same style as the
fifteen that exist, and nothing marks them.

A client written from the table lands on the dispatcher's default arm and
gets **`BadFormat`** — the status meaning *your framing is wrong* — for a
frame framed exactly as specified. The seven statuses have no way to say
"unbuilt", so a documented-but-absent verb is indistinguishable on the wire
from a corrupt one.

The missing verb 2 is the sharp one: **corvus has no passphrase-change
path.** The only verb that sets a new passphrase is `RECOVER`, which demands
the 24-word paper phrase and rolls it — so changing a passphrase means
spending and replacing the recovery phrase. The design's own threat model
carries a row for "snapshot rollback after passphrase change", an operation
nothing implements.

Adjacent: the 9P dispatcher's header comment says a `BadFormat` staged by the
verb dispatcher "signals tear-down to the caller". True at two of three
staging sites — version mismatch and oversize payload, which set
`tear_down_after_drain` explicitly — and false at the unknown-verb arm, which
stages and continues. That is the one a real client reaches.

## What is genuinely better, and it is the same file

The new comments are the best in the tree at saying what they cost. The
`CAP_JIT` user-default states its own weakening at the site — widening
eligibility from one user to all multiplies the population of tokens that
yield the capability, and **what bounds the damage is I-42 containment, not
the eligibility table** — rather than presenting the containment argument as
though it made the widening free. `handle_auth`'s repaired comment names
itself an implementation narrowing and says what it cost. Both are the form
this vault keeps asking for: the argument, its scope, and the price.

Verb 18 is also the right shape. `CLEARANCE_ACTIVATE_SELF` is verb 15 with
the token removed and the identity taken from the connection's kernel stamp —
the Plan 9 factotum move, authorized as who you demonstrably are rather than
what you can quote. Tighter on two axes at once: no bearer secret to steal or
delegate, and the identity axis and grant target collapse to the same Proc.
Made a distinct verb rather than a magic value inside 15, on the stated
reasoning that two authorization paths should be nameable on the wire and
separately auditable.

## Two verifications worth recording, because they went opposite ways

**The spin caveat held and I nearly mis-corrected it.** A first read of the
accept path saw `if conns.len() < MAX_CONNS` with no else and no comment, and
I was about to record that this note's "the comment calls it a deferral" was
wrong. The else branch and the comment exist eleven lines further down, past
where the first window ended. Checking before fixing is what stopped a
correct caveat being corrupted into a wrong one.

Then both ends were verified rather than reasoned: the kernel's listener
probe sets POLLIN whenever `backlog_count > 0`, level-triggered under the
registry lock, and `SRV_ACCEPT_BACKLOG` is 16 against corvus's 8 — so the
window is twice corvus's own capacity, as wide as those two constants can
make it rather than as narrow.

## The frontmatter lagged the prose again

`validated-by` said `[prose, gate-smp]` while the body cites the model
repeatedly — "the design's model already permits a set of session records
keyed by owner", "the property the design's model names explicitly and
carries a negative counterexample for". `spec-corvus` exists. So the graph
did not link this dossier to its own spec, and every spec-coverage view
under-counted corvus.

Second time today, after [[chg-2026-08-16-boot-banner-mirror-set]]'s
`mirrors` field. Same direction both times: **the prose knew and the field
did not.** That is where the thinking happens, so that is where the finding
lands first — and the field is the half the tooling can act on.
