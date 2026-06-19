// Native TLS client substrate (NET-DESIGN section 9). rustls + the RustCrypto
// CryptoProvider, no_std. The kernel CSPRNG backs the handshake's randomness;
// the LS-K wall clock backs server-cert validity.
//
// net-7c-1 proved the provider + CSPRNG + record layer run on aarch64 (the
// `client_hello` ClientHello prover). net-7c-2 adds the real transport:
// `TlsStream<S>` drives the unbuffered rustls state machine over any
// `Read + Write` byte stream (a `/net` `TcpStream`), `load_roots_pem` reads the
// baked CA bundle, and `loopback_roundtrip` runs a full client<->server
// handshake + app-data exchange entirely in memory -- the deterministic,
// peer-independent in-guest proof (the net-3d loopback pattern) that the real
// WebPki cert verification (against the trust anchors + the wall clock) and the
// record layer compose end to end.
#![no_std]
extern crate alloc;

use alloc::sync::Arc;
use alloc::vec;
use alloc::vec::Vec;
use core::fmt;

use libthyla_rs::io::{Read, Write};

use rustls::client::UnbufferedClientConnection;
use rustls::server::{ServerConfig, UnbufferedServerConnection};
use rustls::time_provider::TimeProvider;
use rustls::unbuffered::{
    ConnectionState, EncodeError, EncodeTlsData, EncryptError, UnbufferedStatus, WriteTraffic,
};
use rustls::{ClientConfig, Error as RustlsError, RootCertStore};
use rustls_pki_types::pem::PemObject;
use rustls_pki_types::{CertificateDer, PrivateKeyDer, ServerName, UnixTime};

// =============================================================================
// RNG + clock providers.
// =============================================================================

// The RustCrypto provider draws ephemeral-key + nonce entropy through getrandom,
// which has no bare-metal backend -- route it to the kernel CSPRNG. Registered
// once here for every tls consumer (the only getrandom user in native userspace).
fn thyla_rng(buf: &mut [u8]) -> Result<(), getrandom::Error> {
    let n = unsafe { libthyla_rs::t_getrandom(buf.as_mut_ptr(), buf.len(), 0) };
    if n < 0 || n as usize != buf.len() {
        return Err(getrandom::Error::UNSUPPORTED);
    }
    Ok(())
}
getrandom::register_custom_getrandom!(thyla_rng);

// no_std rustls has no built-in clock; webpki reads the wall time to bound a
// server cert's validity window. net-7a's LS-K CLOCK_REALTIME is that source.
#[derive(Debug)]
struct ThylaTime;
impl TimeProvider for ThylaTime {
    fn current_time(&self) -> Option<UnixTime> {
        Some(UnixTime::since_unix_epoch(
            libthyla_rs::time::SystemTime::now().since_epoch(),
        ))
    }
}

// =============================================================================
// Errors.
// =============================================================================

/// A TLS-layer error. `Rustls` wraps a handshake/verification failure (a bad
/// cert, an unsupported suite); `Io` is a transport failure on the underlying
/// stream; `Config`/`Protocol`/`Handshake` are local misuse or an out-of-order
/// state.
#[derive(Debug)]
pub enum TlsError {
    /// Bad config input (unparseable PEM, empty trust store, bad server name).
    Config,
    /// The handshake did not complete (transport closed, or no convergence).
    Handshake,
    /// The connection produced an unexpected state for the requested operation.
    Protocol,
    /// The underlying byte stream failed.
    Io,
    /// A rustls error (cert verification, alert, unsupported parameters).
    Rustls(RustlsError),
}

impl From<RustlsError> for TlsError {
    fn from(e: RustlsError) -> Self {
        TlsError::Rustls(e)
    }
}

impl fmt::Display for TlsError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            TlsError::Config => write!(f, "tls config error"),
            TlsError::Handshake => write!(f, "tls handshake failed"),
            TlsError::Protocol => write!(f, "tls protocol error"),
            TlsError::Io => write!(f, "tls transport io error"),
            TlsError::Rustls(e) => write!(f, "tls: {}", e),
        }
    }
}

// =============================================================================
// Config construction.
// =============================================================================

