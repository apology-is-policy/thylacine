---
id: chg-2026-08-03-utopia-parser-sweep
type: chg
title: "the ut parser — a bound found by an audit, and 878 tests that have never run"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-utopia-parser
  - moc-userspace-shell-tui
  - moc-userspace
established:
  - sub-utopia-parser
  - moc-userspace-shell-tui
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-03
---
Batch 38: the `ut` shell's parser — lexer, token, parse, expr, ast, error,
span, mod. 8 files, 7884 lines. Main unchanged at `2f7cbc83`. L-1 absent on the
TWENTY-SIXTH check.

**#93 SPLIT THREE WAYS BEFORE STARTING, IN DEPENDENCY ORDER.** utopia is 19864
lines and separates cleanly: parser (text -> AST), evaluator (AST -> effects),
interactive layer (line editor + REPL + the `ut` binary). Now #101/#102/#103.
Parser first because it has no dependencies — the evaluator's dossier links to
it, not the reverse, so writing it first means no dangling wikilink.

**THE SUBJECT IS PURE COMPUTATION AND STILL NEEDS A DOSSIER.** No syscall, no
filesystem, no capability — `audit: none`, and a mis-parse cannot escalate
anything. But **a deep enough parse exhausts the stack of a `no_std` program
with no guard page of its own, and a shell that dies takes the session with
it.** That is a liveness property with no invariant number, and it is most of
what the parser's design is about.

**THE BEST THING HERE IS A COUNTERWEIGHT WITH A HISTORY.** There are THREE
recursion bounds, because the recursion has three shapes and no single counter
sees all of them:

- **bracket nesting (64)** — a flat pre-pass over the token stream, run before
  any recursion starts;
- **operator recursion (256)** — right-associative `**` chains and prefix
  `!`/`-`/`~` chains, which are not brackets, so the pre-pass counts nothing;
- **re-lex depth (32)** — `$($($(…)))`, where a whole substitution is ONE token
  in the outer stream, so the pre-pass sees depth 1.

The middle one was added by an audit round after the first shipped, and the
comment says so: *"(RW-9 round-2 F1; the bracket-only check_token_nesting missed
this)"*. **A bound that covered the case it was written for, and an adjacent
recursion shape that inherited nothing** — the arc's b32 pattern, caught by a
round-2 audit and then written into the code as its own explanation. Verified
complete this pass: the pre-pass re-runs inside the token-stream entry point, so
it also covers every re-lexed body; `[`/`]` are glob word chars with no bracket
tokens, so its three-pair set is not missing a fourth; and the re-lex counter is
decremented on both the success and error paths.

**F1 -- 878 TEST FUNCTIONS ACROSS SIX NATIVE CRATES CANNOT COMPILE, AND THE
PATTERN THAT FIXES THEM IS PROVEN IN-TREE (#105).** Ground-truthed by running
cargo, not inferred:

    libutopia 385 (188 in the parser) · nora 238 · kaua 92 · libdriver 86
    parley 73 · tapestryd 4

Two independent blockers, both verified. `usr/.cargo/config.toml` pins
`target = "aarch64-unknown-none"` for every invocation; that target has no test
crate, so `cargo test -p libutopia --lib` fails with **`error[E0463]: can't find
crate for 'test'`** before an assertion runs. The escape — an explicit host
target — dies one level down in libthyla-rs, whose `global_asm` will not
assemble for Darwin (**"error: unknown directive"**).

**And the fix is not hypothetical: exactly one crate already does it.**
`usr/lib/netdev` carries `#![cfg_attr(not(test), no_std)]` AND an optional
libthyla-rs dependency, and

    cargo test -p netdev --lib --no-default-features --target aarch64-apple-darwin
    -> running 7 tests ... ok. 7 passed

Both ingredients are necessary and neither sufficient — netdev without the
explicit target still fails; a host target without dropping libthyla-rs still
fails; kaua HAS the optional dependency but keeps unconditional `#![no_std]` and
fails on both counts. The sharpest detail: **the largest stranded block is a
pure-logic parser whose own header says "Pure logic; no I/O; host-testable"** —
false in the third clause, in the file that would benefit most.

Partially mitigated, which is why nothing has failed: `usr/u-test` (3469 lines)
re-covers the parser IN-GUEST, importing `libutopia::parser` directly and
running every boot. So the surface is covered — by a second, independently
written body of test intent, of which the older and more granular one is dead.
This also **corrects the net-2d audit's recorded seam** ("netd has no host-test
harness -- libthyla-rs is no_std + aarch64-asm"): right about the cause,
generalized too far, and netdev had already disproved the generalization before
it was written.

**F2 -- A SUBSTITUTION BODY'S ERRORS ARE REPORTED IN THE WRONG COORDINATE
SYSTEM, ON THE ONE PATH OF NINE THAT DOES NOT RE-ANCHOR (#104).** A `$(...)`
body is parsed as an independent source, so its spans index the BODY; the
evaluator renders spans as bare byte offsets with nothing marking which source
they belong to. `run_command_substitution_script_inner` re-anchors deliberately
on eight early-return arms, and `run_command_substitution` goes further —
discarding the inner `ParseError` entirely (`|_e|`) to avoid the problem, paying
the diagnostic to buy the correctness. The ninth path is a bare `?` out of
`evaluate_argv`, which propagates a body-relative span unchanged. So
`echo $(ls $((1/0)))` reports an offset into `ls $((1/0))` — off by the 6-byte
prefix, confidently and silently.

**F3 -- THE ONLY WRITTEN RECORD OF THAT HAZARD IS ATTACHED TO THE WRONG ITEM.**
The paragraph explaining that sub-script coordinates are NOT translated is an
orphaned doc comment: its function moved, and with no blank line to stop it the
text now documents `const PARSE_MAX_RELEX`, whose rendered documentation opens
*"Eagerly parse a substitution body … into a sub-Script"*. It also names
`_outer_span`, a parameter neither item has. **So the reader who needs the
warning — someone reading the sub-parse function — never sees it**, and the
reader of a constant sees a function description. F2 and F3 are one story: the
hazard was understood well enough to pay for it on eight paths, and the note
explaining it drifted onto a constant.

**F4 [minor] -- the nesting pre-pass's counter is not clamped at zero**, so a
stream of leading unmatched closers buys extra depth. Unreachable (the parser
rejects them long before), but it measures NET rather than MAXIMUM nesting,
which is not the quantity the bound wants.

**PATTERN, FIFTEEN BATCHES.** b36 the fix carried its own bug report and landed
on one of two identical sites; b37 the claim is simply false and nothing read
it; **b38 the claim is false, something DOES read it — and what reads it is a
second implementation of the same intent, written because the first never
worked.** 878 tests are not a documentation defect; they are maintained,
assertion-bearing work that has never once executed, and the response was to
write a 3469-line in-guest runner rather than to notice. The parser is well
covered. It is just not covered by its tests.

LEDGER, read off the rendered view. Corpus 829 -> **832** (three notes again:
another area opened). Coverage 198 -> **206 owned of 421**, 47% -> **48%**;
unswept lines 87022 -> **79138** (-9.1%). `usr/utopia` 0/26 -> **8/18**, 19864
-> 11980. `usr/lib` unchanged at 3/38. Both metrics moved together for the
second batch running.
