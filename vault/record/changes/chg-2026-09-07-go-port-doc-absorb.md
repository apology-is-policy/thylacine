---
id: chg-2026-09-07-go-port-doc-absorb
type: chg
title: "absorb docs/reference/133-go-port (GOOS=thylacine capability map): fold the Loom SETATTR truncate-only fail-close into sub-kernel-loom; fork is external"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched: [sub-kernel-loom]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
A capability map for the native Go toolchain. The FORK is external
(~/projects/go-thylacine, ~54 *_thylacine* files) -- not a Thylacine-tree
surface, exactly as gopls (137). Verified the in-tree atoms atom-by-atom.

WHERE EACH IN-TREE ATOM LIVES (verified against the dossiers + code):
- T_WSTAT_SIZE (the 4th SYS_WSTAT axis): SIZE=content demands RIGHT_WRITE + skips
  perm_wstat_check; INT64_MAX bound; the #81 O_PATH/CWALKONLY hollow-rights
  truncate reject -> sub-kernel-syscall-dispatch (handler reject+bound, incl. the
  Prosecution line) + sub-kernel-perm (SIZE-has-no-policy-arm). FULLY covered.
- env chain: login seeds /env/{HOME,USER,PATH}, kernel-env child inherit, joey's
  go4c hermetic env unlinked-after -> sub-stratum-boot + sub-stratum-session.
- libthyla_rs::env::var read side + the no-set_var seam -> sub-libthyla-rs.
- nora gofmt-on-save -> sub-nora-host.

THE FOLD (genuine gap -> sub-kernel-loom, depth rich):
- The Loom SETATTR async twin was HOMELESS (grep: loom_setattr_e2e / truncate-only
  / async identity-setattr -> ZERO vault hits). The audit found the async
  LOOM_OP_SETATTR path had the same O_PATH truncate bypass PLUS ran no identity
  check at all; closed by splitting on authority-kind: SIZE stays (RIGHT_WRITE,
  non-O_PATH, s64 bound), MODE/UID/GID reject fail-closed -> v1.0 Loom SETATTR is
  truncate-only (the async submit cannot run the sync owner-only perm_wstat_check
  without a blocking stat). Folded into the "Pin at submit" section (it IS one of
  the 15 dispatching opcodes; the authority-kind split is the natural extension of
  the submit-time rights snapshot). updated: 08-16 -> 09-07.

Redirect stub. Fork findings (capMask=^0 inherit, resolver ORDER, os.Executable
Args[0]) named as external. Zero Thylacine-tree code change beyond the fold.
