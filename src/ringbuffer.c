#include <assert.h>

#include "ringbuffer.h"


static unsigned int
normalize (unsigned int a, unsigned int size)
{
	return a % size;
}


static unsigned int
distance (unsigned int a, unsigned int b, unsigned int size)
{
	if (a <= b) {
		return (b - a) % size;
	} else {
		return size - (a - b) % size;
	}
}


static unsigned int
increment (unsigned int a, unsigned int delta, unsigned int size)
{
	return (a + delta) % size;
}


static unsigned int
decrement (unsigned int a, unsigned int delta, unsigned int size)
{
	if (delta <= a) {
		return (a - delta) % size;
	} else {
		return size - (delta - a) % size;
	}
}


unsigned int
ringbuffer_normalize (unsigned int a, unsigned int begin, unsigned int end)
{
	assert (end >= begin);
	assert (a >= begin);

	return normalize (a, end - begin);
}


unsigned int
ringbuffer_distance (unsigned int a, unsigned int b, unsigned int begin, unsigned int end)
{
	assert (end >= begin);
	assert (a >= begin);

	return distance (a, b, end - begin);
}


unsigned int
ringbuffer_increment (unsigned int a, unsigned int delta, unsigned int begin, unsigned int end)
{
	assert (end >= begin);
	assert (a >= begin);

	return increment (a - begin, delta, end - begin) + begin;
}


unsigned int
ringbuffer_decrement (unsigned int a, unsigned int delta, unsigned int begin, unsigned int end)
{
	assert (end >= begin);
	assert (a >= begin);

	return decrement (a - begin, delta, end - begin) + begin;
}
