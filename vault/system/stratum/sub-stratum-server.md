---
id: sub-stratum-server
type: sub
parent: moc-stratum
title: "The server side — aname kinds, whose identity it believes, what it answers"
code:
  - "stratum: src/9p/server.c"
  - "stratum: src/cmd/stratumd/serve.c"
  - "stratum: src/cmd/stratumd/peer_creds.c"
  - "stratum: src/cmd/stratumd/run.c"
audit: hard
guarded-by: [inv-i1, inv-i28]
validated-by: [prose, gate-smp]
locks: []
abis: []
design: ["docs/IDENTITY-DESIGN.md section 9.7", "docs/POUNCE-DESIGN.md"]
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

The other end of [[moc-kernel-ninep]]'s wire. What Stratum's 9P server does
that Thylacine's correctness depends on: how it interprets `aname`, whose
identity it believes, and the two Thylacine-authored extension ops it
answers.

## Contract

`Tattach` takes one of four `aname` kinds, and anything else is `EINVAL`:

| aname | Kind | Binds the root fid to |
|---|---|---|
| `""` or `"/"` | default | root dataset, ino 1 |
| `"/abs/path"` | chroot | the resolved ino; the connection treats that subtree as its root |
| `"spec:src=tgt,…"` | bind spec | root dataset ino 1, then applies each entry as a Bind |
| `"ds:<name>"` | child dataset | **that dataset's** ino 1 — the per-user encrypted home |

`afid` is ignored (Tauth is a no-op — the daemon authenticated the socket).
`n_uname` is ignored. See below; this is the correction that matters.

## Mechanism

**Identity arrives by `SO_PEERCRED` and by nothing else.** `h_attach`'s
comment is explicit that `n_uname` — 9P2000.L's numeric uid hint — is read
and discarded because "we already have peer-creds". Thylacine's kernel
*does* substitute the caller's `principal_id` into that field, correctly,
but at v1.0 nothing on this side consumes it; it is the foreign-server path
for a future transport with no peer credentials to read.

The channel that works is `stm_peer_creds`, and Thylacine shares the Linux
arm of it verbatim: pouch's musl marshals `getsockopt(SO_PEERCRED)` onto
`SYS_SRV_PEER` underneath, and since A-3 fills `ucred.uid` with
`principal_id` and `ucred.gid` with `primary_gid` — kernel-stamped, so a
connecting Proc cannot forge it. Stratum's own source carries the warning
back across the boundary in as many words: *this is load-bearing, do not
"simplify" the marshal back toward 0*, because the `/ctl` SYSTEM gate keys
on this uid.

**`ds:<name>` and the three access gates.** Selecting a child dataset by
name is what makes a per-user encrypted home reachable, and *three*
independent gates stand in front of it — none of which is this parser:

1. The proxy's `--datasets-allowed` decides which aname a connection may
   even request. That is the user-vs-user boundary ([[inv-i1]]).
2. The dataset root's 0700 owner, enforced by the *kernel's* rwx layer.
3. The installed DEK. Binding reads the child's root inode, which requires
   its key — so an un-provisioned or locked home makes the attach *inert*
   rather than merely unreadable.

The lookup and the stat are separate locked steps, not one critical
section, and that is safe by an argument worth keeping: a concurrent
destroy in the window only makes the stat fail, yielding a clean attach
abort. And dataset ids are **monotonic and never reused**, so a resolved
child id can never alias a dataset created later — no id-confusion.

**The Thylacine-authored extensions.** `h_walkgetattr` answers POUNCE's
`Twalkgetattr` (140/141): Twalk semantics — same fid gates, same
partial-walk rule, same bind — plus one Rgetattr body per walked component,
which is exactly what the kernel's per-component X-search consumes.
`newfid == NOFID` is permitted as a **walk-query**: walk and sample, bind
nothing, nothing to clunk. That is the one-RPC stat. A partial walk binds
nothing either, matching Twalk. The op-number registry and the #371
renumber live in the wire dossier ([[sub-kernel-ninep-wire]]).

