/* Copyright (C) 2007-2012 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 * \author Eric Leblond <eric@regit.org>
 *
 * Thread management functions.
 */

#include "suricata-common.h"
#include "suricata.h"
#include "stream.h"
#include "runmodes.h"
#include "threadvars.h"
#include "tm-queues.h"
#include "tm-queuehandlers.h"
#include "tm-threads.h"
#include "tmqh-packetpool.h"
#include "threads.h"
#include "util-debug.h"
#include "util-privs.h"
#include "util-cpu.h"
#include "util-optimize.h"
#include "util-profiling.h"
#include "util-signal.h"
#include "queue.h"

#ifdef PROFILE_LOCKING
__thread uint64_t mutex_lock_contention;
__thread uint64_t mutex_lock_wait_ticks;
__thread uint64_t mutex_lock_cnt;

__thread uint64_t spin_lock_contention;
__thread uint64_t spin_lock_wait_ticks;
__thread uint64_t spin_lock_cnt;

__thread uint64_t rww_lock_contention;
__thread uint64_t rww_lock_wait_ticks;
__thread uint64_t rww_lock_cnt;

__thread uint64_t rwr_lock_contention;
__thread uint64_t rwr_lock_wait_ticks;
__thread uint64_t rwr_lock_cnt;
#endif

#ifdef OS_FREEBSD
#include <sched.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/cpuset.h>
#include <sys/thr.h>
#define cpu_set_t cpuset_t
#endif /* OS_FREEBSD */

#ifdef __tilegx__
#include "source-mpipe.h"
#include <gxio/mica.h>
#include <tmc/task.h>

#define VERIFY(VAL, WHAT)                                       \
  do {                                                          \
    int __val = (VAL);                                          \
    if (__val < 0)                                              \
      tmc_task_die("Failure in '%s': %d: %s.",                  \
                   (WHAT), __val, gxio_strerror(__val));        \
  } while (0)

#endif

/* prototypes */
static int SetCPUAffinity(uint16_t cpu);
#ifdef __tile__
static void *TmThreadsThreadWrap(void *td);
#endif

/* root of the threadvars list */
ThreadVars *tv_root[TVT_MAX] = { NULL };

/* lock to protect tv_root */
#ifdef __tile__
SCMutex tv_root_lock = TMC_SPIN_QUEUED_MUTEX_INIT;
#else
SCMutex tv_root_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

/* Action On Failure(AOF).  Determines how the engine should behave when a
 * thread encounters a failure.  Defaults to restart the failed thread */
uint8_t tv_aof = THV_RESTART_THREAD;

void TmThreadExchange(ThreadVars *otv, ThreadVars *ntv, int type);

/**
 * \brief Check if a thread flag is set.
 *
 * \retval 1 flag is set.
 * \retval 0 flag is not set.
 */
int TmThreadsCheckFlag(ThreadVars *tv, uint16_t flag)
{
    return (SC_ATOMIC_GET(tv->flags) & flag) ? 1 : 0;
}

/**
 * \brief Set a thread flag.
 */
void TmThreadsSetFlag(ThreadVars *tv, uint16_t flag)
{
    SC_ATOMIC_OR(tv->flags, flag);
}

/**
 * \brief Unset a thread flag.
 */
void TmThreadsUnsetFlag(ThreadVars *tv, uint16_t flag)
{
    SC_ATOMIC_AND(tv->flags, ~flag);
}

/**
 * \brief Clone ThreadVars.  On Tilera new ThreadVars is
 * cached only on thread's tile and creates an mspace
 * for future per thread allocations.
 */
static ThreadVars *TmCloneThreadVars(ThreadVars *td)
{
    ThreadVars *tv;

#ifdef __tile__
    tmc_alloc_t attr = TMC_ALLOC_INIT;
    tmc_alloc_set_home(&attr, TMC_ALLOC_HOME_TASK);
    td->mspace = tmc_mspace_create_special(64*1024, 0, &attr);
    tv = SCThreadMalloc(td, sizeof(ThreadVars));
    if (tv == NULL) {
        printf("ERror: TmTCloneThreadVars could not clone ThreadVars\n");
        exit(EXIT_FAILURE);
    }
    TmThreadsSetFlag((ThreadVars *)td, THV_INIT_DONE);
    memcpy(tv, td, sizeof(ThreadVars));
    TmThreadExchange(td, tv, td->type);
/*
    SCFree(td);
*/
#else
    tv = td;
#endif
    return tv;
}

#ifdef __tile__
static void *TmThreadsThreadWrap(void *td)
{
    int result;
    extern int mica_memcpy_enabled;
    extern void *tile_packet_page;
    extern unsigned long tile_vhuge_size;
    ThreadVars *tv = (ThreadVars *)td;
    TmThreadAppend(tv, tv->type);
    tv = TmCloneThreadVars((ThreadVars *)td);
    if (mica_memcpy_enabled) {
        result = gxio_mica_init(&tv->mica_cb, GXIO_MICA_ACCEL_CRYPTO, 0);
        VERIFY(result, "gxio_mica_init");
        /* Register the page with MiCA so that it can perform DMA operations
         * to and from this memory.
         */
        result = gxio_mica_register_page(&tv->mica_cb, tile_packet_page, tile_vhuge_size, 0);
        VERIFY(result, "gxio_mica_register_page()");
    }
    if (!((tv->thread_setup_flags == THREAD_SET_AFFTYPE) && 
          (tv->cpu_affinity ==MANAGEMENT_CPU_SET))) {
        tmc_sync_barrier_wait(&startup_barrier);
    }
    return (*tv->tm_func)(tv);
}
#endif

/**
 * \brief Function to use as dummy stack function
 *
 * \retval TM_ECODE_OK
 */
TmEcode TmDummyFunc(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    return TM_ECODE_OK;
}

/* 1 slot functions */
void *TmThreadsSlot1NoIn(void *td)
{
    /* block usr2.  usr2 to be handled by the main thread only */
    UtilSignalBlock(SIGUSR2);

    ThreadVars *tv = (ThreadVars *)td;
    TmSlot *s = (TmSlot *)tv->tm_slots;
    char run = 1;
    TmEcode r = TM_ECODE_OK;

    /* Set the thread name */
    if (SCSetThreadName(tv->name) < 0) {
        SCLogWarning(SC_ERR_THREAD_INIT, "Unable to set thread name");
    }

    if (tv->thread_setup_flags != 0)
        TmThreadSetupOptions(tv);

    /* Drop the capabilities for this thread */
    SCDropCaps(tv);

    if (s->SlotThreadInit != NULL) {
        void *slot_data = NULL;
        r = s->SlotThreadInit(tv, s->slot_initdata, &slot_data);
        if (r != TM_ECODE_OK) {
            EngineKill();

            TmThreadsSetFlag(tv, THV_CLOSED | THV_RUNNING_DONE);
            pthread_exit((void *) -1);
            return NULL;
        }
        (void)SC_ATOMIC_SET(s->slot_data, slot_data);
    }
    memset(&s->slot_pre_pq, 0, sizeof(PacketQueue));
    memset(&s->slot_post_pq, 0, sizeof(PacketQueue));

    TmThreadsSetFlag(tv, THV_INIT_DONE);

#ifdef __tilegx__
    MpipeRegisterPipeStage(tv);
#endif

    while (run) {
        if (TmThreadsCheckFlag(tv, THV_PAUSE)) {
            TmThreadsSetFlag(tv, THV_PAUSED);
            TmThreadTestThreadUnPaused(tv);
            TmThreadsUnsetFlag(tv, THV_PAUSED);
        }
        TmSlotFunc SlotFunc = SC_ATOMIC_GET(s->SlotFunc);

        r = SlotFunc(tv, NULL, SC_ATOMIC_GET(s->slot_data), &s->slot_pre_pq, &s->slot_post_pq);

        /* handle error */
        if (r == TM_ECODE_FAILED) {
            TmqhReleasePacketsToPacketPool(&s->slot_pre_pq);

            SCMutexLock(&s->slot_post_pq.mutex_q);
            TmqhReleasePacketsToPacketPool(&s->slot_post_pq);
            SCMutexUnlock(&s->slot_post_pq.mutex_q);

            TmThreadsSetFlag(tv, THV_FAILED);
            break;
        }

        /* handle pre queue */
        while (s->slot_pre_pq.top != NULL) {
            Packet *extra_p = PacketDequeue(&s->slot_pre_pq);
            if (extra_p != NULL)
                tv->tmqh_out(tv, extra_p);
        }

        /* handle post queue */
        if (s->slot_post_pq.top != NULL) {
            SCMutexLock(&s->slot_post_pq.mutex_q);
            while (s->slot_post_pq.top != NULL) {
                Packet *extra_p = PacketDequeue(&s->slot_post_pq);
                if (extra_p != NULL)
                    tv->tmqh_out(tv, extra_p);
            }
            SCMutexUnlock(&s->slot_post_pq.mutex_q);
        }

        if (TmThreadsCheckFlag(tv, THV_KILL)) {
            SCPerfSyncCounters(tv, 0);
            run = 0;
        }
    } /* while (run) */

    TmThreadsSetFlag(tv, THV_RUNNING_DONE);
    TmThreadWaitForFlag(tv, THV_DEINIT);

    if (s->SlotThreadExitPrintStats != NULL) {
        s->SlotThreadExitPrintStats(tv, SC_ATOMIC_GET(s->slot_data));
    }

    if (s->SlotThreadDeinit != NULL) {
        r = s->SlotThreadDeinit(tv, SC_ATOMIC_GET(s->slot_data));
        if (r != TM_ECODE_OK) {
            TmThreadsSetFlag(tv, THV_CLOSED);
            pthread_exit((void *) -1);
            return NULL;
        }
    }

    TmThreadsSetFlag(tv, THV_CLOSED);
    pthread_exit((void *) 0);
    return NULL;
}

