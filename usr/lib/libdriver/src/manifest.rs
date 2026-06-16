// The driver manifest -- the declarative contract that makes a driver droppable
// and its grant auditable (MENAGERIE.md section 6). The warden's bind database
// is a set of these, one per driver binary; on `DeviceAdded(node)` it matches
// the node's `compatible` against each manifest's `binds`, then intersects the
// node's resources with the matched manifest's `needs` to compute the narrowed
// allowance (resource.rs::resolve).
//
// The on-disk form is the section-6 brace block:
//
//     driver "rp1-eth" {
//         abi      = 1
//         binds    = ["raspberrypi,rp1-eth", "brcm,genet-v5"]
//         needs {
//             mmio = "node:reg"        # the bound node's own reg window(s)
//             irq  = "msi:1"           # one MSI vector (or "node:interrupts")
//             dma  = "pool: 2 MiB"
//         }
//         serves   = "/dev/net/%instance"
//         restart  = on-crash
//         sig      = "<ed25519 over the package>"   # optional; section 9
//     }
//
// `needs` is bounded by the node: the warden intersects the asks with the actual
// node resources, so a manifest can never widen a driver's reach beyond its
// device. That intersection is the I-34 auditable-grant property.
//
// This module is PURE (no libthyla-rs) so the parser is exercised on the host.

use alloc::string::{String, ToString};
use alloc::vec::Vec;

use crate::Error;

/// The framework ABI a manifest targets. A driver built on this crate declares
/// `abi = 1`; the warden refuses to bind a manifest whose `abi` it does not
/// implement (forward-compat: a v2 framework can still read a v1 manifest only
/// if it chooses to).
pub const MANIFEST_ABI: u32 = 1;

/// A parsed driver manifest.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Manifest {
    /// The driver's name (the `driver "<name>"` header). Informational +
    /// the supervisor's identity for a bound instance.
    pub name: String,
    /// The framework ABI the driver targets (`MANIFEST_ABI`).
    pub abi: u32,
    /// The `compatible` strings this driver binds, most-specific first by
    /// convention. The warden binds the most-specific match.
    pub binds: Vec<String>,
    /// The declared hardware needs -- the axes the warden grants from the node.
    pub needs: Needs,
    /// The service path the driver publishes; `%instance` expands to the
    /// per-bind instance number (resource.rs::resolve).
    pub serves: String,
    /// The supervisor restart policy on a driver crash.
    pub restart: Restart,
    /// An optional package signature -- the section-9 authorization input. Carried
    /// verbatim; this crate neither produces nor verifies it (a warden/policy
    /// concern), it only round-trips the field.
    pub sig: Option<String>,
}

/// The hardware a driver declares it needs. Each axis is a *selection*: the
/// warden supplies the concrete values from the bound node (MMIO/IRQ) or grants
/// a budget (DMA), never more than the axis names.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Needs {
    pub mmio: MmioNeed,
    pub irq: IrqNeed,
    pub dma: DmaNeed,
}

impl Needs {
    pub const NONE: Needs = Needs {
        mmio: MmioNeed::None,
        irq: IrqNeed::None,
        dma: DmaNeed::None,
    };
}

/// What MMIO a driver needs. v1.0 supports the whole-node window set; finer
/// per-window selection is a v1.x refinement (the auditable property -- the
/// grant never exceeds the node -- holds either way).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MmioNeed {
    /// No MMIO.
    None,
    /// `"node:reg"` -- every `reg` window the bound node exposes.
    Node,
}

/// What interrupts a driver needs.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IrqNeed {
    /// No interrupts.
    None,
    /// `"node:interrupts"` -- the bound node's wired GIC INTID(s).
    Node,
    /// `"msi:N"` -- N MSI vectors. Resolved by the PCIe source (not a DTB node),
    /// so `resolve` against a DTB node yields no INTIDs for this variant; it is
    /// carried for the real-hardware (RPi5 / brcmstb) path.
    Msi(u32),
}

/// What DMA a driver needs -- a budget, not a device resource (DMA is memory the
/// driver allocates, capped by the manifest; the I-34 `dma_max` axis).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DmaNeed {
    /// No DMA.
    None,
    /// `"pool: N"` -- a per-buffer DMA ceiling of N bytes.
    Pool(u64),
}

