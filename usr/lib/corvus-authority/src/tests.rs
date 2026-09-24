use super::*;
use alloc::vec;

const T: Time = Time {
    mono: 10,
    utc: Some(100),
};
fn reference(id: u64) -> Reference {
    Reference { id, revision: 1 }
}
fn actions() -> Actions {
    Actions::GRANT
        .union(Actions::DELEGATE)
        .union(Actions::REVOKE)
        .union(Actions::ENROLL)
}
fn scope(action: Actions) -> Scope {
    Scope {
        domain: 1,
        actions: action,
        subjects: vec![10, 20, 30, 40],
        resources: vec![Resource {
            owner: 1,
            object: 1,
        }],
    }
}
fn envelope() -> Envelope {
    Envelope {
        kinds: Kinds::ALL,
        scope: scope(Actions::NET_CONNECT),
        max_term: Term::UntilRevoked,
        auth_floor: Auth::DistinctKey,
        delegation_depth: 4,
    }
}
fn root() -> Mandate {
    Mandate {
        reference: reference(1),
        subject: 10,
        issuer: 0,
        kind: Kind::Admin,
        scope: scope(actions()),
        term: Term::UntilRevoked,
        authentication: Auth::DistinctKey,
        envelope: Some(envelope()),
        supports: vec![],
        domain_generation: 1,
        state: State::Live,
    }
}
fn ledger() -> Ledger {
    let mut l = Ledger::new();
    l.install_founding(root()).unwrap();
    l
}
fn activation(l: &Ledger, id: u64, actor: u32) -> Activation {
    Activation {
        actor,
        authority: reference(id),
        policy_revision: l.revision(),
        scope_id: 1,
        expires_mono: 20,
        authentication: Auth::DistinctKey,
    }
}
fn use_grant(id: u64, subject: u32, issuer: u32, support: u64) -> Mandate {
    let mut s = scope(Actions::NET_CONNECT);
    s.subjects = vec![subject];
    Mandate {
        reference: reference(id),
        subject,
        issuer,
        kind: Kind::Use,
        scope: s,
        term: Term::UntilRevoked,
        authentication: Auth::DistinctKey,
        envelope: None,
        supports: vec![reference(support)],
        domain_generation: 1,
        state: State::Live,
    }
}
fn admin(id: u64, subject: u32, issuer: u32, support: u64, depth: u8) -> Mandate {
    let mut m = use_grant(id, subject, issuer, support);
    m.kind = Kind::Admin;
    m.scope = scope(actions());
    let mut e = envelope();
    e.delegation_depth = depth;
    m.envelope = Some(e);
    m
}

