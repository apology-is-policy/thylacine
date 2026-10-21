// go-fs: the GOOS=thylacine Go-port Stage-3a proof.
//
// A Go binary that drives the os + syscall packages end-to-end against the
// writable post-pivot Stratum FS: os.Mkdir / os.Create / (*File).Write /
// os.ReadFile / os.Stat / (*File).Seek / os.ReadDir / os.Rename / os.Remove,
// and the creation mask (syscall.Umask) with os.Getppid. It runs POST-pivot
// (devramfs is read-only); joey spawns it under /go-fs.
//
// Self-checking: every step is asserted; any mismatch panics (exit != 0),
// which joey's gating turns into a boot failure -- the Stage-3a regression
// sentinel. On success it prints its leg census and exits 0.
package main

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"syscall"
)

const (
	root    = "/go-fs"
	file    = "/go-fs/probe.txt"
	renamed = "/go-fs/probe2.txt"
)

var payload = []byte("thylacine go fs stage 3a\n") // 25 bytes

// joey requires this line (GO_CENSUS_FS in usr/pouch-hello/pouch-census.h; Go
// cannot include the header, and a disagreement fails the boot). It names every
// leg, so a stale binary that never ran a newer one cannot pass for this one.
const census = "go-fs: legs=mkdir,create,read,stat,seek,readdir,race-create,excl,umask,getppid,links,root,rename,remove: exit 0"

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

	// 6d. M8 (IDENTITY-DESIGN 9.7): the kernel takes a create's mode less the
	//     creator's umask, then bounds it by the directory's. syscall.Umask
	//     exchanges the mask, and joey left the default. Under 027 a 0666 file
	//     and a 0777 directory in this 0755 directory come out 0640 and 0750;
	//     with the mask ignored they would be 0644 and 0755.
	must(fmt.Sprintf("/go-fs is 0755, the directory the mask legs assume (got %#o)", di.Mode().Perm()),
		di.Mode().Perm() == 0o755)
	old := syscall.Umask(0o027)
	must(fmt.Sprintf("syscall.Umask returns the default 0022 (got %#o)", old), old == 0o022)
	maskFile, maskDir := "/go-fs/m8.txt", "/go-fs/m8d"
	mf, err := os.OpenFile(maskFile, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o666)
	must("create under umask 027", err == nil)
	must("m8 File.Close", mf.Close() == nil)
	must("os.Mkdir under umask 027", os.Mkdir(maskDir, 0o777) == nil)
	back := syscall.Umask(old)
	must(fmt.Sprintf("syscall.Umask returns the mask it set (got %#o)", back), back == 0o027)
	mfi, err := os.Stat(maskFile)
	must("stat m8.txt", err == nil)
	mdi, err := os.Stat(maskDir)
	must("stat m8d", err == nil)
	must(fmt.Sprintf("a 0666 create under 027 is 0640 (got %#o)", mfi.Mode().Perm()), mfi.Mode().Perm() == 0o640)
	must(fmt.Sprintf("a 0777 mkdir under 027 is 0750 (got %#o)", mdi.Mode().Perm()), mdi.Mode().Perm() == 0o750)
	must("os.Remove(m8.txt)", os.Remove(maskFile) == nil)
	must("os.Remove(m8d)", os.Remove(maskDir) == nil)
	// joey spawned this probe, so its parent is neither kproc (0) nor itself.
	ppid := os.Getppid()
	must(fmt.Sprintf("os.Getppid names the parent (got %d)", ppid), ppid > 0 && ppid != os.Getpid())

	// 6e. Symbolic links (dec-2026-10-20-native-lstat-readlink), on the pool's
	//     baked fixture: os.Lstat reports the link itself (SYS_LSTAT),
	//     os.Readlink reads its target (SYS_READLINK), so filepath.EvalSymlinks
	//     expands it, and os.ReadDir types it from the qid. Every check runs
	//     and prints before the leg fails, so one red boot shows them all.
	links()

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
	fmt.Println(census)
}

const fix = "/lib/pouch-links"

// The fixture's targets, as build.sh bakes them. long's is 64 bytes: exactly
// os.Readlink's first buffer, so reading it must grow the buffer once.
var linkTargets = map[string]string{
	"live": "target", "dead": "nosuch", "dir": "sub", "abs": fix + "/target",
	"loop": "loop", "chain": "live", "long": strings.Repeat("x", 64),
	"up": "..",
}