/// The supervisor restart policy (MENAGERIE.md section 5 -- supervision).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Restart {
    /// Do not restart on exit.
    Never,
    /// Restart on a crash (a non-zero exit), bounded by the supervisor's
    /// back-off + give-up threshold.
    OnCrash,
    /// Restart on any exit.
    Always,
}

impl Manifest {
    /// Parse a manifest from its section-6 brace-block text. Returns
    /// `Error::Parse` on any malformed input -- the warden treats a bad manifest
    /// as "this driver is not bindable", never a panic.
    pub fn parse(text: &str) -> Result<Manifest, Error> {
        let toks = tokenize(text)?;
        let mut p = Parser {
            toks: &toks,
            pos: 0,
        };
        let m = p.parse_manifest()?;
        if p.pos != p.toks.len() {
            return Err(Error::Parse); // trailing garbage after the closing brace
        }
        Ok(m)
    }

    /// Emit the canonical section-6 brace-block text. The inverse of `parse`
    /// (`parse(m.to_text())` recovers `m`); used for round-trip testing and as
    /// the documented canonical form. Not used at runtime in v1.0.
    pub fn to_text(&self) -> String {
        let mut s = String::new();
        s.push_str("driver \"");
        s.push_str(&self.name);
        s.push_str("\" {\n");
        push_kv_u32(&mut s, "abi", self.abi);
        s.push_str("    binds = [");
        for (i, b) in self.binds.iter().enumerate() {
            if i != 0 {
                s.push_str(", ");
            }
            s.push('"');
            s.push_str(b);
            s.push('"');
        }
        s.push_str("]\n");
        s.push_str("    needs {\n");
        s.push_str("        mmio = \"");
        s.push_str(mmio_need_str(self.needs.mmio));
        s.push_str("\"\n        irq = \"");
        push_irq_need_str(&mut s, self.needs.irq);
        s.push_str("\"\n        dma = \"");
        push_dma_need_str(&mut s, self.needs.dma);
        s.push_str("\"\n    }\n");
        s.push_str("    serves = \"");
        s.push_str(&self.serves);
        s.push_str("\"\n    restart = ");
        s.push_str(restart_str(self.restart));
        s.push('\n');
        if let Some(sig) = &self.sig {
            s.push_str("    sig = \"");
            s.push_str(sig);
            s.push_str("\"\n");
        }
        s.push_str("}\n");
        s
    }
}

// =============================================================================
// to_text helpers
// =============================================================================

fn push_kv_u32(s: &mut String, key: &str, v: u32) {
    s.push_str("    ");
    s.push_str(key);
    s.push_str(" = ");
    s.push_str(&v.to_string());
    s.push('\n');
}

fn mmio_need_str(n: MmioNeed) -> &'static str {
    match n {
        MmioNeed::None => "none",
        MmioNeed::Node => "node:reg",
    }
}

fn push_irq_need_str(s: &mut String, n: IrqNeed) {
    match n {
        IrqNeed::None => s.push_str("none"),
        IrqNeed::Node => s.push_str("node:interrupts"),
        IrqNeed::Msi(count) => {
            s.push_str("msi:");
            s.push_str(&count.to_string());
        }
    }
}

fn push_dma_need_str(s: &mut String, n: DmaNeed) {
    match n {
        DmaNeed::None => s.push_str("none"),
        DmaNeed::Pool(bytes) => {
            s.push_str("pool:");
            s.push_str(&bytes.to_string());
        }
    }
}

fn restart_str(r: Restart) -> &'static str {
    match r {
        Restart::Never => "never",
        Restart::OnCrash => "on-crash",
        Restart::Always => "always",
    }
}

// =============================================================================
// Tokenizer
// =============================================================================

#[derive(Clone, Debug, PartialEq, Eq)]
enum Tok {
    Ident(String),
    Str(String),
    Int(u64),
    LBrace,
    RBrace,
    LBrack,
    RBrack,
    Eq,
    Comma,
}

