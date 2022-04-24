/*
 * Copyright (c) 2012-2015, Citrix Systems Inc.
 * All rights reserved.
 *
 * Portions also:
 * Copyright (c) 2003 Fabrice Bellard
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
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdbool.h>


#include <sys/types.h>
#include <sys/mman.h>

#include <xenctrl.h>

#include "vga.h"

#include "log.h"

#include "demu.h"
#include "device.h"
#include "mapcache.h"

#define DEBUG_VGA_MEMORY 0
#define DEBUG_VGA_PORT 0

/* force some bits to zero */
static const uint8_t __sr_mask[8] = {
    (uint8_t)~0xfc,
    (uint8_t)~0xc2,
    (uint8_t)~0xf0,
    (uint8_t)~0xc0,
    (uint8_t)~0xf1,
    (uint8_t)~0xff,
    (uint8_t)~0xff,
    (uint8_t)~0x00,
};

static const uint8_t __gr_mask[16] = {
    (uint8_t)~0xf0, /* 0x00 */
    (uint8_t)~0xf0, /* 0x01 */
    (uint8_t)~0xf0, /* 0x02 */
    (uint8_t)~0xe0, /* 0x03 */
    (uint8_t)~0xfc, /* 0x04 */
    (uint8_t)~0x84, /* 0x05 */
    (uint8_t)~0xf0, /* 0x06 */
    (uint8_t)~0xf0, /* 0x07 */
    (uint8_t)~0x00, /* 0x08 */
    (uint8_t)~0xff, /* 0x09 */
    (uint8_t)~0xff, /* 0x0a */
    (uint8_t)~0xff, /* 0x0b */
    (uint8_t)~0xff, /* 0x0c */
    (uint8_t)~0xff, /* 0x0d */
    (uint8_t)~0xff, /* 0x0e */
    (uint8_t)~0xff, /* 0x0f */
};

#define GET_PLANE(data, p) (((data) >> ((p) * 8)) & 0xff)

#define PAT(x) (x)

static const uint32_t mask16[16] = {
    PAT(0x00000000),
    PAT(0x000000ff),
    PAT(0x0000ff00),
    PAT(0x0000ffff),
    PAT(0x00ff0000),
    PAT(0x00ff00ff),
    PAT(0x00ffff00),
    PAT(0x00ffffff),
    PAT(0xff000000),
    PAT(0xff0000ff),
    PAT(0xff00ff00),
    PAT(0xff00ffff),
    PAT(0xffff0000),
    PAT(0xffff00ff),
    PAT(0xffffff00),
    PAT(0xffffffff),
};

#undef PAT

typedef struct device {
    vga_t   vga;
} device_t;

static  device_t   device_state;

#define WRITE_ITEM(i)    do{if (demu_write_state(&i, sizeof(i))) return -1;}while(0)
#define WRITE_BLOCK(i,s) do{if (demu_write_state(i, s))      return -1;}while(0)
#define READ_ITEM(i)     do{if (demu_read_state(&i, sizeof(i)))  return -1;}while(0)
#define READ_BLOCK(i,s)  do{if (demu_read_state(i, s))       return -1;}while(0)

static int vga_part1_dirty = 1;
static int vga_part2_dirty = 1;

#pragma pack(1)

/* VGA block is cut up into contiguous parts we want */
struct VGA_1_s1 {
    int32_t     latch;
    uint8_t     sr_index;
    uint8_t     sr[8];
};

struct VGA_1_s2 {
    uint8_t     gr_index;
    uint8_t     gr[16];
};

struct VGA_1_s3 {

    uint8_t     ar_index;
    uint8_t     ar[21];
    int32_t     ar_flip_flop;
    uint8_t     cr_index;
    uint8_t     cr[256];
    uint8_t     msr;            /* Misc Output Register */
    uint8_t     fcr;            /* Feature Control Register */
    uint8_t     st00;           /* status 0 */
    uint8_t     st01;           /* status 1 */

    uint8_t     dac_state;
    uint8_t     dac_sub_index;
    uint8_t     dac_read_index;

    uint8_t     dac_write_index;
    uint8_t     dac_cache[3];   /* Used when writing */
};

struct VGA_1_s4 {
    int32_t     bank_offset;

    uint16_t    vbe_index;
    uint16_t    vbe_regs[VBE_DISPI_INDEX_NB];
    uint32_t    vbe_start_addr;
    uint32_t    vbe_line_offset;
    uint32_t    vbe_bank_mask;
};

struct VGA_1_sf {
    struct record_header hdr;
    struct VGA_1_s1 s1;
    struct VGA_1_s2 s2;
    struct VGA_1_s3 s3;
    struct VGA_1_s4 s4;
};

struct VGA_2_s {
    uint8_t     palette[768];
};

struct VGA_2_sf {
    struct record_header hdr;
    struct VGA_2_s s;
};

#pragma pack(0)

const size_t VGA_1_s_all_size = sizeof(struct VGA_1_s1) + sizeof(struct VGA_1_s2) + sizeof(struct VGA_1_s3) + sizeof(struct VGA_1_s4);

