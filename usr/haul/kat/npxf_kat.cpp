// Generate cross-implementation known-answer vectors for the npxf secure
// channel, for Thylacine's guest-side client (usr/haul) to be pinned against.
//
// THE POINT OF THE #include: `derive`, `absorb`, `transcript_start`,
// `confirm_tag` and `expand_label` live in channel.cpp's ANONYMOUS namespace,
// so a generator that linked against channel.o could not reach them and would
// have to RE-DERIVE the key schedule from the public primitives. That is
// exactly the failure this file must not have: a re-derivation is my reading of
// channel.cpp, so if I misread it, the generator and the Rust it pins would
// agree with each other and not with npxf. Including the translation unit makes
// the vectors come from npxf's REAL code.
//
// The path is resolved through -I, not relative to this file: npxf lives in its
// own tree, so `regen.sh` passes -I "$NPXF_ROOT" and this include names the
// source from there. The earlier `../src/channel.cpp` form silently pointed at
// usr/haul/src -- the Rust -- and made the generator unbuildable from the very
// location it was committed to.
#include "src/channel.cpp"

#include <sys/socket.h>

#include <cstdio>
#include <string>
#include <thread>

using namespace npxf;

static void emit(const char *name, const uint8_t *p, size_t n) {
    std::printf("%-12s = %s\n", name, to_hex({p, n}).c_str());
}

// Reads exactly n bytes off a socketpair end that is known to hold them.
static Bytes drain(int fd, size_t n) {
    Bytes b(n);
    if (!net::read_full(fd, b.data(), n)) { std::fprintf(stderr, "short read\n"); std::exit(1); }
    return b;
}

[[noreturn]] static void die(const char *what) {
    std::fprintf(stderr, "npxf_kat: %s\n", what);
    std::exit(1);
}

// The fixture names k_c2s the CLIENT's send key. That binding is not made in
// derive() -- it is made one level up, by client_handshake's `out.send = d.c2s`
// and server_handshake's mirror. So a direction swap THERE would leave every
// vector above byte-identical while inverting what they mean, and the Rust
// pinned against them would agree with the fixture and disagree with npxf.
//
// THE OBVIOUS CHECK IS VACUOUS, and this file shipped it for a day. Running
// client_handshake against server_handshake and asserting
// `ckeys.send == skeys.recv` proves the two halves agree WITH EACH OTHER -- and
// they still agree after a swap applied CONSISTENTLY to both, which is exactly
// the swap that breaks us against npxf's own client. A one-sided swap is caught,
// but a one-sided swap already breaks npxf against itself and npxf's selftest
// catches that. The check only appeared to discriminate because the sabotage
// that "verified" it was one-sided too.
//
// So the responder here uses the FIXED `se_sk`, not a random one. That is what
// makes the resulting keys COMPARABLE TO THE EMITTED LABELS: with both
// ephemerals known we can call npxf's own derive() from the server's side and
// assert `ckeys.send == d.c2s` -- the label itself, not a symmetry.
//
// What is re-implemented below is the flight SEQUENCING (read 40, write 64,
// read 32) and nothing else: every cryptographic step calls the same
// `transcript_start`/`absorb`/`derive`/`confirm_tag` out of the included
// translation unit that server_handshake calls. The sequencing is not the thing
// under test and has no room to be subtly wrong -- a mistake there fails the
// handshake outright.
static void kat_responder(int fd, std::span<const uint8_t> token, const uint8_t se_sk[32],
                          crypto::Key &send_out, uint8_t ce_pk_out[32], Derived &d_out,
                          std::string &err) {
    try {
        crypto::Key psk = derive_psk(token);

        uint8_t msg1[kHandshakeMsg1];
        if (!net::read_full(fd, msg1, sizeof msg1)) { err = "responder: no flight 1"; return; }
        std::memcpy(ce_pk_out, msg1 + 8, 32);
        crypto::Hash h1 = absorb(transcript_start(), msg1, sizeof msg1);

        uint8_t se_pk[32];
        crypto::x25519_base(se_sk, se_pk);
        crypto::Hash h2 = absorb(h1, se_pk, 32);

        derive(psk, se_sk, ce_pk_out, h2, d_out);

        uint8_t msg2[kHandshakeMsg2];
        std::memcpy(msg2, se_pk, 32);
        confirm_tag(d_out.cfm, "server", msg2 + 32);
        net::write_full(fd, msg2, sizeof msg2);

        uint8_t msg3[kHandshakeMsg3], want[32];
        if (!net::read_full(fd, msg3, sizeof msg3)) { err = "responder: no flight 3"; return; }
        confirm_tag(d_out.cfm, "client", want);
        if (!crypto::ct_eq(want, msg3, 32)) { err = "responder: client tag mismatch"; return; }

        send_out = d_out.s2c;   // what a server would send with
    } catch (const std::exception &e) {
        err = e.what();
    }
}

