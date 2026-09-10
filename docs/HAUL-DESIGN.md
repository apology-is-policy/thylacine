# HAUL — mounting a remote 9P2000.L tree

**Status**: AS-BUILT. Plain transport `5094f1ad`; the npxf secure channel this
document specifies landed with the row added to `docs/AUDIT-TRIGGERS.md`.

The thylacine ranged well beyond its den to feed; what it brought back went in
the larder. `haul` grafts a tree from *outside the machine* into the local
namespace, and the pages it yields are cached by the Larder (the guest-side FS
cache, I-38). The name was **ratified by the operator 2026-09-09**, replacing
`forage`, which collided with `tools/forage.sh` (the build-input collector).
`mount` and `srv` are the Plan 9 words; `quarry` and `prowl` are taken.

---

## 1. The problem

The operator asked for Thylacine to mount an
[npxf](https://github.com/) server — their own tool, `~/projects/npxf`, which
exports a directory over 9P2000.L on an authenticated, encrypted channel — "as a
directory, transparently."

Two things stood between us and that, and only the first is obvious.

**1. The kernel's 9P client does not speak TCP.** It speaks 9P2000.L over a
**byte pipe**: `9p_spoor_transport` moves messages across a pair of Spoors, and
there is deliberately no TCP in the kernel at all (netd owns the stack). This is
not an oversight to route around — it is the design. `SYS_ATTACH_9P`'s own
documentation names the way across:

> For half-duplex (Plan 9 pipes from `SYS_PIPE`), userspace creates two pipe
> pairs and passes the matching write-end and read-end.

So a userspace shim holds the *server* ends of two pipes, hands the *client*
ends to `SYS_ATTACH_9P`, and shuttles bytes between those and the connection.
The kernel believes it is talking to a local 9P server; the server believes it
is talking to a local 9P client. `usr/viv` already drives this exact attach for
the container diorama — the pattern is live, not new.

**2. npxf has no plaintext mode.** Every connection begins with a mutual
authentication handshake and every 9P message afterwards is one AEAD record.
There is no flag to turn that off, by design. So "mount npxf" is not "mount 9P
over TCP and add encryption later" — the channel *is* the transport.

### 1.1 The fork, and what the operator chose

Where should the encryption terminate?

| Option | Shape | Cost |
|---|---|---|
| **A host-side bridge** | a host process terminates npxf and re-exports plain 9P to the guest | plaintext 9P crosses the host/guest boundary; the guest holds no credential and authenticates nothing; a second moving part to deploy |
| **In the guest** | haul speaks npxf itself | the crypto runs in Thylacine userspace; the guest holds the token and is a real authenticated party |

**The operator chose: terminate in the guest.** They also chose to build the
plain-9P transport first, so the two halves could be landed and reviewed
separately. Both halves are now in.

The choice is the right one on Thylacine's own terms, not merely as a
preference. A host bridge would put the authority boundary in the wrong place:
the guest would inherit a tree it never authenticated, over a channel it could
not attest, from a process outside its namespace. Terminating in the guest keeps
the credential, the authentication and the authority in the same Proc that then
grafts the result into its own namespace — which is the Plan 9 shape (a process
builds its own namespace out of things it can name and prove) and the
capability-microkernel shape (authority arrives with the thing it authorises),
at once.

---

## 2. Shape

```
  ut ──9P──> kernel 9P client ──pipe──┐
                                      │   haul (userspace)
                                      ├── pump_up   ──seal──> TCP ──> npxf-server
                                      └── pump_down <─open─── TCP <──
```

`haul [-a aname] [-t file | --token-env VAR] [-v] host!port mountpoint [cmd ...]`

Two half-duplex pipes: `c2s` carries T-messages, `s2c` carries R-messages. Two
pump threads, one per direction. The main thread performs the attach, then
either runs `cmd` (which sees the tree, being a child — §4.3) or parks for the
mount's lifetime. haul *is* the transport, so exiting while someone is walking
the tree would tear the session down under them.

### 2.1 The two orderings that matter

Both of these are the kind of thing that produces a hang rather than an error,
which is why they are stated here rather than left to the code.

**The handshake runs BEFORE the pumps.** It is a strictly ordered three-flight
exchange on the raw socket; a pump concurrently reading that socket would steal
the server's reply. Once it returns, the socket carries records only, and the
pumps own it.

**The pumps run BEFORE the attach.** `SYS_ATTACH_9P` performs Tversion +
Tattach *synchronously inside the syscall*, so it blocks until replies arrive.
Nothing can reply unless the pumps are already running. Reversing these
deadlocks — the kernel waits for an Rversion that only an unspawned thread could
deliver — and a deadlock at mount time is much harder to read than a refusal.

### 2.2 Framing

The pump parses the 9P `size[4]` prefix even on the plain path, where a blind
byte splice would be correct. Two reasons, and the first is what made the
secure channel a small change:

- npxf seals **one 9P message per AEAD record**, so message boundaries are
  required. Because the plain pump already framed, the secure path is a swap of
  the read/write pair, not a rewrite.
- A hostile or confused peer's absurd length claim is refused *here* rather than
  handed inward as an allocation or a lie.

`MSG_MIN` is 7, not 4: a 9P message is at minimum `size[4] type[1] tag[2]`, and
npxf's server refuses anything shorter as a runt. Matching it means a malformed
frame dies at the transport instead of reaching a parser on either side.

---

## 3. The npxf channel

Reproduced from npxf's `src/channel.hpp` because we must match it byte for byte.

```
  psk = HMAC-SHA256("npxf token v1", token)
  h0  = SHA256("npxf-handshake-v1")

    c->s   "NPXF" | ver | rsv[3] | CE     (40)   h1 = SHA256(h0 | msg1)
    s->c   SE | HMAC(k_cfm, "server")     (64)   h2 = SHA256(h1 | SE)
    c->s   HMAC(k_cfm, "client")          (32)

  prk   = HKDF-Extract(salt = psk, ikm = X25519(own eph, peer eph))
  k_c2s = HKDF-Expand(prk, "npxf c2s v1" | h2, 32)   -- client's SEND key
  k_s2c = HKDF-Expand(prk, "npxf s2c v1" | h2, 32)   -- client's RECV key
  k_cfm = HKDF-Expand(prk, "npxf cfm v1" | h2, 32)
```

Records: `len[4] LE | ciphertext[len-16] | tag[16]`, AAD = those four length
bytes, nonce = `00000000 | LE64(counter)`, an independent counter per direction.

**The record payload is the COMPLETE 9P message, `size[4]` included** — npxf's
server enforces `declared != msg.size()` and fails the connection otherwise. We
enforce the same on the way in, because the kernel reads a *byte stream*: a
payload whose declared size disagreed with its length would silently desync the
pipe rather than fail.

### 3.1 What it buys, and what it does not

| Property | Mechanism |
|---|---|
| Mutual authentication | both confirmation MACs derive from the token |
| The token never transmitted | possession is proved by a MAC; there is no bearer credential on the wire to capture or replay |
| Forward secrecy | session keys depend on two fresh ephemeral X25519 keys |
| Transcript binding | both MACs cover `h2`, which covers the version and both ephemerals |
| Contributory behaviour | an all-zero X25519 result (small-order peer key) aborts |
| Reorder / replay / truncation detection | per-direction nonce counter; any mismatch fails the tag |

**Stated plainly, as npxf's own README does: this is hand-written cryptography
that has not had third-party review.** Our client is a second implementation of
that protocol, which does not make the protocol better-reviewed — it makes two
implementations of an unreviewed protocol. What our side *does* get is standard,
widely-reviewed primitives (RustCrypto's `chacha20poly1305`, `x25519-dalek`,
`sha2`, `hmac`, `hkdf`) rather than a second hand-rolled set.

### 3.2 Fail-closed on entropy

The ephemeral scalar is the whole of the session's forward secrecy. `haul`
takes it from `SYS_GETRANDOM` (which gates on `CAP_CSPRNG_READ`) and **refuses
to connect if that fails**. There is no weaker source and no degraded mode: a
channel that merely looks encrypted is worse than a refusal, because the user
would trust it.

This is also why `npxf::Client::start` takes the ephemeral as an *argument*
rather than generating it. A hidden RNG dependency would make the handshake
untestable and would hide where the randomness comes from; as an argument, the
known-answer tests can pin the exact scalar npxf's vectors used, and `main.rs`
is visibly the one place that must reach the kernel CSPRNG.

### 3.3 Poisoning

Any record failure — a bad tag, a length disagreement, an exhausted counter —
**poisons the session structurally**, not by asking the caller to be careful.
A tag mismatch means the stream was tampered with, reordered or truncated;
continuing past one lets an attacker probe. The counter refuses rather than
wraps, because a repeated nonce under one key breaks ChaCha20-Poly1305
completely.

---

## 4. Verification

The transport had been built, reviewed and booted without a single byte ever
crossing it, because there was no plain 9P2000.L endpoint to point it at. Three
independent layers now stand between that and a claim of correctness.

**1. Known-answer vectors, from npxf's own code** (`usr/haul/kat/`).
`kat/npxf_kat.cpp` `#include`s npxf's `channel.cpp` so it calls the reference
`derive()` / `absorb()` / `confirm_tag()` — *not* a re-derivation of them. That
matters: a generator that re-implemented the schedule would encode my reading of
npxf, so a misreading would make the generator and the Rust agree with each
other and not with npxf. 45 host tests now assert the PSK, the base
multiplication (the clamping — a classic silent interop break), the DH from both
sides, the transcript, all three keys, both confirmation tags, both handshake
flights, two sealed records at counters 0 and 1, the token-file trim rule, the
dial-string grammar and the argument boundary.

The second record is not redundant: an implementation stuck on nonce 0
reproduces record 0 exactly and is caught only by record 1. **Sabotage-verified**
— one character changed in an HKDF label fails exactly the two key tests; a
frozen nonce counter fails exactly the counter test.

The `#include` rationale held for the key schedule and *leaked* at the layer
below it: the record framing, the nonce construction and the counter were
hand-assembled in the generator, duplicating `Channel::send` and `make_nonce`
— the latter sitting in the very translation unit already included. Those now
come from `Channel::send` itself, pointed at a socketpair and read back. **Every
vector was byte-identical before and after**, which is the useful part: the
transcription had been right, and is now not a transcription.

`kat/regen.sh` rebuilds the generator and diffs it against the committed file,
so the fixture is a *checked* recording rather than an asserted one. It skips
(exit 77) where npxf is absent, and `make test-haul-kat` absorbs that 77 — make
maps any non-zero recipe status to a build failure, so without the absorb a skip
and a vector mismatch would be the same verdict.

The generator also refuses to emit unless the direction LABELS hold — `k_c2s` is
the key `client_handshake` assigns to `out.send`, and `k_s2c` the one
`server_handshake` does. That binding is made one level above `derive()`, so a
swap there leaves every vector byte-identical while inverting what they mean.

**It takes TWO legs to check that, and the reason is the useful part.** Each leg
runs one REAL npxf handshake against a shim holding a PINNED ephemeral, so both
ephemerals are known and npxf's own `derive()` can be called from the other side
to produce the labels the assertion compares against. A symmetric check — the
obvious one, running both real handshakes against each other and asserting they
agree — cannot detect a swap applied to both halves, and the first repair of
that, which pinned only the responder, stopped calling `server_handshake` at all
and so could not detect a swap in the server half either. Each version was
"sabotage-verified" by a sabotage shaped like the hole it left.

Discrimination, all four MEASURED against the current generator:

| npxf under test | verdict |
|---|---|
| unmodified | PASS, 23 vectors |
| client half swapped only | FAIL — `client_handshake's send key is NOT c2s` |
| server half swapped only | FAIL — `server_handshake's send key is NOT s2c` |
| both halves swapped | FAIL — caught by the server leg |

An HKDF label change is caught by the vector diff instead.

**2. Live interop against the real server** (`interoperates_with_a_live_npxf_server`,
`#[ignore]`d). A fixture is a recording; it cannot prove interoperation, because
both sides of a mistake made while generating it would agree. This test drives
the actual `npxf-server` binary over TCP with a **random** ephemeral, completes
the handshake, seals a Tversion and decrypts an authenticated Rversion. It skips
loudly without a server rather than passing silently, and a wrong token fails it
at `ServerAuth` — so it discriminates.

**3. The guest E2E** (`tools/interactive/haul-npxf.exp`). Everything above
proves the *protocol*; only a boot proves the *plumbing* — the pipes, the pumps,
the synchronous attach and the mount. The scenario asserts the mount line
including the mode word (a haul that silently fell back to plaintext would
otherwise print an equally cheerful line), then a **directory listing** of the
remote tree through the encrypted channel.

It does NOT read file content, and section 8.2 explains why: the content read is
commented out pending the spawn-inheritance question. This section claimed it
did for a while, contradicting 8.2 two hundred lines below — worth naming
because this is the section a reader consults to learn what the E2E proves, so
its being the wrong one of the two is the expensive direction.

### 4.1 Running it

**npxf's SERVER is Linux-only; its crypto core is not, and that distinction is
worth more than it sounds.** The blockers that stop `npxf-server` compiling on a
BSD host are not a shim: `server_ops.cpp` has 18 Linux-specific sites built on an
`O_PATH` descriptor as the fid representation, reopened through `/proc/self/fd`
(5 sites), plus `linkat(AT_EMPTY_PATH)`, `statx` and `SYS_fchmodat2`. Darwin has
no `O_PATH` and no `/proc`, so porting it means replacing the fid model — a
redesign of the server's containment story, not a compatibility header.

The `crypto` / `channel` / `ninep` / `net` core, which is the whole of what haul
interoperates with, ports with two small changes and now does: `getrandom` →
`getentropy` behind one helper, and the `SOCK_CLOEXEC`/`SOCK_NONBLOCK` socket
flags behind a `socket_ce()` wrapper that takes the fcntl round trip where libc
has no atomic form. `build.sh` grew a Darwin branch for the ELF-only link flags.
**`./build.sh npxf-selftest` now builds and runs on macOS: 9/9 green**, including
the new token-trim test — so the KAT generator and the protocol tests no longer
need thyla-pi at all. Only the SERVER does, which is why the recipe below stands.

(Those npxf changes live in a tree that is **not under version control**:
`~/projects/npxf` has no `.git`. They are `src/crypto.cpp`, `src/net.cpp`,
`src/util.hpp`, `src/npxf_main.cpp`, `src/selftest.cpp` and `build.sh`.)

On a macOS dev host the server therefore runs on **thyla-pi** and an `ssh -L`
tunnel presents it on `127.0.0.1`. The guest reaches the host's loopback via
slirp's `10.0.2.2`, so the tunnel is transparent to it.

```bash
# on thyla-pi (npxf builds there with CXX=g++; clang++ is absent)
npxf-server -l 127.0.0.1:5640 -r <tree> -t <tokenfile> -R

# on the dev host
ssh -f -N -L 5640:127.0.0.1:5640 thyla-pi
HAUL_NPXF_ADDR=127.0.0.1:5640 HAUL_NPXF_TOKEN=<token> \
  cargo test -p haul --lib --no-default-features \
    --target aarch64-apple-darwin -- --ignored --nocapture
HAUL_NPXF_PORT=5640 HAUL_NPXF_TOKEN=<token> \
  tools/test-interactive.sh haul-npxf
```

The exported tree must hold `hello.txt` containing `the thylacine is real`.

### 4.2 What the self-audit found

Four things, before any formal round. Recorded because the fixes are cheap to
read and the reasoning is the reusable part.

**The PRK was left on the stack.** `Hkdf::extract` returns a `GenericArray`,
which is `Copy` — so `prk.into()` *copies* rather than consuming, and zeroizing
only `prk` left the key schedule's root in `prk_bytes`. The single visible
`zeroize()` call is exactly what makes this survive a reading: it looks like the
subject was covered.

**A failed seal left the plaintext in the caller's buffer.** Encryption is in
place, so on an AEAD error `out` holds the header followed by the *plaintext*. A
caller that ignored the `Err` and wrote the buffer would put the message on the
wire in the clear. Now cleared, rather than trusting the caller to notice.

**`open` did not poison on a length violation**, though the contract right above
it said "ANY failure poisons the session" — the comment was true about the
tag path and false about the length path. A length violation reached through
`open` is a protocol violation by the peer and is now as terminal as a bad tag.
`body_len` alone still does not poison: it is a pure query the caller uses to
size a buffer before committing to anything. Regression test:
`a_length_violation_through_open_poisons_but_a_query_does_not`.

**A pump closed an fd the other pump could be writing.** The obvious shutdown —
the dying pump closes the kernel's write end so a waiting session sees EOF
rather than parking — puts one thread's `close` against the other's in-flight
`write` on the same fd, with the fd *number* recyclable in between. Electing a
single closer by CAS removes the double close but not that race. A pump now
closes **nothing**: the main thread observes `STOPPED` and lets the *process*
exit, which closes every fd at once from outside both pumps. The kernel sees the
same EOF, bounded by the 200 ms poll. Exiting is correct rather than merely
convenient — haul *is* the transport, so once either direction is dead the
mount is dead.

### 4.3 The one that changes the shape: a mount is not visible to the shell

With the channel finally up and the tree mounted, the read still failed:

```
haul: 10.0.2.2!5640 mounted at /home/cora/host (aname /, npxf encrypted)
cat: /home/cora/host/hello.txt: no such file or namespace entry
```

Both lines are correct. **A mount lands in the calling Proc's Territory and
nowhere else** — that is I-1, working exactly as specified — and a child
inherits its parent's namespace at spawn, never the reverse. `haul ... &` from
a shell is a *child* of that shell, so it mounted into its own namespace and the
shell could not see it. `login` gets this right by construction: it mounts the
user's home and *then* spawns the shell, so the shell is downstream of the
mount.

This is not a bug in haul; it is the capability model refusing to let a child
reach into its parent. But it does mean haul's original shape — background it,
then use the tree from the shell — **cannot work**, and the operator's request
was precisely "mount that endpoint transparently as a directory."

The obvious answer is Plan 9's own — `rfork; mount; exec` — so
`haul <addr> <mnt> [cmd ...]` was built: mount, then run `cmd` as a child,
which should see the tree by being downstream of the mount. **It does not
work, and that is the more interesting finding.**

```
haul: .. mount check: 5 entr(y/ies) here, first "."
cat: /tmp/host/hello.txt: no such file or namespace entry
```

haul lists the remote tree from its own namespace — a real Twalk + Treaddir
through the channel — and its own child cannot see the mount. Measured at
`/home/cora/host` (inside another mount) **and** at `/tmp/host` (plain ramfs),
so the mount point's location is not the discriminator: **a `SYS_SPAWN` child
does not appear to receive its parent's mounts at all.**

That is surprising, because `territory_clone` deep-copies the mount table, and
because `login` mounts the user's home and then spawns the shell. Whether the
spawn path reaches that clone, and what actually makes login's case work, are
both untested. It is a kernel-side question, tracked in
`memory/bug_nested_mount_lost_at_spawn_clone.md`, and **not haul's to answer**
— which is why the E2E now asserts what haul itself proves (the mount, and a
readdir of the remote tree through the encrypted channel) and leaves the reader
out of it.

The interactive answer therefore remains **posting the connection to `/srv`**
and letting the shell mount it. `/srv` exists for exactly this: its registry is
reached *through* the inherited devsrv mount, so a child's post IS visible to
the parent — which is how login's home proxy delivers a tree to login. That is a
separate chunk with a real design question attached (naming, lifetime, who
unmounts), and it wants the operator's vote.

**A note on method, since it cost a boot and would have cost more.** The
nested-mount explanation fit the first measurement perfectly: haul's mount
point was inside the home's 9P mount, login's is in the ramfs root, and a
mount-cross keyed on Spoor identity would plausibly fail across a clone. It was
still wrong. The one-variable control — same scenario, ramfs mount point —
refuted it in a single run. One measurement plus a plausible mechanism is not a
cause.

### 4.4 What only the E2E could find

**haul advertised one address syntax and implemented another.** Its usage
line, its doc comments and this document all said Plan 9's `host!port` — the
form every dial-style tool in the lineage takes and the form the tree uses
internally (`tcp!127.0.0.1!80`). The code handed the string straight to
`SocketAddrV4::parse`, which wants `a.b.c.d:port`. So the documented form was
rejected outright:

```
haul: address (want host!port)
```

The error message asked for exactly what it had just refused.

Nothing caught this — not the type checker, not clippy, not a review, not a
boot — because **until the npxf channel gave haul a server to talk to, no byte
had ever crossed it**. A whole surface can agree with itself about a syntax it
does not implement, for as long as nothing ever runs it. This is the concrete
answer to "was the E2E worth the loop cost": it found a bug that made the tool
unusable by its own documented interface, on its first real invocation.

Now `usr/haul/src/addr.rs` — pure, host-tested, 6 tests — accepts both forms,
splitting from the right so a Plan 9 network prefix (`tcp!host!port`) leaves a
host that fails *visibly* rather than being truncated into something that
happens to parse.

### 4.5 A guestfwd defect this surfaced

`run-vm.sh`'s guestfwd block claimed a missing host server was "inert (a closed
target RSTs) ... a fast SKIP, never a hang." That is false: qemu connects to
every guestfwd target **eagerly at startup** and refuses to run if any is
refused. It installed three rules unconditionally, so a scenario needing one
endpoint had to stand up two unused listeners. np3-bench always starts all three
before booting, so it never met the case its own comment described.

