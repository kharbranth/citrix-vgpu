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
 * @file vmioplugin.h
 *
 * @brief
 * Interface definitions for the vmioplugin API.
 */

/**
 * @mainpage vmioplugin
 *
 * The vmioplugin API defines interfaces for plugins for I/O emulation and
 * remote display support, for use in virtualization environments.
 * The interfaces are intended to be independent of specific operating
 * systems and virtualization environments, so that plugins written to
 * these interfaces will be portable across such environments.
 *
 * The interfaces are of two broad types, each including data structures
 * and entry points.   The first type represents how the emulation 
 * environment calls the plugin.  The second type represents facilities
 * which the emulation environment provides to plugins.  While a plugin
 * may use host operating system facilities directly, it will only be
 * portable across operating systems if it does not do so.
 *
 * The interfaces are designed to be extensible, to allow newer plugins
 * to be used in environments designed with older versions of the
 * interfaces, and to allow older plugins to be used with newer
 * environments, with perhaps some limitation on functionality.
 * This forward- and backward-compatibility allows release cycles
 * for the environment and the plugins to be decoupled.
 *
 * The definitions are grouped as follows:
 * - @ref CommonTypes
 * - @ref EmulationSupport
 * - @ref PluginInterfaces
 * - @ref BufferFormats
 */

#ifndef _VMIOPLUGIN_H_
/**
 * Multiple-include tolerance.
 */
#define _VMIOPLUGIN_H_

#include <stdint.h>
#include <stdarg.h>

/**********************************************************************/
/**
* @defgroup CommonTypes     Common type definitions.
*/
/**********************************************************************/
/*@{*/

/**
 * Default NULL pointer value.
 */
#ifndef NULL
#define NULL ((void *) 0)
#endif /* NULL */

