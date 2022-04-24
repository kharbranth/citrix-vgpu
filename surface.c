/*
 * Copyright (c) 2012, Citrix Systems Inc.
 * All rights reserved.
 *
 * Portions also:
 * Copyright (c) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <err.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "log.h"
#include "demu.h"
#include "device.h"
#include "surface.h"

#define GMODE_TEXT      0
#define GMODE_GRAPHIC   1
#define GMODE_BLANK     2

#define CH_ATTR_SIZE (160 * 100)

typedef struct surface {
    shared_surface_t    *shared;
    uint32_t            offset;
    uint8_t             *vram;
    uint32_t            font_offsets[2];
    int                 graphic_mode;
    uint8_t             shift_control;
    uint8_t             double_scan;
    uint32_t            line_offset;
    uint32_t            line_compare;
    uint32_t            start_addr;
    uint32_t            bits_per_pixel;
    uint32_t            bytes_per_pixel;
    uint32_t            linesize;
    uint32_t            last_line_offset;
    uint8_t             last_cw;
    uint8_t             last_ch;
    uint32_t            last_width;
    uint32_t            last_height;
    uint32_t            last_scr_width;
    uint32_t            last_scr_height;
    uint32_t            last_depth;
    uint8_t             cursor_start;
    uint8_t             cursor_end;
    uint32_t            cursor_offset;
    unsigned int        (*rgb_to_pixel)(unsigned int r, unsigned int g, unsigned b);
    uint32_t            last_palette[256];
    uint32_t            last_ch_attr[CH_ATTR_SIZE];
} surface_t;

static surface_t    surface_state;

static uint32_t     expand4[256];
static uint16_t     expand2[256];
static uint8_t      expand4to8[16];

static uint8_t get_ar_index(surface_t *s)
{
    vga_t   *vga = device_get_vga();

    return vga->ar_index;
}

static uint8_t get_ar(surface_t *s, int reg)
{
    vga_t   *vga = device_get_vga();

    return vga->ar[reg];
}

static uint8_t get_cr(surface_t *s, int reg)
{
    vga_t   *vga = device_get_vga();

    return vga->cr[reg];
}

static uint8_t get_sr(surface_t *s, int reg)
{
    vga_t   *vga = device_get_vga();

    return vga->sr[reg];
}

static uint8_t get_gr(surface_t *s, int reg)
{
    vga_t   *vga = device_get_vga();

    return vga->gr[reg];
}

static uint8_t get_palette(surface_t *s, int offset)
{
    vga_t   *vga = device_get_vga();

    return vga->palette[offset];
}

static int is_dac_8bit(surface_t *s)
{
    vga_t   *vga = device_get_vga();

    return !!vga->dac_8bit;
}

static int test_and_clear_plane2(surface_t *s)
{
    vga_t   *vga = device_get_vga();
    uint32_t val;

    val = vga->plane_updated & (1 << 2);
    if (val)
        vga->plane_updated = 0;

    return !!val;
}

static uint16_t get_vbe_regs(surface_t *s, int reg)
{
    vga_t   *vga = device_get_vga();

    return vga->vbe_regs[reg];
}

static uint32_t get_vbe_start_addr(surface_t *s)
{
    vga_t   *vga = device_get_vga();

    return vga->vbe_start_addr;
}

static uint32_t get_vbe_line_offset(surface_t *s)
{
    vga_t   *vga = device_get_vga();

    return vga->vbe_line_offset;
}

static void get_offsets(surface_t *s,
                        uint32_t *pline_offset,
                        uint32_t *pstart_addr,
                        uint32_t *pline_compare)
{
    uint32_t start_addr, line_offset, line_compare;

    if (get_vbe_regs(s, VBE_DISPI_INDEX_ENABLE) & VBE_DISPI_ENABLED) {
        line_offset = get_vbe_line_offset(s);
        start_addr = get_vbe_start_addr(s);
        line_compare = 65535;
    } else {
        /* compute line_offset in bytes */
        line_offset = get_cr(s, 0x13);
        line_offset <<= 3;

        /* starting address */
        start_addr = get_cr(s, 0x0d) | (get_cr(s, 0x0c) << 8);

        /* line compare */
        line_compare = get_cr(s, 0x18) |
                       ((get_cr(s, 0x07) & 0x10) << 4) |
                       ((get_cr(s, 0x09) & 0x40) << 3);
    }
    *pline_offset = line_offset;
    *pstart_addr = start_addr;
    *pline_compare = line_compare;
}

