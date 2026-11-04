---
id: dec-2026-11-04-account-control-sandbox-sak-rerun
type: dec
title: "A child account's control stays with its maker, a sandbox reads its own chain, a job with nothing answered re-runs"
date: 2026-11-04
status: standing
decided-by: user-vote
affects: [sub-kernel-accounts, sub-interaction-native]
created: 2026-11-04
---
## Fork

Three, asked together. How a parent's control over a child memory account reaches it, given that handles cannot move between Procs today. Whether a sandboxed Proc needs less than the account query shows it. Whether a copy or paste whose connection was closed under it may run again, since a SAK closes an idle connection and nothing tells the client until its next use.

## Research

Fork copies only transferable kinds (`handle_slot_may_alias`), spawn's descriptor array takes only Spoors, and `handle_transfer_via_9p` is named and not built. Spawn's record grows by SPAWN_EXT_* tails. Joey spawns login directly and login stays alive as the session's parent, so the session owner is already a long-lived direct parent. Genode's quota trading moves budget down the parent chain with no ambient id; Zircon passes a job handle in process args. For the sandbox: the query reaches no address, lock or per-key data, and the alternative to asking a budget is allocating until refused. For the re-run: AS-4c measured 39 cut points, the value intact at every one, and a SAK before Bind invisible, during a job one failed key, and the next use recovers.

## Decision

Votes 58 to 60 of 2026-11-04, each the recommended option. 58: any Proc may make a child of its own account and holds a non-transferable handle to it; naming the handle in a spawn tail starts the new Proc under it; login holds the session's and joey the services'. 59: no capability gate and no coarsening; giving sessions children narrows the query to the session, and a sandbox that should see less gets its own child. 60: a copy before its Commit, or any paste, runs again once on a fresh connection if nothing was answered on the old one. Text in SHARED-MEMORY-ACCOUNTING 13 and ASYNC-SERVICE-LIFECYCLE. Vote 61 deleted a merged local branch and has no bearing here.

## Rejected

58: adding the kind to spawn's descriptor array, which moves it to the transferable side of I-4 and I-5 and changes what the handles spec's partition means; waiting for transfer over 9P, which holds MM-2 behind a large unbuilt path. Accepted cost: a creator that exits leaves its limits fixed. 59: a capability gate, which leaves only the destructive probe; rounding the figures, which costs an ABI rule and blinds programs near their limit. 60: keeping the written rule, one failed key after every trusted-path episode; watching the idle connection, which costs an armed poll in every client and still fails a job started as the close lands.
