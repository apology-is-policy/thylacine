//! Canonical MDTM v1 records. Successful decode means well-formed data only.
//! Neither a founding flag nor a kernel Activation can be deserialized here.
use crate::{abi::*, *};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Record {
    pub mandate: Mandate,
    pub transaction: [u8; 16],
}

struct Reader<'a> {
    bytes: &'a [u8],
    pos: usize,
}
impl<'a> Reader<'a> {
    fn take<const N: usize>(&mut self) -> Result<[u8; N], Error> {
        let end = self.pos.checked_add(N).ok_or(Error::Invalid)?;
        let bytes = self.bytes.get(self.pos..end).ok_or(Error::Invalid)?;
        self.pos = end;
        bytes.try_into().map_err(|_| Error::Invalid)
    }
    fn u8(&mut self) -> Result<u8, Error> {
        Ok(self.take::<1>()?[0])
    }
    fn u16(&mut self) -> Result<usize, Error> {
        Ok(u16::from_le_bytes(self.take()?) as usize)
    }
    fn u32(&mut self) -> Result<u32, Error> {
        Ok(u32::from_le_bytes(self.take()?))
    }
    fn u64(&mut self) -> Result<u64, Error> {
        Ok(u64::from_le_bytes(self.take()?))
    }
    fn count(&mut self, max: usize, allow_empty: bool) -> Result<usize, Error> {
        let n = self.u16()?;
        if n > max || (!allow_empty && n == 0) {
            Err(Error::Invalid)
        } else {
            Ok(n)
        }
    }
    fn subjects(&mut self, n: usize) -> Result<Vec<u32>, Error> {
        let mut v = Vec::new();
        v.try_reserve_exact(n).map_err(|_| Error::Capacity)?;
        for _ in 0..n {
            v.push(self.u32()?);
        }
        Ok(v)
    }
    fn resources(&mut self, n: usize) -> Result<Vec<Resource>, Error> {
        let mut v = Vec::new();
        v.try_reserve_exact(n).map_err(|_| Error::Capacity)?;
        for _ in 0..n {
            v.push(Resource {
                owner: self.u64()?,
                object: self.u64()?,
            });
        }
        Ok(v)
    }
}
fn auth(value: u8) -> Result<Auth, Error> {
    match value as usize {
        AUTH_SESSION => Ok(Auth::Session),
        AUTH_DISTINCT_KEY => Ok(Auth::DistinctKey),
        AUTH_FOUNDING => Ok(Auth::Founding),
        _ => Err(Error::Invalid),
    }
}
fn auth_byte(value: Auth) -> u8 {
    (match value {
        Auth::Session => AUTH_SESSION,
        Auth::DistinctKey => AUTH_DISTINCT_KEY,
        Auth::Founding => AUTH_FOUNDING,
    }) as u8
}
fn term(kind: u8, end: u64) -> Result<Term, Error> {
    match (kind as usize, end) {
        (TERM_UNTIL_REVOKED, 0) => Ok(Term::UntilRevoked),
        (TERM_UNTIL_UTC, 1..) => Ok(Term::UntilUtc(end)),
        _ => Err(Error::Invalid),
    }
}
fn term_parts(value: Term) -> (u8, u64) {
    match value {
        Term::UntilRevoked => (TERM_UNTIL_REVOKED as u8, 0),
        Term::UntilUtc(end) => (TERM_UNTIL_UTC as u8, end),
    }
}
fn put16(out: &mut Vec<u8>, value: usize) {
    out.extend_from_slice(&(value as u16).to_le_bytes());
}
fn put32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}
fn put64(out: &mut Vec<u8>, value: u64) {
    out.extend_from_slice(&value.to_le_bytes());
}
fn selectors(out: &mut Vec<u8>, scope: &Scope) {
    for &s in &scope.subjects {
        put32(out, s);
    }
    for r in &scope.resources {
        put64(out, r.owner);
        put64(out, r.object);
    }
}