#ifdef __cplusplus
extern "C" {
#endif

/** 
 *  Error codes for plugin interfaces.
 */

enum vmiop_error_e {
    vmiop_error_min = 0,

    vmiop_success = 0,              /*!< successful completion */
    vmiop_error_none = 0,           
    vmiop_error_inval = 1,          /*!< invalid parameters */
    vmiop_error_resource = 2,       /*!< resource unavailable */
    vmiop_error_range = 3,          /*!< offset or length range */
    vmiop_error_read_only = 4,      /*!< write to read-only location */
    vmiop_error_not_found = 5,      /*!< object not found */
    vmiop_error_no_address_space = 6, /*!< not enough address space */
    vmiop_error_timeout = 7,        /*!< wait time expired */
    vmiop_error_not_allowed_in_callback = 8, /*!< request not allowed */

    vmiop_error_max = 8             /* highest number */
};

/**
 *  Error codes for plugin interfaces.
 */

typedef enum vmiop_error_e vmiop_error_t;

/**
 * Address in the emulated address space (zero-extended in the high-order 
 * bits if the address space is smaller than the full type).
 */

typedef uint64_t vmiop_emul_addr_t;

/**
 * Reserved value (all ones) to indicate no address is supplied on a 
 * mapping request.
 */

#define VMIOP_EMUL_ADDR_NONE (~ ((vmiop_emul_addr_t) 0))

/**
 * Length of an emulated address space range or a local address
 * space range.
 */

typedef uint64_t vmiop_emul_length_t;

/**
 * Type of an emulated address space.
 */

enum vmiop_emul_space_e {
    vmiop_emul_space_config = 0, /*!< PCI configuration space */
    vmiop_emul_space_io = 1,     /*!< I/O register space */
    vmiop_emul_space_mmio = 2    /*!< Memory-mapped I/O space */
};

/**
 * Type of an emulated address space.
 */

typedef enum vmiop_emul_space_e vmiop_emul_space_t;

/**
 * Emulation operation.
 */

enum vmiop_emul_op_e {
    vmiop_emul_op_read = 0,     /*!< Read by virtual machine */
    vmiop_emul_op_write = 1     /*!< Write by virtual machine */
};

/**
 * Emulation operation.
 */

typedef enum vmiop_emul_op_e vmiop_emul_op_t;

/**
 * Cacheability of data returned to emulation environment.
 */

enum vmiop_emul_state_e {
    vmiop_emul_noncacheable = 0, /*!< not cacheable in emulator */
    vmiop_emul_cacheable = 1,    /*!< cacheable in emulator */
    vmiop_emul_trap = 2          /*!< signal address fault in VM */
};                            

/**
 * Cacheability of data returned to emulation environment.
 */

typedef enum vmiop_emul_state_e vmiop_emul_state_t;

/**
 * Guest ID type
 */
enum vmiop_guest_id_type_e {
    vmiop_guest_domain_id, /*!< 4 byte domain ID */
    vmiop_guest_uuid      /*!< 16 byte UUID */
};

/**
 * Guest ID type
 */

typedef enum vmiop_guest_id_type_e vmiop_guest_id_type_t;

/**
 * Reference to an emulation environment object.
 */

typedef uint32_t vmiop_handle_t;

/**
 * Reserved value to indicate a vmiop_handle_t which never refers
 * to an object.
 */

#define VMIOP_HANDLE_NULL ((vmiop_handle_t) 0)

/**
 * Boolean type.
 */

enum vmiop_bool_e {
    vmiop_false = 0,        /*!< Boolean false */
    vmiop_true = 1          /*!< Boolean true */
};

/**
 * Boolean type.
 */

typedef enum vmiop_bool_e vmiop_bool_t;

/**
 * Guest (emulated system) memory mapping access mode.
 */

enum vmiop_access_e {
    vmiop_access_none = 0,      /*!< No access by guest */
    vmiop_access_read_write = 1, /*!< Read/write by guest */
    vmiop_access_read_only = 2  /*!< Read-only by guest */
};

/**
 * Guest (emulated system) memory mapping access mode.
 */

typedef enum vmiop_access_e vmiop_access_t;

/**
 * Type of plugin attribute value.
 */

enum vmiop_attribute_type_e {
    vmiop_attribute_type_min = 0,

    vmiop_attribute_type_unsigned_integer = 0, /*!< unsigned long long (uint64_t) */
    vmiop_attribute_type_integer = 1,          /*!< long long (int64_t)     */
    vmiop_attribute_type_string = 2,           /*!< string in buffer        */
    vmiop_attribute_type_reference = 3,        /*!< void *                  */

    vmiop_attribute_type_max = 3
};

/**
 * Type of plugin attribute value.
 */

typedef enum vmiop_attribute_type_e vmiop_attribute_type_t;

/**
 * Value of an attribute.  Value is variable length if the type of the
 * attribute is vmiop_attribute_type_string.  For that type, the length
 * includes a terminating NUL character for the string.
 */

typedef union vmiop_value_u {
    uint64_t vmiop_value_unsigned_integer;
    /*!< Unsigned 64-bit integer */
    int64_t vmiop_value_integer;
    /*!< Signed 64-bit integer */
    char vmiop_value_string[0];
    /*!< Variable length string buffer */
    void *vmiop_value_reference;
    /*!< Pointer value */
} vmiop_value_t;

/**
 * vGPU Capbilities
 *
 */

#define VMIOP_ATTRIBUTE_TYPE_VGPU_CAP       vmiop_attribute_type_unsigned_integer

#define VMIOP_ATTRIBUTE_VGPU_CAP            "vmiop_vgpu_cap"
#define VMIOP_ATTRIBUTE_VGPU_CAP_MIGRATION  (1 << 0)


/**
 * Hypervisor migration support
 *
 */

#define VMIOP_ATTRIBUTE_TYPE_VMM_MIGRATION_SUPPORTED    vmiop_attribute_type_unsigned_integer
#define VMIOP_ATTRIBUTE_VMM_MIGRATION_SUPPORTED         "vmiop_vmm_migration_supported"

/**
 * Reference to an initializer function for a new thread.
 * A function of this type is passed to the thread allocation
 * routine, which arranges for it to be called as the main
 * function of the thread, with the handle of the new thread
 * as an argument.  The function is passed a private argument,
 * which was supplied to the thread allocation routine by its
 * caller.
 *
 * @param[in] handle        Handle of the new thread
 * @param[in] private_object Reference to private object 
 * @returns No value: thread exits on return.
 */

typedef void 
(*vmiop_thread_init_t)(vmiop_handle_t handle,
                       void *private_object);

/**
 * Time in nanoseconds, from an undefined base.   Base may be assumed
 * to be small enough that time will not wrap in the life of the 
 * system.
 */

typedef uint64_t vmiop_time_t; 

/**
 * Reserved value (all ones) for vmiop_time_t, indicating that no
 * limit is desired, when passed as a time limit argument.
 */

#define VMIOP_TIME_NO_LIMIT (~ ((vmiop_time_t) 0))

/**
 * Log severity level.
 */

enum vmiop_log_level_e {
    vmiop_log_min    = 0,  /*!< Min log level */

    vmiop_log_fatal  = 0,  /*!< fatal errors */
    vmiop_log_error  = 1,  /*!< non-fatal errors */
    vmiop_log_notice = 2,  /*!< normally occurring events */
    vmiop_log_status = 3,  /*!< normally recurring events */
    vmiop_log_debug  = 4,  /*!< debug messages */

    vmiop_log_max          /*!< Max log level */
};

/**
 * Log severity level.
 */

typedef enum vmiop_log_level_e vmiop_log_level_t;

/**
 * Debug message level (0 = none, 9 = maximum)
 */

extern uint32_t vmiop_option_debug;

/**
 * List header reference.
 */

typedef struct vmiop_list_header_s *vmiop_list_header_ref_t;

/**
 * List header.
 *
 * This structure is used at the start of other structures which the
 * environment keeps in lists.  Plugins should not access it.
 */

typedef struct vmiop_list_header_s {
    vmiop_list_header_ref_t next; /*!< next item in list */
    vmiop_list_header_ref_t prev; /*!< previous item in list */
} vmiop_list_header_t;    

/**
 * Data put callback routine.
 * 
 * This type defines a reference to a callback routine which may be called
 * to put (write) data, as when saving the state of a virtual machine.
 *
 * @param[in] private_object    Reference to private object passed with
 *                              the callback routine reference.
 * @param[out] buf_p            Reference to buffer containing data to 
 *                              be put.
 * @param[in] data_len          Length of data in buffer.
 * @returns Error code:
 * -            vmiop_success   Successful completion.
 * -            vmiop_error_resource Insufficient resources to complete put.
 */

typedef vmiop_error_t 
(*vmiop_put_data_t)(void *private_object,
                    void *buf_p,
                    uint32_t data_len);


/**
 * Data get callback routine.
 * 
 * This type defines a reference to a callback routine which may be called
 * to get (read) data, as when restoring the state of a virtual machine.
 *
 * @param[in] private_object    Reference to private object passed with
 *                              the callback routine reference.
 * @param[out] buf_p            Reference to buffer containing data to 
 *                              be put.
 * @param[in] buf_len           Length of buffer
 * @param[out] data_len_p       Reference to variable to recieve length of
 *                              data read.   A length of zero on a successful
 *                              return indicates end of data.
 * @returns Error code:
 * -            vmiop_success   Successful completion.
 * -            vmiop_error_resource Insufficient resources to complete get.
 */

typedef vmiop_error_t 
(*vmiop_get_data_t)(void *private_object,
                    void *buf_p,
                    uint32_t buf_len,
                    uint32_t *data_len_p);


/*@}*/

/**********************************************************************/
/**
 * @defgroup EmulationSupport   Emulation Environment Interfaces
 */
/**********************************************************************/
/*@{*/

/**
 * This type defines the callback function called by the emulation environment
 * when a PCI configuration space, ioport, or MMIO read or write is issued 
 * for the registered device.  
 *
 * @param[in] private_object Pointer private to the plugin, provided on registration and passed
 *         to the callback routine unchanged on every call.
 * @param[in] emul_op   Operation type (read or write)
 * @param[in] address_space Address space of operation
 * @param[in] data_offset Offset to the required data (from base of rewgistered block)
 * @param[in] data_width Width of the required data in bytes
 * @param[in,out] data_p Pointer to data to be written or to a buffer to receive the data to
 *         be read.   The content of the data buffer is left unchanged after
 *         a write.  It is undefined after a read which fails.
 * @param[in,out] cacheable_p Reference to a variable to receive an indication of whether the 
 *         caller may cache all of the returned data for all future calls,
 *         or if the reference should fault in the virtual machine.
 *         Data should be marked cacheable only if it will never change in
 *         the life of the registration.  If state is set to vmiop_state_trap
 *         after a read, the content of the data buffer is left unchanged.
 * @returns Error code:
 * -            vmiop_success:      successful read or write
 * -            vmiop_error_inval:   NULL data_p or cacheable_p
 * -            vmiop_error_range:   data_offset+data_length too large
 * -            vmiop_error_read_only: Write to read-only location
 * -            vmiop_error_resource:  No memory or other resource unavaiable
 */

typedef vmiop_error_t (*vmiop_emul_callback_t)(void *private_object,
                                               const vmiop_emul_op_t emul_op,
                                               const vmiop_emul_space_t address_space,
                                               const vmiop_emul_addr_t data_offset,
                                               const vmiop_emul_length_t data_width,
                                               void *data_p,
                                               vmiop_emul_state_t *cacheable_p);


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
extern vmiop_error_t
vmiop_register_emul_device(void *private_object,
                           const vmiop_emul_callback_t emul_callback,
                           const char *object_label,
                           vmiop_handle_t *handle_p);

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
vmiop_unregister_emul_device(const vmiop_handle_t handle);

/*
 * Access to guest virtual machine address space
 */

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
extern vmiop_error_t
vmiop_map_guest_memory(const vmiop_emul_addr_t range_base,
                       const vmiop_emul_length_t range_length,
                       void **local_address_p,
                       const vmiop_bool_t map_read_only,
                       vmiop_handle_t *handle_p);

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
                             vmiop_handle_t *handle_p);