void vga_markdirty()
{
    vga_part1_dirty = 1;
    vga_part2_dirty = 1;
}

int dump_vga()
{
    int r=0;
    vga_t *s = &device_state.vga;

    if (vga_part1_dirty) {
        struct VGA_1_sf sf1;
        struct demu_record *record;

        memset(&sf1, 0, sizeof(sf1));
        sf1.hdr.type = demu_state_vga;
        sf1.hdr.stype = 1;
        sf1.hdr.length = VGA_1_s_all_size;

        /* lock */
        memcpy(&sf1.s1, &s->latch,    sizeof(struct VGA_1_s1));
        memcpy(&sf1.s2, &s->gr_index, sizeof(struct VGA_1_s2));
        memcpy(&sf1.s3, &s->ar_index, sizeof(struct VGA_1_s3));
        memcpy(&sf1.s4, &s->bank_offset, sizeof(struct VGA_1_s4));
        /* unlock */

        record = malloc(sizeof(sf1));
        if (!record)
            return -1;

        memcpy(record, &sf1, sizeof(sf1));
        r = write_record(record);

        vga_part1_dirty = 0;
    }

    if ((r>=0) && vga_part2_dirty) {
        struct VGA_2_sf sf2;
        struct demu_record *record;

        memset(&sf2, 0, sizeof(sf2));
        sf2.hdr.type = demu_state_vga;
        sf2.hdr.stype = 2;
        sf2.hdr.length = sizeof(sf2.s);

        /* lock*/
        memcpy(&sf2.s, s->palette, sizeof(sf2.s));
        /* unlock */

        record = malloc(sizeof(sf2));
        if (!record)
            return -1;

        memcpy(record, &sf2, sizeof(sf2));
        r = write_record(record);

        vga_part2_dirty = 0;
    }
    return r;
}

int read_vga(struct record_header* hdr)
{
    vga_t   *s = &device_state.vga;

    if (hdr->type != demu_state_vga) {
        ERR("Cannot process type %d", hdr->type);
        return -1;
    }

    switch (hdr->stype) {
    case 1: {
        struct VGA_1_sf* sf1 = (struct VGA_1_sf*) hdr;

        if (hdr->length != VGA_1_s_all_size) {
            ERR("Size mismatch %d != %d", hdr->length, VGA_1_s_all_size);
            return -1;
        }

        /* lock */
        memcpy(&s->latch,       &sf1->s1, sizeof(struct VGA_1_s1));
        memcpy(&s->gr_index,    &sf1->s2, sizeof(struct VGA_1_s2));
        memcpy(&s->ar_index,    &sf1->s3, sizeof(struct VGA_1_s3));
        memcpy(&s->bank_offset, &sf1->s4, sizeof(struct VGA_1_s4));
        /* unlock */

        vga_part1_dirty = 1;
        break;
    }
    case 2: {
        struct VGA_2_sf* sf2 = (struct VGA_2_sf*) hdr;

        if (hdr->length != sizeof(struct VGA_2_s)) {
            ERR("Size mismatch %d != %d", hdr->length, sizeof(struct VGA_2_s));
            return -1;
        }

        /* lock*/
        memcpy(s->palette, &sf2->s, sizeof(sf2->s));
        /* unlock */

        vga_part2_dirty = 1;
        break;
    }
    default:
        ERR("Unknown sub type for VGA %d", hdr->stype);
        return -1;
    }
    return 0;
}

