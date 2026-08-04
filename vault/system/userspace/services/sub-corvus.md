---
id: sub-corvus
type: sub
title: "corvus — the key agent: one session, one keypair in mlock'd RAM, and a design whose justification was deleted underneath it"
parent: moc-userspace
code:
  - usr/corvus/src/main.rs
  - usr/corvus/Cargo.toml
audit: hard
guarded-by: [inv-i22, inv-i23]
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/CORVUS-DESIGN.md", "docs/IDENTITY-DESIGN.md"]
created: 2026-08-04
updated: 2026-08-04
---
## Purpose

The key agent. corvus is where a user's cryptographic identity lives: a
hybrid X25519 + ML-KEM-768 keypair, wrapped at rest under a
passphrase-derived key, unwrapped into locked RAM for the duration of one
authenticated session, and used from there to open the data-encryption-key
envelopes that make a per-user encrypted home readable.

Four authorities converge in this one daemon, which is why it is the
tree's smallest program with the largest blast radius:

- **Authentication.** It is the only thing that can turn a passphrase into
  a live identity. `login` does not verify passwords; it asks corvus to.
- **Key custody.** The unwrapped keypair exists in exactly one place in
  the system — corvus's `mlockall`ed, undumpable, untraceable address
  space — and leaves it only as a derived DEK for a dataset its owner
  requested.
- **Elevation.** The clearance database (who may activate which level,
  and what capability set that level confers) is corvus's, and the
  activation path is what mints a legate.
- **Recovery.** The second keyslot — a BIP-39 phrase that unwraps the same
  keypair — is minted, stored and rolled here, for users and for the
  system identity alike.

It is *not* the identity name service alone: `RESOLVE_ID` / `RESOLVE_NAME`
exist, but the interesting half is that everything above rides the same
14-verb wire on the same one connection.

## Contract

**Reached as a 9P server.** corvus posts `/srv/corvus` and serves a
two-node namespace: a directory root containing a single `ctl` file.
Verb frames travel as `Twrite` payloads on `ctl`; responses drain back as
`Tread` payloads. The verb layer is deliberately *inside* 9P rather than
beside it — a client needs no new syscall, only walk/open/write/read.

**A request frame** is a 4-byte header — verb id, protocol version,
16-bit payload length — followed by the payload. A version mismatch or an
over-length payload is answered `BadFormat` and then the connection is
torn down, because the wire contract says a stream cannot be safely
re-synced across a framing disagreement. The reply is delivered *before*
the EOF: the teardown is deferred until the staged response has been fully
drained by a subsequent read.

**A response frame** is three bytes — status, 16-bit payload length —
plus payload. Seven statuses: OK, BadAuth, PermissionDenied, NotFound,
RateLimited, BadFormat, InternalError. The distinction between BadAuth and
NotFound is deliberately *not* preserved at the AUTH boundary — an unknown
user and a wrong passphrase both return BadAuth, so the wire does not
enumerate accounts.

**A session** is one user, one 33-byte opaque token, and one unwrapped
keypair. `AUTH` mints it and returns the token; `WRAP` / `UNWRAP` present
the token; `SESSION_CLOSE` or the owning connection's close ends it and
wipes the keypair.

**Its filesystem world is a capability.** corvus is handed a storage-root
descriptor at fd 0 and `chroot`s to it before doing anything else that
touches a file, so every path it later names resolves inside that
capability and nothing above it is reachable. A missing or invalid fd 0 is
a fatal boot error, not a fallback — see [[inv-i23]].

## Mechanism

**The startup order is load-bearing and reads backwards.** corvus posts
its service *first*, using the namespace it inherited, and chroots
*second*. That is not an oversight: the chroot displaces the namespace
root, after which `/srv` is unreachable, so the post must precede it. The
listener survives because it is a capability — a handle, not a name. This
is the one intentional use of the inherited namespace, and the comment
that says so is the reason the ordering has not been "tidied" into
breaking the daemon.

