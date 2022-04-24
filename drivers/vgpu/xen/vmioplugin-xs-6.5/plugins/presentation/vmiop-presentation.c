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
 * @file vmiop-presentation.c
 *
 * @brief
 * Presentation virtualization plugin for the vmioplugin API.
 */

/**
 * @page vmiop-presentation
 *
 * The vmiop-presentation plugin presents in a window frames produced by the 
 * display plugin.
 */

/**
 * @cond vmiopPresentationMiscellaneousDeclarations
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
/*!< Enable strchrnul() in header file */
#endif /* _GNU_SOURCE */
/**
 * @endcond 
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#include <vmioplugin.h>
#include <vmiop-env.h>

#include <xenctrl.h>
#include <vga.h>
#include <demu.h>
#include <surface.h>

#define VMIOPE_PRESENTATION_EDID_SIZE 256
/*!< Maximum size of cached EDID */

xen_pfn_t vram_mfn[(VRAM_RESERVED_SIZE - TARGET_PAGE_SIZE) >> TARGET_PAGE_SHIFT];
/*!< List of VRAM mfn,
     last page of VRAM used for demu-qemu communication is excluded */

/**
 * Presentation state
 */

typedef struct vmiope_presentation_state_s {
    vmiop_handle_t handle;  /*!< plugin handle */
    vmiop_display_configuration_t cfg; /*!< last configuration from upstream */
    vmiop_bool_t needs_upstream_update; /*!< need to send configuration message */
    uint32_t sequence;      /*!< message sequence number */
    uint32_t edid_length;   /*!< valid length of EDID */
    uint8_t edid[VMIOPE_PRESENTATION_EDID_SIZE]; /*!< cached EDID */
} vmiope_presentation_state_t;

static vmiope_presentation_state_t vmiope_ps = 
    { 
        .handle = VMIOP_HANDLE_NULL,
        .cfg =  { 
            .vnum = 0,
            .height = 0,
            .width = 0,
            .ptype = vmiop_pf_inval
        },
        .needs_upstream_update = vmiop_true,
        .sequence = 1u,
        .edid_length = 128u,
#if 1
        .edid = {
            0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
            0x0F, 0x36, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x32, 0x10, 0x01, 0x03, 0x80, 0x00, 0x00, 0x00,
            0x0B, 0x04, 0x85, 0xA0, 0x57, 0x4A, 0x9B, 0x26,
            0x12, 0x50, 0x54, 0x21, 0x08, 0x00, 0x01, 0x01,
            0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
            0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x8C, 0x23,
            0x00, 0xA0, 0x50, 0x00, 0x1E, 0x40, 0x30, 0x20,
            0x37, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1A,
            0x1A, 0x18, 0x00, 0xA0, 0x50, 0xB8, 0x14, 0x20,
            0x30, 0x20, 0x3A, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x1A, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x4E,
            0x56, 0x49, 0x44, 0x49, 0x41, 0x5F, 0x31, 0x32,
            0x78, 0x37, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0xFD,
            0x00, 0x00, 0xFF, 0x00, 0xFF, 0x21, 0x00, 0x0A,
            0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x01
        }/* EDID with Citrix/Transformer Prime native resolution */
#else
        .edid = {
            0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 
            0x38, 0xa3, 0xc6, 0x66, 0x01, 0x01, 0x01, 0x01, 
            0x30, 0x11, 0x01, 0x03, 0x80, 0x29, 0x1f, 0x78, 
            0xea, 0xee, 0x95, 0xa3, 0x54, 0x4c, 0x99, 0x26, 
            0x0f, 0x50, 0x54, 0xbf, 0xef, 0x80, 0x71, 0x4f, 
            0x81, 0x40, 0x81, 0x80, 0x90, 0x40, 0x90, 0x4f, 
            0xa9, 0x40, 0x01, 0x01, 0x01, 0x01, 0x48, 0x3f, 
            0x40, 0x30, 0x62, 0xb0, 0x32, 0x40, 0x40, 0xc0, 
            0x13, 0x00, 0x98, 0x32, 0x11, 0x00, 0x00, 0x1e, 
            0x00, 0x00, 0x00, 0xfd, 0x00, 0x38, 0x4b, 0x1f, 
            0x53, 0x11, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 
            0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x4c, 
            0x43, 0x44, 0x32, 0x30, 0x37, 0x30, 0x56, 0x58, 
            0x0a, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xff, 
            0x00, 0x37, 0x59, 0x33, 0x30, 0x32, 0x34, 0x31, 
            0x34, 0x47, 0x41, 0x0a, 0x20, 0x20, 0x00, 0x75 
        }/* EDID for an NEC LCD2070VX monitor */
#endif
    };
