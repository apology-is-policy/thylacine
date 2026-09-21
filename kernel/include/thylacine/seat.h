// Trusted graphical seat ABI. Authority is boot-stamped, never a caller-supplied
// role or CAP_HW_CREATE. All state is serialized by g_proc_table_lock.
#ifndef THYLACINE_SEAT_H
#define THYLACINE_SEAT_H
#include <thylacine/types.h>
struct Proc;
#define SEAT_FRAME_MAX 512u
#define SEAT_NORMAL 0u
#define SEAT_QUIESCING 1u
#define SEAT_EXCLUSIVE 2u
#define SEAT_RESTORING 3u
#define SEAT_FAILED 4u
#define SEAT_STATUS 1u
#define SEAT_INPUT 2u
#define SEAT_ACK 3u
#define SEAT_FRAME 4u
#define SEAT_KEY 5u
#define SEAT_RESTORED 6u
#define SEAT_FAIL 7u
#define SEAT_QUERY 8u
#define SEAT_VISIBLE 9u
#define SEAT_MASK 10u
#define SEAT_GRANT 11u
#define SEAT_CLIENT 12u
// The reserved attention gesture, in evdev key codes: either Control, either
// Alt, and one of the keys below. Two final keys because Delete is absent
// from compact and laptop keyboards; both are scanned here, below every
// compositor, so neither can be intercepted or synthesized by a client.
#define SEAT_KEY_LEFTCTRL   29u
#define SEAT_KEY_RIGHTCTRL  97u
#define SEAT_KEY_LEFTALT    56u
#define SEAT_KEY_RIGHTALT  100u
#define SEAT_KEY_DELETE    111u
#define SEAT_KEY_F10        68u
// Fixed byte envelope. The kernel snapshots input before taking its lock and
// copies output after release. No userspace pointers are stored in seat state.
struct seat_message {
    u64 generation;
    u64 sequence;
    u32 phase;
    u32 length;
    u32 code;
    u32 value;
    u8 data[SEAT_FRAME_MAX];
};
_Static_assert(sizeof(struct seat_message) == 544, "seat ABI");
// Volatile stores also prevent secret scrubbing from being optimized away.
static inline void seat_zero(void *dst, u64 n) {
    volatile u8 *p = (volatile u8 *)dst;
    for (u64 i = 0; i < n; i++) p[i] = 0;
}
static inline void seat_copy(void *dst, const void *src, u64 n) {
    volatile u8 *d = (volatile u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < n; i++) d[i] = s[i];
}
int proc_set_seat_service(struct Proc *p);
int proc_set_seat_client(struct Proc *p);
bool proc_is_seat_service(struct Proc *p);
bool proc_is_seat_manager(struct Proc *p);
void proc_mark_seat_manager(struct Proc *p);
int proc_seat_op(struct Proc *p, u32 op, struct seat_message *msg);
#endif
