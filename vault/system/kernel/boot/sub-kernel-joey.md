---
id: sub-kernel-joey
type: sub
parent: moc-kernel-boot
title: "joey — the kernel-to-userspace handoff, and where the boot namespace is built"
code:
  - kernel/joey.c
  - kernel/include/thylacine/joey.h
audit: hard
guarded-by: [inv-i27]
validated-by: [prose, gate-smp, gate-interactive]
locks: []
abis: []
design:
  - "docs/ARCHITECTURE.md section 5.1"
  - "docs/CORVUS-DESIGN.md section 3"
created: 2026-09-06
updated: 2026-09-06
---
## Purpose

The kernel side of init. `boot_main`'s last real step is `joey_run`, which
builds the boot namespace, loads the first userspace binary (`/joey`) from the
initrd, rforks it as the first user Proc, and waits on it. Everything a userspace
Proc later inherits — a root to walk from, `/srv`, `/proc`, `/ctl`, `/dev`,
`/hw`, `/env` — is grafted here, and the trust roots the whole login chain rests
on (the console anchor, the capability delegate, the orphan reaper) are stamped
here in the child's own context before it reaches EL0.

The userspace half — the long-running supervisor that pivots root, brings up
stratumd and corvus, and runs the getty loop — is [[sub-stratum-boot]]'s
`usr/joey/joey.c`. This dossier is the kernel-resident kproc that hands off to
it.

## Contract

`joey_run` runs exactly once per boot (a static one-call guard extincts on a
double call — the v1.0 single-use invariant). It reads `/joey` from the initrd
cpio by name; a missing, zero-size, or over-`EXEC_FILE_MAX` blob is boot-fatal,
as is any failure in the namespace construction or the rfork. There is no
degraded mode — a boot that cannot build its namespace or start init is
unrecoverable, and each failure extincts with a message a boot log can diagnose
from.

## Mechanism

### The init blob is an exec-window transient

