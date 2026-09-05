---
id: fnd-kt1-r2-b5
type: fnd
title: "`session on` is refused E_BUSY while the previous conn's retire is still in flight, and the compositor gives up instead of retrying -- the connect retry loop the fix wrapped can now defeat itself"
round: adt-kt1-r2
severity: P3
status: fixed
surface: [sub-tapestryd]
threatens: []
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "ls-gfx-session (the declared bootstrap); the mid-retire race unconstructed"
created: 2026-09-05
---
## Prosecution

**File**: usr/halcyond/src/session.rs:426-458 (442-445); usr/tapestryd/src/server.rs:15517-15541 (15528-15531), 6708-6715
**Invariant**: robustness of the login -> compositor bootstrap (14.12 step 2)
**Prosecution**:
1. Per iteration the loop connects, declares, then mints: `if let Err(e) = r.global_ctl("session on") { say!(...); return None; }` then `match Surface::fullscreen_on(&r) { Ok(s) => return Some((r, s)), Err(e) => { ... sleep(CONNECT_DELAY_MS); } }` (session.rs:442-455). A failed `fullscreen_on` drops `r` at the end of the iteration and reconnects 25 ms later.
2. tapestryd keeps exactly one declared conn: `if let Some(&other) = comp.session_conns.first() { if other != self.conn_id { return Err(p9::E_BUSY); } ... }` (server.rs:15528-15531), cleared only by `retire_conn` when the old conn's teardown is processed (6708-6709).
3. The dropped ring's fids clunk asynchronously; if tapestryd has not processed that conn's teardown within the 25 ms nap (it is single-threaded and composes frames), the NEXT iteration's `session on` answers E_BUSY -> `return None` -> halcyond exits 1 -> login logs the user out. The same shape applies to a fast logout->login (the previous session's conn retire vs the new `session on`). Before the fix a failed `fullscreen_on` simply retried.
**Suggested fix**: treat a `session on` failure like the other two arms of the loop (retry with the nap up to `CONNECT_TRIES`), or declare only once a conn has succeeded in minting and write `session off` before dropping a conn whose mint failed.

## Disposition

Fixed with C2-F1 in the round-2 close: server-side the previous conn of the same principal is taken over even before its retire is processed, and an idle holder is taken over by anyone; client-side the declaration is retried through `DECLARE_TRIES` and then tolerated (undeclared), so the connect loop can no longer defeat itself.
