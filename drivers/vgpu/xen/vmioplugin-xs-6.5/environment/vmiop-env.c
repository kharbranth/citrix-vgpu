/*
 * Copyright (c) 2014, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file vmiop-env.c
 *
 * @brief
 * Environmental functions for vmioplugin API in qemu-dm.
 */

/**
 * @page vmiop-env
 *
 * The vmiop-env module implements the host environment for 
 * vmioplugin API.  It provides the facilities available to 
 * plugins, locates plugins based on the qemu-dm configuration,
 * and loads and initializes plugins.
 *
 * The implementations are grouped as follows:
 * - @ref DataStructureSupportImpl
 * - @ref EmulationSupportImpl
 * - @ref TaskSupportImpl
 * - @ref MonitorImpl
 * - @ref PluginManagementImpl
 * - @ref BufferManagementImpl
 */

/**
 * @cond vmiopeForwardDeclarations
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
/*!< Enable strchrnul() in header file */
#endif /* _GNU_SOURCE */
/**
 * @endcond 
 */

#include <stdlib.h>
#include <unistd.h>
#include <sys/unistd.h>
#include <time.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>
#include <pthread.h>
#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <vmioplugin.h>
#include <vmiop-env.h>
#include <xenctrl.h>
#include <signal.h>
#include <syslog.h>

#include <demu.h>

/* List of VRAM mfn */
extern xen_pfn_t vram_mfn[];

static vmiop_plugin_t *vmiop_plugin;

vmiop_bool_t vmiop_support_discard_presentation_surface_params = vmiop_true;

/**********************************************************************/
/**
 * @defgroup DataStructureSupportImpl Data Structure Support
 */
/**********************************************************************/
/*@{*/

#define VMIOPD_NS_PER_SEC (1000000000ull)
/*!< nanoseconds per second for time conversions */

/**
 * Insert entry in a list.
 *
 * @param[in,out] list_p Reference to list head pointer.
 * @param[in] link_p Reference to link pair in the object.
 */

static void
vmiope_list_insert(vmiop_list_header_ref_t *list_p,
                   vmiop_list_header_ref_t link_p)
{
    link_p->next = (*list_p);
    if ((*list_p) != NULL) {
        (*list_p)->prev = link_p;
    } else {
        link_p->prev = NULL;
    }
    (*list_p) = link_p;
}

/**
 * Remove entry from a list.
 *
 * @param[in,out] list_p Reference to list head pointer.
 * @param[in] link_p Reference to link pair in the object.
 */

static void
vmiope_list_remove(vmiop_list_header_ref_t *list_p,
                   vmiop_list_header_ref_t link_p)
{
    if (link_p->prev != NULL) {
        link_p->prev->next = link_p->next;
    } else {
        (*list_p) = link_p->next;
    }
    if (link_p->next != NULL) {
        link_p->next->prev = link_p->prev;
    }
    link_p->prev = NULL;
    link_p->next = NULL;
}

/**
 * Reference to function to apply to a list.
 *
 * @param[in] private_object Reference to private object
 * @param[in] link_p    Reference to list element
 * @returns vmiop_bool_t:
 * -            vmiop_true  Terminate loop
 * -            vmiop_false Continue loop
 */

typedef vmiop_bool_t
(*vmiope_list_function_t)(void *private_object,
                          vmiop_list_header_t *link_p);

/**
 * Apply function to a list.
 *
 * @param[in] list_p Reference to list head pointer.
 * @param[in] function_p Reference to function to apply.
 * @param[in] private_object Reference to private object
 *                           to passs to function.
 * @returns vmiop_bool_t:
 * -            vmiop_true: Function terminated loop
 * -            vmiop_false: Function did not terminate loop
 */

static vmiop_bool_t
vmiope_list_apply(vmiop_list_header_ref_t *list_p,
                  vmiope_list_function_t function_p,
                  void *private_object)
{
    vmiop_list_header_ref_t link_p;

    for (link_p = (*list_p);
         link_p != NULL;
         link_p = link_p->next) {
        if (function_p(private_object,
                       link_p)) {
            return(vmiop_true);
        }
    }
    return(vmiop_false);
}

/**
 * Handle assignment next available handle
 */

static vmiop_handle_t next_handle = ((vmiop_handle_t) 1u);

/**
 * Handle assignment.
 *
 * The next available handle is returned.  At present, freed
 * handles are not recycled, since the number of handles needed
 * over time is small.
 *
 * @param[out] handle_p Reference to variable to receive new handle
 * @returns Error code:
 * -            vmiop_success           Handle assigned
 * -            vmiop_error_resource    No handle available
 */

static vmiop_error_t
vmiope_handle_alloc(vmiop_handle_t *handle_p)
{
    if ((next_handle + 1) == VMIOP_HANDLE_NULL) {
        *handle_p = VMIOP_HANDLE_NULL;
        return(vmiop_error_resource);
    }
    *handle_p = next_handle++;
    return(vmiop_success);
}

/**
 * Release a handle.
 *
 * At present, handles are not recycled, as noted above, due to lack
 * of need.
 *
 * @param[in] handle    Handle to release
 * @returns Error code:
 * -            vmiop_success           Handle released
 * -            vmiop_error_inval       handle is VMIOP_HANDLE_NULL or
 *                                      is not a valid handle
 */

static inline vmiop_error_t
vmiope_handle_free(vmiop_handle_t handle)
{
    if (handle == VMIOP_HANDLE_NULL) {
        return(vmiop_error_inval);
    }
    return(vmiop_success);
}

#define VMIOPE_OBJECT_TABLE_BLOCK 128
/*!< number of objects per table record */

typedef struct vmiope_object_table_s *vmiope_object_table_ref_t;
/*!< reference to an object table block */

typedef uint64_t vmiope_object_table_element_t[1];
/*!< type for dummy element in table block */

/**
 * Multi-level table block index by handle.
 *
 * A multi-level table is formed of a linked list of
 * fixed-size blocks (arrays) of objects, indexed by the handle
 * number.  Allocating a handle may use an existing slot or, if
 * all slots are active, allocate a new block.  This header
 * is present in every block, and is wrapped by the object-specific
 * block which contains the objects.
 *
 * Locking is the responsibility of the caller of the access routines.
 * A lookup does not require locking, as the space is never freed.
 */

typedef struct vmiope_object_table_s {
    vmiope_object_table_ref_t next; /*!< next in list */
    vmiop_handle_t base_handle;     /*!< first handle in block */
    uint32_t num_free;          /*!< number of free objects */
    vmiope_object_table_element_t element;  /*!< dummy element for alignment */
} vmiope_object_table_t;

/**
 * Callback to test if object table entry is free.
 *
 * @param[in] object_p          Pointer to object 
 * @returns vmiop_bool_t        True if free, false if in use
 *                              or invalid index.
 */

typedef vmiop_bool_t
(*vmiope_object_is_free_t)(void *object_p);

/**
 * Object table desription.
 *
 * This structure is passed by reference to the allocation and lookup routines.
 */

typedef struct vmiope_object_table_defn_s {
    vmiope_object_table_ref_t head;     /*!< head of table list */
    uint32_t table_limit;           /*!< limit on index values */
    vmiope_object_is_free_t is_free;    /*!< callback to test if free */
    uint32_t element_size;          /*!< size of array of one element */
} vmiope_object_table_defn_t;

/**
 * Convert object table index to object reference.
 *
 * @param[in] table_p           Reference to object table block
 * @param[in] table_index       Index in the block
 * @param[in] element_size      Size of an element
 * @returns Reference to element
 */

static inline void *
vmiope_index_to_object(vmiope_object_table_ref_t table_p,
                       uint32_t table_index,
                       uint32_t element_size)
{
    return((void *) (((uint8_t *) table_p->element) + 
                     (table_index * element_size)));
}

/**
 * Convert handle to object reference
 *
 * @param[in] table_defn        Reference to table definition
 * @param[in] handle            ELement handle
 * @returns Reference to element, or NULL if out of range
 */

static void *
vmiope_handle_to_object(vmiope_object_table_defn_t *table_defn,
                        vmiop_handle_t handle)
{
    vmiope_object_table_t *table_p;
    void *object_p;

    if (table_defn == NULL || 
        handle == VMIOP_HANDLE_NULL) {
        return(NULL);
    }

    handle--;
    for (table_p = table_defn->head;
         table_p != NULL;
         table_p = table_p->next) {
        if (handle < table_p->base_handle) {
            return(NULL);
        }
        if (handle < 
            (table_p->base_handle + VMIOPE_OBJECT_TABLE_BLOCK)) {
            object_p = vmiope_index_to_object(table_p,
                                              (handle - table_p->base_handle),
                                              table_defn->element_size);
            if (object_p == NULL ||
                table_defn->is_free(object_p)) {
                return(NULL);
            }
            return(object_p);
        }
    }
    return(NULL);
}

#define VMIOPE_INVALID_INDEX ((uint32_t) (~0u))
/*!< symbol for invalid index from vmiope_object_to_index() */

/**
 * Convert object reference to index
 *
 * @param[in] table_p       Reference to table block
 * @param[in] object_p      Reference to object
 * @param[in] element_size  Size of element
 * @returns index in block, or VMIOPE_INVALID_INDEX if not in range.
 */

static inline uint32_t
vmiope_object_to_index(vmiope_object_table_ref_t table_p,
                       void *object_p,
                       uint32_t element_size)
{
    if ((((char *) object_p) >= 
         ((char *) table_p->element)) &&
        (((char *) object_p) < 
         (((char *) table_p->element) +
          (element_size * VMIOPE_OBJECT_TABLE_BLOCK)))) {
        return((((char *) object_p) - ((char *) table_p->element)) /
               element_size);
    }
    return(VMIOPE_INVALID_INDEX);
}

/**
 * Convert object reference to handle
 *
 * @param[in] table_defn        Reference to table definition
 * @param[in] object_p Reference to thread
 * @returns Handle, or VMIOP_HANDLE_NULL if arguments are NULL or
 *              out of range.
 */

static vmiop_handle_t 
vmiope_object_to_handle(vmiope_object_table_defn_t *table_defn,
                        void *object_p)
{
    vmiope_object_table_t *table_p;
    uint32_t element_index;

    if (object_p == NULL) {
        return(VMIOP_HANDLE_NULL);
    }

    for (table_p = table_defn->head;
         table_p != NULL;
         table_p = table_p->next) {
        element_index = vmiope_object_to_index(table_p,
                                               object_p,
                                               table_defn->element_size);
        if (element_index != VMIOPE_INVALID_INDEX) {
            return(table_p->base_handle +
                   element_index +
                   1);
        }
    }
    return(VMIOP_HANDLE_NULL);
}

/**
 * Allocate object from table and return handle and object reference.
 *
 * @param[in] table_defn            Reference to table definition.
 * @param[out] handle_p             Reference to variable to receive handle
 *                                  for assigned element.
 * @param[out] object_p             Reference to variable to receive pointer
 *                                  to assigned element.
 * @returns Error code:
 * -            vmiop_success       Normal completion
 * -            vmiop_error_inval   NULL argument
 * -            vmiop_error_resource Insufficient memory to allocate element.
 */

static vmiop_error_t
vmiope_allocate_table_element(vmiope_object_table_defn_t *table_defn,
                              vmiop_handle_t *handle_p,
                              void **object_p)
{
    vmiop_handle_t handle;
    vmiope_object_table_t *new_object_table;
    void *new_object;
    vmiope_object_table_t *table_p;

    if (table_defn == NULL ||
        handle_p == NULL) {
        if (object_p != NULL) {
            *object_p = NULL;
        }
        return(vmiop_error_inval);
    }

    for (table_p = table_defn->head;
         table_p != NULL;
         table_p = table_p->next) {
        for (handle = 0;
             handle < VMIOPE_OBJECT_TABLE_BLOCK;
             handle++) {
            new_object = vmiope_index_to_object(table_p,
                                                handle,
                                                table_defn->element_size);
            if (table_defn->is_free(new_object)) {
                handle += table_p->base_handle;
                goto alloc_entry;
            }
        }
    }
    new_object_table = (vmiope_object_table_t *) calloc((sizeof(vmiope_object_table_t) +
                                                         ((table_defn->element_size *
                                                           VMIOPE_OBJECT_TABLE_BLOCK) -
                                                          sizeof(vmiope_object_table_element_t))),
                                                        1);
    if (new_object_table == NULL) {
        *handle_p = VMIOP_HANDLE_NULL;
        if (object_p != NULL) {
            *object_p = NULL;
        }
        return(vmiop_error_resource);
    }

    new_object_table->base_handle = table_defn->table_limit;
    table_defn->table_limit += VMIOPE_OBJECT_TABLE_BLOCK;

    if (table_defn->head == NULL) {
        table_defn->head = new_object_table;
    } else {
        for (table_p = table_defn->head;
             ;
             table_p = table_p->next) {
            if (table_p->next == NULL) {
                table_p->next = new_object_table;
                break;
            }
        }
    }
    new_object = ((void *) new_object_table->element);
    handle = new_object_table->base_handle;
 alloc_entry:
    (void) memset((void *) new_object,
                  (int) 0,
                  (size_t) table_defn->element_size);
    handle++;
    *handle_p = handle;
    if (object_p != NULL) {
        *object_p = new_object;
    }
    return(vmiop_success);
}

/*
 * memory allocation
 */

/**
 * Type for page numbers.
 */

typedef vmiop_emul_addr_t vmiope_emul_page_t;

/**
 * Round up length 
 *
 * @param[in] value     Value to round up
 * @param[in] alignment Alignment to which value should be rounded up.
 *                      Must be a power of two.
 * @returns Rounded value
 */

static inline vmiop_emul_length_t
vmiope_round_up(vmiop_emul_length_t value,
                uint32_t alignment)
{
    return((value +
            (alignment - 1)) &
           ~((vmiop_emul_length_t) (alignment - 1)));
}

/**
 * Convert length to page count (rounded up).
 *
 * @param[in] data_length length in bytes
 * @returns count of pages 
 */

static inline vmiop_emul_length_t
vmiope_bytes_to_pages(vmiop_emul_length_t data_length)
{
    return((data_length + TARGET_PAGE_SIZE - 1) /
           TARGET_PAGE_SIZE);
}

/**
 * Allocate local memory
 *
 * @param[in] alloc_length  Length of memory required
 * @param[out] alloc_addr_p Reference to variable to receive
 *         address of allocated memory.  Initial value is 
 *         undefined.  Receives the address of the allocated
 *         memory on success, and NULL if the allocation fails.
 * @param[in] clear_memory   If true, allocated memory is set to
 *         all zero bytes.  If false, content of allocated memory
 *         is undefined.        
 * @returns Error code:
 * -            vmiop_success:          Successful allocation
 * -            vmiop_error_inval:      NULL alloc_addr_p
 * -            vmiop_error_resource:   Not enough memory
 */

vmiop_error_t
vmiop_memory_alloc_internal(const vmiop_emul_length_t alloc_length,
                            void **alloc_addr_p,
                            const vmiop_bool_t clear_memory)
{
    void *alloc_addr;

    if (alloc_addr_p == NULL) {
        return(vmiop_error_inval);
    }
    if (clear_memory) {
        alloc_addr = calloc((size_t) alloc_length,(size_t) 1);
    } else {
        alloc_addr = malloc((size_t) alloc_length);
    }
    *alloc_addr_p = alloc_addr;
    if (alloc_addr == NULL) {
        return(vmiop_error_resource);
    }
    return(vmiop_success);
}

/**
 * Free local memory
 *
 * @param[in] alloc_addr    Address to free
 * @param[in] alloc_length  Length of block to free
 * @returns Error code:
 * -            vmiop_success:          Successful free
 * -            vmiop_error_inval:      Not an allocated block
 */

vmiop_error_t
vmiop_memory_free_internal(void *alloc_addr,
                           const vmiop_emul_length_t alloc_length)
{
    if (alloc_addr == NULL) {
        return(vmiop_error_inval);
    }
    free(alloc_addr);
    return(vmiop_success);
}

/**
 * Get emulated system page size.
 *
 * @param[out] page_size_p Size of a page on the emulated system.
 * @returns Error code:
 * -            vmiop_success:          Successful allocation
 * -            vmiop_error_inval:      NULL page_size_p
 */

vmiop_error_t
vmiop_get_page_size(vmiop_emul_length_t *page_size_p)
{
    *page_size_p = (vmiop_emul_length_t) TARGET_PAGE_SIZE;
    return(vmiop_success);
}

/**
 * Get a unique identifier for this guest.
 *
 * @param[out] guest_id_p Unique ID of the guest
 * @returns Error code:
 * -            vmiop_success:          Successful
 * -            vmiop_error_inval:      NULL guest_id_p or unknown guest ID.
 */

vmiop_error_t
vmiop_get_guest_id(uint64_t *guest_id_p)
{
    if(guest_id_p == NULL)
        return vmiop_error_inval;
    
    *guest_id_p = demu_get_domid();

    return vmiop_success;
}

/**
 * Allocate paged memory
 *
 * @param[in] alloc_length  Length of memory required.
 *         Must be a multiple of the guest domain's page size.
 * @param[in,out] alloc_addr_p Reference to variable to receive
 *         address of allocated memory.
 *         Initial value is NULL if no address required, or
 *         a required address if needed.  Requested address
 *         must be a multiple of the guest domain's page size, 
 *         if an address is requested.   Receives the address 
 *         of the allocated memory, or NULL if the allocation fails.
 * @param[in] clear_memory   If true, allocated memory is set to
 *         all zero bytes.  If false, content of allocated memory
 *         is undefined.        
 * @returns Error code:
 * -            vmiop_success:          Successful allocation
 * -            vmiop_error_inval:      NULL alloc_addr_p
 * -            vmiop_error_range:      Address plus length overflow
 * -            vmiop_error_resource:   Not enough memory
 * -            vmiop_error_no_address_space: Not enough address 
 *                                      space to map memory
 */

vmiop_error_t
vmiop_paged_memory_alloc(const vmiop_emul_length_t alloc_length,
                         void **alloc_addr_p,
                         const vmiop_bool_t clear_memory)
{
    void *alloc_addr;

    if (alloc_addr_p == NULL ||
        alloc_length == 0 ||
        (alloc_length % TARGET_PAGE_SIZE) != 0) {
        return(vmiop_error_inval);
    }
    alloc_addr = *alloc_addr_p;
    if (alloc_addr != NULL &&
        (((uintptr_t) alloc_addr) % TARGET_PAGE_SIZE) != 0) {
        return(vmiop_error_inval);
    }
    alloc_addr = mmap(alloc_addr,
                      (size_t) alloc_length,
                      (int) (PROT_READ | PROT_WRITE | PROT_EXEC),
                      (int) (MAP_ANONYMOUS | 
                             ((alloc_addr != NULL) ? MAP_FIXED : 0)),
                      (int) (-1),
                      (off_t) 0);
    if (alloc_addr == MAP_FAILED) {
        *alloc_addr_p = NULL;
        if (errno == EAGAIN ||
            errno == EWOULDBLOCK ||
            errno == ENOMEM) {
            return(vmiop_error_resource);
        } else {
            return(vmiop_error_inval);
        }
    }
    *alloc_addr_p = alloc_addr;
    return(vmiop_success);
}

/**
 * Free paged memory
 *
 * @param[in] alloc_addr    Address to free
 * @param[in] alloc_length  Length of block to free
 * @returns Error code:
 * -            vmiop_success:          Successful free
 * -            vmiop_error_inval:      Not an allocated block,
 *                                      or alloc_addr or alloc_length
 *                                      not a multiple of guest domain's
 *                                      page size
 * -            vmiop_error_range:      Overflow of 
 *                                      alloc_addr+alloc_length
 */

vmiop_error_t
vmiop_paged_memory_free(void *alloc_addr,
                        const vmiop_emul_length_t alloc_length)
{
    if (alloc_addr == NULL ||
        (((uintptr_t) alloc_addr) % TARGET_PAGE_SIZE) != 0 ||
        alloc_length == 0 ||
        (alloc_length % TARGET_PAGE_SIZE) != 0) {
        return(vmiop_error_inval);
    }
    if (munmap(alloc_addr,
               (size_t) alloc_length) != 0) {
        return(vmiop_error_inval);
    }
    return(vmiop_success);
}


/*@}*/

/**********************************************************************/
/**
 * @defgroup MonitorImpl    qemu Monitor Implementation
 */
/**********************************************************************/
/*@{*/

static pthread_mutex_t vmiope_monitor_lock = PTHREAD_MUTEX_INITIALIZER;
/*!< lock for qemu monitor */

static vmiop_bool_t vmiope_monitor_held = vmiop_false;
/*!< set true when monitor held (except briefly when entering or leaving) */

static pthread_t vmiope_monitor_holder;
/*!< pthread ID of thread holding the monitor */

/**
 * Enter vmioplugin monitor for qemu.
 *
 n * When built with vmioplugin support, qemu code should call
 * this routine to reenter the monitor after a select() or
 * other indeterminate wait.   qemu code should call 
 * vmiope_leave_monitor() ahead of any such wait.
 * vmiope_initialize() automatically enters the monitor at
 * startup time.
 *
 * vmiop-env internally will leave the monitor around callbacks
 * to plugins and reenter the monitor on return from such callbacks,
 * and enter the monitor when call from plugins and leave it
 * before return, in cases where vmiop-env will be using qemu
 * facilities.
 *
 * @param[out] in_monitor   Reference to variable to receive 
 *                          vmiop_true if already in monitor
 *                          and vmiop_false otherwise.  Ignored
 *                          in reference is NULL.
 */

void
vmiope_enter_monitor(vmiop_bool_t *in_monitor)
{
    if (vmiope_monitor_held &&
        vmiope_monitor_holder == pthread_self()) {
        if (in_monitor != NULL) {
            *in_monitor = vmiop_true;
        }
    } else {
        pthread_mutex_lock(&vmiope_monitor_lock);
        vmiope_monitor_holder = pthread_self();
        vmiope_monitor_held = vmiop_true;
        if (in_monitor != NULL) {
            *in_monitor = vmiop_false;
        }
    }
}

/**
 * Leave vmioplugin monitor for qemu.
 *
 * @param[out] in_monitor   Reference to variable to receive 
 *                          vmiop_true if already in monitor
 *                          and vmiop_false otherwise.  Ignored
 *                          in reference is NULL.
 */

void
vmiope_leave_monitor(vmiop_bool_t *in_monitor)
{
    if (vmiope_monitor_held &&
        vmiope_monitor_holder == pthread_self()) {
        vmiope_monitor_held = vmiop_false;
        (void) pthread_mutex_unlock(&vmiope_monitor_lock);
        if (in_monitor != NULL) {
            *in_monitor = vmiop_true;
        }
    } else {
        if (in_monitor != NULL) {
            *in_monitor = vmiop_false;
        }
    }
}

/**
 * Leave monitor and acquire internal lock
 *
 * @param[out] in_monitor   Set to vmiop_true if was in monitor, and
 *                          to vmiop_false if not
 * @param[in,out] lock_p    Reference to lock to acquire
 * @returns Error code:
 * -            vmiop_success   Locks exchanged
 * -            vmiop_error_resource Locking failed
 */

static vmiop_error_t
vmiope_enter_lock(vmiop_bool_t *in_monitor,
                  pthread_mutex_t *lock_p)
{
    vmiope_leave_monitor(in_monitor);
    if (pthread_mutex_lock(lock_p) != 0) {
        if (*in_monitor) {
            vmiope_enter_monitor(NULL);
        }
        return(vmiop_error_resource);
    }
    return(vmiop_success);
}

