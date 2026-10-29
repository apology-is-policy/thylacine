//! Model-mapped, immutable issuance approval (UA-3 pure core, no runtime yet).
//!
//! Peer/proof/receipt inputs MUST come from kernel/TCB observations, never from
//! a decoded client claim. A prepared preview confers no authority. Admitted
//! output is deliberately not Clone and does not itself publish policy. The
//! durable owner must serialize publication with source restriction, persist
//! its audit, and recheck support at materialization (mandate_commit::Publish).
use crate::*;

pub const PREPARE_LIFETIME_NS: u64 = 120_000_000_000;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Peer {
    pub principal: u32,
    pub stripes: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ScopeKind {
    Execution,
    Administrative,
}
#[derive(Clone, Copy, Debug)]
pub struct Proof {
    pub activation: Activation,
    pub kind: ScopeKind,
    pub transaction: [u8; 16],
    pub root_stripes: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase {
    Prepared,
    AwaitingSak,
    Visible,
    Authenticated,
    Restored,
    Cancelled,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ViewReceipt {
    pub transaction: [u8; 16],
    pub policy_revision: u64,
    pub episode: u64,
    pub sequence: u64,
}

/// One exact resolved grant. The caller owns pre-auth transaction quotas and
/// storage/audit reservations; this object only reserves its bounded byte image.
#[derive(Debug)]
pub struct PreparedIssue {
    record: Mandate,
    canonical: Vec<u8>,
    peer: Peer,
    source: Reference,
    revision: u64,
    expires: u64,
    required_auth: Auth,
    verified_auth: Option<Auth>,
    receipt: Option<ViewReceipt>,
    phase: Phase,
}

/// Admit is the authorization linearization point. Root exit/active-scope expiry
/// afterwards do not erase this exact decision. Persistent supports, generations
/// and audit are still publication obligations; no runtime consumer exists yet.
#[derive(Debug)]
pub struct AdmittedIssue {
    record: Mandate,
    canonical: Vec<u8>,
    peer: Peer,
    source: Reference,
    policy_revision: u64,
    scope: u64,
    authentication: Auth,
    admitted_mono: u64,
}
impl AdmittedIssue {
    pub fn record(&self) -> &Mandate {
        &self.record
    }
    pub fn canonical(&self) -> &[u8] {
        &self.canonical
    }
    pub fn peer(&self) -> Peer {
        self.peer
    }
    pub fn source(&self) -> Reference {
        self.source
    }
    pub fn policy_revision(&self) -> u64 {
        self.policy_revision
    }
    pub fn scope(&self) -> u64 {
        self.scope
    }
    pub fn authentication(&self) -> Auth {
        self.authentication
    }
    pub fn admitted_mono(&self) -> u64 {
        self.admitted_mono
    }
}
impl PreparedIssue {
    /// mandate_commit::Prepare. Planning uses eligibility only; no fake kernel
    /// Activation is manufactured to get through the authorization function.
    pub fn new(
        ledger: &Ledger,
        peer: Peer,
        source: Reference,
        record: Mandate,
        time: Time,
    ) -> Result<Self, Error> {
        if peer.stripes == 0 {
            return Err(Error::Invalid);
        }
        ledger.preview_issue(peer.principal, source, &record, time)?;
        let canonical = record.encode()?;
        let expires = time
            .mono
            .checked_add(PREPARE_LIFETIME_NS)
            .ok_or(Error::Overflow)?;
        Ok(Self {
            record,
            canonical,
            peer,
            source,
            revision: ledger.revision(),
            expires,
            required_auth: ledger.get(source)?.authentication,
            verified_auth: None,
            receipt: None,
            phase: Phase::Prepared,
        })
    }
    pub fn phase(&self) -> Phase {
        self.phase
    }
    pub fn record(&self) -> &Mandate {
        &self.record
    }
    pub fn canonical(&self) -> &[u8] {
        &self.canonical
    }
    pub fn revision(&self) -> u64 {
        self.revision
    }
    fn before_auth(&self, now: u64) -> Result<(), Error> {
        if now >= self.expires {
            Err(Error::Expired)
        } else {
            Ok(())
        }
    }
    pub fn await_sak(&mut self, now: u64) -> Result<(), Error> {
        if self.phase != Phase::Prepared {
            return Err(Error::Conflict);
        }
        self.before_auth(now)?;
        self.phase = Phase::AwaitingSak;
        Ok(())
    }
    /// mandate_commit::Show. Runtime supplies the receipt only once the trusted
    /// renderer has shown ALL semantic pages and enabled final confirmation.
    /// An ordinary client/UI cannot provide this acknowledgement.
    pub fn visible(&mut self, receipt: ViewReceipt, now: u64) -> Result<(), Error> {
        if self.phase != Phase::AwaitingSak {
            return Err(Error::Conflict);
        }
        self.before_auth(now)?;
        if receipt.transaction != self.record.transaction
            || receipt.policy_revision != self.revision
            || receipt.episode == 0
            || receipt.sequence == 0
        {
            return Err(Error::Denied);
        }
        self.receipt = Some(receipt);
        self.phase = Phase::Visible;
        Ok(())
    }
    /// mandate_commit::Authenticate. Corvus calls only after checking the key
    /// through the active trusted episode; no key bytes live in this object.
    pub fn authenticated(
        &mut self,
        receipt: ViewReceipt,
        auth: Auth,
        now: u64,
    ) -> Result<(), Error> {
        if self.phase != Phase::Visible {
            return Err(Error::Conflict);
        }
        self.before_auth(now)?;
        if Some(receipt) != self.receipt || !auth.meets(self.required_auth) {
            return Err(Error::Denied);
        }
        self.verified_auth = Some(auth);
        self.phase = Phase::Authenticated;
        Ok(())
    }
    /// mandate_commit::Restore. Only a successful kernel restoration receipt
    /// enters Restored. Caller cancels on failure, never admits in the episode.
    pub fn restored(&mut self, episode: u64) -> Result<(), Error> {
        if self.phase != Phase::Authenticated {
            return Err(Error::Conflict);
        }
        if self.receipt.map(|r| r.episode) != Some(episode) {
            return Err(Error::Denied);
        }
        self.phase = Phase::Restored;
        Ok(())
    }
    /// Cancellation/requester death BEFORE admission drops the decision.
    pub fn cancel(&mut self) {
        self.phase = Phase::Cancelled;
        self.verified_auth = None;
    }
    /// mandate_commit::Admit(root). Consumes the transaction; no child may use
    /// a copied ID, and no mutable payload can be substituted after the preview.
    pub fn admit(
        self,
        ledger: &Ledger,
        peer: Peer,
        proof: Proof,
        time: Time,
    ) -> Result<AdmittedIssue, Error> {
        if self.phase != Phase::Restored {
            return Err(Error::Conflict);
        }
        let a = &proof.activation;
        if peer != self.peer
            || proof.root_stripes != peer.stripes
            || proof.transaction != self.record.transaction
            || proof.kind != ScopeKind::Administrative
            || a.actor != peer.principal
            || a.authority != self.source
            || Some(a.authentication) != self.verified_auth
        {
            return Err(Error::Denied);
        }
        if ledger.revision() != self.revision || a.policy_revision != self.revision {
            return Err(Error::Conflict);
        }
        ledger.check_issue(a, &self.record, time)?;
        Ok(AdmittedIssue {
            record: self.record,
            canonical: self.canonical,
            peer,
            source: self.source,
            policy_revision: self.revision,
            scope: a.scope_id,
            authentication: a.authentication,
            admitted_mono: time.mono,
        })
    }
}
