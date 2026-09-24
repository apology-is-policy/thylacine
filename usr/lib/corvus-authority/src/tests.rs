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
        transaction: [0x42; 16],
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
        transaction: [0x42; 16],
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

#[test]
fn canonical_record_has_a_pinned_header_and_roundtrips_all_states() {
    let rec = use_grant(2, 20, 10, 1);
    let bytes = rec.encode().unwrap();
    assert_eq!(&bytes[..16], b"MDTM\x01\0\0\0\x84\0\0\0\x01\x01\x01\0");
    assert_eq!(&bytes[72..88], &[0x42; 16]);
    assert_eq!(Mandate::decode(&bytes), Ok(rec));
    for state in [State::Live, State::Revoking, State::Revoked] {
        for term in [
            Term::UntilRevoked,
            Term::UntilUtc(1),
            Term::UntilUtc(u64::MAX),
        ] {
            for kind in [Kind::Use, Kind::Activate, Kind::Admin] {
                let mut m = if kind == Kind::Admin {
                    admin(2, 20, 10, 1, 3)
                } else {
                    use_grant(2, 20, 10, 1)
                };
                m.kind = kind;
                m.state = state;
                m.term = term;
                let rec = m;
                let bytes = rec.encode().unwrap();
                assert_eq!(Mandate::decode(&bytes), Ok(rec));
            }
        }
    }
}
#[test]
fn codec_rejects_every_truncation_and_trailing_data() {
    let rec = admin(2, 20, 10, 1, 3);
    let good = rec.encode().unwrap();
    for n in 0..good.len() {
        assert!(Mandate::decode(&good[..n]).is_err(), "truncated at {n}");
        if n >= abi::MANDATE_HEADER_LEN {
            let mut bad = good[..n].to_vec();
            bad[8..12].copy_from_slice(&(n as u32).to_le_bytes());
            assert!(
                Mandate::decode(&bad).is_err(),
                "truncated and patched at {n}"
            );
        }
    }
    let mut bad = good;
    bad.push(0);
    let len = bad.len() as u32;
    assert!(Mandate::decode(&bad).is_err());
    bad[8..12].copy_from_slice(&len.to_le_bytes());
    assert!(Mandate::decode(&bad).is_err());
}
#[test]
fn codec_refuses_unknown_tags_reserved_bytes_and_counts() {
    let good = use_grant(2, 20, 10, 1).encode().unwrap();
    for (offset, value) in [
        (0, 0),
        (4, 2),
        (6, 1),
        (7, 1),
        (12, 4),
        (13, 3),
        (14, 4),
        (15, 2),
        (88, 17),
        (89, 1),
        (90, 0),
        (92, 9),
        (94, 2),
        (95, 1),
    ] {
        let mut bad = good.clone();
        bad[offset] = value;
        assert!(Mandate::decode(&bad).is_err(), "accepted offset {offset}");
    }
    let mut bad = good.clone();
    bad[72..88].fill(0);
    assert!(Mandate::decode(&bad).is_err());
    let mut bad = good.clone();
    bad[63] = 0x80;
    assert!(Mandate::decode(&bad).is_err());
    let mut bad = good.clone();
    bad[64] = 1;
    assert!(Mandate::decode(&bad).is_err());
    let mut bad = good;
    bad[15] = 1;
    assert!(Mandate::decode(&bad).is_err());
}
#[test]
fn codec_refuses_invalid_envelope_and_noncanonical_selectors() {
    let good = admin(2, 20, 10, 1, 3).encode().unwrap();
    let envelope_offset = abi::MANDATE_HEADER_LEN + 4 * 4 + 16 + 16;
    for (offset, value) in [
        (24, 0),
        (24, 8),
        (25, 3),
        (26, 17),
        (27, 2),
        (28, 17),
        (30, 0),
    ] {
        let mut bad = good.clone();
        bad[envelope_offset + offset] = value;
        assert!(Mandate::decode(&bad).is_err());
    }
    let mut bad = good.clone();
    bad[100..104].copy_from_slice(&10u32.to_le_bytes());
    assert!(Mandate::decode(&bad).is_err()); // repeated subject
    let mut bad = good;
    bad[12] = abi::KIND_USE as u8;
    assert!(Mandate::decode(&bad).is_err()); // admin envelope on Use
}
#[test]
fn maximum_record_fits_the_reserved_bound_without_extra_bytes() {
    let mut m = admin(99, 20, 10, 1, 3);
    m.scope.subjects = (1..=16).collect();
    m.scope.resources = (1..=16)
        .map(|object| Resource { owner: 1, object })
        .collect();
    m.supports = (1..=8).map(reference).collect();
    let e = m.envelope.as_mut().unwrap();
    e.scope.subjects = m.scope.subjects.clone();
    e.scope.resources = m.scope.resources.clone();
    let rec = m;
    let bytes = rec.encode().unwrap();
    assert_eq!(bytes.len(), abi::MANDATE_MAX_LEN);
    assert_eq!(Mandate::decode(&bytes), Ok(rec.clone()));
    let mut too_big = rec;
    too_big.scope.subjects.push(17);
    assert_eq!(too_big.encode(), Err(Error::Invalid));
}
#[test]
fn decoding_never_turns_an_unauthorized_envelope_into_authority() {
    let l = ledger();
    let mut m = use_grant(2, 20, 10, 1);
    m.scope.actions = Actions::FS_WRITE;
    let bytes = m.encode().unwrap();
    let rec = Mandate::decode(&bytes).unwrap();
    assert_eq!(
        l.check_issue(&activation(&l, 1, 10), &rec, T),
        Err(Error::Denied)
    );
}
#[test]
fn per_subject_record_capacity_is_checked_before_mutation() {
    let mut l = ledger();
    for id in 2..=MAX_PER_SUBJECT as u64 + 1 {
        l.issue(&activation(&l, 1, 10), use_grant(id, 20, 10, 1), T)
            .unwrap();
    }
    let rev = l.revision();
    assert_eq!(
        l.issue(&activation(&l, 1, 10), use_grant(258, 20, 10, 1), T),
        Err(Error::Capacity)
    );
    assert_eq!(l.revision(), rev);
    assert_eq!(l.records().count(), MAX_PER_SUBJECT + 1);
}
#[test]
fn total_physical_capacity_includes_revoked_tombstones() {
    let mut l = Ledger::new();
    for id in 1..=MAX_RECORDS as u64 {
        let mut m = root();
        m.reference = reference(id);
        m.subject = id as u32;
        l.install_founding(m).unwrap();
    }
    let mut m = root();
    m.reference = reference(MAX_RECORDS as u64 + 1);
    m.subject = 9999;
    assert_eq!(l.install_founding(m), Err(Error::Capacity));
    assert_eq!(l.records().count(), MAX_RECORDS);
}
#[test]
fn support_graph_depth_is_bounded_even_without_administrative_redelegation() {
    let mut l = ledger();
    for id in 2..=MAX_DEPTH as u64 {
        // Independent same-subject records are allowed; their dependency edges
        // must use different principals to avoid a self-escalation path.
        let mut r = root();
        r.reference = reference(id);
        r.subject = id as u32 + 100;
        r.supports = vec![reference(id - 1)];
        // Corruption fixture exercises the walker directly; ordinary insertion
        // separately enforces envelope/depth and never gets this bypass.
        l.entries.push(Entry {
            mandate: r,
            founding: false,
        });
    }
    assert_eq!(l.is_live(reference(MAX_DEPTH as u64), T.utc), Ok(()));
    let mut r = root();
    r.reference = reference(MAX_DEPTH as u64 + 1);
    r.supports = vec![reference(MAX_DEPTH as u64)];
    l.entries.push(Entry {
        mandate: r,
        founding: false,
    });
    assert_eq!(
        l.is_live(reference(MAX_DEPTH as u64 + 1), T.utc),
        Err(Error::Depth)
    );
}
#[test]
fn revocation_finds_diamond_descendants_once_in_sorted_order() {
    let mut l = ledger();
    l.issue(&activation(&l, 1, 10), admin(2, 20, 10, 1, 3), T)
        .unwrap();
    l.issue(&activation(&l, 1, 10), admin(3, 30, 10, 1, 3), T)
        .unwrap();
    let mut child = use_grant(4, 40, 20, 2);
    child.supports.push(reference(3));
    l.issue(&activation(&l, 2, 20), child, T).unwrap();
    assert_eq!(
        l.revocation_closure(&[reference(1)]).unwrap(),
        vec![reference(1), reference(2), reference(3), reference(4)]
    );
    assert_eq!(
        l.revocation_closure(&[reference(2), reference(3)]).unwrap(),
        vec![reference(2), reference(3), reference(4)]
    );
}
#[test]
fn single_byte_mutations_are_either_refused_or_exactly_canonical() {
    let good = admin(2, 20, 10, 1, 3).encode().unwrap();
    for pos in 0..good.len() {
        for byte in 0..=255u8 {
            let mut candidate = good.clone();
            candidate[pos] = byte;
            if let Ok(decoded) = Mandate::decode(&candidate) {
                assert_eq!(
                    decoded.encode().unwrap(),
                    candidate,
                    "noncanonical at {pos}"
                );
            }
        }
    }
}

