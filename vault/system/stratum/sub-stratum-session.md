---
id: sub-stratum-session
type: sub
parent: moc-stratum
title: "The per-user encrypted home — a second stratumd, and the DEK's lifetime"
code:
  - usr/login/src/main.rs
  - "stratum: src/cmd/stratumd/proxy_9p.c"
  - "stratum: src/cmd/stratumd/dataset_pattern.c"
  - "stratum: src/cmd/stratumd/corvus_notify.c"
audit: hard
guarded-by: [inv-i1]
validated-by: [prose, gate-interactive]
locks: []
abis: []
design: ["docs/IDENTITY-DESIGN.md section 9.9", "docs/CORVUS-DESIGN.md"]
created: 2026-08-02
updated: 2026-09-23
---
## Purpose

Give each logged-in user an encrypted home that only their session can
read, without `login` ever holding the key and without the two users'
sessions being able to name each other's data.

## Contract

**The home proxy holds `CAP_TCB_DIAL` (U, 2026-09-23).** The proxy dials
byte-mode `/srv/stratum-fs`, a TCB service, which since (U) requires that
capability ([[sub-kernel-devsrv]]). login confers it in the proxy's spawn
`.caps(...)`, one hop, exactly as it confers `MAY_POST_SERVICE`.

**This is the case that forced the gate to be a capability rather than an
identity check.** The proxy runs AS THE USER on purpose -- that is the whole
mechanism by which the coordinator attributes the user's home files to them --
and the user's own shell is the same principal, spawned by the same login. No
owner+mode rule can admit one and refuse the other; only a capability can
([[inv-i22]]). The shell's `SHELL_CAPS` omits the bit, and capabilities never
grow after creation (I-2), so nothing the user runs can acquire it.

**But the capability alone was not enough, and that is the (U) F1 finding.**
Holding the key is only half the problem; the other half is that the key-holder
must not be puppetable by a peer wearing the same identity. `devproc_debug_-`
`authorized` admits the OWNER, and the proxy's owner is the user -- so the user's
shell could debug-attach the proxy and drive its live coordinator transport,
reaching the system store with no capability at all. The front door was locked
and the side door stood open. Closed by login passing `T_SPAWN_PERM_NOTRACE`
(bit 9) on the proxy spawn, so the kernel stamps `PROC_FLAG_NOTRACE` before the
proxy's first EL0 instruction; the bit's placement and its deliberate lack of a
grant gate are in [[sub-kernel-syscall-dispatch]].

**Spawn-time, not a self-call, and the difference is a real race here.** A proxy
that sealed itself with `SYS_SET_TRACEABLE(0)` would be attachable between exec
and the call. The Proc able to use that window is not hypothetical in a login
session: a second login, or a process the user backgrounded in a prior session,
is already running as the right principal when the new proxy appears.

**The caveat to carry, because NOTRACE does not cover it.** Only the debug route
is closed. The planned `/proc/<pid>/fd/` surface (deferred in `kernel/devproc.c`,
in the header comment enumerating the per-pid files) would be gated on the OWNER
axis, and an owner-gated fd
listing reopens the same reach by a different mechanism -- dup the proxy's live
coordinator fd instead of attaching to the proxy. That surface must gate on more
than the owner axis, and this dossier is where the requirement is recorded
because the proxy is the asset it would expose.

**And `/proc/<pid>/fd/` is not the only one -- the class is "owner-gated per-pid
file", and it has a second member already named.** Enumerating the per-pid files
by their gate rather than by their purpose (self-audit, (U) F1): `mem`, `regs`,
`fpregs`, `wait`, `kregs` and `kstack` take the DEBUG gate, which is what makes
NOTRACE sufficient today. But `environ`, `sched`, `maps`, `cwd`, `exe`, `status`,
`cmdline` and `imperium` take **owner-or-`CAP_HOSTOWNER`**, so the user's shell
reads all of them on the proxy right now. None leaks anything today: `environ` is
empty because envp pass-through does not exist at v1.0 (`_pad_envp` "must be 0",
rejected loudly), and the rest are names, addresses and counters.

