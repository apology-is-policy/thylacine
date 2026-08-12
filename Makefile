# Thylacine OS — convenience Makefile.
#
# Thin aliases over tools/build.sh + tools/run-vm.sh + tools/test.sh.
# Per ARCHITECTURE.md §3: real build system is CMake (kernel) + Cargo (Rust).
# This Makefile is just for muscle memory (`make kernel`, `make test`, etc.).

.PHONY: all kernel production sysroot userspace disk pool clean test test-tcg test-cross-reboot test-interactive test-classify smp-gate idle-gate check-floor test-a72 run run-tcg gdb specs help

all:
	@tools/build.sh all

kernel:
	@tools/build.sh kernel

# #61: the V1.0 lean production image -- no in-kernel test suite, no joey
# boot-test probe ladder. Boots straight to the login getty.
production:
	@tools/build.sh all --production

sysroot:
	@tools/build.sh sysroot

userspace:
	@tools/build.sh userspace

disk:
	@tools/build.sh disk

pool:
	@tools/build.sh pool

clean:
	@tools/build.sh clean

test:
	@tools/test.sh

# Compat reference run: force full emulation (TCG + -cpu max + GICv3, incl.
# RNDR). The default `make test` uses HVF on a capable host (Lazarus W3.5).
test-tcg:
	@THYLACINE_ACCEL=tcg tools/test.sh

test-cross-reboot:
	@tools/test-cross-reboot.sh

# Interactive E2E regression net (LS-CI): drive a real PTY into the console via
# `expect`, log in, assert rendered output. Optional gate -- SKIPs without
# `expect`. THYLACINE_ACCEL=tcg by default (deterministic compat run).
test-interactive:
	@tools/test-interactive.sh

smp-gate:
	@tools/ci-smp-gate.sh

# The SMP gate's failure classifier, exercised without booting anything (#222).
# Fast, so there is no excuse for the ladder to go untested again -- the
# EXTERNAL-KILL bucket was structurally unable to see SIGKILL for as long as
# nobody drove it.
test-classify:
	@tools/test-smp-classify.sh

idle-gate:
	@tools/ci-idle-gate.sh

# #91: the FULL ARMv8.0 floor scan, including the big pool payloads (/clade,
# /goroot). build.sh already runs the fast ramfs scan on every bake; this adds
# the ~6 min tail that only matters when the builder has rebuilt the device
# toolchain.
check-floor:
	@tools/check-v80-floor.py --all

# #91: the A72 boot. PORTABILITY.md section 3 names this the verification bar
# for the floor -- an ARMv8.0-only core, so any LSE that a runtime check did
# not skip is a real #UD rather than an unsupported config. It was a bar with
# no enforcer until this target existed.
test-a72:
	@THYLACINE_ACCEL=tcg THYLACINE_CPU=cortex-a72 tools/test.sh

run:
	@tools/run-vm.sh

run-tcg:
	@THYLACINE_ACCEL=tcg tools/run-vm.sh

gdb:
	@tools/run-vm.sh --gdb

