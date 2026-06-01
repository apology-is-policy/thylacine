# 102 -- The legate: bounded clearance elevation

As-built reference for the A-4a legate mechanism (IDENTITY-DESIGN.md §9.8,
invariant I-25). Landed A-4a-2b. Cross-references: `75-devcap.md` (the `cap`
device this generalizes), `95-identity.md` (durable identity, unchanged by
elevation), `99-fs-permission.md` (the `perm_check` axis the new caps feed),
`14-process-model.md` (rfork's `~CAP_ELEVATION_ONLY` strip).

## Purpose

A **legate** is a durable user (its `principal_id` is UNCHANGED) granted extra
capabilities for a bounded *scope* -- a process subtree plus an optional time
bound. It is Thylacine's answer to "elevate for a task, then fully de-escalate,"
the capability-axis counterpart of sudo, but stronger: when the scope ends, **no
elevated process outlives it** (sudo's backgrounded jobs survive; a legate's do
not).

Elevation is conferred through the `cap` device's two-phase grant (the same
factotum-shape as hostowner elevation, `75-devcap.md`): corvus -- the clearance
authority, the only holder of `CAP_GRANT_CLEARANCE` -- registers a pending
*clearance grant* against a target Proc's `stripes`; that Proc redeems its own
grant via `/cap/use`; the kernel stamps the cleared caps and records the scope.
There is **no local crypto** -- the kernel cap-stamp is the unforgeable
substrate, and corvus's `CAP_GRANT_CLEARANCE` is the trust root (it verified the
clearance's `auth_required` before registering).

## The v1.0 elevation model (read this first)

At v1.0 the clearance-grantable set (`CAP_GRANTABLE_CLEARANCE`) is
`CAP_DAC_OVERRIDE | CAP_CHOWN | CAP_KILL` -- **all elevation-only**, which
`rfork` strips (`~CAP_ELEVATION_ONLY`, A-4-pre). Consequence: only the legate
**ROOT** (the Proc that redeemed) holds the elevated caps; a scope **MEMBER** (an
`rfork` descendant) inherits the scope *tag* (`legate_scope_id`) but NOT the
elevated caps. Per scripture §3.1 "a clearance's caps that flow to a subtree are
the fork-grantable members of its set; the elevation-only members stay on the
root" -- and at v1.0 every clearance cap is elevation-only, so none flows.

This is load-bearing for understanding I-25:

- The privilege guarantee ("no *elevated* Proc outlives the scope") rests
  entirely on the ROOT. The root's elevation ends when (a) the root exits, or
  (b) its `valid_until` deadline passes (it self-terminates at its next EL0
  return).
- The member teardown sweep is the scripture-mandated *tidiness* guarantee (the
  whole episode's process subtree dies together). A member is never elevated, so
  a member that escapes the sweep (e.g. spawned racing the teardown walk) is a
  benign, UNELEVATED straggler with a stale scope tag that nothing acts on -- NOT
  an I-25 violation.

The v1.0 use case is therefore "a privileged agent/daemon holds the clearance and
performs the privileged operations itself," not "an elevated shell forks elevated
commands" (the latter would need a clearance whose caps are fork-grantable, a
v1.x consideration).

## Public API

```c
// kernel/devcap.c -- the cap-device clearance grant/redeem cores.

// Register a pending CLEARANCE grant (the 32-byte /cap/grant form). Gated on
// CAP_GRANT_CLEARANCE. cap_mask must be a non-empty subset of
// CAP_GRANTABLE_CLEARANCE; session_id nonzero + fits u32; target_stripes != 0.
// valid_for_ns is the legate-lifetime DURATION (0 = no time bound). Returns
// CAP_GRANT_CLEARANCE_WRITE_LEN (32) on success, -1 on any gate/bounds failure.
long cap_register_clearance_grant_for_writer(struct Proc *writer,
        caps_t cap_mask, u64 target_stripes, u64 valid_for_ns, u64 session_id);

// The unified /cap/use redeem. ONE locked lookup of the writer's pending grant,
// then a branch on the grant's kind: HOSTOWNER (console-gated, exact-equality,
// ORs caps -- unchanged) or CLEARANCE (no console; the request is the
// self-restriction = a non-empty SUBSET of the granted set; CREATES a legate via
// proc_become_legate). Returns CAP_USE_WRITE_LEN (8) on success, -1 otherwise; a
// gate/kind failure does NOT consume the grant.
long cap_redeem_grant_for_writer(struct Proc *writer, caps_t cap_mask);
```

```c
// kernel/proc.c -- the single audited legate-creation write site.

// Make `p` a legate ROOT: atomically OR caps_to_or (already self-restricted by
// the redeem), allocate a FRESH kernel legate_scope_id, record session_id +
// valid_until, set PROC_FLAG_LEGATE_ROOT. principal_id UNCHANGED.
void proc_become_legate(struct Proc *p, u64 caps_to_or, u32 session_id,
                        u64 valid_until);
```

