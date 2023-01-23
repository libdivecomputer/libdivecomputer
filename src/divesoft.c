/*
 * libdivecomputer
 *
 * Copyright (C) 2021 Ryan Gardner, Jef Driesen
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "divesoft.h"
#include "context-private.h"
#include "device-private.h"
#include "platform.h"
#include "checksum.h"
#include "array.h"

#define THUMBPRINT_SIZE 20
#define MAXDATA 256

typedef struct divesoft_device_t {
    dc_device_t base;
    dc_iostream_t *iostream;
    unsigned char fingerprint[THUMBPRINT_SIZE];
    unsigned char last_data[MAXDATA]; // buffer of received raw data where packets are extracted from
    size_t last_data_pos;
    size_t last_data_size;
    unsigned int model; // model
    unsigned int serial; // serial number
    unsigned int firmware; // firmware version
} divesoft_device_t;

static dc_status_t divesoft_device_set_fingerprint (dc_device_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t divesoft_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);

static const dc_device_vtable_t divesoft_device_vtable = {
    sizeof(divesoft_device_t),
    DC_FAMILY_DIVESOFT,
    divesoft_device_set_fingerprint, /* set_fingerprint */
    NULL, /* read */
    NULL, /* write */
    NULL, /* dump */
    divesoft_device_foreach, /* foreach */
    NULL, /* timesync */
    NULL, /* close */
};

typedef enum divesoft_message_t {
    MSG_ECHO = 0,
    MSG_RESULT = 1,
    MSG_CONNECT = 2,
    MSG_CONNECTED = 3,
    MSG_GET_DIVE_DATA = 64,
    MSG_DIVE_DATA = 65,
    MSG_GET_DIVE_LIST = 66,
    MSG_DIVE_LIST_V1 = 67,
    MSG_DIVE_LIST_V2 = 71,
} divesoft_message_t;

typedef struct divesoft_packet_t {
    char packet_id;
    char options;
    unsigned short message;
    unsigned short length;
    char data[MAXDATA];
    unsigned short checksum;
} divesoft_packet_t;

static unsigned short
divesoft_checksum_crc16_ccitt (const unsigned char data[], unsigned int size, unsigned short init)
{
    static const unsigned short crc_ccitt_table[] = {
            0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
            0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
            0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
            0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
            0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
            0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
            0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
            0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
            0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
            0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
            0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
            0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
            0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
            0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
            0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
            0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
            0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
            0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
            0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
            0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
            0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
            0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
            0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
            0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
            0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
            0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
            0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
            0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
            0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
            0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
            0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
            0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78,
    };

    unsigned short crc = init;
    for (unsigned int i = 0; i < size; ++i)
        crc = (crc >> 8) ^ crc_ccitt_table[(crc ^ data[i]) & 0xff];

    return crc;
}

/*
 * Communication layers
 *
 * - [3] application
 * | packet splitted / merged; crc made / checked
 * - [2] packets
 * | raw message generated / parsed for packets
 * - [1] raw message, escaped
 */

// layer 1 functions

#define BLE_CHUNK_SIZE 20
#define BUFFER_SIZE 1024
#define DIVESOFT_FLAG (char)0x7E

static dc_status_t
divesoft_send_raw (divesoft_device_t *device, const char * data, unsigned int size)
{
    if (!data || !size)
        return DC_STATUS_SUCCESS;

    dc_status_t status = DC_STATUS_SUCCESS;
    dc_device_t *abstract = (dc_device_t *) device;

    if (device_is_cancelled (abstract))
        return DC_STATUS_CANCELLED;

    for (unsigned int sub_size; (sub_size = size > BLE_CHUNK_SIZE ? BLE_CHUNK_SIZE : size); data += sub_size, size -= sub_size) {
        status = dc_iostream_write (device->iostream, data, sub_size, NULL);
        if (status != DC_STATUS_SUCCESS) {
            ERROR (abstract->context, "Failed to send data.");
            return status;
        }
    }

    return status;
}

static int
strnchr(const unsigned char * str, int len, unsigned char c) {
    for(int i = 0; i < len; i++) {
        if(str[i] == c) return i;
    }
    return len;
}