static int get_bpp(surface_t *s)
{
    int ret;

    if (get_vbe_regs(s, VBE_DISPI_INDEX_ENABLE) & VBE_DISPI_ENABLED) {
        ret = get_vbe_regs(s, VBE_DISPI_INDEX_BPP);
    } else  {
        ret = 0;
    }

    return ret;
}

static void get_resolution(surface_t *s, int *pwidth, int *pheight)
{
    int width, height;

    if (get_vbe_regs(s, VBE_DISPI_INDEX_ENABLE) & VBE_DISPI_ENABLED) {
        width = get_vbe_regs(s, VBE_DISPI_INDEX_XRES);
        height = get_vbe_regs(s, VBE_DISPI_INDEX_YRES);
    } else  {
        width = (get_cr(s, 0x01) + 1) * 8;
        height = get_cr(s, 0x12) |
                 ((get_cr(s, 0x07) & 0x02) << 7) |
                 ((get_cr(s, 0x07) & 0x40) << 3);
        height = (height + 1);
    }
    *pwidth = width;
    *pheight = height;
}

static inline int c6_to_8(int v)
{
    int b;
    v &= 0x3f;
    b = v & 1;
    return (v << 2) | (b << 1) | b;
}

/* return true if the palette was modified */
static int vgpu_update_palette16(surface_t *s)
{
    int full_update, i;
    uint32_t v, col, *palette;

    full_update = 0;
    palette = s->last_palette;
    for(i = 0; i < 16; i++) {
        v = get_ar(s, i);
        if (get_ar(s, 0x10) & 0x80)
            v = ((get_ar(s, 0x14) & 0xf) << 4) | (v & 0xf);
        else
            v = ((get_ar(s, 0x14) & 0xc) << 4) | (v & 0x3f);
        v = v * 3;
        col = s->rgb_to_pixel(c6_to_8(get_palette(s, v)),
                              c6_to_8(get_palette(s, v + 1)),
                              c6_to_8(get_palette(s, v + 2)));
        if (col != palette[i]) {
            full_update = 1;
            palette[i] = col;
        }
    }
    return full_update;
}

static int vgpu_update_palette256(surface_t *s)
{
    int full_update, i;
    uint32_t v, col, *palette;

    full_update = 0;
    palette = s->last_palette;
    v = 0;
    for(i = 0; i < 256; i++) {
        if (is_dac_8bit(s)) {
            col = s->rgb_to_pixel(get_palette(s, v),
                                  get_palette(s, v + 1),
                                  get_palette(s, v + 2));
        } else {
            col = s->rgb_to_pixel(c6_to_8(get_palette(s, v)),
                                  c6_to_8(get_palette(s, v + 1)),
                                  c6_to_8(get_palette(s, v + 2)));
        }
        if (col != palette[i]) {
            full_update = 1;
            palette[i] = col;
        }
        v += 3;
    }
    return full_update;
}

/* update start_addr and line_offset. Return TRUE if modified */
static int update_basic_params(surface_t *s)
{
    int full_update;
    uint32_t start_addr, line_offset, line_compare;

    full_update = 0;

    get_offsets(s, &line_offset, &start_addr, &line_compare);

    if (line_offset != s->line_offset ||
            start_addr != s->start_addr ||
            line_compare != s->line_compare) {
        s->line_offset = line_offset;
        s->start_addr = start_addr;
        s->line_compare = line_compare;
        full_update = 1;
    }
    return full_update;
}

