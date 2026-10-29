# User authority wire reservations (UA-0)

Implements part of the numeric reservation obligation in the accepted
USER-AUTHORITY-DESIGN. Reservations do not enable an endpoint. Existing Corvus
outer framing remains version 1. Authority verbs 21..25 are reserved, in order:
QUERY, PREPARE, REQUEST, STATUS, CANCEL. Their inner authority schema version is
1; unknown versions/flags must be refused. Detailed request/reply and LCUR v2
layouts, kernel Admin scope and admission ABI are still pending and must land
before those consumers. Old servers continue to refuse unknown verbs.

Canonical persistent/public mandate records use the following layout. The C
mirror is `kernel/include/thylacine/authority_wire.h`; the Rust mirror is
`usr/lib/corvus-authority/src/abi.rs`. Byte arrays avoid host endianness/alignment
assumptions. Both mirror structs assert every offset and total size. No native
struct memory is accepted as a serialized record.

| Offset | Field | Bytes |
|---:|---|---:|
| 0 | magic, bytes `MDTM` | 4 |
| 4 | schema version = 1 | 2 |
| 6 | reserved = 0 | 2 |
| 8 | exact total record length | 4 |
| 12 | kind: Use=1, Activate=2, Admin=3 | 1 |
| 13 | authentication: Session=0, DistinctKey=1, Founding=2 | 1 |
| 14 | state: Live=1, Revoking=2, Revoked=3 | 1 |
| 15 | term: UntilRevoked=0, UntilUtc=1 | 1 |
| 16 | mandate ID | 8 |
| 24 | immutable record revision | 8 |
| 32 | subject principal | 4 |
| 36 | issuer principal (attribution, never authority by itself) | 4 |
| 40 | domain | 8 |
| 48 | domain generation | 8 |
| 56 | typed action mask | 8 |
| 64 | UTC term end; exactly zero for UntilRevoked | 8 |
| 72 | opaque transaction ID, nonzero | 16 |
| 88 | subject selector count (1..16) | 2 |
| 90 | resource selector count (1..16) | 2 |
| 92 | support count (0..8) | 2 |
| 94 | envelope present, exactly 0 or 1 | 1 |
| 95 | reserved = 0 | 1 |
| 96 | counted vectors, followed by optional envelope | variable |

All integers are unsigned little-endian; no padding or trailing extension bytes.
Vector order: subject IDs (4 bytes each); resources (owner ID + object ID, 8+8);
supports (mandate ID + revision, 8+8). Each vector is strictly sorted and unique.
An absent support list denotes a potential founding record, not authorization to
install it; only the trusted installer/replay path may select that insertion.
A non-founding record must pass the ordinary issue checks with live authority.

The optional envelope has a 32-byte header: domain u64 at0, actions u64 at8,
term-end u64 at16, grant-kind mask u8 at24 (Use=1, Activate=2, Admin=4),
authentication floor u8 at25, remaining delegation depth u8 at26 (0..16),
term-kind u8 at27, subject count u16 at28 and resource count u16 at30. Its
subject/resource vectors follow, in that order. It is legal only on Admin records.
All selectors and auth/term tags use the same canonical validation as above.

Admin action bit positions 0..14, in order: enroll, profile, suspend, resume,
retire, group-create, group-membership, grant, revoke, delegate, clearance-enroll,
key-reset, rotate-domain, floor-define, audit-read. Use/Activate action bits32..38,
in order: filesystem-read, filesystem-write, filesystem-chown, network-connect,
network-listen, signal, post-service. These are typed scoped policy actions, not
kernel CAP bits. Mixing admin and use actions in a record is invalid. Unknown
bits/tags are invalid, including on revoked records; no partial decoding.

Maximum canonical record size is 896 bytes: 96 + 16*4 + 16*16 + 8*16 + 32 +
16*4 + 16*16. A decoder checks the input size and every count before allocation,
requires exact exhaustion, and constructs a temporary result before returning it.
Encoding and decoding do not confer authority or prove integrity/freshness. The
journal owns checksums, published-root binding, transaction audit and replay.
Corrupt input must never be repaired into a weaker or partial grant.

Mutation payload limit remains 4096 bytes and response page limit 8192 bytes,
with at most64 records per page. A page obeys both byte and count limits;64
maximum-sized records do not fit in a single page.
