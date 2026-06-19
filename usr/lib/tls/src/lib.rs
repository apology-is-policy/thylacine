// Native TLS client substrate (NET-DESIGN section 9). rustls + the RustCrypto
// CryptoProvider, no_std. The kernel CSPRNG backs the handshake's randomness;
// the LS-K wall clock backs server-cert validity. The TlsStream transport (the
// unbuffered state machine over a /net TcpStream) lands with its loopback E2E.
#![no_std]
extern crate alloc;

use alloc::sync::Arc;
use alloc::vec;
use alloc::vec::Vec;
use rustls::client::UnbufferedClientConnection;
use rustls::time_provider::TimeProvider;
use rustls::unbuffered::ConnectionState;
use rustls::{ClientConfig, RootCertStore};
use rustls_pki_types::{ServerName, UnixTime};

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

/// A TLS 1.2/1.3 client config: the pure-Rust RustCrypto provider, the LS-K wall
/// clock, and `roots` as trust anchors (the baked CA bundle, loaded by net-7c-2).
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
