// /dev Dev tests (#57b).
//
// Covers registration, walks, the trivial leaves (null/zero/full/random/
// urandom), and -- the load-bearing one -- the I-27 gate-at-namespace-open:
// devdev.open enforces proc_is_console_attached for cons/consctl, so binding
// /dev/cons as a walkable path adds NO ungated console front door.
//
// The mount + cross (reuse-nc through clone_walk_zero) is covered by
// test_namespace_layout (the /dev step).

#include "test.h"

#include <thylacine/cons.h>                  // #94-B: cons_test_termios + CONS_ICANON
#include <thylacine/dev.h>
#include <thylacine/poll.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>               // #55: struct t_stat + T_S_IFCHR
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_devdev_bestiary_smoke(void);
void test_devdev_attach_returns_dir(void);
void test_devdev_walk_to_each_leaf(void);
void test_devdev_walk_unknown_misses(void);
void test_devdev_walk_pts_dir(void);
void test_devdev_trivial_leaves(void);
void test_devdev_cons_gate(void);
void test_devdev_consctl_renderer_mint(void);   // #55
void test_devdev_winsize_leaf(void);            // #55

// =============================================================================
// Helpers.
// =============================================================================

// Walk /dev/<name> (NULL nc = the legacy direct-call shape). Caller spoor_unref's
// the result. Does NOT open -- the gate test drives open() separately.
static struct Spoor *walk_to(const char *name) {
    struct Spoor *root = devdev.attach("");
    if (!root) return NULL;
    const char *names[1] = { name };
    struct Walkqid *wq = devdev.walk(root, NULL, names, 1);
    spoor_unref(root);
    if (!wq) return NULL;
    if (wq->nqid != 1) {
        spoor_unref(wq->spoor);
        walkqid_free(wq);
        return NULL;
    }
    struct Spoor *leaf = wq->spoor;
    walkqid_free(wq);
    return leaf;
}

// =============================================================================
// Tests.
// =============================================================================

void test_devdev_bestiary_smoke(void) {
    TEST_EXPECT_EQ(dev_lookup_by_dc('d'),     &devdev, "lookup 'd' = devdev");
    TEST_EXPECT_EQ(dev_lookup_by_name("dev"), &devdev, "lookup 'dev' = devdev");
    TEST_EXPECT_EQ(devdev.dc, 'd',                     "devdev.dc = 'd'");
}

void test_devdev_attach_returns_dir(void) {
    struct Spoor *c = devdev.attach("");
    TEST_ASSERT(c != NULL, "attach OK");
    TEST_EXPECT_EQ(c->qid.path, (u64)0, "root qid.path = 0");
    TEST_EXPECT_EQ(c->qid.type, QTDIR, "root QTDIR");
    spoor_unref(c);
}

void test_devdev_walk_to_each_leaf(void) {
    static const char *leaf_names[] = {
        "null", "zero", "full", "random", "urandom", "cons", "consctl",
    };
    for (size_t i = 0; i < sizeof(leaf_names) / sizeof(leaf_names[0]); i++) {
        struct Spoor *leaf = walk_to(leaf_names[i]);
        TEST_ASSERT(leaf != NULL, "walk to leaf succeeds");
        TEST_EXPECT_EQ(leaf->qid.type, QTFILE, "leaf is QTFILE");
        TEST_ASSERT(leaf->qid.path != 0, "leaf path != root");
        spoor_unref(leaf);
    }
}

void test_devdev_walk_unknown_misses(void) {
    struct Spoor *root = devdev.attach("");
    const char *names[1] = { "does-not-exist" };
    struct Walkqid *wq = devdev.walk(root, NULL, names, 1);
    TEST_ASSERT(wq != NULL, "walk allocates");
    TEST_EXPECT_EQ(wq->nqid, 0, "walk to unknown leaf misses");
    spoor_unref(wq->spoor);
    walkqid_free(wq);
    spoor_unref(root);
}