fn tokenize(text: &str) -> Result<Vec<Tok>, Error> {
    let bytes = text.as_bytes();
    let mut i = 0usize;
    let mut toks = Vec::new();
    while i < bytes.len() {
        let c = bytes[i];
        match c {
            b' ' | b'\t' | b'\r' | b'\n' => {
                i += 1;
            }
            b'#' => {
                while i < bytes.len() && bytes[i] != b'\n' {
                    i += 1;
                }
            }
            b'{' => {
                toks.push(Tok::LBrace);
                i += 1;
            }
            b'}' => {
                toks.push(Tok::RBrace);
                i += 1;
            }
            b'[' => {
                toks.push(Tok::LBrack);
                i += 1;
            }
            b']' => {
                toks.push(Tok::RBrack);
                i += 1;
            }
            b'=' => {
                toks.push(Tok::Eq);
                i += 1;
            }
            b',' => {
                toks.push(Tok::Comma);
                i += 1;
            }
            b'"' => {
                i += 1; // opening quote
                let start = i;
                while i < bytes.len() && bytes[i] != b'"' {
                    if bytes[i] == b'\n' {
                        return Err(Error::Parse); // unterminated string on this line
                    }
                    i += 1;
                }
                if i >= bytes.len() {
                    return Err(Error::Parse); // unterminated string at EOF
                }
                let s = core::str::from_utf8(&bytes[start..i]).map_err(|_| Error::Parse)?;
                toks.push(Tok::Str(s.to_string()));
                i += 1; // closing quote
            }
            b'0'..=b'9' => {
                let start = i;
                while i < bytes.len() && bytes[i].is_ascii_digit() {
                    i += 1;
                }
                let s = core::str::from_utf8(&bytes[start..i]).map_err(|_| Error::Parse)?;
                let v: u64 = s.parse().map_err(|_| Error::Parse)?;
                toks.push(Tok::Int(v));
            }
            _ if is_ident_byte(c) => {
                let start = i;
                while i < bytes.len() && is_ident_byte(bytes[i]) {
                    i += 1;
                }
                let s = core::str::from_utf8(&bytes[start..i]).map_err(|_| Error::Parse)?;
                toks.push(Tok::Ident(s.to_string()));
            }
            _ => return Err(Error::Parse), // an unexpected byte
        }
    }
    Ok(toks)
}

fn is_ident_byte(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_' || c == b'-'
}

// =============================================================================
// Recursive-descent parser
// =============================================================================

struct Parser<'a> {
    toks: &'a [Tok],
    pos: usize,
}

impl<'a> Parser<'a> {
    fn peek(&self) -> Option<&Tok> {
        self.toks.get(self.pos)
    }

    fn next(&mut self) -> Result<&Tok, Error> {
        let t = self.toks.get(self.pos).ok_or(Error::Parse)?;
        self.pos += 1;
        Ok(t)
    }

    fn expect(&mut self, want: &Tok) -> Result<(), Error> {
        if self.next()? == want {
            Ok(())
        } else {
            Err(Error::Parse)
        }
    }

    fn expect_ident(&mut self) -> Result<String, Error> {
        match self.next()? {
            Tok::Ident(s) => Ok(s.clone()),
            _ => Err(Error::Parse),
        }
    }

    fn expect_str(&mut self) -> Result<String, Error> {
        match self.next()? {
            Tok::Str(s) => Ok(s.clone()),
            _ => Err(Error::Parse),
        }
    }

