#ifndef CHECKSUM_H
#define CHECKSUM_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

unsigned char
checksum_add_uint8 (const unsigned char data[], unsigned int size, unsigned char init);

unsigned short
checksum_add_uint16 (const unsigned char data[], unsigned int size, unsigned short init);

unsigned char
checksum_xor_uint8 (const unsigned char data[], unsigned int size, unsigned char init);

unsigned short
checksum_crc_ccitt_uint16 (const unsigned char data[], unsigned int size);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CHECKSUM_H */
