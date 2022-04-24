/*
 * Copyright (c) 2012-2015, Citrix Systems Inc.
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

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <ucontext.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>

#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <aio.h>
#include <pthread.h>

#include <locale.h>

#include <xenctrl.h>
#include <xenstore.h>
#include <xen/hvm/ioreq.h>
#include <libempserver.h>

#include "log.h"
#include "demu.h"
#include "device.h"
#include "mapcache.h"
#include "surface.h"
#include "control.h"

#include <vmiop-env.h>
#include <vmiop-vga-int.h>

#ifdef __i386__
#define DEFAULT_VMIOPLUGIN_SEARCH_PATH "/usr/lib"
#else
#define DEFAULT_VMIOPLUGIN_SEARCH_PATH "/usr/lib64"
#endif

extern vmiop_plugin_t vmiop_presentation;

#define mb() asm volatile ("" : : : "memory")

enum demu_resuming {
    not_resuming = 0,
    set_to_resume,
    resume_when_ready,
    currently_resuming
};

static enum demu_resuming demu_resuming = not_resuming;

enum {
    DEMU_OPT_DOMAIN,
    DEMU_OPT_VCPUS,
    DEMU_OPT_GPU,
    DEMU_OPT_CONFIG,
    DEMU_OPT_DEVICE,
    DEMU_OPT_RESUME,
    DEMU_OPT_SUSPEND,
    DEMU_NR_OPTS
};

static struct option demu_option[] = {
    {"domain", 1, NULL, 0},
    {"vcpus", 1, NULL, 0},
    {"gpu", 1, NULL, 0},
    {"config", 1, NULL, 0},
    {"device", 1, NULL, 0},
    {"resume", 0, NULL, 0},
    {"suspend", 1, NULL, 0},
    {NULL, 0, NULL, 0}
};

static const char *demu_option_text[] = {
    "<domid>",
    "<vcpu count>",
    "<gpu id>",
    "<config>",
    "<device_id>",
    "",
    "<ignored>",
    NULL
};

static const char *prog;

static void usage(void)
{
    int i;

    fprintf(stderr, "Usage: %s <options>\n\n", prog);

    for (i = 0; i < DEMU_NR_OPTS; i++)
        fprintf(stderr, "\t--%s %s\n",
                demu_option[i].name, demu_option_text[i]);

    fprintf(stderr, "\n");

    exit(2);
}

typedef enum {
    DEMU_SEQ_UNINITIALIZED = 0,
    DEMU_SEQ_XS_OPEN,
    DEMU_SEQ_XC_OPEN,
    DEMU_SEQ_VMIOP_ENV_INITIALIZED,
    DEMU_SEQ_VMIOP_VGA_INITIALIZED,
    DEMU_SEQ_VMIOP_PLUGINS_REGISTERED,
    DEMU_SEQ_SERVER_REGISTERED,
    DEMU_SEQ_SHARED_IOPAGE_MAPPED,
    DEMU_SEQ_BUFFERED_IOPAGE_MAPPED,
    DEMU_SEQ_SERVER_ENABLED,
    DEMU_SEQ_PORT_ARRAY_ALLOCATED,
    DEMU_SEQ_EVTCHN_OPEN,
    DEMU_SEQ_PORTS_BOUND,
    DEMU_SEQ_VRAM_MAPPED,
    DEMU_SEQ_SOCKET_CREATED,
    DEMU_SEQ_SURFACE_INITIALIZED,
    DEMU_SEQ_INITIALIZED,
    DEMU_NR_SEQS
} demu_seq_t;


struct demu_space {
    demu_space_t *next;
    uint64_t start;
    uint64_t end;
    const io_ops_t *ops;
    void *priv;
    int io_init;
};

typedef struct demu {
    demu_seq_t seq;
    xc_interface *xch;
    struct xs_handle *xsh;
    xc_evtchn *xceh;
    domid_t domid;
    unsigned int vcpus;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    char config[MAXPATHLEN];
    char *suspend_file;
    ioservid_t ioservid;
    timer_t timer_id;
    uint8_t *vram;
    uint64_t vram_addr;
    unsigned long vram_dirty_map[(VRAM_ACTUAL_SIZE >> TARGET_PAGE_SHIFT) /
                                 (sizeof(unsigned long) * 8)];
    shared_iopage_t *shared_iopage;
    buffered_iopage_t *buffered_iopage;
    evtchn_port_t bufioreq_local_port;
    evtchn_port_t *ioreq_local_port;
    demu_space_t *memory;
    demu_space_t *port;
    demu_space_t *pci_config;
    int io_up;
    vmiop_handle_t presentation_handle;
    in_port_t sport;
    int sfd;
    unsigned int scount;
    int console_active;
    int full_update;

    statefile_section_t statefile_sec;
    int statefile_mode;
    int statefile_fd;
    off_t statefile_offset;

    int migrate_abort;

    struct emp_sock_inf *cs_inf;
} demu_t;

static demu_t demu_state;

#define P2ROUNDUP(_x, _a) -(-(_x) & -(_a))
#define CONST_MAX(X, Y) (((X) > (Y)) ? (X) : (Y))
#define PCI_SBDF(s, b, d, f)                    \
    ((((s) & 0xffff) << 16) |                   \
     (((b) & 0xff) << 8) |                      \
     (((d) & 0x1f) << 3) |                      \
     ((f) & 0x07))

#define SEC_NOTREADY  0
#define SEC_CLOSED    1
#define SEC_R_OPENED  2
#define SEC_W_OPENED  3


uint64_t demu_ident = 0x746d662d554d4544;
uint32_t demu_version = 1;
uint32_t demu_options = 0;

/* demu error code */

enum dec_code {
    dec_unknown = 0,
    dec_internal,
    dec_socket,
    dec_xs,
    dec_libxc,
    dec_filesystem,
    dec_badstate,
    dec_vmiop,
    dec_nomem,
    dec_ioreq,
    dec_max_codes
};

const char xenstore_base_str[] = "/local/domain/";
const char xenstore_vram_str[] = "/vm-data/vram";
const char xenstore_pid_str[] = "/vgpu-pid";
const char xenstore_status_str[] = "/vgpu/state";
const char xenstore_error_str[] = "/vgpu/error-code";

const size_t keysize = sizeof(xenstore_base_str) + sizeof("XXXXX") +
                       CONST_MAX(sizeof(xenstore_vram_str), sizeof(xenstore_error_str));

void gen_key(char *target, const char *key)
{
    (void) snprintf(target, keysize, "%s%d%s",
                    xenstore_base_str, demu_state.domid, key);
}

#define SET_ERROR(_err_code) __set_error(#_err_code, _err_code)

static void set_demu_status(char status[])
{
    char key[keysize];
    gen_key(key, xenstore_status_str);

    if (!xs_write(demu_state.xsh, 0, key, status, strlen(status)))
        ERRN("xs_write");
}

static void __set_error(char errorcode[], enum dec_code dc)
{
    char key[keysize];
    char *errstr;
    if ((dc < 0) || (dc >= dec_max_codes)) {
        ERR("Demu attempted to return invalid code %d %s", dc, errorcode);
        return;
    }

    errstr = &(errorcode[4]);

    ERR("Demu is returing error %s", errstr);

    gen_key(key, xenstore_error_str);

    if (!xs_write(demu_state.xsh, 0, key, errstr, strlen(errstr)))
        ERRN("xs_write");

    set_demu_status("error");
}

static int do_write(void *buffer, size_t count)
{
    int fd = demu_state.statefile_fd;
    struct aiocb new_cb = {
        .aio_fildes = fd,
        .aio_buf = buffer,
        .aio_nbytes = count,
    };
    static struct aiocb current_cb;
    ssize_t r;

    if (current_cb.aio_fildes == fd) {
        const struct aiocb *cbs[1] = { &current_cb };

        r = aio_suspend(cbs, 1, NULL);
        if (r < 0)
            return r;

        r = aio_return(&current_cb);
        if (r < 0)
            return r;

        if (r != current_cb.aio_nbytes) {
            r = -1;
            return r;
        }

        free((void *)current_cb.aio_buf);
        memset(&current_cb, 0, sizeof(current_cb));
    }

    if (count == 0)
        return 0;

    if (demu_state.migrate_abort)
        return -1;

    if (demu_state.statefile_offset >= 0)
        new_cb.aio_offset = demu_state.statefile_offset;

    current_cb = new_cb;

    r = aio_write(&current_cb);
    if (r < 0)
        return r;

    if (demu_state.statefile_offset >= 0)
        demu_state.statefile_offset = current_cb.aio_offset +
            current_cb.aio_nbytes;

    return 0;
}

static int do_read(int fd, void *buffer, size_t count)
{
    char *buf = (char *)buffer;

    while (count) {
        ssize_t r = read(fd, buf, count);

        if (r == 0) {
            ERR("premature EOF");
            errno = ENODATA;
            return -1;
        } else if (r < 0) {
            ERRN("Stream read");
            INFO("read fd = %d", fd);
            return -1;
        } else {
            buf += r;
            count -= r;
        }
    }

    return 0;
}

static int write_header()
{
    const size_t size = sizeof(demu_ident) + sizeof(demu_version) +
        sizeof(demu_options);
    void *header;
    char *buf;

    header = calloc(1, size);
    buf = header;

    if (!header)
        return -1;

    memcpy(buf, &demu_ident, sizeof(demu_ident));
    buf += sizeof(demu_ident);

    memcpy(buf, &demu_version, sizeof(demu_version));
    buf += sizeof(demu_version);

    memcpy(buf, &demu_options, sizeof(demu_options));

    return do_write(header, size);
}

static int write_footer()
{
    struct record_header header = {
        .type = demu_state_close,
    };
    struct demu_record *record;
    int r;

    record = calloc(1, sizeof(*record));
    if (!record)
        return -1;

    record->header = header;

    r = write_record(record);
    if (r)
        return r;

    return do_write(NULL, 0); /* flush */
}

static int demu_read_header(int fd)
{
    int r;

    uint64_t ident = 0;
    uint32_t version = 0;
    uint32_t options = 0;

    if (demu_state.statefile_mode != SEC_CLOSED) {
        ERR("read_header - not closed. %d", demu_state.statefile_mode);
        return -1;
    }

    r = do_read(fd, &ident, sizeof(ident));

    if (r || ident != demu_ident) {
        ERR("Bad ident 0x%" PRIx64 " r:%d", ident, r);
        return -1;
    }

    r = do_read(fd, &version, sizeof(version));

    if (r || version != demu_version) {
        ERR("Bad version %d", version);
        return -1;
    }

    r = do_read(fd, &options, sizeof(options));

    if (r || options != demu_options) {
        ERR("Bad options 0x%x", options);
        return -1;
    }

    demu_state.statefile_mode = SEC_R_OPENED;

    return 0;
}