Everything after that is fail-closed and boot-fatal, in a fixed order:
lock memory, disable core dumps, disable tracing, prove the CSPRNG
answers, prove the storage capability confines (create and read inside it,
and assert a path above it is *unreachable*), self-test the recovery codec
and wrap layout, load the host-baked system identity, load the identity
database, load the clearance database. Any failure exits rather than
serving. The confinement smoke and the recovery self-test are the
interesting two: neither proves corvus works, both prove a specific
silent-brick cannot have happened.

**The 9P server is the template's ancestor.** The per-connection state
machine — Tversion, Tattach binds the root fid, Twalk to `ctl`, Tlopen,
then Tread/Twrite — is the same shape [[sub-ptyfs]], [[sub-tapestryd]] and
[[sub-netd-server]] use, because the shared 9P codec those three link was
lifted out of corvus's private module. corvus is the original; the other
three are descendants. That inverts the usual reading of a shared
template: a fix that reached the library reached them, and a fix that
reached only a descendant did not come back here.

**Verb frames accumulate across writes.** A `Twrite` payload is appended
to a per-connection accumulator, which is then scanned for one complete
frame. The accumulator is capped at twice the maximum frame, and an
overflow *clears* it rather than latching — so a pathological client that
never completes a header gets an error and can then recover, instead of
wedging the connection permanently.

**Exactly one frame is dispatched per write.** A client that pipelines a
second request before draining the first response is a protocol error, and
the staged response is simply overwritten. Strict request-response is the
contract; corvus does not multiplex.

**Authorization is re-queried, never remembered.** Every capability-gated
verb — USER_CREATE, GROUP_CREATE, ADMIN_ELEVATE, RECOVER of the system
subject, and the three clearance-administration verbs — asks the kernel
for the peer's *current* capabilities and console attachment at the moment
of the call. The peer snapshot taken at accept time is deliberately not an
input to any gate, because capabilities are mutable on a live process (a
peer can elevate mid-conversation) and console attachment is revocable in
both directions. A dead peer or a failed query yields zero capabilities,
so the gate fails closed by construction rather than by a branch.

**Persistence is atomic-by-rename.** The identity database and the
clearance database are each serialized whole, written to a temporary name,
fsynced, and renamed over the real one. The per-user keypair wrap and the
per-user recovery keyslot use the same swap. A present-but-corrupt
database aborts the boot rather than silently re-bootstrapping — which
matters because the first user creation is the one that needs no
authority, so a database that reads as empty would hand the next caller a
free hostowner candidate.

**Recovery is a second keyslot, not a backdoor.** The recovery wrap holds
the *same* keypair under a phrase-derived key, so recovering does not
re-encrypt any data — every existing DEK envelope stays valid. corvus
stores no copy of a keypair that any authority other than the user's own
passphrase or own phrase can open; the host owner has no user-data
recovery verb at all. That is the no-escrow property, and it is what makes
mutually-encrypted homes survive a malicious host owner.

## Data structures

**`Session`** — a single global slot: an active flag, the user name and
its length, the token, the unwrapped keypair, and the identifier of the
connection that ran the successful AUTH. Installed whole, cleared whole;
there is no in-place setter for the user or the keypair, which is how the
"a session's identity never changes" property is made unexpressible rather
than checked. The keypair is volatile-wiped on clear.

**`Conn`** — the per-connection arena: the handle, a monotonic
process-unique identifier assigned at accept, the peer snapshot, the 9P
protocol state (version negotiated, fid table), and four buffers —
incoming 9P bytes, outgoing 9P bytes, the verb-frame accumulator, and the
staged response with its drain offset. Plus one flag: tear down once the
staged response has drained.

**`CorvusUserState`** — the per-user record: name, principal and group
identifiers, and the wrap parameters (Argon2 costs, salt, nonce,
ciphertext, tag). The ciphertext is the encrypted keypair; the record is
what the identity database serializes.

