---
id: sub-stratum-boot
type: sub
parent: moc-stratum
title: "Bringup — spawn, wait for an event, attach, pivot"
code:
  - usr/joey/joey.c
  - kernel/syscall.c
  - kernel/9p_srvconn_transport.c
  - kernel/territory.c
audit: hard
guarded-by: [inv-i28]
validated-by: [prose, gate-smp, gate-interactive]
locks: []
abis: []
design: ["docs/reference/86-pouch-stratumd-boot.md", "docs/reference/88-pouch-stratumd-boot-16c.md"]
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

Take the machine from a read-only initrd to a disk-backed root. joey spawns
`stratumd` with the capabilities it needs, waits for it to be genuinely
ready, wraps its byte-mode service in the kernel 9P client, and swaps the
process root onto the result — carrying forward every mount the old root
held.

## Contract

Ordered, and every step is boot-fatal:

1. **Spawn** `stratumd` with `CAP_HW_CREATE | CAP_CSPRNG_READ` and
   `SPAWN_PERM_MAY_POST_SERVICE`, argv naming the pool, the keyfile, the
   FS and `/ctl` listen paths, the corvus socket, and the SYSTEM uid.
2. **Wait for readiness** — block on stratumd's stdout pipe until the
   token `bound and ready` appears, or EOF.
3. **Open** `/srv/stratum-fs` (open-is-connect through the namespace),
   yielding a client byte-conn Spoor.
4. **`SYS_ATTACH_9P_SRV`** wraps it in the kernel 9P client → a `KOBJ_SPOOR`
   for the FS root. Close the now-redundant conn fd.
5. **Pre-pivot probe** — walk + read `/thylacine-version` through the
   attach handle, proving Twalk + Tlopen + Tread end to end.
6. **Grab O_PATH handles** to everything the old root carries.
7. **`SYS_PIVOT_ROOT`** — atomic root swap.
8. **Re-graft** each saved handle onto the new root by `mkdir` + `MREPL`.
9. **Post-pivot probe** — read the sentinel again, from the new root.

## Mechanism

**Readiness is an EVENT, never a duration. This is the dossier's central
fact.** The original code polled `/srv` on a fixed retry budget — which was
really a *guess* at how long a crypto-heavy mount takes. Fast enough under
HVF; under TCG the guess raced a **healthy** stratumd and declared the boot
fatal mid-mount. A boot must be deterministic and oblivious to host speed
("M-series HVF or a potato"), so joey blocks on the pipe until stratumd
emits its token.

The token choice is the sharp part. stratumd prints a `serving …` line
early — and that line is **optimistic**: emitted *before* the mount and the
bind. Keying on it would reproduce the original bug with extra steps. The
truthful signal is `bound and ready`, printed only once every fallible step
has succeeded — FS mounted, socket bound and posted, `/ctl` up, workers
spawned — immediately before `accept()`. This is
[[haz-harness-fail-open]]'s rule stated positively: *verify the artifact,
not the intent.* The optimistic line reports intent.

EOF on that pipe is the other terminal outcome, and it is unambiguous
because joey dropped its own write reference first: EOF means stratumd's
last writer closed, i.e. it died before signalling. The blocking read also
drains the pipe, so stratumd can never stall on a full buffer while joey
waits for it. The scan carries `rtlen-1` bytes across read boundaries so
the token is found even when it straddles two chunks. The harness's 180 s
boot timeout is the only outer backstop — for a stratumd that hangs *alive*
without signalling, which is a stratumd bug and not a host-speed race.

**After readiness, a dedicated thread owns the pipe for the daemon's
lifetime.** joey stops reading at handoff. Without the drainer, a full pipe
blocks a stratumd thread mid-write — and under the FS that wedges the
**whole system**, proven live when an instrumented stratumd deadlocked a
boot. If the drainer fails to spawn joey *closes* the read end instead, so
later writes fail fast with EPIPE: the degraded mode is silenced
diagnostics, never an FS wedge. Same failure shape as the LS-CI relay's
back-pressure bug — see [[haz-unread-pipe-wedge]].

**The attach opts into loose cache mode, and the premise is written down.**
`T_ATTACH_9P_LOOSE` on the SYSTEM mount asserts: this pool's block device
is exclusively guest-owned, concurrent sessions reach disjoint datasets,
own-writes invalidate through this same client's Larder, and corvus's
private tree has no system-mount byte reader. Those are the conditions
under which [[inv-i38]]'s close-to-open guarantees still hold with
revalidation relaxed. Any of them ceasing to be true revokes the flag here.

