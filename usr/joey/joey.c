// /joey — first userspace process; the long-running init.
//
// At P5-spawn-wait, /joey demonstrates orchestration: spawn /hello,
// wait for it, report its status. The supervisor-loop extension (with
// stratumd-stub etc.) lands in subsequent chunks; the syscall surface
// (t_spawn + t_wait_pid) is the prerequisite that this chunk validates
// end-to-end in the production boot path.

#include <thyla/syscall.h>

static const char *itoa_dec(long v, char *buf, size_t buf_sz) {
    if (buf_sz == 0) return "";
    if (v == 0) {
        if (buf_sz < 2) return "";
        buf[0] = '0'; buf[1] = '\0';
        return buf;
    }
    int negative = 0;
    unsigned long u;
    if (v < 0) { negative = 1; u = (unsigned long)(-(v + 1)) + 1; }
    else       { u = (unsigned long)v; }
    char tmp[24];
    size_t n = 0;
    while (u && n < sizeof(tmp)) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
    size_t need = n + (negative ? 1 : 0) + 1;
    if (need > buf_sz) return "";
    size_t i = 0;
    if (negative) buf[i++] = '-';
    while (n > 0) buf[i++] = tmp[--n];
    buf[i] = '\0';
    return buf;
}

int main(void) {
    t_putstr("joey: hello from /joey (real userspace binary, loaded from ramfs)\n");

    const char hello_name[] = "hello";
    long pid = t_spawn(hello_name, sizeof(hello_name) - 1);
    if (pid <= 0) {
        t_putstr("joey: t_spawn(\"hello\") FAILED\n");
        return 1;
    }

    char buf[24];
    t_putstr("joey: spawned /hello pid=");
    t_putstr(itoa_dec(pid, buf, sizeof(buf)));
    t_putstr("\n");

    int status = -1;
    long reaped = t_wait_pid(&status);
    if (reaped != pid) {
        t_putstr("joey: t_wait_pid returned wrong pid=");
        t_putstr(itoa_dec(reaped, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }
    if (status != 0) {
        t_putstr("joey: /hello exited non-zero status=");
        t_putstr(itoa_dec(status, buf, sizeof(buf)));
        t_putstr("\n");
        return 1;
    }

    t_putstr("joey: /hello reaped status=0; orchestration verified\n");
    return 0;
}