static void
device_vga_bios_init(void)
{
    vga_t   *vga = &device_state.vga;

    uint8_t palette_model[192] = {
        0,   0,   0,   0,   0, 170,   0, 170,
        0,   0, 170, 170, 170,   0,   0, 170,
        0, 170, 170,  85,   0, 170, 170, 170,
        85,  85,  85,  85,  85, 255,  85, 255,
        85,  85, 255, 255, 255,  85,  85, 255,
        85, 255, 255, 255,  85, 255, 255, 255,
        0,  21,   0,   0,  21,  42,   0,  63,
        0,   0,  63,  42,  42,  21,   0,  42,
        21,  42,  42,  63,   0,  42,  63,  42,
        0,  21,  21,   0,  21,  63,   0,  63,
        21,   0,  63,  63,  42,  21,  21,  42,
        21,  63,  42,  63,  21,  42,  63,  63,
        21,   0,   0,  21,   0,  42,  21,  42,
        0,  21,  42,  42,  63,   0,   0,  63,
        0,  42,  63,  42,   0,  63,  42,  42,
        21,   0,  21,  21,   0,  63,  21,  42,
        21,  21,  42,  63,  63,   0,  21,  63,
        0,  63,  63,  42,  21,  63,  42,  63,
        21,  21,   0,  21,  21,  42,  21,  63,
        0,  21,  63,  42,  63,  21,   0,  63,
        21,  42,  63,  63,   0,  63,  63,  42,
        21,  21,  21,  21,  21,  63,  21,  63,
        21,  21,  63,  63,  63,  21,  21,  63,
        21,  63,  63,  63,  21,  63,  63,  63
    };

    vga->latch = 0;

    vga->sr_index = 3;
    vga->sr[0] = 3;
    vga->sr[1] = 0;
    vga->sr[2] = 3;
    vga->sr[3] = 0;
    vga->sr[4] = 2;
    vga->sr[5] = 0;
    vga->sr[6] = 0;
    vga->sr[7] = 0;

    vga->gr_index = 5;
    vga->gr[0] = 0;
    vga->gr[1] = 0;
    vga->gr[2] = 0;
    vga->gr[3] = 0;
    vga->gr[4] = 0;
    vga->gr[5] = 16;
    vga->gr[6] = 14;
    vga->gr[7] = 15;
    vga->gr[8] = 255;

    /* changed by out 0x03c0 */
    vga->ar_index = 32;
    vga->ar[0] = 0;
    vga->ar[1] = 1;
    vga->ar[2] = 2;
    vga->ar[3] = 3;
    vga->ar[4] = 4;
    vga->ar[5] = 5;
    vga->ar[6] = 6;
    vga->ar[7] = 7;
    vga->ar[8] = 8;
    vga->ar[9] = 9;
    vga->ar[10] = 10;
    vga->ar[11] = 11;
    vga->ar[12] = 12;
    vga->ar[13] = 13;
    vga->ar[14] = 14;
    vga->ar[15] = 15;
    vga->ar[16] = 12;
    vga->ar[17] = 0;
    vga->ar[18] = 15;
    vga->ar[19] = 8;
    vga->ar[20] = 0;

    vga->ar_flip_flop = 1;

    vga->cr_index = 15;
    vga->cr[0] = 95;
    vga->cr[1] = 79;
    vga->cr[2] = 80;
    vga->cr[3] = 130;
    vga->cr[4] = 85;
    vga->cr[5] = 129;
    vga->cr[6] = 191;
    vga->cr[7] = 31;
    vga->cr[8] = 0;
    vga->cr[9] = 79;
    vga->cr[10] = 14;
    vga->cr[11] = 15;
    vga->cr[12] = 0;
    vga->cr[13] = 0;
    vga->cr[14] = 5;
    vga->cr[15] = 160;
    vga->cr[16] = 156;
    vga->cr[17] = 142;
    vga->cr[18] = 143;
    vga->cr[19] = 40;
    vga->cr[20] = 31;
    vga->cr[21] = 150;
    vga->cr[22] = 185;
    vga->cr[23] = 163;
    vga->cr[24] = 255;

    vga->msr = 103;
    vga->fcr = 0;
    vga->st00 = 0;
    vga->st01 = 0;

    /* dac_* & palette will be initialized by os through out 0x03c8 &
     * out 0c03c9(1:3) */
    vga->dac_state = 0;
    vga->dac_sub_index = 0;
    vga->dac_read_index = 0;
    vga->dac_write_index = 16;
    vga->dac_cache[0] = 255;
    vga->dac_cache[1] = 255;
    vga->dac_cache[2] = 255;

    /* palette */
    memcpy(vga->palette, palette_model, 192);

    vga->bank_offset = 0;

    vga_part1_dirty = 1;
    vga_part2_dirty = 1;
}

static void
device_vga_reset(void)
{
    vga_t   *vga = &device_state.vga;

    vga->lfb_addr = 0;
    vga->lfb_size = 0;
    vga->sr_index = 0;
    memset(vga->sr, '\0', sizeof(vga->sr));
    vga->gr_index = 0;
    memset(vga->gr, '\0', sizeof(vga->gr));
    vga->ar_index = 0;
    memset(vga->ar, '\0', sizeof(vga->ar));
    vga->ar_flip_flop = 0;
    vga->cr_index = 0;
    memset(vga->cr, '\0', sizeof(vga->cr));
    vga->msr = 0;
    vga->fcr = 0;
    vga->st00 = 0;
    vga->st01 = 0;
    vga->dac_state = 0;
    vga->dac_sub_index = 0;
    vga->dac_read_index = 0;
    vga->dac_write_index = 0;
    memset(vga->dac_cache, '\0', sizeof(vga->dac_cache));
    vga->dac_8bit = 0;
    memset(vga->palette, '\0', sizeof(vga->palette));
    vga->bank_offset = 0;
    vga->vbe_index = 0;
    memset(vga->vbe_regs, '\0', sizeof(vga->vbe_regs));
    vga->vbe_regs[VBE_DISPI_INDEX_ID] = VBE_DISPI_ID0;
    vga->vbe_start_addr = 0;
    vga->vbe_line_offset = 0;
    vga->vbe_bank_mask = (VRAM_ACTUAL_SIZE >> 16) - 1;
    vga->plane_updated = 0;

    device_vga_bios_init();

    vga_part1_dirty = 1;
    vga_part2_dirty = 1;
}

vga_t *
device_get_vga(void)
{
    return &device_state.vga;
}