Fixed by `THYLACINE_GUESTFWD_RULES` (default 3, preserving np3-bench), and the
false comment replaced with the measured behaviour and the qemu error text.

A second followed immediately. With one rule at the default guest port, the boot
then **wedged before login** at `netperf: NET-PERF NP-3`: joey's boot probe
dials `10.0.2.100:7820` expecting an *echo* server, and I had pointed 7820 at
npxf-server, which waits for a 40-byte handshake. The probe connected and waited
for a reply that would never come — past 13 minutes. Forwarding a port to the
wrong kind of server does not make the probe skip; it makes it hang, because
**the guest port is a rendezvous, not a private address**.

And then the third, which killed the approach outright. Moved to a private guest
port, haul reported `sent flight 1 (40 bytes)` — a *successful* write — while
the server logged `read: Resource temporarily unavailable`. The 40 bytes went
nowhere.

The two measured facts compose into a deduction, and it is worth stating as one
because neither half is alarming alone:

- qemu opens the guestfwd's host connection **at launch** (already proven: with
  a closed target it refuses to start).
- npxf-server accepts it and starts its handshake with a **15-second timeout**
  (`kHandshakeTimeoutMs`).
- The guest needs **~90 seconds** to boot and log in.

So the server times out and closes the only host connection the rule owns
roughly 75 seconds before the guest can possibly dial, and the guest's bytes
then vanish into a dead socket while the client's write succeeds. **A guestfwd
is unusable for any peer that expects to hear from its client promptly** — the
rule's connection is spent before the guest exists.

