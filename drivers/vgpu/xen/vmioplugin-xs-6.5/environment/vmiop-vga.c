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
 * @file vmiop-vga.c
 *
 * @brief
 * Environmental functions for vmioplugin VGA emulation in qemu-dm.
 */

/**
 * @page vmiop-vga
 *
 * The vmioplugin environment VGA module encapsulates basic VGA emulation,
 * for use by accelerated graphics display plugins.
 *
 * The implementations are grouped as follows:
 * - @ref VgaDataStructureSupportImpl
 * - @ref VgaEmulationSupportImpl
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
#include <vmiop-vga-int.h>
#include <xenctrl.h>

#include <vga.h>
#include <demu.h>
#include <device.h>
#include <surface.h>

/**********************************************************************/
/**
* @defgroup VgaDataStructureSupportImpl Data Structure Support
*/
/**********************************************************************/
/*@{*/

/**
 * VGA emulation state for vmioplugin 
 */

typedef struct vmiope_display_state_s {
    vmiop_vga_descriptor_t vds; /*!< state passed to callbacks */
    vmiop_vga_callback_table_t *cbt; /*!< callbacks to display plugin */
    void *cbt_opaque;           /*!< value to pass back to callbacks */
} vmiope_display_state_t;

static vmiope_display_state_t *vmiope_dpy;

/*@}*/

/**********************************************************************/
/**
 * @defgroup VgaEmulationSupportImpl   Emulation Support 
 */
/**********************************************************************/
/*@{*/

/**
 * Initialize VGA emulation
 *
 * @returns Error code:
 * -            vmiop_success       Successful initialization
 * -            vmiop_error_resource Resources not avaiable
 */

vmiop_error_t
vmiop_vga_init(void)
{
    vmiop_error_t error_code;
    vmiope_display_state_t *dpy;
    int rc;

    error_code = vmiop_memory_alloc_internal(sizeof(vmiope_display_state_t),
                                             (void **) &dpy,
                                             vmiop_true);
    if (error_code != vmiop_success) {
        return(error_code);
    }

    rc = device_initialize();
    if (rc < 0) {
        error_code = vmiop_error_resource;
        vmiop_memory_free_internal(dpy, sizeof(vmiope_display_state_t));
        return(error_code);
    }

    vmiope_dpy = dpy;

    return(vmiop_success);
}

/**
 * Terminate VGA emulation
 *
 * @returns Error code:
 * -            vmiop_success       Successful completion
 * -            vmiop_error_resource Resource problem on shutdown
 */

vmiop_error_t
vmiop_vga_shutdown(void)
{
    device_teardown();
    return(vmiop_success);
}

/**
 * Set presentation callbacks.
 *
 * The callback routines are called when the VGA emulation has
 * a frame to display or needs to resize.
 *
 * @param[in] vnum      Display number (0 is lowest)
 * @param[in] cbt       Reference to callback table for presentation
 * @param[in] opaque    Pointer to pass through to callback
 * @returns Error code:
 * -            vmiop_success       Successful initialization
 * -            vmiop_error_inval   Invalid operand
 * -            vmiop_error_not_found Unknown vnum
 */

vmiop_error_t
vmiop_vga_set_callback_table(uint32_t vnum,
                             vmiop_vga_callback_table_t *cbt,
                             void *opaque)
{
    vmiop_bool_t in_monitor;

    if (cbt != NULL &&
        (cbt->display_frame == NULL ||
         cbt->resize_frame == NULL)) {
        return(vmiop_error_inval);
    }
    if (vnum != 0) {
        return(vmiop_error_not_found);
    }
    vmiope_enter_monitor(&in_monitor);
    vmiope_dpy->cbt = cbt;
    vmiope_dpy->cbt_opaque = opaque;

    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }
    return(vmiop_success);
}

/**
 * Perform an ioport read or write on behalf of the display plugin.
 *
 * This call is used when the plugin implements aliases for some of
 * VGA ioport registers.
 *
 * @param[in] vnum      Display number
 * @param[in] emul_op   Operation type (read or write)
 * @param[in] data_offset Offset to the required data (from base of rewgistered block)
 * @param[in] data_width Width of the required data in bytes (must be 1, 2, or 4)
 * @param[in,out] data_p Pointer to data to be written or to a buffer to receive the data to
 *         be read.   The content of the data buffer is left unchanged after
 *         a write.  It is undefined after a read which fails.
 * @returns Error code:
 * -            vmiop_success:      successful read or write
 * -            vmiop_error_inval:   NULL data_p or invalid width
 * -            vmiop_error_not_found:  Not a VGA register 
 * -            vmiop_error_read_only: Write to read-only location
 * -            vmiop_error_resource:  No memory or other resource unavaiable
 */

vmiop_error_t 
vmiop_vga_ioport_access(uint8_t vnum,
                        const vmiop_emul_op_t emul_op,
                        const vmiop_emul_addr_t data_offset,
                        const vmiop_emul_length_t data_width,
                        void *data_p)
{
    demu_space_t *space;
    uint64_t data;
    vmiop_error_t error_code = vmiop_success;
    vmiop_bool_t in_monitor;

    vmiope_enter_monitor(&in_monitor);

    space = demu_find_port_space(data_offset);

    switch (emul_op) {
    case vmiop_emul_op_read:
        data = demu_io_read(space, data_offset, data_width);
        switch (data_width) {
        case sizeof(uint8_t):
            *((uint8_t *) data_p) = data;
            break;
        case sizeof(uint16_t):
            *((uint16_t *) data_p) = data;
            break;
        case sizeof(uint32_t):
            *((uint32_t *) data_p) = data;
            break;
        default:
            error_code = vmiop_error_inval;
            break;
        }
        break;

    case vmiop_emul_op_write:
        switch (data_width) {
        case sizeof(uint8_t):
            data = *((uint8_t *) data_p);
            break;
        case sizeof(uint16_t):
            data = *((uint16_t *) data_p);
            break;
        case sizeof(uint32_t):
            data = *((uint32_t *) data_p);
            break;
        default:
             error_code = vmiop_error_inval;
            break;
        }
        if (error_code == vmiop_success)
            demu_io_write(space, data_offset, data_width, data);
        break;

    default:
        error_code = vmiop_error_inval;
        break;
    }

    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }
    return(error_code);
}

/**
 * Set whether the guest system is now in VGA mode or not.
 *
 * @param[in] now_in_vga       Specifies if the guest is now in VGA mode
 */

static vmiop_bool_t VGA_state = vmiop_true;

extern void
vmiop_vga_set_VGA_state(vmiop_bool_t now_in_vga) {
    if (now_in_vga == VGA_state)
        return;

    VGA_state = now_in_vga;
}

/**
 * Check whether the guest system is now in VGA mode or not.
 *
 * @returns the state:
 * -            vmiop_true:      the guest system is in VGA
 * -            vmiop_false:     the guest system is not in VGA
 */

extern vmiop_bool_t
vmiop_vga_in_VGA_state(void) {
    return VGA_state;
}

/**
 * Update VGA visible frame buffer.
 *
 * @param[in] vnum      Display number (0 is lowest)
 * @returns Error code:
 * -            vmiop_success       Successful initialization
 * -            vmiop_error_not_found Unknown vnum
 */

vmiop_error_t
vmiop_vga_update_frame_buffer(uint32_t vnum)
{
    if (vnum != 0) {
        return(vmiop_error_not_found);
    }

    return(vmiop_success);
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