/// A TLS 1.3 client config: the pure-Rust RustCrypto provider, the LS-K wall
/// clock, and `roots` as trust anchors (the baked CA bundle, via
/// `load_roots_pem`). The crate is built without the `tls12` feature, so the
/// "safe default" protocol versions resolve to TLS 1.3 only.
pub fn client_config(roots: RootCertStore) -> Arc<ClientConfig> {
    let cfg = ClientConfig::builder_with_details(
        Arc::new(rustls_rustcrypto::provider()),
        Arc::new(ThylaTime),
    )
    .with_safe_default_protocol_versions()
    .expect("rustls ships default versions")
    .with_root_certificates(roots)
    .with_no_client_auth();
    Arc::new(cfg)
}

/// A client config that trusts NO roots -- every server certificate fails to
/// chain to a trust anchor. For the negative half of the loopback E2E: proving
/// the verifier actually rejects an untrusted chain (i.e. is not a permissive
/// accept-all -- the cert-validation regression a real TLS client must never
/// have).
pub fn client_config_untrusting() -> Arc<ClientConfig> {
    client_config(RootCertStore::empty())
}

/// Parse a PEM CA bundle (the baked `/etc/ssl/certs/ca-certificates.crt`) into a
/// `RootCertStore`. Each `CERTIFICATE` block becomes a trust anchor; a bundle
/// with no usable certs is a `Config` error (fail closed -- a client with an
/// empty trust store would accept nothing, but we want the loud failure).
pub fn load_roots_pem(pem: &[u8]) -> Result<RootCertStore, TlsError> {
    let mut roots = RootCertStore::empty();
    let mut added = 0usize;
    for cert in CertificateDer::pem_slice_iter(pem) {
        let cert = cert.map_err(|_| TlsError::Config)?;
        roots.add(cert).map_err(TlsError::from)?;
        added += 1;
    }
    if added == 0 {
        return Err(TlsError::Config);
    }
    Ok(roots)
}

/// A TLS 1.3 server config from one PEM cert chain + one PEM private key. Used by
/// the in-memory loopback E2E (the server end of the deterministic handshake);
/// netd serves raw TCP, so a real Thylacine TLS *server* is a later concern.
pub fn server_config_single_cert(
    cert_pem: &[u8],
    key_pem: &[u8],
) -> Result<Arc<ServerConfig>, TlsError> {
    let certs: Vec<CertificateDer<'static>> = CertificateDer::pem_slice_iter(cert_pem)
        .collect::<core::result::Result<_, _>>()
        .map_err(|_| TlsError::Config)?;
    if certs.is_empty() {
        return Err(TlsError::Config);
    }
    let key = PrivateKeyDer::from_pem_slice(key_pem).map_err(|_| TlsError::Config)?;
    let cfg = ServerConfig::builder_with_details(
        Arc::new(rustls_rustcrypto::provider()),
        Arc::new(ThylaTime),
    )
    .with_safe_default_protocol_versions()
    .map_err(TlsError::from)?
    .with_no_client_auth()
    .with_single_cert(certs, key)
    .map_err(TlsError::from)?;
    Ok(Arc::new(cfg))
}

/// Drive the handshake to its first flight and return the ClientHello record.
/// Proves the provider + CSPRNG + ECDHE key share + record layer run on the
/// target -- the net-7c-1 runtime feasibility proof (no peer/cert required).
pub fn client_hello(host: &str) -> Option<Vec<u8>> {
    let cfg = client_config(RootCertStore::empty());
    let name = ServerName::try_from(host).ok()?.to_owned();
    let mut conn = UnbufferedClientConnection::new(cfg, name).ok()?;
    let mut out = vec![0u8; 4096];
    let status = conn.process_tls_records(&mut []);
    match status.state.ok()? {
        ConnectionState::EncodeTlsData(mut enc) => {
            let n = enc.encode(&mut out).ok()?;
            out.truncate(n);
            Some(out)
        }
        _ => None,
    }
}

// =============================================================================
// The unbuffered state-machine driver.
// =============================================================================
//
// rustls's no_std `UnbufferedConnectionCommon` is a pull state machine: feed it
// the TLS bytes received so far, it tells you what to do next (encode a
// handshake record, transmit, read decrypted app data, block for more peer
// bytes). `process_tls_records` is defined per role (client/server), but every
// state it yields -- and every method on those states -- is generic over the
// connection's `Data`, so the post-process logic lives once in `drive_state`
// and only the `process_tls_records` call differs by role (the `tlsconn_role!`
// macro). See the rustls `unbuffered` module docs for the protocol.