#[test]
fn dense_ledger_at_capacity_keeps_graph_work_and_payload_bounded() {
    extern crate std;
    let mut l = Ledger::new();
    let mut template = root();
    template.scope.subjects = (100..116).collect();
    template.scope.resources = (1..=16)
        .map(|object| Resource { owner: 1, object })
        .collect();
    template.envelope.as_mut().unwrap().scope.subjects = template.scope.subjects.clone();
    template.envelope.as_mut().unwrap().scope.resources = template.scope.resources.clone();
    for id in 1..=8 {
        let mut m = template.clone();
        m.reference = reference(id);
        m.subject = id as u32;
        l.install_founding(m).unwrap();
    }
    let started = std::time::Instant::now();
    for id in 9..=MAX_RECORDS as u64 {
        let mut m = template.clone();
        m.reference = reference(id);
        m.subject = 100 + ((id - 9) % 16) as u32;
        m.issuer = 1;
        m.envelope.as_mut().unwrap().delegation_depth = 3;
        m.supports = (1..=8).map(reference).collect();
        l.issue(&activation(&l, 1, 1), m, T).unwrap();
    }
    let closure_start = std::time::Instant::now();
    let closure = l.revocation_closure(&[reference(1)]).unwrap();
    let closure_elapsed = closure_start.elapsed();
    assert_eq!(closure.len(), MAX_RECORDS - 7);
    assert_eq!(closure.first(), Some(&reference(1)));
    assert_eq!(closure.last(), Some(&reference(MAX_RECORDS as u64)));
    let scope_bytes = |s: &Scope| {
        s.subjects.capacity() * core::mem::size_of::<u32>()
            + s.resources.capacity() * core::mem::size_of::<Resource>()
    };
    let payload = l.entries.capacity() * core::mem::size_of::<Entry>()
        + l.generations.capacity() * core::mem::size_of::<(u64, u64)>()
        + l.entries
            .iter()
            .map(|e| {
                let m = &e.mandate;
                scope_bytes(&m.scope)
                    + m.supports.capacity() * core::mem::size_of::<Reference>()
                    + m.envelope.as_ref().map_or(0, |e| scope_bytes(&e.scope))
            })
            .sum::<usize>();
    // This measures retained Vec payload, NOT allocator metadata or guest RSS.
    std::println!("dense policy: {MAX_RECORDS} records, payload={payload} bytes, setup={:?}, closure={closure_elapsed:?}", started.elapsed());
    assert!(payload < 5 * 1024 * 1024);
    let mut m = template;
    m.reference = reference(MAX_RECORDS as u64 + 1);
    m.subject = 9999;
    assert_eq!(l.install_founding(m), Err(Error::Capacity));
}

