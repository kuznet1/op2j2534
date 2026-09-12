/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 CMS DriveTech AB
 *
 * SAE J2534-1 v04.04 entry points for the Tactrix OpenPort 2.0,
 * implemented directly over the device's CDC serial interface.
 *
 * No kernel driver is involved: Windows' inbox usbser.sys binds the OP2's
 * CDC-ACM interface, and we speak the device's own AT protocol over it.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/j2534.h"
#include "op2_proto.h"

#define OP2_DLL_VERSION "1.0.0"
#define OP2_API_VERSION "04.04"

static op2_dev  g_dev;
static BOOL     g_init;
static CRITICAL_SECTION g_lock;
static char     g_last_error[256] = "No error";

#define LOCK()   EnterCriticalSection(&g_lock)
#define UNLOCK() LeaveCriticalSection(&g_lock)

static long fail(long code, const char *msg)
{
    strncpy(g_last_error, msg, sizeof(g_last_error) - 1);
    g_last_error[sizeof(g_last_error) - 1] = '\0';
    op2_log("-> error %ld: %s", code, msg);
    return code;
}

static long ok(void)
{
    strcpy(g_last_error, "No error");
    return STATUS_NOERROR;
}

/* Map a device-reported error onto a sensible J2534 status. The OP2 already
 * returns standard J2534 codes (see PROTOCOL.md), so this is mostly a pass
 * through with a readable message. */
static long device_error(long code)
{
    char buf[96];
    _snprintf(buf, sizeof(buf), "device returned J2534 error 0x%02lX", code);
    return fail(code, buf);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        InitializeCriticalSection(&g_lock);
        op2_log_init();
        op2_log("--- op2j2534 " OP2_DLL_VERSION " loaded ---");
        break;
    case DLL_PROCESS_DETACH:
        if (g_init) {
            op2_shutdown(&g_dev);
            g_init = FALSE;
        }
        DeleteCriticalSection(&g_lock);
        break;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ open */

long WINAPI PassThruOpen(void *pName, unsigned long *pDeviceID)
{
    const char *want = (const char *)pName;
    long rc;

    LOCK();
    op2_log("PassThruOpen(name=%s)", want && *want ? want : "(null)");

    if (!pDeviceID) {
        rc = fail(ERR_NULL_PARAMETER, "pDeviceID is NULL");
        UNLOCK();
        return rc;
    }
    if (g_init) {
        rc = fail(ERR_DEVICE_IN_USE, "device already open");
        UNLOCK();
        return rc;
    }

    memset(&g_dev, 0, sizeof(g_dev));
    g_dev.next_channel_id = 1;

    /* pName may name a specific unit by its USB serial, which is how you
     * pick between two OpenPorts on one machine. NULL means "the first". */
    if (!op2_connect(&g_dev, want)) {
        rc = fail(ERR_DEVICE_NOT_CONNECTED,
                  "no OpenPort 2.0 found (expected a COM port for "
                  OP2_VID_PID ")");
        UNLOCK();
        return rc;
    }

    g_dev.device_id = 1;
    g_init = TRUE;
    *pDeviceID = g_dev.device_id;
    op2_log("PassThruOpen -> deviceId=%lu on %s", g_dev.device_id, g_dev.port.port);
    rc = ok();
    UNLOCK();
    return rc;
}

long WINAPI PassThruClose(unsigned long DeviceID)
{
    long rc;
    LOCK();
    op2_log("PassThruClose(%lu)", DeviceID);
    if (!g_init || DeviceID != g_dev.device_id) {
        rc = fail(ERR_INVALID_DEVICE_ID, "invalid device ID");
        UNLOCK();
        return rc;
    }
    op2_shutdown(&g_dev);
    g_init = FALSE;
    rc = ok();
    UNLOCK();
    return rc;
}

/* --------------------------------------------------------------- connect */

long WINAPI PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID,
                            unsigned long Flags, unsigned long Baudrate,
                            unsigned long *pChannelID)
{
    op2_channel *c = NULL;
    long rc;
    int slot;

    LOCK();
    op2_log("PassThruConnect(dev=%lu proto=%lu flags=0x%08lX baud=%lu)",
            DeviceID, ProtocolID, Flags, Baudrate);

    if (!g_init || DeviceID != g_dev.device_id) {
        rc = fail(ERR_INVALID_DEVICE_ID, "invalid device ID");
        UNLOCK(); return rc;
    }
    if (!pChannelID) {
        rc = fail(ERR_NULL_PARAMETER, "pChannelID is NULL");
        UNLOCK(); return rc;
    }
    switch (ProtocolID) {
    case ISO9141: case ISO14230: case CAN: case ISO15765:
        break;
    case J1850VPW: case J1850PWM:
        /* The hardware advertises these but this firmware rejects them. */
        rc = fail(ERR_INVALID_PROTOCOL_ID,
                  "J1850 is not supported by this OpenPort firmware");
        UNLOCK(); return rc;
    default:
        rc = fail(ERR_INVALID_PROTOCOL_ID, "unsupported protocol");
        UNLOCK(); return rc;
    }
    if (ProtocolID == ISO14230 && Baudrate != 10400) {
        rc = fail(ERR_INVALID_BAUDRATE, "ISO14230 supports 10400 baud only");
        UNLOCK(); return rc;
    }
    if (op2_channel_by_protocol(&g_dev, ProtocolID)) {
        rc = fail(ERR_CHANNEL_IN_USE, "a channel for this protocol is open");
        UNLOCK(); return rc;
    }

    for (slot = 0; slot < OP2_MAX_CHANNELS; slot++)
        if (!g_dev.ch[slot].in_use) { c = &g_dev.ch[slot]; break; }
    if (!c) {
        rc = fail(ERR_EXCEEDED_LIMIT, "all three channels are in use");
        UNLOCK(); return rc;
    }

    /* ato<protocol> <flags> <baud> <channel> <seq>
     *
     * Argument order matches the J2534 PassThruConnect prototype: Flags before
     * Baudrate. This mirrors the genuine op20pt32.dll (verified by disassembly)
     * and is confirmed on hardware: with the baud in this slot a GET_CONFIG of
     * DATA_RATE reads back the value we set. Putting baud in the wrong slot
     * leaves DATA_RATE at 0 and the CAN controller never syncs to the bus. */
    rc = op2_command(&g_dev, NULL, 3000, NULL, 0, "ato%lu %lu %lu %d",
                     ProtocolID, Flags, Baudrate, slot);
    if (rc != STATUS_NOERROR) {
        rc = device_error(rc);
        UNLOCK(); return rc;
    }

    memset(c, 0, sizeof(*c));
    c->in_use     = TRUE;
    c->protocol   = ProtocolID;
    c->baud       = Baudrate;
    c->flags      = Flags;
    c->slot       = (unsigned long)slot;
    c->channel_id = g_dev.next_channel_id++;
    *pChannelID   = c->channel_id;

    op2_log("PassThruConnect -> channelId=%lu (device slot %d)", c->channel_id, slot);
    rc = ok();
    UNLOCK();
    return rc;
}