/*!< State of plugin */

/**
 * Update display configuration
 *
 * Only sends a configuration message upstream.
 * Next downstream message will refresh the display.
 */

static void 
vmiope_pr_update_display(void)
{
    vmiop_error_t error_code;
    vmiop_buffer_ref_t buf;
    vmiop_message_presentation_t *pm;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    vmiop_display_configuration_t *cfg;
    uint8_t *edid;

    error_code = vmiop_buffer_alloc(&buf,
                                    vmiop_plugin_class_presentation,
                                    vmiop_plugin_class_display,
                                    1,
                                    (sizeof(vmiop_message_presentation_t) +
                                     VMIOPE_PRESENTATION_EDID_SIZE));
    if (error_code != vmiop_success) {
        vmiope_ps.needs_upstream_update = vmiop_true;
        return;
    }

    pm = (vmiop_message_presentation_t *) buf->element[0].data_p;
    pm->mc.signature = VMIOP_MC_SIGNATURE;
    pm->mc.version = VMIOP_MC_VERSION;
    pm->mc.header_length = sizeof(vmiop_message_presentation_t);
    pm->mc.message_class = vmiop_plugin_class_presentation;
    pm->display_number = vmiope_ps.cfg.vnum;

    pm->mc.sequence = vmiope_ps.sequence++;
    pm->type_code = vmiop_dt_get_configuration;
    pm->content_length = sizeof(vmiop_display_configuration_t);

    /* send back configuration (with possibly updated pixel depth) */
    surface_get_dimensions(&width, &height, &depth);

    cfg = (vmiop_display_configuration_t *)(pm + 1);
    cfg->height = height;
    cfg->width  = width;
    cfg->ptype  = vmiope_pixel_depth_bgr_to_type(depth, 0);

    error_code = vmiop_deliver_message(vmiope_ps.handle,
                                       buf,
                                       vmiop_direction_up);
    if (error_code != vmiop_success) {
        goto done;
    }

    /* send back edid */
    pm->mc.sequence = vmiope_ps.sequence++;
    pm->type_code = vmiop_pt_edid_report;
    pm->content_length = vmiope_ps.edid_length;
    edid = (uint8_t *) (pm + 1);
    if (vmiope_ps.edid_length > 0) {
        memcpy(edid,
               vmiope_ps.edid,
               vmiope_ps.edid_length);
    }
    error_code = vmiop_deliver_message(vmiope_ps.handle,
                                       buf,
                                       vmiop_direction_up);
done:
    if (error_code != vmiop_success) {
        vmiope_ps.sequence--;     // roll back on an error
        vmiope_ps.needs_upstream_update = vmiop_true;
    } else {
        vmiope_ps.needs_upstream_update = vmiop_false;
    }
    (void) vmiop_buffer_free(buf);
}

/**
 * Invalidate display (request refresh).
 *
 * Does nothing, since next upstream message will refresh.
 *
 * @param[in] opaque        Pointer to state record
 */

void vmiope_pr_invalidate_display(void *opaque)
{
    /* nothing to do */
}

/**
 * Initialization function, called when plugin is loaded,
 * before domain is started.
 *
 * @param[in] handle        Handle for this plugin
 * @returns Error code:
 * -            vmiop_success:          Successful initialization
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL or
 *                                      display state is not defined
 * -            vmiop_error_resource:   Resource allocation error
 * -            vmiop_error_no_address_space: Insufficient address space
 */

static vmiop_error_t 
vmiop_presentation_init(vmiop_handle_t handle)
{
    vmiop_bool_t in_monitor;
    vmiop_error_t error_code = vmiop_success;

    if (handle == VMIOP_HANDLE_NULL) {
        return(vmiop_error_inval);
    }

    vmiope_enter_monitor(&in_monitor);
    vmiope_ps.handle = handle;

    if (! in_monitor) {
        vmiope_leave_monitor(NULL);
    }
    return(error_code);
}

/**
 * Shutdown function, called when domain is shutting down
 * gracefully, after domain has stopped.
 *
 * @param[in] handle        Handle for this plugin
 * @returns Error code:
 * -            vmiop_success:          Successful termination
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL
 * -            vmiop_error_timeout:    Timeout waiting for
 *                                      threads to terminate
 */

