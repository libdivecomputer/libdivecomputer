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

#define NOGDI
#include <windows.h>

#include "serial.h"
#include "context-private.h"

struct serial_t {
	/* Library context. */
	dc_context_t *context;
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
	/* Half-duplex settings */
	int halfduplex;
	unsigned int baudrate;
	unsigned int nbits;
};

int
serial_enumerate (serial_callback_t callback, void *userdata)
{
	// Open the registry key.
	HKEY hKey;
	LONG rc = RegOpenKeyExA (HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_QUERY_VALUE, &hKey);
	if (rc != ERROR_SUCCESS) {
		if (rc == ERROR_FILE_NOT_FOUND)
			return 0;
		else
			return -1;
	}

	// Get the number of values.
	DWORD count = 0;
	rc = RegQueryInfoKey (hKey, NULL, NULL, NULL, NULL, NULL, NULL, &count, NULL, NULL, NULL, NULL);
	if (rc != ERROR_SUCCESS) {
		RegCloseKey(hKey);
		return -1;
	}

	for (DWORD i = 0; i < count; ++i) {
		// Get the value name, data and type.
		char name[512], data[512];
		DWORD name_len = sizeof (name);
		DWORD data_len = sizeof (data);
		DWORD type = 0;
		rc = RegEnumValueA (hKey, i, name, &name_len, NULL, &type, (LPBYTE) data, &data_len);
		if (rc != ERROR_SUCCESS) {
			RegCloseKey(hKey);
			return -1;
		}

		// Ignore non-string values.
		if (type != REG_SZ)
			continue;

		// Prevent a possible buffer overflow.
		if (data_len >= sizeof (data)) {
			RegCloseKey(hKey);
			return -1;
		}

		// Null terminate the string.
		data[data_len] = 0;

		callback (data, userdata);
	}

	RegCloseKey(hKey);

	return 0;
}

//
// Open the serial port.
//

int
serial_open (serial_t **out, dc_context_t *context, const char* name)
{
	if (out == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	INFO (context, "Open: name=%s", name ? name : "");

	// Build the device name.
	const char *devname = NULL;
	char buffer[MAX_PATH] = "\\\\.\\";
	if (name && strncmp (name, buffer, 4) != 0) {
		size_t length = strlen (name) + 1;
		if (length + 4 > sizeof (buffer))
			return -1;
		memcpy (buffer + 4, name, length);
		devname = buffer;
	} else {
		devname = name;
	}

	// Allocate memory.
	serial_t *device = (serial_t *) malloc (sizeof (serial_t));
	if (device == NULL) {
		SYSERROR (context, ERROR_OUTOFMEMORY);
		return -1; // ERROR_OUTOFMEMORY (Not enough storage is available to complete this operation)
	}

	// Library context.
	device->context = context;

	// Default to full-duplex.
	device->halfduplex = 0;
	device->baudrate = 0;
	device->nbits = 0;

	// Open the device.
	device->hFile = CreateFileA (devname,
			GENERIC_READ | GENERIC_WRITE, 0,
			NULL, // No security attributes.
			OPEN_EXISTING,
			0, // Non-overlapped I/O.
			NULL);
	if (device->hFile == INVALID_HANDLE_VALUE) {
		SYSERROR (context, GetLastError ());
		goto error_free;
	}

	// Retrieve the current communication settings and timeouts,
	// to be able to restore them when closing the device.
	// It is also used to check if the obtained handle
	// represents a serial device.
	if (!GetCommState (device->hFile, &device->dcb) ||
		!GetCommTimeouts (device->hFile, &device->timeouts)) {
		SYSERROR (context, GetLastError ());
		goto error_close;
	}

	*out = device;

	return 0;

error_close:
	CloseHandle (device->hFile);
error_free:
	free (device);
	return -1;
}

//
// Close the serial port.
//

int
serial_close (serial_t *device)
{
	int errcode = 0;

	if (device == NULL)
		return 0;

	// Restore the initial communication settings and timeouts.
	if (!SetCommState (device->hFile, &device->dcb) ||
		!SetCommTimeouts (device->hFile, &device->timeouts)) {
		SYSERROR (device->context, GetLastError ());
		errcode = -1;
	}

	// Close the device.
	if (!CloseHandle (device->hFile)) {
		SYSERROR (device->context, GetLastError ());
		errcode = -1;
	}

	// Free memory.
	free (device);

	return errcode;
}

//
// Configure the serial port (baudrate, databits, parity, stopbits and flowcontrol).
//

int
serial_configure (serial_t *device, int baudrate, int databits, int parity, int stopbits, int flowcontrol)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	INFO (device->context, "Configure: baudrate=%i, databits=%i, parity=%i, stopbits=%i, flowcontrol=%i",
		baudrate, databits, parity, stopbits, flowcontrol);

	// Retrieve the current settings.
	DCB dcb;
	if (!GetCommState (device->hFile, &dcb)) {
		SYSERROR (device->context, GetLastError ());
		return -1;
	}

	dcb.fBinary = TRUE; // Enable Binary Transmission
	dcb.fAbortOnError = FALSE;

	// Baudrate.
	dcb.BaudRate = baudrate;

	// Character size.
	if (databits >= 5 && databits <= 8)
		dcb.ByteSize = databits;
	else
		return -1;

	// Parity checking.
	switch (parity) {
	case SERIAL_PARITY_NONE: // No parity
		dcb.Parity = NOPARITY;
		dcb.fParity = FALSE;
		break;
	case SERIAL_PARITY_EVEN: // Even parity
		dcb.Parity = EVENPARITY;
		dcb.fParity = TRUE;
		break;
	case SERIAL_PARITY_ODD: // Odd parity
		dcb.Parity = ODDPARITY;
		dcb.fParity = TRUE;
		break;
	default:
		return -1;
	}
	// Stopbits.
	switch (stopbits) {
	case 1: // One stopbit
		dcb.StopBits = ONESTOPBIT;
		break;
	case 2: // Two stopbits
		dcb.StopBits = TWOSTOPBITS;
		break;
	default:
		return -1;
	}

	// Flow control.
	switch (flowcontrol) {
	case SERIAL_FLOWCONTROL_NONE: // No flow control.
		dcb.fInX = FALSE;
		dcb.fOutX = FALSE;
		dcb.fOutxCtsFlow = FALSE;
		dcb.fOutxDsrFlow = FALSE;
		dcb.fDtrControl = DTR_CONTROL_ENABLE;
		dcb.fRtsControl = RTS_CONTROL_ENABLE;
		break;
	case SERIAL_FLOWCONTROL_HARDWARE: // Hardware (RTS/CTS) flow control.
		dcb.fInX = FALSE;
		dcb.fOutX = FALSE;
		dcb.fOutxCtsFlow = TRUE;
		dcb.fOutxDsrFlow = TRUE;
		dcb.fDtrControl = DTR_CONTROL_HANDSHAKE;
		dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
		break;
	case SERIAL_FLOWCONTROL_SOFTWARE: // Software (XON/XOFF) flow control.
		dcb.fInX = TRUE;
		dcb.fOutX = TRUE;
		dcb.fOutxCtsFlow = FALSE;
		dcb.fOutxDsrFlow = FALSE;
		dcb.fDtrControl = DTR_CONTROL_ENABLE;
		dcb.fRtsControl = RTS_CONTROL_ENABLE;
		break;
	default:
		return -1;
	}

	// Apply the new settings.
	if (!SetCommState (device->hFile, &dcb)) {
		SYSERROR (device->context, GetLastError ());
		return -1;
	}

	device->baudrate = baudrate;
	device->nbits = 1 + databits + stopbits + (parity ? 1 : 0);

	return 0;
}