/**
 * Release internal lock and enter monitor
 *
 * @param[in] in_monitor   vmiop_true if was in monitor, and
 *                         vmiop_false if not
 * @param[in,out] lock_p   Reference to lock to release
 */

static void
vmiope_leave_lock(vmiop_bool_t in_monitor,
                  pthread_mutex_t *lock_p)
{
    (void) pthread_mutex_unlock(lock_p);
    if (in_monitor) {
        vmiope_enter_monitor(NULL);
    }
}

/*@}*/

/**********************************************************************/
/**
 * @defgroup EmulationSupportImpl   Emulation Support 
 */
/**********************************************************************/
/*@{*/

/**
 * Pixel type to pixel width in bits
 */

uint8_t vmiope_pixel_width[vmiop_pf_max + 1] = 
    { [vmiop_pf_8] = 8,
      [vmiop_pf_15] = 15,
      [vmiop_pf_16] = 16,
      [vmiop_pf_32] = 32,
      [vmiop_pf_32_bgr] = 32
    };

/**
 * Pixel type to pixel depth in bytes
 */

uint8_t vmiope_pixel_depth[vmiop_pf_max + 1] = 
    { [vmiop_pf_8] = 1,
      [vmiop_pf_15] = 2,
      [vmiop_pf_16] = 2,
      [vmiop_pf_32] = 4,
      [vmiop_pf_32_bgr] = 4
    };

/**
 * Pixel type to pixel BGR mode
 */

uint8_t vmiope_pixel_bgr[vmiop_pf_max + 1] = 
    { [vmiop_pf_8] = 0,
      [vmiop_pf_15] = 0,
      [vmiop_pf_16] = 0,
      [vmiop_pf_32] = 0,
      [vmiop_pf_32_bgr] = 1
    };

/**
 * Pixel depth and BGR to pixel type
 *
 * @param[in] Pixel depth
 * @param[in] Pixel BGR
 * @returns Pixel type
 */

vmiop_pixel_format_t
vmiope_pixel_depth_bgr_to_type(uint8_t depth, uint8_t bgr)
{
    switch (depth) {
    case 8:
        return(vmiop_pf_8);
    case 15:
        return(vmiop_pf_15);
    case 16:
        return(vmiop_pf_16);
    default:
        return(bgr 
               ? vmiop_pf_32_bgr
               : vmiop_pf_32);
    }
}
/**
 * Reference to emulated device registration entry
 */

typedef struct vmiope_emul_device_s *vmiope_emul_device_ref_t;

/**
 * Emulated device registration entry 
 */

typedef struct vmiope_emul_device_s {
    vmiop_list_header_t list_head;
    /*!< list pointers */
    void *private_object;
    /*!< private object reference for callback function */
    vmiop_emul_callback_t callback;
    /*!< reference to emulation callback routine */
    const char *object_label;
    /*!< logical label for emulated object */
    vmiop_handle_t handle;
    /*!< handle for registration */
} vmiope_emul_device_t;
    
/**
 * Emulated device registration list
 */

static vmiop_list_header_ref_t emul_device_list = NULL;

/**
 * Free list of emulated device mmio registration objects.
 */

static vmiop_list_header_ref_t free_emul_device_list = NULL;

/**
 * Parameter block for list function for emulated
 * device list.
 */

typedef struct vmiope_emul_device_apply_s {
    vmiop_handle_t handle;
    /*!< handle to locate */
    vmiope_emul_device_ref_t emul_device;
    /*!< receives reference to device object found */
} vmiope_emul_device_apply_t;

/**
 * Check if device entry matches handle in list apply.
 *
 * @param[in] private_object Reference to 
 *                      vmiope_emul_device_apply_t object.
 *                      Reference to list member stored
 *                      in emul_device on a match.
 * @param[in] link_p    Reference to list header in list
 *                      member
 * @returns vmiop_bool_t:
 *              vmiop_true: Terminate loop (item found)
 *              vmiop_false:  Continue loop (item not found)
 */

static vmiop_bool_t
vmiope_emul_device_match(void *private_object,
                         vmiop_list_header_ref_t link_p)
{
    if (((vmiope_emul_device_apply_t *) private_object)->handle ==
        ((vmiope_emul_device_ref_t) link_p)->handle) {
        ((vmiope_emul_device_apply_t *) private_object)->emul_device =
            ((vmiope_emul_device_ref_t) link_p);
        return(vmiop_true);
    }
    return(vmiop_false);
}

/**
 * Locate emulated device registration by handle.
 *
 * @param[in] handle            Handle to locate.
 * @param[out] emul_device_p    Reference to variable to
 *      receive reference to device registration.
 * @returns Error code:
 * -            vmiop_success:          Successful unregistration
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL or
 *                                      emul_device_p is NULL
 * -            vmiop_error_not_found:  Handle does not refer to a
 *                                      registration.
 */

static vmiop_error_t
vmiope_locate_emul_device(vmiop_handle_t handle,
                          vmiope_emul_device_ref_t *emul_device_p)
{
    vmiope_emul_device_apply_t appl;

    if (emul_device_p == NULL) {
        return(vmiop_error_inval);
    }
    if (handle == VMIOP_HANDLE_NULL) {
        *emul_device_p = NULL;
        return(vmiop_error_inval);
    }
    appl.handle = handle;
    appl.emul_device = NULL;
    if (vmiope_list_apply(&emul_device_list,
                          vmiope_emul_device_match,
                          (void *) &appl)) {
        *emul_device_p = appl.emul_device;
        return(vmiop_success);
    }
    *emul_device_p = NULL;
    return(vmiop_error_not_found);
}

/**
 * Configuration space read callback routine.
 *
 * @param[in] emd       Reference to emulated device registration
 *                      entry
 * @param[in] address   Address to read
 * @param[in] len       Length of data to read
 * @returns uint32_t    Data read (or 0xffffffff if out of bounds)
 */

static uint32_t
vmiope_config_read(vmiope_emul_device_t *emd,
                   uint32_t address,
                   int len)
{
    vmiop_error_t error_code;
    vmiop_emul_state_t cacheable = vmiop_emul_noncacheable;
    uint32_t data_value_l;
    uint16_t data_value_w;
    uint8_t data_value_b;
    vmiop_bool_t in_monitor;

    if (emd == NULL ||
        address >= 256) {
        return(0xfffffffful);
    }
    vmiope_leave_monitor(&in_monitor);
    switch (len) {
    case 1:
        error_code = emd->callback(emd->private_object,
                                   vmiop_emul_op_read,
                                   vmiop_emul_space_config,
                                   address,
                                   sizeof(data_value_b),
                                   (void *) &data_value_b,
                                   &cacheable);
        data_value_l = data_value_b;
        break;
    
    case 2:
        error_code = emd->callback(emd->private_object,
                                   vmiop_emul_op_read,
                                   vmiop_emul_space_config,
                                   address,
                                   sizeof(data_value_w),
                                   (void *) &data_value_w,
                                   &cacheable);
        data_value_l = data_value_w;
        break;

    case 4:
        error_code = emd->callback(emd->private_object,
                                   vmiop_emul_op_read,
                                   vmiop_emul_space_config,
                                   address,
                                   sizeof(data_value_l),
                                   (void *) &data_value_l,
                                   &cacheable);
        break;
    default:
        error_code = vmiop_error_inval;
        break;
    }

    if (in_monitor) {
        vmiope_enter_monitor(NULL);
    }

    if (error_code != vmiop_success) {
        return(0xfffffffful);
    }

    return(data_value_l);
}

static uint8_t
vmiope_config_readb(void *opaque, uint64_t address)
{ 
    vmiope_emul_device_t *emd = opaque;
    uint8_t val;

    val = (uint8_t)vmiope_config_read(emd, (uint32_t)address, 1); 

    return val;
}

static uint16_t
vmiope_config_readw(void *opaque, uint64_t address)
{ 
    vmiope_emul_device_t *emd = opaque;
    uint16_t val;

    val = (uint16_t)vmiope_config_read(emd, (uint32_t)address, 2); 

    return val;
}

static uint32_t
vmiope_config_readl(void *opaque, uint64_t address)
{ 
    vmiope_emul_device_t *emd = opaque;
    uint32_t val;

    val = (uint32_t)vmiope_config_read(emd, (uint32_t)address, 4); 

    return val;
}

/**
 * Configuration space write callback routine.
 *
 * @param[in] emd       Reference to emulated device registration
 *                      entry
 * @param[in] address   Address to write
 * @param[in] val       Value to store
 * @param[in] len       Length of data to write
 */

static void
vmiope_config_write(vmiope_emul_device_t *emd,
                    uint32_t address,
                    uint32_t val,
                    int len)
{
    vmiop_emul_state_t cacheable = vmiop_emul_noncacheable;
    uint16_t data_value_w;
    uint8_t data_value_b;
    vmiop_bool_t in_monitor;

    if (emd == NULL ||
        address >= 256) {
        return;
    }

    vmiope_leave_monitor(&in_monitor);
    switch (len) {
    case 1:
        data_value_b = val;
        (void) emd->callback(emd->private_object,
                             vmiop_emul_op_write,
                             vmiop_emul_space_config,
                             address,
                             sizeof(data_value_b),
                             (void *) &data_value_b,
                             &cacheable);
        break;
    
    case 2:
        data_value_w = val;
        (void) emd->callback(emd->private_object,
                             vmiop_emul_op_write,
                             vmiop_emul_space_config,
                             address,
                             sizeof(data_value_w),
                             (void *) &data_value_w,
                             &cacheable);
        break;

    case 4:
        (void) emd->callback(emd->private_object,
                             vmiop_emul_op_write,
                             vmiop_emul_space_config,
                             address,
                             sizeof(val),
                             (void *) &val,
                             &cacheable);
        break;
    default:
        break;
    }

    if (in_monitor) {
        vmiope_enter_monitor(NULL);
    }
}

static void
vmiope_config_writeb(void *opaque, uint64_t address, uint8_t val)
{ 
    vmiope_emul_device_t *emd = opaque;

    vmiope_config_write(emd, (uint32_t)address, (uint32_t)val, 1); 
}

static void
vmiope_config_writew(void *opaque, uint64_t address, uint16_t val)
{ 
    vmiope_emul_device_t *emd = opaque;

    vmiope_config_write(emd, (uint32_t)address, (uint32_t)val, 2); 
}

static void
vmiope_config_writel(void *opaque, uint64_t address, uint32_t val)
{ 
    vmiope_emul_device_t *emd = opaque;

    vmiope_config_write(emd, (uint32_t)address, (uint32_t)val, 4); 
}

static io_ops_t vmiope_config_ops = {
    .readb = vmiope_config_readb,
    .readw = vmiope_config_readw,
    .readl = vmiope_config_readl,
    .writeb = vmiope_config_writeb,
    .writew = vmiope_config_writew,
    .writel = vmiope_config_writel
};

/**
 * PCI Configuration space emulation. Virtual devices can register 
 * a function to be called when their PCI configuration registers 
 * are accessed by the Virtual machine. 
 *
 * If a registration is done before the virtual machine starts up, as
 * part of plugin initialization, the device will appear in the initial
 * configuration of the machine when the operating system starts.  Later
 * registrations and unregistrations will appear as PCI hotplug events,
 * This implies that IO and MMIO address ranges should be registered before 
 * the configuration space is registered, and that the latter should be 
 * unregistered first.
 *
 * @param[in] private_object Pointer private to the caller, which will be
 *        passed to the callback routine on any call.
 * @param[in] emul_callback Pointer to a callback routine, which will be
 *        called on any read or write to the PCI configuration registers.
 * @param[in] object_label Pointer to text string, representing a
 *        label for the registration instance, or NULL, if none.  May
 *        be used to select an optional configured PCI configuration
 *        address from a configuration database.  If not supplied, or
 *        no match, environment to select an unused address of its
 *        choice.
 * @param[in] handle_p Reference to variable to receive a handle, private to the
 *        environment, for the registration, to be supplied when
 *        removing the registration.  Content of referenced variable
 *        is undefined on entry, and will be set to NULL on any error.
 * @returns Error code: 
 * -            vmiop_success:          Successful registration
 * -            vmiop_error_inval:      NULL range_base_p or handle_p
 * -            vmiop_error_resource:   No memory or other resource unavailable
 */

static inline vmiop_error_t
_vmiop_register_emul_device(void *private_object,
                            const vmiop_emul_callback_t emul_callback,
                            const char *object_label,
                            vmiop_handle_t *handle_p)
{
    vmiope_emul_device_ref_t emul_device = NULL;
    vmiop_handle_t handle = VMIOP_HANDLE_NULL;
    vmiop_error_t error_code;

    /* check parameters */
    if (emul_callback == NULL ||
        object_label == NULL ||
        handle_p == NULL) {
        return(vmiop_error_inval);
    }

    emul_device = ((vmiope_emul_device_ref_t) free_emul_device_list);
    if (emul_device != NULL) {
        (void) vmiope_list_remove(&free_emul_device_list,
                                  &(emul_device->list_head));
    } else {
        error_code = vmiop_memory_alloc_internal((vmiop_emul_length_t) sizeof(vmiope_emul_device_t),
                                        (void **) &emul_device,
                                        vmiop_true);
        if (error_code != vmiop_success) {
            goto error_exit;
        }
    }
    error_code = vmiope_handle_alloc(&handle);
    if (error_code != vmiop_success) {
        goto error_exit;
    }
    emul_device->private_object = private_object;
    emul_device->callback = emul_callback;
    emul_device->object_label = object_label;
    emul_device->handle = handle;

    if (demu_register_pci_config_space(&vmiope_config_ops, emul_device) < 0)
        goto error_exit;

    vmiope_list_insert(&emul_device_list,
                       &(emul_device->list_head));
    
    *handle_p = handle;
    return(vmiop_success);

 error_exit:
    if (emul_device != NULL) {
        vmiope_list_insert(&free_emul_device_list,
                           &(emul_device->list_head));
        emul_device = NULL;
    }
    if (handle != VMIOP_HANDLE_NULL) {
        (void) vmiope_handle_free(handle);
        handle = VMIOP_HANDLE_NULL;
    }
    *handle_p = handle;
    return(error_code);
}

/**
 * PCI Configuration space emulation. Virtual devices can register 
 * a function to be called when their PCI configuration registers 
 * are accessed by the Virtual machine. 
 *
 * If a registration is done before the virtual machine starts up, as
 * part of plugin initialization, the device will appear in the initial
 * configuration of the machine when the operating system starts.  Later
 * registrations and unregistrations will appear as PCI hotplug events,
 * This implies that IO and MMIO address ranges should be registered before 
 * the configuration space is registered, and that the latter should be 
 * unregistered first.
 *
 * @param[in] private_object Pointer private to the caller, which will be
 *        passed to the callback routine on any call.
 * @param[in] emul_callback Pointer to a callback routine, which will be
 *        called on any read or write to the PCI configuration registers.
 * @param[in] object_label Pointer to text string, representing a
 *        label for the registration instance, or NULL, if none.  May
 *        be used to select an optional configured PCI configuration
 *        address from a configuration database.  If not supplied, or
 *        no match, environment to select an unused address of its
 *        choice.
 * @param[in] handle_p Reference to variable to receive a handle, private to the
 *        environment, for the registration, to be supplied when
 *        removing the registration.  Content of referenced variable
 *        is undefined on entry, and will be set to NULL on any error.
 * @returns Error code: 
 * -            vmiop_success:          Successful registration
 * -            vmiop_error_inval:      NULL range_base_p or handle_p
 * -            vmiop_error_resource:   No memory or other resource unavailable
 */

vmiop_error_t
vmiop_register_emul_device(void *private_object,
                           const vmiop_emul_callback_t emul_callback,
                           const char *object_label,
                           vmiop_handle_t *handle_p)
{
    vmiop_error_t error_code;
    vmiop_bool_t in_monitor;

    vmiope_enter_monitor(&in_monitor);
    error_code = _vmiop_register_emul_device(private_object,
                                             emul_callback,
                                             object_label,
                                             handle_p);
    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }
    return(error_code);
}

/**
 * Remove a registration previously made.   This will trigger a hotplug event
 * in the virtual machine, if the virtual machine is running and the space
 * is vmiop_emul_space_config.
 *
 * @param[in] handle Handle of registration to remove (as returned on registration).
 * @return Error code:
 * -            vmiop_success:          Successful unregistration
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found:  Handle does not refer to a
 *                                      registration.
 */

static inline vmiop_error_t
_vmiop_unregister_emul_device(const vmiop_handle_t handle)
{
    vmiope_emul_device_ref_t emul_device;
    vmiop_error_t error_code;
    
    error_code = vmiope_locate_emul_device(handle,&emul_device);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    demu_deregister_pci_config_space();
    
    /* remove from device list */
    vmiope_list_remove(&emul_device_list,
                       &(emul_device->list_head));

    /* release object */
    (void) vmiope_handle_free(emul_device->handle);
    vmiope_list_insert(&free_emul_device_list,
                       &(emul_device->list_head));

    return(vmiop_success);
}

/**
 * Remove a registration previously made.   This will trigger a hotplug event
 * in the virtual machine, if the virtual machine is running and the space
 * is vmiop_emul_space_config.
 *
 * @param[in] handle Handle of registration to remove (as returned on registration).
 * @return Error code:
 * -            vmiop_success:          Successful unregistration
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found:  Handle does not refer to a
 *                                      registration.
 */

vmiop_error_t
vmiop_unregister_emul_device(const vmiop_handle_t handle)
{
    vmiop_error_t error_code;
    vmiop_bool_t in_monitor;

    vmiope_enter_monitor(&in_monitor);
    error_code = _vmiop_unregister_emul_device(handle);
    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }
    return(error_code);
}

/*
 * Access to guest virtual machine address space
 */

/**
 * Mapping table entry
 */

typedef struct vmiope_mapping_s {
    void *local_address;    /*!< address in this process */
    vmiop_emul_addr_t range_base; /*!< address in guest, or
                                   * VMIOP_EMUL_ADDR_NONE if not 
                                   * contiguous */
    vmiop_emul_length_t range_length; /*!< length of range */
    vmiop_bool_t    read_only;  /*!< true if read-only */
    vmiop_bool_t    is_local;   
    /*!< true if dynamic mapping into this process, false if into guest */
} vmiope_mapping_t;

typedef vmiope_mapping_t vmiope_mapping_array_t[1];
/*!< mapping entry array of one element for sizing */

/**
 * Test if mapping entry is free
 *
 * @param[in] object_p      Pointer to entry (as void *)
 * @returns vmiop_bool_t    True if free, false if not or NULL
 */

static vmiop_bool_t
vmiope_mapping_is_free(void *object_p)
{
    return(object_p != NULL &&
           ((vmiope_mapping_t *) object_p)->range_length == 0);
}
        
/**
 * Mapping table.
 *
 * This table records all dynamic mappings by vmiop-env between the
 * guest and qemu-dm.
 */

static vmiope_object_table_defn_t mapping_table = {
    .head = NULL,
    .table_limit = 0,
    .is_free = vmiope_mapping_is_free,
    .element_size = sizeof(vmiope_mapping_array_t)
};

static pthread_mutex_t mapping_lock = PTHREAD_MUTEX_INITIALIZER;
/*!< lock for mapping management */

/**
 * Convert handle to mapping reference
 *
 * @param[in] handle    Thread handle
 * @returns Reference to mapping, or NULL if out of range
 */

static inline vmiope_mapping_t *
vmiope_handle_to_mapping(vmiop_handle_t handle)
{
    return((vmiope_mapping_t *) vmiope_handle_to_object(&mapping_table,
                                                        handle));
}

/**
 * Map a section of the guest address space into an address range visible
 * to the plugin.   IF any portion of the specified guest address
 * range is not mapped in the guest, the corresponding portion of
 * the local address range will also not be mapped, but the mapping
 * request will still succeed.  Note that subsequent changes to
 * the guest address space mapping will not affect this mapping.
 *
 * @param[in] range_base    Address in guest domain to map
 * @param[in] range_length  Length of address range to map
 * @param[in,out] local_address_p Pointer to variable to receive 
 *         address of mapping visible to the caller.
 *         Variable should be NULL or a suggested address on
 *         entry.  Variable will be set to NULL on any error and
 *         to the selected address on return.
 * @param[in] map_read_only False if map read/write, true if read-only
 * @param[out] handle_p     Pointer to variable to receive handle 
 *         for mapping. Initial value is undefined.
 *         Variable will be set to VMIOP_HANDLE_NULL on error.
 * @returns  Error code:
 * -            vmiop_success:          Successful mapping
 * -            vmiop_error_inval:      NULL local_address_p or
 *                                      handle_p or zero range_length
 * -            vmiop_error_no_address_space: Not enough local address
 *                                      space
 */

vmiop_error_t
vmiop_map_guest_memory(const vmiop_emul_addr_t range_base,
                       const vmiop_emul_length_t range_length,
                       void **local_address_p,
                       const vmiop_bool_t map_read_only,
                       vmiop_handle_t *handle_p)
{
    vmiope_mapping_t *mapping;
    vmiop_error_t error_code;
    vmiop_bool_t in_monitor;

    if (local_address_p == NULL ||
        range_length == 0) {
        return(vmiop_error_inval);
    }

    error_code = vmiope_enter_lock(&in_monitor,
                                   &mapping_lock);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    error_code = vmiope_allocate_table_element(&mapping_table,
                                               handle_p,
                                               (void **) &mapping);
    if (error_code != vmiop_success) {
        vmiope_leave_lock(in_monitor,
                          &mapping_lock);
        return(error_code);
    }

    mapping->local_address = NULL;
    mapping->range_length = range_length;
    mapping->range_base = range_base;
    mapping->read_only = map_read_only;
    mapping->is_local = vmiop_true;

    vmiope_leave_lock(vmiop_true,
                      &mapping_lock);

    mapping->local_address = 
        demu_map_guest_range(range_base,
                             range_length,
                             map_read_only,
                             0);
    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }
    if (mapping->local_address == MAP_FAILED) {
        mapping->local_address = NULL;
        mapping->range_length = 0;
        return(vmiop_error_inval);
    }

    *local_address_p = mapping->local_address;

    return(vmiop_success);
}


/**
 * Map a list of guest physical page numbers into a contiguous
 * address range visible to the plugin.   IF any portion of the 
 * specified guest address range is not mapped in the guest, 
 * the mapping request will fail.  Note that subsequent changes to
 * the guest address space mapping will not affect this mapping.
 *
 * @param[in] pfn_list      Reference to array of uint32_t elements
 *                          each containing a guest physical page number
 * @param[in] pfn_count     Count of elements in page_list
 * @param[in,out] local_address_p Pointer to variable to receive 
 *         address of mapping visible to the caller.
 *         Variable should be NULL or a suggested address on
 *         entry.  Variable will be set to NULL on any error and
 *         to the selected address on return.
 * @param[in] map_read_only False if map read/write, true if read-only
 * @param[out] handle_p     Pointer to variable to receive handle 
 *         for mapping. Initial value is undefined.
 *         Variable will be set to VMIOP_HANDLE_NULL on error.
 * @returns  Error code:
 * -            vmiop_success:          Successful mapping
 * -            vmiop_error_inval:      NULL local_address_p or
 *                                      handle_p or zero range_length
 * -            vmiop_error_no_address_space: Not enough local address
 *                                      space
 */