// /dev/pts: the synthetic mount-stub DIRECTORY child (PTY-2a-2). Walkable +
// QTDIR (the mount-point identity joey's t_mount("/dev/pts") keys on); EMPTY
// (no devdev children -- ptyfs's mounted tree provides ptmx/<n>); ".." climbs
// back to the root; read/write fail (a stub, not a data node).
void test_devdev_walk_pts_dir(void) {
    struct Spoor *pts = walk_to("pts");
    TEST_ASSERT(pts != NULL, "walk to /dev/pts succeeds");
    TEST_EXPECT_EQ(pts->qid.type, QTDIR, "/dev/pts is QTDIR");
    TEST_ASSERT(pts->qid.path != 0, "/dev/pts path != root");

    // Empty pre-mount: no child resolves from the pts dir here (ptmx lives in
    // the MOUNTED ptyfs tree, never in devdev).
    const char *names[1] = { "ptmx" };
    struct Walkqid *wq = devdev.walk(pts, NULL, names, 1);
    TEST_ASSERT(wq != NULL, "walk from pts allocates");
    TEST_EXPECT_EQ(wq->nqid, 0, "pts dir has no devdev children");
    spoor_unref(wq->spoor);
    walkqid_free(wq);

    // ".." climbs back to the /dev root.
    const char *up[1] = { ".." };
    struct Walkqid *wu = devdev.walk(pts, NULL, up, 1);
    TEST_ASSERT(wu != NULL && wu->nqid == 1, "pts .. walks");
    TEST_EXPECT_EQ(wu->spoor->qid.path, (u64)0, "pts .. = the /dev root");
    spoor_unref(wu->spoor);
    walkqid_free(wu);

    // A stub, not a data node: read/write fail (-1).
    char buf[8];
    TEST_EXPECT_EQ(devdev.read(pts, buf, 8, 0), (long)-1, "pts read fails");
    TEST_EXPECT_EQ(devdev.write(pts, "x", 1, 0), (long)-1, "pts write fails");

    spoor_unref(pts);
}

// The trivial leaves are world-rw + UNGATED: these opens succeed regardless of
// console-attach state (the test thread is not console-attached). That implicitly
// proves the gate is leaf-specific (cons/consctl only).
void test_devdev_trivial_leaves(void) {
    char buf[16];

    // /dev/null: read EOF; write consumed.
    struct Spoor *nul = walk_to("null");
    TEST_ASSERT(nul != NULL && devdev.open(nul, 0) != NULL, "open /dev/null (ungated)");
    TEST_EXPECT_EQ(devdev.read(nul, buf, 16, 0), (long)0, "/dev/null read EOF");
    TEST_EXPECT_EQ(devdev.write(nul, "abc", 3, 0), (long)3, "/dev/null consumes write");
    devdev.close(nul); spoor_unref(nul);

    // /dev/zero: read zero-fills; write consumed.
    struct Spoor *zer = walk_to("zero");
    TEST_ASSERT(zer != NULL && devdev.open(zer, 0) != NULL, "open /dev/zero");
    for (int i = 0; i < 16; i++) buf[i] = 0x5a;
    TEST_EXPECT_EQ(devdev.read(zer, buf, 16, 0), (long)16, "/dev/zero reads 16");
    {
        bool all_zero = true;
        for (int i = 0; i < 16; i++) if (buf[i] != 0) all_zero = false;
        TEST_ASSERT(all_zero, "/dev/zero zero-fills");
    }
    TEST_EXPECT_EQ(devdev.write(zer, "x", 1, 0), (long)1, "/dev/zero consumes write");
    devdev.close(zer); spoor_unref(zer);

    // /dev/full: read zero-fills; write FAILS (full disk).
    struct Spoor *ful = walk_to("full");
    TEST_ASSERT(ful != NULL && devdev.open(ful, 0) != NULL, "open /dev/full");
    TEST_EXPECT_EQ(devdev.read(ful, buf, 8, 0), (long)8, "/dev/full reads 8");
    TEST_EXPECT_EQ(devdev.write(ful, "x", 1, 0), (long)-1, "/dev/full write fails");
    devdev.close(ful); spoor_unref(ful);

    // /dev/random: CSPRNG (two reads differ); write consumed.
    struct Spoor *rnd = walk_to("random");
    TEST_ASSERT(rnd != NULL && devdev.open(rnd, 0) != NULL, "open /dev/random");
    char a[16], b[16];
    long ra = devdev.read(rnd, a, 16, 0);
    long rb = devdev.read(rnd, b, 16, 0);
    TEST_ASSERT(ra == 16 && rb == 16, "/dev/random reads 16");
    {
        bool differ = false;
        for (int i = 0; i < 16; i++) if (a[i] != b[i]) differ = true;
        TEST_ASSERT(differ, "/dev/random two reads differ (CSPRNG)");
    }
    TEST_EXPECT_EQ(devdev.write(rnd, "x", 1, 0), (long)1, "/dev/random consumes write");
    devdev.close(rnd); spoor_unref(rnd);

    // /dev/urandom: alias of random.
    struct Spoor *urn = walk_to("urandom");
    TEST_ASSERT(urn != NULL && devdev.open(urn, 0) != NULL, "open /dev/urandom");
    TEST_EXPECT_EQ(devdev.read(urn, buf, 8, 0), (long)8, "/dev/urandom reads 8");
    devdev.close(urn); spoor_unref(urn);
}

