/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 CMS DriveTech AB
 *
 * OpenPort 2.0 AT protocol layer. See PROTOCOL.md for the wire format. */
#include "op2_proto.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ log */

static FILE *g_log;
static CRITICAL_SECTION g_log_lock;
static LONG g_log_ready;

void op2_log_init(void)
{
    char path[MAX_PATH];
    if (InterlockedExchange(&g_log_ready, 1))
        return;
    InitializeCriticalSection(&g_log_lock);
    if (GetEnvironmentVariableA("OP2_J2534_LOG", path, sizeof(path)) > 0)
        g_log = fopen(path, "a");
}

void op2_log(const char *fmt, ...)
{
    va_list ap;
    SYSTEMTIME st;

    if (!g_log)
        return;
    GetLocalTime(&st);
    EnterCriticalSection(&g_log_lock);
    fprintf(g_log, "%02d:%02d:%02d.%03d ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_lock);
}

/* --------------------------------------------------------------- parsing */

/* Parse "ar<type> <n> <n> ... <seq>" into a reply struct. */
static BOOL parse_reply(const char *line, op2_reply *rep)
{
    const char *p = line;

    memset(rep, 0, sizeof(*rep));
    strncpy(rep->text, line, sizeof(rep->text) - 1);

    if (p[0] != 'a' || p[1] != 'r')
        return FALSE;
    rep->type = p[2];
    if (!rep->type)
        return FALSE;
    p += 3;

    /* "arg5 3 1 8" glues the protocol id to the type letter; treat any
     * digits immediately following as the first argument. */
    if (*p >= '0' && *p <= '9') {
        rep->args[rep->nargs++] = strtoul(p, (char **)&p, 10);
    }

    while (*p && rep->nargs < (int)(sizeof(rep->args) / sizeof(rep->args[0]))) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p < '0' || *p > '9')
            break;              /* non-numeric tail, e.g. 'ari main code ...' */
        rep->args[rep->nargs++] = strtoul(p, (char **)&p, 10);
    }

    if (rep->nargs > 0) {
        rep->seq = rep->args[rep->nargs - 1];
        rep->has_seq = TRUE;
    }
    return TRUE;
}

/* ------------------------------------------------------------ rx queue */

BOOL op2_rx_push(op2_dev *d, const PASSTHRU_MSG *m)
{
    int next = (d->rx_head + 1) % OP2_RX_QUEUE;
    if (next == d->rx_tail)
        return FALSE;                    /* full: drop, caller may log */
    d->rx[d->rx_head] = *m;
    d->rx_head = next;
    return TRUE;
}

BOOL op2_rx_pop(op2_dev *d, PASSTHRU_MSG *m)
{
    if (d->rx_tail == d->rx_head)
        return FALSE;
    *m = d->rx[d->rx_tail];
    d->rx_tail = (d->rx_tail + 1) % OP2_RX_QUEUE;
    return TRUE;
}

void op2_rx_clear(op2_dev *d)
{
    d->rx_head = d->rx_tail = 0;
    d->rbuf_pos = d->rbuf_len = 0;   /* also drop any buffered stream bytes */
}

/* --------------------------------------------------------- channel table */

op2_channel *op2_channel_by_id(op2_dev *d, unsigned long channel_id)
{
    int i;
    for (i = 0; i < OP2_MAX_CHANNELS; i++)
        if (d->ch[i].in_use && d->ch[i].channel_id == channel_id)
            return &d->ch[i];
    return NULL;
}

op2_channel *op2_channel_by_protocol(op2_dev *d, unsigned long protocol)
{
    int i;
    for (i = 0; i < OP2_MAX_CHANNELS; i++)
        if (d->ch[i].in_use && d->ch[i].protocol == protocol)
            return &d->ch[i];
    return NULL;
}

/* ---------------------------------------------------- stream tokenizer */

/* The device multiplexes two kinds of output on the one serial stream:
 *
 *   - ASCII command replies, CRLF terminated:  "aro 4\r\n", "are 9 4\r\n",
 *     "arr 16 108 4\r\n", "arf5 0 3\r\n", "arg5 1 500000 3\r\n", "ari ...\r\n"
 *   - Binary received-frame records, NOT terminated, self-describing by a
 *     length byte:  'a' 'r' <proto-digit> <len> <status> <ts0..3> <id0..3>
 *     <data...>, where <len> counts every byte after it (so len = 9 + datalen).
 *
 * Both begin "ar". The third byte disambiguates: a decimal digit ('1'..'6')
 * means a binary frame for that protocol id; a letter means an ASCII reply.
 * Because binary frames are read by their length byte, data bytes that happen
 * to be 0x61 0x72 ("ar") never cause a mis-parse once we are correctly framed.
 */

#define TOK_NONE  0
#define TOK_REPLY 1
#define TOK_FRAME 2

