# Aux track roadmap

**As of 2026-08-16. Branch `aux-2`.** Read the branch off the worktree
(`git branch --show-current`), never off this line — on 2026-08-16 three
sources gave three answers (main's CLAUDE.md said `aux/userspace-apps`, this
header said `gfx-4 @ 45186b64`, the worktree was on `aux-2`), and `gfx-4` is a
separate live branch rather than a rename of anything. A branch name written
into a document is stale from the moment the track moves; the worktree cannot
be.

Supersedes the narrow `usr/apps/AUX-ROADMAP.md` (the old userspace-apps-only aux
agent). This track now owns the graphics arc, the Aurora environment, and the
VIVARIUM arc — **plus the kernel surfaces those arcs land on**, which is where
its recent work actually sits: measured over the 250 commits from `45186b64` to
`aux-2`, the touched tree is `kernel/test` (52 files), `docs/reference` (41),
`usr/ports` (28), `kernel/include` (27), `usr/lib` (19), `tools/interactive`
(13), `arch/arm64` (11). The notes / signals / job-control / PTY line
(aux#240..aux#255) is a kernel arc by any honest reading, and this track runs
its own full bar for it: the suite, the SMP gate, the pty spec set, and LS-CI.

The 2026-07-28 version of this header described the track as graphics-only and
went unrefreshed for three weeks while the work moved. Refresh this block
whenever the arc changes, not whenever it is convenient.

---

## Where the track stands

| Arc | State |
|---|---|
| **Graphics G-0..G-7** | **COMPLETE.** G-0..G-5 + PTY are already ancestors of `main`; G-6 (compositor) + G-7 (SDL/Quake) are on `gfx-4` awaiting the merge. |
| **Track B — Aurora config (cfg-1..cfg-5)** | **COMPLETE.** OSD + persistence + OSC session push + the apply-authority gate + runtime chords/gaps + baked font sizes, each audited. |
| **VIVARIUM (V-0..V-8)** | **STARTED.** V-0 scripture + V-1a (phenotype ledger + brand hint) landed; V-4 specced build-ready. |
| **Halcyon G-8/G-9** | Not started. The graphics endgame. |
| **Notes / job control / PTY (the kernel line, aux#240..)** | **ACTIVE.** See Stream 4 below. |

---

## Stream 1 — VIVARIUM (the active arc)

Phase 8's fourth pole: run unmodified Linux binaries. Design + build arc:
`docs/VIVARIUM.md` (all four decisions resolved — fork = **C** hybrid, build-now,
declare-not-infer branding, names adopted). Task **#62**.

**V-4a-0 + V-4a-0b LANDED** — the two kernel prerequisites the build surfaced.
V-4a was specced as pure userspace; ground-truthing the Tier-1 file set against the
tree before writing the crate found that **two of its entries had no native source
at all**:

- **`/proc/self/exe`** — `struct Proc` carried no executable identity whatsoever
  (the Image cache is qid-keyed, the text Burrow anonymous, `format_cmdline` a
  stub). Fixed by `Proc.exe_path` + `/proc/<pid>/exe` (§6.5), pinning the #66 `Path`
  the exec resolver already held.
- **`self` itself** — `srv_peer_info` reported `stripes` (an opaque tag with no
  userspace pid mapping) and **no pid**, so a 9P server could learn which
  *principal* was talking to it but never which *process*. Fixed by
  `srv_peer_info.pid` filling the reserved slot in place (§6.6).

Both were the pull-forward default, not scope creep: §6.2's rule is that the
diorama renders **only** from natively-reachable sources — that is what makes I-43
structural — so a missing source is a *kernel* gap by construction, never a licence
for the diorama to invent or accept an answer. §6.7 records the lesson and flags
`/proc/self/cwd` + `/proc/self/maps` (both V-4b) as the same shape.

**V-4a is DONE** — `usr/diorama` on the ptyfs skeleton, joey-spawned with
`MAY_POST_SERVICE`, selftest-before-serve, read-only, serving Tier-1 `/proc`. It
mounts at `/dio`; joey creates the mount point but deliberately does **not** mount
it, because `self` resolves to the connection's peer — i.e. the *mounter* — so a
shared mount would report joey to every reader. Each client mounts privately,
which is also how V-7 will set up a container.

**V-4b-1 and V-4b-2 are DONE**: `/self/cwd` and `/self/maps`, each with its kernel
source. Both confirmed §6.7's "budget these as kernel + userspace", but neither for
the predicted reason — `cwd` needed no new kernel *state* (the Territory has
carried `dot_path` since LS-4), and `maps` inherited its lock-order argument from
`devproc_mem_walk_cb`, which had already established and audited
`g_proc_table_lock → vma_lock`. The refined lesson (§6.7): **grep for an existing
accessor and an existing lock-order precedent before budgeting either.**

`maps` also forced the first real "which layer speaks Linux" decision, settled in
§6.8: the kernel emits a Thylacine-native table, the diorama translates. Anything
else is phenotype leaking into the kernel.

**V-4b-3 is DONE**: the numeric `/proc/<pid>/…` dirs, the root pid enumeration,
and `sys/kernel/{ostype,osrelease,version,hostname}`. **Pure userspace this time —
kernel byte-unchanged** — and the reason is worth carrying forward: `/self` was
*always* a per-pid render with the pid supplied by the connection's peer rather
than by the path, so the pid had been a parameter from the start and per-pid was a
generalization, not a new mechanism.

Two design findings landed with it:

- **§6.9 — the fourth source.** `sys/kernel/ostype` reformats nothing; the answer
  *is* the phenotype. That is not a §6.2 violation (a constant carries no
  information about the system, so there is nothing to leak), but the distinction
  had to be written down or it becomes the loophole every later file is argued
  through. The rule: *derived from kernel state needs a native source; a constant
  declaring which ABI you are looking at is the phenotype speaking about itself.*
- **§7.1 — the V-7 pid-visibility obligation.** The diorama's pid view matches
  native `/proc`'s exactly (all-pids, Plan 9 posture), so there is no new
  authority — but a contained Proc seeing every host pid is a leak, and that leak
  is in native `/proc` + `/ctl/procs` first. Scoping the diorama alone would be
  theatre. Owed at V-7, against the native surface.

**V-4b-4 is DONE** — the *shape* half (§6.11), which turned out bigger than the
one file that named it (task #66). §6.2 governs where a value comes from; V-4b-4 is
the second question: **in what shape does the consumer expect to read it?** Linux
serves `/proc/{self,<pid>}/{exe,cwd}` as symlinks, we serve regular files whose
contents are the path, and the consumer (`getMainExecutable`) calls `readlink`
specifically. Thylacine has no symlink surface to grow, so the translation went
into the **phenotype** — the pouch boundary-line — exactly as V-4b-3 predicted.

The finding that made it larger: the seam had parked `readlink` at the `ENOSYS`
sentinel, and on a symlink-free system that is the *wrong* answer rather than an
absent one. musl's `realpath()` is a pure userspace resolver (it does **not** use
`/proc/self/fd`, contrary to the note the LLVM fork's patch carries) that calls
`readlink()` per path prefix and treats anything but `EINVAL` as fatal — so
**`realpath()` was broken for every path on the system, for every ported program**.
The truthful `EINVAL` repairs it whole with no `realpath` patch. Revert-probed:
with the general arm back at `ENOSYS`, `realpath("/proc/./")` fails in-guest with
errno 38, while `realpath("/")` still passes (no components to walk).

It also surfaced a kernel gap and a family behind it (task #67): the `/proc` apex
answered `-1` from `stat_native`, i.e. `EIO`, so `stat("/proc")` failed — fixed
here (the apex is a real directory and now stats as one). The rest was left tracked
because each member needed a per-qid posture decision across a whole Dev.

The LLVM fork delta is now *available* to drop, **not dropped**: that needs an LLVM
rebuild + the Clade gates, which belong to the track owning that fork. Recording it
as available rather than done is the correction V-4b-3 made to V-4a, applied to
itself.

**V-4b-5 is DONE** — the synthetic-Dev stat family (§6.12; task #67), three gaps
closed together because they are one question asked of three Devs: *what does this
synthetic object claim to be?*

`/ctl` and `/env` had **no `stat_native` slot at all**, so `stat()` on the
directory, `fstat` on any fd beneath it, `lseek(SEEK_END)`, and — by the §6.11
mechanism — `realpath()` of anything underneath, all failed with `EIO`. Both now
answer, and they answer *differently about size*, which is the interesting part: a
`/ctl` file is generated at read time (so a size measured at `stat` is stale before
the read, and 0 is the honest answer — Linux's `/proc/meminfo` convention), while
an `/env` value is stored (so its size is real, and `SEEK_END` lands on it).

devproc's modes carried **no file-type bits**, so `S_ISDIR("/proc/<pid>")` was
false and every POSIX walker that classifies before descending stopped there.

And a fourth found while writing the prover's regression rather than while
designing the fix: `parse_decimal` accepted **leading zeros**, so one Proc answered
to unboundedly many names — which meant native `/proc` and the diorama disagreed
about which paths exist, and coherence between the two is the point of §6.2. Linux
rejects them for the same reason; `"0"` stays legal (kproc).

**Self-audit caught the one that mattered most, before it shipped.** Reporting a
size for `/env` entries has a consequence beyond `fstat`: `exec_resolve_from_
namespace` gates only on `dev->read` and a non-zero size, so a real size makes
`exec("/env/FOO")` reach the REVENANT Image cache — keyed on `(dc, devno,
qid_path, …)`. Every other Dev's qid namespace is global, so a static `devno == 0`
still leaves that pair unique; **devenv's is per-Proc** (ids restart at 1 in every
`Env`), so two Procs' unrelated variables both reported `(0, 1)` and the cache
would serve one Proc another's bytes — an I-1 leak out of the one device whose
premise is that a Proc sees only its own environment. Fixed with a per-`Env`
`devno` stamped onto the walked Spoor. The identity was equally wrong before, but
nothing had asked: **reporting a field that was never reported is a claim, and the
claim has to be true before the report is added.**

Revert-probed in three boots: all three kernel tests fail on pre-fix code at
exactly the reverted assertion; the in-guest prover reaches its `/ctl`, `/env` and
`S_ISDIR` legs before failing at the padded-pid one; and dropping the `devno` stamp
fails `devenv.stat_native_shapes` at "the walked Spoor carries it".

**V-4b-6 is DONE** — `/self/environ` (§6.13; task #65 part 1). A new kernel
source `/proc/<pid>/environ` renders the per-Proc `Env` as Linux's flat
`NAME=VALUE\0` block. Two things the §6.7 prediction ("a renderer over an existing
group") could not have known from outside:

* it had to be **offset-aware** rather than format-and-slice. An `Env` holds up to
  64 x 4096 bytes against devproc's 2 KiB buffer, and the failure mode of
  format-and-slice is *silently dropping environment variables* — which reads to
  the consumer as never-set, not as an error. One call clamps at 8 KiB (the copy
  runs IRQs-off); the file does not.
* it is the **first devproc info file with a real read gate** — owner-or-
  `CAP_HOSTOWNER`, because `/env` is self-only by construction so nothing else
  discloses a peer's environment, and environment variables carry secrets by
  convention. Same posture Linux takes.

**The self-audit find:** a gate that keys on the READER cuts both ways, and the
second way is a leak. The diorama is SYSTEM, so the kernel would let it read any
SYSTEM Proc's environ and hand those bytes to a client of any principal — who
natively would have been denied. `/srv` is the shared boot registry re-grafted
post-pivot, so a user session can mount the diorama: reachable, not theoretical.
Fixed by serving environ under `/self` ONLY (sound by construction: the target is
the connection's own peer). Replicating the owner check against `peer.principal_id`
was rejected — it makes a component whose design property is having no policy into
a policy point, for a file no v1.0 consumer reads. The generalized rule, recorded
for V-4c/V-7: *before proxying a file, ask not only "could the client read this
natively" but "could the client read this natively FOR THIS TARGET".*

Revert-probed in four boots: all four kernel-side claims at once (gate/clamp/skip/
windowing) — each test fails at exactly its reverted assertion; the gate alone
(which also fails BOTH sched-gate tests, proving the shared-predicate extraction is
live); the diorama trim alone (selftest FAIL → the server refuses to post, boot-
visible); and the cross-principal fix alone (same boot-fatal signal). An earlier
probe attempt landed on `devproc_kill_authorized` instead — the pattern matched an
identical line — which proved the I-26 kill gate covered but left mine unprobed
until re-run.

**V-4b is CLOSED.** Its last two files are dispositioned rather than built, each
with its evidence written down (§6.14, §6.10) so that neither is a silent
omission:

* **`auxv` — weighed, and deliberately not built.** Zero live readers in the
  tree: every consumer takes the stack path (`getauxval`, or a hand-walk of the
  `_start` frame), and the one file containing the literal string — SDL2's
  `SDL_cpuinfo.c` — is compiled out twice over on aarch64 (`!defined(__arm__)`,
  and `HAVE_GETAUXVAL 1`). But an in-tree grep is weak evidence about a *compat*
  surface, so the argument that carries is structural: **auxv on the stack is a
  prerequisite of V-7, not an optional extra** — a Linux ELF bootstraps out of
  `AT_PHDR`/`AT_ENTRY`, so `viv` cannot launch a foreign binary without building
  one, and `/proc/self/auxv` is the fallback for a thread that never received an
  entry frame (`dlopen`'d into a foreign host; a sanitizer off the main path).
  That is the named trigger. Recorded with it: if it is ever built it must be a
  *retained kernel copy* (Linux's `mm->saved_auxv`), never a reconstruction —
  `AT_RANDOM` and `AT_PHDR` are per-exec pointers into the process's own stack and
  image, so a recomputed answer is wrong rather than stale, and a consumer
  dereferences `AT_RANDOM`.
* **`fd` — blocked on #66c**, the #926 handle-table lifetime restructure: a
  kernel chunk, not a Vivarium one. The diorama must not route around it.

**V-4c is RESCOPED before any of it was written** (§6.15, scripture-first —
the design-conversation pattern, landed as docs with no code). Ground-truthing
Tier 3 found that **§6's `/dev` bullet contradicts §6.1**: the diorama is
read-only (`h_write` → `E_PERM`, unconditional), and `/dev/null` is *defined* by
accepting writes. Native devdev already implements `null`/`zero`/`full`/`random`/
`urandom` correctly — `/dev/full` even fails writes, the right shape — so routing
them through the diorama would take files that work today and break them. The
rule that generalizes, recorded beside §6.2's: **a re-presentation that loses a
capability the native tree already has is a downgrade wearing a compatibility
label.**

A container's `/dev` is composed by **bind**, in its own territory — the same
mechanism `viv` uses at V-7 and joey already uses at boot. The entries devdev
lacks land where the question already lives: `/dev/ptmx` is **already done** in
the phenotype (PTY-3's redirect, whose patch says in its own words that
"`/dev/ptmx` is a compat symlink Thylacine cannot provide"); `/dev/std{in,out,err}`
and `/dev/fd/N` are `dup(N)` at the boundary-line; `/dev/tty` is a bind of the
container's own pts, which `viv` knows and a server would have to guess.

So **V-4c = a minimal `/sys` + the per-container mount wiring** (promoted from
afterthought to the substantive half) **+ the two Tier-1 stragglers** `cpuinfo`
and `stat` **+ the arc's focused audit**, which owes a close on §6.13's
deputy-authority rule as well as §6.2's no-new-authority property and §6.12's
file-identity claim.

Two supporting findings, both from the same grep pass:

* `/sys` is thin — the entire tree holds exactly **one** `/sys` path (SDL2's
  cache-line-size read), and it is a *soft* read whose failure is benign. But it
  is a **second tree**, not a subdirectory: the diorama's existing `N_SYS` nodes
  are `/proc/sys/kernel/…`, a different thing sharing a name. One server exporting
  two trees is `Tattach` with a different `aname` (Stratum's `ds:<name>` is the
  in-tree precedent), and `h_attach` currently **ignores `aname`** and always
  lands on `N_ROOT` — so that dispatch is the real work, and the per-container
  mount wiring wants it anyway since V-7 attaches twice per container.
  > **Half of that was wrong, and V-4c-1 corrected it** (§6.16). `/sys` is
  > indeed a second tree — but the aname route is **closed**, not merely harder:
  > `devsrv_open_connect` attaches a 9P-mode service with a hardcoded *empty*
  > aname, and `SYS_ATTACH_9P_SRV` (which does carry one) is byte-mode-gated and
  > rejects a 9P-mode conn for a sound reason. Serving `/sys` that way would
  > need a **new kernel ABI**. The answer was the one §6.15 had already chosen
  > for `/dev` one paragraph earlier: **bind**. `SYS_MOUNT` takes any readable
  > Spoor, subdirectory included, so the diorama serves ONE tree whose root is
  > the *world* with `proc` and `sys` as siblings — **no kernel change at all**.
  > Recurring lesson: ground-truth the mechanism before the table entry.
* `cpuinfo` and `stat` are Tier 1 by §6.3 but have **no in-guest consumer today**
  (`config.guess` is a host script; SDL2's is the `__ANDROID__` branch), and both
  are only *partly* sourceable: `MIDR_EL1` is not EL0-readable at all, and
  `ctxt`/`intr`/`processes` have no native source. That makes them a per-*field*
  §6.7 question with a third answer the doc did not have: for a line-parsed file,
  omitting a line and fabricating one are different failures, and "report 0" is
  fabrication with a plausible face.

**V-4c-1 LANDED** — one server, two trees, by bind (§6.16). The diorama root is
now the synthetic *world*; today's content moved to `/proc/…` and `/sys` joined it
as a sibling, carrying `devices/system/cpu/{online,possible,present}` plus one
`cpuN` dir per CPU. Every byte is sourced from `/ctl/cpu`, whose `cpus:` header is
the *declared* set and whose `offline` row marker (prowl-5 F2) is exactly Linux's
present-vs-online distinction — a gift from devctl having had to make that same
distinction for prowl. The prover reads the cpulists AND **binds `/dio/sys` at a
second path**, reading identical bytes through the new name: the composition V-7
depends on, proven rather than assumed. Both new selftest legs are revert-probed
(each fails the boot when broken). 1215/1215 · boot OK · 0 EXT.

Two things the build settled that the scripture had left open:

* **A bound subtree is genuinely sealed.** The worry was `..` — the server still
  records `/sys`'s parent as the world root, so could a container climb out into
  `/proc`? No: `stalk` resolves `..` by *popping its own trail* and never sends a
  `Twalk("..")`, so `<mount>/..` lands on the mount point's parent in the
  **client's** namespace. Same property that contains `..` at `root_spoor` for
  I-28, doing double duty.
* **The per-field question now has a THIRD instance** with an identical shape.
  `cpuN/cache/index0/coherency_line_size` reads `CTR_EL0`, which is EL0-trapped
  exactly as `MIDR_EL1` is (`SCTLR_EL1.UCT` clear in `INIT_SCTLR_EL1_MMU_OFF`).
  So `cpuinfo`'s MIDR, `stat`'s ctxt/intr/processes, and the `cpuN` contents all
  await **one** decision — omit, or give the kernel a source — deliberately made
  once rather than piecemeal. That is **V-4c-2**, with the per-container mount
  wiring; **V-4c-3** is the arc's owed focused audit (a merge gate).

**V-4c-2 groundwork (researched; it mostly dissolves the fork).** Grepping for
the sources first turns "one decision" into four exposures and one real question:

| field | source status (verified) | cost |
|---|---|---|
| `stat: processes` | **exists** — `g_next_pid` (`proc.c:386`) is a monotonic atomic from 1, bumped per `proc_alloc`, so `−1` is exactly Linux's forks-since-boot | expose |
| `stat: intr` | **exists but PARTIAL** — `kobj_irq_total_fires()` (`irqfwd.h:114`) counts only *forwarded* userspace-driver IRQs; timer/UART/kernel-internal are not counted | expose + widen, or label honestly |
| `stat: ctxt` | **material exists** — prowl's per-thread `nsched` (`thread.h:460`); no global aggregate | aggregate |
| `cpuN/cache` | **exists in-kernel** — `CTR_EL0` read at `mmu.c:962`/`:982` for I-cache strides, simply unexposed | expose |
| `cpuinfo` MIDR | **none** — `grep MIDR` over `kernel/` + `arch/` returns nothing | new kernel read + surface |

`intr` is the trap, and it is a shape the original note did not anticipate: a
source *exists*, so the danger is no longer an invented zero but a **real number
that means something narrower than the field it fills** — fabrication with a
plausible face arriving by the back door. Either widen the counter or do not
call it `intr`.

**V-4c-2a DECIDED** (§6.17, scripture-first, no code). Two things the research
found that the table above had not:

* **`cpuinfo`'s `Features` line is already sourced** — `g_hw_features.linux_hwcap`
  carries the arm64 *uapi* bit numbers for the exec auxv, so the CF-4 AT_HWCAP
  chunk already paid for the field that capability-detecting consumers actually
  parse. Only the MIDR *identity* quartet is unsourced.
* **A sixth instance nobody had counted**: `stat`'s `cpu`/`cpuN` jiffies line. No
  EL0-vs-EL1 time accounting exists anywhere in the tree, and — unlike every other
  field — it **cannot be omitted**, because the columns are positional, so a
  missing middle column is a wrong answer rather than an absent one. All non-idle
  time is reported as `system` under a *stated premise* (the pattern `maps`
  already uses); utilization, which is what essentially every consumer computes,
  is exactly right either way.

The rule covering all seven: **give the kernel a source, per-CPU, in the kernel's
own shape — and omit only what has no truth to tell.** Per-CPU is not a detail: it
makes both new counters free (each CPU stores to a line it already owns), it is
how Linux accounts them, and it is the only form that stays correct on a
heterogeneous board — where `MIDR_EL1` genuinely differs per core, which is why
Linux prints a per-`processor` block at all.

The exposures land as **columns on `/ctl/cpu`** (whose row already *is* the
kernel's native per-CPU description — a `/proc`-shaped kernel file would be §6.8's
phenotype leaking inward) plus one global scalar on `/ctl/sched`. Appending is
safe: prowl's parser takes three tokens positionally and ignores the rest, and an
`offline` row stays two tokens and is still skipped — which is also right for the
diorama, since Linux lists only online CPUs as `cpuN`. So no new `/ctl` file, and
the `kernel-base` precedent (`CAP_HOSTOWNER`-gated after the #57a F1 KASLR-slide
leak) is not engaged; the added values are hardware description and event counts,
carrying no address or layout secret.

**V-4c-2b LANDED** — the kernel sources. Two per-CPU counters (`gic_dispatch` for
`intr`, the `sched()` switch chokepoint for `ctxt`), a per-CPU `hw_cpu_ident`
recorded at bring-up (MIDR + the CTR_EL0 line size), and the hwcap word — all
surfaced as `/ctl/cpu` columns plus one `/ctl/sched` scalar. 1216/1216.

**And a correction the build produced**: the table above named `g_next_pid − 1`
for `processes`. Wrong — **`proc_total_created()` already existed** (`proc.c:599`,
a dedicated `u64`, ~10 tests), and is strictly better than the derivation. The
fifth exposure dissolved to *zero kernel code*. That is §6.7's own lesson
recurring, and the instructive part is how it slipped past a pass explicitly doing
that research: the grep asked **where is this value produced**, found `g_next_pid`,
and stopped — it never asked **is this value already published**. Producer and
accessor are different searches, and finding the first is exactly what stops you
running the second.

**V-4c-2c LANDED** — the diorama half: `/proc/stat`, `/proc/cpuinfo`, and the
`cpuN/cache/index0/coherency_line_size` leaf that lifts V-4c-1's deliberately-
empty `cpuN` dir. The cpu qid gained a *kind* above the index, so `cpu_qid(n)`
stays bit-identical and the subtree is an extension, not a renumbering.

Two things the build settled. First, `walk_child` already had an
`is_cpu_node(dir)` arm that returned early — a second arm appended lower in the
same function would have been **unreachable**, and the cache subtree would have
silently not resolved. Second, that existing arm hardcoded `..` → `.../system/cpu`,
which was correct only while `cpuN` was a leaf dir; left alone, `cache/..` would
have skipped a level. Both are the same shape: *V-4c-1's code was right for
V-4c-1's tree, and extending the tree makes previously-correct shortcuts wrong.*

**V-4c-2 is COMPLETE. V-4c-3 — the arc's focused audit — is next, and is a merge
gate**: the two new counters sit on audit-trigger surfaces (the scheduler switch
chokepoint and GIC dispatch), V-4b-1..6 and V-4c all landed on self-audit only,
and devproc/devenv are ARCH §25.4 trigger surfaces.

**V-1b is merge-ordered, not blocked-forever**: it wants `kernel/exec.c` +
`kernel/syscall.c`, which CL-4 also touched. Land `clade-cl4-wip` → `main` → then
`gfx-4` → `main`, and V-1b is clear. See `docs/MERGE-gfx-4.md`.

Later: V-2 (the total-and-stateless translation table) · V-3 (the supervisor
channel — **spec-first**, `specs/phenotype.tla`, a new wait/wake on the death
lineage) · V-5 sockets → `/net` (gate: `curl` fetches a URL) · V-6 signals
(audit-bearing) · V-7 `viv` (gate: an Alpine shell) · V-8 audit on I-43.

## Stream 2 — Halcyon (G-8/G-9), the graphics endgame

The last stage of the arc and of Phase 10. Four parts to G-8:

1. **The native TTF rasterizer** (`no_std`, AA + hinting). The natural first
   sub-chunk: self-contained, testable, and it is simultaneously the user's
   "Apple-quality fonts" ask *and* the prerequisite for the Acme tag bar. Scripture
   already calls it "foundational, not a nicety" (`TAPESTRY.md §14`).
2. The transcript pane (Helix-modal, selection-first scrollback).
3. Inline graphical surfaces in the transcript.
4. `halcyon.rc` (the policy layer).

Then **G-9**: Aurora-terminals-as-panes, video player, image display, the Halcyon
audit + `docs/HALCYON.md`.

**Recorded direction — the Acme tag bar** (`TAPESTRY.md §14`, from the user's i3
find): render the pane `tag` as text in the Stacked/Tabbed strip (today glyph-free
colored segments, per D7). The strip becomes a thin **renderer-drawn title surface**
so the compositor never grows a glyph path; the richer end state is Acme's
**executable tag line** — the title bar as a live command surface.

## Stream 3 — polish + debt (small, satisfying, interleavable)

- **cfg-6 — the `letterbox`/zoom-policy OSD row.** A standing user ask; small
  (`Comp::letterbox()` exists, the Display section already shows it info-only —
  needs a config key + a live row like Mode).
- **#39 — the Aurora host-test harness** (the netd-style
  `cfg_attr(not(test), no_std)` refactor) so the dormant vt/render/osd/config
  regressions actually run in CI. Highest leverage of the debt items.
- **#57 — the live-display (cocoa) border under-paint + lingering dead pane.**
  User-observed, no headless repro (the #31 class) — needs eyes on a live window.
- #43 (synthetic key-release on focus change — stuck key) · #44 (4K weave cap +
  multi-point pixel asserts) · #32 (`ls /srv` → "I/O error"; devsrv has no
  `.readdir`) · #13 (per-pts ownership + 0600).

---

## Stream 4 — notes / job control / PTY (the kernel line)

The line the header names: the EL0-return tail's note dispatch, the STOP
class, the tty family, the pts job-control seam, and the tests that construct
their states. It runs the full bar (suite + SMP gate + pty specs + LS-CI).

**Landed (newest first, 2026-08-17 back to aux#240):**

- *(pending)* -- the `c8ab2744` audit close (Fable 5 round: 0 P0 / 1 P1 / 1 P2 /
  2 P3, ALL pre-existing three lines above the audited arm). F1 [P1]: both
  class scans (terminate + STOP) now gate every hit per note on
  `notes_proc_default_applies(p, name)` -- the terminate scan was phenotype-
  blind and any-index, so a Linux guest died of a CAUGHT `tty:hup`/`interrupt`
  queued behind a `SIG_DFL` candidate. F2 [P2]: a `SIG_DFL` `pipe` on
  PHENO_LINUX was consumed by NO arm (no native latch, #237) and sat as the
  dispatcher candidate for life; the phenotype branch now `exits()` on a
  `SIG_DFL` terminate-default candidate (`viv_signote_default_is_terminate`).
  F3: the consumer's dead drain call deleted; F4: three "never queued"
  contract sentences reworded. Unit `notes.class_scans_read_phenotype_sigtab`
  + L-6c legs J/K/L -- whose POSITIVE CONTROL (K) caught a second bug on its
  first boot: viv `fcntl(F_DUPFD)` answered EMFILE for a CLOSED fd, ash's
  `N>&M` probe aborted every `3>&1`, and J/L passed vacuously on an empty
  capture. Fixed (EBADF/EMFILE split; `vivarium.fcntl_dupfd_errnos`).
- `4525023a` -- the change-of-watch pair + Stop hook + launcher imported from
  main (aux's first self-compaction followed).
- `11173762` -- LS-CI failure-time state probe (`::lc_fail_probe`); pty-4 arms
  it so its next burned retry says INPUT vs OUTPUT vs lost-^Z from the log.
- `3a7f50f1` -- `mask note 'tty:*'` masked NOTHING (parser had no tty arm);
  prefix -> `NoteClass::Tty`; u-job-test 15c pins it + reads the kernel mask.
- `ffb8f0ab` -- `notes.masked_susp_stops_at_delivery` observes -> tears down
  -> asserts (an early-return assertion left ALIVE linked Procs and hung the
  next test's `wait_pid` loop).
- `c8ab2744` -- the deferred-^Z stop arm was reached by NOTHING (an
  unconstructed state); `/susp-mask-child` + jc-probe `maskstop` construct it;
  reading the arm found a P1 (decided class-filtered, consumed class-blind: a
  queued `child_exit` destroyed) -> `notes_stop_dequeue_locked`.
- aux#254 (sigtab UAF at exec), aux#253 (self-kill on a full queue), aux#251 +
  aux#252 (phenotype-blind catchability gate; the STOP class had no
  delivery-time reader), aux#247, aux#240 (`susp_stop_armed` freshness).

**Open, in order:**

1. **The tail's delivery-time SIG_IGN discard arm is reached by nothing** --
   the second unconstructed state found by sweeping for the class
   (`bug_delivery_time_sigign_discard_uncovered.md`); its own chunk (same
   file as the close above).
2. **pty-4's burned retry** -- instrumented, not diagnosed; wait for the next
   miss and read `build/ls-ci-pty-4.attemptN.steps`.
3. **#237 stays open and is now sharper**: the phenotype answers SIG_DFL
   SIGPIPE for its own Procs; the NATIVE `pipe` note still carries no latch,
   so a native program that writes to a closed pipe with no handler and no fd
   reader keeps a stranded `pipe` note -- a Plan 9 ABI decision (signoff).

---

## Recommended sequencing

1. **V-4a** now — unblocked, specced, has a real consumer.
2. **The merge** when the main track is ready (`docs/MERGE-gfx-4.md`), which
   unlocks **V-1b**.
3. **cfg-6** as a warm-up whenever a short chunk fits.
4. **G-8's TTF rasterizer** as the next *big* pick after Vivarium's foundation is
   in — or ahead of it if the user prefers the visible win.

Vivarium and Halcyon are both large and both sit in the endgame beside `v1.0-rc.1`
(`ROADMAP §11.5` keeps the fallback: v1.0-rc ships without either if neither
converges). They can proceed in either order; Vivarium is the one currently moving.

---

## Coordination with the main track

**Merge round 1 is DONE** (2026-07-27): `gfx-4` merged into local `main` at
`15edb01e` + the `de451566` pouch O_APPEND restore. The pouch series collision the
handoff predicted was real and was resolved there. **Not pushed** — `origin/main`
is still `b0bf63f2`.

Two things carry forward, both in `docs/MERGE-gfx-4.md` (rewritten as a round-2
handoff):

- **Five `gfx-4` commits landed after the merge point** (`b7df5b21..5af01124`),
  including both VIVARIUM kernel prerequisites. Round 1's analysis said
  `struct Proc` would auto-merge because V-1a's `phenotype` fit the tail pad — that
  is no longer true: V-4a-0 grows it 352 -> 360. The size assert is the drift
  detector, so a bad merge fails the build loudly.
- **Clade CL-4 never landed.** The handoff advised `clade-cl4-wip` -> `main`
  first; in the event `gfx-4` went first, so CL-4's four commits still merge on top
  of the gfx-4 kernel changes. An inconvenience, not a defect — but it means
  `kernel/syscall.c` now has a three-way overlap (main-via-gfx-4, CL-4, and
  V-4a-0b's small `pid` out-param thread-through).

**Consequence for V-1b**: it is *still* best sequenced after CL-4 lands, for the
original reason — CL-4 touches `kernel/elf.c` + `kernel/syscall.c`, which is
exactly where V-1b's syscall-entry phenotype branch goes. V-4a (the diorama crate)
is pure userspace and has no such constraint, which is another reason to do it
first.

- The aux track has touched kernel files (cfg-3's `srv_peer` stamp, V-1a's
  `Proc.phenotype`, V-4a-0's `exe_path`, V-4a-0b's `srv_peer_info.pid`); it is not
  `usr/`-only. Check the main worktree's dirty state before editing shared kernel
  files.
