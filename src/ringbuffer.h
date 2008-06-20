#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

unsigned int
ringbuffer_normalize (unsigned int a, unsigned int begin, unsigned int end);

unsigned int
ringbuffer_distance (unsigned int a, unsigned int b, unsigned int begin, unsigned int end);

unsigned int
ringbuffer_increment (unsigned int a, unsigned int delta, unsigned int begin, unsigned int end);

unsigned int
ringbuffer_decrement (unsigned int a, unsigned int delta, unsigned int begin, unsigned int end);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* RINGBUFFER_H */