static vmiop_error_t 
vmiop_presentation_shutdown(vmiop_handle_t handle)
{
    return(vmiop_success);
}

/**
 * Return a named attribute fo the plugin in the referenced variable.
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

static vmiop_error_t 
vmiop_presentation_get_attribute(vmiop_handle_t handle,
                            const char *attr_name,
                            vmiop_attribute_type_t attr_type,
                            vmiop_value_t *attr_value_p,
                            vmiop_emul_length_t attr_value_length)
{
    return(vmiop_error_not_found);
}

/**
 * Set a named attribute for the plugin in the referenced variable.
 *
 * @param[in] handle        Handle for this plugin
 * @param[in] attr_name     Attribute name
 * @param[in] attr_type     Value type
 * @param[in] attr_value_p  Reference to variable containing value
 * @param[in] attr_value_length   Value variable length in case of strings
 * @returns Error code:
 * -            vmiop_success:          Attribute is set successfully
 * -            vmiop_error_inval:      handle is VMIOP_HANDLE_NULL,
 *                                      or attr_value_p is NULL,
 *                                      attr_type is unknown, or
 *                                      attr_type or attr_length is
 *                                      mismatched
 * -            vmiop_error_read_only   attribute may not be set
 * -            vmiop_error_not_found   No such attribute
 * -            vmiop_error_resource    No space in buffer
 */

static vmiop_error_t
vmiop_presentation_set_attribute(vmiop_handle_t handle,
                                 const char *attr_name,
                                 vmiop_attribute_type_t attr_type,
                                 vmiop_value_t *attr_value_p,
                                 vmiop_emul_length_t attr_value_length)
{
    if ((handle == VMIOP_HANDLE_NULL) || (attr_value_p == NULL)) {
        return(vmiop_error_inval);
    }

    if ((strcmp(attr_name, "vnc_console_state") == 0) &&
        (attr_type == vmiop_attribute_type_unsigned_integer)) {
        vmiop_error_t error_code;
        vmiop_buffer_ref_t buf;
        vmiop_message_presentation_t *pm;

        error_code = vmiop_buffer_alloc(&buf,
                                        vmiop_plugin_class_presentation,
                                        vmiop_plugin_class_display,
                                        1,
                                        (sizeof(vmiop_message_presentation_t) +
                                         sizeof(uint32_t)));
        if (error_code != vmiop_success) {
            return (vmiop_error_resource);
        }

        pm = (vmiop_message_presentation_t *) buf->element[0].data_p;
        pm->mc.signature = VMIOP_MC_SIGNATURE;
        pm->mc.version = VMIOP_MC_VERSION;
        pm->mc.header_length = sizeof(vmiop_message_presentation_t);
        pm->mc.message_class = vmiop_plugin_class_presentation;
        pm->display_number = vmiope_ps.cfg.vnum;
        pm->mc.sequence = vmiope_ps.sequence++;
        pm->type_code = vmiop_dt_set_vnc_console_state;
        pm->content_length = sizeof(uint32_t);
        *((uint32_t *)(pm + 1)) = attr_value_p->vmiop_value_unsigned_integer;

        error_code = vmiop_deliver_message(vmiope_ps.handle,
                                           buf,
                                           vmiop_direction_up);

        (void) vmiop_buffer_free(buf);
        return error_code;
    }
    return(vmiop_error_not_found);
}


/**
 * Copy 32-bit pixel, change ARGB to RGBA
 *
 * Used by vmiop_presentation_put_message();
 *
 * @param[in] opaque Pointer supplied by caller of
 *                   vmiop_buffer_apply().
 * @param[in] data_p Data to be processed
 * @returns vmiop_bool_t:
 * -            vmiop_true  Terminate loop early
 * -            vmiop_false Continue processing
 */

static vmiop_bool_t 
vmiop_pt_pixel_copy_and_swap(void *opaque,
                             void *data_p) 
{
    uint8_t *dp;
    int i;

    dp = *((uint8_t **) opaque);
    *((uint8_t **) opaque) = (dp + 4);
#ifdef nolonger
    *dp++ = ((uint8_t *) data_p)[3];
    for (i = 2;
         i >= 0;
         i--) {
        *dp++ = ((uint8_t *) data_p)[i];
    }
#endif
    for (i = 0;
         i <= 3;
         i++) {
        *dp++ = ((uint8_t *) data_p)[i];
    }

    return(vmiop_false);
}