func links() {
	bad := 0
	check := func(what string, ok bool) {
		if !ok {
			fmt.Printf("go-fs: FAIL: links: %s\n", what)
			bad++
		}
	}
	for name, want := range linkTargets {
		p := fix + "/" + name
		fi, err := os.Lstat(p)
		check(fmt.Sprintf("os.Lstat(%s) (err %v)", name, err), err == nil)
		if err == nil {
			check(fmt.Sprintf("os.Lstat(%s) is a link (mode %v)", name, fi.Mode()), fi.Mode()&os.ModeSymlink != 0)
			check(fmt.Sprintf("os.Lstat(%s) size is the target's length (got %d)", name, fi.Size()), fi.Size() == int64(len(want)))
		}
		got, err := os.Readlink(p)
		check(fmt.Sprintf("os.Readlink(%s) = %q (err %v)", name, got, err), err == nil && got == want)
	}
	fi, err := os.Lstat(fix + "/target")
	check(fmt.Sprintf("os.Lstat(target) is a regular file (err %v)", err), err == nil && fi.Mode().IsRegular())
	fi, err = os.Stat(fix + "/live")
	check(fmt.Sprintf("os.Stat(live) follows to the target (err %v)", err), err == nil && fi.Mode().IsRegular() && fi.Size() == int64(len("pouch-links target\n")))
	_, err = os.Stat(fix + "/dead")
	check(fmt.Sprintf("os.Stat(dead) is not-exist (err %v)", err), os.IsNotExist(err))
	_, err = os.Readlink(fix + "/target")
	check(fmt.Sprintf("os.Readlink of a file is EINVAL (err %v)", err), errors.Is(err, syscall.EINVAL))
	resolved, err := filepath.EvalSymlinks(fix + "/chain")
	check(fmt.Sprintf("filepath.EvalSymlinks(chain) = %q (err %v)", resolved, err), err == nil && resolved == fix+"/target")
	ents, err := os.ReadDir(fix)
	check(fmt.Sprintf("os.ReadDir(%s) (err %v)", fix, err), err == nil)
	types := map[string]os.FileMode{}
	for _, e := range ents {
		types[e.Name()] = e.Type()
	}
	check(fmt.Sprintf("ReadDir types live as a link (got %v)", types["live"]), types["live"] == os.ModeSymlink)
	check(fmt.Sprintf("ReadDir types sub as a directory (got %v)", types["sub"]), types["sub"] == os.ModeDir)
	check(fmt.Sprintf("ReadDir types target as a file (got %v)", types["target"]), types["target"] == 0)
	// A trailing slash makes even lstat follow a final link (the kernel's
	// rule, which the Root below must model).
	fi, err = os.Lstat(fix + "/up/")
	check(fmt.Sprintf("os.Lstat(up/) follows to a directory (err %v)", err), err == nil && fi.IsDir())
	// An os.Root follows the links that stay inside it and refuses the rest:
	// an absolute target escapes it whatever it names, and so does up (..),
	// with or without a trailing slash.
	r, err := os.OpenRoot(fix)
	check(fmt.Sprintf("os.OpenRoot(%s) (err %v)", fix, err), err == nil)
	if err == nil {
		text, err := r.ReadFile("chain")
		check(fmt.Sprintf("Root.ReadFile(chain) reads the target (err %v)", err), err == nil && string(text) == "pouch-links target\n")
		text, err = r.ReadFile("dir/leaf")
		check(fmt.Sprintf("Root.ReadFile(dir/leaf) goes through the link (err %v)", err), err == nil && string(text) == "pouch-links leaf\n")
		_, err = r.ReadFile("abs")
		check(fmt.Sprintf("Root.ReadFile(abs) is refused as an escape (err %v)", err), err != nil && strings.Contains(err.Error(), "path escapes"))
		for _, name := range []string{"up", "up/", "up//", "up/pouch-links/target"} {
			f, err := r.Open(name)
			if err == nil {
				f.Close()
			}
			check(fmt.Sprintf("Root.Open(%s) is refused as an escape (err %v)", name, err), err != nil && strings.Contains(err.Error(), "path escapes"))
		}
		_, err = r.Lstat("up/")
		check(fmt.Sprintf("Root.Lstat(up/) is refused as an escape (err %v)", err), err != nil && strings.Contains(err.Error(), "path escapes"))
		fi, err = r.Lstat("up")
		check(fmt.Sprintf("Root.Lstat(up) is the link itself (err %v)", err), err == nil && fi.Mode()&os.ModeSymlink != 0)
		check("Root.Close", r.Close() == nil)
	}
	must(fmt.Sprintf("links: %d checks failed (above)", bad), bad == 0)
	fmt.Printf("go-fs: links OK (%d links: lstat, readlink, EvalSymlinks, ReadDir types, Root)\n", len(linkTargets))
}
