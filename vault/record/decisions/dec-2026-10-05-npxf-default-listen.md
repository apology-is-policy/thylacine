---
id: dec-2026-10-05-npxf-default-listen
type: dec
title: "npxf-server listens on localhost unless told otherwise"
date: 2026-10-05
status: standing
decided-by: user-vote
affects: [sub-haul]
created: 2026-10-06
---
## Fork

Haul's P3c pass on npxf (the export server Haul talks to) changed
npxf-server's default listen address from every interface to localhost. A
server started without `-l` is then unreachable from other machines. None of
Thylacine's gates notices: each one passes `-l`.

The question put to the operator was "Keep that change in the branch you'll
push?", with two options: keep localhost, so that serving remotely needs an
explicit `-l :5640`, as the README's quick start already shows; or keep every
interface, relying on P3c's pre-authentication bounds (a 15 s handshake
deadline and a per-source cap) and documenting the exposure.

## Decision

The operator voted on 2026-10-05: **keep localhost**. The change stays in
npxf's `p3c-final` branch, which the operator pushes. npxf's usage text and
README say that without `-l` the server listens on localhost only.

## Rationale

Safe by default: a server reachable from the network is one the person running
it asked for. The pre-authentication bounds limit what an unauthenticated peer
costs the server; they do not make an unintended exposure intended.