static uint8_t
device_vga_port_readb(void *priv, uint64_t addr)
{
    vga_t   *vga = &device_state.vga;
    uint8_t index;
    uint8_t val;

    assert(priv == NULL);

    /* check port range access depending on color/monochrome mode */
    if ((addr >= 0x3b0 && addr <= 0x3bf && (vga->msr & MSR_COLOR_EMULATION)) ||
            (addr >= 0x3d0 && addr <= 0x3df && !(vga->msr & MSR_COLOR_EMULATION))) {
        val = 0xff;
    } else {
        switch(addr) {
        case 0x3c0:
            if (vga->ar_flip_flop == 0) {
                val = vga->ar_index;
            } else {
                val = 0;
            }
            break;
        case 0x3c1:
            index = vga->ar_index & 0x1f;
            if (index < 21)
                val = vga->ar[index];
            else
                val = 0;
            break;
        case 0x3c2:
            val = vga->st00;
            break;
        case 0x3c4:
            val = vga->sr_index;
            break;
        case 0x3c5:
            val = vga->sr[vga->sr_index];
            break;
        case 0x3c7:
            val = vga->dac_state;
            break;
        case 0x3c8:
            val = vga->dac_write_index;
            break;
        case 0x3c9:
            vga_part1_dirty = 1;

            val = vga->palette[vga->dac_read_index * 3 + vga->dac_sub_index];
            if (++vga->dac_sub_index == 3) {
                vga->dac_sub_index = 0;
                vga->dac_read_index++;
            }
            break;
        case 0x3ca:
            val = vga->fcr;
            break;
        case 0x3cc:
            val = vga->msr;
            break;
        case 0x3ce:
            val = vga->gr_index;
            break;
        case 0x3cf:
            val = vga->gr[vga->gr_index];
            break;
        case 0x3b4:
        case 0x3d4:
            val = vga->cr_index;
            break;
        case 0x3b5:
        case 0x3d5:
            val = vga->cr[vga->cr_index];
            break;
        case 0x3ba:
        case 0x3da:
            /* just toggle to fool polling */
            vga->st01 ^= (ST01_V_RETRACE | ST01_DISP_ENABLE);
            val = vga->st01;
            vga->ar_flip_flop = 0;
            break;
        default:
            val = 0x00;
            break;
        }
    }

#if  DEBUG_VGA_PORT
    DBG("[0x%"PRIx64"] = 0x%02x", addr, val);
#endif

    return val;
}

static void
device_vga_port_writeb(void *priv, uint64_t addr, uint8_t val)
{
    vga_t   *vga = &device_state.vga;
    uint8_t index;

    assert(priv == NULL);

#if  DEBUG_VGA_PORT
    DBG("[0x%"PRIx64"] = 0x%02x", addr, val);
#endif

    /* check port range access depending on color/monochrome mode */
    if ((addr >= 0x3b0 && addr <= 0x3bf && (vga->msr & MSR_COLOR_EMULATION)) ||
            (addr >= 0x3d0 && addr <= 0x3df && !(vga->msr & MSR_COLOR_EMULATION)))
        return;

    vga_part1_dirty = 1;

    switch(addr) {
    case 0x3c0:
        if (vga->ar_flip_flop == 0) {
            val &= 0x3f;
            vga->ar_index = val;
        } else {
            index = vga->ar_index & 0x1f;
            switch(index) {
            case 0x00 ... 0x0f:
                vga->ar[index] = val & 0x3f;
                break;
            case 0x10:
                vga->ar[index] = val & ~0x10;
                break;
            case 0x11:
                vga->ar[index] = val;
                break;
            case 0x12:
                vga->ar[index] = val & ~0xc0;
                break;
            case 0x13:
                vga->ar[index] = val & ~0xf0;
                break;
            case 0x14:
                vga->ar[index] = val & ~0xf0;
                break;
            default:
                break;
            }
        }
        vga->ar_flip_flop ^= 1;
        break;
    case 0x3c2:
        vga->msr = val & ~0x10;
        break;
    case 0x3c4:
        vga->sr_index = val & 7;
        break;
    case 0x3c5:
        vga->sr[vga->sr_index] = val & __sr_mask[vga->sr_index];
        break;
    case 0x3c7:
        vga->dac_read_index = val;
        vga->dac_sub_index = 0;
        vga->dac_state = 3;
        break;
    case 0x3c8:
        vga->dac_write_index = val;
        vga->dac_sub_index = 0;
        vga->dac_state = 0;
        break;
    case 0x3c9:
        vga->dac_cache[vga->dac_sub_index] = val;
        if (++vga->dac_sub_index == 3) {
            memcpy(&vga->palette[vga->dac_write_index * 3], vga->dac_cache, 3);
            vga->dac_sub_index = 0;
            vga->dac_write_index++;

            vga_part2_dirty = 1;
        }
        break;
    case 0x3ce:
        vga->gr_index = val & 0x0f;
        break;
    case 0x3cf:
        vga->gr[vga->gr_index] = val & __gr_mask[vga->gr_index];
        break;
    case 0x3b4:
    case 0x3d4:
        vga->cr_index = val;
        break;
    case 0x3b5:
    case 0x3d5:
        /* handle CR0-7 protection */
        if ((vga->cr[0x11] & 0x80) && vga->cr_index <= 7) {
            /* can always write bit 4 of CR7 */
            if (vga->cr_index == 7)
                vga->cr[7] = (vga->cr[7] & ~0x10) | (val & 0x10);
            return;
        }
        switch(vga->cr_index) {
        case 0x01: /* horizontal display end */
        case 0x07:
        case 0x09:
        case 0x0c:
        case 0x0d:
        case 0x12: /* vertical display end */
            vga->cr[vga->cr_index] = val;
            break;
        default:
            vga->cr[vga->cr_index] = val;
            break;
        }

        break;
    case 0x3ba:
    case 0x3da:
        vga->fcr = val & 0x10;
        break;
    }
}

