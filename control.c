/*
 * Copyright (c) 2017, Citrix Systems Inc.
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

#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <time.h>
#include <demu.h>
#include <semaphore.h>
#include <inttypes.h>

#include "log.h"
#include "control.h"

#include <vmiop-env.h>

#define SOCKET_NAME "./control"

enum mig_state
{
    mig_not_init = 0,
    mig_init,
    mig_track,
    mig_live,
    mig_paused,
    mig_restore
};

static struct
{
    int paused;
    enum mig_state mig;
} runstate;

static sem_t sem_stopped;

struct emu_client progress_cli;
struct emu_client initiator_cli;

void send_migrate_progress(uint64_t sent, uint64_t remaining)
{
    if (progress_cli.num >= 0)
        emp_send_event_migrate_progress(progress_cli, sent / 4096,
                                        remaining / 4096, -1);
}

void do_migrate_init(emp_call_args *args)
{
    enum mig_state mig;
    int fd = args->fd;

    if (fd < 0) {
        emp_send_error(args->cli, "need fd");
        INFO("Init without FD");
        return;
    }

    mig = runstate.mig;
    if (mig == mig_not_init)
        runstate.mig = mig_init;
    else {
        emp_send_error(args->cli, "busy");
        close(fd);
        return;
    }

    if (demu_init_migrate(fd)) {
        emp_send_error(args->cli, "bad fd");
        runstate.mig = mig_not_init;
        INFO("Init with bad FD");
        return;
    }
    emp_send_return(args->cli, NULL);
}

void migrate_loop(int live)
{
    int r;
    /* for non-live case avoid any stop checks */
    int stopped = !live;

    r = demu_start_migrate();
    if (r)
        ERR("Failed to start migrate");
    else {
        r = 1;
        INFO("Start migrate loop");
        while (r > 0) {
            if (!stopped) {
                if ((r = sem_getvalue(&sem_stopped, &stopped)) < 0)
                    break;
                if (stopped)
                    INFO("Received pause message");
            }

            r = demu_migrate();

            /* Most likely stopped message came while in demu_migrate
             * But vga may have changed, before stopped, so should try
             * one last time.  Unlikely to actually wait on semaphore.
             */

            if ((r == 0) && !stopped) {
                INFO("Wait for pause");
                if ((r = sem_wait(&sem_stopped)) < 0)
                    break;
                stopped = 1;
                INFO("Paused - try once more.");
                r = demu_migrate();
            }
        }
        INFO("Loop complete");
        if (r >= 0) {
            r = demu_finish_migrate();
            if (r)
                ERR("Failed to finish migrate");
        } else
            ERRN("During migrate");
    }
    demu_migrate_cleanup();
    if (progress_cli.num >= 0)
        emp_send_event_migrate_completed(progress_cli,
                                         (r) ? migration_failed :
                                         migration_success);
    if (progress_cli.num != initiator_cli.num)
        emp_send_event_migrate_completed(initiator_cli,
                                         (r) ? migration_failed :
                                         migration_success);
}

int check_mig(emp_call_args *args)
{
    enum mig_state mig = runstate.mig;

    if (mig == mig_not_init) {
        emp_send_error(args->cli, "init first");
        return -1;
    } else if (mig > mig_track) {
        emp_send_error(args->cli, "busy");
        return -1;
    }

    return 0;
}

void do_migrate_live(emp_call_args *args)
{
    if (check_mig(args))
        return;

    if (sem_init(&sem_stopped, 0, 0)) {
        emp_send_error(args->cli, "internal");
        ERRN("Re-init Migration semaphore");
        return;
    }
    initiator_cli = args->cli;
    runstate.mig = mig_live;

    emp_unlock();

    emp_send_return(args->cli, NULL);

    migrate_loop(true);

    emp_lock();
    runstate.mig = mig_not_init;
    runstate.paused = 0;
    emp_unlock();
}

void do_migrate_nonlive(emp_call_args *args)
{
    if (check_mig(args))
        return;

    if (!runstate.paused) {
        emp_send_error(args->cli, "must be paused");
        return;
    }

    runstate.mig = mig_paused;
    initiator_cli = args->cli;

    emp_unlock();

    emp_send_return(args->cli, NULL);

    migrate_loop(false);

    emp_lock();
    runstate.mig = mig_not_init;
    runstate.paused = 0;
    emp_unlock();
}

