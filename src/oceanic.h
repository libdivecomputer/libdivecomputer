#ifndef OCEANIC_H
#define OCEANIC_H

#define OCEANIC_SUCCESS         0
#define OCEANIC_ERROR          -1
#define OCEANIC_ERROR_IO       -2
#define OCEANIC_ERROR_MEMORY   -3
#define OCEANIC_ERROR_PROTOCOL -4
#define OCEANIC_ERROR_TIMEOUT  -5

typedef void (*dive_callback_t) (const unsigned char *data, unsigned int size, void *userdata);

#include "oceanic_atom2.h"

#endif /* OCEANIC_H */
