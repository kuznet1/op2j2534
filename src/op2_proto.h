/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 CMS DriveTech AB
 *
 * OpenPort 2.0 AT protocol layer. See PROTOCOL.md. */
#ifndef OP2_PROTO_H
#define OP2_PROTO_H

#include <windows.h>
#include "op2_port.h"
#include "../include/j2534.h"

#define OP2_MAX_CHANNELS   3
#define OP2_RX_QUEUE       512
#define OP2_LINE_MAX       512

/* A parsed reply line: "ar<type> [args...] [seq]". */
typedef struct {
    char          type;                 /* 'o' ok, 'e' error, 'i', 'r', 'g' */
    unsigned long args[6];
    int           nargs;
    unsigned long seq;
    BOOL          has_seq;
    char          text[OP2_LINE_MAX];   /* raw line, for 'ari' and logging */
} op2_reply;

/* One open logical channel. */
typedef struct {
    BOOL          in_use;
    unsigned long protocol;             /* J2534 protocol id, also the wire index */
    unsigned long baud;
    unsigned long flags;
    unsigned long slot;                 /* device channel number 0..2 */
    unsigned long channel_id;           /* handle handed to the application */
    /* Host-side mirror of the filter table; the device has no "list filters". */
    struct {
        BOOL          in_use;
        unsigned long type;
        unsigned long id;
    } filters[16];
} op2_channel;

typedef struct {
    op2_port      port;
    CRITICAL_SECTION lock;
    BOOL          open;
    unsigned long device_id;
    unsigned long seq;
    char          firmware[64];
    op2_channel   ch[OP2_MAX_CHANNELS];
    unsigned long next_channel_id;

    /* Received messages waiting for PassThruReadMsgs. */
    PASSTHRU_MSG  rx[OP2_RX_QUEUE];
    int           rx_head, rx_tail;

    /* Read-ahead buffer for the stream tokenizer. The serial read blocks in
     * the kernel until data arrives (see COMMTIMEOUTS in op2_port_open), so
     * the tokenizer never spins or sleeps - that is what keeps per-command
     * latency near the ~1 ms the device actually needs. */
    unsigned char rbuf[4096];
    int           rbuf_len, rbuf_pos;
} op2_dev;

/* Diagnostic log; enabled by setting the OP2_J2534_LOG environment variable
 * to a file path. Safe to call at any time. */
void op2_log(const char *fmt, ...);
void op2_log_init(void);

BOOL op2_connect(op2_dev *d, const char *want_serial);
void op2_shutdown(op2_dev *d);

/* Send a command and wait for the reply carrying our sequence number.
 * 'body'/'body_len' is an optional raw payload appended after the CRLF.
 * Returns a J2534 status code. */
long op2_command(op2_dev *d, op2_reply *rep, DWORD timeout_ms,
                 const void *body, size_t body_len,
                 const char *fmt, ...);

/* Drain anything the device has sent unprompted into the RX queue. */
void op2_pump(op2_dev *d);

/* Block up to timeout_ms for one received bus frame (kernel wait, no Sleep). */
BOOL op2_wait_rx(op2_dev *d, PASSTHRU_MSG *m, DWORD timeout_ms);

/* RX queue helpers. */
BOOL op2_rx_push(op2_dev *d, const PASSTHRU_MSG *m);
BOOL op2_rx_pop(op2_dev *d, PASSTHRU_MSG *m);
void op2_rx_clear(op2_dev *d);

op2_channel *op2_channel_by_id(op2_dev *d, unsigned long channel_id);
op2_channel *op2_channel_by_protocol(op2_dev *d, unsigned long protocol);

#endif /* OP2_PROTO_H */