long WINAPI PassThruDisconnect(unsigned long ChannelID)
{
    op2_channel *c;
    long rc;

    LOCK();
    op2_log("PassThruDisconnect(%lu)", ChannelID);
    if (!g_init) {
        rc = fail(ERR_DEVICE_NOT_CONNECTED, "device not open");
        UNLOCK(); return rc;
    }
    c = op2_channel_by_id(&g_dev, ChannelID);
    if (!c) {
        rc = fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
        UNLOCK(); return rc;
    }

    /* atc<protocol> <seq> */
    op2_command(&g_dev, NULL, 2000, NULL, 0, "atc%lu", c->protocol);
    memset(c, 0, sizeof(*c));
    rc = ok();
    UNLOCK();
    return rc;
}

/* ------------------------------------------------------------- messaging */

long WINAPI PassThruReadMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg,
                             unsigned long *pNumMsgs, unsigned long Timeout)
{
    op2_channel *c;
    unsigned long want, got = 0;
    DWORD deadline;
    long rc;

    LOCK();
    if (!g_init) {
        rc = fail(ERR_DEVICE_NOT_CONNECTED, "device not open");
        UNLOCK(); return rc;
    }
    c = op2_channel_by_id(&g_dev, ChannelID);
    if (!c) {
        rc = fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
        UNLOCK(); return rc;
    }
    if (!pMsg || !pNumMsgs) {
        rc = fail(ERR_NULL_PARAMETER, "NULL parameter");
        UNLOCK(); return rc;
    }

    want = *pNumMsgs;
    *pNumMsgs = 0;
    deadline = GetTickCount() + Timeout;

    /* First hand back anything already queued (drained while we were parsing a
     * command reply, or by a previous call). */
    {
        PASSTHRU_MSG m;
        op2_pump(&g_dev);
        while (got < want && op2_rx_pop(&g_dev, &m)) {
            if (m.ProtocolID == c->protocol)
                pMsg[got++] = m;
        }
    }

    /* Then block in the kernel for more, up to the caller's timeout. This
     * returns within ~1 ms of each frame arriving - no Sleep()-granularity
     * penalty. */
    while (got < want && Timeout != 0) {
        PASSTHRU_MSG m;
        long remaining = (long)(deadline - GetTickCount());
        if (remaining <= 0)
            break;
        if (!op2_wait_rx(&g_dev, &m, (DWORD)remaining))
            break;
        if (m.ProtocolID == c->protocol)
            pMsg[got++] = m;
    }

    *pNumMsgs = got;
    if (got == 0) {
        rc = fail(ERR_BUFFER_EMPTY, "no messages available");
        UNLOCK(); return rc;
    }
    rc = ok();
    UNLOCK();
    return rc;
}