#[test]
fn grants_from_a_complete_envelope_retain_attribution() {
    let mut l = ledger();
    let a = activation(&l, 1, 10);
    l.issue(&a, use_grant(2, 20, 10, 1), T).unwrap();
    assert_eq!(l.get(reference(2)).unwrap().issuer, 10);
    assert_eq!(l.get(reference(2)).unwrap().subject, 20);
    assert_eq!(l.is_live(reference(2), T.utc), Ok(()));
    assert_eq!(
        l.authorize(&a, &scope(Actions::ENROLL), T),
        Err(Error::Conflict)
    );
}
#[test]
fn use_does_not_authorize_persistent_delegation() {
    let mut l = ledger();
    let a = activation(&l, 1, 10);
    l.issue(&a, use_grant(2, 20, 10, 1), T).unwrap();
    assert_eq!(
        l.issue(&activation(&l, 2, 20), use_grant(3, 30, 20, 2), T),
        Err(Error::Denied)
    );
}
#[test]
fn cross_domain_request_is_not_ordered_by_administrator_rank() {
    let l = ledger();
    let a = activation(&l, 1, 10);
    let mut m = use_grant(2, 20, 10, 1);
    m.scope.domain = 2;
    assert_eq!(l.check_issue(&a, &m, T), Err(Error::Denied));
}
#[test]
fn different_resources_and_subjects_do_not_match() {
    let l = ledger();
    let a = activation(&l, 1, 10);
    let mut m = use_grant(2, 20, 10, 1);
    m.scope.resources[0].object = 2;
    assert_eq!(l.check_issue(&a, &m, T), Err(Error::Denied));
    let m = use_grant(2, 99, 10, 1);
    assert_eq!(l.check_issue(&a, &m, T), Err(Error::Denied));
}
#[test]
fn requested_use_actions_cannot_escape_the_envelope() {
    let l = ledger();
    let a = activation(&l, 1, 10);
    let mut m = use_grant(2, 20, 10, 1);
    m.scope.actions = Actions::NET_CONNECT.union(Actions::NET_LISTEN);
    assert_eq!(l.check_issue(&a, &m, T), Err(Error::Denied));
}
#[test]
fn malformed_or_mixed_action_sets_fail_closed() {
    assert_eq!(Actions::new(1 << 63), Err(Error::UnknownAction));
    assert_eq!(Actions::new(0), Err(Error::EmptyAuthority));
    let l = ledger();
    let mut m = use_grant(2, 20, 10, 1);
    m.scope.actions = Actions::NET_CONNECT.union(Actions::GRANT);
    assert_eq!(
        l.check_issue(&activation(&l, 1, 10), &m, T),
        Err(Error::Invalid)
    );
}
#[test]
fn canonical_selectors_and_bounds_are_required() {
    let l = ledger();
    let mut m = use_grant(2, 20, 10, 1);
    m.scope.subjects = vec![20, 10];
    assert_eq!(
        l.check_issue(&activation(&l, 1, 10), &m, T),
        Err(Error::Invalid)
    );
    m.scope.subjects = vec![20, 20];
    assert_eq!(m.scope.validate(), Err(Error::Invalid));
    m.scope.subjects = vec![20];
    m.scope.resources.clear();
    assert_eq!(m.scope.validate(), Err(Error::Invalid));
}
#[test]
fn issuer_spoof_and_self_grants_are_denied() {
    let l = ledger();
    let a = activation(&l, 1, 10);
    assert_eq!(
        l.check_issue(&a, &use_grant(2, 20, 99, 1), T),
        Err(Error::Invalid)
    );
    assert_eq!(
        l.check_issue(&a, &use_grant(2, 10, 10, 1), T),
        Err(Error::SelfEscalation)
    );
    let mut wrong_actor = a;
    wrong_actor.actor = 20;
    assert_eq!(
        l.authorize(&wrong_actor, &scope(Actions::GRANT), T),
        Err(Error::Denied)
    );
}
#[test]
fn circular_principal_delegation_is_denied() {
    let mut l = ledger();
    let a = activation(&l, 1, 10);
    l.issue(&a, admin(2, 20, 10, 1, 3), T).unwrap();
    let b = activation(&l, 2, 20);
    assert_eq!(
        l.issue(&b, admin(3, 10, 20, 2, 2), T),
        Err(Error::SelfEscalation)
    );
    l.issue(&b, admin(3, 30, 20, 2, 2), T).unwrap();
    assert_eq!(
        l.issue(&activation(&l, 3, 30), use_grant(4, 10, 30, 3), T),
        Err(Error::SelfEscalation)
    );
}
#[test]
fn administrative_delegation_requires_explicit_depth_reduction() {
    let l = ledger();
    let a = activation(&l, 1, 10);
    assert_eq!(
        l.check_issue(&a, &admin(2, 20, 10, 1, 4), T),
        Err(Error::Denied)
    );
    let mut m = admin(2, 20, 10, 1, 3);
    m.envelope.as_mut().unwrap().scope.actions = Actions::FS_WRITE;
    assert_eq!(l.check_issue(&a, &m, T), Err(Error::Denied));
}
#[test]
fn nondelegating_admin_can_be_appointed() {
    let mut l = ledger();
    let mut m = admin(2, 20, 10, 1, 0);
    m.envelope = None;
    m.scope.actions = Actions::ENROLL;
    l.issue(&activation(&l, 1, 10), m, T).unwrap();
    assert_eq!(
        l.authorize(&activation(&l, 2, 20), &scope(Actions::ENROLL), T),
        Ok(())
    );
    assert_eq!(
        l.check_issue(&activation(&l, 2, 20), &use_grant(3, 30, 20, 2), T),
        Err(Error::Denied)
    );
}
#[test]
fn zero_depth_cannot_appoint_another_administrator() {
    let mut l = ledger();
    let mut m = admin(2, 20, 10, 1, 0);
    m.envelope.as_mut().unwrap().delegation_depth = 0;
    l.issue(&activation(&l, 1, 10), m, T).unwrap();
    let mut child = admin(3, 30, 20, 2, 0);
    child.envelope = None;
    assert_eq!(
        l.check_issue(&activation(&l, 2, 20), &child, T),
        Err(Error::Denied)
    );
}
#[test]
fn neither_authentication_nor_perpetuity_can_be_weakened() {
    let mut l = Ledger::new();
    let mut r = root();
    r.term = Term::UntilUtc(200);
    r.envelope.as_mut().unwrap().max_term = Term::UntilUtc(180);
    l.install_founding(r).unwrap();
    let a = activation(&l, 1, 10);
    let mut m = use_grant(2, 20, 10, 1);
    assert_eq!(l.check_issue(&a, &m, T), Err(Error::Denied));
    m.term = Term::UntilUtc(181);
    assert_eq!(l.check_issue(&a, &m, T), Err(Error::Denied));
    m.term = Term::UntilUtc(180);
    m.authentication = Auth::Session;
    assert_eq!(l.check_issue(&a, &m, T), Err(Error::Denied));
    m.authentication = Auth::DistinctKey;
    assert_eq!(l.check_issue(&a, &m, T), Ok(()));
}
#[test]
fn persistent_expiry_requires_trusted_time_and_is_exclusive() {
    let mut l = Ledger::new();
    let mut r = root();
    r.term = Term::UntilUtc(100);
    l.install_founding(r).unwrap();
    assert_eq!(l.is_live(reference(1), None), Err(Error::ClockUnavailable));
    assert_eq!(l.is_live(reference(1), Some(99)), Ok(()));
    assert_eq!(l.is_live(reference(1), Some(100)), Err(Error::Expired));
}
#[test]
fn activation_requires_live_scope_strong_auth_and_current_revision() {
    let l = ledger();
    let mut a = activation(&l, 1, 10);
    a.scope_id = 0;
    assert_eq!(
        l.authorize(&a, &scope(Actions::ENROLL), T),
        Err(Error::Inactive)
    );
    a.scope_id = 1;
    a.expires_mono = T.mono;
    assert_eq!(
        l.authorize(&a, &scope(Actions::ENROLL), T),
        Err(Error::Inactive)
    );
    a.expires_mono = 11;
    a.authentication = Auth::Session;
    assert_eq!(
        l.authorize(&a, &scope(Actions::ENROLL), T),
        Err(Error::Denied)
    );
    a.authentication = Auth::DistinctKey;
    a.policy_revision -= 1;
    assert_eq!(
        l.authorize(&a, &scope(Actions::ENROLL), T),
        Err(Error::Conflict)
    );
}
#[test]
fn supports_are_conjunctive_and_revisions_are_exact() {
    let l = ledger();
    let a = activation(&l, 1, 10);
    let mut m = use_grant(2, 20, 10, 1);
    m.supports.push(reference(99));
    assert_eq!(l.check_issue(&a, &m, T), Err(Error::Missing));
    m.supports = vec![Reference { id: 1, revision: 2 }];
    assert_eq!(l.check_issue(&a, &m, T), Err(Error::Denied)); // selected source absent
    assert_eq!(
        l.is_live(Reference { id: 1, revision: 2 }, T.utc),
        Err(Error::Stale)
    );
}
#[test]
fn a_second_support_cannot_launder_a_larger_envelope() {
    let mut l = ledger();
    let mut r = root();
    r.reference = reference(2);
    r.envelope.as_mut().unwrap().scope.actions = Actions::NET_LISTEN;
    l.install_founding(r).unwrap();
    let mut m = use_grant(3, 20, 10, 1);
    m.supports.push(reference(2));
    m.scope.actions = Actions::NET_LISTEN;
    assert_eq!(
        l.check_issue(&activation(&l, 1, 10), &m, T),
        Err(Error::Denied)
    );
}
#[test]
fn revoke_closes_the_entire_descendant_graph_before_completion() {
    let mut l = ledger();
    l.issue(&activation(&l, 1, 10), admin(2, 20, 10, 1, 3), T)
        .unwrap();
    l.issue(&activation(&l, 2, 20), use_grant(3, 30, 20, 2), T)
        .unwrap();
    let closure = l
        .begin_revoke(&activation(&l, 1, 10), &[reference(2)], T)
        .unwrap();
    assert_eq!(closure, vec![reference(2), reference(3)]);
    assert_eq!(l.is_live(reference(2), T.utc), Err(Error::Revoked));
    assert_eq!(l.is_live(reference(3), T.utc), Err(Error::Revoked));
    assert_eq!(
        l.finish_revoke(l.revision(), &[reference(2)]),
        Err(Error::Conflict)
    );
    l.finish_revoke(l.revision(), &closure).unwrap();
    assert_eq!(l.get(reference(3)).unwrap().state, State::Revoked);
}
#[test]
fn revoked_descendant_does_not_block_later_parent_revocation() {
    let mut l = ledger();
    l.issue(&activation(&l, 1, 10), admin(2, 20, 10, 1, 3), T)
        .unwrap();
    l.issue(&activation(&l, 2, 20), use_grant(3, 30, 20, 2), T)
        .unwrap();
    let c = l
        .begin_revoke(&activation(&l, 1, 10), &[reference(3)], T)
        .unwrap();
    l.finish_revoke(l.revision(), &c).unwrap();
    let c = l
        .begin_revoke(&activation(&l, 1, 10), &[reference(2)], T)
        .unwrap();
    assert_eq!(c, vec![reference(2)]);
    l.finish_revoke(l.revision(), &c).unwrap();
}
#[test]
fn founding_records_are_not_ordinary_revocation_targets() {
    let mut l = ledger();
    assert_eq!(
        l.begin_revoke(&activation(&l, 1, 10), &[reference(1)], T),
        Err(Error::Denied)
    );
    assert_eq!(l.is_live(reference(1), T.utc), Ok(()));
}
#[test]
fn failures_do_not_mutate_policy_or_consume_the_id() {
    let mut l = ledger();
    let a = activation(&l, 1, 10);
    let revision = l.revision();
    let mut m = use_grant(2, 20, 10, 1);
    m.scope.domain = 9;
    assert_eq!(l.issue(&a, m, T), Err(Error::Denied));
    assert_eq!(l.revision(), revision);
    assert_eq!(l.records().count(), 1);
    l.issue(&a, use_grant(2, 20, 10, 1), T).unwrap();
}
#[test]
fn an_id_cannot_be_reused_after_revocation() {
    let mut l = ledger();
    l.issue(&activation(&l, 1, 10), use_grant(2, 20, 10, 1), T)
        .unwrap();
    let c = l
        .begin_revoke(&activation(&l, 1, 10), &[reference(2)], T)
        .unwrap();
    l.finish_revoke(l.revision(), &c).unwrap();
    assert_eq!(
        l.issue(&activation(&l, 1, 10), use_grant(2, 20, 10, 1), T),
        Err(Error::Duplicate)
    );
}
#[test]
fn all_supports_bound_the_term_even_when_envelope_allows_perpetuity() {
    let mut l = ledger();
    let mut r = root();
    r.reference = reference(2);
    r.term = Term::UntilUtc(150);
    l.install_founding(r).unwrap();
    let mut m = use_grant(3, 20, 10, 1);
    m.supports.push(reference(2));
    assert_eq!(
        l.check_issue(&activation(&l, 1, 10), &m, T),
        Err(Error::Denied)
    );
    m.term = Term::UntilUtc(150);
    assert_eq!(l.check_issue(&activation(&l, 1, 10), &m, T), Ok(()));
}
#[test]
fn nested_administrative_auth_floor_cannot_be_weakened() {
    let l = ledger();
    let mut m = admin(2, 20, 10, 1, 3);
    m.envelope.as_mut().unwrap().auth_floor = Auth::Session;
    assert_eq!(
        l.check_issue(&activation(&l, 1, 10), &m, T),
        Err(Error::Denied)
    );
}
