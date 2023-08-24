/*
 * libdivecomputer
 *
 * Copyright (C) 2008 Jef Driesen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <windows.h>

#include <libdivecomputer/serial.h>

#include "common-private.h"
#include "context-private.h"
#include "iostream-private.h"
#include "iterator-private.h"
#include "platform.h"

static dc_status_t dc_serial_iterator_next (dc_iterator_t *iterator, void *item);
static dc_status_t dc_serial_iterator_free (dc_iterator_t *iterator);

static dc_status_t dc_serial_set_timeout (dc_iostream_t *iostream, int timeout);
static dc_status_t dc_serial_set_break (dc_iostream_t *iostream, unsigned int value);
static dc_status_t dc_serial_set_dtr (dc_iostream_t *iostream, unsigned int value);
static dc_status_t dc_serial_set_rts (dc_iostream_t *iostream, unsigned int value);
static dc_status_t dc_serial_get_lines (dc_iostream_t *iostream, unsigned int *value);
static dc_status_t dc_serial_get_available (dc_iostream_t *iostream, size_t *value);
static dc_status_t dc_serial_configure (dc_iostream_t *iostream, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol);
static dc_status_t dc_serial_poll (dc_iostream_t *iostream, int timeout);
static dc_status_t dc_serial_read (dc_iostream_t *iostream, void *data, size_t size, size_t *actual);
static dc_status_t dc_serial_write (dc_iostream_t *iostream, const void *data, size_t size, size_t *actual);
static dc_status_t dc_serial_ioctl (dc_iostream_t *iostream, unsigned int request, void *data, size_t size);
static dc_status_t dc_serial_flush (dc_iostream_t *iostream);
static dc_status_t dc_serial_purge (dc_iostream_t *iostream, dc_direction_t direction);
static dc_status_t dc_serial_sleep (dc_iostream_t *iostream, unsigned int milliseconds);
static dc_status_t dc_serial_close (dc_iostream_t *iostream);

struct dc_serial_device_t {
	char name[256];
};

typedef struct dc_serial_iterator_t {
	dc_iterator_t base;
	dc_descriptor_t *descriptor;
	HKEY hKey;
	DWORD count;
	DWORD current;
} dc_serial_iterator_t;

typedef struct dc_serial_t {
	dc_iostream_t base;
	/*
	 * The file descriptor corresponding to the serial port.
	 */
	HANDLE hFile;
	/*
	 * Serial port settings are saved into this variables immediately
	 * after the port is opened. These settings are restored when the
	 * serial port is closed.
	 */
	DCB dcb;
	COMMTIMEOUTS timeouts;

	HANDLE hReadWrite, hPoll;
	OVERLAPPED overlapped;
	DWORD events;
	BOOL pending;
} dc_serial_t;

static const dc_iterator_vtable_t dc_serial_iterator_vtable = {
	sizeof(dc_serial_iterator_t),
	dc_serial_iterator_next,
	dc_serial_iterator_free,
};

static const dc_iostream_vtable_t dc_serial_vtable = {
	sizeof(dc_serial_t),
	dc_serial_set_timeout, /* set_timeout */
	dc_serial_set_break, /* set_break */
	dc_serial_set_dtr, /* set_dtr */
	dc_serial_set_rts, /* set_rts */
	dc_serial_get_lines, /* get_lines */
	dc_serial_get_available, /* get_available */
	dc_serial_configure, /* configure */
	dc_serial_poll, /* poll */
	dc_serial_read, /* read */
	dc_serial_write, /* write */
	dc_serial_ioctl, /* ioctl */
	dc_serial_flush, /* flush */
	dc_serial_purge, /* purge */
	dc_serial_sleep, /* sleep */
	dc_serial_close, /* close */
};

static dc_status_t
syserror(DWORD errcode)
{
	switch (errcode) {
	case ERROR_INVALID_PARAMETER:
		return DC_STATUS_INVALIDARGS;
	case ERROR_OUTOFMEMORY:
		return DC_STATUS_NOMEMORY;
	case ERROR_FILE_NOT_FOUND:
		return DC_STATUS_NODEVICE;
	case ERROR_ACCESS_DENIED:
		return DC_STATUS_NOACCESS;
	default:
		return DC_STATUS_IO;
	}
}

const char *
dc_serial_device_get_name (dc_serial_device_t *device)
{
	if (device == NULL || device->name[0] == '\0')
		return NULL;

	return device->name;
}

void
dc_serial_device_free (dc_serial_device_t *device)
{
	free (device);
}