static io_ops_t device_vga_port_ops  = {
    .readb = device_vga_port_readb,
    .writeb = device_vga_port_writeb
};

static void
__copy_from_vram(uint64_t addr, uint8_t *dst, uint64_t size)
{
    uint8_t *vram = demu_get_vram();

    memcpy(dst, &vram[addr], size);
}

static uint8_t
device_vga_memory_readb(void *priv, uint64_t addr)
{
    vga_t   *vga = &device_state.vga;
    uint8_t memory_map_mode;
    uint8_t plane;
    uint8_t val;

    assert(priv == NULL);

    /* convert to VGA memory offset */
    memory_map_mode = (vga->gr[6] >> 2) & 3;
    addr &= 0x1ffff;

    switch(memory_map_mode) {
    case 0:
        break;
    case 1:
        if (addr >= 0x10000)
            return 0xff;
        addr += vga->bank_offset;
        break;
    case 2:
        addr -= 0x10000;
        if (addr >= 0x8000)
            return 0xff;
        break;
    default:
    case 3:
        addr -= 0x18000;
        if (addr >= 0x8000)
            return 0xff;
        break;
    }

    if (vga->sr[4] & 0x08) {
        /* chain 4 mode : simplest access */
        __copy_from_vram(addr, &val, 1);
    } else if (vga->gr[5] & 0x10) {
        /* odd/even mode (aka text mode mapping) */
        plane = (vga->gr[4] & 2) | (addr & 1);
        __copy_from_vram(((addr & ~1) << 1) | plane, &val, 1);
    } else {
        /* standard VGA latched access */
        __copy_from_vram(addr << 2, (uint8_t *)&vga->latch, 4);

        vga_part1_dirty = 1;

        if (!(vga->gr[5] & 0x08)) {
            /* read mode 0 */
            plane = vga->gr[4];
            val = GET_PLANE(vga->latch, plane);
        } else {
            uint32_t    tmp;
            /* read mode 1 */
            tmp = (vga->latch ^ mask16[vga->gr[2]]) & mask16[vga->gr[7]];
            tmp |= tmp >> 16;
            tmp |= tmp >> 8;
            val = (~tmp) & 0xff;
        }
    }

    return val;
}

static void
__copy_to_vram(uint8_t *src, uint64_t addr, uint64_t size)
{
    uint8_t *vram = demu_get_vram();

    memcpy(&vram[addr], src, size);
    demu_vram_set_page_dirty(addr >> TARGET_PAGE_SHIFT);
}