/**
 * Unmap the prior mapping defined by the handle.
 *
 * @param[in] handle        Mapping to unmap
 * @returns Error code:
 * -            vmiop_success:          Successful unmapping
 * -            vmiop_error_inval:      NULL local_address_p or
 *                                      handle_p
 * -            vmiop_error_not_found:  Not a guest mapping
 */

extern vmiop_error_t
vmiop_unmap_guest_memory(const vmiop_handle_t handle);

/*
 * Modification of guest address space
 */

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
 *         
 * @param[out] region_handle_p handle for region
 * @returns Error code:
 * -            vmiop_success:          Successful allocation
 * -            vmiop_error_inval:      Zero range_length
 * -            vmiop_error_no_address_space:  Specified range not available
 *                                      in the guest.
 */

extern vmiop_error_t
vmiop_create_guest_region(const vmiop_emul_length_t range_length,
                          const vmiop_emul_callback_t emul_callback,
                          void *private_object,
                          const vmiop_emul_space_t address_space,
                          vmiop_handle_t *region_handle_p);
/**
 * Relocate a region of guest pseudo-physical address space.
 *
 * The region and all its mappings are hidden from the guest address space
 * if the range_base is VMIOP_EMUL_ADDR_NONE, or relocated in the guest 
 * address space.
 *
 * @param[in] region_handle handle for region
 * @param[in] range_base    Pseudo-physical address in the guest domain at
 *         which to start the mapping, or VMIOP_EMUL_ADDR_NONE.
 * @returns Error code:
 * -            vmiop_success:          Successful mapping
 * -            vmiop_error_inval:      Null region handle
 * -            vmiop_error_no_address_space:  Specified range not available
 *                                      in the guest.
 */

extern vmiop_error_t
vmiop_relocate_guest_region(vmiop_handle_t region_handle,
                            vmiop_emul_addr_t range_base);

/**
 * Update guest VESA linear frame buffer address.
 *
 * @param[in] region_handle handle for region
 * @param[in] range_base    address in the guest domain to be used as
 *         VESA linear frame buffer address.
 * @returns Error code:
 * -            vmiop_success:          Successful mapping
 * -            vmiop_error_not_found:  Null region handle
 */

extern vmiop_error_t
vmiop_update_guest_lfb(vmiop_handle_t region_handle,
                       vmiop_emul_addr_t range_base);

/*
 * Restore guest VRAM to its original address.
 * @params none
 * @returns nothing
 */

void
vmiop_restore_original_lfb(void);

/**
 * Release a region of guest pseudo-physical address space.
 *
 * The region and all its mappings are hidden from the guest address space
 * if the range_base is VMIOP_EMUL_ADDR_NONE, or relocated in the guest 
 * address space.
 *
 * @param[in] region_handle             handle for region
 * @returns Error code:
 * -            vmiop_success:          Successful delete
 * -            vmiop_error_inval:      Invalid region handle
 */

extern vmiop_error_t
vmiop_delete_guest_region(vmiop_handle_t region_handle);

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
 *         which to start the mapping.
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
                       void * host_virt_addr,
                       const vmiop_emul_length_t range_length,
                       vmiop_emul_addr_t range_base,
                       const vmiop_access_t access_mode);

/**
 * Gain superuser privileges. Some operations might require access
 * to resources granted only to a privileged user.
 */
extern void
vmiop_set_su(void);


/**
 * Drop superuser privileges obtained by vmiop_set_su.
 */
extern void
vmiop_drop_su(void);

#define VMIOP_PAGE_NUMBER_NULL (~ ((unsigned long) 0u))
/*!< value to indicate an unmapped physical page number */

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
                      uint32_t pfn_count);

/*
 * Interrupt control
 */

/**
 * Interrupt control mode
 */

enum vmiop_interrupt_mode_e {
    vmiop_intr_off = 0, /*!< turn interrupt off */
    vmiop_intr_on = 1   /*!< turn interrupt on */
};

typedef enum vmiop_interrupt_mode_e vmiop_interrupt_mode_t;
/*!< Interrupt control mode */

/**
 * Control interrupt
 *
 * @param[in] handle           Emulated device handle from vmiop_register_emul_device()
 *                               for type vmiop_emul_space_config.
 * @param[in] interrupt_line   PIC interrupt line# (0-3)
 * @param[in] mode             Interrupt mode (on, off, pulse)
 * @returns Error code:
 * -                vmiop_success   Successful completion
 * -                vmiop_erorr_inval Not a PCI configuration space handle
 *                                  or irq out of range
 * -                vmiop_error_not_found Handle not found
 */

vmiop_error_t
vmiop_control_interrupt(vmiop_handle_t handle,
                        uint32_t interrupt_line,
                        vmiop_interrupt_mode_t mode);

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
                            uint32_t msg_data);         // IN

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
 *                       using vmiop_memory_free().
 * @returns Error code:
 * -            vmiop_success:          Successful
 * -            vmiop_error_inval:      NULL key or value_p
 * -            vmiop_error_not_found:  Config option not defined
 */
vmiop_error_t
vmiop_config_get(vmiop_handle_t handle,
                 const char *key,
                 char **value_p);

/*
 * Local memory allocation
 */

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

extern vmiop_error_t
vmiop_memory_alloc_internal(const vmiop_emul_length_t alloc_length,
                            void **alloc_addr_p,
                            const vmiop_bool_t clear_memory);

/**
 * Free local memory
 *
 * @param[in] alloc_addr    Address to free
 * @param[in] alloc_length  Length of block to free
 * @returns Error code:
 * -            vmiop_success:          Successful free
 * -            vmiop_error_inval:      Not an allocated block
 */

extern vmiop_error_t
vmiop_memory_free_internal(void *alloc_addr,
                           const vmiop_emul_length_t alloc_length); 

/**
 * Get emulated system page size.
 *
 * @param[out] page_size_p Size of a page on the emulated system.
 * @returns Error code:
 * -            vmiop_success:          Successful allocation
 * -            vmiop_error_inval:      NULL page_size_p
 */

extern vmiop_error_t
vmiop_get_page_size(vmiop_emul_length_t *page_size_p);

/**
 * Get a unique identifier for this guest.
 *
 * @param[out] guest_id_p Unique ID of the guest
 * @returns Error code:
 * -            vmiop_success:          Successful
 * -            vmiop_error_inval:      NULL guest_id_p or unknown guest ID.
 */

