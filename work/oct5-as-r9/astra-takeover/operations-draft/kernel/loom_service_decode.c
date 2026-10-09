#include <thylacine/loom.h>
#include <thylacine/loom_service_decode.h>
#include <thylacine/errno.h>

static bool service_no_buffer(const struct loom_sqe *s) {
    return !s->len && !s->buf_idx_or_off && !s->_resv1[0];
}
static bool service_fixed_shape(const struct loom_sqe *s) {
    return s->buf_idx_or_off<LOOM_MAX_REG_BUFFERS &&
           s->_resv1[0]<=UINT64_MAX-s->len;
}
static bool service_destination_shape(const struct loom_sqe *s) {
    return s->offset<LOOM_SERVICE_SLOT_COUNT && s->_resv1[2] &&
           s->offset!=s->handle_idx;
}
int loom_private_sqe_shape(const struct loom_sqe *s, bool pools_enabled) {
    if (!s || s->_resv0 || s->handle_idx>=LOOM_SERVICE_SLOT_COUNT ||
        !s->_resv1[1]) return -T_E_INVAL;
    bool pooled=s->opcode==LOOM_OP_READ && (s->flags&LOOM_SQE_BUFFER_SELECT);
    u8 allowed=pooled ? LOOM_SQE_BUFFER_SELECT|LOOM_SQE_MULTISHOT : 0;
    if (s->flags&~allowed) return -T_E_INVAL;
    switch (s->opcode) {
    case LOOM_OP_SERVICE_CONNECT:
        if (!service_no_buffer(s) || !service_destination_shape(s)) return -T_E_INVAL;
        break;
    case LOOM_OP_WALK:
        if (!service_destination_shape(s) || s->len>255 ||
            (s->len ? !service_fixed_shape(s) : !service_no_buffer(s))) return -T_E_INVAL;
        break;
    case LOOM_OP_LOPEN:
        if (s->offset>2 || s->_resv1[2] || !service_no_buffer(s)) return -T_E_INVAL;
        break;
    case LOOM_OP_READ:
        if (pooled) {
            if (!pools_enabled) return -T_E_OPNOTSUPP;
            if (!s->len || s->buf_idx_or_off>=LOOM_SERVICE_SLOT_COUNT ||
                !s->_resv1[2] || s->_resv1[0]) return -T_E_INVAL;
            break;
        }
        // Fixed READ and WRITE have identical slice encodings.
        /* fall through */
    case LOOM_OP_WRITE:
        if (s->_resv1[2] || !service_fixed_shape(s)) return -T_E_INVAL;
        break;
    case LOOM_OP_CLUNK:
        if (s->offset || s->_resv1[2] || !service_no_buffer(s)) return -T_E_INVAL;
        break;
    default: return -T_E_OPNOTSUPP;
    }
    return 0;
}
int loom_private_walk_component(const u8 *name, u32 n) {
    if (!n) return name ? -T_E_INVAL : 0;
    if (!name || n>255 || (n==1 && name[0]=='.') ||
        (n==2 && name[0]=='.' && name[1]=='.')) return -T_E_INVAL;
    for (u32 i=0;i<n;++i) if (!name[i] || name[i]=='/') return -T_E_INVAL;
    return 0;
}
