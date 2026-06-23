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
