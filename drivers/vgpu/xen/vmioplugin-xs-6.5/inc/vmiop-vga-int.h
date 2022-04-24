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
 * @file vmiop-vga-int.h
 *
 * @brief
 * Internal interface definitions for VGA emulation in the vmioplugin environment,
 * to be used in the implmeentation, not in plugins.
 */

/**
 * @page vmiop-vga-int interfaces
 *
 * The vmioplugin environment VGA module encapsulates basic VGA emulation,
 * for use by accelerated graphics display plugins.
 */

#ifndef _VMIOP_VGA_INT_H
/**
 * Multiple-include tolerance
 */
#define _VMIOP_VGA_INT_H

#include <vmiop-env.h>
#include <vmiop-vga.h>

/**********************************************************************/
/**
* @defgroup VmiopVgaIntInterfaces Routines to access VGA display
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

extern vmiop_error_t
vmiop_vga_init(void);

/**
 * Terminate VGA emulation
 *
 * @returns Error code:
 * -            vmiop_success       Successful completion
 * -            vmiop_error_resource Resource problem on shutdown
 */

extern vmiop_error_t
vmiop_vga_shutdown(void);


/*@}*/

#endif /* _VMIOP_VGA_INT_H */

/*
  ;; Local Variables: **
  ;; mode:c **
  ;; c-basic-offset:4 **
  ;; tab-width:4 **
  ;; indent-tabs-mode:nil **
  ;; End: **
*/
