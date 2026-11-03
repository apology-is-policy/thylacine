---
id: chg-2026-11-02-b3h3-b3h4-cached-locks-pouch
type: chg
title: "B-3h3 and B-3h4: a cached open reaches its file; Pouch fcntl locks and flock"
date: 2026-11-02
arc: arc-boosty
commits: ["dcdfd037a", "c2eeda2cc", "ae1f84374", "4b0bdd849", "2a0844874", "676fbd9a7", "adfe466d6", "b6d39f97e", "6c085a1a3", "52ecb0a77"]
touched:
  - sub-kernel-ninep-dev9p
  - sub-kernel-larder
  - sub-kernel-handle
  - sub-kernel-filelock
  - sub-kernel-syscall-abi
  - sub-libthyla-rs
  - sub-pouch-fs
  - sub-pouch-seam
  - sub-stratum-boot
  - abi-errno
established: []
closed: [seam-co-fidless-wstat]
opened: []
mirrors-checked: [docs/ARCHITECTURE.md, docs/FID-LIFECYCLE-DESIGN.md, docs/manual/22-file-locks.md, docs/manual/20-file-times.md]
depth: skeletal
created: 2026-11-02
---
B-3h3 gives a cached open a way to reach its file, and B-3h4 gives programs built
with Pouch the POSIX locks ([[dec-2026-10-29-b3h-file-locks]], operator votes 34-35).
A strict cached open keeps the fid its revalidating walk binds, as vote 20 decided.
A loose one, on the system mount, binds nothing at open, which keeps vote B1's
removal of about 1604 wire walks per run. On its first lock, wstat or Loom
registration it walks its kept names again and accepts only the same number and
generation, else ESTALE, as ARCH 9.6.11 had specified. The operator confirmed that
choice on 2026-11-02. Once a take covers it, a cached open reads through a read fid
and stops serving its snapshot, and a Loom ring addresses that fid, which also fixes
the path where a ring had no fid to address. Pouch's 0065 adds `fcntl` locks with
Linux's range rules and refusal order, and `flock`. `/bin/pouch-hello-locks` proves
them at every boot, boot-fatal, over Stratum's table.

The B-3h2 lease's evidence, for the record, since that change note's body is closed
(2026-11-02 08:48-08:58Z, Stratum `579f142`): bake rc 0, suite 2111/2111 on the base
and on the restored tree, Stratum TLC 10/10, mutants F2, F3, F4b and F5-F13 each red
at their own leg. F1 and F4a did not run and are owed to this chunk's lease. The red
leg of the lease before it, `filelock.stale_entry_retired`, was the fake's: a
diagnostic of the original test printed that the fresh mint had taken the dead
owner's address (fix `a22aad62a`).