/// What a single `process_tls_records` step asked us to do next.
enum Action {
    /// Made progress (encoded/transmitted a record, or surfaced app data). The
    /// state machine may have more to do -- loop and call it again.
    Progressed,
    /// Reached `WriteTraffic`: the handshake is complete and the connection is
    /// idle (nothing pending). The caller may now send app data or read.
    Established,
    /// `BlockedHandshake`: more TLS bytes are needed from the peer.
    WantRead,
    /// The peer sent close_notify (edge-triggered once); no more app data comes.
    PeerClosed,
    /// The connection is fully closed (terminal).
    Closed,
}

/// Consume one `ConnectionState`, accumulating any to-send TLS bytes into
/// `outgoing` and any decrypted app data into `inbox`, and report the `Action`.
/// `discard` is updated with per-record discard counts (added to the caller's
/// base discard before it trims the incoming buffer). Generic over the role's
/// `Data`, so it is shared by both client and server.
fn drive_state<D>(
    state: core::result::Result<ConnectionState<'_, '_, D>, RustlsError>,
    discard: &mut usize,
    outgoing: &mut Vec<u8>,
    inbox: &mut Vec<u8>,
) -> Result<Action, TlsError> {
    match state.map_err(TlsError::from)? {
        ConnectionState::EncodeTlsData(mut enc) => {
            encode_into(&mut enc, outgoing)?;
            Ok(Action::Progressed)
        }
        ConnectionState::TransmitTlsData(st) => {
            // The encoded bytes are already staged in `outgoing`; the caller
            // transmits the whole buffer when the pump returns.
            st.done();
            Ok(Action::Progressed)
        }
        ConnectionState::BlockedHandshake => Ok(Action::WantRead),
        ConnectionState::WriteTraffic(_) => Ok(Action::Established),
        ConnectionState::ReadTraffic(mut rt) => {
            while let Some(rec) = rt.next_record() {
                let rec = rec.map_err(TlsError::from)?;
                *discard += rec.discard;
                inbox.extend_from_slice(rec.payload);
            }
            Ok(Action::Progressed)
        }
        // Server 0-RTT early data is never offered by our configs; treat as a
        // no-op step. The `_` also absorbs `non_exhaustive` forward-compat.
        ConnectionState::ReadEarlyData(_) => Ok(Action::Progressed),
        ConnectionState::PeerClosed => Ok(Action::PeerClosed),
        ConnectionState::Closed => Ok(Action::Closed),
        _ => Ok(Action::WantRead),
    }
}

/// Encode a handshake record into `outgoing`, growing the staging region until
/// it fits (the unbuffered API reports the required size on a short buffer).
fn encode_into<D>(enc: &mut EncodeTlsData<'_, D>, outgoing: &mut Vec<u8>) -> Result<(), TlsError> {
    let mut need = 4096usize;
    loop {
        let base = outgoing.len();
        outgoing.resize(base + need, 0);
        match enc.encode(&mut outgoing[base..]) {
            Ok(n) => {
                outgoing.truncate(base + n);
                return Ok(());
            }
            Err(EncodeError::InsufficientSize(e)) => {
                outgoing.truncate(base);
                need = e.required_size;
            }
            Err(EncodeError::AlreadyEncoded) => {
                outgoing.truncate(base);
                return Ok(());
            }
        }
    }
}

/// Encrypt `data` as an app-data record into `outgoing`, growing as needed.
fn encrypt_into<D>(
    wt: &mut WriteTraffic<'_, D>,
    data: &[u8],
    outgoing: &mut Vec<u8>,
) -> Result<(), TlsError> {
    let mut need = data.len() + 256;
    loop {
        let base = outgoing.len();
        outgoing.resize(base + need, 0);
        match wt.encrypt(data, &mut outgoing[base..]) {
            Ok(n) => {
                outgoing.truncate(base + n);
                return Ok(());
            }
            Err(EncryptError::InsufficientSize(e)) => {
                outgoing.truncate(base);
                need = e.required_size;
            }
            Err(_) => {
                outgoing.truncate(base);
                return Err(TlsError::Protocol);
            }
        }
    }
}

