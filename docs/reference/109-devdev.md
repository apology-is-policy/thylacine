# 109 — devdev: the /dev char-device directory + the I-27 gate-at-namespace-open

**Status**: as-built at #57b (`71d5254`; scripture `b0d0fb4`). The container-keystone
namespace layout's last half. Audited (the #57b focused round).

**Naming**: `devdev` is the C identifier (the `dev` + role convention, where the role
*is* the device directory); the dc is `'d'`, the name is `"dev"` (the universally-expected
path). No thematic rename — `/dev` is what every program expects.

---

## Purpose

`devdev` presents the kernel char devices under a single mountable directory at `/dev`
(ARCHITECTURE.md §9.4). It is the Plan 9 `#c`-as-a-directory model — one aggregating
directory Dev serving many named single-file leaves — realized in Thylacine's established
one-Dev-many-leaves pattern (the same shape as `devctl` (§33), `devproc` (§32), `devcap`).

The leaves:

| Leaf | qid kind | Behavior | Gated? |
|---|---|---|---|
| `null`    | `DEV_KIND_NULL`    | read → EOF (0); write → consumed | no (world-rw) |
| `zero`    | `DEV_KIND_ZERO`    | read → NUL-fill; write → consumed | no |
| `full`    | `DEV_KIND_FULL`    | read → NUL-fill; write → -1 (full disk) | no |
| `random`  | `DEV_KIND_RANDOM`  | read → `kern_random_bytes` (CSPRNG); write → consumed | no |
| `urandom` | `DEV_KIND_URANDOM` | alias of `random` (POSIX compat) | no |
| `cons`    | `DEV_KIND_CONS`    | read → console RX drain; write → UART | **yes (I-27)** |
| `consctl` | `DEV_KIND_CONSCTL` | read → EOF; write → -1 (no modes at v1.0) | **yes (I-27)** |

The trivial leaves (`null`/`zero`/`full`/`random`/`urandom`) are world-rw and ungated —
the same on every Unix. `cons`/`consctl` are the console, gated at open (below).

---

## The I-27 gate-at-namespace-open (the load-bearing mechanism)

Before #57b, the console had **no filesystem path**: it was reachable only via the
capability-gated `SYS_CONSOLE_OPEN` syscall, which checks `proc_is_console_attached(p)`
(`kernel/syscall.c::sys_console_open_handler` — the A-5a-F2 fix). #57b binds `/dev/cons` as
a walkable path; **the soundness obligation is that this must NOT create a second, ungated
front door to the console.**

The console is a single-reader resource: `cons.c`'s `g_cons.reader_busy` busy-guard makes a
second concurrent blocking read return -1 (the data Rendez is single-waiter). If
`open("/dev/cons")` were ungated, any EL0 Proc could become that single reader and steal the
getty's console input — and a passphrase typed at the login prompt would land in the thief's
read (exactly the A-5a-F2 break the syscall gate closed).

So `devdev_open` enforces the **same** gate the syscall does, for the `cons`/`consctl` qids:

```c
static struct Spoor *devdev_open(struct Spoor *c, int omode) {
    if (!c) return NULL;
    if (dev_kind_is_console((u32)c->qid.path)) {
        struct Thread *t = current_thread();
        if (!t || !proc_is_console_attached(t->proc)) return NULL;  // -> walk-open -1
    }
    return dev_simple_open(c, omode);
}
```

Properties:

- **The trusted-path gate now covers the namespace open, not just the syscall.** I-27 is
  preserved: `walk("/dev/cons")` resolves the name (Plan 9 shape), but `open` fails -1 for a
  non-console-attached caller. Only the console-attach holder (joey pre-relinquish / post-SAK
  corvus) can open it — exactly as via the syscall.
- **The gate is at OPEN, covering all subsequent read AND write.** A non-attached Proc cannot
  even reach the write path, so it cannot spoof console *output* either. (`read`/`write`
  require an opened fd; `bread` returns NULL and `bwrite` returns -1, so there is no
  open-bypassing I/O path.)
