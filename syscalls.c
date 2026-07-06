/* Copyright 2023 OneOfEleven
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include <sys/types.h>
#include <errno.h>

// Heap end symbol (defined in linker script)
extern uint32_t __HeapBase;
extern uint32_t __HeapLimit;

// Current heap position
static char *heap_ptr = NULL;

/**
 * _sbrk - Increase program data space.
 * Malloc and related functions depend on this.
 */
void *_sbrk(int incr)
{
    char *prev_heap_ptr;

    // Initialize heap pointer if not set
    if (heap_ptr == NULL) {
        heap_ptr = (char *)&__HeapBase;
    }

    prev_heap_ptr = heap_ptr;

    // Check if we would exceed the heap limit
    if (((char *)&__HeapLimit - heap_ptr) < incr) {
        errno = ENOMEM;
        return (void *)-1;
    }

    heap_ptr += incr;
    return (void *)prev_heap_ptr;
}
