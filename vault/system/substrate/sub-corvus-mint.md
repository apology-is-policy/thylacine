---
id: sub-corvus-mint
type: sub
parent: moc-substrate
title: "corvus-mint — the host minter that writes the system identity the device must later open"
code:
  - tools/corvus-mint/src/main.rs
  - tools/corvus-mint/Cargo.toml
audit: hard
guarded-by: [inv-i22]
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/CORVUS-DESIGN.md", "docs/reference/105-corvus-recovery.md"]
created: 2026-09-06
updated: 2026-09-06
---
## Purpose

The host-target minter for the **system identity**. At build time it
generates the admin hybrid keypair and writes two on-disk wraps of it into
the pool's `/var/lib/corvus`: `system-wrap` (the keypair under the build
system passphrase) and `system-recovery-wrap` (the *same* keypair under a
24-word BIP-39 recovery phrase). These are the system analog of `pool.img`
and `system.key` — the device's [[sub-corvus]] reads them at boot and cannot
start without them.

It is one of the two binaries [[sub-corvus-crypto]] exists to keep
byte-identical: the on-device agent unwraps this identity at boot, and this
tool writes it at build time. Both link the same crate so there is no second
implementation of the wrap layout to drift — and drift here has a specific
failure mode, a device that cannot open its own system wrap.

**Why this dossier is `audit: hard` in an `audit: none` area.** Every other
[[moc-substrate]] dossier is a harness, and a harness is judged by the
revert-probe, not the adversarial round. corvus-mint is the exception: it is
not a harness but a **secret producer**, and the secret it produces is the
most privileged one in the system. Its correctness is load-bearing in the
same way [[sub-corvus]]'s is, so it carries the same audit weight even though
it lives among the build tools and never runs on the device.

## Contract

Two modes, selected by the first argument:

- `corvus-mint <out-dir>` — the **bake**. Generate the keypair, seal both
  wraps, self-verify both open the same keypair, write the two blobs into
  `<out-dir>`, and print the recovery phrase to stdout (the build log keeps
  it, forensically, like the mkfs seed).
- `corvus-mint emit-phrase <header-path>` — derive **only** the recovery
  phrase from the seed and write it as a C header (`#define
  CORVUS_SYSTEM_RECOVERY_PHRASE "..."`). No keypair, no pool — pure
  derivation, so it can run *before* the userspace build, which is when joey
  needs to `#include` the phrase.

`tools/build.sh` runs `emit-phrase` before the userspace build and the bake
during the pool populate. The tool must be launched from the repo **root**,
not `usr/` — see [[sub-substrate-build]] for the cwd trap.

Two environment overrides, both defaulting to build-baked known test values:
`CORVUS_SYSTEM_PASSPHRASE` (default `thylacine`) and
`CORVUS_SYSTEM_RECOVERY_SEED` (a 64-hex seed). A v1.x installer supplies real
per-install secrets through the same two seams.

## Mechanism

**The recovery phrase is deterministic; everything else is random per build.**
The keypair and both wraps' salts and nonces come from `OsRng`, so the
identity itself is fresh every build. Only the recovery-phrase *entropy* is
pinned, derived from the seed — because the point of pinning it is that joey's
generated header carries the matching phrase and can drive a live
`RECOVER(system)` boot E2E against the baked `system-recovery-wrap`. The bake
and `emit-phrase` derive the phrase from the same seed, so they agree by
construction and there is no drift to keep in sync.

**The self-verify is the guard, and it is the area's discipline exactly.**
Before the bytes are written, the tool unwraps `system-wrap` with the
passphrase and `system-recovery-wrap` with the seed entropy, and asserts each
result equals the keypair it generated. A wrap/unwrap mismatch — a KDF
regression, an AD-prefix slip, a layout drift — fails the build loudly rather
than baking an identity the device cannot open. This is "verify the artifact,
not the intent" applied to a secret: the tool does not trust that its own
seal round-trips, it checks.

**The two wraps hold one keypair**, which is the whole basis of the recovery
model — recovering the system identity from the phrase yields the same
keypair, so nothing encrypted to its public keys is invalidated. It is the
build-time origin of the twin-wrap crash-safety [[sub-corvus]]'s
`RECOVER(system)` relies on.

## Data structures

None owned. It is pure orchestration over [[sub-corvus-crypto]]'s functions
(`generate_hybrid_keypair`, `wrap_keypair_passphrase`, `make_recovery_wrap`,
`unwrap_keypair_passphrase`, `unwrap_recovery`, `bip39_encode`) plus a hex
seed parser. The wrap layouts and their `_Static_assert`s are the crate's.

## Concurrency

None. A single-shot host process; no threads, no shared state.

## Invariants enforced

**[[inv-i22]]** — indirectly, at its origin. The system keypair this tool
mints is the identity `ADMIN_ELEVATE` gates the `CAP_HOSTOWNER` grant on;
minting it correctly (and proving the wrap round-trips) is the build-time end
of the chain whose runtime end is [[sub-corvus]]'s elevation gate. corvus-mint
holds no standing authority itself — it writes ciphertext into a pool.

## Error paths

Every failure is a hard exit: a bad seed length exits 2; a keygen, wrap,
unwrap or self-verify mismatch panics the build; a file create/write failure
exits 1. There is no partial success — either both verified blobs are written
or the build stops. A secret buffer (keypair, entropy, phrase, and the two
self-verify results) is wiped before the process returns.

## Performance

Irrelevant — one keygen and four Argon2id derivations (two seals, two
self-verifies) at build time, once per pool bake.

## Prosecution

- **The self-verify must survive any change to the wrap path.** It is the
  only thing between a wrap-layout or KDF regression and a device that bricks
  on `system_identity_load`. Never "optimize away" the unwrap-and-compare.
- **Byte-identical wraps are the reason the crate is shared.** A second
  implementation of the layout here — even a "small" convenience — reintroduces
  exactly the drift the one-crate rule exists to forbid.
- **The seed pins only the phrase, never the keypair.** Pinning the keypair
  would bake one admin identity across every build; the security posture rests
  on the keypair being random and only the *test* phrase being reproducible.
- **The build-baked passphrase and seed are v1.0 known test values**, not
  secrets. A v1.x installer MUST supply real per-install values through the two
  env seams, and MUST gate the boot test harness that consumes the known phrase
  (the same requirement the known passphrase and test users already impose —
  task #880).

## Seams

- **The two env overrides are the installer seam**: a real deployment sets
  `CORVUS_SYSTEM_PASSPHRASE` and `CORVUS_SYSTEM_RECOVERY_SEED` to real
  randomness. Overriding the seed without rebuilding joey makes the baked wrap
  and joey's header disagree, and the boot E2E fails loudly rather than
  silently — the intended tripwire.

## Caveats

- **The recovery phrase is printed to stdout at build time.** That is
  deliberate and forensic (the mkfs seed does the same), and harmless while the
  phrase is a build-baked known test value. It becomes a real handling concern
  the moment a v1.x build supplies real entropy.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