dc_status_t
dc_serial_iterator_new (dc_iterator_t **out, dc_context_t *context, dc_descriptor_t *descriptor)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_serial_iterator_t *iterator = NULL;
	HKEY hKey = NULL;
	DWORD count = 0;
	LONG rc = 0;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	iterator = (dc_serial_iterator_t *) dc_iterator_allocate (context, &dc_serial_iterator_vtable);
	if (iterator == NULL) {
		SYSERROR (context, ERROR_OUTOFMEMORY);
		return DC_STATUS_NOMEMORY;
	}

	// Open the registry key.
	rc = RegOpenKeyExA (HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_QUERY_VALUE, &hKey);
	if (rc != ERROR_SUCCESS) {
		if (rc == ERROR_FILE_NOT_FOUND) {
			hKey = NULL;
		} else {
			SYSERROR (context, rc);
			status = syserror (rc);
			goto error_free;
		}
	}

	// Get the number of values.
	if (hKey) {
		rc = RegQueryInfoKey (hKey, NULL, NULL, NULL, NULL, NULL, NULL, &count, NULL, NULL, NULL, NULL);
		if (rc != ERROR_SUCCESS) {
			SYSERROR (context, rc);
			status = syserror (rc);
			goto error_close;
		}
	}

	iterator->descriptor = descriptor;
	iterator->hKey = hKey;
	iterator->count = count;
	iterator->current = 0;

	*out = (dc_iterator_t *) iterator;

	return DC_STATUS_SUCCESS;

error_close:
	RegCloseKey (hKey);
error_free:
	dc_iterator_deallocate ((dc_iterator_t *) iterator);
	return status;
}

static dc_status_t
dc_serial_iterator_next (dc_iterator_t *abstract, void *out)
{
	dc_serial_iterator_t *iterator = (dc_serial_iterator_t *) abstract;
	dc_serial_device_t *device = NULL;

	while (iterator->current < iterator->count) {
		// Get the value name, data and type.
		char name[256], data[sizeof(device->name)];
		DWORD name_len = sizeof (name);
		DWORD data_len = sizeof (data);
		DWORD type = 0;
		LONG rc = RegEnumValueA (iterator->hKey, iterator->current++, name, &name_len, NULL, &type, (LPBYTE) data, &data_len);
		if (rc != ERROR_SUCCESS) {
			SYSERROR (abstract->context, rc);
			return syserror (rc);
		}

		// Ignore non-string values.
		if (type != REG_SZ)
			continue;

		// Prevent a possible buffer overflow.
		if (data_len >= sizeof (data)) {
			return DC_STATUS_NOMEMORY;
		}

		// Null terminate the string.
		data[data_len] = 0;

		if (!dc_descriptor_filter (iterator->descriptor, DC_TRANSPORT_SERIAL, data)) {
			continue;
		}

		device = (dc_serial_device_t *) malloc (sizeof(dc_serial_device_t));
		if (device == NULL) {
			SYSERROR (abstract->context, ERROR_OUTOFMEMORY);
			return DC_STATUS_NOMEMORY;
		}

		strncpy(device->name, data, sizeof(device->name));

		*(dc_serial_device_t **) out = device;

		return DC_STATUS_SUCCESS;
	}

	return DC_STATUS_DONE;
}