/**
 * Copy 16-bit pixel to 32-bit pixel.
 *
 * Used by vmiop_presentation_put_message();
 *
 * @param[in] opaque Pointer supplied by caller of vmiop_buffer_apply - output buffer.
 * @param[in] data_p Data to be processed - input buffer.
 * @returns vmiop_bool_t:
 * -            vmiop_true  Terminate loop early
 * -            vmiop_false Continue processing
 */

static vmiop_bool_t
vmiop_pt_pixel_16_copy_to_32(void *opaque,
                             void *data_p)
{
    uint32_t *outp;
    uint8_t *inb;
    uint8_t r = 0, g = 0, b = 0;

    outp = *((uint32_t **) opaque);
    inb  = (uint8_t *) data_p;

    if (inb[0] || inb[1])
    {
        b = inb[0] & 0x1F;
        b = (float)(b / 31.0f) * 0xFF;

        g = ((inb[0] >> 5) & 0x7) | ((inb[1] & 0x7) << 3);
        g = (float)(g / 63.0f) * 0xFF;

        r = inb[1] >> 3;
        r = (float)(r / 31.0f) * 0xFF;
    }

    *outp++ = (r << 16) | (g << 8) | b;
    *((uint32_t **) opaque) = outp; // update caller's output buffer ptr

    return(vmiop_false);
}

/**
 * Check if the current configuration (VGA or hires) differs from the
 * configuration passed from an upstream module (i.e. vmiop-display.so)
 *
 * @param[in] handle        Handle for this plugin
 * @param[in] handle        Handle for this plugin
 * @param[in] attr_name     Attribute name
 * @returns vmiop_bool_t:
 * -            vmiop_true  Some component is different
 * -            vmiop_false No changes
 */
static inline vmiop_bool_t
vmiop_has_config_changed(vmiop_display_configuration_t *cur_cfg,
                         vmiop_display_configuration_t *msg_cfg)
{
    return ((cur_cfg->height != msg_cfg->height) ||
            (cur_cfg->width  != msg_cfg->width)  ||
            (cur_cfg->ptype  != msg_cfg->ptype)  ||
            (cur_cfg->pitch  != msg_cfg->pitch)) ? vmiop_true : vmiop_false;
}


