/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 CMS DriveTech AB
 *
 * Serial transport for the OpenPort 2.0 CDC interface. */
#ifndef OP2_PORT_H
#define OP2_PORT_H

#include <windows.h>

#define OP2_VID_PID "VID_0403&PID_CC4D"

typedef struct {
    HANDLE h;
    char   port[32];       /* e.g. "COM8" */
    char   serial[64];     /* USB instance serial, e.g. "TAHJALXT" */
} op2_port;

/* Find the OpenPort 2.0 COM port. Returns TRUE and fills port/serial.
 * If 'want_serial' is non-NULL and non-empty, only a device whose USB
 * instance serial matches is accepted. */
BOOL op2_port_find(char *port_out, size_t port_len,
                   char *serial_out, size_t serial_len,
                   const char *want_serial);

/* Enumerate every attached OpenPort 2.0. Returns the count; writes up to
 * 'max' entries of "COM8|TAHJALXT" form into list[]. */
int  op2_port_enumerate(char list[][96], int max);

BOOL op2_port_open(op2_port *p, const char *want_serial);
void op2_port_close(op2_port *p);
BOOL op2_port_is_open(const op2_port *p);

/* Raw I/O. */
BOOL op2_port_write(op2_port *p, const void *buf, size_t len);
void op2_port_flush_input(op2_port *p);

/* Read one CRLF/LF terminated line. Returns number of bytes in 'line'
 * (excluding terminator), or -1 on timeout/error. */
int  op2_port_read_line(op2_port *p, char *line, size_t len, DWORD timeout_ms);

/* Read exactly 'len' raw bytes (used for binary receive payloads). */
BOOL op2_port_read_exact(op2_port *p, void *buf, size_t len, DWORD timeout_ms);

/* Non-blocking peek: how many bytes are waiting. */
DWORD op2_port_avail(op2_port *p);

#endif /* OP2_PORT_H */