void *TmThreadsSlot1NoOut(void *td)
{
    /* block usr2.  usr2 to be handled by the main thread only */
    UtilSignalBlock(SIGUSR2);

    ThreadVars *tv = (ThreadVars *)td;
    TmSlot *s = (TmSlot *)tv->tm_slots;
    Packet *p = NULL;
    char run = 1;
    TmEcode r = TM_ECODE_OK;

    /* Set the thread name */
    if (SCSetThreadName(tv->name) < 0) {
        SCLogWarning(SC_ERR_THREAD_INIT, "Unable to set thread name");
    }

    if (tv->thread_setup_flags != 0)
        TmThreadSetupOptions(tv);

    /* Drop the capabilities for this thread */
    SCDropCaps(tv);

    if (s->SlotThreadInit != NULL) {
        void *slot_data = NULL;
        r = s->SlotThreadInit(tv, s->slot_initdata, &slot_data);
        if (r != TM_ECODE_OK) {
            EngineKill();

            TmThreadsSetFlag(tv, THV_CLOSED | THV_RUNNING_DONE);
            pthread_exit((void *) -1);
            return NULL;
        }
        (void)SC_ATOMIC_SET(s->slot_data, slot_data);
    }
    memset(&s->slot_pre_pq, 0, sizeof(PacketQueue));
    memset(&s->slot_post_pq, 0, sizeof(PacketQueue));

    TmThreadsSetFlag(tv, THV_INIT_DONE);

#ifdef __tilegx__
    MpipeRegisterPipeStage(tv);
#endif
    while (run) {
        if (TmThreadsCheckFlag(tv, THV_PAUSE)) {
            TmThreadsSetFlag(tv, THV_PAUSED);
            TmThreadTestThreadUnPaused(tv);
            TmThreadsUnsetFlag(tv, THV_PAUSED);
        }
        TmSlotFunc SlotFunc = SC_ATOMIC_GET(s->SlotFunc);

        p = tv->tmqh_in(tv);

        PACKET_PROFILING_TMM_START(p, s->tm_id);
        r = SlotFunc(tv, p, SC_ATOMIC_GET(s->slot_data), /* no outqh no pq */ NULL,
                        /* no outqh no pq */ NULL);
        PACKET_PROFILING_TMM_END(p, s->tm_id);

        /* handle error */
        if (r == TM_ECODE_FAILED) {
            TmqhOutputPacketpool(tv, p);
            TmThreadsSetFlag(tv, THV_FAILED);
            break;
        }

        if (TmThreadsCheckFlag(tv, THV_KILL)) {
            SCPerfSyncCounters(tv, 0);
            run = 0;
        }
    } /* while (run) */

    TmThreadsSetFlag(tv, THV_RUNNING_DONE);
    TmThreadWaitForFlag(tv, THV_DEINIT);

    if (s->SlotThreadExitPrintStats != NULL) {
        s->SlotThreadExitPrintStats(tv, SC_ATOMIC_GET(s->slot_data));
    }

    if (s->SlotThreadDeinit != NULL) {
        r = s->SlotThreadDeinit(tv, SC_ATOMIC_GET(s->slot_data));
        if (r != TM_ECODE_OK) {
            TmThreadsSetFlag(tv, THV_CLOSED);
            pthread_exit((void *) -1);
            return NULL;
        }
    }

    TmThreadsSetFlag(tv, THV_CLOSED);
    pthread_exit((void *) 0);
    return NULL;
}

void *TmThreadsSlot1NoInOut(void *td)
{
    /* block usr2.  usr2 to be handled by the main thread only */
    UtilSignalBlock(SIGUSR2);

    ThreadVars *tv = (ThreadVars *)td;
    TmSlot *s = (TmSlot *)tv->tm_slots;
    char run = 1;
    TmEcode r = TM_ECODE_OK;

    /* Set the thread name */
    if (SCSetThreadName(tv->name) < 0) {
        SCLogWarning(SC_ERR_THREAD_INIT, "Unable to set thread name");
    }

    if (tv->thread_setup_flags != 0)
        TmThreadSetupOptions(tv);

    /* Drop the capabilities for this thread */
    SCDropCaps(tv);

    SCLogDebug("%s starting", tv->name);

    if (s->SlotThreadInit != NULL) {
        void *slot_data = NULL;
        r = s->SlotThreadInit(tv, s->slot_initdata, &slot_data);
        if (r != TM_ECODE_OK) {
            EngineKill();

            TmThreadsSetFlag(tv, THV_CLOSED | THV_RUNNING_DONE);
            pthread_exit((void *) -1);
            return NULL;
        }
        (void)SC_ATOMIC_SET(s->slot_data, slot_data);
    }
    memset(&s->slot_pre_pq, 0, sizeof(PacketQueue));
    memset(&s->slot_post_pq, 0, sizeof(PacketQueue));

    TmThreadsSetFlag(tv, THV_INIT_DONE);

#ifdef __tilegx__
    MpipeRegisterPipeStage(tv);
#endif
    while (run) {
        TmSlotFunc SlotFunc = SC_ATOMIC_GET(s->SlotFunc);
        if (TmThreadsCheckFlag(tv, THV_PAUSE)) {
            TmThreadsSetFlag(tv, THV_PAUSED);
            TmThreadTestThreadUnPaused(tv);
            TmThreadsUnsetFlag(tv, THV_PAUSED);
        }

        r = SlotFunc(tv, NULL, SC_ATOMIC_GET(s->slot_data), /* no outqh, no pq */NULL, NULL);

        /* handle error */
        if (r == TM_ECODE_FAILED) {
            TmThreadsSetFlag(tv, THV_FAILED);
            break;
        }

        if (TmThreadsCheckFlag(tv, THV_KILL)) {
            SCPerfSyncCounters(tv, 0);
            run = 0;
        }
    } /* while (run) */

    TmThreadsSetFlag(tv, THV_RUNNING_DONE);
    TmThreadWaitForFlag(tv, THV_DEINIT);

    if (s->SlotThreadExitPrintStats != NULL) {
        s->SlotThreadExitPrintStats(tv, SC_ATOMIC_GET(s->slot_data));
    }

    if (s->SlotThreadDeinit != NULL) {
        r = s->SlotThreadDeinit(tv, SC_ATOMIC_GET(s->slot_data));
        if (r != TM_ECODE_OK) {
            TmThreadsSetFlag(tv, THV_CLOSED);
            pthread_exit((void *) -1);
            return NULL;
        }
    }

    TmThreadsSetFlag(tv, THV_CLOSED);
    pthread_exit((void *) 0);
    return NULL;
}

void *TmThreadsSlot1(void *td)
{
    /* block usr2.  usr2 to be handled by the main thread only */
    UtilSignalBlock(SIGUSR2);

    ThreadVars *tv = (ThreadVars *)td;
    TmSlot *s = (TmSlot *)tv->tm_slots;
    Packet *p = NULL;
    char run = 1;
    TmEcode r = TM_ECODE_OK;

    /* Set the thread name */
    if (SCSetThreadName(tv->name) < 0) {
        SCLogWarning(SC_ERR_THREAD_INIT, "Unable to set thread name");
    }

    if (tv->thread_setup_flags != 0)
        TmThreadSetupOptions(tv);

    /* Drop the capabilities for this thread */
    SCDropCaps(tv);

    SCLogDebug("%s starting", tv->name);

    if (s->SlotThreadInit != NULL) {
        void *slot_data = NULL;
        r = s->SlotThreadInit(tv, s->slot_initdata, &slot_data);
        if (r != TM_ECODE_OK) {
            EngineKill();

            TmThreadsSetFlag(tv, THV_CLOSED | THV_RUNNING_DONE);
            pthread_exit((void *) -1);
            return NULL;
        }
        (void)SC_ATOMIC_SET(s->slot_data, slot_data);
    }
    memset(&s->slot_pre_pq, 0, sizeof(PacketQueue));
    SCMutexInit(&s->slot_pre_pq.mutex_q, NULL);
    memset(&s->slot_post_pq, 0, sizeof(PacketQueue));
    SCMutexInit(&s->slot_post_pq.mutex_q, NULL);

    TmThreadsSetFlag(tv, THV_INIT_DONE);
#ifdef __tilegx__
    MpipeRegisterPipeStage(tv);
#endif
    while (run) {
        if (TmThreadsCheckFlag(tv, THV_PAUSE)) {
            TmThreadsSetFlag(tv, THV_PAUSED);
            TmThreadTestThreadUnPaused(tv);
            TmThreadsUnsetFlag(tv, THV_PAUSED);
        }

        /* input a packet */
        p = tv->tmqh_in(tv);

        if (p != NULL) {
            TmSlotFunc SlotFunc = SC_ATOMIC_GET(s->SlotFunc);
            PACKET_PROFILING_TMM_START(p, s->tm_id);
            r = SlotFunc(tv, p, SC_ATOMIC_GET(s->slot_data), &s->slot_pre_pq,
                            &s->slot_post_pq);
            PACKET_PROFILING_TMM_END(p, s->tm_id);

            /* handle error */
            if (r == TM_ECODE_FAILED) {
                TmqhReleasePacketsToPacketPool(&s->slot_pre_pq);

                SCMutexLock(&s->slot_post_pq.mutex_q);
                TmqhReleasePacketsToPacketPool(&s->slot_post_pq);
                SCMutexUnlock(&s->slot_post_pq.mutex_q);

                TmqhOutputPacketpool(tv, p);
                TmThreadsSetFlag(tv, THV_FAILED);
                break;
            }

            while (s->slot_pre_pq.top != NULL) {
                /* handle new packets from this func */
                Packet *extra_p = PacketDequeue(&s->slot_pre_pq);
                if (extra_p != NULL) {
                    tv->tmqh_out(tv, extra_p);
                }
            }

            /* output the packet */
            tv->tmqh_out(tv, p);
        }
        if (s->slot_post_pq.top != NULL) {
            SCMutexLock(&s->slot_post_pq.mutex_q);
            while (s->slot_post_pq.top != NULL) {
                /* handle new packets from this func */
                Packet *extra_p = PacketDequeue(&s->slot_post_pq);
                if (extra_p != NULL) {
                    tv->tmqh_out(tv, extra_p);
                }
            }
            SCMutexUnlock(&s->slot_post_pq.mutex_q);
        }

        if (TmThreadsCheckFlag(tv, THV_KILL)) {
            SCPerfSyncCounters(tv, 0);
            run = 0;
        }
    } /* while (run) */

    TmThreadsSetFlag(tv, THV_RUNNING_DONE);
    TmThreadWaitForFlag(tv, THV_DEINIT);

    if (s->SlotThreadExitPrintStats != NULL) {
        s->SlotThreadExitPrintStats(tv, SC_ATOMIC_GET(s->slot_data));
    }

    if (s->SlotThreadDeinit != NULL) {
        r = s->SlotThreadDeinit(tv, SC_ATOMIC_GET(s->slot_data));
        if (r != TM_ECODE_OK) {
            TmThreadsSetFlag(tv, THV_CLOSED);
            pthread_exit((void *) -1);
            return NULL;
        }
    }

    SCLogDebug("%s ending", tv->name);
    TmThreadsSetFlag(tv, THV_CLOSED);
    pthread_exit((void *) 0);
    return NULL;
}

