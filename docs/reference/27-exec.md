# 27 — kernel-internal exec (P3-Eb) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-exec-internal-doc-absorb`).
`exec_setup` — the bridge from a parsed ELF to a populated address space: one VMA
per `PT_LOAD` segment, a user stack with its guard page, the init frame, and (since
REVENANT) demand-paged file backing. A P3-Eb-era doc, updated through LINEAGE L-4a
and DISTRO D-4 and comprehensively superseded; its content is carried by:

- the **exec spine** — the three `exec_setup` forms (`exec_setup` for the boot
  path, `exec_setup_from_spoor` for `SYS_SPAWN_*`, plus the argv form), the
  validate/map-each-`PT_LOAD`/map-the-stack sequence, the sub-page `PT_LOAD` floor
  geometry, the sparse backing of non-executable segments (L-4a), the #107 I-cache
  sync over the whole executable span, the init `argc/argv/envp/auxv` frame, the
  DISTRO D-4 `PT_INTERP` rewrite, and the exec-onto-a-Proc-with-threads guard:

      vault/system/kernel/execution/sub-kernel-exec.md

- the **ELF parse it consumes** — `elf_load`, the rejection taxonomy, the W^X
  parse-time check:

      vault/system/kernel/execution/sub-kernel-elf.md

- the **REVENANT file-backed demand-paging** (I-36) — the FILE fault arm that
  pages executable segments in on first touch:

      vault/system/kernel/memory/sub-kernel-fault.md

- the **Image cache** that REVENANT shares text through, and its eviction safety
  (I-36):

      vault/system/kernel/execution/sub-kernel-image.md

- the **VMA + user-stack guard page** the segments and stack are mapped through:

      vault/system/kernel/memory/sub-kernel-addrspace.md

- the **BURROW mapping lifecycle** (I-7) the per-segment maps participate in — a
  page lives until the last handle closes AND the last mapping is gone:

      vault/system/kernel/memory/sub-kernel-burrow.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Its P3-Eb "kernel-internal only" framing is long past.** "`exec_setup` … does
  NOT transition the calling thread to EL0. The ERET-to-EL0 step is P3-Ed"; "Tests
  at P3-Eb validate the address-space population in isolation; end-to-end userspace
  runs at P3-Ed" — the EL0 transition, `SYS_SPAWN_*`, `SYS_EXECVE`, and the whole
  REVENANT demand-paged path are all built.
- **`exec_setup` is the spawn-into-an-empty-child path**, distinct from the
  *detached* image build `execve` uses (`exec_load_into`, LINEAGE L-2a — see
  `docs/reference/147-execve.md`, itself absorbed). This doc predates that split.
- **The L-4a and D-4 updates it carries are now the dossiers' content**: the sparse
  private backing, the two Burrow primitives, the I-32 page-axis join, and the
  `PT_INTERP` interpreter rewrite all live in `sub-kernel-exec` at more depth.