**The eligibility table** — `(subject kind, subject, level)` triples,
where the subject is a user or a group. A user is eligible for a level if
a triple names them directly or names a group they belong to.

**The recovery-failure table** — per-subject counters of
checksum-valid-but-unwrap-failed recovery attempts. A typo fails the cheap
BIP-39 checksum first and is *not* counted, so only a crafted or guessed
phrase charges the limit and a legitimate holder is never locked out. The
table is bounded at one entry per possible user *plus one* — the extra
slot exists so that table exhaustion cannot evict the fixed system
subject and silently un-rate-limit system recovery.

## Concurrency

corvus is single-threaded. One `poll` over the listener and every live
connection, then a service pass; no locks, no shared mutable state across
threads, and none of the wait/wake reasoning the kernel notes carry.

The interesting consequence is that "single-threaded" does not mean
"single-client". Up to eight connections are live at once, they interleave
at request granularity, and the *global* session is the shared state
between them. Two mechanisms make that safe, and only one of them is
written down:

- **The AUTH gate.** A second AUTH while a session is bound is refused
  with PermissionDenied. This is what actually prevents one client
  overwriting another's session — and its consequence is that corvus
  serves one *user* at a time, not one connection at a time.
- **The ownership tag.** The session records which connection created it,
  and only that connection's close — or an explicit, token-authenticated
  SESSION_CLOSE — clears it. A non-owning bearer-token connection
  disconnecting must not wipe a live login session, because the storage
  coordinator presents the login token over its own transient connection
  to pull a home DEK, and mid-session legate elevation re-presents the
  same token.

The stated justification for the single global slot is neither of these;
see the caveats.

## Invariants enforced

**[[inv-i23]]** — corvus is the worked example of a service bounded by its
endowed storage capability. It chroots to the handed descriptor before its
first file touch and proves the confinement at boot by asserting that a
path above the capability is unreachable.

**[[inv-i22]]** — no identity carries ambient authority here. corvus
authenticates but does not elevate on its own initiative: every
administrative verb gates on a capability the *peer* holds, re-read live,
and the privileged pair (system recovery, admin elevation) additionally
requires live console attachment. corvus is the mechanism by which a
scope-bounded legate elevation is created, not a holder of standing
authority.

**The unwrap owner gate** — a DEK envelope is opened only when the
session's bound user is the recorded owner of the named dataset. A
cross-user unwrap is refused even with a valid token, which is the
property the design's model names explicitly and carries a negative
counterexample for.

## Error paths

Boot failures exit; there is no degraded mode. A failed hardening step
reports a numbered stage and dies, so a boot log identifies which of the
five steps failed without a debugger.

At the wire, malformed frames answer `BadFormat`. Two of those are
fail-stop rather than continue — a protocol-version mismatch and an
oversize payload — because after either the byte stream's framing is
unknown. Everything else is a normal reply and the connection continues.

Inside a verb, the discipline is that every early return still scrubs.
The failure paths through AUTH wipe the derived key, the unwrapped keypair
copy, and the token entropy independently, because each is reached before
the others exist. The pattern is uniform enough that its absence would be
conspicuous.

Connection teardown wipes all four per-connection buffers, because each
can hold a secret: the accumulator holds a passphrase frame, the staged
response holds a token or a DEK.

## Performance

Argon2id at 16 MiB and two passes for a login, eight passes for a
recovery — deliberately expensive, and the reason corvus carries a 24 MiB
static heap. The memory cost is the bringup quarter of the design default,
bounded by that heap; the cost parameters are stored per-record on disk,
so raising them later resizes the heap rather than invalidating wraps.

Everything else is negligible: the identity database is tens of kilobytes
at the maximum user count, the 9P messages are one page, and the daemon is
idle in `poll` between logins.

## Prosecution

