---
id: sub-kernel-exec
type: sub
parent: moc-kernel-execution
title: "Exec — turning a forked Proc into a running program, by three paths that are not the ones the docs describe"
code: [kernel/exec.c, kernel/include/thylacine/exec.h]
audit: hard
guarded-by: [inv-i36, inv-i12, inv-i32, inv-i33, inv-i44]
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/EXEC-LOAD-DESIGN.md", "docs/ARCHITECTURE.md", "docs/LINEAGE.md"]
created: 2026-08-03
updated: 2026-08-18
---
## Purpose

`rfork` makes an empty Proc. Exec fills it: parse an ELF, install a VMA per
loadable segment, add a stack and a guard page and the shared clock page, write
the System V startup frame a C runtime expects, and hand back an entry PC and a
stack pointer. It never transitions to userspace itself — the caller does that.

Creation in this tree is two steps, and this is the second one.

## Contract

Five entry points over **three paths**, and which is which matters more than the
count:

| entry | source | target | who calls it |
|---|---|---|---|
| `exec_setup` | a whole ELF already in memory | the Proc | **the boot path** — `joey.c` loads init this way — and the kernel tests |
| `exec_setup_with_argv` | same, plus argv | the Proc | nothing today |
| `exec_setup_from_spoor` | a pinned file, header only | the Proc | every `SYS_SPAWN_*` |
| `exec_load_into` | a pinned file, header only | **a detached address space** | `execve` |
| `exec_stage_env` | the caller's own environment | a kernel buffer | `execve`'s native front end |

The fourth is the fork arc's addition and it is what closed the
replace-in-place seam — see Mechanism. The fifth is not a loader at all: it
projects a process's environment into kernel memory so the *new* image's stack
can carry it, which has to happen before the commit that would otherwise make
the source unreachable.

The header describes the blob path as *"retained for the kernel test suite"*, and
the audit-trigger row calls it *"kept test-only"*. Both are wrong: `kernel/joey.c`
is compiled unconditionally, contains no test guard, and loads the first user
Proc in the system through `exec_setup`. See Caveats — the error is worth more
than a correction, because the two paths have genuinely different properties and
a reader told "test-only" will not prosecute the one that boots the machine.

All five share the same failure contract: **on any error the target is left
partially built and the caller must dispose of it.** Nothing here unwinds — and
for the detached-target loader that is not a caveat but the design: the caller's
only reference is the one it is about to release.

## Mechanism

Every path runs the same spine — validate, map each `PT_LOAD`, map the stack,
map the vDSO, build the frame — and they differ in where segment bytes come
from and what they build *into*.

### Replace-in-place was built, and not the way this dossier expected

The recorded seam said there is no `exec(2)`: both bodies reject a target that
already has mappings, so exec is spawn-only. `execve` now exists, and **the
reject did not move.** `exec_load_into` still refuses a non-empty target — the
comment on the line reads *clean target only* — because the target is no longer
the process. It is a **freshly allocated, detached address space**, and the swap
happens one layer up ([[sub-kernel-proc]]) after the load has completely
succeeded.

That is worth stating as a shape rather than a fact, because it is the better
answer to the problem the seam described. Teaching exec to replace in place
would have meant tearing down a live image *while* building its successor, with
a failure in the middle leaving neither. Keeping "clean target only" and
supplying a clean target instead means a failed load has touched nothing the
caller can observe: the detached space is the only reference, and releasing it
drains whatever got mapped.

**And the prediction attached to that seam came true exactly.** This dossier's
Concurrency section recorded that nothing rejects an exec on a Proc that already
has threads — the clean-address-space test excludes the multi-thread case only
*as a side effect* — and warned that this is "a real guarantee arrived at
sideways ... exactly the guarantee replace-in-place removes." It did remove it,
and two explicit checks replaced it: the exec-alone gate in the syscall's shared
core, and a re-check of the live-peer count *inside the same critical section as
the swap*, which extincts if a peer appeared. The second exists because proving
the property at the moment it matters is cheaper than asking a future reader to
reconstruct why no peer could have appeared.

**The blob path** copies each segment out of the in-memory image into a fresh
anonymous Burrow through the kernel direct map. Simple, and bounded by however
big the caller's buffer is.

**The file-backed path** ([[arc-revenant]]) reads only the ELF header and
program-header table — 16 KiB, whatever the binary's size — and then routes each
segment by a three-part gate:

```
file_shareable = PF_R  &&  !PF_W  &&  round_up(vaddr+filesz) == round_up(vaddr+memsz)
```

Each conjunct is a different argument, and none is decoration:

- **`PF_R`** keeps a no-access (`flags == 0`) segment on the eager path. It can
  never be read or faulted, so caching it would pin an Image slot and a Spoor
  ref forever in exchange for nothing.
- **`!PF_W`** is [[inv-i36]]'s fourth condition. A writable file-backed mapping
  is the thing this design refuses; writable data terminates in private
  anonymous memory or not at all.
- **The rounded-end equality** says every mapped page has a file page behind it.
  A segment whose `memsz` runs a whole page past its `filesz` has a bss page the
  fault arm could not fill from anywhere, so it takes the eager path and gets
  its tail from `KP_ZERO`.

Shareable segments go through the [[sub-kernel-image]] cache and are demand-paged
by the fault handler's FILE arm ([[sub-kernel-fault]]). Everything else is read
eagerly into a private anonymous Burrow, in a loop, because a single `dev->read`
can return short.

Since #45 this is **not** a text-vs-data split but a writable-vs-not split:
read-only rodata — roughly half a Go binary — rides the same shared, demand-paged
path as text.

### A segment no longer has to be page-aligned, and that changed the arithmetic everywhere

A real-world binary arrived whose `PT_LOAD` did not start on a page boundary,
and ELF does not require one. The mapping now starts at the **page-aligned floor
below** the segment's address, carrying an intra-page `lead` offset; the end
still rounds up. So the Burrow's offset 0 is the floor, and the segment's own
bytes land `lead` into it, with `[0, lead)` keeping its zero fill.

That single change propagates into four separate calculations, each of which had
silently assumed alignment, and the source marks all four: the copy destination
(`+ lead`), the file-page count inside a sparse backing (`lead + filesz`, which
is why the populated run always starts at slot 0), the instruction-cache span
(deliberately taken from the Burrow base rather than the copy pointer, *because*
that pointer is `lead`-offset), and the demand-paging fault arm — which still
**requires** offset 0 to equal the segment address, so the eager and file-backed
arms now differ in a property they used to share.

### Non-executable segments are sparsely backed

The eager-data cost has been partly lifted, and the reason is a measurement:
init's writable segment is 8 bytes of data behind 345 KiB of zero, and the
identity daemon's is 128 bytes behind 24 MiB. Eagerly allocating *and zeroing*
all of that at every exec was pure waste, so a non-executable segment now gets a
lazy backing populated only over the pages its file bytes cover; the rest
demand-zeroes on first touch.

Executable segments stay eager, and the reason is the instruction-cache
maintenance: the coherency sweep runs over the whole page-rounded segment from
the Burrow base, which is only a valid pointer because the allocation is eager.
The source says so at the sync site — widening the sparse gate to admit
executable segments would run that sweep over a lazily-populated Burrow. The
gate and the sweep are two hundred lines apart and each names the other, which
is what makes the coupling checkable.

Either way `[filesz, size)` reads as zero. Only *when* the page is allocated
differs.

## Data structures

No types of its own. It consumes `struct elf_image` from [[sub-kernel-elf]] and
produces VMAs and Burrows.

The one layout it owns is the **System V startup frame**, in two shapes. Shape A
is a fixed 176 bytes: argc, two NULL terminators, up to eight auxv entries, and a
16-byte `AT_RANDOM` block at the end. Shape B is variable — real argc, an argv
array pointing into a strings region, **an envp array between argv and auxv**,
the same auxv block, the same random block 16-aligned, then both strings regions.

The environment vector is the fork arc's addition and it is bounded on two axes
independently: a pointer count and a data size, because a caller can exhaust
either without the other. The frame's zero case is still exactly the fixed size —
asserted — which is what keeps shape A a special case of shape B rather than a
second layout.

Both shapes route through one auxv builder, deliberately, *"so the entry set
cannot diverge"* — which is the right instinct and the reason a reader can trust
that a binary sees the same auxiliary vector however it was spawned.

The frame always reserves room for all eight auxv entries even though
`AT_VDSO_CLOCK` is written only when the clock page mapped. That is what keeps
the random block and strings region at stable offsets: a conditional entry that
*moved* everything after it would make the layout depend on a boot-time
allocation.

## Concurrency