/**
 * \brief Separate run function so we can call it recursively.
 *
 * \todo Deal with post_pq for slots beyond the first.
 */
TmEcode TmThreadsSlotVarRun(ThreadVars *tv, Packet *p,
                                          TmSlot *slot)
{
    TmEcode r;
    TmSlot *s;
    Packet *extra_p;

    for (s = slot; s != NULL; s = s->slot_next) {
        TmSlotFunc SlotFunc = SC_ATOMIC_GET(s->SlotFunc);
        PACKET_PROFILING_TMM_START(p, s->tm_id);

        if (unlikely(s->id == 0)) {
            r = SlotFunc(tv, p, SC_ATOMIC_GET(s->slot_data), &s->slot_pre_pq, &s->slot_post_pq);
        } else {
            r = SlotFunc(tv, p, SC_ATOMIC_GET(s->slot_data), &s->slot_pre_pq, NULL);
        }

        PACKET_PROFILING_TMM_END(p, s->tm_id);

        /* handle error */
        if (unlikely(r == TM_ECODE_FAILED)) {
            /* Encountered error.  Return packets to packetpool and return */
            TmqhReleasePacketsToPacketPool(&s->slot_pre_pq);

            SCMutexLock(&s->slot_post_pq.mutex_q);
            TmqhReleasePacketsToPacketPool(&s->slot_post_pq);
            SCMutexUnlock(&s->slot_post_pq.mutex_q);

            TmThreadsSetFlag(tv, THV_FAILED);
            return TM_ECODE_FAILED;
        }

        /* handle new packets */
        while (s->slot_pre_pq.top != NULL) {
            extra_p = PacketDequeue(&s->slot_pre_pq);
            if (unlikely(extra_p == NULL))
                continue;

            /* see if we need to process the packet */
            if (s->slot_next != NULL) {
                r = TmThreadsSlotVarRun(tv, extra_p, s->slot_next);
                if (unlikely(r == TM_ECODE_FAILED)) {
                    TmqhReleasePacketsToPacketPool(&s->slot_pre_pq);

                    SCMutexLock(&s->slot_post_pq.mutex_q);
                    TmqhReleasePacketsToPacketPool(&s->slot_post_pq);
                    SCMutexUnlock(&s->slot_post_pq.mutex_q);

                    TmqhOutputPacketpool(tv, extra_p);
                    TmThreadsSetFlag(tv, THV_FAILED);
                    return TM_ECODE_FAILED;
                }
            }
            tv->tmqh_out(tv, extra_p);
        }
    }

    return TM_ECODE_OK;
}

/*

    pcap/nfq

    pkt read
        callback
            process_pkt

    pfring

    pkt read
        process_pkt

    slot:
        setup

        pkt_ack_loop(tv, slot_data)

        deinit

    process_pkt:
        while(s)
            run s;
        queue;

 */

void *TmThreadsSlotPktAcqLoop(void *td) {
    /* block usr2.  usr2 to be handled by the main thread only */
    UtilSignalBlock(SIGUSR2);

    ThreadVars *tv = (ThreadVars *)td;
    TmSlot *s = tv->tm_slots;
    char run = 1;
    TmEcode r = TM_ECODE_OK;
    TmSlot *slot = NULL;

    /* Set the thread name */
    if (SCSetThreadName(tv->name) < 0) {
        SCLogWarning(SC_ERR_THREAD_INIT, "Unable to set thread name");
    }

    if (tv->thread_setup_flags != 0)
        TmThreadSetupOptions(tv);

    /* Drop the capabilities for this thread */
    SCDropCaps(tv);

    /* check if we are setup properly */
    if (s == NULL || s->PktAcqLoop == NULL || tv->tmqh_in == NULL || tv->tmqh_out == NULL) {
        SCLogError(SC_ERR_FATAL, "TmSlot or ThreadVars badly setup: s=%p,"
                                 " PktAcqLoop=%p, tmqh_in=%p,"
                                 " tmqh_out=%p",
                   s, s ? s->PktAcqLoop : NULL, tv->tmqh_in, tv->tmqh_out);
        EngineKill();

        TmThreadsSetFlag(tv, THV_CLOSED | THV_RUNNING_DONE);
        pthread_exit((void *) -1);
        return NULL;
    }

    for (slot = s; slot != NULL; slot = slot->slot_next) {
        if (slot->SlotThreadInit != NULL) {
            void *slot_data = NULL;
            r = slot->SlotThreadInit(tv, slot->slot_initdata, &slot_data);
            if (r != TM_ECODE_OK) {
                if (r == TM_ECODE_DONE) {
                    EngineDone();
                    TmThreadsSetFlag(tv, THV_CLOSED | THV_INIT_DONE | THV_RUNNING_DONE);
                    pthread_exit((void *) -1);
                } else {
                    EngineKill();
                    TmThreadsSetFlag(tv, THV_CLOSED | THV_RUNNING_DONE);
                    pthread_exit((void *) -1);
                    return NULL;
                }
            }
            (void)SC_ATOMIC_SET(slot->slot_data, slot_data);
        }
        memset(&slot->slot_pre_pq, 0, sizeof(PacketQueue));
        SCMutexInit(&slot->slot_pre_pq.mutex_q, NULL);
        memset(&slot->slot_post_pq, 0, sizeof(PacketQueue));
        SCMutexInit(&slot->slot_post_pq.mutex_q, NULL);
    }

    TmThreadsSetFlag(tv, THV_INIT_DONE);

    while(run) {
        if (TmThreadsCheckFlag(tv, THV_PAUSE)) {
            TmThreadsSetFlag(tv, THV_PAUSED);
            TmThreadTestThreadUnPaused(tv);
            TmThreadsUnsetFlag(tv, THV_PAUSED);
        }

        r = s->PktAcqLoop(tv, SC_ATOMIC_GET(s->slot_data), s);

        if (r == TM_ECODE_FAILED || TmThreadsCheckFlag(tv, THV_KILL)
            || suricata_ctl_flags) {
            run = 0;
        }
        if (r == TM_ECODE_DONE) {
            run = 0;
        }
    }
    SCPerfSyncCounters(tv, 0);

    TmThreadsSetFlag(tv, THV_RUNNING_DONE);
    TmThreadWaitForFlag(tv, THV_DEINIT);

    for (slot = s; slot != NULL; slot = slot->slot_next) {
        if (slot->SlotThreadExitPrintStats != NULL) {
            slot->SlotThreadExitPrintStats(tv, SC_ATOMIC_GET(slot->slot_data));
        }

        if (slot->SlotThreadDeinit != NULL) {
            r = slot->SlotThreadDeinit(tv, SC_ATOMIC_GET(slot->slot_data));
            if (r != TM_ECODE_OK) {
                TmThreadsSetFlag(tv, THV_CLOSED);
                pthread_exit((void *) -1);
                return NULL;
            }
        }
    }

    SCLogDebug("%s ending", tv->name);
    TmThreadsSetFlag(tv, THV_CLOSED);
    pthread_exit((void *) 0);
    return NULL;
}


/**
 * \todo Only the first "slot" currently makes the "post_pq" available
 *       to the thread module.
 */
