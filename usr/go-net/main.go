// go-net: the GOOS=thylacine Stage 3c probe -- a self-contained TCP round-trip
// over netd's /net, exercising the full plan9-shaped net stack:
//
//   net.Listen  -> queryCS1(/net/cs) -> open clone -> announce <ip>!<port>
//   net.Dial    -> queryCS1(/net/cs) -> open clone -> connect <ip>!<port> -> data
//   Accept      -> open /net/<proto>/N/listen (blocking, deferred 9P reply)
//   Read/Write  -> /net/<proto>/N/data
//
// The listener binds an EXPLICIT 127.0.0.1 endpoint, which netd migrates onto
// the resident `lo` stack (net-8a), so the whole exchange stays in-guest with
// no NIC. This is the net-8b native round-trip, driven by the Go net package.
//
// The blocking accept-open and the blocking data Read ride the Stage-3b
// entersyscall-wrapped Syscall path: while one goroutine parks in the SVC the
// runtime keeps scheduling the others (and GC can stop the world).
package main

import (
	"fmt"
	"io"
	"net"
	"os"
	"time"
)

// Port 0 is rejected by netd's announce (it cannot listen on an ephemeral
// port), so the probe pins a fixed high port.
const addr = "127.0.0.1:9099"

func fail(stage string, err error) {
	fmt.Printf("go-net: FAIL at %s: %v\n", stage, err)
	os.Exit(1)
}

func main() {
	// Watchdog: any hang in the dial/accept/data path (e.g. an accept that
	// never completes) fails the boot fast and loud instead of stalling the
	// test harness to its own timeout.
	time.AfterFunc(20*time.Second, func() {
		fmt.Println("go-net: TIMEOUT (round-trip did not complete)")
		os.Exit(1)
	})

	msg := []byte("go net stage 3c over /net")

	// Listen first: net.Listen returns only after the `announce` ctl write
	// lands, so the listener is armed before we dial -- no connect-before-
	// announce race.
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		fail("listen", err)
	}
	defer ln.Close()
	fmt.Printf("go-net: listening on %s\n", ln.Addr())

	srvErr := make(chan error, 1)
	go func() {
		c, err := ln.Accept()
		if err != nil {
			srvErr <- fmt.Errorf("accept: %w", err)
			return
		}
		defer c.Close()
		buf := make([]byte, len(msg))
		if _, err := io.ReadFull(c, buf); err != nil {
			srvErr <- fmt.Errorf("server read: %w", err)
			return
		}
		if _, err := c.Write(buf); err != nil {
			srvErr <- fmt.Errorf("server echo: %w", err)
			return
		}
		srvErr <- nil
	}()

	c, err := net.Dial("tcp", addr)
	if err != nil {
		fail("dial", err)
	}
	defer c.Close()
	fmt.Printf("go-net: dialed %s -> %s (local %s)\n", addr, c.RemoteAddr(), c.LocalAddr())

	if _, err := c.Write(msg); err != nil {
		fail("client write", err)
	}
	rbuf := make([]byte, len(msg))
	if _, err := io.ReadFull(c, rbuf); err != nil {
		fail("client read", err)
	}
	if err := <-srvErr; err != nil {
		fail("server", err)
	}

	if string(rbuf) != string(msg) {
		fail("verify", fmt.Errorf("got %q want %q", rbuf, msg))
	}
	fmt.Printf("go-net: round-trip OK (%q)\n", rbuf)
	fmt.Println("go-net: STAGE 3c OK")
}