static void closestate(void)
{
    if (demu_state.statefile_fd >= 0) {
        close(demu_state.statefile_fd);
        demu_state.statefile_fd = -1;
    } else
        ERR("no state fd to close");

    if (demu_state.statefile_mode != SEC_NOTREADY)
        demu_state.statefile_mode = SEC_NOTREADY;
    else
        ERR("state not ready to close");
}

#define MAX_REC_SIZE (1024 * 8)
#define MAX_REC_DATA_SIZE (MAX_REC_SIZE - sizeof(struct record_header))

char padding[] = "Padding";

int write_record(struct demu_record *rec)
{
    int byte;
    int i = 0;
    uint32_t padded =
        P2ROUNDUP(rec->header.length + sizeof(struct record_header), 8);

    for (byte = rec->header.length;
         byte < padded - sizeof(struct record_header);
         byte++) {
        rec->c_data[byte] = padding[i++];
    }

    return do_write(rec, padded);
}

static int demu_state_dirty = 1;

static int demu_checksend_demustate()
{
    struct record_header header = {
        .type = demu_state_demu,
        .length = sizeof(uint64_t),
    };
    struct demu_record *record;
    int r;

    if (!demu_state_dirty)
        return 0;

    record = calloc(1, sizeof(*record));
    if (!record) {
        demu_state_dirty = 0;
        return -1;
    }

    record->header = header;

    r = write_record(record);
    if (!r)
        demu_state_dirty = 0;

    return r;
}

static int demu_read_demustate(struct demu_record *rec)
{
    if (rec->header.length != sizeof(uint64_t)) {
        ERR("Wrong record size %d", rec->header.length);
        return -1;
    }
    errno = 0;

    /* rec->l_data[0]; unneed data */
    demu_state_dirty = 0;

    return 0;
}

static struct sent_stats_s {
    uint64_t sent;
    uint64_t remaining;
    pthread_mutex_t lock;

    uint64_t total_sent;
    uint64_t times_sent;
    uint64_t rtotal_sent;
    uint64_t rtimes_sent;
} sent_stats;

int get_sent_stats(uint64_t * sent, uint64_t * remaining, int reset)
{
    int r;

    r = pthread_mutex_lock(&sent_stats.lock);
    if (r)
        return r;

    *sent = sent_stats.sent;
    *remaining = sent_stats.remaining;

    if (reset)
        sent_stats.sent = 0;

    r = pthread_mutex_unlock(&sent_stats.lock);
    if (r)
        return r;

    return 0;
}

static int do_vmiop_dump(void)
{
    vmiop_error_t v_r;
    int r = 0;
    struct record_header header = {
        .type = demu_state_vmiope,
    };
    struct demu_record *record;
    uint64_t bytes_remaining;
    uint64_t bytes_written;

    if (demu_state.migrate_abort) {
        ERR("Abort");
        return -1;
    }

    record = malloc(MAX_REC_SIZE);
    if (!record) {
            ERR("Failed to allocate record buffer");
            return -1;
    }

    record->header = header;

    /* ask vmiope to fill buffer */
    v_r = vmiope_read_device_buffer(VMIOP_HANDLE_NULL, record->c_data,
                                    MAX_REC_DATA_SIZE, &bytes_remaining,
                                    &bytes_written);
    record->header.length = bytes_written;

    if (v_r) {
        ERR("vmiope_read_device_buffer returned error %d", v_r);
        free(record);
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&sent_stats.lock);
    sent_stats.sent += bytes_written;
    sent_stats.remaining = bytes_remaining;
    pthread_mutex_unlock(&sent_stats.lock);

    /* debug stats */
    sent_stats.rtotal_sent += bytes_written;
    sent_stats.rtimes_sent++;

    if ((sent_stats.rtotal_sent >= 50000000)
            || (sent_stats.rtimes_sent >= 10000)) {
        sent_stats.total_sent += sent_stats.rtotal_sent;
        sent_stats.times_sent += sent_stats.rtimes_sent;

        send_migrate_progress(sent_stats.total_sent, bytes_remaining);

        INFO("Bytes written %" PRIu64 " (+%" PRIu64 ") over %" PRIu64
             " (+%" PRIu64 ") writes", sent_stats.total_sent,
             sent_stats.rtotal_sent, sent_stats.times_sent,
             sent_stats.rtimes_sent);
        sent_stats.rtimes_sent = 0;
        sent_stats.rtotal_sent = 0;
    }

    if (bytes_written != 0) {
        r = write_record(record);

        return r ? -1 : 1;
    }

    free(record);

    /* Finished, now show some stats */
    INFO("Bytes written %" PRIu64 " over %" PRIu64 " writes",
         sent_stats.total_sent + sent_stats.rtotal_sent,
         sent_stats.times_sent + sent_stats.rtimes_sent);
    INFO("Bytes written %d.  All done for this phase.", bytes_written);

    sent_stats.rtimes_sent = 0;
    sent_stats.rtotal_sent = 0;

    sent_stats.times_sent = 0;
    sent_stats.total_sent = 0;

    return 0;
}

void demu_migrate_abort(void)
{
    demu_state.migrate_abort = 1;
    vmiope_notify_device(VMIOP_HANDLE_NULL, vmiop_migration_none);
}

int demu_migrate_phase_stop(void)
{
    int r;

    r = xc_hvm_set_ioreq_server_state(demu_state.xch, demu_state.domid,
                                      demu_state.ioservid, 0);

    INFO("sending vmiop_migration_quiesce");
    vmiope_notify_device(VMIOP_HANDLE_NULL, vmiop_migration_quiesce);

    return r;
}

int demu_start_migrate(void)
{
    int fd = demu_state.statefile_fd;
    struct stat statbuf;
    int r;

    if (demu_state.statefile_mode != SEC_CLOSED) {
        ERR("State section must be closed");
        return -1;
    }

    demu_state.statefile_mode = SEC_W_OPENED;

    r = fstat(fd, &statbuf);
    if (r < 0)
        return r;

    if (S_ISSOCK(statbuf.st_mode)) {
        demu_state.statefile_offset = -1;
    } else {
        off_t offset = lseek(fd, 0, SEEK_CUR);

        if (offset < 0)
            return -1;

        demu_state.statefile_offset = offset;
    }

    demu_state.migrate_abort = 0;

    INFO("About to migrate fd = %d (%s)", fd,
         (demu_state.statefile_offset >= 0) ? "file" : "socket");

    if ((r = write_header()) < 0) {
        ERR("Failed to write header. %s",
            (r == -1) ? strerror(errno) : "");
        return r;
    }

    return 0;
}

void demu_migrate_cleanup(void)
{
    closestate();

    demu_state.migrate_abort = 0;

    INFO("Migration all cleaned up.");
}

int demu_finish_migrate(void)
{
    int fd = demu_state.statefile_fd;
    int r;

    if (demu_state.statefile_mode != SEC_W_OPENED) {
        ERR("State section must be closed");
        return -1;
    }

    demu_state.statefile_mode = SEC_NOTREADY;

    if ((r = write_footer()) < 0) {
        ERR("Failed to write footer. %s",
            (r == -1) ? strerror(errno) : "");
        return r;
    }

    if (demu_state.statefile_offset >= 0)
        lseek(fd, demu_state.statefile_offset, SEEK_SET);

    return 0;
}

int demu_migrate(void)
{
    int r;

    if (demu_state.statefile_mode != SEC_W_OPENED) {
        ERR("State section must be open");
        return -1;
    }

    if ((r = demu_checksend_demustate()) < 0) {
        ERR("Failed to write demustate. %s",
            (r == -1) ? strerror(errno) : "");
        return r;
    }

    if ((r = dump_vga()) < 0) {
        ERR("Failed to write vga state. %s",
            (r == -1) ? strerror(errno) : "");
        return r;
    }

    if ((r = do_vmiop_dump()) < 0) {
        ERR("Failed to write vmiop state. %s",
            (r == -1) ? strerror(errno) : "");
        return r;
    }

    return (r);
}

int demu_init_migrate(int fd)
{
    if (demu_state.statefile_mode != SEC_NOTREADY) {
        ERR("Atempt to migrate while already in progress");
        return -1;
    }

    /* initialise stats */
    pthread_mutex_lock(&sent_stats.lock);
    sent_stats.sent = 0;
    sent_stats.remaining = UINT64_MAX;
    pthread_mutex_unlock(&sent_stats.lock);

    sent_stats.rtotal_sent = 0;
    sent_stats.rtimes_sent = 0;

    sent_stats.total_sent = 0;
    sent_stats.times_sent = 0;

    demu_state.statefile_fd = fd;
    demu_state.statefile_mode = SEC_CLOSED;
    /*demu_state.statefile_sec = statefile_top; */

    demu_state_dirty = 1;
    vga_markdirty();

    INFO("emu init with fd %d", fd);
    return 0;
}

int read_vmiope(struct demu_record *rec)
{
    vmiop_error_t v_r;

    if (rec->header.stype == 1) /* Legacy mode */
        ERR("legacy vmiope buffer type no loneg supported!");
    else {          /* future mode */
        v_r =
            vmiope_write_device_buffer(VMIOP_HANDLE_NULL, rec->c_data,
                                       rec->header.length);
        if (v_r) {
            ERR("vmiope_write_device_buffer returned %d", v_r);
            return -1;
        }
    }

    return 0;
}

int process_record(struct demu_record *rec)
{
    int r;
    static int old_type = -1, old_stype = -1;

    if ((rec->header.type != old_type) && (rec->header.stype != old_stype)) {
        ERR("Reading section type %d.%d len %d", rec->header.type,
            rec->header.stype, rec->header.length);
        old_type = rec->header.type;
        old_stype = rec->header.stype;
    } else {
        CONTEXT("Reading another section type %d.%d len %d",
                rec->header.type, rec->header.stype, rec->header.length);
    }

    switch (rec->header.type) {
    case demu_state_close:
        ERR("state close !");
        return 1;
    case demu_state_demu:
        r = demu_read_demustate(rec);
        break;
    case demu_state_vga:
        r = read_vga(rec);
        break;
    case demu_state_vmiope:
        r = read_vmiope(rec);
        break;
    default:
        ERR("Unknown state type %d", rec->header.type);
        return -1;
    }
    return ((r > 0) ? 0 : r);
}

