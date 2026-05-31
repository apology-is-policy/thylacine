# Halls of Extinction — crash-dump design (binding scripture)

When the kernel dies, that boot's lineage is extinct (see `kernel/extinction.c`).
**The Halls of Extinction** record the fallen: at the fatal moment, capture
everything that is hard or impossible to obtain after the fact — registers,
stack, a backtrace, the fault syndrome — and (Tier 3) persist a compact record
across the reboot.

Status: **design** (2026-05-31, user-requested). Tier 1 + Tier 2 are BUILD;
Tier 3 is DESIGN-only this pass (built later — Lazarus-adjacent). Naming is
thematic (extinction = ELE, the kernel panic; the Halls archive the dead).

---

## 0. Motivation

`extinction(msg)` today prints `EXTINCTION: <msg>` and halts; `extinction_with_addr`
adds **one** hex address. That is the entire dump. Three of the most expensive
bugs in this project — the year-long "AEGIS corruption" (→ #713 eret-window
race), #788 (the `thread_free`/`on_cpu` UAF), and F-B (the `_boot_stack`
guard fault) — each cost days precisely because a bare faulting address carries
almost no signal. For F-B I symbolized `0x…245000 → _boot_stack_guard` *by hand*
with `nm`. The Halls turn that bare address into a full crash scene.

This is the generalization of the F4(a) fix (`f9cd54f`), which made the
stack-overflow extinction *name its guard flavor*. The Halls name everything.

---

## 1. Ground truth (as-built, the dump builds on this)

- **`struct exception_context`** (`arch/arm64/exception.h`) is the full saved
  register frame — `regs[31]` (x0–x30), `sp`, `elr`, `spsr`, `esr`, `far`.
  `vectors.S` builds it (`sub sp, #EXCEPTION_CTX_SIZE` … `mov x0, sp`) and the
  C handlers already RECEIVE it (`exception_sync_curr_el`,
  `exception_irq_curr_el`, `exception_unexpected`). The frame is captured today;
  it just never reaches the dump.
- **x29 is the AAPCS64 frame pointer**, maintained kernel-wide (`context.S`,
  `userland.S` set it; new frames chain it). A frame-pointer backtrace
  (`[x29]` = caller fp, `[x29+8]` = return address) is feasible *today*.
- **No in-kernel symbol table.** Symbolization is OFFLINE: `kaslr_get_offset()`
  + `addr2line -e build/kernel/thylacine.elf <runtime_addr - offset>`. The boot
  banner prints the KASLR offset once.
- **`_torpor()`** is the WFI halt the extinction path ends in.

---

## 2. Tier 1 — rich UART dump  (BUILD: HX-1)

A new `halls_dump(const struct exception_context *ctx)` invoked from the
extinction path, AFTER the `EXTINCTION: <msg>` line, BEFORE `_torpor()`.

- **Register set.** From a fault/exception site, the saved `struct
  exception_context` (x0–x30, SP, ELR, SPSR, ESR, FAR). From a *bare*
  `extinction()` (an assert deep in code, no exception frame), capture the
  CURRENT GP regs + SP + LR via a small asm stub, and read the EL1 system regs
  (ELR/SPSR/ESR/FAR may be stale at a bare assert — labelled as such). Always
  print `CurrentEL` + `MPIDR_EL1` (which CPU died).
