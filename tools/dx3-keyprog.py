#!/usr/bin/env python3
"""Emit DX3K.COM -- the DX-3 "a keystroke reaches DOS" witness (Cryptid arc).

A 67-byte DOS .COM that, via INT 21h, prints a "HITKEY" prompt, waits for ONE
keystroke (read from STDIN with NO echo -- AH=08h), then creates OUT.TXT on the
current drive, writes "KEY=<c>\r\n" where <c> is the byte that was read, closes
it, and terminates. The file readback proves the WHOLE input path end to end:
QMP send-key -> QEMU virtio-keyboard -> tapestryd (compositor) -> the focused
DOSBox-X surface -> SDL_thylacine event pump -> DOSBox BIOS keyboard buffer ->
INT 21h AH=08h. A FILE is the verifiable signal because DOS console output (the
prompt and the echoed key) lands on the Tapestry pane, not the serial log.

A .COM loads at CS:0x100 with DS=CS, so the three data pointers below are
absolute offsets into the same segment. They are COMPUTED from the fixed code
length (not hand-transcribed) so the file stays correct if the code is edited:
    PROMPT_OFF   = 0x100 + len(code)
    FNAME_OFF    = PROMPT_OFF + len(PROMPT)
    MSG_OFF      = FNAME_OFF + len(FNAME)
    MSG_CHAR_OFF = MSG_OFF + 4          # the '?' placeholder in "KEY=?\r\n"

The single self-modifying store (AH=08h's AL -> the '?' byte) is legal in a
.COM: DS=CS, so [MSG_CHAR_OFF] addresses this program's own image.

Usage:  dx3-keyprog.py <out.com>
"""
import struct
import sys

PROMPT = b"HITKEY$"      # 7 bytes: AH=09h '$'-terminated prompt (pane-visible)
FNAME  = b"OUT.TXT\x00"  # 8 bytes: ASCIIZ filename for INT 21h AH=3Ch
MSG    = b"KEY=?\r\n"    # 7 bytes: the marker AH=40h writes ('?' <- read byte)

CODE_LEN     = 45                        # fixed length of the code block (asserted)
LOAD         = 0x100                      # .COM entry point (CS:0x100, DS=CS)
PROMPT_OFF   = LOAD + CODE_LEN            # -> "HITKEY$"     (0x012D)
FNAME_OFF    = PROMPT_OFF + len(PROMPT)   # -> "OUT.TXT",0   (0x0134)
MSG_OFF      = FNAME_OFF + len(FNAME)     # -> "KEY=?\r\n"   (0x013C)
MSG_CHAR_OFF = MSG_OFF + 4                # -> the '?' byte  (0x0140)


def build():
    code = b"".join([
        b"\xB4\x09",                                 # mov ah, 09h         ; DOS print '$'-string
        b"\xBA" + struct.pack("<H", PROMPT_OFF),     # mov dx, PROMPT_OFF  ; DS:DX -> "HITKEY$"
        b"\xCD\x21",                                 # int 21h             ; print the prompt
        b"\xB4\x08",                                 # mov ah, 08h         ; DOS read STDIN, no echo
        b"\xCD\x21",                                 # int 21h             ; -> AL = key byte
        b"\xA2" + struct.pack("<H", MSG_CHAR_OFF),   # mov [MSG_CHAR_OFF],al ; patch '?' <- key
        b"\xB4\x3C",                                 # mov ah, 3Ch         ; DOS create/truncate file
        b"\x31\xC9",                                 # xor cx, cx          ; file attributes = 0
        b"\xBA" + struct.pack("<H", FNAME_OFF),      # mov dx, FNAME_OFF   ; DS:DX -> "OUT.TXT",0
        b"\xCD\x21",                                 # int 21h             ; -> AX = file handle
        b"\x89\xC3",                                 # mov bx, ax          ; BX = handle
        b"\xB4\x40",                                 # mov ah, 40h         ; DOS write to handle
        b"\xB9" + struct.pack("<H", len(MSG)),       # mov cx, len(MSG)    ; byte count
        b"\xBA" + struct.pack("<H", MSG_OFF),        # mov dx, MSG_OFF     ; DS:DX -> "KEY=<c>\r\n"
        b"\xCD\x21",                                 # int 21h             ; write CX bytes
        b"\xB4\x3E",                                 # mov ah, 3Eh         ; DOS close handle (BX)
        b"\xCD\x21",                                 # int 21h
        b"\xB4\x4C",                                 # mov ah, 4Ch         ; DOS terminate
        b"\xB0\x00",                                 # mov al, 0           ; exit code 0
        b"\xCD\x21",                                 # int 21h
    ])
    assert len(code) == CODE_LEN, f"code is {len(code)} bytes, expected {CODE_LEN}"
    blob = code + PROMPT + FNAME + MSG
    assert len(blob) == 67, f"blob is {len(blob)} bytes, expected 67"
    return blob


def main():
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <out.com>")
    with open(sys.argv[1], "wb") as f:
        f.write(build())


if __name__ == "__main__":
    main()
