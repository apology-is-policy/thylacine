//! Corvus's bounded, pure authority policy engine (UA-1).
//!
//! This crate owns no secret, syscall, clock or persistence primitive. A caller
//! must supply the live principal, trusted time, and a kernel-authenticated
//! administrative activation. The ledger validates every record at insertion
//! and rechecks the complete support graph on use; indexes/caches are never the
//! source of authority. Runtime integration MUST NOT substitute user-supplied
//! values for those inputs. See docs/USER-AUTHORITY-DESIGN.md.
//!
//! Storage is intentionally bounded. Grants have one immutable record revision;
//! replacement is revoke + a newly allocated ID. IDs are never reused, including
//! revoked records. The durable allocator/replay layer owns monotonic IDs across
//! restarts. Record/action wire reservations are in abi.rs; decoding never confers authority.
#![no_std]
extern crate alloc;

pub mod abi;
pub mod codec;

use alloc::vec::Vec;

pub const MAX_SELECTORS: usize = 16;
pub const MAX_SUPPORTS: usize = 8;
pub const MAX_DEPTH: usize = 16;
pub const MAX_RECORDS: usize = 4096;
pub const MAX_PER_SUBJECT: usize = 256;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Invalid,
    UnknownAction,
    EmptyAuthority,
    Duplicate,
    Capacity,
    Missing,
    Stale,
    Revoked,
    Expired,
    ClockUnavailable,
    Denied,
    SelfEscalation,
    Cycle,
    Depth,
    Inactive,
    Conflict,
    Overflow,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Use,
    Activate,
    Admin,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Kinds(u8);
impl Kinds {
    pub const USE: Self = Self(1);
    pub const ACTIVATE: Self = Self(2);
    pub const ADMIN: Self = Self(4);
    pub const ALL: Self = Self(7);
    pub fn new(bits: u8) -> Result<Self, Error> {
        if bits == 0 || bits & !7 != 0 {
            Err(Error::Invalid)
        } else {
            Ok(Self(bits))
        }
    }
    pub fn contains(self, kind: Kind) -> bool {
        let bit = match kind {
            Kind::Use => 1,
            Kind::Activate => 2,
            Kind::Admin => 4,
        };
        self.0 & bit != 0
    }
    fn covers(self, other: Self) -> bool {
        other.0 & !self.0 == 0
    }
}

/// Actions on user policy and resources occupy disjoint sets. In particular,
/// operational possession cannot be confused with permission to delegate it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Actions(u64);
impl Actions {
    pub const ENROLL: Self = Self(abi::ACTION_ENROLL);
    pub const PROFILE: Self = Self(abi::ACTION_PROFILE);
    pub const SUSPEND: Self = Self(abi::ACTION_SUSPEND);
    pub const RESUME: Self = Self(abi::ACTION_RESUME);
    pub const RETIRE: Self = Self(abi::ACTION_RETIRE);
    pub const GROUP_CREATE: Self = Self(abi::ACTION_GROUP_CREATE);
    pub const GROUP_MEMBERSHIP: Self = Self(abi::ACTION_GROUP_MEMBERSHIP);
    pub const GRANT: Self = Self(abi::ACTION_GRANT);
    pub const REVOKE: Self = Self(abi::ACTION_REVOKE);
    pub const DELEGATE: Self = Self(abi::ACTION_DELEGATE);
    pub const CLEARANCE_ENROLL: Self = Self(abi::ACTION_CLEARANCE_ENROLL);
    pub const KEY_RESET: Self = Self(abi::ACTION_KEY_RESET);
    pub const ROTATE_DOMAIN: Self = Self(abi::ACTION_ROTATE_DOMAIN);
    pub const FLOOR_DEFINE: Self = Self(abi::ACTION_FLOOR_DEFINE);
    pub const AUDIT_READ: Self = Self(abi::ACTION_AUDIT_READ);
    pub const FS_READ: Self = Self(abi::ACTION_FS_READ);
    pub const FS_WRITE: Self = Self(abi::ACTION_FS_WRITE);
    pub const FS_CHOWN: Self = Self(abi::ACTION_FS_CHOWN);
    pub const NET_CONNECT: Self = Self(abi::ACTION_NET_CONNECT);
    pub const NET_LISTEN: Self = Self(abi::ACTION_NET_LISTEN);
    pub const SIGNAL: Self = Self(abi::ACTION_SIGNAL);
    pub const POST_SERVICE: Self = Self(abi::ACTION_POST_SERVICE);
    const ADMIN_MASK: u64 = (1 << 15) - 1;
    const USE_MASK: u64 = ((1 << 7) - 1) << 32;
    pub fn new(bits: u64) -> Result<Self, Error> {
        if bits & !(Self::ADMIN_MASK | Self::USE_MASK) != 0 {
            Err(Error::UnknownAction)
        } else if bits == 0 {
            Err(Error::EmptyAuthority)
        } else {
            Ok(Self(bits))
        }
    }
    pub fn union(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }
    pub fn covers(self, other: Self) -> bool {
        other.0 & !self.0 == 0
    }
    fn valid_for(self, kind: Kind) -> bool {
        let mask = if kind == Kind::Admin {
            Self::ADMIN_MASK
        } else {
            Self::USE_MASK
        };
        self.0 != 0 && self.0 & !mask == 0
    }
}