int read_record(int fd, struct demu_record *rec)
{
    uint32_t padded;
    int r;

    r = do_read(fd, rec, sizeof(rec->header));
    if (r)
        return -1;

    padded =
        P2ROUNDUP(rec->header.length + sizeof(rec->header), 8);

    if (padded > MAX_REC_SIZE)
    {
        ERR("too big");
        return -1;
    }

    padded -= sizeof(rec->header);

    r = do_read(fd, &rec->c_data, padded);
    if (r)
        return -1;

    return 0;
}

static int readstate(int fd)
{
    struct demu_record *rec;
    int r;
    int done = 0;

    void *buffer = malloc(MAX_REC_SIZE);
    rec = buffer;

    if (buffer == NULL) {
        ERR("No memory!");
        return -1;
    }

    do {
        r = read_record(fd, rec);
        if (r)
            done = -1;
        else
            done = process_record(rec);
    } while (done == 0);

    INFO("Reading state finished on %s",
         (done == 1) ? "Success" : "Failure");

    free(buffer);
    return ((done > 0) ? 0 : done);
}

int demu_trigger_resume(void)
{
    if (demu_resuming != set_to_resume)
        return -1;

    demu_resuming = resume_when_ready;
    return 0;
}

static int listen_and_wait_to_resume(void)
{
    int rc;

    while (demu_resuming < resume_when_ready) {
        fd_set rfds;
        fd_set wfds;
        fd_set xfds;
        int nfds;

        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_ZERO(&xfds);

        nfds = emp_select_fdset(demu_state.cs_inf, &rfds, NULL);
        nfds++;
        rc = select(nfds, &rfds, &wfds, &xfds, NULL);

        if (rc < 0 && errno != EINTR)
            break;

        if (rc > 0)
            rc = emp_select_fdread(demu_state.cs_inf, &rfds, rc);
    }
    return ((demu_resuming == resume_when_ready) ? 0 : -1);
}

int demu_get_state()
{
    int fd = -1;
    int r = -1;

    INFO("Waiting for start prompt");

    if (listen_and_wait_to_resume())
        goto error;

    fd = demu_state.statefile_fd;
    if (fd == -1) {
        ERR("Have not recived needed fd.");
        goto error;
    }

    demu_resuming = currently_resuming;
    INFO("Starting restore");

    if (demu_read_header(fd))
        goto error;

    r = readstate(fd);

error:
    closestate();
    report_resume_done((r) ? migration_failed : migration_success);
    return r;
}

domid_t demu_get_domid(void)
{
    return demu_state.domid;
}

void *demu_map_guest_pages(xen_pfn_t pfn[], int count, int read_only,
                           int populate)
{
    void *ptr;

    if (populate) {
        int rc;

        rc = xc_domain_populate_physmap_exact(demu_state.xch,
                                              demu_state.domid, count, 0,
                                              0, pfn);
        if (rc < 0) {
            ERRN("xc_domain_populate_physmap_exact");
            return NULL;
        }
    }

    ptr = xc_map_foreign_pages(demu_state.xch, demu_state.domid,
                               (read_only) ? PROT_READ : PROT_READ |
                               PROT_WRITE, pfn, count);
    if (ptr == NULL)
        ERRN("xc_map_foreign_pages");

    return ptr;
}

void *demu_map_guest_range(uint64_t addr, uint64_t size, int read_only,
                           int populate)
{
    xen_pfn_t *pfn;
    int i, n;
    void *ptr;

    size = P2ROUNDUP(size, TARGET_PAGE_SIZE);

    n = size >> TARGET_PAGE_SHIFT;
    pfn = malloc(sizeof(xen_pfn_t) * n);

    if (pfn == NULL) {
        ERRN("malloc");
        return NULL;
    }

    for (i = 0; i < n; i++)
        pfn[i] = (addr >> TARGET_PAGE_SHIFT) + i;

    ptr = demu_map_guest_pages(pfn, n, read_only, populate);
    if (ptr == NULL) {
        ERRN("map_guest_pages");
    }

    free(pfn);
    return ptr;
}

int demu_relocate_guest_range(uint64_t old, uint64_t new, uint64_t size)
{
    int i, n;
    int rc;

    size = P2ROUNDUP(size, TARGET_PAGE_SIZE);

    n = size >> TARGET_PAGE_SHIFT;

    for (i = 0; i < n; i++) {
        unsigned long idx = (old >> TARGET_PAGE_SHIFT) + i;
        xen_pfn_t pfn = (new >> TARGET_PAGE_SHIFT) + i;

        rc = xc_domain_add_to_physmap(demu_state.xch, demu_state.domid,
                                      XENMAPSPACE_gmfn, idx, pfn);
        if (rc < 0)
            goto fail1;

    }

    return 0;

fail1:
    ERR("fail1: %s", strerror(errno));

    return -1;
}

void demu_set_guest_dirty_page(xen_pfn_t pfn)
{
    int rc;

    rc = xc_hvm_modified_memory(demu_state.xch, demu_state.domid, pfn, 1);
    if (rc < 0)
        ERR("xc_hvm_modified_memory returned %d", rc);
}

int
demu_set_guest_dirty_pages(uint64_t count, const struct pages *page_list)
{
    uint64_t i;
    int r;

    for (i = 0; i < count; i++) {
        r = xc_hvm_modified_memory(demu_state.xch, demu_state.domid,
                                   page_list[i].first_gfn,
                                   page_list[i].count);
        if (r < 0)
            return r;
    }
    return 0;
}

int demu_set_guest_pages(xen_pfn_t gfn, xen_pfn_t mfn, int count, int add)
{
    int rc;

    if (add) {
        rc = xc_domain_iomem_permission(demu_state.xch, demu_state.domid,
                                        mfn, count, add);
        if (rc < 0) {
            ERR("Failed to add iomem perms: d%d, mfn %lx, nr %d: rc %d, %s", demu_state.domid, mfn, count, rc, strerror(errno));
            return rc;
        }
    }

    rc = xc_domain_memory_mapping(demu_state.xch, demu_state.domid,
                                  gfn, mfn, count, add);
    if (rc < 0) {
        ERR("Failed to map memory: d%d, mfn %lx, nr %d, gfn %lx, add %d: rc %d, %s", demu_state.domid, mfn, count, gfn, add, rc, strerror(errno));
        return rc;
    }

    if (!add) {
        rc = xc_domain_iomem_permission(demu_state.xch, demu_state.domid,
                                        mfn, count, add);
        if (rc < 0) {
            ERR("Failed to remove iomem perms: d%d, mfn %lx, nr %d: rc %d, %s", demu_state.domid, mfn, count, rc, strerror(errno));
            return rc;
        }
    }

    return 0;
}

int demu_maximum_guest_pages(void)
{
    xen_pfn_t max_pfn;
    return xc_domain_maximum_gpfn(demu_state.xch, demu_state.domid,
                                  &max_pfn) ? : max_pfn;

}

int
demu_translate_guest_pages(xen_pfn_t pfn_array[], xen_pfn_t mfn_array[],
                           int count)
{
    struct pv_iommu_op *ops;
    int i;
    int rc;

    ops = alloca(sizeof(*ops) * count);
    memset(ops, 0, sizeof(*ops) * count);

    for (i = 0; i < count; i++) {
        ops[i].subop_id = IOMMUOP_lookup_foreign_page;
        ops[i].flags = IOMMU_OP_writeable;
        ops[i].u.lookup_foreign_page.gfn = pfn_array[i];
        ops[i].u.lookup_foreign_page.domid = demu_state.domid;
        ops[i].u.lookup_foreign_page.ioserver = demu_state.ioservid;
    }

    rc = xc_iommu_op(demu_state.xch, ops, count);
    if (rc < 0) {
        ERRN("xc_iommu_op");
        return rc;
    }

    for (i = 0; i < count; i++) {
        /*
         * We don't expect any lookup failures, if one happens return
         * an obviously bad BFN.
         */
        if (ops[i].status != 0)
            mfn_array[i] = -1;
        else
            mfn_array[i] = ops[i].u.lookup_foreign_page.bfn;
    }

    return 0;
}

int demu_release_guest_pages(xen_pfn_t mfn[], int count)
{
    struct pv_iommu_op *ops;
    int i;
    int rc;

    ops = alloca(sizeof(*ops) * count);
    memset(ops, 0, sizeof(*ops) * count);

    for (i = 0; i < count; i++) {
        ops[i].subop_id = IOMMUOP_unmap_foreign_page;
        ops[i].u.unmap_foreign_page.bfn = mfn[i];
        ops[i].u.unmap_foreign_page.ioserver = demu_state.ioservid;
    }

    rc = xc_iommu_op(demu_state.xch, ops, count);
    if (rc < 0) {
        ERRN("xc_iommu_op");
        return rc;
    }

    /* Unmaps are always be successful so no need to check status. */

    return 0;
}

static demu_space_t *demu_find_space(demu_space_t * head, uint64_t addr)
{
    demu_space_t *space;

    for (space = head; space != NULL; space = space->next)
        if (addr >= space->start && addr <= space->end)
            return space;

    return NULL;
}

demu_space_t *demu_find_pci_config_space(uint8_t bdf)
{
    return demu_find_space(demu_state.pci_config, bdf);
}

demu_space_t *demu_find_port_space(uint64_t addr)
{
    return demu_find_space(demu_state.port, addr);
}

demu_space_t *demu_find_memory_space(uint64_t addr)
{
    return demu_find_space(demu_state.memory, addr);
}

static int
demu_register_space(demu_space_t ** headp, uint64_t start, uint64_t end,
                    const io_ops_t * ops, void *priv)
{
    demu_space_t *space;

    if (demu_find_space(*headp, start) || demu_find_space(*headp, end))
        goto fail1;

    space = malloc(sizeof(demu_space_t));
    if (space == NULL)
        goto fail2;

    space->start = start;
    space->end = end;
    space->ops = ops;
    space->priv = priv;
    space->io_init = 0;

    space->next = *headp;
    *headp = space;

    return 0;

fail2:
    ERR("fail2");

fail1:
    ERR("fail1: %s", strerror(errno));

    return -1;
}

static void
demu_deregister_space(demu_space_t ** headp, uint64_t start,
                      uint64_t * endp)
{
    demu_space_t **spacep;
    demu_space_t *space;

    spacep = headp;
    while ((space = *spacep) != NULL) {
        if (start == space->start) {
            *spacep = space->next;

            if (endp != NULL)
                *endp = space->end;

            free(space);
            return;
        }
        spacep = &(space->next);
    }
    assert(false);
}

