# Merge handoff: `gfx-4` -> `main`

**For the MAIN agent, when the Clade CL-4 WIP settles.** Written by the aux/gfx
agent 2026-07-23 from a **trial merge that was then aborted** — the conflicts below
are measured, not predicted. `gfx-4` is clean and green at its pushed tip; nothing
is half-merged.

- `gfx-4` tip: **`7b46820c`** (pushed, both mirrors)
- `main` tip at time of writing: `b0bf63f2` (Clade CL-3b)
- **63 commits** on `gfx-4` not in `main`; `gfx-4` is **34 behind** `main`
- Prior aux arcs are ALREADY in `main` (verified ancestors): `gfx-1`, `gfx-2`,
  `gfx-3`, `pty-1`, `pty-followups`. Only `gfx-4` (and the dormant
  `aux/userspace-apps`, 8 commits) is outstanding.

---

## 1. What `gfx-4` carries

| Arc | Contents |
|---|---|
| **Track B — the Aurora config subsystem, COMPLETE** | cfg-1 (the F10 Turbo-Vision OSD) · cfg-2a/2b (system-tier persistence + the OSC 7770 session push + `aurora-push`) · **cfg-3** (the apply-authority gate: `SYS_SRV_PEER` stamps `SRV_PEER_FLAG_CONSOLE_RENDERER` + the `mode W H` verb) · **cfg-4** (runtime chords + gaps) · **cfg-5** (several baked Cornucopia font sizes: `font-size`, `atlas-{9,8,7,6}.bin`) · the max-resolution **brick fix** (set_mode pre-flights the real weave; aurora self-heals a persisted bad mode) |
| **G-7 close** | G-7c (pointer input) + G-7d (audit close, present-rate measure) |
| **Kernel** | #58 (namespace exec sweep) · #57 · debug-fs bits · **VIVARIUM V-1a** (`Proc.phenotype` @347 + `elf_brand_hint`) |
| **Scripture** | `docs/VIVARIUM.md` (the Linux-binary-compat design pass, all 4 decisions resolved) · the TAPESTRY Acme-tag-bar note · ARCH §11.5/§11.6 corrections · ROADMAP §9 pointer |

Gates at the tip: suite **1198/1198**, boot OK, 0 EXTINCTION, `ls-gfx-font` +
`ls-gfx-panes` PASS, full SMP gate 40/40 on the cfg-3 kernel delta.

---

## 2. THE HEADLINE: the pouch patch series collides

**Both tracks independently numbered pouch patches 0024/0025/0026.**

| | `gfx-4` | `main` |
|---|---|---|
| 0024 | `0024-pouch-fopen-create.patch` | `0024-pouch-fs-process-wires.patch` |
| 0025 | `0025-pouch-net-nonblock.patch` | `0025-pouch-env.patch` |
| 0026 | `0026-pouch-cons-winsize.patch` | `0026-pouch-process.patch` |
| 0027 | — | `0027-pouch-remove.patch` |