/// Exact, stable object IDs issued by the appropriate owner. No client path or
/// claimed parent relationship is used for containment. A subtree/endpoint-view
/// ID denotes an already validated owner-issued view, not a guessed path prefix.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct Resource {
    pub owner: u64,
    pub object: u64,
}

/// Selectors are explicit sets, never implicit wildcards. Future realm/template
/// expansion must be version-bound by the Corvus resolver before reaching here.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Scope {
    pub domain: u64,
    pub actions: Actions,
    pub subjects: Vec<u32>,
    pub resources: Vec<Resource>,
}
fn canonical<T: Ord>(xs: &[T]) -> bool {
    xs.windows(2).all(|w| w[0] < w[1])
}
impl Scope {
    pub fn validate(&self) -> Result<(), Error> {
        if self.domain == 0
            || self.subjects.is_empty()
            || self.resources.is_empty()
            || self.subjects.len() > MAX_SELECTORS
            || self.resources.len() > MAX_SELECTORS
            || !canonical(&self.subjects)
            || !canonical(&self.resources)
            || self.resources.iter().any(|r| r.owner == 0 || r.object == 0)
        {
            return Err(Error::Invalid);
        }
        Actions::new(self.actions.0)?;
        Ok(())
    }
    pub fn covers(&self, other: &Self) -> bool {
        self.domain == other.domain
            && self.actions.covers(other.actions)
            && other
                .subjects
                .iter()
                .all(|s| self.subjects.binary_search(s).is_ok())
            && other
                .resources
                .iter()
                .all(|r| self.resources.binary_search(r).is_ok())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Auth {
    Session,
    DistinctKey,
    Founding,
}
impl Auth {
    fn strength(self) -> u8 {
        match self {
            Self::Session => 0,
            Self::DistinctKey => 1,
            Self::Founding => 2,
        }
    }
    fn meets(self, floor: Self) -> bool {
        self.strength() >= floor.strength()
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Term {
    UntilRevoked,
    UntilUtc(u64),
}
impl Term {
    fn valid(self, now: Option<u64>) -> Result<(), Error> {
        match self {
            Self::UntilRevoked => Ok(()),
            Self::UntilUtc(0) => Err(Error::Invalid),
            Self::UntilUtc(end) => match now {
                None => Err(Error::ClockUnavailable),
                Some(now) if now >= end => Err(Error::Expired),
                Some(_) => Ok(()),
            },
        }
    }
    fn covers(self, child: Self) -> bool {
        match (self, child) {
            (Self::UntilRevoked, _) => true,
            (Self::UntilUtc(a), Self::UntilUtc(b)) => b <= a,
            _ => false,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Envelope {
    pub kinds: Kinds,
    pub scope: Scope,
    pub max_term: Term,
    pub auth_floor: Auth,
    pub delegation_depth: u8,
}
impl Envelope {
    fn validate(&self) -> Result<(), Error> {
        self.scope.validate()?;
        if self.delegation_depth as usize > MAX_DEPTH {
            return Err(Error::Depth);
        }
        if self.scope.actions.0 & !Actions::USE_MASK != 0 {
            return Err(Error::Invalid);
        }
        if matches!(self.max_term, Term::UntilUtc(0)) {
            return Err(Error::Invalid);
        }
        Ok(())
    }
    fn covers(&self, child: &Self) -> bool {
        self.kinds.covers(child.kinds)
            && self.scope.covers(&child.scope)
            && self.max_term.covers(child.max_term)
            && child.auth_floor.meets(self.auth_floor)
            && child.delegation_depth < self.delegation_depth
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct Reference {
    pub id: u64,
    pub revision: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Live,
    Revoking,
    Revoked,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Mandate {
    pub reference: Reference,
    pub subject: u32,
    pub issuer: u32,
    pub kind: Kind,
    pub scope: Scope,
    pub term: Term,
    pub authentication: Auth,
    pub envelope: Option<Envelope>,
    pub supports: Vec<Reference>,
    pub domain_generation: u64,
    pub state: State,
}
impl Mandate {
    fn validate(&self, root: bool) -> Result<(), Error> {
        self.scope.validate()?;
        if self.reference.id == 0
            || self.reference.revision == 0
            || self.domain_generation == 0
            || !self.scope.actions.valid_for(self.kind)
            || self.supports.len() > MAX_SUPPORTS
            || !canonical(&self.supports)
            || (!root && self.supports.is_empty())
            || (root && !self.supports.is_empty())
            || self.supports.iter().any(|r| r.id == 0 || r.revision == 0)
            || matches!(self.term, Term::UntilUtc(0))
        {
            return Err(Error::Invalid);
        }
        match (&self.envelope, self.kind) {
            (Some(e), Kind::Admin) => e.validate()?,
            (Some(_), _) => return Err(Error::Invalid),
            _ => (),
        }
        if self.kind == Kind::Admin && !self.authentication.meets(Auth::DistinctKey) {
            return Err(Error::Denied);
        }
        Ok(())
    }
}

/// Runtime must construct this from a fresh kernel peer/scope snapshot. This is
/// an input value, NOT a bearer credential and never decoded from client bytes.
#[derive(Clone, Copy, Debug)]
pub struct Activation {
    pub actor: u32,
    pub authority: Reference,
    pub policy_revision: u64,
    pub scope_id: u64,
    pub expires_mono: u64,
    pub authentication: Auth,
}
#[derive(Clone, Copy, Debug)]
pub struct Time {
    pub mono: u64,
    pub utc: Option<u64>,
}

#[derive(Clone, Debug)]
struct Entry {
    mandate: Mandate,
    founding: bool,
}
#[derive(Debug)]
pub struct Ledger {
    entries: Vec<Entry>,
    generations: Vec<(u64, u64)>,
    revision: u64,
    high_id: u64,
}
impl Default for Ledger {
    fn default() -> Self {
        Self::new()
    }
}
impl Ledger {
    pub fn new() -> Self {
        Self {
            entries: Vec::new(),
            generations: Vec::new(),
            revision: 1,
            high_id: 0,
        }
    }
    pub fn revision(&self) -> u64 {
        self.revision
    }
    pub fn records(&self) -> impl Iterator<Item = &Mandate> {
        self.entries.iter().map(|e| &e.mandate)
    }
    pub fn get(&self, r: Reference) -> Result<&Mandate, Error> {
        Ok(&self.entry(r)?.mandate)
    }
    fn entry(&self, r: Reference) -> Result<&Entry, Error> {
        let index = self
            .entries
            .binary_search_by_key(&r.id, |e| e.mandate.reference.id)
            .map_err(|_| Error::Missing)?;
        let e = &self.entries[index];
        if e.mandate.reference.revision != r.revision {
            return Err(Error::Stale);
        }
        Ok(e)
    }
    fn generation(&self, domain: u64) -> Result<u64, Error> {
        self.generations
            .iter()
            .find(|g| g.0 == domain)
            .map(|g| g.1)
            .ok_or(Error::Missing)
    }
    fn next_revision(&self) -> Result<u64, Error> {
        self.revision.checked_add(1).ok_or(Error::Overflow)
    }
    fn capacity(&self, m: &Mandate) -> Result<(), Error> {
        if self.entries.len() >= MAX_RECORDS
            || self
                .entries
                .iter()
                .filter(|e| e.mandate.subject == m.subject && e.mandate.state != State::Revoked)
                .count()
                >= MAX_PER_SUBJECT
        {
            return Err(Error::Capacity);
        }
        if m.reference.id <= self.high_id {
            return Err(Error::Duplicate);
        }
        Ok(())
    }
    /// Only installer/replay code may invoke founding insertion. No ordinary
    /// request handler is allowed to expose it. The resulting root is immutable.
    pub fn install_founding(&mut self, m: Mandate) -> Result<(), Error> {
        m.validate(true)?;
        if m.state != State::Live {
            return Err(Error::Invalid);
        }
        self.capacity(&m)?;
        let rev = self.next_revision()?;
        self.entries.try_reserve(1).map_err(|_| Error::Capacity)?;
        self.generations
            .try_reserve(1)
            .map_err(|_| Error::Capacity)?;
        match self.generations.iter().find(|g| g.0 == m.scope.domain) {
            Some(g) if g.1 != m.domain_generation => return Err(Error::Stale),
            None => self.generations.push((m.scope.domain, m.domain_generation)),
            _ => (),
        }
        self.high_id = m.reference.id;
        self.entries.push(Entry {
            mandate: m,
            founding: true,
        });
        self.revision = rev;
        Ok(())
    }
    /// Iterative DAG traversal with a greatest-depth memo. A diamond graph
    /// must cost O(depth * edges), not one recursive visit for every path.
    /// Insertions only reference smaller, existing IDs; replay must use the
    /// same insertion validator, so an edge to self/newer ID is corruption.
    fn support_indices(&self, root: Reference, initial_depth: u8) -> Result<Vec<usize>, Error> {
        if initial_depth == 0 || initial_depth as usize > MAX_DEPTH {
            return Err(Error::Depth);
        }
        // DFS pending siblings fit depth * fanout. Reserve before traversal;
        // fallible allocation never leaves a partially published mutation.
        let mut todo = Vec::new();
        todo.try_reserve_exact(MAX_DEPTH * MAX_SUPPORTS)
            .map_err(|_| Error::Capacity)?;
        todo.push((root, initial_depth));
        let mut depths = Vec::new();
        depths
            .try_reserve_exact(self.entries.len())
            .map_err(|_| Error::Capacity)?;
        depths.resize(self.entries.len(), 0u8);
        let mut result = Vec::new();
        result
            .try_reserve_exact(self.entries.len())
            .map_err(|_| Error::Capacity)?;
        while let Some((r, depth)) = todo.pop() {
            if depth as usize > MAX_DEPTH {
                return Err(Error::Depth);
            }
            let e = self.entry(r)?;
            let index = self
                .entries
                .binary_search_by_key(&r.id, |e| e.mandate.reference.id)
                .map_err(|_| Error::Missing)?;
            if depths[index] >= depth {
                continue;
            }
            if depths[index] == 0 {
                result.push(index);
            }
            depths[index] = depth;
            for &support in &e.mandate.supports {
                if support.id >= r.id {
                    return Err(Error::Cycle);
                }
                todo.push((support, depth + 1));
            }
        }
        Ok(result)
    }
    pub fn is_live(&self, r: Reference, now: Option<u64>) -> Result<(), Error> {
        for index in self.support_indices(r, 1)? {
            let m = &self.entries[index].mandate;
            if m.state != State::Live {
                return Err(Error::Revoked);
            }
            if self.generation(m.scope.domain)? != m.domain_generation {
                return Err(Error::Stale);
            }
            m.term.valid(now)?;
        }
        Ok(())
    }
    pub fn authorize(&self, a: &Activation, scope: &Scope, time: Time) -> Result<(), Error> {
        scope.validate()?;
        if a.scope_id == 0 || time.mono >= a.expires_mono {
            return Err(Error::Inactive);
        }
        if a.policy_revision != self.revision {
            return Err(Error::Conflict);
        }
        self.is_live(a.authority, time.utc)?;
        let m = &self.entry(a.authority)?.mandate;
        if m.kind != Kind::Admin
            || m.subject != a.actor
            || !a.authentication.meets(m.authentication)
            || !m.scope.covers(scope)
        {
            return Err(Error::Denied);
        }
        Ok(())
    }
    fn ancestor_subject(&self, r: Reference, target: u32) -> Result<bool, Error> {
        Ok(self
            .support_indices(r, 1)?
            .iter()
            .any(|&index| self.entries[index].mandate.subject == target))
    }
    /// Validate a grant using ONE complete selected envelope. Other supports
    /// are conjunctive restrictions, never fragments that widen the envelope.
    pub fn check_issue(&self, a: &Activation, child: &Mandate, time: Time) -> Result<(), Error> {
        child.validate(false)?;
        if child.state != State::Live || child.issuer != a.actor {
            return Err(Error::Invalid);
        }
        self.capacity(child)?;
        if child.subject == a.actor {
            return Err(Error::SelfEscalation);
        }
        if !child.supports.contains(&a.authority) {
            return Err(Error::Denied);
        }
        let parent = &self.entry(a.authority)?.mandate;
        let action = if child.kind == Kind::Admin {
            Actions::DELEGATE
        } else {
            Actions::GRANT
        };
        let request = Scope {
            domain: child.scope.domain,
            actions: action,
            subjects: alloc::vec![child.subject],
            resources: child.scope.resources.clone(),
        };
        self.authorize(a, &request, time)?;
        let envelope = parent.envelope.as_ref().ok_or(Error::Denied)?;
        if !envelope.kinds.contains(child.kind)
            || !envelope.max_term.covers(child.term)
            || !child.authentication.meets(envelope.auth_floor)
        {
            return Err(Error::Denied);
        }
        match child.kind {
            Kind::Admin => {
                if !parent.scope.covers(&child.scope) {
                    return Err(Error::Denied);
                }
                if envelope.delegation_depth == 0 {
                    return Err(Error::Denied);
                }
                if let Some(next) = &child.envelope {
                    if !envelope.covers(next) {
                        return Err(Error::Denied);
                    }
                }
            }
            _ if !envelope.scope.covers(&child.scope) => return Err(Error::Denied),
            _ => (),
        }
        if self.generation(child.scope.domain)? != child.domain_generation {
            return Err(Error::Stale);
        }
        child.term.valid(time.utc)?;
        for &s in &child.supports {
            self.is_live(s, time.utc)?;
            if !self.entry(s)?.mandate.term.covers(child.term) {
                return Err(Error::Denied);
            }
            if self.ancestor_subject(s, child.subject)? {
                return Err(Error::SelfEscalation);
            }
            // Appending a child consumes one additional support-graph level.
            self.support_indices(s, 2)?;
        }
        Ok(())
    }
    /// Pure in-memory commit. The durable transaction layer must persist the
    /// approved canonical result + audit atomically BEFORE exposing this state.
    pub fn issue(&mut self, a: &Activation, child: Mandate, time: Time) -> Result<(), Error> {
        self.check_issue(a, &child, time)?;
        let rev = self.next_revision()?;
        self.entries.try_reserve(1).map_err(|_| Error::Capacity)?;
        self.high_id = child.reference.id;
        self.entries.push(Entry {
            mandate: child,
            founding: false,
        });
        self.revision = rev;
        Ok(())
    }
    pub fn revocation_closure(&self, roots: &[Reference]) -> Result<Vec<Reference>, Error> {
        if roots.is_empty() || roots.len() > MAX_SUPPORTS || !canonical(roots) {
            return Err(Error::Invalid);
        }
        let mut selected = Vec::new();
        selected
            .try_reserve_exact(self.entries.len())
            .map_err(|_| Error::Capacity)?;
        selected.resize(self.entries.len(), false);
        for &r in roots {
            if self.entry(r)?.mandate.state == State::Revoked {
                return Err(Error::Revoked);
            }
            let i = self
                .entries
                .binary_search_by_key(&r.id, |e| e.mandate.reference.id)
                .map_err(|_| Error::Missing)?;
            selected[i] = true;
        }
        let mut closure = Vec::new();
        closure
            .try_reserve_exact(self.entries.len())
            .map_err(|_| Error::Capacity)?;
        // Insertion order is a topological order: every support ID is lower.
        // A single forward pass computes transitive descendants without a
        // quadratic growing-vector membership scan at the 4096-record limit.
        for (i, e) in self.entries.iter().enumerate() {
            let m = &e.mandate;
            if m.state == State::Revoked {
                continue;
            }
            for &support in &m.supports {
                if support.id >= m.reference.id {
                    return Err(Error::Cycle);
                }
                self.entry(support)?;
                let parent = self
                    .entries
                    .binary_search_by_key(&support.id, |e| e.mandate.reference.id)
                    .map_err(|_| Error::Missing)?;
                selected[i] |= selected[parent];
            }
            if selected[i] {
                closure.push(m.reference);
            }
        }
        Ok(closure)
    }
    /// Return a public transaction plan; a caller cannot turn this into authority
    /// by submitting it back. Recheck live activation and expected revision at commit.
    pub fn plan_revoke(
        &self,
        a: &Activation,
        roots: &[Reference],
        time: Time,
    ) -> Result<Vec<Reference>, Error> {
        let closure = self.revocation_closure(roots)?;
        for &r in &closure {
            let e = self.entry(r)?;
            if e.founding {
                return Err(Error::Denied);
            }
            let m = &e.mandate;
            self.authorize(
                a,
                &Scope {
                    domain: m.scope.domain,
                    actions: Actions::REVOKE,
                    subjects: alloc::vec![m.subject],
                    resources: m.scope.resources.clone(),
                },
                time,
            )?;
        }
        Ok(closure)
    }
    /// Close pure policy admission before the runtime drains affected sessions.
    /// Returns the exact closure to bind into the durable barrier transaction.
    pub fn begin_revoke(
        &mut self,
        a: &Activation,
        roots: &[Reference],
        time: Time,
    ) -> Result<Vec<Reference>, Error> {
        let closure = self.plan_revoke(a, roots, time)?;
        let next = self.next_revision()?;
        for &r in &closure {
            let i = self
                .entries
                .binary_search_by_key(&r.id, |e| e.mandate.reference.id)
                .map_err(|_| Error::Missing)?;
            if self.entries[i].mandate.state == State::Live {
                self.entries[i].mandate.state = State::Revoking;
            }
        }
        self.revision = next;
        Ok(closure)
    }

    /// Pure transition only. The runtime must establish its kernel/service
    /// barrier before calling this; a client-supplied list is NOT an ACK.
    /// All entries are checked before any state is changed.
    pub fn finish_revoke(
        &mut self,
        expected_revision: u64,
        closure: &[Reference],
    ) -> Result<(), Error> {
        if expected_revision != self.revision {
            return Err(Error::Conflict);
        }
        if closure.is_empty() || !canonical(closure) {
            return Err(Error::Invalid);
        }
        for &r in closure {
            if self.entry(r)?.mandate.state != State::Revoking {
                return Err(Error::Conflict);
            }
        }
        if self.entries.iter().any(|e| {
            e.mandate.state != State::Revoked
                && e.mandate
                    .supports
                    .iter()
                    .any(|s| closure.binary_search(s).is_ok())
                && closure.binary_search(&e.mandate.reference).is_err()
        }) {
            return Err(Error::Conflict);
        }
        let next = self.next_revision()?;
        for &r in closure {
            let i = self
                .entries
                .binary_search_by_key(&r.id, |e| e.mandate.reference.id)
                .map_err(|_| Error::Missing)?;
            self.entries[i].mandate.state = State::Revoked;
        }
        self.revision = next;
        Ok(())
    }
}

#[cfg(test)]
mod tests;
