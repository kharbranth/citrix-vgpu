/*
 * Copyright (c) 2012, Citrix Systems Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include <err.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/mman.h>

#include <xenctrl.h>

#include "log.h"
#include "demu.h"

typedef struct mapcache_entry {
    uint8_t     *ptr;
    xen_pfn_t   pfn;
    uint64_t    epoch;
} mapcache_entry_t;

#define MAPCACHE_BUCKET_SHIFT   5
#define MAPCACHE_BUCKET_SIZE    (1 << MAPCACHE_BUCKET_SHIFT)

#define MAPCACHE_BUCKET_COUNT   32

static mapcache_entry_t mapcache[MAPCACHE_BUCKET_SIZE *
                                 MAPCACHE_BUCKET_COUNT];
static uint64_t mapcache_epoch;

static inline uint8_t *
__mapcache_lookup(xen_pfn_t pfn)
{
    int     bucket;
    int     i;
    uint8_t *ptr;

    bucket = pfn % MAPCACHE_BUCKET_COUNT;

    ptr = NULL;
    for (i = 0; i < MAPCACHE_BUCKET_SIZE; i++) {
        mapcache_entry_t *entry;

        entry = &mapcache[(bucket * MAPCACHE_BUCKET_SIZE) + i];

        if (entry->pfn == pfn) {
            entry->epoch = mapcache_epoch++;
            ptr = entry->ptr;
        }
    }

    return ptr;
}

static inline void
__mapcache_fault(xen_pfn_t pfn)
{
    int         bucket;
    int         i;
    uint64_t    oldest_epoch;

    DBG("%llx", (unsigned long long)pfn);

    bucket = pfn % MAPCACHE_BUCKET_COUNT;

    oldest_epoch = mapcache_epoch;
    for (i = 0; i < MAPCACHE_BUCKET_SIZE; i++) {
        mapcache_entry_t *entry;

        entry = &mapcache[(bucket * MAPCACHE_BUCKET_SIZE) + i];

        if (entry->epoch < oldest_epoch)
            oldest_epoch = entry->epoch;
    }

    for (i = 0; i < MAPCACHE_BUCKET_SIZE; i++) {
        mapcache_entry_t *entry;

        entry = &mapcache[(bucket * MAPCACHE_BUCKET_SIZE) + i];

        if (entry->epoch != oldest_epoch)
            continue;

        if (entry->ptr != NULL) {
            munmap(entry->ptr, TARGET_PAGE_SIZE);
            entry->ptr = NULL;
        }

        entry->ptr = demu_map_guest_pages(&pfn, 1, 0, 0);
        if (entry->ptr != NULL)
            entry->pfn = pfn;

        break;
    }
}

uint8_t *
mapcache_lookup(uint64_t addr)
{
    xen_pfn_t       pfn;
    unsigned int    offset;
    uint8_t         *ptr;
    int             faulted;

    pfn = addr >> TARGET_PAGE_SHIFT;
    offset = addr & (TARGET_PAGE_SIZE - 1);

    faulted = 0;
again:
    ptr = __mapcache_lookup(pfn);
    if (ptr == NULL) {
        if (!faulted) {
            faulted = true;
            __mapcache_fault(pfn);
            goto again;
        }
        goto fail1;
    }

    ptr += offset;
    return ptr;

fail1:
    ERR("fail1");

    return NULL;
}

void
mapcache_invalidate(void)
{
    int i;

    for (i = 0; i < MAPCACHE_BUCKET_SIZE * MAPCACHE_BUCKET_COUNT; i++) {
        mapcache_entry_t *entry = &mapcache[i];

        if (entry->ptr != NULL) {
            munmap(entry->ptr, TARGET_PAGE_SIZE);
            entry->ptr = NULL;
        }
    }
}