impl Record {
    fn validate(&self) -> Result<(), Error> {
        if self.transaction == [0; 16] {
            return Err(Error::Invalid);
        }
        self.mandate.validate(self.mandate.supports.is_empty())
    }
    /// Reserve the proven maximum before serializing. Even hostile in-memory
    /// objects are validated before an allocation or lossy length conversion.
    pub fn encode(&self) -> Result<Vec<u8>, Error> {
        self.validate()?;
        let m = &self.mandate;
        let mut out = Vec::new();
        out.try_reserve_exact(MANDATE_MAX_LEN)
            .map_err(|_| Error::Capacity)?;
        put32(&mut out, MANDATE_MAGIC as u32);
        put16(&mut out, MANDATE_VERSION);
        put16(&mut out, 0);
        put32(&mut out, 0); // total patched after vectors
        out.push(match m.kind {
            Kind::Use => KIND_USE,
            Kind::Activate => KIND_ACTIVATE,
            Kind::Admin => KIND_ADMIN,
        } as u8);
        out.push(auth_byte(m.authentication));
        out.push(match m.state {
            State::Live => STATE_LIVE,
            State::Revoking => STATE_REVOKING,
            State::Revoked => STATE_REVOKED,
        } as u8);
        let (term_kind, term_end) = term_parts(m.term);
        out.push(term_kind);
        put64(&mut out, m.reference.id);
        put64(&mut out, m.reference.revision);
        put32(&mut out, m.subject);
        put32(&mut out, m.issuer);
        put64(&mut out, m.scope.domain);
        put64(&mut out, m.domain_generation);
        put64(&mut out, m.scope.actions.0);
        put64(&mut out, term_end);
        out.extend_from_slice(&self.transaction);
        put16(&mut out, m.scope.subjects.len());
        put16(&mut out, m.scope.resources.len());
        put16(&mut out, m.supports.len());
        out.push(u8::from(m.envelope.is_some()));
        out.push(0);
        selectors(&mut out, &m.scope);
        for r in &m.supports {
            put64(&mut out, r.id);
            put64(&mut out, r.revision);
        }
        if let Some(e) = &m.envelope {
            put64(&mut out, e.scope.domain);
            put64(&mut out, e.scope.actions.0);
            let (kind, end) = term_parts(e.max_term);
            put64(&mut out, end);
            out.push(e.kinds.0);
            out.push(auth_byte(e.auth_floor));
            out.push(e.delegation_depth);
            out.push(kind);
            put16(&mut out, e.scope.subjects.len());
            put16(&mut out, e.scope.resources.len());
            selectors(&mut out, &e.scope);
        }
        // Bounds follow from validate() and are asserted again at the boundary.
        if out.len() > MANDATE_MAX_LEN {
            return Err(Error::Capacity);
        }
        let len = out.len() as u32;
        out[8..12].copy_from_slice(&len.to_le_bytes());
        Ok(out)
    }
    pub fn decode(bytes: &[u8]) -> Result<Self, Error> {
        if !(MANDATE_HEADER_LEN..=MANDATE_MAX_LEN).contains(&bytes.len()) {
            return Err(Error::Invalid);
        }
        let mut r = Reader { bytes, pos: 0 };
        if r.u32()? != MANDATE_MAGIC as u32
            || r.u16()? != MANDATE_VERSION
            || r.u16()? != 0
            || r.u32()? as usize != bytes.len()
        {
            return Err(Error::Invalid);
        }
        let kind = match r.u8()? as usize {
            KIND_USE => Kind::Use,
            KIND_ACTIVATE => Kind::Activate,
            KIND_ADMIN => Kind::Admin,
            _ => return Err(Error::Invalid),
        };
        let authentication = auth(r.u8()?)?;
        let state = match r.u8()? as usize {
            STATE_LIVE => State::Live,
            STATE_REVOKING => State::Revoking,
            STATE_REVOKED => State::Revoked,
            _ => return Err(Error::Invalid),
        };
        let term_kind = r.u8()?;
        let reference = Reference {
            id: r.u64()?,
            revision: r.u64()?,
        };
        let subject = r.u32()?;
        let issuer = r.u32()?;
        let domain = r.u64()?;
        let domain_generation = r.u64()?;
        let actions = Actions::new(r.u64()?)?;
        let term = term(term_kind, r.u64()?)?;
        let transaction = r.take()?;
        let ns = r.count(MAX_SELECTORS, false)?;
        let nr = r.count(MAX_SELECTORS, false)?;
        let np = r.count(MAX_SUPPORTS, true)?;
        let has_envelope = match r.u8()? {
            0 => false,
            1 => true,
            _ => return Err(Error::Invalid),
        };
        if r.u8()? != 0 {
            return Err(Error::Invalid);
        }
        let scope = Scope {
            domain,
            actions,
            subjects: r.subjects(ns)?,
            resources: r.resources(nr)?,
        };
        let mut supports = Vec::new();
        supports
            .try_reserve_exact(np)
            .map_err(|_| Error::Capacity)?;
        for _ in 0..np {
            supports.push(Reference {
                id: r.u64()?,
                revision: r.u64()?,
            });
        }
        let envelope = if has_envelope {
            let domain = r.u64()?;
            let actions = Actions::new(r.u64()?)?;
            let end = r.u64()?;
            let kinds = Kinds::new(r.u8()?)?;
            let auth_floor = auth(r.u8()?)?;
            let delegation_depth = r.u8()?;
            let max_term = self::term(r.u8()?, end)?;
            let ns = r.count(MAX_SELECTORS, false)?;
            let nr = r.count(MAX_SELECTORS, false)?;
            Some(Envelope {
                kinds,
                scope: Scope {
                    domain,
                    actions,
                    subjects: r.subjects(ns)?,
                    resources: r.resources(nr)?,
                },
                max_term,
                auth_floor,
                delegation_depth,
            })
        } else {
            None
        };
        if r.pos != bytes.len() {
            return Err(Error::Invalid);
        }
        let result = Self {
            mandate: Mandate {
                reference,
                subject,
                issuer,
                kind,
                scope,
                term,
                authentication,
                envelope,
                supports,
                domain_generation,
                state,
            },
            transaction,
        };
        result.validate()?;
        Ok(result)
    }
}
