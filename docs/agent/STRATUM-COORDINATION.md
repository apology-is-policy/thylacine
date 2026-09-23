# Stratum coordination

> Moved verbatim from `CLAUDE.md` on 2026-09-23 (the CLAUDE.md trim). 
> Section headings keep their original levels.

## Stratum coordination

**Stratum is in scope: operate on it like it's your own (user-authorized 2026-05-29).** The Stratum tree at `~/projects/stratum/v2` (branch `thylacine-pouch-arm`) may be modified directly as part of Thylacine work -- root-cause, fix, test, and commit Stratum-side bugs (e.g. the `bdev_thylacine` virtio-blk port, the pouch boundary-line, the 9P server) without asking first. The Thylacine<->Stratum boundary is a single engineering surface for this project; a bug that surfaces in Thylacine but lives in Stratum gets fixed in Stratum. Standing constraints still apply on the Stratum tree: ASCII commit messages, no force-push, the user pushes (you commit), and `third_party/` stays byte-pristine. Stratum's own on-disk-format / wire-ABI breaks remain escalation-worthy (they ripple to the installer/upgrade path) -- but ordinary code fixes do not.

**Stratum v2 is feature-complete and shipping.** The POSIX surface (P8) and the 9P client interfaces (P9) both landed during 2026 Q1-Q2; Stratum exposes three concurrent ABIs that Thylacine Phase 5 binds to:

