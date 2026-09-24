---
id: chg-2026-09-24-lex-curiata-fidelity
type: chg
title: "Restore Lex curiata dialog and make F10 primary"
date: 2026-09-24
arc: arc-astra-halcyon-followup
commits: ["*(pending)*"]
touched: [sub-lictor, sub-imperium]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-24
---
Replace the compact monospace prototype with the approved Lex curiata layout,
baked Plex/Cornucopia type, exact capability names and bounded compact layout.
Promote the already-supported Ctrl-Alt-F10 chord in help, manual and all three
graphical gates; preserve the legacy Delete chord. Kernel authority and seat
ABI are unchanged. Host tests pass 25/25, the three QEMU graphical gates pass,
and a fresh boot passes 1667/1667 kernel tests. Actual screenshots were
inspected. The backdrop remains the private neutral fallback; a separate
review proposes completed private capture. Review is explicitly single-agent.
