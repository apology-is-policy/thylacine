# 149 — Warp: the GPU seam (I-45) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-warp-doc-absorb`).
The Warp arc's as-built log — the GPU seam from the kernel GPU-BO handle up
through the Mesa winsys, the fenced controlq, the Venus/virgl 3D path, and the
WSI swapchain. Audit-bearing; enforces **I-45** (GPU authority bounded by the
context). I-45 is **STAGED**: the guest-exposure half is enforced; the host half
on virgl/Venus is documented TRUSTED-not-enforced (`docs/GPU-DESIGN.md §9.2`), and
v3d is where it becomes ours to keep. Its content lives, code-verified and current,
in:

- the **kernel GPU substrate** — the GPU-BO subtype the caller cannot get wrong,
  the `SYS_DMA_CREATE` envelope (reject-at-the-door: unaligned / zero / wrapping /
  past-window), and the containment arithmetic that pins a GPU-BO Burrow's
  physical base inside the discovered shared-memory window (the I-45 guest half —
  the physical base never escapes the BAR):

      vault/system/kernel/devices/sub-kernel-hwcap.md   (guarded-by inv-i45)

- the **compositor + GPU host** — the `/dev/warp` tree tapestryd serves (the virgl
  context / GPU-BO mint / 3D submit half that "holds inv-i45"), the isolation
  contract F2 (a surface resolves only for its context; a never-reused host
  resource id bounded to one context's worth; `wring_kick`'s cross-ctx rejection),
  the fenced lane (the coherent shared-memory ring, one per `ring_idx`, a
  weft-shared guest blob), the Mesa winsys + warp client, the Venus integration
  (V-0 gating / V-0b context creates / V-1 guest blob / V-2 host-visible BAR /
  V-3 host3d rings), the WSI DIRECT path (W-3c / W-3d / W-3e), the present
  integration (Warp-4 mutual adoption), and the performance decompositions
  (#215 the hardware-GL frame path, C-4 the composed residual):

      vault/system/userspace/services/sub-tapestryd.md   (guarded-by inv-i45/i40)

- the **share substrate** — the GPU-BO Burrow-share and the weave share the seam
  rides:

      vault/system/kernel/memory/sub-kernel-burrow.md
      vault/system/kernel/async/sub-kernel-weft.md

The design intent + the I-45 staging (guest half enforced, host half trusted, v3d
the fork where it becomes ours) is `docs/GPU-DESIGN.md §8/§9.2`. I-45 has **no
spec module** (proven by `warp-prove` on thyla-pi's real V3D, not TLC).

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold.** The doc is a 2261-line as-built
  log of the whole Warp arc, most of it host-side (the Mesa/Venus/WSI integration
  — the TRUSTED-not-enforced half of I-45), tooling (`warp-prove`, `warp-host.sh`,
  `quarry`, the gate/coherent-ring/health provers), and measured performance. The
  load-bearing kernel + compositor halves of I-45 — the GPU-BO envelope and
  physical-base containment (hwcap), and the per-client context isolation +
  cross-ctx rejection + the fenced lane (tapestryd) — were verified present in
  the two today-current dossiers.
- **The staged rungs are cross-referenced, not re-homed.** The V-0..V-3 and
  W-3c/W-3d/W-3e rungs each have their own row in `docs/AUDIT-TRIGGERS.md` and
  land on tapestryd/mesa surfaces `sub-tapestryd` already carries; the provers and
  the thyla-pi measurements are as-built history, not a dossier's subject.
- **The content is distributed** — the kernel handle to `sub-kernel-hwcap`, the
  compositor/GPU host to `sub-tapestryd`, the share substrate to
  `sub-kernel-burrow` / `sub-kernel-weft`, the design + I-45 staging to
  `GPU-DESIGN.md`.