    fn parse_manifest(&mut self) -> Result<Manifest, Error> {
        // header: driver "<name>" {
        if self.expect_ident()? != "driver" {
            return Err(Error::Parse);
        }
        let name = self.expect_str()?;
        self.expect(&Tok::LBrace)?;

        let mut abi: Option<u32> = None;
        let mut binds: Option<Vec<String>> = None;
        let mut needs: Option<Needs> = None;
        let mut serves: Option<String> = None;
        let mut restart: Option<Restart> = None;
        let mut sig: Option<String> = None;

        loop {
            match self.peek() {
                Some(Tok::RBrace) => {
                    self.pos += 1;
                    break;
                }
                Some(Tok::Ident(_)) => {}
                _ => return Err(Error::Parse),
            }
            let key = self.expect_ident()?;
            match key.as_str() {
                "needs" => {
                    if needs.is_some() {
                        return Err(Error::Parse); // duplicate
                    }
                    needs = Some(self.parse_needs()?);
                }
                "abi" => {
                    if abi.is_some() {
                        return Err(Error::Parse);
                    }
                    self.expect(&Tok::Eq)?;
                    abi = Some(self.parse_u32()?);
                }
                "binds" => {
                    if binds.is_some() {
                        return Err(Error::Parse);
                    }
                    self.expect(&Tok::Eq)?;
                    binds = Some(self.parse_str_list()?);
                }
                "serves" => {
                    if serves.is_some() {
                        return Err(Error::Parse);
                    }
                    self.expect(&Tok::Eq)?;
                    serves = Some(self.expect_str()?);
                }
                "restart" => {
                    if restart.is_some() {
                        return Err(Error::Parse);
                    }
                    self.expect(&Tok::Eq)?;
                    restart = Some(self.parse_restart()?);
                }
                "sig" => {
                    if sig.is_some() {
                        return Err(Error::Parse);
                    }
                    self.expect(&Tok::Eq)?;
                    sig = Some(self.expect_str()?);
                }
                _ => return Err(Error::Parse), // unknown key
            }
        }

        let binds = binds.ok_or(Error::Parse)?;
        if binds.is_empty() {
            return Err(Error::Parse); // a driver that binds nothing is meaningless
        }
        Ok(Manifest {
            name,
            abi: abi.ok_or(Error::Parse)?,
            binds,
            needs: needs.unwrap_or(Needs::NONE),
            serves: serves.ok_or(Error::Parse)?,
            restart: restart.unwrap_or(Restart::OnCrash),
            sig,
        })
    }

    fn parse_needs(&mut self) -> Result<Needs, Error> {
        self.expect(&Tok::LBrace)?;
        let mut mmio: Option<MmioNeed> = None;
        let mut irq: Option<IrqNeed> = None;
        let mut dma: Option<DmaNeed> = None;
        loop {
            match self.peek() {
                Some(Tok::RBrace) => {
                    self.pos += 1;
                    break;
                }
                Some(Tok::Ident(_)) => {}
                _ => return Err(Error::Parse),
            }
            let key = self.expect_ident()?;
            self.expect(&Tok::Eq)?;
            let val = self.expect_str()?;
            match key.as_str() {
                "mmio" => {
                    if mmio.is_some() {
                        return Err(Error::Parse);
                    }
                    mmio = Some(parse_mmio_need(&val)?);
                }
                "irq" => {
                    if irq.is_some() {
                        return Err(Error::Parse);
                    }
                    irq = Some(parse_irq_need(&val)?);
                }
                "dma" => {
                    if dma.is_some() {
                        return Err(Error::Parse);
                    }
                    dma = Some(parse_dma_need(&val)?);
                }
                _ => return Err(Error::Parse),
            }
        }
        Ok(Needs {
            mmio: mmio.unwrap_or(MmioNeed::None),
            irq: irq.unwrap_or(IrqNeed::None),
            dma: dma.unwrap_or(DmaNeed::None),
        })
    }

    fn parse_u32(&mut self) -> Result<u32, Error> {
        match self.next()? {
            Tok::Int(v) => u32::try_from(*v).map_err(|_| Error::Parse),
            _ => Err(Error::Parse),
        }
    }

    fn parse_str_list(&mut self) -> Result<Vec<String>, Error> {
        self.expect(&Tok::LBrack)?;
        let mut out = Vec::new();
        // empty list
        if self.peek() == Some(&Tok::RBrack) {
            self.pos += 1;
            return Ok(out);
        }
        loop {
            out.push(self.expect_str()?);
            match self.next()? {
                Tok::Comma => {
                    // allow a trailing comma before ]
                    if self.peek() == Some(&Tok::RBrack) {
                        self.pos += 1;
                        break;
                    }
                }
                Tok::RBrack => break,
                _ => return Err(Error::Parse),
            }
        }
        Ok(out)
    }

