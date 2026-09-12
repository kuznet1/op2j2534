/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 CMS DriveTech AB
 *
 * Serial transport for the OpenPort 2.0 CDC interface.
 *
 * The OP2 presents a USB CDC-ACM interface, so Windows binds the inbox
 * usbser.sys driver and it shows up as a plain COM port. We locate it by
 * walking the Ports device class and matching the USB hardware ID.
 */
#include "op2_port.h"

#include <setupapi.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "setupapi.lib")

static const GUID GUID_CLASS_PORTS =
    { 0x4D36E978, 0xE325, 0x11CE, { 0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18 } };

/* Pull the trailing instance id segment out of e.g.
 * "USB\VID_0403&PID_CC4D\TAHJALXT" -> "TAHJALXT". */
static void instance_serial(const char *instance_id, char *out, size_t len)
{
    const char *p = strrchr(instance_id, '\\');
    out[0] = '\0';
    if (p) {
        strncpy(out, p + 1, len - 1);
        out[len - 1] = '\0';
    }
}

static int enumerate(char list[][96], int max,
                     char *port_out, size_t port_len,
                     char *serial_out, size_t serial_len,
                     const char *want_serial)
{
    HDEVINFO set;
    SP_DEVINFO_DATA dev;
    DWORD idx;
    int found = 0;

    set = SetupDiGetClassDevsA(&GUID_CLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return 0;

    dev.cbSize = sizeof(dev);
    for (idx = 0; SetupDiEnumDeviceInfo(set, idx, &dev); idx++) {
        char hwid[512] = { 0 };
        char instance[512] = { 0 };
        char portname[32] = { 0 };
        char serial[64] = { 0 };
        HKEY key;
        DWORD type = 0, cb;

        if (!SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_HARDWAREID, &type,
                                               (PBYTE)hwid, sizeof(hwid), NULL))
            continue;
        if (!strstr(hwid, OP2_VID_PID))
            continue;

        /* COM port name lives under the device's Device Parameters key. */
        key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0,
                                   DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE)
            continue;
        cb = sizeof(portname);
        if (RegQueryValueExA(key, "PortName", NULL, NULL,
                             (LPBYTE)portname, &cb) != ERROR_SUCCESS) {
            RegCloseKey(key);
            continue;
        }
        RegCloseKey(key);
        if (strncmp(portname, "COM", 3) != 0)
            continue;

        if (SetupDiGetDeviceInstanceIdA(set, &dev, instance, sizeof(instance), NULL))
            instance_serial(instance, serial, sizeof(serial));

        if (want_serial && want_serial[0] && _stricmp(want_serial, serial) != 0)
            continue;

        if (list && found < max)
            _snprintf(list[found], 96, "%s|%s", portname, serial);

        if (port_out && found == 0) {
            strncpy(port_out, portname, port_len - 1);
            port_out[port_len - 1] = '\0';
            if (serial_out) {
                strncpy(serial_out, serial, serial_len - 1);
                serial_out[serial_len - 1] = '\0';
            }
        }
        found++;
        if (!list && port_out)
            break;              /* caller only wanted the first match */
    }

    SetupDiDestroyDeviceInfoList(set);
    return found;
}

BOOL op2_port_find(char *port_out, size_t port_len,
                   char *serial_out, size_t serial_len,
                   const char *want_serial)
{
    return enumerate(NULL, 0, port_out, port_len,
                     serial_out, serial_len, want_serial) > 0;
}

int op2_port_enumerate(char list[][96], int max)
{
    return enumerate(list, max, NULL, 0, NULL, 0, NULL);
}

