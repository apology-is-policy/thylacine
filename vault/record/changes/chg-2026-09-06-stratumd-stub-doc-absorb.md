---
id: chg-2026-09-06-stratumd-stub-doc-absorb
type: chg
title: "absorb docs/reference/61-stratumd-stub (P5 stub-bringup test-scaffold arc): zero-fold, test-probe redirect stub"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---

# docs/reference/61-stratumd-stub.md -> ABSORBED (test-scaffold arc doc)

Absorbed the P5-stratumd-stub-bringup arc doc into a multi-redirect stub. It is a
test-scaffold doc — the stub 9P responder + its three client probes + the kernel
test framework are UNOWNED (no dossier owns a test binary; same disposition as
`u-test` and `attach-probe`), named in the stub as the records they are.

The production surfaces the arc exercises are each owned elsewhere:

- the userspace-server 9P path -> `sub-kernel-ninep-attach` + `sub-kernel-ninep-dev9p`
  + `sub-kernel-syscall-dispatch`.
- the transport pipes + EOF cascade -> `sub-kernel-pipe`.
- fd-inheritance spawn (`SYS_SPAWN_WITH_FDS`) -> `sub-pouch-process` (positional
  `fd_list[i]` inheritance) + `sub-kernel-exec`; its own live syscall reference is
  `62-sys-spawn-with-fds.md`.
- the joey boot-path orchestration (`do_stratumd_stub_bringup`) -> `sub-kernel-joey`.
- `SYS_WALK_OPEN` (e1, incl. the shallow-`aux` walk-failure cleanup trap) ->
  `sub-kernel-stalk` + `sub-kernel-spoor` + `sub-kernel-dev`.
- `SYS_CHROOT` + `Territory.root_spoor` + the FROM_ROOT sentinel (e2) ->
  `sub-kernel-territory`; its own live syscall reference is `77-sys-chroot.md`.

Zero fold. Every atom is covered, several dossiers well ahead of the doc:

- `pivot_root` is BUILT — the doc calls `SYS_PIVOT_ROOT`/`SYS_UNCHROOT` "v1.x
  deferred"; `sub-kernel-territory` documents `territory_pivot_root` as present
  (differs from chroot only in requiring an existing root), and even records the
  reference-doc-era staleness on this point.
- the chroot-in-joey deadlock is a CONSEQUENCE of the documented five-site
  refcount discipline (`root_spoor` holds its own ref for the Territory's life,
  independent of the handle-table fd), not an atom unique to this doc.
- the walk-failure shallow-`aux` trap is covered in depth in
  `sub-kernel-spoor` / `sub-kernel-dev` / `sub-kernel-ninep-dev9p`.

The last of the P5 test-probe docs. Verified: quaestor owner reports the stub +
probes + test framework UNOWNED (test binaries); the exercised surfaces all
carried; `SYS_SPAWN_WITH_FDS`(=23) has its own live reference doc (62).