void do_migrate_pause(emp_call_args *args)
{
    int r = 0;

    /* started with runstate locked */
    r = runstate.paused;
    if (r) {
        emp_send_error(args->cli, "already paused");
        return;
    }
    runstate.paused = 1;
    sem_post(&sem_stopped);

    r = demu_migrate_phase_stop();
    if (r) {
        ERR("demu_migrate_phase_stop failed");
        emp_send_error(args->cli, "stop error");
        return;
    }

    emp_send_return(args->cli, NULL);
}

void do_cmd_progress(emp_call_args *args)
{
    progress_cli = args->cli;
    INFO("setting progress_cli to %d", progress_cli.num);
    emp_send_return(args->cli, NULL);
    return;
}

void do_track_dirty(emp_call_args *args)
{
    int mig;

    /* started with runstate locked */

    mig = runstate.mig;

    if (mig == mig_init)
        runstate.mig = mig_track;

    if ((mig == mig_not_init) || (mig > mig_track)) {
        ERR("Bad state to set track_dirty");
        emp_send_error(args->cli, "bad state");
    } else {
        INFO("Sending vmiop_migration_track_modified  %d",
             progress_cli.num);
        vmiope_notify_device(VMIOP_HANDLE_NULL,
                             vmiop_migration_track_modified);
        emp_send_return(args->cli, NULL);
    }
    return;
}

static void do_mig_abort(emp_call_args *args)
{
    enum mig_state mig;
    int paused;

    INFO("Received abort cmd");
    mig = runstate.mig;
    paused = runstate.paused;
    if (mig == mig_live) {
        demu_migrate_abort();

        /* thread could be waiting for pause */
        if (!paused)
            sem_post(&sem_stopped);
    }
    if (mig > mig_init)
        demu_migrate_cleanup();

    if (paused)
        runstate.paused = 0;

    /* If migrating, vmiope_notify_device is called when done
       Otherwise we can call it now */
    if (mig == mig_not_init) {
        if (paused) {
            INFO("Sending vmiop_migration_resume");
            vmiope_notify_device(VMIOP_HANDLE_NULL, vmiop_migration_resume);
        }
        emp_send_error(args->cli, "not in progress");
    } else
        emp_send_return(args->cli, NULL);
}

void do_cmd_quit(emp_call_args *args)
{

    if (runstate.mig == mig_not_init) {
        emp_send_return(args->cli, NULL);
        demu_shutdown();
    } else
        emp_send_error(args->cli, "busy");

    return;
}

void ignore(emp_call_args *args)
{
    emp_send_return(args->cli, NULL);
    return;
}

void do_not_supported(emp_call_args *args)
{
    emp_send_error(args->cli, "not supported");
    return;
}

void do_cmd_restore(emp_call_args *args)
{
    int r;

    if (check_mig(args))
        return;

    runstate.mig = mig_restore;
    initiator_cli = args->cli;

    r = demu_trigger_resume();

    if (r) {
        emp_send_error(args->cli, "bad state");

        runstate.mig = mig_not_init;
        ERR("restore failed to start");
    } else
        emp_send_return(args->cli, NULL);
}

void report_resume_done(enum emp_migration_status status)
{
    runstate.mig = mig_not_init;

    if (progress_cli.num >= 0)
        emp_send_event_migrate_completed(progress_cli, status);
    if (progress_cli.num != initiator_cli.num)
        emp_send_event_migrate_completed(initiator_cli, status);
}

struct command_actions demu_actions[] = {
    {cmd_track_dirty,      &do_track_dirty,     0},
    {cmd_migrate_abort,    &do_mig_abort,       0},
    {cmd_migrate_init,     &do_migrate_init,    0},
    {cmd_migrate_live,     &do_migrate_live,    1},
    {cmd_migrate_pause,    &do_migrate_pause,   0},
    {cmd_migrate_paused,   &ignore,             0},
    {cmd_migrate_progress, &do_cmd_progress,    0},
    {cmd_migrate_nonlive,  &do_migrate_nonlive, 1},
    {cmd_quit,             &do_cmd_quit,        0},
    {cmd_restore,          &do_cmd_restore,     0},
    {cmd_set_args,         &do_not_supported,   0}
};

int demu_control_sock_init(struct emp_sock_inf **inf)
{
    runstate.paused = 0;
    runstate.mig = mig_not_init;
    progress_cli.num = -1;
    initiator_cli.num = -1;

    return emp_sock_init(SOCKET_NAME, inf, demu_actions);
}

int demu_control_sock_close(struct emp_sock_inf **inf)
{
    int r = emp_sock_close(inf);
    runstate.mig = mig_not_init;
    return r;
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
