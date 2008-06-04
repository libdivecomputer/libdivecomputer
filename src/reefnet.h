#ifndef REEFNET_H
#define REEFNET_H

#define REEFNET_SUCCESS         0
#define REEFNET_ERROR          -1
#define REEFNET_ERROR_IO       -2
#define REEFNET_ERROR_MEMORY   -3
#define REEFNET_ERROR_PROTOCOL -4
#define REEFNET_ERROR_TIMEOUT  -5

typedef void (*dive_callback_t) (const unsigned char *data, unsigned int size, void *userdata);

#include "reefnet_sensuspro.h"
#include "reefnet_sensusultra.h"

#endif /* REEFNET_H */
