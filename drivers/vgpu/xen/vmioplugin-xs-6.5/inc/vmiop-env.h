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
 * @file vmiop-env.h
 *
 * @brief
 * Interface definitions for the vmioplugin environment module.
 */

/**
 * @page vmiop-env interfaces
 *
 * The vmioplugin environment module implements the environmental
 * support for the vmioplugin API.  In addition to the API, it exports
 * initialization and shutdown entry points.
 *
 * The definitions are here:
 * - @ref VmiopEnvInterfaces
 */

#ifndef _VMIOP_ENV_H_
/**
 * Multiple-include tolerance.
 */
#define _VMIOP_ENV_H_

#include <vmioplugin.h>

/**********************************************************************/
/**
* @defgroup VmiopEnvInterfaces Initialization and Shutdown
*/
/**********************************************************************/
/*@{*/

/**
 * Initialize the vmioplugin environment.
 *
 * This routine must be called before any other parts of vmiop-env
 * are used.
 * @returns Error code: 
 * -            vmiop_success:          Successful registration
 * -            vmiop_error_resource:   No memory or other resource unavailable
 */

extern vmiop_error_t
vmiope_initialize(void);

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

extern vmiop_error_t
vmiope_shutdown(void);

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

extern vmiop_error_t
vmiope_register_plugin(vmiop_plugin_ref_t plugin,
                       vmiop_handle_t *handle_p);

/**
 * Load and initialize a dynamic plugin.
 * 
 * This routine should be called, for each dynamic plugin,
 * after vmiope_register_plugin() has been called for each
 * of the staticly linked plugins.  This routine searches
 * for the plugin in the plugin search list, and loads the
 * first plugin with a matching name.
 *
 * @param[in] plugin_name Pointer to string name of plugin.
 * @param[in] config_index  Index used for this plugin in config file.
 * @param[out] handle_p Reference to variable to receive
 *                      handle for plugin.
 * @returns Error code:
 * -            vmiop_success:          Object found
 * -            vmiop_error_inval:      Invalid configuration.
 * -            vmiop_err_resource:     Memory or other resource not
 *                                      available
 */

extern vmiop_error_t
vmiope_register_dynamic_plugin(char *plugin_name,
                               uint32_t config_index,
                               vmiop_handle_t *handle_p);

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

extern vmiop_error_t
vmiope_connect_plugins(vmiop_handle_t upper_plugin,
                       vmiop_handle_t lower_plugin);

/**
 * Process configuration parameter.
 *
 * Parameter settings are in the form of name=value pairs, separated
 * by white-space, where the value may be enclosed in matching single or
 * double quotation marks or a matching pair of parentheses, braces, or
 * square brackets.
 *
 * Parameters:
 * -    plugin-path:    colon-separated list of directory paths to 
 *                      search for plugins.
 * -    debug:          numeric level (0 to 9, 9 most verbose) for
 *                      debug messages.
 * -    pause:          numeric value, for number of seconds to pause
 *                      during parameter setting.
 *
 * Unrecognized parameter names are ignored.
 * 
 * @param[in] param_s Pointer to string with parameter values
 * @returns Error code:
 * -            vmiop_success:      Normal completion
 * -            vmiop_error_inval:  Invalid parameter
 */

extern vmiop_error_t
vmiope_set_parameters(const char *param_s);

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

extern vmiop_error_t
vmiope_set_search_path(char *path_p);

/**
 * Process VMIOPLUGIN configuration file.
 *
 * @param[in] pluginconfig  Plugin config options:filename,gpu_pci_id=00:00:0.0,vdev_id=0x100:100,...
 * @returns Error code:
 * -            vmiop_success       Normal completion
 * -            vmiop_error_inval   Invalid configuration
 * -            vmiop_error_not_found File not found
 * -            vmiop_error_resource Memory or other resource
 *                                  not available.
 */

extern vmiop_error_t
vmiope_process_configuration(const char *pluginconfig);

#define VMIOPE_CONF_MAX_LINE 1024
/*!< maximum length of a configuration file line */

#define VMIOPE_MAX_PLUGINS 20
/*!< maximum number of plugins  */

#define VMIOPE_DEFAULT_NUM_PLUGINS 1
/*!< Default number of plugins attached to the VM */