// recieves one packet
static dc_status_t
divesoft_recv_raw (divesoft_device_t *device, char ** p_data, unsigned int * p_size)
{
    dc_status_t status = DC_STATUS_SUCCESS;

    // read data until flag
    *p_size = 0;
    size_t buf_size = BUFFER_SIZE;
    *p_data = (char *)realloc(*p_data, buf_size);
    while(1) {
        while(!*p_size && device->last_data_size != device->last_data_pos && device->last_data[device->last_data_pos] == DIVESOFT_FLAG) {
            // skip all flags that are outright in the beginning
            device->last_data_pos++;
        }
        // copy data that we have, until we encounter flag
        int flag_distance = strnchr(device->last_data + device->last_data_pos, device->last_data_size - device->last_data_pos, DIVESOFT_FLAG);
        while(*p_size + flag_distance > buf_size) {
            buf_size *= 2;
            *p_data = (char *)realloc(*p_data, buf_size);
        }
        memcpy(*p_data + *p_size, device->last_data + device->last_data_pos, flag_distance);
        *p_size += flag_distance;
        device->last_data_pos += flag_distance;
        if(device->last_data_size != device->last_data_pos) {
            // we are done, whole 1 packet was read
            return status;
        }
        // otherwise, we need to read another part
        for(int i = 0; dc_iostream_poll(device->iostream, 100) == DC_STATUS_TIMEOUT; i++) {
            if(device_is_cancelled((dc_device_t *)device)) {
                return DC_STATUS_CANCELLED;
            }
            //dc_iostream_purge (device->iostream, DC_DIRECTION_INPUT);
            DEBUG(device->base.context, "Waiting for response, has %u data", *p_size);
            if(i == 20) {
                ERROR(device->base.context, "No response, giving up...");
                return DC_STATUS_TIMEOUT;
            }
        }
        status = dc_iostream_read(device->iostream, device->last_data, BUFFER_SIZE, &device->last_data_size);
        if (status != DC_STATUS_SUCCESS || !device->last_data_size) {
            ERROR(device->base.context, "Failed to receive the packet.");
            return status;
        }
        device->last_data_pos = 0;
    }
}

// layer 2 functions

#define DIVESOFT_ESC  (char)0x7D
#define DIVESOFT_XOR  (char)0x20

static unsigned int
divesoft_encode_check_size(const char * data, unsigned int size)
{
    unsigned int result = size;
    for (unsigned int i = 0; i < size; i++, data++) {
        result += (*data == DIVESOFT_FLAG || *data == DIVESOFT_ESC);
    }
    return result;
}

static char *
divesoft_encode(const char * data, unsigned int size, char * w)
{
    for (unsigned int i = 0; i < size; i++, data++) {
        if (*data == DIVESOFT_FLAG || *data == DIVESOFT_ESC) {
            *(w++) = DIVESOFT_ESC;
            *(w++) = *data ^ DIVESOFT_XOR;
        }
        else {
            *(w++) = *data;
        }
    }
    return w;
}

static const char *
divesoft_decode(const char * data, unsigned int decoded_size, char * w)
{
    for (unsigned int i = 0; i < decoded_size; data++, i++) {
        if (*data == DIVESOFT_ESC) {
            *(w++) = *++data ^ DIVESOFT_XOR;
        }
        else {
            *(w++) = *data;
        }
    }
    return data;
}

static dc_status_t
divesoft_send_packets (divesoft_device_t *device, const divesoft_packet_t * packets, unsigned int count)
{
    if (!packets || !count)
        return DC_STATUS_SUCCESS;

    unsigned int total_size = 1;
    for (unsigned int p = 0; p < count; p++) {
        total_size += divesoft_encode_check_size(&packets[p].packet_id, 6);
        total_size += divesoft_encode_check_size(packets[p].data, packets[p].length);
        total_size += divesoft_encode_check_size((const char *)&packets[p].checksum, 2);
        total_size += 1;
    }

    char * buffer = (char *) malloc(total_size), * w = buffer;
    *(w++) = DIVESOFT_FLAG;
    for (unsigned int p = 0; p < count; p++) {
        w = divesoft_encode(&packets[p].packet_id, 6, w);
        w = divesoft_encode(packets[p].data, packets[p].length, w);
        w = divesoft_encode((const char *)&packets[p].checksum, 2, w);
        *(w++) = DIVESOFT_FLAG;
    }

    dc_status_t status = divesoft_send_raw(device, buffer, total_size);

    free(buffer);

    return status;
}