The fix is to stop using guestfwd. Slirp already routes the guest to the host at
**10.0.2.2** (its gateway alias; TCP to it lands on the host's `127.0.0.1`),
dialled **lazily** when the guest actually connects — so there is no window to
expire. It is also what a real deployment does: dial the server's address. The
`THYLACINE_GUESTFWD_GUESTPORT` lever this section originally proposed was
dropped, because the case for it evaporated once the right route was found;
`THYLACINE_GUESTFWD_RULES` stays, since needing three listeners for one endpoint
is a real defect independent of all this.

And a third, in this scenario's own first draft: it exited **0** when the server
was absent, and the harness reported `PASS: haul-npxf [0s]`. The harness has a
real skip convention — exit **77** plus an `LS-CI SKIP:` line, which it reports
as SKIP and annotates "NOT a guest result, and NOT coverage". A green that means
nothing ran is strictly worse than a red, and this one appeared the moment an
`ssh -L` tunnel died underneath it.

---

## 5. Open

- **Where the guest gets the token.** Today: `-t FILE` or `--token-env VAR`,
  mirroring npxf's own two mechanisms, which is deliberately the least
  surprising interface for the operator who designed them. Neither is the right
  long-term home. Thylacine's answer to "who holds a credential" is **corvus**
  (the key agent, the factotum in this lineage), and a `haul` that asked
  corvus for the token would keep the secret out of the filesystem and out of
  `/env` entirely. That is its own chunk, and it wants the operator's vote on
  the shape.
- **`/dev/random` is world-rw while `SYS_GETRANDOM` is capability-gated**
  (the standing H-4b-1 item). haul deliberately uses the *gated* path and
  fails closed; a future consumer reaching for the ungated one would be a hole.
- **`ut` and `host!port` — FIXED for the literal form, still open for the
  composed one.** `ut` used to emit `Bang` for any `!` that was not `!=`, so
  `10.0.2.100!7830` died with
  `UnexpectedToken { expected: "`;`, newline, or end of input" }` and every
  address in the tree had to be quoted at the prompt. `308bb26e` makes
  `scan_word` keep `!` inside a word, so a **literal** address is now typeable
  and the E2E types one unquoted as the only execution those lexer tests get.

  **The composed form is not fixed and that is a different mechanism**, so it
  did not come along for free: `$host!$port` lexes as `Var`, `Bang`, `Var`, and
  `parse_word` glues words only with `~` and `^` — "plain span-adjacent values
  stay separate words" (`parse.rs:622-624`). Even `10.0.2.2!$port` splits, since
  the literal half ends at the `$`. A script composing an address from variables
  must still quote it, and `'$host!$port'` will not do (single quotes suppress
  the expansion) — `"$host:$port"` is the working form. Making adjacent values
  glue is a parser change with a much wider blast radius than a lexer branch,
  and it is not obviously desirable.
- **The name.** `haul` is unratified.
