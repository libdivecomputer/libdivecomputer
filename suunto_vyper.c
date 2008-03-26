#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <assert.h>	// assert

#include "suunto.h"
#include "serial.h"
#include "utils.h"

#define MIN(a,b)	(((a) < (b)) ? (a) : (b))
#define MAX(a,b)	(((a) > (b)) ? (a) : (b))

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}


struct vyper {
	struct serial *port;
	int extraanswertime;
	int ifacealwaysechos;
	int breakprofreadearly;
	unsigned int delay;
};


int
suunto_vyper_open (vyper **out, const char* name)
{
	if (out == NULL)
		return SUUNTO_ERROR;

	// Allocate memory.
	struct vyper *device = malloc (sizeof (struct vyper));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return SUUNTO_ERROR_MEMORY;
	}

	// Set the default values.
	device->port = NULL;
	device->extraanswertime = 0;
	device->ifacealwaysechos = 0;
	device->breakprofreadearly = 0;
	device->delay = 500;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return SUUNTO_ERROR_IO;
	}

	// Set the serial communication protocol (2400 8O1).
	rc = serial_configure (device->port, 2400, 8, SERIAL_PARITY_ODD, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return SUUNTO_ERROR_IO;
	}

	// Set the timeout for receiving data (1000 ms).
	if (serial_set_timeout (device->port, 1000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return SUUNTO_ERROR_IO;
	}

	// Set the DTR line (power supply for the interface).
	if (serial_set_dtr (device->port, 1) == -1) {
		WARNING ("Failed to set the DTR line.");
		serial_close (device->port);
		free (device);
		return SUUNTO_ERROR_IO;
	}

	// Give the interface 100 ms to settle and draw power up.
	serial_sleep (100);

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = device;

	return SUUNTO_SUCCESS;
}


int
suunto_vyper_close (vyper *device)
{
	if (device == NULL)
		return SUUNTO_SUCCESS;

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return SUUNTO_ERROR_IO;
	}

	// Free memory.	
	free (device);

	return SUUNTO_SUCCESS;
}


static unsigned char
suunto_vyper_checksum (const unsigned char data[], unsigned int size, unsigned char init)
{
	unsigned char crc = init;
	for (unsigned int i = 0; i < size; ++i)
		crc ^= data[i];

	return crc;
}


static void
suunto_vyper_reverse (unsigned char data[], unsigned int size)
{
	for (unsigned int i = 0; i < size / 2; ++i) {
		unsigned char hlp = data[i];
		data[i] = data[size-1-i];
		data[size-1-i] = hlp;
	}
}


static int
suunto_vyper_send_testcmd (vyper *device, const unsigned char* data, unsigned int size)
{
	if (serial_write (device->port, data, size) != size) {
		WARNING ("Failed to send the test sequence.");
		return SUUNTO_ERROR_IO;
	}
	serial_drain (device->port);
	return SUUNTO_SUCCESS;
}


int
suunto_vyper_detect_interface (vyper *device)
{
	int rc, detectmode_worked = 1;
	unsigned char command[3] = {'A', 'T', '\r'}, reply[3] = {0}, extra = 0;

	if (device == NULL)
		return SUUNTO_ERROR;

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	// Charge power supply?
	serial_set_rts (device->port, 1);
	serial_sleep (300);

	// Try detection mode first.

	serial_set_rts (device->port, 0);
	rc = suunto_vyper_send_testcmd (device, command, 3);
	if (rc != SUUNTO_SUCCESS)
		return rc;
	rc = serial_read (device->port, reply, 3);
	if (rc != 3 || memcmp (command, reply, 3) != 0) {
		WARNING ("Interface not responding in probe mode.");
    	detectmode_worked = 0;
	}
	if (serial_read (device->port, &extra, 1) == 1) {
		WARNING ("Got an extraneous character in the detection phase. Maybe the line is connected to a modem?");
	}

	// Try transfer mode now.

	serial_set_rts (device->port, 1);
	rc = suunto_vyper_send_testcmd (device, command, 3);
	if (rc != SUUNTO_SUCCESS)
		return rc;
	serial_set_rts (device->port, 0);
	rc = serial_read (device->port, reply, 3);
	if (rc == 0) {
		if (detectmode_worked) {
			WARNING ("Detected an original suunto interface with RTS-switching.");
		} else {
			WARNING ("Can't detect the interface. Hoping it's an original suunto interface with the DC already attached.");
		}
		device->ifacealwaysechos = 0;
		return SUUNTO_SUCCESS;
	}
	if (rc != 3 || memcmp (command, reply, 3) != 0) {
		WARNING ("Interface not responding in transfer mode.");
	}
	if (serial_read (device->port, &extra, 1) == 1) {
		WARNING ("Got an extraneous character in the detection phase. Maybe the line is connected to a modem?");
	}
	WARNING ("Detected a clone interface without RTS-switching.");
	device->ifacealwaysechos = 1;
	return SUUNTO_SUCCESS;
}


