---
id: dec-2026-10-01-session-registries
type: dec
title: "Complete D7 with private registries, fixed routes and session budgets"
date: 2026-10-01
status: standing
decided-by: user-vote
affects: [sub-kernel-devsrv, sub-kernel-srvconn]
created: 2026-10-01
---
## Fork

D7 selected per-session registries, but login still inherited the shared boot
registry. Trusted tombstones accumulated; creation authority, resident routes,
poster-death ownership and connection-resource isolation needed completion.

## Research

Plan 9 retains published channel references independently of descriptors;
Fuchsia explicitly routes providers into child namespaces; Genode parents
route requests and assign child budgets. The reviewed proposal applies those
ideas to Thylacine's identity-stamped connections and trusted restart names.
Source links and verified tree constraints are in
`docs/SRV-SESSION-REGISTRY-DESIGN.md`.

## Options

Larger tables postpone exhaustion. General tombstone recycling loses trusted
name protection. A mutable parent union leaks the shared posting surface.
A transferable factory descriptor is a possible later supervisor interface;
a dedicated spawn role fits the current Joey/login delegation model.

## The call

The operator answered "Approve the proposed contract and implementation" to
the concrete factory-role/syscall, fixed-route and resource-policy proposal.
Reserve syscall 127 and spawn permission bit 10. Keep the global 64-connection
bound; sessions may consume 48 combined and 16 each, with at most 16 retained
domains. Boot may use unused capacity. Domain charges survive retained transport
storage; name registries and resource domains have separate lifetimes.

## Rationale

Name isolation must not reset resource limits or weaken connect authority.
A dedicated factory role separates session creation from ordinary posting;
fixed routes preserve resident access without exposing a mutable boot alias.
Implementation and verification remain owed. No Main landing is implied.
