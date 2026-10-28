// go-threads holds more OS threads at once than the old fixed cap of 256 (ARCH
// 7.3.5; thread-model review F-5). Each of 300 goroutines locks itself to its
// own M and parks, so the runtime must keep 300 Ms alive together. Go treats a
// refused thread create as fatal (newosproc throws), so joey runs this twice
// as a user, who is not exempt from the thread budget: under the default it
// must exit 0, and under a budget of 256 it must die naming the refused create
// -- the second run is what shows the first one reached past 256.
//
// It uses only the runtime and sync, like go-goroutines.
package main

import (
	"runtime"
	"sync"
)

func main() {
	const locked = 300
	var ready, release sync.WaitGroup
	ready.Add(locked)
	release.Add(1)
	for i := 0; i < locked; i++ {
		go func() {
			runtime.LockOSThread()
			ready.Done()
			release.Wait()
			runtime.UnlockOSThread()
		}()
	}
	ready.Wait()
	release.Done()
	println("go-threads: 300 goroutines held 300 locked OS threads at once")
}