static dc_status_t
divesoft_recv_packet (divesoft_device_t *device, divesoft_packet_t * packet)
{
    char * data = NULL;
    unsigned int size = 0;
    dc_status_t status = divesoft_recv_raw(device, &data, &size);
    if (status != DC_STATUS_SUCCESS) {
        free(data);
        return status;
    }

    const char * read = data;
    read = divesoft_decode(read, 6, (char *) packet);
    if(packet->length > MAXDATA) {
        ERROR(device->base.context, "Oversize packet, %u", packet->length);
        free(data);
        return DC_STATUS_DATAFORMAT;
    }
    read = divesoft_decode(read, packet->length, packet->data);
    read = divesoft_decode(read, 2, (char *) &packet->checksum);
    unsigned short checksum = divesoft_checksum_crc16_ccitt((const unsigned char *) packet, packet->length + 6, 0xFFFF) ^ 0xFFFF;
    if (packet->checksum != checksum) {
        free(data);
        ERROR (device->base.context, "Invalid packet checksum.");
        return DC_STATUS_DATAFORMAT;
    }
    free(data);

    return status;
}

// layer 3 functions

static dc_status_t
divesoft_send (divesoft_device_t *device, divesoft_message_t message, const char * data, unsigned int size)
{
    // Split message into multiple packets
    unsigned int count = (size + MAXDATA - 1) / MAXDATA;
    divesoft_packet_t * packets = (divesoft_packet_t *) malloc(count * sizeof(divesoft_packet_t));

    static unsigned int request_id = 0;
    request_id++;

    for (unsigned int i = 0; i < count; i++)
    {
        packets[i].packet_id = (i << 4) | (request_id & 0x0F);
        packets[i].options = (char)0x80 | ((i + 1 == count) << 6);
        packets[i].message = message;
        packets[i].length = (i + 1 == count) ? size % MAXDATA : MAXDATA;
        memcpy(packets[i].data, data, packets[i].length);
        data += packets[i].length;
        packets[i].checksum = divesoft_checksum_crc16_ccitt((const unsigned char *) &packets[i], packets[i].length + 6, 0xFFFF) ^ 0xFFFF;
    }

    dc_status_t status = divesoft_send_packets(device, packets, count);

    free(packets);

    return status;
}

static dc_status_t
divesoft_recv (divesoft_device_t *device, divesoft_message_t * message, char ** data, unsigned int * size)
{
    divesoft_packet_t packet;
    unsigned int count = 0;
    size_t capacity = sizeof(packet.data);
    *data = malloc(capacity);
    *size = 0;

    dc_status_t status;
    while((status = divesoft_recv_packet(device, &packet)) == DC_STATUS_SUCCESS) {
        count++;
        *size += packet.length;
        *message = packet.message;
        while(*size >= capacity) {
            capacity *= 2;
            *data = (char *)realloc(*data, capacity);
        }
        memcpy(*data + *size - packet.length, packet.data, packet.length);
        if(packet.options >> 6) {
            // last packet
            break;
        }
    }

    if(status != DC_STATUS_SUCCESS || !count) {
        *message = MSG_ECHO;
        ERROR(device->base.context, "Failed to receive packets, status id %d", status);
        return status;
    }

    return status;
}

typedef struct divesoft_message_data_t {
    divesoft_message_t message;
    char * data;
    unsigned int size;
} divesoft_message_data_t;

static dc_status_t
divesoft_transfer(divesoft_device_t * device, divesoft_message_data_t request, divesoft_message_data_t * response)
{
    dc_status_t status = DC_STATUS_SUCCESS;

    // Make sure everything is in a sane state.
    dc_iostream_sleep (device->iostream, 300);
    status = dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);
    if (status != DC_STATUS_SUCCESS) {
        ERROR (device->base.context, "Failed to purge.");
        return status;
    }

    status = divesoft_send(device, request.message, request.data, request.size);
    if(status != DC_STATUS_SUCCESS) {
        ERROR(device->base.context, "Failed to send request.");
        return status;
    }

    status = divesoft_recv(device, &response->message, &response->data, &response->size);
    if(status != DC_STATUS_SUCCESS) {
        ERROR(device->base.context, "Failed to receive response.");
        return status;
    }
    return status;
}

