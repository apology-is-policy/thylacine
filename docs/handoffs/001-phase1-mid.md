# Handoff 001 — Phase 1 mid-flight (P1-C close)

**Date**: 2026-05-04
**Tip commit**: `e978929` (P1-C hash-fixup)
**Author**: Claude Opus 4.7 (1M context)
**Audience**: the next Claude Code session (or human collaborator) picking this up.

This is the first formal handoff document for the project. It captures the project's state at P1-C close — the boundary where the kernel goes from "boots and prints" to "MMU on with hardware-enforced memory protection." A fresh session should be able to read this document and pick up productively without re-deriving context from scratch.

---

## TL;DR

- **Phase 0 (design + scripture) is done and binding** — VISION, COMPARISON, NOVEL, ARCHITECTURE, ROADMAP, TOOLING, CLAUDE.md.
- **Phase 1 is in progress.** P1-A + P1-B + P1-C have landed. The kernel boots in QEMU `virt`, parses the DTB, enables the MMU with per-section permissions enforcing W^X (invariant I-12), and exposes `extinction()` (the kernel's ELE — Extinction Level Event) as the panic primitive.
- **Next chunk: P1-C-extras** (KASLR + boot-stack guard + EL2-drop diagnostic). Then P1-D (physical allocator).
- Working tree is clean. `tools/test.sh` PASS. No P0/P1 audit findings open.

---

## Current state of the world

### Build

- **Toolchain**: clang 22.1.4 + ld.lld 22.1.4 (Homebrew), cmake 4, qemu 10. Cross-compile target `aarch64-none-elf`. Apple Silicon Mac under Hypervisor.framework — near-native ARM64 emulation.
- **Build invocation**: `tools/build.sh kernel` (or `make kernel`). Produces `build/kernel/thylacine.elf` (debug, 95 KB) and `build/kernel/thylacine.bin` (flat binary, 8.2 KB). `tools/run-vm.sh` uses the flat binary so QEMU's `load_aarch64_image()` recognizes us as a Linux Image and loads the DTB.
- **Phase tag**: `THYLACINE_PHASE=P1-C` (CMake cache variable; gets baked into the boot banner).

### What runs

```
$ make test
==> PASS: boot banner observed.
Thylacine v0.1.0-dev booting...
  arch: arm64
  cpus: 1 (P1-C; SMP at P1-F)
  mem:  2048 MiB at 0x0000000040000000
  dtb:  0x0000000048000000 (parsed)
  uart: 0x0000000009000000 (DTB-driven)
  hardening: MMU+W^X+extinction (P1-C; KASLR/PAC/MTE/CFI at later sub-chunks)
  kernel base: 0x0000000040080000 (KASLR at P1-C)
  phase: P1-C
Thylacine boot OK
```

The kernel halts in a WFI loop after the banner (no scheduler, no IRQ handlers — those land at P1-F).

### Layout on disk

```
thylacine/
├── arch/arm64/
│   ├── kernel.ld           # linker script + per-section symbols
│   ├── start.S              # Linux ARM64 image header + _real_start
│   ├── mmu.h, mmu.c         # MMU + W^X (P1-C)
│   ├── uart.h, uart.c       # PL011 polled I/O
│
├── kernel/
│   ├── main.c               # boot_main(): banner + halt
│   ├── extinction.c         # ELE (panic) infra (P1-C)
│   ├── include/thylacine/
│   │   ├── types.h
│   │   ├── dtb.h
│   │   └── extinction.h
│   └── CMakeLists.txt
│
├── lib/
│   └── dtb.c                # FDT parser (P1-B)
│
├── tools/
│   ├── build.sh
│   ├── run-vm.sh
│   ├── test.sh
│
├── docs/
│   ├── VISION.md            # binding scripture
│   ├── COMPARISON.md
│   ├── NOVEL.md
│   ├── ARCHITECTURE.md
│   ├── ROADMAP.md
│   ├── TOOLING.md           # supplementary scripture
│   ├── REFERENCE.md         # as-built index
│   ├── reference/
│   │   ├── 00-overview.md
│   │   ├── 01-boot.md       # boot path; updated through P1-C
│   │   ├── 02-dtb.md        # FDT parser
│   │   ├── 03-mmu.md        # MMU + W^X
│   │   └── 04-extinction.md # ELE infra
│   ├── USER-MANUAL.md       # user-facing index (mostly empty pre-Utopia)
│   ├── manual/
│   │   └── 00-overview.md
│   ├── phase1-status.md     # active phase pickup guide
│   ├── decisions/           # (empty — for future architectural memos)
│   ├── handoffs/
│   │   └── 001-phase1-mid.md  # ← this document
│   └── tlcprimer/           # priming docs (Phase 0 input, decision archive)
│
├── specs/
│   └── README.md            # TLA+ setup; specs land at Phase 2+
│
├── cmake/
│   └── Toolchain-aarch64-thylacine.cmake
│
├── build/                   # gitignored
│   └── kernel/
│       ├── thylacine.elf
│       └── thylacine.bin
│
├── share/                   # 9P host share for QEMU virtfs (gitignored)
│
├── CMakeLists.txt
├── Makefile
├── CLAUDE.md                # operational framework
└── .gitignore
```

### Memory (`~/.claude/projects/-Users-northkillpd-projects-thylacine/memory/`)

```
MEMORY.md                       # one-line index
project_active.md               # current state: Phase 0 done, P1-A/B/C landed
project_next_session.md         # pickup pointer with decision tree
user_profile.md                 # Michal's role + working preferences
project_motto.md                # "The thylacine runs again." reserved usage
feedback_reference_discipline.md # maintain BOTH technical + user references per-chunk
```

The next session will be auto-loaded with these on context init.

---

## Read order for the next session

1. `CLAUDE.md` — operational framework. Mandatory.
2. `docs/VISION.md` §1, §3, §8 (load-bearing invariants), §13 (Utopia milestone).
3. `docs/ARCHITECTURE.md` §6 (memory) + §24 (hardening) + §28 (invariants enumerated).
4. `docs/ROADMAP.md` §4 (Phase 1).
5. `docs/TOOLING.md` §3 (run-vm.sh), §10 (boot banner / EXTINCTION ABI).
6. **This document** — for current state.
7. `docs/phase1-status.md` — active phase pickup with deferred items.
8. The relevant subsystem reference under `docs/reference/` for whatever you're touching.

When in doubt, scripture wins. Per CLAUDE.md "Reference documentation discipline": if scripture, technical reference, code, and user reference disagree, the order of authority is spec → technical reference → code → user reference, with `ARCHITECTURE.md` as the authoritative design-intent source.

---

## What landed in this push (P1-A → P1-C)

### P1-A — Toolchain + minimal boot stub (commit `2b332d8`)

- CMake build system + `aarch64-none-elf` cross-compile.
- ARM64 entry: EL check, BSS clear, stack setup, `boot_main()` call.
- PL011 polled UART output.
- `tools/run-vm.sh` + `tools/test.sh` + Makefile aliases.
- Boot banner per TOOLING.md §10 ABI contract.

### P1-B — DTB parser + Linux ARM64 image header (commit `d3e33a8`)

- `lib/dtb.c` — hand-rolled FDT v17 parser. Public API: `dtb_init`, `dtb_get_memory`, `dtb_get_compat_reg`, `dtb_get_chosen_kaslr_seed`. ~340 LOC.
- `start.S` — 64-byte Linux ARM64 image header at offset 0 (so QEMU's `load_aarch64_image()` loads the DTB and passes the address in `x0`).
- CMake post-link `objcopy -O binary` produces `thylacine.bin` alongside the ELF.
- `uart.c` — PL011 base now runtime-configurable (`uart_set_base()`); `boot_main()` discovers it via `dtb_get_compat_reg("arm,pl011")`. Resolves invariant I-15.
- Banner now shows DTB-derived memory size + DTB pointer + DTB-driven UART base.

**Two notable bugs caught and codified during P1-B**:

1. **Compiler fusion of `be32_load` calls into 8-byte loads.** With MMU off, all kernel data is Device-nGnRnE; an unaligned 8-byte access faults. clang fused two adjacent 4-byte property reads into a single `ldr x_, [x_]` which then faulted on 4-aligned-but-not-8-aligned propdata. Mitigated via `volatile` u32 read inside `be32_load`. **Codified** in source comment + `docs/reference/02-dtb.md`.
2. **PL011 `compatible` property comes after `reg`.** First implementation of `dtb_get_compat_reg` set a single match-pending flag and looked for `reg` after seeing `compatible`. In QEMU's DTB, the order is `reg` (4th property) then `compatible` (5th) — the flag was set after `reg` was already past. Fixed via stack-based per-node accumulator that records `(compat_matched, reg_data)` per node, decides at `END_NODE`. **Independent of property order.**

### P1-C — MMU + W^X + extinction infra (commit `6462227`)

- `arch/arm64/mmu.{h,c}` — 4-level page tables (1 L0 + 1 L1 + 4 L2 + 1 L3 = 28 KiB BSS-allocated). Identity-maps low 4 GiB. Per-section permissions for kernel image (`.text` RX, `.rodata` R, `.data + .bss` RW). PL011 region as Device-nGnRnE block.
- **W^X invariant I-12 encoded in PTE constructor macros + `_Static_assert`**. Build breaks if a future PTE helper allows writable AND executable-at-EL1.
- MMU enable in canonical order: MAIR → TCR → TTBR0 → ISB → SCTLR.M|C|I → ISB.
- `kernel/extinction.{c,h}` — `extinction(msg)`, `extinction_with_addr(msg, addr)`, `ASSERT_OR_DIE(expr, msg)`. **The kernel panic primitive renamed to `extinction`** thematically (ELE = Extinction Level Event; the thylacine's own fate transposed onto a kernel that has lost the will to continue). The TOOLING ABI prefix is now `EXTINCTION:` (was `PANIC:`); update propagated to `tools/test.sh`, `TOOLING.md`, `CLAUDE.md`.

---

## Trip hazards (durable; remember these)

### 1. Compiler fusion + Device memory alignment

**With MMU off**, all kernel data accesses are Device-nGnRnE. Device memory mandates natural alignment for the access width — an unaligned 4/8/16-byte load faults synchronously. The DTB structure block guarantees only **4-byte alignment** for property data.

clang **will** fuse adjacent 4-byte loads on consecutive memory into a single 8-byte load. Without intervention, this produces an `ldr x_, [x_]` on 4-aligned-but-not-8-aligned property data, which faults on Device memory.

**Mitigation in place**: `lib/dtb.c`'s `be32_load` uses `*(const volatile uint32_t *)p` to forbid fusion. Document: `docs/reference/02-dtb.md` "Byte-order helpers — IMPORTANT".

**At P1-C and later**, the MMU is on with cacheable Normal memory for kernel data. The fusion mitigation is no longer load-bearing for normal ops, but **keep it** — defends against bare-metal recovery paths and any future code that accesses pre-MMU memory.

### 2. QEMU `-kernel` ELF entry without Linux header doesn't load DTB

QEMU's `load_aarch64_image()` only fires when the binary has the ARM64 image magic (`ARM\x64`) at offset 0x38. For ELF kernels without the header, QEMU's `load_elf_as` runs with `is_linux = 0`, and `arm_load_dtb()` is **skipped entirely** — the DTB is never loaded into RAM.

**Mitigation in place**: `arch/arm64/start.S` includes the 64-byte Linux ARM64 image header at offset 0; `kernel/CMakeLists.txt` runs `objcopy -O binary` post-link to produce `thylacine.bin`; `tools/run-vm.sh` uses `-kernel thylacine.bin`.

If you ever wonder why `_saved_dtb_ptr == 0` again: check that you're booting the `.bin` and that the header is intact. The header's `_image_size` symbol is linker-resolved (`_kernel_end - _kernel_start`); if the linker script changes those bounds the header still works (re-link picks up the new value), but if the symbol is renamed or removed the build breaks loud.

### 3. The W^X invariant is encoded in PTE constructors

`arch/arm64/mmu.h` defines `PTE_KERN_TEXT` / `PTE_KERN_RO` / `PTE_KERN_RW` / `PTE_KERN_RW_BLOCK` / `PTE_DEVICE_RW_BLOCK`. **Use these helpers**. Don't construct PTEs by hand from raw bits — the helpers carry `_Static_assert`s that fail the build if W^X is violated. A future helper that breaks the rule should fail the build; if you find yourself disabling an assert, stop and reconsider.

`pte_violates_wxe(pte)` is the runtime check — call it from fault handlers (P1-F+) before deciding whether to recover or `extinction()`.

### 4. EXTINCTION: prefix is kernel ABI

Per TOOLING.md §10, the literal string `"EXTINCTION: "` (12 bytes: 11 ASCII + space, on a fresh line) is the agentic loop's catastrophic-failure-detection signal. Don't change it without coordinated updates to `tools/run-vm.sh`, `tools/test.sh`, `tools/agent-protocol.md`, `CLAUDE.md`, and `TOOLING.md` in the same commit. Same for the `Thylacine boot OK` success line.

### 5. Identity mapping is transitional

P1-C maps kernel in TTBR0 low half. KASLR (P1-C-extras) moves the kernel to TTBR1 high half. Until then, kernel runs at low VAs. If you write a function that depends on the kernel's VA being in `0xFFFF_*` range (e.g., a future debug printer that filters addresses), test that it works post-KASLR too.

### 6. Page tables are static at P1-C

`arch/arm64/mmu.c` declares page table arrays as BSS. Fine for identity-mapping the low 4 GiB. **Won't scale** to per-process address spaces (Phase 2). When P1-D's physical allocator lands, page tables become dynamically allocated. The current static allocation is the bootstrap baseline.

### 7. boot_main runs with MMU on

After `bl mmu_enable` in `_real_start`, the kernel is already in virtual memory with caches enabled. From `boot_main()`'s perspective there is no "MMU off" path. If you write code that needs MMU-off (e.g., bare-metal recovery), it goes BEFORE `mmu_enable` in `start.S`, not in C.

---

## Naming conventions established

- **Kernel panic = "extinction"**. Function: `extinction(msg)`. Prefix: `EXTINCTION:`. Theme: ELE = Extinction Level Event; the thylacine's own fate. Approved by user during P1-C.
- **Project motto: "The thylacine runs again."** Reserved for end-of-phase / release / milestone moments per `memory/project_motto.md`. Don't wear it out.
- **Names already chosen and stable**: Thylacine (OS), Stratum (FS), Halcyon (shell, Phase 8), janus (key agent, runs at Phase 4 from Stratum), `_saved_dtb_ptr` (DTB physical address), `_image_size` (linker-resolved kernel-image-size symbol).
- **Held for explicit signoff before applying**:
  - `_hang` (the WFI loop) → `_torpor` (marsupial deep-sleep state).
  - Future audit-prosecutor agent name: stays as "prosecutor" for Stratum continuity unless explicit signoff.

The user said "don't be shy suggesting more fitting names along the way." If a fitting name surfaces, mention it; defer the rename to explicit signoff before touching code. Don't rename load-bearing identifiers (kernel-tooling ABI, public APIs already documented) without coordination.

---

## What's next

### Decision tree for the next chunk

**Option A — P1-C-extras (recommended)**: KASLR + boot stack guard page + EL2→EL1 drop diagnostic.

This is the deferred portion of P1-C. Three sub-deliverables, each load-bearing:

- **KASLR**: compile kernel `-fpie`, switch linker script to a PIE-friendly form, process `R_AARCH64_RELATIVE` relocations at boot using a randomized offset derived from `dtb_get_chosen_kaslr_seed()` (with low-entropy boot-counter fallback if the DTB property is absent). Move kernel to TTBR1 high half (`0xFFFF_A000_*` per ARCH §6.2). Verify base address differs across 10 boots. ROADMAP §4.2 exit criterion.
- **Boot stack guard page**: map the page below `_boot_stack_bottom` as non-present (PTE_VALID=0). Stack overflow → page fault → `extinction("kernel stack overflow", FAR_EL1)`. Requires the fault handler from P1-F to fully work; at P1-C-extras we install the page-table mapping; the diagnostic gets wired in P1-F.
- **EL2→EL1 drop**: replace the silent `.Lnot_el1` halt in `start.S` with a proper EL2→EL1 drop sequence + UART diagnostic. Mostly a Pi 5 concern (post-v1.0); QEMU virt boots at EL1. Worth doing now while we're in the boot path.

Total LOC: ~400-600. Token estimate: **120-180k**. Likely 1-2 fresh-build iterations on the KASLR relocation processing (tricky).

**Option B — Skip P1-C-extras for now, go straight to P1-D**: Physical allocator (buddy + per-CPU magazines).

Pros: Allocator unblocks more (every later subsystem needs it). KASLR is non-essential for development. Cons: Deviates from the documented ordering. KASLR becomes harder to add later because by then we have many more kernel structures whose addresses need handling under relocation.

**Option C — Set up CI infrastructure**: GitHub Actions workflow that runs `tools/build.sh kernel` + `tools/test.sh` on every push. Lands `~/projects/thylacine/.github/workflows/ci.yml`. Doesn't depend on any P1 chunk. Token cost: 20-40k.

**Recommendation**: **Option A**. KASLR is a committed P1 deliverable; deferring it further compounds the cost. P1-C-extras is naturally bounded and unblocks P1-F's fault handler from "stack overflow corrupts BSS silently" to "stack overflow → clean EXTINCTION".

---

## Quick-reference commands

```bash
cd ~/projects/thylacine

# Build
tools/build.sh kernel
make kernel

# Run interactively (Ctrl-A x to quit QEMU)
tools/run-vm.sh
make run

# Boot test
tools/test.sh
make test

# GDB stub on :1234, halted at entry
tools/run-vm.sh --gdb
make gdb

# All TLA+ specs (none yet — first ones land Phase 2)
make specs

# Clean
tools/build.sh clean
make clean

# Look at the kernel
/opt/homebrew/opt/llvm/bin/llvm-objdump -d build/kernel/thylacine.elf | less
/opt/homebrew/opt/llvm/bin/llvm-readelf -SW build/kernel/thylacine.elf

# Inspect the flat binary's Linux ARM64 header
od -A x -t x1 -N 64 build/kernel/thylacine.bin

# Dump QEMU's auto-generated DTB for the virt machine
qemu-system-aarch64 -machine virt -cpu max -m 2G -machine dumpdtb=/tmp/virt.dtb -nographic
dtc -I dtb -O dts /tmp/virt.dtb | less

# Git
git log --oneline -20
git diff HEAD^
```

---

## Posture summary

| Metric | Value |
|---|---|
| Tip commit | `e978929` (P1-C hash-fixup) |
| Phase | Phase 1 — P1-C closed |
| Next chunk | P1-C-extras (KASLR + guard page + EL2 drop) |
| Build matrix | default Debug — green |
| `tools/test.sh` | PASS |
| Specs | 0/9 (Phase 2 introduces first spec — `scheduler.tla`) |
| LOC | ~1030 (640 C99 + 125 ASM + 55 LD + 210 sh/cmake) |
| Kernel ELF | 95 KB debug / 87 KB stripped |
| Kernel flat binary | 8.2 KB |
| Page tables | 28 KiB BSS-allocated |
| Open audit findings | 0 (audit pass at Phase 1 exit / P1-I) |
| Open deferred items | KASLR + boot stack guard + EL2 drop (P1-C-extras) |

---

## Stratum coordination

Stratum is feature-complete on Phases 1-7 of its v2 roadmap. Phase 8 (POSIX surface — inodes, dirents, xattrs, ACLs, modern POSIX) is in progress. Phase 9 (9P server + Stratum extensions: `Tbind` / `Tunbind` / `Tpin` / `Tunpin` / `Tsync` / `Treflink` / `Tfallocate`) is the integration target for **Thylacine Phase 4**.

Thylacine Phases 1-3 (where we are now) proceed in parallel with Stratum's Phase 8-9 work — no dependency. The Stratum repo is at `~/projects/stratum/`; reference its `docs/` for protocol details when the 9P client lands at Phase 4.

---

## Format ABI surfaces in flight

These are stable contracts that next-session work must not break without coordinated multi-document updates:

| Surface | Where | Contract |
|---|---|---|
| Boot banner success line | `kernel/main.c` `boot_main()` | "Thylacine boot OK\n" — agent's success signal |
| Extinction prefix | `kernel/extinction.c` | "EXTINCTION: " — agent's catastrophic-failure signal |
| Linux ARM64 image header | `arch/arm64/start.S` offset 0..0x40 | `b _real_start; nop; <text_offset>; <image_size>; <flags>; <res>; "ARM\x64"` |
| `_image_size` linker symbol | `arch/arm64/kernel.ld` | `_kernel_end - _kernel_start` |
| `_saved_dtb_ptr` BSS variable | `arch/arm64/start.S` | DTB physical address |
| `_text_end` / `_rodata_end` / `_data_end` | `arch/arm64/kernel.ld` | section boundary symbols used by mmu.c |
| Page table layout (4-level, 4 KiB granule) | `arch/arm64/mmu.c` | 1 L0 + 1 L1 + 4 L2 + 1 L3, BSS-allocated |
| MAIR attribute indices | `arch/arm64/mmu.h` | 0=Device, 1=NormalNC, 2=NormalWB, 3=NormalWT |
| PTE constructor macros | `arch/arm64/mmu.h` | W^X-safe by `_Static_assert` |
| FDT tokens + magic | `lib/dtb.c` | per devicetree spec v0.4 |

Changing any of these requires a documented rationale in the commit message + updates to `docs/reference/`.

---

## Things I would NOT recommend deviating from (lessons from P1-A through P1-C)

- **Always use the helper macros** (`PTE_KERN_*`, `read_reg_pair`, `be32_load`). They're load-bearing.
- **`tools/test.sh` is the canonical test gate**. Pre-commit; if it fails, don't commit. Even "trivial" changes can break boot in subtle ways (cf. the compiler-fusion-into-Device-memory issue).
- **Doc-update-per-PR**. If you touch `arch/arm64/mmu.c`, update `docs/reference/03-mmu.md` in the same commit. If you touch boot banner format, update `docs/TOOLING.md §10` and this handoff (or write a successor handoff) in the same commit.
- **Two commits per substantive landing**: one substantive + one hash-fixup. Per CLAUDE.md "Audit-close commit anatomy". Even non-audit-close chunks benefit from this pattern (the hash-fixup commits make `git log --oneline` instantly readable).
- **Volatile in `be32_load` is load-bearing** — don't optimize it away even after MMU is on.

---

## Open questions / future-work tags

These are tracked here so they don't get lost across sessions:

- **U-1**: Should `extinction()` print a register dump? Currently it prints just the message. Useful for fault handlers (P1-F+) when the dump is meaningful. Decision: defer until P1-F has the exception infrastructure to capture the registers.
- **U-2**: Should we add a `dmesg`-equivalent kernel log buffer? Currently every kernel print goes straight to UART. A ring buffer would let `/ctl/log/` (kernel admin tree) expose past messages. Defer to Phase 2 once we have processes to consume it.
- **U-3**: KASLR seed entropy. QEMU populates `/chosen/kaslr-seed` with a per-boot random. On bare-metal Pi 5, this depends on the firmware. The fallback is a low-entropy boot-counter; ARCH §5.3 says "logged warning" — implement at P1-C-extras.
- **U-4**: `mmu.tla` formal spec. Page-table walking under concurrent updates (relevant when SMP arrives in Phase 2). Not load-bearing at v1.0; consider for v1.1 hardening pass.
- **U-5**: Hardened malloc / Scudo for userspace heap (per `ARCHITECTURE.md §24.3`). Lands when musl port arrives at Phase 5.

---

## How to write the next handoff

When the next session reaches a milestone (phase exit, gate signoff, major architectural decision), write `docs/handoffs/00N-<title>.md` following this document's structure:

- TL;DR
- Current state of the world
- Read order
- What landed in this push
- Trip hazards (durable; learned-from-experience gotchas)
- Naming conventions established
- What's next + decision tree
- Quick-reference commands
- Posture summary
- Stratum coordination status
- Format ABI surfaces in flight

Number them sequentially. The handoff thread is the project's continuity story — sessions chain through these documents. Any session reading the latest handoff plus the binding scripture can reconstruct the project's full state.

---

## Sign-off

The thylacine runs again. 🐅

(Project motto, used per `memory/project_motto.md` for milestone moments. P1-C close — the kernel now has hardware-enforced memory protection — qualifies.)