extern vmiop_error_t
vmiop_get_guest_id(uint64_t *guest_id_p);

/*
 * Thread management
 */

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

extern vmiop_error_t
vmiop_thread_alloc(void *private_object,
                   vmiop_thread_init_t init_p,
                   vmiop_handle_t *handle_p);

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

extern vmiop_error_t
vmiop_thread_event_alloc(vmiop_handle_t *handle_p);

/**
 * To join a thread
 *
 * @param[in] handle        Handle for the thread to be joined
 * @returns Error code:
 * -            vmiop_success:          Successful join
 * -            vmiop_error_resource:   Thread handle unavailable
 * -            vmiop_error_not_found:  Thread could not be joined
 *                                      successfully
 */

extern vmiop_error_t
vmiop_thread_join(vmiop_handle_t handle);

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

extern vmiop_error_t
vmiop_thread_event_free(vmiop_handle_t handle);

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
 *                                      posted
 */

extern vmiop_error_t
vmiop_thread_event_wait(vmiop_handle_t handle,
                        vmiop_time_t time_value,
                        vmiop_bool_t clear_before_return);

/**
 * Get the current time (base not defined)
 *
 * @param[out] time_value_p Reference to variable to receive
 *                          the current time
 * @returns Error code:
 * -            vmiop_success:          Successful fetch of time
 * -            vmiop_error_inval:      NULL time_value_p
 */

extern vmiop_error_t
vmiop_thread_get_time(vmiop_time_t *time_value_p);

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

extern vmiop_error_t
vmiop_thread_event_post(vmiop_handle_t handle,
                        vmiop_bool_t wakeup_first);
                                       /* false for wakeup all */

/*
 * Synchronization
 */

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

extern vmiop_error_t
vmiop_lock_alloc(vmiop_handle_t *handle_p);

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

extern vmiop_error_t
vmiop_lock_free(vmiop_handle_t handle);


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

extern vmiop_error_t
vmiop_lock(vmiop_handle_t handle,
           vmiop_bool_t try_only);

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

extern vmiop_error_t
vmiop_unlock(vmiop_handle_t handle);

/*
 * Logging and error reporting
 */

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

extern vmiop_error_t
vmiop_log(vmiop_log_level_t log_level,
          const char *message_p,
          ...);

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

extern vmiop_error_t
vmiop_convert_value(vmiop_attribute_type_t attr_type,
                    vmiop_value_t *attr_value_p,
                    vmiop_emul_length_t attr_value_length,
                    vmiop_attribute_type_t new_attr_type,
                    vmiop_value_t *new_attr_value_p,
                    vmiop_emul_length_t new_attr_value_length);

/*@}*/

/**********************************************************************/
/**
 * @defgroup PluginInterfaces Plugin Interfaces
 */
/**********************************************************************/
/*@{*/

/**
 * A plugin has a class, which defines its role, such as display
 * emulation, network transport, or display presentation.
 */

enum vmiop_plugin_class_e {
    vmiop_plugin_class_min = 0,

    vmiop_plugin_class_null = 0,    /*!< no external function */
    vmiop_plugin_class_display = 1,  /*!< graphics device emulation */
    vmiop_plugin_class_presentation = 9, /*!< display presentation */

    vmiop_plugin_class_max = 9
};

/**
 * A plugin has a class, which defines its role, such as display
 * emulation, network transport, or display presentation.
 * Values must be in the enumeration vmiop_plugin_class_e.
 */

typedef uint32_t vmiop_plugin_class_t;

/**
 * Set of vmiop_plugin_class_t items.
 */

typedef uint32_t vmiop_plugin_class_set_t;

/**
 * Convert a constant vmiop_plugin_class_t value to a member of
 * a vmiop_plugin_class_set_t.
 */

#define vmiop_const_plugin_class_to_mask(y) \
    (((vmiop_plugin_class_set_t) 1ul) << (y))

/**
 * Convert a vmiop_plugin_class_t value to a member of
 * a vmiop_plugin_class_set_t.
 */

static inline vmiop_plugin_class_set_t
vmiop_plugin_class_to_mask(vmiop_plugin_class_t y) 
{ 
    return(((vmiop_plugin_class_set_t) 1ul) << (y));
}

/**
 * Test if a vmiop_plugin_class_t value is a member of
 * a vmiop_plugin_class_set_t.
 */

static inline int
vmiop_plugin_class_in_set(vmiop_plugin_class_set_t x,
                          vmiop_plugin_class_t y)
{
    return(((x) & vmiop_plugin_class_to_mask(y)) != 0);
}

/**
 * Reference to a shared buffer object.
 */

typedef struct vmiop_buffer_s *vmiop_buffer_ref_t;

/**
 * Release a reference to a shared buffer object.  If this was
 * the last reference, release the object.
 *
 * @param[in] buf_p         Buffer reference
 * @returns Error code:
 * -            vmiop_success:          Successful release
 * -            vmiop_error_inval:      NULL buf_p or not a buffer
 *                                      reference
 */

typedef vmiop_error_t 
(*vmiop_buffer_release_t)(vmiop_buffer_ref_t buf_p);

/*
 * Buffer object
 */

/**
 * The data elements of a buffer are defined by a variable
 * length array of this type.
 */

typedef struct vmiop_buffer_element_s {
    void *data_p; /*!< reference to data array */
    vmiop_emul_length_t length; /*!< length of array in bytes */
} vmiop_buffer_element_t;

/**
 * A buffer points to a list of buffer elements, and includes a hold count
 * and a release callback routine.
 */

typedef struct vmiop_buffer_s {
    vmiop_list_header_t list_head;
    /*!< Header for list of buffers */
    vmiop_plugin_class_t source_class; 
    /*!< Plugin class of source of buffer */
    vmiop_plugin_class_t destination_class;
    /*!< Plugin class of destination of buffer */
    vmiop_buffer_release_t release_p;
    /*!< Reference to function to release a hold on a buffer */ 
    uint32_t references;
    /*!< Count of references to the buffer */
    uint32_t count; 
    /*!< Number of elements in the array of data elements */
    vmiop_buffer_element_t *element;
    /*!< Reference to the array of data elements */
    vmiop_bool_t discard_config;
    /*!< Flag to indicate that the present config state is to be discarded */
} vmiop_buffer_t;

/**
 * Release a reference to a shared buffer object.
 *
 * @param[in] buf_p     Buffer reference
 * @returns Error code:
 * -            vmiop_success:          Successful release
 * -            vmiop_error_inval:      NULL buf_p or not a buffer
 *                                      reference
 */

static inline vmiop_error_t
vmiop_buffer_release(vmiop_buffer_ref_t buf_p)
{
    return(buf_p->release_p(buf_p));
}
        
/**
 * Direction of message delivery for vmiop_deliver_message.
 */