long WINAPI PassThruWriteMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg,
                              unsigned long *pNumMsgs, unsigned long Timeout)
{
    op2_channel *c;
    unsigned long i, sent = 0;
    long rc = STATUS_NOERROR;

    LOCK();
    if (!g_init) {
        rc = fail(ERR_DEVICE_NOT_CONNECTED, "device not open");
        UNLOCK(); return rc;
    }
    c = op2_channel_by_id(&g_dev, ChannelID);
    if (!c) {
        rc = fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
        UNLOCK(); return rc;
    }
    if (!pMsg || !pNumMsgs) {
        rc = fail(ERR_NULL_PARAMETER, "NULL parameter");
        UNLOCK(); return rc;
    }

    for (i = 0; i < *pNumMsgs; i++) {
        PASSTHRU_MSG *m = &pMsg[i];

        if (m->DataSize == 0 || m->DataSize > J2534_MAX_DATA) {
            rc = fail(ERR_INVALID_MSG, "message DataSize out of range");
            break;
        }
        if (m->ProtocolID != c->protocol) {
            rc = fail(ERR_MSG_PROTOCOL_ID, "message protocol does not match channel");
            break;
        }

        /* att<protocol> <DataSize> <TxFlags> <tx_timeout_us> <seq> + raw Data.
         *
         * Verified on hardware (matches genuine op20pt32.dll by disassembly):
         *   - arg1 = DataSize; the device consumes exactly that many bytes.
         *     For CAN/ISO15765 the Data payload is a 4-byte big-endian CAN ID
         *     followed by the data, and DataSize counts those 4 ID bytes.
         *   - arg2 = TxFlags.
         *   - arg3 = a per-frame transmit timeout in MICROSECONDS. This must be
         *     nonzero: a value of 0 makes the firmware report ERR_TIMEOUT
         *     immediately without ever putting the frame on the bus. This was
         *     the single reason transmit appeared "dead" in early bring-up.
         *
         * The genuine DLL uses (J2534 Timeout * 1000). We do the same, with a
         * sane floor so a caller passing Timeout=0 still actually transmits. */
        {
            unsigned long tx_timeout_us = Timeout ? (Timeout * 1000UL) : 100000UL;
            rc = op2_command(&g_dev, NULL, (Timeout ? Timeout : 1000) + 500,
                             m->Data, m->DataSize,
                             "att%lu %lu %lu %lu",
                             c->protocol, m->DataSize, m->TxFlags,
                             tx_timeout_us);
        }
        if (rc != STATUS_NOERROR) {
            device_error(rc);
            break;
        }
        sent++;
    }

    *pNumMsgs = sent;
    if (sent && rc == STATUS_NOERROR)
        rc = ok();
    UNLOCK();
    return rc;
}

long WINAPI PassThruStartPeriodicMsg(unsigned long ChannelID, PASSTHRU_MSG *pMsg,
                                     unsigned long *pMsgID, unsigned long TimeInterval)
{
    (void)ChannelID; (void)pMsg; (void)pMsgID; (void)TimeInterval;
    /* The device has a periodic-message command ('atm'), but its argument
     * layout is not yet confirmed. Reporting this honestly is safer than
     * silently sending nothing on a bus the caller believes is being fed. */
    return fail(ERR_NOT_SUPPORTED, "periodic messages not implemented yet");
}