/**
 * Enter vmioplugin monitor for qemu.
 *
 * When built with vmioplugin support, qemu code should call
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

extern void
vmiope_enter_monitor(vmiop_bool_t *in_monitor);

/**
 * Leave vmioplugin monitor for qemu.
 *
 * @param[out] in_monitor   Reference to variable to receive 
 *                          vmiop_true if already in monitor
 *                          and vmiop_false otherwise.  Ignored
 *                          in reference is NULL.
 */

extern void
vmiope_leave_monitor(vmiop_bool_t *in_monitor);

/**
 * Pixel type to pixel width in bits
 */

extern uint8_t vmiope_pixel_width[vmiop_pf_max + 1];

/**
 * Pixel type to pixel depth in bytes
 */

extern uint8_t vmiope_pixel_depth[vmiop_pf_max + 1];

/**
 * Pixel type to pixel BGR mode
 */

extern uint8_t vmiope_pixel_bgr[vmiop_pf_max + 1];

/**
 * Pixel depth and BGR to pixel type
 *
 * @param[in] Pixel depth
 * @param[in] Pixel BGR
 * @returns Pixel type
 */

extern vmiop_pixel_format_t
vmiope_pixel_depth_bgr_to_type(uint8_t depth, uint8_t bgr);

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
                            vmiop_bool_t value);

/**
 * Type of function to a apply to a buffer.
 *
 * Used with vmiope_buffer_apply().
 *
 * @param[in] opaque Pointer supplied by caller of
 *                   vmiop_buffer_apply().
 * @param[in] data_p Data to be processed
 * @returns vmiop_bool_t:
 * -            vmiop_true  Terminate loop early
 * -            vmiop_false Continue processing
 */

typedef vmiop_bool_t 
(*vmiope_buffer_apply_function_t)(void *opaque,
                                 void *data_p);

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

extern vmiop_error_t
vmiope_buffer_apply(vmiop_buffer_ref_t buf,
                    uint32_t data_offset,
                    uint32_t data_length,
                    uint32_t unit_length,
                    void *tbuf,
                    vmiope_buffer_apply_function_t function_p,
                    void *opaque);

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

extern vmiop_error_t
vmiope_buffer_pullup(vmiop_buffer_ref_t buf,
                     uint32_t data_offset,
                     uint32_t data_length,
                     void *tbuf,
                     void **data_p);

/**
 * Migration stage notification
 * Wrapper function about the caller to the notify_device function.
 * Note: This only interfaces between demu and env. code.
 * 
 * @param[in] handle   Handle for device
 * @param[in] migration_stage
 * 
 * @returns Error code:
 * -            vmiop_success              Successful completion
 * -            vmiop_error_inval          Invalid state
 */

extern vmiop_error_t
vmiope_notify_device(vmiop_handle_t handle,
                     vmiop_migration_stage_e stage);

/**
 * Read device buffer
 *
 * Wrapper function about the caller to the read_device_buffer function.
 * This only interfaces between demu and env. code.
 *
 * @param[in]   handle          Handle for device
 * @param[in]   buffer          The input buffer device model needs to fill up
 * @param[in]   buffer_size     Input buffer size in bytes
 * @param[out]  remaining_bytes Remaining data size in bytes
 * @param[out]  written_bytes   Written data size in bytes
 * 
 * @returns Error code:
 * -            vmiop_success              Successful completion
 * -            vmiop_error_resource       Unable to retrieve resource
 * -            vmiop_error_inval          Invalid state
 */

extern vmiop_error_t
vmiope_read_device_buffer(vmiop_handle_t handle,
                          void *buffer,
                          uint64_t buffer_size,
                          uint64_t *remaining_bytes,
                          uint64_t *written_bytes);

/**
 * Write device buffer
 *
 * Wrapper function about the caller to the write_device_buffer function.
 * This only interfaces between demu and env. code.
 *
 * @param[in]   handle          Handle for device
 * @param[in]   buffer          The input buffer device model needs to read from
 * @param[in]   buffer_size     Input buffer size in bytes
 * 
 * @returns Error code:
 * -            vmiop_success              Successful completion
 * -            vmiop_error_resource       Unable to retrieve resource
 * -            vmiop_error_inval          Invalid state             
 */

extern vmiop_error_t
vmiope_write_device_buffer(vmiop_handle_t handle,
                           void *buffer,
                           uint64_t buffer_size);

/*@}*/

#endif /* _VMIOP_ENV_H_ */

/*
  ;; Local Variables: **
  ;; mode:c **
  ;; c-basic-offset:4 **
  ;; tab-width:4 **
  ;; indent-tabs-mode:nil **
  ;; End: **
*/
