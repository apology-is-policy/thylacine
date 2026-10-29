//! Bounded AUTH records. Corvus's single-threaded server loop owns this table;
//! no reference is retained across dispatch. A borrowed token selects a record
//! but never owns its retirement. Capacity follows the existing transport bound.
use crate::{KEYPAIR_LEN, MAX_CONNS, MAX_USER_LEN, TOKEN_LEN};

pub struct Session {
    owner: u64,
    connection: u64,
    principal: u32,
    user_len: usize,
    user: [u8; MAX_USER_LEN],
    token: [u8; TOKEN_LEN],
    keypair: [u8; KEYPAIR_LEN],
}
impl Session {
    const fn empty() -> Self {
        Self { owner: 0, connection: 0, principal: 0, user_len: 0,
            user: [0; MAX_USER_LEN], token: [0; TOKEN_LEN], keypair: [0; KEYPAIR_LEN] }
    }
    pub fn user(&self) -> &[u8] { &self.user[..self.user_len] }
    pub fn keypair(&self) -> &[u8; KEYPAIR_LEN] { &self.keypair }
    pub fn connection(&self) -> u64 { self.connection }
    fn clear(&mut self) {
        self.connection = 0; // invalidate before scrubbing, never reusable early
        self.owner = 0;
        self.principal = 0;
        self.user_len = 0;
        for b in self.user.iter_mut().chain(self.token.iter_mut()).chain(self.keypair.iter_mut()) {
            // Secrets must be overwritten even when the whole table is dying.
            unsafe { core::ptr::write_volatile(b, 0); }
        }
    }
}
pub struct Sessions { slots: [Session; MAX_CONNS] }
impl Sessions {
    pub const fn new() -> Self { Self { slots: [const { Session::empty() }; MAX_CONNS] } }
    pub fn can_auth(&self, owner: u64) -> bool {
        owner != 0 && !self.slots.iter().any(|s| s.connection != 0 && s.owner == owner)
            && self.slots.iter().any(|s| s.connection == 0)
    }
    pub fn has_principal(&self, principal: u32) -> bool {
        self.slots.iter().any(|s| s.connection != 0 && s.principal == principal)
    }
    pub fn find(&self, token: &[u8]) -> Option<&Session> {
        if token.len() != TOKEN_LEN { return None; }
        let mut found = None;
        // Compare every token byte in every slot; no early matching-byte exit.
        for s in &self.slots {
            let mut diff = 0u8;
            for (a, b) in s.token.iter().zip(token) { diff |= a ^ b; }
            if diff == 0 && s.connection != 0 { found = Some(s); }
        }
        found
    }
    pub fn install(&mut self, owner: u64, connection: u64, principal: u32,
                   user: &[u8], token: &[u8; TOKEN_LEN], keypair: &[u8; KEYPAIR_LEN]) -> bool {
        if !self.can_auth(owner) || connection == 0 || user.is_empty() || user.len() > MAX_USER_LEN
            || self.slots.iter().any(|s| s.connection == connection) || self.find(token).is_some() {
            return false;
        }
        let s = self.slots.iter_mut().find(|s| s.connection == 0).unwrap();
        s.owner = owner;
        s.principal = principal;
        s.user_len = user.len();
        s.user[..user.len()].copy_from_slice(user);
        s.token.copy_from_slice(token);
        s.keypair.copy_from_slice(keypair);
        s.connection = connection; // publish only the fully initialized record
        true
    }
    pub fn clear_connection(&mut self, connection: u64) {
        for s in &mut self.slots {
            if connection != 0 && s.connection == connection { s.clear(); }
        }
    }
    pub fn clear_all(&mut self) { for s in &mut self.slots { s.clear(); } }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn independent_users_and_owner_only_retirement() {
        let mut t = Sessions::new();
        let a = [1; TOKEN_LEN]; let b = [2; TOKEN_LEN];
        assert!(t.install(11, 101, 1000, b"alice", &a, &[3; KEYPAIR_LEN]));
        assert!(t.install(12, 102, 1001, b"bob", &b, &[4; KEYPAIR_LEN]));
        assert_eq!(t.find(&a).unwrap().user(), b"alice");
        assert_eq!(t.find(&b).unwrap().keypair(), &[4; KEYPAIR_LEN]);
        assert_eq!(t.find(&a).unwrap().connection(), 101);
        t.clear_connection(999); // a borrowing coordinator disconnects
        assert!(t.find(&a).is_some() && t.find(&b).is_some());
        t.clear_connection(101);
        assert!(t.find(&a).is_none());
        assert_eq!(t.find(&b).unwrap().user(), b"bob");
        assert!(!t.has_principal(1000) && t.has_principal(1001));
        assert!(t.slots[0].keypair.iter().all(|b| *b == 0));
        assert!(t.slots[0].token.iter().all(|b| *b == 0));
        t.clear_all(); assert!(!t.has_principal(1001));
    }
    #[test]
    fn same_principal_distinct_owners_and_no_owner_rebinding() {
        let mut t = Sessions::new();
        let a = [1; TOKEN_LEN]; let b = [2; TOKEN_LEN];
        assert!(t.install(11, 101, 1000, b"alice", &a, &[3; KEYPAIR_LEN]));
        assert!(!t.install(11, 102, 1001, b"bob", &b, &[4; KEYPAIR_LEN]));
        assert!(t.install(12, 102, 1000, b"alice", &b, &[3; KEYPAIR_LEN]));
        t.clear_connection(101);
        assert!(t.has_principal(1000));
        assert!(t.find(&b).is_some());
        t.clear_connection(102);
        assert!(!t.has_principal(1000));
    }
    #[test]
    fn capacity_token_collision_and_reuse() {
        let mut t = Sessions::new();
        for i in 0..MAX_CONNS {
            assert!(t.install(i as u64 + 1, i as u64 + 1, i as u32,
                b"user", &[i as u8; TOKEN_LEN], &[7; KEYPAIR_LEN]));
        }
        assert!(!t.can_auth(99));
        assert!(t.find(&[255; TOKEN_LEN]).is_none());
        assert!(t.find(&[0; TOKEN_LEN-1]).is_none());
        t.clear_connection(1);
        assert!(!t.install(99, 99, 99, b"other", &[1; TOKEN_LEN], &[8; KEYPAIR_LEN]));
        assert!(t.install(99, 99, 99, b"other", &[250; TOKEN_LEN], &[8; KEYPAIR_LEN]));
        assert_eq!(t.find(&[250; TOKEN_LEN]).unwrap().user(), b"other");
    }
}