vmiop_error_t
vmiop_map_guest_memory_pages(unsigned long *pfn_list,
                             uint32_t pfn_count,
                             void **local_address_p,
                             const vmiop_bool_t map_read_only,
                             vmiop_handle_t *handle_p)
{
    vmiope_mapping_t *mapping;
    vmiop_error_t error_code;
    vmiop_bool_t in_monitor;
    xen_pfn_t *pfn_arr;

    if (local_address_p == NULL ||
        pfn_count == 0) {
        return(vmiop_error_inval);
    }

    error_code = vmiope_enter_lock(&in_monitor,
                                   &mapping_lock);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    error_code = vmiope_allocate_table_element(&mapping_table,
                                               handle_p,
                                               (void **) &mapping);
    if (error_code != vmiop_success) {
        vmiope_leave_lock(in_monitor,
                          &mapping_lock);
        return(error_code);
    }

    mapping->range_length = pfn_count * TARGET_PAGE_SIZE;
    mapping->range_base = VMIOP_EMUL_ADDR_NONE;
    mapping->read_only = map_read_only;
    mapping->is_local = vmiop_true;
    mapping->local_address = MAP_FAILED;

    vmiope_leave_lock(vmiop_true,
                      &mapping_lock);

    pfn_arr = malloc(sizeof(xen_pfn_t) * pfn_count);
    if (pfn_arr == NULL) {
        vmiope_leave_lock(in_monitor,
                          &mapping_lock);
        return(vmiop_error_inval);
    }

    /* copy list of guest PFNs */
    if (sizeof(xen_pfn_t) == sizeof(uint32_t)) {
        memcpy(pfn_arr, pfn_list, pfn_count * sizeof(uint32_t));
    } else {
        uint32_t i;
        for (i = 0; i < pfn_count; i++) {
            pfn_arr[i] = pfn_list[i];
        }
    }

    mapping->local_address = demu_map_guest_pages(pfn_arr,
                                                  pfn_count,
                                                  map_read_only,
                                                  0);
    free(pfn_arr);
    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }

    if (mapping->local_address == NULL) {
        mapping->local_address = MAP_FAILED;
        mapping->range_length = 0;
        return(vmiop_error_inval);
    }
    *local_address_p = mapping->local_address;
    return(vmiop_success);
}


/**
 * Unmap the prior mapping defined by the handle.
 *
 * @param[in] handle        Mapping to unmap
 * @returns Error code:
 * -            vmiop_success:          Successful unmapping
 * -            vmiop_error_not_found:  Not a guest mapping
 */

vmiop_error_t
vmiop_unmap_guest_memory(const vmiop_handle_t handle)
{
    vmiop_error_t error_code;
    vmiope_mapping_t *mapping;
    vmiop_bool_t in_monitor;

    error_code = vmiope_enter_lock(&in_monitor,
                                   &mapping_lock);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    mapping = vmiope_handle_to_mapping(handle);
    if (mapping == NULL ||
        ! mapping->is_local ||
        mapping->range_length == 0 ||
        mapping->local_address == 0) {
        error_code = vmiop_error_not_found;
    } else {
        (void) munmap(mapping->local_address,
                      mapping->range_length);

        mapping->local_address = NULL;
        mapping->range_length = 0;
    }
                  
    vmiope_leave_lock(in_monitor,
                      &mapping_lock);

    return(error_code);
}


/*
 * Modification of guest address space
 */

/**
 * Region table entry
 */

typedef struct vmiope_region_s {
    vmiop_emul_space_t address_space; /*!< identifies whether the region is MMIO or IO */
    vmiop_emul_addr_t range_base; /*!< address in guest, or 
                                   * VMIOP_EMUL_ADDR_NONE if not enabled */
    vmiop_emul_length_t range_length; /*!< length of range in bytes */
    vmiop_emul_callback_t callback; /*!< reference to emulation callback routine */
    void *private_object; /*!< private object reference for callback function */
    vmiop_list_header_ref_t mapping_list; /*!< mappings associated with region */
} vmiope_region_t;

typedef vmiope_region_t vmiope_region_array_t[1];
/*!< mapping entry array of one element for sizing */

/**
 * Check if a region entry in the region table is free
 *
 * @param[in] object_p  Table entry reference.
 * @returns vmiop_bool_t:
 *          vmiop_true  Table slot is free
 *          vmiop_false Table slot is occupied.
 */
static vmiop_bool_t
vmiope_region_is_free(void *object_p)
{
    return(object_p == NULL);          
}

/**
 * Region table.
 *
 * This table records all memory regions created for the guest.
 */

static vmiope_object_table_defn_t region_table = {
    .head = NULL,
    .table_limit = 0,
    .is_free = vmiope_region_is_free,
    .element_size = sizeof(vmiope_region_array_t)
};

static pthread_mutex_t region_lock = PTHREAD_MUTEX_INITIALIZER;
/*!< lock used to protect access to region table  */
/**
 * Convert handle to region reference
 *
 * @param[in] handle    region handle
 * @returns Reference to mapping, or NULL if out of range
 */

static inline vmiope_region_t *
vmiope_handle_to_region(vmiop_handle_t handle)
{
    return((vmiope_region_t *) vmiope_handle_to_object(&region_table,
                                                       handle));
}

/* 
 * ioport emulation routines
 */

/**
 * Read ioport byte
 *
 * @param[in] opaque        Reference to vmiope_emul_device_t object
 * @param[in] addr          Address of byte in target memory.
 * @returns value read
 */

static uint8_t 
vmiope_ioport_readb(void *opaque, 
                    uint64_t addr)
{
    vmiope_region_t *region = ((vmiope_region_t *) opaque);
    uint8_t data_value;
    vmiop_emul_state_t cacheable;
    vmiop_bool_t in_monitor;

    vmiope_leave_monitor(&in_monitor);
    (void) region->callback(region->private_object,
                            vmiop_emul_op_read,
                            vmiop_emul_space_io,
                            addr,
                            sizeof(data_value),
                            (void *) &data_value,
                            &cacheable);
    if (in_monitor) {
        vmiope_enter_monitor(NULL);
    }
    return(data_value);
}

/**
 * Read ioport short
 *
 * @param[in] opaque        Reference to vmiope_emul_device_t object
 * @param[in] addr          Address of short in target memory.
 * @returns value read
 */

static uint16_t 
vmiope_ioport_readw(void *opaque, 
                    uint64_t addr)
{
    vmiope_region_t *region = ((vmiope_region_t *) opaque);
    uint16_t data_value;
    vmiop_emul_state_t cacheable = vmiop_emul_noncacheable;
    vmiop_bool_t in_monitor;

    vmiope_leave_monitor(&in_monitor);
    (void) region->callback(region->private_object,
                            vmiop_emul_op_read,
                            vmiop_emul_space_io,
                            addr,
                            sizeof(data_value),
                            (void *) &data_value,
                            &cacheable);
    if (in_monitor) {
        vmiope_enter_monitor(NULL);
    }
#ifdef TARGET_WORDS_BIGENDIAN
    data_value = htons(data_value);
#endif
    return(data_value);
}

/**
 * Read ioport word
 *
 * @param[in] opaque        Reference to vmiope_region_t object
 * @param[in] addr          Address of word in target memory.
 * @returns value read
 */

static uint32_t 
vmiope_ioport_readl(void *opaque,
                    uint64_t addr)
{
    vmiope_region_t *region = ((vmiope_region_t *) opaque);
    uint32_t data_value;
    vmiop_emul_state_t cacheable = vmiop_emul_noncacheable;
    vmiop_bool_t in_monitor;

    vmiope_leave_monitor(&in_monitor);
    (void) region->callback(region->private_object,
                            vmiop_emul_op_read,
                            vmiop_emul_space_io,
                            addr,
                            sizeof(data_value),
                            (void *) &data_value,
                            &cacheable);
    if (in_monitor) {
        vmiope_enter_monitor(NULL);
    }
#ifdef TARGET_WORDS_BIGENDIAN
    data_value = htonl(data_value);
#endif
    return(data_value);
}

/**
 * Write ioport byte
 *
 * @param[in] opaque        Reference to vmiope_region_t object
 * @param[in] addr          Address of byte in target memory.
 * @param[in] mem_value     Data to write
 */

static void
vmiope_ioport_writeb(void *opaque, 
                     uint64_t addr, 
                     uint8_t mem_value)
{
    vmiope_region_t *region = ((vmiope_region_t *) opaque);
    uint8_t data_value;
    vmiop_emul_state_t cacheable = vmiop_emul_noncacheable;
    vmiop_bool_t in_monitor;

    data_value = mem_value;
    vmiope_leave_monitor(&in_monitor);
    (void) region->callback(region->private_object,
                            vmiop_emul_op_write,
                            vmiop_emul_space_io,
                            addr,
                            sizeof(data_value),
                            (void *) &data_value,
                            &cacheable);
    if (in_monitor) {
        vmiope_enter_monitor(NULL);
    }
}

/**
 * Write ioport short
 *
 * @param[in] opaque        Reference to vmiope_region_t object
 * @param[in] addr          Address of short in target memory.
 * @param[in] mem_value     Data to write
 */

static void 
vmiope_ioport_writew(void *opaque, 
                     uint64_t addr, 
                     uint16_t mem_value)
{
    vmiope_region_t *region = ((vmiope_region_t *) opaque);
    uint16_t data_value;
    vmiop_emul_state_t cacheable = vmiop_emul_noncacheable;
    vmiop_bool_t in_monitor;

#ifdef TARGET_WORDS_BIGENDIAN
    data_value = ntohs(mem_value);
#else
    data_value = mem_value;
#endif
    vmiope_leave_monitor(&in_monitor);
    (void) region->callback(region->private_object,
                            vmiop_emul_op_write,
                            vmiop_emul_space_io,
                            addr,
                            sizeof(data_value),
                            (void *) &data_value,
                            &cacheable);
    if (in_monitor) {
        vmiope_enter_monitor(NULL);
    }
}

/**
 * Write ioport byte
 *
 * @param[in] opaque        Reference to vmiope_region_t object
 * @param[in] addr          Address of word in target memory.
 * @param[in] mem_value     Data to write
 */

static void 
vmiope_ioport_writel(void *opaque, 
                     uint64_t addr, 
                     uint32_t mem_value)
{
    vmiope_region_t *region = ((vmiope_region_t *) opaque);
    uint32_t data_value;
    vmiop_emul_state_t cacheable = vmiop_emul_noncacheable;
    vmiop_bool_t in_monitor;

#ifdef TARGET_WORDS_BIGENDIAN
    data_value = ntohl(mem_value);
#else
    data_value = mem_value;
#endif
    vmiope_leave_monitor(&in_monitor);
    (void) region->callback(region->private_object,
                            vmiop_emul_op_write,
                            vmiop_emul_space_io,
                            addr,
                            sizeof(data_value),
                            (void *) &data_value,
                            &cacheable);
    if (in_monitor) {
        vmiope_enter_monitor(NULL);
    }
}

static io_ops_t vmiope_ioport_ops = {
    .readb = vmiope_ioport_readb,
    .readw = vmiope_ioport_readw,
    .readl = vmiope_ioport_readl,
    .writeb = vmiope_ioport_writeb,
    .writew = vmiope_ioport_writew,
    .writel = vmiope_ioport_writel
};


/* 
 * MMIO emulation routines
 */

/**
 * Read MMIO byte
 *
 * @param[in] opaque        Reference to vmiope_region_t object
 * @param[in] addr          Address of byte in target memory.
 * @returns value read
 */

static uint8_t 
vmiope_mmio_readb(void *opaque, 
                  uint64_t addr)
{
    vmiope_region_t *region = ((vmiope_region_t *) opaque);
    uint8_t data_value;
    vmiop_emul_state_t cacheable;
    vmiop_bool_t in_monitor;

    vmiope_leave_monitor(&in_monitor);
    if (region->callback) {
        (void) region->callback(region->private_object,
                                vmiop_emul_op_read,
                                vmiop_emul_space_mmio,
                                addr,
                                sizeof(data_value),
                                (void *) &data_value,
                                &cacheable);
    }
    else {
        vmiop_log(vmiop_log_error,
                  "%s: vmiop-env:map-only region access!"
                  "OP: read, addr: 0x%x"
                  "Region{address_space=0x%x, range_base=0x%lx, range_length=0x%lx}",
                  __FUNCTION__, addr,
                  region->address_space, region->range_base, region->range_length);

        data_value = -1;
    }
    if (in_monitor) {
        vmiope_enter_monitor(NULL);
    }
    return(data_value);
}

/**
 * Read MMIO short
 *
 * @param[in] opaque        Reference to vmiope_region_t object
 * @param[in] addr          Address of short in target memory.
 * @returns value read
 */

static uint16_t 
vmiope_mmio_readw(void *opaque, 
                  uint64_t addr)
{
    vmiope_region_t *region = ((vmiope_region_t *) opaque);
    uint16_t data_value;
    vmiop_emul_state_t cacheable = vmiop_emul_noncacheable;
    vmiop_bool_t in_monitor;

    vmiope_leave_monitor(&in_monitor);
    if (region->callback) {
        (void) region->callback(region->private_object,
                                vmiop_emul_op_read,
                                vmiop_emul_space_mmio,
                                addr,
                                sizeof(data_value),
                                (void *) &data_value,
                                &cacheable);
    }
    else {
        vmiop_log(vmiop_log_error,
                  "%s: vmiop-env:map-only region access!"
                  "OP: read, addr: 0x%x"
                  "Region{address_space=0x%x, range_base=0x%lx, range_length=0x%lx}",
                  __FUNCTION__, addr,
                  region->address_space, region->range_base, region->range_length);

        data_value = -1;
    }
    if (in_monitor) {
        vmiope_enter_monitor(NULL);
    }
#ifdef TARGET_WORDS_BIGENDIAN
    data_value = htons(data_value);
#endif
    return(data_value);
}

/**
 * Read MMIO word
 *
 * @param[in] opaque        Reference to vmiope_region_t object
 * @param[in] addr          Address of word in target memory.
 * @returns value read
 */

static uint32_t 
vmiope_mmio_readl(void *opaque,
                  uint64_t addr)
{
    vmiope_region_t *region = ((vmiope_region_t *) opaque);
    uint32_t data_value;
    vmiop_emul_state_t cacheable = vmiop_emul_noncacheable;
    vmiop_bool_t in_monitor;
    
    vmiope_leave_monitor(&in_monitor);
    if (region->callback) {
        (void) region->callback(region->private_object,
                                vmiop_emul_op_read,
                                vmiop_emul_space_mmio,
                                addr,
                                sizeof(data_value),
                                (void *) &data_value,
                                &cacheable);
    }
    else {
        vmiop_log(vmiop_log_error,
                  "%s: vmiop-env:map-only region access!"
                  "OP: read, addr: 0x%x"
                  "Region{address_space=0x%x, range_base=0x%lx, range_length=0x%lx}",
                  __FUNCTION__, addr,
                  region->address_space, region->range_base, region->range_length);

        data_value = -1;
    }
    if (in_monitor) {
        vmiope_enter_monitor(NULL);
    }
#ifdef TARGET_WORDS_BIGENDIAN
    data_value = htonl(data_value);
#endif
    return(data_value);
}

/**
 * Write MMIO byte
 *
 * @param[in] opaque        Reference to vmiope_region_t object
 * @param[in] addr          Address of byte in target memory.
 * @param[in] mem_value     Data to write
 */

static void
vmiope_mmio_writeb(void *opaque, 
                   uint64_t addr, 
                   uint8_t mem_value)
{
    vmiope_region_t *region = ((vmiope_region_t *) opaque);
    uint8_t data_value;
    vmiop_emul_state_t cacheable = vmiop_emul_noncacheable;
    vmiop_bool_t in_monitor;

    data_value = mem_value;
    vmiope_leave_monitor(&in_monitor);
    if (region->callback) {
        (void) region->callback(region->private_object,
                                vmiop_emul_op_write,
                                vmiop_emul_space_mmio,
                                addr,
                                sizeof(data_value),
                                (void *) &data_value,
                                &cacheable);
    }
    else {
        vmiop_log(vmiop_log_error,
                  "%s: vmiop-env:map-only region access!"
                  "OP: write, addr: 0x%x, value=0x%x"
                  "Region{address_space=0x%x, range_base=0x%lx, range_length=0x%lx}",
                  __FUNCTION__, addr, mem_value,
                  region->address_space, region->range_base, region->range_length);
    }
    if (in_monitor) {
        vmiope_enter_monitor(NULL);
    }
}

/**
 * Write MMIO short
 *
 * @param[in] opaque        Reference to vmiope_region_t object
 * @param[in] addr          Address of short in target memory.
 * @param[in] mem_value     Data to write
 */

static void 
vmiope_mmio_writew(void *opaque, 
                   uint64_t addr, 
                   uint16_t mem_value)
{
    vmiope_region_t *region = ((vmiope_region_t *) opaque);
    uint16_t data_value;
    vmiop_emul_state_t cacheable = vmiop_emul_noncacheable;
    vmiop_bool_t in_monitor;

#ifdef TARGET_WORDS_BIGENDIAN
    data_value = ntohs(mem_value);
#else
    data_value = mem_value;
#endif
    vmiope_leave_monitor(&in_monitor);
    if (region->callback) {
        (void) region->callback(region->private_object,
                                vmiop_emul_op_write,
                                vmiop_emul_space_mmio,
                                addr,
                                sizeof(data_value),
                                (void *) &data_value,
                                &cacheable);
    }
    else {
        vmiop_log(vmiop_log_error,
                  "%s: vmiop-env:map-only region access!"
                  "OP: write, addr: 0x%x, value=0x%x"
                  "Region{address_space=0x%x, range_base=0x%lx, range_length=0x%lx}",
                  __FUNCTION__, addr, mem_value,
                  region->address_space, region->range_base, region->range_length);
    }
    if (in_monitor) {
        vmiope_enter_monitor(NULL);
    }
}

/**
 * Write MMIO word
 *
 * @param[in] opaque        Reference to vmiope_region_t object
 * @param[in] addr          Address of word in target memory.
 * @param[in] mem_value     Data to write
 */

static void 
vmiope_mmio_writel(void *opaque, 
                   uint64_t addr, 
                   uint32_t mem_value)
{
    vmiope_region_t *region = ((vmiope_region_t *) opaque);
    uint32_t data_value;
    vmiop_emul_state_t cacheable = vmiop_emul_noncacheable;
    vmiop_bool_t in_monitor;

#ifdef TARGET_WORDS_BIGENDIAN
    data_value = ntohl(mem_value);
#else
    data_value = mem_value;
#endif
    vmiope_leave_monitor(&in_monitor);
    if (region->callback) {
        (void) region->callback(region->private_object,
                                vmiop_emul_op_write,
                                vmiop_emul_space_mmio,
                                addr,
                                sizeof(data_value),
                                (void *) &data_value,
                                &cacheable);
    }
    else {
        vmiop_log(vmiop_log_error,
                  "%s: vmiop-env:map-only region access!"
                  "OP: write, addr: 0x%x, value=0x%x"
                  "Region{address_space=0x%x, range_base=0x%lx, range_length=0x%lx}",
                  __FUNCTION__, addr, mem_value,
                  region->address_space, region->range_base, region->range_length);
    }
    if (in_monitor) {
        vmiope_enter_monitor(NULL);
    }
}

static io_ops_t vmiope_mmio_ops = {
    .readb = vmiope_mmio_readb,
    .readw = vmiope_mmio_readw,
    .readl = vmiope_mmio_readl,
    .writeb = vmiope_mmio_writeb,
    .writew = vmiope_mmio_writew,
    .writel = vmiope_mmio_writel
};


typedef struct vmiope_region_mapping_s {
    vmiop_list_header_t list_head;
    uint32_t region_pagenum;
    uint32_t num_pages;
    uint32_t refcount;
} vmiope_region_mapping_t;

/**
 * Parameter block for list function for region mapping list.
 */

typedef struct vmiope_region_mapping_apply_s {
    unsigned pagenum;
    /*!< page number offset to locate */
    vmiope_region_mapping_t *mapping;
    /*!< receives reference to device object found */
} vmiope_region_mapping_apply_t;

/**
 * Check if region mapping matches input page offset.
 *
 * @param[in] private_object Reference to
 *                      vmiope_region_mapping_apply_t object.
 *                      Reference to list member stored
 *                      on a match.
 * @param[in] link_p    Reference to list header in list
 *                      member
 * @returns vmiop_bool_t:
 *              vmiop_true: Terminate loop (item found)
 *              vmiop_false:  Continue loop (item not found)
 */

static vmiop_bool_t
vmiope_region_mapping_match(void *private_object,
                            vmiop_list_header_ref_t link_p)
{
    vmiope_region_mapping_apply_t *apply = (vmiope_region_mapping_apply_t *) private_object;
    vmiope_region_mapping_t *mapping = (vmiope_region_mapping_t *) link_p;

    if ((apply->pagenum >= mapping->region_pagenum) &&
        (apply->pagenum < mapping->region_pagenum + mapping->num_pages)) {
        apply->mapping = mapping;
        return(vmiop_true);
    }
    return(vmiop_false);
}

/**
 * Locate region mapping entry
 *
 * @param[in] region   Reference to vmiope_region_t object
 * @param[in] pagenum  Page offset from start or region
 * @returns Reference to mapping, or NULL if not found
 */
static vmiope_region_mapping_t *
vmiope_locate_region_mapping(vmiope_region_t *region,
                             const unsigned pagenum)
{
    vmiope_region_mapping_apply_t apply;

    apply.pagenum = pagenum;
    apply.mapping = NULL;
    if (vmiope_list_apply(&(region->mapping_list),
                          vmiope_region_mapping_match,
                          (void *) &apply)) {
        return(apply.mapping);
    }
    return(NULL);
}

/**
 * Disable guest region (make it guest invisible).
 *
 * @param[in] region    Reference to vmiope_region_t object.
 * @returns Error code:
 * -            vmiop_success:          Successful disable.
 * -            vmiop_error_inval:      Invalid region object.
 */

static vmiop_error_t
vmiope_disable_guest_region(vmiope_region_t *region)
{
    vmiop_error_t error_code = vmiop_success;

    switch(region->address_space) {

    case vmiop_emul_space_io:
        demu_deregister_port_space(region->range_base);
        break;

    case vmiop_emul_space_mmio:
        demu_deregister_memory_space(region->range_base);
        break;

    default:
        error_code = vmiop_error_inval;
        break;
    }
    return(error_code);
}

/**
 * Define a region of guest pseudo-physical address space within which later 
 * mappings may be made.
 *
 * @param[in] range_length  Length of address range to define
 * @param[in] emul_callback Pointer to a callback routine, which will be
 *        called on any read or write to the range. If null, the region
 *        is meant to direct-mapped into guest pseudo-physical address space.
 * @param[in] private_object Pointer private to the caller, which will be
 *        passed to the callback routine on any call.
 * @param[in] address_space Emulation space type (MMIO space or I/O register space)
 * @param[out] region_hanle_p handle for region
 * @returns Error code:
 * -            vmiop_success:          Successful allocation
 * -            vmiop_error_inval:      Zero range_length
 * -            vmiop_error_no_address_space:  Specified range not available
 *                                      in the guest.
 */