/* Serve one byte from the read-ahead buffer, refilling it with a single
 * kernel-blocking ReadFile when empty. No Sleep(): ReadFile itself waits
 * (up to the COMMTIMEOUTS constant) and returns the instant data arrives,
 * so latency tracks the device (~1 ms) rather than the OS timer tick. */
static int rd_byte(op2_dev *d, DWORD deadline, unsigned char *out)
{
    for (;;) {
        DWORD n = 0;
        if (d->rbuf_pos < d->rbuf_len) {
            *out = d->rbuf[d->rbuf_pos++];
            return 1;
        }
        d->rbuf_pos = d->rbuf_len = 0;
        if (!ReadFile(d->port.h, d->rbuf, sizeof(d->rbuf), &n, NULL))
            return -1;
        if (n > 0) {
            d->rbuf_len = (int)n;
            continue;
        }
        /* no data within the driver timeout window */
        if ((long)(GetTickCount() - deadline) >= 0)
            return 0;
    }
}

static int rd_exact(op2_dev *d, DWORD deadline, unsigned char *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int r = rd_byte(d, deadline, buf + got);
        if (r <= 0)
            return r;
        got++;
    }
    return 1;
}

static unsigned long be32(const unsigned char *b)
{
    return ((unsigned long)b[0] << 24) | ((unsigned long)b[1] << 16) |
           ((unsigned long)b[2] << 8) | b[3];
}

/* Read the next token from the stream. On TOK_FRAME, *msg is filled; on
 * TOK_REPLY, *rep is filled. Returns TOK_NONE on timeout, -1 on I/O error. */
static int op2_read_token(op2_dev *d, op2_reply *rep, PASSTHRU_MSG *msg,
                          DWORD timeout_ms)
{
    DWORD deadline = GetTickCount() + timeout_ms;
    unsigned char c;
    int r;

    /* sync to 'a' 'r' */
    for (;;) {
        r = rd_byte(d, deadline, &c);
        if (r <= 0)
            return r == 0 ? TOK_NONE : -1;
        if (c != 'a')
            continue;
        r = rd_byte(d, deadline, &c);
        if (r <= 0)
            return r == 0 ? TOK_NONE : -1;
        if (c == 'r')
            break;
    }

    r = rd_byte(d, deadline, &c);       /* third byte: type letter or proto digit */
    if (r <= 0)
        return r == 0 ? TOK_NONE : -1;

    if (c >= '1' && c <= '6') {
        /* binary received-frame record */
        unsigned char len, payload[OP2_LINE_MAX];
        unsigned long proto = (unsigned long)(c - '0');
        if (rd_byte(d, deadline, &len) <= 0)
            return TOK_NONE;
        if (len < 9 || len > sizeof(payload))
            return TOK_NONE;            /* framing lost; caller may resync */
        if (rd_exact(d, deadline, payload, len) <= 0)
            return TOK_NONE;
        if (msg) {
            unsigned long datalen = (unsigned long)len - 9;
            memset(msg, 0, sizeof(*msg));
            msg->ProtocolID = proto;
            msg->RxStatus   = payload[0];
            msg->Timestamp  = be32(payload + 1);
            /* Data = 4-byte big-endian CAN ID followed by the data bytes,
             * which is exactly the on-wire layout: payload[5..]. */
            msg->DataSize = datalen + 4;
            if (msg->DataSize > J2534_MAX_DATA)
                msg->DataSize = J2534_MAX_DATA;
            memcpy(msg->Data, payload + 5, msg->DataSize);
        }
        return TOK_FRAME;
    }

    /* ASCII reply: "ar<c><rest>\r\n" */
    {
        char line[OP2_LINE_MAX];
        size_t used = 0;
        line[used++] = 'a'; line[used++] = 'r'; line[used++] = (char)c;
        for (;;) {
            unsigned char x;
            r = rd_byte(d, deadline, &x);
            if (r <= 0)
                return TOK_NONE;
            if (x == '\n')
                break;
            if (used + 1 < sizeof(line))
                line[used++] = (char)x;
        }
        while (used && line[used - 1] == '\r')
            used--;
        line[used] = '\0';
        if (rep)
            parse_reply(line, rep);
        return TOK_REPLY;
    }
}

void op2_pump(op2_dev *d)
{
    if (!op2_port_is_open(&d->port))
        return;
    while (d->rbuf_pos < d->rbuf_len || op2_port_avail(&d->port) > 0) {
        PASSTHRU_MSG m;
        op2_reply rep;
        int t = op2_read_token(d, &rep, &m, 20);
        if (t == TOK_FRAME) {
            if (!op2_rx_push(d, &m))
                op2_log("RX queue full, dropped frame");
        } else if (t == TOK_REPLY) {
            op2_log("RX async reply: %s", rep.text);
        } else {
            break;
        }
    }
}

