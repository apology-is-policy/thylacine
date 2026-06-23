// go-goroutines is the Stage-2 proof of the GOOS=thylacine Go port: it
// exercises the three runtime subsystems Stage 1 never touched --
//
//   - the scheduler under real parallelism: GOMAXPROCS(4) forces multiple Ps,
//     so the runtime spawns multiple OS threads (SYS_THREAD_SPAWN under load,
//     via the thread_entry trampoline) and the Thylacine kernel scheduler
//     spreads them across the SMP CPUs;
//   - scheduler-sync on torpor: blocking channel ops and sync.WaitGroup park
//     and unpark goroutines, and an idling M parks/wakes on the futex
//     (SYS_TORPOR_WAIT / WAKE -- runtime.notesleep / notewakeup);
//   - the garbage collector + the overcommit memory layer: sustained allocation
//     triggers automatic GC, and an explicit runtime.GC() forces a full
//     stop-the-world cycle WHILE the workers are still allocating -- the real
//     test of STW under Thylacine's cooperative-only preemption (no async
//     preempt, like plan9), and of sysUnused -> SYS_BURROW_DECOMMIT shrinking
//     RSS on the proper overcommit model.
//
// It uses only the language + runtime + sync + sync/atomic (all GOOS-independent
// or already ported) -- NOT fmt / os / syscall / net (the unported Stage-3
// packages). println writes to fd 2 via runtime.write1; main returning calls
// exit_group(0); a failed self-check panics (exit 2), which fails joey's gating
// probe. Every worker loop allocates each iteration, so it is safepoint-rich and
// STW always converges -- a tight no-safepoint loop would stall STW (the
// documented cooperative-preemption limitation), so the probe deliberately
// avoids one.
package main

import (
	"runtime"
	"sync"
	"sync/atomic"
)

func main() {
	// Force real parallelism. Stage 1's getCPUCount() returns 1, so the default
	// is a single P; GOMAXPROCS(procs) gives the scheduler procs logical
	// processors -> multiple Ms the kernel places across the -smp CPUs.
	const procs = 4
	runtime.GOMAXPROCS(procs)

	const workers = 8
	const perWorker = 2000

	results := make(chan int)  // UNBUFFERED fan-in: each send blocks until
	                           // received -> maximal goroutine park/unpark and
	                           // M park/wake on torpor.
	var wg sync.WaitGroup      // the canonical join primitive (runtime sema).
	var allocOps int64         // cross-M atomic (LSE / LL-SC), summed independently.

	wg.Add(workers)
	for w := 0; w < workers; w++ {
		go func(id int) {
			defer wg.Done()
			local := 0
			for i := 0; i < perWorker; i++ {
				// Allocate each iteration: GC pressure AND a cooperative
				// safepoint, so a concurrent STW converges promptly. The size
				// varies so spans churn (exercising the sweeper + scavenger).
				b := make([]byte, 64+(i&63))
				b[0] = byte(id)
				b[len(b)-1] = byte(i & 0x7f)
				local += int(b[0]) + int(b[len(b)-1])
				atomic.AddInt64(&allocOps, 1)
			}
			results <- local // blocking send -> park until the collector takes it
		}(w)
	}

	// Hammer the GC concurrently with the running workers: each runtime.GC() is
	// a full blocking STW cycle, so this stresses stop-the-world while
	// goroutines actively allocate -- the meaningful cooperative-preemption test.
	go func() {
		for i := 0; i < 5; i++ {
			runtime.GC()
			runtime.Gosched()
		}
	}()

	// Fan-in: blocking receives. Sum the per-worker locals deterministically.
	total := 0
	for w := 0; w < workers; w++ {
		total += <-results
	}
	wg.Wait()

	runtime.GC() // a final full STW cycle, all workers now done -> NumGC >= 1.

	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)

	gomax := runtime.GOMAXPROCS(0)
	ops := int(atomic.LoadInt64(&allocOps))

	println("go-goroutines: workers =", workers, " GOMAXPROCS =", gomax)
	println("go-goroutines: allocOps =", ops, " want =", workers*perWorker)
	println("go-goroutines: NumGC =", int(ms.NumGC), " HeapAlloc =", int(ms.HeapAlloc), " bytes")
	println("go-goroutines: fan-in total =", total)

	// Self-check: every allocation ran (no lost goroutine / torn atomic), at
	// least one GC completed (STW worked), and the channel fan-in delivered.
	if ops != workers*perWorker {
		panic("go-goroutines: allocOps mismatch -- a goroutine or atomic was lost")
	}
	if ms.NumGC < 1 {
		panic("go-goroutines: GC never ran -- STW / GC path broken")
	}
	if total == 0 {
		panic("go-goroutines: fan-in delivered nothing -- channel park/wake broken")
	}

	println("go-goroutines: STAGE 2 OK (goroutines + channels + GC)")
}