typedef struct divesoft_packet_connect_t {
    unsigned short compression;
    char client_name[32];
} divesoft_packet_connect_t;

typedef struct divesoft_packet_connected_t {
    unsigned short compression;
    unsigned char protocol_major;
    unsigned char protocol_minor;
    char serial_number[16];
    unsigned char nonce[8];
} divesoft_packet_connected_t;

typedef struct divesoft_packet_get_dive_list_t {
    unsigned int reference;
    unsigned char direction;
    unsigned char max_count;
} divesoft_packet_get_dive_list_t;

#define HEADER_SIGNATURE_V1 0x45766944 // "DivE"
#define HEADER_SIGNATURE_V2 0x45566944 // "DiVE"

#define HEADER_V1_SIZE 32
#define HEADER_V2_SIZE 64

typedef struct divesoft_field_offset_t {
    unsigned offset; // byte offset of the field
    unsigned shift;  // bit offset in the 32bit area (little endian)
    unsigned length; // bit length
} divesoft_field_offset_t;

typedef struct divesoft_dive_header_offsets_t {
    unsigned header_size;
    divesoft_field_offset_t datum;
    divesoft_field_offset_t serial;
    divesoft_field_offset_t records;
    divesoft_field_offset_t mode;
    divesoft_field_offset_t duration;
    divesoft_field_offset_t max_depth;
    divesoft_field_offset_t min_temp;
    divesoft_field_offset_t p_air;
} divesoft_dive_header_info_t;

static const divesoft_dive_header_info_t divesoft_dive_header_v1_offsets = {
        HEADER_V1_SIZE,
        {8, 0, 32},
        {6, 0, 16},
        {16, 0, 18},
        {12, 27, 3},
        {12, 0, 17},
        {20, 0, 16},
        {16, 18, 10},
        {24, 0, 16},
};

static const divesoft_dive_header_info_t divesoft_dive_header_v2_offsets = {
        HEADER_V2_SIZE,
        {8, 0, 32},
        {6, 0, 16},
        {20, 0, 32},
        {18, 0, 8},
        {12, 0, 32},
        {28, 0, 16},
        {24, 0, 16},
        {32, 0, 16},
};

static unsigned divesoft_read_field(const unsigned char * data, divesoft_field_offset_t field) {
    unsigned int mask = 0xFFFFFFFF >> (32 - field.length);
    return (array_uint32_le(data + field.offset) >> field.shift) & mask;
}

typedef struct divesoft_packet_dive_list_v1_t {
    unsigned int handle;
    unsigned char thumbprint[THUMBPRINT_SIZE];
    char header_v1[HEADER_V1_SIZE];
} divesoft_packet_dive_list_v1_t;

typedef struct divesoft_packet_dive_list_v2_t {
    unsigned int handle;
    unsigned char thumbprint[THUMBPRINT_SIZE];
    char header_v2[HEADER_V2_SIZE];
} divesoft_packet_dive_list_v2_t;

static dc_status_t divesoft_device_foreach_dive (dc_device_t *abstract, unsigned int handle, unsigned records, unsigned header_size, const unsigned char * thumbprint, dc_dive_callback_t callback, void *userdata);

typedef struct divesoft_packet_get_dive_data_t {
    unsigned int handle;
    unsigned int start_offset;
    unsigned int length;
} divesoft_packet_get_dive_data_t;

