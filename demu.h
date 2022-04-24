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

#include <netinet/in.h>
#include <xenctrl.h>
#include <vga.h>
#include "vmioplugin.h"

#ifndef  _DEMU_H
#define  _DEMU_H

#define TARGET_PAGE_SHIFT   12
#define TARGET_PAGE_SIZE    (1 << TARGET_PAGE_SHIFT)

#define MAXPATHLEN          256
#define STATEFILE_MAGIC     0x6574617453555047ull

/* define more accurate names for migraiton phases */
#define vmiop_migration_track_modified  vmiop_migration_pre_copy
#define vmiop_migration_quiesce         vmiop_migration_stop_and_copy

domid_t demu_get_domid(void);

void    *demu_map_guest_pages(xen_pfn_t pfn[], int count, int read_only, int populate);
void    *demu_map_guest_range(uint64_t addr, uint64_t size, int read_only, int populate);

void    demu_set_guest_dirty_page(xen_pfn_t pfn);
int     demu_set_guest_dirty_pages(uint64_t count, const struct pages page_list[]);

int     demu_set_guest_pages(xen_pfn_t gfn, xen_pfn_t mfn, int count, int add);

int     demu_maximum_guest_pages(void);
int     demu_translate_guest_pages(xen_pfn_t pfn[], xen_pfn_t mfn[], int count);
int     demu_release_guest_pages(xen_pfn_t mfn[], int count);

#define VRAM_RESERVED_ADDRESS   0xff000000
#define VRAM_RESERVED_SIZE      0x01000000
#define VRAM_ACTUAL_SIZE        0x00400000


uint8_t     *demu_get_vram(void);
void        demu_set_vram_addr(uint64_t);
uint64_t    demu_get_vram_addr(void);

void    demu_sync_vram_dirty_map(void);
int     demu_vram_get_page_dirty(xen_pfn_t pfn);
void    demu_vram_set_page_dirty(xen_pfn_t pfn);
void    demu_clear_vram_dirty_map(void);

typedef struct demu_space   demu_space_t;

typedef struct io_ops {
    uint8_t         (*readb)(void *priv, uint64_t addr);
    uint16_t        (*readw)(void *priv, uint64_t addr);
    uint32_t        (*readl)(void *priv, uint64_t addr);
    void            (*writeb)(void *priv, uint64_t addr, uint8_t val);
    void            (*writew)(void *priv, uint64_t addr, uint16_t val);
    void            (*writel)(void *priv, uint64_t addr, uint32_t val);
} io_ops_t;

int     demu_register_pci_config_space(const io_ops_t *ops, void *priv);
int     demu_register_port_space(uint64_t start, uint64_t size,
                                 const io_ops_t *ops, void *priv);
int     demu_register_memory_space(uint64_t start, uint64_t size,
                                   const io_ops_t *ops, void *priv);

void    demu_inject_msi(uint64_t addr, uint32_t data);
void    demu_set_pci_intx(int line, int level);

demu_space_t    *demu_find_pci_config_space(uint8_t bdf);
demu_space_t    *demu_find_port_space(uint64_t addr);
demu_space_t    *demu_find_memory_space(uint64_t addr);

uint64_t    demu_io_read(demu_space_t *space, uint64_t addr, uint64_t size);
void        demu_io_write(demu_space_t *space, uint64_t addr, uint64_t size, uint64_t val);

void    demu_deregister_pci_config_space(void);
void    demu_deregister_port_space(uint64_t start);
void    demu_deregister_memory_space(uint64_t start);

int demu_timer_start(void);
int demu_timer_stop(void);

in_port_t demu_socket_port(void);

void    demu_vlog(int priority, const char *prefix, const char *fmt, va_list ap);
void    demu_log_context(const char *prefix, const char *fmt, ...);
void    demu_log(int priority, const char *prefix, const char *fmt, ...);

typedef enum {
    statefile_top,
    statefile_vga,
    statefile_vmio,
    statefile_closed
} statefile_section_t;

enum {
    demu_state_close,
    demu_state_demu,
    demu_state_vga,
    demu_state_vmiope,
    demu_state_max
};

struct record_header {
    union {
        uint64_t hblock;
        struct {
            uint16_t type;
            uint16_t stype;
            uint32_t length;
        };
    };
};

struct demu_record {
    struct record_header header;
    union {
        uint8_t  c_data[8];
        uint64_t l_data[1];
    };
};

int write_record(struct demu_record *rec);
int get_sent_stats(uint64_t* sent, uint64_t* remaining,int reset);

int demu_open_state(statefile_section_t sec);
int demu_close_state(statefile_section_t sec);
int demu_write_state(const void *buf, size_t count);
int demu_read_state(void *buf, size_t count);

int demu_init_migrate(int fd);
int demu_start_migrate(void);
int demu_migrate(void);
int demu_finish_migrate(void);
void demu_migrate_abort(void);
int demu_migrate_phase_stop(void);
void demu_migrate_cleanup(void);
void demu_shutdown(void);
int demu_trigger_resume(void);

#endif  /* _DEMU_H */

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