int demu_register_pci_config_space(const io_ops_t * ops, void *priv)
{
    uint64_t sbdf;
    int rc;

    DBG("%02X:%02X.%X",
        demu_state.bus, demu_state.device, demu_state.function);

    sbdf =
        PCI_SBDF(0, demu_state.bus, demu_state.device,
                 demu_state.function);

    rc = demu_register_space(&demu_state.pci_config, sbdf, sbdf, ops,
                             priv);
    if (rc < 0)
        goto fail1;

    if (demu_state.io_up) {
        rc = xc_hvm_map_pcidev_to_ioreq_server(demu_state.xch,
                                               demu_state.domid,
                                               demu_state.ioservid, 0,
                                               demu_state.bus,
                                               demu_state.device,
                                               demu_state.function);
        if (rc < 0)
            goto fail2;

        demu_state.pci_config->io_init = 1;
    }

    return 0;

fail2:
    ERR("fail2");

    demu_deregister_space(&demu_state.pci_config, sbdf, NULL);

fail1:
    ERR("fail1: %s", strerror(errno));

    return -1;
}

int
demu_register_port_space(uint64_t start, uint64_t size,
                         const io_ops_t * ops, void *priv)
{
    uint64_t end = start + size - 1;
    int rc;

    DBG("%" PRIx64 " - %" PRIx64 "", start, end);

    rc = demu_register_space(&demu_state.port, start, end, ops, priv);
    if (rc < 0)
        goto fail1;

    if (demu_state.io_up) {
        rc = xc_hvm_map_io_range_to_ioreq_server(demu_state.xch,
                demu_state.domid,
                demu_state.ioservid, 0,
                start, end);
        if (rc < 0)
            goto fail2;
        demu_state.port->io_init = 1;
    }

    return 0;

fail2:
    ERR("fail2");

    demu_deregister_space(&demu_state.port, start, NULL);

fail1:
    ERR("fail1: %s", strerror(errno));

    return -1;
}

int
demu_register_memory_space(uint64_t start, uint64_t size,
                           const io_ops_t * ops, void *priv)
{
    uint64_t end = start + size - 1;
    int rc;

    DBG("%" PRIx64 " - %" PRIx64 "", start, end);

    rc = demu_register_space(&demu_state.memory, start, end, ops, priv);
    if (rc < 0)
        goto fail1;

    if (demu_state.io_up) {
        rc = xc_hvm_map_io_range_to_ioreq_server(demu_state.xch,
                demu_state.domid,
                demu_state.ioservid, 1,
                start, end);
        if (rc < 0)
            goto fail2;
        demu_state.memory->io_init = 1;
    }

    return 0;

fail2:
    ERR("fail2");

    demu_deregister_space(&demu_state.memory, start, NULL);

fail1:
    ERR("fail1: %s", strerror(errno));

    return -1;
}

void demu_inject_msi(uint64_t addr, uint32_t data)
{
    (void) xc_hvm_inject_msi(demu_state.xch, demu_state.domid, addr, data);
}

void demu_set_pci_intx(int line, int level)
{
    (void) xc_hvm_set_pci_intx_level(demu_state.xch, demu_state.domid,
                                     0, demu_state.bus, demu_state.device,
                                     line, level);
}

void demu_deregister_pci_config_space(void)
{
    uint64_t sbdf;
    demu_space_t *space;
    int io_init;



    sbdf =
        PCI_SBDF(0, demu_state.bus, demu_state.device,
                 demu_state.function);

    space = demu_find_space(demu_state.pci_config, sbdf);
    if (!space)
        return;
    io_init = space->io_init;

    demu_deregister_space(&demu_state.pci_config, sbdf, NULL);

    if (io_init)
        xc_hvm_unmap_pcidev_from_ioreq_server(demu_state.xch,
                                              demu_state.domid,
                                              demu_state.ioservid, 0,
                                              demu_state.bus,
                                              demu_state.device,
                                              demu_state.function);
}

void demu_deregister_port_space(uint64_t start)
{
    uint64_t end;
    demu_space_t *space;
    int io_init;

    space = demu_find_space(demu_state.port, start);
    if (!space)
        return;
    io_init = space->io_init;

    demu_deregister_space(&demu_state.port, start, &end);

    DBG("%" PRIx64 " - %" PRIx64 "", start, end);

    if (io_init)
        xc_hvm_unmap_io_range_from_ioreq_server(demu_state.xch,
                                                demu_state.domid,
                                                demu_state.ioservid, 0,
                                                start, end);
}

void demu_deregister_memory_space(uint64_t start)
{
    uint64_t end;
    demu_space_t *space;
    int io_init;

    space = demu_find_space(demu_state.memory, start);
    if (!space)
        return;
    io_init = space->io_init;

    demu_deregister_space(&demu_state.memory, start, &end);

    DBG("%" PRIx64 " - %" PRIx64 "", start, end);

    if (io_init)
        xc_hvm_unmap_io_range_from_ioreq_server(demu_state.xch,
                                                demu_state.domid,
                                                demu_state.ioservid, 1,
                                                start, end);
}

#define DEMU_IO_READ(_fn, _priv, _addr, _size, _count, _val)        \
    do {                                                            \
        int             _i = 0;                                     \
        unsigned int    _shift = 0;                                 \
                                                                    \
        (_val) = 0;                                                 \
        for (_i = 0; _i < (_count); _i++)                           \
        {                                                           \
            (_val) |= (uint64_t)(_fn)((_priv), (_addr)) << _shift;  \
            _shift += 8 * (_size);                                  \
            (_addr) += (_size);                                     \
        }                                                           \
    } while (false)

uint64_t demu_io_read(demu_space_t * space, uint64_t addr, uint64_t size)
{
    uint64_t val = ~0ull;

    switch (size) {
    case 1:
        val = space->ops->readb(space->priv, addr);
        break;

    case 2:
        if (space->ops->readw == NULL)
            DEMU_IO_READ(space->ops->readb, space->priv, addr, 1, 2, val);
        else
            DEMU_IO_READ(space->ops->readw, space->priv, addr, 2, 1, val);
        break;

    case 4:
        if (space->ops->readl == NULL) {
            if (space->ops->readw == NULL)
                DEMU_IO_READ(space->ops->readb, space->priv, addr, 1, 4,
                             val);
            else
                DEMU_IO_READ(space->ops->readw, space->priv, addr, 2, 2,
                             val);
        } else {
            DEMU_IO_READ(space->ops->readl, space->priv, addr, 4, 1, val);
        }
        break;

    case 8:
        if (space->ops->readl == NULL) {
            if (space->ops->readw == NULL)
                DEMU_IO_READ(space->ops->readb, space->priv, addr, 1, 8,
                             val);
            else
                DEMU_IO_READ(space->ops->readw, space->priv, addr, 2, 4,
                             val);
        } else {
            DEMU_IO_READ(space->ops->readl, space->priv, addr, 4, 2, val);
        }
        break;

    default:
        break;
    }

    return val;
}

#define DEMU_IO_WRITE(_fn, _priv, _addr, _size, _count, _val)   \
    do {                                                        \
        int             _i = 0;                                 \
        unsigned int    _shift = 0;                             \
                                                                \
        for (_i = 0; _i < (_count); _i++)                       \
        {                                                       \
            (_fn)((_priv), (_addr), (_val) >> _shift);          \
            _shift += 8 * (_size);                              \
            (_addr) += (_size);                                 \
        }                                                       \
    } while (false)

void
demu_io_write(demu_space_t * space, uint64_t addr, uint64_t size,
              uint64_t val)
{
    switch (size) {
    case 1:
        space->ops->writeb(space->priv, addr, val);
        break;

    case 2:
        if (space->ops->writew == NULL)
            DEMU_IO_WRITE(space->ops->writeb, space->priv, addr, 1, 2,
                          val);
        else
            DEMU_IO_WRITE(space->ops->writew, space->priv, addr, 2, 1,
                          val);
        break;

    case 4:
        if (space->ops->writel == NULL) {
            if (space->ops->writew == NULL)
                DEMU_IO_WRITE(space->ops->writeb, space->priv, addr, 1, 4,
                              val);
            else
                DEMU_IO_WRITE(space->ops->writew, space->priv, addr, 2, 2,
                              val);
        } else {
            DEMU_IO_WRITE(space->ops->writel, space->priv, addr, 4, 1,
                          val);
        }
        break;

    case 8:
        if (space->ops->writel == NULL) {
            if (space->ops->writew == NULL)
                DEMU_IO_WRITE(space->ops->writeb, space->priv, addr, 1, 8,
                              val);
            else
                DEMU_IO_WRITE(space->ops->writew, space->priv, addr, 2, 4,
                              val);
        } else {
            DEMU_IO_WRITE(space->ops->writel, space->priv, addr, 4, 2,
                          val);
        }
        break;

    default:
        break;
    }
}

static inline void
__copy_to_guest_memory(uint64_t addr, uint64_t size, uint8_t * src)
{
    uint8_t *dst = mapcache_lookup(addr);

    assert(((addr + size - 1) >> TARGET_PAGE_SHIFT) ==
           (addr >> TARGET_PAGE_SHIFT));

    if (dst == NULL)
        goto fail1;

    memcpy(dst, src, size);
    demu_set_guest_dirty_page(addr >> TARGET_PAGE_SHIFT);
    return;

fail1:
    ERR("fail1");
}

static inline void
__copy_from_guest_memory(uint64_t addr, uint64_t size, uint8_t * dst)
{
    uint8_t *src = mapcache_lookup(addr);

    assert(((addr + size - 1) >> TARGET_PAGE_SHIFT) ==
           (addr >> TARGET_PAGE_SHIFT));

    if (src == NULL)
        goto fail1;

    memcpy(dst, src, size);
    return;

fail1:
    ERR("fail1");

    memset(dst, 0xff, size);
}

static void
demu_handle_io(demu_space_t * space, ioreq_t * ioreq, int is_mmio)
{
    if (space == NULL)
        goto fail1;

    if (ioreq->dir == IOREQ_READ) {
        int i, sign;

        sign = ioreq->df ? -1 : 1;

        for (i = 0; i < ioreq->count; i++) {
            if (!ioreq->data_is_ptr) {
                ioreq->data =
                    demu_io_read(space, ioreq->addr, ioreq->size);
            } else {
                uint64_t data;

                data = demu_io_read(space, ioreq->addr, ioreq->size);

                __copy_to_guest_memory(ioreq->data +
                                       (sign * i * ioreq->size),
                                       ioreq->size, (uint8_t *) & data);
            }

            if (is_mmio)
                ioreq->addr += sign * ioreq->size;
        }
    } else if (ioreq->dir == IOREQ_WRITE) {
        int i, sign;

        sign = ioreq->df ? -1 : 1;

        for (i = 0; i < ioreq->count; i++) {
            if (!ioreq->data_is_ptr) {
                demu_io_write(space, ioreq->addr, ioreq->size,
                              ioreq->data);
            } else {
                uint64_t data;

                __copy_from_guest_memory(ioreq->data +
                                         (sign * i * ioreq->size),
                                         ioreq->size, (uint8_t *) & data);

                demu_io_write(space, ioreq->addr, ioreq->size, data);
            }

            if (is_mmio)
                ioreq->addr += sign * ioreq->size;
        }
    }

    return;

fail1:
    ERR("fail1");
}