// The I-27 gate-at-namespace-open: devdev.open of cons/consctl requires
// proc_is_console_attached -- identical to SYS_CONSOLE_OPEN. Without attach, the
// name resolves (walk) but open FAILS, so binding /dev/cons adds no ungated
// console front door. Results are gathered under controlled attach state, then
// the state is RESTORED before the asserts (a failing assert must never leave the
// test thread wrongly console-attached) -- the devctl kernel-base temp-elevate
// pattern.
void test_devdev_cons_gate(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t != NULL && t->proc != NULL, "test thread has a proc");
    struct Proc *p = t->proc;
    bool saved = proc_is_console_attached(p);

    // DENY: not console-attached -> open of cons/consctl fails.
    proc_revoke_console_attached(p);
    struct Spoor *cons_d = walk_to("cons");
    bool cons_deny = (cons_d != NULL) && (devdev.open(cons_d, 0) == NULL);
    struct Spoor *cc_d = walk_to("consctl");
    bool cc_deny = (cc_d != NULL) && (devdev.open(cc_d, 0) == NULL);

    // cons I/O gate (closes the O_PATH-skips-open bypass): read/write/poll
    // directly on an UNOPENED cons spoor without console-attach must fail --
    // T_OPATH skips dev->open, so cons re-checks at the I/O sites. cons_d is
    // walked but never opened (the O_PATH shape); without the I/O gate devdev_read
    // would reach cons_input_read and steal console input.
    char gbuf[8];
    bool cons_read_deny  = (cons_d != NULL) && (devdev.read(cons_d, gbuf, sizeof(gbuf), 0) < 0);
    bool cons_write_deny = (cons_d != NULL) && (devdev.write(cons_d, "x", 1, 0) < 0);
    bool cons_poll_deny  = (cons_d != NULL) && (devdev.poll(cons_d, POLLIN, NULL) == POLLNVAL);

    // #94-B: consctl (the CONTROL leaf) is NOT I/O-re-gated -- a delegated holder
    // of an INHERITED consctl fd (login/ut) must set the line discipline without
    // being console-attached. So a NON-attached caller's consctl I/O SUCCEEDS:
    // read renders the mode line (> 0); write applies (returns n) + takes effect;
    // poll is always-ready (never POLLNVAL). The open-mint gate (still enforced --
    // cc_deny above) + #81 CWALKONLY are the protections. The write mutates the
    // global termios, so snapshot + restore it around the probe.
    bool cc_read_allow = (cc_d != NULL) && (devdev.read(cc_d, gbuf, sizeof(gbuf), 0) > 0);
    u32  saved_tio = cons_test_termios();
    bool cc_write_took = false;
    if (cc_d != NULL) {
        // "+icanon" sets a flag the boot default (ISIG only) lacks -- reading it
        // back via the kernel termios proves the non-attached write reached
        // cons_set_mode_cmd (not merely returned a count).
        bool w = (devdev.write(cc_d, "+icanon", 7, 0) == 7);
        cc_write_took = w && ((cons_test_termios() & CONS_ICANON) != 0u);
    }
    cons_test_set_termios(saved_tio);   // restore: a non-attached write mutated it
    bool cc_poll_allow_un = (cc_d != NULL) && (devdev.poll(cc_d, POLLIN, NULL) != POLLNVAL);

    // ALLOW: console-attached -> open succeeds.
    proc_mark_console_attached(p);
    struct Spoor *cons_a = walk_to("cons");
    struct Spoor *cons_a_open = cons_a ? devdev.open(cons_a, 0) : NULL;
    bool cons_allow = (cons_a_open != NULL);

    // Attached: consctl read renders the mode line (> 0) and poll passes (consctl
    // is always-ready, never POLLNVAL). Read-only here -- a consctl WRITE would
    // mutate the global termios; the parse itself is cons.consctl_parse.
    struct Spoor *cc_a = walk_to("consctl");
    bool cc_allow_read = (cc_a != NULL) && (devdev.read(cc_a, gbuf, sizeof(gbuf), 0) > 0);
    bool cc_poll_allow = (cc_a != NULL) && (devdev.poll(cc_a, POLLIN, NULL) != POLLNVAL);

    // Restore BEFORE asserting (so a failing assert cannot strand the attach bit).
    if (cons_a_open) devdev.close(cons_a);
    if (saved) proc_mark_console_attached(p);
    else       proc_revoke_console_attached(p);

    TEST_ASSERT(cons_d != NULL, "walk /dev/cons resolves the name");
    TEST_ASSERT(cons_deny, "I-27: non-attached open of /dev/cons DENIED");
    TEST_ASSERT(cc_d != NULL, "walk /dev/consctl resolves the name");
    TEST_ASSERT(cc_deny, "I-27: non-attached open of /dev/consctl DENIED");
    TEST_ASSERT(cons_read_deny, "I-27: non-attached read of /dev/cons DENIED (O_PATH bypass)");
    TEST_ASSERT(cons_write_deny, "I-27: non-attached write of /dev/cons DENIED (O_PATH bypass)");
    TEST_ASSERT(cons_poll_deny, "I-27: non-attached poll of /dev/cons -> POLLNVAL");
    TEST_ASSERT(cc_read_allow, "#94-B: non-attached read of /dev/consctl ALLOWED (renders mode line)");
    TEST_ASSERT(cc_write_took, "#94-B: non-attached write of /dev/consctl ALLOWED + applied (inherited-fd capability)");
    TEST_ASSERT(cc_poll_allow_un, "#94-B: non-attached poll of /dev/consctl always-ready (not POLLNVAL)");
    TEST_ASSERT(cons_a != NULL, "walk /dev/cons (attached)");
    TEST_ASSERT(cons_allow, "console-attached open of /dev/cons ALLOWED");
    TEST_ASSERT(cc_allow_read, "console-attached read of /dev/consctl renders the mode line");
    TEST_ASSERT(cc_poll_allow, "console-attached poll of /dev/consctl passes (always ready)");

    if (cons_d) spoor_unref(cons_d);
    if (cc_d) spoor_unref(cc_d);
    if (cons_a) spoor_unref(cons_a);
    if (cc_a) spoor_unref(cc_a);
}

