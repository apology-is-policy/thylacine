---
id: dec-2026-10-05-npxf-pake-v2
type: dec
title: "npxf's offline token oracle is closed by a PAKE in wire version 2"
date: 2026-10-05
status: standing
decided-by: user-vote
affects: [sub-haul]
created: 2026-10-06
---
## Fork

npxf's handshake has the server prove the token first, so anyone who can
connect can collect one reply and test token guesses against it offline.
Before the vote this was documented, haul warned on a token under 16 bytes,
and tokens had to be random.

The question put to the operator was "What should come next?", with two
options: keep and document (recommended: a random 32-byte token makes offline
guessing infeasible, and no wire change is needed; reordering the flights so
that the client proves first is no fix, because it moves the oracle to whoever
answers the dial); or plan a PAKE as npxf protocol version 2, with haul
support, which closes the oracle at the cost of a wire-format change across
npxf and haul.

## Decision

The operator voted on 2026-10-05: **plan a PAKE (wire v2)**, over the
recommendation. The work is enqueued (OPEN-BUGS 2026-10-05 18:40Z), scripture
first: HAUL-DESIGN 3 and 5 and npxf's own documents, then both sides of the
wire and new known-answer vectors.

## Follow-on

The research memo of 2026-10-06 recommends CPace (the CFRG's selected
balanced PAKE) on ristretto255 with SHA-512, explicit key confirmation, v1's
three-flight shape and record layer kept, and a hard cutover: a server that
also spoke v1 would keep the oracle open to anyone sending a v1 flight. An
augmented PAKE buys nothing for a single-user export whose token sits beside
the data it guards. ristretto255 needs libsodium in npxf; whether npxf takes
that dependency, or the design falls back to SPAKE2 over P-256 (RFC 9382) on
OpenSSL, is the operator's next call.
