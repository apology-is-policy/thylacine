---
id: fnd-kt1-r1-c5
type: fnd
title: "the session compositor is capped at 4 surfaces per conn (F9 `MAX_SURFACES_PER_CONN`), so the 5th tile never hosts; the split leaves a focused EMPTY leaf and the keyboard goes dead (keys dropped: no focused surface)"
round: adt-kt1-r1
severity: P2
status: fixed
surface: [sub-tapestryd]
threatens: [inv-i32]
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "unconstructed at 5 tiles; the declared mint path runs in ls-gfx-session"
created: 2026-09-05
---
## Prosecution

**File**: usr/tapestryd/src/server.rs:152, :13385-13391 (h_lopen mint cap), :7011-7022 (key_event drops), usr/halcyond/src/session.rs:331-334 (open_claim_on Err -> `continue`, silent), usr/tapestryd/src/pane.rs:465/504 (split focuses the new leaf)
**Invariant**: I-32 (resource floor -- a bound must fail CLEAN and legibly); HALCYON.md 14.11.6/14.12 multi-tile ("N tiles").
**Prosecution**:
1. The session halcyond is one conn (the H-3c-2 event set: "ONE 9P session ... per client") and is not the renderer (`peer_is_renderer` false, 15412-15418), so its mint cap is `MAX_SURFACES_PER_CONN` = 4 (13385-13388): `if comp.owned_count(self.conn_id) >= cap { return self.err(tag, p9::E_NOMEM); }`.
2. Root tile + 3 Super+H splits = 4 surfaces. The 4th Super+H: `exec_chord Split` creates leaf E, stamps the session principal (7437-7444), `split` sets `self.focused = new_leaf` (pane.rs:465/504) -> reconcile -> `focused_surface()` = None.
3. halcyond reconcile: `mint_claim` succeeds (E is its own empty leaf), `Surface::open_claim_on(ring, w, h, token)` fails at the `surface/new` open with E_NOMEM -> `Err(_) => continue` (session.rs:333) -- nothing said. E stays empty and FOCUSED; the claim is re-minted on every relayout (last mint wins) and refused again each time.
4. Every key: `key_event` -> `self.layout.focused_surface()` = None -> "key {} dropped (no focused surface)" (7011-7022). The user sees a blank focused pane and a dead keyboard until they Super+arrow away or Super+W the empty leaf; nothing tells them why.
5. Observes: the effective tile bound is 4, not MAX_PANES, and the failure is silent + focus-stealing. ls-gfx-session opens exactly 2 tiles.
**Suggested fix**: give `Actor::Session` conns a cap sized like the renderer's (`MAX_SURFACES_PER_CONN + MAX_PANES`; the global pool math at 222 must then grow accordingly), and in session.rs say the refusal once per leaf and close (or unfocus) a leaf it cannot host rather than leaving a focused empty. Witness: a lever leg that splits past the cap and asserts either a hosted 5th tile or a `close`d leaf with focus on a live tile.

## Disposition

Fixed in 062efe18: a DECLARED session conn mints up to `MAX_SURFACES_PER_RENDERER` (the pool grew by `2 * MAX_PANES` so the bound is exact: 36 + 36 + 6 x 4 = 96); a refused claim no longer leaves a focused empty leaf with a dead keyboard -- halcyond says once and closes the leaf. A five-tile session is unconstructed by any gate; the cap path is the one every ls-gfx-session tile takes.