The one to watch is `environ`. **When envp pass-through lands, anything login puts
in the proxy's environment becomes readable by that user's own shell** -- so the
proxy must never be handed a secret that way, and `haul`'s `--token-env` is
precedent that we do sometimes pass secrets through the environment. Recorded here
rather than at the envp work, because the requirement belongs to the asset.
(The owner being able to KILL the proxy is expected, not a hole: [[inv-i26]] makes
kill two-axis, and a user may always end their own session.)


At login, in order:

1. `provision-dek` — idempotent ensure-home. The coordinator folds EEXIST
   into OK, so a returning user is a no-op and the home root is born
   user-owned 0700.
2. name → id bridge — enumerate `/ctl/datasets` and match the user's name.
3. `install-dek` — the coordinator UNWRAPs the home DEK into its live map.
   **The lease is conn-bound**, so login's `/ctl` attach must stay open for
   the whole session.
4. spawn the per-user proxy, attach `ds:<user>`, mount at `/home/<user>`.
5. … session …
6. `evict-dek` — zero and remove. Conn-destroy auto-evicts too; explicit
   eviction zeroes promptly.

Each `/ctl` verb is **one Twrite of an exact-length payload**. A partial
write is a partial payload and therefore EINVAL — a short write is a hard
failure, never resumable.

## Mechanism

**Logout under an elevated shell (2026-09-17).** Login arms the kernel's
session-leader hangup path so exiting the session terminates its remaining
shell peers. Namespace references are dropped at process exit, permitting the
home-server shutdown to finish rather than waiting on unreaped Territory
references. The restored cross-user arm of `ls-imperium` checks actual
michael logout followed by cora login, not just a return to the first shell.


**The echo mask is set BEFORE the prompt is written, never after**, and the
ordering is the whole mechanism — there is no window to lose.

A sender that reacts to the prompt string — an expect script, a paste, a fast
typist — puts bytes on the wire inside *any* gap between the prompt and the
mode flip. In that gap echo is still **on** from the username read, so the byte
is **rendered on the trusted path**; then the flip discards it, because a mode
change starts a fresh line by design. Two failures from one race: a visible
passphrase prefix, and a truncated read that surfaces as **a plain
authentication failure**.

That last part is why ordering rather than narrowing is the fix. The symptom is
"wrong password" — indistinguishable from actually typing the wrong password —
so the defect has no diagnostic signature of its own and could not be found from
a report.

**The username prompt above it already had the ordering right.** A correct
instance and an incorrect one, adjacent, in one function — which is how the
wrong one survived review: the pattern is visibly present in the file.

**`login` never holds a raw DEK.** It forwards only the opaque 33-byte
corvus session token; the coordinator does the UNWRAP and WRAP through its
own corvus connection. So the most privileged process in the session
handles no key material at all.

**The home is served by a second stratumd, run as the user.** `login` does
not serve it directly. It spawns `/bin/stratumd --role client` **as the
user**, so that proxy's connection to the boot coordinator carries the
*user's* `SO_PEERCRED` and the coordinator stamps the user as owner of
everything created through it. The proxy posts `/srv/home-<user>` in the
session namespace; login attaches that and mounts it. The shell inherits
the bind, because `territory_clone` deep-copies the mount table.

**The proxy is a raw-frame relay with one gate.** It reads whole 9P frames
from upstream and forwards them to the coordinator, except that on
`Tattach` it parses the `aname` and matches it against
`--datasets-allowed`. A refusal is emitted upstream as `Rlerror(EACCES)`
and the coordinator **never sees the frame** — its fid table stays clean.
Refuse, don't defer.

