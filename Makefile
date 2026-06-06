# Thylacine OS — convenience Makefile.
#
# Thin aliases over tools/build.sh + tools/run-vm.sh + tools/test.sh.
# Per ARCHITECTURE.md §3: real build system is CMake (kernel) + Cargo (Rust).
# This Makefile is just for muscle memory (`make kernel`, `make test`, etc.).

.PHONY: all kernel sysroot userspace disk pool clean test test-tcg test-cross-reboot smp-gate run run-tcg gdb specs help

all:
	@tools/build.sh all

kernel:
	@tools/build.sh kernel

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

smp-gate:
	@tools/ci-smp-gate.sh

run:
	@tools/run-vm.sh

run-tcg:
	@THYLACINE_ACCEL=tcg tools/run-vm.sh

gdb:
	@tools/run-vm.sh --gdb

specs:
	@cd specs && for s in *.tla; do \
		echo "== $$s =="; \
		java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
			-config "$${s%.tla}.cfg" "$$s" 2>&1 | tail -3; \
	done

help:
	@echo "Thylacine OS — make targets:"
	@echo "  kernel     — build the kernel ELF (build/kernel/thylacine.elf)"
	@echo "  all        — kernel + sysroot + userspace + disk (as available per phase)"
	@echo "  pool       — re-bake build/fixtures/pool.img (clean Stratum boot pool)"
	@echo "  test       — run-vm + boot-banner verify (HVF on a capable host; W3.5)"
	@echo "  test-tcg   — same, forced to full-emulation TCG (-cpu max + GICv3) compat run"
	@echo "  test-cross-reboot — A-1b corvus persistence: boot twice on one pool"
	@echo "  smp-gate   — SMP soundness CI gate: multi-boot the smp4/smp8 x default/UBSan"
	@echo "               matrix N>=10 (single boots lie). SMP_GATE_N / SMP_GATE_CONFIGS env."
	@echo "  run        — launch a dev VM (interactive UART)"
	@echo "  gdb        — launch dev VM with GDB stub on :1234, halted at entry"
	@echo "  specs      — run all TLA+ specs under specs/"
	@echo "  clean      — remove build/"
	@echo ""
	@echo "Underlying scripts: tools/build.sh, tools/run-vm.sh, tools/test.sh."
	@echo "See CLAUDE.md 'Build + test commands' for the canonical reference."