**The pivot is a swap, so everything else must be carried by hand.** Seven
O_PATH handles are taken *before* the swap and re-grafted after: `/srv`,
the whole devramfs root (→ `/bin`), `/proc`, `/ctl`, `/dev`, `/hw`, `/env`.
O_PATH crosses each mount and yields the *Dev root*, not the synthetic
mount point — that distinction is what makes the re-graft land the real
tree. Each re-graft is `mkdir`-then-`MREPL`, and the `mkdir` must be
idempotent because the pool persists across reboots and a later boot finds
its own directories already there.

`/srv` must survive because the retired `SYS_SRV_CONNECT` bypassed the
namespace and its replacement — open-is-connect — resolves *through* it.
`/bin` exists because the disk root holds user data only; the boot medium
is bound into the namespace, Plan 9 style, so post-pivot spawns of the
system binaries resolve through [[inv-i28]].

**The failure branch carries a corrected comment.** When stratumd dies
before signalling, joey drains bounded and returns non-zero — deliberately
*not* calling `wait_pid`, since a long-running daemon never zombifies and
the wait would hang forever. The 16c-era comment here claimed the kernel
then took "the joey-exit-non-zero path (NOT wait_pid)". It did not: that
path is reached *through* kproc's wait, so a reap-any took stratumd's
zombie instead of joey's and extincted on "wrong pid" — destroying the
diagnostic in precisely the branch whose diagnostic mattered most. The
kernel now waits by pid (#94), so joey's own status is what the extinction
reports.

## Data structures

The spawn is one `t_sys_spawn_args`: flat NUL-separated argv (14 strings,
173 bytes) plus an fd list installing one pipe write-end as fds 0, 1 and 2.

## Concurrency

Sequential until the drainer thread is spawned. The drainer then runs for
the daemon's lifetime, forwarding to the console; it exits when stratumd
does.

## Invariants enforced

[[inv-i28]] — every post-pivot spawn resolves its binary through `stalk`
against the re-grafted `/bin`, contained at the new root with per-component
X-search.

The capability grant is where the hardware-authority chain starts: joey
holds `CAP_HW_CREATE` and confers it on exactly one child. `CAP_CSPRNG_READ`
is needed because libsodium's `sodium_init` reaches `getrandom` before the
mount can proceed.

`SPAWN_PERM_MAY_POST_SERVICE` is a *per-bit* delegation, distinct from
console trust: it lets stratumd `bind()` onto `/srv/stratum-fs` and confers
nothing else. The separation is what keeps the trusted-path anchor
console-attach-only (the I-27 property; no registry note yet — the console
surface is unswept).

## Error paths

Every step returns 1 and fails the boot. The one deliberate non-fatality is
the drainer spawn.

## Performance

The mount is crypto-heavy and its duration varies by an order of magnitude
between HVF and TCG. That variance is exactly why the handshake is
event-driven; no timing constant appears in this path.

## Prosecution

- A new readiness signal must be emitted **after** the last fallible step.
  Any line printed before the bind is optimistic and unusable.
- A new pre-pivot mount must acquire its O_PATH handle before the swap and
  re-graft after, or it vanishes silently at pivot.
- The loose-mode premise list is a claim about the whole system, not this
  file. A change that gives some other writer access to this pool revokes
  it here.
- The readiness read must keep draining, and the drainer must keep owning
  the pipe. A reader that stops reading wedges the FS.
- `wait_pid` must not be reintroduced on the daemon path.

## Seams

[[seam-791-smp1-joey]].

## Caveats

- The keyfile is read from a literal `/system.key` at the initrd root; the
  FHS-shaped `/etc/stratum/` placement is a deferred lift.
- `--fs-workers 4` is a flat constant, not probed: in-VM musl `sysconf` has
  no substrate and reports 1, so the deployment states its worker count
  explicitly.
- The coordinator deliberately does **not** pass `--bake-owner-uid`. The
  runtime must stamp per-user files by the proxy's `SO_PEERCRED`, not force
  them SYSTEM-owned; bake-owner is host-bake-only ([[sub-substrate-build]]).

## Provenance

[[chg-2026-08-02-stratum-sweep]].