void *TmThreadsSlotVar(void *td)
{
    /* block usr2.  usr2 to be handled by the main thread only */
    UtilSignalBlock(SIGUSR2);

    ThreadVars *tv = (ThreadVars *)td;
    TmSlot *s = (TmSlot *)tv->tm_slots;
    Packet *p = NULL;
    char run = 1;
    TmEcode r = TM_ECODE_OK;

    /* Set the thread name */
    if (SCSetThreadName(tv->name) < 0) {
        SCLogWarning(SC_ERR_THREAD_INIT, "Unable to set thread name");
    }

    if (tv->thread_setup_flags != 0)
        TmThreadSetupOptions(tv);

    /* Drop the capabilities for this thread */
    SCDropCaps(tv);

    /* check if we are setup properly */
    if (s == NULL || tv->tmqh_in == NULL || tv->tmqh_out == NULL) {
        EngineKill();

        TmThreadsSetFlag(tv, THV_CLOSED | THV_RUNNING_DONE);
        pthread_exit((void *) -1);
        return NULL;
    }

    for (; s != NULL; s = s->slot_next) {
        if (s->SlotThreadInit != NULL) {
            void *slot_data = NULL;
            r = s->SlotThreadInit(tv, s->slot_initdata, &slot_data);
            if (r != TM_ECODE_OK) {
                EngineKill();

                TmThreadsSetFlag(tv, THV_CLOSED | THV_RUNNING_DONE);
                pthread_exit((void *) -1);
                return NULL;
            }
            (void)SC_ATOMIC_SET(s->slot_data, slot_data);
        }
        memset(&s->slot_pre_pq, 0, sizeof(PacketQueue));
        SCMutexInit(&s->slot_pre_pq.mutex_q, NULL);
        memset(&s->slot_post_pq, 0, sizeof(PacketQueue));
        SCMutexInit(&s->slot_post_pq.mutex_q, NULL);
    }

    TmThreadsSetFlag(tv, THV_INIT_DONE);

    s = (TmSlot *)tv->tm_slots;

#ifdef __tile__
#ifdef __tilegx__
    MpipeRegisterPipeStage(tv);
#else
    if (strstr(tv->name, "Outputs") != NULL) {
        NetioRegisterOutputs(tv);
    }
#endif
#endif

    while (run) {
        if (TmThreadsCheckFlag(tv, THV_PAUSE)) {
            TmThreadsSetFlag(tv, THV_PAUSED);
            TmThreadTestThreadUnPaused(tv);
            TmThreadsUnsetFlag(tv, THV_PAUSED);
        }

        /* input a packet */
        p = tv->tmqh_in(tv);

        if (p != NULL) {
            /* run the thread module(s) */
            r = TmThreadsSlotVarRun(tv, p, s);
            if (r == TM_ECODE_FAILED) {
                TmqhOutputPacketpool(tv, p);
                TmThreadsSetFlag(tv, THV_FAILED);
                break;
            }

            /* output the packet */
            tv->tmqh_out(tv, p);

        } /* if (p != NULL) */

        /* now handle the post_pq packets */
        TmSlot *slot;
        for (slot = s; slot != NULL; slot = slot->slot_next) {
            if (slot->slot_post_pq.top != NULL) {
                while (1) {
                    SCMutexLock(&slot->slot_post_pq.mutex_q);
                    Packet *extra_p = PacketDequeue(&slot->slot_post_pq);
                    SCMutexUnlock(&slot->slot_post_pq.mutex_q);

                    if (extra_p == NULL)
                        break;

                    if (slot->slot_next != NULL) {
                        r = TmThreadsSlotVarRun(tv, extra_p, slot->slot_next);
                        if (r == TM_ECODE_FAILED) {
                            SCMutexLock(&slot->slot_post_pq.mutex_q);
                            TmqhReleasePacketsToPacketPool(&slot->slot_post_pq);
                            SCMutexUnlock(&slot->slot_post_pq.mutex_q);

                            TmqhOutputPacketpool(tv, extra_p);
                            TmThreadsSetFlag(tv, THV_FAILED);
                            break;
                        }
                    }
                    /* output the packet */
                    tv->tmqh_out(tv, extra_p);
                } /* while */
            } /* if */
        } /* for */

        if (TmThreadsCheckFlag(tv, THV_KILL)) {
            run = 0;
        }
    } /* while (run) */
    SCPerfSyncCounters(tv, 0);

    TmThreadsSetFlag(tv, THV_RUNNING_DONE);
    TmThreadWaitForFlag(tv, THV_DEINIT);

    s = (TmSlot *)tv->tm_slots;

    for ( ; s != NULL; s = s->slot_next) {
        if (s->SlotThreadExitPrintStats != NULL) {
            s->SlotThreadExitPrintStats(tv, SC_ATOMIC_GET(s->slot_data));
        }

        if (s->SlotThreadDeinit != NULL) {
            r = s->SlotThreadDeinit(tv, SC_ATOMIC_GET(s->slot_data));
            if (r != TM_ECODE_OK) {
                TmThreadsSetFlag(tv, THV_CLOSED);
                pthread_exit((void *) -1);
                return NULL;
            }
        }
    }

    SCLogDebug("%s ending", tv->name);
    TmThreadsSetFlag(tv, THV_CLOSED);
    pthread_exit((void *) 0);
    return NULL;
}

/**
 * \brief We set the slot functions.
 *
 * \param tv   Pointer to the TV to set the slot function for.
 * \param name Name of the slot variant.
 * \param fn_p Pointer to a custom slot function.  Used only if slot variant
 *             "name" is "custom".
 *
 * \retval TmEcode TM_ECODE_OK on success; TM_ECODE_FAILED on failure.
 */
TmEcode TmThreadSetSlots(ThreadVars *tv, char *name, void *(*fn_p)(void *))
{
    if (name == NULL) {
        if (fn_p == NULL) {
            printf("Both slot name and function pointer can't be NULL inside "
                   "TmThreadSetSlots\n");
            goto error;
        } else {
            name = "custom";
        }
    }

    if (strcmp(name, "1slot") == 0) {
        tv->tm_func = TmThreadsSlot1;
    } else if (strcmp(name, "1slot_noout") == 0) {
        tv->tm_func = TmThreadsSlot1NoOut;
    } else if (strcmp(name, "1slot_noin") == 0) {
        tv->tm_func = TmThreadsSlot1NoIn;
    } else if (strcmp(name, "1slot_noinout") == 0) {
        tv->tm_func = TmThreadsSlot1NoInOut;
    } else if (strcmp(name, "varslot") == 0) {
        tv->tm_func = TmThreadsSlotVar;
    } else if (strcmp(name, "pktacqloop") == 0) {
        tv->tm_func = TmThreadsSlotPktAcqLoop;
    } else if (strcmp(name, "custom") == 0) {
        if (fn_p == NULL)
            goto error;
        tv->tm_func = fn_p;
    } else {
        printf("Error: Slot \"%s\" not supported\n", name);
        goto error;
    }

    return TM_ECODE_OK;

error:
    return TM_ECODE_FAILED;
}

ThreadVars *TmThreadsGetTVContainingSlot(TmSlot *tm_slot)
{
    ThreadVars *tv;
    int i;

    SCMutexLock(&tv_root_lock);

    for (i = 0; i < TVT_MAX; i++) {
        tv = tv_root[i];

        while (tv) {
            TmSlot *slots = tv->tm_slots;
            while (slots != NULL) {
                if (slots == tm_slot) {
                    SCMutexUnlock(&tv_root_lock);
                    return tv;
                }
                slots = slots->slot_next;
            }
            tv = tv->next;
        }
    }

    SCMutexUnlock(&tv_root_lock);

    return NULL;
}

/**
 * \brief Appends a new entry to the slots.
 *
 * \param tv   TV the slot is attached to.
 * \param tm   TM to append.
 * \param data Data to be passed on to the slot init function.
 *
 * \retval The allocated TmSlot or NULL if there is an error
 */
static inline TmSlot * _TmSlotSetFuncAppend(ThreadVars *tv, TmModule *tm, void *data)
{
    TmSlot *slot = SCMalloc(sizeof(TmSlot));
    if (unlikely(slot == NULL))
        return NULL;
    memset(slot, 0, sizeof(TmSlot));
    SC_ATOMIC_INIT(slot->slot_data);
    slot->tv = tv;
    slot->SlotThreadInit = tm->ThreadInit;
    slot->slot_initdata = data;
    SC_ATOMIC_INIT(slot->SlotFunc);
    (void)SC_ATOMIC_SET(slot->SlotFunc, tm->Func);
    slot->PktAcqLoop = tm->PktAcqLoop;
    slot->SlotThreadExitPrintStats = tm->ThreadExitPrintStats;
    slot->SlotThreadDeinit = tm->ThreadDeinit;
    /* we don't have to check for the return value "-1".  We wouldn't have
     * received a TM as arg, if it didn't exist */
    slot->tm_id = TmModuleGetIDForTM(tm);

    tv->cap_flags |= tm->cap_flags;

    if (tv->tm_slots == NULL) {
        tv->tm_slots = slot;
        slot->id = 0;
    } else {
        TmSlot *a = (TmSlot *)tv->tm_slots, *b = NULL;

        /* get the last slot */
        for ( ; a != NULL; a = a->slot_next) {
             b = a;
        }
        /* append the new slot */
        if (b != NULL) {
            b->slot_next = slot;
            slot->id = b->id + 1;
        }
    }

    return slot;
}

/**
 * \brief Appends a new entry to the slots.
 *
 * \param tv   TV the slot is attached to.
 * \param tm   TM to append.
 * \param data Data to be passed on to the slot init function.
 */
void TmSlotSetFuncAppend(ThreadVars *tv, TmModule *tm, void *data)
{
    _TmSlotSetFuncAppend(tv, tm, data);
}

typedef struct TmDummySlot_ {
    TmSlot *slot;
    TmEcode (*SlotFunc)(ThreadVars *, Packet *, void *, PacketQueue *,
                        PacketQueue *);
    TmEcode (*SlotThreadInit)(ThreadVars *, void *, void **);
    TAILQ_ENTRY(TmDummySlot_) next;
} TmDummySlot;

static TAILQ_HEAD(, TmDummySlot_) dummy_slots =
    TAILQ_HEAD_INITIALIZER(dummy_slots);

