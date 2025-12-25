#ifndef ALLOC_H
#define ALLOC_H

#include <stddef.h>

void *my_malloc(size_t size);
void *my_calloc(size_t n, size_t size);
void  my_free(void *ptr);
void *my_realloc(void *p, size_t new_size);

#endif