enum vmiop_direction_e {
    vmiop_direction_down = 0,   /*!< toward devices */
    vmiop_direction_up = 1      /*!< toward virtual machine */
};

/**
 * Direction of message delivery for vmiop_deliver_message.
 */

typedef enum vmiop_direction_e vmiop_direction_t;

/**
 * Stages during migration.
 */

typedef enum {

    vmiop_migration_none            = 0,    /*!< device normal running state    */

    vmiop_migration_pre_copy        = 1,    /*!< pre-copy, vCPU running         */

    vmiop_migration_stop_and_copy   = 2,    /*!< stop, vCPU paused              */

    vmiop_migration_resume          = 3,    /*!< start, vCPU paused             */

} vmiop_migration_stage_e;

/**
 * Allocate a message buffer.
 *
 * This routine is built on top of vmiop_memory_alloc(), and the resulting
 * object, which is a single memory allocation including the vmiop_buffer_t,
 * the vmiop_buffer_element_t array, and the specified amount data storage.
 * The implementation stores the total length of the allocation in the first
 * of two uint32_t items immediately following the vmiop_buffer_t and 
 * before the element array, which in turn is followed by the data area.
 * The second uint32_t is currently unused and set to zero, and is reserved
 * to the buffer allocator.   The first item in the element array is set to
 * point to the total data area allocated, if the element array has at least
 * one element.  No data area may be requested if the element array count is
 * zero.  The release_p pointer is set to vmiop_buffer_free, but may be
 * changed by the caller.
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

extern vmiop_error_t
vmiop_buffer_alloc(vmiop_buffer_ref_t *buf_p,
                   vmiop_plugin_class_t source_class,
                   vmiop_plugin_class_t destination_class,
                   uint32_t element_count,
                   uint32_t data_size);

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

extern vmiop_error_t 
vmiop_buffer_free(vmiop_buffer_ref_t buf_p);

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

extern vmiop_error_t
vmiop_deliver_message(vmiop_handle_t handle,
                      vmiop_buffer_ref_t buf_p,
                      vmiop_direction_t direction);
 
/*
 * Plugin object
 *
 * Object definition follows interface definitions.
 */

/**
 * Reference to a plugin object.
 */

typedef struct vmiop_plugin_s *vmiop_plugin_ref_t;

/*
***********************************************************************
*
* Facilities provided by plugins to the environment
*
***********************************************************************
*/

/**
 * Pointer to initialization function, called when plugin is loaded,
 * before domain is started.
 *
 * @param[in] handle        Handle for this plugin
 * @returns Error code:
 * -            vmiop_success:          Successful initialization
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_resource:   Resource allocation error
 * -            vmiop_error_no_address_space: Insufficient address space
 */

typedef vmiop_error_t 
(*vmiop_plugin_init_t)(vmiop_handle_t handle);

/**
 * Pointer to shutdown function, called when domain is shutting down
 * gracefully, after domain has stopped.
 *
 * @param[in] handle        Handle for this plugin
 * @returns Error code:
 * -            vmiop_success:          Successful termination
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_timeout:    Timeout waiting for
 *                                      threads to terminate
 */

typedef vmiop_error_t 
(*vmiop_plugin_shutdown_t)(vmiop_handle_t handle);


/**
 * Return a named attribute for the plugin from the referenced variable.
 *
 * @param[in] handle        Handle for this plugin
 * @param[in] attr_name     Attribute name
 * @param[in] attr_type     Value type
 * @param[out] attr_value_p  Reference to variable to receive value
 * @param[in] attr_value_length   Value variable length
 * @returns Error code:
 * -            vmiop_success:          Successful termination
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL,
 *                                      or attr_value_p is NULL,
 *                                      attr_type is unknown, or
 *                                      attr_type or attr_length is
 *                                      mismatched
 * -            vmiop_error_not_found   No such attribute
 * -            vmiop_error_resource    No space in buffer
 */

typedef vmiop_error_t 
(*vmiop_plugin_get_attribute_t)(vmiop_handle_t handle,
                                const char *attr_name,
                                vmiop_attribute_type_t attr_type,
                                vmiop_value_t *attr_value_p,
                                vmiop_emul_length_t attr_value_length);

/**
 * Set a named attribute for the plugin in the referenced variable.
 *
 * A plugin should accept a string value for an attribute of type
 * vmiop_attribute_type_unsigned_integer or vmiop_attribute_type_integer
 * and convert the value appropriately, using vmiop_convert_value().
 *
 * @param[in] handle        Handle for this plugin
 * @param[in] attr_name     Attribute name
 * @param[in] attr_type     Value type
 * @param[in] attr_value_p  Reference to variable containing value
 * @param[in] attr_value_length   Value variable length
 * @returns Error code:
 * -            vmiop_success:          Successful termination
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL,
 *                                      or attr_value_p is NULL,
 *                                      attr_type is unknown, or
 *                                      attr_type or attr_length is
 *                                      mismatched
 * -            vmiop_error_read_only   attribute may not be set
 * -            vmiop_error_not_found   No such attribute
 * -            vmiop_error_resource    No space in buffer
 */

typedef vmiop_error_t 
(*vmiop_plugin_set_attribute_t)(vmiop_handle_t handle,
                                const char *attr_name,
                                vmiop_attribute_type_t attr_type,
                                vmiop_value_t *attr_value_p,
                                vmiop_emul_length_t attr_value_length);

/**
 * Deliver a message buffer to a plugin.  The caller should have a hold
 * on the buffer ahead of the call, and not release the hold until after
 * the call returns, to allow for asynchronous release of the buffer by
 * all other holders.  The plugin may place its own hold on the buffer.
 *
 * @param[in] handle        Handle for plugin
 * @param[in] buf_p         Reference to buffer being delivered
 * @returns Error code:
 * -            vmiop_success:          No error
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL,
 *                                      or buf_p is NULL
 * -            vmiop_error_not_found:  handle does ot refer to a
 *                                      plugin
 */

typedef vmiop_error_t 
(*vmiop_plugin_put_message_t)(vmiop_handle_t handle,
                              vmiop_buffer_ref_t buf_p);
                                                    
/**
 * Save the state of the emulation for virtual machine suspend.
 *
 * @param[in] handle            Handle for plugin
 * @param[in] put_callback      Reference to routine to put data to storage.
 * @param[in] private_object    Reference to private object to be passed
 *                              to put_callback routine.
 * @param[out] total_length_p   Reference to variable to receive total
 *                              length of data put.  Initial value is undefined.
 * @returns Error code:
 * -                vmiop_success   Successful save.
 * -                vmiop_error_resource Insufficient resources to save.
 */

typedef vmiop_error_t
(*vmiop_plugin_save_state_t)(vmiop_handle_t handle,
                             vmiop_put_data_t put_callback,
                             void *private_object,
                             vmiop_emul_length_t *total_length_p);

