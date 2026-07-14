// go-web: the GOOS=thylacine Stage 5 probe (half 1) -- net/http against a real
// URL, end to end through the whole stack:
//
//   DNS        -> queryCS1(/net/cs) -> netd ndb/DNS -> slirp -> host resolver
//   TCP        -> /net/tcp/clone + connect (the plan9-shaped netFD)
//   TLS        -> crypto/tls (pure Go; AES-GCM rides the AT_HWCAP hardware
//                 path) + x509 chain verification against the system CA bundle
//                 (/etc/ssl/certs/ca-certificates.crt, the Stage 5
//                 reconciliation) + the PL031-anchored wall clock for validity
//   HTTP(S)    -> net/http (h2 via ALPN where the server offers it)
//
// EXTERNAL-NETWORK DEPENDENT: never a boot gate. Driven by the go5.exp
// interactive scenario (online-guarded) and by hand.
//
// Deadline caveat (docs/reference/133-go-port.md): thylacine deadlines are
// checked at op entry but do not abort an in-flight blocked call, so a
// wedged peer wedges the probe -- the caller (expect) owns the timeout.
package main

import (
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"
)

func main() {
	url := "https://example.com/"
	if len(os.Args) > 1 {
		url = os.Args[1]
	}

	t0 := time.Now()
	resp, err := http.Get(url)
	if err != nil {
		fmt.Printf("go-web FAIL: GET %s: %v\n", url, err)
		os.Exit(1)
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(io.LimitReader(resp.Body, 64*1024))
	if err != nil {
		fmt.Printf("go-web FAIL: read body: %v\n", err)
		os.Exit(1)
	}
	elapsed := time.Since(t0)

	tlsInfo := "plaintext"
	if resp.TLS != nil {
		tlsInfo = fmt.Sprintf("TLS ok (%s, verified chain of %d)",
			tlsVersionName(resp.TLS.Version), len(resp.TLS.VerifiedChains))
	}
	fmt.Printf("go-web: %s %s -> %s, %d bytes in %v, proto %s, %s\n",
		resp.Request.Method, url, resp.Status, len(body), elapsed.Round(time.Millisecond),
		resp.Proto, tlsInfo)

	// A recognizable slice of the body (the <title> line when present) so the
	// scenario can assert on CONTENT the typed command line cannot contain.
	if title := extractTitle(body); title != "" {
		fmt.Printf("go-web: title: %s\n", title)
	}

	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		fmt.Printf("go-web FAIL: non-2xx status %s\n", resp.Status)
		os.Exit(1)
	}
	fmt.Println("go-web: STAGE 5 (net/http fetch) OK")
}

func extractTitle(body []byte) string {
	s := string(body)
	lo := strings.Index(strings.ToLower(s), "<title")
	if lo < 0 {
		return ""
	}
	rest := s[lo:]
	gt := strings.IndexByte(rest, '>')
	if gt < 0 {
		return ""
	}
	rest = rest[gt+1:]
	end := strings.Index(strings.ToLower(rest), "</title")
	if end < 0 || end > 200 {
		return ""
	}
	return strings.TrimSpace(rest[:end])
}

func tlsVersionName(v uint16) string {
	switch v {
	case 0x0303:
		return "TLS1.2"
	case 0x0304:
		return "TLS1.3"
	default:
		return fmt.Sprintf("0x%04x", v)
	}
}
