---
id: chg-2026-08-03-crash-debug-sweep
type: chg
title: "the crash + debug tier — the symbolizer was shared and the raw address that made it safe was not"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-kernel-halls
  - sub-kernel-hwdebug
  - sub-kernel-exception
  - moc-kernel-entry
  - moc-kernel-introspection
  - inv-i39
established:
  - sub-kernel-halls
  - sub-kernel-hwdebug
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-03
---
Batch 34, the sixth sweep off the census: the crash + debug tier -- the Halls
crash dump (`halls.c` + `halls.h` + the generated symbol table and its stub,
603 lines) and the arm64 hardware-debug leg (`hwdebug.c` + `hwdebug.h`, 682).
Main had moved to `fe0c2fcd` (the Fable audit rounds across #107 / #94 / #106
/ CL-7k); merged before starting, which is also what made the coverage view
stale and required a render before the baseline was meaningful. L-1 absent on
the TWENTY-SECOND check.

Two dossiers, split across areas rather than kept together, and the split is
the claim: [[sub-kernel-halls]] under [[moc-kernel-entry]] because the per-CPU
frame slot it reads is written by the exception entry wrappers, and
[[sub-kernel-hwdebug]] under [[moc-kernel-introspection]] because it has no
namespace presence at all -- `/proc` is its only control surface, and
[[inv-i39]] is what it enforces.

**THE HEADLINE IS A FUNCTION THAT WAS SHARED WHEN ITS SIBLING DELIBERATELY WAS
NOT.**

`halls_symbolize_table` returns the greatest symbol at or below a query address.
Its documented failure modes are all on the LOW side -- empty table, below the
table base, below the first symbol -- and each is guarded and commented. On the
high side there is nothing, and the generator emits no end-of-text sentinel, so
**every address from the last text symbol up to 4 GiB above the table base
resolves to that last symbol**. The unit test pins this in as many words:
*"Past the last symbol -> still the last (no upper bound at v1.0)."*

The window is not exotic. The kernel image is a few megabytes and read-only data
follows text, so **a pointer to any kernel global, string literal, or the symbol
table itself renders as the last text function** with a large offset. Stack words
holding such pointers are ordinary.

**F1 -- AND THAT WAS FINE UNTIL THE SECOND CONSUMER REMOVED THE THING THAT MADE
IT FINE.** In the crash dump, `halls_emit_code_addr` prints the raw address and
the link address BESIDE every symbol, so a reader who sees a five-digit offset
discounts it -- and the artifact is a best-effort snapshot of a dead machine
anyway. Then 8b reused the symbolizer for `/proc/<pid>/kstack`, and 8b-1d's
holotype F1 split that file's output on capability, because raw kernel addresses
disclose the KASLR slide (an I-16 secret): the CAP tier sees raw + link + symbol,
the OWNER tier sees `#N  name+soff` and nothing else. The `<unknown>` branch that
exists precisely to say "this frame did not resolve" is reachable only through the
LOW-side conditions, so it can never fire above the last symbol. **An owner asking
why their process is hung cannot tell a real frame from a stack word that
happened to look like a code address.** Task #80.

What lifts this above a missing bounds check is what the SAME sub-chunk did one
function over. `halls_walk_kernel_frames` is a near-copy of `halls_backtrace`,
and the source says why it is a copy rather than a refactor: the dying-machine
dump path is an audit-trigger surface whose HX-I1/I2/I4 invariants must not be
perturbed, so the live path got its own function, its own explicit `[lo, hi)`
bounds, and an extra per-frame gate -- because on the live path there is no
re-entrancy guard to catch a bad read. That judgment is exactly right, and it is
about **fault-safety**. The symbolizer was then shared without the same question
being asked about **output honesty**, and the property that made it honest in
consumer one is the raw address that consumer two's own security fix deleted.

**F2 -- THE FIX LANDED ON THE SITE THAT HURT AND THE TWIN KEPT THE OLD NUMBER.**
`DEBUG_HWBP_SLOTS` is 16 and carries a full account of why: it used to be 4,
which starved Delve's `next` (one temporary HW breakpoint per successor PC plus
the return address, so a small step-over wants 4-5 slots alongside the user's
own), and the overflow surfaced as a failed ctl write reading as EPERM. The
stated principle is explicit -- *the software table never caps below the
hardware: a debugger gets EVERY HW breakpoint the CPU implements.*
`DEBUG_HWWP_SLOTS` beside it is 4, with a parenthetical. The DFR0 watchpoint
field is 4 bits exactly as the breakpoint field is, `hwdebug_init_cpu` already
clears all 16, and Delve's `watch` is the consumer. Capability only -- the index
is still bounded -- but the identical confusing failure. Task #81.

