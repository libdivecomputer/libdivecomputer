#include <stdio.h>	// fprintf
#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free

#include "suunto.h"
#include "serial.h"

#define MIN(a,b)	(((a) < (b)) ? (a) : (b))
#define MAX(a,b)	(((a) > (b)) ? (a) : (b))


struct vyper {
	struct serial *port;
	int extraanswertime;
	int ifacealwaysechos;
	int breakprofreadearly;
};


int
suunto_vyper_open (vyper **out, const char* name)
{
	if (out == NULL)
		return -1;

	// Allocate memory.
	struct vyper *device = malloc (sizeof (struct vyper));
	if (device == NULL) {
		fprintf (stderr, "%s:%d: Out of memory.\n", __FILE__, __LINE__);
		return -1;
	}

	// Set the default values.
	device->port = NULL;
	device->extraanswertime = 0;
	device->ifacealwaysechos = 0;
	device->breakprofreadearly = 0;

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		fprintf (stderr, "%s:%d: Can't open serial port %s.\n", __FILE__, __LINE__, name);
		free (device);
		return -1;
	}

	// Set the serial communication protocol (2400 8O1).
	rc = serial_configure (device->port, 2400, 8, SERIAL_PARITY_ODD, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		fprintf (stderr, "%s:%d: Failed to set terminal attributes on %s.\n", __FILE__, __LINE__, name);
		serial_close (device->port);
		free (device);
		return -1;
	}

	// Set the timeout for receiving data (500 ms).
	if (serial_set_timeout (device->port, 500) == -1) {
		fprintf (stderr, "%s:%d: Failed to set timeout on %s.\n", __FILE__, __LINE__, name);
		serial_close (device->port);
		free (device);
		return -1;
	}

	// Set the DTR line (power supply for the interface) and clear
	// the RTS line (toggle for the direction of the half-duplex interface).
	if (serial_set_dtr (device->port, 1) == -1 ||
		serial_set_rts (device->port, 0) == -1) {
		fprintf (stderr, "%s:%d: Error setting RTS/DTR state on %s.\n", __FILE__, __LINE__, name);
		serial_close (device->port);
		free (device);
		return -1;
	}

	// Give the interface 100 ms to settle and draw power up.
	serial_sleep (100);

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = device;

	return 0;
}


int
suunto_vyper_close (vyper *device)
{
	if (device == NULL)
		return 0;

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return -1;
	}

	// Free memory.	
	free (device);

	return 0;
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
		fprintf (stderr, "%s:%d: Trouble sending test sequence to interface.\n", __FILE__, __LINE__);
		return -1;
	}
	serial_drain (device->port);
	return 0;
}


int
suunto_vyper_detect_interface (vyper *device)
{
	int rc, detectmode_worked = 1;
	unsigned char command[3] = {'A', 'T', '\r'}, reply[3] = {0}, extra = 0;

	if (device == NULL)
		return -1;

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	// Charge power supply?
	serial_set_rts (device->port, 1);
	serial_sleep (300);

	// Try detection mode first.

	serial_set_rts (device->port, 0);
	if (suunto_vyper_send_testcmd (device, command, 3) == -1)
		return -1;	// Failed to send? Huh?
	rc = serial_read (device->port, reply, 3);
	if (rc != 3 || memcmp (command, reply, 3) != 0) {
    	fprintf (stderr, "%s:%d: Interface not responding in probe mode.\n", __FILE__, __LINE__);
    	detectmode_worked = 0;
	}
	if (serial_read (device->port, &extra, 1) == 1) {
		fprintf (stderr, "%s:%d: Got extraneous character %02x in detection phase - maybe line is connected to a modem?\n", __FILE__, __LINE__, extra);
	}

	// Try transfer mode now.

	serial_set_rts (device->port, 1);
	if (suunto_vyper_send_testcmd (device, command, 3) == -1)
		return -1;	// Failed to send? Huh?
	serial_set_rts (device->port, 0);
	rc = serial_read (device->port, reply, 3);
	if (rc == 0) {
		if (detectmode_worked) {
			fprintf (stderr, "%s:%d: Detected original suunto interface with RTS-switching.\n", __FILE__, __LINE__);
		} else {
			fprintf (stderr, "%s:%d: Can't detect Interface.\n"
					"Hoping it's an original suunto interface with DC already attached.\n", __FILE__, __LINE__);
		}
		device->ifacealwaysechos = 0;
		return 0;
	}
	if (rc != 3 || memcmp (command, reply, 3) != 0) {
    	fprintf (stderr, "%s:%d: Interface not responding when RTS is on. Strange.\n", __FILE__, __LINE__);
	}
	if (serial_read (device->port, &extra, 1) == 1) {
		fprintf (stderr, "%s:%d: Got extraneous character %02x in detection phase - maybe line is connected to a modem?\n", __FILE__, __LINE__, extra);
	}
	fprintf (stderr, "%s:%d: Detected clone interface without RTS-switching.\n", __FILE__, __LINE__);
	device->ifacealwaysechos = 1;
	return 0;
}