/**
 * \brief Appends a new entry to the slots with a delayed option.
 *
 * \param tv   TV the slot is attached to.
 * \param tm   TM to append.
 * \param data Data to be passed on to the slot init function.
 * \param delayed Delayed start of slot if equal to 1
 */
void TmSlotSetFuncAppendDelayed(ThreadVars *tv, TmModule *tm, void *data,
                                int delayed)
{
    TmSlot *slot = _TmSlotSetFuncAppend(tv, tm, data);
    TmDummySlot *dslot = NULL;

    if ((slot == NULL) || (delayed == 0)) {
        return;
    }

    dslot = SCMalloc(sizeof(TmDummySlot));
    if (unlikely(dslot == NULL)) {
        return;
    }

    memset(dslot, 0, sizeof(*dslot));

    dslot->SlotFunc = SC_ATOMIC_GET(slot->SlotFunc);
    (void)SC_ATOMIC_SET(slot->SlotFunc, TmDummyFunc);
    dslot->SlotThreadInit = slot->SlotThreadInit;
    slot->SlotThreadInit = NULL;
    dslot->slot = slot;

    TAILQ_INSERT_TAIL(&dummy_slots, dslot, next);

    return;
}

/**
 * \brief Activate slots that have been started in delayed mode
 */
void TmThreadActivateDummySlot()
{
    TmDummySlot *dslot;
    TmSlot *s;
    TmEcode r = TM_ECODE_OK;

    TAILQ_FOREACH(dslot, &dummy_slots, next) {
        void *slot_data = NULL;
        s = dslot->slot;
        if (dslot->SlotThreadInit != NULL) {
            s->SlotThreadInit = dslot->SlotThreadInit;
            r = s->SlotThreadInit(s->tv, s->slot_initdata, &slot_data);
            if (r != TM_ECODE_OK) {
                EngineKill();
                TmThreadsSetFlag(s->tv, THV_CLOSED | THV_RUNNING_DONE);
            }
            (void)SC_ATOMIC_SET(s->slot_data, slot_data);
        }
#ifdef __tile__
        /* HACK: having tile-gcc compiler issues here */
#else
        (void)SC_ATOMIC_CAS(&s->SlotFunc, TmDummyFunc, dslot->SlotFunc);
#endif
    }
}

/**
 * \brief Deactivate slots that have been started in delayed mode.
 */
void TmThreadDeActivateDummySlot()
{
    TmDummySlot *dslot;

    TAILQ_FOREACH(dslot, &dummy_slots, next) {
        (void)SC_ATOMIC_CAS(&dslot->slot->SlotFunc, dslot->SlotFunc, TmDummyFunc);
        dslot->slot->SlotThreadInit = NULL;
    }
}

/**
 * \brief Returns the slot holding a TM with the particular tm_id.
 *
 * \param tm_id TM id of the TM whose slot has to be returned.
 *
 * \retval slots Pointer to the slot.
 */
TmSlot *TmSlotGetSlotForTM(int tm_id)
{
    ThreadVars *tv = NULL;
    TmSlot *slots;
    int i;

    SCMutexLock(&tv_root_lock);

    for (i = 0; i < TVT_MAX; i++) {
        tv = tv_root[i];
        while (tv) {
            slots = tv->tm_slots;
            while (slots != NULL) {
                if (slots->tm_id == tm_id) {
                    SCMutexUnlock(&tv_root_lock);
                    return slots;
                }
                slots = slots->slot_next;
            }
            tv = tv->next;
        }
    }

    SCMutexUnlock(&tv_root_lock);

    return NULL;
}

#if !defined __CYGWIN__ && !defined OS_WIN32 && !defined __OpenBSD__
static int SetCPUAffinitySet(cpu_set_t *cs) {
#if defined OS_FREEBSD
    int r = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID,
                               SCGetThreadIdLong(), sizeof(cpu_set_t),cs);
#elif OS_DARWIN
    int r = thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY,
                              (void*)cs, THREAD_AFFINITY_POLICY_COUNT);
#else
    pid_t tid = syscall(SYS_gettid);
    int r = sched_setaffinity(tid, sizeof(cpu_set_t), cs);
#endif /* OS_FREEBSD */

    if (r != 0) {
        printf("Warning: sched_setaffinity failed (%" PRId32 "): %s\n", r,
               strerror(errno));
        return -1;
    }

    return 0;
}
#endif


/**
 * \brief Set the thread affinity on the calling thread.
 *
 * \param cpuid Id of the core/cpu to setup the affinity.
 *
 * \retval 0 If all goes well; -1 if something is wrong.
 */
static int SetCPUAffinity(uint16_t cpuid)
{
#if defined __OpenBSD__
    return 0;
#else
    int cpu = (int)cpuid;

#if defined OS_WIN32 || defined __CYGWIN__
    DWORD cs = 1 << cpu;

    int r = (0 == SetThreadAffinityMask(GetCurrentThread(), cs));
    if (r != 0) {
        printf("Warning: sched_setaffinity failed (%" PRId32 "): %s\n", r,
               strerror(errno));
        return -1;
    }
    SCLogDebug("CPU Affinity for thread %lu set to CPU %" PRId32,
               SCGetThreadIdLong(), cpu);

    return 0;

#else
    cpu_set_t cs;

    CPU_ZERO(&cs);
    CPU_SET(cpu, &cs);
    return SetCPUAffinitySet(&cs);
#endif /* windows */
#endif /* not supported */
}


/**
 * \brief Set the thread options (thread priority).
 *
 * \param tv Pointer to the ThreadVars to setup the thread priority.
 *
 * \retval TM_ECODE_OK.
 */
TmEcode TmThreadSetThreadPriority(ThreadVars *tv, int prio)
{
    tv->thread_setup_flags |= THREAD_SET_PRIORITY;
    tv->thread_priority = prio;

    return TM_ECODE_OK;
}

/**
 * \brief Adjusting nice value for threads.
 */
void TmThreadSetPrio(ThreadVars *tv)
{
    SCEnter();
#ifndef __CYGWIN__
#ifdef OS_WIN32
	if (0 == SetThreadPriority(GetCurrentThread(), tv->thread_priority)) {
        SCLogError(SC_ERR_THREAD_NICE_PRIO, "Error setting priority for "
                   "thread %s: %s", tv->name, strerror(errno));
    } else {
        SCLogDebug("Priority set to %"PRId32" for thread %s",
                   tv->thread_priority, tv->name);
    }
#else
    int ret = nice(tv->thread_priority);
    if (ret == -1) {
        SCLogError(SC_ERR_THREAD_NICE_PRIO, "Error setting nice value "
                   "for thread %s: %s", tv->name, strerror(errno));
    } else {
        SCLogDebug("Nice value set to %"PRId32" for thread %s",
                   tv->thread_priority, tv->name);
    }
#endif /* OS_WIN32 */
#endif
    SCReturn;
}


/**
 * \brief Set the thread options (cpu affinity).
 *
 * \param tv pointer to the ThreadVars to setup the affinity.
 * \param cpu cpu on which affinity is set.
 *
 * \retval TM_ECODE_OK
 */
TmEcode TmThreadSetCPUAffinity(ThreadVars *tv, uint16_t cpu)
{
    tv->thread_setup_flags |= THREAD_SET_AFFINITY;
    tv->cpu_affinity = cpu;

    return TM_ECODE_OK;
}


TmEcode TmThreadSetCPU(ThreadVars *tv, uint8_t type)
{
#ifndef __tile__ /* tilera always sets affinity for mpipe */
    if (!threading_set_cpu_affinity)
        return TM_ECODE_OK;
#endif

    if (type > MAX_CPU_SET) {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "invalid cpu type family");
        return TM_ECODE_FAILED;
    }

    tv->thread_setup_flags |= THREAD_SET_AFFTYPE;
    tv->cpu_affinity = type;

    return TM_ECODE_OK;
}

int TmThreadGetNbThreads(uint8_t type)
{
    if (type >= MAX_CPU_SET) {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "invalid cpu type family");
        return 0;
    }

    return thread_affinity[type].nb_threads;
}

/**
 * \brief Set the thread options (cpu affinitythread).
 *        Priority should be already set by pthread_create.
 *
 * \param tv pointer to the ThreadVars of the calling thread.
 */
TmEcode TmThreadSetupOptions(ThreadVars *tv)
{
    if (tv->thread_setup_flags & THREAD_SET_AFFINITY) {
        SCLogInfo("Setting affinity for \"%s\" Module to cpu/core "
                  "%"PRIu16", thread id %lu", tv->name, tv->cpu_affinity,
                  SCGetThreadIdLong());
        SetCPUAffinity(tv->cpu_affinity);
    }

#if !defined __CYGWIN__ && !defined OS_WIN32 && !defined __OpenBSD__
    if (tv->thread_setup_flags & THREAD_SET_PRIORITY)
        TmThreadSetPrio(tv);
    if (tv->thread_setup_flags & THREAD_SET_AFFTYPE) {
        ThreadsAffinityType *taf = &thread_affinity[tv->cpu_affinity];
        if (taf->mode_flag == EXCLUSIVE_AFFINITY) {
            int cpu = AffinityGetNextCPU(taf);
            SetCPUAffinity(cpu);
            /* If CPU is in a set overwrite the default thread prio */
            if (CPU_ISSET(cpu, &taf->lowprio_cpu)) {
                tv->thread_priority = PRIO_LOW;
            } else if (CPU_ISSET(cpu, &taf->medprio_cpu)) {
                tv->thread_priority = PRIO_MEDIUM;
            } else if (CPU_ISSET(cpu, &taf->hiprio_cpu)) {
                tv->thread_priority = PRIO_HIGH;
            } else {
                tv->thread_priority = taf->prio;
            }
            SCLogInfo("Setting prio %d for \"%s\" Module to cpu/core "
                      "%"PRIu16", thread id %lu", tv->thread_priority,
                      tv->name, cpu, SCGetThreadIdLong());
        } else {
            SetCPUAffinitySet(&taf->cpu_set);
            tv->thread_priority = taf->prio;
            SCLogInfo("Setting prio %d for \"%s\" thread "
                      ", thread id %lu", tv->thread_priority,
                      tv->name, SCGetThreadIdLong());
        }
        TmThreadSetPrio(tv);
    }
#endif

    return TM_ECODE_OK;
}

