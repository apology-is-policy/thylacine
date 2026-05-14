# 59. /joey loaded from the initrd (P5-joey-from-ramfs)

The first userspace process is now a real binary built from `usr/joey/` and shipped in the cpio initrd, replacing the prior 9-instruction kernel-embedded blob. This is the precursor for the P5-stratumd-bringup arc, where `/joey` extends to orchestrate stratumd-system, attach the system pool's 9P tree at `/sysroot`, and pivot root before starting `corvus` and `login`.

This chunk is structural, not behavioural: the bring-up handshake (rfork → exec_setup → userland_enter → wait_pid) is unchanged. What moved is the source of the ELF — from compile-time-embedded to ramfs-loaded.

---

## Why this is the prerequisite

P5-stratumd-bringup (per `CORVUS-DESIGN.md §3 D4` + `§10`) requires `/joey` to:

1. Fork children (`stratumd-system`, then `corvus`, `login`, per-user `stratumd`).
2. Hold long-running supervisor state.
3. Run arbitrary userspace code (a 9-instruction hand-encoded blob can't reach the SVC dispatch surface needed to issue `SYS_ATTACH_9P` etc.).

None of this is possible while `/joey` is a 36-byte hand-encoded program embedded in the kernel image. Replacing the source of the ELF unblocks the rest of the bring-up arc; the orchestration logic lands in subsequent chunks.

---

## Source of truth

### `usr/joey/joey.c`

Userspace binary using `libt`. v1.0 minimum-viable: prints a banner via `t_putstr` and exits 0. The body grows as the bring-up arc lands.

```c
#include <thyla/syscall.h>

int main(void) {
    t_putstr("joey: hello from /joey (real userspace binary, loaded from ramfs)\n");
    return 0;
}
```

### `usr/joey/CMakeLists.txt`

Standard userspace pattern (same as `hello`, `pipe-probe`, `attach-probe`):

- `add_executable(joey joey.c)` + `target_link_libraries(joey PRIVATE t)`.
- W^X linker script enforced via the project-level `add_link_options(-T${USR_LINKER_SCRIPT})` in `usr/CMakeLists.txt`.
- `set_target_properties(joey PROPERTIES SUFFIX "" OUTPUT_NAME joey)` so the cpio entry is exactly `joey` (no `.elf` suffix).

### `tools/build.sh::build_ramfs`

Curated copy list `usr_bins=( "hello" "joey" "pipe-probe" "attach-probe" )`. `joey` is the second entry; placement is alphabetical-by-convenience, not load-bearing.

### `kernel/joey.c`

The kernel-side orchestration. Three changes from the predecessor (P3-F embedded blob):

1. `build_init_elf()` and the embedded `g_joey_program[]` instruction stream are removed.
2. `joey_run()` now calls `devramfs_lookup(JOEY_RAMFS_NAME, &cpio_blob, &blob_size)`. The cpio blob lives in initrd memory for the kernel's lifetime; the pointer is valid past joey's exit.
3. The cpio's 4-byte-aligned bytes are copied into `g_joey_elf_blob` (still in BSS but now `_Alignas(struct Elf64_Ehdr)`, sized 32 KiB) before being handed to `exec_setup`. The ELF loader requires 8-byte alignment for the Ehdr cast (`kernel/elf.c::elf_load` R5-G F61); cpio newc data is only 4-byte aligned, so the static-buffer copy is mandatory. The same pattern is used in `kernel/test/test_userspace_ramfs.c`.

---

## ELF loading boundary

The alignment requirement is non-obvious for callers handing cpio bytes to `exec_setup`. The cpio newc format pads to 4 bytes; the ELF loader's `elf_load` validates 8-byte alignment up front:

```c
if (((uintptr_t)blob) % _Alignof(struct Elf64_Ehdr) != 0)
    return ELF_LOAD_BAD_ALIGN;
```

Three observation-based facts that motivated the static buffer:

1. The 9-instruction predecessor blob was 8-aligned by construction (`_Alignas(struct Elf64_Ehdr)` on the embedded BSS), so this hazard was invisible until the cpio path started feeding `exec_setup`.
2. `/hello`'s ramfs-lookup path in `kernel/test/test_userspace_ramfs.c` already uses an 8-aligned copy buffer; `kernel/joey.c` mirrors that.
3. The static buffer is sized for the userspace binary budget (16 KiB headroom up to 32 KiB; see P4-image-shrink).

---

## Boot diagnostic

`joey_run` prints the rfork message with the loaded blob size (so the source — initrd vs embedded — is immediately visible in the boot log):

```
  joey: rforking child for /joey (12744 byte ELF from initrd)
joey: hello from /joey (real userspace binary, loaded from ramfs)
  joey: /joey pid=N exited cleanly (status=0)
Thylacine boot OK
```

The middle line is `t_putstr` output from `/joey`'s `main`. The flanking lines are `uart_puts` from `kernel/joey.c`.

The "byte ELF from initrd" suffix is the disambiguator: prior boots printed `9-instr hello blob`. If a future regression accidentally fell back to an embedded blob, the byte count would mismatch the cpio entry's size.

---

## Failure paths

| Failure | Symptom | Resolution |
|---|---|---|
| `/joey` missing from cpio | `EXTINCTION: joey: /joey not found in initrd (devramfs_lookup failed)` | Rebuild with `tools/build.sh all`; check `usr_bins` array in `tools/build.sh`. |
| `/joey` cpio entry has zero size | `EXTINCTION: joey: /joey in initrd has zero size` | `tools/mkcpio.py` regression; rerun ramfs build. |
| `/joey` ELF exceeds 32 KiB | `EXTINCTION: joey: /joey ELF exceeds JOEY_BLOB_MAX <size>` | Bump `JOEY_BLOB_MAX` (cheap; BSS is sparse-mapped). |
| `exec_setup` rejects blob | `joey: exec_setup failed rc=<N>; exits(fail-exec)` — and joey reaps with status=1 | Diagnose against `kernel/elf.c::elf_load_status` codes. |
| `/joey` faults in EL0 | `EXTINCTION: EL0 unhandled sync exception (EC in ESR_EL1) <esr>` | Userspace bug in `usr/joey/joey.c` or `libt`'s `_start`. |

---

## What didn't change

- `joey_run` remains single-call (`g_joey_run_called` guard) until the supervisor extension lands.
- Boot-banner contract (`TOOLING.md §10`): `Thylacine boot OK` still prints after `/joey` exits cleanly.
- Test-suite count: unchanged at 416 — no new kernel-internal tests. The boot path itself is the regression: if `/joey` fails to load or exit cleanly, `joey_run` extincts and the banner never prints, which `tools/test.sh` reports as failure.

---

## Status

| Item | State |
|---|---|
| `usr/joey/joey.c` + `CMakeLists.txt` | LANDED |
| `tools/build.sh::build_ramfs` `usr_bins += "joey"` | LANDED |
| `kernel/joey.c` refactor: embedded blob → devramfs_lookup + 8-aligned copy | LANDED |
| Boot log distinguishes initrd vs embedded source | LANDED |
| Long-running joey orchestrator | DEFERRED (next chunk) |
| Forks stratumd-system | DEFERRED (P5-stratumd-bringup-b) |
| Kernel-side 9P mount of /sysroot | DEFERRED (P5-stratumd-bringup-b) |
| pivot_root / chroot | DEFERRED (P5-stratumd-bringup-c) |

Test posture: 416/416 PASS × default + UBSan.

---

## Known caveats

1. **Pre-existing flaky EL1 extinction** unrelated to this chunk: `tools/test.sh` occasionally reports an `EXTINCTION: unhandled sync exception (EC in ESR_EL1) 0x...02000000` line on a secondary CPU during boot. The boot completes; tests pass. Documented in the predecessor handoff (`memory/project_next_session.md` "Traps + pitfalls"). Not caused by P5-joey-from-ramfs.
2. **No explicit kernel-internal regression test** for the new path. Justification: the boot CPU's `boot_main → joey_run → /joey → exits(0) → banner` is the regression. A dedicated test would re-implement the same path against the same `devramfs_lookup`. If a future change refactors `joey_run` (e.g., the long-running supervisor), a dedicated kernel-internal test becomes worthwhile.
3. **JOEY_BLOB_MAX = 32 KiB.** Comfortable for the v1.0 hello-style joey (12744 bytes) and for the orchestrator-extension joey (expected to stay under 32 KiB if no library bloat). Bump on demand; extinction at boot is the failure mode if exceeded.