vmiop_error_t
vmiop_create_guest_region(const vmiop_emul_length_t range_length,
                          const vmiop_emul_callback_t emul_callback,
                          void *private_object,
                          const vmiop_emul_space_t address_space,
                          vmiop_handle_t *region_handle_p)
{
    vmiope_region_t *region;
    vmiop_error_t error_code = vmiop_success;
    vmiop_bool_t in_monitor;

    if ((range_length <= 0) ||
        ((address_space == vmiop_emul_space_mmio) &&
         (vmiope_bytes_to_pages(range_length) <= 0))) {
        return vmiop_error_inval;
    }

    error_code = vmiope_enter_lock(&in_monitor, &region_lock);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    error_code = vmiope_allocate_table_element(&region_table,
                                               region_handle_p,
                                               (void **) &region);
    if (error_code != vmiop_success) {
        vmiope_leave_lock(in_monitor, &region_lock);
        return(error_code);
    }

    region->address_space = address_space;
    region->range_base = VMIOP_EMUL_ADDR_NONE;
    region->range_length = range_length;
    region->private_object = private_object;
    region->callback = emul_callback;

    vmiope_leave_lock(vmiop_true, &region_lock);

    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }

    return error_code;
}

/**
 * Update guest VESA linear frame buffer address.
 *
 * @param[in] region_handle handle for region
 * @param[in] range_base    address in the guest domain to be used as
 *         VESA linear frame buffer address.
 * @returns Error code:
 *              vmiop_success:          Successful mapping
 *              vmiop_error_not_found:  Null range_base
 */

vmiop_error_t
vmiop_update_guest_lfb(vmiop_handle_t region_handle,
                       vmiop_emul_addr_t range_base)
{
    /* Move the VRAM */
    if (range_base != VMIOP_EMUL_ADDR_NONE) {
        demu_set_vram_addr(range_base);
        return vmiop_success;
    }
    return vmiop_error_not_found;
}

/**
 * Restore guest VRAM to its original address.
 * @params none
 * @returns nothing
 */

void
vmiop_restore_original_lfb(void)
{
    demu_set_vram_addr(VRAM_RESERVED_ADDRESS);
}

/**
 * Relocate a region of guest pseudo-physical address space.
 *
 * The region and all its mappings are hidden from the guest address space
 * if the range_base is VMIOP_EMUL_ADDR_NONE, or relocated in the guest 
 * address space.
 *
 * @param[in] handle for region
 * @param[in] range_base  Pseudo-physical address in the guest domain at
 *         which to start the mapping, or VMIOP_EMUL_ADDR_NONE.
 * @returns Error code:
 * -            vmiop_success:          Successful mapping
 * -            vmiop_error_inval:      Null region handle
 * -            vmiop_error_no_address_space:  Specified range not available
 *                                      in the guest.
 */

vmiop_error_t
vmiop_relocate_guest_region(vmiop_handle_t region_handle,
                            vmiop_emul_addr_t range_base)
{
    vmiop_error_t error_code = vmiop_success;
    vmiop_bool_t in_monitor;
    vmiope_region_t *region;

    error_code = vmiope_enter_lock(&in_monitor, &region_lock);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    region = vmiope_handle_to_region(region_handle);
    if (region == NULL) {
        error_code = vmiop_error_not_found;
        vmiope_leave_lock(vmiop_true, &region_lock);
        goto error_exit;
    }

    if (range_base == region->range_base) {
        /* no change in base address, so exit with success */
        vmiope_leave_lock(vmiop_true, &region_lock);
        goto error_exit;
    }

    vmiope_leave_lock(vmiop_true, &region_lock);

    if (range_base == VMIOP_EMUL_ADDR_NONE) {
        /* new base address is none, so disable the region */

        vmiope_disable_guest_region(region);

    } else {
        /* enable new base address */

        if (region->range_base != VMIOP_EMUL_ADDR_NONE) {
            /* region already enabled, so disable current region first */

            vmiope_disable_guest_region(region);
        }

        switch(region->address_space) {

        case vmiop_emul_space_io:
            if (range_base >= 0x10000 ||
                (range_base + region->range_length) > 0x10000) {
                error_code = vmiop_error_inval;
                goto error_exit;
            }
            
            if (demu_register_port_space(range_base, region->range_length,
                                         &vmiope_ioport_ops, region) < 0) {
                error_code = vmiop_error_inval;
                goto error_exit;
            }

            break;

        case vmiop_emul_space_mmio:

            if (demu_register_memory_space(range_base, region->range_length,
                                           &vmiope_mmio_ops, region) < 0) {
                error_code = vmiop_error_inval;
                goto error_exit;
            }

            break;

        default:
            error_code = vmiop_error_inval;
            break;
        }
    }   

    /* save new base address */
    if (error_code == vmiop_success) {
        region->range_base = range_base;
    }

 error_exit:
    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }

    return(error_code);
}

/**
 * Map or unmap a section of the physical address space into a pseudo-physical
 * address range visible to the guest.  If the access_mode is 
 * vmiop_access_none, the mapping is removed, and the physical_address is
 * effectively ignored.
 *
 * Any prior mapping of the guest pseudo-physical address range is
 * completely replaced.   
 *
 * If the region_handle is null, a separate region is created for the mapping,
 * which will then be deleted when the mapping is removed.  If the region_handle
 * is not null, the range_base is still interpreted as obsolute, and the
 * offset within the region is obtained by subtracting the region base.  It 
 * is an erorr to call this routine with a non-null region handle when 
 * the region is not located within the address space.
 *
 * @param[in] region_handle  Handle for mapping region
 * @param[in] physical_address Local address to be mapped into guest domain
 * @param[in] range_length  Length of address range to map
 * @param[in] range_base  Pseudo-physical address in the guest domain at
 *            which to start the mapping.
 * @param[in] access_mode Access mode (none, read/write, read-only)
 * @returns Error code:
 * -            vmiop_success:          Successful mapping
 * -            vmiop_error_inval:      Zero range_length, or
 *                                      vmiop_access_read_only specified
 *                                      and not supported, or region not
 *                                      located in guest address space,
 *                                      or invalid region handle, or
 *                                      specified range outside the region
 *                                      (if the region_handle is not null)
 * -            vmiop_error_no_address_space:  Specified range not available
 *                                      in the guest.  (Only possible if
 *                                      the region_handle is null.)
 */

extern vmiop_error_t
vmiop_map_guest_region(vmiop_handle_t region_handle,
                       vmiop_emul_addr_t physical_address,
                       void *host_virt_addr,
                       const vmiop_emul_length_t range_length,
                       vmiop_emul_addr_t range_base,
                       const vmiop_access_t access_mode)
{
    int error_value = 0;
    vmiop_bool_t in_monitor;
    vmiope_emul_page_t first_page, last_page;
    vmiope_region_t* region;
    vmiope_region_mapping_t *mapping;
    vmiop_error_t error_code = vmiop_success;

    if (range_length == 0 ||
        access_mode == vmiop_access_read_only) {
        return(vmiop_error_inval);
    }

    vmiope_enter_monitor(&in_monitor);

    region = vmiope_handle_to_region(region_handle);

    if ((region == NULL) ||
        (region->address_space != vmiop_emul_space_mmio) ||
        (region->range_base == VMIOP_EMUL_ADDR_NONE)) {
        error_code = vmiop_error_not_found;
        goto exit;
    }

    first_page  = range_base >> TARGET_PAGE_SHIFT;
    last_page   = (range_base + (range_length - 1)) >> TARGET_PAGE_SHIFT;

    mapping = vmiope_locate_region_mapping(region, first_page);
    if (access_mode == vmiop_access_none) {
        if (!mapping) {
            error_code = vmiop_error_not_found;
            goto exit;
        }

        if (mapping->refcount > 1)
        {
            mapping->refcount--;
            error_code = vmiop_success;
            goto exit;
        }

        vmiope_list_remove(&(region->mapping_list), &(mapping->list_head));
        vmiop_memory_free_internal((void *) mapping, sizeof(vmiope_region_mapping_t));
    } else {
        if ((range_base < region->range_base) ||
            ((range_base + range_length) >
             (region->range_base + region->range_length))) {
            error_code = vmiop_error_inval;
            goto exit;
        }
    
        if (mapping) {
            mapping->refcount++;
            error_code = vmiop_success;
            goto exit;
        } else {
            error_code = vmiop_memory_alloc_internal((vmiop_emul_length_t)
                                            sizeof(vmiope_region_mapping_t),
                                            (void **) &mapping,
                                            vmiop_true);
            if (error_code != vmiop_success) {
                goto exit;
            }

            mapping->region_pagenum = first_page;
            mapping->num_pages = (last_page - first_page) + 1;
            mapping->refcount = 1;

            vmiope_list_insert(&(region->mapping_list),
                               &(mapping->list_head));
        }
    }

    error_value = demu_set_guest_pages(range_base >> TARGET_PAGE_SHIFT,
                                       physical_address >> TARGET_PAGE_SHIFT,
                                       (range_length + TARGET_PAGE_SIZE - 1) >> TARGET_PAGE_SHIFT,
                                       (access_mode == vmiop_access_read_write));

    if (error_value != 0) {
        if (access_mode == vmiop_access_read_write)
        {
            vmiope_list_remove(&(region->mapping_list), &(mapping->list_head));
            vmiop_memory_free_internal((void *) mapping, sizeof(vmiope_region_mapping_t));
        }

        error_code = (error_value == -ESRCH)
                      ? vmiop_error_no_address_space
                      : vmiop_error_inval;
    } else {
        error_code = vmiop_success;
    }

exit:
    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }

    return error_code;
}

/**
 * Gain superuser privileges. Some operations might require access
 * to resources granted only to a privileged user.
 * @parmas none
 * @returns nothing
 */

void
vmiop_set_su(void)
{
}

/**
 * Drop superuser privileges obtained by vmiop_set_su.
 * @params none
 * @returns nothing
 */

void
vmiop_drop_su(void)
{
}

#define VMIOPE_TRANSLATE_PAGE_COUNT 128
/*!< Limit for batch of page translations */

static uint32_t vmiope_guest_pfn_count;
/*!< Max guest PFN count */

/**
 * Unpin a set of guest PFNs.
 *
 * @param[in] gpfn_list  Reference to array of guest page frame numbers.
 * @param[in] hpfn_list  Reference to array of host page frame numbers.
 *                       Element set to VMIOP_PAGE_NUMBER_NULL if corresponding
 *                       guest page frame number is not mapped on the host.
 *                       gpfn_list and hpfn_list may refer to the same array.
 * @param[in] pfn_count  Count of elements in each array.
 *
 * @returns Error code:
 * -        vmiop_success   Successful completion.
 * -        vmiop_error_not_found:  Handle not found.
 * -        vmiop_error_not_allowed_from_callback: Cannot unpin
 *                          from emulation callback routine.
 */

vmiop_error_t
vmiop_unpin_guest_pages(unsigned long *gpfn_list,
                        unsigned long *hpfn_list,
                        uint32_t pfn_count)
{
    int i, c = 0, rc = 0;
    xen_pfn_t *mfn_list = NULL;
    vmiop_error_t error_code = vmiop_success;
    vmiop_bool_t did_mlock = vmiop_false;
    xen_pfn_t hpfn;

    mfn_list = (xen_pfn_t *) malloc (sizeof(xen_pfn_t) * VMIOPE_TRANSLATE_PAGE_COUNT);
    if (mfn_list == NULL) {
        error_code = vmiop_error_resource;
        goto error_exit;
    }

    error_code = mlock(mfn_list, sizeof(sizeof(xen_pfn_t) * VMIOPE_TRANSLATE_PAGE_COUNT));
    if (error_code != 0) {
        error_code = vmiop_error_resource;
        goto error_exit;
    }
    did_mlock = vmiop_true;

    for (i = 0; i < pfn_count; i++) {
        hpfn = hpfn_list[i];

        if (hpfn == VMIOP_PAGE_NUMBER_NULL) {
            continue;
        }

        mfn_list[c] = hpfn;
        c++;
        hpfn_list[i] = VMIOP_PAGE_NUMBER_NULL;

	/* Free pages in chunks of VMIOPE_TRANSLATE_PAGE_COUNT */
        if (c == VMIOPE_TRANSLATE_PAGE_COUNT) {
            rc = demu_release_guest_pages(mfn_list, c);
            if (rc < 0) {
                vmiop_log(vmiop_log_error,
                    "vmiop_unpin_guest_pages: unpin failed with error %d\n",
                    rc);
                error_code = vmiop_error_inval;
            }
            c = 0;
        }
    }

    if (c != 0) {
        rc = demu_release_guest_pages(mfn_list, c);
        if (rc < 0) {
            vmiop_log(vmiop_log_error,
                "vmiop_unpin_guest_pages: unpin failed with error %d\n",
                rc);
            error_code = vmiop_error_inval;
        }
    }

 error_exit:
    if (did_mlock) {
        munlock(mfn_list, sizeof(sizeof(xen_pfn_t) * VMIOPE_TRANSLATE_PAGE_COUNT));
        did_mlock = vmiop_false;
    }
    if (mfn_list != NULL) {
        free(mfn_list);
        mfn_list = NULL;
    }
    
    return(error_code);
}

/**
 * Pin a set of guest PFNs and return their associated host PFNs
 *
 * @param[in] gpfn_list  Reference to array of guest page frame numbers.
 * @param[out] hpfn_list Reference to array of host page frame numbers.
 *                       Element set to VMIOP_PAGE_NUMBER_NULL if corresponding
 *                       guest page frame number is not mapped on the host.
 *                       gpfn_list and hpfn_list may refer to the same array.
 * @param[in] pfn_count  Count of elements in each array.
 * @returns Error code:
 * -        vmiop_success:      Successful completion
 * -        vmiop_error_inval:  NULL addr_list
 * -        vmiop_error_range:  Table too large
 * -        vmiop_error_not_allowed_from_callback: Cannot pin
 *                              from emulation callback routine.
 */

extern vmiop_error_t
vmiop_pin_guest_pages(unsigned long *gpfn_list,
                      unsigned long *hpfn_list,
                      uint32_t pfn_count)
{
    xen_pfn_t tmp_gpfn_list[VMIOPE_TRANSLATE_PAGE_COUNT];
    xen_pfn_t tmp_mfn_list[VMIOPE_TRANSLATE_PAGE_COUNT];
    uint32_t translate_limit = VMIOPE_TRANSLATE_PAGE_COUNT;
    uint32_t translate_count;
    uint32_t li;
    uint32_t ti;
    vmiop_error_t error_code = vmiop_success;

    if (gpfn_list == NULL ||
        hpfn_list == NULL) {
        return(vmiop_error_inval);
    }

    if (pfn_count == 0)
        return(vmiop_success);

    for (li = 0; li < pfn_count; li += translate_count) {
        /* Break the Guest PFN list into chunks of translate_limit 
         * before translation 
         */
        translate_count = pfn_count - li;

        if (translate_count > translate_limit)
            translate_count = translate_limit;
        
        for (ti = 0; ti < translate_count; ti++) 
            tmp_gpfn_list[ti] = gpfn_list[li + ti];
        
        error_code = demu_translate_guest_pages(tmp_gpfn_list, tmp_mfn_list,
                                                translate_count);
        if (error_code != 0) {
            if (translate_limit > 1) {
                translate_count = 0;
                translate_limit = 1;
                continue;
            }
            break;
        }

            if (error_code == 0) {
            for (ti = 0; ti < translate_count; ti++) {
                if (tmp_gpfn_list[ti] <= (vmiope_guest_pfn_count - 1)) {
                    /* Insert the translated pages into hpfn_list */
                    hpfn_list[li + ti] = tmp_mfn_list[ti];
                } else {
                    /* check if we need to bump up the max gpfn */
                    int retval = demu_maximum_guest_pages();
                    if (retval < 0) {
                        vmiop_log (vmiop_log_error, 
                            "Unable to fetch domain information %d\n", retval);
                        error_code = retval;
                        goto error_exit;
                    } else {
                        if (retval > vmiope_guest_pfn_count)
                            vmiope_guest_pfn_count = retval;
                    }

                    if (tmp_gpfn_list[ti] <= (vmiope_guest_pfn_count - 1)) {
                        hpfn_list[li + ti] = tmp_mfn_list[ti];
                    } else {
                        vmiop_log(vmiop_log_error, "GPFN (0x%08llx) is beyond max GPFN "
                                  "(0x%08llx) reported by hypervisor\n", tmp_gpfn_list[ti],
                                  vmiope_guest_pfn_count);
                        error_code = vmiop_error_inval;
                        goto error_exit;
                    }
                }
            }
        } else {
            vmiop_log(vmiop_log_error, "demu_translate_guest_pages() failed "
                      "with error: %d\n", error_code);
            goto error_exit;
        }
    }

 error_exit:
    if (error_code != vmiop_success) {
        vmiop_log(vmiop_log_error, "failed to translate guest pages %d", error_code);
    }

    return(error_code);
}

vmiop_error_t
vmiop_set_guest_dirty_pages(uint64_t count, const struct pages * page_list) {
    if (demu_set_guest_dirty_pages(count, page_list) != 0)
        return vmiop_error_inval;

    return vmiop_success;
}

/*
 * Control MSI interrupt
 *
 * @brief Send an MSI or MSI-X interrupt to the guest
 *
 * @param[in] handle         Emulated device handle from vmiop_register_emul_device()
 * @param[in] msg_addr       MSI address assigned by guest OS
 * @param[in] msg_data       MSI data
 *
 * @returns
 *   vmiop_success           Successful completion
 *   vmiop_error_inval       PCI handle VMIOP_HANDLE_NULL
 *   vmiop_error_not_found   Handle not found
 *
 */

vmiop_error_t
vmiop_control_interrupt_msi(vmiop_handle_t handle,      // IN
                            vmiop_emul_addr_t msg_addr, // IN
                            uint32_t msg_data)          // IN
{
    vmiope_emul_device_ref_t emul_device;
    vmiop_error_t error_code;
    vmiop_bool_t in_monitor;

    vmiope_enter_monitor(&in_monitor);

    error_code = vmiope_locate_emul_device(handle,&emul_device);
    if (error_code != vmiop_success) {
        if (! in_monitor) {
            vmiope_leave_monitor(NULL);
        }
        return(error_code);
    }

    demu_inject_msi(msg_addr, msg_data);

    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }

    return (vmiop_success);
}


/**
 * Control interrupt
 *
 * @param[in] handle           Emulated device handle from vmiop_register_emul_device()
 *                                for type vmiop_emul_space_config.
 * @param[in] interrupt_line   PCI interrupt line# (0-3)
 * @param[in] mode             Interrupt mode (on, off)
 * @returns Error code:
 * -                vmiop_success   Successful completion
 * -                vmiop_error_inval Not a PCI configuration space handle
 *                                  or irq out of range
 * -                vmiop_error_not_found Handle not found
 */

vmiop_error_t
vmiop_control_interrupt(vmiop_handle_t handle,
                        uint32_t interrupt_line,
                        vmiop_interrupt_mode_t mode)
{
    vmiope_emul_device_ref_t emul_device;
    vmiop_error_t error_code;
    vmiop_bool_t in_monitor;

    vmiope_enter_monitor(&in_monitor);

    error_code = vmiope_locate_emul_device(handle,&emul_device);
    if (error_code != vmiop_success) {
        if (! in_monitor) {
            vmiope_leave_monitor(NULL);
        }
        return(error_code);
    }

    if (interrupt_line > 3) {
        if (! in_monitor) {
            vmiope_leave_monitor(NULL);
        }
        return(vmiop_error_inval);
    }
    demu_set_pci_intx(interrupt_line,
                      ((mode == vmiop_intr_off)
                       ? 0
                       : 1));
    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }
    return(vmiop_success);
}

/*@}*/


/**********************************************************************/
/**
 * @defgroup TaskSupportImpl   Task Support 
 */
/**********************************************************************/
/*@{*/

/*
 * Thread management
 */

/**
 * Thread entry 
 */

typedef struct vmiope_thread_s {
    pthread_t pthread;              /*!< pthread object */
    vmiop_thread_init_t init_p;     /*!< initialization routine */
    void *private_object;           /*!< object for init_p */
} vmiope_thread_t;

typedef vmiope_thread_t vmiope_thread_array_t[1];
/*!< array of one thread for sizing */

/**
 * Test if thread object is free.
 *
 * @param[in] thread_p    Reference to thread (as void *)
 * @returns vmiop_bool_t  True if thread is free, false otherwise
 */

static vmiop_bool_t
vmiope_thread_is_free(void *thread_p) 
{
    return(((vmiope_thread_t *) thread_p)->init_p == NULL);
}


static vmiope_object_table_defn_t thread_table = {
    .head = NULL,
    .table_limit = 0,
    .is_free = vmiope_thread_is_free,
    .element_size = sizeof(vmiope_thread_array_t)
}; /*!< definition of thread table */

static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER;
/*!< lock for thread management */

/**
 * Convert handle to thread reference
 *
 * @param[in] handle    Thread handle
 * @returns Reference to thread, or NULL if out of range
 */

static inline vmiope_thread_t *
vmiope_handle_to_thread(vmiop_handle_t handle)
{
    return((vmiope_thread_t *) vmiope_handle_to_object(&thread_table,
                                                       handle));
}

/**
 * Convert thread reference to handle
 *
 * @param[in] thread_p Reference to thread
 * @returns Handle, or VMIOP_HANDLE_NULL if thread_p is NULL
 */

static inline vmiop_handle_t 
vmiope_thread_to_handle(vmiope_thread_t *thread_p)
{
    return(vmiope_object_to_handle(&thread_table,
                                   (void *) thread_p));
}

/**
 * Find a free thread handle
 *
 * @param[out] handle_p Reference to variable to receive new handle.
 * @param[out] thread_p Reference to variable to receive pointer to
 *                      new thread object.  Variable set to NULL on
 *                      an error.  Reference ignored if thread_p is NULL.
 * @returns Error code:
 * -            vmiop_success:          Successful registration
 * -            vmiop_error_inval:      NULL handle_p
 * -            vmiop_error_resource:   No memory or other resource unavailable
 */

static inline vmiop_error_t
vmiope_find_free_thread_handle(vmiop_handle_t *handle_p,
                               vmiope_thread_t **thread_p)
{
    return(vmiope_allocate_table_element(&thread_table,
                                         handle_p,
                                         ((void **) thread_p)));
}

/**
 * Initialize a new thread.
 *
 * @param[in] thread_p Reference to vmiope_thread_t as void *
 * @returns NULL
 */

static void *
vmiope_thread_init(void *thread_p)
{
    vmiop_thread_init_t init_p;

    /* block SIGNAL delivery for all threads */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0)
        return(NULL);

    init_p = ((vmiope_thread_t *) thread_p)->init_p;
    if (init_p == NULL) {
        return(NULL);
    }
    /* block until vmiop_thread_alloc() finishes */
    (void) pthread_mutex_lock(&thread_lock); 
    (void) pthread_mutex_unlock(&thread_lock);
    /* enter the new thread */
    init_p(vmiope_thread_to_handle(((vmiope_thread_t *) thread_p)),
           ((vmiope_thread_t *) thread_p)->private_object);
    return(NULL);
}

/*
 * Synchronization
 */

/**
 * Thread event and lock types
 */

typedef enum vmiope_thread_event_type_e {
    vmiope_tetype_unallocated = 0,
    vmiope_tetype_lock = 1,
    vmiope_tetype_event = 2,
    vmiope_tetype_spinlock = 3
} vmiope_thread_event_type_t;
/* thread event and lock types */

/**
 * Thread event and lock entry.
 *
 * The same table is used for both locks and events.  When
 * an entry is allocated as a lock, the cond field is not used.
 */