static dc_status_t
dc_serial_iterator_free (dc_iterator_t *abstract)
{
	dc_serial_iterator_t *iterator = (dc_serial_iterator_t *) abstract;

	if (iterator->hKey) {
		RegCloseKey (iterator->hKey);
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_serial_open (dc_iostream_t **out, dc_context_t *context, const char *name)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_serial_t *device = NULL;

	if (out == NULL || name == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (context, "Open: name=%s", name);

	// Build the device name.
	const char *devname = NULL;
	char buffer[MAX_PATH] = "\\\\.\\";
	if (strncmp (name, buffer, 4) != 0) {
		size_t length = strlen (name) + 1;
		if (length + 4 > sizeof (buffer))
			return DC_STATUS_NOMEMORY;
		memcpy (buffer + 4, name, length);
		devname = buffer;
	} else {
		devname = name;
	}

	// Allocate memory.
	device = (dc_serial_t *) dc_iostream_allocate (context, &dc_serial_vtable, DC_TRANSPORT_SERIAL);
	if (device == NULL) {
		SYSERROR (context, ERROR_OUTOFMEMORY);
		return DC_STATUS_NOMEMORY;
	}

	// Default values.
	memset(&device->overlapped, 0, sizeof(device->overlapped));
	device->events = 0;
	device->pending = FALSE;

	// Create a manual reset event for I/O.
	device->hReadWrite = CreateEvent (NULL, TRUE, FALSE, NULL);
	if (device->hReadWrite == INVALID_HANDLE_VALUE) {
		DWORD errcode = GetLastError ();
		SYSERROR (context, errcode);
		status = syserror (errcode);
		goto error_free;
	}

	// Create a manual reset event for polling.
	device->hPoll = CreateEvent (NULL, TRUE, FALSE, NULL);
	if (device->hPoll == INVALID_HANDLE_VALUE) {
		DWORD errcode = GetLastError ();
		SYSERROR (context, errcode);
		status = syserror (errcode);
		goto error_free_readwrite;
	}

	// Open the device.
	device->hFile = CreateFileA (devname,
			GENERIC_READ | GENERIC_WRITE, 0,
			NULL, // No security attributes.
			OPEN_EXISTING,
			FILE_FLAG_OVERLAPPED,
			NULL);
	if (device->hFile == INVALID_HANDLE_VALUE) {
		DWORD errcode = GetLastError ();
		SYSERROR (context, errcode);
		status = syserror (errcode);
		goto error_free_poll;
	}

	// Retrieve the current communication settings and timeouts,
	// to be able to restore them when closing the device.
	// It is also used to check if the obtained handle
	// represents a serial device.
	if (!GetCommState (device->hFile, &device->dcb) ||
		!GetCommTimeouts (device->hFile, &device->timeouts)) {
		DWORD errcode = GetLastError ();
		SYSERROR (context, errcode);
		status = syserror (errcode);
		goto error_close;
	}

	// Enable event monitoring.
	if (!SetCommMask (device->hFile, EV_RXCHAR)) {
		DWORD errcode = GetLastError ();
		SYSERROR (context, errcode);
		status = syserror (errcode);
		goto error_close;
	}

	*out = (dc_iostream_t *) device;

	return DC_STATUS_SUCCESS;

error_close:
	CloseHandle (device->hFile);
error_free_poll:
	CloseHandle (device->hPoll);
error_free_readwrite:
	CloseHandle (device->hReadWrite);
error_free:
	dc_iostream_deallocate ((dc_iostream_t *) device);
	return status;
}

static dc_status_t
dc_serial_close (dc_iostream_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_serial_t *device = (dc_serial_t *) abstract;

	// Disable event monitoring.
	SetCommMask (device->hFile, 0);

	// Restore the initial communication settings and timeouts.
	if (!SetCommState (device->hFile, &device->dcb) ||
		!SetCommTimeouts (device->hFile, &device->timeouts)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		dc_status_set_error(&status, syserror (errcode));
	}

	// Close the device.
	if (!CloseHandle (device->hFile)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		dc_status_set_error(&status, syserror (errcode));
	}

	CloseHandle (device->hPoll);
	CloseHandle (device->hReadWrite);

	return status;
}

static dc_status_t
dc_serial_configure (dc_iostream_t *abstract, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	// Retrieve the current settings.
	DCB dcb;
	if (!GetCommState (device->hFile, &dcb)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	dcb.fBinary = TRUE; // Enable Binary Transmission
	dcb.fAbortOnError = FALSE;

	// Baudrate.
	dcb.BaudRate = baudrate;

	// Character size.
	if (databits >= 5 && databits <= 8)
		dcb.ByteSize = databits;
	else
		return DC_STATUS_INVALIDARGS;

	// Parity checking.
	switch (parity) {
	case DC_PARITY_NONE:
		dcb.Parity = NOPARITY;
		dcb.fParity = FALSE;
		break;
	case DC_PARITY_EVEN:
		dcb.Parity = EVENPARITY;
		dcb.fParity = TRUE;
		break;
	case DC_PARITY_ODD:
		dcb.Parity = ODDPARITY;
		dcb.fParity = TRUE;
		break;
	case DC_PARITY_MARK:
		dcb.Parity = MARKPARITY;
		dcb.fParity = TRUE;
		break;
	case DC_PARITY_SPACE:
		dcb.Parity = SPACEPARITY;
		dcb.fParity = TRUE;
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	// Stopbits.
	switch (stopbits) {
	case DC_STOPBITS_ONE:
		dcb.StopBits = ONESTOPBIT;
		break;
	case DC_STOPBITS_ONEPOINTFIVE:
		dcb.StopBits = ONE5STOPBITS;
		break;
	case DC_STOPBITS_TWO:
		dcb.StopBits = TWOSTOPBITS;
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	// Flow control.
	switch (flowcontrol) {
	case DC_FLOWCONTROL_NONE:
		dcb.fInX = FALSE;
		dcb.fOutX = FALSE;
		dcb.fOutxCtsFlow = FALSE;
		dcb.fOutxDsrFlow = FALSE;
		dcb.fDtrControl = DTR_CONTROL_ENABLE;
		dcb.fRtsControl = RTS_CONTROL_ENABLE;
		break;
	case DC_FLOWCONTROL_HARDWARE:
		dcb.fInX = FALSE;
		dcb.fOutX = FALSE;
		dcb.fOutxCtsFlow = TRUE;
		dcb.fOutxDsrFlow = TRUE;
		dcb.fDtrControl = DTR_CONTROL_HANDSHAKE;
		dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
		break;
	case DC_FLOWCONTROL_SOFTWARE:
		dcb.fInX = TRUE;
		dcb.fOutX = TRUE;
		dcb.fOutxCtsFlow = FALSE;
		dcb.fOutxDsrFlow = FALSE;
		dcb.fDtrControl = DTR_CONTROL_ENABLE;
		dcb.fRtsControl = RTS_CONTROL_ENABLE;
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	// Apply the new settings.
	if (!SetCommState (device->hFile, &dcb)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_set_timeout (dc_iostream_t *abstract, int timeout)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	// Retrieve the current timeouts.
	COMMTIMEOUTS timeouts;
	if (!GetCommTimeouts (device->hFile, &timeouts)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	// Update the settings.
	if (timeout < 0) {
		// Blocking mode.
		timeouts.ReadIntervalTimeout = 0;
		timeouts.ReadTotalTimeoutMultiplier = 0;
		timeouts.ReadTotalTimeoutConstant = 0;
		timeouts.WriteTotalTimeoutMultiplier = 0;
		timeouts.WriteTotalTimeoutConstant = 0;
	} else if (timeout == 0) {
		// Non-blocking mode.
		timeouts.ReadIntervalTimeout = MAXDWORD;
		timeouts.ReadTotalTimeoutMultiplier = 0;
		timeouts.ReadTotalTimeoutConstant = 0;
		timeouts.WriteTotalTimeoutMultiplier = 0;
		timeouts.WriteTotalTimeoutConstant = 0;
	} else {
		// Standard timeout mode.
		timeouts.ReadIntervalTimeout = 0;
		timeouts.ReadTotalTimeoutMultiplier = 0;
		timeouts.ReadTotalTimeoutConstant = timeout;
		timeouts.WriteTotalTimeoutMultiplier = 0;
		timeouts.WriteTotalTimeoutConstant = 0;
	}

	// Activate the new timeouts.
	if (!SetCommTimeouts (device->hFile, &timeouts)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_poll (dc_iostream_t *abstract, int timeout)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	while (1) {
		COMSTAT stats;
		if (!ClearCommError (device->hFile, NULL, &stats)) {
			DWORD errcode = GetLastError ();
			SYSERROR (abstract->context, errcode);
			return syserror (errcode);
		}

		if (stats.cbInQue)
			break;

		if (!device->pending) {
			memset(&device->overlapped, 0, sizeof(device->overlapped));
			device->overlapped.hEvent = device->hPoll;
			device->events = 0;
			if (!WaitCommEvent (device->hFile, &device->events, &device->overlapped)) {
				DWORD errcode = GetLastError ();
				if (errcode != ERROR_IO_PENDING) {
					SYSERROR (abstract->context, errcode);
					return syserror (errcode);
				}
				device->pending = TRUE;
			}
		}

		if (device->pending) {
			DWORD errcode = 0;
			DWORD rc = WaitForSingleObject (device->hPoll, timeout >= 0 ? (DWORD) timeout : INFINITE);
			switch (rc) {
			case WAIT_OBJECT_0:
				break;
			case WAIT_TIMEOUT:
				return DC_STATUS_TIMEOUT;
			default:
				errcode = GetLastError ();
				SYSERROR (abstract->context, errcode);
				return syserror (errcode);
			}
		}

		DWORD dummy = 0;
		if (!GetOverlappedResult (device->hFile, &device->overlapped, &dummy, TRUE)) {
			DWORD errcode = GetLastError ();
			SYSERROR (abstract->context, errcode);
			return syserror (errcode);
		}

		device->pending = FALSE;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_read (dc_iostream_t *abstract, void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_serial_t *device = (dc_serial_t *) abstract;
	DWORD dwRead = 0;

	OVERLAPPED overlapped = {0};
	overlapped.hEvent = device->hReadWrite;

	if (!ReadFile (device->hFile, data, size, NULL, &overlapped)) {
		DWORD errcode = GetLastError ();
		if (errcode != ERROR_IO_PENDING) {
			SYSERROR (abstract->context, errcode);
			status = syserror (errcode);
			goto out;
		}
	}

	if (!GetOverlappedResult (device->hFile, &overlapped, &dwRead, TRUE)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		status = syserror (errcode);
		goto out;
	}

	if (dwRead != size) {
		status = DC_STATUS_TIMEOUT;
	}

out:
	if (actual)
		*actual = dwRead;

	return status;
}

static dc_status_t
dc_serial_write (dc_iostream_t *abstract, const void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_serial_t *device = (dc_serial_t *) abstract;
	DWORD dwWritten = 0;

	OVERLAPPED overlapped = {0};
	overlapped.hEvent = device->hReadWrite;

	if (!WriteFile (device->hFile, data, size, NULL, &overlapped)) {
		DWORD errcode = GetLastError ();
		if (errcode != ERROR_IO_PENDING) {
			SYSERROR (abstract->context, errcode);
			status = syserror (errcode);
			goto out;
		}
	}

	if (!GetOverlappedResult (device->hFile, &overlapped, &dwWritten, TRUE)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		status = syserror (errcode);
		goto out;
	}

	if (dwWritten != size) {
		status = DC_STATUS_TIMEOUT;
	}

out:
	if (actual)
		*actual = dwWritten;

	return status;
}

static dc_status_t
dc_serial_ioctl (dc_iostream_t *abstract, unsigned int request, void *data, size_t size)
{
	switch (request) {
	case DC_IOCTL_SERIAL_SET_LATENCY:
		return DC_STATUS_SUCCESS;
	default:
		return DC_STATUS_UNSUPPORTED;
	}
}

static dc_status_t
dc_serial_purge (dc_iostream_t *abstract, dc_direction_t direction)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	DWORD flags = 0;

	switch (direction) {
	case DC_DIRECTION_INPUT:
		flags = PURGE_RXABORT | PURGE_RXCLEAR;
		break;
	case DC_DIRECTION_OUTPUT:
		flags = PURGE_TXABORT | PURGE_TXCLEAR;
		break;
	case DC_DIRECTION_ALL:
		flags = PURGE_RXABORT | PURGE_RXCLEAR | PURGE_TXABORT | PURGE_TXCLEAR;
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	if (!PurgeComm (device->hFile, flags)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_flush (dc_iostream_t *abstract)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	if (!FlushFileBuffers (device->hFile)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_set_break (dc_iostream_t *abstract, unsigned int level)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	if (level) {
		if (!SetCommBreak (device->hFile)) {
			DWORD errcode = GetLastError ();
			SYSERROR (abstract->context, errcode);
			return syserror (errcode);
		}
	} else {
		if (!ClearCommBreak (device->hFile)) {
			DWORD errcode = GetLastError ();
			SYSERROR (abstract->context, errcode);
			return syserror (errcode);
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_set_dtr (dc_iostream_t *abstract, unsigned int level)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	int status = (level ? SETDTR : CLRDTR);

	if (!EscapeCommFunction (device->hFile, status)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_set_rts (dc_iostream_t *abstract, unsigned int level)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	int status = (level ? SETRTS : CLRRTS);

	if (!EscapeCommFunction (device->hFile, status)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_get_available (dc_iostream_t *abstract, size_t *value)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	COMSTAT stats;

	if (!ClearCommError (device->hFile, NULL, &stats)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	if (value)
		*value = stats.cbInQue;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_get_lines (dc_iostream_t *abstract, unsigned int *value)
{
	dc_serial_t *device = (dc_serial_t *) abstract;
	unsigned int lines = 0;

	DWORD stats = 0;
	if (!GetCommModemStatus (device->hFile, &stats)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	if (stats & MS_RLSD_ON)
		lines |= DC_LINE_DCD;
	if (stats & MS_CTS_ON)
		lines |= DC_LINE_CTS;
	if (stats & MS_DSR_ON)
		lines |= DC_LINE_DSR;
	if (stats & MS_RING_ON)
		lines |= DC_LINE_RNG;

	if (value)
		*value = lines;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_sleep (dc_iostream_t *abstract, unsigned int timeout)
{
	if (dc_platform_sleep (timeout) != 0) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}
