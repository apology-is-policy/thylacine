---
id: chg-2026-09-05-h4d2a-rich-tiles
type: chg
title: "H-4d-2a: a pts slave is a terminal (SYS_FD_DEVCLASS 't'), the Beacon gate admits it, the pts host declares the tier (kaua-term --beacon), ut's pts branch arms from the inheritance -- session tiles were plain by construction until this"
date: 2026-09-05
arc: arc-tapestry
commits: ["8f553c78"]
touched: [sub-kernel-syscall-dispatch, sub-beacon, sub-kernel-syscall-abi, sub-utopia-interactive, sub-kaua-term, sub-halcyond]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
no-dossier-change: "sub-kernel-syscall-dispatch's delta is one classifier (`spoor_devclass`: the devdev cons normalization + the pts-SLAVE 't' arm via `pts_resolve_spoor`; the master stays '9'; `spoor_is_console` untouched) replacing the inline dc pick in `sys_fd_devclass_handler`, + the devdev.fd_devclass test's negative arms. sub-beacon: `DC_PTS` + the Auto arm admitting 'c' or 't' (+3 host cases). sub-kernel-syscall-abi (libthyla-rs): `stdout_is_terminal()` = 'c' || 't'. sub-utopia-interactive (ut): the jc branch's `env_beacon_tier` arm (the inherited /env/BEACON is the pts host's word; zones arm iff rich AND stdout_is_terminal; no per-prompt re-read). sub-kaua-term: `--beacon <tier>` -> its own /env/BEACON before spawn_on_slave, absent = none. sub-halcyond: `SessionTile::spawn` passes `--beacon rich`. The vault peer folds these from this record + docs/SYS-FD-DEVCLASS-SPEC.md (the 't' row, the pts normalization, the AS-BUILT addendum), docs/BEACON.md 12.4 (amended), docs/KAUA-TERM.md (R1 AS-BUILT), docs/reference/150-halcyond.md ('Rich tiles') + 152-kaua-term.md (the --beacon paragraph)."
created: 2026-09-05
---
Every session tile had been PLAIN by construction, and nothing had asserted otherwise: `ut` resolved its Beacon tier only inside its console branch (its own comment: a pts-hosted ut "takes this branch never"), the console's `/dev/beacon` describes the console renderer, and a pts slave answered dev9p's `'9'` to `SYS_FD_DEVCLASS`, so the Auto gate every native tool composes refused a tile's stdout. The d-1 gate log carries no `ut: beacon` line for any tile. KAUA-TERM.md R1 / HALCYON 14.3 + 14.6 had named the missing half since KT-1; H-4d's welcome depends on it, so it was pulled forward (a current-chunk dependency defaults to BUILD-now). Heritage: rio's window IS the program's `/dev/cons`; Unix pairs `isatty()` (the class: a terminal) with `TERM` (the env: which). The kernel's part keys on its own pts registry through the tty seam's resolve (pointer identity on the SrvConn, a ref-held binding, never a qid bit); I-27 is untouched. In-guest witness: the tile shell's `ut: beacon rich (transcript zones armed)` (ls-gfx-session), the line only the `'t'` answer + the rich inheritance produce; the kernel test pins the negative arms (no fixture builds a dev9p Spoor over a live SrvConn). A ptyhost-hosted ut now says `beacon tier not advertised by the pts host (plain)` -- correct. Unaudited by the double-the-distance rule (the H-arc round; the kernel arm is on the H-1 SYS_FD_DEVCLASS audit surface, row 136).