// =============================================================================
// #55: the consctl MINT-gate widening (ARCH 23.5.3) -- consctl becomes
// mintable by attached OR renderer (the winsize writer self-serves by name);
// cons (the DATA leaf) stays attach-only (the widening must STOP at consctl:
// reading console INPUT is exactly what the renderer role must not confer).
void test_devdev_consctl_renderer_mint(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    bool saved_attach = proc_is_console_attached(t->proc);
    proc_revoke_console_attached(t->proc);
    proc_test_clear_console_renderer();          // known state

    struct Spoor *cc   = walk_to("consctl");
    struct Spoor *cons = walk_to("cons");
    TEST_ASSERT(cc != NULL && cons != NULL, "walk consctl + cons");

    // Neither role: both DENIED (the pre-#55 posture for a plain Proc holds).
    TEST_ASSERT(devdev.open(cc, 0) == NULL,   "plain Proc: consctl mint DENIED");
    TEST_ASSERT(devdev.open(cons, 0) == NULL, "plain Proc: cons mint DENIED");

    // Renderer role: consctl mints; cons STILL denied (the widening's bound).
    TEST_EXPECT_EQ(proc_set_console_renderer(t->proc), 0, "renderer role claimed");
    struct Spoor *ccopen = devdev.open(cc, 1);
    TEST_ASSERT(ccopen != NULL, "renderer: consctl mint ALLOWED (#55)");
    TEST_ASSERT(devdev.open(cons, 0) == NULL, "renderer: cons mint STILL DENIED");

    // The minted fd drives the winsize verb end-to-end (the aurora path).
    cons_test_reset();
    TEST_EXPECT_EQ(devdev.write(ccopen, "winsize 128 36", 14, 0), 14L,
                   "renderer writes winsize via the minted consctl");
    u16 wc = 0, wr = 0;
    cons_winsize_get(&wc, &wr);
    TEST_ASSERT(wc == 128 && wr == 36, "winsize applied 128x36");

    // #55 audit F2: the renderer-minted consctl is WINSIZE-ONLY -- a termios
    // flag token rejects the whole write (no global-termios flip -> no
    // serial-input ECHO-off mask defeat). The minted Spoor carries
    // CCONSWINSZONLY; an attached-minted consctl (login/ut) does NOT and keeps
    // full flags (covered by test_devdev_cons_gate's cc_write_took leg).
    TEST_ASSERT((ccopen->flag & CCONSWINSZONLY) != 0u,
                "renderer-minted consctl tagged CCONSWINSZONLY");
    u32 tio_before = cons_test_termios();
    TEST_EXPECT_EQ(devdev.write(ccopen, "+echo", 5, 0), -1L,
                   "renderer consctl REJECTS a flag token (F2)");
    TEST_EXPECT_EQ((long)cons_test_termios(), (long)tio_before,
                   "the rejected flag write left the termios unchanged");
    TEST_EXPECT_EQ(devdev.write(ccopen, "+icanon winsize 80 24", 21, 0), -1L,
                   "a flag mixed with winsize rejects the whole write");
    cons_winsize_get(&wc, &wr);
    TEST_ASSERT(wc == 128 && wr == 36, "the rejected mixed write left winsize unchanged");

    devdev.close(ccopen);
    proc_test_clear_console_renderer();
    if (saved_attach) proc_mark_console_attached(t->proc);
    spoor_unref(cc);
    spoor_unref(cons);
    cons_test_reset();
}

