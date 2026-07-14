// go-get: the GOOS=thylacine Stage 5 probe (half 2) -- the module workflow,
// end to end, self-contained:
//
//   1. writes a demo project (embedded below) into a work dir
//   2. points the module env at writable dirs + the real proxy
//   3. `go mod tidy`   -- resolves + DOWNLOADS the dependency from
//                         proxy.golang.org through netd's /net (Go's own
//                         net/http + TLS inside cmd/go), verifying it against
//                         sum.golang.org (the proxy's /sumdb passthrough)
//   4. `go build`      -- compiles the project + the downloaded module
//   5. runs the result -- and asserts its output uses the dependency
//   6. `go version -m` -- reads the module manifest out of the built binary
//
// EXTERNAL-NETWORK DEPENDENT (step 3, first run): never a boot gate. Driven by
// the go5.exp interactive scenario (online-guarded) and by hand:
//
//   /bin/go-get [WORKDIR]        (default /tmp/go5)
//
// Env mechanism (G15, the plan9 model): a child inherits the PARENT'S KERNEL
// /env (env_clone_into at rfork) -- os.Setenv is process-local and invisible
// to children, and ProcAttr.Env is deliberately dropped by exec_thylacine. So
// the driver sets the module env by writing /env/KEY files in its own Proc;
// the spawned `go` (and the `go`-spawned compile/link children) inherit them.
package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

const demoGoMod = `module thyla.dev/go5demo

go 1.25
`

const demoMainGo = `package main

import (
	"fmt"

	"github.com/google/go-cmp/cmp"
)

type specimen struct {
	Name    string
	Legs    int
	Extinct bool
}

func main() {
	museum := specimen{Name: "thylacinus cynocephalus", Legs: 4, Extinct: true}
	sighted := specimen{Name: "thylacinus cynocephalus", Legs: 4, Extinct: false}
	d := cmp.Diff(museum, sighted)
	if d == "" {
		fmt.Println("go5demo FAIL: cmp.Diff returned empty")
		return
	}
	fmt.Println("go5demo: cmp.Diff of museum vs sighted:")
	fmt.Print(d)
	fmt.Println("go5demo OK")
}
`

const goBin = "/goroot/bin/go"

func fail(stage string, detail string) {
	fmt.Printf("go-get FAIL at %s: %s\n", stage, detail)
	os.Exit(1)
}

// run executes bin in dir, streaming combined output through a prefix, and
// returns the output. echo=true prints even on success (the download lines are
// the network proof).
func run(stage, dir string, echo bool, bin string, args ...string) string {
	t0 := time.Now()
	cmd := exec.Command(bin, args...)
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	dur := time.Since(t0).Round(time.Millisecond)
	if echo || err != nil {
		for _, line := range strings.Split(strings.TrimRight(string(out), "\n"), "\n") {
			if line != "" {
				fmt.Printf("go-get |%s| %s\n", stage, line)
			}
		}
	}
	if err != nil {
		fail(stage, fmt.Sprintf("%v (%s)", err, dur))
	}
	fmt.Printf("go-get: %s ok (%s)\n", stage, dur)
	return string(out)
}

func setKernelEnv(key, value string) {
	if err := os.WriteFile("/env/"+key, []byte(value), 0o644); err != nil {
		fail("env", fmt.Sprintf("write /env/%s: %v", key, err))
	}
}

func main() {
	work := "/tmp/go5"
	if len(os.Args) > 1 {
		work = os.Args[1]
	}
	src := filepath.Join(work, "src")
	for _, d := range []string{src, filepath.Join(work, "gocache"),
		filepath.Join(work, "gopath"), filepath.Join(work, "tmp"),
		filepath.Join(work, "home")} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			fail("mkdir", fmt.Sprintf("%s: %v", d, err))
		}
	}

	if err := os.WriteFile(filepath.Join(src, "go.mod"), []byte(demoGoMod), 0o644); err != nil {
		fail("write go.mod", err.Error())
	}
	if err := os.WriteFile(filepath.Join(src, "main.go"), []byte(demoMainGo), 0o644); err != nil {
		fail("write main.go", err.Error())
	}

	// The module env, kernel-side (inherited by the spawned toolchain). No
	// ",direct" fallback on GOPROXY: a proxy miss should fail loud, not
	// attempt a git fetch (no git on-device). GOSUMDB stays the go.env
	// default (sum.golang.org via the proxy passthrough) -- deliberately
	// exercised, not disabled.
	for _, kv := range [][2]string{
		{"GOROOT", "/goroot"},
		{"GOPROXY", "https://proxy.golang.org"},
		{"GO111MODULE", "on"},
		{"GOTOOLCHAIN", "local"},
		{"GOFLAGS", ""},
		{"GOCACHE", filepath.Join(work, "gocache")},
		{"GOPATH", filepath.Join(work, "gopath")},
		{"GOTMPDIR", filepath.Join(work, "tmp")},
		{"TMPDIR", filepath.Join(work, "tmp")},
		{"HOME", filepath.Join(work, "home")},
		{"GOENV", "off"},
		{"GOTELEMETRY", "off"},
	} {
		setKernelEnv(kv[0], kv[1])
	}
	fmt.Printf("go-get: work=%s proxy=https://proxy.golang.org\n", work)

	// Step 3: resolve + download over the network (the echo shows the
	// "go: downloading ..." lines -- the pull proof).
	run("mod-tidy", src, true, goBin, "mod", "tidy")

	sum, err := os.ReadFile(filepath.Join(src, "go.sum"))
	if err != nil {
		fail("go.sum", fmt.Sprintf("read: %v", err))
	}
	if !strings.Contains(string(sum), "github.com/google/go-cmp") {
		fail("go.sum", "no go-cmp entry (tidy did not resolve the dependency)")
	}
	fmt.Println("go-get: go.sum ok (go-cmp pinned + sumdb-verified)")

	// Step 4: build against the downloaded module.
	demo := filepath.Join(work, "go5demo-bin")
	run("build", src, false, goBin, "build", "-o", demo, ".")

	// Step 5: the built artifact runs and USES the dependency.
	out := run("run-demo", src, true, demo)
	if !strings.Contains(out, "Extinct") || !strings.Contains(out, "go5demo OK") {
		fail("run-demo", "output does not show the cmp.Diff result")
	}

	// Step 6: the module manifest is embedded in the binary.
	vm := run("version-m", src, false, goBin, "version", "-m", demo)
	if !strings.Contains(vm, "github.com/google/go-cmp") {
		fail("version-m", "built binary carries no go-cmp module record")
	}
	fmt.Println("go-get: STAGE 5 (module pull + build + run) OK")
}
