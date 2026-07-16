// ambush-child -- the Stage-8c-1 iteration-1 attach target for /ambush-probe.
//
// A parking Go program with a known global (Sentinel) and a named, non-inlined
// park function (parkLoop) so an attached Ambush can prove it reads a variable
// from the target's memory (`print main.Sentinel`), unwinds a real stack (`bt`),
// and lists goroutines (`goroutines`) against a running, M-threaded Go target --
// the backend exercise the version smoke deliberately skipped. parkLoop yields
// forever (runtime.Gosched -> SYS_YIELD -> the EL0-return stop checkpoint on
// every iteration), so a debugger stop parks it immediately and it stays alive
// until ambush-probe killgrp's it. Baked UNSTRIPPED (build_go_probes) so its
// DWARF is present for Ambush to load.

package main

import "runtime"

// A known global for `print main.Sentinel`: distinctive so the probe can assert
// the exact value Ambush reads back from the target's address space via the
// cross-Proc /proc/<pid>/mem debug file.
var Sentinel int64 = 0x0AABB00DCAFE0001

//go:noinline
func parkLoop() {
	for {
		runtime.Gosched()
		// Reference Sentinel so the linker + DWARF retain it (a never-taken
		// branch; the target never exits on its own -- the probe kills it).
		if Sentinel == 0 {
			return
		}
	}
}

func main() {
	parkLoop()
}