/* Block up to timeout_ms for one received frame, returning it in *m.
 * Waits in the kernel via ReadFile (no Sleep), so it returns within ~1 ms of
 * the frame actually arriving. Any ASCII replies seen meanwhile are ignored.
 * Returns TRUE if a frame was produced. */
BOOL op2_wait_rx(op2_dev *d, PASSTHRU_MSG *m, DWORD timeout_ms)
{
    DWORD deadline = GetTickCount() + timeout_ms;
    if (!op2_port_is_open(&d->port))
        return FALSE;
    for (;;) {
        op2_reply rep;
        long remaining = (long)(deadline - GetTickCount());
        int t;
        if (remaining < 0)
            return FALSE;
        t = op2_read_token(d, &rep, m, (DWORD)remaining);
        if (t == TOK_FRAME)
            return TRUE;
        if (t < 0)
            return FALSE;
        /* TOK_REPLY or TOK_NONE: keep waiting until the deadline */
    }
}

/* ------------------------------------------------------------- commands */

long op2_command(op2_dev *d, op2_reply *rep, DWORD timeout_ms,
                 const void *body, size_t body_len,
                 const char *fmt, ...)
{
    char cmd[OP2_LINE_MAX];
    unsigned long seq;
    int n;
    va_list ap;
    DWORD deadline;
    op2_reply local;
    PASSTHRU_MSG frame;

    if (!rep)
        rep = &local;
    if (!op2_port_is_open(&d->port))
        return ERR_DEVICE_NOT_CONNECTED;

    seq = ++d->seq;
    if (seq == 0)
        seq = ++d->seq;

    va_start(ap, fmt);
    n = _vsnprintf(cmd, sizeof(cmd) - 16, fmt, ap);
    va_end(ap);
    if (n < 0)
        return ERR_FAILED;

    /* Every command takes a trailing sequence number, which the device echoes.
     * That echo is what lets us match reply to request. */
    n += _snprintf(cmd + n, sizeof(cmd) - n - 1, " %lu\r\n", seq);

    op2_log("TX: %.*s%s", n - 2, cmd, body_len ? " (+payload)" : "");

    if (!op2_port_write(&d->port, cmd, (size_t)n))
        return ERR_DEVICE_NOT_CONNECTED;
    if (body && body_len) {
        if (!op2_port_write(&d->port, body, body_len))
            return ERR_DEVICE_NOT_CONNECTED;
    }

    deadline = GetTickCount() + timeout_ms;
    for (;;) {
        long remaining = (long)(deadline - GetTickCount());
        int t;
        if (remaining <= 0) {
            op2_log("timeout waiting for seq %lu", seq);
            return ERR_TIMEOUT;
        }
        t = op2_read_token(d, rep, &frame, (DWORD)remaining);
        if (t == TOK_FRAME) {
            /* A received bus frame arrived while we waited for our reply.
             * Queue it so PassThruReadMsgs can hand it back. */
            if (!op2_rx_push(d, &frame))
                op2_log("RX queue full, dropped frame");
            continue;
        }
        if (t != TOK_REPLY)
            continue;

        op2_log("RX: %s", rep->text);

        /* 'ari' (identify) carries no sequence number. */
        if (rep->type == 'i')
            return STATUS_NOERROR;

        if (rep->has_seq && rep->seq != seq) {
            op2_log("reply for seq %lu, waiting for %lu", rep->seq, seq);
            continue;
        }

        if (rep->type == 'e') {
            long code = (rep->nargs >= 2) ? (long)rep->args[0]
                                          : (long)ERR_FAILED;
            op2_log("device error %ld", code);
            return code;
        }
        return STATUS_NOERROR;
    }
}

/* --------------------------------------------------------------- session */

BOOL op2_connect(op2_dev *d, const char *want_serial)
{
    op2_reply rep;

    if (!op2_port_open(&d->port, want_serial))
        return FALSE;

    /* Clear any half-finished command left by a previous crashed session,
     * then verify the device answers. */
    op2_port_write(&d->port, "\r\n", 2);
    Sleep(60);
    op2_port_flush_input(&d->port);

    if (op2_command(d, &rep, 1500, NULL, 0, "ati") != STATUS_NOERROR ||
        rep.type != 'i') {
        op2_port_close(&d->port);
        return FALSE;
    }
    strncpy(d->firmware, rep.text, sizeof(d->firmware) - 1);
    op2_log("connected on %s (%s): %s", d->port.port, d->port.serial, d->firmware);

    /* Release any channels a previous session left open. */
    op2_command(d, NULL, 1500, NULL, 0, "ata");
    op2_rx_clear(d);
    return TRUE;
}

void op2_shutdown(op2_dev *d)
{
    if (op2_port_is_open(&d->port)) {
        op2_command(d, NULL, 800, NULL, 0, "ata");
        op2_port_close(&d->port);
    }
    memset(d->ch, 0, sizeof(d->ch));
    op2_rx_clear(d);
}