```c
// kernel/perm.c -- the new caps feed the existing policy helpers (A-2d surface).
// perm_check:       rwx DAC-override is now (CAP_HOSTOWNER | CAP_DAC_OVERRIDE).
// perm_wstat_check: chown/chgrp authority is (CAP_HOSTOWNER | CAP_CHOWN);
//                   chmod-any stays CAP_HOSTOWNER-only (no CAP_FOWNER split).
```

## The `cap` device wire (generalized)

The `/cap/grant` file is **length-discriminated**: a 16-byte write is the legacy
hostowner grant `{cap_mask, target_stripes}`; a 32-byte write is the clearance
grant `{cap_mask, target_stripes, valid_for_ns, session_id}` (all u64 LE). The
two never collide. `/cap/use` is unchanged (8 bytes: the requested cap-set).

**The userspace bridge is a syscall, not a file walk** (A-4a-3). corvus chroots
to its storage cap (`/var/lib/corvus`) at startup, so `/cap` is outside its
namespace -- it cannot walk to the `/cap/grant` file. It reaches the cap device
by **syscall**, exactly as the hostowner grant already does (`SYS_CAP_GRANT`):
the clearance grant rides `SYS_CAP_GRANT_CLEARANCE = 61`
(`x0 = cap_mask, x1 = target_stripes, x2 = valid_for_ns, x3 = session_id`), which
forwards to `cap_register_clearance_grant_for_writer`. The **redeem** rides the
existing `SYS_CAP_USE` (its handler calls the unified `cap_redeem_grant_for_writer`,
which kind-branches to the clearance core) -- so there is NO new *redeem* syscall.
The 32-byte `/cap/grant` Dev write (`devcap_write`) remains the conceptual path
for an un-chrooted writer + the unit tests. libthyla-rs mirrors this as
`t_cap_grant_clearance` / `cap::grant_clearance` (grant) + the existing
`t_cap_use` / `cap::use_grant` (redeem).

A single pending-grant table slot per `stripes` (a re-register replaces in
place). The slot carries a `kind` discriminator + the clearance fields; the
shared `cap_set_entry_locked` writer resets EVERY field on each register, so a
re-register over the other kind cannot leave a stale `kind`/`valid_for`/`session`
that a redeem would misinterpret (the `devcap.clearance_kind_isolation` test
pins this).

The redeem does ONE locked lookup so the grant's `kind` is read atomically with
the rest -- no peek-then-redeem TOCTOU.

## State machine: the legate lifecycle

```
  (corvus) CAP_GRANT_CLEARANCE                (target Proc)
        cap_register_clearance_grant   --->   pending grant {kind=CLEARANCE,
                                                  cap_mask, stripes, expiry,
                                                  valid_for, session}
                                                       |
                              (target writes /cap/use) |  requested subset
                                                       v
                                       cap_redeem_grant_for_writer
                                          - requested subset of granted? (I-2)
                                          - consume the grant (one-shot)
                                          - proc_become_legate:
                                              caps |= requested (atomic)
                                              legate_scope_id = fresh
                                              legate_session_id = session
                                              legate_valid_until = now+valid_for
                                              PROC_FLAG_LEGATE_ROOT
                                                       |
                         rfork descendants inherit legate_scope_id (+ session +
                         valid_until), NOT the caps (elevation-only stripped) nor
                         the ROOT flag -> they are scope MEMBERS
                                                       |
                                    EVAPORATION (either trigger):
   trigger 1  root exits      -> exits() (lock held) proc_for_each_walk:
                                  group-terminate every member (except = root);
                                  root exits via its normal path
   trigger 2  valid_until past -> el0_return_die_check (lockless) proc_for_each:
                                  group-terminate every member INCLUDING self
                                  (except = NULL); self then self-terminates at
                                  the same EL0-return tail
```

The teardown reuses the #809/#811 cascade: `proc_group_terminate` flags each
member's `group_exit_msg` (set-once CAS), wakes its sleepers, and IPIs running
peers; each flagged Thread dies at its next EL0-return die-check. The walk
matches on `legate_scope_id == scope` and is robust to reparenting (an orphaned
member reparented to kproc still matches by tag).

## Invariant

**I-25 -- legate scope bounded + fully revoked.** A legate's elevated caps are
bounded to its `legate_scope_id` subtree, attenuate to children by I-2 (at v1.0:
do not flow at all -- all elevation-only), and are FULLY revoked on the root's
exit or `valid_until` expiry; no elevated Proc outlives the scope; the durable
identity is unchanged by elevation.