static void
device_vga_memory_writeb(void *priv, uint64_t addr, uint8_t val)
{
    vga_t   *vga = &device_state.vga;
    uint8_t memory_map_mode;
    uint8_t plane;
    uint8_t mask;

    assert(priv == NULL);

#if  DEBUG_VGA_MEMORY
    DBG("[0x%"PRIx64"] = 0x%02x", addr, val);
#endif

    /* convert to VGA memory offset */
    memory_map_mode = (vga->gr[6] >> 2) & 3;
    addr &= 0x1ffff;

    switch(memory_map_mode) {
    case 0:
        break;
    case 1:
        if (addr >= 0x10000)
            return;
        addr += vga->bank_offset;
        break;
    case 2:
        addr -= 0x10000;
        if (addr >= 0x8000)
            return;
        break;
    default:
    case 3:
        addr -= 0x18000;
        if (addr >= 0x8000)
            return;
        break;
    }

    if (vga->sr[4] & 0x08) {
        /* chain 4 mode : simplest access */
        plane = addr & 3;
        mask = (1 << plane);
        if (vga->sr[2] & mask) {
            __copy_to_vram(&val, addr, 1);
#if  DEBUG_VGA_MEMORY
            DBG("chain4: [0x%"PRIx64"] val=0x%02x", addr, val);
#endif
            vga->plane_updated |= mask; /* only used to detect font change */

        }
    } else if (vga->gr[5] & 0x10) {
        /* odd/even mode (aka text mode mapping) */
        plane = (vga->gr[4] & 2) | (addr & 1);
        mask = (1 << plane);
        if (vga->sr[2] & mask) {
            addr = ((addr & ~1) << 1) | plane;
            __copy_to_vram(&val, addr, 1);
#if  DEBUG_VGA_MEMORY
            DBG("odd/even: [0x%"PRIx64"] val=0x%02x", addr, val);
#endif
            vga->plane_updated |= mask; /* only used to detect font change */
        }
    } else {
        uint8_t     write_mode;
        uint8_t     func_select;
        uint8_t     b;
        uint32_t    write_mask;
        uint32_t    bit_mask;
        uint32_t    set_mask;
        uint32_t    write_val;
        uint32_t    tmp;

        write_val = val;

        /* standard VGA latched access */
        write_mode = vga->gr[5] & 3;
        switch(write_mode) {
        default:
        case 0:
            /* rotate */
            b = vga->gr[3] & 7;
            write_val = ((write_val >> b) | (write_val << (8 - b))) & 0xff;
            write_val |= write_val << 8;
            write_val |= write_val << 16;

            /* apply set/reset mask */
            set_mask = mask16[vga->gr[1]];
            write_val = (write_val & ~set_mask) | (mask16[vga->gr[0]] & set_mask);
            bit_mask = vga->gr[8];
            break;
        case 1:
            write_val = vga->latch;
            goto do_write;
        case 2:
            write_val = mask16[write_val & 0x0f];
            bit_mask = vga->gr[8];
            break;
        case 3:
            /* rotate */
            b = vga->gr[3] & 7;
            write_val = (write_val >> b) | (write_val << (8 - b));

            bit_mask = vga->gr[8] & write_val;
            write_val = mask16[vga->gr[0]];
            break;
        }

        /* apply logical operation */
        func_select = vga->gr[3] >> 3;
        switch(func_select) {
        case 0:
        default:
            /* nothing to do */
            break;
        case 1:
            /* and */
            write_val &= vga->latch;
            break;
        case 2:
            /* or */
            write_val |= vga->latch;
            break;
        case 3:
            /* xor */
            write_val ^= vga->latch;
            break;
        }

        /* apply bit mask */
        bit_mask |= bit_mask << 8;
        bit_mask |= bit_mask << 16;
        write_val = (write_val & bit_mask) | (vga->latch & ~bit_mask);

do_write:
        /* mask data according to sr[2] */
        mask = vga->sr[2];
        vga->plane_updated |= mask; /* only used to detect font change */
        write_mask = mask16[mask];

        __copy_from_vram(addr << 2, (uint8_t *)&tmp, 4);
        tmp &= ~write_mask;
        tmp |= write_val & write_mask;
        __copy_to_vram((uint8_t *)&tmp, addr << 2, 4);

#if  DEBUG_VGA_MEMORY
        DBG("latch: [0x%"PRIx64"] val=0x%08x", addr << 2, tmp);
#endif

    }
}

static io_ops_t device_vga_memory_ops = {
    .readb = device_vga_memory_readb,
    .writeb = device_vga_memory_writeb
};

static uint16_t
device_vbe_index_read(void *priv)
{
    vga_t   *vga = &device_state.vga;

    return vga->vbe_index;
}

static uint16_t
device_vbe_data_read(void *priv)
{
    vga_t       *vga = &device_state.vga;
    uint16_t    val;

    if (vga->vbe_index <= VBE_DISPI_INDEX_NB) {
        if (vga->vbe_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_GETCAPS) {
            switch(vga->vbe_index) {
            case VBE_DISPI_INDEX_XRES:
                val = VBE_DISPI_MAX_XRES;
                break;
            case VBE_DISPI_INDEX_YRES:
                val = VBE_DISPI_MAX_YRES;
                break;
            case VBE_DISPI_INDEX_BPP:
                val = VBE_DISPI_MAX_BPP;
                break;
            default:
                val = vga->vbe_regs[vga->vbe_index];
                break;
            }
        } else {
            val = vga->vbe_regs[vga->vbe_index];
        }
    } else {
        val = 0;
    }

    return val;
}

static uint16_t
device_vbe_port_readw(void *priv, uint64_t addr)
{
    uint16_t val;

    assert(priv == NULL);

    switch (addr) {
    case 0x1ce:
    case 0xff80:
        val = device_vbe_index_read(priv);
        break;

    case 0x1cf:
    case 0xff81:
        val = device_vbe_data_read(priv);
        break;

    default:
        assert(false);
        val = 0xff;
        break;
    }

    return val;
}

static void
device_vbe_index_write(void *priv, uint16_t val)
{
    vga_t   *vga = &device_state.vga;

    vga_part1_dirty = 1;
    assert(priv == NULL);

    vga->vbe_index = val;
}

void
device_update_lfb_addr(void)
{
    vga_t       *vga = &device_state.vga;
    uint64_t    addr = demu_get_vram_addr();

    if (vga->lfb_addr == addr)
        return;

    DBG("%"PRIx64" -> %"PRIx64"", vga->lfb_addr, addr);

    vga_part1_dirty = 1;
    vga->lfb_addr = addr;

    vga->vbe_regs[VBE_DISPI_INDEX_LFB_ADDRESS_H] = vga->lfb_addr >> 16;
    vga->vbe_regs[VBE_DISPI_INDEX_LFB_ADDRESS_L] = vga->lfb_addr & 0xFFFF;
    vga->vbe_regs[VBE_DISPI_INDEX_VIDEO_MEMORY_64K] = vga->lfb_size >> 16;
}

