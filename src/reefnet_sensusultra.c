#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <assert.h>

#include "device-private.h"
#include "reefnet_sensusultra.h"
#include "serial.h"
#include "checksum.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

#define PROMPT 0xA5
#define ACCEPT PROMPT
#define REJECT 0x00

typedef struct reefnet_sensusultra_device_t reefnet_sensusultra_device_t;

struct reefnet_sensusultra_device_t {
	device_t base;
	struct serial *port;
	unsigned int maxretries;
	unsigned int timestamp;
};

static device_status_t reefnet_sensusultra_device_handshake (device_t *abstract, unsigned char *data, unsigned int size);
static device_status_t reefnet_sensusultra_device_dump (device_t *abstract, unsigned char *data, unsigned int size, unsigned int *result);
static device_status_t reefnet_sensusultra_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t reefnet_sensusultra_device_close (device_t *abstract);

static const device_backend_t reefnet_sensusultra_device_backend = {
	DEVICE_TYPE_REEFNET_SENSUSULTRA,
	reefnet_sensusultra_device_handshake, /* handshake */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	reefnet_sensusultra_device_dump, /* dump */
	reefnet_sensusultra_device_foreach, /* foreach */
	reefnet_sensusultra_device_close /* close */
};

static int
device_is_reefnet_sensusultra (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &reefnet_sensusultra_device_backend;
}