**Exec runs in the execing Proc's own context, before it has a second thread**,
and the file leans on that in three places — the name stamp, the exe-path stamp,
and the whole VMA installation, which is why `exec_setup` is the documented
exemption from the `vma_lock` discipline ([[lock-vma]]).

The claim is sound but it is a *precondition*, not a check: nothing rejects an
`exec_setup` on a Proc that already has threads. What stands in for the check is
the `p->vmas != NULL` reject at the top of both bodies — a Proc with a running
thread has an address space, so a clean-address-space test excludes the
multi-thread case as a side effect. That is a real guarantee arrived at
sideways, and worth knowing if replace-in-place is ever built, because it is
exactly the guarantee replace-in-place removes.

The one genuinely shared thing exec touches is the [[sub-kernel-image]] cache,
which carries its own lock and its own proof.

## Invariants enforced

[[inv-i36]] — conditions 3 and 4 are this file's dispatch gate; the rest live in
the fault arm, the Image cache, and Stratum.

[[inv-i12]] — by construction rather than by check. The gate admits only
non-writable segments to the shared path, and `vma_alloc` refuses `WRITE|EXEC`
for everything else, so no exec-created mapping can be both.

[[inv-i33]] — `proc_set_exe_path` is set last, only on success, and tolerates
NULL both ways: the boot blob path has no namespace name to record, and a #66
allocation failure leaves it empty. Neither fails an exec.

[[inv-i32]] — page charging happens in the map layer, not here.

## Error paths

Every failure is `-1` with the Proc partially built. Three classes: argument
rejects at the top, ELF rejects from `elf_load`, and resource failures during
mapping.

The extinctions inside `exec_build_init_stack` are the interesting case. It
crashes the box on argc over bound, argv data over bound, an unterminated argv
buffer, or a NUL count that disagrees with argc. Those look reachable from a
syscall and are not: the spawn handler validates all four first, including
counting the NULs. They are deliberate defense-in-depth on an exported function
whose contract this file also owns — the right posture, and checked rather than
assumed.

## Performance

The file-backed path's whole point: the eager read is 16 KiB regardless of
binary size, and text arrives on demand in read-ahead clusters. A binary of any
size execs, which the retired whole-ELF slurp could not do.

The eager stack is the standing cost — 1 MiB committed per Proc, raised from
256 KiB after a real port overflowed it into the guard page. The Linux answer
(a large lazy reservation committing only touched pages) is a recorded seam and
the infrastructure for it already exists.

## Prosecution

On any change: that the Spoor ref ledger balances on every path — the Image
lookup consumes a ref, the thunk keeps the borrow, and a NULL return consumed
nothing; that the dispatch gate keeps all three conjuncts, and in particular
that `PF_W` never becomes shareable; that the I-cache sync stays over the
**page-rounded** span and stays ungated on `filesz > 0`, because a pure-bss
executable segment is exactly the case that has no copied bytes; that
`exec_read_header`'s phdr bound stays ahead of `elf_load`'s deref; that the
frame math keeps the auxv block ending at or before the random block in both
shapes; and that any new auxv entry bumps `EXEC_INIT_AUXV_COUNT` (see Caveats —
the assert does not enforce this).

## Seams

- ~~**No exec-replaces-in-place.**~~ **CLOSED**, by supplying a clean target
  rather than by relaxing the clean-target rule — see Mechanism. The reject is
  still there; what it guards moved from the process to a detached address
  space.
- **The blob path's whole-image slurp** survives for boot, bounded by its own
  init-blob cap rather than by the file-backed path's header read.
- **Anonymous copy-on-write data** is the recorded v1.x lift. **Partly
  overtaken**: a non-executable segment's zero tail is now demand-filled rather
  than eagerly allocated, which is a different lift from copy-on-write and
  removes most of the same waste. What remains owed is sharing a *written* data
  page between parent and child, which is the fork arc's business, not this
  file's.
- **Dynamic linking is refused permanently**, not deferred.

## Caveats