typedef struct vmiope_thread_event_s {
    union {
        pthread_mutex_t mutex;   /*!< mutex object */
        pthread_spinlock_t spinlock; /*!< spinlock object */
    } u; /*!< union for locks */
    pthread_cond_t cond;     /*!< condition variable */
    vmiop_bool_t event_set;  /*!< true when event set */
    vmiope_thread_event_type_t type; /*!< type of event */
} vmiope_thread_event_t;

typedef vmiope_thread_event_t vmiope_thread_event_array_t[1];
/*!< single-element event array for sizing */

/**
 * Test if thread event object is free.
 *
 * @param[in] event_p     Reference to thread event (as void *)
 * @returns vmiop_bool_t  True if thread is free, false otherwise
 */

static vmiop_bool_t
vmiope_thread_event_is_free(void *event_p) 
{
    return(((vmiope_thread_event_t *) event_p)->type == 
           vmiope_tetype_unallocated);
}

static vmiope_object_table_defn_t thread_event_table = {
    .head = NULL,
    .table_limit = 0,
    .is_free = vmiope_thread_event_is_free,
    .element_size = sizeof(vmiope_thread_event_array_t)
}; /*!< definition of thread event table */


/**
 * Convert handle to thread event reference
 *
 * @param[in] handle    Thread event handle
 * @param[in] type      Type of object
 * @returns Reference to thread event, or NULL if out of range
 */

static inline vmiope_thread_event_t *
vmiope_handle_to_thread_event(vmiop_handle_t handle,
                              vmiope_thread_event_type_t type)
{
    vmiope_thread_event_t *te;

    if (handle == VMIOP_HANDLE_NULL)
        return(NULL);

    te = ((vmiope_thread_event_t *) 
          vmiope_handle_to_object(&thread_event_table,
                                  handle));
    if (te != NULL &&
        te->type == type) {
        return(te);
    }
    return(NULL);
}

/**
 * Find a free thread event handle
 *
 * @param[out] handle_p Reference to variable to receive new handle.
 * @param[out] event_p  Reference to variable to receive pointer to
 *                      new event.   Variable is set to NULL on 
 *                      on an error.  Reference ignored if NULL.
 * @param[in] type      Type of thread event object
 * @returns Error code:
 * -            vmiop_success:          Successful registration
 * -            vmiop_error_inval:      NULL handle_p
 * -            vmiop_error_resource:   No memory or other resource unavailable
 */

static vmiop_error_t
vmiope_find_free_thread_event_handle(vmiop_handle_t *handle_p,
                                     vmiope_thread_event_t **event_p,
                                     vmiope_thread_event_type_t type)
{
    vmiop_error_t error_code;
    vmiope_thread_event_t *thread_event;
    int error_value;

    error_code = vmiope_allocate_table_element(&thread_event_table,
                                               handle_p,
                                               ((void **) event_p));
    if (error_code != vmiop_success) {
        return(error_code);
    }

    thread_event = *event_p;
    if (type == vmiope_tetype_spinlock) {
        error_value = pthread_spin_init(&thread_event->u.spinlock,
                                        PTHREAD_PROCESS_PRIVATE);
    } else {
        error_value = pthread_mutex_init(&thread_event->u.mutex,
                                         NULL);
        if (error_value == 0 && 
            type == vmiope_tetype_event) {
            error_value = pthread_cond_init(&thread_event->cond,
                                            NULL);
        }
    }
    if (error_value != 0) {
        *handle_p = VMIOP_HANDLE_NULL;
        *event_p = NULL;
        return(vmiop_error_resource);
    }

    thread_event->type = type;
    return(vmiop_success);
}

/**
 * Allocate a new thread.  Thread terminates when initial routine
 * exits.
 *
 * @param[in] private_object    Reference to private object to pass to initial routine
 * @param[in] init_p            Reference to initial routine for thread
 * @param[out] handle_p         Reference to variable to receive handle for thread
 * @returns Error code:
 * -            vmiop_success:          Successful allocation
 * -            vmiop_error_inval:      NULL init_routine or handle_p
 * -            vmiop_error_resource:   Memory or other resource unavailable
 */

vmiop_error_t
vmiop_thread_alloc(void *private_object,
                   vmiop_thread_init_t init_p,
                   vmiop_handle_t *handle_p)
{
    vmiop_handle_t handle;
    vmiop_error_t error_code;
    vmiope_thread_t *new_thread;
    vmiop_bool_t in_monitor;

    error_code = vmiope_enter_lock(&in_monitor,
                                   &thread_lock);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    error_code = vmiope_find_free_thread_handle(&handle,&new_thread);
    if (error_code == vmiop_success) {
        new_thread->init_p = init_p;
        new_thread->private_object = private_object;
        if (pthread_create(&new_thread->pthread,
                           NULL,
                           vmiope_thread_init,
                           (void *) new_thread) != 0) {
            new_thread->init_p = NULL;
            handle = VMIOP_HANDLE_NULL;
            error_code = vmiop_error_resource;
        }
    }

    vmiope_leave_lock(in_monitor,
                      &thread_lock);

    *handle_p = handle;
    return(error_code);
}

/**
 * Join the thread.
 *
 * @param[in] handle  Handle for thread
 *                      
 * @returns Error code:
 * -            vmiop_success:          Successful joined
 * -            vmiop_error_not_found:  No thread_p associated for handle
 * -            vmiop_error_resource:   Thread could not join successfully
 *                                      
 */
vmiop_error_t
vmiop_thread_join(vmiop_handle_t handle)
{
    
    vmiope_thread_t *thread_p;
    thread_p = vmiope_handle_to_thread(handle);
    if (thread_p == NULL) {
        return vmiop_error_not_found;
    }
    if (pthread_join(thread_p->pthread, NULL) != 0) {
        return vmiop_error_resource;
    }
    return vmiop_success;
}

/**
 * Allocate a lock variable.
 *
 * @param[in] handle_p  Reference to variable to receive handle 
 *                      for lock variable
 * @returns Error code:
 * -            vmiop_success:          Successful initialization
 * -            vmiop_error_inval:      NULL handle_p
 * -            vmiop_error_resource:   Memory or other resource
 *                                      unavailable
 */

vmiop_error_t
vmiop_lock_alloc(vmiop_handle_t *handle_p)
{
    vmiop_handle_t handle;
    vmiop_error_t error_code;
    vmiope_thread_event_t *new_event;
    vmiop_bool_t in_monitor;

    error_code = vmiope_enter_lock(&in_monitor,
                                   &thread_lock);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    error_code = vmiope_find_free_thread_event_handle(&handle,
                                                      &new_event,
                                                      vmiope_tetype_lock);

    vmiope_leave_lock(in_monitor,
                      &thread_lock);

    if (error_code == vmiop_success) {
        *handle_p = handle;
    }

    return(error_code);
}

/**
 * Free a lock variable with thread lock held.
 *
 * @param[in] handle    Handle for the lock variable
 * @returns Error code:
 * -            vmiop_success:          Successful release
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found:  handle does not refer to
 *                                      a lock variable
 */

static vmiop_error_t
vmiope_lock_free_nolock(vmiop_handle_t handle)
{
    vmiope_thread_event_t *new_event;
    
    new_event = vmiope_handle_to_thread_event(handle,
                                              vmiope_tetype_lock);
    if (new_event == NULL) {
        return(vmiop_error_inval);
    }

    if (pthread_mutex_destroy(&new_event->u.mutex) == 0) {
        new_event->type = vmiope_tetype_unallocated;
    }

    return(vmiop_success);
}


/**
 * Free a lock variable.
 *
 * @param[in] handle    Handle for the lock variable
 * @returns Error code:
 * -            vmiop_success:          Successful release
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found:  handle does not refer to
 *                                      a lock variable
 */

vmiop_error_t
vmiop_lock_free(vmiop_handle_t handle)
{
    vmiop_error_t error_code;
    vmiop_bool_t in_monitor;

    error_code = vmiope_enter_lock(&in_monitor,
                                   &thread_lock);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    error_code = vmiope_lock_free_nolock(handle);

    vmiope_leave_lock(in_monitor,
                      &thread_lock);

    return(error_code);
}


/**
 * Acquire a lock.
 *
 * @param[in] handle        Lock variable handle
 * @param[in] try_only      If true, try only (do not wait);
 *                          If false, wait until available
 * @returns Error code:
 * -            vmiop_success:          Lock acquired
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found:  handle does not refer to
 *                                      a lock variable
 * -            vmiop_error_timeout:    try_only was true and lock
 *                                      was not avaiable
 */

vmiop_error_t
vmiop_lock(vmiop_handle_t handle,
           vmiop_bool_t try_only)
{
    vmiope_thread_event_t *event;
    int error_code;

    event = vmiope_handle_to_thread_event(handle,
                                          vmiope_tetype_lock);
    if (event == NULL) {
        return(vmiop_error_not_found);
    }

    if (try_only) {
        error_code = pthread_mutex_trylock(&event->u.mutex);
        if (error_code == EBUSY) {
            return(vmiop_error_timeout);
        }
    } else {
        error_code = pthread_mutex_lock(&event->u.mutex);
    }
    if (error_code != 0) {
        return(vmiop_error_inval);
    }

    return(vmiop_success);
}

/**
 * Release a lock.
 *
 * @param[in] handle        Lock variable handle
 * @returns Error code:
 * -            vmiop_success:          Lock released
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found:  handle does not refer to
 *                                      a lock variable
 */

vmiop_error_t
vmiop_unlock(vmiop_handle_t handle)
{
    vmiope_thread_event_t *event;
    int error_code;

    event = vmiope_handle_to_thread_event(handle,
                                          vmiope_tetype_lock);
    if (event == NULL) {
        return(vmiop_error_not_found);
    }

    error_code = pthread_mutex_unlock(&event->u.mutex);
    if (error_code != 0) {
        return(vmiop_error_inval);
    }

    return(vmiop_success);
}

/**
 * Allocate a spinlock variable.
 *
 * @param[in] handle_p  Reference to variable to receive handle 
 *                      for lock variable
 * @returns Error code:
 * -            vmiop_success:          Successful initialization
 * -            vmiop_error_inval:      NULL handle_p
 * -            vmiop_error_resource:   Memory or other resource
 *                                      unavailable
 */

vmiop_error_t
vmiop_spinlock_alloc(vmiop_handle_t *handle_p)
{
    vmiop_handle_t handle;
    vmiop_error_t error_code;
    vmiope_thread_event_t *new_event;
    vmiop_bool_t in_monitor;

    error_code = vmiope_enter_lock(&in_monitor,
                                   &thread_lock);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    error_code = vmiope_find_free_thread_event_handle(&handle,
                                                      &new_event,
                                                      vmiope_tetype_spinlock);

    vmiope_leave_lock(in_monitor,
                      &thread_lock);

    return(error_code);
}

/**
 * Free a spinlock variable.
 *
 * @param[in] handle    Handle for the lock variable
 * @returns Error code:
 * -            vmiop_success:          Successful release
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found:  handle does not refer to
 *                                      a lock variable
 */

vmiop_error_t
vmiop_spinlock_free(vmiop_handle_t handle)
{
    vmiope_thread_event_t *new_event;
    vmiop_error_t error_code = vmiop_success;
    vmiop_bool_t in_monitor;

    error_code = vmiope_enter_lock(&in_monitor,
                                   &thread_lock);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    new_event = vmiope_handle_to_thread_event(handle,
                                              vmiope_tetype_spinlock);
    if (new_event != NULL &&
        pthread_spin_destroy(&new_event->u.spinlock) == 0) {
        new_event->type = vmiope_tetype_unallocated;
    } else {
        error_code = vmiop_error_inval;
    }

    vmiope_leave_lock(in_monitor,
                      &thread_lock);

    return(error_code);
}


/**
 * Acquire a spinlock.
 *
 * @param[in] handle        Lock variable handle
 * @param[in] try_only      If true, try only (do not wait);
 *                          If false, wait until available
 * @returns Error code:
 * -            vmiop_success:          Lock acquired
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found:  handle does not refer to
 *                                      a lock variable
 * -            vmiop_error_timeout:    try_only was true and lock
 *                                      was not avaiable
 */

vmiop_error_t
vmiop_spin_lock(vmiop_handle_t handle,
                vmiop_bool_t try_only)
{
    vmiope_thread_event_t *event;
    int error_code;

    event = vmiope_handle_to_thread_event(handle,
                                          vmiope_tetype_spinlock);
    if (event == NULL) {
        return(vmiop_error_not_found);
    }

    if (try_only) {
        error_code = pthread_spin_trylock(&event->u.spinlock);
        if (error_code == EBUSY) {
            return(vmiop_error_timeout);
        }
    } else {
        error_code = pthread_spin_lock(&event->u.spinlock);
    }
    if (error_code != 0) {
        return(vmiop_error_inval);
    }

    return(vmiop_success);
}

/**
 * Release a spinlock.
 *
 * @param[in] handle        Lock variable handle
 * @returns Error code:
 * -            vmiop_success:          Lock released
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found:  handle does not refer to
 *                                      a lock variable
 */

vmiop_error_t
vmiop_spin_unlock(vmiop_handle_t handle)
{
    vmiope_thread_event_t *event;
    int error_code;

    event = vmiope_handle_to_thread_event(handle,
                                          vmiope_tetype_spinlock);
    if (event == NULL) {
        return(vmiop_error_not_found);
    }

    error_code = pthread_spin_unlock(&event->u.spinlock);
    if (error_code != 0) {
        return(vmiop_error_inval);
    }

    return(vmiop_success);
}


/**
 * Allocate a thread event variable.
 *
 * @param[in] handle_p  Reference to variable to receive handle 
 *                      for event variable
 * @returns Error code:
 * -            vmiop_success:          Successful initialization
 * -            vmiop_error_inval:      NULL handle_p
 * -            vmiop_error_resource:   Memory or other resource
 *                                      unavailable
 */

vmiop_error_t
vmiop_thread_event_alloc(vmiop_handle_t *handle_p)
{
    vmiop_handle_t handle;
    vmiop_error_t error_code;
    vmiope_thread_event_t *new_event;
    vmiop_bool_t in_monitor;

    error_code = vmiope_enter_lock(&in_monitor,
                                   &thread_lock);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    error_code = vmiope_find_free_thread_event_handle(&handle,
                                                      &new_event,
                                                      vmiope_tetype_event);

    vmiope_leave_lock(in_monitor,
                      &thread_lock);

    if (error_code == vmiop_success) {
        *handle_p = handle;
    }
    return(error_code);
}

/**
 * Free a thread event variable.
 *
 * @param[in] handle        Handle for the event variable to free
 * @returns Error code:
 * -            vmiop_success:          Successful release
 * -            vmiop_error_inval:      Handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found:  Handle does not reference
 *                                      an event variable
 */

vmiop_error_t
vmiop_thread_event_free(vmiop_handle_t handle)
{
    vmiop_error_t error_code;
    vmiope_thread_event_t *new_event;
    vmiop_bool_t in_monitor;

    error_code = vmiope_enter_lock(&in_monitor,
                                   &thread_lock);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    new_event = vmiope_handle_to_thread_event(handle,
                                              vmiope_tetype_event);
    if (new_event == NULL) {
        error_code = vmiop_error_not_found;
    } else if (pthread_cond_destroy(&new_event->cond) != 0) {
        error_code = vmiop_error_inval;
    } else {
        new_event->type = vmiope_tetype_lock;
        error_code = vmiope_lock_free_nolock(handle);
    }

    vmiope_leave_lock(in_monitor,
                      &thread_lock);

    return(error_code);
}

/**
 * Wait on a thread event variable.  If the third
 * argument is true, clear the event on a successful wait.
 * A call with a time_value of 0 and a request to clear
 * the event will unconditionally leave the event cleared
 * without waiting.  A call with a variable which no
 * thread ever posts will simply wait for time.  Note
 * that time_value is an absolute time, not the amount
 * of time to wait.   A time_value value in the past is
 * the same as a time_value of 0.
 *
 *
 * @param[in] handle        Event variable handle
 * @param[in] time_value    Time to wait (VMIOP_TIME_NO_LIMIT
 *                          if no timeout, 0 to just test the 
 *                          variable)
 * @param[in] clear_before_return If true, clear event before 
 *                          return on success (event posted)
 * @returns Error code:
 * -            vmiop_success:          Successful wait
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found:  handle does not specify
 *                                      an event variable
 * -            vmiop_error_timeout:    Time expired before event
 *                                      posted, or event was not
 *                                      set after wakeup.
 */

vmiop_error_t
vmiop_thread_event_wait(vmiop_handle_t handle,
                        vmiop_time_t time_value,
                        vmiop_bool_t clear_before_return)
{
    vmiop_error_t error_code;
    vmiope_thread_event_t *event;
    int error_value;
    struct timespec ts;
    vmiop_bool_t in_monitor;

    event = vmiope_handle_to_thread_event(handle,
                                          vmiope_tetype_event);
    if (event == NULL) {
        return(vmiop_error_not_found);
    }

    error_code = vmiope_enter_lock(&in_monitor,
                                   &event->u.mutex);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    if (! event->event_set) {
        if (time_value != VMIOP_TIME_NO_LIMIT) {
            vmiop_time_t tvs;

            tvs = time_value / VMIOPD_NS_PER_SEC;
            if (tvs > INT32_MAX) {
                tvs = INT32_MAX;
            }
            ts.tv_sec = tvs;
            ts.tv_nsec = (time_value % VMIOPD_NS_PER_SEC);
            error_value = pthread_cond_timedwait(&event->cond,
                                                 &event->u.mutex,
                                                 &ts);
            if (error_value == ETIMEDOUT) {
                if (! event->event_set) {
                    error_code = vmiop_error_timeout;
                }
                goto error_exit;
            }
        } else {
            error_value = pthread_cond_wait(&event->cond,
                                            &event->u.mutex);
        }
        if (error_value != 0) {
            error_code = vmiop_error_inval;
        } else if (! event->event_set) {
            error_code = vmiop_error_timeout;
        }
    }

 error_exit:
    if (clear_before_return) {
        event->event_set = vmiop_false;
    }

    vmiope_leave_lock(in_monitor,
                      &event->u.mutex);
    return(error_code);
}

/**
 * Get the current time (base not defined)
 *
 * @param[out] time_value_p Reference to variable to receive
 *                          the current time
 * @returns Error code:
 * -            vmiop_success:          Successful fetch of time
 * -            vmiop_error_inval:      NULL time_value_p
 */

vmiop_error_t
vmiop_thread_get_time(vmiop_time_t *time_value_p)
{
    struct timespec ts;
    int error_value;
    
    if (time_value_p == NULL) {
        return(vmiop_error_inval);
    }

    error_value = clock_gettime(CLOCK_REALTIME,
                                &ts);
    if (error_value != 0) {
        *time_value_p = 0;
        return(vmiop_error_inval);
    }

    *time_value_p = ((((vmiop_time_t) ts.tv_sec) * VMIOPD_NS_PER_SEC) + 
                     ((vmiop_time_t) ts.tv_nsec));
    return(vmiop_success);
}

/**
 * Post a thread event variable (set it true, and wake one or
 * all threads waiting on the variable).  If wakeup_first is true,
 * and there are multiple waiters, the first waiter is awakened
 * and the variable is left false.  Otherwise, the variable is
 * set true and all waiters are awakened.  If any of the waiters
 * requested that the variable be cleared, it is left cleared.
 * If there are no waiters, the variable is unconditionally
 * left set.
 *
 * @param[in] handle        Handle for event variable
 * @param[in] wakeup_first  If true, wakeup only first waiter
 * @returns Error code:
 * -            vmiop_success:          Successful post
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found:  handle does to refer to 
 *                                      an event variable
 */

vmiop_error_t
vmiop_thread_event_post(vmiop_handle_t handle,
                        vmiop_bool_t wakeup_first)
{
    vmiope_thread_event_t *event;
    vmiop_error_t error_code;
    int error_value;
    vmiop_bool_t in_monitor;
    
    event = vmiope_handle_to_thread_event(handle,
                                          vmiope_tetype_event);
    if (event == NULL) {
        return(vmiop_error_not_found);
    }

    error_code = vmiope_enter_lock(&in_monitor,
                                   &event->u.mutex);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    if (! event->event_set) {
        event->event_set = vmiop_true;

        if (wakeup_first) {
            error_value = pthread_cond_signal(&event->cond);
        } else {
            error_value = pthread_cond_broadcast(&event->cond);
        }
        if (error_value != 0) {
            error_code = vmiop_error_inval;
        }
    }

    vmiope_leave_lock(in_monitor,
                      &event->u.mutex);

    return(error_code);
}

vmiop_error_t
vmiop_cv_alloc(vmiop_handle_t *handle_p)
{
    vmiop_handle_t handle;
    vmiop_error_t error_code;
    vmiope_thread_event_t *new_event;
    vmiop_bool_t in_monitor;

    error_code = vmiope_enter_lock(&in_monitor,
                                   &thread_lock);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    error_code = vmiope_find_free_thread_event_handle(&handle,
                                                      &new_event,
                                                      vmiope_tetype_event);

    vmiope_leave_lock(in_monitor,
                      &thread_lock);

    if (error_code == vmiop_success) {
        *handle_p = handle;
    }
    return(error_code);
}

vmiop_error_t
vmiop_cv_free(vmiop_handle_t handle)
{
    vmiop_error_t error_code;
    vmiope_thread_event_t *new_event;
    vmiop_bool_t in_monitor;

    error_code = vmiope_enter_lock(&in_monitor,
                                   &thread_lock);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    new_event = vmiope_handle_to_thread_event(handle,
                                              vmiope_tetype_event);
    if (new_event == NULL) {
        error_code = vmiop_error_not_found;
    } else if (pthread_cond_destroy(&new_event->cond) != 0) {
        error_code = vmiop_error_inval;
    } else {
        new_event->type = vmiope_tetype_lock;
        error_code = vmiope_lock_free_nolock(handle);
    }

    vmiope_leave_lock(in_monitor,
                      &thread_lock);

    return(error_code);
}

vmiop_error_t
vmiop_cv_wait(vmiop_handle_t handle_lock,
              vmiop_handle_t handle_cv,
              vmiop_time_t time_value)
{
    vmiope_thread_event_t *event_lock;
    vmiope_thread_event_t *event_cv;
    int error_value;
    struct timespec ts;
    vmiop_error_t error_code = vmiop_success;

    event_cv = vmiope_handle_to_thread_event(handle_cv,
                                             vmiope_tetype_event);
    event_lock = vmiope_handle_to_thread_event(handle_lock,
                                               vmiope_tetype_lock);
    if (event_cv == NULL || event_lock == NULL) {
        return(vmiop_error_not_found);
    }

    if (time_value != VMIOP_TIME_NO_LIMIT) {
        vmiop_time_t tvs;

        tvs = time_value / VMIOPD_NS_PER_SEC;
        if (tvs > INT32_MAX) {
            tvs = INT32_MAX;
        }
        ts.tv_sec = tvs;
        ts.tv_nsec = (time_value % VMIOPD_NS_PER_SEC);
        error_value = pthread_cond_timedwait(&event_cv->cond,
                                             &event_lock->u.mutex,
                                             &ts);
        if (error_value == ETIMEDOUT) {
            error_code = vmiop_error_timeout;
            goto error_exit;
        }
    } else {
        error_value = pthread_cond_wait(&event_cv->cond,
                                        &event_lock->u.mutex);
    }

    if (error_value != 0) {
        error_code = vmiop_error_inval;
    }

 error_exit:
    return(error_code);
}

