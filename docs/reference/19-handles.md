# 19 — Handle table (P2-F) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-handles-doc-absorb`).
The per-Proc table of typed unforgeable tokens (handles) naming kernel objects.
Its content is carried, more currently and more completely, by:

- the **handle table itself** — the `kobj_kind` universe, the compile-time kind
  partition (I-5), `struct Handle` / `struct HandleTable`, the four duplication
  primitives, `handle_alloc`/`get`/`put`/`close`/`dup`, the per-table lock
  discipline (#844), close-on-exec, fork-copy, and the I-4/I-5/I-6 enforcement:

      vault/system/kernel/security/sub-kernel-handle.md

- the **`RIGHT_*` bit values** (`RIGHT_READ`=0 … `RIGHT_SIGNAL`=5, `RIGHT_ALL`
  = `0x3f`) and the monotonic-reduction rule:

      vault/system/boundary/registries/abi-handle-rights.md

- the **capability axis** (I-2, `rfork`'s cap mask) it forward-references:

      vault/system/kernel/security/sub-kernel-caps.md

- the **hardware-handle CREATION surface** (MMIO/IRQ syscalls + cap gating +
  `HwResourceExclusive`/`HwHandleImpliesCap`) it points at — itself now absorbed
  via `docs/reference/39-hw-handles.md`'s redirects.

**What this file got WRONG or MISSED by the time it was absorbed** — it is a
P2-Fc snapshot, stale on several axes, and `sub-kernel-handle` is more current on
every one:

- **`PROC_HANDLE_MAX` is `1024`, not `64`.** The constant went 64 → 256
  (2026-06-24) → 1024 (2026-08-13); this file says 64 in three places. (The
  dossier documents the drift explicitly, and notes `poll.h`/`syscall.h` still
  say 64 — tasks #184/#166.)
- **The kind partition is four-way** (Transferable / HW / SRV / **Loom**, plus
  PCI in HW), pinned by seven `_Static_assert`s — this file lists only three
  partitions and `KOBJ_KIND_COUNT == 10`, yet its own spec-mapping table still
  says `KOBJ_KIND_COUNT == 9` in two rows (an internal contradiction the
  P5-corvus-srv bump left behind).
- **There are four duplication primitives** (`handle_dup` / `handle_dup_posix`
  / `handle_replace` / `handle_table_copy_into` / `handle_dup_to`), not just
  `handle_dup`; and `handle_get` is a by-value refcount-bumped snapshot paired
  with `handle_put`.
- The **"single-CPU lifecycle, not internally synchronized, lock at Phase 5+"**
  caveat is false: there is one spinlock per table serializing alloc/close/get/
  dup (the #844 discipline), and fork copies the table.
- `handle_init` is now a **no-op** (the table is `kmalloc`/`alloc_pages`-backed,
  not a SLUB cache), and the `struct Proc` "88 → 96 bytes" note is long stale.