static void demu_handle_ioreq(ioreq_t * ioreq)
{
    demu_space_t *space;

    switch (ioreq->type) {
    case IOREQ_TYPE_PIO:
        space = demu_find_port_space(ioreq->addr);
        demu_handle_io(space, ioreq, false);
        break;

    case IOREQ_TYPE_COPY:
        space = demu_find_memory_space(ioreq->addr);
        demu_handle_io(space, ioreq, true);
        break;

    case IOREQ_TYPE_PCI_CONFIG: {
        uint32_t sbdf;

        sbdf = (uint32_t) (ioreq->addr >> 32);

        ioreq->addr &= 0xffffffff;

        space = demu_find_pci_config_space(sbdf);
        demu_handle_io(space, ioreq, false);
        break;
    }
    case IOREQ_TYPE_TIMEOFFSET:
        break;

    case IOREQ_TYPE_INVALIDATE:
        mapcache_invalidate();
        break;

    default:
        ERR("UNKNOWN (%02x)", ioreq->type);
        break;
    }
}

static int demu_timer_create(void)
{
    DBG_V("Create timer");
    int pfd[2];
    int flags;
    timer_t tid;
    struct sigevent sigev;

    memset(pfd, 0, sizeof(pfd));
    if (pipe(pfd) < 0)
        goto fail1;

    flags = fcntl(pfd[0], F_GETFL);
    fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);

    flags = fcntl(pfd[1], F_GETFL);
    fcntl(pfd[1], F_SETFL, flags | O_NONBLOCK);

    sigev.sigev_notify = SIGEV_SIGNAL;
    sigev.sigev_signo = SIGRTMIN;
    sigev.sigev_value.sival_int = pfd[1];

    if (timer_create(CLOCK_MONOTONIC, &sigev, &tid) < 0)
        goto fail2;

    demu_state.timer_id = tid;

    return pfd[0];

fail2:
    ERR("fail2");

    close(pfd[1]);
    close(pfd[0]);

fail1:
    ERR("fail1: %s", strerror(errno));

    return -1;
}

static void demu_timer_destroy(void)
{
    DBG_V("Destroy timer");
    timer_delete(demu_state.timer_id);
}

int demu_console_start(void)
{

    DBG_V("console_start");
    time_t s;
    long ns;
    struct itimerspec it;
    vmiop_error_t error_code;

    s = CONSOLE_REFRESH_PERIOD / 1000000;
    ns = (CONSOLE_REFRESH_PERIOD - (s * 1000000)) * 1000;

    it.it_interval.tv_sec = it.it_value.tv_sec = s;
    it.it_interval.tv_nsec = it.it_value.tv_nsec = ns;

    if (timer_settime(demu_state.timer_id, 0, &it, NULL) < 0)
        goto fail1;

    demu_state.console_active = 1;
    demu_state.full_update = 1;

    error_code = vmiope_set_vnc_console_state(&vmiop_presentation,
                 demu_state.
                 presentation_handle,
                 vmiop_true);
    if (error_code != vmiop_success)
        ERR("vmiope_set_vnc_console_state failed");

    INFO("done");

    return 0;

fail1:
    ERR("fail1: %s", strerror(errno));

    return -1;
}

static int demu_console_stop(void)
{
    DBG_V("console_stop");
    struct itimerspec it;
    vmiop_error_t error_code;

    error_code = vmiope_set_vnc_console_state(&vmiop_presentation,
                 demu_state.
                 presentation_handle,
                 vmiop_false);
    if (error_code != vmiop_success)
        ERR("vmiope_set_vnc_console_state failed");

    demu_state.console_active = 0;

    it.it_interval.tv_sec = it.it_value.tv_sec = 0;
    it.it_interval.tv_nsec = it.it_value.tv_nsec = 0;

    if (timer_settime(demu_state.timer_id, 0, &it, NULL) < 0)
        goto fail1;

    INFO("done");

    return 0;

fail1:
    ERR("fail1: %s", strerror(errno));

    return -1;
}

static void demu_console_refresh(void)
{
    DBG_V("console_refresh");

    if (!demu_state.console_active)
        return;

    if (vmiop_vga_in_VGA_state()) {
        surface_refresh(demu_state.full_update);
        demu_state.full_update = 0;
    }

    assert(demu_state.scount > 0);
    demu_state.scount--;

    if (demu_state.scount == 0)
        demu_console_stop();
}

static int demu_socket_create(void)
{
    DBG_V("demu_socket_create");
    int sfd;
    struct sockaddr_in server;
    socklen_t socklen;
    int rc;

    sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0)
        goto fail1;

    memset(&server, 0, sizeof(server));

    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server.sin_port = htons(INADDR_ANY);

    socklen = sizeof(server);

    rc = bind(sfd, (struct sockaddr *) &server, socklen);
    if (rc < 0)
        goto fail2;

    rc = getsockname(sfd, (struct sockaddr *) &server, &socklen);
    if (rc < 0)
        goto fail3;

    assert(socklen == sizeof(server));

    demu_state.sport = htons(server.sin_port);
    demu_state.sfd = sfd;

    return 0;

fail3:
    ERR("fail3");

fail2:
    ERR("fail2");

    close(sfd);

fail1:
    ERR("fail1: %s", strerror(errno));

    return -1;
}

in_port_t demu_socket_port(void)
{
    return demu_state.sport;
}

static void demu_socket_read(void)
{
    struct sockaddr_in client;
    socklen_t socklen;
    char buf;
    int n;

    n = recvfrom(demu_state.sfd, &buf, 1, MSG_DONTWAIT, &client, &socklen);
    if (n == 1)
        demu_state.scount = 5000000 / CONSOLE_REFRESH_PERIOD;

    if (!demu_state.console_active)
        demu_console_start();
}

static void demu_socket_destroy(void)
{
    close(demu_state.sfd);
}

#define demu_seq_next( _seq) __demu_seq_next(_seq, #_seq)

static void __demu_seq_next(demu_seq_t seq, char *seq_str)
{
    assert(demu_state.seq < DEMU_SEQ_INITIALIZED);
    ++demu_state.seq;

    if (demu_state.seq != seq) {
        ERR("SEQENCE ERROR - got %d %s, expected %d", seq, seq_str,
            demu_state.seq);
    }

    seq_str += strlen("DEMU_SEQ_");
    DBG("> %s", seq_str);

    switch (seq) {
    case DEMU_SEQ_SERVER_REGISTERED:
        DBG("ioservid = %u", demu_state.ioservid);
        break;

    case DEMU_SEQ_VRAM_MAPPED:
        DBG("vram = %p", demu_state.vram);
        break;

    case DEMU_SEQ_SHARED_IOPAGE_MAPPED:
        DBG("shared_iopage = %p", demu_state.shared_iopage);
        break;

    case DEMU_SEQ_BUFFERED_IOPAGE_MAPPED:
        DBG("buffered_iopage = %p", demu_state.buffered_iopage);
        break;

    case DEMU_SEQ_PORTS_BOUND: {
        int i;
        for (i = 0; i < demu_state.vcpus; i++)
            DBG("VCPU%d: %u -> %u", i,
                demu_state.shared_iopage->vcpu_ioreq[i].vp_eport,
                demu_state.ioreq_local_port[i]);

        break;
    }
    default:
        break;
    }
}

static void demu_teardown(void)
{
    if (demu_state.seq == DEMU_SEQ_INITIALIZED) {
        char key[keysize];

        gen_key(key, xenstore_pid_str);
        xs_rm(demu_state.xsh, 0, key);

        gen_key(key, xenstore_vram_str);
        xs_rm(demu_state.xsh, 0, key);


        xs_rm(demu_state.xsh, 0, key);

        DBG("<INITIALIZED");
    }

    if (demu_state.seq >= DEMU_SEQ_SURFACE_INITIALIZED) {
        DBG("<SURFACE_INITIALIZED");

        surface_teardown();
    }

    if (demu_state.seq >= DEMU_SEQ_SOCKET_CREATED) {
        DBG("<SOCKET_CREATED");

        demu_socket_destroy();
    }

    if (demu_state.seq >= DEMU_SEQ_VMIOP_PLUGINS_REGISTERED) {
        DBG("<VMIOP_PLUGINS_REGISTERED");
    }

    if (demu_state.seq >= DEMU_SEQ_VMIOP_VGA_INITIALIZED) {
        DBG("<VMIOP_VGA_INITIALIZED");

        vmiop_vga_shutdown();
    }

    if (demu_state.seq >= DEMU_SEQ_VMIOP_ENV_INITIALIZED) {
        DBG("<VMIOP_ENV_INITIALIZED");

        vmiope_shutdown();
    }

    if (demu_state.seq >= DEMU_SEQ_PORTS_BOUND) {
        DBG("<EVTCHN_PORTS_BOUND");
    }

    if (demu_state.seq >= DEMU_SEQ_EVTCHN_OPEN) {
        int i;

        DBG("<EVTCHN_OPEN");

        for (i = 0; i < demu_state.vcpus; i++) {
            evtchn_port_or_error_t port;

            port = demu_state.ioreq_local_port[i];

            if (port >= 0)
                (void) xc_evtchn_unbind(demu_state.xceh, port);

            if (i == 0) {
                port = demu_state.bufioreq_local_port;

                if (port >= 0)
                    (void) xc_evtchn_unbind(demu_state.xceh, port);
            }
        }

        xc_evtchn_close(demu_state.xceh);
    }

    if (demu_state.seq >= DEMU_SEQ_PORT_ARRAY_ALLOCATED) {
        DBG("<PORT_ARRAY_ALLOCATED");

        free(demu_state.ioreq_local_port);
    }

    if (demu_state.seq >= DEMU_SEQ_SERVER_ENABLED) {
        xc_hvm_set_ioreq_server_state(demu_state.xch, demu_state.domid,
                                      demu_state.ioservid, 0);

        DBG("<SERVER_ENABLED");
    }

    if (demu_state.seq >= DEMU_SEQ_BUFFERED_IOPAGE_MAPPED) {
        DBG("<BUFFERED_IOPAGE_MAPPED");

        munmap(demu_state.buffered_iopage, XC_PAGE_SIZE);
    }

    if (demu_state.seq >= DEMU_SEQ_SHARED_IOPAGE_MAPPED) {
        DBG("<SHARED_IOPAGE_MAPPED");

        munmap(demu_state.shared_iopage, XC_PAGE_SIZE);
    }

    if (demu_state.seq >= DEMU_SEQ_SERVER_REGISTERED) {
        (void) xc_hvm_destroy_ioreq_server(demu_state.xch,
                                           demu_state.domid,
                                           demu_state.ioservid);
        DBG("<SERVER_REGISTERED");
    }

    if (demu_state.seq >= DEMU_SEQ_VRAM_MAPPED) {
        DBG("<VRAM_MAPPED");

        munmap(demu_state.vram, VRAM_RESERVED_SIZE);
    }

    if (demu_state.seq >= DEMU_SEQ_XS_OPEN &&
        demu_state.statefile_fd >= 0) {
        ERR("Statefile was not closed properly!");
        closestate();
    }

    if (demu_state.seq >= DEMU_SEQ_XC_OPEN) {
        DBG("<XC_OPEN");

        xc_interface_close(demu_state.xch);
    }

    if (demu_state.seq >= DEMU_SEQ_XS_OPEN) {
        DBG("<XS_OPEN");

        xs_close(demu_state.xsh);
    }
    demu_state.seq = DEMU_SEQ_UNINITIALIZED;
}