The aname parser is the trust boundary and validates accordingly: empty and
`"/"` refused uniformly; oversize refused; and **every control byte**
(`< 0x20`, `0x7F`, NUL) refused — because an admitted dataset name flows
into `/ctl/events`, which is line-oriented, making a name with an embedded
newline a log-injection vector. UTF-8 multi-byte passes through unchanged.
The wrapper is the right gate for wire-derived input precisely because the
matcher delegates name-side validation to its caller.

**The downstream peer check defends against socket squatting.** With
`--coordinator-uid` set, the proxy verifies the *dialed* socket's peer uid:
a malicious local user could pre-bind a fake socket at the configured coord
path before the real coordinator starts. Fail-closed on resolution error,
and additionally refuse a `(uid_t)-1` sentinel that arrives alongside a
success return — which cannot happen on Linux or BSD, and is pinned so a
future platform shim cannot slip through.

**`--single-session` is not an optimisation. It is the only teardown
available.** The proxy serves login's one attach and exits when login
closes it; login then reaps it. The reason it must be cooperative is a
direct consequence of the capability model: `login` runs as
`PRINCIPAL_SYSTEM`, and the kill axes are owner / `CAP_HOSTOWNER` /
`CAP_KILL` — **none of which login holds against a user-owned Proc**. There
is no ambient root here to fall back on, so serve-one-and-exit is the
mechanism rather than a preference.

login confers `MAY_POST_SERVICE` on the proxy by the one-hop delegation it
holds from joey, and does **not** confer console trust.

**Logout is enforced from the other side too.** Each per-user stratumd
subscribes to corvus's notify socket; when corvus emits `SESSION_CLOSED`
for a matching user, the daemon sets its stop flag and tears down — drain
in-flight writes, unmount, exit. `stm_fs_unmount` → `stm_sync_close` zeroes
per-dataset DEKs, and the process exit takes the rest with it. That
process death **is** the structural mitigation: the DEK is gone because
its holder is gone, not because someone remembered to wipe it.

The notify consumer is tolerant by default: an EOF starts a 30 s window and
a reconnection within it continues the session; otherwise it falls back to
strict behaviour (immediate stop). Its parser bounds every length field and
applies the same control-byte refusal to the user string.

## Data structures

`HomeSession` holds what logout must undo: the proxy `Child` (reaped), the
mount, and the `/ctl` attach whose lifetime *is* the DEK lease.

## Concurrency

The proxy is one thread per upstream connection, strictly request/response
— it forwards one frame and waits for one reply. The notify consumer is one
dedicated thread per daemon with SIGINT/TERM/HUP/QUIT blocked at entry,
cooperating through the same stop flag the accept loops watch.

## Invariants enforced

[[inv-i1]] — user-vs-user isolation, by three gates in series: the proxy's
allow-list (which aname may be *requested*), the 0700 owner (kernel rwx),
and the installed DEK (an un-unlocked dataset attach is inert).

The absence of ambient authority (the I-22 property; no registry note yet —
the capability surface is unswept) is what forces the cooperative-teardown
design above.

## Error paths

Any failure to bind the home fails the login — a home that cannot be bound
is a failed session, not a degraded one. If the proxy never posts its
service, login closes the attach, which EOFs the still-in-`accept` proxy so
it exits, then reaps it.

## Performance

Bounded retry with a 1 ms torpor yield between attempts while waiting for
the proxy's post — busy-spinning would starve the very proxy being waited
for on a single vCPU.

## Prosecution

- The `/ctl` attach must outlive the session; the DEK lease is bound to
  that connection and closing it early evicts the key mid-session.
- A new proxy-forwarded op that can name a dataset needs a gate in the
  wrapper, not in the matcher.
- The control-byte refusal must stay in the parser. Any name that reaches
  `/ctl/events` unvalidated is a log-injection vector.
- `--datasets-allowed` must never be empty in a Thylacine deployment; the
  opt-in exists for tests only and produces an open relay.