// #55: the /dev/winsize leaf -- UNGATED (the trivial-leaf class: geometry is
// not sensitive), read-only, renders the standalone `winsize <cols> <rows>\n`
// line; stat_native stays cons-scoped (the leaf itself is statless).
void test_devdev_winsize_leaf(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    bool saved_attach = proc_is_console_attached(t->proc);
    proc_revoke_console_attached(t->proc);
    proc_test_clear_console_renderer();
    cons_test_reset();                            // winsize -> unset (0x0)

    struct Spoor *ws = walk_to("winsize");
    TEST_ASSERT(ws != NULL, "walk /dev/winsize resolves the name");

    // Unattached, non-renderer: mint + read both succeed (ungated).
    struct Spoor *wsopen = devdev.open(ws, 0);
    TEST_ASSERT(wsopen != NULL, "unattached open of /dev/winsize ALLOWED");
    char buf[24];
    long n = devdev.read(wsopen, buf, (long)sizeof(buf), 0);
    const char *unset = "winsize 0 0\n";
    bool ok = (n == 12);
    for (long i = 0; ok && i < n; i++) if (buf[i] != unset[i]) ok = false;
    TEST_ASSERT(ok, "unset serial posture reads winsize 0 0");

    // Offset semantics (the consctl idiom): a mid-line read + EOF past end.
    TEST_EXPECT_EQ(devdev.read(wsopen, buf, 4, 8), 4L, "offset read");
    TEST_ASSERT(buf[0] == '0' && buf[1] == ' ' && buf[2] == '0' && buf[3] == '\n',
                "offset 8 reads the tail");
    TEST_EXPECT_EQ(devdev.read(wsopen, buf, 4, 100), 0L, "past-end read -> EOF");

    // Read-only: writes fail.
    TEST_EXPECT_EQ(devdev.write(wsopen, "winsize 1 1", 11, 0), -1L,
                   "/dev/winsize write -> -1 (the writer is the consctl verb)");

    // A live value flows through (set via the production verb).
    TEST_EXPECT_EQ(cons_set_mode_cmd("winsize 132 50", 14, true), 14L, "set 132x50");
    n = devdev.read(wsopen, buf, (long)sizeof(buf), 0);
    const char *set = "winsize 132 50\n";
    ok = (n == 15);
    for (long i = 0; ok && i < n; i++) if (buf[i] != set[i]) ok = false;
    TEST_ASSERT(ok, "leaf reads winsize 132 50 after the verb");

    // stat_native is cons-scoped: the cons leaf fills; winsize stays statless.
    struct t_stat st;
    TEST_EXPECT_EQ((long)devdev.stat_native(wsopen, &st), -1L,
                   "winsize leaf is statless (the contract is cons-scoped)");
    struct Spoor *cons = walk_to("cons");
    TEST_ASSERT(cons != NULL, "walk cons");
    TEST_EXPECT_EQ((long)devdev.stat_native(cons, &st), 0L, "cons leaf stat fills");
    TEST_ASSERT((st.mode & T_S_IFMT) == T_S_IFCHR &&
                (st.qid_path & CONS_STAT_QID_FLAG) != 0u,
                "cons leaf reports the S_IFCHR + bit-41 contract");

    devdev.close(wsopen);
    spoor_unref(ws);
    spoor_unref(cons);
    if (saved_attach) proc_mark_console_attached(t->proc);
    cons_test_reset();
}