The cpio bytes are only 4-byte aligned, but `elf_load` casts an `Ehdr` and
requires 8. So `joey_run` copies the blob into an exact-size `kmalloc`'d buffer
(alignment is inherent: `kmalloc`'s large path returns a page-aligned KVA) and
the child frees it the moment `exec_setup` returns — on both arms, because
`exec_setup` fully consumes the blob (the ELF image is scalars-only and segment
bytes are copied into Burrows, so no pointer into the blob survives). The
predecessor was a fixed BSS array bumped six times as boot probes accumulated,
every KiB permanently resident holding a stale copy of an ELF already mapped
into joey's address space; the heap copy costs those bytes only for the exec
window, and the release is logged on every boot so the non-residency is visible.
`KP_ZERO` keeps the rounded-up tail deterministic so a future bounds regression
is content-independent rather than buddy-garbage-sensitive.

### The boot namespace is the /srv idiom, generalized

`joey_root_kproc_at_devramfs` stamps the kproc Territory's root at the devramfs
root (idempotent — the test harness roots it earlier), giving joey and every
descendant a `FROM_ROOT` base and the namespace `SYS_SPAWN` binary resolution
walks. Then `joey_mount_static_dev` grafts each kernel Dev's root onto its
synthetic devramfs mount-point dir: the mount-point is resolved **without
crossing** (`STALK_MOUNT`) so it keys on the synth dir's own identity, and
`MREPL` lets a re-run replace. The set is `/srv` (devsrv over the boot service
registry), `/proc` + `/ctl` (the introspection Devs), `/dev` (devdev — the
console front door and the trivial leaves), `/hw` (the DTB inventory), `/hw/pci`
(mediated PCI, mounted **after** `/hw` because `hw/pci` resolves by crossing the
`/hw` mount first), and `/env` (the per-Proc environment).

Every descendant inherits the whole set through `territory_clone`'s deep copy.
The mounts are keyed on devramfs synth dirs, so the root **pivot drops them**,
and the long-running userspace supervisor re-grafts them onto the pivoted disk
root ([[sub-stratum-boot]]). Each of these mounts widens *visibility*, never
authority — `/proc/<pid>/ctl` writes stay I-26 two-axis-gated and `/dev/cons`
stays I-27-gated at the front door regardless of namespace reachability.

### The trust roots are stamped in the child's own context, before exec

joey is rforked with `rfork_with_caps(CAP_ALL)` — plain `rfork` would give it
`CAP_NONE`, and then a spawn-with-caps AND would zero every child grant, so init
is deliberately the full-ceiling delegate that hands each child its role's
subset (I-2 is preserved structurally: the AND only reduces). Inside
`joey_thunk`, before `exec_setup`, the child stamps its own Proc's flags — which
is race-free because it is joey's own thread setting joey's own Proc:

- **console-attached** — joey is the local-console trust anchor, the root of the
  login chain (I-27). It is the sole console-attached Proc at v1.0; login
  extends the chain to the shells it spawns.
- **console-owner** — the target of the `interrupt` (Ctrl-C) note; cleared when
  joey exits so it never dangles, and the SAK grants console *attach* (not
  ownership) to corvus.
- **may-post-service** — the root of the service-posting chain (A-5b). joey
  relinquishes its console attach at the bringup→session boundary but must remain
  a *holder* of this bit so the getty loop's fresh `/sbin/login` instances can
  each be granted it; the bit is a perm-flag, never rfork-propagated, so holding
  it confers nothing on children automatically.
- **init** — the orphan adopter (the reparent target); stamped as a pointer, not
  a pid, because joey's pid is 1 only on a test-free boot.
- **the name** — joey execs from the boot blob, not a namespace Spoor, so it
  carries no resolved path; the name is set explicitly.

### The wait is by pid, not reap-any

`joey_run` waits with `wait_pid_for(pid, ...)` for **that** child, not a
reap-any scan. When joey exits early its daemon children reparent to kproc,
spliced onto the front of kproc's child list ahead of joey — so a reap-any would
reap an already-exited orphan *instead* of joey and destroy the one status worth
printing on a failed boot. joey is init and does not exit in normal operation,
so this returns only on failure, which is exactly when the diagnostic has to name
the right subsystem.

## Data structures

`struct joey_args` — the blob pointer and size, living on the boot-CPU stack for
the duration of `joey_run`. Ownership of the heap blob transfers to the child,
which frees it after `exec_setup`; the parent cannot be the freer because in
production it parks in `wait_pid_for` for the machine's lifetime.

## Concurrency

None owned. `joey_run` runs single-threaded on the boot CPU before the child
exists; the child stamps its own Proc's flags in its own thread context. The
namespace reads use `territory_root_ref` (atomic read-plus-ref under the
namespace lock) rather than a bare `root_spoor` read, keeping the "no bare
root_spoor read outside territory.c" invariant total even though the kproc
Territory has a single boot-time mutator.

## Invariants enforced

**I-2** (fork-grantable caps monotonically reduce; no vault note yet, referenced
here in prose) — joey is the capability delegate root: `rfork_with_caps(CAP_ALL)`
gives it the full ceiling so it can grant each child its subset, and the AND in
the grant path only reduces. The `may-post-service` bit it holds is a perm-flag,
never rfork-propagated, so it confers nothing automatically.

**[[inv-i27]]** — joey is the local-console trust anchor: it stamps
console-attached and console-owner as the root of the login chain, and
relinquishes the attach at the bringup→session boundary. The SAK later grants the
*attach* to corvus, never ownership. Its console open is the boot end of the
trusted path [[sub-kernel-devdev]] and [[sub-kernel-cons]] gate.

Composes **[[inv-i1]]**: the boot namespace it builds is inherited by every
descendant through `territory_clone`, and every mount widens visibility rather
than authority.

## Error paths

Every step is boot-fatal: a missing/zero/oversize init blob, a `kmalloc`
failure, a failed Territory root, any of the seven mounts, a failed rfork, and a
non-zero or wrong-pid child exit each extinct with a specific message. On
`exec_setup` failure the child `exits("fail-exec")` so the parent's wait observes
a non-zero status rather than a hang.

## Performance

Irrelevant — a one-shot boot path. The one figure worth stating is the negative
one: the init blob no longer costs resident kernel memory (it is freed at the
exec window), which is the whole point of the #85 transient.

## Prosecution

- **The trust-root stamps must stay in the child's own context, before exec.**
  Stamping console-attach, owner, may-post-service and init from joey's own
  thread on joey's own Proc is what makes them race-free; moving them to the
  parent reintroduces a cross-Proc write.
- **The rfork must stay `CAP_ALL`.** A plain rfork zeroes init's ceiling and every
  child grant ANDs to nothing — init cannot be the delegate root without the
  ceiling.
- **The wait must stay by-pid.** A reap-any races orphan adoption and reaps the
  wrong Proc, destroying joey's real exit status on the boot where it matters.
- **`/hw/pci` must mount after `/hw`.** It resolves by crossing the `/hw` mount
  first; reversing the order makes the nested mount-point unreachable.
- **Each mount widens visibility, not authority.** A new boot mount must keep its
  authority gate at the Dev, not lean on namespace reachability.
- **The one-call guard must stay.** `joey_run` is single-use; a supervisor
  refactor re-runs by re-execing, never by re-calling it.

## Seams

- **`joey_run` is one-call, wait-and-extinct.** The long-running supervisor —
  the pivot, stratumd, corvus, the getty loop — is userspace joey
  ([[sub-stratum-boot]]); this kproc keeps its one-shot shape until nothing needs
  it to.
- **The boot mounts are dropped by the pivot** (they key on devramfs synth
  dirs), and re-grafted post-pivot by userspace joey. A pivot-time GC of the
  orphaned pre-pivot mounts is a recorded seam ([[seam-80-pivot-orphan-mounts]]).

## Caveats

- **The console owner is cleared at joey exit** (`proc_become_zombie_locked`), so
  it never dangles — but joey does not exit in normal operation, so this is the
  failure-path guarantee, not the steady state.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
