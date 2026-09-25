// libdlprobe.so -- the object /pouch-hello-dlopen loads (B-1d; ARCHITECTURE.md
// sec 6.5 "Dynamic loading"). Linked -shared with -z separate-loadable-segments,
// so its four PT_LOADs drive every map shape musl's loader asks of the kernel:
// the read-only head is the whole-span map, the text a FIXED R|X window over
// it, and the two writable segments FIXED eager copies, the second ending in a
// bss tail that runs past the file's last page. Each export below is read
// through one of those shapes; the host checks the layout before it trusts a
// leg to have driven one.

#include <string.h>

// .rodata, in the read-only head.
static const char greeting[] = "libdlprobe: loaded";

// .data.rel.ro: pointers the loader relocates and then protects (RELRO).
const char *const dlprobe_table[2] = { greeting, greeting + 11 };

// .data: file bytes the eager copy must carry.
int dlprobe_seed = 0x5eed1d;

// .bss: three pages past the data, so the loader's anonymous tail maps some.
unsigned char dlprobe_bss[3 * 4096];

int dlprobe_inited;

__attribute__((constructor)) static void dlprobe_init(void) {
    dlprobe_inited = 1;
}

const char *dlprobe_greeting(void) {
    return greeting;
}

int dlprobe_add(int a, int b) {
    return a + b;
}

// A call into libc.so through this object's own PLT.
unsigned long dlprobe_len(const char *s) {
    return (unsigned long)strlen(s);
}

// 1 iff every bss byte reads zero; then marks the last one, so a second call
// proves the tail took the write.
int dlprobe_bss_zero(void) {
    for (unsigned long i = 0; i < sizeof dlprobe_bss; i++)
        if (dlprobe_bss[i]) return 0;
    dlprobe_bss[sizeof dlprobe_bss - 1] = 0x5a;
    return 1;
}