- login must not acquire a kill axis over user-owned Procs to "simplify"
  teardown. That would trade a cooperative protocol for ambient authority.

## Seams

[[seam-proxy-coord-eof]] · [[seam-stratum-notify-peercred]].

## Caveats

- The bounded connect timeout in the proxy's coordinator dial degrades to
  an **unbounded blocking connect on Thylacine**: pouch's `fcntl` is ENOSYS
  and its sockets are synchronous-blocking by design, so the non-blocking
  path is skipped. Correct, just unbounded — accepted because the `/srv`
  connect-walk is a fast local open.
- v1.0 handles only `SESSION_CLOSED`; `USER_KEY_ROTATED` and
  `ADMIN_FORCE_EVICT` extend the same parser later.

## Provenance

[[chg-2026-08-02-stratum-sweep]].

[[chg-2026-08-16-seven-small-surfaces]] records this interval.

## login masks the identity capability on every session spawn (2026-09-05, KT-1 C-F1/C-F7)

`Command` defaults to `cap_mask: !0` -- a child inherits every capability its
parent holds -- and login holds `CAP_SET_IDENTITY` (it is the one principal-
stamping act on the seat). Until 062efe18 login masked only its `ut` spawn,
so `halcyond --session`, every kaua-term it spawned, every shell in every
tile, and `aurora-push` (which parses a user-controlled file) ran with the
setuid-equivalent: a program in any tile could spawn as any user. Now every
`Command::new` under login carries a mask: stratumd `T_CAP_CSPRNG_READ`,
aurora-push `0`, `ut` and `halcyond --session` `SHELL_CAPS` (LOCK_PAGES |
CSPRNG_READ); the session compositor masks `!T_CAP_SET_IDENTITY` again on
each tile spawn (the second hop's own guard), and the kernel intersects, so
both hops are monotone. Witness: `/bin/caps-probe` in a session tile -- a
plain spawn succeeds, the same spawn with an identity request is REFUSED.

## login degrades to the console shell when the compositor cannot start (2026-09-07)

[[seam-login-halcyond-fallback]] is now **closed**. A session-lever image
(`/lib/halcyon/session == "on"`) booted console-mode was a real lock-out:
`THYLACINE_DISPLAY=console` drops the GPU stack, so tapestryd never starts, but
the pool still carries the lever, so login spawns `halcyond --session`, which
finds no compositor and exits 1. Pre-fix login treated that exit as a clean
logout and returned -- so joey's getty respawned login, which re-ran halcyond,
which failed identically: an endless login loop with **no usable shell** on any
box where the compositor cannot come up (console-mode, headless, a GPU that
never binds).

The fix distinguishes a bootstrap failure from a logout by the child's exit
status. For the `ut` path an exit is logout regardless of status (unchanged).
For the session path `session_failed = !status.success()` (a `wait()` error also
counts as failed) -- a **clean** last-tile logout returns 0 and returns to the
getty prompt unchanged, but `session_halcyon && session_failed` prints `login:
session compositor unavailable -- console shell fallback` and degrades to the
console shell for that seat. The fallback **reuses the already-constructed
`shell_cmd`** -- the default no-lever image's exact path, built above with
`CONSOLE_OWNER`, the consctl fd, `--home`, the stamped user identity, and the
masked `SHELL_CAPS` (so **no new authority**, and in particular no
`CAP_SET_IDENTITY` leak -- it inherits the masking of the subsection above),
and it enters the same `/home/<user>` bind. `session_failed` covers both a
connect-fail and a mid-session compositor **death** (both yield a non-zero
exit), which is why the fix already satisfies the run-40 F2 [P3] "a mid-session
death should degrade too". Witnessed console-mode (a session-lever image booted
`THYLACINE_DISPLAY=console`): the fallback line, then an interactive `ut` shell
with an echo round-trip, and **no** second `Thylacine login:` prompt.

