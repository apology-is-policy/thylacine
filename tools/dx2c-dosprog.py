#!/usr/bin/env python3
"""Emit DX2C.COM -- the DX-2c "a real DOS program runs" witness (Cryptid arc).

A 49-byte DOS .COM that, via INT 21h, creates OUT.TXT on the current drive,
writes the marker "DX-2C-OK\r\n", closes it, and terminates with exit code 0.
The gate mounts a writable guest dir as C:, runs this from the DOSBox-X autoexec,
then reads C:\\OUT.TXT back from the Thylacine shell -- a FILE is the verifiable
signal because DOS console output lands on the Tapestry pane, not the serial log.

A .COM loads at CS:0x100 with DS=CS, so the two data pointers below are absolute
offsets into the same segment. They are COMPUTED from the fixed code length (not
hand-transcribed) so the file stays correct if the code is ever edited:
    FNAME_OFF = 0x100 + len(code)
    MSG_OFF   = FNAME_OFF + len(FNAME)

Usage:  dx2c-dosprog.py <out.com>
"""
import struct
import sys

FNAME = b"OUT.TXT\x00"   # 8 bytes: ASCIIZ filename for INT 21h AH=3Ch
MSG   = b"DX-2C-OK\r\n"  # 10 bytes: the marker AH=40h writes

CODE_LEN = 31            # fixed length of the code block below (asserted)
LOAD      = 0x100                    # .COM entry point (CS:0x100, DS=CS)
FNAME_OFF = LOAD + CODE_LEN          # -> "OUT.TXT",0     (0x011F)
MSG_OFF   = FNAME_OFF + len(FNAME)   # -> "DX-2C-OK\r\n"  (0x0127)


def build():
    code = b"".join([
        b"\xB4\x3C",                              # mov ah, 3Ch        ; DOS create/truncate file
        b"\x31\xC9",                              # xor cx, cx         ; file attributes = 0 (normal)
        b"\xBA" + struct.pack("<H", FNAME_OFF),   # mov dx, FNAME_OFF  ; DS:DX -> "OUT.TXT",0
        b"\xCD\x21",                              # int 21h            ; -> AX = file handle
        b"\x89\xC3",                              # mov bx, ax         ; BX = handle for write/close
        b"\xB4\x40",                              # mov ah, 40h        ; DOS write to handle
        b"\xB9" + struct.pack("<H", len(MSG)),    # mov cx, len(MSG)   ; byte count
        b"\xBA" + struct.pack("<H", MSG_OFF),     # mov dx, MSG_OFF    ; DS:DX -> marker
        b"\xCD\x21",                              # int 21h            ; write CX bytes
        b"\xB4\x3E",                              # mov ah, 3Eh        ; DOS close handle (BX)
        b"\xCD\x21",                              # int 21h
        b"\xB4\x4C",                              # mov ah, 4Ch        ; DOS terminate
        b"\xB0\x00",                              # mov al, 0          ; exit code 0
        b"\xCD\x21",                              # int 21h
    ])
    assert len(code) == CODE_LEN, f"code is {len(code)} bytes, expected {CODE_LEN}"
    blob = code + FNAME + MSG
    assert len(blob) == 49, f"blob is {len(blob)} bytes, expected 49"
    return blob


def main():
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <out.com>")
    with open(sys.argv[1], "wb") as f:
        f.write(build())


if __name__ == "__main__":
    main()
