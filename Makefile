# Thylacine OS — convenience Makefile.
#
# Thin aliases over tools/build.sh + tools/run-vm.sh + tools/test.sh.
# Per ARCHITECTURE.md §3: real build system is CMake (kernel) + Cargo (Rust).
# This Makefile is just for muscle memory (`make kernel`, `make test`, etc.).

.PHONY: all kernel sysroot userspace disk clean test run gdb specs help

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

clean:
	@tools/build.sh clean

test:
	@tools/test.sh

run:
	@tools/run-vm.sh

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
	@echo "  test       — run-vm + boot-banner verify"
	@echo "  run        — launch a dev VM (interactive UART)"
	@echo "  gdb        — launch dev VM with GDB stub on :1234, halted at entry"
	@echo "  specs      — run all TLA+ specs under specs/"
	@echo "  clean      — remove build/"
	@echo ""
	@echo "Underlying scripts: tools/build.sh, tools/run-vm.sh, tools/test.sh."
	@echo "See CLAUDE.md 'Build + test commands' for the canonical reference."