//
// Configure the serial port (timeouts).
//

int
serial_set_timeout (serial_t *device, long timeout)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	INFO (device->context, "Timeout: value=%li", timeout);

	// Retrieve the current timeouts.
	COMMTIMEOUTS timeouts;
	if (!GetCommTimeouts (device->hFile, &timeouts)) {
		SYSERROR (device->context, GetLastError ());
		return -1;
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
		SYSERROR (device->context, GetLastError ());
		return -1;
	}

	return 0;
}

//
// Configure the serial port (recommended size of the input/output buffers).
//

int
serial_set_queue_size (serial_t *device, unsigned int input, unsigned int output)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	if (!SetupComm (device->hFile, input, output)) {
		SYSERROR (device->context, GetLastError ());
		return -1;
	}

	return 0;
}


int
serial_set_halfduplex (serial_t *device, int value)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	device->halfduplex = value;

	return 0;
}


int
serial_set_latency (serial_t *device, unsigned int milliseconds)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	return 0;
}

int
serial_read (serial_t *device, void* data, unsigned int size)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	DWORD dwRead = 0;
	if (!ReadFile (device->hFile, data, size, &dwRead, NULL)) {
		SYSERROR (device->context, GetLastError ());
		return -1;
	}

	HEXDUMP (device->context, DC_LOGLEVEL_INFO, "Read", (unsigned char *) data, dwRead);

	return dwRead;
}


