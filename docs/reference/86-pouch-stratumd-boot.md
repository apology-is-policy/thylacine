# 86 — pouch-stratumd-boot: running stratumd in Thylacine [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-pouch-stratumd-boot-doc-absorb`).
The Phase 5/6 sub-chunk 16 (16a/16b/16c) that brought the cross-compiled stratumd
into the live boot path: joey spawns stratumd, stratumd mounts a real pool over
its in-process virtio-blk driver, the kernel 9P client connects to stratumd's
`/srv` socket, joey mounts `/sysroot`, ramfs pivots, and the stub is retired —
closing Phases 5 and 6. This was a chunk narrative; its as-built content lives,
code-verified and current, distributed across the vault. The boot sequence itself
has a single dedicated home:

- **the bringup spine** — spawn stratumd, wait for its `/srv` socket to bind
  (the readiness signal), attach, mount `/sysroot`, pivot, retire the stub — plus
  the stratumd-as-driver architecture the chunk chose in-session:

      vault/system/stratum/sub-stratum-boot.md   (audit: hard)  <- PRIMARY

- **the in-process block driver** (`bdev_thylacine` — Stratum drives virtio-blk
  itself, granted `CAP_HW_CREATE`):

      vault/system/stratum/sub-stratum-bdev.md   (audit: hard)

The kernel-side surfaces the chunk introduced or exercised:

- **joey's spawn orchestration** (spawning stratumd, the pivot sequencing):
  `vault/system/kernel/boot/sub-kernel-joey.md` (audit: hard)
- **the spawn ABI** — `SYS_SPAWN_FULL_ARGV` argv pass-through and the
  `CAP_HW_CREATE` grant: `vault/system/kernel/entry/sub-kernel-syscall-dispatch.md`,
  `vault/system/kernel/execution/sub-kernel-exec.md`,
  `vault/system/kernel/security/sub-kernel-caps.md` (all audit: hard), with the
  pouch side in `vault/system/boundary/pouch-seam/sub-pouch-process.md`
- **the SrvConn + `kernel_attached` gate + `SYS_ATTACH_9P_SRV`** (the 16c
  integration): `vault/system/kernel/srv/sub-kernel-srvconn.md` (audit: hard)
- **the 9P srvconn transport + client + dev9p**:
  `vault/system/kernel/ninep/sub-kernel-ninep-client.md`,
  `sub-kernel-ninep-transport.md`, `sub-kernel-ninep-dev9p.md` (all audit: hard)
- **`SYS_PIVOT_ROOT` + `/sysroot` mount + I-1 namespace isolation**:
  `vault/system/kernel/namespace/sub-kernel-territory.md` (audit: hard)
- **the ramfs stub** it pivots away from:
  `vault/system/kernel/devices/sub-kernel-content.md` (audit: hard)

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Clean multi-redirect, zero fold.** This 2189-line doc was a chunk narrative
  (16a load-probe -> 16b argv/block/cap -> 16c 9P-attach/pivot); every load-bearing
  atom was verified present in a fresh audit:hard dossier — the boot ordering and
  the `/srv`-socket-is-the-readiness-signal in sub-stratum-boot (52 stratumd/joey
  hits), the `kernel_attached` gate in sub-kernel-srvconn (70 hits), `pivot_root` +
  I-1 in sub-kernel-territory, the argv pass-through and `CAP_HW_CREATE` across the
  spawn/caps dossiers. The Stratum `src/` code the doc cites is carried by the
  vault's own **stratum area** (sub-stratum-boot / sub-stratum-bdev), not out of
  scope. Nothing was owed a fold; the dossiers are the current source of truth.
  Zero code change.