## Corvus clearance subsystem + verbs (A-4a-3)

corvus owns the clearance POLICY + per-user eligibility (the kernel owns the
cap-stamp + scope). `usr/corvus/src/main.rs`:

- **Built-in clearance levels** (a fixed v1.0 set; no `LEVEL_CREATE` verb exists,
  so runtime authoring + per-level-file persistence are a v1.x seam):
  `fs-admin` (`CAP_DAC_OVERRIDE | CAP_CHOWN` -- the only level with a live consumer
  today, via `perm_check`) + `supervisor` (`CAP_KILL`; its `/proc`-ctl consumer
  lands in A-4b -- inert until then, but it proves the mechanism with a second
  cap). Both `auth_required = RE_AUTH`. A level's caps MUST be a subset of the
  kernel `CAP_GRANTABLE_CLEARANCE` set. hw-dev / user-admin / clearance-admin
  (scripture's other coarse names) are NOT v1.0 levels -- their caps are not in
  `CAP_GRANTABLE_CLEARANCE`.

- **Per-user eligibility**, persisted in `/var/lib/corvus/clearance.db`
  (CRVS-format, atomic rename-swap mirroring `identity.db` §16.6; fail-closed
  parse). A record is `{subject_kind (user|group), subject, level}`; group
  eligibility matches any of the user's groups (`primary_gid` or a `supp_gid`),
  like group membership. For a `RE_AUTH` level there is no secret unlock
  material, so eligibility is a plain record; the secret-bearing CRVS wrap (for
  `DISTINCT_SECRET` / `SYSTEM_KEY` levels) is the A-4c / v1.x extension.

- **Verbs** (CORVUS-DESIGN §6.4 wire):
  - `CLEARANCE_LIST = 14` (user-facing, valid-session-token gated): the levels
    the session user is eligible for, each with `caps` as a versioned TLV (NOT a
    bare u64 -- v1.x resource-scoping is additive), `auth_required`, `time_bound`.
  - `CLEARANCE_ACTIVATE = 15` (the legate path): verify eligibility +
    `auth_required` (v1.0 enforces `RE_AUTH` in-band -- the live session token is
    the proof; high-stakes levels need the A-4c trusted path, REFUSED until it
    lands), read the peer `stripes` (`SYS_SRV_PEER`, the C-22 live read), register
    the kernel grant (`caps = level.caps & self_restrict` -- STS-style narrowing
    bounded to the level set, I-2) via `SYS_CAP_GRANT_CLEARANCE`, reply OK +
    `legate_session_id` + `granted_caps`.
  - `CLEARANCE_GRANT = 16` / `CLEARANCE_REVOKE = 17` (`CAP_HOSTOWNER`-gated, like
    `GROUP_CREATE`): the hostowner manages eligibility; grant is idempotent,
    revoke blocks FUTURE activation (an active legate keeps its caps until scope
    exit).

## Tests

- `kernel/test/test_devcap.c` -- the clearance grant/redeem lifecycle (the
  creation half): gate (no `CAP_GRANT_CLEARANCE` -> reject), bad args (mask=0,
  non-clearance bits, stripes=0, session=0, session>u32), basic redeem (no
  console; legate fields stamped), self-restriction (subset granted, beyond-grant
  rejected), one-shot consume, cross-stripes reject, `valid_until` computed
  (0 -> none; >0 -> deadline), and `kind_isolation` (a clearance-then-hostowner
  re-register fully resets the kind -- a no-console hostowner redeem then fails).
- `kernel/test/test_proc.c` -- the teardown WALK (synthetic Procs spliced into
  the table; the flag is the observable since the death step fires at the EL0
  die-check which kernel test threads never reach): members of the target scope
  flagged, non-members + kproc spared, the `except` (root) spared, and a
  scope-0 / unmatched-scope teardown flags nobody (the catastrophic-mis-call
  guard).
- `kernel/test/test_perm.c` -- the cap->axis mapping: `CAP_DAC_OVERRIDE`
  overrides rwx but not chmod/chown; `CAP_CHOWN` authorizes chown/chgrp but not
  rwx-override nor chmod; `CAP_KILL` is neither.
- `kernel/test/test_caps.c` -- `caps.rfork_inherits_legate_scope` (A-4a-2a):
  a child joins the scope (carries the 3 fields) but not the ROOT flag.

- `usr/legate-prover/` + joey wiring (A-4a-3, the boot E2E) -- the only test that
  exercises the FULL userspace->kernel chain (the unit tests call the cores
  directly): joey (hostowner) `CLEARANCE_GRANT`s michael fs-admin; the prover
  AUTHs michael, `CLEARANCE_LIST`s (asserts fs-admin), `CLEARANCE_ACTIVATE`s
  (corvus reads the prover's `stripes` via `SYS_SRV_PEER`, registers the grant via
  `SYS_CAP_GRANT_CLEARANCE`), redeems via `SYS_CAP_USE` -> becomes a legate root,
  asserts `granted == DAC_OVERRIDE|CHOWN` + `session_id != 0`, prints
  `legate E2E OK`, exits 0. Proves the corvus orchestration + the grant syscall +
  the real redeem + legate creation; a healthy boot through the exits()
  legate-root path is the integration signal.

The actual member *death* (flag -> `thread_exit_self`) is the already-tested
#809/#811 EL0-die-check path. The A-4a-3 prover is a **no-member** legate (the
v1.0 model below): a scope-MEMBER teardown E2E is v1.x (see the caveat below).