| Stratum ABI | Form | Stability | Thylacine consumer |
|---|---|---|---|
| **9P2000.L wire** | Unix socket (`stratumd`'s FS socket) or TCP | Stable; matches Linux v9fs | Thylacine kernel 9P client (the primary integration; ~Phase 5 §1) |
| **`libstratum-9p` C ABI** | `libstratum_9p_client.a` + `include/stratum/9p_client.h` | Stable per `stratum/v2/docs/ARCHITECTURE.md §10.2` | Userland tools written against Stratum's client lib (e.g., `stratum-fs-e2e`); optional for Thylacine — we'd typically reach the same FS via the kernel's mounted-9P-tree |
| **`libstm_fs` in-process C** | `libstm_fs.a` (UNSTABLE; bound to `STM_UB_VERSION`) | NOT stable | NOT consumed by Thylacine. Reserved for in-process bypass; per OS-INTEGRATION.md "always go through 9P." |

The integration target is the 9P2000.L wire surface with Stratum extensions. Per `stratum/v2/docs/OS-INTEGRATION.md`, the recommended deployment is the Linux v9fs-equivalent model: `stratumd` is a userspace daemon (one process per pool), bound to a Unix socket; the OS kernel speaks 9P over that socket; the Stratum-side server multiplexes per-connection fid namespaces. Thylacine consumes this with its own kernel 9P client — the v9fs-equivalent at the Thylacine layer.

Stratum extensions Thylacine speaks (per `stratum/v2/docs/REFERENCE.md` 9P chapter):
- `Tsync` — explicit sync barrier on a fid.
- `Treflink` — single-dataset reflink (cross-dataset is gated on Stratum's rekeying primitive; deferred upstream).
- `Tbind` / `Tunbind` — per-connection subvolume composition (Stratum-side territory; complements Thylacine's per-Proc territory at the kernel level).
- `Txattrwalk` + xattr family — POSIX xattrs end-to-end.
- 9P2000.L core: `Tlopen`, `Tlcreate`, `Tsymlink`, `Tmknod`, `Trename`, `Treaddir`, `Tstatfs`, `Tgetattr`, `Tsetattr`, `Treadlink`, `Tlock`, `Tgetlock`, `Tlink`, `Tmkdir`, `Trenameat`, `Tunlinkat`.

Boot path discipline (per Stratum OS-INTEGRATION.md §4):
- `.key` sidecar lives separately from the pool block device; the separability is the second security factor. Initramfs unwraps and feeds it to `stratumd`; never embed in the pool header.
- `stratumd` owns the block device exclusively after the initramfs hands it over.
- The presence of the FS Unix socket is the readiness signal — don't read it before it binds.
- Failure modes the boot path must surface: `STM_ECORRUPT` (Merkle mismatch — refuse to boot), `STM_EBADTAG` (AEAD MAC failure), `STM_EBADKEY` (wrong `.key`), `STM_EWEDGED` (fs marked wedged at prior unmount).

Admin surface coordination:
- `/ctl/` is itself a synthetic 9P filesystem served by `stratumd` (typically on a second Unix socket). Topology is documented in `stratum/v2/docs/reference/22-ctl.md` (pools / datasets / snapshots / scrub / events / metrics / Prometheus).
- Thylacine's `/ctl` (Phase 4 P4-D — `kernel/devctl.c`) is a *separate* kernel admin surface for OS-level introspection. The Stratum `/ctl/` is consumed BY Thylacine userspace as just another mounted 9P tree (typically at `/srv/stratum-ctl/`).

POSIX surface available from Stratum (Thylacine consumes via 9P; no per-feature work needed unless the kernel needs to mediate):
- Live in v2.x: inodes + dirents + xattrs + file seals (`F_SEAL_*`) + advisory locks (`flock` / OFD locks) + `statx` + `name_to_handle_at` + `copy_file_range` (whole-file MVP) + `reflink` (single-dataset) + `rename` family (`RENAME_EXCHANGE` / `_WHITEOUT` / `_NOREPLACE`) + `fallocate` (PUNCH/COLLAPSE/INSERT/ZERO/UNSHARE) + symlinks + hard links + `O_TMPFILE` + `posix_fadvise` + inline-data optimization + snapshots (create/delete/hold/release/rollback).
- Deferred upstream (Thylacine accommodates as Stratum lands): cross-dataset reflink, `inotify`/`fanotify`, FS-verity API, `O_DIRECT`, OTLP exposition, learned tier policy, content-defined chunking.

Coordination rules:
- Thylacine Phases 1-4 already proceeded with no Stratum dependency. Phase 5 entry depends on Stratum v2 being available, which it now is.
- Phase 5+ stays within Stratum's stable ABI envelope. Any breaking Stratum on-disk format bump (`STM_UB_VERSION`) gets reflected in Thylacine's installer / upgrade path; Stratum's ABI compatibility envelope (mount-side compat for at least one major version) covers normal in-place upgrades.
- Stratum's audit-trigger surfaces remain Stratum's responsibility; Thylacine's audit covers the OS-side integration (9P client, mount path, key handling, `/ctl/` consumption).
- Slate (Plan-9-shaped TUI daemon also served as a 9P filesystem) is shipped by Stratum at `stratum/v2/src/slate/`. Thylacine's Halcyon (Phase 8) can adopt slate directly OR build an equivalent. The adoption story is documented in OS-INTEGRATION.md §17 — Halcyon's design pass should weigh it.
- **Stratum host-side sanitizers (ASan / LeakSan / TSan) run on a DISPOSABLE GCP Linux VM, never locally — both are BROKEN on the macOS dev host** (TSan SIGSEGVs inside `__tsan::InitializePlatform` before any user code, lldb-proven 2026-07-10; ASan hangs producing zero output). Recipe + cost discipline (spot `e2-standard-4` ~$0.04/h, boot-disk-only ≈ $0.02–0.05/run, create → run → **tear down immediately**, batch queued payloads onto one VM, propose-then-execute for mutating `gcloud` ops): `~/projects/stratum/CLAUDE.md` "Sanitizers" section + the stratum session memory `reference_gcp_compute.md`. (The Thylacine KERNEL is unaffected — it has no libc/sanitizer runtime; `tools/build.sh` UBSan + the multi-boot SMP gate remain its witnesses.)

Stratum's repo is at `~/projects/stratum/v2/` (use the v2 path — v1 was the earlier prototype). Reference docs of interest:
- `stratum/v2/docs/OS-INTEGRATION.md` — the integration manual (canonical for Thylacine Phase 5+).
- `stratum/v2/docs/REFERENCE.md` and `stratum/v2/docs/reference/20-9p.md` — as-built 9P semantics.
- `stratum/v2/docs/REFERENCE.md` 22-ctl chapter — admin surface trust boundary.
- `stratum/v2/docs/SLATE-DESIGN.md` — slate schema contract (Halcyon-side input).

---