fn txn_peer() -> transaction::Peer {
    transaction::Peer {
        principal: 10,
        stripes: 77,
    }
}
fn txn_receipt(l: &Ledger) -> transaction::ViewReceipt {
    transaction::ViewReceipt {
        transaction: [0x42; 16],
        policy_revision: l.revision(),
        episode: 5,
        sequence: 9,
    }
}
fn prepare_txn(l: &Ledger) -> transaction::PreparedIssue {
    transaction::PreparedIssue::new(l, txn_peer(), reference(1), use_grant(2, 20, 10, 1), T)
        .unwrap()
}
fn ready_txn(l: &Ledger) -> transaction::PreparedIssue {
    let mut tx = prepare_txn(l);
    tx.await_sak(T.mono).unwrap();
    tx.visible(txn_receipt(l), T.mono).unwrap();
    tx.authenticated(txn_receipt(l), Auth::DistinctKey, T.mono)
        .unwrap();
    tx.restored(5).unwrap();
    tx
}
fn txn_proof(l: &Ledger) -> transaction::Proof {
    transaction::Proof {
        activation: activation(l, 1, 10),
        kind: transaction::ScopeKind::Administrative,
        transaction: [0x42; 16],
        root_stripes: 77,
    }
}
#[test]
fn issue_admission_binds_the_exact_immutable_preview_without_publishing() {
    let l = ledger();
    let tx = ready_txn(&l);
    let shown = tx.canonical().to_vec();
    let admitted = tx.admit(&l, txn_peer(), txn_proof(&l), T).unwrap();
    assert_eq!(admitted.canonical(), shown);
    assert_eq!(admitted.record().encode().unwrap(), shown);
    assert_eq!(admitted.peer(), txn_peer());
    assert_eq!(admitted.scope(), 1);
    assert_eq!(admitted.source(), reference(1));
    assert_eq!(admitted.policy_revision(), l.revision());
    assert_eq!(admitted.authentication(), Auth::DistinctKey);
    assert_eq!(admitted.admitted_mono(), T.mono);
    assert_eq!(l.records().count(), 1); // durable owner still owes publication + audit
}
#[test]
fn preparing_a_preview_does_not_require_or_manufacture_an_activation() {
    let l = ledger();
    let tx = prepare_txn(&l);
    assert_eq!(tx.phase(), transaction::Phase::Prepared);
    let mut bad = use_grant(2, 20, 10, 1);
    bad.scope.actions = Actions::FS_WRITE;
    assert!(matches!(
        transaction::PreparedIssue::new(&l, txn_peer(), reference(1), bad, T),
        Err(Error::Denied)
    ));
    assert_eq!(l.records().count(), 1);
}
#[test]
fn no_issuance_can_be_admitted_before_successful_restoration() {
    let l = ledger();
    for phase in 0..4 {
        let mut tx = prepare_txn(&l);
        if phase > 0 {
            tx.await_sak(T.mono).unwrap();
        }
        if phase > 1 {
            tx.visible(txn_receipt(&l), T.mono).unwrap();
        }
        if phase > 2 {
            tx.authenticated(txn_receipt(&l), Auth::DistinctKey, T.mono)
                .unwrap();
        }
        assert!(matches!(
            tx.admit(&l, txn_peer(), txn_proof(&l), T),
            Err(Error::Conflict)
        ));
    }
}
#[test]
fn a_child_or_new_incarnation_cannot_redeem_the_parents_transaction() {
    let l = ledger();
    let mut child = txn_peer();
    child.stripes += 1;
    let mut proof = txn_proof(&l);
    proof.root_stripes = child.stripes;
    assert!(matches!(
        ready_txn(&l).admit(&l, child, proof, T),
        Err(Error::Denied)
    ));
    let mut other = txn_peer();
    other.principal += 1;
    assert!(matches!(
        ready_txn(&l).admit(&l, other, txn_proof(&l), T),
        Err(Error::Denied)
    ));
}
#[test]
fn execution_scopes_and_unrelated_transaction_ids_are_not_admin_proofs() {
    let l = ledger();
    let mut proof = txn_proof(&l);
    proof.kind = transaction::ScopeKind::Execution;
    assert!(matches!(
        ready_txn(&l).admit(&l, txn_peer(), proof, T),
        Err(Error::Denied)
    ));
    let mut proof = txn_proof(&l);
    proof.transaction[0] ^= 1;
    assert!(matches!(
        ready_txn(&l).admit(&l, txn_peer(), proof, T),
        Err(Error::Denied)
    ));
    let mut proof = txn_proof(&l);
    proof.activation.authority = reference(99);
    assert!(matches!(
        ready_txn(&l).admit(&l, txn_peer(), proof, T),
        Err(Error::Denied)
    ));
}
#[test]
fn any_policy_revision_change_invalidates_the_old_approval() {
    let mut l = ledger();
    let tx = ready_txn(&l);
    let mut r = root();
    r.reference = reference(3);
    r.subject = 40;
    l.install_founding(r).unwrap();
    assert!(matches!(
        tx.admit(&l, txn_peer(), txn_proof(&l), T),
        Err(Error::Conflict)
    ));
}
#[test]
fn admission_rechecks_support_even_if_a_version_bump_were_missed() {
    let mut l = ledger();
    let tx = ready_txn(&l);
    // Corruption/sabotage fixture: bypass the public mutation API's revision
    // bump. The independent support validation must still detect invalidation.
    l.generations[0].1 += 1;
    assert!(matches!(
        tx.admit(&l, txn_peer(), txn_proof(&l), T),
        Err(Error::Stale)
    ));
}
#[test]
fn trusted_receipts_cannot_cross_transactions_or_episodes() {
    let l = ledger();
    let mut tx = prepare_txn(&l);
    tx.await_sak(T.mono).unwrap();
    let mut wrong = txn_receipt(&l);
    wrong.transaction[0] ^= 1;
    assert_eq!(tx.visible(wrong, T.mono), Err(Error::Denied));
    let mut wrong = txn_receipt(&l);
    wrong.policy_revision += 1;
    assert_eq!(tx.visible(wrong, T.mono), Err(Error::Denied));
    tx.visible(txn_receipt(&l), T.mono).unwrap();
    let mut wrong = txn_receipt(&l);
    wrong.sequence += 1;
    assert_eq!(
        tx.authenticated(wrong, Auth::DistinctKey, T.mono),
        Err(Error::Denied)
    );
    tx.authenticated(txn_receipt(&l), Auth::DistinctKey, T.mono)
        .unwrap();
    assert_eq!(tx.restored(6), Err(Error::Denied));
    assert_eq!(tx.phase(), transaction::Phase::Authenticated);
}
#[test]
fn authentication_is_checked_against_the_source_floor_and_bound_to_the_proof() {
    let l = ledger();
    let mut tx = prepare_txn(&l);
    tx.await_sak(T.mono).unwrap();
    tx.visible(txn_receipt(&l), T.mono).unwrap();
    assert_eq!(
        tx.authenticated(txn_receipt(&l), Auth::Session, T.mono),
        Err(Error::Denied)
    );
    let mut proof = txn_proof(&l);
    proof.activation.authentication = Auth::Founding;
    assert!(matches!(
        ready_txn(&l).admit(&l, txn_peer(), proof, T),
        Err(Error::Denied)
    ));
}
#[test]
fn preparation_expiry_and_live_scope_expiry_are_separate_gates() {
    let l = ledger();
    let mut tx = prepare_txn(&l);
    assert_eq!(
        tx.await_sak(T.mono + transaction::PREPARE_LIFETIME_NS),
        Err(Error::Expired)
    );
    let tx = ready_txn(&l);
    let time = Time { mono: 20, ..T };
    assert!(matches!(
        tx.admit(&l, txn_peer(), txn_proof(&l), time),
        Err(Error::Inactive)
    ));
    let time = Time {
        mono: u64::MAX,
        ..T
    };
    assert!(matches!(
        transaction::PreparedIssue::new(
            &l,
            txn_peer(),
            reference(1),
            use_grant(2, 20, 10, 1),
            time
        ),
        Err(Error::Overflow)
    ));
}
#[test]
fn cancellation_before_admission_is_terminal() {
    let l = ledger();
    let mut tx = ready_txn(&l);
    tx.cancel();
    assert_eq!(tx.phase(), transaction::Phase::Cancelled);
    assert_eq!(tx.await_sak(T.mono), Err(Error::Conflict));
    assert!(matches!(
        tx.admit(&l, txn_peer(), txn_proof(&l), T),
        Err(Error::Conflict)
    ));
}