device_status_t
reefnet_sensusultra_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t *) malloc (sizeof (reefnet_sensusultra_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &reefnet_sensusultra_device_backend);

	// Set the default values.
	device->port = NULL;
	device->maxretries = 2;
	device->timestamp = 0;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (115200 8N1).
	rc = serial_configure (device->port, 115200, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (3000ms).
	if (serial_set_timeout (device->port, 3000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_device_close (device_t *abstract)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
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


device_status_t
reefnet_sensusultra_device_set_maxretries (device_t *abstract, unsigned int maxretries)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	device->maxretries = maxretries;

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensusultra_device_set_timestamp (device_t *abstract, unsigned int timestamp)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	device->timestamp = timestamp;

	return DEVICE_STATUS_SUCCESS;
}


static int
reefnet_sensusultra_isempty (const unsigned char *data, unsigned int size)
{
	for (unsigned int i = 0; i < size; ++i) {
		if (data[i] != 0xFF)
			return 0;
	}

	return 1;
}


static device_status_t
reefnet_sensusultra_send_uchar (reefnet_sensusultra_device_t *device, unsigned char value)
{
	// Wait for the prompt byte.
	unsigned char prompt = 0;
	int rc = serial_read (device->port, &prompt, 1);
	if (rc != 1) {
		WARNING ("Failed to receive the prompt byte");
		return EXITCODE (rc);
	}

	// Verify the prompt byte.
	if (prompt != PROMPT) {
		WARNING ("Unexpected answer data.");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Send the value to the device.
	rc = serial_write (device->port, &value, 1);
	if (rc != 1) {
		WARNING ("Failed to send the value.");
		return EXITCODE (rc);
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_send_ushort (reefnet_sensusultra_device_t *device, unsigned short value)
{
	// Send the least-significant byte.
	unsigned char lsb = value & 0xFF;
	device_status_t rc = reefnet_sensusultra_send_uchar (device, lsb);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Send the most-significant byte.
	unsigned char msb = (value >> 8) & 0xFF;
	rc = reefnet_sensusultra_send_uchar (device, msb);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_packet (reefnet_sensusultra_device_t *device, unsigned char *data, unsigned int size, unsigned int header)
{
	assert (size >= header + 2);

	// Receive the data packet.
	int rc = serial_read (device->port, data, size);
	if (rc != size) {
		WARNING ("Failed to receive the packet.");
		return EXITCODE (rc);
	}

	// Verify the checksum of the packet.
	unsigned short crc = data[size - 2] + (data[size - 1] << 8);
	unsigned short ccrc = checksum_crc_ccitt_uint16 (data + header, size - header - 2);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_device_handshake (device_t *abstract, unsigned char *data, unsigned int size)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Flush the input and output buffers.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	device_status_t rc = DEVICE_STATUS_SUCCESS;
	unsigned int nretries = 0;
	unsigned char handshake[REEFNET_SENSUSULTRA_HANDSHAKE_SIZE + 2] = {0};
	while ((rc = reefnet_sensusultra_packet (device, handshake, sizeof (handshake), 0)) != DEVICE_STATUS_SUCCESS) {
		// Automatically discard a corrupted handshake packet, 
		// and wait for the next one.
		if (rc != DEVICE_STATUS_PROTOCOL)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= device->maxretries)
			return rc;

		// According to the developers guide, a 250 ms delay is suggested to
		// guarantee that the prompt byte sent after the handshake packet is 
		// not accidentally buffered by the host and (mis)interpreted as part 
		// of the next packet.

		serial_sleep (250);
		serial_flush (device->port, SERIAL_QUEUE_BOTH);
	}

#ifndef NDEBUG
	message (
		"Version:    %u\n"
		"Serial:     %u\n"
		"Time:       %u\n"
		"Boot Count: %u\n"
		"Boot Time:  %u\n"
		"Dive Count: %u\n"
		"Interval:   %u\n"
		"Threshold:  %u\n"
		"End Count:  %u\n"
		"Averaging:  %u\n",
		handshake[0] + (handshake[1] << 8),
		handshake[2] + (handshake[3] << 8),
		handshake[4] + (handshake[5] << 8) + (handshake[6] << 16) + (handshake[7] << 24),
		handshake[8] + (handshake[9] << 8),
		handshake[10] + (handshake[11] << 8) + (handshake[12] << 16) + (handshake[13] << 24),
		handshake[14] + (handshake[15] << 8),
		handshake[16] + (handshake[17] << 8),
		handshake[18] + (handshake[19] << 8),
		handshake[20] + (handshake[21] << 8),
		handshake[22] + (handshake[23] << 8));
#endif

	if (size >= REEFNET_SENSUSULTRA_HANDSHAKE_SIZE) {
		memcpy (data, handshake, REEFNET_SENSUSULTRA_HANDSHAKE_SIZE);
	} else {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_page (reefnet_sensusultra_device_t *device, unsigned char *data, unsigned int size, unsigned int pagenum)
{
	if (device == NULL)
		return DEVICE_STATUS_ERROR;

	device_status_t rc = DEVICE_STATUS_SUCCESS;
	unsigned int nretries = 0;
	unsigned char package[REEFNET_SENSUSULTRA_PACKET_SIZE + 4] = {0};
	while ((rc = reefnet_sensusultra_packet (device, package, sizeof (package), 2)) != DEVICE_STATUS_SUCCESS) {
		// Automatically discard a corrupted packet, 
		// and request a new one.
		if (rc != DEVICE_STATUS_PROTOCOL)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= device->maxretries)
			return rc;

		// Reject the packet.
		rc = reefnet_sensusultra_send_uchar (device, REJECT);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;
	}

	// Verify the page number.
	unsigned int page = package[0] + (package[1] << 8);
	if (page != pagenum) {
		WARNING ("Unexpected page number."); 
		return DEVICE_STATUS_PROTOCOL;
	}

	if (size >= REEFNET_SENSUSULTRA_PACKET_SIZE) {
		memcpy (data, package + 2, REEFNET_SENSUSULTRA_PACKET_SIZE);
	} else {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_device_dump (device_t *abstract, unsigned char *data, unsigned int size, unsigned int *result)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE)
		return DEVICE_STATUS_ERROR;

	// Enable progress notifications.
	device_progress_state_t progress;
	progress_init (&progress, abstract, REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE);

	// Send the instruction code to the device.
	device_status_t rc = reefnet_sensusultra_send_ushort (device, 0xB421);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	unsigned int nbytes = 0;
	unsigned int npages = 0;
	while (nbytes < REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE) {
		// Receive the packet.
		unsigned int offset = REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE - 
			nbytes - REEFNET_SENSUSULTRA_PACKET_SIZE;
		rc = reefnet_sensusultra_page (device, data + offset, REEFNET_SENSUSULTRA_PACKET_SIZE, npages);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		progress_event (&progress, DEVICE_EVENT_PROGRESS, REEFNET_SENSUSULTRA_PACKET_SIZE);

		// Accept the packet.
		rc = reefnet_sensusultra_send_uchar (device, ACCEPT);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		nbytes += REEFNET_SENSUSULTRA_PACKET_SIZE;
		npages++;
	}

	if (result)
		*result = REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE;

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensusultra_device_read_user (device_t *abstract, unsigned char *data, unsigned int size)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < REEFNET_SENSUSULTRA_MEMORY_USER_SIZE)
		return DEVICE_STATUS_ERROR;

	// Send the instruction code to the device.
	device_status_t rc = reefnet_sensusultra_send_ushort (device, 0xB420);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	unsigned int nbytes = 0;
	unsigned int npages = 0;
	while (nbytes < REEFNET_SENSUSULTRA_MEMORY_USER_SIZE) {
		// Receive the packet.
		rc = reefnet_sensusultra_page (device, data + nbytes, REEFNET_SENSUSULTRA_PACKET_SIZE, npages);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		// Accept the packet.
		rc = reefnet_sensusultra_send_uchar (device, ACCEPT);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		nbytes += REEFNET_SENSUSULTRA_PACKET_SIZE;
		npages++;
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensusultra_device_write_user (device_t *abstract, const unsigned char *data, unsigned int size)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	assert (size >= REEFNET_SENSUSULTRA_MEMORY_USER_SIZE);

	// Send the instruction code to the device.
	device_status_t rc = reefnet_sensusultra_send_ushort (device, 0xB430);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Send the data to the device.
	for (unsigned int i = 0; i < REEFNET_SENSUSULTRA_MEMORY_USER_SIZE; ++i) {
		rc = reefnet_sensusultra_send_uchar (device, data[i]);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;
	}

	// Send the checksum to the device.
	unsigned short crc = checksum_crc_ccitt_uint16 (data, REEFNET_SENSUSULTRA_MEMORY_USER_SIZE);
	rc = reefnet_sensusultra_send_ushort (device, crc);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_write_internal (device_t *abstract, unsigned int code, unsigned int value)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Send the instruction code to the device.
	device_status_t rc = reefnet_sensusultra_send_ushort (device, code);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Send the new value to the device.
	rc = reefnet_sensusultra_send_ushort (device, value);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensusultra_device_write_interval (device_t *abstract, unsigned int value)
{
	if (value < 1 || value > 65535)
		return DEVICE_STATUS_ERROR;

	return reefnet_sensusultra_write_internal (abstract, 0xB410, value);
}


device_status_t
reefnet_sensusultra_device_write_threshold (device_t *abstract, unsigned int value)
{
	if (value < 1 || value > 65535)
		return DEVICE_STATUS_ERROR;

	return reefnet_sensusultra_write_internal (abstract, 0xB411, value);
}


device_status_t
reefnet_sensusultra_device_write_endcount (device_t *abstract, unsigned int value)
{
	if (value < 1 || value > 65535)
		return DEVICE_STATUS_ERROR;

	return reefnet_sensusultra_write_internal (abstract, 0xB412, value);
}


device_status_t
reefnet_sensusultra_device_write_averaging (device_t *abstract, unsigned int value)
{
	if (value != 1 && value != 2 && value != 4)
		return DEVICE_STATUS_ERROR;

	return reefnet_sensusultra_write_internal (abstract, 0xB413, value);
}


device_status_t
reefnet_sensusultra_device_sense (device_t *abstract, unsigned char *data, unsigned int size)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Send the instruction code to the device.
	device_status_t rc = reefnet_sensusultra_send_ushort (device, 0xB440);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Receive the packet.
	unsigned char package[REEFNET_SENSUSULTRA_SENSE_SIZE + 2] = {0};
	rc = reefnet_sensusultra_packet (device, package, sizeof (package), 0);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	if (size >= REEFNET_SENSUSULTRA_SENSE_SIZE) {
		memcpy (data, package, REEFNET_SENSUSULTRA_SENSE_SIZE);
	} else {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_parse (const unsigned char data[], unsigned int begin, unsigned int end, unsigned int *pprevious,
	int *aborted, dive_callback_t callback, void *userdata, unsigned int timestamp)
{
	const unsigned char header[4] = {0x00, 0x00, 0x00, 0x00};
	const unsigned char footer[4] = {0xFF, 0xFF, 0xFF, 0xFF};

	// Initialize the data stream offsets.
	unsigned int previous = (pprevious ? *pprevious : end);
	unsigned int current = end;
	if (current + 4 > previous)
		current = (previous >= 4 ? previous - 4 : 0);

	// Search the data stream for start markers.
	while (current > begin) {
		current--;
		if (memcmp (data + current, header, sizeof (header)) == 0) {
			// Once a start marker is found, start searching
			// for the corresponding stop marker. The search is 
			// now limited to the start of the previous dive.
			int found = 0;
			unsigned int offset = current + 16; // Skip non-sample data.
			while (offset + 4 <= previous) {
				if (memcmp (data + offset, footer, sizeof (footer)) == 0) {
					found = 1;
					break;
				} else {
					offset++;
				}
			}

			// Report an error if no stop marker was found.
			if (!found) {
				WARNING ("No stop marker present.");
				return DEVICE_STATUS_ERROR;
			}

			// Automatically abort when a dive is older than the provided timestamp.
			unsigned int datetime = data[current + 4] + (data[current + 5] << 8) + 
				(data[current + 6] << 16) + (data[current + 7] << 24);
			if (datetime <= timestamp) {
				if (aborted)
					*aborted = 1;
				return DEVICE_STATUS_SUCCESS;
			}

			if (callback && !callback (data + current, offset + 4 - current, userdata)) {
				if (aborted)
					*aborted = 1;
				return DEVICE_STATUS_SUCCESS;
			}

			// Prepare for the next dive.
			previous = current;
			current = (previous >= 4 ? previous - 4 : 0);
		}
	}

	// Return the offset to the last dive.
	if (pprevious)
		*pprevious = previous;

	if (aborted)
		*aborted = 0;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
reefnet_sensusultra_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	reefnet_sensusultra_device_t *device = (reefnet_sensusultra_device_t*) abstract;

	if (! device_is_reefnet_sensusultra (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	unsigned char *data = (unsigned char *) malloc (REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE * sizeof (unsigned char));
	if (data == NULL) {
		WARNING ("Memory allocation error.");
		return DEVICE_STATUS_MEMORY;
	}

	// Enable progress notifications.
	device_progress_state_t progress;
	progress_init (&progress, abstract, REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE);

	// Initialize the state for the parsing code.
	unsigned int previous = REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE;

	// Send the instruction code to the device.
	device_status_t rc = reefnet_sensusultra_send_ushort (device, 0xB421);
	if (rc != DEVICE_STATUS_SUCCESS) {
		free (data);
		return rc;
	}

	unsigned int nbytes = 0;
	unsigned int npages = 0;
	while (nbytes < REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE) {
		// Receive the packet.
		unsigned int index = REEFNET_SENSUSULTRA_MEMORY_DATA_SIZE - 
			nbytes - REEFNET_SENSUSULTRA_PACKET_SIZE;
		rc = reefnet_sensusultra_page (device, data + index, REEFNET_SENSUSULTRA_PACKET_SIZE, npages);
		if (rc != DEVICE_STATUS_SUCCESS) {
			free (data);
			return rc;
		}

		progress_event (&progress, DEVICE_EVENT_PROGRESS, REEFNET_SENSUSULTRA_PACKET_SIZE);

		// Abort the transfer if the page contains no useful data.
		if (reefnet_sensusultra_isempty (data + index, REEFNET_SENSUSULTRA_PACKET_SIZE))
			break;

		// Parse the page data.
		int aborted = 0;
		rc = reefnet_sensusultra_parse (data, index, index + REEFNET_SENSUSULTRA_PACKET_SIZE, &previous, &aborted, callback, userdata, device->timestamp);
		if (rc != DEVICE_STATUS_SUCCESS) {
			free (data);
			return rc;
		}
		if (aborted)
			break;

		// Accept the packet.
		rc = reefnet_sensusultra_send_uchar (device, ACCEPT);
		if (rc != DEVICE_STATUS_SUCCESS) {
			free (data);
			return rc;
		}

		nbytes += REEFNET_SENSUSULTRA_PACKET_SIZE;
		npages++;
	}

	free (data);

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
reefnet_sensusultra_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata, unsigned int timestamp)
{
	return reefnet_sensusultra_parse (data, 0, size, NULL, NULL, callback, userdata, timestamp);
}
