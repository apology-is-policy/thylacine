// go-hello is the Stage-1 proof of the GOOS=thylacine Go port: a
// runtime-direct Go program that the Thylacine native exec path loads and
// runs in-VM.
//
// It deliberately uses only the `println` builtin -- NOT fmt / os / syscall,
// which pull the unported Stage-3 packages. `println` writes to fd 2 via
// runtime.write1 (SYS_WRITE), and after main returns the Go runtime calls
// exit_group(0) (SYS_EXIT_GROUP), which joey reaps. Reaching status 0 proves
// the whole Stage-1 surface ran: the SysV initial-stack frame, osinit /
// schedinit / mallocinit (eager BURROW_ATTACH), sysmon's OS-thread spawn via
// the thread_entry trampoline, clock_gettime, and the clean exit.
//
// The unstripped binary exceeds 1 MiB, so the REVENANT file-backed
// demand-paged exec path carries it (the same path net-echo proves).
package main

func main() {
	println("go-hello: hello from a GOOS=thylacine Go binary")

	// A trivial computation so the printed value proves main actually ran
	// to completion (not just emitted a constant): sum(1..100) == 5050.
	sum := 0
	for i := 1; i <= 100; i++ {
		sum += i
	}
	println("go-hello: sum(1..100) =", sum)
}
