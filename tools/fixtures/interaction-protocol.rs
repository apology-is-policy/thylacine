#[cfg(test)]
mod application_protocol_tests {
    use super::*;
    use controllers::{Peer, RouteKey};
    use hostbindings::{Bindings, Desired, Host};
    use interaction::Interaction;
    use interaction_body::{Request, Response};
    use interaction_control::{Op, Reply};
    struct World {
        p: Protocol,
        routes: Routes,
        owner: Interaction,
        bindings: Bindings,
        desired: Desired,
        peer: Peer,
        queued: Option<interaction_control::Request>,
    }
    impl World {
        fn new() -> Self {
            let peer = Peer {
                connection: 1,
                stripes: 7,
                principal: 1000,
                alive: true,
            };
            let mut routes = Routes::empty();
            assert!(routes.insert(10, 7));
            let route = *routes.get(&10).unwrap();
            let mut desired = Desired::empty();
            assert!(desired.announce(
                &routes,
                Host {
                    route,
                    pid: 55,
                    binding: 99
                }
            ));
            let mut owner = Interaction::new(3, 1000).unwrap();
            owner.seat(Some(1));
            let mut bindings = Bindings::new();
            let action = bindings.plan(&desired, Some(1)).unwrap();
            let q = owner
                .control(
                    Op::Bind,
                    RouteKey {
                        leaf: 7,
                        incarnation: route.incarnation,
                    },
                    55,
                    99,
                    0,
                )
                .unwrap();
            assert!(bindings.started(action, q, 1));
            bindings.terminal_state(7, 99, 4);
            assert!(bindings.complete(q, Ok(())));
            owner
                .complete(
                    q,
                    None,
                    Ok(Reply {
                        op: q.op,
                        request: q.request,
                        focus: 2,
                        seat: 1,
                        foreground: 4,
                    }),
                    0,
                )
                .unwrap();
            let mut p = Protocol::new();
            p.version_done = true;
            p.application = Some(Application::new(3, peer).unwrap());
            for fid in [1, 2] {
                assert!(p.fid_set(fid, Node::Interaction(10), Some(route)));
            }
            let mut w = Self {
                p,
                routes,
                owner,
                bindings,
                desired,
                peer,
                queued: None,
            };
            for fid in [1u32, 2] {
                let mut b = fid.to_le_bytes().to_vec();
                b.extend_from_slice(&2u32.to_le_bytes());
                assert!(matches!(
                    w.call(p9::P9_TLOPEN, 10 + fid as u16, &b),
                    Disp::Reply(_)
                ));
                assert_eq!(w.p.out_buf[4], p9::P9_RLOPEN);
            }
            w
        }
        fn call(&mut self, kind: u8, tag: u16, data: &[u8]) -> Disp {
            let mut frame = vec![0; 7];
            frame[4] = kind;
            frame[5..7].copy_from_slice(&tag.to_le_bytes());
            frame.extend_from_slice(data);
            let n = frame.len() as u32;
            frame[..4].copy_from_slice(&n.to_le_bytes());
            let mut ctx = AppContext {
                owner: &mut self.owner,
                bindings: &self.bindings,
                desired: &self.desired,
                queued: &mut self.queued,
                peer: self.peer,
                now: 0,
            };
            self.p.dispatch_with(
                &frame,
                p9::peek_header(&frame).unwrap(),
                &mut Vec::new(),
                &self.routes,
                Budget {
                    max_pixels: 64,
                    others_reserved: 0,
                    residual_bytes: 512,
                    completion_slots: 2,
                },
                &mut Diag::default(),
                Some(&mut ctx),
                8192,
            )
        }
        fn write(&mut self, fid: u32, tag: u16, off: u64, data: &[u8]) -> Disp {
            let mut b = fid.to_le_bytes().to_vec();
            b.extend_from_slice(&off.to_le_bytes());
            b.extend_from_slice(&(data.len() as u32).to_le_bytes());
            b.extend_from_slice(data);
            self.call(p9::P9_TWRITE, tag, &b)
        }
        fn request(&mut self, fid: u32, tag: u16, id: u64, q: Request<'_>) -> Disp {
            self.write(fid, tag, 0, &q.encode(id).unwrap())
        }
        fn finish(&mut self) {
            let q = self.queued.take().unwrap();
            assert!(self
                .p
                .application
                .as_mut()
                .unwrap()
                .decision(
                    &mut self.owner,
                    q,
                    self.peer,
                    Ok(Reply {
                        op: q.op,
                        request: q.request,
                        focus: 2,
                        seat: 1,
                        foreground: q.foreground
                    }),
                    0
                )
                .unwrap());
            self.p.finish_pending().unwrap();
            assert_eq!(self.p.out_buf[4], p9::P9_RWRITE);
        }
        fn bind(&mut self) {
            assert!(matches!(
                self.request(
                    1,
                    20,
                    1,
                    Request::Bind {
                        session: 3,
                        context: 4,
                        epoch: 5
                    }
                ),
                Disp::Park(_)
            ));
            self.finish();
        }
        fn read(&mut self, fid: u32, off: u64, count: u32) -> Vec<u8> {
            let mut b = fid.to_le_bytes().to_vec();
            b.extend_from_slice(&off.to_le_bytes());
            b.extend_from_slice(&count.to_le_bytes());
            let Disp::Reply(n) = self.call(p9::P9_TREAD, 90, &b) else {
                panic!("expected read reply")
            };
            self.p.out_buf[..n].to_vec()
        }
    }
    #[test]
    fn fragmented_hello_and_positioned_reply_use_application_node() {
        let mut w = World::new();
        let q = Request::Hello.encode(1).unwrap();
        for (off, bytes) in q.chunks(3).enumerate() {
            assert!(matches!(
                w.write(1, 20 + off as u16, (off * 3) as u64, bytes),
                Disp::Reply(_)
            ));
            assert_eq!(w.p.out_buf[4], p9::P9_RWRITE);
        }
        let mut answer = Vec::new();
        loop {
            let b = w.read(1, answer.len() as u64, 7);
            assert_eq!(b[4], p9::P9_RREAD);
            if b.len() == 11 {
                break;
            }
            answer.extend_from_slice(&b[11..]);
        }
        assert_eq!(
            Response::decode(&answer, interaction_wire::Operation::Hello, 1),
            Ok(Response::Hello { session: 3 })
        );
        assert!(w.p.application.as_ref().unwrap().input_reserved() + 8192 <= 32768);
    }
    #[test]
    fn flush_exact_park_retires_publish_and_late_receipt_cannot_bind() {
        let mut w = World::new();
        let Disp::Park(ticket) = w.request(
            1,
            20,
            1,
            Request::Bind {
                session: 3,
                context: 4,
                epoch: 5,
            },
        ) else {
            panic!()
        };
        assert!(matches!(
            w.call(p9::P9_TFLUSH, 21, &19u16.to_le_bytes()),
            Disp::Reply(_)
        ));
        assert!(w.p.pending.is_some());
        assert!(
            matches!(w.call(p9::P9_TFLUSH,22,&20u16.to_le_bytes()),Disp::Cancel(t,_) if t==ticket)
        );
        assert!(w.p.pending.is_none());
        assert!(w.owner.busy());
        assert!(w.p.application.as_ref().unwrap().scope().is_none());
        let q = w.queued.take().unwrap();
        let done = w
            .owner
            .complete(
                q,
                Some(w.peer),
                Ok(Reply {
                    op: q.op,
                    request: q.request,
                    focus: 2,
                    seat: 1,
                    foreground: q.foreground,
                }),
                0,
            )
            .unwrap();
        assert!(!w.p.application.as_mut().unwrap().complete(done));
        assert!(!w.owner.busy());
        assert!(w.p.application.as_ref().unwrap().scope().is_none());
    }
    #[test]
    fn cross_fid_unbind_releases_park_without_waiting_for_check() {
        let mut w = World::new();
        w.bind();
        let scope = w.p.application.as_ref().unwrap().scope().unwrap();
        assert!(matches!(
            w.request(1, 30, 2, Request::Get { scope }),
            Disp::Park(_)
        ));
        assert!(matches!(
            w.request(2, 31, 1, Request::Unbind { scope }),
            Disp::Reply(_)
        ));
        assert_eq!(
            w.p.out_buf[4],
            p9::P9_RWRITE,
            "cross-fid Unbind refused behind pending CHECK"
        );
        w.p.finish_pending().unwrap();
        assert_eq!(w.p.out_buf[4], p9::P9_RLERROR);
        assert!(
            w.owner.busy(),
            "local cancellation stole ordered HIA receipt"
        );
    }
    #[test]
    fn clunk_pending_and_reused_numeric_fid_gets_new_incarnation() {
        let mut w = World::new();
        let old = w.p.fids[w.p.fid_find(1).unwrap()].unwrap();
        let Disp::Park(ticket) = w.request(
            1,
            20,
            1,
            Request::Bind {
                session: 3,
                context: 4,
                epoch: 5,
            },
        ) else {
            panic!()
        };
        assert!(
            matches!(w.call(p9::P9_TCLUNK,22,&1u32.to_le_bytes()),Disp::Cancel(t,_)if t==ticket)
        );
        assert!(w.p.fid_set(1, old.node, old.route));
        let mut b = 1u32.to_le_bytes().to_vec();
        b.extend_from_slice(&2u32.to_le_bytes());
        w.call(p9::P9_TLOPEN, 23, &b);
        assert!(w.p.fids[w.p.fid_find(1).unwrap()].unwrap().app_id > old.app_id);
    }
    #[test]
    fn unsupported_application_state_never_falls_through_to_media_read() {
        let mut w = World::new();
        let id = w.p.fids[w.p.fid_find(1).unwrap()].unwrap().app_id;
        w.p.application
            .as_mut()
            .unwrap()
            .clunk(id, &mut w.owner)
            .unwrap();
        let b = w.read(1, 0, 32);
        assert_eq!(b[4], p9::P9_RLERROR);
    }
}