## Error paths

- `cap_register_clearance_grant_for_writer`: -1 on missing `CAP_GRANT_CLEARANCE`,
  `cap_mask == 0` or escaping `CAP_GRANTABLE_CLEARANCE`, `target_stripes == 0`,
  `session_id == 0` or `> u32`, or a full grant table.
- `cap_redeem_grant_for_writer` (clearance arm): -1 on `requested == 0`,
  `stripes == 0`, no pending grant / expired grant, or `requested` escaping the
  granted set; none of these consumes the grant.

## Known caveats / footguns

- **`valid_for_ns` overflow is fail-safe.** `legate_valid_until = now +
  valid_for_ns`; a near-`U64_MAX` `valid_for_ns` wraps to a small value, which
  reads as already-expired -> the legate is torn down at its next EL0 tail.
  Fail-safe (revokes early, never grants-forever). corvus is trusted not to pass
  garbage; the wrap is a backstop.
- **Blocked-root expiry latency.** `valid_until` is checked at the EL0-return
  tail. If the root is blocked in a long kernel sleep when its deadline passes,
  the expiry is not detected until it (or any active scope member) next returns
  to EL0 -- at which point `proc_group_terminate` wakes the blocked root via the
  #811 death-interruptible sleep and it dies before executing more userspace. A
  bounded window; a timer-driven expiry sweep is a v1.x refinement.
- **Spawn-during-teardown straggler.** A member `rfork`'d racing the teardown
  walk may escape the sweep and briefly outlive the scope. It is UNELEVATED (see
  the v1.0 model above), so this is a tidiness gap, not an I-25 violation. A
  strict whole-subtree close (an `rfork`-under-lock parent-flag check) is a
  documented v1.x refinement.
- **Member teardown vs the kproc orphan-reaper (v1.x).** When a legate root with
  a LIVE scope member exits, the teardown correctly group-terminates the member;
  the member then zombies and reparents up to the boot init (`kproc`). At v1.0
  `kproc.wait_pid` is a *strict* "wait for joey" loop (`kernel/joey.c`), not a
  general orphan-reaper, so an orphaned member reaching it extincts on a wrong-pid
  (the same limitation documented for an unreaped stratumd, `usr/joey/joey.c`).
  This is orthogonal to the legate mechanism -- a general `kproc` reaper (the
  documented `wait_pid_for(pid)` v1.x lift) closes it for stragglers of every
  kind. It does NOT affect the v1.0 legate model (above): a v1.0 legate HOLDS the
  clearance and does the privileged work itself -- it has no scope members, so no
  orphan arises. The member-bearing case (an elevated shell forking commands) is
  the same v1.x consideration as fork-grantable clearance caps. The A-4a-3 prover
  is therefore a no-member legate; the member teardown WALK is unit-covered
  (`test_proc::legate_scope_teardown`) and the death step is #809/#811.

## Status

Landed A-4a-2b: the kernel mechanism (devcap clearance grant/redeem + the two
teardown triggers + `proc_become_legate` + the perm cap-honoring) + full kernel
unit tests. Landed A-4a-3-alpha: `SYS_CAP_GRANT_CLEARANCE = 61` (the grant-side
userspace bridge -- corvus is chrooted, so it reaches the cap device by syscall)
+ the libthyla-rs mirror. Landed A-4a-3-beta: the corvus clearance subsystem (the
built-in level table + per-user eligibility persist + the four verbs 14-17).
Landed A-4a-3-gamma: the boot E2E legate prover (`usr/legate-prover/`) + joey
wiring (corvus gets `CAP_GRANT_CLEARANCE`; joey `CLEARANCE_GRANT`s michael
fs-admin); the full chain is GREEN end-to-end at boot (`legate E2E OK` ->
`Thylacine boot OK`). **A-4a kernel + userspace COMPLETE.** The focused A-4a
adversarial audit (covering A-4-pre + A-4a-1 + A-4a-2a + A-4a-2b + A-4a-3, under
UBSan + smp8) is the remaining A-4a step.