long WINAPI PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID)
{
    (void)ChannelID; (void)MsgID;
    return fail(ERR_NOT_SUPPORTED, "periodic messages not implemented yet");
}

long WINAPI PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType,
                                   PASSTHRU_MSG *pMaskMsg, PASSTHRU_MSG *pPatternMsg,
                                   PASSTHRU_MSG *pFlowControlMsg,
                                   unsigned long *pFilterID)
{
    op2_channel *c;
    long rc;
    int i;

    LOCK();
    if (!g_init) {
        rc = fail(ERR_DEVICE_NOT_CONNECTED, "device not open");
        UNLOCK(); return rc;
    }
    c = op2_channel_by_id(&g_dev, ChannelID);
    if (!c) {
        rc = fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
        UNLOCK(); return rc;
    }
    if (!pFilterID) {
        rc = fail(ERR_NULL_PARAMETER, "pFilterID is NULL");
        UNLOCK(); return rc;
    }
    if (FilterType != PASS_FILTER && FilterType != BLOCK_FILTER &&
        FilterType != FLOW_CONTROL_FILTER) {
        rc = fail(ERR_INVALID_FILTER_ID, "unknown filter type");
        UNLOCK(); return rc;
    }
    if (FilterType == FLOW_CONTROL_FILTER && !pFlowControlMsg) {
        rc = fail(ERR_NULL_PARAMETER, "flow control filter needs pFlowControlMsg");
        UNLOCK(); return rc;
    }
    if (!pMaskMsg || !pPatternMsg) {
        rc = fail(ERR_NULL_PARAMETER, "filter needs mask and pattern messages");
        UNLOCK(); return rc;
    }

    for (i = 0; i < 16; i++)
        if (!c->filters[i].in_use)
            break;
    if (i == 16) {
        rc = fail(ERR_EXCEEDED_LIMIT, "filter table full");
        UNLOCK(); return rc;
    }

    /* atf<protocol> <type> <mask_len> <pattern_len> <seq> + <mask><pattern>.
     *
     * Confirmed on hardware: the mask and pattern are sent as a binary payload
     * after the CRLF, exactly like 'att' sends its data. For CAN each is the
     * 4-byte big-endian message-ID selector. The device replies
     * 'arf<proto> <filter_id> <seq>'. A pass filter with mask 0 / pattern 0
     * accepts every ID; INSTALLING A FILTER IS REQUIRED before the device
     * forwards any received frame to the host.
     *
     * Filter type on the wire matches J2534: 1=PASS, 2=BLOCK, 3=FLOW_CONTROL. */
    {
        unsigned char payload[8];
        unsigned long mask = 0, patt = 0;
        op2_reply rep;

        if (pMaskMsg->DataSize >= 4)
            mask = ((unsigned long)pMaskMsg->Data[0] << 24) |
                   ((unsigned long)pMaskMsg->Data[1] << 16) |
                   ((unsigned long)pMaskMsg->Data[2] << 8) | pMaskMsg->Data[3];
        if (pPatternMsg->DataSize >= 4)
            patt = ((unsigned long)pPatternMsg->Data[0] << 24) |
                   ((unsigned long)pPatternMsg->Data[1] << 16) |
                   ((unsigned long)pPatternMsg->Data[2] << 8) | pPatternMsg->Data[3];
        payload[0] = (unsigned char)(mask >> 24); payload[1] = (unsigned char)(mask >> 16);
        payload[2] = (unsigned char)(mask >> 8);  payload[3] = (unsigned char)mask;
        payload[4] = (unsigned char)(patt >> 24); payload[5] = (unsigned char)(patt >> 16);
        payload[6] = (unsigned char)(patt >> 8);  payload[7] = (unsigned char)patt;

        rc = op2_command(&g_dev, &rep, 2000, payload, sizeof(payload),
                         "atf%lu %lu 4 4", c->protocol, FilterType);
        if (rc != STATUS_NOERROR) {
            rc = device_error(rc);
            UNLOCK(); return rc;
        }
        /* Reply 'arf<proto> <filter_id> <seq>': arg[0]=proto, arg[1]=filter id. */
        c->filters[i].in_use = TRUE;
        c->filters[i].type   = FilterType;
        c->filters[i].id     = (rep.nargs >= 3) ? rep.args[1] : (unsigned long)(i + 1);
        *pFilterID = c->filters[i].id;
    }

    op2_log("StartMsgFilter: type=%lu -> id=%lu", FilterType, *pFilterID);
    rc = ok();
    UNLOCK();
    return rc;
}