dc_status_t
divesoft_device_open(dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
    DEBUG(context, "Opening divesoft device.\n");

    dc_status_t status = DC_STATUS_SUCCESS;
    divesoft_device_t *device = NULL;

    if (out == NULL)
        return DC_STATUS_INVALIDARGS;

    // Allocate memory.
    device = (divesoft_device_t *) dc_device_allocate (context, &divesoft_device_vtable);
    if (device == NULL) {
        ERROR (context, "Failed to allocate memory.");
        return DC_STATUS_NOMEMORY;
    }

    // Set the default values.
    device->iostream = iostream;
    memset(device->fingerprint, 0, sizeof(device->fingerprint));
    device->last_data_pos = 0;
    device->last_data_size = 0;

    // init io
    // Set the serial communication protocol (115200 8N1).
    status = dc_iostream_configure (device->iostream, 115200, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
    if (status != DC_STATUS_SUCCESS) {
        ERROR (context, "Failed to set the terminal attributes.");
        return status;
    }

    // Set the timeout for receiving data (3000ms).
    status = dc_iostream_set_timeout (device->iostream, 3000);
    if (status != DC_STATUS_SUCCESS) {
        ERROR (context, "Failed to set the timeout.");
        return status;
    }

    *out = (dc_device_t *) device;

    // Get info
    divesoft_packet_connect_t packet_connect = { 1, "libdivecomputer" };
    divesoft_message_data_t request = { MSG_CONNECT, (char *)&packet_connect, 2 + strlen(packet_connect.client_name)};
    divesoft_message_data_t response = { 0 };
    status = divesoft_transfer(device, request, &response);
    if (status != DC_STATUS_SUCCESS) {
        ERROR (context, "Could not receive connection response.");
        free(response.data);
        return status;
    }
    if (response.message == MSG_RESULT) {
        ERROR (context, "Invalid response.");
        free(response.data);
        return DC_STATUS_INVALIDARGS;
    }
    if (response.message != MSG_CONNECTED) {
        ERROR (context, "Unexpected response.");
        free(response.data);
        return DC_STATUS_PROTOCOL;
    }
    divesoft_packet_connected_t * packet_connected = (divesoft_packet_connected_t *)response.data;
    INFO(context, "Connected to device. Compression type mask: %04x, protocol: %d.%d, serial: %.16s", packet_connected->compression, packet_connected->protocol_major, packet_connected->protocol_minor, packet_connected->serial_number);
    device->model = 0;
    device->serial = 0;
    device->serial = 0;
    for (unsigned int i = 0; i < sizeof(packet_connected->serial_number); i++) {
        device->serial *= 10;
        device->serial += packet_connected->serial_number[i] - '0';
    }
    free(response.data);

    return status;
}
static dc_status_t
divesoft_device_set_fingerprint (dc_device_t *abstract, const unsigned char *data, unsigned int size)
{
    divesoft_device_t *device = (divesoft_device_t *)abstract;

    if (size && size != sizeof (device->fingerprint))
        return DC_STATUS_INVALIDARGS;

    if (size)
        memcpy (device->fingerprint, data, sizeof (device->fingerprint));
    else
        memset (device->fingerprint, 0, sizeof (device->fingerprint));

    return DC_STATUS_SUCCESS;
}

#define TIMESTAMP_BASE 946684800 // 1st Jan 2000 00:00:00
#define INVALID_HANDLE_VALUE 0xFFFFFFFF
#define DIVE_LIST_MAX 100 // chosen arbitrarily, may try more

static dc_status_t
divesoft_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
    dc_status_t status = DC_STATUS_SUCCESS;
    divesoft_device_t *device = (divesoft_device_t *) abstract;

    // Enable progress notifications.
    dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
    device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);
    progress.maximum = 0;

    // Give device info
    dc_event_devinfo_t devinfo;
    devinfo.serial = 0;
    devinfo.firmware = 0;
    devinfo.model = 0;
    device_event_emit(abstract, DC_EVENT_DEVINFO, &devinfo);

    // Read dive list
    for(unsigned int start_handle = INVALID_HANDLE_VALUE;;) {
        divesoft_packet_get_dive_list_t packet_get_dive_list = {start_handle, 1, DIVE_LIST_MAX};
        divesoft_message_data_t request = {MSG_GET_DIVE_LIST, (char *) &packet_get_dive_list, 6};
        divesoft_message_data_t response = {0};
        status = divesoft_transfer(device, request, &response);
        if (status != DC_STATUS_SUCCESS) {
            ERROR (abstract->context, "Could not process dive list request.");
            free(response.data);
            return status;
        }
        if (response.message == MSG_DIVE_LIST_V1 || response.message == MSG_DIVE_LIST_V2) {
            if(response.size == 0) {
                // we are done
                free(response.data);
                break;
            }
            // determine version
            size_t element_size, header_size;
            const divesoft_dive_header_info_t * offsets;
            if(response.message == MSG_DIVE_LIST_V1) {
                element_size = sizeof(divesoft_packet_dive_list_v1_t);
                header_size = HEADER_V1_SIZE;
                offsets = &divesoft_dive_header_v1_offsets;
            } else {
                element_size = sizeof(divesoft_packet_dive_list_v2_t);
                header_size = HEADER_V2_SIZE;
                offsets = &divesoft_dive_header_v2_offsets;
            }
            // append new jobs to progress counter
            progress.maximum += response.size / element_size;
            device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);
            // loop through fetched dives
            for(size_t i = 0; i < response.size; i += element_size) {
                if(device_is_cancelled(abstract)) {
                    free(response.data);
                    return DC_STATUS_CANCELLED;
                }
                // extract data
                const unsigned char * record = (const unsigned char *)&response.data[i];
                const unsigned char * thumbprint = record + offsetof(divesoft_packet_dive_list_v1_t, thumbprint);
                if(memcmp(device->fingerprint, thumbprint, sizeof(device->fingerprint)) == 0) {
                    // same thumbprint -> we will not continue in our download, it was downloaded before
                    free(response.data);
                    return DC_STATUS_SUCCESS;
                }
                unsigned serial = divesoft_read_field(record + offsetof(divesoft_packet_dive_list_v1_t, header_v1), offsets->serial);
                int handle = (int)array_uint32_le(record + offsetof(divesoft_packet_dive_list_v1_t, handle));
                dc_datetime_t date;
                dc_datetime_gmtime(&date, TIMESTAMP_BASE + divesoft_read_field(record + offsetof(divesoft_packet_dive_list_v1_t, header_v1), offsets->datum));
                INFO (abstract->context, "Downloading... serial: %u, handle: %10d, datum: %4d-%02d-%02d",
                       serial,
                       handle,
                       date.year, date.month, date.day
                );
                // set start handle for next list request
                start_handle = handle;
                unsigned records = divesoft_read_field(record + offsetof(divesoft_packet_dive_list_v1_t, header_v1), offsets->records);
                // parse dive data
                status = divesoft_device_foreach_dive(abstract, handle, records, header_size, thumbprint, callback, userdata);
                if(status != DC_STATUS_SUCCESS) {
                    free(response.data);
                    return status;
                }
                // emit progress
                progress.current++;
                device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);
            }
        } else {
            ERROR (abstract->context, "Wrong response for dive list request.");
            free(response.data);
            return status;
        }

        free(response.data);
    }

    return status;
}

