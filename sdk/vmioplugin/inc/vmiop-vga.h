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
 * @file vmiop-vga.h
 *
 * @brief
 * Interface definitions for VGA emulation in the vmioplugin environment.
 */

/**
 * @page vmiop-vga interfaces
 *
 * The vmioplugin environment VGA module encapsulates basic VGA emulation,
 * for use by accelerated graphics display plugins.
 */

#ifndef _VMIOP_VGA_H
/**
 * Multiple-include tolerance
 */
#define _VMIOP_VGA_H

#include <vmioplugin.h>

/**********************************************************************/
/**
* @defgroup VmiopVgaDataStructures Data structures shared with client
*/
/**********************************************************************/
/*@{*/

/**
 * Display descriptor
 */
typedef struct vmiop_vga_descriptor_s {
    vmiop_display_configuration_t vdc; /*!< display configuration */
    uint8_t bpp;            /*!< bits per pixel     */
    uint8_t depth;          /*!< bytes per pixel    */
    uint8_t *data;          /*!< frame buffer       */
} vmiop_vga_descriptor_t;
    

/**
 * Callback from vmiop-vga for new frame or resizing
 *
 * @param[in] vds           reference to VGA display descriptor
 * @param[in] opaque        value passed through from client of vmiop-vga
 * @returns Error code:
 * -            vmiop_success       Successful completion
 * -            vmiop_error_inval   NULL vds
 * -            vmiop_error_resource Insufficient memory or
 *                                  other resource
 */

typedef vmiop_error_t 
(*vmiop_vga_callback_t)(vmiop_vga_descriptor_t *vds,
                        void *opaque);


/**
 * VGA display callback routines 
 */

typedef struct vmiop_vga_callback_table_s {
    vmiop_vga_callback_t display_frame;
    vmiop_vga_callback_t resize_frame;
} vmiop_vga_callback_table_t;

/*@}*/

/**********************************************************************/
/**
* @defgroup VmiopVgaInterfaces Routines to access VGA display
*/
/**********************************************************************/
/*@{*/

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
o * -            vmiop_error_not_found Unknown vnum
 */

extern vmiop_error_t
vmiop_vga_set_callback_table(uint32_t vnum,
                             vmiop_vga_callback_table_t *cbt,
                             void *opaque);

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

extern vmiop_error_t 
vmiop_vga_ioport_access(uint8_t vnum,
                        const vmiop_emul_op_t emul_op,
                        const vmiop_emul_addr_t data_offset,
                        const vmiop_emul_length_t data_width,
                        void *data_p);

/**
 * Update VGA visible frame buffer.
 *
 * @param[in] vnum      Display number (0 is lowest)
 * @returns Error code:
 * -            vmiop_success       Successful initialization
 * -            vmiop_error_not_found Unknown vnum
 */

extern vmiop_error_t
vmiop_vga_update_frame_buffer(uint32_t vnum);

/**
 * Set whether the guest system is now in VGA mode or not.
 *
 * @param[in] now_in_vga       Specifies if the guest is now in VGA mode
 */

extern void
vmiop_vga_set_VGA_state(vmiop_bool_t now_in_vga);

/**
 * Check whether the guest system is now in VGA mode or not.
 *
 * @returns the state:
 * -            vmiop_true:      the guest system is in VGA
 * -            vmiop_false:     the guest system is not in VGA
 */

extern vmiop_bool_t
vmiop_vga_in_VGA_state(void); 


/*@}*/

#endif /* _VMIOP_VGA_H */

/*
  ;; Local Variables: **
  ;; mode:c **
  ;; c-basic-offset:4 **
  ;; tab-width:4 **
  ;; indent-tabs-mode:nil **
  ;; End: **
*/