long WINAPI PassThruStopMsgFilter(unsigned long ChannelID, unsigned long FilterID)
{
    op2_channel *c;
    long rc;
    int i;

    LOCK();
    if (!g_init) {
        rc = fail(ERR_DEVICE_NOT_CONNECTED, "device not open");
        UNLOCK(); return rc;
    }
    c = op2_channel_by_id(&g_dev, ChannelID);
    if (!c) {
        rc = fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID");
        UNLOCK(); return rc;
    }
    for (i = 0; i < 16; i++) {
        if (c->filters[i].in_use && c->filters[i].id == FilterID) {
            c->filters[i].in_use = FALSE;
            rc = ok();
            UNLOCK(); return rc;
        }
    }
    rc = fail(ERR_INVALID_FILTER_ID, "unknown filter ID");
    UNLOCK();
    return rc;
}

/* ------------------------------------------------------ voltage / version */

long WINAPI PassThruSetProgrammingVoltage(unsigned long DeviceID,
                                          unsigned long PinNumber,
                                          unsigned long Voltage)
{
    long rc;
    LOCK();
    op2_log("PassThruSetProgrammingVoltage(pin=%lu voltage=%lu)", PinNumber, Voltage);
    if (!g_init || DeviceID != g_dev.device_id) {
        rc = fail(ERR_INVALID_DEVICE_ID, "invalid device ID");
        UNLOCK(); return rc;
    }

    /* atp <pin> <millivolts> <seq>. SHORT_TO_GROUND and VOLTAGE_OFF are
     * passed through as the sentinel values the J2534 API defines. */
    rc = op2_command(&g_dev, NULL, 2000, NULL, 0, "atp %lu %lu",
                     PinNumber, Voltage);
    if (rc != STATUS_NOERROR) {
        rc = device_error(rc);
        UNLOCK(); return rc;
    }
    rc = ok();
    UNLOCK();
    return rc;
}

long WINAPI PassThruReadVersion(unsigned long DeviceID, char *pFirmwareVersion,
                                char *pDllVersion, char *pApiVersion)
{
    long rc;
    LOCK();
    if (!g_init || DeviceID != g_dev.device_id) {
        rc = fail(ERR_INVALID_DEVICE_ID, "invalid device ID");
        UNLOCK(); return rc;
    }
    if (!pFirmwareVersion || !pDllVersion || !pApiVersion) {
        rc = fail(ERR_NULL_PARAMETER, "NULL version buffer");
        UNLOCK(); return rc;
    }

    /* 'ari main code version : 1.17.4877' -> '1.17.4877' */
    {
        const char *v = strrchr(g_dev.firmware, ':');
        v = v ? v + 1 : g_dev.firmware;
        while (*v == ' ') v++;
        strncpy(pFirmwareVersion, v, 79);
        pFirmwareVersion[79] = '\0';
    }
    strcpy(pDllVersion, OP2_DLL_VERSION);
    strcpy(pApiVersion, OP2_API_VERSION);
    rc = ok();
    UNLOCK();
    return rc;
}

long WINAPI PassThruGetLastError(char *pErrorDescription)
{
    if (!pErrorDescription)
        return ERR_NULL_PARAMETER;
    strncpy(pErrorDescription, g_last_error, 79);
    pErrorDescription[79] = '\0';
    return STATUS_NOERROR;
}

/* ------------------------------------------------------------------ ioctl */