vmiop_error_t
vmiop_cv_signal(vmiop_handle_t handle)
{
    vmiope_thread_event_t *event;
    vmiop_error_t error_code;
    int error_value;

    event = vmiope_handle_to_thread_event(handle,
                                          vmiope_tetype_event);
    if (event == NULL) {
        return(vmiop_error_not_found);
    }

    error_value = pthread_cond_signal(&event->cond);

    if (error_value != 0) {
        error_code = vmiop_error_inval;
    }

    return(error_code);
}

vmiop_error_t
vmiop_cv_broadcast(vmiop_handle_t handle)
{
    vmiope_thread_event_t *event;
    vmiop_error_t error_code;
    int error_value;

    event = vmiope_handle_to_thread_event(handle,
                                          vmiope_tetype_event);
    if (event == NULL) {
        return(vmiop_error_not_found);
    }

    error_value = pthread_cond_broadcast(&event->cond);

    if (error_value != 0) {
        error_code = vmiop_error_inval;
    }

    return(error_code);
}

/*
 * Logging and error reporting
 */

/**
 * Table of names for log levels.
 */

static const char *vmiope_log_level_name[] = {
    [vmiop_log_fatal]  = "fatal",
    [vmiop_log_error]  = "error",
    [vmiop_log_notice] = "notice",
    [vmiop_log_status] = "status",
    [vmiop_log_debug]  = "debug",
};

/**
 * Table of syslog priorities for log levels.
 */

static const int vmiope_log_level_priority[] = {
    [vmiop_log_fatal]  = LOG_CRIT,
    [vmiop_log_error]  = LOG_ERR,
    [vmiop_log_notice] = LOG_NOTICE,
    [vmiop_log_status] = LOG_INFO,
    [vmiop_log_debug]  = LOG_DEBUG,
};


/**
 * Adds the message to the log stream. If the argument 1 is
 * vmiop_log_fatal, resets the domain execution and exits
 * without further action.
 *
 * @param[in] log_level     Severity level of message
 * @param[in] message_p     Message format string and arguments
 * @returns Error code:
 * -            vmiop_success:          Successful logging
 */

vmiop_error_t
vmiop_log(vmiop_log_level_t log_level,
          const char *message_p,
          ...)
{
    va_list ap;
    const char *log_name;
    int log_priority;
    vmiop_bool_t in_monitor;

    vmiope_enter_monitor(&in_monitor);

    if ((log_level > vmiop_log_debug) || (log_level < vmiop_log_fatal)) {
        log_level = vmiop_log_fatal;
    }
    log_name = vmiope_log_level_name[log_level];
    log_priority = vmiope_log_level_priority[log_level];

    va_start(ap, message_p);
    demu_vlog(log_priority, log_name, message_p, ap);
    va_end(ap);

    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }

    return(vmiop_success);
}

/**
 * Check length for type
 *
 * @param[in] attr_type         Type of attribute
 * @param[in] attr_value_length Length of variable
 * @returns Error code:
 * -            vmiop_success   Length acceptable
 * -            vmiop_error_inval Length invalid or
 *                              type invalid
 */

static vmiop_error_t
vmiope_check_type_and_length(vmiop_attribute_type_t attr_type,
                             vmiop_emul_length_t attr_value_length)
{
    switch (attr_type) {
    case vmiop_attribute_type_unsigned_integer:
        if (attr_value_length != sizeof(uint64_t)) {
            return(vmiop_error_inval);
        }
        break;
    case vmiop_attribute_type_integer:
        if (attr_value_length != sizeof(int64_t)) {
            return(vmiop_error_inval);
        }
        break;
    case vmiop_attribute_type_string:
        if (attr_value_length == 0) {
            return(vmiop_error_inval);
        }
        break;
    default:
        return(vmiop_error_inval);
    }
    return(vmiop_success);
}


/**
 * Convert an attribute value.
 *
 * The output variable is undefined on any error.  Only the
 * following attribute types are allowed:
 * -        vmiop_attribute_type_unsigned_integer
 * -        vmiop_attribute_type_integer
 * -        vmiop_attribute_type_string
 *
 * @param[in] attr_type     Type of input value
 * @param[in] attr_value_p  Reference to variable containing input value
 * @param[in] attr_value_length Input variable length
 * @param[in] new_attr_type Type of output value desired
 * @param[out] new_attr_value_p Reference to variable to receive the 
 *                          output value
 * @param[in] new_attr_value_length Output variable length
 * @returns Error code:
 * -            vmiop_success   Value converted
 * -            vmiop_error_inval NULL attr_value_p or new_attr_value_p,
 *                              or an unsupported attribute type,
 * -            vmiop_error_resource Integer overflow or output string
 *                              too long.
 */

vmiop_error_t
vmiop_convert_value(vmiop_attribute_type_t attr_type,
                    vmiop_value_t *attr_value_p,
                    vmiop_emul_length_t attr_value_length,
                    vmiop_attribute_type_t new_attr_type,
                    vmiop_value_t *new_attr_value_p,
                    vmiop_emul_length_t new_attr_value_length)
{
    vmiop_emul_length_t len;
    uint64_t uval;
    int64_t val;
    void *s;

    if ((vmiope_check_type_and_length(attr_type,
                                      attr_value_length) !=
         vmiop_success) ||
        (attr_value_p == NULL) ||
        (vmiope_check_type_and_length(new_attr_type,
                                      new_attr_value_length) !=
         vmiop_success) ||
        (new_attr_value_p == NULL)) {
        return(vmiop_error_inval);
    }

    switch(attr_type) {
    case vmiop_attribute_type_unsigned_integer:
        uval = attr_value_p->vmiop_value_unsigned_integer;
        switch (new_attr_type) {
        case vmiop_attribute_type_unsigned_integer:
            new_attr_value_p->vmiop_value_unsigned_integer = uval;
            break;
        case vmiop_attribute_type_integer:
            if (uval > INT64_MAX) {
                return(vmiop_error_resource);
            }
            new_attr_value_p->vmiop_value_integer = (int64_t) uval;
            break;
        case vmiop_attribute_type_string:
            if (snprintf(new_attr_value_p->vmiop_value_string,
                         new_attr_value_length,
                         "%llu",
                         (long long unsigned int) uval) >=
                new_attr_value_length) {
                return(vmiop_error_resource);
            }
        default:
            return(vmiop_error_inval);
            break;
        }
        break;

    case vmiop_attribute_type_integer:
        val = attr_value_p->vmiop_value_integer;
        switch (new_attr_type) {
        case vmiop_attribute_type_unsigned_integer:
            if (val < 0) {
                return(vmiop_error_resource);
            }
            new_attr_value_p->vmiop_value_unsigned_integer = (uint64_t) val;
            break;
        case vmiop_attribute_type_integer:
            new_attr_value_p->vmiop_value_integer = val;
            break;
        case vmiop_attribute_type_string:
            if (snprintf(new_attr_value_p->vmiop_value_string,
                         new_attr_value_length,
                         "%lld",
                         (long long int) val) >=
                new_attr_value_length) {
                return(vmiop_error_resource);
            }
        default:
            return(vmiop_error_inval);
            break;
        }
        break;

    case vmiop_attribute_type_string:
        if (new_attr_type == vmiop_attribute_type_string) {
            len = attr_value_length;
            if (new_attr_value_length < len) {
                len = new_attr_value_length;
            }
            (void) strncpy(new_attr_value_p->vmiop_value_string,
                           attr_value_p->vmiop_value_string,
                           len);
            if (new_attr_value_p->vmiop_value_string[len - 1] != 0) {
                return(vmiop_error_resource); /* string overflow */
            }
        } else {
            s = memchr(attr_value_p->vmiop_value_string,'\0',attr_value_length);
            if (s == NULL) {
                return(vmiop_error_inval); /* not nul terminated */
            }
            switch (new_attr_type) {
            case vmiop_attribute_type_unsigned_integer:
                if (sscanf(attr_value_p->vmiop_value_string,
                           "0x%llx",
                           (long long unsigned int *) &uval) != 1) {
                    if (sscanf(attr_value_p->vmiop_value_string,
                               "0%llo",
                               (long long unsigned int *) &uval) != 1) {
                        if (sscanf(attr_value_p->vmiop_value_string,
                                   "%llu",
                                   (long long unsigned int *) &uval) != 1) {
                            return(vmiop_error_inval);
                        }
                    }
                }
                new_attr_value_p->vmiop_value_unsigned_integer = uval;
                break;
            case vmiop_attribute_type_integer:
                if (sscanf(attr_value_p->vmiop_value_string,
                           "%lli",
                           (long long int *) &val) != 1) {
                    return(vmiop_error_inval);
                }
                new_attr_value_p->vmiop_value_integer = val;
                break;
            default:
                return(vmiop_error_inval);
            }
        }
        break;

    default:
        return(vmiop_error_inval);
    }
    return(vmiop_success);
}

/*@}*/

/**********************************************************************/
/**
 * @defgroup PluginManagementImpl Plugin Management
 */
/**********************************************************************/
/*@{*/

/*
 * Plugin object
 */

static char *vmiope_plugin_search_list = NULL;
/*!< Colon-separated list of paths to search for plugins */

/**
 * Plugin table entry reference
 */

typedef struct vmiope_entry_s *vmiope_entry_ref_t;

#define VMIOPE_PLUGIN_LINK_MAX (5)
/*!< Maximum number of upward or downward plugin links per plugin */

/**
 * Plugin link set
 */

typedef struct vmiope_link_s {
    uint32_t count;
    /*!< Count of plugin links */
    vmiope_entry_ref_t link[VMIOPE_PLUGIN_LINK_MAX];
    /*!< Array of links */
} vmiope_link_t;                             

/**
 * Plugin table entry
 */

typedef struct vmiope_entry_s {
    vmiop_list_header_t list_head;
    /*!< list pointers */
    vmiop_plugin_ref_t plugin;
    /*!< Reference to plugin object */
    vmiop_handle_t handle;
    /*!< Handle assigned to plugin */
    vmiope_link_t down;
    /*!< Set of downward links */
    vmiope_link_t up;
    /*!< Set of upward links */
    void *dl_handle;
    /*!< dlopen() handle */
    uint32_t config_index;
    /*!< index of this plugin specified in configuration file */
} vmiope_entry_t;

/**
 * @cond vmiopeForwardDeclarations
 */

static vmiop_error_t
vmiope_locate_plugin(vmiop_handle_t handle,
                     vmiope_entry_ref_t *plugin_p);

static vmiop_error_t
vmiope_find_matching_plugin(vmiope_link_t *link_set,
                            vmiop_plugin_class_set_t plugin_class_set,
                            vmiope_entry_ref_t *plugin_p);
static vmiop_error_t
vmiope_find_plugin_by_name(char *name,
                           vmiope_entry_ref_t *plugin_p);
/**
 * @endcond 
 */

/**
 * Plugin table
 */

static vmiop_list_header_ref_t plugin_table = NULL;

/**
 * Parameter block for list function for plugin list.
 */

typedef struct vmiope_entry_apply_s {
    vmiop_handle_t handle;
    /*!< handle to locate */
    vmiope_entry_ref_t plugin;
    /*!< receives reference to device object found */
    char *name;
    /*!< name for matching by name */
} vmiope_entry_apply_t;

/**
 * Check if plugin entry matches handle in list apply.
 *
 * @param[in] private_object Reference to 
 *                      vmiope_entry_apply_t object.
 *                      Reference to list member stored
 *                      in plugin on a match.
 * @param[in] link_p    Reference to list header in list
 *                      member
 * @returns vmiop_bool_t:
 *              vmiop_true: Terminate loop (item found)
 *              vmiop_false:  Continue loop (item not found)
 */

static vmiop_bool_t
vmiope_plugin_match(void *private_object,
                    vmiop_list_header_ref_t link_p)
{
    if (((vmiope_entry_apply_t *) private_object)->handle ==
        ((vmiope_entry_ref_t) link_p)->handle) {
        ((vmiope_entry_apply_t *) private_object)->plugin =
            ((vmiope_entry_ref_t) link_p);
        return(vmiop_true);
    }
    return(vmiop_false);
}

/**
 * Locates the plugin object for a handle 
 * 
 * @param[in] handle    Handle for plugin
 * @param[out] plugin_p Reference to variable to receive 
 *          pointer to plugin definition object.
 *          Variable is undefined on entry and set to NULL on error.
 * @returns Error code:
 * -            vmiop_success:          Object found
 * -            vmiop_error_inval:      NULL plugin_p or handle
 *                                      is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found:  handle does not reference
 *                                      a plugin
 */

static vmiop_error_t
vmiope_locate_plugin(vmiop_handle_t handle,
                     vmiope_entry_ref_t *plugin_p)
{
    vmiope_entry_apply_t appl;

    if (plugin_p == NULL) {
        return(vmiop_error_inval);
    }
    if (handle == VMIOP_HANDLE_NULL) {
        (*plugin_p) = NULL;
        return(vmiop_error_inval);
    }
    appl.handle = handle;
    appl.plugin = NULL;
    if (vmiope_list_apply(&plugin_table,
                          vmiope_plugin_match,
                          (void *) &appl)) {
        (*plugin_p) = appl.plugin;
        return(vmiop_success);
    }
    (*plugin_p) = NULL;
    return(vmiop_error_not_found);
}

/**
 * Check if plugin entry is next closer entry following
 * handle in list apply.
 *
 * @param[in] private_object Reference to 
 *                      vmiope_entry_apply_t object.
 *                      Reference to list member stored
 *                      in plugin on a match.
 * @param[in] link_p    Reference to list header in list
 *                      member
 * @returns vmiop_bool_t:
 *              vmiop_true: Terminate loop (item found)
 *              vmiop_false:  Continue loop (item not found)
 */

static vmiop_bool_t
vmiope_plugin_next(void *private_object,
                   vmiop_list_header_ref_t link_p)
{
    if (((vmiope_entry_apply_t *) private_object)->handle <
        ((vmiope_entry_ref_t) link_p)->handle &&
        (((vmiope_entry_apply_t *) private_object)->plugin == NULL ||
         (((vmiope_entry_apply_t *) private_object)->plugin->handle >
          ((vmiope_entry_ref_t) link_p)->handle))) {
        ((vmiope_entry_apply_t *) private_object)->plugin =
            ((vmiope_entry_ref_t) link_p);
    }
    return(vmiop_false);
}

/**
 * Find next plugin object in sequence.   This may be used to search the
 * set of plugins.
 *
 * @param[in,out] handle_p    Reference to handle variable.  Variable should be 
 *         set to VMIOP_HANDLE_NULL to start sequencing through the plugin 
 *         objects.  Variable is set to handle found, or VMIOP_HANDLE_NULL if no more.
 * @param[out] plugin_p       Reference to plugin object pointer variable.  
 *         Variable is undefined on input and set to point to plugin object, 
 *         or NULL if no more.
 * @returns Error code:
 * -            vmiop_success:          Next object found, or no more objects
 *                                      (*plugin_p is set to NULL)
 * -            vmiop_error_inval:      NULL handle_p or plugin_p
 */

vmiop_error_t
vmiop_find_plugin(vmiop_handle_t *handle_p,
                  vmiop_plugin_ref_t *plugin_p)
{
    vmiope_entry_apply_t appl;
    vmiop_bool_t in_monitor;

    if (plugin_p == NULL) {
        return(vmiop_error_inval);
    }

    vmiope_enter_monitor(&in_monitor);

    appl.handle = (*handle_p);
    appl.plugin = NULL;
    appl.name = NULL;
    (void) vmiope_list_apply(&plugin_table,
                             vmiope_plugin_next,
                             (void *) &appl);
    if (appl.plugin != NULL) {
        (*handle_p) = appl.plugin->handle;
        (*plugin_p) = appl.plugin->plugin;
        if (! in_monitor) {
            vmiope_leave_monitor(NULL);
        }
        return(vmiop_success);
    }

    (*handle_p) = VMIOP_HANDLE_NULL;
    (*plugin_p) = NULL;
    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }
    return(vmiop_error_not_found);
}


/**
 * Check if plugin entry matches name in list apply.
 *
 * @param[in] private_object Reference to 
 *                      vmiope_entry_apply_t object.
 *                      Reference to list member stored
 *                      in plugin on a match.
 * @param[in] link_p    Reference to list header in list
 *                      member
 * @returns vmiop_bool_t:
 *              vmiop_true: Terminate loop (item found)
 *              vmiop_false:  Continue loop (item not found)
 */

static vmiop_bool_t
vmiope_plugin_match_by_name(void *private_object,
                            vmiop_list_header_ref_t link_p)
{
    if (! strcmp(((vmiope_entry_apply_t *) private_object)->name,
                 ((vmiope_entry_ref_t) link_p)->plugin->name)) {
        ((vmiope_entry_apply_t *) private_object)->plugin = 
            ((vmiope_entry_ref_t) link_p);
        return(vmiop_true);
    }
    return(vmiop_false);
}

/**
 * Find plugin object by name   This may be used to search the
 * set of plugins.
 *
 * @param[in] name      Reference to string with name.
 * @param[out] plugin_p       Reference to plugin object pointer variable.  
 *         Variable is undefined on input and set to point to plugin object, 
 *         or NULL on error.
 * @returns Error code:
 * -            vmiop_success:          Object found.
 * -            vmiop_error_inval:      NULL handle_p or plugin_p
 * -            vmiop_error_not_found   No such object
 */

vmiop_error_t
vmiope_find_plugin_by_name(char *name,
                           vmiope_entry_ref_t *plugin_p)
{
    vmiope_entry_apply_t appl;
    vmiop_bool_t in_monitor;

    if (name == NULL ||
        plugin_p == NULL) {
        return(vmiop_error_inval);
    }

    vmiope_enter_monitor(&in_monitor);

    appl.handle = VMIOP_HANDLE_NULL;
    appl.plugin = NULL;
    appl.name = name;
    (void) vmiope_list_apply(&plugin_table,
                             vmiope_plugin_match_by_name,
                             (void *) &appl);
    if (appl.plugin != NULL) {
        (*plugin_p) = appl.plugin;
        if (! in_monitor) {
            vmiope_leave_monitor(NULL);
        }
        return(vmiop_success);
    }

    (*plugin_p) = NULL;
    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }
    return(vmiop_error_not_found);
}

/**
 * Open the library file dynamically.
 *
 * @param[in] file      Path to the library file
 *
 * @returns:  Handle to the library 
 */

static void*
vmiope_open_dynamic_library(char* file)
{
    void* handle_p = dlopen(file, RTLD_NOW | RTLD_LOCAL);

    if ((handle_p) == NULL) {
        vmiop_log(vmiop_log_error,
                "vmioplugin dlopen: %s\n",
                dlerror());
    }

    return (handle_p);
}

/**
 * Initialize the vmioplugin environment.
 *
 * This routine must be called before any other parts of vmiop-env
 * are used.
 * @returns Error code: 
 * -            vmiop_success:          Successful registration
 * -            vmiop_error_resource:   No memory or other resource unavailable
 */

vmiop_error_t
vmiope_initialize(void) 
{
    int retval;

    (void) pthread_mutex_init(&thread_lock,NULL);
    (void) pthread_mutex_init(&vmiope_monitor_lock,NULL);
    vmiope_enter_monitor(NULL);

    retval = demu_maximum_guest_pages();
    if (retval < 0) {
        return vmiop_error_range;
    }

    vmiope_guest_pfn_count = retval;
    vmiop_log(vmiop_log_notice, 
        "vmiop-env: guest_max_gpfn:0x%x", vmiope_guest_pfn_count);

    return(vmiop_success);
}


/**
 * Shutdown the vmioplugin environment.
 *
 * This routine will call all of the plugin shutdown routines, from top to bottom,
 * and then return.
 * @return Error code:
 * -            vmiop_success:          Successful unregistration
 * -            vmiop_error_timeout:    Some shutdown timed out
 * -            vmiop_error_resource:   Some shutdown failed due to lack
 *                                      of resources
 */

vmiop_error_t
vmiope_shutdown(void)
{
    vmiop_handle_t handle = VMIOP_HANDLE_NULL;
    vmiop_error_t error_code;
    vmiop_plugin_ref_t plugin;
    vmiop_error_t fault_error_code = vmiop_success;
    vmiop_bool_t in_monitor;

    while (vmiop_true) {
        error_code = vmiop_find_plugin(&handle,
                                       &plugin);
        if (error_code != vmiop_success) {
            break;
        }
        vmiope_leave_monitor(&in_monitor);
        error_code = plugin->shutdown(handle);
        if (in_monitor) {
            vmiope_enter_monitor(NULL);
        }
        if (fault_error_code == vmiop_success) {
            fault_error_code = error_code;
        }
    }

    /* release the VRAM mfn */
    if (vram_mfn[0] != 0) {
        (void) demu_release_guest_pages(vram_mfn,
                                        (VRAM_RESERVED_SIZE - TARGET_PAGE_SIZE)
                                        >> TARGET_PAGE_SHIFT);
    }

    return(vmiop_success);
}

static uint32_t vmiope_plugin_count=0;
/*!< Number of plugins registered  */

/**
 * Register a staticly linked plugin.
 *
 * This routine should be called once for each staticly linked plugin,
 * before calling vmiope_register_dynamic_plugins().
 *
 * @param[in] plugin Reference to vmiop_plugin_t structure for plugin.
 * @param[out] handle_p Reference to variable to receive handle for plugin.
 * @returns Error code:
 * -            vmiop_success:          Object found
 * -            vmiop_error_inval:      NULL plugin or handle_p or
 *                                      invalid plugin structure
 * -            vmiop_err_resource:     Memory or other resource not
 *                                      available
 */

vmiop_error_t
vmiope_register_plugin(vmiop_plugin_ref_t plugin,
                       vmiop_handle_t *handle_p)
{
    vmiope_entry_ref_t plugin_entry = NULL;
    vmiop_handle_t handle = VMIOP_HANDLE_NULL;
    vmiop_error_t error_code;
    vmiop_bool_t in_monitor;

    if (handle_p == NULL) {
        return(vmiop_error_inval);
    };
    if (plugin == NULL) {
        *handle_p = VMIOP_HANDLE_NULL;
        return(vmiop_error_inval);
    };

    vmiope_enter_monitor(&in_monitor);

    error_code = vmiop_memory_alloc_internal((vmiop_emul_length_t) sizeof(vmiope_entry_t),
                                             (void **) &plugin_entry,
                                             vmiop_true);
    if (error_code != vmiop_success) {
        goto error_exit;
    }
    error_code = vmiope_handle_alloc(&handle);
    if (error_code != vmiop_success) {
        goto error_exit;
    }

    plugin_entry->plugin = plugin;
    plugin_entry->handle = handle;
    vmiope_list_insert(&plugin_table,
                       &(plugin_entry->list_head));

    vmiope_leave_monitor(NULL);
    error_code = plugin->init_routine(handle);
    vmiope_enter_monitor(NULL);
    if (error_code != vmiop_success) {
        vmiope_list_remove(&plugin_table,
                           &(plugin_entry->list_head));
        goto error_exit;
    }

    *handle_p = handle;
    vmiope_plugin_count++;
    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }
    return(vmiop_success);

 error_exit:
    if (plugin_entry != NULL) {
        (void) vmiop_memory_free_internal((void *) plugin_entry,
                                          sizeof(vmiope_entry_t));
        plugin_entry = NULL;
    }
    if (handle != VMIOP_HANDLE_NULL) {
        (void) vmiope_handle_free(handle);
        handle = VMIOP_HANDLE_NULL;
    }
    *handle_p = handle;
    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }
    return(error_code);
}

