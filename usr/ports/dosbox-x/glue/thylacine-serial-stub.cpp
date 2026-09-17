/* Thylacine DX-1 port glue: stub the host serial-port API (SERIAL_*). Only
 * opl3duoboard.cpp (a real OPL3 chip reached over a host serial port) calls
 * these; directserial.cpp is gated off (C_DIRECTSERIAL). libserial.cpp has
 * win32/linux/macosx/os2 arms only, so a non-platform target (thylacine) gets
 * no definitions. Thylacine has no host serial passthrough at v1.0, so these
 * stubs let the port LINK and Opl3DuoBoard::connect() fails cleanly (no board).
 * Including libserial.h makes the compiler verify the signatures. */
#include "hardware/serialport/libserial.h"

bool SERIAL_open(const char* portname, COMPORT* port) {
    (void)portname; if (port) *port = 0; return false;
}
void SERIAL_close(COMPORT port) { (void)port; }
void SERIAL_getErrorString(char* buffer, size_t length) {
    if (buffer && length) buffer[0] = 0;
}
bool SERIAL_setCommParameters(COMPORT port, int baudrate, char parity,
                              int bytesize, int stopbits) {
    (void)port; (void)baudrate; (void)parity; (void)bytesize; (void)stopbits;
    return false;
}
void SERIAL_setDTR(COMPORT port, bool value) { (void)port; (void)value; }
void SERIAL_setRTS(COMPORT port, bool value) { (void)port; (void)value; }
void SERIAL_setBREAK(COMPORT port, bool value) { (void)port; (void)value; }
int  SERIAL_getmodemstatus(COMPORT port) { (void)port; return 0; }
bool SERIAL_setmodemcontrol(COMPORT port, int flags) { (void)port; (void)flags; return false; }
bool SERIAL_sendchar(COMPORT port, char data) { (void)port; (void)data; return false; }
int  SERIAL_getextchar(COMPORT port) { (void)port; return 0; }
