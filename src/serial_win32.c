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
#include <windows.h>

#include "serial.h"
#include "utils.h"

#define TRACE(expr) \
{ \
	DWORD error = GetLastError (); \
	message ("TRACE (%s:%d, %s): %s (%d)\n", __FILE__, __LINE__, \
		expr, serial_errmsg (), serial_errcode ()); \
	SetLastError (error); \
}

struct serial_t {
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
};

//
// Error reporting.
//

int serial_errcode (void)
{
	return GetLastError ();
}


const char* serial_errmsg (void)
{
	static char buffer[256] = {0};
	unsigned int size = sizeof (buffer) / sizeof (char);

	DWORD errcode = GetLastError ();
	DWORD rc = FormatMessageA (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, errcode, 0, buffer, size, NULL);
	// Remove certain characters ('\r', '\n' and '.')
	// at the end of the error message.
	while (rc > 0 && (
			buffer[rc-1] == '\n' ||
			buffer[rc-1] == '\r' ||
			buffer[rc-1] == '.')) {
		buffer[rc-1] = '\0';
		rc--;
	}
	if (rc) {
		return buffer;
	} else {
		return NULL;
	}
}

//
// Open the serial port.
//

int
serial_open (serial_t **out, const char* name)
{
	if (out == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

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
		TRACE ("malloc");
		return -1; // ERROR_OUTOFMEMORY (Not enough storage is available to complete this operation)
	}

	// Open the device.
	device->hFile = CreateFileA (devname,
			GENERIC_READ | GENERIC_WRITE, 0,
			NULL, // No security attributes.
			OPEN_EXISTING,
			0, // Non-overlapped I/O.
			NULL);
	if (device->hFile == INVALID_HANDLE_VALUE) {
		TRACE ("CreateFile");
		free (device);
		return -1;
	}

	// Retrieve the current communication settings and timeouts,
	// to be able to restore them when closing the device.
	// It is also used to check if the obtained handle
	// represents a serial device.
	if (!GetCommState (device->hFile, &device->dcb) ||
		!GetCommTimeouts (device->hFile, &device->timeouts)) {
		TRACE ("GetCommState/GetCommTimeouts");
		CloseHandle (device->hFile);
		free (device);
		return -1;
	}

	*out = device;

	return 0;
}

//
// Close the serial port.
//

int
serial_close (serial_t *device)
{
	if (device == NULL)
		return 0;

	// Restore the initial communication settings and timeouts.
	if (!SetCommState (device->hFile, &device->dcb) || 
		!SetCommTimeouts (device->hFile, &device->timeouts)) {
		TRACE ("SetCommState/SetCommTimeouts");
		CloseHandle (device->hFile);
		free (device);
		return -1;
	}

	// Close the device.
	if (!CloseHandle (device->hFile)) {
		TRACE ("CloseHandle");
		free (device);
		return -1;
	}

	// Free memory.	
	free (device);

	return 0;
}

//
// Configure the serial port (baudrate, databits, parity, stopbits and flowcontrol).
//

int
serial_configure (serial_t *device, int baudrate, int databits, int parity, int stopbits, int flowcontrol)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	// Retrieve the current settings.
	DCB dcb;
	if (!GetCommState (device->hFile, &dcb)) {
		TRACE ("GetCommState");
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
		TRACE ("SetCommState");
		return -1;
	}

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

	// Retrieve the current timeouts.
	COMMTIMEOUTS timeouts;
	if (!GetCommTimeouts (device->hFile, &timeouts)) {
		TRACE ("GetCommTimeouts");
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
		TRACE ("SetCommTimeouts");
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
		TRACE ("SetupComm");
		return -1;
	}

	return 0;
}

int
serial_read (serial_t *device, void* data, unsigned int size)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	DWORD dwRead = 0;
	if (!ReadFile (device->hFile, data, size, &dwRead, NULL)) {
		TRACE ("ReadFile");
		return -1;
	}

	return dwRead;
}


int
serial_write (serial_t *device, const void* data, unsigned int size)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	DWORD dwWritten = 0;
	if (!WriteFile (device->hFile, data, size, &dwWritten, NULL)) {
		TRACE ("WriteFile");
		return -1;
	}

	return dwWritten;
}


int
serial_flush (serial_t *device, int queue)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

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
		TRACE ("PurgeComm");
		return -1;
	}

	return 0;
}


int
serial_drain (serial_t *device)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	if (!FlushFileBuffers (device->hFile)) {
		TRACE ("FlushFileBuffers");
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
		TRACE ("SetCommBreak");
		return -1;
	}

	Sleep (250);

	if (!ClearCommBreak (device->hFile)) {
		TRACE ("ClearCommBreak");
		return -1;
	}

	return 0;
}


int
serial_set_break (serial_t *device, int level)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	if (level) {
		if (!SetCommBreak (device->hFile)) {
			TRACE ("SetCommBreak");
			return -1;
		}
	} else {
		if (!ClearCommBreak (device->hFile)) {
			TRACE ("ClearCommBreak");
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

	int status = (level ? SETDTR : CLRDTR);
	
	if (!EscapeCommFunction (device->hFile, status)) {
		TRACE ("EscapeCommFunction");
		return -1;
	}
	
	return 0;
}


int
serial_set_rts (serial_t *device, int level)
{
	if (device == NULL)
		return -1; // ERROR_INVALID_PARAMETER (The parameter is incorrect)

	int status = (level ? SETRTS : CLRRTS);
	
	if (!EscapeCommFunction (device->hFile, status)) {
		TRACE ("EscapeCommFunction");
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
		TRACE ("ClearCommError");
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
		TRACE ("ClearCommError");
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
		TRACE ("GetCommModemStatus");
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
serial_sleep (unsigned long timeout)
{
	Sleep (timeout);

	return 0;
}


int
serial_timer (void)
{
	return GetTickCount ();
}