The sharp part is that **`hwdebug.c` contains both the reasoning and its
non-application**. `hwdebug_init_cpu` deliberately clears every IMPLEMENTED slot
rather than only the ones the v1.0 tables use, and says why: a stale enable bit
in ANY slot fires the moment MDE goes on for a breakpoint, because that one bit
gates breakpoints and watchpoints alike. That is the implemented-versus-used gap
reasoned about carefully, one function from the constant that ignores it.

**F3 and F4, both small.** The in-dump guard has two comments about the same
hypothetical future caller: the set-site says a survivable caller would require
converting the guard to save/restore, the clear-site says the tail clear already
keeps it honest for one. Both half true -- the tail clear covers the clean path,
not the faulting one -- and a reader who meets the second first concludes the
case is handled (task #82). And an out-of-range CPU index gets three
dispositions across the two files: clamped to 0 with a paragraph in halls, an
early return that silently skips both load and clear in `hwdebug_switch_in`, and
a clear-the-hardware-but-skip-the-bookkeeping in `hwdebug_disable_this_cpu`. All
dormant; only the first says so (task #83).

**THE COUNTERWEIGHTS, AND THERE ARE THREE GOOD ONES.** The deliberate
non-refactor above is the first, and it is the right instinct stated out loud:
*duplicate the code rather than perturb an audited path, and write down that the
duplication is on purpose.* Second, the crash dump reads
`ID_AA64ISAR1_EL1` DIRECTLY to decide whether to strip pointer-auth bits, rather
than consulting the boot-populated feature block -- because the dump can fire
BEFORE feature detection has run, so it must not depend on initialized global
state. That is a rare thing to get right and rarer to explain. Third, output
ordering: the register block goes out first because it is pure field reads,
before the backtrace and hexdump touch possibly-corrupt memory -- **the dump is
ordered by probability of surviving**, not by readability.

And a fourth worth keeping for its shape: all three debug exception arms deliver
their stop through the attach-gated path under the process table lock, not the
raw deliver. That is a fix (the SA-1 finding: an ungated arm parked targets whose
debugger had already detached), and what makes it good is that the fix was
applied to all three arms and the singlestep arm's detached path additionally
disables ALL debug registers rather than just the step bit -- because a step
loads MDE and the breakpoint table too. Symmetry maintained across three sites
that could easily have diverged. That is the shape F2 is missing.

**PATTERN, ELEVEN BATCHES.** b24 assertions pin values not their description;
b25 models pin mechanisms not their own scope; b26 each copy pinned to itself
not to the others; b27 the guard travelled but not its reason; b28 the ledger
pins the areas not the areas to the tree; b29 the enforcement list names a guard
that cannot fire; b30 plus a justification whose stated and real reasons
diverged; b31 the documents are wrong about which code runs; b32 the guard is
right about the case it was written for and silently wrong about the one nobody
asked it; b33 the exclusion list has one element and one reason, and the five
unexcluded cases are safe by mechanisms built for other questions; **b34 the
shared helper was safe because of something at the CALL SITE, and the second
call site removed it.**

b33 was about a reason that was never written. b34 is worse in one specific way:
the reason WAS written, carefully, in the original consumer -- the raw and link
addresses are emitted next to every symbol precisely so a bad resolution is
visible. It just was not written **as a precondition on the helper**, so the
second consumer could not inherit it and did not know it had broken it. The
lesson is narrower than "document your reasoning": a helper whose safety depends
on what its caller does with the result has a CONTRACT, and the place that
contract belongs is the helper.

**AND THE SWEEP CAUGHT THE SAME SHAPE IN THE VAULT'S OWN BOOKKEEPING.** The dump
reads `struct exception_context` and reconstructs the interrupted stack pointer
by adding `EXCEPTION_CTX_SIZE` to the frame address, so writing
[[sub-kernel-halls]] meant reading `arch/arm64/exception.h` -- which turned out
to be the ONLY unowned header in the tree whose implementation sibling is owned.
[[sub-kernel-exception]] claimed `exception.c`, `vectors.S` and `userland.S` and
described the frame a dozen times over, while the header holding the layout its
own comment calls LOAD-BEARING went unclaimed. One line to fix; recorded because
it is b34 reflexively -- the dossier claimed the code and not the contract, and
nothing failed, so nothing asked.

LEDGER. Corpus 819 -> **822**. Coverage 158 -> **165 owned of 421 (39%)**;
`arch` 27 owned / 11 unowned -> **34 / 4** (the six swept files plus that
header; the four left are the uart pair and the psci pair). [[inv-i39]] gains
[[sub-kernel-hwdebug]] as a third guard -- its no-escape clause ("breakpoints
are hardware registers") is literally true only there -- and its settled-thread
relaxation now carries the F1 qualification: the capability split closes the
KASLR disclosure and opens a smaller honesty gap the owner tier cannot see.