    fn parse_restart(&mut self) -> Result<Restart, Error> {
        match self.next()? {
            Tok::Ident(s) => match s.as_str() {
                "never" => Ok(Restart::Never),
                "on-crash" => Ok(Restart::OnCrash),
                "always" => Ok(Restart::Always),
                _ => Err(Error::Parse),
            },
            _ => Err(Error::Parse),
        }
    }
}

// =============================================================================
// `needs` value parsers (the section-6 need strings)
// =============================================================================

fn parse_mmio_need(s: &str) -> Result<MmioNeed, Error> {
    match s.trim() {
        "none" => Ok(MmioNeed::None),
        "node:reg" => Ok(MmioNeed::Node),
        _ => Err(Error::Parse),
    }
}

fn parse_irq_need(s: &str) -> Result<IrqNeed, Error> {
    let s = s.trim();
    if s == "none" {
        return Ok(IrqNeed::None);
    }
    if s == "node:interrupts" {
        return Ok(IrqNeed::Node);
    }
    if let Some(rest) = s.strip_prefix("msi:") {
        let n: u32 = rest.trim().parse().map_err(|_| Error::Parse)?;
        return Ok(IrqNeed::Msi(n));
    }
    Err(Error::Parse)
}

fn parse_dma_need(s: &str) -> Result<DmaNeed, Error> {
    let s = s.trim();
    if s == "none" {
        return Ok(DmaNeed::None);
    }
    if let Some(rest) = s.strip_prefix("pool:") {
        let bytes = parse_size(rest.trim())?;
        if bytes == 0 {
            return Ok(DmaNeed::None);
        }
        return Ok(DmaNeed::Pool(bytes));
    }
    Err(Error::Parse)
}

/// Parse a byte count with an optional binary unit: `2097152`, `"2 MiB"`,
/// `"2MiB"`, `"512 KiB"`, `"1 GiB"`. Units are binary (1 KiB = 1024). Overflow
/// rejects.
fn parse_size(s: &str) -> Result<u64, Error> {
    let s = s.trim();
    // split the leading digits from the trailing unit
    let digits_end = s.find(|c: char| !c.is_ascii_digit()).unwrap_or(s.len());
    if digits_end == 0 {
        return Err(Error::Parse);
    }
    let num: u64 = s[..digits_end].parse().map_err(|_| Error::Parse)?;
    let unit = s[digits_end..].trim();
    let mult: u64 = match unit.to_ascii_lowercase().as_str() {
        "" | "b" => 1,
        "k" | "kb" | "kib" => 1024,
        "m" | "mb" | "mib" => 1024 * 1024,
        "g" | "gb" | "gib" => 1024 * 1024 * 1024,
        _ => return Err(Error::Parse),
    };
    num.checked_mul(mult).ok_or(Error::Parse)
}

#[cfg(test)]
mod tests {
    use super::*;

    const EXAMPLE: &str = r#"
# the section-6 example
driver "rp1-eth" {
    abi      = 1
    binds    = ["raspberrypi,rp1-eth", "brcm,genet-v5"]
    needs {
        mmio = "node:reg"
        irq  = "msi:1"
        dma  = "pool: 2 MiB"
    }
    serves   = "/dev/net/%instance"
    restart  = on-crash
    sig      = "ed25519-sig-bytes"
}
"#;

    #[test]
    fn parses_the_section6_example() {
        let m = Manifest::parse(EXAMPLE).expect("parse");
        assert_eq!(m.name, "rp1-eth");
        assert_eq!(m.abi, 1);
        assert_eq!(m.binds, ["raspberrypi,rp1-eth", "brcm,genet-v5"]);
        assert_eq!(m.needs.mmio, MmioNeed::Node);
        assert_eq!(m.needs.irq, IrqNeed::Msi(1));
        assert_eq!(m.needs.dma, DmaNeed::Pool(2 * 1024 * 1024));
        assert_eq!(m.serves, "/dev/net/%instance");
        assert_eq!(m.restart, Restart::OnCrash);
        assert_eq!(m.sig.as_deref(), Some("ed25519-sig-bytes"));
    }

