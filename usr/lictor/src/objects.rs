//! Admission ledger for the normal broker. Trusted resources are never entered
//! here, exported or attached to a normal rendering context. Resource IDs in GPU
//! command streams are additionally scoped by the backend's context attachments;
//! this ledger is not a claim to validate an arbitrary GPU command language.
use alloc::vec::Vec;

pub const TRUSTED_ID_START: u32 = 0x8000_0000;
/// The service's own resources. No client names one (`valid_id`), and each has
/// one owner: the trusted screen's image is in one of the first two, and the
/// normal cursor's plane is the third.
pub const TRUSTED_IMAGES: [u32; 2] = [TRUSTED_ID_START + 1, TRUSTED_ID_START + 3];
pub const TRUSTED_CURSOR: u32 = TRUSTED_ID_START + 2;
const _: () = assert!(TRUSTED_IMAGES[0] != TRUSTED_IMAGES[1]
    && TRUSTED_IMAGES[0] != TRUSTED_CURSOR && TRUSTED_IMAGES[1] != TRUSTED_CURSOR);
pub const MAX_OBJECTS: usize = 4096;
pub const MAX_ATTACHMENTS: usize = 16384;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind { Resource, Context }
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error { Invalid, Missing, Busy, Full, WrongOwner }

#[derive(Clone, Copy)]
struct Object { owner: u64, id: u32, kind: Kind, retiring: bool, serial: u64, born_in: u32 }
#[derive(Clone, Copy)]
struct Attachment { owner: u64, ctx: u32, res: u32 }

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Retirement { owner: u64, id: u32, kind: Kind, serial: u64 }

#[derive(Default)]
pub struct Objects {
    next_serial: u64,
    entries: Vec<Object>,
    attachments: Vec<Attachment>,
}
impl Objects {
    fn valid_id(id: u32) -> bool { id != 0 && id < TRUSTED_ID_START }
    pub fn reserve(&mut self, owner: u64, id: u32, kind: Kind) -> Result<(), Error> {
        if owner == 0 || !Self::valid_id(id) { return Err(Error::Invalid); }
        if self.entries.iter().any(|o| o.id == id && o.kind == kind) { return Err(Error::Busy); }
        if self.entries.len() == MAX_OBJECTS { return Err(Error::Full); }
        let serial = self.next_serial.checked_add(1).ok_or(Error::Full)?;
        self.next_serial = serial;
        self.entries.push(Object { owner, id, kind, retiring: false, serial, born_in: 0 });
        Ok(())
    }
    /// A host blob is minted under a context, with or without a later attach.
    pub fn reserve_in(&mut self, owner: u64, id: u32, ctx: u32) -> Result<(), Error> {
        self.reserve(owner, id, Kind::Resource)?;
        self.entries.last_mut().unwrap().born_in = ctx;
        Ok(())
    }
    /// Whether a stream on `ctx` can reach `res`. Asked of retiring objects too:
    /// the link outlives the retirement until the device has dropped it. Only a
    /// resource is ever born in a context, so `born_in` needs no kind beside it.
    pub fn holds(&self, ctx: u32, res: u32) -> bool {
        ctx != 0 && (self.attachments.iter().any(|a| a.ctx == ctx && a.res == res)
            || self.entries.iter().any(|o| o.id == res && o.born_in == ctx))
    }
    pub fn check(&self, owner: u64, id: u32, kind: Kind) -> Result<(), Error> {
        if !Self::valid_id(id) { return Err(Error::Invalid); }
        let o = self.entries.iter().find(|o| o.id == id && o.kind == kind).ok_or(Error::Missing)?;
        if o.owner != owner { return Err(Error::WrongOwner); }
        if o.retiring { return Err(Error::Busy); }
        Ok(())
    }
    pub fn attach(&mut self, owner: u64, ctx: u32, res: u32) -> Result<(), Error> {
        self.check(owner, ctx, Kind::Context)?;
        self.check(owner, res, Kind::Resource)?;
        if self.attachments.iter().any(|a| a.ctx == ctx && a.res == res) { return Err(Error::Busy); }
        if self.attachments.len() == MAX_ATTACHMENTS { return Err(Error::Full); }
        self.attachments.push(Attachment { owner, ctx, res });
        Ok(())
    }
    pub fn attached(&self, owner: u64, ctx: u32, res: u32) -> bool {
        self.check(owner, ctx, Kind::Context).is_ok()
            && self.check(owner, res, Kind::Resource).is_ok()
            && self.attachments.iter().any(|a| a.owner == owner && a.ctx == ctx && a.res == res)
    }
    /// Call only after backend detach completion. A failed detach retains the
    /// relationship, preventing premature reuse or backing release.
    pub fn detached(&mut self, owner: u64, ctx: u32, res: u32) -> Result<(), Error> {
        let i = self.attachments.iter().position(|a| a.owner == owner && a.ctx == ctx && a.res == res)
            .ok_or(Error::Missing)?;
        self.attachments.swap_remove(i);
        Ok(())
    }
    /// A completed backend context destroy removes all of that context's
    /// attachment references. This must never run on a mere submission or
    /// timeout; callers retain both context and backing pins in those cases.
    pub fn context_destroyed(&mut self, owner: u64, ctx: u32) -> Result<(), Error> {
        let object = self.entries.iter().find(|o| o.id == ctx && o.kind == Kind::Context)
            .ok_or(Error::Missing)?;
        if object.owner != owner { return Err(Error::WrongOwner); }
        self.attachments.retain(|a| !(a.owner == owner && a.ctx == ctx));
        Ok(())
    }

