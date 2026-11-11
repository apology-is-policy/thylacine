---
id: chg-2026-11-11-aux-mac-app
type: chg
title: "The Mac app's first window lands with its headless test; run-vm grows the display modes it attaches to"
date: 2026-11-11
arc: arc-mac-runner
commits: ["f003526d8", "4f02edd44", "7c17bf6b6"]
touched:
  - sub-macrunner-app
  - sub-substrate-machine
  - sub-substrate-gates
established: []
closed: []
opened: []
mirrors-checked:
  - "tools/test.sh's skip table lists the display modes that drop gpu-mmio0: dbus and app added beside dbus-gl"
  - "docs/TOOLING.md and docs/DISPLAY-MODES.md name the two modes; DISPLAY-MODES section 5's testing-hybrid list gains dbus"
depth: skeletal
created: 2026-11-11
---
The first station of [[arc-mac-runner]] past the kernel: `macrunner/app` (crate `thylacine-mac`), a window that shows gpu0 and sends keys and the pointer, attaching to a QEMU run with `-display dbus,p2p=on`; its headless test ([[gate-macrunner-headless]]); and `THYLACINE_DISPLAY=dbus` and `=app` in `tools/run-vm.sh` (f003526d8), which drop `gpu-mmio0` so that gpu0 is the console a client is handed. The dossier is [[sub-macrunner-app]]; the surface is audit-bearing (a row in `docs/AUDIT-TRIGGERS.md`), and its first round is owed before anything of it reaches `main`.

**Measured without a guest** (QEMU 10.0.2 and 11.1.3, macOS 27.2.1): the pipeline's 1:1 output is QEMU's screendump byte for byte on both pixel paths, and 22 scripted events arrive as scripted. Three controls were red when they should be (a flipped shader, the missing Command key-up, a leg on the wrong pixel path).

**Found by reading the source before it landed**, none reachable by the test: a frame of zero or oversized dimensions reached Metal; a map longer than its file was accepted (macOS grants a 1 MiB shared mapping of an 8192-byte file, measured, and faults on the read); a frame was lost when the layer had no drawable; a trackpad scrolled a notch per event. Each is fixed, the first two and the last with unit tests.

**Owed:** the guest measurement (`docs/MAC-RUNNER.md` M-3: colour, damage rates, key to damage, the upload at 2560x1600), the audit round, a person's look at the glass.