**Recommended resolution: renumber the THREE `gfx-4` patches to `0028`/`0029`/`0030`**
(after main's stack). Rationale: main's is the active Clade arc and the larger
stack; the aux side is the cheaper thing to move. Do NOT renumber main's.

**But renumbering is not sufficient on its own — check for overlap first.** Both
stacks patch musl, and these two in particular are suspicious:

- main's `0024-pouch-fs-process-wires` reportedly includes an
  `open(O_CREAT) -> SYS_WALK_CREATE` arm.
- `gfx-4`'s `0024-pouch-fopen-create` is the #50 create-mode `fopen`/`tmpfile` fix,
  which lives on exactly that surface.

So the aux patch may be **wholly or partly subsumed** by main's. Please diff the two
before stacking them; if main's already covers it, DROP `gfx-4`'s rather than
renumbering it (and note that task #50 is thereby closed by main's work). If they
are complementary, renumber and re-verify the stack applies in order.

The `pouch-hello-fopen` prover binary on `gfx-4` is the regression that tells you
which it is — it must still pass after the merge either way.

---

## 3. The other four conflicts (all mechanical)

Measured against `main@b0bf63f2`:

| File | Size | Resolution |
|---|---|---|
| `tools/build.sh` | 4 hunks | **Pure union.** Take BOTH sides of each list. `gfx-4` adds `aurora-push` (usr_rs_bins), `pouch-hello-fopen` (pouch_bins + the prog loop), and the `quake-host)` dispatch case; `main` adds `prowl`, `pouch-hello-{fs,env,spawn,cxx}`, `make`, and the `gnumake)`/`libcxx)` cases. Keep every entry from both. |
| `usr/lib/pouch/patches/series` | 1 hunk | Union, in the §2 order (main's 0024-0027 first, then the renumbered aux 0028-0030 — minus any dropped per §2). |
| `docs/reference/78-pouch.md` | ~322 lines | The big one. Both tracks documented their pouch patches heavily. Union by section — no logical conflict, just adjacent prose. |
| `kernel/devproc.c` | ~17 lines | Small; both tracks touched it (aux: debug-fs; main: prowl). |
| `docs/reference/111-cons.md` | ~12 lines | Small; both documented cons (aux: #55 winsize; main: prowl/cons). |

**Everything else auto-merged clean** — notably `kernel/proc.c`,
`kernel/syscall.c`, `kernel/include/thylacine/proc.h`, `kernel/cons.c`,
`kernel/test/test.c`, `docs/ARCHITECTURE.md`, `docs/ROADMAP.md`, `CLAUDE.md`,
`usr/joey/joey.c`. In particular **V-1a's `Proc.phenotype` @347 auto-merges** — the
`_Static_assert` will catch it instantly if that ever stops being true.

---

## 4. Two small follow-ups owed on the merged result

1. **The `elf.c` brand-hint comment.** `kernel/elf.c::elf_brand_hint`'s header says
   Clade's `ELFOSABI_GNU(3)` output "is why `elf.c:77` was widened to accept 3."
   That widening is currently only in the **uncommitted** CL-4 working tree, not in
   `main` — so on the merged result the parenthetical describes something that may
   not have landed yet. The *reasoning* is unaffected (the hint deliberately never
   reads `EI_OSABI`); just reword the aside to match whatever is true post-merge.
2. **VIVARIUM §12.2 is addressed to you.** A positive native brand
   (a `.note.thylacine`, emitted by Clade and `pouch-ld`) is what would let
   `elf_brand_hint` speak in both directions instead of only recognising Linux. It
   is cheap to add while Clade is being built, and it is recorded as the main
   track's seam. Not required for v1.0 — the `PHENO_NATIVE` default is safe without
   it.

---

## 5. Gates to run on the merged result

```
tools/build.sh all           # sysroot rebuilds -- the pouch stack changed
tools/test.sh                # expect >= 1198 PASS, 0 FAIL, boot OK, 0 EXTINCTION
tools/ci-smp-gate.sh         # kernel delta on both sides
tools/test-interactive.sh ls-gfx ls-gfx-panes ls-gfx-mode ls-gfx-font
```

Plus main's own Clade gates (`/clade` on-device, the pouch prover ladder incl.
`pouch-hello-fopen`).

`ls-gfx-chords` SKIPs on a default image by design (it is env-gated on
`THYLACINE_AURORA_CFG4=1`) — a SKIP is a pass, not a miss.

---

## 6. What NOT to do

- **Do not renumber main's pouch patches** to make room; move the aux three.
- **Do not merge across a dirty main worktree.** *(STATUS UPDATE 2026-07-23, later
  the same day: this has CLEARED — CL-4 is committed at `7cfcabce` on
  `clade-cl4-wip`, and `kernel/exec.c` + `kernel/syscall.c` are clean again. Note
  the CL-4 work is on its own branch, not yet in `main`, so the natural order is
  `clade-cl4-wip` -> `main` FIRST, then `gfx-4` -> `main`. CL-4 also touched
  `kernel/elf.c` and `kernel/syscall.c`, which VIVARIUM V-1b will touch next — so
  landing CL-4 before the gfx-4 merge is what keeps V-1b conflict-free.)*
- **Do not resolve `docs/reference/78-pouch.md` by taking one side.** Both tracks'
  documentation is real; it is a union, not a choice.

---

## 7. After the merge — the aux track's next step

VIVARIUM **V-1b** (the syscall-entry phenotype branch + the exec-time set) is
blocked purely on `kernel/exec.c` + `kernel/syscall.c` being free. Once the merge
lands, the aux track picks it up. V-1a deliberately shipped *without* those two
files touched so this handoff would be clean — nothing reads `Proc.phenotype` yet,
so the native path is byte-unchanged and the merge risk was kept to zero.

Design + build arc: `docs/VIVARIUM.md` (V-0..V-8). Tracked as task #62.