void demu_shutdown()
{
    char dir[sizeof("/var/xen/vgpu/XXXXX")];

    (void) snprintf(dir, sizeof(dir), "/var/xen/vgpu/%d",
                    demu_state.domid);

    demu_teardown();
    (void) rmdir(dir);

    exit(0);
}

static struct sigaction sigterm_handler;

static void demu_sigterm(int num)
{
    DBG("%s", strsignal(num));
    demu_shutdown();
}

static struct sigaction sighup_handler;

static void demu_sighup(int num)
{
    ERR("SIGHUP is no longer used for suspend!");
    demu_sigterm(num);
}


static struct sigaction sigusr1_handler;

static void demu_sigusr1(int num)
{
    DBG("%s", strsignal(num));

    sigaction(SIGUSR1, &sigusr1_handler, NULL);
}

static struct sigaction sigrt_handler;

static void demu_sigrt(int num, siginfo_t * si, void *arg)
{
    int tfd;
    char buf = 'T';

    sigaction(SIGRTMIN, &sigrt_handler, NULL);

    tfd = (int) si->si_value.sival_int;
    write(tfd, &buf, 1);
}

static struct sigaction sigseg_handler;

static void demu_sigseg(int num, siginfo_t * si, void *arg)
{
    ucontext_t *u = (ucontext_t *) arg;
    unsigned char *pc = (unsigned char *) u->uc_mcontext.gregs[REG_RIP];

    signal(num, SIG_DFL);
    fprintf(stderr, "Got SIGSEGV for address 0x%lx from %p\n",
            (long) si->si_addr, pc);
    SET_ERROR(dec_internal);
    demu_teardown();
    raise(num);
}

int init_io()
{
    unsigned long ioreq_pfn;
    unsigned long bufioreq_pfn;
    evtchn_port_t bufioreq_port;
    uint64_t number = 0;
    int rc;
    int i;
    int subcount = 0;
    int first = 1;

    do {
        rc = xc_hvm_param_get(demu_state.xch, demu_state.domid,
                              HVM_PARAM_NR_IOREQ_SERVER_PAGES, &number);

        if (rc < 0) {
            ERR("xc_hvm_param_set no worky");
            SET_ERROR(dec_ioreq);
            return -1;
        }

        if (first || number > 0)
            INFO("HVM_PARAM_NR_IOREQ_SERVER_PAGES = %ld", number);
        first = 0;

        if (number == 0) {
            if (!subcount)
                INFO("Waiting for ioreq server");
            usleep(100000);
            subcount++;
            if (subcount > 10)
                subcount = 0;
        }
    } while (number == 0);

    rc = xc_hvm_create_ioreq_server(demu_state.xch, demu_state.domid, 1,
                                    &demu_state.ioservid);
    if (rc < 0) {
        ERRN("xc_hvm_create_ioreq_server failed");
        SET_ERROR(dec_ioreq);
        return -1;
    }

    demu_seq_next(DEMU_SEQ_SERVER_REGISTERED);

    rc = xc_hvm_get_ioreq_server_info(demu_state.xch, demu_state.domid,
                                      demu_state.ioservid, &ioreq_pfn,
                                      &bufioreq_pfn, &bufioreq_port);
    if (rc < 0) {
        ERR("xc_hvm_get_ioreq_server_info failed with %d", rc);
        SET_ERROR(dec_ioreq);
        return -1;
    }


    demu_state.shared_iopage = xc_map_foreign_range(demu_state.xch,
                               demu_state.domid,
                               XC_PAGE_SIZE,
                               PROT_READ | PROT_WRITE,
                               ioreq_pfn);
    if (demu_state.shared_iopage == NULL) {
        ERRN("shared iopage xc_map_foreign_range");
        SET_ERROR(dec_libxc);
        return -1;
    }

    demu_seq_next(DEMU_SEQ_SHARED_IOPAGE_MAPPED);

    demu_state.buffered_iopage = xc_map_foreign_range(demu_state.xch,
                                 demu_state.domid,
                                 XC_PAGE_SIZE,
                                 PROT_READ |
                                 PROT_WRITE,
                                 bufioreq_pfn);
    if (demu_state.buffered_iopage == NULL) {
        ERRN("shared iopage xc_map_foreign_range");
        SET_ERROR(dec_libxc);
        return -1;
    }

    demu_seq_next(DEMU_SEQ_BUFFERED_IOPAGE_MAPPED);

    rc = xc_hvm_set_ioreq_server_state(demu_state.xch, demu_state.domid,
                                       demu_state.ioservid, 1);
    if (rc < 0) {
        ERR("xc_hvm_set_ioreq_server_state failed with %d", rc);
        SET_ERROR(dec_ioreq);
        return -1;
    }

    demu_seq_next(DEMU_SEQ_SERVER_ENABLED);

    demu_state.ioreq_local_port = malloc(sizeof(evtchn_port_t) *
                                         demu_state.vcpus);
    if (demu_state.ioreq_local_port == NULL) {
        ERRN("ioreq_local_port malloc");
        SET_ERROR(dec_nomem);
        return -1;
    }

    for (i = 0; i < demu_state.vcpus; i++)
        demu_state.ioreq_local_port[i] = -1;

    demu_seq_next(DEMU_SEQ_PORT_ARRAY_ALLOCATED);

    demu_state.xceh = xc_evtchn_open(NULL, 0);
    if (demu_state.xceh == NULL) {
        ERRN("xc_evtchn_open");
        SET_ERROR(dec_libxc);
        return -1;
    }

    demu_seq_next(DEMU_SEQ_EVTCHN_OPEN);

    for (i = 0; i < demu_state.vcpus; i++) {
        evtchn_port_t ioreq_port =
            demu_state.shared_iopage->vcpu_ioreq[i].vp_eport;

        DBG("VCPU%d evtchn %d", i, ioreq_port);

        rc = xc_evtchn_bind_interdomain(demu_state.xceh, demu_state.domid,
                                        ioreq_port);
        if (rc < 0) {
            ERR("xc_evtchn_bind_interdomain failed with %d", rc);
            SET_ERROR(dec_libxc);
            return -1;
        }

        demu_state.ioreq_local_port[i] = rc;

        if (i == 0) {
            DBG("BUF evtchn %d", bufioreq_port);

            rc = xc_evtchn_bind_interdomain(demu_state.xceh,
                                            demu_state.domid,
                                            bufioreq_port);
            if (rc < 0) {
                ERR("xc_evtchn_bind_interdomain (buf) failed with %d",
                    rc);
                SET_ERROR(dec_libxc);
                return -1;
            }

            demu_state.bufioreq_local_port = rc;
        }
    }

    demu_seq_next(DEMU_SEQ_PORTS_BOUND);

    return 0;
}

int enable_io()
{
    int rc;
    demu_space_t *space;

    demu_state.io_up = 1;

    for (space = demu_state.pci_config; space != NULL; space = space->next) {
        if (space->io_init == 0) {
            rc = xc_hvm_map_pcidev_to_ioreq_server(demu_state.xch,
                                                   demu_state.domid,
                                                   demu_state.ioservid, 0,
                                                   demu_state.bus,
                                                   demu_state.device,
                                                   demu_state.function);
            if (rc < 0) {
                ERR("xc_hvm_map_pcidev_to_ioreq_server failed, %d", rc);
                return -1;
            }
            space->io_init = 1;
        }
    }

    for (space = demu_state.memory; space != NULL; space = space->next) {
        if (space->io_init == 0) {
            rc = xc_hvm_map_io_range_to_ioreq_server(demu_state.xch,
                    demu_state.domid,
                    demu_state.ioservid,
                    1, space->start,
                    space->end);
            if (rc < 0) {
                ERR("xc_hvm_map_io_range_to_ioreq_server failed with  memory, %d", rc);
                return -1;
            }
            space->io_init = 1;
        }
    }

    for (space = demu_state.port; space != NULL; space = space->next) {
        if (space->io_init == 0) {
            rc = xc_hvm_map_io_range_to_ioreq_server(demu_state.xch,
                    demu_state.domid,
                    demu_state.ioservid,
                    0, space->start,
                    space->end);
            if (rc < 0) {
                ERR("xc_hvm_map_io_range_to_ioreq_server failed with port, %d", rc);
                return -1;
            }
            space->io_init = 1;
        }
    }
    return 0;
}