**Thylacine never binds, and the server knows it.** `Twalk` has two paths:
a full-lock fallback when bindings exist (the component loop consults a
mutable array, so it cannot run unlocked) and a three-phase fast path —
pin the source, run verify + component loop *unlocked*, bind under the
re-acquired lock. The comment states plainly: "The Thylacine deployment
never binds." So Thylacine always takes the fast path, and a binding
installed concurrently by `Tbind` simply serializes after the walk —
skipping the consult *is* the walk-before-bind order.

**The socket buffer carries the frame class.** `stm_stratumd_listen_unix`
requests `SO_SNDBUF`/`SO_RCVBUF` before `bind`, and on Thylacine the pouch
layer folds a value ≥ 128 KiB into the bind-time service post as the
**bulk ring class**. On a host it is an ordinary advisory hint. Both FS
listeners pass `2 × STM_9P_MSIZE_DEFAULT`, so the system mount and the
per-user home proxy both negotiate 128 KiB. Best-effort by design: the
listener works either way, just at the default class.

The same helper hardens the socket path generally — `lstat` refuses to
clobber a non-socket (a typo'd `--listen /etc/passwd` deserves protection),
mode defaults to 0600 with non-permission bits masked away, and a stale
socket is unlinked with only ENOENT tolerated.

**Role validation refuses loudly rather than no-op silently.** A flag that
has no effect in the selected mode is an error, not a shrug:
`--single-session` without `--role client`, `--coordinator-uid` without
client mode, `--corvus-admin-uid` without `--ctl-listen`,
`--provision-corvus-dataset` with client mode. And most consequentially,
`--role client` with **no** `--datasets-allowed` is refused outright,
because it would forward every Tattach to the coordinator with no policy
enforcement — an open relay. The opt-in flag exists only for tests and
non-Thylacine deployments.

## Data structures

Per-connection: a fid table, `auth_uid`/`auth_gid` from peer creds, the
root dataset id, and per-fid `conn_root_dataset` / `conn_root_ino` — the
containment anchor that makes the chroot and child-dataset kinds real.

## Concurrency

One `s->lock` over the fid table and bindings; the Twalk fast path drops it
for the component loop under a fid pin. The FS accept loop dispatches to a
worker pool (`--fs-workers`, adaptive: inline when nothing is in flight,
dispatched when the client pipelines). The `/ctl` listener has its own
serial path.

## Invariants enforced

[[inv-i1]] — the per-user boundary, though the enforcement is the proxy's
allow-list and the kernel's rwx, not this parser.

[[inv-i28]] — Thylacine's containment is not delegated here. Stratum
enforces **dataset scope only**; per-file rwx is the kernel's job at its
own FS chokepoint. The two must not be confused: this server will happily
serve a file the kernel would refuse, because refusing is not its office.

## Error paths

`Rlerror` with Linux ecode conventions. Malformed frames are `EPROTO`; an
unknown aname shape is `EINVAL`; a failed child-dataset lookup or root
stat surfaces the Stratum status directly, releasing the fid first.

## Performance

The Twalk fast path exists because the component loop dominates walk cost
and Thylacine's zero-binding deployment can run it unlocked. POUNCE folds a
multi-RPC stat into one round trip.

## Prosecution

- `n_uname` must stay ignored while `SO_PEERCRED` is the channel. Honouring
  a client-asserted uid would make identity forgeable.
- A new aname kind must state its containment anchor and cannot widen an
  existing connection's root.
- The dataset-id monotonicity is load-bearing for `ds:` — an allocator that
  reuses ids reopens id-confusion.
- Any move toward Stratum enforcing file rwx must be refused; the kernel is
  the enforcement point and a second one would diverge.
- A new flag that is meaningless in some mode must refuse loudly, per the
  existing pattern.

## Seams

[[seam-stratum-notify-peercred]].

## Caveats

- Multi-dataset routing by path (`tank/home/alice`) is deferred; `ds:` is
  the single-level child selector that replaced it for the home case.
- `Tbind` exists and is exercised by Stratum's own tests, but no Thylacine
  path uses it — the fast-path reasoning depends on that staying true.

## Provenance

[[chg-2026-08-02-stratum-sweep]].
