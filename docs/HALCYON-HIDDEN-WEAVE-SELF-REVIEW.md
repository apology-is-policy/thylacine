# Hidden pixel storage: single-agent review, October 4

Binding authority: operator option1, scripture cfa478824, model-first3a5c9a0e8.
Not an independent adversarial audit. No Main landing or broad qualification.

Reviewed boundaries:
- Comp owns placement visibility and backend-bound checks. No client assertion,
  focus surrogate or missed FRAME authorizes retirement. Offers wait for unbind.
- Opt-in scoped to owned content ctl; incompatible roles/GL refused. Legacy
  surfaces retain old resize/present behavior. Extra fid generation fits160bytes.
- Fresh pixel generation reserved before resume allocation; failed allocation
  aborts only its token. Suspend invalidates fid admission before release_gen.
- release_gen uses existing unshare and Lictor retirement; client mappings and
  device pins survive until their independent holders retire. No force-free.
- Lib closes old mapping only after accepted suspend; no borrow survives this
  single renderer boundary. Fresh weave/present fids are installed together;
  partial setup closes new fids and aborts the unpresented generation.
- Present verifies complete unheld first frame. Failed first present clears
  shown_slot; publication/direct scanout remains gated. Later CONFIGURE is
  re-offered after full frame if a resume temporarily rejected resize.
- Halcyon retains dirty/model/job state without pixel calls or idle spin while
  dormant; blind text/pointer clicks are suppressed until repaint. Existing
  independent SAK cancellation owners are unchanged. Failure notices are bounded.
- Actual source tests cover stale token/generation, unbind, first full frame,
  failure retry, wrap, legacy and no-flood. Eleven guard removals fail intended
  tests. Formal storage clean287/7namedmutants; present6clean/10namedmutants.

Native observations:
- Full-width16PTY/14controller pressure+physicalF10:107.63s, all14 reconnect;
  resumed1272x701 at1280x800, no Map refusal. CPU1 boot1830/1830.
- Protocol36.67s: stale visibility/fid refusal, full unheld first frame, abort
  token exactness, no idle retry, successful later reveal, same semantic leaf,
  legacy sibling; raw original map pinned across retirement and nonaliasing.
- Actual shared-map pressure:22retained generations, subsequent map refused,
  abort without semantic destroy, release old maps, successful resume/present,
  shell health and logout. No kernel quota or fault-injection modification.
- Hidden output/resize/close60.29s: inspected five captures; output from sleep3
  appears after tab reveal and survives half-width resize/revisit; shell responds.
- Initial protocol fixtures failed because non-SQPOLL polling did not progress
  parked reads, then because a second fid cannot reclaim a consume-once share.
  Both failures retained; corrected fixtures use SQPOLL and raw original mapping.
  Geometry refusal does not promise EBADF; present does. No product fix needed.

Remaining before full qualification:
- Legacy media/manual graphical regression passed71.60s. Inline View, pane/zoom
  Gallery, physicalSAK return, manual catalogue and live Signal theme inspected.
- No freshly completed broad SMP/sanitizer matrix, Pi or minimum display claim.
- Visible failure notice exists, but no screenshot of a real Halcyon failed
  reveal yet. Raw protocol mapping failure/recovery is measured separately.
- Initial mapping denial remains generic Map, no errno-specific ENOMEM claim.
- Clipboard HI1-R24 demand ledger/multiple-session/partial-output tests and
  application/modal clients remain outstanding; this change does not activate it.