// Assert the fixture's LABEL, not a symmetry: `k_c2s` must be the key
// client_handshake assigns to `out.send`.
static void check_direction_binding(std::span<const uint8_t> token, const uint8_t se_sk[32]) {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) die("socketpair");

    // If the responder fails, the client sits in read_full with the other end
    // still open (both fds are main's), so there is no EOF to end it. Bounded
    // anyway: client_handshake's own net::set_timeout(fd, kHandshakeTimeoutMs)
    // caps that at 15 s and throws, which reaches `die`.
    SessionKeys ckeys;
    crypto::Key server_send{};
    uint8_t ce_pk[32] = {0};
    Derived d_srv;
    std::string err;
    std::thread srv([&] { kat_responder(sv[1], token, se_sk, server_send, ce_pk, d_srv, err); });
    try {
        client_handshake(sv[0], token, ckeys);
    } catch (const std::exception &e) {
        srv.join();
        die(e.what());
    }
    srv.join();
    if (!err.empty()) die(err.c_str());

    // THE ASSERTION THAT IS NOT A SYMMETRY. `d_srv` came from npxf's own
    // derive() with BOTH ephemerals known, so `d_srv.c2s` IS the value the
    // fixture emits as `k_c2s` for this pair. A swap inside client_handshake --
    // one-sided OR applied to both halves -- moves `ckeys.send` off it.
    if (ckeys.send != d_srv.c2s)
        die("client_handshake's send key is NOT c2s -- the fixture's k_c2s label is wrong");
    if (ckeys.recv != d_srv.s2c)
        die("client_handshake's recv key is NOT s2c -- the fixture's k_s2c label is wrong");
    if (server_send != d_srv.s2c)
        die("a server's send key is not s2c");
    if (ckeys.send == ckeys.recv) die("the two directions share a key");

    // And a record really crosses client -> server under it.
    SessionKeys skeys;
    skeys.send = d_srv.s2c;
    skeys.recv = d_srv.c2s;
    Channel c(sv[0], ckeys, kMaxRecordPayload);
    Channel s(sv[1], skeys, kMaxRecordPayload);
    const uint8_t probe[] = {'k', 'a', 't'};
    c.send(probe, sizeof probe);
    Bytes got;
    if (!s.recv(got) || got.size() != sizeof probe ||
        std::memcmp(got.data(), probe, sizeof probe) != 0)
        die("a record did not survive the client -> server direction");

    ::close(sv[0]);
    ::close(sv[1]);
}

