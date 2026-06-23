// go-env: the GOOS=thylacine Stage 4a probe -- proves the per-Proc /env
// environment device (G15, ARCH 9.7). joey sets a couple of /env vars on its
// own environment before spawning this probe; the kernel copies joey's env into
// the child (env_clone_into, the Plan 9 copy-on-rfork), so this probe inherits
// them. The Go runtime's goenvs reads /env at startup (via SYS_READDIR + per-var
// reads), and os.Getenv / os.Environ surface the result -- the full
// set -> inherit -> goenvs -> Getenv loop, with NO envp ABI.
package main

import (
	"fmt"
	"os"
)

func fail(msg string) {
	fmt.Printf("go-env: FAIL: %s\n", msg)
	os.Exit(1)
}

func main() {
	// joey set these on its /env before spawning us; we inherited a copy.
	if got := os.Getenv("GOENVTEST"); got != "stage4a-ok" {
		fail(fmt.Sprintf("GOENVTEST = %q, want \"stage4a-ok\"", got))
	}
	if got := os.Getenv("GOENVNUM"); got != "42" {
		fail(fmt.Sprintf("GOENVNUM = %q, want \"42\"", got))
	}
	// An unset variable reads as empty (no /env/NAME -> not in the cache).
	if os.Getenv("GOENVABSENT") != "" {
		fail("GOENVABSENT should be empty")
	}

	env := os.Environ()
	if len(env) < 2 {
		fail(fmt.Sprintf("os.Environ() = %d vars, want >= 2", len(env)))
	}
	fmt.Printf("go-env: GOENVTEST=%q GOENVNUM=%q; os.Environ() has %d vars\n",
		os.Getenv("GOENVTEST"), os.Getenv("GOENVNUM"), len(env))
	fmt.Println("go-env: STAGE 4a OK (per-Proc /env device: goenvs + inheritance + os.Getenv)")
}
