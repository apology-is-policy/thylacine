---
id: view-audit-triggers
type: view
title: "Audit-trigger surfaces"
query: audit-triggers
---
# Audit-trigger surfaces

Generated from note fields — do not edit between the markers
(`quaestor render`). Replaces: the CLAUDE.md trigger table + the ARCH section-25.4 twin.

<!-- generated:begin -->
| surface | code | invariants | prosecution |
|---|---|---|---|
| [[sub-aurora]] | usr/aurora/src/main.rs, usr/aurora/src/vt.rs, usr/aurora/src/render.rs, usr/aurora/src/osd.rs, usr/aurora/src/config.rs, usr/aurora/Cargo.toml | inv-i27 | - **The drain and feed opens must precede surface creation.** Without the |
| [[sub-corvus]] | usr/corvus/src/main.rs, usr/corvus/Cargo.toml | inv-i22, inv-i23 | - **The authority gates re-query.** Any new gated verb must ask the kernel |
| [[sub-corvus-crypto]] | usr/lib/corvus-crypto/src/lib.rs, usr/lib/corvus-crypto/src/bip39_wordlist.rs, usr/lib/corvus-crypto/Cargo.toml |  | - **A new wrap kind needs a new associated-data prefix**, built through a |
| [[sub-diorama]] | usr/diorama/src/server.rs, usr/diorama/src/main.rs, usr/diorama/Cargo.toml | inv-i43 | - **Every new file needs a native source.** The rule is at the top of the |
| [[sub-kernel-addrspace]] | kernel/addrspace.c, kernel/include/thylacine/addrspace.h, kernel/cow.c, kernel/include/thylacine/cow.h | inv-i44, inv-i32 | What a change must re-establish: |
| [[sub-kernel-allowance]] | kernel/allowance.c, kernel/include/thylacine/allowance.h | inv-i34 | - The gate must remain complete across **all four** create sites. A new |
| [[sub-kernel-alternatives]] | arch/arm64/alternatives.c, arch/arm64/alternatives.h, arch/arm64/atomic_lse.h |  | - **The write must never go through an executable mapping.** The scratch alias's |
| [[sub-kernel-asid]] | arch/arm64/asid.c, arch/arm64/asid.h | inv-i31 | - **The rollover-versus-switch race** is the whole surface. Any change to the |
| [[sub-kernel-boot-entry]] | arch/arm64/start.S, arch/arm64/kernel.ld | inv-i16, inv-i21 | - **The eret window.** Any hand-rolled path that sets `ELR_EL1` and returns to |
| [[sub-kernel-boot-sequence]] | kernel/main.c, arch/arm64/hwfeat.c, arch/arm64/hwfeat.h | inv-i15 | - **Every reordering is a potential correctness change**, and the dependencies are |
| [[sub-kernel-burrow]] | kernel/burrow.c, kernel/include/thylacine/burrow.h | inv-i7, inv-i32 | - **The free decision must stay under the lock and the free must stay outside |
| [[sub-kernel-caps]] | kernel/include/thylacine/caps.h, kernel/devcap.c, kernel/proc.c |  | - A new capability bit must be added to `CAP_ALL` **or** to |
| [[sub-kernel-cons]] | kernel/cons.c, kernel/include/thylacine/cons.h | inv-i27, inv-i9 | - **Nothing that needs [[lock-proc-table]] or a hook-list walk may be called |
| [[sub-kernel-content]] | kernel/devramfs.c, kernel/devenv.c, kernel/env.c, kernel/include/thylacine/env.h, kernel/random.c, kernel/chacha20.c, kernel/include/thylacine/random.h, kernel/include/thylacine/chacha20.h | inv-i1, inv-i16, inv-i28, inv-i32, inv-i33 | - **The random buffer's first fill is not secret, and only boot ordering hides |
| [[sub-kernel-death]] | kernel/proc.c | inv-i24, inv-i9, inv-i44 | The #811 audit's **verified-sound set** is the do-not-re-prosecute preamble |
| [[sub-kernel-devdev]] | kernel/devdev.c | inv-i27 | - **A new console-adjacent leaf must be added to the right gate set.** The sets |
| [[sub-kernel-devproc]] | kernel/devproc.c | inv-i26, inv-i39 | - **The four gates must not converge.** Each near-miss is a decision: |
| [[sub-kernel-devsrv]] | kernel/devsrv.c, kernel/include/thylacine/devsrv.h | inv-i1 | What an auditor attacks here: |
| [[sub-kernel-dtb]] | lib/dtb.c, kernel/include/thylacine/dtb.h | inv-i15 | - **Property order independence.** Any new node-matching lookup must accumulate |
| [[sub-kernel-elf]] | kernel/elf.c, kernel/include/thylacine/elf.h | inv-i12 | On any change: that the W^X check stays **above** the switch, so a new segment |
| [[sub-kernel-exception]] | arch/arm64/vectors.S, arch/arm64/exception.c, arch/arm64/exception.h, arch/arm64/userland.S | inv-i21, inv-i13, inv-i24, inv-i39 | - **Any new hand-rolled `eret` to EL0 must mask across the link-register-set to |
| [[sub-kernel-exec]] | kernel/exec.c, kernel/include/thylacine/exec.h | inv-i36, inv-i12, inv-i32, inv-i33, inv-i44 | On any change: that the Spoor ref ledger balances on every path — the Image |
| [[sub-kernel-fault]] | arch/arm64/fault.c, arch/arm64/fault.h | inv-i12, inv-i32, inv-i7, inv-i36, inv-i44 | On any change here: that the file arm's four-step protocol keeps its pin across |
| [[sub-kernel-gic]] | arch/arm64/gic.c, arch/arm64/gic.h | inv-i15, inv-i18 | - The two generations are **different code for the same behaviour**, and a run |
| [[sub-kernel-halls]] | arch/arm64/halls.c, arch/arm64/halls.h, arch/arm64/halls_symtab.h, arch/arm64/halls_symtab.stub.c |  | On any change, re-establish: |
| [[sub-kernel-handle]] | kernel/handle.c, kernel/include/thylacine/handle.h |  | - A new `kobj_kind` must be classified into exactly one partition; the |
| [[sub-kernel-hwcap]] | kernel/mmio_handle.c, kernel/include/thylacine/mmio_handle.h, kernel/dma_handle.c, kernel/include/thylacine/dma_handle.h, kernel/pci_handle.c, kernel/include/thylacine/pci_handle.h | inv-i5, inv-i32, inv-i34 | - The three exclusivity mechanisms are **different code for the same property**. |
| [[sub-kernel-hwdebug]] | arch/arm64/hwdebug.c, arch/arm64/hwdebug.h | inv-i39 | On any change, re-establish: |
| [[sub-kernel-image]] | kernel/image.c, kernel/include/thylacine/image.h | inv-i36, inv-i7 | On any change: that every path still clunks the Spoor exactly once — miss adopts, |
| [[sub-kernel-irqfwd]] | kernel/irqfwd.c, kernel/include/thylacine/irqfwd.h | inv-i9 | - The pre-seeded reservations must cover every number the kernel attaches |
| [[sub-kernel-jobctl]] | kernel/proc.c, kernel/include/thylacine/proc.h | inv-i20, inv-i39, inv-i9, inv-i19 | - The two stop flags must stay separate, and each resume must clear only its |
| [[sub-kernel-kaslr]] | arch/arm64/kaslr.c, arch/arm64/kaslr.h | inv-i16 | - **The never-zero guarantee** must survive any change to the mask or the mixing. |
| [[sub-kernel-larder]] | kernel/larder.c, kernel/include/thylacine/larder.h | inv-i38 | - **The gen-ring event-logging completeness**: every NEW mutation path |
| [[sub-kernel-loom]] | kernel/loom.c, kernel/include/thylacine/loom.h | inv-i29, inv-i30, inv-i32 | - **Never compute an index from a shared word.** The private counter and private |
| [[sub-kernel-mm-phys]] | mm/phys.c, mm/phys.h, mm/buddy.c, mm/buddy.h, mm/magazines.c, mm/magazines.h, kernel/include/thylacine/page.h |  | - Any new caller of `pa_to_kva` on an allocator-returned PA is bound |
| [[sub-kernel-mm-slub]] | mm/slub.c, mm/slub.h |  | - The destroy guard must stay `alloc_count - free_count` — reverting |
| [[sub-kernel-mmu]] | arch/arm64/mmu.c, arch/arm64/mmu.h | inv-i12, inv-i13, inv-i16, inv-i31, inv-i39 | On any change here: that no PTE constructor can produce writable-and-executable |
| [[sub-kernel-ninep-attach]] | kernel/9p_attach.c, kernel/include/thylacine/9p_attach.h |  | - **The failure-path ledger**: every exit must leave (adapter ref × |
| [[sub-kernel-ninep-client]] | kernel/9p_client.c, kernel/9p_session.c, kernel/9p_transport.c, kernel/9p_srvconn_transport.c, kernel/9p_transport_mq.c, kernel/9p_attach.c, kernel/include/thylacine/9p_client.h | inv-i9, inv-i10, inv-i11 | What an auditor attacks here (the single home of the trigger-row content for |
| [[sub-kernel-ninep-dev9p]] | kernel/dev9p.c, kernel/include/thylacine/dev9p.h | inv-i38 | - **The coherence pairing**: every mutation path must carry its exact |
| [[sub-kernel-ninep-dev9p-poll]] | kernel/dev9p_poll.c | inv-i9 | - **The I-9 window**: any reordering of register-hook / ensure-probe / |
| [[sub-kernel-ninep-session]] | kernel/9p_session.c, kernel/include/thylacine/9p_session.h | inv-i10, inv-i11 | - **The retirement matrix**: any new path that clears an `awaiting_flush` |
| [[sub-kernel-ninep-transport]] | kernel/9p_transport.c, kernel/9p_spoor_transport.c, kernel/9p_srvconn_transport.c, kernel/9p_transport_loopback.c, kernel/9p_transport_mq.c, kernel/include/thylacine/9p_transport.h |  | - **The EAGAIN classification boundary**: EAGAIN accepted anywhere past |
| [[sub-kernel-ninep-wire]] | kernel/9p_wire.c, kernel/include/thylacine/9p_wire.h |  | What an auditor attacks here (changes to this surface ride the |
| [[sub-kernel-notes]] | kernel/notes.c, kernel/include/thylacine/notes.h, kernel/devnotes.c | inv-i19, inv-i9, inv-i39 | On any change: that peek, pop and push stay inside one lock hold, and that every |
| [[sub-kernel-path]] | kernel/path.c, kernel/include/thylacine/path.h | inv-i33 | - The refcount balances on EVERY create/destroy/replace path (the #66a |
| [[sub-kernel-perm]] | kernel/perm.c, kernel/include/thylacine/perm.h |  | - No `principal_id` may ever be special-cased here. Adding a |
| [[sub-kernel-pipe]] | kernel/pipe.c, kernel/include/thylacine/pipe.h | inv-i9 | - Every mutation that can enable a waiter must keep its wake — the |
| [[sub-kernel-poll]] | kernel/poll.c, kernel/include/thylacine/poll.h | inv-i9 | - The sweep's three phases must keep their order: unregister → |
| [[sub-kernel-proc]] | kernel/proc.c, kernel/include/thylacine/proc.h | inv-i1, inv-i32, inv-i33, inv-i44 | - The `rfork` ledger: every field is inherited, freshened or stripped |
| [[sub-kernel-pts]] | kernel/pts.c, kernel/include/thylacine/pts.h | inv-i20, inv-i1, inv-i22, inv-i9 | - The signal call must never grow a target parameter. The invariant is the |
| [[sub-kernel-rendez]] | kernel/sched.c, kernel/include/thylacine/rendez.h | inv-i9, inv-i8 | - **The unconditional `r->lock` acquire in `wakeup` is LOAD-BEARING** |
| [[sub-kernel-sched]] | kernel/sched.c, kernel/include/thylacine/sched.h | inv-i8, inv-i17, inv-i21 | - **The mask-before-read rule holds at every per-CPU read.** Any new site |
| [[sub-kernel-sched-smp]] | kernel/sched.c, kernel/smp.c, arch/arm64/context.S | inv-i21, inv-i18, inv-i8, inv-i9 | - **The claim happens under the victim's lock.** Moving `on_cpu = true` |
| [[sub-kernel-spoor]] | kernel/spoor.c, kernel/include/thylacine/spoor.h | inv-i33 | - **The refcount balances on every path.** `spoor_total_allocated` / |
| [[sub-kernel-srvconn]] | kernel/srvconn.c, kernel/include/thylacine/srvconn.h | inv-i9 | What an auditor attacks here (the CLAUDE.md CF-3 B row absorbed): |
| [[sub-kernel-stalk]] | kernel/stalk.c, kernel/include/thylacine/stalk.h | inv-i28, inv-i33 | Standing obligations for any change (the ARCH §25.4 POUNCE row is the |
| [[sub-kernel-syscall-abi]] | kernel/include/thylacine/syscall.h, usr/lib/libt/include/thyla/syscall.h, usr/lib/libt/include/thyla/poll.h, usr/lib/libt/src/start.S, usr/lib/libthyla-rs/src/lib.rs | inv-i5, inv-i13, inv-i32 | - **A new syscall appends. It never fills a hole.** The three retired numbers |
| [[sub-kernel-syscall-dispatch]] | kernel/syscall.c | inv-i13, inv-i32, inv-i22, inv-i27, inv-i34, inv-i43, inv-i44 | - **A new syscall's authority gate goes in the inner, not the handler**, unless |
| [[sub-kernel-territory]] | kernel/territory.c, kernel/include/thylacine/territory.h | inv-i1, inv-i3, inv-i33 | On any change to this file, prosecute: |
| [[sub-kernel-thread]] | kernel/thread.c, kernel/include/thylacine/thread.h | inv-i21, inv-i31, inv-i44 | - **#788 is the shape to keep in mind.** `thread_free` freeing a |
| [[sub-kernel-timer]] | arch/arm64/timer.c, arch/arm64/timer.h, arch/arm64/rtc.c, arch/arm64/rtc.h | inv-i15, inv-i17 | - The periodic path must stay byte-unchanged for a running CPU — the slice model |
| [[sub-kernel-torpor]] | kernel/torpor.c, kernel/include/thylacine/torpor.h | inv-i9, inv-i24 | - The lock-free mismatch return must never be extended to the EQUAL |
| [[sub-kernel-uaccess]] | arch/arm64/uaccess.S, arch/arm64/uaccess.c, arch/arm64/uaccess.h | inv-i13 | - **A new fault point needs a table entry.** The entry is what separates |
| [[sub-kernel-vivarium]] | kernel/vivarium.c, kernel/include/thylacine/vivarium.h | inv-i43 | What a change must re-establish: |
| [[sub-kernel-vma]] | kernel/vma.c, kernel/include/thylacine/vma.h | inv-i12, inv-i7, inv-i32 | The things to re-examine when this file changes: that `vma_alloc` remains the |
| [[sub-kernel-weft]] | kernel/weft.c, kernel/include/thylacine/weft.h | inv-i37, inv-i30, inv-i9, inv-i32 | - **Admission stays kernel-minted.** Anonymous, or the allocation-time |
| [[sub-libdriver-discovery]] | usr/lib/libdriver/src/source.rs, usr/lib/libdriver/src/dtb.rs, usr/lib/libdriver/src/supervise.rs, usr/lib/libdriver/src/readyline.rs | inv-i34 | - **The warden must never read a device register.** The moment it does, the |
| [[sub-libdriver-grant]] | usr/lib/libdriver/src/lib.rs, usr/lib/libdriver/src/manifest.rs, usr/lib/libdriver/src/resource.rs, usr/lib/libdriver/src/driver.rs | inv-i34 | - **Every axis must stay node-supplied.** A new axis whose concrete values come |
| [[sub-netd-nic]] | usr/netd/src/main.rs, usr/netd/Cargo.toml |  | On any change, prosecute: |
| [[sub-netd-server]] | usr/netd/src/server.rs, usr/netd/src/ndb.rs, usr/netd/ndb/local | inv-i9 | On any change, prosecute (the standing list, accreted across |
| [[sub-netdev]] | usr/lib/netdev/src/lib.rs, usr/lib/netdev/src/ring.rs, usr/lib/netdev/src/virtio.rs, usr/lib/netdev/src/virtio_pci.rs, usr/lib/netdev/Cargo.toml |  | On any change, prosecute: |
| [[sub-pouch-fs]] | usr/lib/pouch/patches/0009-pouch-openat.patch, usr/lib/pouch/patches/0010-pouch-fstat-lseek.patch, usr/lib/pouch/patches/0019-pouch-stat.patch, usr/lib/pouch/patches/0023-pouch-fopen.patch, usr/lib/pouch/patches/0024-pouch-fs-process-wires.patch, usr/lib/pouch/patches/0027-pouch-remove.patch, usr/lib/pouch/patches/0030-pouch-fopen-append.patch, usr/lib/pouch/patches/0031-pouch-readlink.patch | inv-i28 | - Every successful-open exit must route through `pouch_open_ret`, or |
| [[sub-pouch-net]] | usr/lib/pouch/patches/0005-pouch-poll.patch, usr/lib/pouch/patches/0006-pouch-sockets.patch, usr/lib/pouch/patches/0014-pouch-srv-stubs.patch, usr/lib/pouch/patches/0015-pouch-poll-tag.patch, usr/lib/pouch/patches/0016-pouch-net-sockets.patch, usr/lib/pouch/patches/0017-pouch-net-datacalls.patch, usr/lib/pouch/patches/0018-pouch-net-poll.patch, usr/lib/pouch/patches/0020-pouch-srv-bulk.patch, usr/lib/pouch/patches/0028-pouch-net-nonblock.patch | inv-i1, inv-i28 | - **Every fd-consuming call must be tag-aware.** The completeness of that |
| [[sub-pouch-process]] | usr/lib/pouch/patches/0026-pouch-process.patch, usr/lib/pouch/patches/0025-pouch-env.patch, usr/lib/pouch/patches/0011-pouch-abort.patch, usr/lib/pouch/patches/0012-pouch-mallocng-crash.patch, usr/lib/pouch/patches/0013-pouch-mallocng-diag.patch, usr/lib/pouch/patches/0003-pouch-mman.patch | inv-i24 | - The fd model's contiguity check and the `PSPAWN_MAXSLOT` bounds must |
| [[sub-pouch-seam]] | usr/lib/pouch/patches/0001-pouch-syscall-seam.patch, usr/lib/pouch/patches/0002-pouch-stdio-no-iovec.patch, usr/lib/pouch/patches/0008-pouch-hw-syscalls.patch |  | - **The seam-check list must grow with the series.** `build_sysroot` |
| [[sub-pouch-signal]] | usr/lib/pouch/patches/0007-pouch-signals.patch | inv-i24 | - The bootstrap must reach a `SYS_NOTED` on EVERY arm (unknown name |
| [[sub-pouch-thread]] | usr/lib/pouch/patches/0004-pouch-pthread.patch, usr/lib/pouch/patches/0022-pouch-nanosleep.patch | inv-i9 | - The 1-hour clamp must stay ≤ `TORPOR_MAX_TIMEOUT_US`; raising the |
| [[sub-pouch-tty]] | usr/lib/pouch/patches/0021-pouch-pty.patch, usr/lib/pouch/patches/0029-pouch-cons-winsize.patch |  | - The `S_ISCHR`-then-flag gate order, and the disjointness of bit 40 |
| [[sub-ptyfs]] | usr/ptyfs/src/server.rs, usr/ptyfs/src/main.rs | inv-i20, inv-i9, inv-i1 | - The **ECHO-off** guarantee is the one to re-derive on any ldisc |
| [[sub-stratum-bdev]] | stratum: src/block/bdev_thylacine.c |  | - Any new op must take the mutex; B-2 is what makes the single shared |
| [[sub-stratum-boot]] | usr/joey/joey.c | inv-i28, inv-i45 | - A new readiness signal must be emitted **after** the last fallible step. |
| [[sub-stratum-server]] | stratum: src/9p/server.c, stratum: src/cmd/stratumd/serve.c, stratum: src/cmd/stratumd/peer_creds.c, stratum: src/cmd/stratumd/run.c | inv-i1, inv-i28 | - `n_uname` must stay ignored while `SO_PEERCRED` is the channel. Honouring |
| [[sub-stratum-session]] | usr/login/src/main.rs, stratum: src/cmd/stratumd/proxy_9p.c, stratum: src/cmd/stratumd/dataset_pattern.c, stratum: src/cmd/stratumd/corvus_notify.c | inv-i1 | - The `/ctl` attach must outlive the session; the DEK lease is bound to |
| [[sub-tapestryd]] | usr/tapestryd/src/server.rs, usr/tapestryd/src/gpu.rs, usr/tapestryd/src/pane.rs, usr/tapestryd/src/input.rs, usr/tapestryd/src/main.rs, usr/tapestryd/src/chords.rs, usr/tapestryd/src/keymap.rs | inv-i40, inv-i5, inv-i34, inv-i1, inv-i45 | - The **retire order** is the invariant: unshare before any backing |
| [[sub-warden]] | usr/warden/src/main.rs, usr/warden/Cargo.toml | inv-i34 | - **The grant arithmetic**, because nothing re-derives it. The intersection |
<!-- generated:end -->