/**
 * Accept a message buffer.  The caller should have a hold
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

static vmiop_error_t 
vmiop_presentation_put_message(vmiop_handle_t handle,
                               vmiop_buffer_ref_t buf_p)
{
    vmiop_error_t error_code;
    vmiop_message_display_t *md;
    vmiop_bool_t in_monitor;

    if (buf_p == NULL ||
        buf_p->count == 0 ||
        buf_p->element[0].length < sizeof(vmiop_message_display_t)) {
        return(vmiop_error_inval);
    }
    md = (vmiop_message_display_t *) buf_p->element[0].data_p;
    if (md->mc.header_length > buf_p->element[0].length ||
        md->mc.message_class != vmiop_plugin_class_display) {
        return(vmiop_error_inval);
    }
        
    switch (md->type_code) {
    case vmiop_dt_edid_request:
        if (md->display_number != vmiope_ps.cfg.vnum &&
            md->display_number != VMIOP_DISPLAY_ALL) {
            return(vmiop_error_not_found);
        }
        vmiope_ps.needs_upstream_update = vmiop_true;
        break;

    case vmiop_dt_get_memory_optimization_info:
        {
            vmiop_buffer_element_t *ep;
            vmiop_bool_t* memory_optimization_info_ptr;

            ep = buf_p->element;
            memory_optimization_info_ptr = (vmiop_bool_t*)
                (((uint8_t*)(ep->data_p)) + sizeof(vmiop_message_display_t));
            *memory_optimization_info_ptr = vmiop_true;
        }
        break;

    case vmiop_dt_frame:
    case vmiop_dt_set_configuration:
        if (md->display_number != vmiope_ps.cfg.vnum) {
            return(vmiop_error_not_found);
        }

        {
            vmiop_display_configuration_t dcfg_buf;
            vmiop_display_configuration_t *dcfg;
            vmiop_buffer_element_t *ep;
            void *db;
            vmiop_page_list_t *demu_page_list;

            error_code = vmiope_buffer_pullup(buf_p,
                                              sizeof(vmiop_message_display_t),
                                              sizeof(vmiop_display_configuration_t),
                                              &dcfg_buf,
                                              (void **) &dcfg);
            if (error_code != vmiop_success) {
                return(error_code);
            }
            if (dcfg->vnum != vmiope_ps.cfg.vnum) {
                return(vmiop_error_inval);
            }

            /* look for any configuration changes */
            if (vmiop_has_config_changed(&vmiope_ps.cfg, dcfg) ||
                buf_p->discard_config) {
                buf_p->discard_config = vmiop_false;

                /* copy new configuration */
                vmiope_ps.cfg = *dcfg;

                vmiope_enter_monitor(&in_monitor);
                surface_resize(0,
                               vmiope_ps.cfg.width * 32 / 8,
                               vmiope_ps.cfg.width,
                               vmiope_ps.cfg.height,
                               vmiope_pixel_width[vmiope_ps.cfg.ptype]);

                if (! in_monitor) {
                    vmiope_leave_monitor(NULL);
                }

                if (vmiope_ps.cfg.ptype == vmiop_pf_inval) {
                    /* plugin needs update, if initially was unset */
                    vmiope_ps.needs_upstream_update = vmiop_true;
                }
            }

            if (md->type_code == vmiop_dt_set_configuration) {
                xen_pfn_t    *pfn;
                xen_pfn_t    *mfn;
                int         i, num_pages, ret;
                uint64_t    addr = demu_get_vram_addr();

                ep = buf_p->element;
                demu_page_list = (vmiop_page_list_t*)(((uint8_t *)
                            (ep->data_p)) + sizeof(vmiop_message_display_t));
                num_pages = (VRAM_RESERVED_SIZE - TARGET_PAGE_SIZE) >> TARGET_PAGE_SHIFT;
                mfn = malloc(sizeof (xen_pfn_t) * num_pages);
                if (mfn == NULL) {
                    (void) vmiop_log(vmiop_log_error,
                                     "vmiop-presentation: failed to allocate mfn array");
                    return (vmiop_error_resource);
                }

                /* Find MFN for guest VRAM */
                if (vram_mfn[0] == 0) {
                    pfn = malloc(sizeof (xen_pfn_t) * num_pages);
                    if (pfn == NULL) {
                        (void) vmiop_log(vmiop_log_error,
                                         "vmiop-presentation: failed to allocate pfn array");
                        free(mfn);
                        return (vmiop_error_resource);
                    }
                    for (i = 0; i < num_pages; i++)
                        pfn[i] = (addr >> TARGET_PAGE_SHIFT) + i;

                    ret = demu_translate_guest_pages(pfn, mfn, num_pages);
                    if (ret != vmiop_success) {
                        (void) vmiop_log(vmiop_log_error,
                                         "vmiop-presentation: failed"
                                         " demu_translate_guest_pages, ret = %d",
                                         ret);
                        demu_translate_guest_pages(pfn, mfn, 1);
                        (void) vmiop_log(vmiop_log_error, "failed to translate even 1 page\n");

                        free(pfn);
                        free(mfn);
                        return (vmiop_error_resource);
                    }
                    free(pfn);

                    for (i = 0; i < num_pages; i++) {
                        vram_mfn[i] = mfn[i];
                    }
                } else {
                    for (i = 0; i < num_pages; i++) {
                        mfn[i] = vram_mfn[i];
                    }
                }
                demu_page_list->pte_array = (void*)mfn;
                demu_page_list->num_pte = num_pages; 
            }

            if (md->type_code == vmiop_dt_frame) {
                vmiop_pixel_format_t display_format;
                uint32_t pixel_length_msg, pixel_length_cfg;
                uint32_t surf_width, surf_height, surf_depth;
                uint8_t *surf_datap = NULL;

                surface_get_dimensions(&surf_width, &surf_height, &surf_depth);

                display_format = vmiope_pixel_depth_bgr_to_type(surf_depth, 0);

                surf_datap   = surface_get_buffer();

                pixel_length_msg = (md->content_length - sizeof(*dcfg));
                pixel_length_cfg = vmiope_ps.cfg.pitch * vmiope_ps.cfg.height;

                if (surf_datap == NULL) {
                    (void) vmiop_log(vmiop_log_error, "vmiop-presentation: surface data pointer is NULL");
                    return (vmiop_error_inval);
                }

                /* move the pixels */
                if (vmiope_ps.cfg.ptype == display_format) {
                    if (pixel_length_msg != pixel_length_cfg) {
                        /* pixel format matches, but incorrect size */
                        (void) vmiop_log(vmiop_log_error,
                                         "vmiop-presentation: mismatch on pixel length (expected 0x%x received 0x%x)",
                                         pixel_length_cfg, pixel_length_msg);
                        return (vmiop_error_inval);
                    }

                    error_code = vmiope_buffer_pullup(buf_p,
                                                      (sizeof(vmiop_message_display_t) +
                                                      sizeof(vmiop_display_configuration_t)),
                                                      pixel_length_msg,
                                                      surf_datap,
                                                      &db);
                    if (error_code != vmiop_success) {
                        return (error_code);
                    }

                    if ((db != NULL) && (db != surf_datap)) {
                        /* all in one place in buffer: copy it ourselves */
                        memcpy(surf_datap, db, pixel_length_msg);
                    }
                } else {
                    uint32_t tbuf; /* temporary pixel buffer */
                    uint32_t input_pixel; /* length of input pixel */
                    vmiope_buffer_apply_function_t funcp = NULL;

                    (void) vmiop_log(vmiop_log_error,
                                     "vmiop-presentation: unexpected attempt at pixel format conversion (cfg type %d : msg type %d)",
                                     vmiope_ps.cfg.ptype, display_format);

                    /* attempt input/output conversion */
                    if ((vmiope_ps.cfg.ptype == vmiop_pf_32) && (display_format == vmiop_pf_32_bgr)) {
                        funcp = vmiop_pt_pixel_copy_and_swap;
                        input_pixel = sizeof(uint32_t);
                    } else if ((vmiope_ps.cfg.ptype == vmiop_pf_16) && (display_format == vmiop_pf_32)) {
                        funcp = vmiop_pt_pixel_16_copy_to_32;
                        input_pixel = sizeof(uint16_t);
                    }

                    if (funcp != NULL) {
                        error_code = vmiope_buffer_apply(buf_p,
                                                         (sizeof(vmiop_message_display_t) +
                                                         sizeof(vmiop_display_configuration_t)),
                                                         pixel_length_msg,
                                                         input_pixel,
                                                         (void *) &tbuf,
                                                         funcp,
                                                         (void *) (&surf_datap));
                        if (error_code != vmiop_success) {
                            return(error_code);
                        }
                    } else {
                        (void) vmiop_log(vmiop_log_error,
                                         "vmiop-presentation: display pixel format (cfg type %d : msg type %d) mismatch, not supported",
                                         vmiope_ps.cfg.ptype, display_format);
                    }
                }

                /* display the new pixels */
                vmiope_enter_monitor(&in_monitor);

                surface_update(0, 0, surf_width, surf_height);

                if (! in_monitor) {
                    vmiope_leave_monitor(NULL);
                }
            }
        }
        break;

    case vmiop_dt_hdcp_request:
        if (md->display_number != vmiope_ps.cfg.vnum) {
            return(vmiop_error_not_found);
        }

        /* XXX */
        break;

    default:
        break;
    }

    if (vmiope_ps.needs_upstream_update) {
        vmiope_pr_update_display();
    }

    return(vmiop_success);
}
                                                    
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
 * can accept presentation and presentation messages as input.  
 * A link plugin can accept all messages.
 *
 */

vmiop_plugin_t_v2 vmiop_presentation = {
    .length = sizeof(vmiop_plugin_t),
    .version = VMIOP_PLUGIN_VERSION_V2,
    .signature = VMIOP_PLUGIN_SIGNATURE,
    .name = (char *)"vmiop-presentation",
    .plugin_class = vmiop_plugin_class_presentation,
    .input_classes = (vmiop_const_plugin_class_to_mask(vmiop_plugin_class_presentation) |
                      vmiop_const_plugin_class_to_mask(vmiop_plugin_class_display)),
    .connect_down_allowed = vmiop_false,
    .connect_up_allowed = vmiop_true,
    .init_routine = vmiop_presentation_init,
    .shutdown = vmiop_presentation_shutdown,
    .get_attribute = vmiop_presentation_get_attribute,
    .set_attribute = vmiop_presentation_set_attribute,
    .put_message = vmiop_presentation_put_message,
    .notify_device = NULL,
    .read_device_buffer = NULL,
    .write_device_buffer = NULL,
};

/*
  ;; Local Variables: **
  ;; mode:c **
  ;; c-basic-offset:4 **
  ;; tab-width:4 **
  ;; indent-tabs-mode:nil **
  ;; End: **
*/