static int
suunto_vyper_send_command (vyper *device, const unsigned char* data, unsigned int size)
{
	// Send the command to the dive computer.
	serial_sleep (500);
	serial_set_rts (device->port, 1);
	serial_write (device->port, data, size);
	serial_drain (device->port);
	serial_sleep (200);
	serial_set_rts (device->port, 0);

	// If the interface sends an echo back (which is the case for many clone 
	// interfaces), those echos should be removed from the input queue before 
	// reading the real reply from the dive computer. Otherwise, the data 
	// transfer will fail.
	if (device->ifacealwaysechos) {
		// Echos should be there instantly.
		unsigned char echo[37] = {0}; // An echo is maximum 37 bytes long.
		int rc = serial_read (device->port, echo, size);
		if (rc != size || memcmp (data, echo, size) != 0) {
			if (rc == 0) 
				fprintf (stderr, "%s:%d: Timeout waiting for echos.\n", __FILE__, __LINE__);
			else
				fprintf (stderr, "%s:%d: Echo incorrect.\nMaybe another device is listening on this port?\n", __FILE__, __LINE__);
			return -1;
		}
	}

	return 0;
}


static int
suunto_vyper_read_memory_package (vyper *device, unsigned int address, unsigned char data[], unsigned int size)
{
	// Prepare the command.
	unsigned char command[5] = {0x05,
			(address >> 8) & 0xFF, // high
			(address     ) & 0xFF, // low
			size, // count
			0x00};  // CRC
	command[4] = suunto_vyper_checksum (command, 4, 0x00);

	// Send the command to the dive computer.
	if (suunto_vyper_send_command (device, command, 5) != 0) {
		fprintf (stderr, "%s:%d: Error sending command.\n", __FILE__, __LINE__);
		return -1;
	}

	// FIXME: Give the DC extra answer time to send its first byte, 
	//        then let the standard timeout apply.

	// Receive the header of the package.
	unsigned char reply[4] = {0, 0, 0, 0};
	int rc = serial_read (device->port, reply, 4);
	if (rc != 4 || memcmp (command, reply, 4) != 0) {
		fprintf (stderr, "%s:%d: Reply to read memory malformed (expected %02x%02x%02x%02x, got %02x%02x%02x%02x).\n",
			__FILE__, __LINE__,
			command[0], command[1], command[2], command[3],
			reply[0], reply[1], reply[2], reply[3]);
		if (rc == 0) {
			fprintf (stderr, "%s:%d: Interface present, but DC does not answer. Check connection.\n", __FILE__, __LINE__);
		}
		return -1;
	}

	// Receive the contents of the package.
	rc = serial_read (device->port, data, size);
	if (rc != size) {
		fprintf (stderr, "%s:%d: Unexpected EOF in answer.\n", __FILE__, __LINE__);
		return -1;
	}

	// Calculate the checksum.
	unsigned char ccrc = 0x00;
	ccrc = suunto_vyper_checksum (reply, 4, ccrc);
	ccrc = suunto_vyper_checksum (data, size, ccrc);

	// Receive (and verify) the checksum of the package.
	unsigned char crc = 0x00;
	rc = serial_read (device->port, &crc, 1);
	if (rc != 1 || ccrc != crc) {
		fprintf (stderr, "%s:%d: Reply failed CRC check. Line noise?\n", __FILE__, __LINE__);
		return -1;
	}

#ifndef NDEBUG
	printf ("VyperRead(0x%04x,%d)=\"", (command[1] << 8) | command[2], command[3]);
	for (unsigned int i = 0; i < size; ++i) {
		printf("%02x",data[i]);
	}
	printf("\"\n");
#endif

	return 0;
}


