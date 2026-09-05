---
id: fnd-kt1-r3-c6
type: fnd
title: "the takeover rule lets a same-principal `session on` steal the seat from a LIVE compositor that hosts tiles (the battery's new declared control does exactly this if run inside a session), and the declare -> first-create window lets a foreign IDLE re-claimer demote a legitimate compositor that then believes itself declared"
round: adt-kt1-r3
severity: P3
status: fixed
surface: [sub-tapestryd]
threatens: []
fixed-by: chg-2026-09-05-kt1-audit-close-r3
regression: "ls-gfx-session (the declared bootstrap incl. the re-declare); takeover arms unconstructed"
created: 2026-09-05
---
## Prosecution

**File**: usr/tapestryd/src/server.rs:15579-15607 (`if other == self.conn_id { return Ok(()) } ... if other_p != self.peer_principal && comp.conn_hosts(other) { E_BUSY } ... comp.session_conns.clear(); ... push((self.conn_id, self.peer_principal))`), :15609 (`"off" => retain(c != self.conn_id)` -- no restore of a previous holder); usr/halcyond/src/session.rs:457-475 (`declared` is decided ONCE, before `fullscreen_on` at 478), :579 (the "(undeclared)" marker rides that one verdict); usr/tapestry-battery/src/main.rs:1635, 1677 (`a.global_ctl("session on")` ... `("session off")`)
**Invariant**: I-22 (holds: neither arm grants pane authority across principals -- `actor_owns_subtree_all` etc. are untouched); HALCYON 13.7's ratified same-principal mutual authority (covers arm (a)); the C2-F1 disposition's stated semantics ("a restarted compositor whose dead conn is not yet retired") vs what the code admits
**Prosecution**:
(a) Same principal, live holder. State: halcyond H (conn c1, principal p) declared, hosting tiles. The user runs `tapestry-battery` (or a second `halcyond --session`, or any program writing `session on`) in a tile: `other_p == p` -> the `conn_hosts` test is skipped -> takeover (:15591-15604), `session_conns = [(c2, p)]`. Consequences for H: `session_declared(c1)` false -> its mint cap drops to `MAX_SURFACES_PER_CONN` = 4 (:13474-13476) -> the next split past 4 surfaces is E_NOMEM -> `close` (session.rs:399-414); `session_notify_surface` now names c2 -> H gets no `TEV_LAYOUT`; when the battery writes `session off` (:1677) `session_conns` is EMPTY -> `has_session_tree` false -> the console un-backgrounds beside H's tiles; nothing re-declares c1 (halcyond never re-issues `session on`). The live session is degraded until logout. Authority-neutral (13.7), self-inflicted -- but the battery now ships this verb, and the seat model's "restarted compositor" case did not need the LIVE-holder arm.
(b) Foreign idle re-claim. State: a previous user's orphan spins on `session on` (the round-2 C2-F1 chain established that orphans survive logout). User A logs in; `connect()` declares (takeover of the idle foreign holder, correct) and then mints `fullscreen_on` -- two RPCs later. Between them the orphan re-declares: A's conn hosts nothing yet -> `conn_hosts(A)` false -> the orphan takes the seat back. A's `fullscreen_on` mints on an undeclared conn (cap 4, no backgrounding, no LAYOUT) while `declared == true` -> `session up 1280x800 px` with no "(undeclared)" marker. A runs the degraded mode for its whole session, mislabelled. Strictly better than round 1 (a login loop), and a contention on a shared resource rather than pane authority -- P3.
**Suggested fix**: declare AFTER the first surface hosts (or re-issue `session on` after `fullscreen_on` and take THAT verdict as `declared` -- the repeat is idempotent for the holder, :15580-15582), so `conn_hosts` protects a legitimate compositor from the first RPC it is visible; for arm (a), take over a same-principal holder only when its peer is dead (Comp would need the holder's srv handle, or `Conn::new`-style `t_srv_peer` liveness stored beside the conn id) or keep a one-deep previous-holder that `off`/`retire_conn` restores. Witness: the two takeover arms the dossier lists as unconstructed.

## Disposition

Fixed in the round-3 close, both arms. (a) The same-principal takeover of a LIVE holder is gone: `session on` takes the seat over only from a holder that hosts nothing, whoever the parties are -- a same-principal newcomer is the user's own program, and stealing the seat from the user's live compositor degraded it for the session (the mint cap, no `TEV_LAYOUT`, no re-declare); the restart case never needed the exception, because a crashed compositor's conn is retired and un-declared as soon as its EOF is serviced, within halcyond's retry budget. (b) halcyond re-issues `session on` after its first surface hosts and takes THAT verdict as `declared` (idempotent for the holder, a takeover of an idle usurper), so an idle re-claimer in the declare -> first-mint window cannot leave it running undeclared while mislabelled. Both takeover arms remain unconstructed by a gate; the declare/re-declare path runs in ls-gfx-session.