- **The authority gates re-query.** Any new gated verb must ask the kernel
  for live capabilities rather than reading the accept-time snapshot. The
  snapshot exists to satisfy a model property, not to authorize anything;
  a gate that reads it is stale in both directions.
- **Fail-closed on a failed query.** The live-capability helper returns
  zero on any error and on a dead peer. A caller that distinguishes "query
  failed" from "no capabilities" reintroduces the branch this design
  removed.
- **The chroot precedes every file touch, and the post precedes the
  chroot.** Reordering either breaks a different thing: a file touch
  before the chroot escapes the capability, and a post after it cannot
  find `/srv`.
- **The session is installed and cleared whole.** Adding a setter for the
  bound user or the keypair would make the immutability property checkable
  instead of structural.
- **Wipe on every path.** A new early return inside a verb must scrub
  whatever secrets already exist at that point. The existing paths scrub
  the derived key, the keypair copy, the token, the payload buffer and the
  response buffer at distinct points, and a `clear()` without a preceding
  wipe leaves the bytes in the vector's capacity.
- **The accumulator cap must stay reset-on-overflow.** Latching it wedges
  the connection for every subsequent write, which is the bug the current
  form was written to close.
- **The first-user bootstrap is unauthenticated by design.** Any change to
  how "no users exist" is determined is a change to who may become the
  first host owner — which is why a corrupt identity database aborts the
  boot rather than reading as empty.

## Seams

The **recovery rate limit is process-lifetime and in-memory**: it counts
failed attempts per subject since corvus started, and a restart clears it.
The daemon does not restart in normal operation — its failures are
boot-fatal and it is spawned once — but the time-windowed, persisted limit
covering authentication as well is a separate design item that has not
landed.

**Multi-session** is the standing lift. The session table is one slot; the
design's model already permits a set of session records keyed by owner,
and the connection-ownership tag is the piece that was added in
anticipation. What remains is making the session per-connection rather
than global, and the AUTH gate's refusal is what stands in for it today.

**Rate limiting does not cover authentication.** Repeated wrong-passphrase
AUTH attempts are bounded only by the cost of Argon2id itself.

## Caveats

- **The justification for the single global session names a kernel cap
  that no longer exists** (task #148). The session table's comment
  explains that one slot is adequate because a per-process kernel cap of
  one connection means at most one is live. That cap was removed when the
  service registry moved into the namespace; the live bound is a global
  64, and corvus itself sizes for eight simultaneous connections. The
  design is still safe, but for a mechanism the comment does not mention —
  AUTH's refusal while a session is bound — and the resulting limit, one
  *user* at a time, is written down nowhere. The contradiction is visible
  inside the same declaration: the ownership field twenty lines below
  exists precisely because several connections coexist. Eight other sites
  across the source, the specs and the reference docs still name the
  deleted cap.

- **A full connection table plus a waiting connection is a spin, and the
  comment calls it a deferral** (task #149). When the table is full corvus
  declines to accept and continues. But the listener's readiness is
  level-triggered on a non-empty backlog, so the infinite-timeout poll
  returns immediately every iteration, no work is done, and the loop
  burns a processor until a slot frees. Same root as the caveat above:
  while the per-process cap existed this branch was unreachable, so
  writing it as benign cost nothing.

- **The file header describes a daemon four arcs younger than this one**
  (task #150). It states that storage is in-memory only and that
  filesystem persistence "lands once that tree is mounted" — persistence
  landed, and three of its loaders are boot-fatal. It marks the peer
  snapshot dead code awaiting the administrative verbs — those verbs
  landed, they deliberately refuse to read it, and a different handler
  reads it anyway to report file ownership. And the daemon prints its
  construction sub-chunk in the boot banner on every boot, so the
  staleness is not merely internal.

- **The recovery-failure table's bound is one entry per user plus one, and
  the plus-one is the whole point.** A flat per-user bound would let table
  exhaustion drop the fixed system subject and silently remove the rate
  limit from system recovery. This is recorded here because it reads like
  an off-by-one and is not.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
