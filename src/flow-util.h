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
 */

#ifndef __FLOW_UTIL_H__
#define __FLOW_UTIL_H__

#include "detect-engine-state.h"
#ifdef __tile__
#include <tmc/spin.h>
#endif
#include "tmqh-flow.h"

#define COPY_TIMESTAMP(src,dst) ((dst)->tv_sec = (src)->tv_sec, (dst)->tv_usec = (src)->tv_usec)

#ifdef DEBUG
#define RESET_COUNTERS(f) do { \
        (f)->todstpktcnt = 0; \
        (f)->tosrcpktcnt = 0; \
        (f)->bytecnt = 0; \
    } while (0)
#else
#define RESET_COUNTERS(f)
#endif

#define FLOW_INITIALIZE(f) do { \
        (f)->sp = 0; \
        (f)->dp = 0; \
        SC_ATOMIC_INIT((f)->use_cnt); \
        (f)->probing_parser_toserver_al_proto_masks = 0; \
        (f)->probing_parser_toclient_al_proto_masks = 0; \
        (f)->flags = 0; \
        (f)->lastts_sec = 0; \
        FLOWLOCK_INIT((f)); \
        (f)->protoctx = NULL; \
        (f)->alproto = 0; \
        (f)->de_ctx_id = 0; \
        (f)->alparser = NULL; \
        (f)->alstate = NULL; \
        (f)->de_state = NULL; \
        (f)->sgh_toserver = NULL; \
        (f)->sgh_toclient = NULL; \
        (f)->tag_list = NULL; \
        (f)->flowvar = NULL; \
        SCMutexInit(&(f)->de_state_m, NULL); \
        (f)->hnext = NULL; \
        (f)->hprev = NULL; \
        (f)->lnext = NULL; \
        (f)->lprev = NULL; \
        SC_ATOMIC_INIT((f)->autofp_tmqh_flow_qid);  \
        (void) SC_ATOMIC_SET((f)->autofp_tmqh_flow_qid, -1);  \
        RESET_COUNTERS((f)); \
    } while (0)

/** \brief macro to recycle a flow before it goes into the spare queue for reuse.
 *
 *  Note that the lnext, lprev, hnext, hprev fields are untouched, those are
 *  managed by the queueing code. Same goes for fb (FlowBucket ptr) field.
 */
#define FLOW_RECYCLE(f) do { \
        (f)->sp = 0; \
        (f)->dp = 0; \
        SC_ATOMIC_RESET((f)->use_cnt); \
        (f)->probing_parser_toserver_al_proto_masks = 0; \
        (f)->probing_parser_toclient_al_proto_masks = 0; \
        (f)->flags = 0; \
        (f)->lastts_sec = 0; \
        (f)->protoctx = NULL; \
        FlowCleanupAppLayer((f)); \
        (f)->alparser = NULL; \
        (f)->alstate = NULL; \
        (f)->alproto = 0; \
        (f)->de_ctx_id = 0; \
        if ((f)->de_state != NULL) { \
            DetectEngineStateReset((f)->de_state); \
        } \
        (f)->sgh_toserver = NULL; \
        (f)->sgh_toclient = NULL; \
        DetectTagDataListFree((f)->tag_list); \
        (f)->tag_list = NULL; \
        GenericVarFree((f)->flowvar); \
        (f)->flowvar = NULL; \
        if (SC_ATOMIC_GET((f)->autofp_tmqh_flow_qid) != -1) {   \
            (void) SC_ATOMIC_SET((f)->autofp_tmqh_flow_qid, -1);   \
        }                                       \
        RESET_COUNTERS((f)); \
    } while(0)

#define FLOW_DESTROY(f) do { \
        SC_ATOMIC_DESTROY((f)->use_cnt); \
        \
        FLOWLOCK_DESTROY((f)); \
        FlowCleanupAppLayer((f)); \
        if ((f)->de_state != NULL) { \
            DetectEngineStateFree((f)->de_state); \
        } \
        DetectTagDataListFree((f)->tag_list); \
        GenericVarFree((f)->flowvar); \
        SCMutexDestroy(&(f)->de_state_m); \
        SC_ATOMIC_DESTROY((f)->autofp_tmqh_flow_qid);   \
        (f)->tag_list = NULL; \
    } while(0)

/** \brief check if a memory alloc would fit in the memcap
 *
 *  \param size memory allocation size to check
 *
 *  \retval 1 it fits
 *  \retval 0 no fit
 */
#define FLOW_CHECK_MEMCAP(size) \
    ((((uint64_t)SC_ATOMIC_GET(flow_memuse) + (uint64_t)(size)) <= flow_config.memcap))

Flow *FlowAlloc(void);
Flow *FlowAllocDirect(void);
void FlowFree(Flow *);
uint8_t FlowGetProtoMapping(uint8_t);
void FlowInit(Flow *, Packet *);
#ifdef __tile__
void FlowAllocPoolInit(void);
#endif

#endif /* __FLOW_UTIL_H__ */