/**
 * Lookup and open a plugin by name in the search path.
 *
 * This routine loops over the paths in the search path list,
 * looking for a loadable module matching the given name.
 *
 * @param[in] plugin_name   Pointer to string containing base
 *                          name of the plugin.
 * @param[out] handle_p     Pointer to variable to receive handle
 *                          for the opened plugin executable.
 *                          Set to NULL on an error.
 * @returns Error code:
 * -            vmiop_success           Success
 * -            vmiop_error_not_found   No such plugin
 * -            vmiop_error_inval       Access or other error
 * -            vmiop_error_range       Path too long
 */

static vmiop_error_t
vmiope_lookup_plugin(char *plugin_name,
                     void **handle_p)
{
    char    *cp = NULL;
    char    *ep = NULL;
    char    path_buf[PATH_MAX] = {0};
    int     cc = 0;
    int     plugin_len = 0;

    if (handle_p == NULL) {
        return(vmiop_error_inval);
    }

    *handle_p = NULL;
    if (plugin_name == NULL) {
        return(vmiop_error_inval);
    }

    cp = vmiope_plugin_search_list;
    plugin_len = strlen(plugin_name);

    /* If no plugin path is specified */
    if (cp == NULL) {
        /* Copy string if plugin_len + 4(.so and NULL) is within PATH_MAX */
        if ((plugin_len + 4)  <= PATH_MAX) {
            sprintf(path_buf, "%s.so", plugin_name);

            (*handle_p) = vmiope_open_dynamic_library(path_buf);
            if ((*handle_p) == NULL) {
                vmiop_log(vmiop_log_error, "Unable to locate the plugin");
                return(vmiop_error_inval);
            }
            return(vmiop_success);
        }
        return (vmiop_error_range);
    }

    while (*cp != 0) {
        ep = strchrnul(cp,':');
        cc = ep - cp;
        if (cc > 0) {
            /* PATH_MAX should include cc chars from the path + 4(/.so) + NULL */
            if ((cc + plugin_len + 5) <= PATH_MAX) {
                strncpy(path_buf, cp, cc);
                cc = snprintf(path_buf + cc,
                              PATH_MAX - cc,
                              "/%s.so",
                              plugin_name);
            
                (*handle_p) = vmiope_open_dynamic_library(path_buf);
                if ((*handle_p) == NULL) {
                    cp = ep + 1;
                    memset(path_buf, PATH_MAX, 0);
                    continue;
                }
                return(vmiop_success);
            }
            return(vmiop_error_range);
        }
        if (*ep == 0) {
            break;
        }
        cp = ep + 1;
    }

    return(vmiop_error_not_found);
}

vmiop_error_t
vmiope_enable_migration_support(vmiop_plugin_ref_t plugin, vmiop_handle_t handle,
                                vmiop_bool_t value);
vmiop_error_t
vmiope_check_migration_cap(vmiop_plugin_ref_t plugin, vmiop_handle_t handle,
                           vmiop_bool_t *value);

/**
 * Load and initialize a dynamic plugin.
 * 
 * This routine should be called, for each dynamic plugin,
 * after vmiope_register_plugin() has been called for each
 * of the staticly linked plugins.  This routine searches
 * for the plugin in the plugin search list, and loads the
 * first plugin with a matching name.
 *
 * @param[in] plugin_name   Pointer to string name of plugin.
 * @param[in] config_index  Index used for this plugin in config file.
 * @param[out] handle_p Reference to variable to receive
 *                      handle for plugin.
 * @returns Error code:
 * -            vmiop_success:          Object found
 * -            vmiop_error_inval:      Invalid configuration.
 * -            vmiop_err_resource:     Memory or other resource not
 *                                      available
 */

vmiop_error_t
vmiope_register_dynamic_plugin(char *plugin_name,
                               uint32_t config_index,
                               vmiop_handle_t *handle_p)
{
#define VMIOP_PLUGIN_DESCRIPTOR_NAME "vmiop_display_vmiop_plugin"
#define VMIOP_PLUGIN_ENV_DESCRIPTOR_NAME "vmiop_display_vmiop_env"
    void *dl_handle = NULL;
    vmiop_error_t error_code;
    vmiop_plugin_t *plugin;
    vmiop_plugin_env_t *env_plugin;
    vmiope_entry_ref_t plugin_entry = NULL;
    vmiop_handle_t handle = VMIOP_HANDLE_NULL;
    vmiop_bool_t in_monitor;
    vmiop_bool_t vgpu_supports_migration = vmiop_false;

    if (handle_p == NULL) {
        return(vmiop_error_inval);
    }
    *handle_p = VMIOP_HANDLE_NULL;

    vmiope_enter_monitor(&in_monitor);

    error_code = vmiope_lookup_plugin(plugin_name,
                                      &dl_handle);
    if (error_code != vmiop_success) {
        if (! in_monitor) {
            vmiope_leave_monitor(NULL);
        }
        return(error_code);
    }
    
    plugin = (vmiop_plugin_t *) dlvsym(dl_handle,
                                       VMIOP_PLUGIN_DESCRIPTOR_NAME,
                                       "VER_2_0");
    if (plugin == NULL) {
        plugin = (vmiop_plugin_t *) dlsym(dl_handle,
                                          VMIOP_PLUGIN_DESCRIPTOR_NAME);
    }

    if (plugin == NULL ||
        plugin->signature == NULL ||
        strcmp(plugin->signature,
               VMIOP_PLUGIN_SIGNATURE) ||
        plugin->name == NULL) {
        error_code = vmiop_error_not_found;
        vmiop_log(vmiop_log_error, "Failed to load the vGPU plugin");
        goto error_exit;
    }

    error_code = vmiop_memory_alloc_internal((vmiop_emul_length_t) sizeof(vmiope_entry_t),
                                             (void **) &plugin_entry,
                                             vmiop_true);
    if (error_code != vmiop_success) {
        goto error_exit;
    }
    error_code = vmiope_handle_alloc(&handle);
    if (error_code != vmiop_success) {
        goto error_exit;
    }

    plugin_entry->plugin = plugin;
    plugin_entry->handle = handle;
    plugin_entry->dl_handle = dl_handle;
    plugin_entry->config_index = config_index;
    vmiope_list_insert(&plugin_table,
                       &(plugin_entry->list_head));

    /**
     * Provide a direct pointer access to the plugin so as to be able
     * to be used without a lookup in the plugin_table.
     */
    vmiop_plugin = plugin;

    // Initialize the plugin env. stub
    env_plugin = (vmiop_plugin_env_t *) dlsym(dl_handle,
                                              VMIOP_PLUGIN_ENV_DESCRIPTOR_NAME);

    if (env_plugin != NULL) {
        env_plugin->control_msi = vmiop_control_interrupt_msi;
        env_plugin->restore_lfb = vmiop_restore_original_lfb;
        env_plugin->can_discard_presentation_surface_params = vmiop_true;
        env_plugin->unpin_pages = vmiop_unpin_guest_pages;
        vmiop_log(vmiop_log_notice, "Successfully update the env symbols!");
    }

    error_code = vmiope_check_migration_cap((vmiop_plugin_t *) plugin,
                                             VMIOP_HANDLE_NULL, &vgpu_supports_migration);

    if (vgpu_supports_migration == vmiop_true) {

        error_code = vmiope_enable_migration_support((vmiop_plugin_t *) plugin,
                                                      VMIOP_HANDLE_NULL, vmiop_true);
        if (error_code != vmiop_success) {
            vmiop_log(vmiop_log_notice,
                    "Fail to enable migration support:0x%x", error_code);
        }
    }

    vmiope_leave_monitor(NULL);
    error_code = plugin->init_routine(handle);
    vmiope_enter_monitor(NULL);
    if (error_code != vmiop_success) {
        vmiope_list_remove(&plugin_table,
                           &(plugin_entry->list_head));
        goto error_exit;
    }

    *handle_p = handle;
    vmiope_plugin_count++;
    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }

    return(vmiop_success);

 error_exit:
    if (plugin_entry != NULL) {
        (void) vmiop_memory_free_internal((void *) plugin_entry,
                                          sizeof(vmiope_entry_t));
        plugin_entry = NULL;
    }
    if (handle != VMIOP_HANDLE_NULL) {
        (void) vmiope_handle_free(handle);
        handle = VMIOP_HANDLE_NULL;
    }
    if (dl_handle != NULL) {
        (void) dlclose(dl_handle);
    }
    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }
    return(error_code);
}

/**
 * Find a plugin for a delivery class.
 *
 * @param[in] link_set          Reference to link set
 * @param[in] plugin_class_set  Set of plugin classes to match
 * @param[out] plugin_p         Reference to variable to receive
 *                              pointer to plugin entry.  Set to
 *                              NULL on error.
 * @returns Error code:
 * -            vmiop_success       Link successful
 * -            vmiop_error_inval   dest or plugin_p is NULL
 * -            vmiop_error_not_found No match
 */

static vmiop_error_t
vmiope_find_matching_plugin(vmiope_link_t *link_set,
                            vmiop_plugin_class_set_t plugin_class_set,
                            vmiope_entry_ref_t *plugin_p)
{
    int i;
    vmiope_entry_t *plugin_entry;

    if (plugin_p == NULL) {
        return(vmiop_error_inval);
    }
    *plugin_p = NULL;
    if (link_set == NULL) {
        return(vmiop_error_inval);
    }

    for (i = 0;
         i < link_set->count;
         i++) {
        plugin_entry = link_set->link[i];
        if (plugin_class_set &
            plugin_entry->plugin->input_classes) {
            *plugin_p = plugin_entry;
            return(vmiop_success);
        }
    }

    return(vmiop_error_not_found);
}

/**
 * Connect a single plugin to another.
 *
 * @param[in] dest  Reference to link set
 * @param[in] src   Reference to plugin entry to be connected.
 * @returns Error code:
 * -            vmiop_success       Link successful
 * -            vmiop_error_inval   dest or src is NULL
 * -            vmiop_error_resource Too many connections
 * -            vmiop_error_no_address_space Matching plugin
 *                          already connected.
 */

static vmiop_error_t
vmiope_connect_single_plugin(vmiope_link_t *dest,
                             vmiope_entry_ref_t src)
{
    vmiope_entry_ref_t plugin_entry;


    if (dest == NULL) {
        return(vmiop_error_inval);
    }

    if (dest->count >= VMIOPE_PLUGIN_LINK_MAX) {
        return(vmiop_error_resource);
    }

    if (vmiope_find_matching_plugin(dest,
                                    src->plugin->input_classes,
                                    &plugin_entry) ==
        vmiop_success) {
        return(vmiop_error_no_address_space);
    }

    dest->link[dest->count] = src;
    dest->count++;

    return(vmiop_success);
}

/**
 * Connect upper plugin to lower plugin.
 *
 * This routine should be called after all plugins are registered,
 * but before the emulation starts, to connect each upper plugin
 * to the next lower plugin, such as display to compression (in the remote
 * case) or display to presentation (in the local case).
 *
 * @param[in] upper_plugin  Handle for upper plugin 
 * @param[in] lower_plugin  Handle for lower plugin
 * @returns Error code:
 * -            vmiop_success       Successful connection
 * -            vmiop_error_inval   A handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found A handle does not refer to
 *                                  a register plugin.
 * -            vmiop_error_resource Memory or other resource not
 *                                  available.
 */

vmiop_error_t
vmiope_connect_plugins(vmiop_handle_t upper_plugin,
                       vmiop_handle_t lower_plugin)
{
    vmiop_error_t error_code;
    vmiope_entry_ref_t upper_entry;
    vmiope_entry_ref_t lower_entry;
    vmiop_bool_t in_monitor;

    vmiope_enter_monitor(&in_monitor);

    error_code = vmiope_locate_plugin(upper_plugin,&upper_entry);
    if (error_code != vmiop_success) {
        goto error_exit;
    }

    if (! upper_entry->plugin->connect_down_allowed) { 
        error_code = vmiop_error_inval;
        goto error_exit;
    }

    error_code = vmiope_locate_plugin(lower_plugin,&lower_entry);
    if (error_code != vmiop_success) {
        goto error_exit;
    }

    if (! lower_entry->plugin->connect_up_allowed) { 
        error_code = vmiop_error_inval;
        goto error_exit;
    }

    error_code = vmiope_connect_single_plugin(&upper_entry->down,
                                              lower_entry);
    if (error_code != vmiop_success) {
        goto error_exit;
    }

    error_code = vmiope_connect_single_plugin(&lower_entry->up,
                                              upper_entry);
 error_exit:
    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }
    return(error_code);
}

/**
 * Skip over whitespace.
 *
 * @param[in] cp        Pointer to string
 * @returns Pointer to string after skipping leading whitespace.
 */

static inline char *
vmiope_skip_space(char *cp)
{
    while (*cp != 0 &&
           isspace(*cp)) {
        cp++;
    }
    return(cp);
}

/**
 * Copy string to allocated buffer.
 *
 * @param[in] cp    Pointer to string to copy.
 * @returns Pointer to allocated string, or NULL if allocation
 *          failed.
 */

static char *
vmiope_copy_string(char *cp)
{
    char *np;

    np = (char *) malloc(strlen(cp) + 1);
    if (np != NULL) {
        strcpy(np,cp);
    }
    return(np);
}

/** 
 * Replace an allocated string with a copy of new
 * string.
 *
 * @param[in,out] dpp   Pointer to pointer to existing string.
 * @param[in] cp        Pointer to string to copy.
 * @returns Error code:
 * -            vmiop_success       Successful copy.
 * -            vmiop_error_resource No memory for copy
 */

static vmiop_error_t
vmiope_replace_string(char **dpp,
                      char *cp)
{
    char *np;

    np = vmiope_copy_string(cp);
    if (np == NULL) {
        return(vmiop_error_resource);
    }
    if (*dpp != NULL) {
        free((void *) (*dpp));
    }
    *dpp = np;
    return(vmiop_success);
}

/**
 * Set search path.
 *
 * Set the search path for dynamic plugins.
 *
 * @param[in] path_p    Pointer to string containing colon-separated list
 *                      of directory paths to search for plugins.
 * @returns Error code:
 * -            vmiop_success:      Normal completion
 * -            vmiop_error_inval:  Invalid parameter
 */

vmiop_error_t
vmiope_set_search_path(char *path_p)
{
    return(vmiope_replace_string(&vmiope_plugin_search_list,
                                 path_p));
}

/**
 *
 * @param[in]   plugin   Reference to vmiop_plugin_t structure for plugin.
 * @param[in]   handle   Handle for plugin
 * @param[out]  value    Set value to vmiop_true if the vnc console state is active
 *                       else set value to vmiop_false if the vnc console state
 *                       is inactive
 * @returns Error code:
 * -            vmiop_success:          vnc console state is set successfully
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_resource    No space in buffer
 */
vmiop_error_t
vmiope_set_vnc_console_state(vmiop_plugin_ref_t plugin, vmiop_handle_t handle,
                            vmiop_bool_t value)
{
    vmiop_error_t error_code;
    vmiop_value_t vnc_console_state;
    vnc_console_state.vmiop_value_unsigned_integer = value;

    error_code = plugin->set_attribute(handle,
                                       "vnc_console_state",
                                       vmiop_attribute_type_unsigned_integer,
                                       &vnc_console_state,
                                       0);
    return error_code;
}

vmiop_error_t
vmiope_check_migration_cap(vmiop_plugin_ref_t plugin, vmiop_handle_t handle,
                           vmiop_bool_t *value)
{
    vmiop_error_t error_code;
    vmiop_value_t vmiop_vgpu_cap;

    if (!value) return vmiop_error_inval;

    error_code = plugin->get_attribute(handle,
                                       VMIOP_ATTRIBUTE_VGPU_CAP,
                                       VMIOP_ATTRIBUTE_TYPE_VGPU_CAP,
                                       &vmiop_vgpu_cap,
                                       sizeof(vmiop_vgpu_cap));

    if (error_code == vmiop_success)
        *value = (vmiop_vgpu_cap.vmiop_value_unsigned_integer & VMIOP_ATTRIBUTE_VGPU_CAP_MIGRATION);

    return error_code;
}

/**
 *
 * @param[in]   plugin   Reference to vmiop_plugin_t structure for plugin.
 * @param[in]   handle   Handle for plugin
 * @param[out]  value    Set value to vmiop_true if migration is supported
 * @returns Error code:
 * -            vmiop_success:          migration support flag is set successfully
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_resource    No space in buffer
 */
vmiop_error_t
vmiope_enable_migration_support(vmiop_plugin_ref_t plugin, vmiop_handle_t handle,
                                vmiop_bool_t value)
{
    vmiop_error_t error_code;
    vmiop_value_t migration_feature;
    migration_feature.vmiop_value_unsigned_integer = value;

    error_code = plugin->set_attribute(handle,
                                       VMIOP_ATTRIBUTE_VMM_MIGRATION_SUPPORTED,
                                       VMIOP_ATTRIBUTE_TYPE_VMM_MIGRATION_SUPPORTED,
                                       &migration_feature,
                                       sizeof(migration_feature));
    return error_code;
}

/*@}*/

/**********************************************************************/
/**
 * @defgroup BufferManagementImpl Buffer Management
 */
/**********************************************************************/
/*@{*/

/*
 * Buffer object
 */

/**
 * Validate Plugin Class
 *
 * @param[in] plugin_class   Class to check
 * @returns Boolean:
 * -            vmiop_true   if valid
 * -            vmiop_false  if not valid
 */

static inline vmiop_bool_t
vmiope_validate_plugin_class(vmiop_plugin_class_t plugin_class)
{
    return(plugin_class >= vmiop_plugin_class_min &&
           plugin_class <= vmiop_plugin_class_max);
}

/**
 * Allocate a message buffer.
 *
 * This routine is built on top of vmiop_memory_alloc_internal(), and the resulting
 * object, which is a single memory allocation including the vmiop_buffer_t,
 * the vmiop_buffer_element_t array, and the specified amount data storage.
 * The implementation stores the total length of the allocation in the first
 * of two uint32_t items immediately following the vmiop_buffer_t and 
 * before the element array, which in turn is followed by the data area.
 * The second uint32_t is currently unused and set to zero, and is reserved
 * to the buffer allocator.   The first item in the element array is set to
 * point to the total data area allocated, if the element array has at least
 * one element.  No data area may be requested if the element array count is
 * zero.
 *
 * @param[out] buf_p        Reference to variable to receive pointer to buffer.
 *                          Set to NULL on an error.
 * @param[in] source_class  Value for buffer source_class.
 * @param[in] destination_class Value for buffer destination_class.
 * @param[in] element_count Count of elements required (1 or more)
 * @param[in] data_size     Size of data area required (may be zero)
 * @returns Error code:
 * -            vmiop_success       Buffer allocated
 * -            vmiop_error_inval   Invalid class or zero element_count
 * -            vmiop_error_resource Not enough memory
 */

vmiop_error_t
vmiop_buffer_alloc(vmiop_buffer_ref_t *buf_p,
                   vmiop_plugin_class_t source_class,
                   vmiop_plugin_class_t destination_class,
                   uint32_t element_count,
                   uint32_t data_size)
{
    vmiop_error_t error_code;
    vmiop_buffer_ref_t buf;
    vmiop_emul_length_t len;
    uint32_t *buf_count;

    if (buf_p == NULL) {
        return(vmiop_error_inval);
    }

    if (element_count == 0 ||
        ! vmiope_validate_plugin_class(source_class) ||
        ! vmiope_validate_plugin_class(destination_class)) {
        *buf_p = NULL;
        return(vmiop_error_inval);
    }

    len = vmiope_round_up(sizeof(vmiop_buffer_t) +
                          (2 * sizeof(uint32_t)) +
                          (element_count * sizeof(vmiop_buffer_element_t)) +
                          data_size,
                          sizeof(uint64_t));
    error_code = vmiop_memory_alloc_internal(len,
                                             (void **) &buf,
                                             vmiop_true);
    if (error_code != vmiop_success) {
        *buf_p = NULL;
        return(error_code);
    }

    (void) memset((void *) buf,
                  0,
                  len - data_size);
    buf->source_class = source_class;
    buf->destination_class = destination_class;
    buf->release_p = vmiop_buffer_free;
    buf->references = 1u;
    buf->count = element_count;
    buf_count = ((uint32_t *) (buf + 1));
    buf->element = (vmiop_buffer_element_t *) (buf_count + 2);
    buf_count[0] = len;
    buf->element[0].data_p = (void *) (buf->element + buf->count);
    buf->element[0].length = data_size;

    *buf_p = buf;
    return(vmiop_success);
}

/**
 * Free a message buffer allocated via vmiop_buffer_alloc().
 *
 * Decrements the reference count and, if it goes to zero,
 * frees the buffer.
 *
 * @param[in] buf_p         Buffer reference
 * @returns Error code:
 * -            vmiop_success:          Successful release
 * -            vmiop_error_inval:      NULL buf_p or not a buffer
 *                                      reference
 */

vmiop_error_t 
vmiop_buffer_free(vmiop_buffer_ref_t buf_p)
{
    uint32_t *buf_count;

    if (buf_p == NULL ||
        buf_p->count == 0) {
        return(vmiop_error_inval);
    }
    if (buf_p->references > 1) {
        buf_p->references--; /* XXX -- make atomic */
        return(vmiop_success);
    }
    buf_count = ((uint32_t *) (buf_p + 1));
    if (buf_count[0] < 
        (sizeof(vmiop_buffer_t) +
         (2 * sizeof(uint32_t)) +
         (buf_p->count * sizeof(vmiop_buffer_element_t)))) {
        return(vmiop_error_inval);
    }
    return(vmiop_memory_free_internal((void *) buf_p,
                                      buf_count[0]));
}

/**
 * Deliver message buffer to the appropropriate upstream or downstream plugin.
 * The caller must have a hold on the buffer across the call, and should
 * not release it (as in a separate thread) until the call returns.
 *
 * @param[in] handle        Plugin handle for caller
 * @param[in] buf_p         Reference to buffer
 * @param[in] direction     Direction (upstream or downstream)
 * @returns Error code:
 * -            vmiop_success:          Buffer delivered
 * -            vmiop_err_inval:        NULL buf_p
 * -            vmiop_err_not_found:    Caller is at bottom for downstream
 *                                      or top for upstream, or caller 
 *                                      handle does not match a plugin
 * -            vmiop_err_resource:     Memory or other resource not
 *                                      available
 */

vmiop_error_t
vmiop_deliver_message(vmiop_handle_t handle,
                      vmiop_buffer_ref_t buf_p,
                      vmiop_direction_t direction)
{
    vmiope_entry_ref_t src_entry;
    vmiope_entry_ref_t dest_entry;
    vmiop_error_t error_code;
    vmiop_bool_t in_monitor;

    vmiope_enter_monitor(&in_monitor);

    error_code = vmiope_locate_plugin(handle,
                                      &src_entry);
    if (error_code != vmiop_success) {
        if (! in_monitor) {
            vmiope_leave_monitor(NULL);
        }
        return(error_code);
    }

    error_code = vmiope_find_matching_plugin(((direction == vmiop_direction_up)
                                              ? &src_entry->up
                                              : &src_entry->down),
                                             vmiop_plugin_class_to_mask(buf_p->destination_class),
                                             &dest_entry);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    vmiope_leave_monitor(NULL);
    error_code = dest_entry->plugin->put_message(dest_entry->handle,
                                                 buf_p);
    if (in_monitor) {
        vmiope_enter_monitor(NULL);
    }
    return(error_code);
}
 
