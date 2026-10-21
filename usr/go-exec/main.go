// go-exec: the GOOS=thylacine Go-port Stage-3b proof.
//
// A Go binary that drives os/exec end-to-end: spawn a /bin coreutil via
// SYS_SPAWN_FULL_ARGV (os.StartProcess), capture its stdout through a pipe
// (os.Pipe + internal/poll), and reap it via SYS_WAIT_PID (os.Process.Wait).
// Four checks:
//
//  1. exec.Command("/bin/echo", ...).Output() -- spawn + pipe-capture + wait,
//     asserting the captured bytes and the clean (0) exit.
//  2. exec.Command("/bin/false").Run() -- a non-zero exit propagates as an
//     *exec.ExitError with the right code (proves the wait status path).
//  3. Process.Signal(syscall.SIGTERM) on a sleeping child posts `term`, and
//     Wait reads a death by signal 15, ExitCode -1. Before signal chunk S-3
//     Signal wrote the note name as a /proc ctl verb, which knows only kill,
//     and Wait read every death as exit status 1.
//  4. Signal to a pid no process holds reads os.ErrProcessDone: the kernel's
//     ESRCH, which SYS_POSTNOTE answered as a bare -1 before S-3.
//
// It runs POST-pivot (the binaries live under /bin, bound from the initrd).
// Self-checking: any mismatch prints "go-exec: FAIL" and exits 1, which
// joey's gating turns into a boot failure -- the Stage-3b regression sentinel.
package main

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"syscall"
)

func fail(what string) {
	fmt.Printf("go-exec: FAIL: %s\n", what)
	os.Exit(1)
}

func main() {
	// 1. Spawn /bin/echo, capture its stdout via a pipe, wait for a clean exit.
	out, err := exec.Command("/bin/echo", "go", "exec", "stage", "3b").Output()
	if err != nil {
		fail(fmt.Sprintf("echo Output: %v", err))
	}
	got := bytes.TrimRight(out, "\n")
	if string(got) != "go exec stage 3b" {
		fail(fmt.Sprintf("echo output %q (want %q)", got, "go exec stage 3b"))
	}

	// 2. A non-zero exit must surface as *exec.ExitError with the exact code.
	//    /bin/false exits 1; the wait-status path carries that back.
	rerr := exec.Command("/bin/false").Run()
	ee, ok := rerr.(*exec.ExitError)
	if !ok {
		fail(fmt.Sprintf("false Run: want *exec.ExitError, got %v", rerr))
	}
	if code := ee.ExitCode(); code != 1 {
		fail(fmt.Sprintf("false exit code %d (want 1)", code))
	}

	// 3. A note other than kill: SIGTERM ends a sleeping child, and the death
	//    reads back as that signal.
	sl := exec.Command("/bin/sleep", "3600")
	if err := sl.Start(); err != nil {
		fail(fmt.Sprintf("sleep Start: %v", err))
	}
	if err := sl.Process.Signal(syscall.SIGTERM); err != nil {
		fail(fmt.Sprintf("Signal(SIGTERM): %v", err))
	}
	_ = sl.Wait()
	ws, ok := sl.ProcessState.Sys().(*syscall.Waitmsg)
	if !ok || !ws.Signaled() || ws.Sig != 15 || sl.ProcessState.ExitCode() != -1 {
		fail(fmt.Sprintf("sleep after SIGTERM: %v (want a death by signal 15, ExitCode -1)",
			sl.ProcessState))
	}

	// 4. A refusal is the kernel's errno: no process holds this pid.
	gone, _ := os.FindProcess(0x3ffffff0)
	if err := gone.Signal(syscall.SIGTERM); !errors.Is(err, os.ErrProcessDone) {
		fail(fmt.Sprintf("Signal(missing pid): %v (want os.ErrProcessDone)", err))
	}

	fmt.Printf("go-exec: captured %q via os/exec + pipe; /bin/false exit code %d; SIGTERM reads %v\n",
		string(got), ee.ExitCode(), sl.ProcessState)
	fmt.Println("go-exec: STAGE 3b OK (os/exec: spawn /bin coreutil + capture stdout + wait + exit-status)")
}