- **How the fault path reaches `ctx`.** A per-CPU "current exception frame"
  pointer, set at exception entry and cleared at exit, lets `halls_dump` find
  the live frame without threading `ctx` through `arch_fault_handle`'s
  signature (per-CPU, mirrors Linux's per-cpu `pt_regs`). Bare-`extinction`
  sites see it NULL → capture-current.
- **Frame-pointer backtrace.** Walk the x29 chain, printing each return address.
  **Bounded + sanity-gated** (HX-I2): cap depth (e.g. 32 frames); require each
  fp to be 16-aligned, within the current stack bounds, and strictly greater
  than the previous fp — stop the instant any check fails. A wild/looping x29
  must never read off-stack or spin.
- **Stack hexdump.** A bounded window (e.g. 256 B) around SP, raw hex — for
  offline inspection of locals / spilled regs / the corrupting value.
- **KASLR-relative addressing.** EVERY code address (PC/ELR, LR, each backtrace
  return addr) is printed BOTH raw AND as `(raw − kaslr_get_offset())` =
  link-time address, so `addr2line -e thylacine.elf <link-addr>` is one
  copy-paste. (Tier 2 makes this live; Tier 1 makes the offline step trivial.)
- **Re-entrancy guard (HX-I1).** A per-CPU "in `halls_dump`" flag. If a fault
  occurs DURING the dump (e.g. the backtrace touches a bad page despite the
  sanity gates), the second entry skips straight to `_torpor()` — the dump
  never loops or recurses.
- **ABI (HX-I3).** The `EXTINCTION: <msg>` line is emitted FIRST and UNCHANGED
  (the agentic-loop signal per TOOLING.md §10). The dump follows under a
  greppable `HALLS:` line prefix so tooling can parse it; `tools/test.sh` keeps
  matching `^EXTINCTION:`.

The as-built `docs/reference/NN-halls.md` details the exact output format +
field layout at HX-1 landing.

---

## 3. Tier 2 — in-kernel symbolization  (BUILD: HX-2)

Embed a minimal, sorted `(link_addr, name)` symbol table in the kernel image so
the backtrace prints `func+0xN` live, no offline step.

- **Build step.** `nm` over `thylacine.elf` → a generated table (`.c`/`.S`),
  addresses stored **link-relative** (KASLR-independent; the runtime lookup
  subtracts `kaslr_get_offset()` first). Wired in `tools/build.sh` +
  `kernel.ld` placement.
- **Lookup.** `symbolize(link_addr) → "name+0xN"` via binary search over the
  sorted table; feed it into the Tier-1 backtrace + PC/LR lines.
- **Cost.** ~tens of KiB of image (we have ~2 MiB headroom under the 3.5 MiB L3
  cap — see `docs/reference/03-mmu.md`). Offline `addr2line` already resolves
  the same info, so Tier 2 is ergonomics, not capability — but cheap and worth
  it for live triage.

---

## 4. Tier 3 — persistence  (DESIGN ONLY this pass)

Goal: a crash record survives the reboot, accumulating into a durable, queryable
**Halls archive**. The user's framing: "if stratumd is sound, persist a small
dump." The mechanism must NOT be stratumd.

### 4.1 Why not stratumd / 9P (panic-safety, HX-I4)
At extinction the kernel is in an unknown, possibly-corrupt state. `stratumd` is
a **userspace Proc** reached through the scheduler → 9P client → virtio-blk →
MMU → locks — i.e. the entire stack that may *be* what just broke (an extinction
in the scheduler or 9P path cannot safely run stratumd). Panic-time I/O must use
the simplest possible durable path. This is exactly why Linux panic persistence
uses **pstore/ramoops**, never the filesystem.

### 4.2 The sink (low-level, panic-safe)
- **Primary: a reserved raw-disk region** — a few sectors at a fixed LBA,
  written by a MINIMAL direct virtio-blk path that bypasses stratumd, 9P, the
  buddy/SLUB, and all locks beyond the device's own ring. Survives reboot.
- **Fallback: a reserved RAM region** (ramoops-style) for warm reboots — but
  QEMU clears RAM on reset, so disk is the durable target.
- **Record format (ABI-bearing).** A versioned fixed header (magic, version,
  length, CRC, boot id, CPU, timestamp-ticks) + the Tier-1 dump bytes. Pinned
  with `_Static_assert`s + a version constant; on-disk-format changes are
  escalation-worthy.

### 4.3 Retrieval (the user's fork — RESOLVED: both, layered = the pstore model)
1. **Automatic next-boot drain (primary, lossless).** Once the system is healthy
   and Stratum FS is mounted, a boot-time step reads any valid record from the
   sink, copies it into the durable FS at `/var/halls/<bootid>.dump`, then
   clears the sink. *Automatic* matters: it evacuates the record before a future
   crash can overwrite the small sink, so the archive accumulates losslessly.
2. **On-demand userspace.** A `halls` tool reads the raw sink directly (for when
   the FS will not mount) AND browses the archived dumps under `/var/halls`.

### 4.4 Build-deferred
The panic-safe direct virtio-blk write is real effort and Lazarus-adjacent (it
is the same "minimal block path on real hardware" muscle the portability arc
needs). Recorded here as the committed design; built in a later chunk.

---

## 5. Invariants

| # | Invariant | Tier |
|---|---|---|
| HX-I1 | The dump never loops or recursively faults — a fault during the dump goes straight to `_torpor()` (per-CPU re-entrancy guard). | 1 |
| HX-I2 | The backtrace walk is bounded (max depth) and fp-sanity-gated (aligned, in-stack, strictly increasing); a wild x29 cannot read off-stack or spin. | 1 |
| HX-I3 | The `EXTINCTION: ` ABI line (TOOLING.md §10) is emitted first and unchanged; the dump follows. | 1 |
| HX-I4 | Tier-3 panic-time I/O touches ONLY the minimal direct sink path — never stratumd / 9P / buddy / scheduler. | 3 |
| HX-I5 | The Tier-3 sink record format is versioned + ABI-pinned (`_Static_assert`). | 3 |

---

## 6. Audit surface

HX-1/HX-2 touch the **exception-entry audit-trigger surface** (CLAUDE.md +
`ARCHITECTURE.md §25.4`): a per-CPU current-frame pointer set/cleared at
exception entry, and register/stack/backtrace dumping in the fatal path. The
HX-audit prosecutor round prosecutes HX-I1/HX-I2 (re-entrancy + bounded walk),
reading the saved frame without clobbering it, and no regression to the existing
guard-overflow detection (F4a). Tier 3, when built, adds the on-disk sink format
(ABI) + a panic-time virtio-blk path — both audit-bearing.

---

## 7. Naming rationale

"Halls of Extinction" — the subsystem that records every kernel death. Fits the
project identity (extinction = ELE; the Halls are where the lineage's dead are
kept). Surfaces: `halls_dump()` (the capture), the `HALLS:` UART prefix, the
`/var/halls` archive, the `halls` userspace tool. The `EXTINCTION:` prefix is
unchanged — the Halls *follow* it.