long WINAPI PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID,
                          void *pInput, void *pOutput)
{
    op2_channel *c;
    long rc;

    LOCK();
    op2_log("PassThruIoctl(ch=%lu id=0x%lX)", ChannelID, IoctlID);
    if (!g_init) {
        rc = fail(ERR_DEVICE_NOT_CONNECTED, "device not open");
        UNLOCK(); return rc;
    }

    switch (IoctlID) {

    case READ_VBATT: {
        op2_reply rep;
        if (!pOutput) { rc = fail(ERR_NULL_PARAMETER, "pOutput is NULL"); break; }
        /* atr <pin> <seq> -> arr <pin> <millivolts> <seq>; pin 16 is battery */
        rc = op2_command(&g_dev, &rep, 2000, NULL, 0, "atr 16");
        if (rc != STATUS_NOERROR) { rc = device_error(rc); break; }
        if (rep.type != 'r' || rep.nargs < 3) {
            rc = fail(ERR_FAILED, "malformed voltage reply");
            break;
        }
        *(unsigned long *)pOutput = rep.args[1];     /* millivolts */
        rc = ok();
        break;
    }

    case READ_PROG_VOLTAGE: {
        op2_reply rep;
        if (!pOutput) { rc = fail(ERR_NULL_PARAMETER, "pOutput is NULL"); break; }
        rc = op2_command(&g_dev, &rep, 2000, NULL, 0, "atr 17");
        if (rc != STATUS_NOERROR) { rc = device_error(rc); break; }
        if (rep.type != 'r' || rep.nargs < 3) {
            rc = fail(ERR_FAILED, "malformed voltage reply");
            break;
        }
        *(unsigned long *)pOutput = rep.args[1];
        rc = ok();
        break;
    }

    case SET_CONFIG: {
        SCONFIG_LIST *list = (SCONFIG_LIST *)pInput;
        unsigned long i;
        c = op2_channel_by_id(&g_dev, ChannelID);
        if (!c) { rc = fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID"); break; }
        if (!list || (list->NumOfParams && !list->ConfigPtr)) {
            rc = fail(ERR_NULL_PARAMETER, "bad SCONFIG_LIST"); break;
        }
        rc = STATUS_NOERROR;
        for (i = 0; i < list->NumOfParams; i++) {
            /* ats<protocol> <param> <value> <seq> */
            rc = op2_command(&g_dev, NULL, 2000, NULL, 0, "ats%lu %lu %lu",
                             c->protocol, list->ConfigPtr[i].Parameter,
                             list->ConfigPtr[i].Value);
            if (rc != STATUS_NOERROR) { rc = device_error(rc); break; }
        }
        if (rc == STATUS_NOERROR) rc = ok();
        break;
    }

    case GET_CONFIG: {
        SCONFIG_LIST *list = (SCONFIG_LIST *)pInput;
        unsigned long i;
        c = op2_channel_by_id(&g_dev, ChannelID);
        if (!c) { rc = fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID"); break; }
        if (!list || (list->NumOfParams && !list->ConfigPtr)) {
            rc = fail(ERR_NULL_PARAMETER, "bad SCONFIG_LIST"); break;
        }
        rc = STATUS_NOERROR;
        for (i = 0; i < list->NumOfParams; i++) {
            op2_reply rep;
            /* atg<protocol> <param> <seq> -> arg<protocol> <param> <value> <seq> */
            rc = op2_command(&g_dev, &rep, 2000, NULL, 0, "atg%lu %lu",
                             c->protocol, list->ConfigPtr[i].Parameter);
            if (rc != STATUS_NOERROR) { rc = device_error(rc); break; }
            if (rep.nargs < 4) { rc = fail(ERR_FAILED, "malformed config reply"); break; }
            list->ConfigPtr[i].Value = rep.args[2];
        }
        if (rc == STATUS_NOERROR) rc = ok();
        break;
    }

    case CLEAR_RX_BUFFER:
        op2_rx_clear(&g_dev);
        op2_port_flush_input(&g_dev.port);
        rc = ok();
        break;

    case CLEAR_TX_BUFFER:
        rc = ok();
        break;

    case CLEAR_MSG_FILTERS: {
        int i;
        c = op2_channel_by_id(&g_dev, ChannelID);
        if (!c) { rc = fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID"); break; }
        for (i = 0; i < 16; i++)
            c->filters[i].in_use = FALSE;
        rc = ok();
        break;
    }

    case CLEAR_PERIODIC_MSGS:
        rc = ok();
        break;

    case FAST_INIT:
    case FIVE_BAUD_INIT:
        rc = fail(ERR_NOT_SUPPORTED, "K-line init routines not implemented yet");
        break;

    default:
        rc = fail(ERR_INVALID_IOCTL_ID, "unknown ioctl ID");
        break;
    }

    UNLOCK();
    return rc;
}
