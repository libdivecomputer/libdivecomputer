#ifndef SUUNTO_COMMON_H
#define SUUNTO_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "suunto.h"

int suunto_common_extract_dives (const unsigned char data[], unsigned int begin, unsigned int end, unsigned int eop, unsigned int peek, dive_callback_t callback, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* SUUNTO_COMMON_H */