# Runs each spec's DEFAULT (clean) cfg and FAILS if any TLC run fails.
# Specs with no default cfg (per-option cfgs only, e.g. sched_oncpu) are
# skipped by name. TTrace replay modules (TLC counterexample droppings)
# are skipped. The buggy-cfg counterexample gate is a separate, manual
# per-surface discipline today (RW-10 F3; the tiered runner is tracked).
# Three things this recipe has to do that TLC will not do for us.
#
# 0. BE CHUNKABLE (#124). The suite is 1.5-2+ hours, and two modules are almost
#    all of it -- measured 2026-08-01: corvus 36m48s, handles ~50m, the other 31
#    a few minutes between them. That overruns an agent's background-task limit,
#    which killed a full run at ~83 minutes with 22 modules never started. So
#    `SPECS=` selects a subset, the SMP_GATE_CONFIGS pattern:
#
#        make specs                      # everything (budget hours)
#        make specs SPECS=handles        # just the long pole
#        make specs SPECS='pipe poll'    # a chunk
#
#    Each module's elapsed time is printed so a new long pole is visible in the
#    log rather than discovered by a kill. Note that TLC writes each module's
#    output to /tmp/tlc-<module>.tla.log DIRECTLY, so per-module truth survives
#    a killed run even when this recipe's own stdout does not -- read those logs,
#    not the exit status, when a run is interrupted.
#
# 1. FIND JAVA. The bare `java` on macOS is /usr/bin/java, a stub that errors
#    "Unable to locate a Java Runtime" unless a JDK is registered -- while the
#    real one sits in the Homebrew cellar, which is why CLAUDE.md's TLA+ setup
#    says to prepend it to PATH. Finding it here means the documented one-liner
#    works without an undocumented export; without it the run fails every module
#    for the same reason, which is loud but wastes a full pass.
#
# 2. CLEAN specs/states (#123). TLC drops a timestamped checkpoint dir there per
#    invocation and never removes it; fifteen had accumulated to 16 GB, the
#    largest 2.8 GB, on a host with 19 GiB free while a live run was writing a
#    sixteenth. Nothing reads them back -- this recipe never passes -recover --
#    and the tree gitignores them. Cleaning at the START rather than the end
#    leaves a killed run's checkpoint inspectable until the next run -- which
#    also means two CONCURRENT `make specs` runs now clobber each other's
#    checkpoints. They already clobbered each other's /tmp/tlc-*.log, so this
#    target has never supported concurrent invocation; do not start one while
#    another is live.
specs:
	@cd specs && \
	rm -rf states; \
	JAVA=java; \
	if ! $$JAVA -version >/dev/null 2>&1; then \
		for c in /opt/homebrew/opt/openjdk/bin/java /usr/local/opt/openjdk/bin/java; do \
			if [ -x "$$c" ]; then JAVA="$$c"; break; fi; \
		done; \
	fi; \
	if ! $$JAVA -version >/dev/null 2>&1; then \
		echo "specs: no Java runtime (see CLAUDE.md 'TLA+ setup')" >&2; exit 1; fi; \
	if [ ! -f /tmp/tla2tools.jar ]; then \
		echo "specs: /tmp/tla2tools.jar missing (see CLAUDE.md 'TLA+ setup')" >&2; exit 1; fi; \
	sel=""; for m in $(SPECS); do sel="$$sel $${m%.tla}.tla"; done; \
	[ -n "$$sel" ] || sel=$$(echo *.tla); \
	fail=0; for s in $$sel; do \
		case "$$s" in *_TTrace_*) continue;; esac; \
		if [ ! -f "$$s" ]; then echo "== $$s == (no such module)" >&2; fail=1; continue; fi; \
		cfg="$${s%.tla}.cfg"; \
		if [ ! -f "$$cfg" ]; then echo "== $$s == (no default cfg; skipped)"; continue; fi; \
		t0=$$(date +%s); \
		echo "== $$s =="; \
		if $$JAVA -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
			-config "$$cfg" "$$s" > "/tmp/tlc-$$s.log" 2>&1; \
		then tail -3 "/tmp/tlc-$$s.log"; \
		else tail -5 "/tmp/tlc-$$s.log"; echo "** $$s FAILED **"; fail=1; fi; \
		echo "   ($$s took $$(( $$(date +%s) - t0 ))s)"; \
	done; exit $$fail

help:
	@echo "Thylacine OS — make targets:"
	@echo "  kernel     — build the kernel ELF (build/kernel/thylacine.elf)"
	@echo "  all        — kernel + sysroot + userspace + disk (as available per phase)"
	@echo "  pool       — re-bake build/fixtures/pool.img (clean Stratum boot pool)"
	@echo "  test       — run-vm + boot-banner verify (HVF on a capable host; W3.5)"
	@echo "  test-tcg   — same, forced to full-emulation TCG (-cpu max + GICv3) compat run"
	@echo "  test-cross-reboot — A-1b corvus persistence: boot twice on one pool"
	@echo "  test-interactive — LS-CI: expect/PTY interactive E2E (login + see output);"
	@echo "               optional gate, SKIPs without 'expect'. THYLACINE_ACCEL=tcg default."
	@echo "  smp-gate   — SMP soundness CI gate: multi-boot the smp4/smp8 x default/UBSan"
	@echo "               matrix N>=10 (single boots lie). SMP_GATE_N / SMP_GATE_CONFIGS env."
	@echo "  check-floor— #91: full ARMv8.0 floor scan incl. /clade + /goroot (~6 min)."
	@echo "               build.sh already runs the fast ramfs scan on every bake."
	@echo "  test-a72   — boot on -cpu cortex-a72 (ARMv8.0-only): the floor's"
	@echo "               verification bar, PORTABILITY.md section 3."
	@echo "  run        — launch a dev VM (interactive UART)"
	@echo "  gdb        — launch dev VM with GDB stub on :1234, halted at entry"
	@echo "  specs      — run all TLA+ specs under specs/"
	@echo "  clean      — remove build/"
	@echo ""
	@echo "Underlying scripts: tools/build.sh, tools/run-vm.sh, tools/test.sh."
	@echo "See CLAUDE.md 'Build + test commands' for the canonical reference."
