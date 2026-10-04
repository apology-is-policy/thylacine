//! Cooperative pixel residency, TAPESTRY-STORAGE (scripture cfa478824).
//!
//! The compositor alone supplies visibility and the backend-unbound guard.
//! Offers are freshness, not authority. A stale event never changes residency.
//! Fids capture pixel generations; suspension invalidates all pixel admission,
//! and resume/resize never reuses a generation. The server performs resource
//! operations around these transitions on its single normal-dispatch owner.
//! This module cannot certify a device fence: release still uses Lictor's pins.
//! Model: tapestry_storage::{Visibility, Suspend, Resume, Abort, Present}.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Suspend = 1,
    Resume = 2,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Offer {
    pub kind: Kind,
    pub token: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase {
    Resident,
    Dormant,
    Repainting,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refusal {
    Stale,
    Exhausted,
}

#[derive(Clone, Copy, Debug)]
pub struct Storage {
    enabled: bool,
    visible: bool,
    phase: Phase,
    generation: u64,
    serial: u64,
    offer: Option<Offer>,
    resume_token: u64,
    failed_visible: bool,
}
impl Default for Storage {
    fn default() -> Self {
        Self::new()
    }
}
impl Storage {
    pub const fn new() -> Self {
        Self {
            enabled: false,
            visible: true,
            phase: Phase::Resident,
            generation: 1,
            serial: 0,
            offer: None,
            resume_token: 0,
            failed_visible: false,
        }
    }
    pub fn enabled(&self) -> bool {
        self.enabled
    }
    pub fn generation(&self) -> u64 {
        self.generation
    }
    pub fn phase(&self) -> Phase {
        self.phase
    }
    pub fn resident(&self) -> bool {
        self.phase != Phase::Dormant
    }
    /// Role/creation/GL eligibility is checked by the ctl owner before this.
    pub fn enable(&mut self) {
        self.enabled = true;
    }

    /// Return only a newly published offer. Repeated reconciliation is quiet.
    /// A reversal invalidates the old offer even if it is already in a client
    /// event queue. A failed resume is retried only in a later visible episode.
    pub fn reconcile(&mut self, visible: bool, unbound: bool) -> Option<Offer> {
        if !self.enabled {
            return None;
        }
        if self.visible != visible {
            self.visible = visible;
            self.offer = None;
            self.failed_visible = false;
        }
        if self.offer.is_some() || self.failed_visible || (!visible && !unbound) {
            return None;
        }
        let kind = match (visible, self.phase) {
            (false, Phase::Resident | Phase::Repainting) => Kind::Suspend,
            (true, Phase::Dormant) => Kind::Resume,
            _ => return None,
        };
        // Never suspend a last generation that cannot later be recreated.
        if self.generation == u64::MAX {
            return None;
        }
        self.serial = self.serial.checked_add(1)?;
        let offer = Offer {
            kind,
            token: self.serial,
        };
        self.offer = Some(offer);
        Some(offer)
    }
    fn matches(&self, kind: Kind, token: u64) -> bool {
        self.enabled && token != 0 && self.offer == Some(Offer { kind, token })
    }
    /// Validate before detaching any generation; a refused request changes no
    /// state. The caller must supply a real no-scanout/no-external-use guard.
    pub fn suspend(&mut self, token: u64, unbound: bool) -> Result<(), Refusal> {
        if !self.matches(Kind::Suspend, token) || self.visible || !self.resident() || !unbound {
            return Err(Refusal::Stale);
        }
        if self.generation == u64::MAX {
            return Err(Refusal::Exhausted);
        }
        self.phase = Phase::Dormant;
        self.offer = None;
        self.resume_token = 0;
        Ok(())
    }
    /// Reserve a fresh generation before allocation. If allocation or client
    /// mapping fails, abort(token) preserves identity and suppresses idle retry.
    pub fn resume(&mut self, token: u64) -> Result<u64, Refusal> {
        if !self.matches(Kind::Resume, token) || !self.visible || self.phase != Phase::Dormant {
            return Err(Refusal::Stale);
        }
        let generation = self.generation.checked_add(1).ok_or(Refusal::Exhausted)?;
        self.generation = generation;
        self.phase = Phase::Repainting;
        self.offer = None;
        self.resume_token = token;
        Ok(generation)
    }
    pub fn abort(&mut self, token: u64) -> Result<(), Refusal> {
        if token == 0 || self.phase != Phase::Repainting || self.resume_token != token {
            return Err(Refusal::Stale);
        }
        self.phase = Phase::Dormant;
        self.offer = None;
        self.resume_token = 0;
        self.failed_visible = self.visible;
        Ok(())
    }
    /// Geometry/map/present gates additionally require a live owned Surface
    /// and its actual weave. Legacy clients retain their original FIFO fence.
    pub fn admits(&self, fid_generation: u64) -> bool {
        !self.enabled || (self.resident() && fid_generation == self.generation)
    }
    pub fn accepts_present(&self, fid_generation: u64, full: bool, held: bool) -> bool {
        self.admits(fid_generation)
            && (!self.enabled || self.phase != Phase::Repainting || (full && !held))
    }
    /// Call only after successful unheld presentation, never submit intent.
    pub fn presented(&mut self) {
        if self.phase == Phase::Repainting {
            self.phase = Phase::Resident;
            self.resume_token = 0;
        }
    }
    /// Reserve before ordinary resize allocates. A failed allocation leaves
    /// this copy uncommitted; publish it with the new weave after success.
    /// Do not replace a resume generation until its full repaint completes.
    pub fn resized(&self) -> Result<Self, Refusal> {
        if self.enabled && self.phase != Phase::Resident {
            return Err(Refusal::Stale);
        }
        let mut next = *self;
        next.generation = next.generation.checked_add(1).ok_or(Refusal::Exhausted)?;
        if next.enabled {
            next.phase = Phase::Repainting;
        }
        Ok(next)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn active() -> Storage {
        let mut s = Storage::new();
        s.enable();
        s
    }
    fn dormant() -> Storage {
        let mut s = active();
        let o = s.reconcile(false, true).unwrap();
        s.suspend(o.token, true).unwrap();
        s
    }
    #[test]
    fn visibility_reversal_rejects_an_already_delivered_offer() {
        let mut s = active();
        let old = s.reconcile(false, true).unwrap();
        assert_eq!(s.reconcile(true, true), None);
        assert_eq!(s.suspend(old.token, true), Err(Refusal::Stale));
        let new = s.reconcile(false, true).unwrap();
        assert_ne!(old.token, new.token);
        assert_eq!(s.suspend(old.token, true), Err(Refusal::Stale));
        s.suspend(new.token, true).unwrap();
        assert!(!s.admits(1));
    }
    #[test]
    fn bound_display_refusal_leaves_pixels_and_offer_intact() {
        let mut s = active();
        let o = s.reconcile(false, true).unwrap();
        assert_eq!(s.suspend(o.token, false), Err(Refusal::Stale));
        assert!(s.admits(1));
        assert_eq!(s.reconcile(false, true), None);
        s.suspend(o.token, true).unwrap();
    }
    #[test]
    fn fresh_generation_requires_fresh_fids_and_full_unheld_first_frame() {
        let mut s = dormant();
        let o = s.reconcile(true, true).unwrap();
        let g = s.resume(o.token).unwrap();
        assert_eq!(g, 2);
        assert!(!s.admits(1));
        assert!(s.admits(2));
        assert!(!s.accepts_present(2, false, false));
        assert!(!s.accepts_present(2, true, true));
        assert!(s.accepts_present(2, true, false));
        s.presented();
        assert!(s.accepts_present(2, false, false));
        assert_eq!(s.abort(o.token), Err(Refusal::Stale));
    }
    #[test]
    fn failed_mapping_preserves_dormancy_without_frame_tick_retry() {
        let mut s = dormant();
        let o = s.reconcile(true, true).unwrap();
        s.resume(o.token).unwrap();
        s.abort(o.token).unwrap();
        assert_eq!(s.phase(), Phase::Dormant);
        for _ in 0..100 {
            assert_eq!(s.reconcile(true, true), None);
        }
        assert_eq!(s.reconcile(false, true), None);
        let fresh = s.reconcile(true, true).unwrap();
        assert_ne!(fresh.token, o.token);
        assert_eq!(s.resume(o.token), Err(Refusal::Stale));
        assert_eq!(s.resume(fresh.token), Ok(3));
    }
    #[test]
    fn hide_during_repaint_can_suspend_or_abort_only_its_generation() {
        let mut s = dormant();
        let resume = s.reconcile(true, true).unwrap();
        s.resume(resume.token).unwrap();
        let hide = s.reconcile(false, true).unwrap();
        assert_eq!(s.abort(hide.token), Err(Refusal::Stale));
        s.suspend(hide.token, true).unwrap();
        assert_eq!(s.abort(resume.token), Err(Refusal::Stale));
        let next = s.reconcile(true, true).unwrap();
        assert_eq!(s.resume(next.token), Ok(3));
    }
    #[test]
    fn resize_is_transactional_and_invalidates_only_opted_in_fids() {
        let s = active();
        let next = s.resized().unwrap();
        assert!(s.admits(1));
        assert!(!next.admits(1));
        assert!(next.admits(2));
        let legacy = Storage::new().resized().unwrap();
        assert!(legacy.admits(1));
        assert_eq!(dormant().resized().unwrap_err(), Refusal::Stale);
    }
    #[test]
    fn counters_do_not_wrap_or_suspend_an_unresumable_last_generation() {
        let mut s = active();
        s.serial = u64::MAX;
        assert_eq!(s.reconcile(false, true), None);
        assert!(s.admits(1));
        assert_eq!(s.suspend(0, true), Err(Refusal::Stale));
        s.serial = 0;
        s.generation = u64::MAX;
        assert_eq!(s.reconcile(false, true), None);
        assert_eq!(s.resized().unwrap_err(), Refusal::Exhausted);
    }
    #[test]
    fn repeated_reconcile_and_enable_do_not_flood_or_change_offer() {
        let mut s = active();
        let o = s.reconcile(false, true).unwrap();
        for _ in 0..100 {
            s.enable();
            assert_eq!(s.reconcile(false, true), None);
        }
        s.suspend(o.token, true).unwrap();
    }
    #[test]
    fn no_offer_before_backend_unbind_but_visibility_still_invalidates() {
        let mut s = active();
        assert_eq!(s.reconcile(false, false), None);
        let o = s.reconcile(false, true).unwrap();
        assert_eq!(s.reconcile(true, false), None);
        assert_eq!(s.suspend(o.token, true), Err(Refusal::Stale));
        assert_eq!(s.reconcile(false, false), None);
        let next = s.reconcile(false, true).unwrap();
        assert_ne!(next.token, o.token);
    }
    #[test]
    fn legacy_has_no_offers_and_keeps_old_present_contract() {
        let mut s = Storage::new();
        assert_eq!(s.reconcile(false, true), None);
        assert!(s.accepts_present(0, false, true));
        assert_eq!(s.suspend(1, true), Err(Refusal::Stale));
    }
}
