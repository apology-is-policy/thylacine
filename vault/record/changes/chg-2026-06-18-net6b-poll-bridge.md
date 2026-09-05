---
id: chg-2026-06-18-net6b-poll-bridge
type: chg
title: "net-6b: the dev9p.poll readiness bridge (spec-first) + netd ready + pouch poll"
date: 2026-06-18
arc: arc-net
commits: ["fea625bc", "8bbcb2a5", "eeab08e8", "9ef2de67", "c49313fa"]
touched: [sub-kernel-ninep-dev9p-poll, sub-kernel-ninep-dev9p]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The net arc's one kernel ABI: model-first (`fea625bc` net_poll.tla), the
netd `ready` file (`8bbcb2a5`), the QTPOLL qid bit reservation
(`eeab08e8`), the kernel bridge + global poll-pump kthread (`9ef2de67`),
the pouch poll()/select() consumer (`c49313fa`). The formal close is
[[chg-2026-06-18-net6b4-close]].