/**
 * \brief Creates and returns the TV instance for a new thread.
 *
 * \param name       Name of this TV instance
 * \param inq_name   Incoming queue name
 * \param inqh_name  Incoming queue handler name as set by TmqhSetup()
 * \param outq_name  Outgoing queue name
 * \param outqh_name Outgoing queue handler as set by TmqhSetup()
 * \param slots      String representation for the slot function to be used
 * \param fn_p       Pointer to function when \"slots\" is of type \"custom\"
 * \param mucond     Flag to indicate whether to initialize the condition
 *                   and the mutex variables for this newly created TV.
 *
 * \retval the newly created TV instance, or NULL on error
 */
ThreadVars *TmThreadCreate(char *name, char *inq_name, char *inqh_name,
                           char *outq_name, char *outqh_name, char *slots,
                           void * (*fn_p)(void *), int mucond)
{
    ThreadVars *tv = NULL;
    Tmq *tmq = NULL;
    Tmqh *tmqh = NULL;

    SCLogDebug("creating thread \"%s\"...", name);

    /* XXX create separate function for this: allocate a thread container */
    tv = SCMalloc(sizeof(ThreadVars));
    if (unlikely(tv == NULL))
        goto error;
    memset(tv, 0, sizeof(ThreadVars));

    SC_ATOMIC_INIT(tv->flags);
    SCMutexInit(&tv->sc_perf_pctx.m, NULL);

    tv->name = name;
    /* default state for every newly created thread */
    TmThreadsSetFlag(tv, THV_PAUSE);
    TmThreadsSetFlag(tv, THV_USE);
    /* default aof for every newly created thread */
    tv->aof = THV_RESTART_THREAD;

    /* set the incoming queue */
    if (inq_name != NULL && strcmp(inq_name, "packetpool") != 0) {
        SCLogDebug("inq_name \"%s\"", inq_name);

        tmq = TmqGetQueueByName(inq_name);
        if (tmq == NULL) {
            tmq = TmqCreateQueue(inq_name);
            if (tmq == NULL)
                goto error;
        }
        SCLogDebug("tmq %p", tmq);

        tv->inq = tmq;
        tv->inq->reader_cnt++;
        SCLogDebug("tv->inq %p", tv->inq);
    }
    if (inqh_name != NULL) {
        SCLogDebug("inqh_name \"%s\"", inqh_name);

        tmqh = TmqhGetQueueHandlerByName(inqh_name);
        if (tmqh == NULL)
            goto error;

        tv->tmqh_in = tmqh->InHandler;
        tv->InShutdownHandler = tmqh->InShutdownHandler;
        SCLogDebug("tv->tmqh_in %p", tv->tmqh_in);
    }

    /* set the outgoing queue */
    if (outqh_name != NULL) {
        SCLogDebug("outqh_name \"%s\"", outqh_name);

        tmqh = TmqhGetQueueHandlerByName(outqh_name);
        if (tmqh == NULL)
            goto error;

        tv->tmqh_out = tmqh->OutHandler;
        tv->outqh_name = tmqh->name;

        if (outq_name != NULL && strcmp(outq_name, "packetpool") != 0) {
            SCLogDebug("outq_name \"%s\"", outq_name);

            if (tmqh->OutHandlerCtxSetup != NULL) {
                tv->outctx = tmqh->OutHandlerCtxSetup(outq_name);
                tv->outq = NULL;
            } else {
                tmq = TmqGetQueueByName(outq_name);
                if (tmq == NULL) {
                    tmq = TmqCreateQueue(outq_name);
                    if (tmq == NULL)
                        goto error;
                }
                SCLogDebug("tmq %p", tmq);

                tv->outq = tmq;
                tv->outctx = NULL;
                tv->outq->writer_cnt++;
            }
        }
    }

    if (TmThreadSetSlots(tv, slots, fn_p) != TM_ECODE_OK) {
        goto error;
    }

    if (mucond != 0)
        TmThreadInitMC(tv);

    return tv;

error:
    SCLogError(SC_ERR_THREAD_CREATE, "failed to setup a thread");

    if (tv != NULL)
        SCFree(tv);
    return NULL;
}

/**
 * \brief Creates and returns a TV instance for a Packet Processing Thread.
 *        This function doesn't support custom slots, and hence shouldn't be
 *        supplied \"custom\" as its slot type.  All PPT threads are created
 *        with a mucond(see TmThreadCreate declaration) of 0. Hence the tv
 *        conditional variables are not used to kill the thread.
 *
 * \param name       Name of this TV instance
 * \param inq_name   Incoming queue name
 * \param inqh_name  Incoming queue handler name as set by TmqhSetup()
 * \param outq_name  Outgoing queue name
 * \param outqh_name Outgoing queue handler as set by TmqhSetup()
 * \param slots      String representation for the slot function to be used
 *
 * \retval the newly created TV instance, or NULL on error
 */
ThreadVars *TmThreadCreatePacketHandler(char *name, char *inq_name,
                                        char *inqh_name, char *outq_name,
                                        char *outqh_name, char *slots)
{
    ThreadVars *tv = NULL;

    tv = TmThreadCreate(name, inq_name, inqh_name, outq_name, outqh_name,
                        slots, NULL, 0);

    if (tv != NULL)
        tv->type = TVT_PPT;

    return tv;
}

/**
 * \brief Creates and returns the TV instance for a Management thread(MGMT).
 *        This function supports only custom slot functions and hence a
 *        function pointer should be sent as an argument.
 *
 * \param name       Name of this TV instance
 * \param fn_p       Pointer to function when \"slots\" is of type \"custom\"
 * \param mucond     Flag to indicate whether to initialize the condition
 *                   and the mutex variables for this newly created TV.
 *
 * \retval the newly created TV instance, or NULL on error
 */
ThreadVars *TmThreadCreateMgmtThread(char *name, void *(fn_p)(void *),
                                     int mucond)
{
    ThreadVars *tv = NULL;

    tv = TmThreadCreate(name, NULL, NULL, NULL, NULL, "custom", fn_p, mucond);

    if (tv != NULL) {
        tv->type = TVT_MGMT;
        TmThreadSetCPU(tv, MANAGEMENT_CPU_SET);
    }

    return tv;
}

/**
 * \brief Creates and returns the TV instance for a CMD thread.
 *        This function supports only custom slot functions and hence a
 *        function pointer should be sent as an argument.
 *
 * \param name       Name of this TV instance
 * \param fn_p       Pointer to function when \"slots\" is of type \"custom\"
 * \param mucond     Flag to indicate whether to initialize the condition
 *                   and the mutex variables for this newly created TV.
 *
 * \retval the newly created TV instance, or NULL on error
 */
ThreadVars *TmThreadCreateCmdThread(char *name, void *(fn_p)(void *),
                                     int mucond)
{
    ThreadVars *tv = NULL;

    tv = TmThreadCreate(name, NULL, NULL, NULL, NULL, "custom", fn_p, mucond);

    if (tv != NULL) {
        tv->type = TVT_CMD;
        TmThreadSetCPU(tv, MANAGEMENT_CPU_SET);
    }

    return tv;
}

/**
 * \brief Appends this TV to tv_root based on its type
 *
 * \param type holds the type this TV belongs to.
 */
void TmThreadAppend(ThreadVars *tv, int type)
{
    SCMutexLock(&tv_root_lock);

    if (tv_root[type] == NULL) {
        tv_root[type] = tv;
        tv->next = NULL;
        tv->prev = NULL;

        SCMutexUnlock(&tv_root_lock);

        return;
    }

    ThreadVars *t = tv_root[type];

    while (t) {
        if (t->next == NULL) {
            t->next = tv;
            tv->prev = t;
            tv->next = NULL;
            break;
        }

        t = t->next;
    }

    SCMutexUnlock(&tv_root_lock);

    return;
}

/**
 * \brief Removes this TV from tv_root based on its type
 *
 * \param tv   The tv instance to remove from the global tv list.
 * \param type Holds the type this TV belongs to.
 */
void TmThreadRemove(ThreadVars *tv, int type)
{
    SCMutexLock(&tv_root_lock);

    if (tv_root[type] == NULL) {
        SCMutexUnlock(&tv_root_lock);

        return;
    }

    ThreadVars *t = tv_root[type];
    while (t != tv) {
        t = t->next;
    }

    if (t != NULL) {
        if (t->prev != NULL)
            t->prev->next = t->next;
        if (t->next != NULL)
            t->next->prev = t->prev;

    if (t == tv_root[type])
        tv_root[type] = t->next;;
    }

    SCMutexUnlock(&tv_root_lock);

    return;
}

/**
 * \brief Exchange existing TV in tv_root with a cloned copy
 *
 * \param tv   The tv instance to remove from the global tv list.
 * \param type Holds the type this TV belongs to.
 */