#define DEFAULT_DIVE_LENGTH 0xFFFFFFFF
#define DIVE_REC_LENGTH 16

static dc_status_t
divesoft_device_foreach_dive (dc_device_t *abstract, unsigned int handle, unsigned records, unsigned header_size, const unsigned char * thumbprint, dc_dive_callback_t callback, void *userdata)
{
    dc_status_t status = DC_STATUS_SUCCESS;
    if (!callback) {
        return status;
    }
    divesoft_device_t *device = (divesoft_device_t *) abstract;

    unsigned int size = header_size + records * DIVE_REC_LENGTH;
    dc_buffer_t * buffer = dc_buffer_new(size);

    // Read dive data
    divesoft_packet_get_dive_data_t packet_get_dive_data = { handle, 0, size };
    INFO (abstract->context, "Number of records: %d", records);

    INFO (abstract->context, "Getting: offset %u, length %u", packet_get_dive_data.start_offset, packet_get_dive_data.length);
    divesoft_message_data_t request = {MSG_GET_DIVE_DATA, (char *) &packet_get_dive_data,
                                       sizeof(packet_get_dive_data)};
    divesoft_message_data_t response = {0};
    status = divesoft_transfer(device, request, &response);
    if (status != DC_STATUS_SUCCESS) {
        ERROR (abstract->context, "Could not process dive data request.");
        free(response.data);
        dc_buffer_free(buffer);
        return status;
    }
    if (response.message != MSG_DIVE_DATA) {
        ERROR (abstract->context, "Wrong response for dive data request, got ID = %d", response.message);
        free(response.data);
        dc_buffer_free(buffer);
        return DC_STATUS_DATAFORMAT;
    }
    dc_buffer_append(buffer, (const unsigned char *) response.data, response.size);
    free(response.data);

    // transfer data to parser
    if(dc_buffer_get_data(buffer)) callback(dc_buffer_get_data(buffer), size, thumbprint, THUMBPRINT_SIZE, userdata);

    // clean buffers
    dc_buffer_free(buffer);

    return status;
}
