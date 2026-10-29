Pre-check of the merge host harness on thyla-pi, 2026-10-22 11:54-12:05Z,
under corona's pi lease, while the mac was queued. NOT the evidence of record:
the Mac run (host.sh at the fix commit) is. Host: Linux 6.18.34+rpt-rpi-v8
aarch64, gcc (Debian 14.2.0-19) 14.2.0, Python 3.13.5; CC=gcc explicit.

Each directory is one source tree shipped with `git archive` (kernel mm tools
usr/lib/libt/include usr/lib/libthyla-rs/src):
  1-d7d77ebed  the WIP commit d7d77ebed, all 18 legs (precheck.sh)
  2-233b9035f  `git stash create` object: d7d77ebed + addrspace drain double
               external + srvconn principals-crossed keeps server_principal used
  3-5eebcc759  + 9p-progress backend supplies recv_ready/recv_now (sentinels)
  4-f03b0bb30  + test-9p-progress.py builds with -iquote kernel
(the stash objects are unreferenced; the fix commit carries the same files)

Results (ordinary, --mutants):
  srvconn-admission   run 1: principals-crossed died at COMPILE (unused
                      parameter server_principal) = a survivor; run 2: clean +
                      12/12 killed by their own messages
  service-admission   run 1: clean + 12/12
  9p-client-progress  run 1: build red (1 fatal: ../mm/slub.h, absent from the
                      archive -- not a harness fault; + gcc-only
                      -Wmisleading-indentation in kernel/9p_wire.c's
                      `if (rc < 0) return -1; off += ...;` idiom); run 2 with
                      CFLAGS=-Wno-misleading-indentation (pi only): clean + 14/14
  9p-progress         run 1: as 9p-client-progress; run 2: CLEAN RED at run
                      time ("transport init": main 2faa703ed made recv_ready and
                      recv_now mandatory, the fixture backend had neither); run
                      3: clean + 11, then version:dialect died at compile (the
                      mutated 9p_session.c copy in a temp dir cannot resolve
                      "../mm/slub.h"); run 4: clean + 13/13, then the driver's
                      clang-only ARM64 syntax step (--target) fails under gcc
                      -- expected here, run on the Mac
  addrspace-lifetime  run 1: clean + 6, then skip-drain died at COMPILE
                      (static vma_drain_in double unused) = a survivor; run 2:
                      clean + 15/15
  loom-service-pool   run 1: clean + 11/11
  pty-interaction-abi run 1: PASS (Linux host cc, ELF native)
  every sanitized leg  rc 3, SANITIZER TOOLING: gcc's ASan aborts at startup on
                      this kernel (sanitizer_allocator_primary64.h:131 kSpaceBeg
                      CHECK) -- the selector stopped in its own state, as built