void TmThreadExchange(ThreadVars *otv, ThreadVars *ntv, int type)
{
    SCMutexLock(&tv_root_lock);

    if (tv_root[type] == NULL) {
        SCMutexUnlock(&tv_root_lock);

        return;
    }

    /* find and remove old threadvar */
    ThreadVars *t = tv_root[type];
    while (t != otv) {
        t = t->next;
    }

    if (t != NULL) {
        if (t->prev != NULL)
            t->prev->next = t->next;
        if (t->next != NULL)
            t->next->prev = t->prev;

    if (t == tv_root[type])
        tv_root[type] = t->next;;
    }

    /* append new threadvar */
    if (tv_root[type] == NULL) {
        tv_root[type] = ntv;
        ntv->next = NULL;
        ntv->prev = NULL;

        SCMutexUnlock(&tv_root_lock);

        return;
    }

    t = tv_root[type];

    while (t) {
        if (t->next == NULL) {
            t->next = ntv;
            ntv->prev = t;
            ntv->next = NULL;
            break;
        }

        t = t->next;
    }

    SCMutexUnlock(&tv_root_lock);

    return;
}

/**
 * \brief Kill a thread.
 *
 * \param tv A ThreadVars instance corresponding to the thread that has to be
 *           killed.
 */
void TmThreadKillThread(ThreadVars *tv)
{
#ifndef __tile__
    int i = 0;
#endif

    if (tv == NULL)
        return;

    if (tv->inq != NULL) {
        /* we wait till we dry out all the inq packets, before we
         * kill this thread.  Do note that you should have disabled
         * packet acquire by now using TmThreadDisableReceiveThreads()*/
        if (!(strlen(tv->inq->name) == strlen("packetpool") &&
              strcasecmp(tv->inq->name, "packetpool") == 0)) {
            PacketQueue *q = &trans_q[tv->inq->id];
#ifdef __tile__
            while (q->len > 0) {
#else
            while (q->len != 0) {
#endif
                usleep(1000);
            }
        }
    }

    /* set the thread flag informing the thread that it needs to be
     * terminated */
    TmThreadsSetFlag(tv, THV_KILL);
    TmThreadsSetFlag(tv, THV_DEINIT);

    /* to be sure, signal more */
    int cnt = 0;
    while (1) {
        if (TmThreadsCheckFlag(tv, THV_CLOSED)) {
            SCLogDebug("signalled the thread %" PRId32 " times", cnt);
            break;
        }

        cnt++;

        if (tv->InShutdownHandler != NULL) {
            tv->InShutdownHandler(tv);
        }
#ifndef __tile__
        /* removed for tile, at least temporarily */ 
        if (tv->inq != NULL) {
            for (i = 0; i < (tv->inq->reader_cnt + tv->inq->writer_cnt); i++) {
                if (tv->inq->q_type == 0)
#ifdef __tile__
                {
                    SCMutexLock(&trans_q[tv->inq->id].mutex_q);
                    trans_q[tv->inq->id].cond_q = 1;
                    SCMutexUnlock(&trans_q[tv->inq->id].mutex_q);
                }
#else
                    SCCondSignal(&trans_q[tv->inq->id].cond_q);
#endif
                else
#ifdef __tile__
                {
                    SCMutexLock(&data_queues[tv->inq->id].mutex_q);
                    data_queues[tv->inq->id].cond_q = 1;
                    SCMutexUnlock(&data_queues[tv->inq->id].mutex_q);
                }
#else
                    SCCondSignal(&data_queues[tv->inq->id].cond_q);
#endif
            }
            SCLogDebug("signalled tv->inq->id %" PRIu32 "", tv->inq->id);
        }
#endif /* __tile__ */

        if (tv->cond != NULL ) {
            pthread_cond_broadcast(tv->cond);
        }

        usleep(100);
    }

    if (tv->outctx != NULL) {
        Tmqh *tmqh = TmqhGetQueueHandlerByName(tv->outqh_name);
        if (tmqh == NULL)
            BUG_ON(1);

        if (tmqh->OutHandlerCtxFree != NULL) {
            tmqh->OutHandlerCtxFree(tv->outctx);
        }
    }

    /* join it */
    pthread_join(tv->t, NULL);
    SCLogDebug("thread %s stopped", tv->name);

    return;
}

/**
 * \brief Disable all threads having the specified TMs.
 */
void TmThreadDisableThreadsWithTMS(uint8_t tm_flags)
{
    /* value in seconds */
#define THREAD_KILL_MAX_WAIT_TIME 60
    /* value in microseconds */
#define WAIT_TIME 100

    double total_wait_time = 0;

    ThreadVars *tv = NULL;

    SCMutexLock(&tv_root_lock);

    /* all receive threads are part of packet processing threads */
    tv = tv_root[TVT_PPT];

    /* we do have to keep in mind that TVs are arranged in the order
     * right from receive to log.  The moment we fail to find a
     * receive TM amongst the slots in a tv, it indicates we are done
     * with all receive threads */
    while (tv) {
        int disable = 0;
        /* obtain the slots for this TV */
        TmSlot *slots = tv->tm_slots;
        while (slots != NULL) {
            TmModule *tm = TmModuleGetById(slots->tm_id);

            if (tm->flags & tm_flags) {
                disable = 1;
                break;
            }

            slots = slots->slot_next;
            continue;
        }

        if (disable) {
            if (tv->inq != NULL) {
                /* we wait till we dry out all the inq packets, before we
                 * kill this thread.  Do note that you should have disabled
                 * packet acquire by now using TmThreadDisableReceiveThreads()*/
                if (!(strlen(tv->inq->name) == strlen("packetpool") &&
                      strcasecmp(tv->inq->name, "packetpool") == 0)) {
                    PacketQueue *q = &trans_q[tv->inq->id];
                    while (q->len != 0) {
                        usleep(1000);
                    }
                }
            }

            /* we found our receive TV.  Send it a KILL signal.  This is all
             * we need to do to kill receive threads */
            TmThreadsSetFlag(tv, THV_KILL);

            if (tv->inq != NULL) {
                int i;
                for (i = 0; i < (tv->inq->reader_cnt + tv->inq->writer_cnt); i++) {
                    if (tv->inq->q_type == 0) {
#ifdef __tile__
                        /* hack to wake up tilera tmqh-simple */
                        trans_q[tv->inq->id].len = -1;
#else
                        SCCondSignal(&trans_q[tv->inq->id].cond_q);
#endif
                    } else {
#ifdef __tile__
                        /* hack to wake up tilera tmqh-simple */
                        data_queues[tv->inq->id].len = -1;
#else
                        SCCondSignal(&data_queues[tv->inq->id].cond_q);
#endif
                    }
                }
                SCLogDebug("signalled tv->inq->id %" PRIu32 "", tv->inq->id);
            }

            while (!TmThreadsCheckFlag(tv, THV_RUNNING_DONE)) {
                usleep(WAIT_TIME);
                total_wait_time += WAIT_TIME / 1000000.0;
                if (total_wait_time > THREAD_KILL_MAX_WAIT_TIME) {
                    SCLogError(SC_ERR_FATAL, "Engine unable to "
                               "disable detect thread - \"%s\".  "
                               "Killing engine", tv->name);
                    exit(EXIT_FAILURE);
                }
            }
        }

        tv = tv->next;
    }

    SCMutexUnlock(&tv_root_lock);

    return;
}

TmSlot *TmThreadGetFirstTmSlotForPartialPattern(const char *tm_name)
{
    ThreadVars *tv = NULL;
    TmSlot *slots = NULL;

    SCMutexLock(&tv_root_lock);

    /* all receive threads are part of packet processing threads */
    tv = tv_root[TVT_PPT];

    while (tv) {
        slots = tv->tm_slots;

        while (slots != NULL) {
            TmModule *tm = TmModuleGetById(slots->tm_id);

            char *found = strstr(tm->name, tm_name);
            if (found != NULL)
                goto end;

            slots = slots->slot_next;
        }

        tv = tv->next;
    }

 end:
    SCMutexUnlock(&tv_root_lock);
    return slots;
}

void TmThreadKillThreadsFamily(int family)
{
    ThreadVars *tv = NULL;

    if ((family < 0) || (family >= TVT_MAX))
        return;

    tv = tv_root[family];

    while (tv) {
        TmThreadKillThread(tv);

        tv = tv->next;
    }
}

void TmThreadKillThreads(void)
{
    int i = 0;

    for (i = 0; i < TVT_MAX; i++) {
        TmThreadKillThreadsFamily(i);
    }

    return;
}

void TmThreadFree(ThreadVars *tv)
{
    TmSlot *s;
    TmSlot *ps;
    if (tv == NULL)
        return;

    SCLogDebug("Freeing thread '%s'.", tv->name);

    SCMutexDestroy(&tv->sc_perf_pctx.m);

    s = (TmSlot *)tv->tm_slots;
    while (s) {
        ps = s;
        s = s->slot_next;
        SCFree(ps);
    }
    SCFree(tv);
}

void TmThreadClearThreadsFamily(int family)
{
    ThreadVars *tv = NULL;
    ThreadVars *ptv = NULL;

    if ((family < 0) || (family >= TVT_MAX))
        return;

    tv = tv_root[family];

    while (tv) {
        ptv = tv;
        tv = tv->next;
        TmThreadFree(ptv);
    }
    tv_root[family] = NULL;
}

/**
 * \brief Spawns a thread associated with the ThreadVars instance tv
 *
 * \retval TM_ECODE_OK on success and TM_ECODE_FAILED on failure
 */
TmEcode TmThreadSpawn(ThreadVars *tv)
{
    pthread_attr_t attr;
    if (tv->tm_func == NULL) {
        printf("ERROR: no thread function set\n");
        return TM_ECODE_FAILED;
    }

    /* Initialize and set thread detached attribute */
    pthread_attr_init(&attr);

    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

#ifdef __tile__
    int rc = pthread_create(&tv->t, &attr, TmThreadsThreadWrap, (void *)tv);
#else
    int rc = pthread_create(&tv->t, &attr, tv->tm_func, (void *)tv);
#endif
    if (rc) {
        printf("ERROR; return code from pthread_create() is %" PRId32 "\n", rc);
        return TM_ECODE_FAILED;
    }

#ifndef __tile__
    /* Disabling this on tile for the time being.
     * this doesn't play well with tilera threadvar swaping cache optimization
     * or the tilera startup barrier.  Will try to find a way to put this back.
     */
    TmThreadWaitForFlag(tv, THV_INIT_DONE | THV_RUNNING_DONE);

    TmThreadAppend(tv, tv->type);
#endif


    return TM_ECODE_OK;
}

/**
 * \brief Sets the thread flags for a thread instance(tv)
 *
 * \param tv    Pointer to the thread instance for which the flag has to be set
 * \param flags Holds the thread state this thread instance has to be set to
 */
#if 0
void TmThreadSetFlags(ThreadVars *tv, uint8_t flags)
{
    if (tv != NULL)
        tv->flags = flags;

    return;
}
#endif
/**
 * \brief Sets the aof(Action on failure) for a thread instance(tv)
 *
 * \param tv  Pointer to the thread instance for which the aof has to be set
 * \param aof Holds the aof this thread instance has to be set to
 */
void TmThreadSetAOF(ThreadVars *tv, uint8_t aof)
{
    if (tv != NULL)
        tv->aof = aof;

    return;
}

/**
 * \brief Initializes the mutex and condition variables for this TV
 *
 * \param tv Pointer to a TV instance
 */
void TmThreadInitMC(ThreadVars *tv)
{
    if ( (tv->m = SCMalloc(sizeof(SCMutex))) == NULL) {
        SCLogError(SC_ERR_FATAL, "Fatal error encountered in TmThreadInitMC.  "
                   "Exiting...");
        exit(EXIT_FAILURE);
    }

    if (SCPtMutexInit(tv->m, NULL) != 0) {
        printf("Error initializing the tv->m mutex\n");
        exit(0);
    }

    if ( (tv->cond = SCMalloc(sizeof(SCCondT))) == NULL) {
        SCLogError(SC_ERR_FATAL, "Fatal error encountered in TmThreadInitMC.  "
                   "Exiting...");
        exit(0);
    }

    if (SCPtCondInit(tv->cond, NULL) != 0) {
        SCLogError(SC_ERR_FATAL, "Error initializing the tv->cond condition "
                   "variable");
        exit(0);
    }

    return;
}

/**
 * \brief Tests if the thread represented in the arg has been unpaused or not.
 *
 *        The function would return if the thread tv has been unpaused or if the
 *        kill flag for the thread has been set.
 *
 * \param tv Pointer to the TV instance.
 */
void TmThreadTestThreadUnPaused(ThreadVars *tv)
{
    while (TmThreadsCheckFlag(tv, THV_PAUSE)) {
#ifdef __tile__
	cycle_pause(10000);
#else
        usleep(100);
#endif

        if (TmThreadsCheckFlag(tv, THV_KILL))
            break;
    }

    return;
}

/**
 * \brief Waits till the specified flag(s) is(are) set.  We don't bother if
 *        the kill flag has been set or not on the thread.
 *
 * \param tv Pointer to the TV instance.
 */
void TmThreadWaitForFlag(ThreadVars *tv, uint16_t flags)
{
    while (!TmThreadsCheckFlag(tv, flags)) {
#ifdef __tile__
	cycle_pause(10000);
#else
        usleep(100);
#endif
    }

    return;
}

/**
 * \brief Unpauses a thread
 *
 * \param tv Pointer to a TV instance that has to be unpaused
 */
void TmThreadContinue(ThreadVars *tv)
{
    TmThreadsUnsetFlag(tv, THV_PAUSE);

    return;
}

/**
 * \brief Unpauses all threads present in tv_root
 */
void TmThreadContinueThreads()
{
    ThreadVars *tv = NULL;
    int i = 0;

    for (i = 0; i < TVT_MAX; i++) {
        tv = tv_root[i];
        while (tv != NULL) {
            TmThreadContinue(tv);
            tv = tv->next;
        }
    }

    return;
}

/**
 * \brief Pauses a thread
 *
 * \param tv Pointer to a TV instance that has to be paused
 */
void TmThreadPause(ThreadVars *tv)
{
    TmThreadsSetFlag(tv, THV_PAUSE);

    return;
}

/**
 * \brief Pauses all threads present in tv_root
 */
void TmThreadPauseThreads()
{
    ThreadVars *tv = NULL;
    int i = 0;

    for (i = 0; i < TVT_MAX; i++) {
        tv = tv_root[i];
        while (tv != NULL) {
            TmThreadPause(tv);
            tv = tv->next;
        }
    }

    return;
}

/**
 * \brief Restarts the thread sent as the argument
 *
 * \param tv Pointer to the thread instance(tv) to be restarted
 */
static void TmThreadRestartThread(ThreadVars *tv)
{
    if (tv->restarted >= THV_MAX_RESTARTS) {
        SCLogError(SC_ERR_TM_THREADS_ERROR,"thread restarts exceeded "
                "threshold limit for thread \"%s\"", tv->name);
        exit(EXIT_FAILURE);
    }

    TmThreadsUnsetFlag(tv, THV_CLOSED);
    TmThreadsUnsetFlag(tv, THV_FAILED);

    if (TmThreadSpawn(tv) != TM_ECODE_OK) {
        SCLogError(SC_ERR_THREAD_SPAWN, "thread \"%s\" failed to spawn", tv->name);
        exit(EXIT_FAILURE);
    }

    tv->restarted++;
    SCLogInfo("thread \"%s\" restarted", tv->name);

    return;
}

/**
 * \brief Used to check the thread for certain conditions of failure.  If the
 *        thread has been specified to restart on failure, the thread is
 *        restarted.  If the thread has been specified to gracefully shutdown
 *        the engine on failure, it does so.  The global aof flag, tv_aof
 *        overrides the thread aof flag, if it holds a THV_ENGINE_EXIT;
 */
void TmThreadCheckThreadState(void)
{
    ThreadVars *tv = NULL;
    int i = 0;

    for (i = 0; i < TVT_MAX; i++) {
        tv = tv_root[i];

        while (tv) {
            if (TmThreadsCheckFlag(tv, THV_FAILED)) {
                TmThreadsSetFlag(tv, THV_DEINIT);
                pthread_join(tv->t, NULL);
                if ((tv_aof & THV_ENGINE_EXIT) || (tv->aof & THV_ENGINE_EXIT)) {
                    EngineKill();
                    return;
                } else {
                    /* if the engine kill-stop has been received by now, chuck
                     * restarting and return to kill the engine */
                    if ((suricata_ctl_flags & SURICATA_KILL) ||
                        (suricata_ctl_flags & SURICATA_STOP)) {
                        return;
                    }
                    TmThreadRestartThread(tv);
                }
            }
            tv = tv->next;
        }
    }

    return;
}

/**
 *  \brief Used to check if all threads have finished their initialization.  On
 *         finding an un-initialized thread, it waits till that thread completes
 *         its initialization, before proceeding to the next thread.
 *
 *  \retval TM_ECODE_OK all initialized properly
 *  \retval TM_ECODE_FAILED failure
 */
TmEcode TmThreadWaitOnThreadInit(void)
{
    ThreadVars *tv = NULL;
    int i = 0;
    uint16_t mgt_num = 0;
    uint16_t ppt_num = 0;

#ifdef __tile__
    tmc_sync_barrier_wait(&startup_barrier);
#endif

    for (i = 0; i < TVT_MAX; i++) {
        tv = tv_root[i];
        while (tv != NULL) {
            char started = FALSE;
            while (started == FALSE) {
                if (TmThreadsCheckFlag(tv, THV_INIT_DONE)) {
                    started = TRUE;
                } else {
                    /* sleep a little to give the thread some
                     * time to finish initialization */
#ifdef __tile__
                    cycle_pause(10000);
#else
                    usleep(100);
#endif
                }

                if (TmThreadsCheckFlag(tv, THV_FAILED)) {
                    SCLogError(SC_ERR_THREAD_INIT, "thread \"%s\" failed to "
                            "initialize.", tv->name);
                    return TM_ECODE_FAILED;
                }
                if (TmThreadsCheckFlag(tv, THV_CLOSED)) {
                    SCLogError(SC_ERR_THREAD_INIT, "thread \"%s\" closed on "
                            "initialization.", tv->name);
                    return TM_ECODE_FAILED;
                }
            }

            if (i == TVT_MGMT) mgt_num++;
            else if (i == TVT_PPT) ppt_num++;

            tv = tv->next;
        }
    }

    SCLogInfo("all %"PRIu16" packet processing threads, %"PRIu16" management "
              "threads initialized, engine started.", ppt_num, mgt_num);

    return TM_ECODE_OK;
}

/**
 * \brief Returns the TV for the calling thread.
 *
 * \retval tv Pointer to the ThreadVars instance for the calling thread;
 *            NULL on no match
 */
ThreadVars *TmThreadsGetCallingThread(void)
{
    pthread_t self = pthread_self();
    ThreadVars *tv = NULL;
    int i = 0;

    SCMutexLock(&tv_root_lock);

    for (i = 0; i < TVT_MAX; i++) {
        tv = tv_root[i];
        while (tv) {
            if (pthread_equal(self, tv->t)) {
                SCMutexUnlock(&tv_root_lock);
                return tv;
            }
            tv = tv->next;
        }
    }

    SCMutexUnlock(&tv_root_lock);

    return NULL;
}
