// go-fs: the GOOS=thylacine Go-port Stage-3a proof.
//
// A Go binary that drives the os + syscall packages end-to-end against the
// writable post-pivot Stratum FS: os.Mkdir / os.Create / (*File).Write /
// os.ReadFile / os.Stat / (*File).Seek / os.ReadDir / os.Rename / os.Remove.
// It runs POST-pivot (devramfs is read-only); joey spawns it under /go-fs.
//
// Self-checking: every step is asserted; any mismatch panics (exit != 0),
// which joey's gating turns into a boot failure -- the Stage-3a regression
// sentinel. On success it prints "go-fs: STAGE 3a OK" and exits 0.
package main

import (
	"bytes"
	"fmt"
	"os"
	"runtime"
	"sync"
)

const (
	root    = "/go-fs"
	file    = "/go-fs/probe.txt"
	renamed = "/go-fs/probe2.txt"
)

var payload = []byte("thylacine go fs stage 3a\n") // 25 bytes

func must(what string, ok bool) {
	if !ok {
		fmt.Printf("go-fs: FAIL: %s\n", what)
		panic("go-fs assertion failed: " + what)
	}
}

func main() {
	// Idempotent: reclaim any stale tree from a prior boot (the Stratum pool
	// persists across reboots). RemoveAll exercises ReadDir + Remove.
	_ = os.RemoveAll(root)

	// 1. Mkdir at the (writable) pivot root.
	must("os.Mkdir(/go-fs)", os.Mkdir(root, 0o755) == nil)

	// 2. Create + write a file (os.Create -> SYS_WALK_CREATE, (*File).Write).
	f, err := os.Create(file)
	must("os.Create(probe.txt)", err == nil)
	n, err := f.Write(payload)
	must("File.Write", err == nil && n == len(payload))
	must("File.Sync", f.Sync() == nil)
	must("File.Close", f.Close() == nil)

	// 3. Read it back whole (os.ReadFile -> Open + loop Read + Close).
	got, err := os.ReadFile(file)
	must("os.ReadFile", err == nil)
	must("ReadFile content", bytes.Equal(got, payload))

	// 4. Stat -> size (os.Stat -> Open O_PATH + Fstat).
	fi, err := os.Stat(file)
	must("os.Stat", err == nil)
	must("Stat.Size", fi.Size() == int64(len(payload)))
	must("Stat.IsDir false", !fi.IsDir())
	di, err := os.Stat(root)
	must("os.Stat(dir)", err == nil)
	must("Stat dir IsDir", di.IsDir())

	// 5. Seek to offset 10 ("go fs") + partial Read ((*File).Seek -> SYS_LSEEK).
	rf, err := os.Open(file)
	must("os.Open", err == nil)
	off, err := rf.Seek(10, 0)
	must("File.Seek(10,0)", err == nil && off == 10)
	buf := make([]byte, 5)
	rn, err := rf.Read(buf)
	must("File.Read after seek", err == nil && rn == 5)
	must("seek content == 'go fs'", string(buf) == "go fs")
	must("Open.Close", rf.Close() == nil)

	// 6. ReadDir the directory (os.ReadDir -> SYS_READDIR + 9P dirent parse).
	ents, err := os.ReadDir(root)
	must("os.ReadDir", err == nil)
	found := false
	for _, e := range ents {
		if e.Name() == "probe.txt" {
			found = true
		}
	}
	must("ReadDir lists probe.txt", found)

	// 6b. #99: concurrent open-or-create of ONE fresh file -- the gopls filecache
	//     scenario (parallel goroutines racing os.OpenFile of a shared content-
	//     addressed path). Thylacine's open-or-create is two non-atomic syscalls
	//     (Open then Create), and Go's scheduler yields at each, so the racers all
	//     Open->ENOENT before the first Create lands, then race the Create: one
	//     wins, the rest get SYS_WALK_CREATE -> Tlcreate -> EEXIST. Without O_EXCL
	//     that is NOT an error -- POSIX opens the existing file, which the port's
	//     retry-Open now does. Pre-fix the kernel collapsed EEXIST to a blanket -1
	//     ("operation not permitted"), so every loser here would have FAILED.
	raceFile := "/go-fs/race.txt"
	_ = os.Remove(raceFile)
	const racers = 8
	errs := make(chan error, racers)
	var wg sync.WaitGroup
	for i := 0; i < racers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			rf, e := os.OpenFile(raceFile, os.O_WRONLY|os.O_CREATE, 0o600)
			if e == nil {
				_ = rf.Close()
			}
			errs <- e
		}()
	}
	wg.Wait()
	close(errs)
	for e := range errs {
		must("concurrent open-or-create (no O_EXCL) succeeds under a race", e == nil)
	}
	must("os.Remove(race.txt)", os.Remove(raceFile) == nil)

	// 6c. #99 F4 (deterministic, single-threaded): an O_CREATE|O_EXCL create of an
	//     EXISTING file must report EEXIST -- not the pre-fix blanket EPERM. This
	//     pins the SYS_WALK_CREATE errno propagation (the handler returns the real
	//     -T_E_EXIST): reverting the kernel record OR the handler return surfaces
	//     EPERM and os.IsExist goes false. (The O_EXCL path is a distinct branch
	//     from 6b's non-EXCL retry, so it pins the errno independently of a race.)
	exclFile := "/go-fs/excl.txt"
	_ = os.Remove(exclFile)
	ef0, ee0 := os.OpenFile(exclFile, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	must("O_EXCL create of a fresh file", ee0 == nil)
	must("O_EXCL File.Close", ef0.Close() == nil)
	_, ee1 := os.OpenFile(exclFile, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	must("O_EXCL create of an existing file reports EEXIST (not EPERM)", os.IsExist(ee1))
	must("os.Remove(excl.txt)", os.Remove(exclFile) == nil)

	// 7. Rename within the directory (os.Rename -> SYS_RENAME).
	must("os.Rename", os.Rename(file, renamed) == nil)
	_, err = os.Stat(file)
	must("old name gone after rename", err != nil)
	_, err = os.Stat(renamed)
	must("new name present after rename", err == nil)

	// 8. Remove the file + dir (os.Remove -> SYS_UNLINK).
	must("os.Remove(file)", os.Remove(renamed) == nil)
	must("os.Remove(dir)", os.Remove(root) == nil)
	_, err = os.Stat(root)
	must("dir gone after remove", err != nil)

	// NumCPU reflects getCPUCount() (reads /ctl/sched). With -smp 4 it should
	// be 4, not the old stub's 1 -- the Stage-2 getCPUCount seam, closed.
	fmt.Printf("go-fs: wrote+read %d bytes; stat size=%d; seek+readdir+rename+remove OK; NumCPU=%d\n",
		len(payload), fi.Size(), runtime.NumCPU())
	fmt.Println("go-fs: STAGE 3a OK (fs file I/O: create/write/read/stat/seek/readdir/rename/remove)")
}
