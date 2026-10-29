/* Independent C construction of every HIN1 body. The frozen vectors are shared
 * with Rust; neither test derives its expected bytes from the other encoder. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "halcyon_interaction.h"

static void u16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void u32(uint8_t *p, uint32_t v) { for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static void u64(uint8_t *p, uint64_t v) { for(unsigned i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static void scope(uint8_t *b) {
    u64(b+HIN_SCOPE_SESSION,1); u64(b+HIN_SCOPE_CONTROLLER,2);
    u64(b+HIN_SCOPE_CONTEXT,3); u64(b+HIN_SCOPE_EPOCH,4);
}
static unsigned request(unsigned op, uint8_t *b) {
    switch(op) {
    case HIN_HELLO: return 0;
    case HIN_BIND_CONTROLLER:
        u64(b+HIN_BIND_SESSION,1); u64(b+HIN_BIND_CONTEXT,3); u64(b+HIN_BIND_EPOCH,4); return HIN_BIND_BYTES;
    case HIN_REPORT_MODE:
        scope(b); u64(b+HIN_MODE_SEQUENCE,5); b[HIN_MODE_VALUE]=HIN_VIS; b[HIN_MODE_FLAGS]=HIN_MODE_READONLY;
        u16(b+HIN_MODE_RESERVED,0); u32(b+HIN_MODE_LABEL_LENGTH,4); memcpy(b+HIN_MODE_LABEL,"Nora",4); return HIN_MODE_LABEL+4;
    case HIN_GET_CLIPBOARD: case HIN_UNBIND_CONTROLLER: scope(b); return HIN_SCOPE_BYTES;
    case HIN_READ_CLIPBOARD: case HIN_WRITE_COPY:
        u64(b+HIN_TRANSFER_ID,6); u32(b+HIN_TRANSFER_OFFSET,7); u32(b+HIN_TRANSFER_COUNT,3);
        if(op==HIN_READ_CLIPBOARD) return HIN_READ_REQUEST_BYTES;
        memcpy(b+HIN_WRITE_DATA,"abc",3); return HIN_WRITE_DATA+3;
    case HIN_BEGIN_COPY:
        scope(b); u32(b+HIN_BEGIN_LENGTH,3); u32(b+HIN_BEGIN_RESERVED,0); return HIN_BEGIN_BYTES;
    case HIN_COMMIT_COPY:
        scope(b); u64(b+HIN_COMMIT_TRANSFER,6); u64(b+HIN_COMMIT_EXPECTED,0); return HIN_COMMIT_BYTES;
    case HIN_CANCEL: u64(b+HIN_TRANSFER_ID,6); return 8;
    default: return 999;
    }
}
static unsigned response(unsigned op, uint8_t *b) {
    switch(op) {
    case HIN_HELLO:
        u64(b+HIN_HELLO_SESSION,1); u32(b+HIN_HELLO_MAX_TEXT,HIN_MAX_TEXT);
        u32(b+HIN_HELLO_MAX_CHUNK,HIN_MAX_CHUNK); u32(b+HIN_HELLO_MAX_LABEL,HIN_MAX_LABEL);
        u32(b+HIN_HELLO_MAX_RECORD,HIN_MAX_RECORD); u16(b+HIN_HELLO_WRITE_SLOTS,HIN_WRITE_SLOTS);
        u16(b+HIN_HELLO_READ_SLOTS,HIN_READ_SLOTS); u16(b+HIN_HELLO_CONTROLLERS,HIN_MAX_CONTROLLERS);
        u16(b+HIN_HELLO_RESERVED,0); u32(b+HIN_HELLO_IDLE_MS,HIN_IDLE_MS);
        u32(b+HIN_HELLO_LIFETIME_MS,HIN_LIFETIME_MS); return HIN_HELLO_REPLY_BYTES;
    case HIN_BIND_CONTROLLER: u64(b,2); return 8;
    case HIN_REPORT_MODE: case HIN_CANCEL: case HIN_UNBIND_CONTROLLER: return 0;
    case HIN_GET_CLIPBOARD:
        u64(b+HIN_CLIPBOARD_TRANSFER,6); u64(b+HIN_CLIPBOARD_GENERATION,0);
        u32(b+HIN_CLIPBOARD_LENGTH,3); u32(b+HIN_CLIPBOARD_RESERVED,0); return HIN_CLIPBOARD_BYTES;
    case HIN_READ_CLIPBOARD:
        u32(b+HIN_READ_REPLY_OFFSET,7); u32(b+HIN_READ_REPLY_COUNT,3);
        memcpy(b+HIN_READ_REPLY_DATA,"abc",3); return HIN_READ_REPLY_DATA+3;
    case HIN_BEGIN_COPY: u64(b,6); return 8;
    case HIN_WRITE_COPY: u32(b+HIN_WRITTEN_COUNT,3); u32(b+HIN_WRITTEN_RESERVED,0); return HIN_WRITTEN_BYTES;
    case HIN_COMMIT_COPY: u64(b,1); return 8;
    default: return 999;
    }
}
int main(void) {
    for(unsigned reply=0;reply<2;reply++) for(unsigned op=HIN_HELLO;op<=HIN_UNBIND_CONTROLLER;op++) {
        uint8_t out[256]={0};
        unsigned n=reply?response(op,out+HIN_HEADER_BYTES):request(op,out+HIN_HEADER_BYTES);
        if(n+HIN_HEADER_BYTES>sizeof(out)) return 1;
        memcpy(out+HIN_OFF_MAGIC,"HIN1",4); u16(out+HIN_OFF_VERSION,HIN_VERSION);
        u16(out+HIN_OFF_OPERATION,(uint16_t)op); u32(out+HIN_OFF_LENGTH,HIN_HEADER_BYTES+n);
        u32(out+HIN_OFF_FLAGS,reply?HIN_FLAG_RESPONSE:0); u64(out+HIN_OFF_REQUEST_ID,9);
        printf("%c%u ",reply?'R':'Q',op);
        for(unsigned i=0;i<HIN_HEADER_BYTES+n;i++) printf("%02x",out[i]);
        putchar('\n');
    }
    return ferror(stdout)?1:0;
}
