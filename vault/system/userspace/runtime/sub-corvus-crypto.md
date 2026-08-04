---
id: sub-corvus-crypto
type: sub
title: "corvus-crypto — the at-rest wrap core, and the discipline of never dropping a buffer unwiped"
parent: moc-userspace-runtime
code:
  - usr/lib/corvus-crypto/src/lib.rs
  - usr/lib/corvus-crypto/src/bip39_wordlist.rs
  - usr/lib/corvus-crypto/Cargo.toml
audit: hard
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/CORVUS-DESIGN.md"]
created: 2026-08-04
updated: 2026-08-04
---
## Purpose

Everything that turns a secret into bytes on disk, and back. The wrap
layout, the key-derivation function, the authenticated cipher, the hybrid
keypair, the data-encryption-key envelope, and the recovery-phrase codec
all live here — as pure functions over slices, with no filesystem, no
syscalls and no state.

**It exists as a separate crate for one reason: two different binaries
must produce byte-identical wraps.** The on-device key agent
([[sub-corvus]]) unwraps the system identity at boot; a host-target
minter writes that identity at build time. If those were two
implementations of the same layout they would drift, and the failure mode
of drift here is a device that cannot open its own system wrap. One
crate, two linkers, no second implementation.

The two things the library does *not* own are the two things that differ
between its consumers: the randomness source is a type parameter (the
daemon passes a kernel-CSPRNG adapter, the minter passes the host's), and
the allocator belongs to whichever binary links it.

## Contract

**A wrap** is a fixed 3752-byte blob: a 72-byte header — magic, version,
the three Argon2 cost parameters, salt, nonce — followed by the 3648-byte
encrypted keypair and a 32-byte authentication tag. Three different
things share that exact layout: the passphrase wrap, the recovery
keyslot, and the daemon's per-user record. **Only the associated data
distinguishes them**, and it is chosen at wrap time rather than encoded in
the file. A recovery keyslot therefore cannot be opened as a passphrase
wrap even if the two files are swapped on disk — the authentication fails
because the associated-data prefix differs.

**A keypair** is the concatenation of an X25519 secret and public key and
an ML-KEM-768 encapsulation and decapsulation key, at fixed offsets. The
recovery keyslot and the passphrase wrap hold the *same* keypair, which
is the property every data-encryption-key envelope depends on: recovering
an identity does not re-encrypt anything.

**An envelope** is a 1217-byte hybrid-PKE blob wrapping one 32-byte
data-encryption key: a version byte, the ML-KEM ciphertext, an ephemeral
X25519 public key, a nonce, the encrypted key and its tag. The wrapping
key is SHA-256 over a domain string, both shared secrets, the ephemeral
public key and the ML-KEM ciphertext — so the key is bound to the exact
transcript, and neither the post-quantum nor the classical half alone
suffices.

**A recovery phrase** is 24 BIP-39 words over 256 bits of entropy plus an
8-bit checksum. Derivation uses the *decoded entropy*, never the phrase
text, so whitespace and case cannot change the resulting key.

Every fallible entry point returns an option. There are no error codes and
no panics on data — only on a caller passing mismatched buffer lengths,
which is deliberate.

## Mechanism

**The key hierarchy is two-layer and the layers are different shapes.** A
passphrase or a recovery phrase goes through Argon2id to a 32-byte
key-encryption key, which authenticated-encrypts the keypair. The keypair
then decapsulates a data-encryption key out of an envelope. The first
layer is memory-hard because its input is a human secret; the second is
not, because its input is already a 256-bit key.

**The associated data is the domain separation, and it is built the same
way three times.** Prefix, then subject, then a discriminator byte or a
backend identifier. Passphrase wraps and recovery wraps use different
prefixes; envelopes use a third; the system identity uses a fixed subject
string where a user wrap uses the user's name. The pattern is uniform
enough that a new wrap kind that *forgot* its prefix would be
conspicuous — which is the point of building all three through named
helpers rather than inline.

**The unpacker rejects cost parameters outside what the writers emit.**
The Argon2 costs live in the file header, which means a tampered or
bit-rotted header is an *input* to a key-derivation function. Without a
bound, a large time cost wedges the single-threaded daemon on a
multi-billion-pass derivation and a large memory cost aborts it against
its fixed heap — turning what should be a per-user authentication failure
into a whole-daemon denial of service. So the parser bounds all three
before Argon2 ever sees them. The ceilings sit above every value the
writers produce, so no valid wrap is ever rejected. **This is a file
format defending against itself**, and it is the right instinct: the
header is attacker-reachable in exactly the same way a network field is.

**Nothing is dropped unwiped, including on failure paths.** The
discipline is total and each instance has a reason:

- The authenticated decryption decrypts *then* verifies. On a
  correct-key/corrupt-tag case the buffer holds real plaintext at the
  moment the tag check fails — so the failure path wipes before
  returning, rather than letting a discarded result carry a secret into
  freed memory.
- The keypair generator wipes its byte copies on the unreachable
  size-mismatch path as well as the success path, because "unreachable"
  is a claim about today's types.
- The phrase encoder wipes the bit buffer it built the words from,
  because that buffer *is* the entropy.
- Every derived key-encryption key is wiped immediately after use, at
  each of the four call sites that derive one.

**The compile-time discipline is unusually heavy for a library this
size**, and every assertion earns its place. Layout constants are pinned
against their own arithmetic, so a changed field width fails the build
rather than producing an unreadable wrap. The word/bit accounting is
pinned. And the wordlist — 2048 entries of pure data — is proved
**strictly ascending by a `const fn` that runs at compile time**, because
the lookup is a binary search: a mis-sorted or duplicated edit would not
crash, it would silently resolve the wrong word, and a wrong word is a
wrong key. That is the failure this crate could most plausibly ship
without noticing, and it is the one it made impossible.

## Data structures

Two structs, and they are the same struct: the passphrase wrap and the
recovery wrap each hold the three costs, the salt, the nonce, the
ciphertext and the tag. They are kept distinct as *types* — so a function
that takes one cannot be handed the other — while sharing one packer and
one parser, so the byte layout cannot diverge between them. Type-level
separation, byte-level unification.

The parsed-fields struct is private and exists only to hold the parser's
output on the way to either public type.

The wordlist is a static array of 2048 string slices.

## Concurrency

None. Every function is pure over its arguments; there is no static
mutable state, no interior mutability, and nothing to lock. Thread-safety
is a property of the caller's data, not of this crate.

## Invariants enforced

None of the enumerated system invariants directly — this crate is beneath
them. What it supplies is the mechanism [[sub-corvus]]'s no-escrow
property rests on: because a recovery keyslot is a second wrap of the
*same* keypair under a *user-held* phrase, and because the library offers
no operation that derives a key-encryption key from anything an
administrator holds, there is no code path by which any authority other
than the subject's own passphrase or own phrase opens a wrap. The
property is structural — an absence of a function, not a check.

## Error paths

Uniformly `Option`, uniformly fail-closed: a length mismatch, a bad
magic, a wrong version, an out-of-envelope cost, a failed derivation, a
tag mismatch, an unknown word, a bad checksum, a wrong word count all
return nothing. The caller cannot distinguish "wrong passphrase" from
"corrupt file", which is intentional at this layer — the daemon maps both
to a single authentication failure so the wire does not leak which.

One deliberate exception: the wrapper asserts, **in release builds**,
that the ciphertext buffer matches the plaintext length. That is a caller
bug rather than data, the underlying copy would panic anyway, and the
assertion names why — so the failure reports the contract rather than a
slice length.

## Performance

Argon2id at 16 megabytes and two passes for a login, eight passes for a
recovery. The memory cost is a quarter of the design default, bounded by
the daemon's fixed heap; the recovery path pays four times the time cost
because recovery is rare. Both are stored per-record, so raising them is a
one-constant change that does not invalidate existing wraps.

ML-KEM-768 encapsulation and X25519 agreement are microseconds and
irrelevant beside the derivation.

## Prosecution

- **A new wrap kind needs a new associated-data prefix**, built through a
  helper rather than inline. Reusing an existing prefix makes two wrap
  kinds interchangeable on disk.
- **A new failure path must wipe whatever secrets already exist.** The
  existing paths wipe at the point of failure, not at the end of the
  function, because the failure returns early.
- **The cost envelope must stay tighter than what the daemon can
  survive.** It is not a sanity check on the format; it is the bound that
  keeps a corrupt header from denying service.
- **The wordlist's sorted-and-unique proof must survive any edit to the
  list.** It is the only thing standing between a data-entry slip and a
  silently wrong key.
- **The layout assertions must be updated deliberately, never
  mechanically.** They pin an on-disk format; a build that fails there is
  telling you existing wraps are about to become unreadable.
- **The recovery keyslot must continue to wrap the identical keypair.**
  A regenerated keypair on recovery would invalidate every existing
  envelope, which is the failure the current design specifically avoids.

## Seams

The **Argon2 working-memory matrix is not scrubbed** — the upstream crate
has no zeroize-on-drop, so passphrase-derived intermediate state is left
to the allocator. This is documented at the derivation site rather than
hidden. The mitigation is the daemon's, not the library's: locked memory
plus disabled core dumps mean the residue never leaves RAM. A library
consumer that does neither inherits the exposure.

The **memory cost is heap-bounded rather than security-bounded.** The
comparable "sensitive" preset in the reference implementation is a
gigabyte; sixteen megabytes is what the daemon's fixed heap allows. The
per-record storage of costs is what makes raising it later a heap-resize
rather than a format break.

**One backend, one version.** Passphrase is backend zero and the wrap
format is version one; both are encoded so a second of either is
possible, and neither exists.

## Caveats

- **The library is `no_std` for the device and `std` under test**, which
  is what lets it carry thirteen host tests — round trips for both wrap
  kinds and the envelope, the checksum and unknown-word rejects,
  whitespace canonicality, the out-of-envelope cost reject, and an
  explicit associated-data domain-separation test. That is a materially
  better proof position than its sibling [[sub-tls]], which is
  unconditionally `no_std` and therefore has none.

- **The memory-cost ceiling is described as equalling "the heap bound"**
  where it actually equals the *emitted* cost — sixteen megabytes against
  a twenty-four megabyte heap. The intent is clear and the bound is
  correct; the phrase is loose. Recorded because the surrounding
  paragraph is otherwise precise enough that a reader would trust it
  literally.

- **The deterministic test generator is a counter stream asserting
  `CryptoRng`.** That is sound — the trait is a marker and the tests need
  distinctness rather than secrecy, and the file says so. It is worth
  knowing that the marker is therefore not load-bearing anywhere in this
  crate's own validation.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