static void
device_vbe_data_write(void *priv, uint16_t val)
{
    vga_t   *vga = &device_state.vga;
    vga_part1_dirty = 1;
    assert(priv == NULL);

    if (vga->vbe_index <= VBE_DISPI_INDEX_NB) {
        switch(vga->vbe_index) {
        case VBE_DISPI_INDEX_ID:
            if (val == VBE_DISPI_ID0 ||
                    val == VBE_DISPI_ID1 ||
                    val == VBE_DISPI_ID2 ||
                    val == VBE_DISPI_ID3 ||
                    val == VBE_DISPI_ID4) {
                vga->vbe_regs[vga->vbe_index] = val;
            }
            break;
        case VBE_DISPI_INDEX_XRES:
            if ((val <= VBE_DISPI_MAX_XRES) && ((val & 7) == 0)) {
                vga->vbe_regs[vga->vbe_index] = val;
            }
            break;
        case VBE_DISPI_INDEX_YRES:
            if (val <= VBE_DISPI_MAX_YRES) {
                vga->vbe_regs[vga->vbe_index] = val;
            }
            break;
        case VBE_DISPI_INDEX_BPP:
            if (val == 0)
                val = 8;
            if (val == 4 || val == 8 || val == 15 ||
                    val == 16 || val == 24 || val == 32) {
                vga->vbe_regs[vga->vbe_index] = val;
            }
            break;
        case VBE_DISPI_INDEX_BANK:
            if (vga->vbe_regs[VBE_DISPI_INDEX_BPP] == 4) {
                val &= (vga->vbe_bank_mask >> 2);
            } else {
                val &= vga->vbe_bank_mask;
            }
            vga->vbe_regs[vga->vbe_index] = val;
            vga->bank_offset = (val << 16);
            break;
        case VBE_DISPI_INDEX_ENABLE:
            if ((val & VBE_DISPI_ENABLED) &&
                    !(vga->vbe_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_ENABLED)) {
                int h, shift_control;

                device_update_lfb_addr();

                vga->vbe_regs[VBE_DISPI_INDEX_VIRT_WIDTH] =
                    vga->vbe_regs[VBE_DISPI_INDEX_XRES];
                vga->vbe_regs[VBE_DISPI_INDEX_VIRT_HEIGHT] =
                    vga->vbe_regs[VBE_DISPI_INDEX_YRES];
                vga->vbe_regs[VBE_DISPI_INDEX_X_OFFSET] = 0;
                vga->vbe_regs[VBE_DISPI_INDEX_Y_OFFSET] = 0;

                if (vga->vbe_regs[VBE_DISPI_INDEX_BPP] == 4)
                    vga->vbe_line_offset = vga->vbe_regs[VBE_DISPI_INDEX_XRES] >> 1;
                else
                    vga->vbe_line_offset = vga->vbe_regs[VBE_DISPI_INDEX_XRES] *
                                           ((vga->vbe_regs[VBE_DISPI_INDEX_BPP] + 7) >> 3);
                vga->vbe_start_addr = 0;

                /* clear the screen (should be done in BIOS) */
                if (!(val & VBE_DISPI_NOCLEARMEM)) {
                    uint8_t *vram = demu_get_vram();

                    memset(vram, 0,
                           vga->vbe_regs[VBE_DISPI_INDEX_YRES] * vga->vbe_line_offset);
                }

                /* we initialize the VGA graphic mode (should be done in BIOS) */
                vga->gr[0x06] = (vga->gr[0x06] & ~0x0c) | 0x05; /* graphic mode + memory map 1 */
                vga->cr[0x17] |= 3; /* no CGA modes */
                vga->cr[0x13] = vga->vbe_line_offset >> 3;
                /* width */
                vga->cr[0x01] = (vga->vbe_regs[VBE_DISPI_INDEX_XRES] >> 3) - 1;
                /* height (only meaningful if < 1024) */
                h = vga->vbe_regs[VBE_DISPI_INDEX_YRES] - 1;
                vga->cr[0x12] = h;
                vga->cr[0x07] = (vga->cr[0x07] & ~0x42) |
                                ((h >> 7) & 0x02) | ((h >> 3) & 0x40);
                /* line compare to 1023 */
                vga->cr[0x18] = 0xff;
                vga->cr[0x07] |= 0x10;
                vga->cr[0x09] |= 0x40;

                if (vga->vbe_regs[VBE_DISPI_INDEX_BPP] == 4) {
                    shift_control = 0;
                    vga->sr[0x01] &= ~8; /* no double line */
                } else {
                    shift_control = 2;
                    vga->sr[4] |= 0x08; /* set chain 4 mode */
                    vga->sr[2] |= 0x0f; /* activate all planes */
                }
                vga->gr[0x05] = (vga->gr[0x05] & ~0x60) | (shift_control << 5);
                vga->cr[0x09] &= ~0x9f; /* no double scan */
            } else {
                /* XXX: the bios should do that */
                vga->bank_offset = 0;
            }
            vga->dac_8bit = (val & VBE_DISPI_8BIT_DAC) > 0;
            vga->vbe_regs[vga->vbe_index] = val;
            break;
        case VBE_DISPI_INDEX_VIRT_WIDTH: {
            int w, h, line_offset;

            if (val < vga->vbe_regs[VBE_DISPI_INDEX_XRES])
                return;
            w = val;
            if (vga->vbe_regs[VBE_DISPI_INDEX_BPP] == 4)
                line_offset = w >> 1;
            else
                line_offset = w * ((vga->vbe_regs[VBE_DISPI_INDEX_BPP] + 7) >> 3);
            h = VRAM_ACTUAL_SIZE / line_offset;
            if (h < vga->vbe_regs[VBE_DISPI_INDEX_YRES])
                return;
            vga->vbe_regs[VBE_DISPI_INDEX_VIRT_WIDTH] = w;
            vga->vbe_regs[VBE_DISPI_INDEX_VIRT_HEIGHT] = h;
            vga->vbe_line_offset = line_offset;
        }
        break;
        case VBE_DISPI_INDEX_X_OFFSET:
        case VBE_DISPI_INDEX_Y_OFFSET: {
            int x;
            vga->vbe_regs[vga->vbe_index] = val;
            vga->vbe_start_addr = vga->vbe_line_offset * vga->vbe_regs[VBE_DISPI_INDEX_Y_OFFSET];
            x = vga->vbe_regs[VBE_DISPI_INDEX_X_OFFSET];
            if (vga->vbe_regs[VBE_DISPI_INDEX_BPP] == 4)
                vga->vbe_start_addr += x >> 1;
            else
                vga->vbe_start_addr += x * ((vga->vbe_regs[VBE_DISPI_INDEX_BPP] + 7) >> 3);
            vga->vbe_start_addr >>= 2;
        }
        break;
        default:
            break;
        }
    }
}

