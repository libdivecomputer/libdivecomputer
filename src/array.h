#ifndef ARRAY_H
#define ARRAY_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

void
array_reverse_bytes (unsigned char data[], unsigned int size);

void
array_reverse_bits (unsigned char data[], unsigned int size);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* ARRAY_H */