#[test]
fn transaction_provenance_survives_issue_and_both_revocation_phases() {
    let mut l = ledger();
    let mut m = use_grant(2, 20, 10, 1);
    m.transaction = [0xa7; 16];
    let bytes = m.encode().unwrap();
    l.issue(&activation(&l, 1, 10), Mandate::decode(&bytes).unwrap(), T)
        .unwrap();
    assert_eq!(l.get(reference(2)).unwrap().encode().unwrap(), bytes);
    let closure = l
        .begin_revoke(&activation(&l, 1, 10), &[reference(2)], T)
        .unwrap();
    assert_eq!(l.get(reference(2)).unwrap().transaction, [0xa7; 16]);
    l.finish_revoke(l.revision(), &closure).unwrap();
    let decoded = Mandate::decode(&l.get(reference(2)).unwrap().encode().unwrap()).unwrap();
    assert_eq!(decoded.transaction, [0xa7; 16]);
    assert_eq!(decoded.state, State::Revoked);
}
#[test]
fn policy_cannot_insert_a_record_without_transaction_provenance() {
    let mut l = ledger();
    let rev = l.revision();
    let mut m = use_grant(2, 20, 10, 1);
    m.transaction = [0; 16];
    assert_eq!(l.issue(&activation(&l, 1, 10), m, T), Err(Error::Invalid));
    assert_eq!(l.revision(), rev);
    let mut m = root();
    m.transaction = [0; 16];
    assert_eq!(Ledger::new().install_founding(m), Err(Error::Invalid));
}