static int
suunto_vyper_write_memory_prepare (vyper *device)
{
	// Prepare the commmand.
	unsigned char command[3] = {0x07, 0xA5, 0x00};
	command[2] = suunto_vyper_checksum (command, 2, 0x00);

	// Send the command to the dive computer.
	if (suunto_vyper_send_command (device, command, 3) != 0) {
		fprintf (stderr, "%s:%d: Error sending command.\n", __FILE__, __LINE__);
		return -1;
	}

	// FIXME: Give the DC extra answer time to send its first byte, 
	//        then let the standard timeout apply.

	// Receive the header of the package.
	unsigned char reply[3] = {0, 0, 0};
	int rc = serial_read (device->port, reply, 3);
	if (rc != 3 || memcmp (command, reply, 3) != 0) {
		fprintf (stderr, "%s:%d: Reply to prepare write memory malformed (expected %02x%02x%02x, got %02x%02x%02x).\n",
			__FILE__, __LINE__,
			command[0], command[1], command[2],
			reply[0], reply[1], reply[2]);
		if (rc == 0) {
			fprintf (stderr, "%s:%d: Interface present, but DC does not answer. Check connection.\n", __FILE__, __LINE__);
		}
		return -1;
	}

#ifndef NDEBUG
	printf("VyperPrepareWrite();\n");
#endif

	return 0;
}


static int
suunto_vyper_write_memory_package (vyper *device, int address, const unsigned char data[], unsigned int size)
{
	// Prepare the command.
	unsigned char command[37] = {0x06,
			(address >> 8) & 0xFF, // high
			(address     ) & 0xFF, // low
			size, // count
			0x00};  // data + CRC
	memcpy (command + 4, data, size);
	command[size + 4] = suunto_vyper_checksum (command, size + 4, 0x00);

	// Send the command to the dive computer.
	if (suunto_vyper_send_command (device, command, size + 5) != 0) {
		fprintf (stderr, "%s:%d: Error sending command.\n", __FILE__, __LINE__);
		return -1;
	}

	// FIXME: Give the DC extra answer time to send its first byte, 
	//        then let the standard timeout apply.

	// Receive the header of the package.
	unsigned char reply[4] = {0, 0, 0, 0};
	int rc = serial_read (device->port, reply, 4);
	if (rc != 4 || memcmp (command, reply, 4) != 0) {
		fprintf (stderr, "%s:%d: Reply to write memory malformed (expected %02x%02x%02x%02x, got %02x%02x%02x%02x).\n",
			__FILE__, __LINE__,
			command[0], command[1], command[2], command[3],
			reply[0], reply[1], reply[2], reply[3]);
		if (rc == 0) {
			fprintf (stderr, "%s:%d: Interface present, but DC does not answer. Check connection.\n", __FILE__, __LINE__);
		}
		return -1;
	}

	// Calculate the checksum.
	unsigned char ccrc = 0x00;
	ccrc = suunto_vyper_checksum (reply, 4, ccrc);

	// Receive (and verify) the checksum of the package.
	unsigned char crc = 0x00;
	rc = serial_read (device->port, &crc, 1);
	if (rc != 1 || ccrc != crc) {
		fprintf (stderr, "%s:%d: Reply failed CRC check. Line noise?\n", __FILE__, __LINE__);
		return -1;
	}

#ifndef NDEBUG
	printf ("VyperWrite(0x%04x,%d,\"", (command[1] << 8) | command[2], command[3]);
	for (unsigned int i = 0; i < size; ++i) {
		printf ("%02x", command[i + 4]);
	}
	printf ("\");\n");
#endif

	return 0;
}


int
suunto_vyper_read_memory (vyper *device, unsigned int address, unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return -1;

	// The data transmission is split in packages
	// of maximum $SUUNTO_VYPER_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = MIN (size - nbytes, SUUNTO_VYPER_PACKET_SIZE);

		// Read the package.
		if (suunto_vyper_read_memory_package (device, address + nbytes, data + nbytes, len) != 0)
			return -1;

		// Increment the total number of bytes read.
		nbytes += len;
	}

	return 0;
}


int
suunto_vyper_write_memory (vyper *device, unsigned int address, const unsigned char data[], unsigned int size)
{
	if (device == NULL)
		return -1;

	// The data transmission is split in packages
	// of maximum $SUUNTO_VYPER_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = MIN (size - nbytes, SUUNTO_VYPER_PACKET_SIZE);

		// Prepare to write the package.
		if (suunto_vyper_write_memory_prepare (device) != 0)
			return -1;

		// Write the package.
		if (suunto_vyper_write_memory_package (device, address + nbytes, data + nbytes, len) != 0)
			return -1;

		// Increment the total number of bytes written.
		nbytes += len;
	}

	return 0;
}


