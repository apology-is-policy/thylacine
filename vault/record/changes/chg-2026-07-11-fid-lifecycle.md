---
id: chg-2026-07-11-fid-lifecycle
type: chg
title: "FID-LIFECYCLE: async-clunk + cached-open + the Larder re-size (the keeper stack)"
date: 2026-07-11
arc: arc-go-build
commits: ["61928c8d", "9aaea2eb"]
touched: [sub-kernel-ninep-dev9p, sub-kernel-ninep-client, sub-kernel-ninep-session]
established: []
closed: []
opened: [seam-co-fidless-wstat]
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The per-file fid-overhead attack (the measured 67% of the warm go-build
floor): fire-and-forget Tclunk on the close hot path (the tag reserved
until its ownerless Rclunk -- the async_clunk_tag_leak buggy cfg joined
9p_client.tla; `p9_session_has_free_tag` is the pool-full pre-check) + the
fidless cached open (query-Twalkgetattr revalidation + a budgeted
[0,size) snapshot; B2 strict / B1 loose variants) + the attr/dentry
4096-entry heap+hash re-size (task #25, the engagement fix). Design doc:
docs/FID-LIFECYCLE-DESIGN.md; as-built `9aaea2eb`.