static int
demu_initialize(domid_t domid, unsigned int vcpus,
                unsigned int bus, unsigned int device,
                unsigned int function, const char *gpu,
                const char *config)
{
    int rc;
    vmiop_error_t error_code;
    vmiop_handle_t handle;
    xc_dominfo_t dominfo;
    char key[keysize];
    char value[sizeof("XXXXXXXXXXXXXXXX")];

    demu_state.domid = domid;
    demu_state.vcpus = vcpus;
    demu_state.bus = bus;
    demu_state.device = device;
    demu_state.function = function;
    demu_state.vram_addr = VRAM_RESERVED_ADDRESS;
    demu_state.statefile_fd = -1;
    demu_state.statefile_mode = SEC_NOTREADY;
    demu_state.io_up = 0;

    pthread_mutex_init(&sent_stats.lock, NULL);

    (void) snprintf(demu_state.config, sizeof(demu_state.config),
                    "%s,gpu-pci-id=%s", config, gpu);

    demu_state.xsh = xs_open(0);
    if (demu_state.xsh == NULL) {
        ERRN("xs_open");
        return -1;
    }

    set_demu_status("initialising");

    gen_key(key, xenstore_pid_str);
    (void) snprintf(value, sizeof(value), "%d", getpid());

    if (!xs_write(demu_state.xsh, 0, key, value, strlen(value))) {
        ERRN("xs_write");
        SET_ERROR(dec_xs);
        return -1;
    }

    demu_seq_next(DEMU_SEQ_XS_OPEN);

    demu_state.xch = xc_interface_open(NULL, NULL, 0);
    if (demu_state.xch == NULL) {
        ERRN("xc_interface_open");
        SET_ERROR(dec_libxc);
        return -1;
    }

    demu_seq_next(DEMU_SEQ_XC_OPEN);

    rc = xc_domain_getinfo(demu_state.xch, demu_state.domid, 1, &dominfo);
    if (rc < 0 || dominfo.domid != demu_state.domid) {
        ERR("xc_domain_getinfo failed with %d", rc);
        if (dominfo.domid != demu_state.domid)
            ERR(" dominfo mismatch.  Got domid %d expected %d",
                dominfo.domid, demu_state.domid);
        SET_ERROR(dec_libxc);
        return -1;
    }

    error_code = vmiope_initialize();
    if (error_code != vmiop_success) {
        ERR("vmiope_initialize failed with %d", error_code);
        SET_ERROR(dec_vmiop);
        return -1;
    }

    demu_seq_next(DEMU_SEQ_VMIOP_ENV_INITIALIZED);

    error_code = vmiop_vga_init();
    if (error_code != vmiop_success) {
        ERR("vmiop_vga_init failed with %d", error_code);
        SET_ERROR(dec_vmiop);
        return -1;
    }

    demu_seq_next(DEMU_SEQ_VMIOP_VGA_INITIALIZED);

    error_code = vmiope_register_plugin(&vmiop_presentation, &handle);
    if (error_code != vmiop_success) {
        ERR("vmiope_register_plugin failed with %d", error_code);
        SET_ERROR(dec_vmiop);
        return -1;
    }

    demu_state.presentation_handle = handle;

    error_code = vmiope_set_search_path(DEFAULT_VMIOPLUGIN_SEARCH_PATH);
    if (error_code != vmiop_success) {
        ERR("vmiope_set_search_path failed with %d", error_code);
        SET_ERROR(dec_vmiop);
        return -1;
    }

    INFO("PLUGIN CONFIG: %s", demu_state.config);

    error_code = vmiope_process_configuration(demu_state.config);
    if (error_code != vmiop_success) {
        ERR("vmiope_process_configuration failed with %d", error_code);
        SET_ERROR(dec_vmiop);
        return -1;
    }

    vmiope_leave_monitor(NULL);


    demu_seq_next(DEMU_SEQ_VMIOP_PLUGINS_REGISTERED);

    if (demu_resuming) {
        set_demu_status("resuming");
        if (demu_get_state()) {
            ERR("Failed to process state");
            SET_ERROR(dec_badstate);
            return -1;
        }

        DBG("Device state loaded");
    }

    if (init_io()) {
        ERR("Failed to init IO");
        return -1;
    }

    if (enable_io()) {
        ERR("Failed to enable IO");
        return -1;
    }

    if (demu_resuming) {
        INFO("Sending vmiop_migration_resume");
        vmiope_notify_device(VMIOP_HANDLE_NULL, vmiop_migration_resume);
    }

    vmiope_enter_monitor(NULL);

    demu_state.vram = demu_map_guest_range(demu_state.vram_addr,
                                           VRAM_RESERVED_SIZE,
                                           0, demu_resuming ? 0 : 1);
    if (demu_state.vram == NULL) {
        ERRN("vram demu_map_guest_range");
        SET_ERROR(dec_libxc);
        return -1;
    }

    if (!demu_resuming)
        memset(demu_state.vram, 0, VRAM_RESERVED_SIZE);

    /* resuming stage is over */
    demu_resuming = not_resuming;

    demu_seq_next(DEMU_SEQ_VRAM_MAPPED);

    rc = demu_socket_create();
    if (rc < 0) {
        ERR("socket creation failed with %d", rc);
        SET_ERROR(dec_socket);
        return -1;
    }

    demu_seq_next(DEMU_SEQ_SOCKET_CREATED);

    rc = surface_initialize();
    if (rc < 0) {
        ERR("surface_initialize failed with %d", rc);
        return -1;
    }
    demu_seq_next(DEMU_SEQ_SURFACE_INITIALIZED);

    gen_key(key, xenstore_vram_str);
    (void) snprintf(value, sizeof(value), "%jX", demu_state.vram_addr);

    if (!xs_write(demu_state.xsh, 0, key, value, strlen(value))) {
        ERRN("xs_write vram_addr");
        SET_ERROR(dec_xs);
        return -1;
    }

    demu_seq_next(DEMU_SEQ_INITIALIZED);

    set_demu_status("running");

    assert(demu_state.seq == DEMU_SEQ_INITIALIZED);
    return 0;
}

uint8_t *demu_get_vram(void)
{
    return demu_state.vram;
}

void demu_set_vram_addr(uint64_t vram_addr)
{
    int rc;

    DBG("%" PRIx64 " -> %" PRIx64 "", demu_state.vram_addr, vram_addr);

    if (demu_resuming)
        demu_state.vram_addr = vram_addr;


    if (demu_state.vram_addr == vram_addr)
        return;

    rc = demu_relocate_guest_range(demu_state.vram_addr,
                                   vram_addr, VRAM_RESERVED_SIZE);
    if (rc < 0)
        goto fail1;

    demu_state.vram_addr = vram_addr;

    (void) xc_hvm_track_dirty_vram(demu_state.xch, demu_state.domid,
                                   vram_addr >> TARGET_PAGE_SHIFT,
                                   VRAM_ACTUAL_SIZE >> TARGET_PAGE_SHIFT,
                                   NULL);
    device_update_lfb_addr();
    demu_state_dirty = 1;

    return;

fail1:
    ERR("fail1: %s", strerror(errno));
}

uint64_t demu_get_vram_addr(void)
{
    return demu_state.vram_addr;
}

void demu_sync_vram_dirty_map(void)
{
    xen_pfn_t pfn = demu_state.vram_addr >> TARGET_PAGE_SHIFT;
    const unsigned int n = VRAM_ACTUAL_SIZE >> TARGET_PAGE_SHIFT;
    unsigned long map[n / (sizeof(unsigned long) * 8)];
    unsigned int i;
    int rc;

    rc = xc_hvm_track_dirty_vram(demu_state.xch, demu_state.domid,
                                 pfn, n, map);
    if (rc < 0)
        memset(map, 0xff, n / 8);

    for (i = 0; i < n / (sizeof(unsigned long) * 8); i++)
        demu_state.vram_dirty_map[i] |= map[i];
}

#define ARRAY_SIZE(_a) (sizeof (_a) / sizeof ((_a)[0]))

int demu_vram_get_page_dirty(xen_pfn_t pfn)
{
    uint64_t sel = pfn / (sizeof(unsigned long) * 8);
    uint64_t bit = pfn % (sizeof(unsigned long) * 8);
    int dirty;

    assert(sel < ARRAY_SIZE(demu_state.vram_dirty_map));
    dirty = ! !(demu_state.vram_dirty_map[sel] & (1ul << bit));

    return dirty;
}

void demu_vram_set_page_dirty(xen_pfn_t pfn)
{
    uint64_t sel = pfn / (sizeof(unsigned long) * 8);
    uint64_t bit = pfn % (sizeof(unsigned long) * 8);

    assert(sel < ARRAY_SIZE(demu_state.vram_dirty_map));
    demu_state.vram_dirty_map[sel] |= (1ul << bit);
}

void demu_clear_vram_dirty_map(void)
{
    const unsigned int n = VRAM_ACTUAL_SIZE >> TARGET_PAGE_SHIFT;

    memset(demu_state.vram_dirty_map, 0, n / 8);
}

static void demu_poll_buffered_iopage(void)
{
    DBG_V("demu_poll_buffered_iopages");

    if (demu_state.seq != DEMU_SEQ_INITIALIZED)
        return;

    for (;;) {
        unsigned int read_pointer;
        unsigned int write_pointer;

        read_pointer = demu_state.buffered_iopage->read_pointer;
        write_pointer = demu_state.buffered_iopage->write_pointer;
        mb();

        if (read_pointer == write_pointer)
            break;

        while (read_pointer != write_pointer) {
            unsigned int slot;
            buf_ioreq_t *buf_ioreq;
            ioreq_t ioreq;

            slot = read_pointer % IOREQ_BUFFER_SLOT_NUM;

            buf_ioreq = &demu_state.buffered_iopage->buf_ioreq[slot];

            ioreq.size = 1UL << buf_ioreq->size;
            ioreq.count = 1;
            ioreq.addr = buf_ioreq->addr;
            ioreq.data = buf_ioreq->data;
            ioreq.state = STATE_IOREQ_READY;
            ioreq.dir = buf_ioreq->dir;
            ioreq.df = 1;
            ioreq.type = buf_ioreq->type;
            ioreq.data_is_ptr = 0;

            read_pointer++;

            if (ioreq.size == 8) {
                slot = read_pointer % IOREQ_BUFFER_SLOT_NUM;
                buf_ioreq = &demu_state.buffered_iopage->buf_ioreq[slot];

                ioreq.data |= ((uint64_t) buf_ioreq->data) << 32;

                read_pointer++;
            }

            demu_handle_ioreq(&ioreq);
            mb();
        }

        demu_state.buffered_iopage->read_pointer = read_pointer;
        mb();
    }
}

static void demu_poll_shared_iopage(unsigned int i)
{
    ioreq_t *ioreq;

    ioreq = &demu_state.shared_iopage->vcpu_ioreq[i];
    if (ioreq->state != STATE_IOREQ_READY)
        return;

    mb();

    ioreq->state = STATE_IOREQ_INPROCESS;

    demu_handle_ioreq(ioreq);
    mb();

    ioreq->state = STATE_IORESP_READY;
    mb();

    xc_evtchn_notify(demu_state.xceh, demu_state.ioreq_local_port[i]);
}