static void
device_vbe_port_writew(void *priv, uint64_t addr, uint16_t val)
{
    assert(priv == NULL);

    switch (addr) {
    case 0x1ce:
    case 0xff80:
        device_vbe_index_write(priv, val);
        break;

    case 0x1cf:
    case 0xff81:
        device_vbe_data_write(priv, val);
        break;

    default:
        assert(false);
        break;
    }
}

static io_ops_t device_vbe_port_ops = {
    .readw = device_vbe_port_readw,
    .writew = device_vbe_port_writew
};

static int
device_vga_register(void)
{
    vga_t   *vga = &device_state.vga;
    int     rc;

    rc = demu_register_port_space(0x3c0, 16, &device_vga_port_ops, NULL);
    if (rc < 0)
        goto fail1;

    rc = demu_register_port_space(0x3b4, 2, &device_vga_port_ops, NULL);
    if (rc < 0)
        goto fail2;

    rc = demu_register_port_space(0x3d4, 2, &device_vga_port_ops, NULL);
    if (rc < 0)
        goto fail3;

    rc = demu_register_port_space(0x3ba, 1, &device_vga_port_ops, NULL);
    if (rc < 0)
        goto fail4;

    rc = demu_register_port_space(0x3da, 1, &device_vga_port_ops, NULL);
    if (rc < 0)
        goto fail5;

    vga->lfb_size = VRAM_ACTUAL_SIZE;
    device_update_lfb_addr();

    vga->vbe_regs[VBE_DISPI_INDEX_ID] = VBE_DISPI_ID0;
    vga->vbe_bank_mask = ((VRAM_ACTUAL_SIZE >> 16) - 1);

    rc = demu_register_port_space(0x1ce, 4, &device_vbe_port_ops, NULL);
    if (rc < 0)
        goto fail6;

    rc = demu_register_port_space(0xff80, 4, &device_vbe_port_ops, NULL);
    if (rc < 0)
        goto fail7;

    rc = demu_register_memory_space(0xa0000, 0x20000, &device_vga_memory_ops, NULL);
    if (rc < 0)
        goto fail8;

    return 0;

fail8:
    ERR("fail8");

    demu_deregister_port_space(0xff80);

fail7:
    ERR("fail7");

    demu_deregister_port_space(0x1ce);

fail6:
    ERR("fail6");

    demu_deregister_port_space(0x3da);

fail5:
    ERR("fail5");

    demu_deregister_port_space(0x3ba);

fail4:
    ERR("fail4");

    demu_deregister_port_space(0x3d4);

fail3:
    ERR("fail3");

    demu_deregister_port_space(0x3b4);

fail2:
    ERR("fail2");

    demu_deregister_port_space(0x3c0);

fail1:
    ERR("fail1");

    return -1;
}

static void
device_vga_deregister(void)
{
    demu_deregister_memory_space(0xa0000);

    demu_deregister_port_space(0xff80);
    demu_deregister_port_space(0x1ce);
    demu_deregister_port_space(0x3da);
    demu_deregister_port_space(0x3ba);
    demu_deregister_port_space(0x3d4);
    demu_deregister_port_space(0x3b4);
    demu_deregister_port_space(0x3c0);
}

int
device_initialize()
{
    DBG("device_initialize");
    int         rc;

    device_vga_reset();

    rc = device_vga_register();
    if (rc < 0)
        goto fail1;

    return 0;

fail1:
    ERR("fail1: %s", strerror(errno));

    return -1;
}

void
device_teardown(void)
{
    device_vga_deregister();
}

/*
 * Local variables:
 * mode: C
 * c-tab-always-indent: nil
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * c-basic-indent: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