/**
 * Restore the state of the emulation for virtual machine resume.
 *
 * This routine will be called after all plugins have been initialized, but
 * before the actual virtual machine resumes execution.
 *
 * @param[in] handle            Handle for plugin
 * @param[in] get_callback      Reference to routine to get data from
 *                              storage
 * @param[in] private_object    Reference to private object to be passed
 *                              to put_callback routine.
 * @param[out] total_length     Total length of data to restore.
 * @returns Error code:
 * -                vmiop_success   Successful save.
 * -                vmiop_error_resource Insufficient resources to save.
 */

typedef vmiop_error_t
(*vmiop_plugin_restore_state_t)(vmiop_handle_t handle,
                                vmiop_get_data_t get_callback,
                                void *private_object,
                                vmiop_emul_length_t total_length);

/**
 * Pointer to reset function, called when domain is resetting.
 *
 * @param[in] handle                Handle for this plugin
 * @returns Error code:
 * -            vmiop_success:      Successful reset
 * -            vmiop_error_inval:  handle is VMIOP_HANDLE_NULL
 */

typedef vmiop_error_t
(*vmiop_plugin_reset_t)(vmiop_handle_t handle);

/**
 * Migration stage notification (vmiop_plugin_t_v2)
 *
 * This function will be called by the hypervisor device model to notify the
 * start of each migration stage and the iteration counter of that stage
 * whenever it applies.
 *
 * @param[in] handle                       Handle for the device
 * @param[in] migration_stage              Notify stage
 *
 * @returns Error code:
 * -            vmiop_success              Successful completion
 * -            vmiop_error_inval          Invalid state
 */

typedef vmiop_error_t
(*vmiop_plugin_notify_device)(vmiop_handle_t handle,
                              vmiop_migration_stage_e stage);

/**
 * Read device buffer (vmiop_plugin_t_v2)
 *
 * This function will be called by the hypervisor device model, when the hypervisor 
 * is going to read device buffer for migration or creating check-point, and it can 
 * be called since the beginning of "pre-copy" phase till the end of "stop-and-copy" 
 * phase. Hypervisor should not start the device model termination request until the 
 * "remaining_bytes" are returned as zero.
 *
 * @param[in]  i    handle          Handle for the device
 * @param[in,out]   buffer          The input buffer vGPU plugin needs to fill up
 * @param[in]       buffer_size     Input buffer size in bytes
 * @param[out]      remaining_bytes Remaining data size in bytes
 * @param[out]      written_bytes   Written data size in bytes
 * 
 * @returns Error code:
 * -            vmiop_success              Successful completion
 * -            vmiop_error_resource       Unable to retrieve resource
 * -            vmiop_error_inval          Invalid state
 */

typedef vmiop_error_t
(*vmiop_plugin_read_device_buffer)(vmiop_handle_t handle,
                                   void *buffer, 
                                   uint64_t buffer_size, 
                                   uint64_t *remaining_bytes, 
                                   uint64_t *written_bytes);
/**
 * Write device buffer
 *
 * This function will be called by the hypervisor device model, when the hypervisor 
 * is going to write device buffer for the migrated vGPU device / VM. It will be called 
 * since the initialization of the migrated vGPU device model. Hypervisor can start such
 * writes even the vGPU device model initialization is not fully completed, although the 
 * write request will be blocked until device model is ready to process the incoming data.
 * 
 * @param[in]   handle          Handle for the device
 * @param[in]   buffer          The input buffer vGPU plugin needs to read from
 * @param[in]   buffer_size     Input buffer size in bytes
 *
 * @returns Error code:
 * -            vmiop_success              Successful completion
 * -            vmiop_error_resource       Unable to retrieve resource
 * -            vmiop_error_inval          Invalid state
 */

typedef vmiop_error_t
(*vmiop_plugin_write_device_buffer)(vmiop_handle_t handle,
                                    void *buffer, 
                                    uint64_t buffer_size);


/**
 * Pointers to the env. layer code
 */

/**
 * Pointer to msi injection function.
 *
 * @param[in] handle                Handle for this plugin
 * @param[in] msg_addr              MSI address allocated by the guest OS
 * @param[in] msg_data              MSI data assigned by the guest OS
 *
 * @return Error code:
 *
 */

typedef vmiop_error_t
(*vmiop_env_control_interrupt_msi)(vmiop_handle_t handle,
                                   vmiop_emul_addr_t msg_addr,
                                   uint32_t msg_data);

/*
 * Pointer to initial guest VRAM address function.
 *
 * @returns VRAM address
 *
 */

typedef void
(*vmiop_env_restore_original_lfb)(void);

/**
 * Pointer to unpin a set of guest pfn.
 * 
 * @returns Error code:
 *   vmiop_success              Successful completion
 *   vmiop_error_resource       Unable to allocate or lock memory    
 *   vmiop_error_inval          invalid page numbers
 *   
 */

typedef vmiop_error_t
(*vmiop_env_unpin_guest_pages)(unsigned long *gpfn_list,
                               unsigned long *hpfn_list,
                               uint32_t pfn_count);

/**
 * Pointer to provide vGPU plugin's handles info
 */

typedef void
(*vmiop_env_guest_handle)(void *handle_info);

/**
 * Pointer to type of guest ID
 * @return guest ID type:
 *   vmiop_guest_domain_id      4 byte domain ID
 *   vmiop_guest_uuid           16 byte UUID
 */

typedef vmiop_guest_id_type_t
(*vmiop_env_guest_id_type)(void);

/**
 * Wait on a condition variable. Note that time_value is an 
 * absolute time, not the amount of time to wait. A time_value 
 * value in the past is the same as a time_value of 0.
 *
 * @param[in] handle_lock  Handle for the lock variable
 * @param[in] handle_cv    Handle for the cond variable
 * @param[in] time_value   Time to wait (VMIOP_TIME_NO_LIMIT
 *                         if no timeout, 0 to just test the
 *                         variable)
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
vmiop_cv_wait(vmiop_handle_t handle_lock,
              vmiop_handle_t handle_cv,
              vmiop_time_t time_value) __attribute__((weak));

/**
 * Signal the conditional variable and wake the first thread waiting 
 * on the variable). 
 * 
 * @param[in] handle        Handle for conditional variable
 * @returns Error code:
 * -            vmiop_success:          Successful post
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found:  handle does to refer to
 *                                      an event variable
 */
vmiop_error_t
vmiop_cv_signal(vmiop_handle_t handle) __attribute__((weak));

/**
 * Broadcast the conditional variable and wake all the threads waiting
 * on the variable).
 *
 * @param[in] handle        Handle for conditional variable
 * @returns Error code:
 * -            vmiop_success:          Successful post
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found:  handle does to refer to
 *                                      an event variable
 */
