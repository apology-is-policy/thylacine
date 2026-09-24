#ifndef HALCYON_INTERACTION_H
#define HALCYON_INTERACTION_H

/* HIN1 v1 envelope and typed body layout. Offsets are bytes, never a native
 * C struct. The asynchronous client API is a separate adapter.
 * Decoding an envelope is not authorization to perform its operation. */
#include <stdint.h>
#define HIN_VERSION 1u
#define HIN_HEADER_BYTES 24u
#define HIN_MAX_RECORD 32768u
#define HIN_MAX_TEXT 1048576u
#define HIN_MAX_CHUNK 16384u
#define HIN_MAX_LABEL 64u
#define HIN_MAX_QUERY 4096u
#define HIN_FLAG_RESPONSE 1u

#define HIN_OFF_MAGIC 0u
#define HIN_OFF_VERSION 4u
#define HIN_OFF_OPERATION 6u
#define HIN_OFF_LENGTH 8u
#define HIN_OFF_FLAGS 12u
#define HIN_OFF_REQUEST_ID 16u

enum hin_operation {
    HIN_HELLO = 1, HIN_BIND_CONTROLLER = 2, HIN_REPORT_MODE = 3,
    HIN_GET_CLIPBOARD = 4, HIN_READ_CLIPBOARD = 5, HIN_BEGIN_COPY = 6,
    HIN_WRITE_COPY = 7, HIN_COMMIT_COPY = 8, HIN_CANCEL = 9,
    HIN_UNBIND_CONTROLLER = 10
};
enum hin_mode {
    HIN_INS = 1, HIN_NOR = 2, HIN_VIS = 3, HIN_CMD = 4, HIN_APP = 5
};

static const uint8_t hin_hello_fixture[HIN_HEADER_BYTES] = {
    0x48, 0x49, 0x4e, 0x31, 1, 0, 1, 0, 24, 0, 0, 0, 0, 0, 0, 0,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01
};
enum hin_failure {
    HIN_DENIED=1, HIN_GONE=2, HIN_TOO_LARGE=7, HIN_BAD_HANDLE=9,
    HIN_CONFLICT=11, HIN_NO_MEMORY=12, HIN_BUSY=16, HIN_INVALID=22,
    HIN_UNSUPPORTED=95, HIN_TIMEOUT=110
};

/* Body-relative offsets; Scope is session/controller/context/epoch. */
#define HIN_SCOPE_BYTES 32u
#define HIN_SCOPE_SESSION 0u
#define HIN_SCOPE_CONTROLLER 8u
#define HIN_SCOPE_CONTEXT 16u
#define HIN_SCOPE_EPOCH 24u
#define HIN_BIND_BYTES 24u
#define HIN_BIND_SESSION 0u
#define HIN_BIND_CONTEXT 8u
#define HIN_BIND_EPOCH 16u
#define HIN_MODE_SEQUENCE 32u
#define HIN_MODE_VALUE 40u
#define HIN_MODE_FLAGS 41u
#define HIN_MODE_RESERVED 42u
#define HIN_MODE_LABEL_LENGTH 44u
#define HIN_MODE_LABEL 48u
#define HIN_MODE_READONLY 1u
#define HIN_TRANSFER_ID 0u
#define HIN_TRANSFER_OFFSET 8u
#define HIN_TRANSFER_COUNT 12u
#define HIN_WRITE_DATA 16u
#define HIN_READ_REQUEST_BYTES 16u
#define HIN_BEGIN_LENGTH 32u
#define HIN_BEGIN_RESERVED 36u
#define HIN_BEGIN_BYTES 40u
#define HIN_COMMIT_TRANSFER 32u
#define HIN_COMMIT_EXPECTED 40u
#define HIN_COMMIT_BYTES 48u
#define HIN_CLIPBOARD_TRANSFER 0u
#define HIN_CLIPBOARD_GENERATION 8u
#define HIN_CLIPBOARD_LENGTH 16u
#define HIN_CLIPBOARD_RESERVED 20u
#define HIN_CLIPBOARD_BYTES 24u
#define HIN_READ_REPLY_OFFSET 0u
#define HIN_READ_REPLY_COUNT 4u
#define HIN_READ_REPLY_DATA 8u
#define HIN_WRITTEN_COUNT 0u
#define HIN_WRITTEN_RESERVED 4u
#define HIN_WRITTEN_BYTES 8u
#define HIN_HELLO_SESSION 0u
#define HIN_HELLO_MAX_TEXT 8u
#define HIN_HELLO_MAX_CHUNK 12u
#define HIN_HELLO_MAX_LABEL 16u
#define HIN_HELLO_MAX_RECORD 20u
#define HIN_HELLO_WRITE_SLOTS 24u
#define HIN_HELLO_READ_SLOTS 26u
#define HIN_HELLO_CONTROLLERS 28u
#define HIN_HELLO_RESERVED 30u
#define HIN_HELLO_IDLE_MS 32u
#define HIN_HELLO_LIFETIME_MS 36u
#define HIN_HELLO_REPLY_BYTES 40u
#define HIN_WRITE_SLOTS 2u
#define HIN_READ_SLOTS 2u
#define HIN_MAX_CONTROLLERS 32u
#define HIN_IDLE_MS 30000u
#define HIN_LIFETIME_MS 120000u
#endif