- **Fail-closed.** `current_thread() == NULL` or `t->proc == NULL` → deny (the gate's `!t ||
  !proc_is_console_attached(t->proc)`, and `proc_is_console_attached(NULL)` returns false).
- **Orthogonal to perms.** The gate is a console-attach *state* check, not an rwx check; the
  leaf perms stay 0666 (`devdev.perm_enforced == false`), mirroring `/ctl/kernel-base`'s
  `CAP_HOSTOWNER` read-gate being orthogonal to its perms (#57a F1).
- **Two gated entry points = defense-in-depth.** `SYS_CONSOLE_OPEN` keeps its gate (the
  bootstrap needs the syscall before `/dev` is mounted); `devdev_open` is the namespace
  front-door's gate. Retiring `SYS_CONSOLE_OPEN` for the path alone is a v1.x seam.

The user shell never opens `/dev/cons`; it **inherits** the fd login opened while attached
and passed as stdio. So gating the namespace open does not regress the session's console I/O.

---

## One console implementation, two front doors

`cons.c` exposes a public API shared by both console front doors:

```c
long cons_input_read(void *buf, long n);     // the blocking RX-ring drain
long cons_output_write(const void *buf, long n);  // forward each byte to the UART
```

`devcons` (the `SYS_CONSOLE_OPEN` syscall Dev, dc='c') and `devdev`'s `cons` leaf BOTH call
these. The single-reader busy-guard, the INTERACTIVE-band promotion (RW-11 SA-1b), and the
death-interruptible (#811 `SLEEP_INTR`) read all live in `cons_input_read` and are reused, not
forked. Because both doors share the SAME `g_cons.reader_busy`, the console is bounded to ONE
reader *across both doors* — there is no second reader path that could race the first.
`devcons_read`/`devcons_write` are thin wrappers around the shared API (behavior-identical to
the pre-#57b code).

**Deliberate revoke-semantics asymmetry between the two doors (the #57b audit F2 note).** The
two front doors gate at different points: `SYS_CONSOLE_OPEN` (devcons) gates only at OPEN, so
an already-opened console fd *survives* a later console-attach revoke (a SAK) — the inherited
session stdio relies on this (joey opens while attached, hands the fd to login, then
relinquishes). `devdev`'s `/dev/cons` gates at OPEN **and** at every read/write (the SA-1
fix), so an opened `/dev/cons` fd *stops working* the instant the caller de-attaches. The
every-I/O gate is the stricter, more-I-27-correct semantic (it closes the fd-outlives-revoke
window) and is required to close the O_PATH bypass; it does not regress the session stdio
(that path is a devcons fd, dc='c', not devdev). A future consumer that opens `/dev/cons`
expecting POSIX "the fd survives a privilege change" semantics will be surprised — use the
inherited stdio fd (devcons) for a delegated console, not a freshly-opened `/dev/cons`.

---

## The reuse-nc walk (mount-cross correctness)

`devdev_walk` is dual-mode (the #57a lesson): when `nc != NULL` it reuses the caller's
pre-clone Spoor and returns it as `wq->spoor` (a 0-element walk yields `nqid == 0`), the shape
`clone_walk_zero` needs to cross the `/dev` mount; when `nc == NULL` it `spoor_clone`s the
input (the legacy direct-call shape used by the kernel tests). Without the reuse-nc mode a
mounted devdev would be unreachable through `stalk` (`wq->spoor != nc` → reject) — the same
bug `devramfs`/`devctl`/`devproc` carried before they were mounted. On the `spoor_clone`-fail
path `walkqid_free(wq)` is called before returning NULL (no leak). The pattern is byte-for-byte
the audited `devctl_walk`.

---

## The mount (the /srv idiom)

`/dev` is grafted with the same idiom as `/srv` + `/proc` + `/ctl` + `/bin`:

- **Kernel boot mount** (`kernel/joey.c`): `joey_mount_static_dev(kt, &devdev, "dev", 3)` in
  the kproc boot namespace, onto the `dev` synthetic devramfs mount-point dir
  (`RAMFS_QID_SYNTH_DEV`, 0555 SYSTEM-owned). Inherited by every Proc via `territory_clone`.
- **Post-pivot re-graft** (`usr/joey/joey.c`): the long-running init grabs a pre-pivot
  `t_open("/dev", O_PATH)` handle (crossing the mount → the devdev root), then post-pivot
  `mkdir /dev` + `t_mount("/dev", 4, dev_dev_h, MREPL)` onto the pivoted disk root.

### Mount-table sizing (PGRP_MAX_MOUNTS 8 → 12)

joey is the high-water mark: the kproc boot namespace mounts `srv`/`proc`/`ctl`/`dev` (4) and
the init re-grafts `srv`/`bin`/`proc`/`ctl`/`dev` (5) onto the pivoted disk root = **9 live
entries**. The pre-pivot kernel mounts **orphan** after pivot (their devramfs mount points
become unreachable from the disk root) but remain in the per-Territory mount table and
propagate to every post-pivot child via `territory_clone`'s deep-copy — so the count is
pre+post per re-grafted dir. `PGRP_MAX_MOUNTS` was bumped 8 → 12 (the `PgrpMount` /
`Territory` `_Static_assert`s are symbolic in the constant, so they auto-adjust). 12 leaves
headroom for `/net` (Phase 8). A **pivot-time GC of dead mounts** would halve joey's count —
tracked as a seam (task #80; it would touch `territory_pivot_root` semantics, which currently
deliberately "does not touch mounts").

---

## Tests

- `kernel/test/test_devdev.c`:
  - `devdev.bestiary_smoke` — dc='d', name "dev", registered.
  - `devdev.attach_returns_dir` — QTDIR root, qid.path 0.
  - `devdev.walk_to_each_leaf` — all 7 leaves resolve to QTFILE.
  - `devdev.walk_unknown_misses` — nqid 0.
  - `devdev.trivial_leaves` — null EOF/consume, zero/full NUL-fill, full write -1, random
    two-reads-differ (CSPRNG), urandom alias; all open ungated.
  - **`devdev.cons_gate`** — the I-27 deny/allow: a non-console-attached open of cons AND
    consctl fails NULL; a console-attached open of cons succeeds. (Captures results under
    controlled attach state, restores the test thread's attach bit BEFORE asserting, so a
    failing assert cannot strand the bit — the devctl kernel-base temp-elevate pattern.)
- `kernel/test/test_namespace_layout.c::test_namespace_layout_proc_ctl_cross` gains a `/dev`
  mount + `stalk("dev/null")` cross step — proves devdev's reuse-nc walk through
  `clone_walk_zero` (a mounted Dev whose walk ignored `nc` would be unreachable here).
- `kernel/test/test_devramfs.c` — the paginated-readdir count (`files + 4 synth dirs`) and the
  synth-dir-empty test updated for the 4th synth dir (`dev`).
- **Boot E2E**: the kernel boot mount + the post-pivot re-graft run on every boot (`joey:
  /dev mounted`), and the login E2E exercises the namespace.

---

## Error paths

- `devdev_open` on a `cons`/`consctl` qid without console-attach → NULL → walk-open returns -1.
- `devdev_read`/`write` with `c == NULL` / `buf == NULL` (read) / `n < 0` → -1.
- `devdev_read` on the root dir (`DEV_KIND_ROOT`) → -1 (readdir deferred, matches devctl).
- `/dev/full` write → -1 (the full-disk semantic; collapses to EIO at the syscall layer at
  v1.0, per the devfull caveat).
- `/dev/random` read when the CSPRNG is unseeded → `kern_random_bytes` returns -1 (fail-closed).
- `devdev_create` → NULL (no create on /dev at v1.0); `devdev_bread` → NULL; `devdev_bwrite`,
  `devdev_wstat`, `devdev_stat` → -1.

---

## Known caveats / footguns

- **`consctl` is present + gated but carries no v1.0 modes** (read EOF / write -1). There is no
  termios / line-discipline yet — that is LS-8 (#952). The path + the gate exist now so LS-8
  lands its mode-control content into an already-secured, already-named file. A program that
  writes a mode string to `/dev/consctl` today gets -1.
- **`/dev/random` writes are consumed without stirring the pool** (return n). The standalone
  `devrandom` Dev stirs on write (`rng_rekey_locked`), but that Dev is no longer reachable by
  any path; pool-stir-on-write via `/dev/random` is a v1.x ergonomic (the CSPRNG reseeds from
  virtio-rng on its own cadence). No security loss — stirring is a contribution, not required.
- **The orphaned pre-pivot mounts** (task #80) waste 4 mount slots per Territory and propagate
  to every child. Harmless at v1.0 (the 12-cap holds through /net) but a pivot-time GC would
  reclaim them.
- **The deferred /dev entries** (`mem`/`tty`/`ptmx`/`pts`/`fb`/`video`/`janus`, etc.) land with
  their servers (Phase 8 / the container runner #70 / LS-8). `/dev/mem` is `CAP_RAW_MEM`-gated
  when it lands.

---

## Spec cross-reference

No new spec module (per the 2026-05-23 spec-to-code broadening). The mount/cross mechanics
inherit `territory.tla` (mount DAG cycle-freedom, I-3) — #57b changed only the capacity
constant `PGRP_MAX_MOUNTS`, not the modeled mechanism, so no buggy-cfg re-run is owed. I-27 is
prose-validated (IDENTITY-DESIGN.md §9.8) + the `devdev.cons_gate` test.

## See also

- `docs/reference/32-devproc.md`, `33-devctl.md` — the sibling directory Devs.
- `docs/reference/106-random.md` — the CSPRNG behind `/dev/random`.
- `docs/reference/104-stalk.md` — the resolver + the reuse-nc walk contract.
- `docs/IDENTITY-DESIGN.md` §9.8 — the I-27 trusted-path design (the gate-at-namespace-open).
- `docs/ARCHITECTURE.md` §9.4 (/dev layout), §28 I-27, §25.4 (the audit-trigger row).