vmiop_error_t
vmiop_cv_broadcast(vmiop_handle_t handle) __attribute__((weak));

/**
 * Allocate a control variable.
 *
 * @param[in] handle_p  Reference to variable to receive handle
 *                      for condition variable
 *
 * @returns Error code:
 * -            vmiop_success:          Successful initialization
 * -            vmiop_error_inval:      NULL handle_p
 * -            vmiop_error_resource:   Memory or other resource
 *                                      unavailable
 */
vmiop_error_t
vmiop_cv_alloc(vmiop_handle_t *handle_p) __attribute__((weak));

/**
 * Free a condition variable.
 *
 * @param[in] handle        Handle for the condition variable to free
 * @returns Error code:
 * -            vmiop_success:          Successful release
 * -            vmiop_error_inval:      Handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_not_found:  Handle does not reference
 *                                      an event variable
 */
vmiop_error_t
vmiop_cv_free(vmiop_handle_t handle) __attribute__((weak));

/**
 * Structure representing a contiguous list of pages in a compressed form.
 *
 * E.g. If we have the list of dirty pages gfns as "3,4,5,10,11,31,32,101,102,103",
 * each contiguous chunk of gfns will be denoted by this structure. So, the
 * complete list will result in the following array of this structure:
 * {[3,3], [10,2], [31,2], [101,3]}
 *
 */

struct pages {
    uint64_t count;
    uint64_t first_gfn;
};

/**
 * Mark the guest pfns dirty.
 *
 * @param[in] count         Number of items in the page_list array
 * @param[in] page_list     Array of struct pages representing the
 *                          list of dirty pages.
 * @returns Error code:
 *              vmiop_success           dirty pages set successfully
 *              vmiop_error_inval       generic error
 *              vmiop_error_not_found   invalid gfn
 */

vmiop_error_t
vmiop_set_guest_dirty_pages(uint64_t count, 
                            const struct pages * page_list) __attribute__((weak));

/**
 * Signature for a vmiop_plugin_t object.
 */

#define VMIOP_PLUGIN_SIGNATURE "VMIOP_PLUGIN_SIGNATURE"

/**
 * Suffix to be added to the plugin module's base name to create the
 * name of the plugin object.
 */

#define VMIOP_PLUGIN_SUFFIX "_vmiop_plugin"

#define VMIOP_PLUGIN_VERSION 0x00010000ul
/*!< Version 1.0.0 encoded in three 8-bit bytes, one per version element */

/**
 * Plugin definition object:
 *
 * The environment, after dynamically loading the plugin module,
 * looks up the plugin definition object by name, by concatenating to the
 * base name of the module (without file extension or extensions) the
 * string VMIOP_PLUGIN_SUFFIX.  It then calls the initialization routine.
 *
 * The vmiop_plugin_input_classes set defines the set of message classes
 * this plugin can accept as input.   For example, a compression plugin
 * can accept display and presentation messages as input.  
 * A link plugin can accept all messages.
 *
 */

typedef struct vmiop_plugin_s {
    uint32_t length;
    /*!< Length of plugin object.  Must be set to sizeof(vmiop_plugin_t). */
    uint32_t version;
    /*!< Version number.  Must be set to VMIOP_PLUGIN_VERSION. */
    char *signature;
    /*!< Pointer to VMIOP_PLUGIN_SIGNATURE string for verification */
    char *name;
    /*!< Pointer to string containing the name of the plugin */
    vmiop_plugin_class_t plugin_class;
    /*!< Class of the plugin */
    vmiop_plugin_class_set_t input_classes;
    /*!< Set of plugin classes from which this plugin will accept buffers */
    vmiop_bool_t connect_down_allowed;
    /*!< True if a plugin may be connected below this one */
    vmiop_bool_t connect_up_allowed;
    /*!< True if a plugin may be connected above this one */
    vmiop_plugin_init_t init_routine;
    /*!< Reference to initialization routine */
    vmiop_plugin_shutdown_t shutdown;
    /*!< Reference to shutdown routine */
    vmiop_plugin_get_attribute_t get_attribute;
    /*!< Reference to routine to get attributes */
    vmiop_plugin_set_attribute_t set_attribute;
    /*!< Reference to routine to get attributes */
    vmiop_plugin_put_message_t put_message;
    /*!< Reference to routine to accept buffers */
    vmiop_plugin_save_state_t save_state;
    /*!< Reference to routine to save state for suspend */
    vmiop_plugin_restore_state_t restore_state;
    /*!< Reference to routine to restore state for resume */
    vmiop_plugin_reset_t reset;
    /*!< Reference to reset routine */
} vmiop_plugin_t;

#define VMIOP_PLUGIN_VERSION_V2 0x00020000ul
/*!< Version 2.0.0 encoded in three 8-bit bytes, one per version element */

typedef struct vmiop_plugin_s_v2 {
    uint32_t length;
    /*!< Length of plugin object.  Must be set to sizeof(vmiop_plugin_t_v2). */
    uint32_t version;
    /*!< Version number.  Must be set to VMIOP_PLUGIN_VERSION. */
    char *signature;
    /*!< Pointer to VMIOP_PLUGIN_SIGNATURE string for verification */
    char *name;
    /*!< Pointer to string containing the name of the plugin */
    vmiop_plugin_class_t plugin_class;
    /*!< Class of the plugin */
    vmiop_plugin_class_set_t input_classes;
    /*!< Set of plugin classes from which this plugin will accept buffers */
    vmiop_bool_t connect_down_allowed;
    /*!< True if a plugin may be connected below this one */
    vmiop_bool_t connect_up_allowed;
    /*!< True if a plugin may be connected above this one */
    vmiop_plugin_init_t init_routine;
    /*!< Reference to initialization routine */
    vmiop_plugin_shutdown_t shutdown;
    /*!< Reference to shutdown routine */
    vmiop_plugin_get_attribute_t get_attribute;
    /*!< Reference to routine to get attributes */
    vmiop_plugin_set_attribute_t set_attribute;
    /*!< Reference to routine to get attributes */
    vmiop_plugin_put_message_t put_message;
    /*!< Reference to routine to accept buffers */
    vmiop_plugin_save_state_t save_state __attribute__((deprecated));
    /*!< Reference to routine to save state for suspend */
    vmiop_plugin_restore_state_t restore_state __attribute__((deprecated));
    /*!< Reference to routine to restore state for resume */
    vmiop_plugin_reset_t reset;
    /*!< Reference to reset routine */
    vmiop_plugin_notify_device notify_device;
    /*!< Reference to routine to info device checkpoint or migration stages */
    vmiop_plugin_read_device_buffer read_device_buffer;
    /*!< Reference to routine saving device state to hypervisor provided buffer */
    vmiop_plugin_write_device_buffer write_device_buffer;
    /*!< Reference to routine restoring device state from hypervisor provided buffer */
} vmiop_plugin_t_v2;