/// Encrypt a close_notify alert into `outgoing` (best-effort clean shutdown).
fn close_notify_into<D>(
    wt: &mut WriteTraffic<'_, D>,
    outgoing: &mut Vec<u8>,
) -> Result<(), TlsError> {
    let mut need = 256usize;
    loop {
        let base = outgoing.len();
        outgoing.resize(base + need, 0);
        match wt.queue_close_notify(&mut outgoing[base..]) {
            Ok(n) => {
                outgoing.truncate(base + n);
                return Ok(());
            }
            Err(EncryptError::InsufficientSize(e)) => {
                outgoing.truncate(base);
                need = e.required_size;
            }
            Err(_) => {
                outgoing.truncate(base);
                return Ok(());
            }
        }
    }
}

/// A role-agnostic connection driver: the unbuffered rustls connection plus the
/// byte buffers the state machine reads/writes. `incoming` holds TLS bytes
/// received from the peer (trimmed as the machine consumes them); `outgoing`
/// holds TLS bytes to transmit; `inbox` holds decrypted app data ready for the
/// caller.
struct TlsConn<C> {
    conn: C,
    incoming: Vec<u8>,
    outgoing: Vec<u8>,
    inbox: Vec<u8>,
    established: bool,
    peer_closed: bool,
    closed: bool,
}

impl<C> TlsConn<C> {
    fn wrap(conn: C) -> Self {
        TlsConn {
            conn,
            incoming: Vec::new(),
            outgoing: Vec::new(),
            inbox: Vec::new(),
            established: false,
            peer_closed: false,
            closed: false,
        }
    }

    /// Append peer TLS bytes to the incoming buffer (to be processed on the next
    /// `pump`).
    fn feed(&mut self, data: &[u8]) {
        self.incoming.extend_from_slice(data);
    }

    /// Take the staged outgoing TLS bytes (to transmit to the peer).
    fn take_outgoing(&mut self) -> Vec<u8> {
        core::mem::take(&mut self.outgoing)
    }

    /// Copy decrypted app data into `buf`, returning the count.
    fn read_app(&mut self, buf: &mut [u8]) -> usize {
        let n = core::cmp::min(buf.len(), self.inbox.len());
        buf[..n].copy_from_slice(&self.inbox[..n]);
        self.inbox.drain(..n);
        n
    }

    /// Move all decrypted app data into `dst`.
    fn drain_app(&mut self, dst: &mut Vec<u8>) {
        dst.append(&mut self.inbox);
    }

    fn has_app(&self) -> bool {
        !self.inbox.is_empty()
    }
}

impl TlsConn<UnbufferedClientConnection> {
    fn new_client(cfg: Arc<ClientConfig>, host: &str) -> Result<Self, TlsError> {
        let name = ServerName::try_from(host)
            .map_err(|_| TlsError::Config)?
            .to_owned();
        let conn = UnbufferedClientConnection::new(cfg, name).map_err(TlsError::from)?;
        Ok(TlsConn::wrap(conn))
    }
}

impl TlsConn<UnbufferedServerConnection> {
    fn new_server(cfg: Arc<ServerConfig>) -> Result<Self, TlsError> {
        let conn = UnbufferedServerConnection::new(cfg).map_err(TlsError::from)?;
        Ok(TlsConn::wrap(conn))
    }
}

