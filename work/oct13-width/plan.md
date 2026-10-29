# Vote-8 width sweep -- design notes (corona, 2026-10-13 18:43-19:02Z, HEAD 5ef40bebb)

Yip: call 0219 to main (turn 1 + note 1 correcting Q3). Wait for main's answer before the first edit.

## Census (work/oct13-width/)
- dispatch-census.py: all 113 dispatch calls pass ctx->regs to a 64-bit param (control: a planted u32 param is flagged).
- width-census.py (regex fixed for (int)(s64)x double casts): census-reachable-*.txt = 85 rows over the dispatch callees + static handlers.
- Helpers fed raw words (outside the reachable filter): attach_9p tx/rx fd, attach_9p_srv fd, tty_fd_op fd + arg, tty_signal sig_class, walk_create_kname parent fd, open_create_kpath start fd, mmap_file / mmap_fixed_file fd.
- Already full-width (drop): rights (mmio/irq/dma create*), dup new_rights, spawn perm_flags, wait flags, unlink flags, walk/resolve omode (mask with ~(u64)), attach_9p_srv flags (flags_ok(u64)), burrow prot (burrow_prot_word_check), pci_map_range, pty_register, srv_registry_new, seat_import, set_nonblock, trusted_seat, console_episode, getrandom flags, Loom.
- Excluded with reason: exits/exit_group status (never return; & 0xff like Linux), pci_claim (high half IS the nth field: a packed encoding, fully consumed).
- viv_* translators narrow per Linux's declared types: that IS the Linux ABI (keep).

## Categories
- (a) descriptor: sys_hidx_arg (wide -> -1 -> the handler's own dead-descriptor answer; order kept; FROM_ROOT (u64)-1 compared full-width first).
  irq_wait, mmio_map, burrow_from_hostmem, dma_map, dma_segments, pci_info, write(+fastpath, rw lookup, staged), read, pread, pwrite, close, fstat_for_proc, lseek, walk_open, fd2path, fd_devclass, resolve_kpath, fsync, readdir, wstat, weft_map, dup, mount, srv_accept, srv_peer, attach_9p(tx, rx), attach_9p_srv, tty_fd_op, walk_create_kname, open_create_kpath, mmap_file, mmap_fixed_file.
- (b) signed 32-bit (libt declares long; pouch int sign-extends): accept iff (s64)raw == (s32)raw, else -EINVAL.
  postnote pid, setpgid pid+pgid, getpgid pid, getsid pid, wait_pid want_pid, poll timeout_ms, tty set_fg pgid (check tty_cont arg).
- (c) unsigned 32-bit: raw > 0xffffffff -> -EINVAL.
  mmio_map prot, dma_map prot, lseek whence (moves #102 F6's narrowing into the vivarium), wstat valid/mode/uid/gid, torpor_wake count, tty_signal sig_class.
- (d) 32-bit bit pattern compared to memory: either extension. torpor_wait expected
  (pouch 0004 __futexwait passes `int val` -> sign-extended; musl locks use INT_MIN+n; a zero-ext-only check would spin every contended lock).

## Vivarium (Linux phenotype narrows per Linux's declared types BEFORE the native handler)
- T1 rows copy six words verbatim (vivarium.c vivarium_translate ~560): add per-row narrow masks (u32 zero-ext / s32 sign-ext).
  read/write/close/pread64/pwrite64 fd u32; lseek fd + whence u32; getpgid/getsid pid s32; exit_group status s32 (no-op).
- T2 shells passing a bare Linux word into a changed native function: fsync fd (13871), fstat fd (14144), mmap fd x2 (14197/14207, fget(unsigned int)).
  Pre-existing divergence in the same family: getrandom flags (native refuses > 0xffffffff with -1; Linux truncates) -- narrow in the shell. readlinkat bufsiz (int): check.

## Tests
- Kernel tests via TEST_SYSCALL_DISPATCH (test.h:318) in a child Proc: wide arg -> refusal, and the low half alone -> the normal answer (positive control one variable away); a wide close must leave the fd open.
- Vivarium: viv_linux_dispatch_for_test: a T1 close/lseek with junk high bits still renumbers to the narrowed word.
- Harness: derive from work/oct13-rollback/kernel-run.sh; mutants: drop one guard per category + drop the T1 narrowing.

## Not in scope (tracked elsewhere)
- ER-3: bare -1 returns (ERRORS.md:338-368). New refusals answer -T_E_INVAL; a wide descriptor gets the handler's existing dead-fd answer.
- -Werror=shorten-64-to-32 lands with aux-3 30f807e0a; re-check this tree with the flag under the lease.