**Two documents call the blob path test-only, and it is the boot path.** The
header says "retained for the kernel test suite"; the audit-trigger row says
"kept test-only". `joey.c` loads init through it, unconditionally. This is not a
tidiness complaint: the blob path slurps a whole image, never consults the Image
cache (so init's text is shared with nothing), and reads from memory rather than
through a death-interruptible `dev->read`. A prosecutor working from either
document would skip all of that. Task #63.

**`docs/REVENANT.md` does not exist and never did.** Five source files cite it,
six times, with section numbers. The sections resolve — in
`docs/EXEC-LOAD-DESIGN.md`, which is the same document under the name that was
actually committed. Task #64.

**The argv bounds exist in seven places and one is stale.** Two authoritative
macros, two hardcoded literals here, two more inside the frame-size macro, and
two prose statements — of which one says 4096 for a constant that is 65536, in
the same header that says 64 KiB seventy lines earlier. Nothing ties any copy to
any other. Task #65.

**The segment mapper's step list still describes the alignment rule that was
removed, and its own correction is four lines below.** The block heading
`exec_map_segment` says step 1 is *"round vaddr range up to page boundaries"* —
the start now rounds **down** — and the copy comment beneath it says *"vmaddr_start
corresponds to BURROW offset 0 (the segment is page-aligned)"*. The parenthetical
is exactly the assumption the page-floor change deleted, and `vmaddr_start` is a
variable that no longer exists; the geometry struct's `floor` replaced it.

The *conclusion* survives — the floor really does correspond to Burrow offset 0,
because the mapping is installed at the floor. Only its stated reason is false.
That is the sharper version of the defect, not the milder one: a reader checking
the justification finds it untrue and has no way to tell whether the claim above
it survived. The correcting comment sits four lines down and says the opposite in
plain terms, so the file contains both readings and does not mark which is
current. No behavioural risk; filed as task #178.

**The auxv-count assert pins the macro, not the writer.** It fixes
`EXEC_INIT_AUXV_COUNT` at 8 and its message enumerates the eight entries, but
nothing relates that number to how many entries `exec_fill_auxv` actually
writes. A ninth entry added without bumping the macro would overrun the
`AT_RANDOM` block in every process, and the assert would still pass. Correct
today; the coupling is a comment.

## Provenance

Born as the P3-Eb blob loader; grew argv at the pouch-stratumd boot chunk;
rebuilt as the file-backed path at [[arc-revenant]] R-4, which retired the
1 MiB whole-binary cap. #45 widened the shared path from text to all
non-writable segments. #107 fixed the I-cache span. The name and exe-path
stamps arrived with prowl and [[arc-vivarium]]. The fork arc added the
detached-target loader that closed the replace-in-place seam, the environment
vector in the frame, the page-floor mapping geometry, and sparse backing for
non-executable segments: [[chg-2026-08-15-exec-lineage]].

## Tests

`exec.setup_*` covers both frame shapes, the auxv block with and without a
covering phdr segment, multi-segment loads, the constraint rejects, a lifecycle
round-trip that proves every page returns to the allocator, and the #107
bss-tail sync. `exec.setup_from_spoor` drives the file-backed path against a
synthetic file with no filesystem behind it.

## Referenced by

[[moc-kernel-execution]] · [[inv-i36]] · [[sub-kernel-elf]] ·
[[sub-kernel-image]] · [[sub-kernel-fault]] · [[sub-kernel-proc]]

## exec's failure diagnostics: one unit, and the cap is GLOBAL (2026-08-18)

`exec_report_fail` / `exec_say` emit ONE `cons_diag_line` unit each, under a
single global cap (`EXEC_FAIL_MAX_REPORTS`), spent only on a line that landed.

Two things about that are load-bearing:

- **Not raw `uart_*`.** They were five lock-free calls apiece, which takes
  neither the console writer role nor the TX ring lock -- so they bypassed the
  extinction ring claim and could land bytes inside a peer's `EXTINCTION:`
  line. The old comment defended the raw loop as "non-blocking, must not
  acquire the console role"; `cons_diag_line` satisfies both (it never spins
  and takes no role), so that reason no longer selects `uart_puts`.
- **The cap is GLOBAL, never per-Proc.** Every `SYS_SPAWN_*` thunk reaches exec
  through here and spawn carries no capability gate, so an unprivileged Proc
  can drive this path in a loop with a malformed ELF. A per-Proc bound is
  re-armed by spawning again, which IS the attack. This was the worse of the
  two EL0-triggerable console sites -- it had no dedupe and no cap at all while
  the one that got fixed first had both.

**Coverage caveat, stated because it is easy to assume otherwise**: no test
drives this path. `test_exec.c` asserts the SUCCESS case (`rc == 0`) and no
boot log in the tree contains an `exec: ` line. The conversion is
compile-verified only.
