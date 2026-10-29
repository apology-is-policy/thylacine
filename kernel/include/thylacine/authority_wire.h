// UA-0 reservations only. All multibyte fields are little-endian byte arrays.
// Never cast untrusted bytes to native integer structs. Rust mirror:
// usr/lib/corvus-authority/src/abi.rs. Contract: USER-AUTHORITY-ABI.md.
#ifndef THYLACINE_AUTHORITY_WIRE_H
#define THYLACINE_AUTHORITY_WIRE_H
#define UA_AUTHORITY_VERSION 1u
#define UA_AUTHORITY_QUERY 21u
#define UA_AUTHORITY_PREPARE 22u
#define UA_AUTHORITY_REQUEST 23u
#define UA_AUTHORITY_STATUS 24u
#define UA_AUTHORITY_CANCEL 25u
#define UA_MANDATE_MAGIC 0x4d54444du
#define UA_MANDATE_VERSION 1u
#define UA_MANDATE_HEADER_LEN 96u
#define UA_ENVELOPE_HEADER_LEN 32u
#define UA_MANDATE_MAX_LEN 896u
#define UA_KIND_USE 1u
#define UA_KIND_ACTIVATE 2u
#define UA_KIND_ADMIN 3u
#define UA_AUTH_SESSION 0u
#define UA_AUTH_DISTINCT_KEY 1u
#define UA_AUTH_FOUNDING 2u
#define UA_TERM_UNTIL_REVOKED 0u
#define UA_TERM_UNTIL_UTC 1u
#define UA_STATE_LIVE 1u
#define UA_STATE_REVOKING 2u
#define UA_STATE_REVOKED 3u
#define UA_MAX_MUTATION_PAYLOAD 4096u
#define UA_MAX_RESPONSE_PAGE 8192u
#define UA_MAX_PAGE_RECORDS 64u

struct UaMandateHeader {
    unsigned char magic[4];
    unsigned char version[2];
    unsigned char reserved[2];
    unsigned char total_len[4];
    unsigned char kind[1];
    unsigned char authentication[1];
    unsigned char state[1];
    unsigned char term_kind[1];
    unsigned char id[8];
    unsigned char revision[8];
    unsigned char subject[4];
    unsigned char issuer[4];
    unsigned char domain[8];
    unsigned char domain_generation[8];
    unsigned char actions[8];
    unsigned char term_end[8];
    unsigned char transaction[16];
    unsigned char subject_count[2];
    unsigned char resource_count[2];
    unsigned char support_count[2];
    unsigned char envelope_present[1];
    unsigned char reserved_tail[1];
};
_Static_assert(sizeof(struct UaMandateHeader) == 96, "MandateHeader size");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, magic) == 0, "MandateHeader.magic offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, version) == 4, "MandateHeader.version offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, reserved) == 6, "MandateHeader.reserved offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, total_len) == 8, "MandateHeader.total_len offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, kind) == 12, "MandateHeader.kind offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, authentication) == 13, "MandateHeader.authentication offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, state) == 14, "MandateHeader.state offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, term_kind) == 15, "MandateHeader.term_kind offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, id) == 16, "MandateHeader.id offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, revision) == 24, "MandateHeader.revision offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, subject) == 32, "MandateHeader.subject offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, issuer) == 36, "MandateHeader.issuer offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, domain) == 40, "MandateHeader.domain offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, domain_generation) == 48, "MandateHeader.domain_generation offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, actions) == 56, "MandateHeader.actions offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, term_end) == 64, "MandateHeader.term_end offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, transaction) == 72, "MandateHeader.transaction offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, subject_count) == 88, "MandateHeader.subject_count offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, resource_count) == 90, "MandateHeader.resource_count offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, support_count) == 92, "MandateHeader.support_count offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, envelope_present) == 94, "MandateHeader.envelope_present offset");
_Static_assert(__builtin_offsetof(struct UaMandateHeader, reserved_tail) == 95, "MandateHeader.reserved_tail offset");

struct UaEnvelopeHeader {
    unsigned char domain[8];
    unsigned char actions[8];
    unsigned char term_end[8];
    unsigned char kinds[1];
    unsigned char auth_floor[1];
    unsigned char delegation_depth[1];
    unsigned char term_kind[1];
    unsigned char subject_count[2];
    unsigned char resource_count[2];
};
_Static_assert(sizeof(struct UaEnvelopeHeader) == 32, "EnvelopeHeader size");
_Static_assert(__builtin_offsetof(struct UaEnvelopeHeader, domain) == 0, "EnvelopeHeader.domain offset");
_Static_assert(__builtin_offsetof(struct UaEnvelopeHeader, actions) == 8, "EnvelopeHeader.actions offset");
_Static_assert(__builtin_offsetof(struct UaEnvelopeHeader, term_end) == 16, "EnvelopeHeader.term_end offset");
_Static_assert(__builtin_offsetof(struct UaEnvelopeHeader, kinds) == 24, "EnvelopeHeader.kinds offset");
_Static_assert(__builtin_offsetof(struct UaEnvelopeHeader, auth_floor) == 25, "EnvelopeHeader.auth_floor offset");
_Static_assert(__builtin_offsetof(struct UaEnvelopeHeader, delegation_depth) == 26, "EnvelopeHeader.delegation_depth offset");
_Static_assert(__builtin_offsetof(struct UaEnvelopeHeader, term_kind) == 27, "EnvelopeHeader.term_kind offset");
_Static_assert(__builtin_offsetof(struct UaEnvelopeHeader, subject_count) == 28, "EnvelopeHeader.subject_count offset");
_Static_assert(__builtin_offsetof(struct UaEnvelopeHeader, resource_count) == 30, "EnvelopeHeader.resource_count offset");

#define UA_ACTION_ENROLL 0x1ull
#define UA_ACTION_PROFILE 0x2ull
#define UA_ACTION_SUSPEND 0x4ull
#define UA_ACTION_RESUME 0x8ull
#define UA_ACTION_RETIRE 0x10ull
#define UA_ACTION_GROUP_CREATE 0x20ull
#define UA_ACTION_GROUP_MEMBERSHIP 0x40ull
#define UA_ACTION_GRANT 0x80ull
#define UA_ACTION_REVOKE 0x100ull
#define UA_ACTION_DELEGATE 0x200ull
#define UA_ACTION_CLEARANCE_ENROLL 0x400ull
#define UA_ACTION_KEY_RESET 0x800ull
#define UA_ACTION_ROTATE_DOMAIN 0x1000ull
#define UA_ACTION_FLOOR_DEFINE 0x2000ull
#define UA_ACTION_AUDIT_READ 0x4000ull
#define UA_ACTION_FS_READ 0x100000000ull
#define UA_ACTION_FS_WRITE 0x200000000ull
#define UA_ACTION_FS_CHOWN 0x400000000ull
#define UA_ACTION_NET_CONNECT 0x800000000ull
#define UA_ACTION_NET_LISTEN 0x1000000000ull
#define UA_ACTION_SIGNAL 0x2000000000ull
#define UA_ACTION_POST_SERVICE 0x4000000000ull

#endif
