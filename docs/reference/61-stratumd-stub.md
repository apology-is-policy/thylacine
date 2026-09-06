# 61 — /stratumd-stub — userspace 9P responder (P5-stratumd-stub-bringup) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-stratumd-stub-doc-absorb`).
This is a **test-scaffold arc doc**: the P5-stratumd-stub-bringup arc (a-e2) that
proved a *userspace* process can be the 9P responder (the production shape for
`stratumd-system`), and along the way introduced two real syscalls. No dossier
owns a test binary; the production surfaces the arc introduced and exercises are
each owned elsewhere.

**The test scaffold** — the stub responder + its three client probes + the
kernel test framework — is UNOWNED (a test binary has no dossier home; the same
disposition as `attach-probe` and `u-test`). Named here as the records they are:

- `usr/stratumd-stub/stratumd-stub.c` — the userspace 9P responder (Tversion /
  Tattach / Twalk / Tlopen / Tread / Tclunk over a one-file synthetic FS
  `/hello`; a 16-slot per-session fid table; inlined byte-order helpers because
  `libt` has no libc).
- `usr/attach-probe`, `usr/stub-fs-probe`, `usr/stub-walk-probe` — the three
  mirror-image clients (handshake-only; raw-wire walk/open/read; kernel-9P-client
  walk_open + chroot).
- `kernel/test/test_stratumd_stub.c` — the `userspace.stratumd_stub_*_round_trip`
  framework (two userspace Procs wired via two pipe pairs, both reaped).

**The production surfaces the arc exercises**, each with its owning dossier:

- the **userspace-server 9P path** it validates end-to-end — `SYS_ATTACH_9P`, the
  dev9p Dev vtable, and the client dispatch:

      vault/system/kernel/ninep/sub-kernel-ninep-attach.md
      vault/system/kernel/ninep/sub-kernel-ninep-dev9p.md
      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

- the **transport pipes + the EOF cascade** the refcount discipline rides (the
  transfer-not-bump handle install; last-drop `devpipe_close` → `write_eof`
  propagation that makes the stub see EOF and exit):

      vault/system/kernel/ipc-wake/sub-kernel-pipe.md

- **fd-inheritance spawn** (`SYS_SPAWN_WITH_FDS`, the production shape from
  sub-chunk b) — the positional `fd_list[i]` → child fd `i` inheritance, and the
  spawn/exec handler:

      vault/system/boundary/pouch-seam/sub-pouch-process.md
      vault/system/kernel/execution/sub-kernel-exec.md

  (its own dedicated syscall reference is `docs/reference/62-sys-spawn-with-fds.md`.)

- the **joey boot-path orchestration** — `do_stratumd_stub_bringup` (pipe + spawn
  + attach + mount + unmount + walk_open + read on every boot, so a regression in
  those surfaces is a boot-time signal):

      vault/system/kernel/boot/sub-kernel-joey.md

- **`SYS_WALK_OPEN`** (introduced e1 — the single-component walk-through-mount
  primitive `spoor_clone` + `dev->walk` + `dev->open` + `handle_alloc`, and its
  load-bearing walk-failure cleanup where the clone's `aux` is still a *shallow*
  copy of the source's so a naive `spoor_unref` would clunk the source's fid
  through the shared pointer):

      vault/system/kernel/namespace/sub-kernel-stalk.md
      vault/system/kernel/namespace/sub-kernel-spoor.md
      vault/system/kernel/namespace/sub-kernel-dev.md

- **`SYS_CHROOT` + `Territory.root_spoor` + the `SYS_WALK_OPEN_FROM_ROOT`
  sentinel** (introduced e2 — the v1.0 territory-root pivot; refcounted, cloned
  across rfork, dropped at Territory destruction):

      vault/system/kernel/namespace/sub-kernel-territory.md

  (its own dedicated syscall reference is `docs/reference/77-sys-chroot.md`.)

**What this file got WRONG or MISSED by the time it was absorbed:**

- **`pivot_root` is BUILT, not "v1.x deferred".** This doc's Status table and
  caveat 3 say `SYS_PIVOT_ROOT` / `SYS_UNCHROOT` is a v1.x follow-up and "chroot
  is one-way at v1.0." `sub-kernel-territory` documents `territory_pivot_root` +
  `SYS_PIVOT_ROOT` as present now — it differs from chroot only in requiring an
  existing `root_spoor` (it refuses `-1` without one). The territory dossier even
  records that the reference-doc-era treatment was stale on exactly this point.
- **The chroot-in-joey deadlock is a consequence, not a unique atom.** This doc's
  reasoning for running the pivot test in the short-lived `/stub-walk-probe` (a
  chroot in the long-running init holds a `root_spoor` ref past `t_close(attach_fd)`
  → the stub never sees EOF → `t_wait_pid` deadlocks) falls straight out of the
  refcount discipline the dossier already documents: `root_spoor` holds its OWN
  ref for the Territory's whole life, at five matched sites, dropped only at
  final-release, independent of any handle-table fd. Derivable there; kept in the
  arc only as the reason the test vehicle is a child Proc.
- **The walk-failure `aux` shallow-copy trap is covered in depth elsewhere** —
  `sub-kernel-spoor` (the deliberately-shallow clone + the failure-path unwind),
  `sub-kernel-dev` (the partial-walk clone-still-shallow-shared exit), and
  `sub-kernel-ninep-dev9p` (`fid_owned`, partial-walk as a hard failure).
- **Test counts are era-frozen** (423→424 … 527→533 across sub-chunks); the
  current suite is far larger. The content is distributed across the dossiers
  above; the two syscalls the arc introduced have their own dedicated reference
  docs (62, 77) that will absorb on their own.