// Generate `pump` / `write_app` / `queue_close` for each role. The only
// role-specific line is the `process_tls_records` call; everything else (the
// state handling, the buffer dance) is the shared `drive_state` and helpers.
// The destructure-then-call pattern gives the borrow checker disjoint field
// borrows: `conn` and `incoming` are borrowed by the returned state, while
// `outgoing`/`inbox` are mutated by `drive_state` -- all four are distinct
// fields, so the splits don't conflict, and the state is consumed before
// `incoming.drain` releases that borrow.
macro_rules! tlsconn_role {
    ($t:ty) => {
        impl TlsConn<$t> {
            /// Run the state machine to a blocking point (need-read, established
            /// idle, peer-closed, or closed), staging outgoing TLS bytes and
            /// draining decrypted app data along the way.
            fn pump(&mut self) -> Result<(), TlsError> {
                if self.closed {
                    return Ok(());
                }
                loop {
                    let TlsConn {
                        conn,
                        incoming,
                        outgoing,
                        inbox,
                        established,
                        peer_closed,
                        closed,
                    } = self;
                    let UnbufferedStatus { mut discard, state } =
                        conn.process_tls_records(incoming.as_mut_slice());
                    let action = drive_state(state, &mut discard, outgoing, inbox)?;
                    // Defensive: rustls's contract keeps `discard <= incoming
                    // .len()`, but clamp anyway -- a `drain` past the end would
                    // panic, and panic=abort turns that into a self-DoS on a
                    // path that talks to hostile servers.
                    incoming.drain(..core::cmp::min(discard, incoming.len()));
                    match action {
                        Action::Progressed => continue,
                        Action::Established => {
                            *established = true;
                            return Ok(());
                        }
                        Action::WantRead => return Ok(()),
                        Action::PeerClosed => {
                            *peer_closed = true;
                            return Ok(());
                        }
                        Action::Closed => {
                            *closed = true;
                            return Ok(());
                        }
                    }
                }
            }

            /// Encrypt `data` as app data into the outgoing buffer. Pumps to a
            /// settled state first; requires the handshake to have completed.
            fn write_app(&mut self, data: &[u8]) -> Result<(), TlsError> {
                self.pump()?;
                if !self.established {
                    return Err(TlsError::Protocol);
                }
                let TlsConn {
                    conn,
                    incoming,
                    outgoing,
                    ..
                } = self;
                let UnbufferedStatus { discard, state } =
                    conn.process_tls_records(incoming.as_mut_slice());
                let r = match state.map_err(TlsError::from)? {
                    ConnectionState::WriteTraffic(mut wt) => encrypt_into(&mut wt, data, outgoing),
                    _ => Err(TlsError::Protocol),
                };
                // Clamp like pump (line 450): a drain past the end panics, and
                // panic=abort is a self-DoS on the hostile-server path.
                incoming.drain(..core::cmp::min(discard, incoming.len()));
                r
            }

            /// Stage a close_notify alert (best-effort clean shutdown).
            #[allow(dead_code)]
            fn queue_close(&mut self) -> Result<(), TlsError> {
                if !self.established || self.closed {
                    return Ok(());
                }
                let TlsConn {
                    conn,
                    incoming,
                    outgoing,
                    ..
                } = self;
                let UnbufferedStatus { discard, state } =
                    conn.process_tls_records(incoming.as_mut_slice());
                let r = match state.map_err(TlsError::from)? {
                    ConnectionState::WriteTraffic(mut wt) => close_notify_into(&mut wt, outgoing),
                    _ => Ok(()),
                };
                // Clamp like pump (line 450): a drain past the end panics, and
                // panic=abort is a self-DoS on the hostile-server path.
                incoming.drain(..core::cmp::min(discard, incoming.len()));
                r
            }
        }
    };
}

tlsconn_role!(UnbufferedClientConnection);
tlsconn_role!(UnbufferedServerConnection);

// =============================================================================
// TlsStream<S> -- the blocking client transport.
// =============================================================================

/// A TLS client connection over a blocking byte stream `S` (a `/net`
/// `TcpStream`). `connect` runs the handshake to completion (validating the
/// server cert against the config's trust anchors and the wall clock); the
/// `Read`/`Write` impls then carry plaintext app data, encrypting/decrypting
/// through the rustls record layer transparently.
pub struct TlsStream<S> {
    inner: TlsConn<UnbufferedClientConnection>,
    sock: S,
}

impl<S: Read + Write> TlsStream<S> {
    /// Open a TLS connection to `host` over `sock`, completing the handshake.
    /// Fails (`Rustls`) if the server cert does not validate against `cfg`'s
    /// trust anchors / the wall clock.
    pub fn connect(sock: S, host: &str, cfg: Arc<ClientConfig>) -> Result<TlsStream<S>, TlsError> {
        let inner = TlsConn::new_client(cfg, host)?;
        let mut s = TlsStream { inner, sock };
        s.do_handshake()?;
        Ok(s)
    }

    fn do_handshake(&mut self) -> Result<(), TlsError> {
        loop {
            self.inner.pump()?;
            self.flush_out()?;
            if self.inner.established {
                return Ok(());
            }
            if self.inner.closed {
                return Err(TlsError::Handshake);
            }
            self.fill_in()?;
        }
    }

    fn flush_out(&mut self) -> Result<(), TlsError> {
        let out = self.inner.take_outgoing();
        if !out.is_empty() {
            self.sock.write_all(&out).map_err(|_| TlsError::Io)?;
        }
        Ok(())
    }

