// ambush-child -- the Stage-8c-1 iteration-1 attach target for /ambush-probe.
//
// A parking Go program with a known global (Sentinel) and a named, non-inlined
// park function (parkLoop) so an attached Ambush can prove it reads a variable
// from the target's memory (`print main.Sentinel`), unwinds a real stack (`bt`),
// and lists goroutines (`goroutines`) against a live, M-threaded Go target --
// the backend exercise the version smoke deliberately skipped.
//
// parkLoop parks between yields (runtime.Gosched -> SYS_YIELD, then a short
// time.Sleep) rather than busy-spinning. This is LOAD-BEARING for idle health:
// stages C/D drive `ambush exec`/`dap-selftest`, which LAUNCH this target and
// then exit on stdin EOF -- and a debugger that launched a target RESUMES it on
// exit (I-39 NoStrand: never strand a stopped quarry), orphaning it to init.
// ambush-probe holds no handle to those debugger-launched children, so a
// busy-yielding parkLoop leaked one pegged host core per instance FOREVER (the
// HVF-idle-~300% regression). Sleeping between yields makes a leaked/resumed
// instance idle at ~0% CPU -- like any real idle Go program -- while staying
// promptly stoppable: each yield + sleep-wake hits the EL0-return checkpoint,
// and 8c-2 stops a futex-parked M outright (the sleeper-stop this probe already
// relies on), and stages C/D break at THIS function's ENTRY (hit before the
// first sleep). It still never exits on its own. Baked UNSTRIPPED
// (build_go_probes) so its DWARF is present for Ambush to load.

package main

import (
	"runtime"
	"time"
)

// A known global for `print main.Sentinel`: distinctive so the probe can assert
// the exact value Ambush reads back from the target's address space via the
// cross-Proc /proc/<pid>/mem debug file.
var Sentinel int64 = 0x0AABB00DCAFE0001

//go:noinline
func parkLoop() {
	for {
		runtime.Gosched()
		// Park, do not spin: a leaked/resumed instance idles at ~0% CPU. The
		// sleep-wake also re-hits the EL0-return checkpoint, so a debugger stop
		// still lands promptly.
		time.Sleep(50 * time.Millisecond)
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