    #[test]
    fn round_trips_through_to_text() {
        let m = Manifest::parse(EXAMPLE).expect("parse");
        let text = m.to_text();
        let m2 = Manifest::parse(&text).expect("re-parse");
        assert_eq!(m, m2);
    }

    #[test]
    fn wired_irq_and_no_dma() {
        let src = r#"driver "blk" {
            abi = 1
            binds = ["virtio,mmio"]
            needs { mmio = "node:reg" irq = "node:interrupts" dma = "none" }
            serves = "/dev/blk/%instance"
            restart = always
        }"#;
        let m = Manifest::parse(src).expect("parse");
        assert_eq!(m.needs.irq, IrqNeed::Node);
        assert_eq!(m.needs.dma, DmaNeed::None);
        assert_eq!(m.restart, Restart::Always);
        assert_eq!(m.sig, None);
    }

    #[test]
    fn defaults_when_optional_fields_absent() {
        // no needs block, no restart, no sig
        let src = r#"driver "x" { abi = 1 binds = ["a"] serves = "/dev/x" }"#;
        let m = Manifest::parse(src).expect("parse");
        assert_eq!(m.needs, Needs::NONE);
        assert_eq!(m.restart, Restart::OnCrash);
        assert_eq!(m.sig, None);
    }

    #[test]
    fn size_units() {
        assert_eq!(parse_size("0").unwrap(), 0);
        assert_eq!(parse_size("4096").unwrap(), 4096);
        assert_eq!(parse_size("2 MiB").unwrap(), 2 * 1024 * 1024);
        assert_eq!(parse_size("2MiB").unwrap(), 2 * 1024 * 1024);
        assert_eq!(parse_size("512 KiB").unwrap(), 512 * 1024);
        assert_eq!(parse_size("1 GiB").unwrap(), 1024 * 1024 * 1024);
        assert_eq!(parse_size("64 B").unwrap(), 64);
        assert!(parse_size("MiB").is_err()); // no number
        assert!(parse_size("4 furlongs").is_err()); // bad unit
        assert!(parse_size("99999999999999999999 GiB").is_err()); // overflow
    }

    #[test]
    fn rejects_malformed() {
        assert!(Manifest::parse("").is_err()); // empty
        assert!(Manifest::parse("driver \"x\" {").is_err()); // unterminated block
        assert!(Manifest::parse("driver x { abi = 1 binds=[\"a\"] serves=\"/x\" }").is_err()); // unquoted name
        assert!(Manifest::parse("driver \"x\" { abi=1 serves=\"/x\" }").is_err()); // missing binds
        assert!(Manifest::parse("driver \"x\" { binds=[\"a\"] serves=\"/x\" }").is_err()); // missing abi
        assert!(Manifest::parse("driver \"x\" { abi=1 binds=[\"a\"] }").is_err()); // missing serves
        assert!(Manifest::parse("driver \"x\" { abi=1 binds=[] serves=\"/x\" }").is_err()); // empty binds
        assert!(
            Manifest::parse("driver \"x\" { abi=1 abi=2 binds=[\"a\"] serves=\"/x\" }").is_err()
        ); // duplicate key
        assert!(
            Manifest::parse("driver \"x\" { abi=1 binds=[\"a\"] serves=\"/x\" bogus=1 }").is_err()
        ); // unknown key
        assert!(Manifest::parse(
            "driver \"x\" { abi=1 binds=[\"a\"] serves=\"/x\" needs { mmio=\"node:bogus\" } }"
        )
        .is_err()); // bad need value
        assert!(Manifest::parse(
            "driver \"x\" { abi=1 binds=[\"a\"] serves=\"/x\" restart=sometimes }"
        )
        .is_err()); // bad restart
        assert!(
            Manifest::parse("driver \"x\" { abi=1 binds=[\"a\"] serves=\"/x\" } extra").is_err()
        );
        // trailing garbage
    }

    #[test]
    fn comments_and_whitespace_tolerant() {
        let src = "  # lead\n driver \"x\"{abi=1 # inline\n binds=[\"a\"]serves=\"/x\"}  # trail\n";
        let m = Manifest::parse(src).expect("parse");
        assert_eq!(m.name, "x");
        assert_eq!(m.binds, ["a"]);
    }
}