static void demu_poll_iopages(void)
{
    evtchn_port_t port;
    int i;

    if (demu_state.seq != DEMU_SEQ_INITIALIZED)
        return;

    port = xc_evtchn_pending(demu_state.xceh);
    if (port < 0)
        return;

    if (port == demu_state.bufioreq_local_port) {
        xc_evtchn_unmask(demu_state.xceh, port);
        demu_poll_buffered_iopage();
    } else {
        for (i = 0; i < demu_state.vcpus; i++) {
            if (port == demu_state.ioreq_local_port[i]) {
                xc_evtchn_unmask(demu_state.xceh, port);
                demu_poll_shared_iopage(i);
            }
        }
    }
}

#define MAX_SPAM     6
#define MAX_SPAM_LEN 80

static int dbg_buff_sz = 0;
static int dbg_buff_pos = 0;

char db_buff[MAX_SPAM][MAX_SPAM_LEN];

void demu_log_context(const char *prefix, const char *fmt, ...)
{
    int rc;
    char msg[MAX_SPAM_LEN];
    va_list ap;

    if (dbg_buff_sz <= MAX_SPAM)
        dbg_buff_sz++;

    va_start(ap, fmt);
    rc = vsnprintf(msg, MAX_SPAM_LEN, fmt, ap);

    va_end(ap);

    if (rc < 0)
        return;

    snprintf(db_buff[dbg_buff_pos], MAX_SPAM_LEN, "%s: %s", prefix, msg);

    dbg_buff_pos++;
    if (dbg_buff_pos == MAX_SPAM)
        dbg_buff_pos = 0;
}

void demu_dump_context()
{
    int pos;

    if (dbg_buff_sz > MAX_SPAM) {
        syslog(LOG_DEBUG, "...");
        dbg_buff_sz--;
    }

    for (pos = dbg_buff_pos; dbg_buff_sz > 0; dbg_buff_sz--) {
        if (pos == 0)
            pos = MAX_SPAM;
        pos--;
        syslog(LOG_DEBUG, db_buff[pos]);
    }
}

void
demu_vlog(int priority, const char *prefix, const char *fmt, va_list ap)
{
    char *msg;
    int rc;

    rc = vasprintf(&msg, fmt, ap);
    if (rc < 0)
        return;

    syslog(priority, "%s: %s", prefix, msg);

    free(msg);
}


void demu_log(int priority, const char *prefix, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    if (dbg_buff_sz)
        demu_dump_context();

    demu_vlog(priority, prefix, fmt, ap);
    va_end(ap);
}

#define DEMU_DEVICE     11


static int max(int a, int b, int c, int d)
{
    int m1 = (a > b) ? a : b;
    int m2 = (c > d) ? c : d;
    return (m1 > m2) ? m1 : m2;
}

struct demu_args {
    domid_t domid;
    unsigned int vcpus;
    unsigned int device;

    char *gpu_str;
    char *config_str;
};

void get_uint_arg(unsigned int *val, char **strval, char *arg, int *err,
                  char *name)
{
    char *end;

    if (*strval)
        ERR("Can't set %s to %s as already set to %s", name, arg, strval);
    else {
        *strval = arg;
        *val = (unsigned int) strtol(arg, &end, 0);
        if (!end || *end != '\0')
            ERR("Bad value '%s' for %s", arg, name);
        else
            return;
    }
    (*err)++;
}

int process_args(int argc, char **argv, char **envp, struct demu_args *a)
{
    char *domain_str = NULL;
    char *vcpus_str = NULL;
    char *device_str = NULL;

    int index;
    int badargs = 0;

    a->gpu_str = NULL;
    a->config_str = NULL;

    prog = basename(argv[0]);

    for (;;) {
        char c;

        c = getopt_long(argc, argv, "", demu_option, &index);

        if (c == -1)
            break;

        assert(c == 0);
        switch (index) {
        case DEMU_OPT_DOMAIN:
            get_uint_arg((unsigned int *) &a->domid, &domain_str, optarg,
                         &badargs, "domid");
            break;

        case DEMU_OPT_VCPUS:
            get_uint_arg(&a->vcpus, &vcpus_str, optarg, &badargs, "vcpus");
            break;

        case DEMU_OPT_GPU:
            a->gpu_str = optarg;
            break;

        case DEMU_OPT_CONFIG:
            a->config_str = optarg;
            break;

        case DEMU_OPT_DEVICE:
            get_uint_arg(&a->device, &device_str, optarg, &badargs,
                         "device");
            break;

        case DEMU_OPT_RESUME:
            demu_resuming = set_to_resume;
            break;

        case DEMU_OPT_SUSPEND:
            INFO("Suspend arg ignored");
            break;

        default:
            assert(false);
            break;
        }
    }

    if (domain_str == NULL ||
            vcpus_str == NULL || a->gpu_str == NULL || a->config_str == NULL) {
        usage();
        /*NOTREACHED*/
    }

    if (device_str == NULL) {
        a->device = DEMU_DEVICE;
    }

    if (badargs)
        return -1;

    return 0;
}


void set_sigactions(void)
{
    sigset_t block;

    sigfillset(&block);

    memset(&sigseg_handler, 0, sizeof(struct sigaction));
    sigseg_handler.sa_flags = SA_SIGINFO;
    sigseg_handler.sa_sigaction = demu_sigseg;

    sigaction(SIGSEGV, &sigseg_handler, NULL);
    sigdelset(&block, SIGSEGV);

    memset(&sigterm_handler, 0, sizeof(struct sigaction));
    sigterm_handler.sa_handler = demu_sigterm;

    memset(&sighup_handler, 0, sizeof(sighup_handler));
    sighup_handler.sa_handler = demu_sighup;

    sigaction(SIGTERM, &sigterm_handler, NULL);
    sigdelset(&block, SIGTERM);

    sigaction(SIGINT, &sigterm_handler, NULL);
    sigdelset(&block, SIGINT);

    sigaction(SIGHUP, &sighup_handler, NULL);
    sigdelset(&block, SIGHUP);

    memset(&sigusr1_handler, 0, sizeof(struct sigaction));
    sigusr1_handler.sa_handler = demu_sigusr1;

    sigaction(SIGUSR1, &sigusr1_handler, NULL);
    sigdelset(&block, SIGUSR1);

    memset(&sigrt_handler, 0, sizeof(struct sigaction));
    sigrt_handler.sa_flags = SA_SIGINFO;
    sigrt_handler.sa_sigaction = demu_sigrt;

    sigaction(SIGRTMIN, &sigrt_handler, NULL);
    sigdelset(&block, SIGRTMIN);

    sigprocmask(SIG_BLOCK, &block, NULL);
}

static void emp_log(enum emp_log_level level, const char *msg)
{
    int syslog_level;

#ifndef VERBOSE
    if (level <= emp_level_dbg)
        return;
#endif

    switch (level) {
    case emp_level_dbg:
        syslog_level = LOG_DEBUG;
        break;
    case emp_level_info:
        syslog_level = LOG_INFO;
        break;
    case emp_level_warn:
        syslog_level = LOG_WARNING;
        break;
    case emp_level_err:
        syslog_level = LOG_ERR;
        break;
    default:
        abort();
    }

    demu_log(syslog_level, "libempserver", "%s", msg);
}

int main(int argc, char **argv, char **envp)
{
    struct demu_args args;
    char *ident;

    char dir[sizeof("/var/xen/vgpu/XXXXX")];
    struct rlimit rlim;

    int evtchn_fd;
    int refresh_sock_fd;
    int timer_fd;
    int rc;

    openlog(NULL, LOG_PID, LOG_DAEMON);

    INFO("demu started");

    rc = process_args(argc, argv, envp, &args);
    if (rc)
        goto fail1;

    rc = asprintf(&ident, "%s-%d", prog, args.domid);
    if (rc > 0)
        openlog(ident, LOG_PID, LOG_DAEMON);

    emp_set_log_cb(emp_log);

    (void) snprintf(dir, sizeof(dir), "/var/xen/vgpu");
    (void) mkdir(dir, S_IRWXU | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

    (void) snprintf(dir, sizeof(dir), "/var/xen/vgpu/%d", args.domid);
    (void) mkdir(dir, S_IRWXU | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

    INFO("working directory: %s", dir);
    if (chdir(dir) < 0) {
        INFO("Could not change into %s", dir);
        goto fail2;
    }

    rlim.rlim_cur = rlim.rlim_max = 64 * 1024 * 1024;
    setrlimit(RLIMIT_CORE, &rlim);
    prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);

    set_sigactions();

    if (demu_control_sock_init(&demu_state.cs_inf))
        goto fail3;

    rc = demu_initialize(args.domid, args.vcpus, 0, args.device, 0,
                         args.gpu_str, args.config_str);
    if (rc < 0)
        goto fail4;

    evtchn_fd = xc_evtchn_fd(demu_state.xceh);
    assert(evtchn_fd > 0);

    refresh_sock_fd = demu_state.sfd;
    assert(refresh_sock_fd > 0);

    timer_fd = demu_timer_create();
    if (timer_fd < 0) {
        SET_ERROR(dec_internal);
        goto fail5;
    }

    /* Main Loop */
    for (;;) {
        fd_set rfds;
        fd_set wfds;
        fd_set xfds;
        int nfds;
        struct timeval tv;

        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_ZERO(&xfds);

        FD_SET(evtchn_fd, &rfds);
        FD_SET(refresh_sock_fd, &rfds);
        FD_SET(timer_fd, &rfds);

        tv.tv_sec = 10;
        tv.tv_usec = 0;

        nfds = emp_select_fdset(demu_state.cs_inf, &rfds, NULL);

        nfds = max(nfds, evtchn_fd, refresh_sock_fd, timer_fd) + 1;

        vmiope_leave_monitor(NULL);
        rc = select(nfds, &rfds, &wfds, &xfds, &tv);
        vmiope_enter_monitor(NULL);

        if (rc < 0 && errno != EINTR)
            break;

        if (rc > 0) {
            if (FD_ISSET(evtchn_fd, &rfds)) {

                demu_poll_iopages();
                rc--;
            }
            if (FD_ISSET(timer_fd, &rfds)) {
                char buf;

                read(timer_fd, &buf, 1);
                demu_console_refresh();
                rc--;
            }
            if (FD_ISSET(refresh_sock_fd, &rfds)) {
                demu_socket_read();
                rc--;
            }

            if (rc > 0)
                rc = emp_select_fdread(demu_state.cs_inf, &rfds, rc);

            if (rc > 0)
                DBG("Warning: expected to find %d more fcs.", rc);
        }
    }

    demu_timer_destroy();

fail5:
fail4:
    demu_teardown();
    demu_control_sock_close(&demu_state.cs_inf);
fail3:
fail2:
fail1:
    return 1;
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
