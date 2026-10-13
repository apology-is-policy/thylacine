//! The native adapter against a simulated kernel (sim.rs).
mod sim;
use crate::native::{PoolMember, Ring, Shot};
use sim::{ECANCELED, EINVAL, ENOENT, ETIMEDOUT, RETIRED, Sim, SimKernel, SlotKind};
use std::future::Future;

const MS: u64 = 1_000_000;

fn ring(sim: &Sim, entries: u32, buffers: &[usize]) -> Ring<SimKernel> {
    Ring::setup(sim.kernel(), entries, buffers).expect("setup")
}
fn run<F: Future>(ring: &Ring<SimKernel>, f: F) -> F::Output {
    ring.block_on(f, None).expect("the ring stays up")
}
fn range(buffer_index: u32, offset: u64, length: u64) -> PoolMember {
    PoolMember {
        buffer_index,
        reserved: 0,
        offset,
        length,
    }
}
fn file(sim: &Sim, name: &str, bytes: &[u8]) {
    sim.s()
        .files
        .insert(name.as_bytes().to_vec(), bytes.to_vec());
}

#[test]
fn setup_registers_every_buffer_and_drop_detaches_every_region() {
    let sim = Sim::new();
    let r = ring(&sim, 8, &[4096, 5000]);
    // Two caller buffers (the second rounded up to two pages), the name arena,
    // and the ring mapping itself.
    assert_eq!(sim.regions(), 4);
    drop(r);
    assert_eq!(sim.regions(), 0);
    assert_eq!(sim.s().closed_fds, vec![7]);
}

#[test]
fn a_refused_attach_detaches_what_was_attached() {
    let sim = Sim::new();
    sim.s().attach_fail_at = Some(2);
    assert_eq!(Ring::setup(sim.kernel(), 8, &[4096, 4096]).err(), Some(-12));
    assert_eq!(sim.regions(), 0);
}

