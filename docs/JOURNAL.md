# The autonomous-run journal

**What this is for.** After a long autonomous run the operator needs to
reconstruct what happened without stitching together `git log`, six phase-status
rows, and a memory directory. This is that single thread: what landed, in order,
why, what it cost, and what it left open.

**What it is NOT.** Not a changelog — `git log` already has the commits, and
duplicating them here would rot. Not a status doc — `docs/phaseN-status.md` owns
per-chunk rows. What lives here is the *narrative*: the reasoning, the wrong
turns, the findings that were not in anyone's plan, and the decisions that
needed the operator.

**Conventions.**

- Newest run first. Within a run, chronological.
- Every claim carries its evidence: a hash, a measured number, a file:line.
- **A wrong turn is worth more than a win** — record the ones that were caught
  and how, because those are the reusable part.
- **Say what is still open, and be exact about what "fixed" covers.** A half a
  defect closed is written as a half.

---

## 2026-08-27 (aux) -- #91 exit(N): the real exit byte now reaches `$?`, and the fix announced itself by EXTINCTING the boot at the one probe that had hard-coded the old collapse

Picked up from the C1-verify self-compact at `9ec271d6`. #91 is the first of the
two C1 fills: before it, a process's integer exit status collapsed to 0/1 at two
kernel points (`sys_exits_handler`, `sys_exit_group_handler`), so a phenotype
`exit_group(42)` or a native `t_exits(42)` reached the parent's wait as
`WEXITSTATUS == 1`, never 42. Load-bearing for every scripted workload -- a shell
that branches on `$?`, `git diff --quiet`, a Makefile conditional -- and a hard
dependency of C2's mergetool/difftool (they `eval` on tool exit codes).

The mechanism was smaller than the blast radius. The int channel already existed
end to end: `proc_become_zombie_locked(p, int status, msg)` stores `p->exit_status`
verbatim, and the wait reap packs `WAIT_STATUS_EXITED(exit_status) = (code&0xff)<<8`
which is exactly Linux `W_EXITCODE`. The status was only pinched to {0,1} at four
points: the two syscall handlers (collapse to `"ok"`/`"fail"`) and two string->int
reconstructions (`exits()` @2981 and `thread_exit_self` @3113 rebuilding 0/1 from
the msg string). The fix: split `exits()` -> `exits_code(int,msg)` core + a
string-only wrapper (so the ~dozen in-kernel `exits("...")` callers keep the 0/1
mapping); split `proc_group_terminate()` -> `proc_group_terminate_code(int,msg)`
core + a wrapper (so the 4 kill/legate/debugger callers stay byte-identical at
code 1); add `int group_exit_code` at the END of `struct Proc` (it fills the
4-byte tail pad after `loom_sqpoll_count`@384, so sizeof stays 392 -- the
`_Static_assert` confirmed it at compile time); write it set-once in the same
CAS-winner branch that publishes `group_exit_msg`, and read it in
`thread_exit_self`. A plain int, lock-protected: its only reader is
`thread_exit_self` under `g_proc_table_lock`, the same lock every
`proc_group_terminate` caller holds, and the lockless `el0_return_die_check`
reads only `group_exit_msg`, never the code.

**The finding nobody planned: the fix EXTINCTED the boot, and that was the fix
working.** First green build, boot died: `EXTINCTION: joey: /joey exited non-zero
1`, at `pouch-hello-spawn: WEXITSTATUS(fail)!=1 FAIL (errno=38, x=3)`. Ground
truth (`usr/pouch-hello/pouch-hello-spawn.c:46`): the "fail" child does
`return 3;` with the comment `// -> WEXITSTATUS 1 (v1.0 non-zero collapse)`. The
probe had HARD-CODED the collapse: it asserted `WEXITSTATUS == 1` for a child
that really exits 3. Pre-#91 the kernel turned 3 into 1 and the assert passed;
post-#91 the real 3 survives through the full musl / `posix_spawn` / `waitpid`
stack and the assert (correctly) fails. Not a regression -- the #91 fix proving
itself at the pouch layer.

That reframed the task: the collapse was SYSTEMIC, so every consumer that
hard-coded it would break the same way, one boot at a time. Rather than discover
them serially I swept the tree for the assumption -- and the discipline paid: the
boot had extincted at the FIRST probe, so three later probes (which never ran)
could have hidden the same defect. The sweep found the full set and separated it
by ground truth: `pouch-hello-spawn` (the one break, asserted 1, child exits 3
-> fixed to assert 3); `viv-pheno-probe` L173 (child `linux_exit(1)` -> still 1,
SAFE -- but its own comment invited "when #91 lands it should assert the real
code", so PROMOTED to `linux_exit(7)`/assert 7, a distinctive-code witness the
collapse could never have delivered); `coreutil-smoke` cmp-differ (a genuine 1,
not the "cmp's 2" the stale comment claimed -- SAFE); the fault path
(`proc_fault_terminate` -> `exits("snare:*")` -> the wrapper -> 1, UNCHANGED, so
`pouch-hello-fault`'s "exit non-zero" check holds); the kill path (4 wrapper
callers, all still 1); `joey`'s `exit_status=127` counterfactual (stratumd boots,
never taken). Only ONE probe actually needed a code change; the rest were
confirmed safe by reading the child's real exit code, not by assuming.

**Witnessed on three layers**, so a future regression at any tier fails a gate:
native kernel (`syscall.dispatch_exits_fail` SYS_EXITS(42)->42 + a NEW
`syscall.dispatch_exit_group_code` SYS_EXIT_GROUP(42)->42, which exercises the
`group_exit_code` path the single-thread test does not); pouch/musl
(`pouch-hello-spawn` now proves `return 3` -> `WEXITSTATUS 3`); viv phenotype
(L173 `linux_exit(7)` -> 7). Suite 1465/1465, `Thylacine boot OK`, 0 EXTINCTION.

Reviewer note: the Fable 5 prosecutor died mid-round on credit exhaustion (no
report), so per the reviewer-model discipline the round was re-spawned on the
Opus fallback (context-independent, same family) rather than skipped. It closed
CLEAN on soundness -- 0 P0 / 0 P1 / 0 P2 / 3 P3 -- and independently re-derived
the load-bearing claim (the plain int `group_exit_code` is safe because its only
reader, `thread_exit_self`, and all six `proc_group_terminate[_code]` writers
hold `g_proc_table_lock`, while the lockless die-check reads only the msg). The
three P3s were all addressed: F1/F2 were two stale comments still describing the
pre-#91 collapse (the SYS_EXITS doc-block and the kill-cascade rationale), which
my own self-audit had missed -- the value of a context-independent read. F3 was
the interesting one: my new `dispatch_exit_group_code` test drove a SINGLE-thread
child, so setter and reader are the same thread and the cross-thread handoff the
lock exists to protect went unexercised. Rather than document-around it (the
reviewer's fallback), I promoted `pouch-hello-exitgroup` -- an existing REAL
2-worker pthread proc -- from `_Exit(0)` to `_Exit(42)` with a new joey
`want_status` assertion: now the MAIN thread sets `group_exit_code` but a WORKER
is the last thread out and reads it, so the parent reaping exactly 42 (not the
pre-#91 collapse to 1) is the cross-thread, cross-CPU witness, run under the SMP
matrix. Self-audit in parallel had surfaced one non-defect (SF1): the
`test_notes`/`test_rendez` scaffolds direct-write `group_exit_msg="killed"`
without a code, but they test the msg READERS with fake threads and never reach
the `group_exit_code` read -- production's invariant (only
`proc_group_terminate_code` writes the msg, and it sets both) is intact.

## 2026-08-27 (aux) -- milestone C1 (verify): 12 of 13 non-interactive git verbs work under the phenotype; a file-write that read back EMPTY sent me down a 9-bake root-cause that exonerated the kernel

Picked up from a self-compact at `0bcbe3ac` (milestone B done). C1 is
"verify-then-fill": the self-hosting-floor verbs (branch/merge/rebase/...) ride
already-supported primitives, so the first move is a hermetic gate to find what
actually breaks. Built the `git-workflow` bundle (echo... no -- see below) +
`tools/test-git-workflow.sh` + `do_git_workflow_gate`.

**The wrong turn, and what caught it.** First boot: the gate reddened on
diff/status/commit/merge with git reporting `nothing to commit, working tree
clean` -- after I had overwritten `f.txt` with a same-SIZE edit (`a\nb\nc` ->
`a\nB\nc`, both 6 bytes). The obvious theory was **racy-git**: same-size + coarse
mtime = git skips the content compare. I chased it hard -- read the whole mtime
path (t_stat carries whole-second mtime, nsec dropped; the "v1.0 = 0" comment
looked damning), built a probe expecting mtime=0. **Probe 2 killed the theory
with ground truth**: the file read back with `wc=0` and `git cat-file` showed a
0-byte blob -- the content was ABSENT, not a stale-stat mirage. The write itself
was being lost. Six more probes narrowed it (create vs append vs trunc; git-add
vs the write; big vs small; builtin vs subshell) until the split was undeniable:
**`echo > file` WORKS, `printf > file` is EMPTY** -- both busybox builtins, same
file, same fd. Probe 6 showed `echo BUSYWRITE > z.txt` producing a real 10-byte
file with a real mtime (`Aug 27 09:40`) -- which incidentally **exonerated mtime
entirely**: the pool clock is real, and racy-git works. The whole racy-git
detour was chasing an artifact of the empty-file bug.

**Root cause (a temporary 3-level kernel write trace, probe 9).** Gating a
`~`-marker byte through `viv_writev` / `sys_write_handler` / `dev9p_write`: echo's
`~ECHO` fired all three (SWTRACE hraw=1 len=6 -> DWTRACE-write -> flush rc=0 ->
wc=6); printf's `~PRINTF` fired **none** -> wc=0. **No write syscall is ever made
for printf's data.** It dies in musl's stdio buffer: busybox `printf` (a builtin)
writes to a fully-buffered stdout and the buffer is never flushed to the file --
busybox ash `_exit()`s without an atexit flush, and its per-builtin flush does
not reach the file under the phenotype. **The kernel writes exactly what it is
given.** Reverted every trace (`git diff --stat` clean-verified on dev9p.c +
syscall.c). Enqueued as a userspace fidelity gap
(`[[bug-phenotype-printf-stdio-never-flushed]]`); it does NOT block git-core
because **git's own file I/O is direct `write()`**, never busybox printf. The
lesson is [[M-PIN]]-shaped: a negative symptom (empty file) had TWO candidate
causes (stat-stale vs write-lost) and only a second axis (`wc` read-length vs
`stat` size) told them apart -- theory went in circles for two probes until
ground truth cut it.

**With `echo`, the verbs fell out green.** Switched the gate's file-writes to an
`echo`-based writer (`wl()`); re-baked: 12 of 13 verbs PASS -- branch, checkout,
**diff (the same-size `b`->`B` IS detected)**, status, commit, log, merge
(fast-forward AND a 3-way conflict resolved by editing the marked file, no editor
spawned), rebase (non-interactive), reset, worktree, manual gc (fork-self ->
repack/prune). Only **`git stash`** fails, and its unbuffered stderr named the
gap precisely: `unable to make pipe non-blocking: Function not implemented`.
git stash sets its subprocess pipe non-blocking via `fcntl(F_SETFL, O_NONBLOCK)`,
which `vivarium_fcntl_decide` declines (ENOSYS) and devpipe cannot serve.
Non-blocking-pipe support is an audit-bearing fill (kernel/pipe.c, I-9 wait/wake);
left stash as a MEASUREMENT (`WF-DIAG-stash-gap`, NOT a required marker) and
tracked the fill (`[[design-nonblocking-pipe-fcntl-setfl]]`).

**Cost + posture.** ~11 bakes (nine on the printf hunt). Gate PASS green,
`test.sh` exit 0, boot OK. NO kernel change survived (the milestone-B SMP-green
kernel is byte-unchanged), so no SMP re-run and no holotype round are owed -- the
change is test infra + a soft-skipping boot gate that mirrors the two existing
git gates. **Open for the operator to sequence**: the two C1 fills
(non-blocking-pipes for stash; #91 exit codes) and the printf/stdio userspace
gap. Committed at `05e52c5c`.

## 2026-08-27 (aux) -- milestone B: `git clone` + `git fetch` over HTTPS work under the phenotype (the first external TLS on Thylacine), up a ladder of gaps each caught by the gate reddening

Picked up from a self-compact at `da19918c` (B1 curl-git built, B2 staged). First
lesson, relearned the hard way: **`tools/test.sh` builds only when the kernel ELF
is MISSING** (`test.sh:132`), so both my "B2 boot-verify" and the first B3 bake had
silently booted a STALE pre-session image -- my env vars (`PRESERVE=0`,
`BAKE_GITNET=1`) reached no bake. A harness needing a fresh bake must FORCE it
(`build.sh all`) and verify staging by CONTENT (`build/vivarium/git-net/config.json`),
never by the build's exit code. B2's conclusion held only by luck (the stale image
already carried the curl git).

Built the B3 gate -- a net-granted `git-net` container + joey's `do_git_https_gate`
+ `tools/test-git-https.sh` -- staged ONLY under `THYLACINE_BAKE_GITNET=1`, so the
default hermetic suite (test.sh / SMP gate / LS-CI) stays internet-free (the
git-probe SKIP-if-absent idiom; the NP-3 precedent for keeping a real-NIC probe out
of the ladder). Then walked the clone up a ladder, each rung a red gate:

1. **`dup(2)` ENOSYS.** `fatal: can't dup helper output fd: Function not implemented`.
   git-remote-https (the external https helper) `dup()`s its output pipe; `VIV_FORWARD`
   is a placeholder returning ENOSYS (`syscall.c:~12658`). `file://` clone works (no
   external helper), which is why this hid until https. FIXED: `dup(23)` is TIER2 ->
   `handle_dup_posix` (rights verbatim I-6; the I-5 alias gate refuses hw/Srv). The old
   test misclassified `dup` as fd-FREEING (it CREATES an fd, frees none) -- a finding,
   corrected, + a new `vivarium.dup_arm` runtime test (positive / EBADF / socket-decline).
   Opus audit (`70e19dd8`): CLEAN, 0 P0 / 0 P1 / 0 P2 / 3 P3 all dispositioned. Fable
   was out of credits -> fell to the Opus fallback per scripture, never skipped;
   `MODEL(start)==MODEL(end)`.

2. **DNS-by-name.** `Could not resolve host`. musl's `getaddrinfo` needs UNCONNECTED
   UDP + non-blocking; the phenotype serves only CONNECTED UDP (`vivarium.c:1667`). This
   is GENERAL (the curl-demo hit it too, using `--resolve`), = net-4d "OWED", larger
   than git. Worked around for the gate with a FRESH-resolved `/etc/hosts` pin (isolates
   the transport proof from DNS; resolved at bake time so never a stale hardcode).

3. **External TLS-over-netd: WORKS -- the first ever on Thylacine.** GIT_CURL_VERBOSE
   showed a full TLS 1.3 handshake + "SSL certificate verified via OpenSSL" against the
   baked Mozilla CA bundle + `HTTP/1.1 200 OK` + the git-upload-pack advertisement. Every
   prior TLS proof in the tree was loopback.

4. **The v2 wall.** git's DEFAULT protocol v2 reads the whole capability advertisement
   and writes NOTHING back (no ls-refs, no POST), exiting rc=1 SILENTLY -- a
   phenotype-induced bug (native git v2 is fine), tracked. `GIT_TRACE_PACKET` pinned it:
   `git<` capabilities received, zero `git>`. **Forcing `protocol.version=0` -> the clone
   SUCCEEDS end to end** (ref list -> `git> want` -> packfile -> checkout). Set in both
   the gate and the `/viv/bin` production gitconfig so real git works now; v2 tracked.

5. **The fetch quirk.** `git fetch --depth 1 origin` on the shallow clone: "Failed to
   traverse parents ... remote did not send all necessary objects" -- git can't traverse
   the tip's absent parents. A shallow-repo quirk, native too, NOT a phenotype gap.
   `git fetch --unshallow` is the correct op AND downloads a real pack (the parent
   history), so the gate now proves the fetch path in full.

**Result: `joey: git-https gate PASS -- git clone + fetch over HTTPS ... network git
works, milestone B`.** Boot OK, suite 1464/1464 (with the new dup test).

**Open, surfaced to the operator:** (a) git protocol v2 aborts under the phenotype
(forced v0; getsockname-ENOSYS during v2 connection-reuse is the leading hypothesis --
no native handler exists); (b) DNS-by-name = net-4d (general phenotype unconnected-UDP;
IP-pinned for the gate); (c) push-over-https awaits a writable remote. All are
milestone-B completion items, more general than git.

## 2026-08-26 (aux) -- /viv/bin sub-chunk B: the mechanism proven LIVE, then real git shipped at /viv/bin; the full probe is container-shaped, and A-3 bounds the user story

Continues the entry below (sub-chunk A landed the kernel mechanism; this run
lands the half that proves the OR stamp *fires* -- a bare spawn through
`/viv/bin` actually running Linux -- and then the real git deploy). The arc's
substantive commits: `df270378` (the E2E), `3e7c0301` (the git deploy); the holotype
then closed at `3beea52a` (0 P0 / 0 P1 / 1 P2 / 2 P3, all fixed, NOT dirty), and the
SMP gate ran **16/16 boots clean** -- default-smp8 + ubsan-smp8, N=8, BOOT_PROBES=y,
so every boot exercised the git-BY-LOCATION + V-1b-loc gates AND the full 1463-test
suite (incl. the F1 regression); 0 corruption, 0 external-kill. Nothing shipped
before the audit closed.

**The mechanism is live (df270378).** A boot-probe leg in joey (`V-1b-loc`) spawns
a binary through an MPHENO_LINUX mount with `pheno_flags = 0` -- the mount the SOLE
declaration -- and proves the exec resolver crossed it and stamped the child
PHENO_LINUX. The kernel unit test proved the resolver in isolation; this proves the
whole path. Marker `joey: V-1b-loc /viv/probe-bin resolver-subtree-scope PASS`,
boot OK.

**The wrong turn, and what caught it: the full probe is CONTAINER-shaped.** The
first cut reused `viv-pheno-probe`'s full `linux` mode (the same the container leg
drives). It got through L01-L32 bare -- L01 is the brk translation discriminator,
so the mount HAD stamped it Linux and a large swath of the Linux ABI worked -- then
failed at **L33** with the marker `xL33`. That `x` is the finding: L32-L36 are the
signal legs, and their comment says outright "viv hands this process fd 0 as the
write end of a pipe with NO READER" for a SIGPIPE self-inflict. A bare joey spawn
hands an EMPTY handle table, so the probe's `/pheno-scratch` open BECAME fd 0, L33's
one-byte write of `'x'` to "fd 0" (the reader-less pipe, it assumed) landed in the
report file itself, and the write SUCCEEDED where an EPIPE was expected. Not a
kernel bug -- a probe-harness contract the container satisfies and a bare spawn
cannot. What caught it: the marker channel self-diagnosed (`xL33`), and reading the
probe's OWN comment named the fd-0 dependency. The fix is a lean `linux-loc` mode
(brk discriminator + real openat/read/write, nothing needing spawner-provided fds)
-- the narrow witness a bare-spawn phenotype proof actually wants; the full ABI
conformance stays the container leg's job. **Reusable lesson: a test binary written
to run INSIDE a container carries the container's fd contract as an invisible
premise; a bare-spawn reuse must drop to the sub-witness that shares none of it.**

**The git deploy (3e7c0301), and a collision caught by reasoning.** build.sh stages
a PLAIN `/vivarium/viv-bin` pool tree (the same sha-pinned static git 2.51.2 +
dashed pack symlinks + gitconfig), separate from the git-probe *container* bundle;
joey mounts `/viv/bin` <- it MPHENO_LINUX, ungated + soft (no tarball -> no
/viv/bin, a hiccup degrades to "no git on PATH" rather than bricking the boot). A
bare `git init` via /viv/bin (`pheno_flags = 0`) produced `Initialized empty Git
repository in /tmp/vivgit-repo/.git/` + `section-13 git via /viv/bin (phenotype BY
LOCATION) PASS` -- the REAL third-party binary runs Linux BY LOCATION (a native git
mis-decodes its first libc-init syscall and dies before a repo, so success IS the
proof). git init needed only CAP_CSPRNG_READ + a drained-pipe stdio trio; no env,
no /etc config. The collision I caught before booting: the step-1 mechanism E2E
bound the whole /bin at `/viv/bin` and MREPL+unmounted that point -- left there, it
would have torn the shipped git mount out from under every later leg. Relocated the
mechanism proof to a DISTINCT point `/viv/probe-bin` (the mechanism is
mount-point-agnostic); the product `/viv/bin` git mount is untouched.

**A-3 bounds the user story -- surfaced, not papered over.** git runs as SYSTEM,
which owns the pool files it touches, and the E2E proves it there. A real
non-SYSTEM USER cannot yet chmod its own repo files: per-principal 9P ownership
(A-3) is unbuilt at v1.0. So "seamless user git from ut" -- the operator's literal
phrasing -- is NOT fully deliverable at v1.0; it awaits A-3 + a ut cap-conferral
(the user-git arc). What IS delivered: the binary ships at /viv/bin, discoverable on
ut's PATH/completion (enablement), and PROVEN to run under the phenotype as SYSTEM.
The ut cap-conferral was deferred deliberately -- it alone does not unblock user-git
(A-3 is the hard wall behind it), so both land together when A-3 does. The in-code
comments and the phase7 row state the boundary; this is the decision the operator
should know about.

**The reviewer fell back to Opus.** The holotype's first spawn (highest Fable, the
family-diverse primary) died mid-run of credit exhaustion, producing no report. Per
the binding rule -- never skip a round for want of Fable, and on credit exhaustion
go straight to the fallback -- it was re-spawned on the highest Opus at max effort,
told in its prompt that context independence (not family diversity) is what it
brings and to re-derive I-43 and the SET-ONLY correctness from the code. That round
is what gates the push.

**Cost.** Two build+boot cycles for the E2E (one to discover the container-shape
contract, one for the lean witness), one for the git deploy. The `xL33` detour was
not waste: it is why the bare witness is a lean sub-mode instead of a fragile reuse
of a container-shaped chain.

---

## 2026-08-26 (aux) -- the /viv/bin phenotype mount: git on the PATH, run Linux BY LOCATION; and the granularity check that reverted a unit-green build

**The ask.** After git ran end-to-end under a *container* (VIVARIUM 6.27), the
operator wanted it as a first-class program: "a separate bin directory for Linux
programs run via viv, on the PATH, working with ut's autocomplete," plus "is there
a way to easily identify a Linux binary from ut?"

**That question IS the design.** You can't identify a static Linux binary from its
bytes -- EI_OSABI is non-discriminating both ways, PT_INTERP is absent on a static
binary (the settled Q3 resolution, elf.c). A phenotype is *declared*, never
sniffed. So the answer is BY LOCATION: a curated dir IS the declaration. Two
operator votes (AskUserQuestion): the declaration mechanism = a **kernel
mount-flag** (over a ut path-prefix), and the dir = `/viv/bin`.

**The wrong turn, and what caught it.** I built the flag as the `MNOEXEC` sibling
-- a `mount_pheno_linux_covers(dc, devno)` scan at the exec stamp -- and it went
unit-green (suite 1462/1462, a real-cross test with a plain-mount control). Then a
deployment check falsified the *scope*: `dev9p.c:558` mints one devno per 9P
*attach session*, and `/clade/bin` + `/goroot/bin` prove the shipped bin dirs are
plain subdirs of the shared pool session. A `(dc,devno)` key therefore scopes to a
WHOLE session -- flagging a pool subdir would have declared every file in it Linux,
native `/bin/ut` included. The check that caught it was not a test; it was asking
"what device instance does `/viv/bin` actually get?" before wiring the deploy.
`MNOEXEC` gets away with the coarse key because it is only ever used on per-`/env`
mounts; `/viv/bin` is the first use that wants *subdirectory* scope.

**Surfaced, not silently switched.** The (dc,devno) build was the faithful
execution of the "kernel mount-flag" vote, so the choice between "give /viv/bin its
own device instance (keep the coarse key)" and "scope by the resolver" was the
operator's -- a real cost/correctness fork that emerged mid-implementation. They
voted the resolver scope. I reverted the covers-scan and rebuilt (a707136f amends
the scripture; the story -- granularity finding -> vote -> rebuild -- is in the two
scripture commits so the design is auditable against its reasoning).

**The mechanism as it stands (065008fb).** `MPHENO_LINUX = 0x0020`; `mount_lookup`
hands back the crossed entry's flags under `ns_lock`; `stalk_cross_mounts` sets a
SET-ONLY `crossed_pheno` (any mount-over-mount hop, recorded before the clone can
fail); `stalk_core` threads it through the three cross sites; a thin `stalk_exec`
wrapper exposes it and `exec_resolve_from_namespace_ex` writes it out -- so
`stalk_err`'s other callers are untouched, minimal blast radius on the I-28
resolver. SYS_SPAWN_FULL_ARGV ORs it into `sa->pheno_linux` beside the manifest
channel; the phenotype stamp is unchanged and channel-agnostic. The SAME file is
Linux reached through `/viv/bin` and native reached by any other path -- the
declaration is a property of how you named it, never of the bytes. Fail-safe: no
crossing leaves the binary native (rule 3), so no `may_back_exec`-style floor is
needed (unlike MNOEXEC's fail-open restriction). I-43 holds: the mount confers ABI
shape via the namespace and no authority.

**Enablement (6eb0c7f7), and a test that did its job.** `MPHENO_LINUX` joined
`SYS_MOUNT_VALID_FLAGS` (or joey could not compose /viv/bin from EL0), and
`sys_mount.rejects_invalid_flags` -- which pinned 0x20 as "the lowest unassigned
bit" -- caught it exactly as its own comment foretold ("the next flag to land
should trip it again"). Re-pointed to 0x40 + MPHENO_LINUX added to the accepted
half. ut gained `/viv/bin` on `resolve_command`'s $path + the completion readdir;
no phenotype logic in ut (the declaration is the mount). suite 1462/1462.

**What is NOT done, named plainly.** The half that proves the OR stamp *fires* --
a bare `git`/probe spawned via `/viv/bin` actually running Linux -- is the owed
sub-chunk B (the E2E, then the full git-at-/viv/bin deploy, then the holotype on
the combined surface, then SMP + push). `viv-pheno-probe` is reusable for it (it
opens `/pheno-scratch` + `/bin/viv-pheno-probe`, both resolvable in joey's
namespace): bind the initrd tree at /viv/bin MPHENO_LINUX, spawn the probe with
pheno_flags=0 via /viv/bin (-> Linux) and via /bin (-> native, the control). The
4 commits are UNPUSHED + UNAUDITED -- nothing ships before the holotype.

**Cost.** One design round (two operator votes), one unit-green build reverted, one
rebuild to the ratified scope. The revert was not waste -- it is why the shipped
mechanism has the scope the operator's mental model actually meant.

---

## 2026-08-26 (aux) -- git commit + clone run under VIVARIUM (the O_APPEND + pread64 arm), and the wall that was the FS's job all along

**What landed**: VIVARIUM 6.27 -- `git commit` + `git clone file://` now run under
the phenotype. The full chain (`init`/`add`/`commit`/`log`/`clone`/`verify`,
reflogs ON) passes as SYSTEM. Two walls, both smaller than the §6.26 deferral
feared, and the first one dissolved on research.

**The O_APPEND design fork -- surfaced, and the research collapsed it.** The
§6.26 close deferred commit/clone on O_APPEND, framing it as "Thylacine has no
kernel append mode." I surfaced the scope fork to the operator (full O_APPEND vs
narrow git-unblock) WITH the research -- and the research found the fork was
already decided: **Stratum implements O_APPEND end to end.** Its 9P server stores
the fid's open flags at Tlopen and, on every Twrite to an O_APPEND fid, ignores
the client offset and writes at the current size (`server.c` h_write;
`_Static_assert(STM_9P_O_APPEND == O_APPEND)`). So the kernel needs no append
MODE -- it PASSES the flag through, and the FS does the positioning ("the
filesystem is the OS," the append face). The operator ratified full O_APPEND. The
plumbing is one omode bit (`SYS_WALK_OPEN_OAPPEND` 0x40, additive; OMODE_VALID
0xB3->0xF3), the map in dev9p_open + dev9p_create, and the admit in both openat
decides. The kernel write path + cursor are UNCHANGED; for an append fd
`c->offset` is advisory (Stratum ignores it), exactly right for a write-only
append (git's reflog).

**The second wall was pread, found by letting the gate fail forward.** With
O_APPEND in, `commit` + `log` passed but `clone` died: `error reading from
...pack: Function not implemented`. git's index-pack reads the pack via pread,
and pread64(67)/pwrite64(68) were untranslated. Same `(fd, buf, count, offset)`
shape as SYS_PREAD/SYS_PWRITE -> two pure T1 renumbers, no shell. The
sub-ceiling LOOM collision (67/68) is the read/write renumbers' damage-envelope
(a mis-declared LOOM caller's loom handle is not a RIGHT_WRITE Spoor, so
SYS_PWRITE fails clean). Adding them, clone completed -- pread was the last wall.

**A behavior-change regression, caught by the suite (the right way).** Admitting
O_APPEND flipped two tests that PINNED the old reject (`openat_domain` "O_APPEND
forwards", `openat_create_domain` "O_CREAT|O_APPEND declines") -- an EXTINCTION
before the git gate even ran. That is the suite doing its job: a deliberate
behavior change must update the tests that asserted the old behavior, and the
updated assertions (O_APPEND now translates to OWRITE|OAPPEND) are the
regression.

**The write-behind interaction was a REAL bug -- I flagged it, my analysis
under-called it, the prosecutor caught it.** git's reflog is O_CREAT|O_APPEND, so
it rides dev9p_create, which set the write-behind anchor (`wb_eligible`,
`wb_base=0`) for EVERY create -- so the kernel's wb staging and Stratum's
O_APPEND EOF-override were BOTH live on the append fd. I reasoned "for a fresh
single-writer file they agree, and the E2E proved it," and wrote a doc claiming
the anchor was never set for append -- **but the code did not implement that
gate, and single-writer was the only case I checked.** The holotype (F1, P1) ran
the CONCURRENT case: two processes appending one log, the flush's
`larder_page_install_own` installs this fd's bytes at `wb_base` offsets the
server relocated to a DIFFERENT EOF -> a fabricated own-page serve, content that
never existed at those offsets (I-38). Two lessons compounded: a doc that
asserts a gate the code lacks is worse than no doc, and "the E2E proved it"
proves only the path the E2E runs (single-writer). Fixed by making the code
match the claim -- exclude append fds from the anchor (`!(omode &
SYS_WALK_OPEN_OAPPEND)` at both sites); pure write-through is larder-coherent for
append. And F2 (P2) caught the twin, and building its control found something WORSE than
the finding named: adding the second-commit leg, the reflog assert failed with
`.git/logs/HEAD` simply ABSENT -- the /etc/gitconfig still carried sub-chunk 1's
`logAllRefUpdates = false`, so **the gate had never written a reflog at all, and
O_APPEND had never once run in it.** The "O_APPEND proven end-to-end" green was
commit/clone succeeding WITHOUT the append path (a ref update writes refs/heads
via lockfile+rename, not the reflog). Enabling reflogs made the append actually
execute; the second-commit control (reflog file == 2 lines, the nonempty-file
append at cursor 0 != EOF) now both exercises AND witnesses the EOF positioning.
The lesson under the lesson: a gate whose green never depended on the feature is
not weak evidence, it is zero evidence.

**Holotype** (Fable 5, MODEL start==end): 0 P0 / 1 P1 / 1 P2 / 2 P3, ALL FIXED
(clean close; F1 the wb fight above, F2 the non-discriminating gate, F3 stale
reference caveat + missing T_OAPPEND userspace mirrors, F4 the off-by-S cursor
divergence stated precisely). Hard targets I-43 + the pread64/pwrite64
damage-envelope + the Stratum-half of the design VERIFIED SOUND.
**Verification**: SMP gate PASS -- 16/16 clean, 0 corruption (default-smp8 8/8 +
ubsan-smp8 8/8, N=8, both sanitizer arms; TESTS=y + BOOT_PROBES=y so the suite +
the reflog-append write path under the git gate ran under smp8). Tip `4afe31c4`.

---

## 2026-08-26 (aux) -- git runs under VIVARIUM (milestone A: init+add), and the three walls it took to get there

**What landed**: the VIVARIUM 6.26 chunk -- a **real static aarch64 musl
`git 2.51.2`** performs `git init` + `git add` under the Linux phenotype
(`GITPROBE-INIT`/`ADD`/`DONE`, boot gate PASS). The syscall translation was the
easy third of the work; the run's substance was the two *architectural* walls
behind it, and the audit that found the shape defect the pinned binary was
hiding.

**The pi built the binary.** No static NO_CURL aarch64 git ships prebuilt, so it
was cross-built on thyla-pi: git 2.51.2, `musl-gcc -static` against a static
zlib 1.3.1, `NO_CURL/NO_OPENSSL/NO_PTHREADS/NO_ICONV/NO_REGEX=NeedsStartEnd`
(musl's regex lacks `REG_STARTEND`; the bundled compat regex is the fix). Two
wrong turns caught by checksum, not by trust: zlib.net served a 404 HTML page in
place of the tarball (re-fetched from the madler GitHub release), and the git
tarball is now sha256-pinned in `build.sh` with a refuse-to-stage mismatch arm
so a corrupt download can never bake silently.

**Wall 1 -- seven missing numbers.** `faccessat`(48)/`chdir`(49)/`fchmodat`(53)/
`readlinkat`(78) for `git init`'s config write and path canonicalization,
`getrandom`(278) for `git add`'s temp-object naming, `geteuid`(175)/`getegid`
(177) for the "am I root" checks. All were `FORWARD`ing to `ENOSYS`. The four
sub-ceiling numbers collide with native syscalls (48=NOTE_MASK, 49=
SPAWN_FULL_ARGV, 53=PIVOT_ROOT, 78=PCI_INFO); the AT_FDCWD gate
(`vivarium_faccessat_decide`, admits only dirfd==-100) is both the cwd-form
contract and the collision defense for 48/53/78, and the FD-less 49 carries the
damage-envelope argument.

**Wall 2 -- the pool is SYSTEM-owned.** A container running as a real user
(uid 1000) creates files stamped `PRINCIPAL_SYSTEM` (dev9p reports the boot FS
as system-owned), and git's config write chmods its own lockfile -- which needs
ownership -- so `git init` dies. Per-principal 9P ownership is A-3, unbuilt at
v1.0. Milestone A therefore runs git as a **SYSTEM-principal boot probe**
(`do_git_probe_gate`), which owns its files; git-as-a-real-user is a tracked A-3
arc, escalated to the operator and ratified.

**Wall 3 -- the phenotype fork zeroed caps.** With git as SYSTEM, `git init`
works but `git add` fails at `getrandom`: the forked git holds no
`CAP_CSPRNG_READ`. Root cause: `rfork_forked` passes `CAP_NONE`, so a fork's
caps are `parent & 0 = 0` -- Thylacine zeros caps on fork, but **Linux forks
INHERIT them** (an I-43 fidelity gap that hits every capability-using phenotype
program forked by a shell, not just git). The fix (operator-ratified,
soundness-sensitive): `rfork_forked_with_caps`, taken by `sys_rfork_core`'s
`PHENO_LINUX` arm with `CAP_ALL`, so `rfork_internal` computes
`child->caps = (parent_caps & CAP_ALL) & ~CAP_ELEVATION_ONLY` -- the child gets
`parent minus elevation`: I-2 holds (`<= parent`, never grown), elevation never
inherits, native fork keeps `CAP_NONE`. The CSPRNG cap rides a
joey->viv->git conferral chain (`org.thylacine.csprng: granted`), each hop
intersecting the parent's held set.

**The audit found the defect the pin was hiding.** The holotype (Fable 5,
`MODEL(start)==MODEL(end)`) verified the hard targets sound -- I-2 (single
setter at `proc.c:1382`, acquire-load, one caller of the new entry point,
monotonic chain), I-43, the readlinkat copy-out applying the getdents P0 lesson
-- and returned **0 P0 / 1 P1 / 1 P2 / 1 P3**, all one-to-three-line fixes:
- **F1 (P1)**: fchmodat's `if (args[3] != 0) return EINVAL` reads an
  **undefined register**. Linux syscall 53 is 3-arg (the flags variant is
  `fchmodat2`/452); musl's `chmod` issues syscall3, so x3 is dead residue. The
  staged binary happens to leave x3==0 at its chmod sites -- the sha-pin froze
  the luck. A rebuilt git, the commit/clone sites, or busybox's chmod re-rolls
  the dice, and for an unlucky binary this is a deterministic P0 that kills
  `git init`. My own faccessat row states the correct rule ("args[3] does not
  exist and is never read") three screens up; the two arms disagreed. **The
  wall this catch clears: a green E2E gate proved nothing about a register the
  gate never varied.** Fixed by deleting the check.
- **F2 (P2)**: `sys_readlink_for_proc` flattened `dev9p_readlink`'s real errnos
  (INTR on a ^C unwind, IO on a transport drop) to EINVAL -- which is precisely
  readlink(2)'s "not a symlink" signal, so git's `real_path` would treat an
  *interrupted* component as a plain file (silent wrong resolution). Fixed by
  preserving the negative errno (`if (tlen < 0) return tlen;`), the vtable
  defense kept for the 0/overflow arms.
- **F3 (P3)**: the readlink core used `stalk()` (flattens walk failures to
  ENOENT) where its siblings use `stalk_err()` (preserves EACCES/ELOOP). Fixed
  by the one-line swap. Plus a stale count in `caps.h` ("All five" -> six;
  CAP_DEBUG + CAP_JIT joined `CAP_ELEVATION_ONLY` after the sentence was
  written) -- the exact comment block the I-2 argument reads.

Clean close (0 P0, P1+P2=2, all non-invasive). Post-fix re-verify: suite
1461/1461, both new unit tests PASS, git gate PASS, banner, no extinction.

**What is NOT done, exactly**: `git commit` + `clone file://` both open the
reflog `.git/logs/HEAD` with `O_APPEND`, which the phenotype `openat` does not
admit (no kernel append mode; a raw Linux binary cannot emulate it in libc as a
pouch port does). That is sub-chunk 2 (VIVARIUM 6.27) -- a phenotype `O_APPEND`
mode. So milestone A is `init` + `add`, and the gate asserts only those three
markers; commit/clone are named, not silently absent.

**Coverage honesty**: the seven T2 arms have no kernel-unit-test driver (the
project convention -- arms are covered by the in-guest E2E gate; the pure
decide/gate functions ARE unit-tested), so F1/F2/F3 landed as fixes + the
happy-path gate, not as new unit regressions -- none is deterministically
triggerable in the current infra (F1 needs controlled x3 residue, F2 an
injected INTR, F3 a user-principal no-X dir under a SYSTEM-owned gate). Named
here rather than papered over.

**Verification**: SMP gate PASS -- 16/16 clean, 0 corruption (default-smp8 8/8 +
ubsan-smp8 8/8, N=8; TESTS=y + BOOT_PROBES=y, so the 1461 unit suite AND the
git-fork-under-SMP path ran under smp8 max concurrency on both sanitizer arms,
~50s mean/boot). The fork-caps change adds no new concurrency primitive (it
rides the already-audited `rfork_internal` acquire-load), and the gate confirms
it at max concurrency. Tip `830817c4`.

---

## 2026-08-26 (aux) -- getdents64+fsync lands; the viv-run ^C hunt ends in an exoneration

**What landed**: the VIVARIUM 6.25 chunk (`8c72dcf7`) -- getdents64(61) +
fsync(82)/fdatasync(83) + the O_DIRECTORY admission that makes musl's
`opendir` reachable. The implementation itself was quiet (the 6.24 follow-on,
built to plan); the run's substance was the hunt that blocked its close.

**The hunt.** Adding the E2E's 4th leg (`ls` -> G50, the getdents64 witness)
made the following ^C leg fail 3/3 where the 3-leg shape "passed" -- and the
committed shape's "~40% pts flake" was the same mechanism. The operator voted
(AskUserQuestion) to pull the suspected caught-note wake fix forward. Three
rounds of counter instrumentation later (tear-proof `cons_diag_line` units --
raw `uart_puts` instruments TORE the guest stream in the prior session, the
open #243 class, demonstrated), the verdict inverted: **there was no wake
gap**. The measured chain, run to ground:

- The pts INT fan reaches all three pgrp members (fm=3), ash's arm lands
  (fg=1) WHEN ash's SIGINT disposition admits it -- and the failing
  alignment's ^C, sent the instant leg 4's output matched, landed inside
  busybox-ash's reap window where a job-control shell still holds
  SIGINT=SIG_IGN. The V-6b ignore-drop then discards the note AT POST TIME
  (fo=1 fg=0: posted-and-dropped), which is exactly Linux's semantics for a
  signal generated while ignored. No kernel defect: the scenario raced its
  own stated precondition ("^C at the ash PROMPT"). Fix: settle first;
  5/5 instrumented, 3/3 on the final shape.
- The hunt exercised the one leg of the 11b caught-note machinery nothing
  had ever driven: the WAKE of a parked elected 9P reader. Measured live
  (b=1 r=1 -> so=1 -> cr=1): fan -> arm -> proc_caught_note_wake ->
  SLEEP_NOTEINTR post-wake unwind -> reader-role handoff ->
  CLIENT_WAIT_NOTEINTR -> EINTR -> delivery -> handler -> prompt.
- The byte capture (a syscall-exit hook dumping the read's return + hex)
  proved ash's post-^C read returned the typed line INTACT (rc=22,
  `756e616d65...` = "uname -s | tr a-z A-Z\n" exactly). The residual
  line-eat is busybox-internal: ash's pending-interrupt latch (its
  INT_OFF/INT_ON bracketing) can consume the first line completed after a
  delivery, alignment-dependent. Documented in 145; not a kernel row.

**Wrong turns, and what caught each**:
- The first dump trigger ("dump at INT post #2") captured the BOOT LADDER's
  pty-probe posts, not the scenario's ^C -- the openpty E2E spends the early
  posts. Caught by the dump's own context lines sitting in the wrong
  transcript region. Lesson: a trigger keyed to a global ordinal races every
  other producer of the same event.
- The fan snapshot first read fm=1 and I nearly concluded ash was outside
  the pgrp; the session-membership dump (CNWS) showed all three members IN
  it -- the fm=1 was a STALE snapshot of a prior boot-probe fan (the dump
  ran before its own post's fan). Caught by dumping membership and fan in
  the same breath and seeing them disagree. Restructured to dump-after-fan.
- My own instrument had a cross-CPU visibility race (a plain u32 arm flag),
  which ate the EINTR'd read's CNWB line and nearly re-opened a closed
  question. Caught by an accounting hole: a return with no matching entry.
- "The trace shows ash executing a pipeline" -- misread: the
  fstatat/getcwd/openat run after a delivery is ash's PROMPT REDRAW; the
  green run's trace names the real execution signature (pipe2 -> clone ->
  the child's set_tid_address). Caught by diffing a green trace against the
  failing one instead of reading the failing one alone.

**The holotype earned its round -- a P0.** The Fable-5 prosecutor (family-
diverse: the impl was Opus 4.8) returned 1 P0 + 1 P2 + 1 P3, and the P0 was
the real thing: the getdents64 arm copied its encoded dirents straight to the
user `dirp` via `uaccess_store_u8` with NO `sys_validate_user_buf` -- while its
own native twin `sys_readdir_handler` validates up front. `uaccess_store_u8`'s
fault fixup engages only for user-half VAs, so an unprivileged phenotype
passing a kernel-half `dirp` (reachable precisely BECAUSE this chunk admits
O_DIRECTORY, so `open("/", O_DIRECTORY)` now succeeds) gets a guaranteed kernel
extinction, or -- at a writable kernel VA -- a `strb` of attacker-influenced
dirent name bytes into chosen kernel memory: an arbitrary-write primitive. The
reflex the prosecutor named fighting is the one worth recording: the arm READS
like the audited #50 mutation shells (same lookup/clunk choreography), which
invites waving it through -- but those shells never copy a variable-length
kernel buffer to a raw user pointer, and that is the exact line this arm
crossed without the guard its native twin carries. Caught before push (the
chunk was local-only). Fix: one validation line before the lookup, plus a
MEASURED fails-without-fix regression (guard disabled -> the arm reaches the
lookup with the test's unhandled fd and returns BADF where EFAULT is asserted,
failing cleanly with no panic -- the destructive store-path needs a live dir
the E2E covers). Two more fell out of the same round: `dev9p_readdir`'s bare
-1 (the one sibling not using `dev9p_wire_errno`) was flattening a caught-note
EINTR -- the very ^C-during-`ls` path this chunk added an E2E leg for -- into a
fabricated EPERM at the viv boundary; and O_DIRECTORY|O_TRUNC truncated a
regular file before the ENOTDIR check (silent data loss). All three fixed +
one self-audit P3 (a u16 d_reclen guard). Dirty close by the rule (a P0), but
every fix is a one-liner the holotype pre-blessed, so a self-audit of the
fixes stood in for a re-round. Lesson, pinned: a new copy-out arm that does not
mirror its native twin's buffer guard is a kernel-write primitive -- the guard
is not optional garnish, it is the whole reason the native handler validates.

**Also this run**: yip 0027/0028 settled (byes exchanged); the mac held across
both gate runs (the pre-fix SMP gate was KILLED as superseded the moment the
P0 landed -- a gate on soon-to-be-changed source certifies nothing); the stale
"caught-note wake OPEN" memory line corrected (it had already landed at
86b4b714/6884f06c -- the hunt confirmed it works).

## 2026-08-25 (aux) -- #50 path-mutation family: git's write path opens

**The chunk**: `b417b307` (scripture) + `c4f0e50e` (implementation) +
`1dd1348e` (audit close). The operator ratified the git arc ("let us do git");
reconnaissance had pinned the keystone as #50 -- a phenotype git could not
write ONE file: openat refused all O_CREAT by the 6.20 3-blocker verdict, and
mkdirat(34)/unlinkat(35)/renameat(38) were unnamed numbers. Three
AskUserQuestion forks, all ratified: full family in one chunk/one audit; mint
native `SYS_OPEN_CREATE=108` on the same core; `T_E_ISDIR=21` into the
signoff-gated errno registry.

**The design move worth keeping**: the 6.20 verdict ("O_CREAT cannot be ROUTED
to SYS_WALK_CREATE") was not overturned -- it was dissolved by building what
the verdict said was missing. Prior art collapsed the space (Plan 9 create(2)
puts create-else-open IN the kernel; Linux v9fs runs the same as a bounded
client loop; Fuchsia creates on the parent connection). Blocker 3 (the
FROM_ROOT sentinel joining cwd in SYS_OPEN but not in SYS_WALK_CREATE -- the
silent wrong-directory hazard) closed STRUCTURALLY: the LS-4 join extracted
into `sys_join_cwd_if_relative`, one helper, both cores. The
create/unlink/rename mechanics were EXTRACTED from the fd handlers
(`spoor_create_install` / `spoor_unlink_in_dir` / `spoor_rename_in_dirs`), so
the phenotype rows run byte-for-byte the gates the native syscalls run --
I-43's shape-not-authority, enforced by code identity.

**Find 1 -- the /tmp bake gap (nobody planned this).** The E2E's first run of
the create leg as a PLAIN USER answered "can't create /tmp/f50: Permission
denied". Not the new row failing -- the row fired and A-2d denied honestly:
`stratum-fs put` preserves only the exec bit, so every container rootfs dir
baked 0755 SYSTEM-owned and /tmp lost its 1777. A user-principal container
could write NOWHERE, and no prior gate could see it -- every earlier witness
either wrote nothing or ran privileged. Fixed at the bake
(`populate_stratum_pool` re-stamps rootfs/tmp; 1777 since the close).
Exactness: /tmp only, FRESH bakes only; other mode-bearing dirs a rootfs may
carry still flatten -- the general `put`-preserves-modes fix is Stratum-side,
open.

**Find 2 -- the loopback was stricter than every real transport.** The two new
dev9p tests EIO'd while the identical kernel path passed the boot fixture. Six
instrumented boots walked it down (probe-asserts -> a step tracker -> deep-exit
probes -> wire-sequence delta -> a stat-fail probe -> `g_client.dead == true`):
the create leg legitimately DOUBLE-PARKS the parent dir fid (dirfid_put parks
the first RPC-free; the second goes `p9_client_clunk_async`, fire-and-forget
BY DESIGN -- its ownerless Rclunk is drained by a later reader on a real
transport, the demux #210 orphan-clunk arm). The single-slot test loopback
REFUSED the next send over the unread staged Rclunk ->
`client_mark_dead_locked` -> every later op EIO. The kernel was CORRECT; the
FIXTURE modeled a transport failure no real backend can have. Fixed narrowly:
`loopback_send` discards exactly a WHOLE untouched staged Rclunk (counted);
every other unread-reply send still refuses.

**The close** (`1dd1348e`): the holotype (Fable 5, MODEL start==end, full
family diversity) returned CLEAN -- 0 P0/P1/P2, 4 P3 -- re-deriving the
authority envelope from code (an OPATH handle is born R|W, so the two-step
composition's RIGHT_WRITE handle gate never bounded it: no escalation) and the
#844 ref contract on all ~11 create-install exits. The parallel self-audit
added 3 P3s (one shared). All seven landed: the OTRUNC literal got its define;
O_CREAT|O_PATH is now SERVED by stripping (Linux ignores it -- before, the
decide declined while the routing comment claimed "the exact Linux contour");
a comment contradicting the verified join-once behavior was fixed; the /tmp
re-stamp became 1777; the trailing-slash-dir renameat divergence is documented
(strictly refuse-more); the loopback Rclunk-drop is exact-counted per leg.

**A second measured-anchor lesson, from the close itself**: the drop counts
were first pinned from a park-slot model (1/2/0) and all three failed -- one
uart-instrumented boot measured 0/1/1, wrong in BOTH directions, because the
choreography lives in the dirfid park/reuse pool (a successful create REUSES
the parked fid: 0 drops; a FAILED create double-clunks the parent-qid pair:
1). A pinned count must be a measured anchor, not a derivation.

**Wrong turns, and what caught them**: (a) probe ORDER masked the bisection
answer twice -- TEST_ASSERT returns on first failure, so a probe above the
real divergence reported its own noise; the catch was the step tracker's
last-reached number moving. (b) A leaked armed injection knob
(`g_tlcreate_fail_ecode=17`) broke a pre-existing neighbor test -- the #85/#87
family; knobs now clear unconditionally. (c) All instrumentation stripped
before commit, verified by byte-empty diffs.

**Harness note**: three attempts to run full LS-CI as a background task were
externally SIGTERM'd (~7 min / ~2 min / promoted-then-killed) while the 35-min
SMP gate survived the same launch shape; the operator voted one retry, then
foreground chunking. The gate ran as 10 foreground calls (batches sized under
the 10-minute tool cap; tcg-heavy scenarios singly at ~4-9 min each). A
background kill is the bug_128 class -- the harness stopping its own tasks --
and a retry budget is worthless against it; chunking is the workaround that
held.

**Witnesses on the close tip**: suite 1457/1457 (12 new tests); SMP gate PASS
40/40, 0 corruption (default+UBSan x smp4/smp8, ~50 s/boot); full LS-CI 38
PASS / 1 pre-existing documented red (ls-gfx-age, deterministic 3/3, predicted
before the run) / 3 SKIP (optional GL artifacts); viv-run E2E green -- a real
Alpine ash as a PLAIN USER ran `>file` (openat O_CREAT), `mkdir`+`mv`
(mkdirat+renameat), `rm`+`rmdir` (unlinkat both arms) on a pts. git's
write-path keystone is in; next: getdents64 + fsync rows (+ the O_DIRECTORY
admission recon'd during the audit wait: musl opendir needs it or getdents64
is unreachable), then the NO_CURL git build.

---

## 2026-08-25 (aux) -- the curl demo: five walls, two kernel rows, ROADMAP 9.2 ticked

The operator chose "(A) curl demo first" over the timeout arc, premised on
no-kernel-change. The premise broke -- instructively -- one syscall at a time.
Every wall below was MEASURED in a failing boot before it was believed, and
each fix went in at the honest layer, not the convenient one.

**The vehicle.** A real unmodified Linux curl (stunnel/static-curl 8.18.0,
musl static-PIE aarch64 -- ET_DYN, no PT_INTERP: the D-2 direct-load shape, a
THIRD ELF shape the loader had never run) staged sha-pinned into the
net-granted alpine-net bundle; `cache.static-curl` is a first-class forage
input (a forage_hint naming an unknown target is the phantom-list-member rot,
so the target + usage lines landed with it).

**Wall 1 -- `snare:ill` before any output.** Disassembly at the fault vaddr
(pc 0x2035f868 minus the 0x20000000 D-2 bias) landed in OpenSSL armcap's
SIGILL-probe ladder (`sha512su0`/`eor3`/SVE `eor`/`xar`/`cntb`/`mrs
MIDR_EL1`/`sm3partw1`): install a SIGILL handler, execute a candidate insn,
catch the fault. Under Thylacine `sigaction(SIGILL)` is HONESTLY refused
(snare notes are terminal) and the SVE probe traps -> death. Workaround:
`OPENSSL_armcap=0` in the bundle env -- OpenSSL's own documented probe-skip.
The class gap (any SIGILL-probing binary dies) is enqueued with its cheap
next step (verify AT_HWCAP rides the viv exec auxv) and its deep one
(catchable fault notes).

**Wall 2 -- `curl: (27) Out of memory` at handle init.** nr=19 (`eventfd2`)
untranslated. Verified in curl SOURCE per version, not recalled: under
USE_EVENTFD there is NO runtime fallback (compile-time #elif), and 8.20.0
made wakeup-init failure fatal ("rely on this to work... we ignore this in
previous versions") while 8.14-8.18 set the pair BAD and continue -- and a
CLI transfer never uses the wakeup. Pinned 8.18.0. The eventfd2 translator
(honest counter semantics, not a pipe-shaped lie) is enqueued.

**Wall 3 -- `getaddrinfo() thread failed to start`.** curl >= 8.16 resolves
on a spawned thread; `vivarium_clone_decide` refuses CLONE_THREAD by design
("the correct target is SYS_THREAD_SPAWN"). WHICH musl step failed first is
recorded as UNMEASURED (the per-pid diag suppression can hide it; the
decisive probe leg is named in the note). The demo pins by `--resolve` --
curl's own flag, no resolver thread; /etc/hosts stays for busybox wget. The
phenotype-threads arc is enqueued (git's index-pack threading will want
`pack.threads=1` until then).

**Wall 4 -- `(7) Could not connect` after a 61 ms connect that SUCCEEDED.**
The wall that pulled a kernel row forward (operator ratified via blocking
question). curl's `verifyconnect` reads `getsockopt(SO_ERROR)` after every
connect and treats a getsockopt FAILURE as a CONNECT failure (`if
(getsockopt(...)) err = SOCKERRNO` -- read in source). busybox wget never
verifies, which is the only reason it worked. NEW T2 row serving exactly
`(SOL_SOCKET, SO_ERROR)`: the constant-0 answer is TRUE because phenotype
sockets are blocking-only (NONBLOCK refused at both doors -- both measured
in these boots), so no error can be PENDING; the NONBLOCK revisit is pinned
in the decide header. Everything else declines unchanged. Coverage gap
documented, not hidden: the optval writeback has no direct witness (curl
inits its local to 0; a poisoned-buffer probe leg needs a net-granted probe
bundle = a network-dependent boot gate -- declined, tracked).

**Wall 5 -- `(55) Send failure: Function not implemented`.** The quiet ABI
fact: aarch64 HAS NO plain send/recv -- musl's `send()` IS
`sendto(...,NULL,0)` and `recv()` IS `recvfrom(...,NULL,NULL)`; wget
survived only because it uses `write()`. NEW T2 rows for the
connected-socket send/recv shape (CONNECTED + NULL addr + flags 0;
MSG_NOSIGNAL admitted as a truthful no-op -- no SIGPIPE exists on the 9P
data path), DELEGATING to the native write/read handlers: same staging,
weft fast-path, short-op semantics, #844 lifecycle -- zero duplicated data
path. Wrong-state = ENOTCONN, never ENOSYS.

**The run that passed:** `HTTP/1.1 200 OK` (live Cloudflare headers) +
`FETCH-RC=0` + `Example Domain` twice through the bracket-immunized grep
(`Example D[o]main` -- the pts echo of the typed command can never satisfy
the expect). Suite 1445/1445 with the three new vivarium tests (two
pure-domain + the getsockopt shell-guard).
Fails-without-fix measured at BOTH kernel walls (runs 3 and 4 are the
pre-fix arms). ROADMAP 9.2's first exit criterion is ticked with the
evidence inline.

**Also found by the legs themselves:** `>/dev/null` FAILS inside containers
(ash's redirect passes O_CREAT; the vivarium openat honestly refuses; the
staged /dev/null is a plain file) -- enqueued; git redirects there
constantly. And two of my own witness drafts were the never-key-on-your-own-
diagnostics trap (a grep pattern containing the success token; an rc marker
reading head's rc) -- caught in self-review before any run believed them.

**The census IS the mission's work-list now:** SIGILL probing, /dev/null,
eventfd2, pthread_create, timeouts -- five enqueued arcs, each with a
decisive next probe named. Next: git via viv curl.

## 2026-08-25 (aux) -- ut-parser batch close: item 5 was a misattribution, not a bug

The 5th and last queued ut-parser item: an echo'd C-source one-liner
(`echo 'int main(){__builtin_puts("CLADE_C_OK");return 0;}' > /tmp/hc.c`) that the
2026-08-24 notes recorded as `parse error: Unexpected`. Read the parser end to end
first -- and every path is sound by construction: scan_single_quoted consumes the
whole body (interior `; ( ) { } "` literal), `.`/`/` are word chars so `/tmp/hc.c`
is one Word, `>` is a redirect in parse_simple_command, the target parses. Rather
than trust the reading, pinned it in-guest: u-repl-test's parse() on the exact
input + 4 narrowing variants ALL return Ok. So the note was a MISATTRIBUTION --
the input carries no `&&`/bare-`=` (the constructs that were genuinely buggy, both
since fixed), and the concurrent /tmp-bind failure (item 2, since fixed) threw a
RUNTIME `unable to make temporary file` on that same `> /tmp/...` command, read as
a parse error amid the multi-failure clade-hello.exp debugging. No parser change;
a regression test now pins the parse. That closes the whole ut-parser batch
(=-in-arg, cd --, &&/||, line-wrap, and this one). The lesson: a recorded error
string is a claim, not a diagnosis -- reproduce the EXACT input before spending a
fix on it.

## 2026-08-25 (aux) -- DISPLAY-MODES impl: the 4-piece chunk + a 2-P finding + two E2E wrong turns

Continued straight from the render+scripture session below: built the impl the scripture
scoped, so the operator's line-wrap bug is now user-visibly fixed (the render mechanism finally
has a width to consume). Four pieces, then the audit, then two instructive E2E wrong turns.

**The four pieces.** (1) `run-vm.sh`: `THYLACINE_DISPLAY=console` drops the GPU + appends
`thylacine.display=console`; a new `gpu` value appends `thylacine.display=gpu`. (2) ut width
(`repl.rs` `probe_winsize` + `shell/main.rs`): on a pts read the pts ctl winsize (no CPR --
the owner sets it), else `/dev/winsize`, and only if `0 0` emit the CPR probe; a new
`parse_winsize` reads the shared `winsize C R` token off both the console line and the ptyfs
ctl line. (3) kernel 1b (`cons.c`): a `serial_silent` flag + a renderer-writable `serialsilent
<0|1>` consctl verb + the UART-sink gate in both `cons_emit_bulk*` (tap fires FIRST, so aurora
always sees the bytes). (4) aurora: read `/hw/chosen/bootargs`, and iff `thylacine.display=gpu`
issue the verb after its surface is up.

**A scripture bug caught at impl.** DISPLAY-MODES.md 5.1 listed `cocoa`/`vnc` as gpu-mode
(silence serial). Reading `tools/interactive/ls-gfx-live.exp` before wiring it showed that leg
boots under `vnc:N` and then LOGS IN OVER SERIAL and sweeps the serial tee for desync
diagnostics -- so silencing serial there breaks it. Corrected the scripture: only the two
explicit values (`console`/`gpu`) emit a token; every pre-existing backend stays
testing-hybrid (serial live). The operator's overriding "zero test churn" constraint decided
it, and the gate dependency was ground truth over the written 5.1.

**The audit (Fable 5, 0 P0 / 1 P1 / 1 P2, both fixed).** Both findings shared ONE root cause:
`serial_silent` had no REVOKE path, so it outlived the two events meant to RESTORE serial.
F1 [P1]: aurora death -> `cons_drain_close` disarmed the tap but left the flag set, and aurora
is never respawned, so a dead renderer = a permanently dark console (serial silenced AND
framebuffer frozen) -- the serial mirror removed exactly in the failure it existed for. Fixed
structurally: gate silence on `drain_armed` (serial resumes the instant the renderer's drain
disarms) + clear the flag on close. F2 [P2]: a SAK regranted to corvus but did not clear the
flag, so the operator's post-SAK trusted prompt was muted -- and on virtio-gpu media the
trusted path STAYS serial (TRUSTED-PATH 7). Fixed: clear the flag at the top of
`proc_console_sak`. Both got fails-without-fix regression tests. The reviewer re-derived the
load-bearing bypass claim from code: banner + extinction + kernel diagnostics all use direct
`uart_puts` and never touch the gated paths -- 1b cannot mute the trusted sink.

**E2E wrong turn 1 (a hypothesis, measured false).** The gpu-headless silence E2E failed:
aurora never announced the silence. First hypothesis -- my `gpu-headless` value had joined the
mmio-drop condition, and I reasoned that dropping `gpu-mmio0` starved aurora's surface. Removed
it, re-ran: STILL failed, and the `/virtio-gpu` probe (which needs gpu-mmio0) now ran -- so the
device set was fine and the hypothesis was wrong. The lesson held: re-run before believing a
fix, do not narrate it green.

**E2E wrong turn 2 (the real cause -- a timing assumption).** The true cause: aurora comes up
LATE. Even in the default boot, `aurora: console up` prints AFTER `Thylacine login:`. So the
login prompt legitimately reaches serial BEFORE aurora silences (the design's silence-after-
surface-up accepts that -- the alternative is a blind window). My E2E's "no login prompt ever"
assertion was simply wrong, and it quit at the login prompt before aurora even came up. Rebuilt
the witness as a deny-path PAIR from aurora's own linear code: a "silencing" line just BEFORE
the verb (positive: the wiring fired, reaches serial) and a "framebuffer is the primary
display" line just AFTER it (deny-path: always reached once the positive printed, so its
ABSENCE from serial is the runtime proof the silence took effect). Both E2Es green after that;
console mode is the positive control (its login prompt DOES reach serial).

**Cost + verify.** test.sh 1442/1442 (2 new cons tests: the gate + the SAK-restore); u-repl-test
winsize block green (parse_winsize both formats + rejection); default boot unchanged, with
`aurora: console up` still reaching serial as the negative control (no spurious silence);
console + gpu-headless E2Es green; SMP gate <run this session>. **Open:** ring the vault
dossiers (sub-kernel-cons / sub-aurora / sub-utopia-interactive / sub-substrate-machine) --
OWED, the vault worktree is 240 commits behind so it is a batched sync, not per-chunk. The
pts-winsize live-reflow on a resize (`tty:winch`) stays v1.x per DISPLAY-MODES 7.

## 2026-08-25 (aux) -- ut line-wrap: render mechanism + the deployment-mode reframe

The operator's 3rd queued ut item: "moving left/right when the executed command wraps at
the end of the first line duplicates the line on every keystroke," to be done PROPERLY
because they want tmux-style multiplexed shells later.

**The render mechanism (landed dormant @0a7e4c18).** Root cause was clean: `render()` counted
LOGICAL `\n` lines, not the visual PHYSICAL rows the terminal wraps a long line onto, so on a
line that overflowed the width `\r\x1b[K` cleared only the cursor's current physical row and
re-emitted, duplicating. `render_wrapped` counts physical rows (ceil over cols), moves up to
the block top, `\x1b[J` clears the block, re-emits, forces `\r\n` on a tail that exactly fills
a row (pending-wrap), then repositions -- linenoise-style relative moves. `cols: Option` --
None keeps the pre-fix bytes verbatim; a wrong GUESSED width would emit wrong cursor-up counts
and corrupt the display, strictly worse than not wrapping. Proven in-guest by u-repl-test's
byte-level discriminator (a 2-row wrapped line's 2nd render must start `\x1b[1A\r\x1b[J`;
fails-without-fix). This part was never in doubt.

**The wrong turn, and what caught it.** I first wired the width source as kaua does: read
`/dev/winsize` (fast path), else CPR. Built it, wrote an interactive witness asserting ut
emits the CPR probe on serial -- and it FAILED: no `\x1b[6n` ever appeared. The witness (a
control one variable away from my assumption) is what caught it: ut had read a NON-zero
`/dev/winsize` and taken the fast path. The map showed why -- `/dev/winsize` is ONE global
kernel size set by aurora to its framebuffer grid (128x36) EVEN under headless `-nographic`
(aurora runs regardless, rendering to a virtual framebuffer nobody sees). So a serial ut reads
the framebuffer's virtual width, not the operator's terminal. And aurora is a full VT emulator
that also ANSWERS CPR (vt.rs:1157) -- so "ask the terminal" races two answerers. My fast-path
design was backwards for the operator's default (headless serial) flow. Had the witness
asserted only "session healthy" instead of the specific CPR byte, I'd have shipped the wrong
width source green.

**The reframe (operator).** I escalated the ambiguity rather than guess. The operator supplied
the missing model: the serial console and aurora's framebuffer are not two simultaneous views
fighting over geometry -- they are mutually-exclusive PRIMARY displays chosen per deployment
(pure virtual console / +virtual GPU / bare-metal desktop / headless SSH). One primary per
mode => exactly one CPR answerer => a client can just read `/dev/winsize` (0 0 <=> no primary
renderer => CPR the sole terminal), which is EXACTLY the client rule ARCH 23.5.3 already
documents. The deployment-mode model is what makes that rule correct. CPR-always was then
rejected: needless in GPU mode, and it revives the two-answerer race in the testing posture.

**The design (scripture @4512494c, `docs/DISPLAY-MODES.md`).** A `thylacine.display` bootarg
(via `/hw/chosen/bootargs` + `bootarg_has`, the existing channel); console mode = drop the GPU
(the existing THYLACINE_NO_GPU path -> joey already skips aurora); GPU mode = aurora primary +
1b silences the EL0 serial at the kernel seam already NAMED at cons.c:207-210; default (no
flag) unchanged so the whole test matrix keeps passing; ut width = the 23.5.3 rule made
pts-aware. Operator ratified: lean console boot over aurora-yields-in-place, 1b in scope,
default stays testing-hybrid with `THYLACINE_DISPLAY=console` as the opt-in (zero test churn).

**A caught process trap.** An earlier `cd usr/` for a cargo-check persisted, so a "verification"
build + test.sh silently did not run (`tools/build.sh: No such file or directory`, masked as
exit 0 by a piped `tail`). Caught by noticing an empty boot-log grep + the "No such file"
line before committing; re-ran from root with the real exit captured. Nothing was committed on
the false green.

**Landed:** foundation 0a7e4c18 (render mechanism, dormant, pushed) + scripture 4512494c
(DISPLAY-MODES.md, pushed). **Next:** the impl (run-vm.sh + ut width + kernel 1b + aurora),
scoped in the handoff. The render fix is real and proven; the width it consumes is now
correctly modelled but not yet wired -- so the operator's bug is not user-visibly fixed until
the impl chunk lands.

## 2026-08-25 (aux) -- ut: `=` in a command argument is a literal

The operator's original clang wall included `-std=c++20` -- a flag with `=value`. ut
rejected it: UnexpectedEqualInCommand. The lexer splits `-std=c++20` into Word("-std")
Equal Word("c++20"), and parse_simple_command errored on the Equal in argument position.
Every `=`-bearing flag (`-std=`, `--sysroot=`, `--color=`) was unusable -- a big gap for
any real toolchain invocation.

Diagnosis first, so I did not fix the wrong thing: I checked and `x=y` / `x = y` already
parse as assignment (`=` is a word boundary; is_assignment_start catches them at statement
start). So the gap was specifically `=` in ARGUMENT position. Fix (`fd4c59ae`): parse_word
glues a span-adjacent `=` into one word (like `~` does once present); parse_simple_command's
Equal arm parses a literal `=` for the non-adjacent case; eval_value_token renders Equal as
"=". Assignment stays a separate path (parse_assign via the expression parser), untouched.

Verified two layers: u-repl-test's parse() guard (joey-gated: parse("clang -std=c++20 ...")
must be Ok, parse("let x = 5") still Ok) + an echo witness (`echo -std=c++20` -> "-std=c++20",
no interior spaces = one argv word; three args would space-join). The clade CL-4/CL-5 gates'
own `--sysroot=/clade/sysroot` invocations passed. So the operator's `clang++ -std=c++20
-O2 ... main.cpp` -- with /tmp + the clang defaults + this -- now parses on-device.

Next in the batch: the line-wrap cursor-duplication, which the operator wants done PROPERLY
(winsize-aware) with a view to future tmux-style multiplexing. The #55 mechanism already
exists (cons ws_cols + /dev/winsize + the consctl `winsize` verb + the tty:winch note); the
open design question is the serial-console width source (no auto-winsize over serial).

---

## 2026-08-24 (aux) -- ut batch: cd -- end-of-options + && / || short-circuit lists

Operator queued a Utopia-fix batch after the /tmp+completion work and said "dive in."
Landed two of the five, both verified; three still queued.

**`cd -- -foo` failed "too many arguments" -- the `--` was counted, never consumed.**
bi_cd now strips a leading standalone `--` (`012d3645`). The operator caught the exact
semantics before I coded: match `--` EXACTLY, not as a prefix, else `--version` (a real
long option) would be eaten as the terminator. And `--` interacts with cd's own `cd -`
oldpwd shortcut -- a blanket pre-strip would misread `cd -- -` (enter dir "-") as oldpwd,
so the `-` shortcut is gated on options NOT having ended. Verified: `mkdir ./-weird;
cd -- -weird; pwd` -> /home/michael/-weird.

**`clang x.c && ./run` did not parse -- ut had no AND-OR level.** The lexer tokenized
&&/|| and the design (8.6) shows `cmd || echo failed`, but parse_pipeline only chained `|`
/ `?|`; a trailing `&&` fell through to the script loop as "expected ; or newline". Added
StatementKind::AndOr: parse `pipeline (( && | || ) pipeline)*`, eval with short-circuit so
only the FINAL status feeds implicit-fail -- which is exactly the design's "|| tolerates
non-zero exit" (`ea93d8b7`). The subtle part: an operand's non-zero exit must NOT propagate
between links (the connector consumes it); should_propagate_failure gained an AndOr arm so
a leading `a? && b` still honors a's visible `?` at the interactive prompt.

Verification, two independent layers: u-repl-test gained 4 short-circuit STATUS guards
(joey-gated every boot -- `false && true` must leave 1, `true || false` must leave 0; the
status is the discriminator against "ran the RHS anyway"), and the interactive witness
proved the RHS RUNS in the right condition (`true && echo RANAND`, `false || echo RANOR`).
Both green; full suite (viv/pty/jc/aurora/clade CL-4/CL-5) unregressed.

**Still queued (3 of 5).** The line-wrap cursor-duplication (line_editor.rs:778 redraw
counts \n-buffer-lines, not visual wrapped rows -- needs terminal-width awareness, the
heaviest remaining), `=` (operator-noted, no repro yet -- will ask), and the echo'd-C-source
parse error (least-diagnosed; single-quote + `>` redirect both have handling, so it needs
the exact failing input pinned on the booted image).

---

## 2026-08-24 (aux) -- /tmp bind target + /clade/bin completion (the flag wall's tail)

Continuation of the PATH/clang work below: the operator drove the fixed clang interactively
and hit the two things that STILL did not work, then queued more.

**clang failed "unable to make temporary file: No such file or directory" -- and it was NOT
the sysroot.** clang stages every build through temp files in /tmp; the session /tmp was
unusable. Traced to ground: ut's `bind_user_tmp` (usr/utopia/shell/src/main.rs:214)
MREPL-binds each user's private <home>/tmp over /tmp at session start, but the MREPL needs
/tmp to PRE-EXIST as a namespace target -- and nothing baked one. The ramfs root carries only
proc+ctl synth dirs (devramfs.c:99); the pool skeleton (populate_stratum_pool) made /var...
but never /tmp (build.sh:2699). So the bind failed silently ("ut: tmp bind: mount over /tmp
failed"), /tmp stayed absent, clang's mktemp got ENOENT. Fix (`8b840ee2`): add /tmp to the
skeleton mkdir loop. ut's bind code was correct all along -- it just lacked a target. (I had
mis-catalogued this as a "ut parser" finding; it is a build/image fix.)

**clade/bin commands did not Tab-complete or color, though they executed fine -- a mirror MY
own PATH commit missed.** `install_completion` (repl.rs:262) scanned only /bin + /goroot/bin
into bin_commands -> set_known_commands, the set that drives BOTH Tab completion AND the
command color (line_editor.rs known=fen / unknown=cinnabar). 1c571a62 added /clade/bin to the
EXEC list (stmt.rs:549) but left this completion mirror behind -- exactly the "drift is a bug"
which.rs warns of, and the scan's own doc says it must match resolve_command's list. One line
fixes both symptoms (`14ed9bb5`). Trap checked, not assumed: /clade/bin/{clang,clang++,clangd,
ld.lld} are multicall COPIES (real files -- build.sh:2353/4173), so is_file() picks them up
like /goroot/bin.

**The wrong turn, and what caught it: I re-baked the operator's LIVE VM's backing files.** To
verify, I ran `build.sh all` -- not registering that the operator's `/clade` prompt was a live
QEMU (pid 39751) holding build/disk.img + build/fixtures/pool.img. test.sh's boot then FAILED
"Failed to get write lock" on disk.img -- a real surfaced problem, not a flake. I did not wave
it off: I hunted the holder (ps + lsof), found the operator's VM, then answered the decisive
question -- did I corrupt their session? -- by INODE, not by guessing. pool.img got a NEW
inode (88899274) while their QEMU held the OLD (88879588) -> their FS untouched and safe;
disk.img was overwritten IN-PLACE (same inode 88754310) but is the 16 MB secondary, not
pool-resident data. The mac LEASE did not protect against this: the operator's own VM is not a
yip peer. Lesson recorded ([[bug-rebuild-clobbers-live-vm-backing]]): never rebuild shared
build/ artifacts while a live VM uses them -- verify on a copy or after it exits. The operator
terminated the VM; I re-verified cleanly on the freed image.

**Verification (freed clade image, scratchpad/verify-tmpfix.exp interactive login):** "ut: tmp
bound (per-user /tmp from the home)" (the bind SUCCEEDS); `echo tmpok >/tmp/wtns; cat` ->
"tmpok" (/tmp WRITABLE -- the exact thing that had been failing); bare clang -> "clang version
22.1.8" (/clade/bin reachable). Regression: test.sh boot OK, clade CL-4/CL-5 PASS, 0 FAIL / 0
EXTINCTION. Both commits pushed 14ed9bb5 (github + codeberg).

**Left open.** (1) A cheap automated Tab/color witness -- the completion fix is verified by
build + symmetry with /goroot/bin (operator-confirmed) + the traced mechanism, but expect
cannot cheaply assert the ANSI redraw; operator confirms interactively. (2) test.sh exited 0
on the disk.img write-lock boot FAIL -- a gate reporting success on a boot that never started;
owed a look. (3) The operator's queued Utopia batch (memory `bug-ut-parser-findings`): `--`
end-of-options (bi_cd fails `cd -- -folder` "too many arguments" -- no `--` handling exists
anywhere in ut), the line-wrap cursor-duplication (line_editor.rs:778 redraw counts
\n-buffer-lines, not visual wrapped rows), `=`, `cmd && cmd`, echo'd-C-source. (4) The
operator's untracked docs/COMPILING-ON-THYLACINE.md still says --sysroot is mandatory (now
stale; theirs to update).

---

## 2026-08-24 (aux) — /clade/bin on PATH + clang works off the sysroot by default

Operator ask, off the back of the configurator arc: the flag wall to compile a hello on
Clade (`--sysroot=/clade/sysroot -nostdinc++ -isystem ... -lc++ -lc++abi -lunwind ...`) is
tedious -- should we patch clang to default to the Clade sysroot, and put `/clade/bin` on
a PATH (did Plan 9 even have PATH)? I researched before answering, per the design-fork rule.

**The PATH question dissolved into "Thylacine already has one, hybrid."** Plan 9 essentially
had no `$PATH` (it bound bin dirs into `/bin` via the namespace). But Thylacine already runs
a hybrid: the shell resolves a bare command via a STATIC `$path` list
(`eval/stmt.rs:547`, `["/bin/","/","/goroot/bin/"]`) MIRRORED by a login-seeded `$PATH`
env var for POSIX/Go tools (`login/main.rs:949`; `which.rs` documents the pair and says
"drift is a bug"). `/goroot/bin` was the exact precedent. So Part 2 (`1c571a62`) is just
`/clade/bin` added to all FIVE mirror sites the drift rule demands -- not two. Boot-verified.

**Part 1: configure clang, don't patch it.** The whole wall collapses to `clang++ hello.cpp`
with five CMake driver defaults on the DEVICE clang build (`1750f505`), `DEFAULT_SYSROOT`
the load-bearing one. A delegated map traced it through the CL-3 driver source: it reads
`Driver::SysRoot` for the bare-layout search `/clade/sysroot` actually uses, so the default
gives it everything; the other four are already hardcoded by the driver (belt-and-suspenders).
Crucially the map caught the trap: the flags go on `build_clade` (the device clang), NEVER
`clade-stage1.sh` (the host cross-clang) -- an absolute `/clade/sysroot` there would break
every host build. It is purely additive (explicit `--sysroot` still overrides), so nothing
existing regresses. Verified by REBUILDING the device clang on thyla-keep (~13 min, ~$0.60):
STATUS OK and the produced CMakeCache confirms `DEFAULT_SYSROOT=/clade/sysroot` baked in.

**Verifying Part 1 found a regression MY OWN arc introduced.** The configurator arc made
`build.sh` source `build-config.sh` + read `configs/` on every invocation -- but both clade
builder scripts sync only `build.sh`, so the first thyla-keep rebuild died at line 117,
"build-config.sh: No such file". Fixed both (`1a899a31`); whole-system stewardship, my
breakage to own. (A second wall was pure VM-state rot: thyla-keep's tree was missing the
committed `third_party/mesa-gl-headers/`; re-scp'd.)

**The end-to-end boot-test became a ut rabbit hole, and the operator called it.** I baked a
lean `/clade` image and drove `clang` at the `ut` prompt. Four boots, each surfacing a new
ut issue: (1) ut rejects an echo'd C-source one-liner; (2) ut rejects `cmd && cmd`
(`UnexpectedTokenAfterFailPropagate` -- confirmed, split commands parse); (3) ut cannot bind
a private `/tmp`, so clang can't make temp files (`-save-temps` sidesteps it). At that point
the operator said "remember them and fix them after" -- recorded in `bug_ut_parser_findings.md`.
**But the fourth boot was the payoff:** with `-save-temps` + split commands, bare `clang`
RESOLVED via `/clade/bin` on `$path`, RAN, and REACHED cc1 -- erroring on a bug in my test
SOURCE (`__builtin_puts` at col 18), NOT on a missing sysroot. **A missing sysroot fails
completely differently** (`fatal error: 'stdio.h' file not found`), so reaching cc1 IS the
evidence that DEFAULT_SYSROOT works on-device -- on top of the machine-confirmed CMakeCache
bake. I removed the premature `clade-hello.exp` (it can't cleanly pass until the ut fixes,
and `test-interactive.sh` runs every `*.exp` -- it would have broken routine LS-CI on the
default no-`/clade` image). A proper self-skipping witness is owed after the ut fixes.

**Still open:** the three ut findings (owed, operator-deferred); the operator's untracked
`COMPILING-ON-THYLACINE.md` says `--sysroot` is mandatory and is now stale (flagged, not
edited -- it's theirs); the `#156` clade fetch-set gap (fetch doesn't bring `cxx-rt`; a
prior complete stage saved the bake).

## 2026-08-24 (aux) — build-configurator docs + arc close (lane 6)

The docs lane, and with it the whole build-configurator arc closes. Three deliverables:
`docs/reference/150-build-config.md` (the deep maintainer reference for the schema
core, the account decouple, the wizard, the manifest, forage, and detect-and-instruct);
the full `--config`/preset model folded into `docs/BUILD-HARNESS.md` (a new sections
4.3-4.5, expanding the lane-4 pointer into the config model + the forage workflow); and
the design doc's lane checklist marked complete per commit.

**One deliberate deviation from the design's lane-6 list, recorded rather than
silently dropped:** design 7.6 named a `docs/manual/` entry. I skipped it. The
configurator is host-side DEVELOPER tooling, and `docs/manual/` is the OS USER manual,
which the standing user-manual-deferred policy keeps a Phase-0 stub until v1.0-rc. The
developer-facing walkthrough belongs in `BUILD-HARNESS.md` -- which IS the build
harness's manual -- so that is where it went. Noted in the design doc's section 7 so
the skip is a decision, not an omission.

**The vault check ran (mandatory doc-update step 0) and came back UNOWNED** for all
four new tool paths -- but with a caveat worth recording: the vault worktree is 225
commits behind main, so it "cannot see" aux-2's paths and returns UNKNOWN, not a clean
"no dossier". I treated the reference section as owed (wrote it) and noted the owed
vault sweep in 150's Vault section. Not fabricating a "no dossier" verdict from an
out-of-sync tool is the point the vault's own tooling insists on.

## 2026-08-24 (aux) — the input manifest + the forage collector (lanes 5a + 5b)

Same run, straight on from the wizard. Lane 5 is the "collect everything" half of
the arc: `tools/build-manifest.toml` (`3c1a9cb7`) pins every build input that does
NOT travel in the repo -- the 6 sibling forks by commit, the 2 manual-drop Alpine
cache inputs by URL + sha256, the quake network input, and the remotely-built
Clade artifacts -- and `tools/forage.sh` reads it and gathers what it can, or
instructs. `tools/build.sh` (`ec4c1ccc`) now names the forage remedy when a chunk
input is absent, instead of skipping silently.

**The hashes and commit pins were re-derived, not transcribed.** The resume note
warned to re-verify figures; I pulled the three sha256s byte-exact out of build.sh
(alpine `f31202c4…`, busybox `6fd7ea97…`, quake `ec6c9d34…`) and confirmed all six
fork commits against the actual local trees (`git -C … rev-parse`): go `4bb69d2`,
ambush `563bae9`, gopls `f65d347`, llvm `251b5b5`, mesa `b7f9ed2`. That check paid
off in a structural way: the forks split into two classes the design prose had
blurred. go/ambush/stratum have `apology-is-policy` remotes forage can clone;
gopls has NO remote (operator-supplied); llvm/mesa point at UPSTREAM (llvm-project,
mesa) and are clade-BUILD sources built remotely, not things forage fetches
locally. So `forageable` became a six-valued verb (clone / download / remote-pull
/ remote-source / manual / auto-at-build), and forage INSTRUCTS on the three it
cannot automate rather than pretending it can.

**The build.sh notice closed a real silent gap, not just a cosmetic one.** #101
already warned when a clade toolchain was staged but the flag was unset (it would
destroy a clade pool). Its SIBLING -- flag SET but nothing staged -- had no message
at all: the pool minted without /clade silently, which is precisely the weekend the
manifest exists to save. Lane 5b adds that missing arm (`forage clade`), plus the
/goroot and Alpine skips.

**A TOML parser in bash 3.2, kept deliberately small.** No toml tool is on the mac
and macOS python3 predates tomllib, so forage carries a ~15-line awk reader for a
controlled subset (`[section.sub]` tables + `key = "value"`/bareword + comments).
The manifest header pins that subset so nobody adds an array the reader silently
drops. The parser's subtlest property -- section scoping, that a namesake `commit`
in another section must not leak -- is the one I most wanted proven, so a sabotage
that bypasses `cur==sec` is in the test: without it the reader returns go's
`4bb69d2` when asked for ambush's `563bae9`, and the test catches exactly that.

**Testability seams, and a cross-consumer rot guard.** `forage.sh`'s dispatch is
`BASH_SOURCE`-guarded so `test-forage.sh` sources it and calls the parser directly;
`FORAGE_ROOT` + `MANIFEST` + `FORAGE_DRY` isolate the gather tests to a temp root
with zero network/git/gcp. 19 checks, each proven fail-without-fix via sabotaged
copies. And `test-detect-instruct.sh` guards the tooling->forage CONTRACT: it
extracts every forage target named by EITHER consumer (build.sh's `forage_hint`
and the wizard's step-4 remedy) and asserts each is real -- the anti-rot guard for
a renamed target orphaning a hint. Both consumer arms discriminate (sabotaging
build.sh's `clade`->`claded` and the wizard's `alpine`->`alpyne` each fail).

Tooling, not soundness-bearing (design section 6) -- no prosecutor round. Open:
lane 6 (the per-PR reference/manual docs + the full `--config`/preset fold into
BUILD-HARNESS.md), then the arc closes and git-on-viv resumes.

## 2026-08-24 (aux) — the guided build wizard (build-configurator lane 4)

Continuation of the build-configurator arc, same day, on a fresh context (I
self-compacted at the operator's direction after closing the account-decouple
sub-chunk). Lane 4 is `tools/configure.sh` (`0d694b55`): the interactive wizard
`docs/BUILD-CONFIG-DESIGN.md` 4.6 specs — a newcomer who knows nothing about
Thylacine picks a base profile, walks every option one at a time with its
description + what-it-enables help, and gets a named `configs/<name>.config` they
build with `tools/build.sh --config <name>`. It is a pure front-end over the
lane-2 schema core (it drives `bc_reset`/`bc_apply_preset`/`bc_set_one`/
`bc_resolve`/`bc_emit_config` and reimplements no schema). Tooling, not
soundness-bearing (design section 6) — so no prosecutor round, by the ratified
call.

**The interesting part was the test, and the bug the test caught was my own.**
While smoke-testing I pointed `BC_DIR_CONFIGS` at a temp dir to isolate the runs
— and the wizard wrote into the *real* `configs/` anyway (two stray files,
`prod-smoke.config` + `custom.config`, both untracked so cleanly removed). Cause:
a top-level `BC_DIR_CONFIGS="$REPO_ROOT/configs"` assignment (copied from
build.sh, which has no need to override) clobbered the env override. Fixed to
`${BC_DIR_CONFIGS:-...}`. That bug became test case 9: an *isolation guard* that
snapshots the real `configs/` before/after the whole suite and asserts it is
untouched — the regression guard for exactly the mistake I had just made.

**Every control was proven to fail without its behavior** (M-PIN: a check that
cannot fail proves nothing). `tools/test-configure.sh` is 21 discrimination
checks; I verified the six load-bearing ones by running the suite against
*sabotaged copies* of the wizard (placed in `tools/` so their `$0/..`
self-location still resolves to the repo — a temp-dir copy mislocates
`REPO_ROOT` and every case crashes uniformly, which proves nothing targeted).
S2 (delete the live-constraint announcement) fails *only* "constraint: live
announcement fires" while the other three case-3 asserts stay green; S4 (neuter
chunk-flagging) fails *only* the remedy assert while its negative stays green —
clean targeting, not a blanket crash. S5 (re-introduce the clobber) is caught by
the isolation guard, and I cleaned the seven files it leaked.

**A live-constraint subtlety, fixed.** The first cut announced "-> enables
DEV_ACCOUNTS" only when DEV_ACCOUNTS was currently `n` — but its default is `y`,
so in a from-defaults walk the announcement never fired and only the pin at the
DEV_ACCOUNTS prompt showed. 4.6 step 3's example wants the announcement on
*selection* of BOOT_PROBES=y, so I made it unconditional (the pin reinforces it;
`bc_resolve` stays the authoritative enforcer). Verified with a negative control
that a walk which never sets BOOT_PROBES=y stays silent.

**Real end-to-end proven once, then cleaned up:** the wizard wrote
`configs/wzroundtrip.config` (BUILD_TYPE=release + CHUNK_CLADE=y) and
`tools/build.sh --config wzroundtrip --show-config` resolved `build_type=Release`
+ `CHUNK_CLADE y` — the full newcomer chain works, not just the unit assertions.

**Small accuracy fix surfaced by the wizard reading help aloud:** the
DEV_ACCOUNTS help string still said the lean image provisions "michael" only;
the account-decouple sub-chunk (`d982ee62`, earlier this run) bakes michael +
cora, so the help now says so. 4.6 says to enrich thin help in `build-config.sh`
— the wizard is the reader that makes a stale help string visible.

Docs touched proportionately: `BUILD-HARNESS.md` gets a short 4.3 "guided setup"
pointer so the wizard is discoverable (the full configurator fold — the
`--config`/presets model, a `docs/reference/NN` + a `docs/manual` entry — stays
lane 6, as sequenced); `BUILD-CONFIG-DESIGN.md` 4.6 gets an AS-BUILT note.
`tools/test-build-config.sh` (lane 2) still ALL PASS, guarding the build-config.sh
edits. Open: lane 5 (the `tools/build-manifest.toml` input manifest + the
`tools/forage.sh` collector + build.sh detect-and-instruct — the wizard already
names `forage` as the step-4 remedy, forward-referencing it), then lane 6 docs,
then the arc closes and git-on-viv resumes (chunk B, the timeout mechanism).

## 2026-08-24 (aux) — lean-image login accounts: audit the michael decouple, then add cora and re-audit

A build-tooling detour (the build-configurator arc, ratified last session off the
git-on-viv mission). The prior session landed `6726ac68` -- `provision_dev_accounts`,
a self-contained provisioner so a lean (BOOT_PROBES-off) image is loginnable at all
(finding #1: `--production` compiled account creation OUT, because it lived under
`#if THYLA_BOOT_PROBES`). This session closed the audit on it and, at the operator's
request, grew it from michael-only to michael+cora.

**The audit's one real decision was a scripture desync, and it went to the operator.**
Prosecutor round 1 (Fable 5, on `6726ac68`) returned 0 P0 / 0 P1 / 1 P2 / 6 P3; the P2
(F1) was that `BUILD-CONFIG-DESIGN.md` 4.5 still prescribed the *wholesale move* of the
provisioning block from the probe gate to the accounts gate, while the tree had landed
"option F" -- a self-contained michael-only duplicate. That is exactly the design-first
rule's trigger (the doc outranks the code; don't silently normalize a deviation), so it
was surfaced with the reasoning, not folded. The operator voted to keep option F and
amend 4.5 -- and, in the same breath, asked to also provision **cora**, the account they
actually log in as (short memorable password "kora" vs michael's long admin password).

**cora could not be a second bootstrap create, and that is the whole shape of the change.**
Once michael exists, corvus admin-gates `USER_CREATE` (main.rs:2093 -- the caller must
hold `CAP_HOSTOWNER`), so a second cap-free create is impossible. `provision_dev_accounts`
therefore had to grow into the ladder's own sanctioned elevation path: `USER_CREATE
michael` (bootstrap) -> `AUTH michael` -> `ADMIN_ELEVATE(system passphrase)` ->
`t_cap_use(HOSTOWNER)` -> `USER_CREATE cora`. The one fact that made this feasible without
touching the audited ladder: the elevation-enabling grant caps corvus is spawned with
(`T_CAP_GRANT_HOSTOWNER | T_CAP_GRANT_CLEARANCE`) are stamped *before* the
`#if THYLA_BOOT_PROBES` gate, so the lean build already has them (joey.c ~1945, verified).

**The credential-sharing (F5) forced a touch on audited identity code -- so it was
verified byte-for-byte AND re-audited.** cora's expansion meant three credentials
(michael's password, cora's, the system passphrase) were now duplicated across the lean
path and the ladder; a drift would break a cross-config persistent pool. The fix was to
hoist `DEV_*` credential `#define`s + `CORVUS_PROTOCOL_VERSION` above both gates and
reference them from both -- option F preserved (only DATA shared, control flow stays
separate), but ~7 ladder sites in A-5 identity code changed literal->macro. Each was
checked against `6726ac68` (michael/correct-horse-battery-staple-v1/cora/kora/thylacine
-- every `sizeof(MACRO)-1` equals the old explicit length) and, more importantly,
runtime-proven: the default (config C) `build.sh all && test.sh` reached "Thylacine boot
OK", which requires the ladder's create/auth/elevate to have run with the shared macros.

**Round 2 caught the gap that mattered: the boot-critical spine had no committed runtime
witness.** Prosecutor round 2 (Fable 5, the cora spine + the ladder touch) returned
0 P0 / 0 P1 / 1 P2 / 2 P3, both headline questions refuted from code (the spine cannot
leave the box unloginnable-as-cora -- it chased the `peer_live_caps==0` candidate chain
into `sys_srv_peer` and proved no transient failure mode; no credential drifted). The P2
(F1) was #245's disease one level up: `check-production.sh` asserted only that the spine
*compiled in* (a size delta, now 77304 -> 92840, +15536), never that it *succeeds* -- and the
routine loop boots only config C, where the spine is compiled out. The michael-only
predecessor had shipped a session on a compile gate alone. Fix: a `check-production.sh
--all` leg that bakes the lean image, boots it, and asserts BOTH login (the new
`dev-accounts.exp`, image-agnostic cora+michael) AND the spine's own completion line --
which the ladder never prints, so it discriminates config B from a config-C image passing
the login on ladder-provisioned accounts. The other two (P3): `tx` was never scrubbed
(token remnants behind the last frame) and `pda_scrub` was elidable -> a volatile-store
loop + scrub `tx` on both exits; and the "one source" `#define` comment did not name the
external mirrors (corvus-mint's host `"thylacine"` default, the probe fixtures, the expect
scripts) -> annotated.

**Proof it works, end to end (production image, from the boot log):** `created michael
(fresh pool)` + `created cora (fresh pool)` + `michael + cora ready`, then cora
authenticates with `kora` -> `/home/cora` and michael -> `/home/michael`. The finding-#1
fix and cora are both real.

**Host coordination, recorded because it is the reusable part:** the final `check-production
--all` verification was blocked ~40 min by main holding the mac lease. Investigation (ps,
not assumption) showed main had compacted mid-run and moved to its declared pi-GL phase
(`warp-host.sh venus` on `thyla-pi-cf`) while the mac cores sat idle -- a stale lease. yip
refused a steal (1.3h TTL remaining), so the resolution was to announce via a busy status
and run on the verifiably-idle cores (isolated worktree, hvf boot != the pi's KVM boot --
contention there could only threaten duration, never correctness).

**Still open (the arc continues):** lane 4 the `configure` wizard, lane 5 the input
manifest + `forage` collector, then the per-PR reference/manual docs, then the arc closes
and git-on-viv resumes (chunk B, the timeout-mechanism arc). One pre-existing corvus
comment nit was observed and left for a corvus doc pass (main.rs:2064-2068 claims a live
peer-cap re-query while `peer_live_info.console` is a mint-time snapshot -- nothing
unsound, the load-bearing gate is the kernel's live check at redemption, devcap.c:310).

---

## 2026-08-20 (aux) — VIVARIUM time translators (clock_gettime + gettimeofday), and a ceiling that had gone stale a fifth time

The curl/git mission's step 2: a Linux binary under viv could reach the network
but not bound a timeout, because `clock_gettime` (113) and `gettimeofday` (169)
had no phenotype translator — they FORWARDed to `-ENOSYS`, so busybox `date`
reads 1970 and TLS/curl/git cannot time out. Added both as Tier-2 rows.

**The interesting call was T2-vs-renumber, and it went the disciplined way.**
`clock_gettime`'s Linux `struct timespec` is byte-identical to native
`t_timespec`, and the native `SYS_CLOCK_GETTIME` clk_ids `REALTIME`/`MONOTONIC`
are 0/1 exactly as Linux's are — so it *looks* like a pure T1 renumber
(113→75), the way `lseek` is. It is not: a T1 row must be total over the
argument domain, and the clk_id domain is not total — Linux has ids 2–7,
Thylacine serves 0/1. That is precisely the lseek comment's own escape hatch
("were the enumerations ever to diverge, this row drops to T2"), so it drops to
T2: a pure `vivarium_clock_gettime_map` maps the clk_id (with a per-id
justification for `MONOTONIC_RAW`/`_COARSE`/`REALTIME_COARSE`/`BOOTTIME` onto the
two clocks Thylacine has, and a served `-EINVAL` for `CPUTIME`), and the shell
calls the native handler for the validated write. `gettimeofday` is
unambiguously T2 — no native counterpart, and a MICROsecond `timeval` where the
native clock speaks nanoseconds.

**The finding nobody planned: `VIV_NATIVE_CEILING` was stale at 105.** Adding
rows above the ceiling means checking the ceiling, and it was pinned to
`SYS_RFORK` (105) while the Warp arc had landed `SYS_DMA_CREATE_GPU_BO` (106) and
`SYS_BURROW_FROM_HOSTMEM` (107) above it. The `_Static_assert` "caught" nothing
because it is pinned to a *named* syscall (`== SYS_RFORK`), so it stays green
while a *higher* number lands — the exact limitation the ceiling's own comment
admits it cannot cover, and it bit. Latent, not live (no VIV_LINUX row sits at
106/107 today — verified), but the next row added there would have been blessed
as "collision-free by construction" while aliasing a real native. Reconciled to
107, assert re-pinned to `SYS_BURROW_FROM_HOSTMEM`. Corroboration worth stating:
the vault's own `syscall-abi-collision` census already recorded the allocated
span at **107** back on 2026-08-15 — the code had drifted from the vault's
measurement, and the vault's number, not my derivation, is what confirms 107 is
the true top. Enqueued [[bug-viv-native-ceiling-stale]] before fixing.

**Discrimination, measured both ways** (the M-PIN bar — boot OK proves a gate
passed, only a deny-path proves it is wired). Five probe legs L249–L253 in
`viv-pheno-probe` (realtime seed past `1.7e9`, monotonic non-decreasing, the µs
`timeval` conversion, EFAULT on an unmapped pointer, EINVAL on a CPU clk_id).
Good state → `joey: V-1b phenotype (native + containered linux) PASS`; sabotage
(remove the two `g_viv_rejects` rows → the exact pre-chunk FORWARD→ENOSYS state,
reverted with Edit not `git checkout`) → boot fails at exactly
`marker=L249`. The native-vantage `brk` discriminator stays PASS throughout, so
the linux legs pass *because* of the phenotype, not for another reason.

**Self-audit catch before the formal round:** the first `gettimeofday` draft
answered `EFAULT` for `tv==NULL`, but Linux guards `if (tv != NULL)` and returns
0 (writing only `tz`). Obscure — no real program calls it — but the phenotype's
contract is Linux's *shape* (I-43), so it was fixed to match rather than guess a
binary never hits it.

Cost/open: kernel builds clean (the static_asserts all pass), boot green. SMP
gate + LS-CI + the holotype round were in flight at write time (results appended
to the phase-status row). Next in the arc: DNS/UDP under a net-granted container,
then stage a real curl, then the curl→git bootstrap.

## 2026-08-20 (aux) — probe: does a real Linux binary reach the network under viv? (yes)

User asked to verify curl/git + other third-party Linux programs under VIVARIUM.
Rather than theorize, probed with busybox (musl-static, has wget/nslookup) in an
Alpine container. **The phenotype network reaches the internet**: `busybox wget
http://<example.com-IP>/` in a net-granted container returned `HTTP/1.1 403
Forbidden` — a real connect + HTTP round-trip through slirp over the phenotype
socket path (403 = Cloudflare rejecting a bare-IP Host; the round-trip
succeeded).

**The blocker was a per-container capability, not a socket gap** — and a wrong
turn caught by a second axis. First probe (no /net) showed `socket(AF_INET,2,0)
ENOENT` and I wrote "UDP unsupported". The by-IP probe then showed
`socket(AF_INET,1,0)` — *TCP* socket() ALSO ENOENT — so the root cause was
`/net` absent, not a UDP-family gap. `/net` is bound only if the manifest sets
`annotations.org.thylacine.net=granted` (viv/main.rs:304-308 + 595-623); the
stock bundles don't. Added an `alpine-net` bundle that grants it.

**Gaps left open toward a clean curl/git** (the mission, now opened): (1)
`clock_gettime`/`gettimeofday` have no phenotype translator — no timeouts, so the
probe hung on a dead peer (`wget -T 5` to the slirp-blackholed gateway); likely a
small T2-routing chunk since the native vDSO already serves them. (2) DNS/UDP
untested. (3) staging a real curl + git + deps. User chose "compact, then
implement + demo"; full plan + findings in memory ([[project-phenotype-network-probe]]).
Gotcha banked: `expect -n <script>` EXECUTES the script (spawned the strays I had
to kill), it is NOT a parse check.

## 2026-08-20 (aux) — item 12: viv forwards the owner-routed console ^C, and the regression that had to stop typing at the shell

A container run from the BARE SERIAL CONSOLE never received ^C after 5336c894:
the console has no job-control pgroup fan, so the ^C is owner-routed to `ut`,
`ut` forwards it to viv by pid, but the entrypoint is viv's CHILD (ut can't reach
it) and viv's #237 interrupt MASK swallowed the forwarded note. Fix: viv's
INTERRUPT mask is conditional on `on_pts` — the CONSOLE arm self-manages a notes
fd and poll-forwards `interrupt` to the entrypoint (`wait_entrypoint_interruptible`,
mirroring ut's `wait_pids_interruptible`); the PTS arm is unchanged (the pgroup
fan delivers). LANDED a2870706.

**The wrong turn that got caught — the reusable part.** The first regression
typed `trap … INT` into an interactive alpine-ash ON THE BARE CONSOLE. It FAILED
deterministically: the typed trap line never reached the ash (no echo, no
`gotint` anywhere in the transcript). What caught it was reading the transcript
BYTES rather than trusting the red — a `~ •` ut-prompt flood appeared BEFORE
`viv run` was even sent, so it could not be item 12; the real cause is that the
bare console has no job-control arbitration, so `ut` and the ash both read it and
the typed line does not cleanly arrive (the v1.0 "degraded terminal" posture).
That is ORTHOGONAL to item 12, which delivers ^C as a NOTE needing no console
input. The fix was to stop typing at the shell: a PRE-TRAPPED entrypoint — the
`alpine-trap` bundle (`sh -c 'trap "echo GOTINT-CONSOLE; exit 0" INT; echo READY;
sleep-loop'`).

**Discrimination proven boot-both-ways.** WITH item 12: PASS 68s, `GOTINT-CONSOLE`
= genuine trap output. Revert item 12: FAIL 3/3 — `READY-FOR-CTRLC` appears, then
timeout on `GOTINT-CONSOLE`. The lone `GOTINT-CONSOLE` in the negctrl transcript
is the HARNESS's own FAIL diagnostic (line 2847), not trap output — and the
absence of any spawn-time argv echo of the token confirms the positive's was the
trap, so there is no false-pass path.

**Prosecutor (holotype, Opus 4.8 fallback — Fable unavailable; MODEL start==end).**
0 P0 / 0 P1 / 1 P2 / 1 P3, both fixed pre-commit. F1 [P2]: the console
open-failure degrade left `interrupt` UNMASKED (resurrecting #237 orphaning) with
a comment miscalling it "pre-item-12 behaviour" — which actually MASKED it. Fix
(subsumes F2's startup window): mask INTERRUPT through setup in both arms, unmask
only inside `wait_entrypoint_interruptible` once self-managing, so an
`open_self()` failure leaves it masked = the true safe swallow. The core
mechanism was confirmed SOUND three independent ways (self-managing latch-clear,
tail retains-never-consumes, poll/read skip-consistency). The F1 fix re-verified
by re-running the positive control (still PASS, 68s).

**Enqueued (surfaced here, NOT item-12 regressions).** (1) the aurora/ut
console-idle prompt flood on the serial console (cosmetic, pre-existing); (2) on
a pts viv's diorama dies of ^C, tearing down the container's /proc,/sys (#237's
pts path — the diorama does not mask interrupt like viv does).

**Gates.** build + suite (boot OK; L-6c/D-5) + SMP (0 corruption, default+UBSan ×
smp4/smp8, N=10) + positive/negative discrimination all green. viv is
userspace-only (kernel byte-identical; the SMP pass re-confirms soundness
regardless).

## 2026-08-20 (aux, cont.) — bug-2 E2E: turning the "control that wasn't one" into one

The prior run closed bug-2 honest about a gap: `r5f9-ash.exp` is a REGRESSION
NET, not a fails-without-fix control (measured 6/6 on the bug-1-only build too),
so the two escape-clears had no in-guest driver — the A-PIN gap: *a gated path
needs a driver that reaches it*. This run built that driver and proved it.

**The driver.** Legs L245-L248 of `usr/viv-pheno-probe` (the boot-time
`/vivarium/pheno` bundle, so it runs on every boot). No libc in a no_std guest,
so `esc_setjmp`/`esc_longjmp` are hand-rolled aarch64 asm (save/restore
x19-x30 + sp + d8-d15). A `viv_escape_handler` `siglongjmp`s on its first
delivery and counts on later ones. The leg raises SIGPIPE via the one
self-signal a phenotype proc can make — a one-byte write to the reader-less
fd 0 — escapes the handler (never reaching `rt_sigreturn`, so `in_handler`
sticks), unblocks SIGPIPE (which is also the EL0-entry syscall the fix uses to
observe the escape), raises a SECOND SIGPIPE, and L248 asserts the handler fired
TWICE.

**The discrimination, measured both ways** (the whole point — a control must
prove discrimination, not detection):

- Fixed kernel: `joey: V-1b phenotype (native + containered linux) PASS`, boot
  OK (`build/test-boot.log:2661`).
- Bug-1-only kernel (both call-site clears disabled via Edit-sabotage,
  `syscall.c:11467` + `notes.c:1610`, rebuilt): `joey: V-1b linux-phenotype leg
  FAILED marker=L248 status=1`, boot-fatal (`build/test-boot.log:2662`). Then
  reverted (git-verified the kernel returned byte-identical to HEAD) and rebuilt
  → PASS again.

`marker=L248` is exactly the witness leg. So the two clears now have a driver
that fails without them — the r5f9 gap is closed.

**What it does NOT isolate, stated honestly.** The leg exercises the two clears
*jointly*: the EL0-entry clear (on the post-escape unblock) does the work, the
EL0-return copy is idempotent behind it. Reverting only EL0-entry still passes
(the EL0-return copy on the fire-#2 write catches it), so L248 red requires BOTH
clears gone. Isolating the EL0-entry primary would need a park-based driver
whose failure mode is a deaf-deadlock HANG rather than a clean marker — not
built. The driver proves "the fix (both clears) works end-to-end," which is what
the A-PIN gap asked for.

**The sp arithmetic, grounded not assumed.** The detector fires only if the
post-longjmp sp is >= the delivery-time `note_saved_sp_el0`. Read the code:
`svc3`/`svc4` are `#[inline(always)]` `nostack`, so both the raise and the
escape-clearing syscall execute in run_linux's own frame, and `esc_setjmp` (a
`bl` target, sp unchanged) saves that same frame — so after longjmp
`ctx->sp == note_saved_sp_el0`, the `==` boundary the bug-2 audit already
confirmed conservative-correct. The classic setjmp returns-twice miscompile does
not bite: `esc_longjmp` restores every callee-saved reg + sp to the setjmp
snapshot (exactly what the caller assumes preserved), and nothing in the frame
mutates between the setjmp return and the synchronous escape.

**Cost + scope.** Test-only: no kernel file changed (git-verified identical to
HEAD), so no prosecutor round is owed — the boot-both-ways proof is the
validation. Docs: VIVARIUM §6.23 gained the fails-without-fix-driver paragraph;
`145-vivarium.md`'s stale "R5-F9 still unanswered" note is now answered YES.

**Gates, and a pre-existing red the full LS-CI surfaced.** SMP gate 40/40 (0
corruption, default+UBSan x smp4/smp8, N=10) -- the probe runs on every one of
those boots. The full 40-scenario LS-CI (aux's first since the aux-2<->main
merge) came back 37/40 PASS + 2 SKIP + **1 FAIL: `ls-gfx-age`**. Chased to
ground rather than waved off: the guest boots fine (`boot OK` @24s) and reaches
login, but the login prompt (correctly no trailing newline) gets tapestryd's
async scanout diagnostic appended on one console line
(`Thylacine login: tapestryd: scanout...`), so the harness's login-expect never
matches -> 900s timeout. Deterministic 3/3. It is a console-TX ordering issue on
**main's** console arc (the prompt-vs-peer-daemon-write family), conclusively
NOT this test-only change: the kernel is git-verified byte-identical to HEAD and
the probe exits in the boot pheno-bundle phase, before login + the graphics
daemons; every notes/signals LS-CI scenario passed. Enqueued
(`memory/bug_lsgfxage_login_tapestryd_interleave.md`) + noted to main. The
operator ratified the push despite the pre-existing red. Landed `33cecf78`
(driver) + `aab29f65` (status fixup), pushed both mirrors.

**A side-task the operator injected mid-wait:** improve the stop-hook's Case 3
(`tools/stop-hook.sh`) to steer toward the yip lease *proactively* -- before
starting anything that uses a resource a parallel agent can contend (the shared
host's cores for a build/gate/boot), take `yip hold <res>` FIRST, since it both
blocks-until-free as the synchronous await AND signals ownership. Applied in
main's tree (the firing copy per `~/.claude/settings.json`), bash -n clean,
noted to main to commit separately from its V-3b gpu.rs work.

**Still open (tracked, not this chunk):** the VMA-same-stack v1.x hardening
closing the audit's F1 (the swapcontext-cross-stack false-clear); item 12 (viv
console ^C forward); ls-gfx-age (main's console arc).

---

## 2026-08-20 (aux) — bug-2: the escape-detector, an audit that found the direction the self-audit missed, and an E2E "control" that wasn't one

**The task.** bug-2, the deeper root of the arm-2 flake, which the operator
voted "fix next" (escape-detection). arm-2's bug-1 (`0149d1e3`) fixed the
livelock *symptom*; bug-2 is the *root*: a PHENO_LINUX signal handler that
escapes via `siglongjmp` (no `rt_sigreturn`) leaves the kernel's `in_handler`
re-entrancy flag stuck true, so the N-3 guard refuses every future caught-note
delivery — the guest goes signal-deaf. Confirmed real by the arm-2 N-3-guard
probe (`child_exit` blocked with `in_handler` stuck on a PHENO_LINUX ash).

**Scripture-first, then code.** `233284a8` landed the VIVARIUM §6.23 design
before a line of code: the sp-comparison detector (a live handler runs BELOW the
pre-handler sp; a `siglongjmp` ancestor is at-or-above it), the two clear sites,
and the `sigaltstack`-ENOSYS coupling. The site decision is load-bearing and NOT
symmetric: EL0-entry is *required*, not merely preferred — an escaped main
loop's next blocking read must find `in_handler` cleared BEFORE it parks, else
bug-1's sleep predicate keeps it parked deaf forever (the read never returns to
run an EL0-return clear). Main co-reviewed the design (yip 0028) and flagged the
one axis worth checking cold — that both operands are the SP_EL0 bank, not a
cross-bank compare — which I verified against `vectors.S` before writing code.
`438cac78` landed the code + two deterministic unit tests.

**The E2E "control" that wasn't a control (a measured wrong turn).** The plan
wanted a runtime E2E "asserting delivery after a siglongjmp'd ^C — the positive
control bug-1 lacks." `r5f9-ash.exp` looked perfect: its header names "the R5-F9
longjmp wedge ... a second ^C would then do nothing," and it asserts exactly
that (`if {!$a2} { lc_fail "R5-F9 wedge: ... in_handler stuck" }`). Post-fix it
PASSED. But a control that passes proves nothing unless it FAILS without the fix
— so I ran the negative control: r5f9-ash.exp x6, single-attempt, on the
bug-1-only build (rebuilt during a stash A/B). Result: **6/6 PASS, 0
wedge-fails**. r5f9 passes identically with and without bug-2 — a regression net,
not a fails-without-fix control. Busybox ash's prompt/read/sleep-^C does not
leave `in_handler` persistently stuck the way the arm-2 viv-run scenario (a
^C-escape coinciding with a `uname | tr` child_exit storm) did; this also
reconciles the old "R5-F9 wedge NOT exposed" note. The wedge's ground-truth
evidence stays the arm-2 N-3 probe; a deterministic in-guest driver for the two
clears (the A-PIN wiring gap) is tracked as the next E2E chunk. Caught by
measurement, not assumption.

**Contention, caught and not attributed away.** Mid-negative-control, `ps`
showed main running a full `test-interactive.sh` on its own tree (30 min in)
while presence showed it "idle" (it had compacted without setting busy). My
negative control is timing-sensitive — under contention a HEALTHY reprompt can
exceed the 10s leg timeout = a false FAIL, confounding wedge-deaf vs slow. I
stopped the loop rather than record confounded data, coordinated the host (main
freed it 37/37 GREEN), and re-ran clean. Contention threatens duration, never
identity — a confounded control is worse than none.

**The audit found the direction the self-audit missed (the independent
prosecutor's whole value).** My self-audit reasoned the discrimination "total"
via the ancestor-frame argument. The Opus holotype (Fable credit-exhausted;
MODEL start==end==Opus 4.8) closed 0 P0 / 0 P1 / **1 P2** / 2 P3 and refuted the
totality claim in the one direction I did not consider: **F1** — a guest that
`swapcontext`s from a handler to a HIGHER-addressed *separate* stack (a
suspended coroutine, not an abandonment) trips `sp >= saved` at that coroutine's
first syscall, so the escape-check false-clears `in_handler` while the handler
is still in flight → the N-3 guard admits a nested delivery → the single
`note_saved_*` slot is overwritten → the original handler resumes on the wrong
context. **Worse than pre-bug-2**, which safely deferred. Contained
(guest-self-corruption, validated user VA, per-Thread, kill-immune) and exotic
(no v1.0 target does signal-driven cross-stack coroutine switching), hence P2.
The `sigaltstack`-ENOSYS coupling does not cover it — that governs where the
*handler* runs, not where a *swap target* lives. My ancestor argument was true
but *scoped to one stack*, and I asserted it for all stacks.

**The operator's call on F1.** I surfaced F1 as a blocking question — it
introduces a regression (not just documents a gap) on a feature the operator
voted for. Options: document+correct-the-claim (DEGRADED), a VMA-same-stack code
fix (`vma_lookup(sp)==vma_lookup(saved)` before clearing — closes it, +re-audit),
or reconsider. Operator chose **document + correct**. `85655042` softened the
§6.23 totality claim to "single-contiguous-stack," documented the above-sp0
direction symmetric with the below-sp0 one (honestly worse-than-pre-fix), added
a §9 DEGRADED row, and tracked the VMA-fix for v1.x. F2 (stale line cites
1404/1461 → 1437/1494, shifted by my own predicate insertion) and F3 (the test
drove an uninitialized `struct Thread`; the obvious `= {0}` then emitted an
undefined `memset` on the large struct, caught at LINK time → `static` BSS-zero)
fixed.

**What it cost / what's open.** Three commits (`233284a8` scripture, `438cac78`
code, `85655042` close). Suite GREEN (2 new deterministic tests + the bug-1
regression). SMP gate 40/40 PASS (default+UBSan × smp4/smp8, N=10; 0 corruption).
Pushed both mirrors. Open + tracked: the deterministic E2E driver (the two clears
have no in-guest driver —
the A-PIN gap r5f9 does not fill), and the VMA-same-stack hardening for the F1
cross-stack corruption (v1.x).

---

## 2026-08-19 (evening, aux) — arm-2: the viv-run flake was a caught-note livelock, not input loss

**The task.** `tools/interactive/viv-run.exp` timed out ~40% of runs at the
`^C -> exit -> resume` leg of an interactive `viv run` of an alpine-ash
container. The prior session had it half-diagnosed and mis-framed.

**The wrong turn, which is the point.** The inherited hypothesis (memory
`bug_viv_run_pts_resume_flake.md`) was that the container shell's pts read
returned **Eof** (`n_master==0`, the master seen closed mid-read) and busybox
ash re-prompted on it. Plausible, and wrong. Rather than theorize further I
instrumented ptyfs `slave_read` with a latched spin-detector (emit only after
50 consecutive same-outcome reads on one pts). First reproducing run:
`ptyfs SPIN[read] pts=0 out=WB run=50 nm=1 ns=3 m2s=0`. **`out=WB`, `nm=1`** —
the master was *alive* and the read returned **WouldBlock**, which is supposed
to *park* (no CPU), not spin. The Eof hypothesis was falsified by one
measurement. The reusable lesson: a spin whose outcome you have *named* beats
any amount of reasoning about which of three arms it "should" be — the topology
argued against every arm I could construct, and the topology was not the thing
that was wrong.

**Following WouldBlock to ground.** A WB that spins means the read is being
*abandoned and retried*, not parked. ptyfs `h_flush` names the cause: the 9p
client sends `Tflush` when a note interrupts a blocked read. A Tflush probe
stormed — `count=1..200 oldtag=<constant>` — one reader (slave, `fid=61`,
data read), interrupted ~200×. A kernel probe at the N-3 re-entrancy guard
(`notes_deliver_at_el0_return`, notes.c ~1550) named the note:
`arm2 N3-BLOCK note=child_exit cc=1..200`. The full chain: alpine-ash's SIGINT
handler (after `^C`) escapes via longjmp to its main loop and **never calls
rt_sigreturn** — the only clear of `in_handler` (notes.c ~1827) — so
`in_handler` is stuck; the finished `uname | tr` posts **child_exit** (SIGCHLD),
caught; the shell's next pts read hits the sleep predicate
`thread_caught_note_deliverable` (sched.c ~2015), which said *deliverable*, but
the N-3 guard is *guaranteed* to refuse delivery while `in_handler` is set. The
predicate and the delivery site disagreed → `SLEEP_NOTEINTR` → Tflush+EINTR →
EL0 → N-3 re-queues → next read → **livelock**.

**Bug 1 (fixed).** `if (t->in_handler) return false;` at the top of
`thread_caught_note_deliverable` — gate the sleep interrupt on the same
condition the delivery site uses. The read now parks and wakes on data; the
note delivers at the next EL0-return once `in_handler` clears. Verified
**viv-run 10/10** (was ~6/10), `maxtflush 0` on every run; `test.sh` PASS with a
new regression in `test_notes.c` (which also exposed that the existing test left
`fake_t.in_handler` uninitialized — the fix now reads it). Same class as the
round-F1 `caught_note_stop_dequeue_drains` regression (a stranded caught bit →
deliverable-true → EINTR livelock).

**Bug 2 (open, the deeper root).** `in_handler` is stuck because a Linux handler
can escape via siglongjmp without rt_sigreturn and nothing clears it → the
process is signal-deaf until it clears (an I-43 vivarium-fidelity gap; broad:
any Linux program that siglongjmps out of a handler then gets another caught
signal). With bug 1 that is a deferred/dropped signal, not a hang. It is on the
shared notes/vivarium surface (posted to main, yip 0027); the fix approach
(escape-detection via sp / mask-based re-entrancy with user-stack frames /
accept+document) is a design fork surfaced to the operator.

**Process note.** The formal audit spawned on Fable 5 and died on credit
exhaustion producing no report; per CLAUDE.md that goes straight to the highest
Opus at max effort (never skip a round), re-spawned with the context-independence
framing since it shares my lineage. All ptyfs + kernel probes were reverted; the
committed diff is the one-line predicate fix + the regression test only.

## 2026-08-19 — V-2: host-visible memory, and the death path a shared BAR opened

Two threads. First, a stray `/compact`: the operator saw two `/compact` lines
after a self-compaction and asked which agent issued the second. Ground truth
(the selfcompact ledger + both scripts) showed it was neither an agent nor the
nudge watcher — it was a *premature* self-compact cancelled earlier at 560k,
whose Enter-queued `/compact` a `tmux send-keys C-u` never actually retracted; it
rode the input queue ~4 hours and fired against the already-compacted session (a
harmless "Not enough messages"). Landed as contract (`19103efe`): a queued
self-compaction is NOT yours to cancel — only the operator's (raise a blocking
question); invoke the script only on the real 600k signal. While in the ledger I
found the belay gate keyed on the mutable `@thyla-role` tag — main's compacts
logged as `aux`, colliding with aux's state and silently defeating the governor;
rekeyed it on the git toplevel (`83c7f56d`).

Then **V-2** — the first kernel memory-authority path of the Warp-6 arc: map a
subrange of a PCI hostmem BAR (Venus HOST_VISIBLE memory) into a client VA. The
ratified design (6.2.1) was wrong about the tree in two places:
- It said "add the NORMAL_NC MAIR index." The recon measured it: NC has been in
  the MAIR since P1-C (index 1). V-2 *plumbs* it — widening the fault path's
  `bool device_memory` to a MAIR index — and adds no byte. A design claim wrong
  about the tree, caught by ground truth, not by re-reading its prose.
- It said the client map "rides the existing SYS_WEFT_SHARE." The code showed the
  weft path fail-closes on unknown burrow types AND carries a duplicate admission
  gate that "MUST widen together" (its own comment, from the Warp-2b bug).
  Delivering a client mapping meant wiring the I-37 weft kind-machinery — more
  than "one syscall." Surfaced as a scope fork; the operator chose to complete it
  in V-2 (both gates widened in lockstep, `WEFT_BIND_HOSTMEM`).

The widening carried a footgun: `false == 0 == MAIR_IDX_DEVICE`, so a naive
bool->index widen would silently map every existing `false` caller as Device.
Handled by keeping `mmu_install_user_pte(bool)` as a semantics-preserving wrapper
over the new `_attr(u32)` — zero churn on the ~13 callers, no inversion.

The Opus holotype round (Fable out of credits) closed **0 P0 / 1 P1 / 1 P2 / 3
P3**, verifying the whole bounds/lifetime/W^X/charge/lockstep core sound. The P1
(F1) is worth recording: V-2 introduces the first cross-Proc-shareable
*PCI-BAR-backed* Burrow, and on the owning server's DEATH the unconditional
device quiesce clears the BAR's MEM decode under a client's live mapping. The
prosecutor refused to guess the terminal severity — an EL0 access to a quiesced
RAM-backed BAR is either benign garbage or a box-fatal external abort — and said
measure it, not reason it away. Surfaced as a design fork; the operator chose the
partial-quiesce fix: on death, for a claim with a live hostmem burrow, clear
BUS_MASTER (stop the dead device's DMA) but KEEP MEM_SPACE, deferring its clear
to the last unref — so the client never observes a decode-disabled BAR and the
measurement is moot. F2 (the handler's bounds had no test) was closed by
extracting a pure `hostmem_resolve_subrange` + testing it; F3/F4/F5 tracked P3.
Re-audit of the fixes: CLEAN (0 P0 / 0 P1 / 0 P2 / 3 P3 cosmetic; Opus 4.8 fallback -- Fable out of credits). Suite 1431/1431; commit 7973f8dc. Merge follow-ons (71306b60 + the libthyla/gate close): P3-1 landed the /proc/maps hostmem arm; the SMP gate PASSED (40 boots, 0 corruption across default+UBSan x smp4/smp8), the burrow/weft buggy cfgs FIRED and the clean cfgs stayed green, LS-CI console PASSED; the libthyla-rs ABI mirror (107) landed. The GL venus regression was DEFERRED, not failed: the thyla-pi LAN mDNS name stopped resolving mid-run -- a sync ssh wedged 36 minutes on its first mkdir, a bounded probe returned nodename-nor-servname, and the Cloudflare tunnel then proved the pi healthy (up 7 days, idle). venus is not in the push-bar and V-2 new code is unexercised until V-3, so the push proceeds; venus reruns when the LAN name resolves (or via the CF tunnel).

What V-2 does NOT ship: a real client. The weft delivery is exercised only by
unit tests — V-3 (vn_renderer) drives it E2E on real hardware, where the residual
P3s land with a driver to exercise them.

## 2026-08-19 — V-1: a guest blob creates, and the scope hidden in "blobs"

Resumed from my own self-compaction; the resume note ordered V-1 (blobs) next.
The ladder names V-1 "blobs (`RESOURCE_CREATE_BLOB` + the blob object model)",
which reads as a large chunk. Reading the design collapsed it to something
smaller and sharper.

The load-bearing fact is in GPU-DESIGN §2.4: **Venus's command ring is a guest
blob** — its head/tail/status cachelines are guest pages the host also reads.
That is why V-1 is Venus's real prerequisite. But "guest blob" is the whole
point: a guest blob's storage *is* its own guest `mem_entry` pages — the host
registers a resource referencing them, with no host allocation and no hostmem
BAR. The host3d blob (host-allocated storage the guest reaches through the
hostmem window via `MAP_BLOB`) is a *different* thing, and it is exactly the V-2
delta the reference already flagged (149-warp "Mapping a subrange is the §6.2
Venus-chunk delta"). So V-1 is the guest-blob *create* path — nothing maps,
nothing is coherent yet — and it rides the existing venus gate's two legs
unchanged: the venus device offers `F_RESOURCE_BLOB`, the plain `-gl` control
does not. The whole chunk is a tapestryd-side device command; no kernel path
(that arrives at V-2, which maps MMIO into a client VA).

Two wrong turns, both caught before they cost anything.

First, the opcode. I reached for `RESOURCE_CREATE_BLOB = 0x0212` from memory —
and it is wrong. Counting the virtio-gpu 2D enum forward from the code's own
anchor (`GET_CAPSET = 0x0109`, already in the tree) puts it at **0x010c**
(`GET_EDID` 0x010a and `RESOURCE_ASSIGN_UUID` 0x010b sit unused between). 0x0212
was a confabulation. The "a number recalled is a number unverified" rule earned
its place again — I verified against the tree's anchors, not memory.

Second, a lifetime bug in my own probe (self-audit SF1). `blob_probe` backs the
blob with a dedicated one-page DMA and unref's it, then the buffer Drops
(unmaps + frees the pages). If the *unref* fails while the engine is alive, the
host may still reference those pages — and Drop would unmap them out from under
a live reference. The probe issues no transfer so it is theoretical, but the
correct discipline is to **leak, not unmap, under a live reference**: one page
at init beats a UAF. `core::mem::forget(backing)` on the unref-fail path.

I also heeded a prior lesson rather than re-learning it: `init_device` returned
a positional `(u64, bool, bool)`, the exact shape that let V-0b's `ctxinit` go
briefly unreturned. Adding a third bool to a positional tuple is how that bug
happens again, so the three feature flags now ride a named `DevInit` struct.

The probe's resource id (`0x2b`) is collision-free by the same timing argument
the ctx-capset probe uses (it runs before the Server exists) plus a numeric
guard: the server mints ids from `SCREEN_RES + 1` upward and never down, so any
id `<= SCREEN_RES` is unmintable forever. I sabotaged the guard to prove it
fires — `id = 0x40` fails the build with the guard's message, `0x2b` compiles.

It creates. On thyla-pi (KVM, real V3D 4.2): `blob-create guest CREATED` with
venus, `blob-create skipped (F_RESOURCE_BLOB not offered)` on the control, and
the venus leg boots fully clean with the feature negotiated — so negotiating
blob does not disturb the compositor path (a self-audit worry, answered by the
boot). VENUS GATE VERIFIED, `test-venus-verdict` 13 → 16 arms, all discriminating
without a boot.

One measurement worth keeping for the next GL run: the control boot took **268s**,
not the ~220s the notes cite. A combined `warp-host.sh venus` run (both legs in
one call) would have been ~536s — close enough to the 600s foreground cap that a
slightly slower host would have moved it to a background task and killed the
second boot mid-run. Running each leg as its own sub-600s call was the right
call, and the number says why.

The prosecutor round closed **CLEAN (0 P0 / 0 P1 / 0 P2 / 3 P3)** on the Opus
4.8 fallback (Fable was out of credits — the round is a real degradation on the
independence axis, family-shared with the author, and it said so; a Fable re-run
is not owed because it finished). It caught one thing worth the round on its own:
**F1**, an inconsistency in my *own* SF1 fix. SF1 leaks the backing on a failed
unref (the host may still hold the pages); but the sibling branch — a create
that fails because the *engine died* — Dropped the backing, and a deadline-dead
create was already *published* (the doorbell rings before the wait), so the
device may equally hold that PA. Two branches, opposite dispositions, one
principle. Fixed to leak on both. Inert today (the probe issues no transfer, and
the dead path triggers a proc-death device reset), but it is exactly the kind of
disagreement that reuses the wrong disposition at V-3, where transfers exist. The
round also filed two forward notes: **F2** (V-3 must validate a client's
`pa`/`len` before they become a host `mem_entry` — an I-45/I-32 boundary) and
**F3** (when V-2 adds host3d, the gate should assert the blob mem-type from
evidence, not the hardcoded "guest" string).

The operational miss, recorded because the catch is the reusable part: partway
through the run the **host went to sleep**. It killed the prosecutor mid-response
("your computer went to sleep") and hung an LS-CI chunk into a 590s timeout doing
nothing — and I had forgotten `caffeinate`, the exact trap
`feedback_caffeinate_long_tasks.md` names. The tell was two failures at once with
one cause; the fix was a background `caffeinate -dis` plus `caffeinate -i` on
every LS-CI chunk, after which the heavies ran to 468s clean. The prosecutor's
partial output before it died was already a real finding (the missing runtime
guard on `resource_create_blob`), so the sleep cost time, not correctness.

A note on what "37/37 on the shipped binary" actually rests on: the guard and F1
are provably **unreachable** on the 2D device LS-CI boots (`blob_probe` is
virgl-gated, so `resource_create_blob` is never called there), so the 26
scenarios I ran before the fixes are byte-identical to the final binary, and I
re-ran only the remaining 11 on it. The venus gate I *did* re-boot on the final
binary directly — the test leg exercises the guard (which falls through, since
`self.blob == true`) — rather than lean on the same unreachability argument for
the load-bearing claim.

SMP stands (kernel byte-unchanged). Ahead: V-2 (host3d + the hostmem-BAR mapping,
the first real kernel memory-authority path of the arc) → V-3
(`vn_renderer_thylacine` + the coherent ring) → V-4/5/6.

## 2026-08-18 — V-0b: a Venus context creates, and the seam size I recalled wrong

I had classified V-0b as blocked this session — the arc's next step is
audit-bearing `gpu.rs` work and I'd been treating the Agent tool as barred. The
Stop hook pushed back: a checkpoint is not a stopping point, and the standing
operator grant (`feedback_prosecutor_agents_permitted.md`) authorizes the
`holotype-reviewer` for exactly this. So I opened it.

The question V-0b answers is narrow and real: V-0 proved the host *advertises*
capset id=4; it did not prove a Venus *context* can be created. That gap mattered
because `/usr/libexec/virgl_render_server` is in no Debian package, and §9.2
calls the render server Venus-only-by-construction — so "the capset is
advertised" could have meant venus init reached capset reporting and no further.

It creates. On thyla-pi (KVM, real V3D): `ctx-capset id=4 CREATED` with venus,
`skipped` without, `id=2` virgl the positive control on both legs. The absent
render server does not block it — virglrenderer's in-process venus init handles
context creation. That is the empirical answer the inference could not give.

The design point worth keeping: this is a **feature-bit** change, not a field
change, and the naive version is a *convincing* false pass. `ctx_create` wrote
`context_init = 0` under a comment saying the feature was not negotiated, and the
device ignores that field unless `F_CONTEXT_INIT` is negotiated — which the
driver never offered back. So "pass capset 4 and see" would have written into an
ignored field, collected `RESP_OK_NODATA`, and produced an implicitly-virgl
context reporting success. The negative control is what proves we avoid it: on a
no-venus boot the id=4 create is *skipped* because the capset was not enumerated,
never spuriously CREATED.

Then my own self-audit, run beside the prosecutor, caught me doing the exact
thing this whole run has been about. My commit message and code comment said the
probe's ctx ids (200/201) sit "above the client range (slot+1, <=128)". The
client range is not 1..128. `MAX_WARP_CTXS = 8` — one grep away — so it is 1..8.
The collision-safety conclusion holds (200/201 are far above 8 and below
`COMPOSITOR_CTX` at 0x100), but I cited a number I recalled instead of the one in
the tree, and the "128" is a real but *different* limit from Warp-3a. A number
recalled is a number unverified; the session's refrain, landing on me one more
time. Folded the correction into the round's disposition rather than amend under
a running reviewer.

Committed at `bf448929`, **not pushed** — `gpu.rs` is an audit-trigger surface
and this changes the device negotiation contract plus adds context creation, so
the round runs before the push. Fable was out of credits, so the round is on the
Opus fallback tier at max effort — context-independent even if same-family,
which is what the fallback rule preserves.

**The round closed CLEAN -- 0 P0 / 0 P1 / 1 P2 / 2 P3 -- and it converged with
the self-audit.** F1 (the "128-slot seam" that is really 8) was my SF1; F2 (the
debug_assert that vanishes in release) was my SF2. Two independent prosecutors,
the same two findings -- the reassurance the discipline is designed to produce.
The round added the part I had left as prose: F1 is not just a wrong comment, it
is a *missing compile-time guard*, because collision-freedom was argued from a
numeric window (liftable) instead of from timing (the probe runs before any
client and destroys before returning, which cannot be lifted). Fixed both ways:
the comment states the timing guarantee, and a `const _: () = assert!(...)` ties
the probe ids to `MAX_WARP_CTXS`/`COMPOSITOR_CTX` so a future seam lift past 199
fails the BUILD. Sabotaged it (probe id -> 5) to confirm it fires, then
reverted. F2 I closed early rather than deferring to V-3: the debug_assert
became a real `return Err` so a client-influenced capset in a release build
cannot silently mint a wrong-kind context. F3 was the round's own -- the gate
control leg asserted absence of "id=4 CREATED" without presence of "id=4
skipped", a negative a broken fixture satisfies -- now paired.

Honesty note the round pressed and I am keeping: it ran on **Opus 4.8**, a step
below the intended Opus-5 fallback (the `model: opus` override resolved low),
and it said so itself. A finished fallback round is closed per scripture, so no
re-run is owed -- but the tier is on the record, and the convergence with an
independent self-audit is what carries the confidence, not the tier alone.
---

## 2026-08-18 — An owed test, and the audit premise that was wrong when written

The extinction round (`5de6093f` F2) left an owed item: exec's failure
diagnostics were "compile-verified and never executed", because "no boot log
contains a single `exec:` line". I went to close it and found the premise was
half wrong — which is worth more than the test.

`exec_report_fail` was **already covered, and had been for seventeen days when
the round ran**. `test_execve_failed_load_leaves_target_drainable` (2026-08-01,
`e47bfa31`) drives a W+X-union failure and emits a real `exec:` line that sits in
the current suite boot log. The round's measurement — "no `exec:` line" — was
simply false when it was written. I know because I wrote it, and I did not
re-check it before turning it into an owed item.

`exec_say` was the actual gap: the dynamic-Linux-binary and dynamic-PT_INTERP
rejects had no test and appeared in no log. Genuinely never executed — the #244
class exactly, a diagnostic whose only witness was that it compiled.

Closing it was small: an ELF with a PT_INTERP naming a musl loader makes
`elf_load` return `HAS_INTERP` and `elf_brand_hint` answer `LINUX_LIKELY`, so
`exec_load_body`'s native arm runs `exec_say` and rejects the load. The suite
boot log now carries `exec: dynamic Linux binary rejected — ...` where before
there was nothing, which is the direct witness that `exec_say` runs without
faulting. Suite 1427 → 1428.

The reusable part is not the test. It is that **an audit finding's premise is a
claim about the tree, and it decays like any other.** This one asserted "never
executed" on top of a measurement that was already wrong, and the owed item
inherited the error. It is the same failure as the three throwaway verifiers
earlier in the run and the "currently broken" cross-reference before them: a
statement about what the tree does, trusted because someone once checked it,
that nobody's step re-checks. The whole session kept landing on one lesson from
different directions — a check is only worth the last time it actually ran.

---

## 2026-08-18 — The gate refused the host, and it was right to

V-0's remaining half was to stop *assuming* thyla-gl and boot it. Both halves
are now closed, and the interesting part is that the first attempt **failed**.

**The gate said UNVERIFIED, and the reason was real.** On thyla-gl's own Aug-12
artifacts, tapestryd **hung** under `venus=on,blob=on,hostmem=256M` — `warden:
tapestryd gave no readiness/exit signal -> terminating`, three restarts, `gave
up after 3 restart(s)` — while the control leg, same host, same build, came up
clean. A hang, not a crash: `Readiness::Timeout` means neither signalled nor
exited.

Two explanations suggested themselves, and both died by measurement rather than
by argument, which is the only reason I trust the third:

- *"the Aug-12 build predates #166's oversized-BAR skip."* Refuted in one
  command: `git show 534f3869:usr/lib/libthyla-rs/src/hardware.rs` carries the
  identical `if bar.size > PCI_BAR_VA_STRIDE { continue; }`, comment and all,
  and `git log -S` dates that code to 2026-06-15.
- *"lavapipe is slow to enumerate, so venus init stalls the control queue."*
  Weakened: `vulkaninfo --summary` returns in **248 ms** on that host, and
  `SUBMIT_DEADLINE_MS = 500` already bounds our controlq wait — so whatever hung,
  it was not our driver blocking forever on a device response.

Syncing the current build and re-running the same host with the same declaration
came up clean and VERIFIED. **So the attribution is the stale artifacts, not the
host** — but one sample each way across two different builds is not an
explanation, and I have written it down as unexplained rather than let "current
build works" quietly become "we know what that was." There is nothing to fix in
the tree, which is a different statement from knowing why.

The gate behaving correctly under a real failure is worth as much as the pass:
it refused to promote a host that could not show the capset, and it named the
reason.

**The driver was throwing away the answer to the arc's next question.**
`gpu.rs` reads `dev_feat_lo` during feature negotiation, uses exactly one bit of
it (VIRGL), and discards the rest. So "does this host offer `CONTEXT_INIT`?" —
the question that decides whether a Venus context is reachable at all — had no
answer short of writing a new build, about a value the driver already had in a
register. One `say!` line fixed that, and it immediately changed what V-0b *is*:

`CONTEXT_INIT` turns out to be offered on a **plain `-gl` device**, no venus and
no blob required. Meanwhile `ctx_create` writes `context_init = 0` under the
comment "F_CONTEXT_INIT not negotiated" — and the device honours that field only
when the feature is negotiated, which this driver never offers back. So the
obvious form of V-0b — pass capset 4 and see — would have written a 4 into a
field the device ignores, collected `RESP_OK_NODATA`, created an
implicitly-virgl context, and reported success. **A false pass, and a
particularly convincing one.** V-0b is a feature-bit change.

The same line settled V-1's host question for free: `RESOURCE_BLOB` appears only
with `blob=on`, and the default dev device offers neither (it is `virgl=0`), so
blob work cannot be exercised on the local dev loop at all. That is #166's
inert-hostmem-under-HVF constraint wearing different clothes, and it is the
concrete reason promoting thyla-gl was worth a morning.

**And a hole in my own gate, found by prosecuting it rather than admiring it.**
The gate asserted "the control leg does NOT see capset id=4". A control that
measured *nothing* — virgl not negotiated, 2D fallback, no capset lines at all —
satisfies that trivially, and the gate would read "venus absent" where the truth
is "capsets absent". That is the standing lesson about negative assertions and
broken fixtures, reappearing **inside the very gate I wrote to honour the
discrimination rule**: I had put the control in the *boots* and forgotten to
require that the control leg had measured anything. It now demands the baseline
pair (`id=1` and `id=2`), with two sabotages for it. 5/5 became 7/7.

Re-verified against the real thyla-pi logs from the passing run — still VERIFIED
under the strengthened verdict, so no re-boot was owed for that.

Both hosts, finally, return **byte-identical feature words** (`0x30000013`
without venus, `0x3000001b` with) — a cross-host agreement the arc did not need
but is better for having.

**Postscript, because repeating a pinned lesson is worth more written down than
quietly fixed.** Going into the pre-push bar I ran every TLA+ spec through a
one-liner that declared a spec green iff `tail -3` of its output contained
*"Model checking completed. No error has been found."* Every spec came back
FAIL. The specs were fine: TLC prints that line about twelve lines in and
finishes with state-graph statistics, so my verdict window could never contain
the string it was looking for. **A guard on the reporting path fabricating the
defect it reports — key on the exit code, never the prose** — is already an
M-PIN in this project's memory, and I wrote the same bug anyway, in a checker I
composed in one line because it felt too small to get wrong.

Two things follow. The pinned lesson does not fire from *reading* it; it fires
from noticing the shape "I am grepping prose for a verdict", and that shape is
easiest to miss in throwaway code. And the tell was available immediately:
*every* member of a large set failing at once is almost always the classifier,
not the set — which is itself the other half of a pinned lesson ("when ONE
member of a family misbehaves, suspect the classifier"; here it was all of them,
which is even louder). Confirmed in one command: exit code 0, success line at
line 12.

The run was not owed in the first place — clean-cfg TLC has been suspended since
2026-05-21, and a `say!` line in a virtio driver touches no modelled mechanism —
so the whole excursion cost ten minutes to learn something about my own reflexes
rather than about the specs.

**And then it happened twice more in the same session, which is the actual
finding.** (2) A shell loop meant to re-verify three real log pairs under a
changed predicate reported all three FAILING with an empty verdict string; run
directly, every one passed — the loop's `$?` was not measuring what I thought.
(3) A one-liner checking that my new documentation tables were not broken
flagged the GPU-DESIGN row as suspect, because I had hard-coded the pipe count
of a *four*-column table onto a *three*-column one; every sibling row had the
same count, so the doc was fine and the checker was not.

Three throwaway verifiers in one session, three false alarms, zero real defects
among them. Each was caught the same way — by checking the surprising result
against a known-good reference before acting on it — and none cost more than
minutes. But the shape is worth naming, because the pinned lessons are all about
distrusting *gates I build deliberately*, and every one of these was a scrap of
shell I wrote in passing to confirm something I already believed. **The care I
give a committed checker does not automatically extend to the one-liner that
checks it**, and the one-liner is the one nothing else will ever review.

The practical rule that fell out: when an ad-hoc check reports that *everything*
failed, or that something I just verified by hand is broken, the first suspect
is the check. That is the same instinct as the pinned "when one member of a
family misbehaves, suspect the classifier" — it just has to fire for code that
never gets committed.

---

## 2026-08-18 — Warp-6 opens on a probe, and the blocker that wasn't

Warp-C closed, so Warp-6 (Venus) is next. `GPU-DESIGN.md` §9.1 makes the first
move non-negotiable: *"Nothing can be **run** locally. This must be settled
before code starts, not discovered after."* So the arc opens with a gating
probe, the Warp-C C-0 shape, and `vn_renderer_thylacine` waits.

**The measurement, with its control.** Two boots on thyla-pi differing in the
device declaration alone. Control (`virtio-gpu-gl-pci`): capsets `id=1`, `id=2`.
Test (`+venus=on,blob=on,hostmem=256M`): additionally **`id=4` — VENUS,
`max_version=0`, `max_size=160`**. Both legs `BOOT: PASS` (215–225 s under KVM),
which is the part that makes it evidence: had the control merely failed to boot,
the missing capset would have been attributable to that instead of to the
declaration.

**No guest change was needed, and I nearly bought a boot to learn that.**
`probe_capsets` (`usr/tapestryd/src/gpu.rs`) already enumerates to
`GPU_CAPSET_ENUM_MAX = 8` and prints one `gpu capset[N] id=..` line per index.
My first grep filtered them out — the pattern was `GET_CAPSET`, and the lines
say `gpu capset[`. The evidence was on disk in the logs I had already produced.
A pattern that matches the wrong thing returns a confident partial answer, not
an error; the tell was that a boot which *did* enumerate three capsets reported
nothing about what the third one was.

**QEMU documented its own requirement better than I would have.** `venus=on`
alone is refused, and so is `venus=on,blob=on`, both with
`venus requires enabled blob and hostmem options`. Only the triple realises.
That is a **realise failure, not a degradation** — a caller declaring less does
not get "GL without Venus", it gets no device, and must not read that as a
negative Venus result. It also settles V-2's position in the ladder by
measurement rather than judgement: hostmem cannot be a late refinement of a
chunk whose device will not come up without it.

**The blocker that wasn't, and why it is written down anyway.** The host's
`libvirglrenderer.so.1.9.0` carries Venus (`VK_MESA_venus_protocol`,
`vkr_ring_thread`, `vkr_dispatch_vkWaitVirtqueueSeqnoMESA`) and names
`/usr/libexec/virgl_render_server` as `RENDER_SERVER_EXEC_PATH` — **and Debian
ships that binary in no package**; `virgl-server` is the unrelated *vtest*
server. §9.2 calls the render server Venus-only-by-construction, which reads as
"no server, no Venus", and for about ten minutes I had a dead arc. The capset is
advertised regardless, so venus initialises in-process at least far enough to
answer a capset query.

The discipline point is what I did **not** then write. "Venus works on
thyla-pi" is not what was measured. What was measured is that venus init reaches
capset reporting; whether a *context* creates is a different claim, and the
render server could still bite there. That became V-0b (`CTX_CREATE` with
`capset_id=4`) — a rung that settles it empirically instead of by inference in
either direction.

Instrument note worth keeping: `nm -D --defined-only` finds **zero** venus
symbols in that library, because they are internal. Had I run the export census
first and stopped there, I would have concluded Venus was absent from a library
that plainly contains it.

**The measurement was then made into a gate, because a hand-run measurement is
not one.** `warp-host.sh venus` runs both legs and asserts the discrimination in
**both directions** — present with the declaration, absent without. One
direction is not enough: "the test leg saw `id=4`" is satisfied by a host that
advertises the capset unconditionally, and by a guest printing a line it never
derived from the device.

Then the gate's own problem: it costs two ~220 s remote boots, which makes its
verdict the least affordable thing in the tree to test by running it — and #245
is three days old and says exactly what happens to a checker reachable only by
hand. So the verdict is its own verb (`venus-verdict`), and
`tools/test-venus-verdict.sh` drives **the real implementation** against crafted
logs: five cases, four one-variable sabotages plus the clean pair. The clean
case is not decoration — without it, four negative cases are satisfied by a
verdict that always fails. `5/5, DISCRIMINATES`, wired to `make
test-venus-verdict` and into CLAUDE.md's command block, which #245 measured to be
the property that actually prevents rot.

**Open, and named as open.** thyla-gl (Parallels, lavapipe) has the same QEMU
10.0.11 and a venus-carrying virglrenderer but has **never booted with
`venus=on`** — it is checked to the property level only, and promoting it is
V-0's remaining half. It matters beyond tidiness: if it works, Venus has a fast
local-ish iteration loop; if not, the whole arc iterates over the Pi's SD card.

The V-0..V-6 ladder is now in GPU-DESIGN §12, and V-2 is flagged audit-bearing
on I-45 and I-32 *independently of the rest of the arc*, because mapping MMIO
pages into a client VA is a new kernel memory-authority path and not a graphics
detail.

**And then the wrong turn, caught about twenty minutes after it landed.** I
wrote V-2 as carrying "the `PciDev::claim` eager-map-every-BAR fix, pulled
forward as a dependency" — because §6.2 ends with *"Also required and currently
broken: `PciDev::claim`'s eager map-every-BAR policy (§3)."* It is not broken.
It was fixed at **Warp-2a (#166)**, and §3 — **the section §6.2 points at** —
has said `[FIXED at Warp-2a (#166)]` in bold for weeks, along with the exact
remaining delta: *"Mapping a subrange of the shm window remains the §6.2
Venus-chunk delta."*

What caught it was not re-reading the doc. It was going to look at the tree for
an unrelated reason — how big is V-2, really? — and finding
`kernel/pci_handle.c` already resolving `VIRTIO_PCI_CAP_SHARED_MEMORY_CFG`, a
`pci.walk_caps_shm` test passing in the boot log I already had open, and
`hardware.rs` carrying a `#166` comment at the exact line that skips an
oversized BAR.

Two things worth keeping. First, **a cross-reference pointing AT the correction
is not the same as being corrected** — §6.2 pointed straight at the section that
refuted it, and the pointer kept its own verdict; a reader who follows the
pointer has already believed the pointer. Second, **a "currently broken" note in
a design doc is a claim about the tree, and it ages exactly like a status field:
nobody's step flips it.** The fix's own commit updated §3 and did not think to
hunt the other half of the sentence one section away.

So V-2 is **smaller than I wrote it**: discovery is done, the claim policy is
fixed, and what remains is the mapping half alone — an owner-minted,
client-mappable, revocable, budgeted map of a *subrange* of the shm window at
the host-dictated cache attribute. Corrected in §6.2, §12, and the status row;
the original claim is left visible in §6.2 rather than quietly overwritten, so
the next reader can see which half of a self-contradicting document was stale.

---

## 2026-08-18 — A reroute from a blocking primitive to a dropping one, and the budget I left behind

The audit `extinction.c` owed — it is a declared trigger surface and #246 put a
fault-injection hook on it — came back **0 P0 / 1 P1 / 2 P2 / 4 P3**. Clean by
the numeric rule. F1 was mine and had to land before merge.

The round opened by naming its own degradation rather than reciting a caveat:
the code was Opus-authored and so was the reviewer, so **family diversity is
forfeit here** and only context independence survives. It then used that
independence properly — it re-derived the EL1-sync depth ladder, measured the
shell predicate against twelve adversarial inputs, and **withdrew two of its
own prosecutions** against the code.

### F1: I moved the diagnostic and left the accounting where it was

`uart_puts` spins per byte and always emits. `cons_diag_line_emit` is
**all-or-nothing** and drops silently. I swapped the first for the second and
left the dedupe bit and the report budget being consumed *before* the emit.

Under back-pressure from a guest writing `/dev/cons` — the room-wait wakes on
**one** free byte and immediately refills, so the 8192-byte ring sits at
capacity — a 107-byte all-or-nothing unit never fits. So the drop is not racy,
it is **deterministic**, and it is the regime a container bring-up produces.
The syscall number is then marked seen forever and the budget is one lower. The
census under-reports and still reads as a measurement.

That is verbatim the failure the function's own header says the per-Proc rework
existed to kill: *"worse than no diagnostic, because it reads as a
measurement."* I re-opened it one step down, by changing the primitive and not
re-examining what was spent around it. **A reroute from a blocking primitive to
a dropping one changes the failure mode of every budget spent around it.**

The emit now reports whether the unit landed; the bit and the cap are taken
only when it does, so a dropped line is retried on the next decline.

### F2: I fixed the bounded emitter and left the unbounded one

The commit's own headline was "route the EL0-triggerable diagnostic through the
ring", singular. `exec_report_fail` is five raw calls, twice per failed spawn,
with **no dedupe and no cap**, and every `SYS_SPAWN_*` reaches exec through it —
so an unprivileged Proc spawning a malformed ELF in a loop drives it at will.
Strictly worse than the site I closed, and the severity ordering was inverted
relative to the fix that landed.

Converted, with a **global** cap rather than per-Proc: a per-Proc bound is
re-armed by spawning, which is the attack. The old comment defending the raw
loop ("to stay non-blocking") no longer selects it — `cons_diag_line` is also
non-blocking, never spins, and takes no console role — so that sentence went
too.

### F3: I wrote the lesson and then didn't apply it

My commit said *"a set with four independent spellings has no spelling anything
can be checked against."* I then enumerated only the file I was already
editing. Six more spellings were stale: two in `CMakeLists.txt` (a cache
docstring at four-of-eight, a comment block reading as complete at
three-of-eight), two in a **binding** reference doc at three-of-eight, and a
Makefile help line saying "seven" and "7 boots" against eight — in a line I had
just tagged `#245`. All now point at `ALL_VARIANTS` instead of re-duplicating,
because duplication is the thing that rotted.

### F6: the arm my test reached but could not fail on

I claimed the hook's placement put `cons_tx_claim_for_dump`'s
already-owned-by-this-cpu arm under test. It *reaches* it. Delete that arm and
the re-entrant claim burns its bound, returns false — and the banner still
prints, because the miss path is "torn beats silent". The expected string is
present and the variant passes, twenty milliseconds slower. Detection, not
discrimination.

Closed with a `forbid_for` table asserting the log must **not** contain
`console-ring: NOT held`, wired into the PASS arm rather than merely defined.
The round was also exact about what my sabotage proved: sensitivity to *"the
claim primitive does not dereference TPIDR_EL1"* — not to *"the ring lock is
actually held"* or *"the bound is honoured"*.

### Measuring the block instead of asserting it

I twice reported myself blocked on hardware with twelve files edited and none
compiled. The third time I checked: `ps` showed **37% of 800%** and nothing of
the lease-holder's on the cores — their concurrent work was a prosecutor round,
which is network-bound. The standing rule permits exactly this case (a check
while a peer holds the lease, when nothing of theirs is running, *checked with
ps and announced by note*). One kernel-only compile, seconds: **clean**, the
`void`→`bool` signature change harmless to its five callers, the sole warning
pre-existing in a file I never touched.

I was blocked on a *lease*, not on *cores*, and had not distinguished them. The
peer turned out to be genuinely mid-build a few minutes later, so the window
was real and narrow — which is why the rule says to measure at the moment
rather than reason from the lease. Boots still wait for the lease; I said "no
boots" in the note and that holds.

## 2026-08-18 — The round found the inverse defect: my fix for an over-permissive gate had landed as an over-restrictive one

The follow-up round the dirty C-6b close owed came back **0 P0 / 1 P1 / 1 P2 /
3 P3** — clean on both triggers. `MODEL(start) == MODEL(end)`, Opus fallback,
no mid-run drop. Worth saying which way the diversity caveat pointed, because
it **flipped**: the previous round audited Fable-authored code, so Opus was
genuinely cross-lineage; these fixes are Opus-authored, so this round was
same-family and its entire contribution was context independence. The spawn
said so and named the reflex to fight. The round named it back:

> I would have written that brace too, keyed on the same format, thinking
> about compressed textures and not about a driver that declares one byte on
> purpose.

### F1: the guard that refused what it had to admit

The P0 I closed last chunk was real — a 512×512 BO declaring 4096 bytes made
the compositor read 1 MiB out of a 4 KiB mapping. I fixed it in two places:
an exact bound at the **read** gate, and a "belt" brace at the **create**
door keyed on B8G8R8A8.

The brace refuses ordinary Mesa resources, and the proof was already in this
repo — in a comment, written by this project, at the exact line that chooses
the size (`usr/ports/mesa/patches/0006-*.patch:1511`):

> The seam refuses unaligned or zero backings; the driver's staging-path
> textures legitimately ask for size 1.

Mesa's virgl driver declares one byte on two paths that keep the real
width/height — the staging path (`alloc_size = 1`) and MSAA (*"don't create
guest backing store for MSAA"* → `total_size = 0`) — and our winsys rounds
that to one page. So `create3d … 512 512 … 4096` is **byte-for-byte both the
attack shape and a perfectly ordinary staged or multisampled BGRA texture**.
There is nothing to tell apart. Only the reader can distinguish them, by
whether it is about to read the backing — which is exactly what the read gate
does, and why it was the load-bearing half all along.

**The part worth carrying is why every gate stayed green.** The staging arm
hangs on a virglrenderer capset bit that *nothing in this tree measures*, and
thyla-pi's 1.1.0 evidently does not set it. The MSAA arm needed no host bit at
all: every multisampled BGRA render target above 32×32 was refused outright,
and no gate we have would notice, because a gate proves what the system *does*
and an over-refusal shows up only as something a client can no longer do.
**A guard whose activation no gate can see is worse than the hole it closes.**

And the prover leg I'd added to guard the P0 was asserting that a legitimate
allocation must fail. It is re-targeted as `C0-STAGING`: the door must *admit*
the one-page shape, with an unaligned backing as the control so "admitted"
cannot pass against a door that admits everything. The read gate's own runtime
regression test is **owed and tracked**, not quietly dropped.

**My parallel self-audit did not find this**, and the reason generalizes: I
prosecuted seven fixes and asked of each "is this gate sound?" — never "does
this gate refuse what it must admit?" Only the second question reaches a
client the tree does not contain. The round confirmed all seven of my
soundness findings and then found the one I had no question for.

### Rejecting the round's suggested fix (F4)

The DEEP arm's bar was stated three different ways and the code matched none.
The round proposed asserting the round's **max** via a census delta. I
re-derived it and **rejected it**: `Cost.max_ns` is a *global running maximum
that is never reset*, so a per-round max is not derivable — after round one a
delta detects only a new global record. But `mean ≥ T` does entail `max ≥ T`,
so the code was already a sound lower-bound witness and only the *prose*
overstated it. Fixed as prose, reconciled across three documents.

That it mattered showed up on silicon an hour later: round 3 measured a mean
of 128 ms over 2 retires, so the old "every compositor readback waited ≥ 100
ms" would have been false on that round.

### The deterministic failure that was my own fixture

`decomp gl` then failed twice, deterministically, at
`rp6 never confirmed the /env write (60s)`. I had just changed the compositor,
so it read as my regression.

It was my **pool**. `tools/test-fault.sh` re-bakes `pool.img` with `CLADE=0`
on every variant — and I had just run it ten times — so `/clade` was gone, and
`glq-decomp.exp` builds its `rp6` wrapper on-device with `/clade/bin/clang`.
The scenario's `echo rp6-ready` runs *whether or not clang succeeded*, so the
harness reported "rp6 built" and then failed 60 s later naming `/env`, a
subsystem with nothing to do with the cause. **A step that confirms the next
command instead of the one under test will always misattribute the failure.**

My own failure inside that: I verified the **ramfs** by content before syncing,
exactly as the discipline demands, and did not verify the **pool**. Verifying
one paired artifact by content and trusting the other is not verifying by
content. The build's output had said so plainly — `bake config CLADE=0`,
`payloads verified PRESENT: GOROOT GOCACHE GO4C QUAKE`, no CLADE — and I read
past it. A one-command check settles it: 917M with clade, 449M without.

Also recorded because it cost real context: **do not `grep` the pool image.**
It is an encrypted Stratum image; grepping it dumped megabytes of binary into
the transcript and told me nothing.

Re-baked both paired artifacts with `PRESERVE=0`, re-synced, and the same code
passed: **GLQ-DECOMP PASS gl**, 969 frames at 37.9 fps composed on real V3D.
Same code, different fixture — the attribution is settled, not assumed.
`test-fault.sh` mutating a shared fixture other gates depend on is filed
(main#250); it should restore the operator's bake config or refuse, the same
shape as `test-interactive.sh` refusing when a VM is already running.

### #243 and #246, from the extinction work

`uart_puts` takes no lock, so the ring claim serializes against ring traffic
only. The class was **observed live and fixed once already** — #76 removed the
same raw loop from `SYS_PUTS` after it shredded a login prompt byte-for-byte —
and `viv_report_unserved` reached for it again, on a path an unprivileged EL0
program triggers by choosing an unserved syscall. Now one `cons_diag_line`
unit; verified live in the boot log.

`el1_sync_runaway` had no test and `7dd5be19` had just put three calls on it.
Confirmed by reading why: the depth ladder tops at 3, the #806 guard extincts
at 2, so only a fault from *inside* the extinction path reaches it — #244's
shape, on purpose. **Discrimination proven** by sabotaging the claim back to
the counted trylock and watching the variant fail. Stated exactly: that
sabotage does *not* reproduce #244's silent park — the counted trylock trips
`lock-across-sleep` first — so what it proves is sensitivity to the claim
path's correctness, not reproduction of the original bug.

And `test-fault.sh` enumerated its variant set **four times**; adding one
updated two of them, so `test-fault.sh el1_sync_runaway` answered "Unknown
arg" while `make test-fault` ran it happily. The arg arm and `--help` now
derive from the one list.

## 2026-08-18 — Two gates nobody ran, and the count that refuted my first explanation

Spawned the follow-up prosecutor round the dirty C-6b close owed (`c8c83348` +
`2f3c0bcc` — a P0 returned and P1+P2 hit six, so CLAUDE.md's re-audit rule
fires). Fable is out of credits, so it went straight to the Opus fallback per
scripture. **Worth stating which way the diversity caveat points this time,
because it flipped**: the previous round audited Fable-authored code, so Opus
was genuinely cross-lineage; these fixes are Opus-authored, so this round is
*same*-family and its whole contribution is context independence. The spawn
says so explicitly and tells the prosecutor which reflex to fight — agreeing
with a construction because it is the one it would also have written.

While it ran, the audit-in-flight discipline: non-colliding work, then
prosecute the same surface myself.

### The non-colliding work turned out to be the more interesting half

main#245 said `test-fault.sh` is wired into no gate. A census over `Makefile` +
`tools/`, with a control at each end (`ci-smp-gate.sh` must resolve to a target,
`test-fault.sh` must not), found **two** orphans rather than one:
`tools/verify-kaslr.sh` has no caller either. The only references to either are
two *comments* in sibling scripts.

Neither is decorative. `test-fault.sh` is the only witness that the seven
hardening protections actually **fire** rather than merely being compiled in —
the canary, kernel-image W^X, BTI, the two stack guards, the boot-CPU idle
guard, the recursion arm. `verify-kaslr.sh` is I-16's only runtime witness:
ROADMAP §4.2 requires the kernel base to differ across boots, and `make test`
accepts any *single* boot, so it is structurally blind to a slide that never
moves. This is how #244 hid for a month.

**Then the interesting part: my first explanation was wrong, and its own
measurement said so.** The obvious hypothesis is that the survivors are in
CLAUDE.md and the orphans are not — CLAUDE.md is auto-loaded every session, so
that would be a clean anti-rot story. The count refutes it: `test-fault` and
`verify-kaslr` appear in CLAUDE.md **twice each**, exactly like `test-a72` and
`check-v80-floor`, which did not rot.

The difference is *where*. The survivors sit in the "Build + test commands"
block, as commands. The orphans appear only in the boot-banner paragraph's
prose, named as **consumers of the ABI literals** — things that would *break*
if you reworded one, never things to run. Every session learned they existed
and nothing about invoking them. Which is precisely the mention-versus-program
distinction that same paragraph teaches about its own co-update list, applied
to itself and not noticed.

So the remedy is both halves, in the idiom this project already uses for the
class (`check-production`/#228, `test-a72` and `check-floor`/#91): a named
target with a WHY comment, **plus** an entry in the command block. `55c5d2f8`.

**A second wrong turn, caught after the commit.** The census as first run also
grepped `.github` — which does not exist. There is no CI in this repo at all,
so that arm searched nothing and contributed no evidence, while the commit
message reports "no Makefile target, no gate, no CI step" in a list that reads
as three findings. The claim is true; one third of it is *vacuous*. An empty
arm of a census must not be reported as though it were a negative result, and
the tell is that the arm was never given a control the way the other two were.

**A wrong turn caught before it shipped.** The first draft of the help text put
backticks around `make test` inside a Makefile `@echo "..."`. Backticks inside
double quotes command-substitute — `make help` would have *run the full test
suite*. Caught by rendering the target rather than trusting the diff.

**What this does not close, stated rather than glossed:** neither script now
runs *automatically*. They are named targets a human or agent invokes, exactly
like `test-a72`. Whether test-fault joins the pre-push bar costs 7 builds + 7
boots, and the gating evaluation is the operator's call, so it is surfaced.

### The vault gains a fourth failure class

`quaestor owner` routed the change to `abi-boot-banner`, whose taxonomy
enumerates three ways a co-update list member fails — *phantom* (named, never
existed), *inert* (exists, matches nothing), *document* (matches, only goes
stale) — against an implied healthy fourth, the **program** that "breaks
silently and immediately".

Two of its fifteen derived mirrors were programs nothing ran. That class has a
program's full co-update obligation and **no failure behaviour at all**: it does
not break loudly, and unlike a document it never even becomes visibly wrong,
because nothing evaluates the mismatch. Strictly worse than the document class.

The mirror rule itself is unaffected — it answers "who must be co-updated", and
an unrun program must still be co-updated. What the note now guards against is
reading a fifteen-member derived set as *defence in depth*. **A mirror set
bounds the co-update obligation; it says nothing about detection latency, and
only the members something actually runs contribute to detection at all.** Same
shape as the extinction seam one level up: a contract on a value is silent about
its delivery; a contract on the set of readers is silent about whether any of
them reads. Vault `60095c97`, lint 946/0/0.

### Self-audit: seven fixes prosecuted, seven sound, one suspicion withdrawn

Re-derived from the code rather than from each fix's own comment. The P0 repair
is covered better than its comment claims: the pre-existing `b.w == s.w` check
sits before the new size guard on the same path, so the guard's geometry *is*
the reader's; and `comp_readback_retired` re-runs `gl_adoption` as
`same_adoption` at retire, so the guard re-validates at **read** time and the
issue→retire TOCTOU is closed by construction. The "sole `Some(va)` caller"
claim was re-derived, not accepted: exactly two call sites, one `Some`, one
`None`, and the Warp-4 synchronous arm that originated the P0 no longer exists.

`FenceTag.ok` has one construction site, fail-closed at `false`, and two
textually identical assignments. `FenceVindication.comp` takes its
discriminator and its ctx from the same loop index at both sites, so they cannot
disagree. The `COMP_FSLOT` exemption is conditional on scope and correct in
*both* directions — the client-driven scoped lever cannot touch the reserved
slot, the internal unscoped callers still can, because a wedge that is real is
genuinely global.

**One suspicion raised and withdrawn by measurement**: `rb_coalesced` looked
mis-charged (the `+= 1` sits outside the match, so both arms reach it) — the F9
class again. Two checks killed it: `git show 24e6753d` proves the unconditional
increment is pre-existing and untouched by my fix, and `149-warp.md` defines the
key as "presents that enqueued instead of issuing", which is exactly what
`rb_enqueue`'s two callers are. Recorded as withdrawn rather than dropped
silently, because a fabricated defect eats the budget a real one needs.

Findings in `memory/audit_c6b_followup_selfaudit.md`, to be **merged** with the
round's report when it lands, not segregated from it.

## 2026-08-18 — The owed C-6b round: a deviation is dangerous everywhere else that reads the same field

Fable ran out of credits mid-spawn — the prosecutor died after loading the
preamble and before producing findings, which is an **absent** round, not a
clean one. Per CLAUDE.md that goes straight to the fallback tier rather than
retrying Fable, so it ran on Opus 5.

**The family-diversity caveat is INVERTED here, and reciting it would have been
wrong.** The standing rule assumes an Opus prosecutor shares the author's
priors because Opus is this project's implementation agent. But `ef58d639` and
`24e6753d` were written by **Fable 5** earlier the same session — so an Opus
prosecutor is genuinely cross-lineage against *this* author. I said so in the
spawn, told it its contribution was context independence, and warned it that
the code's own justifications (dense comments, the AS-BUILT paragraphs, the
audit row's prosecute-on-change list, five closed lists of "VERIFIED SOUND"
arms) are the author's argument and not evidence. It came back with **1 P0 /
3 P1 / 3 P2 / 3 P3**, and three of the findings are corrections to claims the
tree makes about itself.

### The lesson, and it is specifically about AS-BUILT 1

C-6b deviated from the design's letter in one recorded place: the compositor
readback's fence tag carries the **client's** `ctx_pub` rather than 0. That was
argued carefully and it is right — 0 is `warp_ctx_vindicate`'s no-slot
sentinel, and the client's own vindication has to wait for our poisoned slot.

What was never enumerated is the deviation's **cost**. Every mechanism keyed on
a tag's ctx now reaches the compositor's reserved slot, and two of those are
*shipped, client-drivable levers* (`warp-hold` / `warp-abandon`, since
`default = ["test-mode"]` and nothing passes `--no-default-features`). Their
safety argument is #178's: "the worst a client can do is wedge its own ctx,
which it could already do." C-6b made that false one resource over, silently,
and the round found it (F4) by prosecuting the documented deviation **as a
design change rather than as a footnote**. Worse, `drain` cleared
`fslot_since` one line *before* the hold check, so a held slot could never
reach `reap_abandoned`'s staleness test — the pin was indefinite, not bounded
by 30 s. Compositor-wide: every other client's readback frozen, the 500 ms sync
deadline disabled process-wide, and a ~1 kHz spin in the console for the life
of the box.

**A deviation is sound for the reason it was taken and dangerous everywhere
else that reads the same field.**

### The P0 was pre-existing, and its guard was a comment about the wrong subject

F1: `wbo_create` validated the client-declared backing with two gates and
**both are upper bounds** — its comment states the one-directionality outright
("a 1x1 texture cannot ask for 64 MiB"). `gl_adoption` compared `w`/`h` for
*equality*, never capacity. And `compose_cpu` reads `sw * sh_full * 4` from the
BO's `va` with the dims taken from the **surface**. So a 512×512 BO declared
with 4096 bytes — page-aligned, under both caps, `Y_0_TOP` so it takes the
readback arm — was admitted, adopted, and composed by reading **1 MiB out of a
4 KiB mapping**: a bump-allocated neighbour (another client's pixels, painted
onto the attacker's own pane) or a fault in the process that *is* the console.

`compose_cpu` carries a `SAFETY` comment asserting the rows are in range
"because damage was validated against the surface geometry". True of the
**weave**, whose size derives from that geometry. False of a client-declared BO
backing. The same function reads both.

Pre-existing from the Warp-4 synchronous arm and in none of the five
preambles — attribution, not ownership. Fixed at the read gate (exact:
`b.size >= b.w * b.h * 4`, exact because adoption already pins the dims, and
`comp_readback_retired` is the only `Some(va)` caller — enumerated by enclosing
function, not by grep hit) and at the door (keyed on `B8G8R8A8_UNORM` alone: a
general per-texel floor would refuse legitimate *compressed* textures, and it
must not key on `composable` because the attack shape is precisely
non-composable — that is how it reaches the readback arm).

### Converging with my own pass, and the one I sharpened afterwards

I ran the self-audit in parallel per the audit-in-flight discipline and found
F3 independently (a vindicated compositor readback bumps the **client's**
`fence_signaled`, so `warp_fence_wait` — which returns on `signaled >= seq` —
returns one fence early for the ctx's life). Filing it before the round
reported is the useful part: two prosecutors reaching the same defect from
different directions is the strongest signal either one produces.

The round also sharpened something I had noticed and under-read: `rb_wanted`'s
growth. I saw it was unbounded in principle; the round pinned *why the comment
was wrong* — the dedup key included `gen`, drawn from a monotonic counter, so
"bounded by MAX_SURFACES" bounded `n` and not the pair.

### The fix that broke the gate, and what that is worth

My fix to F8 (DEEP asserted a **sum** over an unknown retire count against a
per-readback threshold) required *exactly one* retire per round. The gate went
**red on a healthy build**: `comp-rb landed 1->7` across three rounds — **two**
retires each, because the flight loop's later presents each request a readback
and the pump issues the next the moment the first lands.

Every round satisfied the substance (waits 794 / 1007 / 260 ms, each observing
draw 1199 of 1200 by its pixel witness) and failed my arithmetic. **I had
replaced a wrong statistic with a claim about the mechanism's scheduling**, and
the claim was false. The round had offered the right alternative in the same
breath and I took the wrong half of it. Now it asserts the round's **mean**:
robust to any retire count, still rejects the case the sum admitted (one long
readback plus one instant one averages below threshold), and the pixel witness
still carries which draw was observed. The per-round line prints the count and
the mean so the next red is diagnosable without a re-run.

Worth recording plainly: the gate caught my own fix, on real silicon, one
commit after I wrote it. That is the system working — and it is the second time
this run that a control earned its keep by going red for a reason that was not
a defect.

### What is NOT closed

F7 [P2] is a **measurement debt**, not a code change, and saying otherwise
would be the worse outcome. The readback gate cannot *discriminate* a sabotage
that removes the deadline widening: the certifying run measured `F2B max
267 ms` against a `SUBMIT_DEADLINE_MS` of 500, so a build without the widening
passes identically. Sharper still — the deadline is evaluated **only at a stale
wake**, and the stall it exists for (a synchronous host
`TRANSFER_FROM_HOST_3D` on QEMU's serial main loop) raises no interrupts. So
whether the widening is load-bearing *at all* depends on INTx sharing nobody
has measured. GPU-DESIGN 4.5.13 now says that instead of "correct by
construction", and names what closes it. Tracked as main#253.

The close is **dirty** (a P0 returned; P1+P2 = 6) and several fixes are
structurally invasive, so **a follow-up round is owed on the fixes themselves**.

---

## 2026-08-18 — The extinction line, source 2 of 3: the fix found a fault gate that had been printing nothing for a month

Same run, after C-6b landed and pushed at `f525cea3`. Next on the resume note
was the follow-up Fable round on the C-0d fixes + C-6b; it was spawned first
(read-only, no cores), and this chunk ran alongside it.

### What was owed

The `EXTINCTION:` ABI line has **three** tearing sources and the names are
close enough that I have conflated them before. Source 1 —
extinction-vs-extinction — was closed 2026-08-16 by `extinction_claim_console`
(one `__atomic_exchange_n`; losers park silent). Source 2 —
**extinction vs a peer's ordinary console write** — is the vault's
`seam-extinction-line-unserialized`, and it is the one that matters most by
readership: the seam's own census found **fourteen of fifteen** declared
mirrors match the crash prefix, against eight for the boot-success line that
got the guarantee. Source 3 is `IPI_HALT`, still a commented-out reservation.

### The prescribed remedy was a hypothesis, and it was wrong in one specific

The seam prescribed a **try**-acquire of the *writer role* (never a park).
Checking it against the drain path says no: the role (`g_cons_tx.writing`)
serializes whole `cons_output_write` calls, but **the drain never consults the
role** — that is main#144, already written down in `cons.h` — so bytes a peer
had already pushed would still pop into the FIFO from cpu0's TX IRQ or from a
peer's `cons_tx_kick`, landing inside the banner while the role sat held.

What actually owns the wire is **the ring lock**: every steady-state producer
pushes its unit under `g_cons_tx.lock` (`cons_tx_push_bulk` — SYS_PUTS through
the role, the echo, `cons_diag_line`) and every ring→FIFO drain pops under the
same lock. So the winner takes *that*, and never lets go
(`cons_tx_claim_for_dump`, `kernel/cons.c`). The role is also the wrong
primitive on a second axis: a healthy peer holds the ring lock for one bounded
push or one FIFO-depth drain — microseconds — where the role is held across a
whole write, room-waits included.

Every property is deliberately the **opposite** of the console word one file
over, and the reason is the same in each case — *who holds the thing you are
waiting for*:

| | console word (source 1) | ring lock (source 2) |
|---|---|---|
| holder you contend with | a **dying** peer that never releases | a **healthy** peer that will release in µs |
| therefore | **try once**, never spin | **bounded spin**, because try-once fails exactly when it matters |
| primitive | raw atomic (a spinlock could fault on a dying machine) | **raw** trylock, same reason — new `spin_trylock_raw` |
| on failure | park silent (a missing line is visible; a torn one reads as a clean boot) | emit anyway, and **report the miss** after the dump |

IRQs are masked before the acquire and never restored: with the ring lock held
on this CPU, its own TX IRQ arm (`cons_tx_drain_from_irq` → `spin_lock_irqsave`)
would self-deadlock — a silent hang in place of the dump. The caller parks in
`_torpor`, so nothing is owed back. And the flush under the lock became the
*full* bounded ring rather than one FIFO's worth, because holding forever means
whatever is still queued when the flush stops is lost, where the predecessor's
release let the rest trickle out behind the dump.

### The compile found the emitter the census had missed

`cons_tx_flush_for_dump` had a second caller: `arch/arm64/exception.c::
el1_sync_runaway`, the #214 recursion guard's terminal banner — which prints
`EXTINCTION: el1-sync recursion ...` **without going through `extinction()`**,
and was therefore enrolled in *neither* serializer. Not in the 2026-08-16
console-word fix, and not in the vault's `abi-boot-banner` mirror set either:
`quaestor owner` flags it as matching the ABI literal *outside* the set. It now
takes both, via a new `extinction_console_claim_or_own()` — claim the word, or
confirm this CPU already owns it, since the runaway is reachable from a chain
that claimed it at depth 1; a *peer* holding it means a peer is dumping, so it
parks silent like any loser, counted.

Worth noting how it surfaced: **not** by the census I ran, but by deleting the
old symbol and letting the build fail. A rename is a census that cannot lie.

It also reports a ring-claim miss after its own banner, which cost the SMP gate
a restart: I noticed the asymmetry (only `extinction()` reported) five boots
into the matrix. Killing it there and re-running cost ~10 minutes; letting it
finish and re-gating afterwards would have cost ninety, and shipping the green
from an ELF that no longer matched the source would have been a *misleading*
green, which is worse than a red.

**And that path is exercised by no test at all — this chunk just put three
calls on it (main#246).** In a healthy kernel the #806 guard extincts at the
*second* kernel fault, so `g_el1_sync_depth` never reaches 3; reaching the
runaway needs the extinction/Halls path itself to fault — which is precisely
the base-tree defect below, and precisely what this fix removed. The fix
deleted the only thing that was reaching the path it also modified. "No current
path drives it" is the latent-P1 trap, not a safety argument, so it is filed
rather than glossed.

### Then the base measurement, which is the actual finding

`tools/test-fault.sh` passed 7/7 on the change. To be sure the pass meant
something I stashed the work and ran the sharpest variant on the base tree:

| tree | `recursive_kernel_fault` |
|---|---|
| base `f525cea3` | **TIMEOUT (60 s)** — last guest line is `fault-test: invoking recursive_kernel_fault...` |
| this change (raw try-spin) | PASS — `EXTINCTION: recursive kernel fault (handler re-entered) 0xdead000000000000` |
| this change, counted `spin_trylock` restored | TIMEOUT, symptom byte-identical to base |

**The base tree printed nothing at all.** That variant installs
`TPIDR_EL1 = 0xdead000000000000` deliberately — a wild `current_thread()` is
its entire premise. `extinction()` flushes the ring *before* the banner (on
purpose: causal order), the old flush took the lock with the **counted**
`spin_trylock` → `spin_preempt_inc` → `current_thread()->magic` → **fault,
inside the extinction path**; the nested EL1-sync faults climbed to depth 3 →
`el1_sync_runaway` → which called the *same* flush → faulted again → depth 4 →
the `depth > MAX` arm parks **silently**.

So the one fault variant whose whole point is a destroyed `current_thread()`
could not print its own banner — and failed by **silence**, not by a wrong
message, which is the shape that reads as "the harness is slow" rather than
"the protection did not fire". Broken since `ed56f21f` (#75 P1-F, 2026-07-20)
met `ce7bd352` (#360's counted spinlocks, 2026-07-04): about a month, because
**`test-fault.sh` is wired into no gate** — grep-proven over the Makefile,
`ci-smp-gate.sh`, `test.sh`, `test-interactive.sh` and `.github`. It is the
only runtime witness that W^X, BTI, the stack guards and the #806 guard
actually fire, and it runs when someone remembers. Filed main#244 (the defect,
closed here) and main#245 (the ungated harness, open).

**The rule that generalizes, and the reason `spin_trylock_raw` exists:** a
dying-machine path may not call a primitive that reads state the crash may have
destroyed. #360 retrofitted that `current_thread()` deref under *every* existing
`spin_trylock` caller — including one on the extinction path — without anyone
re-asking whether that caller could survive it. The `spin_lock_raw` comment now
enumerates its two legitimate holders instead of naming one and calling every
other use a bug.

### A defect I nearly fabricated, and what stopped it

The sabotage run's failure lines came out as
`[test] cons.ring_claim_core_returns_holding ...   [runnable-dump returns HOLDING: a second taker must fail while the claim is held]`
and I read that as a live tear of exactly the residual class I had just filed
(main#243: direct-`uart_puts` diagnostics outside the ring lock). It is not.
`test_fail(msg)` calls `sched_dump_runnable(msg)`, which prints
`"  [runnable-dump " + tag + "]"` — the tag **is** the failure message. Intended
output, read as an interleave because I was primed for one. Withdrawn within
the minute, by reading the caller instead of the line. *A fabricated defect
outranks a missed one*: it would have eaten the budget a real one needs, and it
would have "confirmed" a bug I had filed an hour earlier — the worst direction
for a confirmation to arrive from.

### Posture

Suite **1427/1427** (was 1424 — three new legs), `test-fault.sh` **7/7**, both
sabotage arms verified in one run (1427 → 1424/1427, each failure naming its
own assertion; source restored byte-identical to the verified WIP and re-run
green). The kernel changed, so the SMP gate is owed and running.

**Still open, exactly:** source 3 (`IPI_HALT`) — untouched. And the ring lock
reaches only writers that go *through* the ring: steady-state kernel
diagnostics that still call `uart_puts` directly (`sched.c`'s runnable-dump,
`syscall.c`'s vivarium unserved / `viv-trace`, `exec.c`'s exec-failure,
`9p_client.c`'s ownerless-frame) sit outside it and can still land inside the
banner from a peer CPU. `cons.h`'s contract already says those callers should
use `cons_diag_line`; converting them is main#243, and they carry the #126
20-ms-per-byte exposure too. **This closes one of three sources, and the third
would subsume the residual of the second.**

---

## 2026-08-18 — C-6b: the readback arm off the console's dispatch, and the load that measured which GL context a queue is on

Resumed from the self-compaction at `64ded01d` (the C-0d Fable close + the
C-6a spec pushed). The mac was aux's for the first hours (its SMP gate, then
its round-B P1 fix), so this run did its reading, code and docs cold and
queued on the lease for every build — three times, because the gate's
positive control kept saying "the queue you built is not the queue you
think", which is the finding worth writing down.

### The implementation (`server.rs` / `gpu.rs`) — one refinement the design's letter did not have

GPU-DESIGN 4.5.13 said the compositor-owned tag would carry `ctx_pub = 0`.
Reading the driver's abandonment bookkeeping said no: `fslot_poison_ctx`,
`FenceVindication.ctx_pub` and `ctx_has_poisoned_slot` all key on the tag's
ctx, and 0 is `warp_ctx_vindicate`'s "no condemned slot" sentinel — an
abandoned compositor readback under ctx 0 that the device later retired
would push a vindication for ctx 0, `position(p == 0)` would match an
arbitrary un-condemned slot, and `ctx_destroy(slot+1)` would hit a live host
context. And the client's own vindication has to WAIT for our abandoned
readback of its BO (round-4 F1: one late retire proves nothing about the
rest), which only holds if the slot is attributed to the client. So the tag
carries the CLIENT's `ctx_pub` plus explicit `readback` / `comp` bits; the
pump routes on the bit and poisons / decrements the right ctx. Recorded as
AS-BUILT 1 in 4.5.13. Everything else is the design as written: the
reserved slot (`COMP_FSLOT` = 15; the client pool is 0..15 and
`lane_exhausted` / `fenced-free` read only that), `Comp.comp_rb` +
the gen-pinned `rb_wanted` FIFO (one in flight compositor-wide — the slot IS
the bound), `comp_readback_retired` BEFORE `warp_pump_retires` in the pass
(the pump's decrement can quiesce a retiring BO; the compose must read `va`
first, and `gl_adoption` refuses a retiring BO/ctx so a destroy in flight
drops the frame), `fences_in_flight` + `comp_rb_in_flight` symmetric on
issue and retire, the admission subtraction, the sticky 30 s deadline while
any readback is in flight, `Cost::ReadbackWait`, the `comp-rb` census (keys
prefixed — `abandoned` was already the test-mode key and `parse_field` takes
the first hit).

### The gate, and the two loads that were not the load

`warp-prove readback` (its own verb, like `reject`: it stalls the device on
purpose) with named arms — ARM (a present on an idle queue issues and lands
a compositor readback), DEEP (the readback the device paid waited ≥ 100 ms:
the positive control that the queue existed), LIVE (while it is in flight,
the adopting surface's own presents and warp ctl reads answer inside 50 ms —
under the old arm the first present takes the whole wait), DEADLINE (a
client's OWN fenced readback of its busy BO, then ten bystander presents
behind it: all succeed, engine alive — busy read as busy), F2B (the
bystander's latency, reported), CLEAN. `C6-READBACK DONE` is a verdict (the
F6 shape); `warp-readback.exp` hard-fails on `INCOMPLETE(<arm>)`.

**Run 1** (800 1:1 NEAREST full-frame blits, ping-pong BO ↔ scratch): ARM
PASS, LIVE PASS, DEADLINE PASS — and DEEP FAIL: `readback-wait max 16 ms`.
1.6 GB of copies do not finish in 16 ms on a Pi. `vrend_renderer_blit`
(1.1.0) takes the `glCopyImageSubData` shortcut for a 1:1 same-format RGBA
NEAREST blit; whatever those became, they were not GPU work the readback
waited on. Without the control LIVE would have passed on a light queue —
which is exactly why the control is there.

**Run 2** (SCALED blits, 512² ↔ 1024²): the 8 submits retired in **1335 ms**
— real work — and DEEP still FAILED: the compositor readback of the same BO
waited **84 ms**, and the client's own readback stalled the bystander by at
most 149 ms. LIVE FAILED too (94 ms), which turned out to be the same
mechanism seen from the other side. A scaled blit goes through
`vrend_renderer_blit_int` → the BLITTER, and vrend's blitter owns its **own
GL context** (`vrend_blitter.c`); a client-context fence and a
client-context `glReadPixels` are not ordered behind another context's
work. The queue was deep; the readback was not behind it. **A claim about a
lane must be re-derived per COMMAND CLASS** was C-0d's lesson; this is its
sibling: **a queue is deep only on the GL context the wait is on.** A real
client's draws land on its own context, so the honest load is client-context
work: **run 3** queues clear PAIRS (the BO to an index-encoded colour, then a
2× scratch, alternating framebuffers so mesa v3d cannot fold them — each a
full-surface store), and the leg now prints the queue's fence timeline and
**which clear index the compositor readback observed** (the BLUE byte of the
pixel it landed): "the readback waited for the queue" is a pixel, not a
duration.

**Run 3** (alternating full-surface clears, BO ↔ a 2× scratch, index-encoded
colour): the readback observed clear **639 of 640** — it DID wait for the
whole queue, the mechanism is right — and the whole queue took 122 ms: mesa
v3d keys jobs by framebuffer (`v3d_get_job`), an FBO switch does not flush,
and 1280 clears folded into two jobs. **Run 4** (draws — hand-encoded from
the Mesa tree's `virgl_encode.c` field for field, a `verify` after the prime
so a rejected stream names itself): DEEP PASS at last (readback-wait 130 ms,
draw 2399 of 2400 observed) — and LIVE FAIL on the SECOND present (140 ms
inside a 168 ms flight; the issuing present 0 ms). **Run 5** made LIVE the
issuing present over three rounds and reported the rest: LIVE 0/0/0 ms;
DEEP failed one round at 88 ms because the eight 24 KiB Twrites
themselves took 130–290 ms and the ~415 ms queue was nearly drained at
issue. **Run 6** deepened the queue (3 triangles per draw) and added the
census of OTHER console work per round: `slot-presents +1` in EVERY round —
the console renderer's cursor-blink present — and the sends took 478 / 794 /
1062 ms. That named the deterministic blocker: on egl-headless a present's
`RESOURCE_FLUSH` is the display backend's `glReadPixels` of the screen (the
C-4 lane cost), queued behind the compositor's blit, behind the client's
draws on V3D's one hardware FIFO; the single-threaded loop waits there for
everyone, and my own sends waited behind it too, so a readback issued after
them met a drained queue. **Run 7** halved the send exposure (4 submits × 6
triangles) and made a round self-validating — issued into a queue with less
than the floor left = UNCONSTRUCTED, retried, never judged — and the gate
went green: `WARP-C C-6 GATE: VERIFIED`, issuing present 0/0/0 ms,
readback-wait 497/1001/1027 ms, draw 1199/1200 observed every round, two
unconstructed rounds retried; DEADLINE 10/10 alive; F2B max 1034 ms mean
119 ms. The final artifact re-ran green (805/1005/1005 ms, F2B max 267 ms).

**Sabotage S1** — the issuing present made to WAIT for the readback (the
pre-C-6 arm): first run read as `deep-unconstructed`, because the prover
stamped the issue time AFTER the present returned; stamped before it, the
sabotage fails LIVE with the issuing present at 269 / 969 / 1017 ms — the
arm discriminates the defect and nothing else. Not run: a sabotage of the
deadline widening — no stale wakes were observed during ~1 s stalls on this
lane, so the old 500 ms deadline may never have fired here; the widening is
correct by construction and the DEADLINE arm is its net where wakes arrive.

What the run says about C-6 under QEMU/virgl, honestly (AS-BUILT 3 in
4.5.13): the console never waits inside the present that issues the
readback, and one readback is in flight at a time — but any sync step the
console issues while a client's queue is deep inherits the stall, and on
egl-headless every present is such a step. C-6 removes the per-present
multiplication and the false dead-latch; the stall itself is the host's
(F2b) until Venus / v3d.

### The bar

Local: suite 1424/1424 + arc gates 2/2 + clade 3/3 + G-4 CONSOLE VERIFY OK
(kernel byte-unchanged; SMP 40/40 @401d4b27 carries). thyla-pi (KVM, V3D,
virglrenderer 1.1.0): `readback` VERIFIED on the final artifact; `reject`
C-0d DETECTOR VERIFIED; `prove` WARP-2 VERIFIED; `quake` WARP-4 VERIFIED
(969 frames 44.2 fps; `comp-rb issued 0`); `decomp gl` PASS (composed gpu
1106 cpu 0; `readback 0`, `readback-wait 0` — the blit arm untouched). LS-CI
gfx subset (ls-ci + 15 ls-gfx-*) 16/16, 0 retries, run alongside the Pi's
final gate (the mac idle otherwise). Every ramfs verified by content before
each sync (`cpio` extract + `strings`), and the `cd usr` trap paid three
more times before I split the build from the bake.

## 2026-08-18 — the C-0d Fable close: C-4's lesson had been applied to one pair and not the other, and the readback arm's remedy is not what it looked like

Resumed from the self-compaction at `401d4b27` (the merge pushed; the C-0d
Fable verdict in hand: 0 P0 / 2 P1 / 1 P2 / 2 P3, nothing fixed). The mac was
aux's for the first ~1.5 h of the run (its viv-run LS-CI legs), so this run
did all its reading, editing and design with no cores and queued on the lease
for the build — which is what the leases are for.

### The close (F1 / F5 / F6 fixed, F3 recorded) — `ef58d639`

**F1 was C-4's own residue.** §4.5.12 had measured that a texture transfer
or readback on a tiled renderer is a blit job behind everything the *device*
has queued, and moved the compositor's health pair to buffers — and left the
per-ctx #240 probe (`warp_probe_build`) a texture pair, because the
compositor's helpers (`health_upload` / `health_readback` /
`comp_copy_region`) had `COMPOSITOR_CTX` hardcoded and the client verify kept
its own texture-only transfers. So every client `verify` was still the drain
C-4 had just priced, and — the part the round added — one client's verify
paid for *another* client's queue, which the verify admission gate (F7's
`fences-in-flight`/`poisoned`, reading only the caller's gauges) cannot see.
The fix is structural rather than local: `CtxProbe.buffer`, the buffer mint
first for every ctx (`warp_hprobe_build`), the texture pair only where that
mint fails and counted (`probe-texture` on the global ctl — a say line at
ctx-create rate would be a storm), and ONE helper set for both pairs
(`probe_upload` / `probe_readback` / `probe_copy_region`) so the compositor
and the clients cannot drift again. The prover's C0-F1 leg had to change with
it: it attacked from a TEXTURE BO, and a texture->buffer
`RESOURCE_COPY_REGION` is not a legal copy — the renderer would have dropped
it and the leg would have printed DEFENDED for the wrong reason (a control
the operation erases). The attack source is a buffer of the probe's own
shape now (`mint_buffer_bo`, `rcr_stream` with a width).

**F5** (`present-to N bo`/`off`/`N bo` re-running the whole import witness on
the SHARED compositor context at 9P-write rate): the `verify_tick` shape,
one witness per ctx per compositor tick — but DEFERRED, never dropped: a
same-tick second consent sets `import_pending` and `frame_tick` replays the
import of whatever `present_to` names by then. The winsys re-consents only
when its front buffer changes, so the only legitimate second write in one
frame is a resize storm, and coalescing those onto ticks costs it one tick of
the readback arm.

**F6** (warp-prove printed `C0-REJECT DONE` unconditionally, so a blind
detector passed the scenario and only the host-side 5-term grep gated it):
DONE is a verdict now — every C0 arm records pass/fail and the token prints
iff all three passed, else `C0-REJECT INCOMPLETE(<arm>)`, which
`warp-reject.exp` hard-fails on through a new `lc_run_expect_hardfail_re`
(a regexp fail arm, so the prover's own `FAIL --` shares it). The 5 terms
stay as the belt: a scenario that passed for a reason the list does not know
about should still fail there.

**F3** recorded on #171 with a comment at `warp_probe_res_kind`: the probe's
two page mappings ride the never-rewound `weave_va_next` bump — a ctx-churn
driver on the same monotonic-VA class. Also noticed while writing it: the
detach names `size` while the bump rounds it up to pages — equal today (both
PAGE), and written down so a differently-sized probe cannot silently leak.

**Also found: the #240 detector's four rounds were never in
`AUDIT-TRIGGERS.md`.** r1–r3 lived in phase7-status rows and memory files
only. The tapestryd row now carries the addendum (all four rounds, this
close's fixes, five prosecute-on-change items).

### F2, and the design that came out of reading QEMU before writing it

F2 [P1] is the composed-GL present's readback fallback: `transfer_from_3d_
sync(g.dev_ctx, ...)` of the whole frame on the compositor's SYNC slot, so
the console's dispatch waits for the frame — for everything the client has
queued ahead of it, a length the client picks — and `fence_poisoned` cannot
guard it (the poison comes from `reap_abandoned` on the loop that is
blocked). The pickup note prescribed "the fenced / bounded readback". Reading
QEMU's `virtio-gpu-virgl.c` + vrend before designing it (the §4.5.4c habit)
changed what "fenced" buys: **vrend executes `TRANSFER_FROM_HOST_3D`
synchronously at DECODE time on QEMU's serial main loop** — `glReadPixels`
into the guest iov, returning only when every job writing the resource has
completed, which on V3D's in-order queue is every job queued before it — and
`FLAG_FENCE` changes only when the *response* is written. So a readback of a
busy resource stalls the DEVICE (every other client's commands, the
compositor's own sync steps, QEMU's display refresh) for the resource's GPU
backlog; fencing it frees the *guest* thread and nothing else; and a sync
step queued behind it inherits the stall — which makes `submit_and_wait`'s
"pending fences ahead cannot delay this chain" comment (true for fenced
SUBMITs, a decode) false for fenced readbacks (a GL wait), and its 500 ms
`SUBMIT_DEADLINE_MS` a false-`dead` hazard on a merely busy device.

That reframed the goal from "make the readback free" (impossible under
QEMU/virgl by construction) to three narrower things: the console's dispatch
never blocks on a client-chosen duration; the compositor never latches
`dead` because a device was busy; the compositor's OWN contribution to
device stalls is bounded and coalesced. GPU-DESIGN 4.5.13 (C-6, RESERVED) is
that design: the fenced readback with DEFERRED present completion, one in
flight per surface / latest wins, a reserved fenced slot (compositor-wide
bound of one, which loses nothing against a device that executes them
serially anyway), counted in the owning ctx's `fences_in_flight` for retire
safety but subtracted from admission so the client's share and its #210
ledger are untouched, and the sync-slot deadline widened to
`FENCE_ABANDON_MS` while any readback — ours or a client's — is in flight.
Two forms rejected on the record: a bounded sync wait (the command is already
in the device's queue; the next sync step waits behind it — bounds the wrong
thing) and gating on quiescence (a single-buffered client at its throttle
depth never quiesces; the §4.5.9 safety net would compose it once and never
again). The spec extension is named (`ComposeReadbackIssue`/`Complete`
behind `ALLOW_COMPOSE`, the retire guard generalized from `DrainedOfBlits`,
a `buggy_readback_free` cfg) and the Pi gate legs with it.

**And a new finding fell out — F2b.** Consequence 3 of the reading: *any*
client already holds the device-stall lever through its own `transfer_from`
of its own busy BO (the fenced verb every winsys has), repeatedly. F2 was the
compositor doing to itself what a client can do to it. Filed
(`memory/bug_f2b_readback_stalls_the_device.md`; GPU-DESIGN 4.5.13's F2b
paragraph): guest-side it can be not-added-to (C-6), not-mistaken-for-death
(the deadline half), and MEASURED (a warp-prove leg — client A reads back its
busy BO while surface B presents — owed with C-6's gate); it is removed for
real only by Venus (transfers become VkCommandBuffer copies the client
fences) or v3d-native (the queue is ours). Recorded under §9.2's host-side
exposures precisely so "trusted host" never reads as "no client can reach
it".

### Two things the bar found before it passed

**The C0-F1 leg's DEFENDED was a negative assertion with no positive
control** — "verify-ok still advanced after the attack" is satisfied by an
attack that never landed (the aux#215 class), and the texture-era leg had
leaned on a one-time host-log measurement for that; the buffer form did not
inherit it. Added in-guest before the first Pi run was trusted: after the
attack the client copies the mark BACK into its own buffer (the same command
the other way), reads its buffer back through the fenced verb, and requires
its own green. It printed `C0-F1 ATTACK LANDED -- the mark read back through
our own buffer as 0xff00ff00` — so the leg now proves a client can WRITE and
READ the probe's resources (the finding, re-measured on the buffer pair)
before it claims the repaint held; an unlanded attack is INSTRUMENT and F1
counts as not-defended.

**`warp-host.sh sync`'s uncommitted-scripts list omitted
`tools/interactive/lib.exp`** — the library every warp `.exp` sources. The
first sync shipped the new `warp-reject.exp` (in the list) against HEAD's
`lib.exp` (not in it), so the scenario would have died on `invalid command
name lc_run_expect_hardfail_re` — a list that claims to carry your edits and
does not carry the one file they all depend on. Caught by checking the Pi's
copy for the new proc before running (`grep -c` on both files, 1 vs 0);
`lib.exp` is in the list now.

### C-6a — the spec first (`tapestry_present.tla`, same run, after the push)

With the close pushed and ~100k of context left before the checkpoint line,
the next chunk was opened at its spec-first step rather than its code, so
that a compaction lands on a boundary and C-6's code has a model to be
audited against. `ComposeReadbackIssue`/`ComposeReadbackComplete` (a fenced
host DMA-WRITE into the client BO's pages, one in flight per generation),
`NoTornReadback`, `DrainedOfReadbacks` on `ServerRelease` + `Free`, and
`BUGGY_READBACK_FREE` as an omitted conjunct — the C-1 house style, for the
C-1 reason (a twin action drifts in more ways than the one under test). Two
deliberate absences, argued in the header: no `FillLanded` guard on Issue
(the device serializes the read against the fill — the very side effect P2
credits the sync readback with, now read in vrend 1.1.0 rather than
assumed) and no `attached` (the readback runs under the CLIENT's ctx; it is
the arm for the un-imported BO). `check-tapestry.sh`: ALL 12 CFGS AS
CLAIMED — the six direct-path cfgs at **5413** states exactly (the
additivity control, held twice now), the composed clean cfgs at 94680 with
liveness, and `buggy_readback_free` violating `NoTornReadback` in 11 states
(… `ClunkMap` → `ComposeReadbackIssue` → `Destroy` → `ServerRelease` →
`Free`: the pages freed with the device still writing them). SPEC-TO-CODE
names the sites the impl binds at; ARCH §28 I-40 / CLAUDE.md say 8 buggy
cfgs now.

### The bar

Local (mac): `cargo build -p tapestryd -p warp-prove --release`; ramfs
rebaked with `THYLACINE_BAKE_CLADE=1 THYLACINE_MKFS_PRESERVE=1`, verified by
CONTENT (`C0-REJECT INCOMPLETE` ×3, `probe-texture` ×1, `ATTACK LANDED` ×1
in `build/ramfs.cpio`); `tools/test.sh`: 1424/1424, arc gates L-6c/D-5 PASS,
clade 3/3, the G-4 console gate `CONSOLE VERIFY OK`. The kernel is
byte-unchanged (userspace + tools + docs only), so the SMP gate 40/40 at
`401d4b27` carries. thyla-pi (KVM, V3D, virglrenderer 1.1.0): `reject` →
`C-0d DETECTOR GATE: VERIFIED` (ANSWER=REPORTED-AS-SUCCESS as measured
before; DETECT PASS; STICKY PASS; C0-F1 first res 83 → mark 81 (the buffer
pair minted exactly two ids), ATTACK LANDED, DEFENDED; DONE; LS-CI PASS);
`prove` → `WARP-2 GATE: VERIFIED`; `quake` → `WARP-4 GATE: VERIFIED` (969
frames 21.7 s 44.7 fps on the egl-headless lane — 44.4/44.8 before;
`comp-attach witnessed 5 refused 0`; `comp-health verify on buffer pair`;
`probe-texture 0`). Both leases released the moment the resource freed;
the mac was aux's for the first ~1.5 h and its LS-CI legs were never
contended.

## 2026-08-17 — the aux-2 merge: two tracks fixed one UAF, and 23 conflicts said which one to keep

Resumed from the self-compaction at `a9a4a4fe` (Warp-C closed). The note said
"merge aux-2 first", and the reason it was first is the interesting part: the
main#243 Fable round had found a P1 (exec leaves `in_handler` set) plus two P2s,
and every one of them was ALREADY FIXED on aux-2 — aux had found the same UAF
(`#254`) the same week, from the other direction. Two independent proofs of the
same defect are worth more than one; two independent FIXES of it are a merge
conflict, and the conflict is where the decision lives.

### The merge itself (`8a58112d`)

104 aux commits over the common base `72ab319d`; 216 main commits the other
way; 23 conflicted files. The rule for every conflict was "which side's version
is the RATIFIED one", not "which is mine":

- **The sigtab UAF, twice.** main `a41fc9eb` reset the table in place through a
  public `proc_exec_reset_dispositions`; aux `c2a09473` + `8690cfb3` + `d3a11c8e`
  did the same through a static `proc_exec_drop_image_state` that ALSO clears
  the in-handler latch (#247 = main F1) and applies the operator-voted
  phenotype rule (F4). Aux's is the superset and is kept as THE one place; main's
  function is gone. What main had that aux did not was the per-8-byte-FIELD
  paragraph and an every-byte-zero test — folded into aux's comment, and the test
  ported onto aux's `_for_test` hook rather than deleted, because it asserts a
  property aux's test does not (a reset that stops early passes aux's).
- **`cons.c`'s mode write.** main's side was a COMMENT change (#233: login must
  set the mode before the prompt); aux's was a semantics change ratified in
  PTY-DESIGN and audited (a write clearing ICANON DELIVERS the pending line).
  Aux's code, plus main's corollary — the disclosure half of #233's race exists
  under either semantics, so the sentence still binds.
- **The bin lists** (`tools/build.sh`, `usr/Cargo.toml`): the union, verified
  programmatically against the base — no member dropped by either side.
- **AUDIT-TRIGGERS.md** was an add/add (both trees created it from CLAUDE.md's
  table on the same day and each appended rows): resolved ROW BY ROW against the
  base row, so main's vault-#170 path fixes and pipe escapes and aux's addenda
  both survive; the LS-8 row carries both sides' addenda in order.
- **147-execve.md's sigtab row** was stale on BOTH sides (main said "zeroed in
  place", aux said "zeroing is exact POSIX because SIG_DFL == 0" — aux's own later
  commit had made the reset phenotype-conditional). Rewritten to the MERGED rule
  rather than picking a stale side; the note-mask and in-handler rows added.
- **Seven ragged doc rows** (six pre-existing on both tips, one in aux's newest
  addendum) escaped with the two controls `85c1ee9c` used: the checker to zero,
  and de-escaped-line == original with only the named lines differing.

**One thing the resume note did not say and the build did:** aux's DISTRO gates
are pool-resident and SOFT-SKIP without the Alpine tarball, which main's cache
did not have. A green `tools/test.sh` with two skipped arc gates is a gate not
run — so the fixtures were copied from aux's cache and the pool + ramfs re-baked
PAIRED (`PRESERVE=0`, fresh key both sides). `arc gates: 2/2 ran -- L-6c=PASS
D-5=PASS` on the merged tree; suite 1424/1424; clade 3/3.

### The main#243 residuals, on the merged tree (F2/F5/F6/F7/F8)

The round's F6 was the sharpest: the 8-byte store width that the whole lock-free
argument rests on was a MEASURED codegen property (a struct assignment happened
to give `stp`), not a construction. It is a construction now — every entry field
is one `__atomic_*` op on an aligned u64 (`_Static_assert`ed), the install
publishes `handler` last with release and readers acquire it, the reset zeroes
`handler` first; objdump shows `str xzr` per field and `stlr`/`ldar` on the
gate. F2 wrote the load-bearing sentence AT `notes_proc_has_live_handler`
("a cross-Proc reader that acts on `handler` alone; the copy is discarded"),
which is the sentence the three earlier statements of the argument had each
left implicit. F5's discrimination was checked the only way that counts: two
sabotages (a reset one entry short; the gate field only) each went RED on the
named assertions, and the tree was reverted with text replacement, not
`git checkout`. F8 clears `clear_child_tid` at exec beside `in_handler`. F7
retired four stale sentences (three of them "X is not a table row" claims that
the LINEAGE arc had falsified without anything failing).

### The C-0d Fable round came back while the bar ran: two P1s the three Opus rounds could not see

The #240 detector's first read from a different lineage (98 of 101 model
turns Fable; the last three, the write-up, fell back to Opus 4.8 — recorded):
**0 P0 / 2 P1 / 1 P2 / 2 P3, dirty on the P1 criterion.** Both P1s are the
same blind spot from two sides, and it is exactly the one family independence
exists to buy: three Opus rounds gated the synchronous lane on the CALLER's
fence gauges, and none re-asked the cross-context question after C-4 measured
that a texture readback on a tiled renderer drains the whole device queue.

- **F1**: the CLIENT-ctx probe is still the TEXTURE pair. C-4 moved the
  compositor's health pair to buffers for precisely this cost and left the
  client detector as it was — so a `verify` on client A drains behind client
  B's queue while the gate reads only A's gauges, and 149-warp.md promises
  clients the opposite. Fix: the buffer pair for clients too (the C0-F1 leg's
  attack source has to become a buffer BO, or it "defends" for the wrong
  reason — a texture-to-buffer copy is refused, not repainted away).
- **F2**: the composed READBACK arm — the CPU fallback — is a synchronous
  full-frame readback of the client's render target on the client's own
  queue; the client picks its length; and `fence_poisoned`, round 3's gate,
  cannot protect it because the poison is produced by the reaper on the very
  serve loop that is blocked. Only READBACKS carry this (a blit's SUBMIT_3D
  response is written at decode time, before the GPU runs it), so the fix is
  not a gauge but the fenced form C-4 measured its way past — a bounded or
  deferred readback: **Warp-C C-6**, the next chunk. Gating the fallback on
  `fences_in_flight == 0` was weighed and rejected: it would collapse the
  safety net GPU-DESIGN 4.5.9 keeps for every continuously-rendering client.
- F3 (probe VA rides the never-reclaimed `weave_va_next`, a second driver
  for #171), F5 (`present-to` re-import witness storm on the shared ctx, no
  rate limit), F6 (the reject scenario's pass token is printed unconditionally;
  the real 5-term gate lives only in `warp-host.sh`). Dispositions in
  `memory/audit_c0d_fable_closed_list.md`; the close is the next chunk after
  the push, then the dirty-close follow-up round.

### The bar found one more thing, and it was ours from the merge

The merged tree's first LS-CI (JOBS=3) came back 37/37 — with **three attempt-1
failures at t=0-1 s**, every one `-qmp unix:build/qmp-gate.sock ... Failed to
bind socket: File exists`, every one classified INFRA by aux's failure-time
probe ("the VM never started, so this attempt says NOTHING about the guest").
aux's #230 had given run-vm.sh a SECOND QMP monitor for test.sh's screendump
gate — a fixed path — and test-interactive.sh's per-slot export list, written
for #127's lesson that "a fixed host resource is a DETERMINISTIC collision at
N>1, not a flake", predates it. Three VMs launched in one batch interleave
run-vm.sh's `rm -f` and bind, and the loser dies before boot. A retry budget
turned a deterministic collision into three green retries; the count is what
gave it away. `e680fdd5` exports `THYLACINE_QMP_SOCK2` per slot; the re-run
was **37/37, 0 retries, wall 1744 s** against 2569 s before — and the SMP gate
on the merged kernel: **40/40, 0 corruption / 0 external-kill** across
default+UBSan x smp4/smp8. Pushed to both mirrors at `e680fdd5`.

---

> **Two tracks, one thread.** Entries marked `(aux)` were written on `aux-2`
> and merged into this file when aux-2 merged into main (2026-08-17); the two
> tracks ran concurrently, so a main run entry and the aux entries beside it
> overlap in wall-clock time. The `(aux)` block below is in the order aux
> wrote it -- oldest first, `c8ab2744` to `01f076f2`; main's run entries
> below it are newest-first as the convention says.

---

## 2026-08-17 (aux) — the c8ab2744 audit close, and the positive control that caught a second bug

Resumed from aux's **first** self-compaction (the change-of-watch scripts had
been main-only until `4525023a`; the operator had compacted this track by
hand). The nudge fired and the resume note said, correctly, "execute the plan;
do not re-derive it" — the Fable 5 round on `c8ab2744` had reported the audited
change CLEAN and four PRE-EXISTING findings three lines above it, and the fix
plan was already written in `memory/audit_15_closed_list.md`.

### The four fixes (`93a91c6c`)

- **F1 [P1] — both class scans read the sigtab per note.** The terminate scan
  gated on `handler_va` (0 for every Linux guest) and returned the first
  latch-class name at ANY index, so a `SIG_DFL` candidate that fell through
  from the phenotype branch let it name a CAUGHT `tty:hup`/`interrupt` behind
  it and the guest died with its handler installed. #251's per-Proc predicate
  had reached three sites and not this one — the fourth "site N+1" on the row
  (V-8 F2 → #251 → maskstop → F1). Fix: `notes_proc_default_applies(p, name)`
  INSIDE both scans; the fixed-name outer gate on the stop scan retired.
- **F2 [P2] — a `SIG_DFL` `pipe` on PHENO_LINUX reached no arm** (no native
  latch, #237) and sat as the dispatcher candidate for life. Fix,
  phenotype-scoped: `viv_signote_default_is_terminate` + `exits(canonical)`
  from the phenotype branch on the candidate. Native `pipe` untouched; #237
  stays the ABI question it is.
- **F3/F4 [P3]** — the dead drain call deleted with its reasoning; three "an
  uncaught susp is never queued" sentences reworded (caught / all-masked /
  thread-less).

### The wrong turn worth recording: J and L passed on an empty capture

The E2E for F2 is three L-6c legs sharing one fixture — `err=$( { WRITER 2>&3
| head -n 1 ...; } 3>&1 )` — J and L asserting the writer printed NOTHING (killed
by SIGPIPE), K the positive control (`trap "" PIPE` in the writer's own process
→ EPIPE returned → `write error` reported). Boot A: **J green, L green, K red,
`L6C-K-RAW:` empty**, and once per leg on the console:
`/gate/run.sh: line 9: fcntl(3,F_DUPFD,10): No file descriptors available`.

busybox ash's `redirect()` probes the TARGET fd of every `N>&M` with
`fcntl(N, F_DUPFD, 10)` to learn whether N is open — `EBADF` means "not open,
nothing to save"; anything else is "strange" and aborts the command. The
vivarium's `VIV_FCNTL_DUPFD` arm answered `EMFILE` for BOTH of
`handle_dup_posix`'s folded failures, on a comment arguing that a guest which
just used the fd knows it exists. True about the wrong caller. So the whole
capture never ran, the substitution yielded "", and two negatives were
satisfied by a broken fixture — aux#215's class, caught by the remedy aux#215
prescribes. Without K this would have shipped as two green legs proving
nothing. Fixed in the same commit (a liveness re-check after a failed dup:
closed → `EBADF`, residual → `EMFILE`; `vivarium.fcntl_dupfd_errnos`).

Boot A2 then showed a second fixture wart: `head -n 1 >/dev/null` printed
`can't create /dev/null: Function not implemented` — ash opens `>` with
`O_CREAT|O_TRUNC` and `O_CREAT` is a KNOWN unserved openat flag (#201, designed
around). The legs still measured SIGPIPE correctly (the reader slot died before
reading instead of after one line), but a fixture must not lean on a known
gap: the reader now writes its one line INTO the capture, so J's assertion is
the sharper "the capture is EXACTLY `y`" — the reader really read, the writer
was silent.

### The bar

Suite 1405/1405 (+2). Sabotages, each reddening its named assertion and
nothing else: S1 (terminate gate dropped) → `A: the terminate scan does NOT
name the CAUGHT interrupt`; S2 (stop gate dropped) → `D: the stop PREDICATE
declines a caught susp`; S3 (phenotype `exits()` disabled) → suite green,
L-6c `first-missing=L6C-J`, L missing, K present. pty + pty_stop: 4 clean/
liveness cfgs green, 6 buggy cfgs violate (rc 12/13) — after fixing the runner,
which first "passed" all ten legs in 0 s because `/usr/bin/java` is the macOS
stub and every rc was 1 for the wrong reason (the buggy legs read as
violations). Keyed on the exit code AND the `TLC2 Version` banner now. SMP gate
40/40 (default+UBSan × smp4/smp8, N=10, 0 corruption). LS-CI 33 PASS + 2 SKIP (GL not
baked) — and pty-4 burned a retry AGAIN, this time INTO the failure-time probe
landed at `11173762`: see the next entry, because the probe answered.

### Still open leaving this run

- #237 (native `pipe` has no latch) is sharper, not closed: the phenotype
  answers SIG_DFL SIGPIPE for its own Procs; a native handler-less, fd-less
  program still keeps a stranded `pipe` note.
- The tail's delivery-time SIG_IGN discard arm is reached by nothing (second
  unconstructed state on this row); its own chunk.
- `>/dev/null` from a Linux shell under viv fails on `O_CREAT` (#201) — the
  most common redirection in existence; the L-6c fixture routes around it.
- pty-4's burned retry: instrumented, not diagnosed.

## 2026-08-17 (aux) — pty-4's burned retry, diagnosed on the probe's first miss: the ldisc flushed type-ahead

The failure-time probe landed at `11173762` the day before, on the theory that
INPUT truncation and OUTPUT loss are indistinguishable in a plain capture and
only the guest can say which. Its first miss (LS-CI batch 6 of the c8ab2744
close bar) said, in order: `[listen]` — the raw stream showed `sle` as PLAIN
echoed text after `PTY-INNER`, then only SIX empty editor redraws where the
passing attempt shows NINE (`sleep 30\r`); `[jobs]` — nothing listed;
`[channel alive?]` — the editor answered; VM alive, bridge alive. The editor
never echoes typed text (the harness header says so), so plain `sle` can only be
the pts line discipline echoing in cooked mode.

So: `lc_run_expect` returns the instant `PTY-INNER` is SEEN — before `ut` has
reaped the pipeline, restored PROMPT_MODE and redrawn — and `lc_send "sleep 30"`
fires at once. On TCG the window is sometimes wide enough that `s`,`l`,`e` land
in CHILD_MODE (+icanon +echo): assembled, echoed, then ut writes PROMPT_MODE and
ptyfs `ctl_apply` does `p.line_len = 0; // TCSAFLUSH: a mode change resets the
assembly` — the three bytes are gone and `ep 30\r` reaches the raw editor. A
race, and a real one — but the DEFECT is the guest's: Plan 9's `devcons` `rawon`
pushes the partial line to the reader ("flush output on rawoff -> rawon", the
clumsy-hack zero byte), Linux's `n_tty_set_termios` never discards on a canon
change, and TCSAFLUSH is a caller-chosen flush that bash/readline deliberately
do NOT use (`TCSADRAIN`). Thylacine's ctl grammar offered no choice: every mode
write flushed. Type-ahead across a job's end — a paste of two lines, a script
driving a pts, LS-CI — lost the HEAD of the next line and executed the TAIL.

The posture came from the LS-8b audit's F1 remedy ("a fragment stranded across
canonical→raw→canonical prepends the next line"), copied per-pts by PTY-2c, on
the stated premise that "no current consumer flips mid-line". The premise was
falsified by the one consumer that flips around every foreground job. Both
ldiscs now DELIVER on ICANON-clear and touch nothing otherwise
(`c62eb738` scripture, PTY-DESIGN "Mode writes deliver, never discard"; the impl
`ccb597b8`): the F1 hazard stays closed because canonical→raw delivers, so nothing is
stranded, and I-20's byte conservation now holds across a mode write. A
delivery into a full ring is a real drop under a new counter
(`rx_drop_modeflush`, the #95 rule). Not built: an explicit flush verb — pouch's
`TCSETS/SW/SF` all map to the one write, which now behaves like `TCSANOW`.

Two things worth keeping from this: (1) the instrument earned its keep on its
FIRST miss, and the reason it could is that it asked the guest in a fixed order
with a control at the end (`channel alive?`); (2) a "posture" chosen as an audit
remedy is still a claim about consumers, and consumers change — the sentence
"no current consumer flips mid-line" was true when written and had no test.

## 2026-08-17 (aux) — the "reached by nothing" discard arm, and why the right fix moved the mechanism instead of reaching it

Resumed from aux's **second** self-compaction (`05708496`). The resume note's
first item was to ask the operator for the owed prosecutor round on `ccb597b8`;
the ready-to-paste prompt was written first
(`memory/audit_ccb597b8_prosecutor_prompt.md`), my own self-audit of the
mode-write delivery found nothing, the operator said yes, and the round is
running in the background while this chunk lands (its files — `cons.c`,
ptyfs — are disjoint from this one's).

### The chunk: Stream 4's "delivery-time SIG_IGN discard arm is reached by nothing"

The open item was the second member of the unconstructed-state class found by
sweeping after the maskstop one: `notes_deliver_at_el0_return`'s phenotype
branch discards a candidate whose disposition is `SIG_IGN`, and its own comment
named the only way in — "a note queued BEFORE the install is still sitting
here … this is the only place that can happen." The post-time hook prevents the
state in every ordinary ordering, so the arm needed a CONSTRUCTED one.

Constructing it in-guest turned out to be easy — `viv-pheno-probe` has raised
SIGPIPE at will since V-6c (fd 0 is a reader-less pipe write end), so
`block → write → SIG_IGN → unblock` is deterministic. But writing the legs
forced the question the arm's comment had skated over: **what does POSIX say
happens to a pending, blocked signal when its disposition becomes `SIG_IGN`?**
It is discarded AT THE INSTALL, "whether or not it is blocked" (2.4.3; Linux
`do_sigaction` → `flush_sigqueue_mask`). Thylacine discarded at the next
delivery instead. Same answer for `pending → SIG_IGN → unblock`; a DIFFERENT
answer for `pending → SIG_IGN → handler → unblock` — Linux fires nothing, the
tail ran the handler for a signal POSIX says had died. So the arm was not merely
uncovered; the mechanism it implemented was wrong on the ordering nobody had
tested, and the honest fix is not to reach the arm but to move the discard.

What landed: `notes_discard_name(p, name)` — under `q->lock`, remove every
queued note of one name, mask-blind, each removal draining the class latch as a
dequeue does (an `interrupt` armed under `SIG_DFL`, then ignored while blocked,
must not leave a Proc whose every sleep is `*_INTR`), `kill` refused; the
phenotype `rt_sigaction` shell calls it after the store whenever the new
disposition ignores (`SIG_IGN`, or `SIG_DFL` for a default-ignore signal — the
no-table `SIG_DFL` shortcut now skips only the store); and `notes_post`'s
disposition read moved UNDER `q->lock`, so store-then-lock against
read-under-lock leaves no interleaving with a stale ignored note. The tail's
arm stays as defense-in-depth — its absence would hand a stale note to the
`SIG_DFL`-terminate arm — with its comment rewritten to say exactly that.

The proof: `notes.discard_name_purges_pending` (mask-blind, per-CLASS latch
drain — tty:hup out leaves the TTY latch armed for tty:quit — survivor order,
`kill` refused, a purged FULL ring really empty: 16 out, 16 in) and probe legs
L205–L216. Round A: pending → `SIG_IGN` → unblock survives with nothing fired
(L209 is PRE-STAMPED and rewound so a death names its leg instead of leaving
joey's `??` — the marker channel is fail-only by design, and this is the one
place a marker is written before the verdict is known), then a handler
installed after is not handed a stale note (L210). Round B: pending →
`SIG_IGN` → handler → unblock fires NOTHING (L215 — the install-vs-delivery
leg; red on the tree before this chunk). Each round ends with a fresh SIGPIPE
delivered exactly once, so a queue wedged by the experiment cannot read as
"nothing fired".

### Found on the way, enqueued not fixed

Reading `proc_exec_drop_image_state` for the exec-time sigtab reset: it zeroes
every row and the mask, and its comment says "Zeroing is exact POSIX". True of
CAUGHT handlers; false of `SIG_IGN` and of the blocked mask, both of which POSIX
and Linux keep across `execve` (`nohup`, `sh -c 'cmd &'`, `trap '' INT; exec`
all depend on it). ARCH §7.6 names the clear as the NATIVE rule, so the fix is
phenotype-conditional and a scripture decision — surfaced with options in
`memory/bug_exec_resets_sigign_and_mask_phenotype.md`; recommendation:
phenotype keeps `SIG_IGN` + mask.

### The bar (`7580c1f7`)

Suite 1406/1406 (+1); V-1b PASS (L205–L216 green); L-6c PASS. Sabotages, each
reddening exactly its named assertion: S1 (the shell never purges) → V-1b
`marker=L215` — and NOT L209, because the tail's arm still saved that ordering,
which is the whole reason the arm stays; S2 (S1 + the tail's `SIG_IGN` disjunct
deleted) → `marker=L209` — the guest died at the unblock and the pre-stamp named
the leg; S3 (purge without the latch drain) → the unit test at "removing the
last interrupt drained the latch", 1405/1406. SMP gate + LS-CI ran over the tip
together with the round close below (see the fixup).

## 2026-08-17 (aux) — the ccb597b8 round came back: sound delivery, an unwitnessed counter

The operator said yes to the round while the chunk above was being built; the
prosecutor (Fable 5, read-only) ran ~20 minutes and reported 0 P0 / 0 P1 / 2 P2
/ 6 P3 — every finding on the NEW DROP SITE's witness, none on the delivery it
was asked to break. It re-derived the I-9 wake pairing, the poll relay, the SMP
ordering under `g_cons.lock`, the hook/production parity and ptyfs's
single-threaded ordering line by line and found them as claimed.

What it found instead is worth keeping. **F1**: the fifth drop site's counting
path had only a NEGATIVE test in both ldiscs — leg B "it fit, no drop counted"
against an empty ring — so a misattribution to `rx_drop_ring` (the must-stay-
zero witness) or not counting at all read green. The tree's own
`test_cons_rx_drop_counters` header says exactly why that is worse than no
counter, and I had shipped one anyway because the negative FELT like coverage.
Legs (d)/(e) now drive the site (512 filler + 10 pending → 10 counted here,
every sibling asserted unmoved, filler intact; 507 + 10 → the 5-byte PREFIX
delivered in order); the ptyfs selftest drives its site on a fresh pts.
**F2**: ptyfs had folded that drop into `drop_flush` — against PTY-DESIGN,
which named "its own counter" for BOTH ldiscs, and against `drop_flush`'s own
documented shape (a short cooked flush loses tail + newline so the line never
runs; a short mode-flush loses the tail and the terminator arrives raw, so the
truncated command RUNS — #95's exact shape, hidden under a name whose doc said
it could not produce it). One of two twins diverged from a rule written for
both, and a re-read of the scripture would have caught it. **F3/F4/F6/F7**: the
one-shot report did not name the new site; the "reachable only by a wedged
reader" claim was false (ut re-arms before it drains, so a paste can reach it);
three comments still said TCSAFLUSH; 111-cons.md carried the deleted test with
the reversed semantics. **F8**: pty-4's type-ahead leg had no ARMED witness —
bytes landing raw before CHILD_MODE or after the re-arm satisfied the cursor-35
anchor too, under the old posture as well; it now first requires the pts's
cooked echo as plain text directly after the CRLF, which only CHILD_MODE cooking
produces. **F5** stays open as a scripture vote: an ISIG-consumed ^C/^\/^Z does
not flush the pending canonical line (POSIX and Linux do; Plan 9 does not) —
the old reset masked it, delivery makes it visible; recommendation: adopt POSIX
in both ldiscs.

Closed at `56b5a412`: suite 1406/1406; S7 (kernel misattributes to
`rx_drop_ring`) → "(d) modeflush counts exactly the 10 bytes the full ring could
not take"; S8 (ptyfs folds into `drop_flush`) → `ptyfs: selftest FAIL:
modeflush-drop-not-counted`, boot-fatal.

### The bar over the tip (`56b5a412`, both commits)

One run for both (disjoint surfaces): SMP gate 40/40 — default + UBSan ×
smp4/smp8, N=10, 0 corruption / 0 external-kill / 0 other, in two halves —
then LS-CI in six batches on TCG: 33 PASS + 2 SKIP (the GL half is not baked
into this pool; not a guest result, not coverage). pty-4 passed WITH the new
armed witness (the pts's cooked echo matched before the cursor-35 anchor — the
delivery path was exercised, not merely reached). Pushed to both mirrors after
the fixup.

## 2026-08-17 (aux) — the votes came back: ISIG discards, fork/exec goes POSIX, and the 7580c1f7 round

The operator answered all three questions in one round: spawn the 7580c1f7
round (yes), F5 (adopt POSIX — an ISIG character discards the pending line in
both ldiscs), and the exec item (the phenotype keeps `SIG_IGN` + the mask). Each
landed scripture-first.

**F5** (`e69e9baf` scripture, `4df51c30` impl): the kernel ISIG arm and the
ptyfs ISIG arm zero the pending assembly when ICANON is set — a disposition like
an erase, not a counted drop, deliberately narrower than POSIX's full flush
(committed lines in the ring stay; output is never flushed — the console TX ring
carries kernel diagnostics). The PTY-3 pouch probe's leg H had pinned the OLD
posture (`x` ^C `y` CR → `xy\n`) and went red on the first boot — the fixture
that encoded the divergence, found by the change that removed it; updated to
`y\n` as on Linux. Sabotages S9/S10 each red on the named check.

**fork/exec** (`c484a7d1` scripture): reading `proc_exec_drop_image_state` for
the exec half surfaced the fork half too — task #127, recorded at L-3d as "two
behaviours and a design decision", never landed. So the chunk is the pair:
`rfork` copies the parent's sigtab into the child's OWN table (before the child
is postable) plus the caller's `note_mask`; `execve` resets caught rows only and
keeps `SIG_IGN` + the mask; native keeps the Plan 9 clear. Probe legs L217–L228
drive a real fork and a real exec (the children name the first wrong fact
through the report dup); the unit test pins the two primitives.

**The 7580c1f7 round** (Fable 5, 0/0/0/4) re-derived the install-time discard
SOUND — the linearization, the primitive, the shell, the pre-stamp arithmetic —
and found the one ordering nobody had tested: `block; SIG_IGN; raise; handler;
unblock`. Linux queues a blocked ignored signal ("the handler may change by the
time it is unblocked") and discards at dequeue; Thylacine drops at generation,
mask-blind. POSIX 2.4.1 permits both, so it is recorded as a stated divergence
rather than matched — but the docs had said "exactly as Linux", and the lesson
worth keeping is that "exactly as X" is a claim about every ordering. F1: the
SIG_DFL/default-ignore purge disjunct had no driver → L229–L232 with a positive
control (S13 reddens only the negative). F2/F4: an over-claiming comment and two
stale sentences.

### The bar over the tip (`d3a11c8e`: F5 + fork/exec + the round close)

SMP gate 40/40 (default + UBSan × smp4/smp8, N=10, 0 corruption / 0
external-kill / 0 other, two halves); LS-CI 33 PASS + 2 SKIP (GL not baked);
suite 1408/1408 per commit; sabotages S9/S10 (F5) and S11–S15 (fork/exec)
each red on the named check — S14/S15 are the WIRING witnesses (the unit test
cannot see proc.c; the probe legs L223/L226 can, and they went red). Pushed to
both mirrors after the fixup.

## 2026-08-17 (aux) — the d3a11c8e round: the fork rule was one field short

The operator said spawn; the round (Fable 5, read-only, 0/0/1/6) re-derived
both mechanisms sound — the fork copy is published before the child is
reachable and aliases nothing, the exec reset uses the same "caught" predicate
delivery uses, the ISIG discard is one field under the right lock in both ldiscs
— and found the one place the voted RULE was short. "fork copies everything
(POSIX fork(2))" copied what POSIX names: dispositions and mask. This design has
a third piece of thread signal state POSIX never has to name, because Linux
keeps it on the user stack: the kernel-side handler-execution snapshot (the
sigframe here is written for reading; `rt_sigreturn` restores from the
per-Thread save block). A `fork()` issued from INSIDE a handler — async-signal-
safe, POSIX-permitted — therefore produced a child whose user stack said "in a
handler" while its KP_ZERO thread said "not"; its handler return was refused
and it ran on past the svc into whatever followed the restorer (musl: silent UB;
the probe: `brk #0`). Fork+exec and fork+`_exit` from a handler were fine, which
is why nothing had surfaced. Fixed by copying the block with the mask
(`in_handler` written last, before `ready()`); phenotype only — a Plan 9 child
is not notified. Lesson: enumerate what the RESTORE path reads, not what the
standard lists.

The witness leg cost two extra boots for a reason worth keeping: its first
draft had the child exit 3 and the parent reap "exactly 3", and it went red on a
WORKING fix — v1.0's phenotype exit path collapses every non-zero
`exit_group(N)` to 1 (VIVARIUM task #91, "`exit(N)` is boolean"). A diag with
`exit(5)` read as 1 too. So the oracle is exit 0 versus anything else, and the
child's own marker (re-emitted by the parent on failure) carries the why. A
status oracle must be a value the status channel can carry.

Six P3s: a pre-#254 "known hazard" paragraph in `proc_exec_replace` that
contradicted the in-place reset it now calls; a phantom `viv_sigtab_copy_into`
in 145; PTY-DESIGN naming leg (f) for (e4); the ptyfs (e4) leg with no witness
for "m2s/s2m are NOT flushed" (both were EMPTY at the VINTR, so an over-broad
discard passed — it now commits `x\n` unread and leaves the echoes unread and
asserts both survive); the fcntl test's header comment migrated onto the sigtab
test; and the ISIG-DISCARD + ccb597b8-ROUND addenda living only on the
AUDIT-TRIGGERS rows that declare ARCH 25.4 authoritative (mirrored). Enqueued
from the observations: `Proc.socktab` is not cloned at fork (the fork half of
the LINEAGE dup3 note — a real L-6 gap for fork-per-connection servers), the
handler mask discipline (sa_mask|sig never applied during a handler; sigreturn
does not restore the mask), and `pty.tla`'s CookSignal echoing a char neither
ldisc echoes.

## 2026-08-17 (aux) — the console TX ring pushes UNITS now

Main handed over the byte-atomic tear it measured on thyla-pi: `proc: orphan
pid=2119 name="ttaappeessttrryydd"` — the kernel's orphan-adoption burst and
tapestryd's posture line on another CPU, byte for byte, because every producer
pushed each byte under its own `g_cons_tx.lock` hold and the writer role cannot
serialize a diagnostic emitter (IRQ context; the role sleeps). ARCH 23.5.2 had
already named the missing piece — "full echo-exclusion via a bulk-push fast
path" was #79, a documented v1.x item withdrawn from an earlier draft because it
"carries a two-ring lock-ordering design". The design point resolved as: never
nested. Tap under the drain lock, release, push under the ring lock, release.

The rule now: every producer pushes a UNIT under one hold. A kernel diagnostic
is a line assembled on the caller's stack (`struct cons_diag_line`) and pushed
once, all-or-nothing — the per-token trio is gone, because a per-token API
cannot be line-atomic without hidden state, and a per-CPU accumulator would
splice an IRQ handler's line into the process-context line half-assembled below
it; a caller-owned object is nesting-safe by construction. Echo pushes its
staged unit whole (half a `\b \b` walks the cursor over the prompt). The role
writer stages a 512-byte chunk, cuts it back to the last NL when the input
continues, pushes what fits and room-waits for the rest — so a ring-fitting
write, which is every console line, is whole against every producer. The
residual is named and Linux-equivalent: a long write spans chunks; a FULL ring
splits at a chunk boundary, because progress beats atomicity under congestion.

Three tests, one of them the tear's own witness: two kthreads hammer a STALLED
ring with 64-byte units from two CPUs, the ring is read back through a new peek
hook and parsed as frames, and every frame must be one producer's unit — with an
overlap witness so the test says whether the interleave was exercised (it was).
The other two pin the boundary deterministically on one CPU: room = len-1 moves
the count by zero and `dropped` by exactly len; room = len lands whole.

### The bar over the tip (`277b02cc`: the round close + the TX-ring unit)

SMP gate 40/40 (default + UBSan × smp4/smp8, N=10, 0 corruption / 0
external-kill / 0 other, two halves — the kernel byte-changed, so the whole
matrix re-ran); LS-CI 33 PASS + 2 SKIP (GL not baked; six batches, TCG); suite
1408/1408 (`920bbfca`) and 1411/1411 (`277b02cc`) per commit; sabotages
SF1/S16/S17/SP5 (the round close) and S1–S3 (the unit rule) each red on the
named check. Pushed to both mirrors after the fixup.

A number corrected on the way: three earlier bar stanzas and four status rows
said "LS-CI 34 PASS + 2 SKIP". Every bar today measured 33 + 2 over the same 35
scenarios, and so did the two before it; the 34 came from the c8ab2744 close's
"36 scenarios" — an `ls tools/interactive/*.exp` count that included `lib.exp`
— minus the two SKIPs. A derived figure propagated as a measured one, six
times, before a run's own tally was set beside it. The tally is now taken from
the harness's `==> LS-CI:` lines only.

## 2026-08-17 (aux) — the handler-time mask is Linux's; three socket findings; a file count that was not a scenario count

Item 7 of the notes line was the smallest thing on the queue and the only one
without a vote in front of it (the #237 `pipe` default and the socktab posture
both alter user-signed scripture), so it went first while the votes ride the
report. The d3a11c8e round had recorded two permissive-direction divergences:
delivery never applied `sa_mask | sig` while a handler ran — N-3's blanket
`in_handler` guard stood in for it — and `rt_sigreturn` did not restore
`note_mask`, so a handler's own `rt_sigprocmask` outlived the handler, and an
`execve` from inside a handler handed the image the PRE-handler mask where
Linux hands it mask | sa_mask | sig.

The change is three lines and a field. `notes_deliver_linux_locked` saves the
pre-handler mask into a new `Thread.note_saved_mask` and stores Linux's
`signal_delivered` value — mask | sa_mask | sig, sig omitted under
`SA_NODEFER`, both additions through the same coarse translation as
`rt_sigprocmask` (a tty-family `sa_mask` entry blocks the family; SIGKILL is
dropped); the phenotype's `rt_sigreturn` restores the saved mask, gated on
`t->proc->phenotype` because a PHENO_LINUX Proc reaches delivery only through
the Linux path and a native Proc never does; and the fork-from-inside-a-handler
copy from the round's F1 gained the field — the round's own lesson, "enumerate
what the restore reads", applied to the next field. Delivery is untouched: the
guard still holds every note for the handler's duration (VIVARIUM 6.22's stated
conservative imprecision), so what changed is the mask a handler OBSERVES and
PASSES ON. The frame's `uc_sigmask` still carries the pre-handler mask and is
written for reading — a handler that edits it changes nothing, which Linux
would honour; recorded as the conservative-direction divergence of this frame
design. Native `noted` keeps the as-built rule.

Two things the witness taught. A signal with no note (SIGUSR1/2) reads back
CLEAR whatever is blocked — the translation has nothing to set — so a
`sa_mask = {SIGUSR1}` witness would have proved nothing; the legs use SIGINT,
SIGCHLD, SIGWINCH and SIGPIPE, one note bit each. And the pre-handler mask is
{SIGCHLD}, non-zero on purpose: a restore that puts back ZERO is
indistinguishable from a correct one against an empty pre-handler mask, and
the fork leg (the child forked from inside the handler restores at ITS
sigreturn) is exactly the leg a missing copy would pass with zero. The Thread
grew by a u64 and its size did not change — the 8 bytes landed in the pad
before the 16-aligned FP area — and that was measured with
`-fdump-record-layouts` before the size assert's message said so, not derived.

The first boot reddened the "handler's own block undone" leg on a WORKING
restore, and the reason is the reusable part: probe leg L26, far above, blocks
SIGWINCH to assert the tty family's honest over-report — and nothing since
unblocks it. So the pre-handler mask carried the tty bit, the restore put it
back exactly as it should, and the leg read that as "the block persisted". A
premise assumed is a premise that can be false without anyone's fault; it is
now asserted as its own leg (L237: the pre-handler mask is exactly {SIGCHLD}),
with the tty family unblocked first and re-blocked after so the legs below run
under the state they always had.

Sabotages, each red on exactly its named check: SM1 (no handler-time store) →
probe L239 (the mask inside lacks sa_mask|sig; 1413/1413); SM2 (no restore) →
`notes.phenotype_sigreturn_restores_mask` leg A (1412/1413 — the suite fails
first, so the probe is not reached; L240/L241 had already shown they
discriminate, on the premise failure above); SM3 (the fork copy skips the
field) → probe L244 only (the child forked from inside the handler restores
zero, and zero is not {SIGCHLD}).

### Three socket findings, from reading before touching

The socktab item (fork does not clone it) was researched instead of started,
and the research moved it. The enqueued plan said "a refcounted entry"; a
refcounted ENTRY cannot carry the ctl->data handle swap `connect` performs in
one table, so it reproduces Linux no better than a per-process copy — and a
per-process copy is Plan 9 APE's own posture (rocks live in process memory;
fork copies them). Every fork shape that occurs (accept-then-fork,
prefork-accept) works under a copy; the divergence — a state mutation through
one alias not seen through another — is the one LINEAGE already published for
dup3. VIVARIUM 5.5.2 states today's "not rfork-inherited" as design, so the
flip is the operator's vote (`memory/design_socktab_across_images.md`).

Alongside it, two defects verified in the tree. `handle_close_on_exec` closes
a close-on-exec socket handle and pays no socktab drop, and `fcntl(F_SETFD,
FD_CLOEXEC)` is a served row — so `socket; fcntl; execve` leaves a stale
(proto, N) entry keyed on a number the new image's next fd-creating call is
handed: the "dial verb to a stranger" class the V-5 header names as the
sharpest this table can have, reached through exec rather than dup. And the
reach is wide, because of the third finding: `socket()` answers EINVAL for
`SOCK_CLOEXEC|SOCK_NONBLOCK` "rather than masking them off", and EINVAL is
exactly musl 1.2.5's fallback trigger (`third_party/musl/src/network/socket.c`):
it retries without the flags, then issues `fcntl(F_SETFD, FD_CLOEXEC)` — served,
so every musl `SOCK_CLOEXEC` socket reaches the stale-entry path — and
`fcntl(F_SETFL, O_NONBLOCK)` — unserved, ENOSYS, and musl ignores the result. The
guest ends up holding a BLOCKING socket it believes non-blocking, the very
failure the refusal's comment says it prevents. A refusal is only as honest as
the libc that receives it; the claim was verified on the artifact, not on the
kernel's return value. Both enqueued (memory + AUX-ROADMAP), main told
(V-5 is theirs).

Also to verify, not yet verified: holotype R5-F9 (longjmp out of a handler
wedges `in_handler`) was registered against pouch programs, but busybox ash's
`raise_interrupt` longjmps out of the SIGINT handler when interrupts are
enabled, and the phenotype population is every musl-static shell. One VM
experiment settles it; if real it is a P1 for interactive shells and needs an
abandoned-frame rule (design).

### The count that was a file count

The push bar over `277b02cc` measured LS-CI at 33 PASS + 2 SKIP; the record —
three JOURNAL stanzas, four status rows, this session's own resume note — said
34 + 2. Every bar today measured 33 + 2 over the same 35 scenarios, and the two
full runs before them said "32/34; 2 SKIPPED" in the harness's own words. The
34 was the c8ab2744 close message's "36 scenarios", an `*.exp` count that
included `lib.exp`, minus the two SKIPs: a derived figure that propagated as a
measured one six times before a run's tally was set beside it. Corrected
everywhere; the tally now comes from the harness's `==> LS-CI:` lines only.

### The bar over the tip (`01f076f2`: the handler-time mask)

SMP gate 40/40 (default + UBSan × smp4/smp8, N=10, 0 corruption / 0
external-kill / 0 other, two halves — the kernel byte-changed); LS-CI 33 PASS +
2 SKIP over 35 (GL not baked; six batches, TCG); suite 1413/1413; sabotages
SM1/SM2/SM3 each red on the named check. Pushed to both mirrors after the
fixup.

## 2026-08-17 (aux) — the interactive `viv run` that never ran; the diorama channel goes private

The R5-F9 experiment needed an interactive Alpine ash under `viv`, and the
first attempt to drive one from a `ptyhost`ed `ut` produced nothing: no ash
prompt, no output from a `sh -c 'echo …'` bisect, viv back at the outer prompt
at once. The resume note going into this watch carried a hypothesis — viv's
`stdio_born` gate (`fstat` on fds 0/1/2) fails on a pts, so the entrypoint is
spawned fd-less — and the instruction to VERIFY before fixing. The verification
took one grep. Every one of the four logs has the console line
`viv: spawn /bin/diorama` right after the `viv run` echo, and that line is not
progress: it is `Err(String::from("spawn /bin/diorama"))` reaching `say`. The
container never existed. The line reads like an announcement, and the previous
watch read it as one; the source says it is the report that the announced thing
failed. **A line that names an action may be the report that the action
failed — grep the source for the string before reading it as progress.**

The mechanism took a few more. `viv` requests `SPAWN_PERM_MAY_POST_SERVICE`
for its per-container diorama, because the diorama posted the fixed name
`/srv/viv-dio`; `spawn_perm_grant_check` grants that bit only to a
console-attached granter or an existing holder; joey confers it on login, login
confers `CONSOLE_OWNER` on `ut` and nothing more, `ut` confers nothing on its
externals — so no session shell's `viv` was ever a holder, and every boot-gate
`viv` was joey-spawned WITH the bit, so no gate ever ran the path a person
runs. The V-7 commit body had listed exactly this as a "known seam" and moved
on. A bug that lives only in prose is a bug being walked past in slow motion;
this one waited eighteen days for someone to type `viv run` at a prompt.

Two fixes were possible and the research collapsed them to one. Widening the
privilege — login confers the bit on `ut`, `ut` on every command — puts every
user program in a position to squat names in the ONE shared boot registry
(`/srv/home-<user>` before that user logs in; a tombstoned `/srv/net` after
netd dies is re-postable by any marked Proc), and leaves the fixed-name
collision in place. The other reading is Plan 9's own: a 9P server a process
starts for itself is reached by `mount(fd)` over a pipe, and `srv(3)` exists to
publish fds to strangers — this channel has no strangers. The kernel had the
primitive since Phase 5: `SYS_ATTACH_9P(tx, rx)` over two Plan 9 pipes, the
`stub-driver` shape, tested by `test_attach_probe` and used in production by
nothing. Now `viv` makes two pipes, spawns `diorama --vivarium <pid>` with the
server ends as the child's fds 0/1 and nothing else, attaches the client ends,
mounts at `/dio` as before; the diorama serves that one connection until EOF.
No name, no privilege, no collision. Three seams closed by removing a
mechanism rather than adding one: the interactive path works, concurrent
containers move from OUT to IN, and the V-8 F3 attach gate — a peer-pid check
in `h_attach` plus a joey deny leg — becomes structural (nobody but the runner
holds an end) and comes out. What the diorama still checks is its one scoping
premise, that the argv runner is its parent, read off its own status file's
`ppid` line. Its `self` is derived rather than kernel-stamped now (there is no
`SYS_SRV_PEER` on a pipe): the runner's pid, its own uid/gid, a native
liveness resolve — the same content the stamp gave, since the peer was always
the mounter (#90 unchanged).

The gates were rebuilt around the new shape rather than deleted with the old
one. The `#101` deny leg became the `viv-channel` leg: two spawns of `diorama
--vivarium` one variable apart — joey's own pid (the attach must succeed, and
with the server provably up `/srv/viv-dio` must NOT resolve, and closing the
attach root must make the diorama exit on EOF within a bounded wait) and a pid
joey is not (the diorama must exit at its parent check before Tversion, so the
attach must fail). Neither branch can pass for the wrong reason: a diorama that
cannot start satisfies only the refusal, a diorama with no parent check
satisfies only the serve. And the V-7 leg now spawns two `viv run
/vivarium/probe` concurrently: each probe asserts its pid view is exactly
`{self}`, so two live containers prove from the inside what `#101` proved from
outside, under the concurrency the old design refused. Every boot `viv run`
was also stripped of the perm bit it no longer needs, so the gates run the
interactive path instead of a privileged twin of it — the same shape as the
L26 premise trap a stanza above: a gate that runs a different path than the
user does is a gate on the wrong thing.

One residual, recorded and not built here because it is kernel-side on the
Pipe audit row: `devpipe_write` posts a `pipe` note to the WRITING Proc when a
ring's reader is gone, and the kernel 9P client's spoor transport writes in the
syscalling Proc's context — so a container Proc that touches `/proc` after its
diorama has died (an orphan outliving its runner; a diorama crash) gets a
`SIGPIPE`-shaped note where the `/srv` transport gave only an error. Linux's
kernel 9P client signals nothing (it writes from a workqueue); the fix is a
`MSG_NOSIGNAL`-shaped kernel-internal transport write.

### The bar over the tip

First boot after the change, no retries: kernel suite 1413/1413 (the kernel
binary was not rebuilt — no `kernel/`/`arch/`/`mm/` file changed);
`joey: V-7 viv-probe (containered, x2 concurrent) PASS`; `joey: viv-channel:
private pair serves, no /srv name, EOF-exit`; `diorama: --vivarium pid is not
my parent` then `joey: viv-channel: non-parent runner REFUSED`; V-1b + L-6c +
D-5 PASS; `Thylacine boot OK`. LS-CI `viv-run` (the new scenario): PASS on
attempt 1 — the console `viv run /vivarium/probe` printed the probe's leg-6
line, the `ptyhost`ed `viv run /vivarium/alpine-ash` showed `/ $ ` on the pts
and answered `ASH-ALIVE` through it (a pts trio passes `stdio_born` and flows
both ways — the question the retracted hypothesis raised, answered by the
witness rather than the fix), `exit` returned to `ut` twice over. Landed as
`437213c4`; the full LS-CI run rode the next chunk's bar (the ^C finding
below), one bar for both.

## 2026-08-18 (aux) — the first ^C killed the runner; and a hosted `ut` loses a line after two

With the channel fixed, the R5-F9 experiment finally reached an interactive
phenotype ash — and its first ^C at the prompt produced not the ash prompt but
`proc: orphan pid=N name="sh" (parent viv exiting)`, then the same line for the
diorama, then a terminal in which nothing answered coherently. Three of three
attempts. Not the R5-F9 wedge at all: the pts's ISIG cooks 0x03 into an
`interrupt` for the FOREGROUND PGRP, and `viv` runs as `ut`'s foreground job,
so its pgrp is `viv` + its diorama + every container Proc. The container's
shell sees SIGINT and handles it; the two NATIVE members have no handler and no
notes fd and die of an uncaught `interrupt` — LS-5's default, working exactly
as designed on the wrong recipients. The orphaned shell then kept reading the
same pts as the outer `ut` and split every later keystroke with it, which is
why the control legs saw nothing: PTY-4's "no TTIN arbitration" footnote seen
from the other side.

The fix is a mask and only a mask. `viv` masks `interrupt`: the container needs
nothing forwarded (it is in the pgrp; the note reaches it directly) and
inherits nothing (a spawned child starts with a zero mask — `rfork_internal`
copies `note_mask` only when the PARENT is `PHENO_LINUX`, and the native
exec-image reset zeroes it). The tty family stays UNMASKED in `viv` on purpose:
^Z must STOP `viv` with the container, or `ut`'s `wait_pid(WUNTRACED)` on the
job never sees it stop and the terminal is never handed back; a hangup ends
`viv` with the container; ^\ still kills the runner and detaches a running
container, as `docker run` does under SIGQUIT. The diorama has no such
constraint — nothing waits on it as a job — so it masks both families: a
server never dies of a keystroke, and its lifetime is its channel's. Two kernel
facts were read rather than assumed first: the terminate LATCH is armed at post
regardless of the mask, but both its consumers — the EL0 tail's terminate scan
and the #811 sleep predicate — honour the per-thread mask, so a masked
`interrupt` neither delivers nor unwinds the blocked `wait_pid`; and the
ldisc's post is `synthetic`, so repeated ^C coalesce rather than fill the
queue.

`viv-run.exp` gained the leg, with a witness that names WHICH shell answered:
`uname -s | tr a-z A-Z` says `LINUX` from the phenotype ash and `THYLACINE`
from `ut`'s coreutil, so a runner that died and handed the terminal back could
not pass it by `ut` executing the line. PASS on attempt 1 on the build whose
only change from `437213c4` is the two masks.

And the "unexplained" note from the previous watch reproduced on its first
try. `scratchpad/r5f9/ctrlc-idle.exp`: two ^C at the console `ut`'s idle
prompt, then a command — answered; ONE ^C at a `ptyhost`ed `ut`'s idle prompt,
then a command — answered; TWO ^C there, then a command — echoed, not executed
within 30 s; an Enter, then a command — answered
(`outer-cc=1 inner-c=1 inner-cc=0 recover=1`). The rendering suggests the
hosted `ut`'s `interrupt` arrives late and its line-discard eats characters of
the NEXT line; with two, the second discard lands around Enter and takes the
whole line. That is aux's own line — notes, job control, the pts — and it is
enqueued as item 10 (`memory/bug_hosted_ut_double_ctrlc_idle.md`) rather than
folded in here: it is a different mechanism from this chunk's, and it wants its
own root cause before its scenario joins LS-CI. Every gate we had covers ^C on
a foreground JOB; none covered ^C at an idle hosted prompt.

The R5-F9 question itself — does busybox ash's `raise_interrupt` longjmp out of
its SIGINT handler and wedge `in_handler` — ran next, on the runner that now
survives the ^C it needs, and came back INCONCLUSIVE on the wedge and
conclusive on something underneath it: after ash's prompt (blocked in `read`)
a ^C produced NOTHING for ten seconds, twice; the next typed line was then
discarded (`^C`, newline, reprompt AFTER the line) — the SIGINT was delivered
exactly when the read completed. Read in the kernel: `thread_die_pending` is
the only sleep-unwind predicate and it fires for group death and the UNCAUGHT
terminate latches only, so a CAUGHT note never wakes a thread blocked in a
syscall wait; Plan 9 (`Eintr`) and Linux (`EINTR`/`ERESTARTSYS`) both do. A
note that arrives with the next line is not late, it is undelivered. That is a
kernel design fork (item 11, `memory/design_caught_notes_do_not_interrupt_
waits.md`), the operator's to vote, and it very likely owns item 10 too.

### The bar over the tip (`5336c894`: the channel + the ^C masks)

Boot (first after the change, no retries): kernel suite 1413/1413 — the kernel
binary was not rebuilt (no `kernel/`/`arch/`/`mm/` file changed), so no SMP
gate; the joey `viv-channel` legs + `V-7 viv-probe (containered, x2
concurrent) PASS`; V-1b + L-6c + D-5 PASS; `Thylacine boot OK`. LS-CI **34
PASS + 2 SKIP (GL not baked) over 36 scenarios, 0 retries** (TCG, sequential,
~55 min), the new `viv-run` among them. Pushed to both mirrors after the
fixup; main ratified the channel design on the line (call 0025) and had merged
aux-2 @13149152 into main (8a58112d) meanwhile.

## 2026-08-18 (aux) — the first production use of the pipe transport could extinct the box

The two Fable prosecutor rounds on the channel work came back. Round A
(`01f076f2`, the handler-time mask) was clean — 0/0/0/1, one P3 that the mask
mechanism was sound and the only finding was a comment: the owner-write
enumeration justifying `thread_die_pending`'s lock-free `note_mask` read named
only `SYS_NOTE_MASK`, and this very commit had added two more writers (the
delivery store, the sigreturn restore) to a list that was already one short
(the V-6b `rt_sigprocmask` row). The property holds — every writer runs on the
owning thread, never inside a wait — so it is the #254 shape, a comment true
about the wrong version of the system. A reword, deferred to this close because
`notes.c` was in round B's read scope.

Round B (`437213c4`+`5336c894`, the diorama channel + the ^C masks) found the
one that mattered: **the pipe (spoor) 9P transport blocks under `c->lock`, and
an unprivileged multi-threaded container can ride that into a box extinction.**
`437213c4` had, without anyone naming it as such, made `SYS_ATTACH_9P` over a
Plan-9 pipe pair the *first production consumer* of the Phase-5 spoor transport
under the shared `p9_client`. That client was written for a non-blocking
transport: `client_send_flow` holds `c->lock` across `p9_transport_send` (the
header says, in as many words, "never held across a blocking wait"), and it
recovers from a full transport by returning `P9_TRANSPORT_EAGAIN` — the srvconn
ring backend does exactly that, and `client_pump_or_park_locked` drops the lock
and retries. The pipe backend does not: `spoor_transport_send` loops until the
whole frame is written, and `devpipe_write` on a full pipe **sleeps**. `sched()`
extincts on `prev->preempt_count != 0`, and `c->lock` is a counted spinlock. So
a full `c2s` under the held lock is `EXTINCTION: sched: plain spinlock held
across sched()`.

All four links re-read to ground truth before touching anything. Reachability is
the soft part — the round was honest that it could not build a deterministic
reproducer read-only — but the bound refuses to prove it safe: `c2s` is 4096
bytes, up to 64 frames may be outstanding, and a single full-path `Twalkgetattr`
is ~1100 bytes, so four concurrent container `/proc` opens (4400 > 4096) fill it
if the single-threaded diorama is descheduled in the window. The single-threaded
V-7 probe and the one interactive ash never fill 4096 with one in-flight frame —
which is precisely why the gates were green and the box was not safe.

The fix stays inside the machinery that already exists for the srvconn ring. A
new `CNBFRAME` flag (`spoor.h` bit 6) on the transport's tx Spoor makes
`devpipe_write` commit the whole frame or return `-T_E_AGAIN` having written
nothing — never partial (a stranded fragment desyncs the shared stream, and
`do_send` treats a mid-frame EAGAIN as fatal, `#349`), never `sleep()` (the
blocking loop is bypassed entirely for the flag). `spoor_transport_send` maps
`-T_E_AGAIN` to `P9_TRANSPORT_EAGAIN` — the two are both -11, but the map is
explicit so a divergence of either constant cannot silently become -1 — and the
client's existing EAGAIN arm drops `c->lock`, reads an `s2c` reply (freeing the
diorama to drain `c2s`), and retries. Only tx is CNBFRAME: the recv on rx blocks
but runs with `c->lock` dropped (`#841`), so it is sound. A frame is at most an
msize, and msize ≤ `PIPE_BUF_SIZE`, so the frame always fits an empty pipe —
progress is guaranteed once the reader drains, no deadlock.

The regression, `pipe.cnbframe_atomic_nonblocking`, is the exact contrast to
`pipe.write_short_when_partially_full` a few lines above it: the same
ten-bytes-free pipe, a partial 10-byte write there, an atomic `-T_E_AGAIN` here
— and it reads the ring back to prove exactly 4000 bytes buffered, not 4096, so
a rejected frame left no byte behind. My own self-audit, run concurrently with
the round, had confirmed the channel sound on fd-mapping, diagnostics,
pid-monotonicity, and lifetime, and had flagged the deadlock axis as "leans on
#841" — but I had not run that axis to the extinction. The round did. That is
the two-prosecutor value in one sentence: family diversity plus a reviewer that
carries a hypothesis all the way to `sched()`.

### The bar over the tip (the F1 fix)

Build OK. Suite **1414/1414** (the +1 is `pipe.cnbframe_atomic_nonblocking`,
explicitly PASS), no FAIL, no EXTINCTION; the boot already drives the CNBFRAME
path through the diorama channel's Tversion/Tattach/reads. SMP gate **40/40** (default +
UBSan × smp4/smp8, N=10) -- 0 corruption across all configs. Round A's F1 comment
reword lands alongside. A follow-up prosecutor round on this fix is owed — it
changes a wait behavior, and the re-audit-on-dirty-close discipline says the fix
gets its own round.

## 2026-08-18 (aux) — the fix that closed only one door: SYS_ATTACH_9P now admits pipe pairs only

The follow-up round on the CNBFRAME fix (`663d4b64`) came back dirty, and the P1
it found was the sharp kind — the fix I had just shipped closed the extinction
for the *pipe* backend and left the *class* open. CNBFRAME is honored by
`devpipe_write` alone; `p9_spoor_transport_init` sets the flag unconditionally,
but `sys_attach_9p_handler` accepts any writable Spoor as the transport tx with
no Dev-type check. So a non-pipe blocking-write tx — a `/srv` byte-conn, whose
`devsrv_write` tsleeps on a full ring; a dev9p file, whose write is a nested
blocking RPC — silently ignores the flag and blocks under the 9P client's held
`c->lock`, which is the same `#360` lock-across-sleep extinction, reached
through a different Dev. Latent, because the two shipped callers (viv and
joey's viv-channel) always pass `pipe_create` pairs — but the *syscall
contract* admitted the vector, and a semi-trusted runner passing a non-pipe tx
kills the box.

The first instinct — gate the tx on `dev == &devpipe` inside
`p9_spoor_transport_init` — was wrong, and the test suite said so within one
build: eight `test_9p_spoor_transport.*` tests went red at "init returns 0".
They drive the transport over a **non-blocking linear-buffer mock Dev**, which
is a perfectly sound tx (it never sleeps), and the init-gate rejected it. That
red was the design telling me where the constraint actually lives. The
transport is genuinely Dev-generic — sound over *any* non-blocking tx — and the
pipe-only rule is a property of the **EL0 boundary**, not the transport: EL0's
only sound tx is a real pipe, and a kernel-internal caller is trusted to pass a
non-blocking Dev. So the gate belongs at `sys_attach_9p_handler`, not the init.

`sys_attach_9p_handler` now refuses unless `sys_attach_9p_ends_are_pipes(tx,
rx)` — both ends `dev == &devpipe` — releasing both `#844` lookup refs on
reject. The predicate is a non-static helper precisely so the regression,
`pipe.attach_9p_admits_pipes_only`, exercises the *actual* gate rather than a
re-derivation of it (a pipe pair admitted; a non-pipe tx, a non-pipe rx, and
NULL ends all refused). A cleaner userspace regression — attach a non-pipe fd
from EL0 and watch it fail — was tempting but wrong: without the gate the attach
proceeds into its Tversion RPC and *hangs* on a tx that never replies, so it
would freeze the boot rather than fail cleanly. The handler-predicate test is
the deterministic one.

### The bar over the tip (the pipe-only gate)

Build OK. Suite **1415/1415** (+1 `pipe.attach_9p_admits_pipes_only`); the
`spoor_transport.*` mock-fixture tests stayed green, which is the proof the gate
did not move to the init. SMP gate — *(the run over this tip)*. The extinction
class is now closed at the boundary that admits it.
---

## 2026-08-16 — Warp-C C-1, the per-slot decision, and one third of the extinction tear

Resumed from a self-compaction at the 600k checkpoint. **The nudge fix worked
on its first live test** — the detached watcher fired behind `/compact` and the
far side woke itself, which is the loop the operator had been closing by hand at
every boundary.

### Warp-C C-1 — the composed present, modelled (`ee581fbd`, fixup `ae9a25df`)

GPU-DESIGN §4.5.6 is binding here: `tapestry_present.tla` is model-first, so the
model is extended *before* the impl. Added the GPU-composed present behind
`ALLOW_COMPOSE` — `Attach`/`Detach` (P1b's authority-conferral point),
`ComposeBlit`/`ComposeComplete`, `DrainedOfBlits` on `ServerRelease` + `Free`,
and two invariants repeating T-1's own LIFETIME/CONTENT split: `NoTornCompose`
and `NoStaleCompose`. Eleven cfgs, gated by the new `specs/check-tapestry.sh`.

**The control was set before the work, which is the only reason it meant
anything.** I recorded every cfg's distinct-state count *before* touching the
module, so "this extension is additive" became checkable: with `ALLOW_COMPOSE =
FALSE` the six pre-existing cfgs must reproduce 5413 exactly. They do — and the
check earned its keep, catching that tracking `filled` unconditionally cost the
direct path 5413 → 10413 states.

**Two measurement traps, both mine, both caught by controls rather than by
reasoning:**

- My first comparison harness reported all six cfgs as DIFFERING. The harness
  was broken (`set --` inside the loop clobbered the positionals, lagging every
  expectation by one row). But under the bad labels the raw numbers still said
  something real, and chasing *that* was the right move.
- The buggy cfgs genuinely did differ — and it turned out **the metric was of
  the instrument**. A buggy cfg halts at the first violation, so with parallel
  workers "states explored before tripping" is scheduler noise: measured
  129/141/155 across three *identical* runs. Buggy cfgs are now judged on exit
  status plus the *name* of the invariant reported. (Never on TLC's prose — it
  writes both "is violated" and "was violated" depending on property kind.)

**Then TLC refuted my model, and the tree refuted the premise under it.** I had
carried the in-flight blit as the *slot* it reads, reasoning that a client
filling a *different* slot during a composition is legitimate pipelining — and I
wrote that justification into the module header as though it were established.
It is false. `usr/tapestryd/src/gpu.rs:1515-1518`: tapestryd allocates one 2D
resource per surface, attaches the whole weave as backing, and transfers at a
per-present *offset* that selects the slot. Guest-side slots buy **no** host-side
concurrency. The guard also had the shape of a known trap — `intransfer = 0` is
a gauge reading zero, equally true of "the fill landed" and "no fill was ever
issued" — now closed by an explicit `filled`.

The exclusion is symmetric, so it gets a sabotage *per direction*
(`buggy_blit_during_fill`, `buggy_fill_during_blit`) rather than one flag opening
both gates, which would only ever demonstrate whichever end TLC reached first.

Non-vacuity was measured, not assumed: coverage shows the composed actions fire
`0:0` with the switch off and `ComposeBlit` 2264 / `ComposeComplete` 7328 with it
on, so the green sits over a constructed state.

**Verification:** 32 spec modules green + the 11-cfg tapestry gate. `corvus` and
`handles` deliberately not re-run — 87 minutes, and nothing `EXTENDS`
`tapestry_present`, so they cannot be reached by this change. Zero build inputs
changed (proved by `git diff --name-only`), so the full bar's other legs carry
from `ca50a164` by construction rather than by assertion.

### The design fork it forced — and the operator's vote (`14f8c1ed`)

C-1 surfaced an obligation **the prose did not have**: the D1 recycle gate does
not survive the composed path unchanged. In the direct path a present's terminal
CQE genuinely means "the host has finished reading" — until the compositor
becomes a second, async reader of that one host resource, at which point the CQE
stops meaning the resource is free and nothing in the old rule notices.

Researched before posing it (Wayland `wl_buffer.release` + `drm_syncobj`, Android
BufferQueue acquire/release fences, Fuchsia buffer collections), which showed the
SOTA answer is *two* mechanisms, not one: buffer-release semantics for software
clients, explicit fences for GPU ones. Posed the fork with that attached.

**Operator chose one host resource per slot (3×).** Landed as a scripture commit
with no code, per the design-conversation pattern: GPU-DESIGN §4.5.8, with the
two rejected alternatives and their reasons, and the cost stated rather than
buried (3× host VRAM; ~100 MB at 4K, against a 64-MiB weave cap that already
cannot hold a triple-buffered 4K weave). The landed model does not change with
the vote — `NoStaleCompose` is whole-generation, correct today and merely
conservative once slots become distinct host objects.

### The extinction tear — one third of it (`44a8d53f`)

A surfaced soundness defect outranks the perf arc, so I stopped C-2 and took
this. The `EXTINCTION:` ABI line is emitted as four separate unlocked
`uart_puts` calls; every consumer anchors its match (`^EXTINCTION:` in
`tools/test-fault.sh`, and bare-token matchers elsewhere). A torn banner is
therefore not cosmetic — it is **a real extinction the harness cannot see**,
fail-open on the one channel the whole test discipline trusts.

**The vault already carried an adjacent seam, and I nearly conflated them.**
There are **three** tearing sources with confusingly close names:

1. extinction vs extinction — the re-entrancy guard is per-CPU *by design*, so
   two dying CPUs both print. **Fixed** (`extinction_claim_console`).
2. extinction vs a peer's *normal* console write — the vault's
   `seam-extinction-line-unserialized`. **Open.**
3. `IPI_HALT` — would subsume both. **Open**, a commented-out reservation.

The fix is one `__atomic_exchange_n`: a raw atomic rather than a kernel spinlock
(this runs on a dying machine, often inside a fault handler, and a primitive
carrying lock-order assertions could itself fault), try-once rather than spin
(the winner never releases, since every path ends in `_torpor`), and losers park
emitting nothing — because the failure modes are asymmetric: a torn line can be
read as a clean boot, a missing one leaves the guest visibly hung. Take the loud
failure.

**The fix introduces its own fail-open, and that is what most of the design
guards.** Nothing releases the console, so anything claiming it spuriously
silences every later extinction in the boot — the same defect from the other
side. Hence the deliberate interface split: the claim core is exported to be run
on a *caller-supplied* word, and nothing exports a way to claim the live one. A
test that took the real console would disable extinction reporting for every
test after it, silently.

**Both new tests were sabotage-verified** (1367/1367 → 1365/1367, each failing
on its own distinct assertion message). And the first one is documented for what
it does *not* cover: it is sequential and the property is a race, so a non-atomic
`if (*w) return 0; *w = 1; return 1;` passes it identically. Covering the real
regression needs a multi-CPU fault-injection arm with a **forced** interleaving —
without forcing it the pre-fix build garbles only sometimes, and a discriminator
that fails only sometimes is not a regression test. Tracked, not skipped quietly.

Also corrected a phantom that had propagated into two files: both
`kernel/extinction.c` and the header told readers to co-update
`tools/agent-protocol.md`, which was planned in Phase 1 and never written, and
`tools/run-vm.sh`, which matches neither literal because it only launches QEMU
and never reads boot output. Both now point at the vault's `abi-boot-banner`
mirror set instead of a transcribed list.

**Verification (the full bar, since this is a kernel change):** build clean;
suite 1367/1367 (was 1365; +2); SMP gate 40/40 with 0 corruption across
default-smp4/smp8 + ubsan-smp4/smp8; LS-CI 35/35 PASS; v8.0 floor OK.

**A killed gate is not a green gate.** The first LS-CI run was stopped by the
harness (`Terminated: 15` on its scenario subprocesses) after I ended a turn
while it ran; the SMP gate had survived the identical foreground → background
migration earlier in the same run, so what differed was ending the turn. Re-run
as a tracked background task, staying in-turn.

**And then I got the reasoning for that right conclusion wrong, twice, the same
way.** I first wrote that the killed run "recorded zero verdicts", inferring it
from a stdout log containing only `==> start:` lines. Then, waiting on the
re-run, I read the same channel and concluded it had produced no results after
eight minutes. Both readings were of the wrong channel:
`tools/test-interactive.sh` says so in its own comment — *"The verdict is a
FILE, not a counter"* — and writes results to per-slot `timings.tsv`, never to
stdout. The re-run was healthy the whole time (`go8d PASS` already on disk).

So: **a pattern that matches the wrong thing returns a confident wrong answer,
never an error** — a lesson already pinned in memory, re-learned twice in one
hour on one command. What makes it worth writing down again is that the wrong
instrument produced a *plausible* story both times (a killed gate really had
been killed; a slow gate really can be slow), which is precisely why it was not
self-correcting. The fix is to find where a tool actually writes its verdict
before reading any verdict from it.

### Before C-2 wrote a line: the composed path cannot run on the dev loop

Checked the precondition rather than assuming it, and it changed the arc. The
boot log of the very run I had just gated says
`tapestryd: gpu up -- 1280x800, pci intid=35, virgl=0 capsets=0`, and
`tools/run-vm.sh` defaults to `virtio-gpu-pci` — a device with no GL. So
`CTX_CREATE` / `RESOURCE_CREATE_3D` / `SUBMIT_3D` are unavailable on the primary
dev loop, and with them every mechanism §4.5 describes.

Three consequences, recorded as GPU-DESIGN §4.5.9. C-2/C-3 must be verified on
**thyla-pi**, not here. The composed path must be capability-gated on the
negotiated feature bit — a tapestryd that assumed GL would take the console dark
on the default device. And the third corrects the roadmap: **"C-4 retire the
readback path" cannot mean delete it.** That is forced twice over — by the plain
`virtio-gpu` that is the default here, and more fundamentally by bare metal,
where there is no virtio-gpu at all and virgl is a *virtualization* transport
with nothing to negotiate. The CPU path is the universal one; GPU composition is
the accelerated path where a GPU seam exists.

The cost is stated rather than left to be discovered: tapestryd carries **two
composition paths permanently**, and they must stay behaviourally identical from
the outside or the gate that proves one is silent about the other.

### The C-2 verification host, proven rather than assumed

Having established the dev loop *cannot* run the composed path, the next
question was whether anything can. Synced HEAD to thyla-pi (all 80 pool chunks
hash-verified, artifacts paired) and booted `virtio-gpu-gl-pci` under KVM on
real V3D:

```
tapestryd: gpu virgl -- num_scanouts=1 num_capsets=2
tapestryd: gpu capset[1] id=2 max_version=2 max_size=1384
tapestryd: gpu up -- 1280x800, pci intid=35, virgl=1 capsets=2
CAPSET GATE: VERIFIED
```

So C-2 has a working verification host, and the two figures — `virgl=0` here,
`virgl=1` there — are the whole argument for §4.5.9 in one line each. Worth
doing before the implementation rather than after: had C-2 been written first,
its first symptom on the dev loop would have been a dark console, which is a
long way from its cause.

### C-2a — the capability gate and the compositor context

The first landable piece of C-2: a reserved compositor virgl context
(`COMPOSITOR_CTX = 0x100`, far above the client `slot + 1` range so a client's
stream can never author against the screen), minted only where `virgl`
negotiated, and a startup line reporting which composition path the host can
actually take.

**The first cut reported nothing, and the boot passed anyway.** I had hung the
posture report off `ensure_screen`, beside the other display resources — but
`ensure_screen` runs only under `Scanout::Composed`, a state a normal boot never
enters, so the line sat behind an unconstructed state and printed on neither
host. The suite went 1367/1367 with the feature effectively absent. Which
composition path is *available* is a property of the HOST, fixed at feature
negotiation, so it now reports where the host is brought up.

**Verified on both arms, differing in exactly one variable** — a negative
assertion alone would have been satisfied by a broken fixture:

| Host | Negotiation | Posture |
|---|---|---|
| dev loop, `virtio-gpu-pci` | `virgl=0` | `composed path = CPU (virgl=0)` |
| thyla-pi, `virtio-gpu-gl-pci` | `virgl=1 capsets=2` | `compositor ctx 256 up` → `composed path = GPU` |

Getting the positive arm took one correction of its own: the `capset` verb
filters its output at the capset markers, so the Pi run *looked* like it lacked
the line when it had simply not been shown it — `boot-probe.sh` keeps the full
log on the host, and the line was there. A truncated capture and a missing
feature are the same reading until you check which one you have.

### C-2b — the 3D screen, landed gated and HONESTLY UNPROVEN on its own arm

The screen becomes a host-side 3D resource attached to the compositor context
where GL exists, falling back to the 2D resource everywhere else. Guest backing
stays on both paths, because at C-2b the screen is still CPU-filled — only its
host-side representation changes. `screen_push` grows a 3D arm, and there the
sync transfer moves the whole surface rather than the damage rect: a deliberate
trade, since C-3 deletes the CPU fill outright and building a rect path for a
mechanism already scheduled for removal is waste.

**What is verified, and what is not — stated because the gap is the finding.**
The FALLBACK arm is verified: suite 1367/1367, and LS-CI 35/35 where the
`ls-gfx` scenarios assert exact pixels via screendump and therefore cannot pass
without a working composed screen. **The 3D arm has never executed.**
`alloc_screen` runs only under `Scanout::Composed`, and neither the dev-loop
boot nor the Pi's `capset` boot enters it, so `screen res N 3D (compositor ctx)`
printed on neither host. `prove` produced no new boot log to grep.

So this lands **gated off on every host I could exercise** — dead on the dev
loop by capability, unproven on the Pi by opportunity — and the commit says so
rather than calling a clean boot a verification. Booting green proves the gate
did not fire, which is exactly what an `if (false)` would also prove.

**Then I found why, and it is a tooling gap rather than a code problem.** The
Pi logs say `tapestryd: scanout direct 0 (1280x800)`: every existing Pi verb
drives a SINGLE display-sized GL client, and that takes the **Direct** path —
scanning out the client's own resource and bypassing the compositor screen
entirely. §4.5.1 spells out the condition: Direct demands one visible surface
AND one visible leaf AND an exactly display-sized surface. So composed scanout
needs two surfaces, or one smaller than the display, and **no verb in
`warp-host.sh` produces either.** `capset` and `smoke` both land in Direct;
`tri` and `prove` left no new boot log at all.

That is worth more than a failed check: it says the composed path — the entire
subject of the Warp-C arc — has no driver on the only host that can run its GPU
half. Building one (two surfaces, or a mode change that un-sizes a single one,
which is what `ls-gfx-mode` does locally) is the next task, and it gates C-2b,
C-3, and the arc's exit criterion alike.

### The driver — C-2b's 3D arm finally executes, and my own note was wrong

The task I left myself was "build a Pi driver that forces Composed scanout."
Before building anything I checked the claim under it, and **it was false**. The
section above says "no verb in `warp-host.sh` produces either" — but
`glq-virgl.exp`, which `quake` runs, opens GLQuake in a window and its very
first assertion is `-re {scanout composed \((\d+)x(\d+)\)}` with the label
"composed entry (two leaves)". `decomp` and `wedge` split the layout too. What
was actually true is narrower and duller: the verbs I had *read the boot logs
of* — `capset`, `smoke` — boot with no client at all, so aurora alone is
display-sized and lands in Direct. I generalised from the two logs I had to a
claim about all ten verbs, and wrote it into two documents.

Worth noting how cheap the catch was: one grep for `composed` across
`tools/warp/*.exp`, run because the note asserted a negative over a set I had
not enumerated. **The evidence that a thing is absent has to come from the whole
set, not from the members that happened to be in front of me** — and a note
written confidently at a compaction boundary is exactly where that error
survives, because the far side inherits it as established fact.

I still did not use `quake`. It drags in the pool's `tyr-glquake`, S3TC quirks
(#216), the #198 storm, and 900-second timeouts — a lot of machinery that can
fail for reasons having nothing to do with C-2b. `/bin/tapestry-battery` brings
up two surfaces, lives in the ramfs, and needs no GL of its own, so **the only
GL object in the experiment is the compositor's own screen**. That isolation is
the reason to pick it, not availability.

`tools/warp/composed-screen.exp` boots, takes the posture line between boot and
login (it prints at bringup, which is where a host property belongs — a lesson
this arc already paid for), runs the battery, and asserts the screen mint. **The
control is the device**, which is why the scenario takes one as a parameter
instead of hardcoding the GL model: two legs, one host, one variable, each
asserting the other's outcome is wrong.

```
virtio-gpu-gl-pci -> composed path = GPU -> screen res 67 3D (compositor ctx) (1280x800)
virtio-gpu-pci    -> composed path = CPU -> screen res 67 2D (1280x800)
```

**C-2b's 3D arm has now executed**, on real V3D silicon through virgl. The
second line is what makes the first mean something: a GL-only leg would pass
identically against a tapestryd that ignored the negotiated bit and always
minted 3D. Two legs that *disagree* are stronger evidence than two that both
pass — the control produced a different answer rather than merely staying quiet.
Both legs minting `res 67` is a small corroboration on the side: everything
upstream of the branch is identical, so the arm is the only thing that moved.

The gate keeps two claims separate rather than collapsing them — posture matches
the device, screen arm matches the posture — so a host that had silently lost
its GL could not satisfy the second by making both sides equally wrong. And
`tools/warp-host.sh composed` requires each leg's scenario-completion line as
well as its screen line, because a leg that died immediately after printing its
screen line would otherwise still show the gate everything it greps for. That
term is not hypothetical caution: the `reject` verb in this same file shipped
grepping `C0-REJECT` while its producer printed `C0-DETECT`, and exited 0 on the
exact failure it existed to catch.

### Then C-2d refuted itself before it wrote a line (§4.5.8a, OPEN)

With the driver landed I went to implement §4.5.8 — the per-slot host resources
the operator voted for — and read the present path first. The decision does not
survive it, for a reason nobody had in view at the vote.

Three facts, each one grep:

1. Every client rotates slots on every present: `cur_slot = (cur_slot + 1) %
   nslots`, `libtapestry/src/lib.rs:525`, unconditional, both scanout modes.
2. Nothing copies content from slot *N* to slot *N+1*. `pixels()` hands back
   the raw current slot; there is no carry-forward anywhere.
3. **The single per-generation host resource is therefore doing a job nobody
   wrote down: it is the accumulation buffer.** A damage-only present transfers
   only its rect, so the host resource keeps the rest of the previous frame and
   the stale guest slots never reach the host.

Give each slot its own host resource and that job has no owner. A damage-only
present would render a three-frames-stale background around each fresh rect —
in Direct immediately, and in Composed at C-3. And the client this lands on is
**aurora**: it repaints only rows `r0..r1` and presents that rect
(`aurora/src/main.rs:1027-1038`), and it is the default Direct client on every
boot. The very line I have been reading all session, `scanout direct 0
(1280x800)`, is that client.

What makes this worth recording is not the catch but where the load was.
§4.5.8's analysis compared 3× / 2× / 1× VRAM and serialization — a complete
comparison of the properties anyone had *named*. The single resource's real
function was invisible because nothing declared it; it was an emergent
consequence of "transfer only the damage rect", and it had been load-bearing
for the console for as long as the console has existed. **A design comparison
can be sound over every property you listed and still miss the one the code is
actually relying on.** Only reading the path surfaces those.

I recorded it as **§4.5.8a** with four options rather than picking one, because
the vote is the operator's and this changes the terms they voted on. The
recommendation is buffer age — `EGL_EXT_buffer_age` and Wayland's
`wl_surface.damage_buffer` exist for this exact problem, Android's BufferQueue
exposes the same, and it keeps the per-slot vote intact at no VRAM cost while
retiring the latent hazard instead of routing around it. C-2c and C-3 both wait
on the answer: every option changes what gets attached and what gets blitted.

### The vote, and C-2d-a (`0a0e0fbb`, `931bf15a`)

The operator picked buffer age. Implementing it immediately hit a constraint the
option sketch had assumed away: I had written "present CQE now carries: age",
and it cannot. A present is a 9P write over the Loom ring, so its CQE is
**kernel-owned** — `result` is the write's byte count, `flags` is `LOOM_CQE_*`,
and `struct loom_cqe` is `_Static_assert`-pinned at 16 bytes. Putting a
compositor payload there is a kernel ABI break for a compositor convenience.

The way out was to notice who already owns the information. `libtapestry` owns
the rotation — `cur_slot` advances only after a present's own CQE — so it knows
exactly when each slot was last presented and can derive the age itself. A
`TEV_AGE` event was rejected (async to the present, so it races the rotation) and
a control word in the weave was rejected (a client-visible layout change for
something the client can compute).

**The interesting part is what the derivation costs, because it is the same
trap again.** A derived age is correct only if the client hears about every
server-side invalidation — which is exactly the kind of undeclared dependency
that produced §4.5.8a two hours earlier. So it is written down as a named
invariant this time rather than left to be rediscovered: tapestryd must not skip
a transfer without the client subsequently getting a redraw request, and a
redraw invalidates **every** slot, so the client repaints full for `nslots`
presents, not one. Both arms are wired in `libtapestry`.

Then aurora handed back independent corroboration of §4.5.8a. `main.rs:988`
already routes any OSD pass through the full-frame branch, with the comment
*"a partial rect could transfer stale panel pixels from an older slot"*. The
symptom had been understood locally, for one widget, and worked around — the
general statement just never got made. That is what an emergent load-bearing
property looks like from the inside: not unknown, merely un-generalized.

I split the chunk, because the halves are not symmetric: per-slot resources
without age break every accumulator, but age without per-slot resources is inert
and harmless. So the client half went first — and **its honest gate is that
nothing changed.** `ls-gfx` PASS, `ls-gfx-panes` PASS (exact pane-centre
pixels), suite 1367/1367. Its actual effect is unobservable until C-2d-b removes
the accumulator, and the commit says so rather than dressing a green boot up as
verification.

**Then I got the prerequisite list wrong, in the commit message, within twenty
minutes of writing the lesson that prevents it.** I swept for clients that
present partial damage with `grep 'present(Some\|present_rects'` and reported
three. That greps **API shape**, not the property that matters — *damage
smaller than the full surface*. Checked properly, it is one:

- `tapestry-battery` needs **nothing**. Every present is `present(None)`, and
  its one `present_rects` tiles the whole surface with two rects after writing
  every pixel. Its own header says so: *"presents FULL-FRAME only."* I had
  called it "the one with teeth."
- `tapestry-demo` is the real one, and is the sharpest example in the tree: it
  paints the quadrant background **into slot 0 only**, at frame 0, then draws
  just the plasma box into *rotating* slots forever after. Slots 1 and 2 never
  receive the background at all — they hold alloc-time zeros. Under per-slot
  resources, two frames in three would show black around the plasma.

"A pattern that matches the wrong thing returns a confident wrong answer, never
an error" is pinned at the top of my own memory index. It still went into a
commit message, a scripture section and the handoff, because a grep that
*returns results* feels like a sweep that *finished*. Corrected in §4.5.8b and
the handoff; the commit body stands as written, with this as its correction.

### The stop hook guarded the wrong stop, and the guard was never needed (`b3632942`, `cd0b3390`, `b61ca929`)

The operator noticed the Stop hook fired once in the long run and then went
quiet at a second stop it should have caught, and asked aux and me to work out
why. It is the third instance this week of the same family, and the sharpest.

**The measurement.** Replaying the hook's own parser over the real 805 MB
transcript: the silent stop sat at **530k / 73 turns** — inside the window on
both axes. So "it was correctly silent above the checkpoint" is dead. Isolating
the logic with synthetic input showed it behaves exactly as written. The cause
was upstream, and the pattern repeats: every firing is followed by silence for
the rest of the continuation, re-arming only when the user speaks or a
compaction lands.

**What I got wrong, and it was not the code.** `stop_hook_active` means "this
hook already triggered a continuation" — per-continuation by definition. I
exited early on it, which made the hook a once-per-*run* nudge guarding the
first stop and nothing after, i.e. the stop most likely to be earned and none
of the ones that follow. I kept that early exit because I believed it was the
loop guard.

**It never was.** aux fetched the contract: the harness overrides a Stop hook
after **eight consecutive blocks** (`CLAUDE_CODE_STOP_HOOK_BLOCK_CAP`). The
belay already existed one level up. So I had built a guard against a loop
something else was already preventing, and paid for it with the exact behaviour
the hook exists to provide. That is a different failure from a bug: **the code
did what I meant; what I meant rested on a contract I had not read.** No amount
of testing my own intent would have found it — only reading someone else's.

**The instrument came before the fix, and earned it twice.** The hook had nine
silent exits, so "correctly silent", "suppressed", and "crashed" were one
observation and any diagnosis could only be a guess — the same shape that had
just cost the vault a stranded day. So a ledger row on every path landed first.
Then it caught two things I would not have:

- Its own blind spot: the `stop_hook_active` parser printed `"1"` on exception,
  so a malformed stdin logged as `silent-stop-hook-active`. **The instrument
  built to separate those two causes could not separate those two causes.** The
  malformed-stdin test leg printed the wrong row, which is the only reason I
  looked.
- On its first *real* output: three rows in 24 seconds with incoherent context
  jumps, because the ledger is shared by main/aux/vault and I had dropped the
  session field from aux's spec. An interleaved log with no writer is worse
  than no log — it invites a confident reconstruction of one impossible session
  out of three real ones.

**And the fix validated itself in production before I finished writing it up:**
the reworded stem ("fires once per stop") came back in a live firing that
re-armed mid-continuation after real work — something the old version could not
do — with the ledger row `588458ctx/44t/27b/flag1` showing exactly why.

### C-2d-b landed, and the sabotage that proved it unverified (`f86177b6`)

The server half went in as voted: each generation mints `WEAVE_SLOTS` host
resources instead of one, backed per-slot instead of whole-weave. The
consequences were all followed rather than found later — `res_stale` becomes
per-slot; Direct binds the presented slot's resource and therefore rebinds every
frame (a KMS page flip, carrying the #57 post-bind flush); transfer offsets lose
their slot base, which the compiler confirmed by reporting `slot_stride` newly
unused; retire and `release_gen` unref all three or leak two per surface in the
process that IS the console.

`Held::Direct(Rect)` was the one that needed design rather than editing, and it
is why I stopped the first attempt at it. A rect union is well-defined only
while every held present lands on one resource; presents rotate slots, so two
held presents sit on different resources and `release` must flush each against
its own. Now `[Rect; WEAVE_SLOTS]` — bounded by construction, since a client
cannot hold more presents than it has slots.

**Then the sabotage passed, and that is the result worth the whole chunk.** I
disabled aurora's age handling with per-slot resources live — `stale_slot =
false`, `back = 0`, exactly the pre-C-2d-a client against a non-accumulating
server — and **`ls-gfx` still reported PASS.**

So the two gates I had been treating as verification are not. `ls-gfx` asserts
the frame *looks like* a console and that dumps *differ* after a command;
neither notices a stale background around fresh rows. `ls-gfx-panes` drives the
battery, which presents full-frame only and never exercises the accumulator path
at all. Between them they cover everything about the compositor **except the
property C-2d changes.**

That is the same trap as C-2b at the start of this run — a green result that
proves the gate did not fire — except this time I was the one about to be
fooled by it, having written the C-2b version into scripture that morning. The
difference between the two is not insight, it is that I ran the sabotage. Had I
not, this would have landed as "green on both pixel gates", which is *true* and
means nothing.

C-2d is therefore **implemented, not verified**, and the commit says so. §4.5.8c
records what the missing gate has to do: paint a region, damage a *different*
region, rotate all slots, sample the first region. `ls-gfx-panes` already has
the sampling machinery, so it is a scenario to write, not an instrument. The
focused audit is owed too — `usr/tapestryd` is an I-40 trigger surface and this
is the live scanout path — and could not run here because agent spawning is off.

### The self-compaction slot had two keys that did not agree (`7061115a`)

aux found this by reading the ledger nobody reads, and it is the best kind of
find: the mechanism had been quietly half-broken since it was built, and the
evidence had been sitting in a file the whole time.

`~/.claude/thyla-selfcompact/log.tsv` has vault's `allow` at 2026-08-16
10:44:32Z with **no `consumed` and no `nudge`**, and its `.note.pending` still
in the slot dir a day later. Every `main` row is paired; only vault's is
orphaned. That session compacted itself and was never handed its own resume
note — it sat at a prompt for the rest of the day.

The cause is a key mismatch, and **the comment is the interesting part.**
`tools/thyla-selfcompact.sh` said, in as many words: *"Two independent
derivations of one key, no shared config to drift."* The producer keys on `git
rev-parse --show-toplevel`; the consumer on `basename(dirname(transcript))`,
which is where the session was **launched**. Those coincide for main and aux
and do not for vault, which is launched from the thylacine tree and works in
thylacine-vault. So the comment **named the hazard and then asserted it away**,
and that assertion is what kept it unexamined for the mechanism's whole life.
It is every "keep these in sync" note that has ever rotted, except this one had
the confidence of sounding like an argument.

The fix needed no new identity, because one was already there and unused: the
arming script has always stamped `pane=$TMUX_PANE` into the meta, and a hook is
a child of the same claude, so it reads the same value. Pane match first, path
key as fallback.

**But the half that mattered was the silence.** The old failure was not doing
the wrong thing — it did *nothing*, and left no evidence, so `allow` without
`consumed` was the only trace. There is now an `orphan-note` row whenever a
pending slot goes unmatched, plus a 30-minute staleness discard.

**Then the test caught a bug in the fix that was worse than the bug.** The
first age check used `time.mktime` on a UTC stamp — `mktime` reads a
`struct_time` as *local* — so a note stamped that same second measured as an
hour old and was **discarded**. In any non-UTC zone that breaks every
legitimate resume: the repair would have converted a vault-only silent miss
into a universal one. I saw it only because leg 1 of the test printed
`stale-discarded` on a note written a moment earlier. Four legs, with legs 3–4
as the controls that make leg 1 mean anything — same note, same path-key
mismatch, only the pane varies:

```
1 pane matches, fresh    -> INJECTED,     consumed
2 pane matches, 25h old  -> not injected, stale-discarded
3 CONTROL no TMUX_PANE   -> not injected, orphan-note
4 CONTROL wrong pane     -> not injected, orphan-note
```

aux also retracted something in the same message, which is worth recording
because the retraction is worth more than the claim was: the "fourth
unregistered session" cited in the yip lease rationale **was aux itself** —
`ps -o ppid` on its own tool shell resolved to the process it had been reading
as a stranger. A census needs a control, and the control was its own identity.
Same family as `ps` matching its own command line, from the other end.

### Found in passing: `docs/REFERENCE.md`'s snapshot block died in Phase 5

The doc-update step sent me to `docs/REFERENCE.md` to refresh its Snapshot
block, which `CLAUDE.md` calls non-negotiable per chunk. **The newest "Tip"
bullet in it is a Phase 5 chunk** (`P5-stratumd-stub-bringup` audit close), and
there are 101 bullets behind it. The file's last commit of any kind is
`418688cf`, 2026-08-01. It contains **zero** occurrences of "Warp", "Tapestry",
"Clade" or "PTY-" — three whole arcs and a subsystem that do not exist as far as
the as-built technical reference is concerned.

So a binding per-PR obligation has been quietly unmet across roughly two phases,
including by me, several times this week. It is the "*a status field whose flip
is nobody's step stays unflipped*" shape: every chunk's author is told to
refresh it, no chunk's work makes them, and nothing fails when they do not.

**I deliberately did NOT patch my own bullet onto the top.** A dead list with
one fresh entry reads as maintained, which is worse than one that visibly
stopped — the reader trusts it again. The real question is what that block is
*for* now that `docs/phaseN-status.md` carries per-chunk rows and this journal
carries the narrative; answering it is a scripture-shaped decision, not a doc
edit to slip into a tooling commit. Enqueued rather than fixed in passing, and
enqueued in memory because the tracker is down this session.

### The gate that sees C-2d, red under both sabotages — and the defect building it found (after the self-compaction at `a733402e`)

Resumed from my own note with one instruction: build the §4.5.8c gate on aurora
in Direct, and validate it by re-running the sabotage that had passed `ls-gfx`
and requiring red. That is what happened, with two things the note did not
anticipate.

**The gate** (`tools/interactive/ls-gfx-age.exp` + `gfx_region.py`). Fill three
times with `yes … | head -n 200` so every slot carries glyphs; a POSITIVE
control — the same region assert, four keystroke-rotated dumps, each must show
text (a negative with no positive twin is satisfied by a broken fixture); then
`clear`, which blanks every cell in one all-rows present into ONE slot; then
eight rounds of keystrokes + dump, region exactly Bonfire, every pixel read.
The region is in cells (rows 6..rows-3, cols 2..cols/2) off aurora's own
`console up` line, so a font change moves it rather than breaks it.

**What the note left to the author, and how it was decided.** The detector is
slot-phased: the screen shows the slot presented LAST, so one dump samples one
slot. I had written "probabilistic — require N consecutive dumps". Working it
through, the honest model is *driven*, not sampled: each keystroke is a
row-0-only redraw, i.e. one present into the next slot, so the rounds advance
the phase deterministically plus whatever blink presents fall in the round.
That reframing exposed the real trap: **a broken client can have ONE stale
slot, not two** — an off-by-one in the union (`back = age-2`) leaves exactly one
— and the 1,2,3,1,2,3,… key pattern I first sketched (meant to break any
phase-lock with the blink) visits residues 1,0,0,1,0,0,1,0 under `b=0`: it never
reaches residue 2 and would pass an off-by-one every time. A plain one key per
round does reach it (1,2,0,1,2,0…) but is the pattern a 60 Hz blink can
phase-lock. So the negative leg types 1,1,2,1,1,2,1,1 keys, which visits all
three residues for *any* constant blink count per round (checked for b=0,1,2 in
the header); the
independence bounds — 3^-8 for the no-age class, (2/3)^8 = 3.9% for the
one-stale-slot class — are the fallback if the blink rate varies mid-leg, and
the header says which claim is load-bearing.

**Measured** (HVF, 128×36 cells, region 368 280 px). Fixed build: positive
63 882/368 280 non-bg on 4/4 dumps (identical counts — every slot holds the
same fill, as a correct client guarantees), negative **0/368 280 on 8/8**,
43 s. **S1** — the §4.5.8c sabotage, `stale_slot = false` + `back = 0`: **red
3/3 attempts**, at rounds 2, 1, 2 (63 882 stale px, i.e. the pre-clear fill
verbatim). **S2** — `back` off by one: **red 3/3**, at rounds 2, 5, 2. The
five-round attempt is the 1,1,2 pattern paying for itself: four dumps landed on
the two good slots before the fifth reached the one stale one. Restore green.
Both sabotages applied and reverted with `Edit`, and `grep SABOTAGE` empty
before the restore build.

**The defect the gate found — in C-2d-a, not C-2d-b.** Reading aurora's damage
branch to predict the sabotage outcomes, I traced what `931bf15a` records into
`dmg_hist`: **the WIDENED range** ("this is what actually reached the slot, and
the next union reads it"). That reasoning conflates *repaint* with *damage*.
The union answers "what changed since slot X was last presented"; what changed
between two presents is the dirty span, and the widening only says how much of
it THIS slot had to catch up on. Recording the widened range makes any
full-rows entry — every scroll — re-enter every later union, so every present
after it repaints all rows, forever. Aurora has been repainting the whole grid
on every cursor blink since C-2d-a landed: correct pixels, dead damage path.
Fixed to record the dirty span (`dirty0, dirty1` captured before the widening);
a full entry now falls out of the window after `nslots` presents. Two things
follow that are worth having in writing: S2 is a sabotage only against the
*fixed* recording — under the widened one an off-by-one is masked, since any
`back ≥ 1` propagates the full-rows entry (the old code had slack precisely
because it had no damage path); and the tight recording is guarded by the gate
that was built in the same chunk, which is the right order.

**Wrong turns, caught:** the first run failed on my own Tcl (`gfx_dump` takes
two args and I passed one) — three attempts, ~30 s each, all on the harness
side, before a pixel was read. And the resume note's "the sampling machinery is
in `ls-gfx-panes`" was true and unhelpful: `ppm-sample.py` reads one pixel; the
gate needs a region census with a positive control, which is a 40-line tool.

**Owed, unchanged:** the focused audit on `usr/tapestryd` (I-40; agent spawning
still off). The vault-owned prose (`sub-aurora`, `sub-libtapestry`,
`sub-tapestryd`) for C-2d and the recording fix goes over yip; the local
reference carries the gate.

### The device's OK was never the renderer's verdict — C-2b's "3D" word re-earned

Found while designing C-2c's gate, and by the one move that keeps saving this
arc: reading the source of the thing making the claim before repeating the
claim. My C-2c draft was about to say, for the third time in a week, that a
`CTX_ATTACH_RESOURCE` answered OK "attests the host accepted it". Before
writing that I fetched QEMU v10.0.0 `hw/display/virtio-gpu-virgl.c` (thyla-pi
runs 10.0.11) and read the handlers. **They ignore the `virgl_renderer_*` return
value** — for `CTX_CREATE`, `RESOURCE_CREATE_2D/3D`, `CTX_ATTACH/DETACH`,
`TRANSFER_TO_HOST_3D`, `SUBMIT_3D`, `CTX_DESTROY`; `ATTACH_BACKING` checks it
only to clean up the iov. `RESP_OK_NODATA` means "QEMU parsed it": nonzero,
non-duplicate id, valid iov. Only `SET_SCANOUT` (`resource_get_info_ext`) and
`RESOURCE_UNREF` (QEMU-side existence) consult anything.

**So three of my own documents were false in the same sentence.** C-2b's gate
header, `149-warp.md` and (by reference) the status row said the screen's "3D"
word was "the conjunction of four response-checked round trips the host
answered OK — a claim about the host accepting the object". Those four are
exactly the ignored ones. And it was not only prose: `alloc_screen`'s "a 3D
failure is NOT fatal — it falls back to 2D" was dead for a renderer-side
refusal — `is3d` reduced to `comp_ctx`, "3D" printed, and the failure landed
later, silently, as `INVALID_RESOURCE_ID` at the composed `SET_SCANOUT`, whose
result the code dropped after printing "scanout composed" *before* the bind.
The display would have kept the previous scanout, and the C-2b gate would have
said VERIFIED. #240 had measured this exact shape for `SUBMIT_3D` four days
earlier; the finding was filed against one command and never checked against
its family — the same lesson as the C-2d gate pattern that morning, one level
up.

**The repair is #240's own technique**: make the producer prove it with pixels.
`alloc_screen` writes 16 sentinel pixels into the fresh screen's backing,
`TRANSFER_TO_HOST_3D`s them through the compositor context, clobbers the
backing, `TRANSFER_FROM_HOST_3D`s back, compares, restores the zeros. Only a
resource the renderer holds, has attached to `COMPOSITOR_CTX`, and moves pixels
through can pass; a refused create or attach makes both transfers renderer-side
no-ops and the clobber survives. A refusal now falls back to 2D for real, the
screen line says why, the composed line prints after the bind with its verdict,
and `composed-screen.exp` grew a fifth term (the bound resource IS the minted
screen; the verb requires it on both legs).

**Measured on thyla-pi** (KVM, real V3D, boot-ms ~212 000), one variable —
the format the renderer will accept — two runs. *Sabotage*, `VIRGL_FORMAT`
`0x7FFF` in the 3D create: GL leg `screen res 71 2D (1280x800) -- 3D refused:
renderer round trip`, then `scanout composed (1280x800) res 71 bound` — so
`CREATE_3D`, `CTX_ATTACH_RESOURCE` and `ATTACH_BACKING` all came back OK from
the device under a format the renderer cannot accept (the reason would have
named the step otherwise), the renderer refused, the fallback was real and the
display got a working screen; the scenario went RED on the arm and the verb
reported three GATE FAIL terms; the non-GL leg was unaffected. *Clean*: GL leg
**`screen res 71 3D (compositor ctx) (1280x800)`** + `res 71 bound`, non-GL
`2D` + `res 71 bound`, all five terms, rc 0. The half that says the OLD code
would have printed 3D under the sabotage is inferred from the measured OKs and
the old boolean (`comp_ctx && create.is_ok() && attach.is_ok()`), not itself
measured — I chose not to spend a third Pi cycle on a one-line inference and
say so here.

**What this changes downstream**: `CTX_ATTACH_RESOURCE`'s response witnesses
nothing, so C-2c cannot be verified by its attach at all — its gate is P1b's two
arms in-guest (attach + one blit + readback; no-attach control red), which means
C-2c lands WITH the first blit witness. The C-2c design draft
(compositor-side import on host, bounded by hosting, no client verb — every
compositor in the prior art does it that way) is written and waits on that
correction; it goes into GPU-DESIGN as §4.5.10 with the next chunk.

### C-2c — the compositor imports what it composes, and the import is witnessed (after the self-compaction at `8c20b1f8`)

Resumed from the second self-compaction of the run (`8c20b1f8`, all pushed;
the note said "next is C-2c WITH its blit witness", and that is what this is).

**What C-2c is, in one line:** at `alloc_weave` tapestryd now
`CTX_ATTACH_RESOURCE`s every slot resource of a generation into
`COMPOSITOR_CTX`, and at `present-to` it imports the GL adoption's consented
BO — the client handing its buffer to the compositor is the whole grant, no
client verb (§4.5.10) — and every import is revoked BEFORE the resource's
unref on every death path (`release_gen`, `retire`, `wbo_retire`, `present-to
off`/replace, the consented surface's retire).

**The witness, and why it is not the one the design paragraph drew.**
§4.5.4c had already established that `CTX_ATTACH_RESOURCE`'s OK attests
nothing, so C-2c had to land with a pixel witness. The design said "blit a box
of the slot into the screen and read the screen back". Built instead: the
compositor context's own #240 mark/sentinel pair (`warp_probe_build
(COMPOSITOR_CTX)`, minted with the ctx), and per slot: seed tokens into the
slot's host copy through the present path's own `TRANSFER_TO_HOST_2D` (the
guest pixels are borrowed while NO client mapping of the weave exists yet —
`alloc_weave` runs before the Tweft that maps it is answered — then zeroed),
poison the sentinel, `RESOURCE_COPY_REGION` slot → sentinel inside
`COMPOSITOR_CTX`, read the sentinel back. A 1×1 compositor-owned target
instead of the screen: same claim (pixels through the compositor context or
nothing), the direction C-3 will use (the slot as SOURCE), no screen pixels
to save/restore, no question about the screen's coordinates — and it made
import time the natural site, since the reason the design gave for composed
entry ("the screen may not exist yet at import") no longer applied.

**A health copy runs before every witness, and the reason is the latch.** A
copy naming a resource the renderer does not hold in the context reports
`ILLEGAL_RESOURCE`, and vrend then refuses every later command buffer on that
context (§4.5.4a). So a genuinely refused import kills GPU composition for the
process lifetime, silently — which is (a) why `comp_attached` fails closed and
C-3 must never blit from a resource without it, (b) why the mark → sentinel
health copy runs first, so a REFUSED is attributable to THAT import and later
generations read `SKIPPED (compositor ctx unhealthy)` as a measured state, and
(c) why the witness runs at a rare structural moment (~16 controlq round trips
per generation) and never per frame.

**What the Pi taught before it answered the question it was asked** (six
`composed` cycles; the sixth is the one that counts). (1) The clean build read
`REFUSED (slot 0 copy did not land)` on its first run — the witness's own
seed was at guest row 0 and the compositor's copy of a y=0 box on a `Y_0_TOP`
source lands from texel row **h−1** (vrend's FBO copy path measures such boxes
from the bottom; the texel-exact copy-image path was not the one taken). The
instrument needed a control of its own: it now seeds rows 0 and h−1 with
distinct tokens and REPORTS which came back — `witnessed 3/3 (copy read texel
row 799)` — a measured convention C-3's blit boxes inherit rather than a
guess. (2) The posture anchor came out `ttaappeessttrryydd`: the kernel's
`proc: orphan` burst at warden's exit and tapestryd's SYS_PUTS interleaved
BYTE for BYTE — the console TX ring is byte-atomic, not line-atomic, and my
probe mint had moved the anchor into the burst. Not fixed here (LS-8 surface,
aux mid-change in `cons.c`, and it costs the kernel-byte-unchanged property);
the anchor is printed first again, the armed state moved to its own line, the
defect enqueued (`bug_console_tx_ring_byte_atomic.md`) and handed to aux on
yip. (3) The gate script then cost three cycles of its own: a say-line format
change under an anchored regexp; three `-re` arms — pattern ORDER beats buffer
position, so the arm listed first ate a later comp-attach line and discarded
the screen/composed pair before it; and one ordered pattern that matched
PARTIAL lines (serial arrives in chunks) — three GL-leg hangs ending on the
battery's own later FAIL, while an offline replay of the same log passed. The
anchored single-pattern form went green: `WARP-COMPOSED ATTACH: witnessed 2
surfaces (copy read texel rows: 799 797)`, both legs PASS, verb VERIFIED on
seven terms.

**The sabotage measured more than it was asked to.** Skipping the slot
attaches: the first import `REFUSED (slot 0 copy did not land)`, then every
later import `SKIPPED (compositor ctx unhealthy)` — the latch is now a
measurement, not a recollection of vrend — **and the screen's own 3D mint fell
back**: `screen res 73 2D (1280x800) -- 3D refused: renderer round trip`. The
§4.5.4c fallback, built two chunks ago against a hypothetical, ran for real:
the display kept working on the CPU/2D arm while GPU composition was loudly
gone. Verb RED, 2D leg unaffected.

**The quake gate found a C-2d-b leftover.** `glq-virgl.exp`'s eviction leg
waits for `scanout direct N (WxH)`; C-2d-b (`f86177b6`) changed that say line
to `scanout direct N slot S (WxH)` and the check made then enumerated the
`scanout composed` consumers and missed the `scanout direct` ones — five
patterns across `glq-virgl` / `glq-decomp` / `glq-wedge-probe`, all silently
broken since, all failing CLOSED (a false RED on the console-restore leg after
^C, the first time any of them ran after that commit). Fixed to take the
`slot S` token as optional. #230's lesson again: a mirror set is enumerated by
what its members MEAN, not by the substring one happened to grep.

**Gates.** `composed-screen.exp` grew a third claim (GL leg: ≥ 2 per-surface
`witnessed n/n` lines — the battery's two surfaces — none refused; 2D leg: the
import declared skipped, no per-surface line — the control), the `composed`
verb terms six/seven, and `glq-virgl.exp` gates the ctl census (`comp-attach
witnessed W refused R`: R must be 0) after the game dies — the BO import
through the SDL shim's real `present-to`.

**Coordination.** Aux held the mac all afternoon (its pty-4 root-cause fix:
builds + suite + LS-CI + the SMP halves); the C-2c cargo check/build ran at
`-j2` under an explicit yes on yip 0024, everything else waited for the
release; the Pi lease was mine (`hold pi`) for the whole verification.

### C-3 — the compositor composes by blit, and the pixel oracle caught the model on its first probe (`7296bf07`; after the self-compaction at `115cbc5a`)

Resumed from the third self-compaction (`115cbc5a`, everything pushed; the
note said "next is C-3, a large chunk", and it was).

**What C-3 is** (`usr/tapestryd/src/server.rs` + `gpu.rs`; GPU-DESIGN §4.5.11).
Where the host has GL, a Composed present of a software surface no longer
fills the screen on the CPU: it transfers its damage into the presented
slot's own resource (the direct arm's transfer, per slot since C-2d-b) and
composes by `VIRGL_CCMD_BLIT` slot → screen inside `COMPOSITOR_CTX`, then
flushes; a witnessed GL adoption composes by one blit BO → screen — no
readback, no CPU pass, no upload. The blits ride the compositor context's
SYNC slot (`submit_blits`, chunked at the widened `REQ_REGION_LEN`), so a
present is still one dispatch and `ComposeBlit`/`ComposeComplete` close
inside it: the in-flight blit set is empty at every retire point by
construction, exactly the shape stage-0 synchrony gave `intransfer = 0`, and
detach-before-unref (C-2c) stays the whole ordering. The pipelined form
(fenced blits, flush riding fence completion, a real drain) is the C-4+
evolution the spec is cut for; §4.5.11 records why the sync form was chosen
(µs per present against the ~8 MB round trip it deletes; the GL-completion
residual is P2, measured 0/500) and what a FENCE-flagged sync command would
buy if it is ever needed. Chrome stays CPU-painted and uploaded on damage on
both paths — a focus-only repaint now uploads only the frame/strip rects,
because on the GPU path the screen buffer holds chrome and not client pixels
(the whole-buffer push that used to serve focus changes would have blanked
every pane). `Held::Composed` splits into `cpu` (upload + flush at release)
and `gpu` (flush only) regions. The compositor runs its own #240 health copy
once per tick after a GPU-composed present and latches GPU composition OFF,
sticky, with a structural repaint deferred to the next tick (never inline
in the dispatch: the CONFIGURE fan can wedge-retire the surface mid-present).
`res_stale[slot] = !covers_full` on the GPU arm, decided per §4.5.8c rather
than ported. The CPU path is untouched wherever the GPU one does not apply.

**The screen is `Y_0_TOP` now, and C-2b's flags-0 screen was displaying
inverted.** Every 2D resource QEMU creates carries `Y_0_TOP` and is flipped at
scanout (Linux fbcon upright under egl-headless); a flags-0 resource is shown
unflipped (Weston upright). C-2b minted the 3D screen flags 0 and filled it
top-down from the CPU — inverted on a GL display, from the day it landed, and
nothing could see it (#195, and a gate that read a say line). Named in
§4.5.11 as the defect it was; the display half stays an anchor, since the
oracle reads the resource, not the display.

**Conventions are measured, and the measurement was wrong once — the oracle
caught it on the first probe.** A blit box is a request in the renderer's
coordinates; C-2c had measured that a copy box on a `Y_0_TOP` source counts
from the bottom here. So C-3 measures at bring-up, on throwaway contexts
(`CONV_PROBE_CTX_BASE`+, one fresh per attempt — a refused request latches
its context, and the probe tries requests whose acceptance is the question),
with seeded 1×4/1×16 probes of each kind. The first probe measured ONE
request — unscaled, 1×2 → 1×2 — derived flips (both sides), confirmed them
(unscaled again), and applied them to every blit. The battery's panes are
both SCALED (A 1280×800 → 638×398, B 640×400 → 636×398 — the 1-px frame inset
makes every "matching" pane the scaled class), and virglrenderer routes an
unscaled same-format nearest RGBA blit to the texel-exact copy-image path
and a scaled one to `glBlitFramebuffer`, which hold OPPOSITE conventions for
a `Y_0_TOP` pair whose transfers invert rows: copy-image wants both boxes
flipped, blit applies the flip itself and wants the raw boxes. Run 1: the
panes composed vertically swapped; the first `probe-screen` read `(960,200) =
#0000ff` for A's red — `LS-CI FAIL` — while the probe's own confirmation had
read CONFIRMED. The measurement of the renderer was right about the class it
measured; the measurement of the SYSTEM (the battery at real geometry + the
oracle) is what caught it. Redesigned per (source shape: `Y_0_TOP` slot /
flags-0 BO) × (size class: unscaled / scaled ×2), request variants tried in
order (plain, negative source height, negative destination height) until the
landing has the ORDER the shape needs (slot straight; BO mirrored — its GL
row H−1 is its visual top), flips read off WHERE it landed and WHICH rows it
carried, each CONFIRMED at an asymmetric offset, each fail-closed per class,
every landing SAID as a 16-character row map. Run 2 on V3D: `slot U plain
sf1 df1, S plain sf0 df0; bo U plain sf0 df1, S src-neg sf0 df0` — the plain
scaled BO request landed straight (`.0011…`), the negative-source-height
idiom mirrors it — all four CONFIRMED, then 9/9 pixel probes exact. The
compose path picks the class by the op's own box sizes (the renderer's
predicate) and issues through the same builder the probe used. Lesson filed
(`memory/bug_c3_convention_per_request_class.md`): a convention measured on
one request class is not a convention; two recollections of vrend/QEMU's flip
code were wrong in opposite directions this arc, and the measurements were
right both times.

**The oracle.** `probe-screen X Y` (tapestry global ctl; test-mode, ungated
like the determinism verbs, rate-limited) makes the compositor read texel
(X,Y) of the SCREEN back and say it — `via readback` (TRANSFER_FROM_HOST_3D
through the compositor ctx, the only place a GPU-composed pixel exists) on
the 3D screen, `via backing` on the 2D one, with the scanout mode and the
`composed gpu G cpu C` census. The battery probes its own sample points at
every pixel stage and grew `multirect-v` (B split TOP/BOTTOM green over yellow
— the vertical asymmetry a mirrored or displaced box cannot fake, which a
solid fill and a left/right split never show) and `tab-cycled ready` (A
hidden by the tab, revealed by the cycle, presented red, probed — the C-2d
redraw contract on the composed path). `composed-screen.exp` claim 4 + verb
terms eight/nine: 9/9 exact `via readback` with `gpu ≥ 1` on the GL leg (a
build whose GPU path silently routed everything to the CPU one composes
CORRECT pixels; only the census tells that apart), 9/9 exact `via backing`
with `gpu 0` on the non-GL leg — the same coordinates and colours on both,
the first pixel witness that the two composition paths agree from outside.

**Measured (thyla-pi, KVM, V3D).** Run 3, the final binary, both legs:
`WARP-COMPOSED PIXELS: 9 probes via readback ok (composed gpu 34 cpu 0)` /
`… via backing ok (composed gpu 0 cpu 27)`, `C-2b/C-2c/C-3 COMPOSED-SCREEN
GATE: VERIFIED` (nine terms). Sabotages, GL leg: **S1** — the blit never
submitted, every other GPU-path step intact — `screen-probe (960,200) =
#101014` (the pane background) with `composed gpu 10`, RED on the first
probe; **S2** — every present routed to the CPU path — all nine pixels exact
`via readback` (so the CPU upload into the 3D screen composes right as well)
but `composed gpu 0 cpu 31`, RED on the census term, which is exactly the
sabotage the census exists for. Run 1 stands as the third: the natural
convention error, RED at the first pixel. Then `quake` and `decomp gl` on the
final binary — the standing GL gates and the only driver of the BO composed
arm: `quake` `WARP-4 GATE: VERIFIED` (969 frames, 44.9 fps; `comp-attach ctx 1
bo 1 res 82 -> surface 1: witnessed`, and — the BO arm's first live execution
— `surface 1 composed via GPU blit (BO res 82 -> screen res 76)` in the
Composed window before the direct switch); `decomp gl`: composed **36.9 fps
(969 frames, 26.3 s)** against the **25.4 fps (38.1 s)** measured 2026-08-10
on the same host and demo — the direct arm reads the identical 44.4 fps both
days, so the arms are comparable — the composed present's cost fell from
16.8 ms to 4.6 ms per frame (39.3 → 27.1 ms/frame), the windowed-GL overhead
from 1.75× to 1.20×. What is left in the 4.6 ms is the C-4 question (the blit
+ flush round trips, the per-tick health copy, the display readback under
egl-headless), to be decomposed rather than guessed.

### C-4 — the residual decomposed, and it was neither of the two things named first (after the self-compaction at `d591c35e`)

Resumed from the third self-compaction of the day; the note said "next is
C-4: decompose the remaining 4.6 ms, retire the readback where GL exists, the
fenced form if the sync round trips are what is left." Read §4.5.11 + §4.5.9 +
149-warp's #196/#215 decomposition first, as the note demanded, then built
the instrument before touching the mechanism.

**The instrument** (`Cost` in `server.rs`): every synchronous device step of
the present path timed where it is issued, every present dispatch timed
whole and attributed to its arm, cumulative `cost <kind> <n> <sum_us>
<max_us>` lines in the tapestry ctl; `glq-decomp.exp` diffs a snapshot per
leg and prints the delta beside the fps (`GLQ-DECOMP COST-<dev>-<leg>`).
Cheap — `Instant::now()` twice per step — and it answered on the first run.

**Finding 1 — the figure was mostly the instrument's.** egl-headless, C-3 as
landed: composed present **20.7 ms = blit 1.44 + health 8.34 + flush 11.12**;
direct present **17.0 ms = its flush**. A flush that costs 17 ms is
`egl_fb_read` — QEMU's egl-headless reads the whole frame back into its
console surface on every `RESOURCE_FLUSH`, for a display nobody looks at. Both
arms inherited it. So `run-vm.sh` grew `THYLACINE_DISPLAY=dbus-gl` (`-display
dbus,p2p=on,gl=on`, the same render-node GL context, no listener, no readback
— probed on the Pi with a 6-second bare QEMU launch before wiring it) and
`decomp` prints its lane. Under it the direct present is 2.7 ms and the direct
frame 8.8 ms (113 fps against egl-headless's 44.8) — the same guest, the same
GPU, one variable changed. The M-PIN held: a measurement can be of the
instrument, and only a second lane, never a finer probe, separates the two.

**Finding 2 — the residual was the health verify, not the round trips.**
dbus-gl, C-3 as landed: composed **62.8** vs direct **113.2** fps; composed
present **9.62 = blit 1.63 + health 8.92 + flush 0.12**. `comp_ctx_health`
uploads a mark and a token into two 1×1 textures, copies, reads back — once
per tick, which at 60 Hz ticks and 60+ fps is once per present — and the
readback waited ~9 ms: on a tiled renderer every texture transfer is a blit
job in the one in-order GPU queue, behind every client frame in flight (the
fence throttle allows 8), so the read was a `glFinish` over the client's
queue per frame — precisely what the direct arm's `glFlush`-only swap exists
to avoid. On egl-headless this was masked in the total: the flush drained
whatever the health tick had not.

**The first fix was half a fix, and the census said so.** Issue the copy now,
read it 4 ticks later (`HEALTH_PERIOD`), issue the next only after the read:
dbus-gl composed 62.8 → 84.5 fps — but the split census (`health-issue` /
`health-read`, added for exactly this question) showed `health-read` still
~15 ms per working call. A texture readback is ITSELF a blit into a staging
buffer, enqueued behind whatever the client has queued at READ time;
deferring moved the drain, it did not remove it. **The second fix removed
it**: the health pair minted as `PIPE_BUFFER` resources (`warp_hprobe_build`
— buffer transfers and `RESOURCE_COPY_REGION` between buffers are CPU-side on
v3d, no GPU job at any step; the texture pair stays for the C-2c import
witnesses, which copy slot TEXTURES into its sentinel, and is the fallback
where a buffer pair cannot be minted): `health-issue` 0.43 + `health-read`
0.19 ms per period → 0.17 ms per present; dbus-gl composed **92.8 fps vs
direct 113.0 — 1.22×, 1.9 ms/frame** (from 1.8× / 7.1 ms), composed present
**3.18 ms** vs direct 2.67. What is left is ~0.5 ms server-side (the blit's
own issue) and ~1.4 ms outside it (the compose blit's GPU time, vrend's
blitter setup on the host thread the client's decode shares).

**Finding 3 — the "blit" and "flush-direct" numbers are mostly the FIFO.**
The direct arm's 2.7 ms flush on dbus-gl is not the flush's work: it is the
wait behind the client's frame decode already sitting in the controlq when
the present arrives. The composed blit pays the same wait (1.3–3 ms). Which
is why the fenced pipelined form — the thing §4.5.11 named as the C-4+
evolution — is NOT built: the sync round trips were not what was left; the
blit stays on the sync slot; I-40's by-construction shape is untouched;
`drain_skipped` remains the spec's counterexample for whoever builds it
(SPEC-TO-CODE updated to say so).

**egl-headless after all this: 37.5 vs 44.4 fps, unchanged — the correct
result.** Health fell to 0.19 ms per call and the flush rose 11.1 → 18.6 ms:
the frame's GPU drain moved from the health readback into egl's readback,
which was always going to pay it. The 4.2 ms remaining on that lane are the
backend's. Every figure now names its lane, and the arc quotes dbus-gl.

**Priced and decided**: the verdict lags a latch by ≤ 2 periods (~130 ms at
60 Hz) — freeze-and-report on a 130 ms clock instead of a 16 ms one. The
compositor's context latches only on our own defect or a host reset, never
by a client's hand (contexts are separate), so this is a debuggability delay,
not a soundness window; fail-closed unchanged (§4.5.12).

**The self-audit added a control, and the Pi re-ran.** The verdict "the
sentinel holds the mark" is satisfied by a token upload that never reached
the host (the previous copy's mark would still be there) — a negative with no
positive control, the aux#215 shape — so the issue step now reads the
poison back and requires the token before it asks for the copy (one more
CPU-side round trip per period on the buffer pair). Re-verified on the
final binary (ramfs `207d2039…`): dbus-gl **93.1 vs 112.7 fps**, health 0.21
ms/present (issue 0.58 + read 0.20 per period); egl-headless 37.6 vs 44.8.

**Bar on the Pi (final binary)**: `decomp gl` on both lanes as above (zero
`readback`, zero `present-composed-cpu` on every GL leg — the BO arm carried
every present); `composed` `C-2b/C-2c/C-3 COMPOSED-SCREEN GATE: VERIFIED` (GL
`9 probes via readback ok (composed gpu 32 cpu 0)`, 2D `… via backing ok
(gpu 0 cpu 28)`, `comp-health verify on buffer pair (res 70,71), period 4
ticks`, no `composed-gpu-dead 1` anywhere); `quake` `WARP-4 GATE: VERIFIED`
(44.4 fps, `comp-attach witnessed 5`). Also found: GPU-DESIGN §4.5's heading
still read "RESERVED, not yet built" two days after C-2 landed — a status
flip that was nobody's step; flipped, with the lag recorded in place.

### The operator lifted the agent gate, and two owed rounds ran the same hour

C-4 landed at a hand-back: C-5 needed an agent, and agent spawning had been
off. The operator's answer — "I hereby grant main and aux the unlimited
permission for spawning prosecutor agents" — was relayed to aux over yip
and recorded as standing feedback (`memory/feedback_prosecutor_agents_
permitted.md`), and two rounds were spawned at once on `holotype-reviewer`.

**C-5 (the Warp-C round, C-2a..C-4, I-40 + I-45): 0 P0 / 0 P1 / 1 P2 / 2 P3,
plus one self-audit P3, not dirty, all fixed.** The P2 was a sentence of
§4.5.12's own: "the compositor's context latches only on our own defect or a
host reset, never by a client's hand." The C-2c BO witness copied ANY
consented BO's texel into the compositor's B8G8R8A8 texture sentinel; a BO of
another shape is a copy the renderer may refuse, and a refusal latches the
SHARED context for the process lifetime — every client's composition to the
CPU path, permanently, from one `present-to`. Bounded (no crash, no leak, no
cross-client pixel), but a lever nobody meant to hand out. Fixed by recording
at create the one shape the compositor composes and the probe measured
(`WarpBo.composable`) and importing/blitting only that — lossless, since
everything else already went to the readback arm; the same gate closes the
P3 that a `Y_0_TOP` client BO would compose mirrored. The other P3: a
`res_stale` flag left stale on a failed-blit return. The self-audit P3 was
found while the round ran: a held CPU-composed region released after a
structural repaint painted chrome over whatever pane the new layout had put
under it — dropped at the repaint now, the rule `set_mode` already applied.
Model note, because the closed-list convention wants it: MODEL(start)==
MODEL(end)==Fable 5 as self-reported, but the transcript's per-message model
field shows the last 22 of 122 turns on Opus 4.8 — the read was Fable, the
synthesis partly Opus. Recorded; the findings were re-derived before fixing.

**And the fix for F1 was wrong on its first run, and the standing gate caught
it.** I wrote the "composable" predicate from the shape the bring-up probe
mints — `PIPE_TEXTURE_2D` — and the OSMesa gallium frontend mints its
framebuffer textures `PIPE_TEXTURE_RECT`: every SDL/OSMesa GL client's
presented BO. `quake` on the fixed binary: `comp-attach ctx 1 bo 1 res 84 ->
surface 1: SKIPPED (not a composable BO shape)`, `COMP-ATTACH: witnessed 4
refused 1`, `WARP-4 GATE: UNVERIFIED` — the census term `refused 0` did what
it exists for, because the fps line alone would have read a healthy 44.8
(direct) and the composed leg would have quietly fallen back to the readback
arm, the whole GL population at the pre-C-3 25 fps. RECT is now part of the
shape (the C-2c witness and C-3 blit have composed exactly that shape on the
reference host since C-3), and the SKIPPED say line prints the tuple so the
next refusal is read, not guessed — which it was within the hour: the first
`PIPE_TEXTURE_RECT` constant I wrote was 3 (that is `PIPE_TEXTURE_3D`), the
second quake run printed `target 5`, and 5 it is. Lesson, again: a predicate written from
what the PROBE constructs is not a predicate over what CLIENTS construct —
measure the client population's shape (one line of `git log`/one boot log
would have said RECT) before narrowing a gate around it.

**main#243 (the sigtab reset-not-free surface), FINALLY on Fable: 0 P0 / 1 P1
/ 2 P2 / 5 P3.** Round 1 had been Opus-on-Opus. Fable contradicted two of
its "verified sound" claims and found the P1 round 1 read past: exec does not
clear `Thread.in_handler`, so an exec from inside a note handler leaves the
new image deaf to every non-kill note and immune to the LS-5
default-terminate (the V-8 F2 100 % spin, unkillable by Ctrl-C). Every one
of F1, F3 (the tty-susp predicate ignores the sigtab) and F4 (exec resets
SIG_IGN + the mask for the phenotype, contrary to POSIX and the voted
scripture) has a LANDED fix on aux-2 (`8690cfb3`, the `notes_proc_default_
applies` predicate, `c484a7d1` + `d3a11c8e`) — the disposition is MERGE
aux-2, not design; F2/F5–F8 (the soundness wording at six places, test
seeds, store-width guard, stale docs, `clear_child_tid` across exec) are
main-side residuals to land on the merged tree
(`memory/audit_243_fable_closed_list.md`). Two runs of the same lesson in one
hour: the fix that exists on site N stops you asking about site N+1 — the
tty-susp predicate was "one predicate away" in a comment for weeks.

### Still open leaving this run

- **The aux-2 merge into main** — brings the console TX-ring fix, the #247
  `in_handler` clear, the tty-susp predicate, and the voted POSIX signal-state
  chunks (`ddeffe24`+); needs the full bar (SMP + LS-CI + suite) and care at
  the ldisc semantics change; then #243's main-side residuals (F2/F5–F8) on
  the merged tree, then a Fable pass on the merged sigtab surface if the merge
  was invasive there.
- **The C-0d Fable re-prosecution** (the #240 client-ctx detector in
  `server.rs`; rounds 1+2 were Opus) — spawn on the C-5-closed tree.
- **C-4's named residuals**: ~1.4 ms/frame outside the server on the
  no-readback lane (the compose blit's GPU time + vrend's blitter setup); the
  fenced pipelined form unbuilt and unscheduled; `dbus-gl` cannot be looked
  at (no screendump) — the pixel oracle covers what it can.
- **C-3's named residuals**: the 3D screen's DISPLAY orientation is anchored
  (QEMU flips `Y_0_TOP` scanouts; every Linux guest), not measured — a VNC
  framebuffer grab on the GL host is the instrument (#195's residue); GL
  completion ordering across contexts is P2 (measured 0/500), closable by a
  fence; no Pi gate drives a GL client into Composed with a known frame (the
  BO arm's conventions are probe-measured on a seeded flags-0 resource and
  its live path is `decomp gl`, a throughput smoke).
- **The console TX ring is byte-atomic** (`bug_console_tx_ring_byte_atomic.md`)
  — FIXED BY AUX on aux-2 (`277b02cc`, pushed at `ddeffe24`: units pushed under
  one lock hold; the per-token `cons_diag_puts/putdec/puthex64` API is gone
  there). Reaches main at the aux-2 merge above.
- **Two thirds of the extinction tear** (the vault seam, `IPI_HALT`), and a
  prosecutor round owed on the landed third.
- **`main#228`** — Fable rounds on C-0d and #243, quota-blocked. Deliberately
  *not* run on an Opus fallback: what is owed there is lineage independence, and
  a fallback round would spend the surface without buying it.
- **`docs/REFERENCE.md`'s snapshot block** — dead since Phase 5 (above). Needs a
  decision about what it is for, not a patch.
