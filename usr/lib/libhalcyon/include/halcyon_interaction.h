#ifndef HALCYON_INTERACTION_H
#define HALCYON_INTERACTION_H

/* HIN1 envelope reservations. Fields are bytes, never a native C struct.
 * Operation bodies and the asynchronous client API are not defined here yet.
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
#endif