int main() {
    // Fixed inputs. Nothing here is secret -- these are test vectors, and the
    // ephemerals are deliberately pinned so the whole schedule is deterministic.
    const std::string token = "thylacine-npxf-kat-token";

    uint8_t ce_sk[32], se_sk[32];
    for (int i = 0; i < 32; i++) ce_sk[i] = uint8_t(0x10 + i);
    for (int i = 0; i < 32; i++) se_sk[i] = uint8_t(0xA0 + i);

    // Before emitting anything: prove the fixture's direction claim against
    // npxf's real handshake, so a swap cannot ride out in a green-looking file.
    check_direction_binding(byte_span(std::string_view(token)), se_sk);

    std::printf("# npxf secure-channel known-answer vectors\n");
    std::printf("# Generated by usr/haul/kat/npxf_kat.cpp against npxf's own\n");
    std::printf("# channel.cpp -- see that file's header for why it #includes it.\n");
    std::printf("# Regenerate and diff with usr/haul/kat/regen.sh.\n");
    std::printf("# The generator also runs npxf's client and server handshakes\n");
    std::printf("# against each other and refuses to emit if c2s is not the\n");
    std::printf("# client's send key.\n");
    std::printf("# All values hex, lowercase, no separators.\n\n");

    std::printf("%-12s = %s\n", "token_ascii", token.c_str());
    emit("token", reinterpret_cast<const uint8_t *>(token.data()), token.size());

    crypto::Key psk = derive_psk(byte_span(std::string_view(token)));
    emit("psk", psk.data(), psk.size());

    uint8_t ce_pk[32], se_pk[32];
    crypto::x25519_base(ce_sk, ce_pk);
    crypto::x25519_base(se_sk, se_pk);
    emit("ce_sk", ce_sk, 32);
    emit("ce_pk", ce_pk, 32);
    emit("se_sk", se_sk, 32);
    emit("se_pk", se_pk, 32);

    // Flight 1, exactly as client_handshake builds it.
    uint8_t msg1[kHandshakeMsg1];
    std::memcpy(msg1, "NPXF", 4);
    msg1[4] = kProtocolVersion;
    msg1[5] = msg1[6] = msg1[7] = 0;
    std::memcpy(msg1 + 8, ce_pk, 32);
    emit("msg1", msg1, sizeof msg1);

    crypto::Hash h0 = transcript_start();
    crypto::Hash h1 = absorb(h0, msg1, sizeof msg1);
    crypto::Hash h2 = absorb(h1, se_pk, 32);
    emit("h0", h0.data(), h0.size());
    emit("h1", h1.data(), h1.size());
    emit("h2", h2.data(), h2.size());

    // The raw DH, and the proof both sides land on it.
    uint8_t dh_c[32], dh_s[32];
    if (!crypto::x25519(ce_sk, se_pk, dh_c)) { std::fprintf(stderr, "client dh failed\n"); return 1; }
    if (!crypto::x25519(se_sk, ce_pk, dh_s)) { std::fprintf(stderr, "server dh failed\n"); return 1; }
    if (std::memcmp(dh_c, dh_s, 32) != 0) { std::fprintf(stderr, "dh disagree\n"); return 1; }
    emit("dh", dh_c, 32);

    uint8_t prk[32];
    crypto::hkdf_extract(std::span<const uint8_t>(psk.data(), psk.size()),
                         std::span<const uint8_t>(dh_c, 32), prk);
    emit("prk", prk, 32);

    // npxf's OWN derive(), from the included translation unit.
    Derived d;
    derive(psk, ce_sk, se_pk, h2, d);
    emit("k_c2s", d.c2s.data(), d.c2s.size());
    emit("k_s2c", d.s2c.data(), d.s2c.size());
    emit("k_cfm", d.cfm.data(), d.cfm.size());

    uint8_t tag_server[32], tag_client[32];
    confirm_tag(d.cfm, "server", tag_server);
    confirm_tag(d.cfm, "client", tag_client);
    emit("tag_server", tag_server, 32);
    emit("tag_client", tag_client, 32);

    // Flights 2 and 3 as they appear on the wire.
    uint8_t msg2[kHandshakeMsg2];
    std::memcpy(msg2, se_pk, 32);
    std::memcpy(msg2 + 32, tag_server, 32);
    emit("msg2", msg2, sizeof msg2);
    emit("msg3", tag_client, 32);

    // Record layer. The plaintext is a real 9P Tversion so the fixture also
    // documents what actually rides this channel.
    const uint8_t tversion[] = {
        0x15, 0x00, 0x00, 0x00,                        // size[4] = 21, header included
        100,                                            // Tversion
        0xff, 0xff,                                     // tag = NOTAG
        0x00, 0x20, 0x00, 0x00,                         // msize = 8192
        0x08, 0x00,                                     // version[s]: 8 bytes follow
        '9', 'P', '2', '0', '0', '0', '.', 'L',
    };
    // Both halves of the frame must agree, or the fixture teaches the wrong
    // thing: the array is 21 bytes AND its own size[4] says so.
    static_assert(sizeof tversion == 21, "the fixture plaintext must be 21 bytes");
    if (ld32(tversion) != sizeof tversion) {
        std::fprintf(stderr, "fixture size[4] disagrees with its own length\n");
        return 1;
    }
    emit("record_pt", tversion, sizeof tversion);

    // Two records under the SAME key at counters 0 and 1. Emitting both is what
    // pins the per-direction counter: an implementation that reused nonce 0
    // would match record0 and fail record1.
    //
    // These come out of npxf's REAL Channel::send -- the length prefix, the AAD
    // choice, make_nonce and the counter increment are all its code, reached by
    // pointing a Channel at a socketpair and reading back what it wrote. The
    // previous shape re-assembled all four here, which is the same re-derivation
    // hazard the #include exists to avoid, one layer down.
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        std::perror("socketpair");
        return 1;
    }
    {
        SessionKeys keys;
        keys.send = d.c2s;
        keys.recv = d.s2c;
        Channel ch(sv[0], keys, kMaxRecordPayload);
        for (int i = 0; i < 2; i++) {
            ch.send(tversion, sizeof tversion);
            Bytes rec = drain(sv[1], 4 + sizeof tversion + crypto::kTagLen);
            char name[16];
            std::snprintf(name, sizeof name, "record_c2s%d", i);
            emit(name, rec.data(), rec.size());
        }
    }
    ::close(sv[0]);
    ::close(sv[1]);

    return 0;
}