#[test]
fn connect_walk_open_write_then_read_round_trips() {
    let sim = Sim::new();
    file(&sim, "text", b"hello");
    let r = ring(&sim, 8, &[4096, 4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    let fid = run(&r, scope.walk(b"text", None).unwrap()).unwrap();
    run(&r, fid.open(2, None).unwrap()).unwrap();
    let wrote = run(&r, fid.write(range(0, 0, 5), 0, b"HELLO", None).unwrap()).unwrap();
    assert_eq!(wrote, 5);
    let done = run(&r, fid.read(range(1, 16, 5), 0, None).unwrap()).unwrap();
    assert_eq!(done.bytes(), b"HELLO");
    assert_eq!(sim.s().files[&b"text".to_vec()], b"HELLO");
}

#[test]
fn a_failed_connect_reaps_its_scope() {
    let sim = Sim::new();
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"absent").unwrap();
    assert_eq!(
        run(&r, target.connect(None).unwrap()).err(),
        Some(ENOENT as i32)
    );
    r.turn(Some(r.now())).unwrap();
    assert_eq!(sim.live_slots(), 1, "only the target remains");
    drop(target);
    r.turn(Some(r.now())).unwrap();
    assert_eq!(sim.live_slots(), 0);
}

#[test]
fn each_walk_name_is_read_inside_the_enter_that_consumes_it() {
    // One SQ slot, so both walks use the same name cell. The kernel copies the
    // component before ENTER returns, which is what lets the cell be reused.
    let sim = Sim::new();
    file(&sim, "a", b"first");
    file(&sim, "b", b"second");
    let r = ring(&sim, 1, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    let wa = scope.walk(b"a", None).unwrap();
    let wb = scope.walk(b"b", None).unwrap();
    let a = run(&r, wa).unwrap();
    let b = run(&r, wb).unwrap();
    run(&r, a.open(0, None).unwrap()).unwrap();
    run(&r, b.open(0, None).unwrap()).unwrap();
    assert_eq!(
        run(&r, a.read(range(0, 0, 16), 0, None).unwrap())
            .unwrap()
            .bytes(),
        b"first"
    );
    assert_eq!(
        run(&r, b.read(range(0, 0, 16), 0, None).unwrap())
            .unwrap()
            .bytes(),
        b"second"
    );
}

#[test]
fn queued_walks_keep_their_own_name_cells() {
    // Neither SQE is consumed until both are queued, so each name must still be
    // in its own slot's cell when the kernel reads it.
    let sim = Sim::new();
    file(&sim, "a", b"first");
    file(&sim, "b", b"second");
    let r = ring(&sim, 4, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    sim.s().enter_limit = Some(0);
    let wa = scope.walk(b"a", None).unwrap();
    let wb = scope.walk(b"b", None).unwrap();
    sim.s().enter_limit = None;
    let a = run(&r, wa).unwrap();
    let b = run(&r, wb).unwrap();
    run(&r, a.open(0, None).unwrap()).unwrap();
    run(&r, b.open(0, None).unwrap()).unwrap();
    assert_eq!(
        run(&r, a.read(range(0, 0, 16), 0, None).unwrap())
            .unwrap()
            .bytes(),
        b"first"
    );
    assert_eq!(
        run(&r, b.read(range(0, 0, 16), 0, None).unwrap())
            .unwrap()
            .bytes(),
        b"second"
    );
}

#[test]
fn a_multishot_stream_runs_on_out_of_order_returns() {
    let sim = Sim::new();
    file(&sim, "events", b"");
    let shots = ["one", "two", "three", "four", "five", "six"];
    sim.s().shots.insert(
        b"events".to_vec(),
        shots.iter().map(|s| s.as_bytes().to_vec()).collect(),
    );
    let r = ring(&sim, 8, &[4096]);
    let pool = r
        .pool(&[range(0, 0, 64), range(0, 64, 64), range(0, 128, 64)])
        .unwrap();
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    let fid = run(&r, scope.walk(b"events", None).unwrap()).unwrap();
    run(&r, fid.open(0, None).unwrap()).unwrap();
    let mut stream = fid.stream(&pool, 0, 64, true, None).unwrap();
    let mut seen = Vec::new();
    let mut held = Vec::new();
    let end = loop {
        match run(&r, stream.next()).unwrap() {
            Shot::Payload(p) => {
                seen.push(String::from_utf8(p.bytes().to_vec()).unwrap());
                held.push(p);
                // Three members: hold them all, then give back the newest
                // first, so the server's next member is not the oldest one.
                if held.len() == 3 {
                    let newest = held.pop().unwrap();
                    newest.try_return().map_err(|(e, _)| e).unwrap();
                    drop(held.remove(0));
                }
            }
            Shot::End(rc) => break rc,
        }
    };
    assert_eq!(end, 0);
    assert_eq!(seen, shots);
    drop(held);
    assert_eq!(sim.s().returns, shots.len());
}

#[test]
fn a_deadline_ends_the_operation_and_aborts_its_scope() {
    let sim = Sim::new();
    file(&sim, "slow", b"never");
    let r = ring(&sim, 8, &[4096, 4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    let fid = run(&r, scope.walk(b"slow", None).unwrap()).unwrap();
    run(&r, fid.open(0, None).unwrap()).unwrap();
    sim.s().hold = true;
    let deadline = r.now() + 5 * MS;
    let late = fid.read(range(0, 0, 8), 0, Some(deadline)).unwrap();
    let sibling = fid.read(range(1, 0, 8), 0, None).unwrap();
    assert_eq!(run(&r, late).err(), Some(ETIMEDOUT as i32));
    assert_eq!(run(&r, sibling).err(), Some(ECANCELED as i32));
    assert!(r.now() >= deadline);
}

#[test]
fn poll_rounds_the_time_left_up_to_a_whole_millisecond() {
    let sim = Sim::new();
    let r = ring(&sim, 8, &[4096]);
    let deadline = r.now() + 1_500_000;
    assert_eq!(r.turn(Some(deadline)).unwrap(), 0);
    assert_eq!(sim.s().polls, vec![2]);
    assert!(r.now() >= deadline);
}

#[test]
fn dropping_a_future_aborts_its_scope_and_settles_its_entry() {
    let sim = Sim::new();
    file(&sim, "f", b"data");
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    let fid = run(&r, scope.walk(b"f", None).unwrap()).unwrap();
    run(&r, fid.open(0, None).unwrap()).unwrap();
    sim.s().hold = true;
    let read = fid.read(range(0, 0, 4), 0, None).unwrap();
    let before = sim.s().aborts.len();
    drop(read);
    assert_eq!(sim.s().aborts.len(), before + 1, "one ABORT for the drop");
    // The cancelled READ's terminal arrives and is discarded; nothing waits.
    r.turn(Some(r.now() + MS)).unwrap();
    assert_eq!(sim.pending_ops(), 0);
    // The scope is gone: new work on it ends -ECANCELED.
    sim.s().hold = false;
    assert_eq!(
        run(&r, fid.read(range(0, 0, 4), 0, None).unwrap()).err(),
        Some(ECANCELED as i32)
    );
}

#[test]
fn a_full_cq_defers_completions_and_loses_none() {
    // Two SQ slots, four CQ slots, eight shots: the kernel defers what does not
    // fit, and ENTER after each drain lets it post the rest.
    let sim = Sim::new();
    file(&sim, "burst", b"");
    let shots: Vec<Vec<u8>> = (0..8).map(|i| vec![b'a' + i; 4]).collect();
    sim.s()
        .shots
        .insert(b"burst".to_vec(), shots.iter().cloned().collect());
    let r = ring(&sim, 2, &[4096]);
    let members: Vec<PoolMember> = (0..8).map(|i| range(0, i * 64, 64)).collect();
    let pool = r.pool(&members).unwrap();
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    let fid = run(&r, scope.walk(b"burst", None).unwrap()).unwrap();
    run(&r, fid.open(0, None).unwrap()).unwrap();
    let mut stream = fid.stream(&pool, 0, 64, true, None).unwrap();
    let mut got = Vec::new();
    let mut held = Vec::new();
    loop {
        match run(&r, stream.next()).unwrap() {
            Shot::Payload(p) => {
                got.push(p.bytes().to_vec());
                held.push(p);
            }
            Shot::End(rc) => {
                assert_eq!(rc, 0);
                break;
            }
        }
    }
    assert_eq!(got, shots);
}

#[test]
fn a_short_enter_leaves_the_rest_queued_for_a_later_turn() {
    let sim = Sim::new();
    file(&sim, "f", b"x");
    let r = ring(&sim, 4, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    sim.s().enter_limit = Some(0);
    let walks: Vec<_> = (0..3).map(|_| scope.walk(b"f", None).unwrap()).collect();
    assert_eq!(
        sim.hdr_word(1).wrapping_sub(sim.hdr_word(0)),
        3,
        "all three queued"
    );
    sim.s().enter_limit = Some(1);
    for w in walks {
        assert!(run(&r, w).is_ok());
    }
    assert_eq!(sim.hdr_word(1), sim.hdr_word(0), "every SQE consumed");
}

#[test]
fn a_full_sq_answers_enospc() {
    let sim = Sim::new();
    let r = ring(&sim, 2, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    sim.s().enter_limit = Some(0);
    let a = scope.walk(b"", None).unwrap();
    let b = scope.walk(b"", None).unwrap();
    assert_eq!(scope.walk(b"", None).err(), Some(-28));
    drop((a, b));
}

#[test]
fn a_rejected_sqe_ends_its_future_with_the_kernels_errno() {
    let sim = Sim::new();
    file(&sim, "f", b"x");
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    let fid = run(&r, scope.walk(b"f", None).unwrap()).unwrap();
    assert_eq!(
        run(&r, fid.open(7, None).unwrap()).err(),
        Some(EINVAL as i32)
    );
    assert_eq!(
        run(&r, scope.walk(b"..", None).unwrap()).err(),
        Some(EINVAL as i32)
    );
}

#[test]
#[should_panic(expected = "dropped an SQE")]
fn a_dropped_sqe_ends_the_process() {
    let sim = Sim::new();
    let r = ring(&sim, 8, &[4096]);
    sim.poke_hdr(9, 1);
    let _ = r.target(5, b"clip").unwrap().connect(None);
}

#[test]
fn a_forged_completion_fails_the_ring() {
    let sim = Sim::new();
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    sim.s().forge_next = Some(0xdead);
    let connect = target.connect(None).unwrap();
    assert_eq!(r.turn(None).err(), Some(-5));
    assert_eq!(
        r.block_on(connect, None).and_then(|c| c.map(drop)).err(),
        Some(-5)
    );
    assert_eq!(sim.s().closed_fds, vec![7], "a failed ring is closed");
}

#[test]
fn closing_with_leases_held_keeps_their_bytes_until_they_drop() {
    let sim = Sim::new();
    file(&sim, "f", b"kept");
    let r = ring(&sim, 8, &[4096]);
    let pool = r.pool(&[range(0, 0, 64)]).unwrap();
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    let fid = run(&r, scope.walk(b"f", None).unwrap()).unwrap();
    run(&r, fid.open(0, None).unwrap()).unwrap();
    let mut stream = fid.stream(&pool, 0, 64, false, None).unwrap();
    let Shot::Payload(p) = run(&r, stream.next()).unwrap() else {
        panic!("a payload")
    };
    drop((stream, fid, scope, target, pool));
    drop(r);
    assert_eq!(sim.s().closed_fds, vec![7]);
    // The buffer, the name arena and the ring mapping, all still attached.
    assert_eq!(sim.regions(), 3);
    assert_eq!(p.bytes(), b"kept");
    drop(p);
    assert_eq!(sim.regions(), 0);
}

/// A ring closed under the adapter cancels everything, and its descriptor is
/// never closed again: another thread closed it, so the number may name
/// another file. `latch` is a close that races the adapter's call.
fn closed_under(latch: bool, in_flight: bool) -> Vec<i32> {
    let sim = Sim::new();
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    sim.s().hold = true;
    let walk = in_flight.then(|| scope.walk(b"", None).unwrap());
    if latch {
        sim.latch_under()
    } else {
        sim.close_under()
    }
    match walk {
        // Its cancellation is routed, and the ENTER after it learns of the close.
        Some(walk) => {
            let out = r.block_on(walk, None).and_then(|w| w.map(drop));
            assert_eq!(out.err(), Some(ECANCELED as i32));
        }
        // Nothing to route: the wait learns of it.
        None => assert_eq!(r.turn(None).err(), Some(ECANCELED as i32)),
    }
    assert_eq!(scope.walk(b"", None).err(), Some(ECANCELED as i32));
    drop((scope, target, r));
    let closed = sim.s().closed_fds.clone();
    closed
}

#[test]
fn an_enter_that_finds_the_descriptor_closed_forgets_it() {
    assert_eq!(
        closed_under(false, true),
        vec![7],
        "only the other thread's close"
    );
}

#[test]
fn an_enter_that_races_the_close_forgets_the_descriptor() {
    assert_eq!(closed_under(true, true), Vec::<i32>::new());
}

#[test]
fn a_poll_that_finds_the_descriptor_closed_forgets_it() {
    assert_eq!(
        closed_under(false, false),
        vec![7],
        "only the other thread's close"
    );
}

#[test]
fn a_hangup_while_polling_forgets_the_descriptor() {
    assert_eq!(closed_under(true, false), Vec::<i32>::new());
}

#[test]
fn retire_waits_until_the_kernel_retires_every_scope() {
    let sim = Sim::new();
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    assert!(!r.retired());
    r.retire();
    assert_eq!(target.connect(None).err(), Some(ECANCELED as i32));
    let deadline = r.now() + 10 * MS;
    while !r.retired() && r.now() < deadline {
        r.turn(Some(deadline)).unwrap();
    }
    assert!(r.retired());
    assert_eq!(sim.s().aborts.len(), 1);
    let kernel_retired = sim
        .s()
        .slots
        .iter()
        .flatten()
        .any(|s| s.kind == SlotKind::Scope && s.state == RETIRED);
    assert!(
        kernel_retired,
        "retired() answered only after the kernel retired the scope"
    );
    drop(scope);
}

#[test]
fn a_dropped_fid_is_clunked_then_reaped() {
    let sim = Sim::new();
    file(&sim, "f", b"x");
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    let fid = run(&r, scope.walk(b"f", None).unwrap()).unwrap();
    assert_eq!(sim.live_slots(), 3);
    drop(fid);
    for _ in 0..3 {
        r.turn(Some(r.now() + MS)).unwrap();
    }
    assert_eq!(sim.live_slots(), 2, "the fid's slot is free");
    assert!(sim.s().aborts.is_empty(), "a clunk is not a cancel");
}

#[test]
fn a_dropped_scope_is_reaped_after_its_fids_without_clunking_them() {
    let sim = Sim::new();
    file(&sim, "f", b"x");
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    let a = run(&r, scope.walk(b"f", None).unwrap()).unwrap();
    let b = run(&r, scope.walk(b"", None).unwrap()).unwrap();
    run(&r, a.open(0, None).unwrap()).unwrap();
    assert_eq!(sim.live_slots(), 4);
    // The abort ends the session, so no fid is clunked. Each fid is reaped once
    // the scope has retired, and the scope after its fids, in the same turn.
    drop((a, b, scope));
    r.turn(Some(r.now() + MS)).unwrap();
    assert_eq!(sim.live_slots(), 1, "only the target remains");
    assert_eq!(sim.s().clunks, 0);
    assert_eq!(sim.s().aborts.len(), 1);
}

#[test]
fn a_failed_walk_dropped_unobserved_is_reaped_without_a_clunk() {
    let sim = Sim::new();
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    let walk = scope.walk(b"missing", None).unwrap();
    for _ in 0..3 {
        r.turn(Some(r.now() + MS)).unwrap();
    }
    assert_eq!(
        sim.pending_ops(),
        0,
        "the WALK's -ENOENT arrived unobserved"
    );
    drop(walk);
    for _ in 0..3 {
        r.turn(Some(r.now() + MS)).unwrap();
    }
    assert_eq!(sim.live_slots(), 2, "the reservation is reaped");
    assert_eq!(
        sim.s().clunk_sqes,
        0,
        "a fid that never walked is not clunked"
    );
    assert!(sim.s().aborts.is_empty());
}

#[test]
fn a_walk_dropped_in_flight_aborts_and_reaps_its_reservation_unclunked() {
    let sim = Sim::new();
    file(&sim, "f", b"x");
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    sim.s().hold = true;
    let walk = scope.walk(b"f", None).unwrap();
    assert_eq!(sim.live_slots(), 3);
    drop(walk);
    assert_eq!(sim.s().aborts.len(), 1, "one ABORT for the drop");
    for _ in 0..3 {
        r.turn(Some(r.now() + MS)).unwrap();
    }
    assert_eq!(sim.pending_ops(), 0);
    assert_eq!(
        sim.live_slots(),
        2,
        "the destination is reaped once its WALK ends"
    );
    assert_eq!(
        sim.s().clunk_sqes,
        0,
        "an aborted scope's fid is not clunked"
    );
}

#[test]
fn an_open_dropped_in_flight_aborts_its_scope() {
    let sim = Sim::new();
    file(&sim, "f", b"x");
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    let fid = run(&r, scope.walk(b"f", None).unwrap()).unwrap();
    sim.s().hold = true;
    let open = fid.open(0, None).unwrap();
    drop(open);
    assert_eq!(sim.s().aborts.len(), 1, "one ABORT for the drop");
    r.turn(Some(r.now() + MS)).unwrap();
    assert_eq!(sim.pending_ops(), 0);
    sim.s().hold = false;
    assert_eq!(
        run(&r, fid.open(0, None).unwrap()).err(),
        Some(ECANCELED as i32)
    );
}

#[test]
fn an_explicit_scope_abort_ends_its_operations_once() {
    let sim = Sim::new();
    file(&sim, "f", b"data");
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    let fid = run(&r, scope.walk(b"f", None).unwrap()).unwrap();
    run(&r, fid.open(0, None).unwrap()).unwrap();
    sim.s().hold = true;
    let read = fid.read(range(0, 0, 4), 0, None).unwrap();
    scope.abort();
    scope.abort();
    assert_eq!(sim.s().aborts.len(), 1, "the second abort issues nothing");
    assert_eq!(run(&r, read).err(), Some(ECANCELED as i32));
}

#[test]
fn a_target_dropped_under_its_connect_is_reaped_after_it() {
    let sim = Sim::new();
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    sim.s().hold = true;
    let connect = target.connect(None).unwrap();
    drop(target);
    r.turn(Some(r.now() + MS)).unwrap();
    assert_eq!(sim.live_slots(), 2, "the CONNECT still borrows its target");
    sim.s().hold = false;
    let scope = run(&r, connect).unwrap();
    r.turn(Some(r.now() + MS)).unwrap();
    assert_eq!(sim.live_slots(), 1, "only the scope remains");
    drop(scope);
}

#[test]
fn an_exhausted_slot_table_reaps_before_it_refuses() {
    let sim = Sim::new();
    file(&sim, "f", b"x");
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    // Fill the rest of the table with targets, then let one go unreaped.
    let mut spare: Vec<_> = (0..62).map(|_| r.target(5, b"clip").unwrap()).collect();
    assert_eq!(sim.live_slots(), 64);
    drop(spare.pop());
    let fid = run(&r, scope.walk(b"f", None).unwrap());
    assert!(fid.is_ok(), "the walk took the reaped slot");
}

fn scope_retired_in_kernel(sim: &Sim) -> bool {
    sim.s()
        .slots
        .iter()
        .flatten()
        .any(|s| s.kind == SlotKind::Scope && s.state == RETIRED)
}
fn turn_until_retired(r: &Ring<SimKernel>) {
    let deadline = r.now() + 10 * MS;
    while !r.retired() && r.now() < deadline {
        r.turn(Some(deadline)).unwrap();
    }
    assert!(r.retired());
}

#[test]
fn a_future_held_across_retirement_keeps_its_terminal() {
    let sim = Sim::new();
    file(&sim, "f", b"x");
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    sim.s().hold = true;
    let walk = scope.walk(b"f", None).unwrap();
    r.retire();
    turn_until_retired(&r);
    assert_eq!(
        run(&r, walk).err(),
        Some(ECANCELED as i32),
        "the WALK keeps the terminal its abort gave it"
    );
    assert!(sim.s().closed_fds.is_empty(), "no future closes the ring");
    drop(scope);
}

#[test]
fn retirement_waits_for_terminals_posted_after_the_scope_retires() {
    let sim = Sim::new();
    file(&sim, "f", b"data");
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    let fid = run(&r, scope.walk(b"f", None).unwrap()).unwrap();
    run(&r, fid.open(0, None).unwrap()).unwrap();
    sim.s().hold = true;
    sim.s().hold_terminals = true;
    drop(fid.read(range(0, 0, 4), 0, None).unwrap());
    r.retire();
    for _ in 0..3 {
        r.turn(Some(r.now() + MS)).unwrap();
    }
    assert!(scope_retired_in_kernel(&sim), "the scope retired first");
    assert!(!r.retired(), "retired() waits for the READ's terminal");
    sim.release_terminals();
    turn_until_retired(&r);
    assert!(sim.s().closed_fds.is_empty(), "the late terminal did not fail the ring");
    drop((fid, scope));
}

#[test]
fn an_sqe_not_yet_entered_holds_retirement_back() {
    let sim = Sim::new();
    file(&sim, "f", b"data");
    let r = ring(&sim, 8, &[4096]);
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    let fid = run(&r, scope.walk(b"f", None).unwrap()).unwrap();
    run(&r, fid.open(0, None).unwrap()).unwrap();
    sim.s().enter_limit = Some(0);
    drop(fid.read(range(0, 0, 4), 0, None).unwrap());
    r.retire();
    for _ in 0..3 {
        r.turn(Some(r.now() + MS)).unwrap();
    }
    assert_eq!(
        sim.hdr_word(1).wrapping_sub(sim.hdr_word(0)),
        1,
        "the READ is still queued"
    );
    assert!(scope_retired_in_kernel(&sim), "the scope retired first");
    assert!(!r.retired(), "retired() waits for the queued READ's terminal");
    sim.s().enter_limit = None;
    turn_until_retired(&r);
    assert!(sim.s().closed_fds.is_empty(), "the late terminal did not fail the ring");
    drop((fid, scope));
}

#[test]
fn a_terminal_posted_but_not_yet_routed_holds_retirement_back() {
    // Two SQ slots and four CQ slots: eight shots and the end overflow the CQ,
    // and the ENTER after each drain posts what was deferred.
    let sim = Sim::new();
    file(&sim, "burst", b"");
    sim.s()
        .shots
        .insert(b"burst".to_vec(), (0..8).map(|i| vec![b'a' + i; 4]).collect());
    let r = ring(&sim, 2, &[4096]);
    let members: Vec<PoolMember> = (0..8).map(|i| range(0, i * 64, 64)).collect();
    let pool = r.pool(&members).unwrap();
    let target = r.target(5, b"clip").unwrap();
    let scope = run(&r, target.connect(None).unwrap()).unwrap();
    let fid = run(&r, scope.walk(b"burst", None).unwrap()).unwrap();
    run(&r, fid.open(0, None).unwrap()).unwrap();
    let stream = fid.stream(&pool, 0, 64, true, None).unwrap();
    // The server answers every shot; the first four are routed to the stream.
    r.turn(Some(r.now() + MS)).unwrap();
    drop(stream);
    r.retire();
    sim.engine_visit();
    assert!(scope_retired_in_kernel(&sim), "the scope retired first");
    // Route four shots; the ENTER that follows posts the end.
    assert_eq!(r.turn(Some(r.now() + MS)).unwrap(), 4);
    assert!(!r.retired(), "retired() waits for the end still in the CQ");
    turn_until_retired(&r);
    assert!(sim.s().closed_fds.is_empty(), "the end did not fail the ring");
    assert_eq!(sim.s().returns, 8, "every shot's lease went back");
    drop((fid, scope, pool));
}
