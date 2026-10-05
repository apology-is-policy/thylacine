---
id: chg-2026-10-05-halcyon-mode-chips
type: chg
title: "Show the focused transcript mode in an accent-filled status chip"
date: 2026-10-05
arc: arc-halcyon-interaction
commits: []
touched: [sub-halcyond, abi-boot-banner]
mirrors-checked:
  - "tools/interactive/ls-halcyon-modes.exp (EXTINCTION: only — modal status and focus witness)"
  - "tools/interactive/ls-halcyon-hidden-storage.exp"
  - "tools/interactive/ls-halcyon-storage-output.exp"
  - "tools/interactive/ls-halcyon-clipboard-pty-capacity.exp"
  - "tools/interactive/ls-halcyon-clipboard.exp"
  - "tools/interactive/ls-halcyon-pointer.exp"
  - "tools/interactive/ls-halcyon-session-media.exp"
  - "tools/interactive/ls-graphical-sak.exp"
  - "tools/interactive/ls-graphical-sak-states.exp"
  - "tools/interactive/ls-graphical-sak-recover.exp"
  - "tools/interactive/git-shell.exp"
  - "tools/interactive/ls-halcyon-session-dosbox.exp"
  - "tools/interactive/pci-net-load.exp"
  - "tools/interactive/im1-sak-lever.exp"
  - "tools/interactive/im3-lex-curiata.exp"
  - "tools/interactive/ls-bghome-stall.exp"
  - "tools/interactive/ls-imperium.exp"
  - "tools/interactive/haul-post.exp"
  - "tools/interactive/haul-cape.exp"
  - "tools/interactive/srv-connect-gate.exp"
  - "tools/test.sh"
  - "tools/smp-multiboot.sh"
  - "tools/test-cross-reboot.sh"
  - "tools/test-fault.sh (also the extinction MESSAGE bodies — see below)"
  - "tools/ci-idle-gate.sh"
  - "tools/np3-bench.sh"
  - "tools/verify-kaslr.sh (also `KASLR offset` — see below)"
  - "tools/warp/boot-probe.sh"
  - "tools/interactive/lib.exp"
  - "tools/interactive/dap-nora.exp"
  - "tools/interactive/rust-std-hello.exp (EXTINCTION: only — track R's std-on-device gate)"
  - "tools/interactive/lantern.exp (EXTINCTION: only — the deck presenter's gate)"
  - "tools/interactive/ls-halcyon-lantern.exp (EXTINCTION: only — the deck presenter in a Halcyon tile)"
  - "tools/interactive/ls-halcyon-lantern-haul.exp (EXTINCTION: only — the deck presenter on a Haul mount in a Halcyon tile)"
  - "tools/interactive/ls-halcyon-manual.exp (EXTINCTION: only — the Operator's Manual's Halcyon tasks on the default image)"
  - "tools/interactive/flood-174.exp"
  - "tools/interactive/freeze-172.exp"
  - "tools/interactive/ls-gfx-font.exp"
  - "tools/warp/quarry-wedge.exp"
  - "tools/stall-watch.py (`kernel base:` — see below)"
  - "tools/check-arc-gates.sh"
  - "tools/display-modes/verify-console-mode.exp"
  - "tools/display-modes/verify-gpu-headless-1b.exp"
  - "tools/interactive/item10-ctrlc.exp"
  - "tools/interactive/ls-gfx-age.exp"
  - "tools/interactive/ls-gfx-inline-view.exp"
  - "tools/interactive/ls-gfx-gallery.exp"
  - "tools/interactive/ls-gfx-jpeg.exp"
  - "tools/interactive/ls-gfx-restore.exp"
  - "tools/interactive/ls-gfx-session.exp"
  - "tools/interactive/ls-halcyon-instrument.exp"
  - "tools/interactive/ls-halcyon-session-instrument.exp"
  - "tools/interactive/ls-gfx-session-image.exp"
  - "tools/interactive/ls-halcyon.exp"
  - "tools/interactive/pty-susp-pouch.exp"
  - "tools/interactive/r5f9-ash.exp"
  - "tools/test-smp-classify.sh (the classifier's own fixtures — both literals)"
  - "tools/testdata/smp-classify/real-pass-harness.log (a classifier input fixture)"
  - "tools/warp/composed-screen.exp"
  - "tools/interactive/ls-gfx-dosbox.exp"
  - "tools/interactive/ls-gfx-dosbox-conf.exp"
  - "tools/interactive/ls-gfx-dosbox-duke3d.exp"
  - "tools/interactive/ls-gfx-dosbox-dynarec.exp"
  - "tools/interactive/ls-gfx-dosbox-input.exp"
  - "tools/interactive/ls-gfx-dosbox-tombraider.exp"
  - "tools/interactive/ls-gfx-throttle.exp"
  - "tools/interactive/im1-sak-lever.exp"
  - "tools/interactive/im3-lex-curiata.exp"
  - "tools/interactive/ls-imperium.exp"
  - "tools/interactive/ls-bghome-stall.exp"
  - "tools/interactive/s7-nora-probe.exp"
  - "tools/interactive/manual.exp"
established: []
closed: []
opened: []
depth: skeletal
---
Fixed-width Nora-role chips follow transcript navigation, selection and focus.
558 host tests, native1830/1830, live mode/focus and Lantern gates pass.
Application reports, proportional block caret and clipboard remain unfinished.
Operator-parked kernel lifetime work is preserved without further investigation.

Boot-banner mirror review: existing consumer bytes and contract body are unchanged
from HEAD; the new mode witness retains the EXTINCTION failure matcher. This is
a static compatibility sweep, not a rerun of the existing consumer scenarios.
Evidence: work/oct5-modal-ui/boot-banner-mirror-review.json.
