---
id: fnd-kt1-r1-c9
type: fnd
title: "`exec_chord` stamps the SENTINEL principal (0xFFFF_FFFE SYSTEM / 0xFFFF_FFFF NONE) on a chord-split leaf when the focused leaf hosts a system surface, where every other stamp site writes 0 (the environment)"
round: adt-kt1-r1
severity: P3
status: fixed
surface: [sub-tapestryd]
threatens: []
fixed-by: chg-2026-09-05-kt1-audit-close
regression: "none (a stamp value; read in source)"
created: 2026-09-05
---
## Prosecution

**File**: usr/tapestryd/src/server.rs:7437-7444 vs :186-192 (`actor_owner_principal`: Renderer/Client -> 0), usr/lib/libthyla-rs/src/lib.rs:573-575
**Prosecution**: `let owner = self.layout.leaf_surface(f).and_then(|n| self.surf(n)).map(|s| s.owner_principal).unwrap_or_else(|| self.layout.pane_owner_principal(f));` -- aurora's surface carries `owner_principal = T_PRINCIPAL_SYSTEM` (the sentinel the mint stamps from `peer_principal`, 2396-2403), so a Super+H on the console stamps 4294967294, which `PFK_OWNER` (14536-14554) now prints and `halcyon layout save` records. Every reader today treats it as non-session (`reap_session_empties` 6700-6705, `principal_is_session`, `owner_is_env`), so it is inert -- but the field's documented vocabulary is "0 = the renderer's / the environment" (pane.rs:396-398) and the H-4b close's reasoning about env-owned empties assumed 0. **Fix**: `let owner = if principal_is_session(owner) { owner } else { 0 };` at the chord site.

## Disposition

Fixed in 062efe18: `exec_chord` stamps the focused leaf's owner only when it is a real Session principal, else 0 (the environment), matching every other stamp site.