BOOL op2_port_open(op2_port *p, const char *want_serial)
{
    char path[64];
    DCB dcb;
    COMMTIMEOUTS to;

    memset(p, 0, sizeof(*p));
    p->h = INVALID_HANDLE_VALUE;

    if (!op2_port_find(p->port, sizeof(p->port),
                       p->serial, sizeof(p->serial), want_serial))
        return FALSE;

    /* \\.\ prefix is required for COM10 and above. */
    _snprintf(path, sizeof(path), "\\\\.\\%s", p->port);
    p->h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, 0, NULL);
    if (p->h == INVALID_HANDLE_VALUE)
        return FALSE;

    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(p->h, &dcb)) {
        op2_port_close(p);
        return FALSE;
    }
    /* CDC ignores the line coding, but the values must still be legal. */
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fParity  = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX  = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(p->h, &dcb)) {
        op2_port_close(p);
        return FALSE;
    }

    /* MSDN special case: ReadIntervalTimeout=MAXDWORD with
     * ReadTotalTimeoutMultiplier=MAXDWORD and a nonzero ReadTotalTimeoutConstant
     * makes ReadFile return the instant any byte is available, or after the
     * constant elapses if none arrive. The wait happens efficiently in the
     * driver, so the protocol layer never has to Sleep() - avoiding the ~15.6 ms
     * penalty a Sleep(1) incurs at Windows' default timer resolution, which was
     * the entire cause of the old ~16 ms-per-command latency. */
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutMultiplier  = MAXDWORD;
    to.ReadTotalTimeoutConstant    = 25;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 2000;
    SetCommTimeouts(p->h, &to);

    SetupComm(p->h, 16384, 16384);
    EscapeCommFunction(p->h, SETDTR);
    EscapeCommFunction(p->h, SETRTS);
    PurgeComm(p->h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return TRUE;
}

void op2_port_close(op2_port *p)
{
    if (p->h != INVALID_HANDLE_VALUE) {
        CloseHandle(p->h);
        p->h = INVALID_HANDLE_VALUE;
    }
}

BOOL op2_port_is_open(const op2_port *p)
{
    return p->h != INVALID_HANDLE_VALUE;
}

BOOL op2_port_write(op2_port *p, const void *buf, size_t len)
{
    const char *q = (const char *)buf;
    DWORD done = 0, n;

    if (p->h == INVALID_HANDLE_VALUE)
        return FALSE;
    while (done < len) {
        if (!WriteFile(p->h, q + done, (DWORD)(len - done), &n, NULL))
            return FALSE;
        if (n == 0)
            return FALSE;
        done += n;
    }
    return TRUE;
}

void op2_port_flush_input(op2_port *p)
{
    if (p->h != INVALID_HANDLE_VALUE)
        PurgeComm(p->h, PURGE_RXCLEAR);
}

DWORD op2_port_avail(op2_port *p)
{
    COMSTAT st;
    DWORD errs = 0;
    if (p->h == INVALID_HANDLE_VALUE)
        return 0;
    if (!ClearCommError(p->h, &errs, &st))
        return 0;
    return st.cbInQue;
}

int op2_port_read_line(op2_port *p, char *line, size_t len, DWORD timeout_ms)
{
    DWORD deadline = GetTickCount() + timeout_ms;
    size_t used = 0;

    if (p->h == INVALID_HANDLE_VALUE)
        return -1;

    for (;;) {
        char c;
        DWORD n = 0;

        if (!ReadFile(p->h, &c, 1, &n, NULL))
            return -1;
        if (n == 1) {
            if (c == '\n') {
                /* strip a trailing CR */
                while (used && line[used - 1] == '\r')
                    used--;
                line[used < len ? used : len - 1] = '\0';
                return (int)used;
            }
            if (used + 1 < len)
                line[used++] = c;
            continue;
        }
        if ((long)(GetTickCount() - deadline) >= 0) {
            line[used < len ? used : len - 1] = '\0';
            return -1;
        }
        Sleep(1);
    }
}

BOOL op2_port_read_exact(op2_port *p, void *buf, size_t len, DWORD timeout_ms)
{
    DWORD deadline = GetTickCount() + timeout_ms;
    char *q = (char *)buf;
    size_t done = 0;

    if (p->h == INVALID_HANDLE_VALUE)
        return FALSE;
    while (done < len) {
        DWORD n = 0;
        if (!ReadFile(p->h, q + done, (DWORD)(len - done), &n, NULL))
            return FALSE;
        done += n;
        if (done >= len)
            break;
        if ((long)(GetTickCount() - deadline) >= 0)
            return FALSE;
        if (n == 0)
            Sleep(1);
    }
    return TRUE;
}