    /// Stop admission before starting device retirement. Pins and the ID remain
    /// reserved on failure; dropping a client connection cannot release DMA early.
    pub fn retire(&mut self, owner: u64, id: u32, kind: Kind) -> Result<Retirement, Error> {
        self.check(owner, id, kind)?;
        let object = self.entries.iter_mut().find(|o| o.id == id && o.kind == kind).unwrap();
        object.retiring = true;
        Ok(Retirement { owner, id, kind, serial: object.serial })
    }
    /// Invoked only after the backend proves retirement (including scanout and
    /// fence references) and has detached all context links. Repeated/stale retire
    /// completions are errors, never permissions to free another object's backing.
    pub fn retired(&mut self, ticket: Retirement) -> Result<(), Error> {
        let Retirement { owner, id, kind, serial } = ticket;
        let i = self.entries.iter().position(|o| o.id == id && o.kind == kind).ok_or(Error::Missing)?;
        if self.entries[i].owner != owner { return Err(Error::WrongOwner); }
        if self.entries[i].serial != serial { return Err(Error::Missing); }
        if !self.entries[i].retiring { return Err(Error::Busy); }
        if self.attachments.iter().any(|a| match kind { Kind::Resource => a.res == id, Kind::Context => a.ctx == id }) {
            return Err(Error::Busy);
        }
        self.entries.swap_remove(i);
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn trusted_ids_and_other_owners_never_enter_normal_contexts() {
        let mut o = Objects::default();
        o.reserve(10, 1, Kind::Context).unwrap();
        o.reserve(10, 2, Kind::Resource).unwrap();
        o.reserve(11, 3, Kind::Resource).unwrap();
        for id in [0, TRUSTED_ID_START, u32::MAX] {
            assert_eq!(o.reserve(10, id, Kind::Resource), Err(Error::Invalid));
            assert_eq!(o.attach(10, 1, id), Err(Error::Invalid));
        }
        assert_eq!(o.attach(10, 1, 3), Err(Error::WrongOwner));
        assert_eq!(o.attach(11, 1, 3), Err(Error::WrongOwner));
        o.attach(10, 1, 2).unwrap();
        assert!(o.attached(10, 1, 2));
        assert!(!o.attached(11, 1, 2));
    }
    #[test]
    fn failed_retirement_keeps_id_and_attachment_reserved() {
        let mut o = Objects::default();
        o.reserve(1, 1, Kind::Context).unwrap();
        o.reserve(1, 2, Kind::Resource).unwrap();
        o.attach(1, 1, 2).unwrap();
        let ticket = o.retire(1, 2, Kind::Resource).unwrap();
        assert_eq!(o.check(1, 2, Kind::Resource), Err(Error::Busy));
        assert_eq!(o.reserve(2, 2, Kind::Resource), Err(Error::Busy));
        assert_eq!(o.retired(ticket), Err(Error::Busy));
        assert_eq!(o.detached(2, 1, 2), Err(Error::Missing));
        o.detached(1, 1, 2).unwrap();
        o.retired(ticket).unwrap();
        assert_eq!(o.retired(ticket), Err(Error::Missing));
        o.reserve(2, 2, Kind::Resource).unwrap();
        assert_eq!(o.retired(ticket), Err(Error::WrongOwner));
        let next = o.retire(2, 2, Kind::Resource).unwrap();
        o.retired(next).unwrap();
        o.reserve(1, 2, Kind::Resource).unwrap();
        let current = o.retire(1, 2, Kind::Resource).unwrap();
        assert_eq!(o.retired(ticket), Err(Error::Missing));
        o.retired(current).unwrap();
    }
    #[test]
    fn a_context_holds_what_is_attached_to_it_until_the_device_detaches_it() {
        let mut o = Objects::default();
        o.reserve(1, 3, Kind::Context).unwrap();
        o.reserve(1, 4, Kind::Context).unwrap();
        o.reserve(1, 7, Kind::Resource).unwrap();
        assert!(!o.holds(3, 7), "not attached yet");
        o.attach(1, 3, 7).unwrap();
        assert!(o.holds(3, 7));
        assert!(!o.holds(4, 7), "another context");
        let ticket = o.retire(1, 7, Kind::Resource).unwrap();
        assert!(o.holds(3, 7), "retiring a resource does not end the link");
        assert_eq!(o.retired(ticket), Err(Error::Busy));
        o.detached(1, 3, 7).unwrap();
        assert!(!o.holds(3, 7));
    }
    #[test]
    fn a_context_holds_its_attachments_until_its_destroy_completes() {
        let mut o = Objects::default();
        o.reserve(1, 3, Kind::Context).unwrap();
        o.reserve(1, 7, Kind::Resource).unwrap();
        o.attach(1, 3, 7).unwrap();
        o.retire(1, 3, Kind::Context).unwrap();
        assert!(o.holds(3, 7), "a destroy not yet answered proves nothing");
        o.context_destroyed(1, 3).unwrap();
        assert!(!o.holds(3, 7));
    }
    #[test]
    fn a_blob_is_held_by_the_context_it_was_minted_under_with_no_attach() {
        let mut o = Objects::default();
        o.reserve(1, 3, Kind::Context).unwrap();
        o.reserve_in(1, 7, 3).unwrap();
        assert!(o.holds(3, 7));
        assert!(!o.holds(4, 7));
        o.retire(1, 7, Kind::Resource).unwrap();
        assert!(o.holds(3, 7), "still, while it retires");
        assert_eq!(o.reserve_in(1, 7, 3), Err(Error::Busy));
    }
    #[test]
    fn no_chain_with_no_context_holds_anything() {
        let mut o = Objects::default();
        o.reserve(1, 7, Kind::Resource).unwrap();
        assert!(!o.holds(0, 7), "a plain resource's unset birth context is not context 0");
    }
    #[test]
    fn a_context_id_and_a_resource_id_with_the_same_number_are_not_confused() {
        let mut o = Objects::default();
        o.reserve(1, 7, Kind::Context).unwrap();
        o.reserve(1, 3, Kind::Context).unwrap();
        assert!(!o.holds(3, 7), "7 here is a context");
        o.reserve_in(1, 7, 3).unwrap();
        assert!(o.holds(3, 7));
    }
    #[test]
    fn object_budget_is_global_and_zero_owner_is_invalid() {
        let mut o = Objects::default();
        assert_eq!(o.reserve(0, 1, Kind::Context), Err(Error::Invalid));
        for id in 1..=MAX_OBJECTS as u32 { o.reserve(1, id, Kind::Resource).unwrap(); }
        assert_eq!(o.reserve(2, 1, Kind::Context), Err(Error::Full));
    }
}