    fn fill_in(&mut self) -> Result<(), TlsError> {
        let mut buf = [0u8; 8192];
        let n = self.sock.read(&mut buf).map_err(|_| TlsError::Io)?;
        if n == 0 {
            // Peer closed the TCP stream mid-handshake.
            return Err(TlsError::Io);
        }
        self.inner.feed(&buf[..n]);
        Ok(())
    }

    /// Send a close_notify and stop. Best-effort: a transport error is ignored
    /// (the caller is tearing down anyway).
    pub fn close(&mut self) {
        if self.inner.queue_close().is_ok() {
            let _ = self.flush_out();
        }
    }
}

impl<S: Read + Write> Read for TlsStream<S> {
    fn read(&mut self, buf: &mut [u8]) -> libthyla_rs::err::Result<usize> {
        use libthyla_rs::err::Error as IoError;
        loop {
            let n = self.inner.read_app(buf);
            if n > 0 {
                return Ok(n);
            }
            if self.inner.peer_closed || self.inner.closed {
                return Ok(0); // clean TLS EOF
            }
            // Process whatever we already have, flush any responses, then block
            // for more peer bytes only if still empty.
            self.inner.pump().map_err(|_| IoError::Io)?;
            self.flush_out().map_err(|_| IoError::Io)?;
            if self.inner.has_app() {
                continue;
            }
            if self.inner.peer_closed || self.inner.closed {
                return Ok(0);
            }
            self.fill_in().map_err(|_| IoError::Io)?;
        }
    }
}

impl<S: Read + Write> Write for TlsStream<S> {
    fn write(&mut self, buf: &[u8]) -> libthyla_rs::err::Result<usize> {
        use libthyla_rs::err::Error as IoError;
        self.inner.write_app(buf).map_err(|_| IoError::Io)?;
        self.flush_out().map_err(|_| IoError::Io)?;
        Ok(buf.len())
    }
    fn flush(&mut self) -> libthyla_rs::err::Result<()> {
        Ok(())
    }
}

// =============================================================================
// Deterministic in-memory loopback E2E.
// =============================================================================

/// Run a complete client<->server TLS handshake and a request/response app-data
/// exchange entirely in memory, shuttling TLS records between two unbuffered
/// connections (no sockets, no peer). Returns `(server_saw, client_saw)` -- the
/// decrypted app data each side received.
///
/// This is the deterministic, peer-independent in-guest TLS proof (the net-3d
/// loopback pattern): it exercises the real handshake, the real WebPki server
/// certificate verification against `client_cfg`'s trust anchors and the wall
/// clock, and the record layer on the target. A bad cert, an expired validity
/// window, or a name mismatch makes `connect`/`pump` fail here, in-guest.
pub fn loopback_roundtrip(
    client_cfg: Arc<ClientConfig>,
    server_cfg: Arc<ServerConfig>,
    host: &str,
    request: &[u8],
    response: &[u8],
) -> Result<(Vec<u8>, Vec<u8>), TlsError> {
    let mut client = TlsConn::new_client(client_cfg, host)?;
    let mut server = TlsConn::new_server(server_cfg)?;
    let mut server_saw = Vec::new();
    let mut client_saw = Vec::new();
    let mut request_sent = false;
    let mut response_sent = false;

    // Each iteration advances at least one handshake flight; a 1-RTT TLS 1.3
    // handshake + the round-trip converges in a handful. The bound is a stall
    // backstop (no peer to wait on -- if nothing flows we are stuck).
    for _ in 0..64 {
        client.pump()?;
        if client.established && !request_sent {
            client.write_app(request)?;
            request_sent = true;
        }
        let c_out = client.take_outgoing();
        if !c_out.is_empty() {
            server.feed(&c_out);
        }

        server.pump()?;
        server.drain_app(&mut server_saw);
        if server.established && !server_saw.is_empty() && !response_sent {
            server.write_app(response)?;
            response_sent = true;
        }
        let s_out = server.take_outgoing();
        if !s_out.is_empty() {
            client.feed(&s_out);
        }

        client.pump()?;
        client.drain_app(&mut client_saw);

        if request_sent && response_sent && client_saw.len() >= response.len() {
            return Ok((server_saw, client_saw));
        }
        // No bytes moved this round and nothing left to send => stalled.
        if c_out.is_empty() && s_out.is_empty() && request_sent {
            break;
        }
    }
    Err(TlsError::Handshake)
}