int
suunto_vyper_set_delay (vyper *device, unsigned int delay)
{
	if (device == NULL)
		return SUUNTO_ERROR;

	device->delay = delay;

	return SUUNTO_SUCCESS;
}


static int
suunto_vyper_send (vyper *device, const unsigned char command[], unsigned int csize)
{
	serial_sleep (device->delay);

	// Set RTS to send the command.
	serial_set_rts (device->port, 1);

	// Send the command to the dive computer and 
	// wait until all data has been transmitted.
	serial_write (device->port, command, csize);
	serial_drain (device->port);

	// If the interface sends an echo back (which is the case for many clone 
	// interfaces), this echo should be removed from the input queue before 
	// attempting to read the real reply from the dive computer. Otherwise, 
	// the data transfer will fail. Timing is also critical here! We have to 
	// wait at least until the echo appears (40ms), but not until the reply 
	// from the dive computer appears (600ms).
	// The original suunto interface does not have this problem, because it 
	// does not send an echo and the RTS switching makes it impossible to 
	// receive the reply before RTS is cleared. We have to wait some time 
	// before clearing RTS (around 30ms). But if we wait too long (> 500ms), 
	// the reply disappears again.
	serial_sleep (200);
	serial_flush (device->port, SERIAL_QUEUE_INPUT);

	// Clear RTS to receive the reply.
	serial_set_rts (device->port, 0);

	return SUUNTO_SUCCESS;
}


static int
suunto_vyper_transfer (vyper *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size)
{
	assert (asize >= size + 2);

	// Send the command to the dive computer.
	int rc = suunto_vyper_send (device, command, csize);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Failed to send the command.");
		return rc;
	}

	// Receive the answer of the dive computer.
	rc = serial_read (device->port, answer, asize);
	if (rc != asize) {
		WARNING ("Failed to receive the answer.");
		if (rc == -1)
			return SUUNTO_ERROR_IO;
		return SUUNTO_ERROR_TIMEOUT;
	}

	// Verify the header of the package.
	if (memcmp (command, answer, asize - size - 1) != 0) {
		WARNING ("Unexpected answer start byte(s).");
		return SUUNTO_ERROR_PROTOCOL;
	}

	// Verify the checksum of the package.
	unsigned char crc = answer[asize - 1];
	unsigned char ccrc = suunto_vyper_checksum (answer, asize - 1, 0x00);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return SUUNTO_ERROR_PROTOCOL;
	}

	return SUUNTO_SUCCESS;
}


int
suunto_vyper_read_memory (vyper *device, unsigned int address, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return SUUNTO_ERROR;

	// The data transmission is split in packages
	// of maximum $SUUNTO_VYPER_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = MIN (size - nbytes, SUUNTO_VYPER_PACKET_SIZE);

		// Read the package.
		unsigned char answer[SUUNTO_VYPER_PACKET_SIZE + 5] = {0};
		unsigned char command[5] = {0x05,
				(address >> 8) & 0xFF, // high
				(address     ) & 0xFF, // low
				len, // count
				0};  // CRC
		command[4] = suunto_vyper_checksum (command, 4, 0x00);
		int rc = suunto_vyper_transfer (device, command, sizeof (command), answer, len + 5, len);
		if (rc != SUUNTO_SUCCESS)
			return rc;

		memcpy (data, answer + 4, len);

#ifndef NDEBUG
		message ("VyperRead(0x%04x,%d)=\"", address, len);
		for (unsigned int i = 0; i < len; ++i) {
			message("%02x", data[i]);
		}
		message("\"\n");
#endif

		nbytes += len;
		address += len;
		data += len;
	}

	return SUUNTO_SUCCESS;
}


int
suunto_vyper_write_memory (vyper *device, unsigned int address, const unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return SUUNTO_ERROR;

	// The data transmission is split in packages
	// of maximum $SUUNTO_VYPER_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = MIN (size - nbytes, SUUNTO_VYPER_PACKET_SIZE);

		// Prepare to write the package.
		unsigned char panswer[3] = {0};
		unsigned char pcommand[3] = {0x07, 0xA5, 0xA2};
		int rc = suunto_vyper_transfer (device, pcommand, sizeof (pcommand), panswer, sizeof (panswer), 0);
		if (rc != SUUNTO_SUCCESS)
			return rc;

#ifndef NDEBUG
		message("VyperPrepareWrite();\n");
#endif

		// Write the package.
		unsigned char wanswer[5] = {0};
		unsigned char wcommand[SUUNTO_VYPER_PACKET_SIZE + 5] = {0x06,
				(address >> 8) & 0xFF, // high
				(address     ) & 0xFF, // low
				len, // count
				0};  // data + CRC
		memcpy (wcommand + 4, data, len);
		wcommand[len + 4] = suunto_vyper_checksum (wcommand, len + 4, 0x00);
		rc = suunto_vyper_transfer (device, wcommand, len + 5, wanswer, sizeof (wanswer), 0);
		if (rc != SUUNTO_SUCCESS)
			return rc;

#ifndef NDEBUG
		message ("VyperWrite(0x%04x,%d,\"", address, len);
		for (unsigned int i = 0; i < len; ++i) {
			message ("%02x", data[i]);
		}
		message ("\");\n");
#endif

		nbytes += len;
		address += len;
		data += len;
	}

	return SUUNTO_SUCCESS;
}