// G-4: the renderer pair /dev/consdrain + /dev/consfeed (TAPESTRY.md section
// 18.12 R2-F6 / F8). Gated at open AND re-gated at I/O + poll on
// proc_is_console_renderer -- the third console role. This test claims the
// role on the live test Proc via the production claim (proc_set_console_-
// renderer), exercises both leaves end-to-end (feed -> line discipline ->
// input ring; output -> drain), then releases everything.
// =============================================================================
void test_devdev_renderer_gate(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    cons_test_reset();
    cons_test_echo_capture(true);
    proc_test_clear_console_renderer();          // known state

    struct Spoor *drain = walk_to("consdrain");
    struct Spoor *feed  = walk_to("consfeed");
    TEST_ASSERT(drain != NULL, "walk /dev/consdrain resolves the name");
    TEST_ASSERT(feed  != NULL, "walk /dev/consfeed resolves the name");

    // DENY: not the renderer -> open of either leaf fails (F8: no ungated
    // reader of all console output; no ungated input injector).
    TEST_ASSERT(devdev.open(drain, 0) == NULL, "non-renderer open of consdrain DENIED");
    TEST_ASSERT(devdev.open(feed, 1) == NULL,  "non-renderer open of consfeed DENIED");

    // DENY at I/O too (the O_PATH-shaped bypass: dev->open skipped): direct
    // read/write/poll on unopened spoors without the role must fail.
    u8 b[4];
    TEST_EXPECT_EQ(devdev.read(drain, b, 4, 0), -1L, "non-renderer drain read DENIED");
    TEST_EXPECT_EQ(devdev.write(feed, "x", 1, 0), -1L, "non-renderer feed write DENIED");
    TEST_ASSERT(devdev.poll(drain, POLLIN, NULL) == POLLNVAL,
                "non-renderer drain poll -> POLLNVAL");
    TEST_ASSERT(devdev.poll(feed, POLLOUT, NULL) == POLLNVAL,
                "non-renderer feed poll -> POLLNVAL");

    // Claim the role (the production single-holder claim).
    TEST_EXPECT_EQ(proc_set_console_renderer(t->proc), 0, "renderer role claimed");
    TEST_ASSERT(proc_is_console_renderer(t->proc), "role query true for the holder");

    // ALLOW: the renderer opens the drain (arms the tap) + the feed.
    struct Spoor *dopen = devdev.open(drain, 0);
    TEST_ASSERT(dopen != NULL, "renderer open of consdrain ALLOWED (arms)");
    struct Spoor *fopen = devdev.open(feed, 1);
    TEST_ASSERT(fopen != NULL, "renderer open of consfeed ALLOWED");

    // The pair end-to-end: raw mode so a fed byte goes straight to the input
    // ring; program output lands in the drain.
    cons_test_set_termios(0u);
    TEST_EXPECT_EQ(devdev.write(fopen, "q", 1, 0), 1L, "feed write accepted");
    u8 in[4];
    TEST_EXPECT_EQ(cons_input_read(in, sizeof(in)), 1L, "fed byte reached the input ring");
    TEST_ASSERT(in[0] == 'q', "fed byte is q");

    TEST_EXPECT_EQ(cons_output_write("Z", 1), 1L, "output while armed");
    TEST_EXPECT_EQ(devdev.read(dopen, b, sizeof(b), 0), 1L, "drain read via the leaf");
    TEST_ASSERT(b[0] == 'Z', "drained byte is Z");

    // Directionality: drain writes / feed reads fail.
    TEST_EXPECT_EQ(devdev.write(dopen, "x", 1, 0), -1L, "consdrain is read-only");
    TEST_EXPECT_EQ(devdev.read(fopen, b, 4, 0), -1L, "consfeed is write-only");

    // The role revoked mid-hold (the holder died / a stale fd in a child):
    // the I/O re-gate refuses even on the still-open spoors.
    proc_test_clear_console_renderer();
    TEST_EXPECT_EQ(devdev.read(dopen, b, 4, 0), -1L, "revoked: drain read DENIED");
    TEST_EXPECT_EQ(devdev.write(fopen, "x", 1, 0), -1L, "revoked: feed write DENIED");

    // Close the opened pair (the drain close disarms via the COPEN hook).
    devdev.close(dopen);
    devdev.close(fopen);
    TEST_EXPECT_EQ(cons_drain_open(), 0, "drain disarmed at close (re-arm succeeds)");
    cons_drain_close();

    spoor_unref(drain);
    spoor_unref(feed);
    cons_test_echo_capture(false);
    cons_test_reset();
}