int
serial_write (serial_t *device, const void* data, unsigned int size)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	LARGE_INTEGER begin, end, freq;
	if (device->halfduplex) {
		// Get the current time.
		if (!QueryPerformanceFrequency(&freq) ||
			!QueryPerformanceCounter(&begin)) {
			SYSERROR (device->context, GetLastError ());
			return -1;
		}
	}

	DWORD dwWritten = 0;
	if (!WriteFile (device->hFile, data, size, &dwWritten, NULL)) {
		SYSERROR (device->context, GetLastError ());
		return -1;
	}

	if (device->halfduplex) {
		// Get the current time.
		if (!QueryPerformanceCounter(&end))  {
			SYSERROR (device->context, GetLastError ());
			return -1;
		}

		// Calculate the elapsed time (microseconds).
		unsigned long elapsed = 1000000.0 * (end.QuadPart - begin.QuadPart) / freq.QuadPart + 0.5;

		// Calculate the expected duration (microseconds). A 2 millisecond fudge
		// factor is added because it improves the success rate significantly.
		unsigned long expected = 1000000.0 * device->nbits / device->baudrate * size + 0.5 + 2000;

		// Wait for the remaining time.
		if (elapsed < expected) {
			unsigned long remaining = expected - elapsed;

			// The remaining time is rounded up to the nearest millisecond
			// because the Windows Sleep() function doesn't have a higher
			// resolution.
			serial_sleep (device, (remaining + 999) / 1000);
		}
	}

	HEXDUMP (device->context, DC_LOGLEVEL_INFO, "Write", (unsigned char *) data, dwWritten);

	return dwWritten;
}


int
serial_flush (serial_t *device, int queue)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	INFO (device->context, "Flush: queue=%u, input=%i, output=%i", queue,
		serial_get_received (device),
		serial_get_transmitted (device));

	DWORD flags = 0;

	switch (queue) {
	case SERIAL_QUEUE_INPUT:
		flags = PURGE_RXABORT | PURGE_RXCLEAR;
		break;
	case SERIAL_QUEUE_OUTPUT:
		flags = PURGE_TXABORT | PURGE_TXCLEAR;
		break;
	default:
		flags = PURGE_RXABORT | PURGE_RXCLEAR | PURGE_TXABORT | PURGE_TXCLEAR;
		break;
	}

	if (!PurgeComm (device->hFile, flags)) {
		SYSERROR (device->context, GetLastError ());
		return -1;
	}

	return 0;
}


int
serial_send_break (serial_t *device)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	if (!SetCommBreak (device->hFile)) {
		SYSERROR (device->context, GetLastError ());
		return -1;
	}

	Sleep (250);

	if (!ClearCommBreak (device->hFile)) {
		SYSERROR (device->context, GetLastError ());
		return -1;
	}

	return 0;
}


int
serial_set_break (serial_t *device, int level)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	INFO (device->context, "Break: value=%i", level);

	if (level) {
		if (!SetCommBreak (device->hFile)) {
			SYSERROR (device->context, GetLastError ());
			return -1;
		}
	} else {
		if (!ClearCommBreak (device->hFile)) {
			SYSERROR (device->context, GetLastError ());
			return -1;
		}
	}

	return 0;
}

int
serial_set_dtr (serial_t *device, int level)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	INFO (device->context, "DTR: value=%i", level);

	int status = (level ? SETDTR : CLRDTR);

	if (!EscapeCommFunction (device->hFile, status)) {
		SYSERROR (device->context, GetLastError ());
		return -1;
	}

	return 0;
}


int
serial_set_rts (serial_t *device, int level)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	INFO (device->context, "RTS: value=%i", level);

	int status = (level ? SETRTS : CLRRTS);

	if (!EscapeCommFunction (device->hFile, status)) {
		SYSERROR (device->context, GetLastError ());
		return -1;
	}

	return 0;
}


int
serial_get_received (serial_t *device)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	COMSTAT stats;

	if (!ClearCommError (device->hFile, NULL, &stats)) {
		SYSERROR (device->context, GetLastError ());
		return -1;
	}

	return stats.cbInQue;
}


int
serial_get_transmitted (serial_t *device)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	COMSTAT stats;

	if (!ClearCommError (device->hFile, NULL, &stats)) {
		SYSERROR (device->context, GetLastError ());
		return -1;
	}

	return stats.cbOutQue;
}


int
serial_get_line (serial_t *device, int line)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	DWORD stats = 0;
	if (!GetCommModemStatus (device->hFile, &stats)) {
		SYSERROR (device->context, GetLastError ());
		return -1;
	}

	switch (line) {
	case SERIAL_LINE_DCD:
		return (stats & MS_RLSD_ON) == MS_RLSD_ON;
	case SERIAL_LINE_CTS:
		return (stats & MS_CTS_ON) == MS_CTS_ON;
	case SERIAL_LINE_DSR:
		return (stats & MS_DSR_ON) == MS_DSR_ON;
	case SERIAL_LINE_RNG:
		return (stats & MS_RING_ON) == MS_RING_ON;
	default:
		return -1;
	}

	return 0;
}


int
serial_sleep (serial_t *device, unsigned long timeout)
{
	if (device == NULL)
		return -1;

	INFO (device->context, "Sleep: value=%lu", timeout);

	Sleep (timeout);

	return 0;
}
