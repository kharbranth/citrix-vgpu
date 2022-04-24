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
#ifndef  _LOG_H
#define  _LOG_H

#include <syslog.h>

#include "demu.h"

#ifdef VERBOSE
#define DBG_V(...) DBG( __VA_ARGS__)
#else
#define DBG_V(...)
#endif

#define CONTEXT(...)                                    \
    do {                                                \
        demu_log_context(__func__,  __VA_ARGS__);       \
    } while (0)

#define DBG(...)                                        \
    do {                                                \
        demu_log(LOG_DEBUG, __func__,  __VA_ARGS__);    \
    } while (0)

#define ERR(...)                                        \
    do {                                                \
        demu_log(LOG_ERR, __func__,  __VA_ARGS__);      \
    } while (0)

#define ERRN(str1)                                      \
    do {                                                \
        demu_log(LOG_ERR, __func__,                     \
                 "%s failed with err %s", str1,         \
                 strerror(errno));                      \
    } while (0)

#define INFO(...)                                       \
    do {                                                \
        demu_log(LOG_INFO, __func__,  __VA_ARGS__);     \
    } while (0)

#endif  /* _LOG_H */

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
