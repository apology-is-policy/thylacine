# 54 — SYS_CLOSE / SYS_DUP + /pipe-probe (P5-fd-syscalls) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-fd-syscalls-doc-absorb`).
The final two byte-I/O handlers (`SYS_CLOSE` = 11 / `SYS_DUP` = 12), thin wrappers
over `handle_close` / `handle_dup`, plus `/pipe-probe`, the first userspace binary
to exercise the whole fd round-trip end-to-end. Its content lives, code-verified
and current, in:

- the **handle table** — `handle_close` (release), `handle_dup` (a second
  reference with *reduced* rights — the RightsCeiling, I-6), the dup-variant family
  (`handle_dup_posix` verbatim-rights, `handle_replace`, `handle_dup_to`), the
  per-kind acquire/release asymmetry (KOBJ_SPOOR `spoor_ref`/`spoor_clunk`
  discriminated by the object's leading `u64`), and `handle_close_on_exec`:

      vault/system/kernel/security/sub-kernel-handle.md   (audit: hard)

- the **handlers** — the thin `current_thread()` wrappers + the `new_rights &
  ~RIGHT_ALL` reject:

      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

- the **spec** — `specs/handles.tla`'s `RightsCeiling` (new rights subset of old),
  the invariant the elevation rejection upholds.

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold, and the dossier is AHEAD.** SYS_CLOSE
  and SYS_DUP are basic handle ops fully carried by sub-kernel-handle. The doc's
  "No `dup2` variant" caveat is stale: `handle_dup_posix` (POSIX lowest-free-fd,
  verbatim rights) and `handle_dup_to`/`handle_replace` (forced-index) all exist
  now, carried by the dossier — the fd surface grew past this first-cut doc.
- **`/pipe-probe` is a historical first-witness, correctly left unowned.** It was
  "the first empirical test" of the SVC + uaccess + dev-vtable + handle-release
  composition; that composition is now witnessed by the whole boot suite, and the
  probe binary is a P5-era artifact like the other `*-probe` binaries, not a
  dossier surface. The conservative-rights and per-call-4-KiB caveats are covered
  by sub-kernel-handle and the byte-I/O surface respectively.