int
suunto_vyper_read_dive (vyper *device, unsigned char data[], unsigned int size, int init)
{
	if (device == NULL)
		return -1;

	// Prepare the command.
	unsigned char command[3] = {init ? 0x08 : 0x09, 0xA5, 0x00};
	command[2] = suunto_vyper_checksum (command, 2, 0x00);

	// Send the command to the dive computer.
	if (suunto_vyper_send_command (device, command, 3) != 0) {
		fprintf (stderr, "%s:%d: Error sending command.\n", __FILE__, __LINE__);
		return -1;
	}

	// FIXME: Give the DC extra answer time to send its first byte, 
	//        then let the standard timeout apply.

	// The data transmission is split in packages
	// of maximum $SUUNTO_VYPER_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	for (;;) {
		// Receive the header of the package.
		unsigned char reply = 0;
		int rc = serial_read (device->port, &reply, 1);
		if (rc != 1 || memcmp (command, &reply, 1) != 0) {
			// If no data is received at this point (a timeout occured), 
			// we assume the last package was already received and the 
			// transmission should be finished. This is not 100% reliable, 
			// because there is always a small chance that more data will 
			// arrive later (especially with a short timeout). But I'm not 
			// aware of a better method to detect the end of the transmission. 
			// Only for the very first package, we can be sure there was
			// an error, because it makes no sense to end the transmission 
			// if no data was received so far.
			if (rc == 0 && nbytes != 0)
				break;
			fprintf (stderr, "%s:%d: Unexpected answer start byte (%d).\n", __FILE__, __LINE__, reply);
			return -1;
		}

		// Receive the size of the package.
		unsigned char len = 0;
		rc = serial_read (device->port, &len, 1);
		if (rc != 1 || len > SUUNTO_VYPER_PACKET_SIZE) {
			fprintf (stderr, "%s:%d: Unexpected answer length (%d).\n", __FILE__, __LINE__, len);
			return -1;
		}

		// Receive the contents of the package.
		unsigned char package[SUUNTO_VYPER_PACKET_SIZE] = {0};
		rc = serial_read (device->port, package, len);
		if (rc != len) {
			fprintf (stderr, "%s:%d: Unexpected EOF in answer.\n", __FILE__, __LINE__);
			return -1;
		}

		// Calculate the checksum.
		unsigned char ccrc = 0x00;
		ccrc = suunto_vyper_checksum (&reply, 1, ccrc);
		ccrc = suunto_vyper_checksum (&len, 1, ccrc);
		ccrc = suunto_vyper_checksum (package, len, ccrc);

		// Receive (and verify) the checksum of the package.
		unsigned char crc = 0x00;
		rc = serial_read (device->port, &crc, 1);
		if (rc != 1 || ccrc != crc) {
			fprintf (stderr, "%s:%d: Unexpected answer CRC (%d).\n", __FILE__, __LINE__, crc);
			return -1;
		}

		// Append the package to the output buffer.
		if (nbytes + len <= size) {
			memcpy (data + nbytes, package, len);
			nbytes += len;
		} else {
			fprintf (stderr, "%s:%d: Insufficient buffer space available.\n", __FILE__, __LINE__);
			return -1;
		}

		// If a package is smaller than $SUUNTO_VYPER_PACKET_SIZE bytes, 
		// we assume it's the last packet and the transmission can be 
		// finished early. However, this approach does not work if the 
		// last packet is exactly $SUUNTO_VYPER_PACKET_SIZE bytes long!
		if (device->breakprofreadearly && len != SUUNTO_VYPER_PACKET_SIZE) 
			break;
	}

	// The DC traverses the internal ring buffer backwards while sending. 
	// Therefore, we restore the original order before returning the 
	// data to the application.
	suunto_vyper_reverse (data, nbytes);

#ifndef NDEBUG
	printf ("Vyper%sProfile=\"", command[0] == 0x08 ? "First" : "");
	for (unsigned int i = 0; i < nbytes; ++i) {
		printf("%02x", data[i]);
	}
	printf("\"\n");
#endif

	return nbytes;
}