int
suunto_vyper_read_dive (vyper *device, unsigned char data[], unsigned int size, int init)
{
	if (device == NULL)
		return SUUNTO_ERROR;

	// Send the command to the dive computer.
	unsigned char command[3] = {init ? 0x08 : 0x09, 0xA5, 0x00};
	command[2] = suunto_vyper_checksum (command, 2, 0x00);
	int rc = suunto_vyper_send (device, command, 3);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Failed to send the command.");
		return rc;
	}

	// The data transmission is split in packages
	// of maximum $SUUNTO_VYPER_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	for (unsigned int npackages = 0;; ++npackages) {
		// Receive the header of the package.
		unsigned char answer[SUUNTO_VYPER_PACKET_SIZE + 3] = {0};
		rc = serial_read (device->port, answer, 2);
		if (rc != 2) {
			// If no data is received because a timeout occured, we assume 
			// the last package was already received and the transmission 
			// can be finished. Unfortunately this is not 100% reliable, 
			// because there is always a small chance that more data will 
			// arrive later (especially with a short timeout). But it works 
			// good enough in practice.
			// Only for the very first package, we can be sure there was 
			// an error, because the DC always sends at least one package.
			if (rc == 0 && npackages != 0)
				break;
			WARNING ("Failed to receive the answer.");
			if (rc == -1)
				return SUUNTO_ERROR_IO;
			return SUUNTO_ERROR_TIMEOUT;
		}

		// Verify the header of the package.
		if (answer[0] != command[0] || 
			answer[1] > SUUNTO_VYPER_PACKET_SIZE) {
			WARNING ("Unexpected answer start byte(s).");
			return SUUNTO_ERROR_PROTOCOL;
		}

		// Receive the remaining part of the package.
		unsigned char len = answer[1];
		rc = serial_read (device->port, answer + 2, len + 1);
		if (rc != len + 1) {
			WARNING ("Failed to receive the answer.");
			if (rc == -1)
				return SUUNTO_ERROR_IO;
			return SUUNTO_ERROR_TIMEOUT;
		}

		// Verify the checksum of the package.
		unsigned char crc = answer[len + 2];
		unsigned char ccrc = suunto_vyper_checksum (answer, len + 2, 0x00);
		if (crc != ccrc) {
			WARNING ("Unexpected answer CRC.");
			return SUUNTO_ERROR_PROTOCOL;
		}

		// Append the package to the output buffer.
		if (nbytes + len <= size) {
			memcpy (data + nbytes, answer + 2, len);
			nbytes += len;
		} else {
			WARNING ("Insufficient buffer space available.");
			return SUUNTO_ERROR_MEMORY;
		}

		// The DC sends a null package (a package with length zero) when it 
		// has reached the end of its internal ring buffer. From this point on, 
		// the current dive has been overwritten with newer data. Therefore, 
		// we discard the current (incomplete) dive and end the transmission.
		if (len == 0) {
			WARNING ("Null package received.");
#ifndef NDEBUG
			suunto_vyper_reverse (data, nbytes);
			message ("Vyper%sProfile=\"", init ? "First" : "");
			for (unsigned int i = 0; i < nbytes; ++i) {
				message("%02x", data[i]);
			}
			message("\"\n");
#endif
			return 0;
		}

		// If a package is smaller than $SUUNTO_VYPER_PACKET_SIZE bytes, 
		// we assume it's the last packet and the transmission can be 
		// finished early. However, this approach does not work if the 
		// last packet is exactly $SUUNTO_VYPER_PACKET_SIZE bytes long!
		if (device->breakprofreadearly && len != SUUNTO_VYPER_PACKET_SIZE) 
			break;
	}

	// The DC traverses its internal ring buffer backwards. The most recent 
	// dive is send first (which allows you to download only the new dives), 
	// but also the contents of each dive is reversed. Therefore, we reverse 
	// the bytes again before returning them to the application.
	suunto_vyper_reverse (data, nbytes);

#ifndef NDEBUG
	message ("Vyper%sProfile=\"", init ? "First" : "");
	for (unsigned int i = 0; i < nbytes; ++i) {
		message("%02x", data[i]);
	}
	message("\"\n");
#endif

	return nbytes;
}