/**
 * Pull up message data into a buffer as needed.
 *
 * If the data in the buffer at the specified offset
 * is contigous, return a pointer to it.  Otherwise, 
 * copy the data to the temporary buffer and return
 * a pointer to that buffer.  The temporary buffer must
 * be large enough for the specified data length.
 *
 * @param[in] buf       Reference to the buffer
 * @param[in] data_offset Offset to data 
 * @param[in] data_length Length of data
 * @param[in] tbuf      Reference to temporary buffer
 * @param[out] data_p   Reference to variable to receive
 *                      data pointer.
 * @returns Error code:
 * -            vmiop_success   Data located
 * -            vmiop_error_inval NULL pointer or 
 *                          offset or length out of
 *                          range
 */

vmiop_error_t
vmiope_buffer_pullup(vmiop_buffer_ref_t buf,
                     uint32_t data_offset,
                     uint32_t data_length,
                     void *tbuf,
                     void **data_p)
{
    uint32_t ec;
    vmiop_buffer_element_t *ep;
    uint32_t len;
    uint8_t *dp;

    if (buf == NULL ||
        tbuf == NULL ||
        data_p == NULL) {
        return(vmiop_error_inval);
    }
    dp = NULL;
    ec = buf->count;
    ep = buf->element;
    while (ec > 0 &&
           data_length > 0) {
        if (data_offset < ep->length) {
            len = (ep->length - data_offset);
            if (data_length <= len) {
                if (dp == NULL) {
                    *data_p = ((void *) (((uint8_t *) (ep->data_p)) +
                                         data_offset));
                    return(vmiop_success);
                }
                len = data_length;
            }
            if (dp == NULL) {
                *data_p = tbuf;
                dp = tbuf;
            }
            memcpy(dp,
                   (void *) (((uint8_t *) (ep->data_p)) +
                             data_offset),
                   len);
            dp += len;
            data_offset -= len;
            data_length -= len;
        } else {
            data_offset -= ep->length;
        }
        ec--;
        ep++;
    }
    return(vmiop_success);
}

/**
 * Apply function to message data in a buffer.
 *
 * For each unit of data, apply the specified function.
 * If the unit data in the buffer is contiguous,
 * pass a pointer to it.  Otherwise, 
 * copy the data to the temporary buffer and pass
 * a pointer to that buffer.  The temporary buffer must
 * be large enough for the specified data length.
 * The data_length must be a multiple of the unit_length
 *
 * @param[in] buf       Reference to the buffer
 * @param[in] data_offset Offset to data 
 * @param[in] data_length Length of data
 * @param[in] unit_length Length of each unit of data
 * @param[in] tbuf      Reference to temporary buffer
 * @param[in] function_p Reference to function to apply
 * @param[in] opaque    Opaque pointer to pass to function
 * @returns Error code:
 * -            vmiop_success   Data located
 * -            vmiop_error_inval NULL pointer or 
 *                          offset or length out of
 *                          range or data_length not
 *                          a multiple of unit_length
 */

vmiop_error_t
vmiope_buffer_apply(vmiop_buffer_ref_t buf,
                   uint32_t data_offset,
                   uint32_t data_length,
                   uint32_t unit_length,
                   void *tbuf,
                   vmiope_buffer_apply_function_t function_p,
                   void *opaque)
{
    uint32_t ec;
    vmiop_buffer_element_t *ep;
    uint32_t len;
    uint8_t *data_p = NULL;
    uint8_t *dp;
    uint32_t uc;

    if (buf == NULL ||
        tbuf == NULL ||
        function_p == NULL ||
        unit_length == 0 ||
        (data_length % unit_length) != 0) {
        return(vmiop_error_inval);
    }
    dp = NULL;
    uc = 0;
    ec = buf->count;
    ep = buf->element;
    while (ec > 0 &&
           data_length > 0) {
        if (data_offset < ep->length) {
            len = (ep->length - data_offset);
            if (unit_length <= len &&
                dp == NULL) {
                data_p = (((uint8_t *) (ep->data_p)) +
                          data_offset);
                uc = unit_length;
                len = unit_length;
            } else {
                if (dp == NULL) {
                    data_p = tbuf;
                    dp = tbuf;
                    uc = 0;
                }
                if ((data_length - uc) < len) {
                    len = (data_length - uc);
                }
                memcpy(dp,
                       (void *) (((uint8_t *) (ep->data_p)) +
                                 data_offset),
                       len);
                uc += len;
            }
            if (uc == unit_length) {
                function_p(opaque,data_p);
                dp = NULL;
                uc = 0;
            }
            data_offset += len;
            data_length -= len;
        } else {
            data_offset -= ep->length;
            ec--;
            ep++;
        }
    }
    return(vmiop_success);
}

/**
 * Get token from input string.
 *
 * Gets the next token from the input string from a configuration file.
 * 
 * @param[in,out] cpp   Pointer to cursor variable, pointing into input string,
 *                      which must be nul-terminated and no more than
 *                      VMIOPE_CONF_MAX_LINE bytes long
 * @param[out] tk       Token buffer (VMIOPE_CONF_MAX_LINE + 1 bytes long)
 * @returns Error code:
 * -            vmiop_success   Token returned
 * -            vmiop_error_inval Syntex error
 * -            vmiop_error_not_found No token found
 */
 
static vmiop_error_t
vmiope_get_token(char **cpp,
                 char *tk)
{
    char *tp;
    char ec;
    char *cp;
    char tc;

    if (cpp == NULL ||
        *cpp == NULL ||
        tk == NULL) {
        return(vmiop_error_inval);
    }

    cp = vmiope_skip_space(*cpp);
    if (*cp == 0 ||
        *cp == '#') {
        *cpp = cp;
        return(vmiop_error_not_found);
    }

    tp = tk;
    ec = *cp;
    if (ec == '"' ||
        ec == '\'') {
        cp++;
    } else {
        ec = 0;
    }
    while (1) {
        tc = *cp;
        if (tc == 0 ||
            tc == '#' ||
            isspace(tc)) {
            if (ec != 0) {
                return(vmiop_error_inval);
            }
            break;
        }
        if (tc == ec) {
            cp++;
            break;
        }
        if (tc == '\\') {
            cp++;
            tc = *cp;
            if (tc == 0) {
                break;
            }
        }
        if (tc == '=') {
            cp++;
            break;
        }
        *tp++ = tc;
        cp++;
    }
    *tp = 0;
    *cpp = cp;

    return(vmiop_success);
}

/**
 * vmiop configuration element.
 */
typedef struct vmiope_config_element_s {
    vmiop_list_header_t list_head;    /*!< list header  */
    char key[VMIOPE_CONF_MAX_LINE];   /*!< config item key  */
    char value[VMIOPE_CONF_MAX_LINE]; /*!< value of config item */
} vmiope_config_element_t;

/**
 * Parameter block used for searching config elements
 */
typedef struct vmiope_config_apply_s {
    const char *key;                  /*!< key to match */
    vmiope_config_element_t *config_elem; /*!< config_elem that matches the key */
} vmiope_config_apply_t;

static vmiop_list_header_ref_t vmiope_config_list; 
/*!< list of all configuration items */

/* 
 * Internal configuration lookup functions
 */

/** 
 * Matching function for configuration item search
 *
 * @param[in] private_object  Reference to the apply parameter
 * @param[in] link_p          Reference to the linked list element
 * @returns vmiop_bool_t 
 * -        vmiop_true        apply parameter matches with current element
 * -        vmiop_false       apply parameter does not match.
 */
static vmiop_bool_t
vmiope_config_match(void *private_object,
                    vmiop_list_header_ref_t link_p)
{
    vmiope_config_apply_t *apply = (vmiope_config_apply_t *) private_object;
    vmiope_config_element_t *config_elem = (vmiope_config_element_t *) link_p;
    
    if (config_elem != NULL &&
        strncmp(apply->key, config_elem->key, VMIOPE_CONF_MAX_LINE) == 0) {
        apply->config_elem = config_elem;
        return(vmiop_true);
    }
    
    return(vmiop_false);
}

/** 
 * Get a configuration string value
 * 
 * @param[in] default value   Value to return if configuration key is not found
 * @param[in] key             Configuration item key
 * @returns char *            Value of the configuration item.
 */
static char*
vmiope_config_get_string(char *default_value,
                         const char *key)
{
    vmiope_config_apply_t apply;
    apply.key = key;

    if (vmiope_config_list == NULL) {
        return default_value;
    }
    
    if (vmiope_list_apply(&vmiope_config_list,
                          vmiope_config_match,
                          (void *)&apply)) {
        return(apply.config_elem->value);
    }

    return default_value;
}

/** 
 * Get a configuration value as a long
 * 
 * @param[in] default value   Value to return if configuration key is not found
 * @param[in] key             Configuration item key
 * @returns uint64_t          Value of the configuration item.
 */
static uint64_t 
vmiope_config_get_long(uint64_t default_value,
                       const char *key)
{
    vmiop_error_t error_code;
    vmiop_value_t long_value;
    const char *str_value = vmiope_config_get_string(NULL, key);
    
    if (str_value == NULL)
        return default_value;
    
    error_code = vmiop_convert_value(vmiop_attribute_type_string,
                                     (vmiop_value_t *)str_value,
                                     strlen(str_value) + 1,
                                     vmiop_attribute_type_unsigned_integer,
                                     &long_value,
                                     sizeof(long_value.vmiop_value_unsigned_integer));

    if (error_code == vmiop_success) {
        return long_value.vmiop_value_unsigned_integer;
    }

    return default_value;
}

/** 
 * Get pluginconfig element length
 * 
 * @param[in] pluginconfig          string of plugin configuration elements
 *  @returns  len                   length of the pluginconfig_element
 */
static int
vmiope_get_pluginconfig_element_length(const char *pluginconfig)
{
    int len = 0;
    char *pc;

    if (!pluginconfig)
        return 0;

    pc = vmiope_skip_space((char *)pluginconfig);

    while (*pc && (*pc != ',')) {
        len++;
        pc++;
    }

    return len;
}

/* Add pluginconfig_elem (config info - Key/value pair) to the table
 * 
 * @param[in] pluginconfig_elem         pluginconfig element to be added into the table
 *  @returns Error code:
 * -            vmiop_success           Normal completion
 * -            vmiop_error_inval       Invalid configuration
 * -            vmiop_error_not_found   File not found
 * -            vmiop_error_resource    Memory or other resource
 *                                      not available.
 */
static vmiop_error_t
vmiope_add_pluginconfig_element(char *pluginconfig_element)
{
    vmiop_error_t error_code = vmiop_success;
    char *pluginconfig;
    vmiope_config_element_t *config_elem = NULL;

    pluginconfig = pluginconfig_element;

    /* ignore commented items and empty lines */
    if ((*pluginconfig == '#') || (*pluginconfig == '\n')) {
        goto exit;
    }

    error_code = vmiop_memory_alloc_internal(sizeof(vmiope_config_element_t),
                                             (void **)&config_elem,
                                             vmiop_true);
    if (error_code != vmiop_success) {
        vmiop_log(vmiop_log_error, "Error allocating config list element");
        goto exit;
    }

    error_code = vmiope_get_token(&pluginconfig, config_elem->key);
    if (error_code != vmiop_success) {
        vmiop_log(vmiop_log_error, "Invalid configuration element key");
        goto exit;
    }

    error_code = vmiope_get_token(&pluginconfig, config_elem->value);
    if (error_code != vmiop_success) {
        vmiop_log(vmiop_log_error, "Invalid configuration element value");
        goto exit;
    }

    /* Check for duplicates in the config table */
    if(vmiope_config_get_string(NULL, config_elem->key)) {
        vmiop_log(vmiop_log_notice, "Skipping duplicate plugin config");
        goto exit;
    }

    vmiope_list_insert(&(vmiope_config_list), &(config_elem->list_head));

    config_elem = NULL;

exit:
    vmiop_memory_free_internal((void *)config_elem,
                               sizeof(vmiope_config_element_t));
    return(error_code);
}

/**
 * Read cmd line and config file and store config info
 * in a table. Config information will be
 * of the form
 *
 *   KEY<space>VALUE<\n>
 *
 * Must be called with monitor lock held.
 * @param[in] pluginconfig  plugin config configuration elements.
 * @returns Error code:
 * -            vmiop_success       Normal completion
 * -            vmiop_error_inval   Invalid configuration
 * -            vmiop_error_not_found File not found
 * -            vmiop_error_resource Memory or other resource
 *                                  not available.
 */
static vmiop_error_t
vmiope_read_configuration(const char *pluginconfig)
{
    FILE *f = NULL;
    vmiop_error_t error_code = vmiop_success;
    char filename[VMIOPE_CONF_MAX_LINE + 1];
    char pluginconfig_elem[VMIOPE_CONF_MAX_LINE + 1];
    int length;

    /* path of the conf file is always first argument to pluginconfig */
    length = vmiope_get_pluginconfig_element_length(pluginconfig);
    if (!length || (length > VMIOPE_CONF_MAX_LINE)) {
        error_code = vmiop_error_inval;
        vmiop_log(vmiop_log_error, "Invalid pluginconfig argument");
        goto exit;
    }

    snprintf(filename, (length + 1), "%s", pluginconfig);

    pluginconfig = pluginconfig + length;
    if (*pluginconfig == ',')
        pluginconfig++;

    /* Read Configuration passed from the cmd line. */
    length = vmiope_get_pluginconfig_element_length(pluginconfig);

    while (length) {
        if (length > (VMIOPE_CONF_MAX_LINE - strlen("plugin0."))) {
            vmiop_log(vmiop_log_error,
                      "Config element length greater than max length");
            goto exit;
        }
        snprintf(pluginconfig_elem, (length + strlen("plugin0.") + 1), "plugin0.%s", pluginconfig);

        error_code = vmiope_add_pluginconfig_element(pluginconfig_elem);
        if (error_code != vmiop_success) {
                vmiop_log(vmiop_log_error, "Invalid configuration option");
                goto exit;
        }

        pluginconfig = pluginconfig + length;
        if (*pluginconfig == ',')
            pluginconfig++;
        length = vmiope_get_pluginconfig_element_length(pluginconfig);
    }

    f = fopen(filename, "r");
    if (f == NULL) {
        vmiop_log(vmiop_log_error, "File not found");
        error_code = vmiop_error_not_found;
        goto exit;
    }
    /* Read configurations from the conf file */
    while (fgets(pluginconfig_elem, VMIOPE_CONF_MAX_LINE + 1, f) != NULL) {
        error_code = vmiope_add_pluginconfig_element(pluginconfig_elem);
        if (error_code != vmiop_success) {
            vmiop_log(vmiop_log_error, "Invalid configuration line");
            goto exit;
        }
    }

exit:
    if (f != NULL) {
        fclose(f);
    }
    return(error_code);
}

/**
 * Process VMIOPLUGIN configuration.
 *
 * Plugin Configuration elements:
 * -    debug                Set debug level (0-9)
 * -    numPlugins           Number of plugins specified in the config file
 * -    plugin_path           Value is colon-separated search path for plugins
 * -    plugin[n]            Value is basename of the Nth plugin to load
 * -    connect[k].[up,down] Plugins to connect. connect[k].up specifies the
 *                           upstream plugin and connect[k].down is the downstream
 *                           plugin. k <= n*(n-1)/2, where n is the total number of 
 *                           plugins registered with the environment.
 *
 * @param[in] pluginconfig       PLugin Configuration elements
 * @returns Error code:
 * -            vmiop_success       Normal completion
 * -            vmiop_error_inval   Invalid configuration
 * -            vmiop_error_not_found File not found
 * -            vmiop_error_resource Memory or other resource
 *                                  not available.
 */
vmiop_error_t
vmiope_process_configuration(const char *pluginconfig)
{
    vmiop_error_t error_code = vmiop_success;
    vmiop_handle_t handle;
    char *config_value = NULL;
    uint32_t i=0;
    uint32_t max_plugins, max_connections;
    vmiope_entry_ref_t plugin[2];
    const char *error_msg = "syntax";
    vmiop_bool_t in_monitor;
    char plugin_str[VMIOPE_CONF_MAX_LINE];
    char connect_str[VMIOPE_CONF_MAX_LINE];

    plugin_str[VMIOPE_CONF_MAX_LINE-1] = '\0';
    connect_str[VMIOPE_CONF_MAX_LINE-1] = '\0';

    vmiope_enter_monitor(&in_monitor);

    vmiop_log(vmiop_log_notice,
              "pluginconfig: %s",
              pluginconfig);

    error_code = vmiope_read_configuration(pluginconfig);
    if (error_code != vmiop_success) {
        goto common_exit;
    }

    max_plugins = vmiope_config_get_long(VMIOPE_DEFAULT_NUM_PLUGINS, "numPlugins");

    if (max_plugins > VMIOPE_MAX_PLUGINS) {
        error_msg = "numPlugins exceeds limit";
        goto common_exit;
    }

    /* load plugins */
    for (i = 0; i < max_plugins; ++i) {

        /* plugin search path option(s) */
        snprintf(plugin_str, VMIOPE_CONF_MAX_LINE-2, "plugin%d.plugin_path", i);
        config_value = vmiope_config_get_string(NULL, plugin_str);

        if (config_value != NULL) {
            error_code = vmiope_set_search_path(config_value);
            if (error_code != vmiop_success) {
                error_msg = "set search path";
                goto common_exit;
            }
            vmiop_log(vmiop_log_notice,
                      "Looking for plugin at path %s",
                      config_value);
        }

        /* Get plugin[i] */
        snprintf(plugin_str, VMIOPE_CONF_MAX_LINE-2, "plugin%d", i);
        config_value = vmiope_config_get_string(NULL, plugin_str);

        if (config_value == NULL) {
            vmiop_log(vmiop_log_error, "Plugin%d not specified", i);
            error_code = vmiop_error_not_found;
            goto common_exit;
        }

        vmiop_log(vmiop_log_notice, 
                  "Loading Plugin%d: %s",
                  i, config_value);

        error_code = vmiope_register_dynamic_plugin(config_value, i,
                                                    &handle);
        
        if (error_code != vmiop_success) {
            error_msg = "plugin registration";
            goto common_exit;
        }
    }

    /* All plugins loaded. Now look for connect option(s) 
     * There can be n(n-1)/2 unique edges in a n-node graph.
     */

    max_connections = (vmiope_plugin_count * (vmiope_plugin_count - 1))/2;

    for (i = 0; i < max_connections; ++i) {

        snprintf(connect_str, VMIOPE_CONF_MAX_LINE-2, "connect%d.up", i);
        config_value = vmiope_config_get_string(NULL, connect_str);
        
        if (config_value != NULL) {
            error_code = vmiope_find_plugin_by_name(config_value,
                                                    &plugin[0]);
            
            if (error_code != vmiop_success) {
                error_msg = "locate connect plugin";
                goto common_exit;
            }
            snprintf(connect_str, VMIOPE_CONF_MAX_LINE-2, "connect%d.down", i);
            config_value = vmiope_config_get_string(NULL, connect_str);
            
            if (config_value == NULL) {
                error_msg = "connect specifies only one plugin";
                goto error_exit;
            }
            
            error_code = vmiope_find_plugin_by_name(config_value,
                                                    &plugin[1]);
            if (error_code != vmiop_success) {
                error_msg = "locate connect plugin";
                goto common_exit;
            }
            
            error_code = vmiope_connect_plugins(plugin[0]->handle,
                                                plugin[1]->handle);
            if (error_code != vmiop_success) {
                error_msg = "connect plugin";
                goto common_exit;
            }
        }
    }
    error_msg = NULL;
    
 common_exit:
    if (error_msg != NULL) {
        vmiop_log(vmiop_log_error,
                  "vmiope_process_configuration: %s error",
                  error_msg);
    }
    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }
    return(error_code);

 error_exit:
    error_code = vmiop_error_inval;
    goto common_exit;
}

/**
 * Look up a configuration value for a given plugin. This retrieves a 
 * value from a read-only key value dictionary for per-plugin options.
 * The actual storage format for this dictionary is environment-specific.
 *
 * @param[in]   handle   Handle for the plugin whose configuration 
 *                       is being queried.
 * @param[in]   key      Name of the config option.
 * @param[out]  value_p  Pointer to a variable to receive a dynamically
 *                       allocated string containing the config value.
 *                       Value is undefined on entry, and on exit it will
 *                       always be either a valid string pointer or NULL.
 *                       If non-NULL, the caller must free this string
 *                       using vmiop_memory_free_internal().
 * @returns Error code:
 * -            vmiop_success:          Successful
 * -            vmiop_error_inval:      NULL key or value_p
 * -            vmiop_error_not_found:  Config option not defined
 */
vmiop_error_t
vmiop_config_get(vmiop_handle_t handle,
                 const char *key,
                 char **value_p)
{
    vmiop_error_t error_code;
    vmiope_entry_ref_t plugin_entry;
    char *env_config;
    char plugin_cfg_str[VMIOPE_CONF_MAX_LINE];

    plugin_cfg_str[VMIOPE_CONF_MAX_LINE-1] = '\0';

    if (handle == VMIOP_HANDLE_NULL || 
        value_p == NULL || key == NULL) {
        return vmiop_error_inval;
    }

    error_code = vmiope_locate_plugin(handle,
                                      &plugin_entry);
    
    if (error_code != vmiop_success) {
        return error_code;
    }

    snprintf(plugin_cfg_str, VMIOPE_CONF_MAX_LINE-2, "plugin%d.%s",
             plugin_entry->config_index, key);

    env_config = vmiope_config_get_string(NULL, plugin_cfg_str);
    
    if (env_config == NULL) {
        *value_p = NULL; 
        return vmiop_error_not_found;
    }
    
    error_code = vmiop_memory_alloc_internal(strlen(env_config) + 1, 
                                             (void **)value_p,
                                             vmiop_true);

    if (error_code != vmiop_success) {
        return error_code;
    }

    (void)strncpy(*value_p, env_config, 
                  (size_t)strlen(env_config));
    (*value_p)[(size_t)strlen(env_config)] = 0;

    return vmiop_success;
}

vmiop_error_t
vmiope_notify_device
(
    vmiop_handle_t handle,
    vmiop_migration_stage_e stage
)
{
    if (vmiop_plugin->version >= VMIOP_PLUGIN_VERSION_V2) {
        return (((vmiop_plugin_t_v2 *)vmiop_plugin)->notify_device(handle, stage));
    }

    return vmiop_error_not_found;
}

vmiop_error_t
vmiope_read_device_buffer
(
    vmiop_handle_t handle,
    void *buffer,
    uint64_t buffer_size,
    uint64_t *remaining_bytes,
    uint64_t *written_bytes
)
{

    if (vmiop_plugin->version >= VMIOP_PLUGIN_VERSION_V2) {
        return (((vmiop_plugin_t_v2 *)vmiop_plugin)->read_device_buffer(handle,
                                                      buffer, buffer_size,
                                                      remaining_bytes,
                                                      written_bytes));
    }

    return vmiop_error_not_found;
}

vmiop_error_t
vmiope_write_device_buffer
(
    vmiop_handle_t handle,
    void *buffer,
    uint64_t buffer_size
)
{
    if (vmiop_plugin->version >= VMIOP_PLUGIN_VERSION_V2) {
        return (((vmiop_plugin_t_v2 *)vmiop_plugin)->write_device_buffer(handle,
                                                      buffer, buffer_size));
    }

    return vmiop_error_not_found;
}

/*@}*/

/*
  ;; Local Variables: **
  ;; mode:c **
  ;; c-basic-offset:4 **
  ;; tab-width:4 **
  ;; indent-tabs-mode:nil **
  ;; End: **
*/