static inline unsigned int rgb_to_pixel8(unsigned int r, unsigned int g, unsigned b)
{
    return ((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6);
}

static inline unsigned int rgb_to_pixel15(unsigned int r, unsigned int g, unsigned b)
{
    return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
}

static inline unsigned int rgb_to_pixel16(unsigned int r, unsigned int g, unsigned b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static inline unsigned int rgb_to_pixel32(unsigned int r, unsigned int g, unsigned b)
{
    return (r << 16) | (g << 8) | b;
}

static unsigned int rgb_to_pixel8_dup(unsigned int r, unsigned int g, unsigned b)
{
    unsigned int col;
    col = rgb_to_pixel8(r, g, b);
    col |= col << 8;
    col |= col << 16;
    return col;
}

static unsigned int rgb_to_pixel15_dup(unsigned int r, unsigned int g, unsigned b)
{
    unsigned int col;
    col = rgb_to_pixel15(r, g, b);
    col |= col << 16;
    return col;
}

static unsigned int rgb_to_pixel16_dup(unsigned int r, unsigned int g, unsigned b)
{
    unsigned int col;
    col = rgb_to_pixel16(r, g, b);
    col |= col << 16;
    return col;
}

static unsigned int rgb_to_pixel32_dup(unsigned int r, unsigned int g, unsigned b)
{
    unsigned int col;
    col = rgb_to_pixel32(r, g, b);
    return col;
}

#define NB_DEPTHS 4

static inline int get_depth_index(surface_t *s)
{
    switch(s->bits_per_pixel) {
    default:
    case 8:
        return 0;
    case 15:
        return 1;
    case 16:
        return 2;
    case 32:
        return 3;
    }
}

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

#define cbswap_32(__x) \
((uint32_t)( \
        (((uint32_t)(__x) & (uint32_t)0x000000ffUL) << 24) | \
        (((uint32_t)(__x) & (uint32_t)0x0000ff00UL) <<  8) | \
        (((uint32_t)(__x) & (uint32_t)0x00ff0000UL) >>  8) | \
        (((uint32_t)(__x) & (uint32_t)0xff000000UL) >> 24) ))

#define PAT(x) cbswap_32(x)

static const uint32_t dmask16[16] = {
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

static const uint32_t dmask4[4] = {
    PAT(0x00000000),
    PAT(0x0000ffff),
    PAT(0xffff0000),
    PAT(0xffffffff),
};

#undef PAT

#define BIG 0

#define GET_PLANE(data, p) (((data) >> ((p) * 8)) & 0xff)

#define xglue(_x, _y) _x##_y
#define glue(_x, _y) xglue(_x, _y)

#define cpu_to_32wu(_p, _v) \
    *(_p) = (_v)

#define lduw_raw(_p) \
    (*(uint16_t *)(_p))

#define DEPTH 8
#include "template.h"

#define DEPTH 15
#include "template.h"

#define DEPTH 16
#include "template.h"

#define DEPTH 32
#include "template.h"

typedef void vga_draw_glyph8_func(uint8_t *d, int linesize,
                                  const uint8_t *font_ptr, int h,
                                  uint32_t fgcol, uint32_t bgcol);
typedef void vga_draw_glyph9_func(uint8_t *d, int linesize,
                                  const uint8_t *font_ptr, int h,
                                  uint32_t fgcol, uint32_t bgcol, int dup9);

static vga_draw_glyph8_func *vga_draw_glyph8_table[NB_DEPTHS] = {
    __vga_draw_glyph8_8,
    __vga_draw_glyph8_16,
    __vga_draw_glyph8_16,
    __vga_draw_glyph8_32,
};

static vga_draw_glyph8_func *vga_draw_glyph16_table[NB_DEPTHS] = {
    __vga_draw_glyph16_8,
    __vga_draw_glyph16_16,
    __vga_draw_glyph16_16,
    __vga_draw_glyph16_32,
};

static vga_draw_glyph9_func *vga_draw_glyph9_table[NB_DEPTHS] = {
    __vga_draw_glyph9_8,
    __vga_draw_glyph9_16,
    __vga_draw_glyph9_16,
    __vga_draw_glyph9_32,
};

static const uint8_t cursor_glyph[32 * 4] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

enum {
    VGA_DRAW_LINE2,
    VGA_DRAW_LINE2D2,
    VGA_DRAW_LINE4,
    VGA_DRAW_LINE4D2,
    VGA_DRAW_LINE8D2,
    VGA_DRAW_LINE8,
    VGA_DRAW_LINE15,
    VGA_DRAW_LINE16,
    VGA_DRAW_LINE24,
    VGA_DRAW_LINE32,
    VGA_DRAW_LINE_NB,
};

typedef void vga_draw_line_func(uint32_t *palette, uint32_t plane_enable, uint8_t *d,
                                const uint8_t *s, int width);

static vga_draw_line_func *vga_draw_line_table[NB_DEPTHS * VGA_DRAW_LINE_NB] = {
    __vga_draw_line2_8,
    __vga_draw_line2_16,
    __vga_draw_line2_16,
    __vga_draw_line2_32,

    __vga_draw_line2d2_8,
    __vga_draw_line2d2_16,
    __vga_draw_line2d2_16,
    __vga_draw_line2d2_32,

    __vga_draw_line4_8,
    __vga_draw_line4_16,
    __vga_draw_line4_16,
    __vga_draw_line4_32,

    __vga_draw_line4d2_8,
    __vga_draw_line4d2_16,
    __vga_draw_line4d2_16,
    __vga_draw_line4d2_32,

    __vga_draw_line8d2_8,
    __vga_draw_line8d2_16,
    __vga_draw_line8d2_16,
    __vga_draw_line8d2_32,

    __vga_draw_line8_8,
    __vga_draw_line8_16,
    __vga_draw_line8_16,
    __vga_draw_line8_32,

    __vga_draw_line15_8,
    __vga_draw_line15_15,
    __vga_draw_line15_16,
    __vga_draw_line15_32,

    __vga_draw_line16_8,
    __vga_draw_line16_15,
    __vga_draw_line16_16,
    __vga_draw_line16_32,

    __vga_draw_line24_8,
    __vga_draw_line24_15,
    __vga_draw_line24_16,
    __vga_draw_line24_32,

    __vga_draw_line32_8,
    __vga_draw_line32_15,
    __vga_draw_line32_16,
    __vga_draw_line32_32,
};

typedef unsigned int rgb_to_pixel_dup_func(unsigned int r, unsigned int g, unsigned b);

static rgb_to_pixel_dup_func *rgb_to_pixel_dup_table[NB_DEPTHS] = {
    rgb_to_pixel8_dup,
    rgb_to_pixel15_dup,
    rgb_to_pixel16_dup,
    rgb_to_pixel32_dup,
};

int
surface_initialize(void)
{
    int i;
    int j;
    int v;
    int b;

    for (i = 0; i < 256; i++) {
        v = 0;
        for (j = 0; j < 8; j++)
            v |= ((i >> j) & 1) << (j * 4);
        expand4[i] = v;

        v = 0;
        for (j = 0; j < 4; j++)
            v |= ((i >> (2 * j)) & 3) << (j * 4);
        expand2[i] = v;
    }

    for (i = 0; i < 16; i++) {
        v = 0;
        for (j = 0; j < 4; j++) {
            b = ((i >> j) & 1);
            v |= b << (2 * j);
            v |= b << (2 * j + 1);
        }
        expand4to8[i] = v;
    }

    surface_state.graphic_mode = -1;
    surface_state.vram = demu_get_vram();
    surface_state.shared = (shared_surface_t *)(surface_state.vram +
                           VRAM_RESERVED_SIZE -
                           TARGET_PAGE_SIZE);

    memset(surface_state.shared, 0, TARGET_PAGE_SIZE);
    surface_state.shared->port = demu_socket_port();

    INFO("port = %u", surface_state.shared->port);

    return 0;
}

#define P2ROUNDUP(_x, _a) -(-(_x) & -(_a))

void
surface_resize(uint32_t offset, uint32_t linesize, uint32_t width, uint32_t height, uint32_t depth)
{
    DBG("%ux%ux%u @ %x", width, height, depth, offset);

    surface_state.bits_per_pixel = depth;
    surface_state.bytes_per_pixel = P2ROUNDUP(depth, 8) / 8;
    surface_state.linesize = linesize;
    surface_state.offset = offset;

    surface_state.last_scr_width = width;
    surface_state.last_scr_height = height;

    surface_state.shared->offset = offset;
    surface_state.shared->linesize = linesize;
    surface_state.shared->width = width;
    surface_state.shared->height = height;
    surface_state.shared->depth = depth;
}

void
surface_update(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    surface_state.shared->update++;
}

void
surface_get_dimensions(uint32_t *width, uint32_t *height, uint32_t *depth)
{
    *width = surface_state.last_scr_width;
    *height = surface_state.last_scr_height;
    *depth = surface_state.bits_per_pixel;
}

void *
surface_get_buffer(void)
{
    return surface_state.vram + surface_state.offset;
}

static void
surface_draw_text(surface_t *s, int full_update)
{
    int cx, cy, cheight, cw, ch, cattr, height, width, ch_attr;
    int cx_min, cx_max, linesize, x_incr;
    uint32_t offset, fgcol, bgcol, v, cursor_offset;
    uint8_t *d1, *d, *src, *s1, *dest, *cursor_ptr;
    const uint8_t *font_ptr, *font_base[2];
    int dup9, line_offset, depth_index;
    uint32_t *palette;
    uint32_t *ch_attr_ptr;
    vga_draw_glyph8_func *vga_draw_glyph8;
    vga_draw_glyph9_func *vga_draw_glyph9;

    /* compute font data address (in plane 2) */
    v = get_sr(s, 3);
    offset = (((v >> 4) & 1) | ((v << 1) & 6)) * 8192 * 4 + 2;
    if (offset != s->font_offsets[0]) {
        s->font_offsets[0] = offset;
        full_update = 1;
    }
    font_base[0] = s->vram + offset;

    offset = (((v >> 5) & 1) | ((v >> 1) & 6)) * 8192 * 4 + 2;
    font_base[1] = s->vram + offset;
    if (offset != s->font_offsets[1]) {
        s->font_offsets[1] = offset;
        full_update = 1;
    }

    if (test_and_clear_plane2(s)) {
        /* if the plane 2 was modified since the last display, it
           indicates the font may have been modified */
        full_update = 1;
    }

    full_update |= update_basic_params(s);

    line_offset = s->line_offset;
    s1 = s->vram + (s->start_addr * 4);

    /* total width & height */
    cheight = (get_cr(s, 9) & 0x1f) + 1;
    cw = 8;
    if (!(get_sr(s, 1) & 0x01))
        cw = 9;
    if (get_sr(s, 1) & 0x08)
        cw = 16; /* NOTE: no 18 pixel wide */
    width = (get_cr(s, 0x01) + 1);
    if (get_cr(s, 0x06) == 100) {
        /* ugly hack for CGA 160x100x16 - explain me the logic */
        height = 100;
    } else {
        height = get_cr(s, 0x12) |
                 ((get_cr(s, 0x07) & 0x02) << 7) |
                 ((get_cr(s, 0x07) & 0x40) << 3);
        height = (height + 1) / cheight;
    }
    if ((height * width) > CH_ATTR_SIZE) {
        /* better than nothing: exit if transient size is too big */
        return;
    }

    if (width * cw != s->last_scr_width ||
            height * cheight != s->last_scr_height ||
            cw != s->last_cw ||
            cheight != s->last_ch ||
            s->last_depth) {
        linesize = width * cw * 32 / 8;
        surface_resize(VRAM_ACTUAL_SIZE, linesize, width * cw, height * cheight, 32);
        s->last_depth = 0;
        s->last_width = width;
        s->last_height = height;
        s->last_ch = cheight;
        s->last_cw = cw;
        full_update = 1;
    }

    s->rgb_to_pixel = rgb_to_pixel_dup_table[get_depth_index(s)];
    full_update |= vgpu_update_palette16(s);
    palette = s->last_palette;
    x_incr = cw * s->bytes_per_pixel;

    cursor_offset = ((get_cr(s, 0x0e) << 8) | get_cr(s, 0x0f)) - s->start_addr;
    if (cursor_offset != s->cursor_offset ||
            get_cr(s, 0xa) != s->cursor_start ||
            get_cr(s, 0xb) != s->cursor_end) {
        /* if the cursor position changed, we update the old and new
           chars */
        if (s->cursor_offset < CH_ATTR_SIZE)
            s->last_ch_attr[s->cursor_offset] = -1;
        if (cursor_offset < CH_ATTR_SIZE)
            s->last_ch_attr[cursor_offset] = -1;
        s->cursor_offset = cursor_offset;
        s->cursor_start = get_cr(s, 0xa);
        s->cursor_end = get_cr(s, 0xb);
    }
    cursor_ptr = s->vram + (s->start_addr + cursor_offset) * 4;

    depth_index = get_depth_index(s);
    if (cw == 16)
        vga_draw_glyph8 = vga_draw_glyph16_table[depth_index];
    else
        vga_draw_glyph8 = vga_draw_glyph8_table[depth_index];
    vga_draw_glyph9 = vga_draw_glyph9_table[depth_index];

    dest = s->vram + s->offset;
    linesize = s->linesize;
    ch_attr_ptr = s->last_ch_attr;
    for(cy = 0; cy < height; cy++) {
        d1 = dest;
        src = s1;
        cx_min = width;
        cx_max = -1;
        for(cx = 0; cx < width; cx++) {
            ch_attr = *(uint16_t *)src;
            if (full_update || ch_attr != *ch_attr_ptr) {
                if (cx < cx_min)
                    cx_min = cx;
                if (cx > cx_max)
                    cx_max = cx;
                *ch_attr_ptr = ch_attr;
                ch = ch_attr & 0xff;
                cattr = ch_attr >> 8;
                font_ptr = font_base[(cattr >> 3) & 1];
                font_ptr += 32 * 4 * ch;
                bgcol = palette[cattr >> 4];
                fgcol = palette[cattr & 0x0f];
                if (cw != 9) {
                    vga_draw_glyph8(d1, linesize, font_ptr, cheight, fgcol, bgcol);
                } else {
                    dup9 = 0;
                    if (ch >= 0xb0 && ch <= 0xdf && (get_ar(s, 0x10) & 0x04))
                        dup9 = 1;
                    vga_draw_glyph9(d1, linesize, font_ptr, cheight, fgcol, bgcol, dup9);
                }
                if (src == cursor_ptr &&
                        !(get_cr(s, 0x0a) & 0x20)) {
                    int line_start, line_last, h;
                    /* draw the cursor */
                    line_start = get_cr(s, 0x0a) & 0x1f;
                    line_last = get_cr(s, 0x0b) & 0x1f;
                    /* XXX: check that */
                    if (line_last > cheight - 1)
                        line_last = cheight - 1;
                    if (line_last >= line_start && line_start < cheight) {
                        h = line_last - line_start + 1;
                        d = d1 + linesize * line_start;
                        if (cw != 9) {
                            vga_draw_glyph8(d, linesize, cursor_glyph, h, fgcol, bgcol);
                        } else {
                            vga_draw_glyph9(d, linesize, cursor_glyph, h, fgcol, bgcol, 1);
                        }
                    }
                }
            }
            d1 += x_incr;
            src += 4;
            ch_attr_ptr++;
            if (cx_max != -1)
                surface_update(cx_min * cw, cy * cheight,
                               (cx_max - cx_min + 1) * cw, cheight);

        }
        dest += linesize * cheight;
        s1 += line_offset;
    }
}

static void
surface_draw_graphic(surface_t *s, int full_update)
{
    int y1, y, update, linesize, y_start, double_scan, mask, depth;
    int width, height, shift_control, line_offset, bwidth, bits;
    xen_pfn_t page0, page1;
    int disp_width, multi_scan, multi_run;
    uint8_t *d;
    uint32_t v, addr1, addr;
    vga_draw_line_func *vga_draw_line;

    full_update |= update_basic_params(s);

    get_resolution(s, &width, &height);
    disp_width = width;

    shift_control = (get_gr(s, 0x05) >> 5) & 3;
    double_scan = (get_cr(s, 0x09) >> 7);
    if (shift_control != 1) {
        multi_scan = (((get_cr(s, 0x09) & 0x1f) + 1) << double_scan) - 1;
    } else {
        /* in CGA modes, multi_scan is ignored */
        multi_scan = double_scan;
    }
    multi_run = multi_scan;
    if (shift_control != s->shift_control ||
            double_scan != s->double_scan) {
        full_update = 1;
        s->shift_control = shift_control;
        s->double_scan = double_scan;
    }
    if (shift_control == 1 && (get_sr(s, 0x01) & 8)) {
        disp_width <<= 1;
    }

    if (shift_control == 0) {
        if (get_sr(s, 0x01) & 8) {
            disp_width <<= 1;
        }
    } else if (shift_control == 1) {
        if (get_sr(s, 0x01) & 8) {
            disp_width <<= 1;
        }
    }

    depth = get_bpp(s);
    if (s->line_offset != s->last_line_offset ||
            disp_width != s->last_scr_width ||
            height != s->last_scr_height ||
            s->last_depth != depth) {

        DBG("depth = %d", depth);
        if (depth == 32) {
            surface_resize(s->start_addr * 4, s->line_offset, disp_width, height, 32);
        } else {
            linesize = disp_width * 32 / 8;
            surface_resize(VRAM_ACTUAL_SIZE, linesize, disp_width, height, 32);
        }

        s->last_width = disp_width;
        s->last_height = height;
        s->last_line_offset = s->line_offset;
        s->last_depth = depth;
        full_update = 1;
    }

    s->rgb_to_pixel = rgb_to_pixel_dup_table[get_depth_index(s)];

    if (shift_control == 0) {
        full_update |= vgpu_update_palette16(s);
        if (get_sr(s, 0x01) & 8) {
            v = VGA_DRAW_LINE4D2;
        } else {
            v = VGA_DRAW_LINE4;
        }
        bits = 4;
    } else if (shift_control == 1) {
        full_update |= vgpu_update_palette16(s);
        if (get_sr(s, 0x01) & 8) {
            v = VGA_DRAW_LINE2D2;
        } else {
            v = VGA_DRAW_LINE2;
        }
        bits = 4;
    } else {
        switch(get_bpp(s)) {
        default:
        case 0:
            full_update |= vgpu_update_palette256(s);
            v = VGA_DRAW_LINE8D2;
            bits = 4;
            break;
        case 8:
            full_update |= vgpu_update_palette256(s);
            v = VGA_DRAW_LINE8;
            bits = 8;
            break;
        case 15:
            v = VGA_DRAW_LINE15;
            bits = 16;
            break;
        case 16:
            v = VGA_DRAW_LINE16;
            bits = 16;
            break;
        case 24:
            v = VGA_DRAW_LINE24;
            bits = 24;
            break;
        case 32:
            v = VGA_DRAW_LINE32;
            bits = 32;
            break;
        }
    }

    vga_draw_line = vga_draw_line_table[v * NB_DEPTHS + get_depth_index(s)];

    line_offset = s->line_offset;

    addr1 = (s->start_addr * 4);
    bwidth = (width * bits + 7) / 8;
    y_start = -1;
    d = s->vram + s->offset;
    linesize = s->linesize;
    y1 = 0;

    demu_sync_vram_dirty_map();

    for(y = 0; y < height; y++) {
        addr = addr1;
        if (!(get_cr(s, 0x17) & 1)) {
            int shift;
            /* CGA compatibility handling */
            shift = 14 + ((get_cr(s, 0x17) >> 6) & 1);
            addr = (addr & ~(1 << shift)) | ((y1 & 1) << shift);
        }
        if (!(get_cr(s, 0x17) & 2)) {
            addr = (addr & ~0x8000) | ((y1 & 2) << 14);
        }
        page0 = addr >> TARGET_PAGE_SHIFT;
        page1 = (addr + bwidth - 1) >> TARGET_PAGE_SHIFT;
        update = full_update;
        if (!update) {
            xen_pfn_t page;

            for (page = page0; page <= page1; page++)
                update |= demu_vram_get_page_dirty(page);
        }
        if (update) {
            uint32_t plane_enable;

            if (y_start < 0)
                y_start = y;

            plane_enable = get_ar(s, 0x12) & 0xf;
            if (s->offset != s->start_addr * 4)
                vga_draw_line(s->last_palette, plane_enable, d, s->vram + addr, width);
        } else {
            if (y_start >= 0) {
                /* flush to display */
                surface_update(0, y_start,
                               disp_width, y - y_start);
                y_start = -1;
            }
        }
        if (!multi_run) {
            mask = (get_cr(s, 0x17) & 3) ^ 3;
            if ((y1 & mask) == mask)
                addr1 += line_offset;
            y1++;
            multi_run = multi_scan;
        } else {
            multi_run--;
        }
        /* line compare acts on the displayed lines */
        if (y == s->line_compare)
            addr1 = 0;
        d += linesize;
    }
    if (y_start >= 0) {
        /* flush to display */
        surface_update(0, y_start,
                       disp_width, y - y_start);
        y_start = -1;
    }

    demu_clear_vram_dirty_map();
}

static void
surface_draw_blank(surface_t *s, int full_update)
{
    int val;

    if (!full_update)
        return;

    if (s->last_scr_width <= 0 || s->last_scr_height <= 0)
        return;

    s->rgb_to_pixel = rgb_to_pixel_dup_table[get_depth_index(s)];
    if (s->last_depth == 8)
        val = s->rgb_to_pixel(0, 0, 0);
    else
        val = 0;

    memset(s->vram + s->offset, val,
           s->last_scr_width * s->last_scr_height * s->bytes_per_pixel);

    surface_update(0, 0,
                   s->last_scr_width, s->last_scr_height);
}

void
surface_refresh(int full_update)
{
    surface_t *s = &surface_state;
    int graphic_mode;

    if (!(get_ar_index(s) & 0x20)) {
        graphic_mode = GMODE_BLANK;
    } else {
        graphic_mode = get_gr(s, 6) & 1;
    }

    if (graphic_mode != s->graphic_mode) {
        s->graphic_mode = graphic_mode;

        switch(s->graphic_mode) {
        case GMODE_TEXT:
            DBG("text");
            break;
        case GMODE_GRAPHIC:
            DBG("graphic");
            break;
        case GMODE_BLANK:
        default:
            DBG("blank");
            break;
        }

        full_update = 1;
    }

    switch(graphic_mode) {
    case GMODE_TEXT:
        surface_draw_text(s, full_update);
        break;
    case GMODE_GRAPHIC:
        surface_draw_graphic(s, full_update);
        break;
    case GMODE_BLANK:
    default:
        surface_draw_blank(s, full_update);
        break;
    }
}

void
surface_teardown(void)
{
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
