#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <assert.h>	// assert

#include "device-private.h"
#include "suunto_vyper.h"
#include "suunto_common.h"
#include "serial.h"
#include "checksum.h"
#include "array.h"
#include "utils.h"

#define MIN(a,b)	(((a) < (b)) ? (a) : (b))
#define MAX(a,b)	(((a) > (b)) ? (a) : (b))

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

typedef struct suunto_vyper_device_t suunto_vyper_device_t;

struct suunto_vyper_device_t {
	device_t base;
	struct serial *port;
	int extraanswertime;
	int ifacealwaysechos;
	int breakprofreadearly;
	unsigned int delay;
};

static const device_backend_t suunto_vyper_device_backend;

static int
device_is_suunto_vyper (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &suunto_vyper_device_backend;
}


device_status_t
suunto_vyper_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	suunto_vyper_device_t *device = malloc (sizeof (suunto_vyper_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &suunto_vyper_device_backend);

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
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (2400 8O1).
	rc = serial_configure (device->port, 2400, 8, SERIAL_PARITY_ODD, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (1000 ms).
	if (serial_set_timeout (device->port, 1000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the DTR line (power supply for the interface).
	if (serial_set_dtr (device->port, 1) == -1) {
		WARNING ("Failed to set the DTR line.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Give the interface 100 ms to settle and draw power up.
	serial_sleep (100);

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper_device_close (device_t *abstract)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	if (! device_is_suunto_vyper (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Free memory.	
	free (device);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper_send_testcmd (suunto_vyper_device_t *device, const unsigned char* data, unsigned int size)
{
	if (serial_write (device->port, data, size) != size) {
		WARNING ("Failed to send the test sequence.");
		return DEVICE_STATUS_IO;
	}
	serial_drain (device->port);
	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_vyper_device_detect_interface (device_t *abstract)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	if (! device_is_suunto_vyper (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	int rc, detectmode_worked = 1;
	unsigned char command[3] = {'A', 'T', '\r'}, reply[3] = {0}, extra = 0;

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	// Charge power supply?
	serial_set_rts (device->port, 1);
	serial_sleep (300);

	// Try detection mode first.

	serial_set_rts (device->port, 0);
	rc = suunto_vyper_send_testcmd (device, command, 3);
	if (rc != DEVICE_STATUS_SUCCESS)
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
	if (rc != DEVICE_STATUS_SUCCESS)
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
		return DEVICE_STATUS_SUCCESS;
	}
	if (rc != 3 || memcmp (command, reply, 3) != 0) {
		WARNING ("Interface not responding in transfer mode.");
	}
	if (serial_read (device->port, &extra, 1) == 1) {
		WARNING ("Got an extraneous character in the detection phase. Maybe the line is connected to a modem?");
	}
	WARNING ("Detected a clone interface without RTS-switching.");
	device->ifacealwaysechos = 1;
	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_vyper_device_set_delay (device_t *abstract, unsigned int delay)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	if (! device_is_suunto_vyper (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	device->delay = delay;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper_send (suunto_vyper_device_t *device, const unsigned char command[], unsigned int csize)
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

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper_transfer (suunto_vyper_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size)
{
	assert (asize >= size + 2);

	// Send the command to the dive computer.
	int rc = suunto_vyper_send (device, command, csize);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Failed to send the command.");
		return rc;
	}

	// Receive the answer of the dive computer.
	rc = serial_read (device->port, answer, asize);
	if (rc != asize) {
		WARNING ("Failed to receive the answer.");
		if (rc == -1)
			return DEVICE_STATUS_IO;
		return DEVICE_STATUS_TIMEOUT;
	}

	// Verify the header of the package.
	if (memcmp (command, answer, asize - size - 1) != 0) {
		WARNING ("Unexpected answer start byte(s).");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Verify the checksum of the package.
	unsigned char crc = answer[asize - 1];
	unsigned char ccrc = checksum_xor_uint8 (answer, asize - 1, 0x00);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size, device_progress_state_t *progress)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	if (! device_is_suunto_vyper (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

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
		command[4] = checksum_xor_uint8 (command, 4, 0x00);
		int rc = suunto_vyper_transfer (device, command, sizeof (command), answer, len + 5, len);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		memcpy (data, answer + 4, len);

#ifndef NDEBUG
		message ("VyperRead(0x%04x,%d)=\"", address, len);
		for (unsigned int i = 0; i < len; ++i) {
			message("%02x", data[i]);
		}
		message("\"\n");
#endif

		progress_event (progress, DEVICE_EVENT_PROGRESS, len);

		nbytes += len;
		address += len;
		data += len;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper_device_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	return suunto_vyper_read (abstract, address, data, size, NULL);
}


static device_status_t
suunto_vyper_device_write (device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	if (! device_is_suunto_vyper (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

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
		if (rc != DEVICE_STATUS_SUCCESS)
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
		wcommand[len + 4] = checksum_xor_uint8 (wcommand, len + 4, 0x00);
		rc = suunto_vyper_transfer (device, wcommand, len + 5, wanswer, sizeof (wanswer), 0);
		if (rc != DEVICE_STATUS_SUCCESS)
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

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_vyper_read_dive (device_t *abstract, unsigned char data[], unsigned int size, int init, device_progress_state_t *progress)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	if (! device_is_suunto_vyper (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Send the command to the dive computer.
	unsigned char command[3] = {init ? 0x08 : 0x09, 0xA5, 0x00};
	command[2] = checksum_xor_uint8 (command, 2, 0x00);
	int rc = suunto_vyper_send (device, command, 3);
	if (rc != DEVICE_STATUS_SUCCESS) {
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
				return DEVICE_STATUS_IO;
			return DEVICE_STATUS_TIMEOUT;
		}

		// Verify the header of the package.
		if (answer[0] != command[0] || 
			answer[1] > SUUNTO_VYPER_PACKET_SIZE) {
			WARNING ("Unexpected answer start byte(s).");
			return DEVICE_STATUS_PROTOCOL;
		}

		// Receive the remaining part of the package.
		unsigned char len = answer[1];
		rc = serial_read (device->port, answer + 2, len + 1);
		if (rc != len + 1) {
			WARNING ("Failed to receive the answer.");
			if (rc == -1)
				return DEVICE_STATUS_IO;
			return DEVICE_STATUS_TIMEOUT;
		}

		// Verify the checksum of the package.
		unsigned char crc = answer[len + 2];
		unsigned char ccrc = checksum_xor_uint8 (answer, len + 2, 0x00);
		if (crc != ccrc) {
			WARNING ("Unexpected answer CRC.");
			return DEVICE_STATUS_PROTOCOL;
		}

		// Append the package to the output buffer.
		if (nbytes + len <= size) {
			memcpy (data + nbytes, answer + 2, len);
			nbytes += len;
		} else {
			WARNING ("Insufficient buffer space available.");
			return DEVICE_STATUS_MEMORY;
		}

		// The DC sends a null package (a package with length zero) when it 
		// has reached the end of its internal ring buffer. From this point on, 
		// the current dive has been overwritten with newer data. Therefore, 
		// we discard the current (incomplete) dive and end the transmission.
		if (len == 0) {
			WARNING ("Null package received.");
#ifndef NDEBUG
			array_reverse_bytes (data, nbytes);
			message ("Vyper%sProfile=\"", init ? "First" : "");
			for (unsigned int i = 0; i < nbytes; ++i) {
				message("%02x", data[i]);
			}
			message("\"\n");
#endif
			return 0;
		}

		progress_event (progress, DEVICE_EVENT_PROGRESS, len);

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
	array_reverse_bytes (data, nbytes);

#ifndef NDEBUG
	message ("Vyper%sProfile=\"", init ? "First" : "");
	for (unsigned int i = 0; i < nbytes; ++i) {
		message("%02x", data[i]);
	}
	message("\"\n");
#endif

	return nbytes;
}


device_status_t
suunto_vyper_device_read_dive (device_t *abstract, unsigned char data[], unsigned int size, int init)
{
	return suunto_vyper_read_dive (abstract, data, size, init, NULL);
}


static device_status_t
suunto_vyper_device_dump (device_t *abstract, unsigned char data[], unsigned int size)
{
	if (! device_is_suunto_vyper (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < SUUNTO_VYPER_MEMORY_SIZE)
		return DEVICE_STATUS_ERROR;

	// Enable progress notifications.
	device_progress_state_t progress;
	progress_init (&progress, abstract, SUUNTO_VYPER_MEMORY_SIZE);

	int rc = suunto_vyper_read (abstract, 0x00, data, SUUNTO_VYPER_MEMORY_SIZE, &progress);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	return SUUNTO_VYPER_MEMORY_SIZE;
}


static device_status_t
suunto_vyper_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	if (! device_is_suunto_vyper (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Enable progress notifications.
	device_progress_state_t progress;
	progress_init (&progress, abstract, SUUNTO_VYPER_MEMORY_SIZE - 0x4C);

	// The memory layout of the Spyder is different from the Vyper
	// (and all other compatible dive computers). The Spyder has
	// the largest ring buffer for the profile memory, so we use
	// that value as the maximum size of the memory buffer.

	unsigned char data[SUUNTO_VYPER_MEMORY_SIZE - 0x4C] = {0};

	int rc = 0;
	unsigned int ndives = 0;
	unsigned int offset = 0;
	while ((rc = suunto_vyper_read_dive (abstract, data + offset, sizeof (data) - offset, (ndives == 0), &progress)) > 0) {
		if (callback && !callback (data + offset, rc, userdata))
			return DEVICE_STATUS_SUCCESS;

		ndives++;
		offset += rc;
	}

	if (rc != 0)
		return rc;

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_vyper_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	assert (size >= SUUNTO_VYPER_MEMORY_SIZE);

	unsigned int eop = (data[0x51] << 8) + data[0x52];

	return suunto_common_extract_dives (data, 0x71, SUUNTO_VYPER_MEMORY_SIZE, eop, 5, callback, userdata);
}


device_status_t
suunto_spyder_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	assert (size >= SUUNTO_VYPER_MEMORY_SIZE);

	unsigned int eop = (data[0x1C] << 8) + data[0x1D];

	return suunto_common_extract_dives (data, 0x4C, SUUNTO_VYPER_MEMORY_SIZE, eop, 3, callback, userdata);
}


static const device_backend_t suunto_vyper_device_backend = {
	DEVICE_TYPE_SUUNTO_VYPER,
	NULL, /* handshake */
	NULL, /* version */
	suunto_vyper_device_read, /* read */
	suunto_vyper_device_write, /* write */
	suunto_vyper_device_dump, /* dump */
	suunto_vyper_device_foreach, /* foreach */
	suunto_vyper_device_close /* close */
};