typedef struct vmiop_plugin_env {
    vmiop_env_control_interrupt_msi control_msi;
    vmiop_env_restore_original_lfb restore_lfb;
    vmiop_bool_t can_discard_presentation_surface_params;
    vmiop_env_unpin_guest_pages unpin_pages;
    vmiop_env_guest_id_type guest_id_type;
    vmiop_env_guest_handle set_guest_handle;
} vmiop_plugin_env_t;

/*@}*/

/**********************************************************************/
/**
* @defgroup BufferFormats   Message Buffer Formats.
*
* The definitions are grouped as follows:
* - @ref CommonBuffers
* - @ref DisplayBuffers
* - @ref PresentationBuffers
*
* Note: There will be additional buffer formats for other services.
*/
/**********************************************************************/

/**********************************************************************/
/**
* @defgroup CommonBuffers     Common definitions
*/
/**********************************************************************/
/*@{*/

/**
 * Common message header.
 *
 * The sequence number is incremented from 0 by 1 for each message
 * from a given source.
 */

typedef struct vmiop_message_common_s {
    uint32_t signature;         /*!< set to VMIOP_MC_SIGNATURE */
    uint32_t version;           /*!< set to VMIOP_MC_VERSION */
    uint32_t header_length;    
    /*!< total length, including class-specific header */
    vmiop_plugin_class_t message_class; /*!< original source class */
    uint32_t sequence;          /*!< sequence number */
    uint32_t pad;               /*!< unused pad (must be zero) */
} vmiop_message_common_t;

#define VMIOP_MC_SIGNATURE ((u_int32_t) 0x4f494d56u)
/*!< Message signature (to deduce endianness) */
#define VMIOP_MC_VERSION ((u_int32_t) 0x00010000)
/*!< Message version 1.0.0 */

/*@}*/

/**********************************************************************/
/**
* @defgroup DisplayBuffers     Messages from display plugin for presentation.
*/
/**********************************************************************/
/*@{*/

/**
 * Display message type
 */

enum vmiop_display_type_e {
    vmiop_dt_min = 0,                           /*!< lowest value in range */

    vmiop_dt_null = 0,                          /*!< null message (discard only) */
    vmiop_dt_frame = 1,                         /*!< frame to display */
    vmiop_dt_edid_request = 2,                  /*!< request for EDID from presentation */
    vmiop_dt_get_configuration = 3,             /*!< request to get configuration */
    vmiop_dt_set_configuration = 4,             /*!< request to set configuration */
    vmiop_dt_hdcp_request = 5,                  /*!< HDCP request message */
    vmiop_dt_get_memory_optimization_info = 6,  /*!< request to get memory optimization info */
    vmiop_dt_set_vnc_console_state = 7,         /*!< request to set vnc console state to active/inactve */

    vmiop_dt_max = 7                            /*!< highest value in range */
};

typedef uint32_t vmiop_display_type_t; /*!< type code for display message */

/**
 * Display message header.
 *
 * Field message_class in common header is set to vmiop_plugin_display.
 *
 * Header is followed by optional content.
 * - vmiop_dt_null:  no content
 * - vmiop_dt_frame: configuration record, followed by pixels in row-major order
 * - vmiop_dt_edid_request:  no content
 * - vmiop_dt_set_configuration:  configuration record
 * - vmiop_dt_hdcp_request:  HDCP request message
 */

typedef struct vmiop_message_display_s {
    vmiop_message_common_t mc;      /*!< common header */
    uint32_t type_code;             /*!< vmiop_display_type_t value */
    uint32_t content_length;        /*!< length of pixel data */
    uint32_t display_number;        /*!< ID of destination display */
} vmiop_message_display_t;          /*!< display message header */

#define VMIOP_DISPLAY_ALL ((uint32_t) (~0u))
/*!< reserved value for display number to indicate all displays */

/**
 * Pixel format type.
 */

enum vmiop_pixel_format_e {
    vmiop_pf_min = 0,           /*!< minimum value in range */

    vmiop_pf_inval = 0,         /*!< unset/invalid pixel format */
    vmiop_pf_8 = 1,             /*!< 256 colors via palette in 8 bits in 1 byte  */
    vmiop_pf_15 = 2,            /*!< X1R5G5B5 in 2 bytes */
    vmiop_pf_16 = 3,            /*!< R5G6B5 2 bytes */
    vmiop_pf_32 = 4,            /*!< A8R868B8 in 4 bytes */
    vmiop_pf_32_bgr = 5,        /*!< A8B8G8R8 in 4 bytes */

    vmiop_pf_max = 5            /*!< maximum value in range */
};

typedef uint32_t vmiop_pixel_format_t;
/*!< pixel format type */

/**
 * Page list type
 */

typedef struct vmiop_page_list_s {
    uint64_t          num_pte;
    unsigned long    *pte_array;
} vmiop_page_list_t;
/*!< page list type */

/**
 * Display frame configuration record
 */

typedef struct vmiop_display_configuration_s {
    uint32_t vnum;              /*!< VGA display number */
    uint32_t height;            /*!< height in pixels   */
    uint32_t width;             /*!< width in pixels    */
    vmiop_pixel_format_t ptype; /*!< pixel format       */
    uint32_t pitch;             /*!< pitch of surface   */
} vmiop_display_configuration_t;

/*@}*/

/**********************************************************************/
/**
* @defgroup PresentationBuffers Messages from presentation plugin to display plugin
*/
/**********************************************************************/
/*@{*/

/**
 * Presentation message type
 */

typedef enum vmiop_presentation_type_e {
    vmiop_pt_min = 0,           /*!< lowest value in range */

    vmiop_pt_null = 0,          /*!< null message (discard only) */
    vmiop_pt_edid_report = 1,   /*!< report EDID from presentation */

    vmiop_pt_max = 1            /*!< highest value in range */
} vmiop_presentation_type_t;    /*!< type code for display message */

/**
 * Presentation message header.
 *
 * Field message_class in common header is set to vmiop_plugin_presentation.
 *
 * Header is followed by optional content.
 * - vmiop_pt_null:  no content
 * - vmiop_pt_edid_report: EDID content
 */

typedef struct vmiop_message_presentation_s {
    vmiop_message_common_t mc;      /*!< common header */
    uint32_t type_code;             /*!< vmiop_presentation_type_t value */
    uint32_t content_length;        /*!< length of message content */
    uint32_t display_number;        /*!< ID of destination display */
} vmiop_message_presentation_t;     /*!< presentation message header */

/*@}*/

#ifdef __cplusplus
}
#endif

#endif /* _VMIOPLUGIN_H_ */

/*
  ;; Local Variables: **
  ;; mode:c **
  ;; c-basic-offset:4 **
  ;; tab-width:4 **
  ;; indent-tabs-mode:nil **
  ;; End: **
*/
