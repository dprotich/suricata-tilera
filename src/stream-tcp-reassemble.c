/* Copyright (C) 2007-2010 Open Information Security Foundation
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
 * \author Gurvinder Singh <gurvindersinghdahiya@gmail.com>
 * \author Victor Julien <victor@inliniac.net>
 *
 * Reference:
 * Judy Novak, Steve Sturges: Target-Based TCP Stream Reassembly August, 2007
 *
 */

#include "suricata-common.h"
#include "suricata.h"
#include "debug.h"
#include "detect.h"
#include "flow.h"
#include "threads.h"

#include "flow-util.h"

#include "threadvars.h"
#include "tm-threads.h"

#include "util-pool.h"
#include "util-unittest.h"
#include "util-print.h"
#include "util-host-os-info.h"
#include "util-unittest-helper.h"

#include "stream-tcp.h"
#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"
#include "stream-tcp-inline.h"
#include "stream-tcp-util.h"

#include "stream.h"

#include "util-debug.h"
#include "app-layer-protos.h"
#include "app-layer.h"

#include "detect-engine-state.h"

#include "util-profiling.h"

#define PSEUDO_PACKET_PAYLOAD_SIZE  65416 /* 64 Kb minus max IP and TCP header */

#ifdef DEBUG
static SCMutex segment_pool_memuse_mutex;
static uint64_t segment_pool_memuse = 0;
static uint64_t segment_pool_memcnt = 0;
#endif

/* We define several pools with prealloced segments with fixed size
 * payloads. We do this to prevent having to do an SCMalloc call for every
 * data segment we receive, which would be a large performance penalty.
 * The cost is in memory of course. */
#define segment_pool_num 8
static uint16_t segment_pool_pktsizes[segment_pool_num] = {4, 16, 112, 248, 512,
                                                           768, 1448, 0xffff};
//static uint16_t segment_pool_poolsizes[segment_pool_num] = {2048, 3072, 3072,
//                                                            3072, 3072, 8192,
//                                                            8192, 512};
static uint16_t segment_pool_poolsizes[segment_pool_num] = {0, 0, 0,
                                                            0, 0, 0,
                                                            0, 0};
static uint16_t segment_pool_poolsizes_prealloc[segment_pool_num] = {256, 512, 512,
                                                            512, 512, 1024,
                                                            1024, 128};
static Pool *segment_pool[segment_pool_num];
static SCMutex segment_pool_mutex[segment_pool_num];
#ifdef DEBUG
static SCMutex segment_pool_cnt_mutex;
static uint64_t segment_pool_cnt = 0;
#endif
/* index to the right pool for all packet sizes. */
static uint16_t segment_pool_idx[65536]; /* O(1) lookups of the pool */
static int check_overlap_different_data = 0;

/* Memory use counter */
SC_ATOMIC_DECLARE(uint64_t, ra_memuse);

/* prototypes */
static int HandleSegmentStartsBeforeListSegment(ThreadVars *, TcpReassemblyThreadCtx *,
                                    TcpStream *, TcpSegment *, TcpSegment *, Packet *);
static int HandleSegmentStartsAtSameListSegment(ThreadVars *, TcpReassemblyThreadCtx *,
                                    TcpStream *, TcpSegment *, TcpSegment *, Packet *);
static int HandleSegmentStartsAfterListSegment(ThreadVars *, TcpReassemblyThreadCtx *,
                                    TcpStream *, TcpSegment *, TcpSegment *, Packet *);
void StreamTcpSegmentDataReplace(TcpSegment *, TcpSegment *, uint32_t, uint16_t);
void StreamTcpSegmentDataCopy(TcpSegment *, TcpSegment *);
TcpSegment* StreamTcpGetSegment(ThreadVars *tv, TcpReassemblyThreadCtx *, uint16_t);
void StreamTcpCreateTestPacket(uint8_t *, uint8_t, uint8_t, uint8_t);
void StreamTcpReassemblePseudoPacketCreate(TcpStream *, Packet *, PacketQueue *);
static int StreamTcpSegmentDataCompare(TcpSegment *dst_seg, TcpSegment *src_seg,
                                 uint32_t start_point, uint16_t len);

void StreamTcpReassembleConfigEnableOverlapCheck(void) {
    check_overlap_different_data = 1;
}

/**
 *  \brief  Function to Increment the memory usage counter for the TCP reassembly
 *          segments
 *
 *  \param  size Size of the TCP segment and its payload length memory allocated
 */
void StreamTcpReassembleIncrMemuse(uint64_t size) {
    (void) SC_ATOMIC_ADD(ra_memuse, size);
    return;
}

/**
 *  \brief  Function to Decrease the memory usage counter for the TCP reassembly
 *          segments
 *
 *  \param  size Size of the TCP segment and its payload length memory allocated
 */
void StreamTcpReassembleDecrMemuse(uint64_t size) {
    (void) SC_ATOMIC_SUB(ra_memuse, size);
    return;
}

void StreamTcpReassembleMemuseCounter(ThreadVars *tv, TcpReassemblyThreadCtx *rtv) {
    uint64_t smemuse = SC_ATOMIC_GET(ra_memuse);
    if (tv != NULL && rtv != NULL)
        SCPerfCounterSetUI64(rtv->counter_tcp_reass_memuse, tv->sc_perf_pca, smemuse);
    return;
}

/**
 * \brief  Function to Check the reassembly memory usage counter against the
 *         allowed max memory usgae for TCP segments.
 *
 * \param  size Size of the TCP segment and its payload length memory allocated
 * \retval 1 if in bounds
 * \retval 0 if not in bounds
 */
int StreamTcpReassembleCheckMemcap(uint32_t size) {
    if (stream_config.reassembly_memcap == 0 || size + SC_ATOMIC_GET(ra_memuse) <= stream_config.reassembly_memcap)
        return 1;
    return 0;
}

/** \brief alloc a tcp segment pool entry */
void *TcpSegmentPoolAlloc()
{
    if (StreamTcpReassembleCheckMemcap((uint32_t)sizeof(TcpSegment)) == 0)
    {
        return NULL;
    }

    TcpSegment *seg = NULL;

    seg = SCMalloc(sizeof (TcpSegment));
    if (unlikely(seg == NULL))
        return NULL;
    return seg;
}

int TcpSegmentPoolInit(void *data, void *payload_len)
{
    TcpSegment *seg = (TcpSegment *) data;

    memset(seg, 0, sizeof (TcpSegment));

    seg->pool_size = *((uint16_t *) payload_len);
    seg->payload_len = seg->pool_size;

    seg->payload = SCMalloc(seg->payload_len);
    if (seg->payload == NULL) {
        SCFree(seg);
        return 0;
    }

#ifdef DEBUG
    SCMutexLock(&segment_pool_memuse_mutex);
    segment_pool_memuse += seg->payload_len;
    segment_pool_memcnt++;
    SCLogDebug("segment_pool_memcnt %"PRIu64"", segment_pool_memcnt);
    SCMutexUnlock(&segment_pool_memuse_mutex);
#endif

    StreamTcpReassembleIncrMemuse((uint32_t)seg->pool_size + sizeof(TcpSegment));
    return 1;
}

/** \brief clean up a tcp segment pool entry */
void TcpSegmentPoolCleanup(void *ptr) {
    if (ptr == NULL)
        return;

    TcpSegment *seg = (TcpSegment *) ptr;

    StreamTcpReassembleDecrMemuse((uint32_t)seg->pool_size + sizeof(TcpSegment));

#ifdef DEBUG
    SCMutexLock(&segment_pool_memuse_mutex);
    segment_pool_memuse -= seg->pool_size;
    segment_pool_memcnt--;
    SCLogDebug("segment_pool_memcnt %"PRIu64"", segment_pool_memcnt);
    SCMutexUnlock(&segment_pool_memuse_mutex);
#endif

    SCFree(seg->payload);
    return;
}

/**
 *  \brief Function to return the segment back to the pool.
 *
 *  \param seg Segment which will be returned back to the pool.
 */
void StreamTcpSegmentReturntoPool(TcpSegment *seg)
{
    if (seg == NULL)
        return;

    seg->next = NULL;
    seg->prev = NULL;

    uint16_t idx = segment_pool_idx[seg->pool_size];
    SCMutexLock(&segment_pool_mutex[idx]);
    PoolReturn(segment_pool[idx], (void *) seg);
    SCLogDebug("segment_pool[%"PRIu16"]->empty_list_size %"PRIu32"",
               idx,segment_pool[idx]->empty_list_size);
    SCMutexUnlock(&segment_pool_mutex[idx]);

#ifdef DEBUG
    SCMutexLock(&segment_pool_cnt_mutex);
    segment_pool_cnt--;
    SCMutexUnlock(&segment_pool_cnt_mutex);
#endif
}

/**
 *  \brief return all segments in this stream into the pool(s)
 *
 *  \param stream the stream to cleanup
 */
void StreamTcpReturnStreamSegments (TcpStream *stream)
{
    TcpSegment *seg = stream->seg_list;
    TcpSegment *next_seg;

    if (seg == NULL)
        return;

    while (seg != NULL) {
        next_seg = seg->next;
        StreamTcpSegmentReturntoPool(seg);
        seg = next_seg;
    }

    stream->seg_list = NULL;
    stream->seg_list_tail = NULL;
}

int StreamTcpReassembleInit(char quiet)
{
    StreamMsgQueuesInit();

    /* init the memcap/use tracker */
    SC_ATOMIC_INIT(ra_memuse);

#ifdef DEBUG
    SCMutexInit(&segment_pool_memuse_mutex, NULL);
#endif
    uint16_t u16 = 0;
    for (u16 = 0; u16 < segment_pool_num; u16++)
    {
        SCMutexInit(&segment_pool_mutex[u16], NULL);
        SCMutexLock(&segment_pool_mutex[u16]);
        segment_pool[u16] = PoolInit(segment_pool_poolsizes[u16],
                                     segment_pool_poolsizes_prealloc[u16],
                                     sizeof (TcpSegment),
                                     TcpSegmentPoolAlloc, TcpSegmentPoolInit,
                                     (void *) &segment_pool_pktsizes[u16],
                                     TcpSegmentPoolCleanup, NULL);
        SCMutexUnlock(&segment_pool_mutex[u16]);
    }

    uint16_t idx = 0;
    u16 = 0;
    while (1) {
        if (idx <= segment_pool_pktsizes[u16]) {
            segment_pool_idx[idx] = u16;
            if (segment_pool_pktsizes[u16] == idx)
                u16++;
        }

        if (idx == 0xffff)
            break;

        idx++;
    }
#ifdef DEBUG
    SCMutexInit(&segment_pool_cnt_mutex, NULL);
#endif
    return 0;
}

#ifdef DEBUG
extern uint32_t applayererrors;
extern uint32_t applayerhttperrors;
static uint32_t dbg_app_layer_gap;
static uint32_t dbg_app_layer_gap_candidate;
#endif

void StreamTcpReassembleFree(char quiet)
{
    uint16_t u16 = 0;
    for (u16 = 0; u16 < segment_pool_num; u16++) {
        SCMutexLock(&segment_pool_mutex[u16]);

        if (quiet == FALSE) {
            PoolPrintSaturation(segment_pool[u16]);
            SCLogDebug("segment_pool[u16]->empty_list_size %"PRIu32", "
                       "segment_pool[u16]->alloc_list_size %"PRIu32", alloced "
                       "%"PRIu32"", segment_pool[u16]->empty_list_size,
                       segment_pool[u16]->alloc_list_size,
                       segment_pool[u16]->allocated);
        }
        PoolFree(segment_pool[u16]);

        SCMutexUnlock(&segment_pool_mutex[u16]);
        SCMutexDestroy(&segment_pool_mutex[u16]);
    }

    StreamMsgQueuesDeinit(quiet);

#ifdef DEBUG
    SCLogDebug("segment_pool_cnt %"PRIu64"", segment_pool_cnt);
    SCLogDebug("segment_pool_memuse %"PRIu64"", segment_pool_memuse);
    SCLogDebug("segment_pool_memcnt %"PRIu64"", segment_pool_memcnt);
    SCMutexDestroy(&segment_pool_memuse_mutex);
    SCMutexDestroy(&segment_pool_cnt_mutex);
    SCLogInfo("applayererrors %u", applayererrors);
    SCLogInfo("applayerhttperrors %u", applayerhttperrors);
    SCLogInfo("dbg_app_layer_gap %u", dbg_app_layer_gap);
    SCLogInfo("dbg_app_layer_gap_candidate %u", dbg_app_layer_gap_candidate);
#endif
}

TcpReassemblyThreadCtx *StreamTcpReassembleInitThreadCtx(ThreadVars *tv)
{
    SCEnter();
    TcpReassemblyThreadCtx *ra_ctx = SCThreadMalloc(tv, sizeof(TcpReassemblyThreadCtx));
    if (unlikely(ra_ctx == NULL))
        return NULL;

    memset(ra_ctx, 0x00, sizeof(TcpReassemblyThreadCtx));
    ra_ctx->stream_q = StreamMsgQueueGetNew();

    AlpProtoFinalize2Thread(tv, &ra_ctx->dp_ctx);
    SCReturnPtr(ra_ctx, "TcpReassemblyThreadCtx");
}

void StreamTcpReassembleFreeThreadCtx(TcpReassemblyThreadCtx *ra_ctx)
{
    SCEnter();
    if (ra_ctx->stream_q != NULL) {
        StreamMsg *smsg;
        while ((smsg = StreamMsgGetFromQueue(ra_ctx->stream_q)) != NULL) {
            StreamMsgReturnToPool(smsg);
        }

        StreamMsgQueueFree(ra_ctx->stream_q);
    }

    ra_ctx->stream_q = NULL;
    AlpProtoDeFinalize2Thread(&ra_ctx->dp_ctx);
    SCFree(ra_ctx);
    SCReturn;
}

void PrintList2(TcpSegment *seg)
{
    TcpSegment *prev_seg = NULL;

    if (seg == NULL)
        return;

    uint32_t next_seq = seg->seq;

    while (seg != NULL) {
        if (SEQ_LT(next_seq,seg->seq)) {
            SCLogDebug("missing segment(s) for %" PRIu32 " bytes of data",
                        (seg->seq - next_seq));
        }

        SCLogDebug("seg %10"PRIu32" len %" PRIu16 ", seg %p, prev %p, next %p",
                    seg->seq, seg->payload_len, seg, seg->prev, seg->next);

        if (seg->prev != NULL && SEQ_LT(seg->seq,seg->prev->seq)) {
            /* check for SEQ_LT cornercase where a - b is exactly 2147483648,
             * which makes the marco return TRUE in both directions. This is
             * a hack though, we're going to check next how we end up with
             * a segment list with seq differences that big */
            if (!(SEQ_LT(seg->prev->seq,seg->seq))) {
                SCLogDebug("inconsistent list: SEQ_LT(seg->seq,seg->prev->seq)) =="
                        " TRUE, seg->seq %" PRIu32 ", seg->prev->seq %" PRIu32 ""
                        "", seg->seq, seg->prev->seq);
            }
        }

        if (SEQ_LT(seg->seq,next_seq)) {
            SCLogDebug("inconsistent list: SEQ_LT(seg->seq,next_seq)) == TRUE, "
                       "seg->seq %" PRIu32 ", next_seq %" PRIu32 "", seg->seq,
                       next_seq);
        }

        if (prev_seg != seg->prev) {
            SCLogDebug("inconsistent list: prev_seg %p != seg->prev %p",
                        prev_seg, seg->prev);
        }

        next_seq = seg->seq + seg->payload_len;
        SCLogDebug("next_seq is now %"PRIu32"", next_seq);
        prev_seg = seg;
        seg = seg->next;
    }
}

void PrintList(TcpSegment *seg)
{
    TcpSegment *prev_seg = NULL;
    TcpSegment *head_seg = seg;

    if (seg == NULL)
        return;

    uint32_t next_seq = seg->seq;

    while (seg != NULL) {
        if (SEQ_LT(next_seq,seg->seq)) {
            SCLogDebug("missing segment(s) for %" PRIu32 " bytes of data",
                        (seg->seq - next_seq));
        }

        SCLogDebug("seg %10"PRIu32" len %" PRIu16 ", seg %p, prev %p, next %p, flags 0x%02x",
                    seg->seq, seg->payload_len, seg, seg->prev, seg->next, seg->flags);

        if (seg->prev != NULL && SEQ_LT(seg->seq,seg->prev->seq)) {
            /* check for SEQ_LT cornercase where a - b is exactly 2147483648,
             * which makes the marco return TRUE in both directions. This is
             * a hack though, we're going to check next how we end up with
             * a segment list with seq differences that big */
            if (!(SEQ_LT(seg->prev->seq,seg->seq))) {
                SCLogDebug("inconsistent list: SEQ_LT(seg->seq,seg->prev->seq)) == "
                        "TRUE, seg->seq %" PRIu32 ", seg->prev->seq %" PRIu32 "",
                        seg->seq, seg->prev->seq);
                PrintList2(head_seg);
                abort();
            }
        }

        if (SEQ_LT(seg->seq,next_seq)) {
            SCLogDebug("inconsistent list: SEQ_LT(seg->seq,next_seq)) == TRUE, "
                       "seg->seq %" PRIu32 ", next_seq %" PRIu32 "", seg->seq,
                       next_seq);
            PrintList2(head_seg);
            abort();
        }

        if (prev_seg != seg->prev) {
            SCLogDebug("inconsistent list: prev_seg %p != seg->prev %p",
                       prev_seg, seg->prev);
            PrintList2(head_seg);
            abort();
        }

        next_seq = seg->seq + seg->payload_len;
        SCLogDebug("next_seq is now %"PRIu32"", next_seq);
        prev_seg = seg;
        seg = seg->next;
    }
}

/**
 *  \internal
 *  \brief Get the active ra_base_seq, considering stream gaps
 *
 *  \retval seq the active ra_base_seq
 */
static inline uint32_t StreamTcpReassembleGetRaBaseSeq(TcpStream *stream)
{
    if (!(stream->flags & STREAMTCP_STREAM_FLAG_GAP)) {
        SCReturnUInt(stream->ra_app_base_seq);
    } else {
        SCReturnUInt(stream->ra_raw_base_seq);
    }
}

/**
 *  \internal
 *  \brief  Function to handle the insertion newly arrived segment,
 *          The packet is handled based on its target OS.
 *
 *  \param  stream  The given TCP stream to which this new segment belongs
 *  \param  seg     Newly arrived segment
 *  \param  p       received packet
 *
 *  \retval 0  success
 *  \retval -1 error -- either we hit a memory issue (OOM/memcap) or we received
 *             a segment before ra_base_seq.
 */
int StreamTcpReassembleInsertSegment(ThreadVars *tv, TcpReassemblyThreadCtx *ra_ctx,
        TcpStream *stream, TcpSegment *seg, Packet *p)
{
    SCEnter();

    TcpSegment *list_seg = stream->seg_list;
    TcpSegment *next_list_seg = NULL;

#if DEBUG
    PrintList(stream->seg_list);
#endif

    int ret_value = 0;
    char return_seg = FALSE;

    /* before our ra_app_base_seq we don't insert it in our list,
     * or ra_raw_base_seq if in stream gap state */
    if (SEQ_LT((TCP_GET_SEQ(p)+p->payload_len),(StreamTcpReassembleGetRaBaseSeq(stream)+1)))
    {
        SCLogDebug("not inserting: SEQ+payload %"PRIu32", last_ack %"PRIu32", "
                "ra_(app|raw)_base_seq %"PRIu32, (TCP_GET_SEQ(p)+p->payload_len),
                stream->last_ack, StreamTcpReassembleGetRaBaseSeq(stream)+1);
        return_seg = TRUE;
        ret_value = -1;

        StreamTcpSetEvent(p, STREAM_REASSEMBLY_SEGMENT_BEFORE_BASE_SEQ);
        goto end;
    }

    SCLogDebug("SEQ %"PRIu32", SEQ+payload %"PRIu32", last_ack %"PRIu32", "
            "ra_app_base_seq %"PRIu32, TCP_GET_SEQ(p), (TCP_GET_SEQ(p)+p->payload_len),
            stream->last_ack, stream->ra_app_base_seq);

    if (seg == NULL) {
        goto end;
    }

    /* fast track */
    if (list_seg == NULL) {
        SCLogDebug("empty list, inserting seg %p seq %" PRIu32 ", "
                   "len %" PRIu32 "", seg, seg->seq, seg->payload_len);
        stream->seg_list = seg;
        seg->prev = NULL;
        stream->seg_list_tail = seg;
        goto end;
    }

    /* insert the segment in the stream list using this fast track, if seg->seq
       is equal or higher than stream->seg_list_tail.*/
    if (SEQ_GEQ(seg->seq, (stream->seg_list_tail->seq +
            stream->seg_list_tail->payload_len)))
    {
        stream->seg_list_tail->next = seg;
        seg->prev = stream->seg_list_tail;
        stream->seg_list_tail = seg;

        goto end;
    }

    /* If the OS policy is not set then set the OS policy for this stream */
    if (stream->os_policy == 0) {
        StreamTcpSetOSPolicy(stream, p);
    }

    for (; list_seg != NULL; list_seg = next_list_seg) {
        next_list_seg = list_seg->next;

        SCLogDebug("seg %p, list_seg %p, list_prev %p list_seg->next %p, "
                   "segment length %" PRIu32 "", seg, list_seg, list_seg->prev,
                   list_seg->next, seg->payload_len);
        SCLogDebug("seg->seq %"PRIu32", list_seg->seq %"PRIu32"",
                   seg->seq, list_seg->seq);

        /* segment starts before list */
        if (SEQ_LT(seg->seq, list_seg->seq)) {
            /* seg is entirely before list_seg */
            if (SEQ_LEQ((seg->seq + seg->payload_len), list_seg->seq)) {
                SCLogDebug("before list seg: seg->seq %" PRIu32 ", list_seg->seq"
                           " %" PRIu32 ", list_seg->payload_len %" PRIu32 ", "
                           "list_seg->prev %p", seg->seq, list_seg->seq,
                           list_seg->payload_len, list_seg->prev);
                seg->next = list_seg;
                if (list_seg->prev == NULL) {
                    stream->seg_list = seg;
                }
                if (list_seg->prev != NULL) {
                    list_seg->prev->next = seg;
                    seg->prev = list_seg->prev;
                }
                list_seg->prev = seg;

                goto end;

            /* seg overlap with next seg(s) */
            } else {
                ret_value = HandleSegmentStartsBeforeListSegment(tv, ra_ctx, stream, list_seg, seg, p);
                if (ret_value == 1) {
                    ret_value = 0;
                    return_seg = TRUE;
                    goto end;
                } else if (ret_value == -1) {
                    SCLogDebug("HandleSegmentStartsBeforeListSegment failed");
                    ret_value = -1;
                    return_seg = TRUE;
                    goto end;
                }
            }

        /* seg starts at same sequence number as list_seg */
        } else if (SEQ_EQ(seg->seq, list_seg->seq)) {
            ret_value = HandleSegmentStartsAtSameListSegment(tv, ra_ctx, stream, list_seg, seg, p);
            if (ret_value == 1) {
                ret_value = 0;
                return_seg = TRUE;
                goto end;
            } else if (ret_value == -1) {
                SCLogDebug("HandleSegmentStartsAtSameListSegment failed");
                ret_value = -1;
                return_seg = TRUE;
                goto end;
            }

        /* seg starts at sequence number higher than list_seg */
        } else if (SEQ_GT(seg->seq, list_seg->seq)) {
            if (((SEQ_GEQ(seg->seq, (list_seg->seq + list_seg->payload_len))))
                    && SEQ_GT((seg->seq + seg->payload_len),
                    (list_seg->seq + list_seg->payload_len)))
            {
                SCLogDebug("starts beyond list end, ends after list end: "
                           "seg->seq %" PRIu32 ", list_seg->seq %" PRIu32 ", "
                           "list_seg->payload_len %" PRIu32 " (%" PRIu32 ")",
                           seg->seq, list_seg->seq, list_seg->payload_len,
                           list_seg->seq + list_seg->payload_len);

                if (list_seg->next == NULL) {
                    list_seg->next = seg;
                    seg->prev = list_seg;
                    stream->seg_list_tail = seg;
                    goto end;
                }
            } else {
                ret_value = HandleSegmentStartsAfterListSegment(tv, ra_ctx, stream, list_seg, seg, p);
                if (ret_value == 1) {
                    ret_value = 0;
                    return_seg = TRUE;
                    goto end;
                } else if (ret_value == -1) {
                    SCLogDebug("HandleSegmentStartsAfterListSegment failed");
                    ret_value = -1;
                    return_seg = TRUE;
                    goto end;
                }
            }
        }
    }

end:
    if (return_seg == TRUE && seg != NULL) {
        StreamTcpSegmentReturntoPool(seg);
    }

#ifdef DEBUG
    PrintList(stream->seg_list);
#endif
    SCReturnInt(ret_value);
}

/**
 *  \brief Function to handle the newly arrived segment, when newly arrived
 *         starts with the sequence number lower than the original segment and
 *         ends at different position relative to original segment.
 *         The packet is handled based on its target OS.
 *
 *  \param list_seg Original Segment in the stream
 *  \param seg      Newly arrived segment
 *  \param prev_seg Previous segment in the stream segment list
 *  \param p        Packet
 *
 *  \retval 1 success and done
 *  \retval 0 success, but not done yet
 *  \retval -1 error, will *only* happen on memory errors
 */

static int HandleSegmentStartsBeforeListSegment(ThreadVars *tv, TcpReassemblyThreadCtx *ra_ctx,
        TcpStream *stream, TcpSegment *list_seg, TcpSegment *seg, Packet *p)
{
    SCEnter();

    uint16_t overlap = 0;
    uint16_t packet_length = 0;
    uint32_t overlap_point = 0;
    char end_before = FALSE;
    char end_after = FALSE;
    char end_same = FALSE;
    char return_after = FALSE;
    uint8_t os_policy = stream->os_policy;
#ifdef DEBUG
    SCLogDebug("seg->seq %" PRIu32 ", seg->payload_len %" PRIu32 "", seg->seq,
                seg->payload_len);
    PrintList(stream->seg_list);
#endif

    if (SEQ_GT((seg->seq + seg->payload_len), list_seg->seq) &&
        SEQ_LT((seg->seq + seg->payload_len),(list_seg->seq +
                                                        list_seg->payload_len)))
    {
        /* seg starts before list seg, ends beyond it but before list end */
        end_before = TRUE;

        /* [aaaa[abab]bbbb] a = seg, b = list_seg, overlap is the part [abab]
         * We know seg->seq + seg->payload_len is bigger than list_seg->seq */
        overlap = (seg->seq + seg->payload_len) - list_seg->seq;
        overlap_point = list_seg->seq;
        SCLogDebug("starts before list seg, ends before list end: seg->seq "
                   "%" PRIu32 ", list_seg->seq %" PRIu32 ", "
                   "list_seg->payload_len %" PRIu16 " overlap is %" PRIu32 ", "
                   "overlap point %"PRIu32"", seg->seq, list_seg->seq,
                   list_seg->payload_len, overlap, overlap_point);
    } else if (SEQ_EQ((seg->seq + seg->payload_len), (list_seg->seq +
                                                        list_seg->payload_len)))
    {
        /* seg fully overlaps list_seg, starts before, at end point
         * [aaa[ababab]] where a = seg, b = list_seg
         * overlap is [ababab], which is list_seg->payload_len */
        overlap = list_seg->payload_len;
        end_same = TRUE;
        overlap_point = list_seg->seq;
        SCLogDebug("starts before list seg, ends at list end: list prev %p"
                   "seg->seq %" PRIu32 ", list_seg->seq %" PRIu32 ","
                   "list_seg->payload_len %" PRIu32 " overlap is %" PRIu32 "",
                   list_seg->prev, seg->seq, list_seg->seq,
                   list_seg->payload_len, overlap);
        /* seg fully overlaps list_seg, starts before, ends after list endpoint */
    } else if (SEQ_GT((seg->seq + seg->payload_len), (list_seg->seq +
                                                        list_seg->payload_len)))
    {
        /* seg fully overlaps list_seg, starts before, ends after list endpoint
         * [aaa[ababab]aaa] where a = seg, b = list_seg
         * overlap is [ababab] which is list_seg->payload_len */
        overlap = list_seg->payload_len;
        end_after = TRUE;
        overlap_point = list_seg->seq;
        SCLogDebug("starts before list seg, ends after list end: seg->seq "
                   "%" PRIu32 ", seg->payload_len %"PRIu32" list_seg->seq "
                   "%" PRIu32 ", list_seg->payload_len %" PRIu32 " overlap is"
                   " %" PRIu32 "", seg->seq, seg->payload_len,
                   list_seg->seq, list_seg->payload_len, overlap);
    }

    if (overlap > 0) {
        /* handle the case where we need to fill a gap before list_seg first */
        if (list_seg->prev != NULL && SEQ_LT((list_seg->prev->seq + list_seg->prev->payload_len), list_seg->seq)) {
            SCLogDebug("GAP to fill before list segment, size %u", list_seg->seq - (list_seg->prev->seq + list_seg->prev->payload_len));

            uint32_t new_seq = (list_seg->prev->seq + list_seg->prev->payload_len);
            if (SEQ_GT(seg->seq, new_seq)) {
                new_seq = seg->seq;
            }

            packet_length = list_seg->seq - new_seq;
            if (packet_length > seg->payload_len) {
                packet_length = seg->payload_len;
            }

            TcpSegment *new_seg = StreamTcpGetSegment(tv, ra_ctx, packet_length);
            if (new_seg == NULL) {
                SCLogDebug("segment_pool[%"PRIu16"] is empty", segment_pool_idx[packet_length]);

                StreamTcpSetEvent(p, STREAM_REASSEMBLY_NO_SEGMENT);
                SCReturnInt(-1);
            }
            new_seg->payload_len = packet_length;

            new_seg->seq = new_seq;

            SCLogDebug("new_seg->seq %"PRIu32" and new->payload_len "
                    "%" PRIu16"", new_seg->seq, new_seg->payload_len);

            new_seg->next = list_seg;
            new_seg->prev = list_seg->prev;
            list_seg->prev->next = new_seg;
            list_seg->prev = new_seg;

            /* create a new seg, copy the list_seg data over */
            StreamTcpSegmentDataCopy(new_seg, seg);

#ifdef DEBUG
            PrintList(stream->seg_list);
#endif
        }

        /* Handling case when the segment starts before the first segment in
         * the list */
        if (list_seg->prev == NULL) {
            if (end_after == TRUE && list_seg->next != NULL &&
                    SEQ_LT(list_seg->next->seq, (seg->seq + seg->payload_len)))
            {
                packet_length = (list_seg->seq - seg->seq) + list_seg->payload_len;
            } else {
                packet_length = seg->payload_len + (list_seg->payload_len - overlap);
                return_after = TRUE;
            }

            SCLogDebug("entered here packet_length %" PRIu32 ", seg->payload_len"
                       " %" PRIu32 ", list->payload_len %" PRIu32 "",
                       packet_length, seg->payload_len, list_seg->payload_len);

            TcpSegment *new_seg = StreamTcpGetSegment(tv, ra_ctx, packet_length);
            if (new_seg == NULL) {
                SCLogDebug("segment_pool[%"PRIu16"] is empty", segment_pool_idx[packet_length]);

                StreamTcpSetEvent(p, STREAM_REASSEMBLY_NO_SEGMENT);
                SCReturnInt(-1);
            }
            new_seg->payload_len = packet_length;
            new_seg->seq = seg->seq;
            new_seg->next = list_seg->next;
            new_seg->prev = list_seg->prev;

            StreamTcpSegmentDataCopy(new_seg, list_seg);

            /* first the data before the list_seg->seq */
            uint16_t replace = (uint16_t) (list_seg->seq - seg->seq);
            SCLogDebug("copying %"PRIu16" bytes to new_seg", replace);
            StreamTcpSegmentDataReplace(new_seg, seg, seg->seq, replace);

            /* if any, data after list_seg->seq + list_seg->payload_len */
            if (SEQ_GT((seg->seq + seg->payload_len), (list_seg->seq +
                    list_seg->payload_len)) && return_after == TRUE)
            {
                replace = (uint16_t)(((seg->seq + seg->payload_len) -
                                             (list_seg->seq +
                                              list_seg->payload_len)));
                SCLogDebug("replacing %"PRIu16"", replace);
                StreamTcpSegmentDataReplace(new_seg, seg, (list_seg->seq +
                                             list_seg->payload_len), replace);
            }

            /* update the stream last_seg in case of removal of list_seg */
            if (stream->seg_list_tail == list_seg)
                stream->seg_list_tail = new_seg;

            StreamTcpSegmentReturntoPool(list_seg);
            list_seg = new_seg;
            if (new_seg->prev != NULL) {
                new_seg->prev->next = new_seg;
            }
            if (new_seg->next != NULL) {
                new_seg->next->prev = new_seg;
            }

            stream->seg_list = new_seg;
            SCLogDebug("list_seg now %p, stream->seg_list now %p", list_seg,
                        stream->seg_list);

        } else if (end_before == TRUE || end_same == TRUE) {
            /* Handling overlapping with more than one segment and filling gap */
            if (SEQ_GT(list_seg->seq, (list_seg->prev->seq +
                                   list_seg->prev->payload_len)))
            {
                SCLogDebug("list_seg->prev %p list_seg->prev->seq %"PRIu32" "
                           "list_seg->prev->payload_len %"PRIu16"",
                            list_seg->prev, list_seg->prev->seq,
                            list_seg->prev->payload_len);
                if (SEQ_LT(list_seg->prev->seq, seg->seq)) {
                    packet_length = list_seg->payload_len + (list_seg->seq -
                                                                    seg->seq);
                } else {
                    packet_length = list_seg->payload_len + (list_seg->seq -
                           (list_seg->prev->seq + list_seg->prev->payload_len));
                }

                TcpSegment *new_seg = StreamTcpGetSegment(tv, ra_ctx, packet_length);
                if (new_seg == NULL) {
                    SCLogDebug("segment_pool[%"PRIu16"] is empty", segment_pool_idx[packet_length]);

                    StreamTcpSetEvent(p, STREAM_REASSEMBLY_NO_SEGMENT);
                    SCReturnInt(-1);
                }

                new_seg->payload_len = packet_length;
                if (SEQ_GT((list_seg->prev->seq + list_seg->prev->payload_len),
                        seg->seq))
                {
                    new_seg->seq = (list_seg->prev->seq +
                                    list_seg->prev->payload_len);
                } else {
                    new_seg->seq = seg->seq;
                }
                SCLogDebug("new_seg->seq %"PRIu32" and new->payload_len "
                           "%" PRIu16"", new_seg->seq, new_seg->payload_len);
                new_seg->next = list_seg->next;
                new_seg->prev = list_seg->prev;

                StreamTcpSegmentDataCopy(new_seg, list_seg);

                uint16_t copy_len = (uint16_t) (list_seg->seq - seg->seq);
                SCLogDebug("copy_len %" PRIu32 " (%" PRIu32 " - %" PRIu32 ")",
                            copy_len, list_seg->seq, seg->seq);
                StreamTcpSegmentDataReplace(new_seg, seg, seg->seq, copy_len);

                /*update the stream last_seg in case of removal of list_seg*/
                if (stream->seg_list_tail == list_seg)
                    stream->seg_list_tail = new_seg;

                StreamTcpSegmentReturntoPool(list_seg);
                list_seg = new_seg;
                if (new_seg->prev != NULL) {
                    new_seg->prev->next = new_seg;
                }
                if (new_seg->next != NULL) {
                    new_seg->next->prev = new_seg;
                }
            }
        } else if (end_after == TRUE) {
            if (list_seg->next != NULL) {
                if (SEQ_LEQ((seg->seq + seg->payload_len), list_seg->next->seq))
                {
                    if (SEQ_GT(seg->seq, (list_seg->prev->seq +
                                list_seg->prev->payload_len)))
                    {
                        packet_length = list_seg->payload_len + (list_seg->seq -
                                                                 seg->seq);
                    } else {
                        packet_length = list_seg->payload_len + (list_seg->seq -
                                                (list_seg->prev->seq +
                                                 list_seg->prev->payload_len));
                    }

                    packet_length += (seg->seq + seg->payload_len) -
                                        (list_seg->seq + list_seg->payload_len);

                    TcpSegment *new_seg = StreamTcpGetSegment(tv, ra_ctx, packet_length);
                    if (new_seg == NULL) {
                        SCLogDebug("segment_pool[%"PRIu16"] is empty", segment_pool_idx[packet_length]);

                        StreamTcpSetEvent(p, STREAM_REASSEMBLY_NO_SEGMENT);
                        SCReturnInt(-1);
                    }
                    new_seg->payload_len = packet_length;
                    if (SEQ_GT((list_seg->prev->seq +
                                    list_seg->prev->payload_len), seg->seq))
                    {
                        new_seg->seq = (list_seg->prev->seq +
                                            list_seg->prev->payload_len);
                    } else {
                        new_seg->seq = seg->seq;
                    }
                    SCLogDebug("new_seg->seq %"PRIu32" and new->payload_len "
                           "%" PRIu16"", new_seg->seq, new_seg->payload_len);
                    new_seg->next = list_seg->next;
                    new_seg->prev = list_seg->prev;

                    /* create a new seg, copy the list_seg data over */
                    StreamTcpSegmentDataCopy(new_seg, list_seg);

                    /* copy the part before list_seg */
                    uint16_t copy_len = list_seg->seq - new_seg->seq;
                    StreamTcpSegmentDataReplace(new_seg, seg, new_seg->seq,
                                                copy_len);

                    /* copy the part after list_seg */
                    copy_len = (seg->seq + seg->payload_len) -
                                    (list_seg->seq + list_seg->payload_len);
                    StreamTcpSegmentDataReplace(new_seg, seg, (list_seg->seq +
                                              list_seg->payload_len), copy_len);

                    if (new_seg->prev != NULL) {
                        new_seg->prev->next = new_seg;
                    }
                    if (new_seg->next != NULL) {
                        new_seg->next->prev = new_seg;
                    }
                    /*update the stream last_seg in case of removal of list_seg*/
                    if (stream->seg_list_tail == list_seg)
                        stream->seg_list_tail = new_seg;

                    StreamTcpSegmentReturntoPool(list_seg);
                    list_seg = new_seg;
                    return_after = TRUE;
                }
            /* Handle the case, when list_seg is the end of segment list, but
               seg is ending after the list_seg. So we need to copy the data
               from newly received segment. After copying return the newly
               received seg to pool */
            } else {
                if (SEQ_GT(seg->seq, (list_seg->prev->seq +
                                list_seg->prev->payload_len)))
                {
                    packet_length = list_seg->payload_len + (list_seg->seq -
                            seg->seq);
                } else {
                    packet_length = list_seg->payload_len + (list_seg->seq -
                            (list_seg->prev->seq +
                             list_seg->prev->payload_len));
                }

                packet_length += (seg->seq + seg->payload_len) -
                    (list_seg->seq + list_seg->payload_len);

                TcpSegment *new_seg = StreamTcpGetSegment(tv, ra_ctx, packet_length);
                if (new_seg == NULL) {
                    SCLogDebug("segment_pool[%"PRIu16"] is empty",
                            segment_pool_idx[packet_length]);

                    StreamTcpSetEvent(p, STREAM_REASSEMBLY_NO_SEGMENT);
                    SCReturnInt(-1);
                }
                new_seg->payload_len = packet_length;

                if (SEQ_GT((list_seg->prev->seq +
                                list_seg->prev->payload_len), seg->seq))
                {
                    new_seg->seq = (list_seg->prev->seq +
                            list_seg->prev->payload_len);
                } else {
                    new_seg->seq = seg->seq;
                }
                SCLogDebug("new_seg->seq %"PRIu32" and new->payload_len "
                        "%" PRIu16"", new_seg->seq, new_seg->payload_len);
                new_seg->next = list_seg->next;
                new_seg->prev = list_seg->prev;

                /* create a new seg, copy the list_seg data over */
                StreamTcpSegmentDataCopy(new_seg, list_seg);

                /* copy the part before list_seg */
                uint16_t copy_len = list_seg->seq - new_seg->seq;
                StreamTcpSegmentDataReplace(new_seg, seg, new_seg->seq,
                        copy_len);

                /* copy the part after list_seg */
                copy_len = (seg->seq + seg->payload_len) -
                    (list_seg->seq + list_seg->payload_len);
                StreamTcpSegmentDataReplace(new_seg, seg, (list_seg->seq +
                            list_seg->payload_len), copy_len);

                if (new_seg->prev != NULL) {
                    new_seg->prev->next = new_seg;
                }

                /*update the stream last_seg in case of removal of list_seg*/
                if (stream->seg_list_tail == list_seg)
                    stream->seg_list_tail = new_seg;

                StreamTcpSegmentReturntoPool(list_seg);
                list_seg = new_seg;
                return_after = TRUE;
            }
        }

        if (check_overlap_different_data &&
                !StreamTcpSegmentDataCompare(seg, list_seg, list_seg->seq, overlap)) {
            /* interesting, overlap with different data */
            StreamTcpSetEvent(p, STREAM_REASSEMBLY_OVERLAP_DIFFERENT_DATA);
        }

        if (StreamTcpInlineMode()) {
            if (StreamTcpInlineSegmentCompare(seg, list_seg) != 0) {
                StreamTcpInlineSegmentReplacePacket(p, list_seg);
            }
        } else {
            switch (os_policy) {
                case OS_POLICY_SOLARIS:
                case OS_POLICY_HPUX11:
                    if (end_after == TRUE || end_same == TRUE) {
                        StreamTcpSegmentDataReplace(list_seg, seg, overlap_point,
                                overlap);
                    } else {
                        SCLogDebug("using old data in starts before list case, "
                                "list_seg->seq %" PRIu32 " policy %" PRIu32 " "
                                "overlap %" PRIu32 "", list_seg->seq, os_policy,
                                overlap);
                    }
                    break;
                case OS_POLICY_VISTA:
                case OS_POLICY_FIRST:
                    SCLogDebug("using old data in starts before list case, "
                            "list_seg->seq %" PRIu32 " policy %" PRIu32 " "
                            "overlap %" PRIu32 "", list_seg->seq, os_policy,
                            overlap);
                    break;
                case OS_POLICY_BSD:
                case OS_POLICY_HPUX10:
                case OS_POLICY_IRIX:
                case OS_POLICY_WINDOWS:
                case OS_POLICY_WINDOWS2K3:
                case OS_POLICY_OLD_LINUX:
                case OS_POLICY_LINUX:
                case OS_POLICY_MACOS:
                case OS_POLICY_LAST:
                default:
                    SCLogDebug("replacing old data in starts before list seg "
                            "list_seg->seq %" PRIu32 " policy %" PRIu32 " "
                            "overlap %" PRIu32 "", list_seg->seq, os_policy,
                            overlap);
                    StreamTcpSegmentDataReplace(list_seg, seg, overlap_point,
                            overlap);
                    break;
            }
        }
        /* To return from for loop as seg is finished with current list_seg
           no need to check further (improve performance) */
        if (end_before == TRUE || end_same == TRUE || return_after == TRUE) {
            SCReturnInt(1);
        }
    }

    SCReturnInt(0);
}

/**
 *  \brief  Function to handle the newly arrived segment, when newly arrived
 *          starts with the same sequence number as the original segment and
 *          ends at different position relative to original segment.
 *          The packet is handled based on its target OS.
 *
 *  \param  list_seg    Original Segment in the stream
 *  \param  seg         Newly arrived segment
 *  \param  prev_seg    Previous segment in the stream segment list
 *
 *  \retval 1 success and done
 *  \retval 0 success, but not done yet
 *  \retval -1 error, will *only* happen on memory errors
 */

static int HandleSegmentStartsAtSameListSegment(ThreadVars *tv, TcpReassemblyThreadCtx *ra_ctx,
        TcpStream *stream, TcpSegment *list_seg, TcpSegment *seg, Packet *p)
{
    uint16_t overlap = 0;
    uint16_t packet_length;
    char end_before = FALSE;
    char end_after = FALSE;
    char end_same = FALSE;
    char handle_beyond = FALSE;
    uint8_t os_policy = stream->os_policy;

    if (SEQ_LT((seg->seq + seg->payload_len), (list_seg->seq +
                                               list_seg->payload_len)))
    {
        /* seg->seg == list_seg->seq and list_seg->payload_len > seg->payload_len
         * [[ababab]bbbb] where a = seg, b = list_seg
         * overlap is the [ababab] part, which equals seg->payload_len. */
        overlap = seg->payload_len;
        end_before = TRUE;
        SCLogDebug("starts at list seq, ends before list end: seg->seq "
                   "%" PRIu32 ", list_seg->seq %" PRIu32 ", "
                   "list_seg->payload_len %" PRIu32 " overlap is %" PRIu32,
                   seg->seq, list_seg->seq, list_seg->payload_len, overlap);

    } else if (SEQ_EQ((seg->seq + seg->payload_len), (list_seg->seq +
                                                        list_seg->payload_len)))
    {
        /* seg starts at seq, ends at seq, retransmission.
         * both segments are the same, so overlap is either
         * seg->payload_len or list_seg->payload_len */

        /* check csum, ack, other differences? */
        overlap = seg->payload_len;
        end_same = TRUE;
        SCLogDebug("(retransmission) starts at list seq, ends at list end: "
                   "seg->seq %" PRIu32 ", list_seg->seq %" PRIu32 ", "
                   "list_seg->payload_len %" PRIu32 " overlap is %"PRIu32"",
                   seg->seq, list_seg->seq, list_seg->payload_len, overlap);

    } else if (SEQ_GT((seg->seq + seg->payload_len),
            (list_seg->seq + list_seg->payload_len))) {
        /* seg starts at seq, ends beyond seq. */
        /* seg->seg == list_seg->seq and seg->payload_len > list_seg->payload_len
         * [[ababab]aaaa] where a = seg, b = list_seg
         * overlap is the [ababab] part, which equals list_seg->payload_len. */
        overlap = list_seg->payload_len;
        end_after = TRUE;
        SCLogDebug("starts at list seq, ends beyond list end: seg->seq "
                   "%" PRIu32 ", list_seg->seq %" PRIu32 ", "
                   "list_seg->payload_len %" PRIu32 " overlap is %" PRIu32 "",
                   seg->seq, list_seg->seq, list_seg->payload_len, overlap);
    }
    if (overlap > 0) {
        /*Handle the case when newly arrived segment ends after original
          segment and original segment is the last segment in the list
          or the next segment in the list starts after the end of new segment*/
        if (end_after == TRUE) {
            char fill_gap = FALSE;

            if (list_seg->next != NULL) {
                /* first see if we have space left to fill up */
                if (SEQ_LT((list_seg->seq + list_seg->payload_len),
                            list_seg->next->seq))
                {
                    fill_gap = TRUE;
                }

                /* then see if we overlap (partly) with the next seg */
                if (SEQ_GT((seg->seq + seg->payload_len), list_seg->next->seq))
                {
                    handle_beyond = TRUE;
                }
            /* Handle the case, when list_seg is the end of segment list, but
               seg is ending after the list_seg. So we need to copy the data
               from newly received segment. After copying return the newly
               received seg to pool */
            } else {
                fill_gap = TRUE;
            }

            SCLogDebug("fill_gap %s, handle_beyond %s", fill_gap?"TRUE":"FALSE",
                        handle_beyond?"TRUE":"FALSE");

            if (fill_gap == TRUE) {
                /* if there is a gap after this list_seg we fill it now with a
                 * new seg */
                SCLogDebug("filling gap: list_seg->next->seq %"PRIu32"",
                            list_seg->next?list_seg->next->seq:0);
                if (handle_beyond == TRUE) {
                    packet_length = list_seg->next->seq -
                                        (list_seg->seq + list_seg->payload_len);
                } else {
                    packet_length = seg->payload_len - list_seg->payload_len;
                }

                SCLogDebug("packet_length %"PRIu16"", packet_length);

                TcpSegment *new_seg = StreamTcpGetSegment(tv, ra_ctx, packet_length);
                if (new_seg == NULL) {
                    SCLogDebug("egment_pool[%"PRIu16"] is empty", segment_pool_idx[packet_length]);

                    StreamTcpSetEvent(p, STREAM_REASSEMBLY_NO_SEGMENT);
                    return -1;
                }
                new_seg->payload_len = packet_length;
                new_seg->seq = list_seg->seq + list_seg->payload_len;
                new_seg->next = list_seg->next;
                if (new_seg->next != NULL)
                    new_seg->next->prev = new_seg;
                new_seg->prev = list_seg;
                list_seg->next = new_seg;
                SCLogDebug("new_seg %p, new_seg->next %p, new_seg->prev %p, "
                           "list_seg->next %p", new_seg, new_seg->next,
                           new_seg->prev, list_seg->next);
                StreamTcpSegmentDataReplace(new_seg, seg, new_seg->seq,
                                            new_seg->payload_len);

                /*update the stream last_seg in case of removal of list_seg*/
                if (stream->seg_list_tail == list_seg)
                    stream->seg_list_tail = new_seg;
            }
        }

        if (check_overlap_different_data &&
                !StreamTcpSegmentDataCompare(list_seg, seg, seg->seq, overlap)) {
            /* interesting, overlap with different data */
            StreamTcpSetEvent(p, STREAM_REASSEMBLY_OVERLAP_DIFFERENT_DATA);
        }

        if (StreamTcpInlineMode()) {
            if (StreamTcpInlineSegmentCompare(list_seg, seg) != 0) {
                StreamTcpInlineSegmentReplacePacket(p, list_seg);
            }
        } else {
            switch (os_policy) {
                case OS_POLICY_OLD_LINUX:
                case OS_POLICY_SOLARIS:
                case OS_POLICY_HPUX11:
                    if (end_after == TRUE || end_same == TRUE) {
                        StreamTcpSegmentDataReplace(list_seg, seg, seg->seq, overlap);
                    } else {
                        SCLogDebug("using old data in starts at list case, "
                                "list_seg->seq %" PRIu32 " policy %" PRIu32 " "
                                "overlap %" PRIu32 "", list_seg->seq, os_policy,
                                overlap);
                    }
                    break;
                case OS_POLICY_LAST:
                    StreamTcpSegmentDataReplace(list_seg, seg, seg->seq, overlap);
                    break;
                case OS_POLICY_LINUX:
                    if (end_after == TRUE) {
                        StreamTcpSegmentDataReplace(list_seg, seg, seg->seq, overlap);
                    } else {
                        SCLogDebug("using old data in starts at list case, "
                                "list_seg->seq %" PRIu32 " policy %" PRIu32 " "
                                "overlap %" PRIu32 "", list_seg->seq, os_policy,
                                overlap);
                    }
                    break;
                case OS_POLICY_BSD:
                case OS_POLICY_HPUX10:
                case OS_POLICY_IRIX:
                case OS_POLICY_WINDOWS:
                case OS_POLICY_WINDOWS2K3:
                case OS_POLICY_VISTA:
                case OS_POLICY_MACOS:
                case OS_POLICY_FIRST:
                default:
                    SCLogDebug("using old data in starts at list case, list_seg->seq"
                            " %" PRIu32 " policy %" PRIu32 " overlap %" PRIu32 "",
                            list_seg->seq, os_policy, overlap);
                    break;
            }
        }

        /* return 1 if we're done */
        if (end_before == TRUE || end_same == TRUE || handle_beyond == FALSE) {
            return 1;
        }
    }
    return 0;
}

/**
 *  \internal
 *  \brief  Function to handle the newly arrived segment, when newly arrived
 *          starts with the sequence number higher than the original segment and
 *          ends at different position relative to original segment.
 *          The packet is handled based on its target OS.
 *
 *  \param  list_seg    Original Segment in the stream
 *  \param  seg         Newly arrived segment
 *  \param  prev_seg    Previous segment in the stream segment list

 *  \retval 1 success and done
 *  \retval 0 success, but not done yet
 *  \retval -1 error, will *only* happen on memory errors
 */

static int HandleSegmentStartsAfterListSegment(ThreadVars *tv, TcpReassemblyThreadCtx *ra_ctx,
        TcpStream *stream, TcpSegment *list_seg, TcpSegment *seg, Packet *p)
{
    SCEnter();
    uint16_t overlap = 0;
    uint16_t packet_length;
    char end_before = FALSE;
    char end_after = FALSE;
    char end_same = FALSE;
    char handle_beyond = FALSE;
    uint8_t os_policy = stream->os_policy;

    if (SEQ_LT((seg->seq + seg->payload_len), (list_seg->seq +
                list_seg->payload_len)))
    {
        /* seg starts after list, ends before list end
         * [bbbb[ababab]bbbb] where a = seg, b = list_seg
         * overlap is the part [ababab] which is seg->payload_len */
        overlap = seg->payload_len;
        end_before = TRUE;

        SCLogDebug("starts beyond list seq, ends before list end: seg->seq"
            " %" PRIu32 ", list_seg->seq %" PRIu32 ", list_seg->payload_len "
            "%" PRIu32 " overlap is %" PRIu32 "", seg->seq, list_seg->seq,
            list_seg->payload_len, overlap);

    } else if (SEQ_EQ((seg->seq + seg->payload_len),
            (list_seg->seq + list_seg->payload_len))) {
        /* seg starts after seq, before end, ends at seq
         * [bbbb[ababab]] where a = seg, b = list_seg
         * overlapping part is [ababab], thus seg->payload_len */
        overlap = seg->payload_len;
        end_same = TRUE;

        SCLogDebug("starts beyond list seq, ends at list end: seg->seq"
            " %" PRIu32 ", list_seg->seq %" PRIu32 ", list_seg->payload_len "
            "%" PRIu32 " overlap is %" PRIu32 "", seg->seq, list_seg->seq,
            list_seg->payload_len, overlap);

    } else if (SEQ_LT(seg->seq, list_seg->seq + list_seg->payload_len) &&
               SEQ_GT((seg->seq + seg->payload_len), (list_seg->seq +
                       list_seg->payload_len)))
    {
        /* seg starts after seq, before end, ends beyond seq.
         *
         * [bbb[ababab]aaa] where a = seg, b = list_seg.
         * overlap is the [ababab] part, which can be get using:
         * (list_seg->seq + list_seg->payload_len) - seg->seg */
        overlap = (list_seg->seq + list_seg->payload_len) - seg->seq;
        end_after = TRUE;

        SCLogDebug("starts beyond list seq, ends after list seq end: "
            "seg->seq %" PRIu32 ", seg->payload_len %"PRIu16" (%"PRIu32") "
            "list_seg->seq %" PRIu32 ", list_seg->payload_len %" PRIu32 " "
            "(%"PRIu32") overlap is %" PRIu32 "", seg->seq, seg->payload_len,
            seg->seq + seg->payload_len, list_seg->seq, list_seg->payload_len,
            list_seg->seq + list_seg->payload_len, overlap);
    }
    if (overlap > 0) {
        /*Handle the case when newly arrived segment ends after original
          segment and original segment is the last segment in the list*/
        if (end_after == TRUE) {
            char fill_gap = FALSE;

            if (list_seg->next != NULL) {
                /* first see if we have space left to fill up */
                if (SEQ_LT((list_seg->seq + list_seg->payload_len),
                            list_seg->next->seq))
                {
                    fill_gap = TRUE;
                }

                /* then see if we overlap (partly) with the next seg */
                if (SEQ_GT((seg->seq + seg->payload_len), list_seg->next->seq))
                {
                    handle_beyond = TRUE;
                }
            } else {
                fill_gap = TRUE;
            }

            SCLogDebug("fill_gap %s, handle_beyond %s", fill_gap?"TRUE":"FALSE",
                        handle_beyond?"TRUE":"FALSE");

            if (fill_gap == TRUE) {
                /* if there is a gap after this list_seg we fill it now with a
                 * new seg */
                if (list_seg->next != NULL) {
                    SCLogDebug("filling gap: list_seg->next->seq %"PRIu32"",
                            list_seg->next?list_seg->next->seq:0);

                    packet_length = list_seg->next->seq - (list_seg->seq +
                            list_seg->payload_len);
                } else {
                    packet_length = seg->payload_len - overlap;
                }
                if (packet_length > (seg->payload_len - overlap)) {
                    packet_length = seg->payload_len - overlap;
                }
                SCLogDebug("packet_length %"PRIu16"", packet_length);

                TcpSegment *new_seg = StreamTcpGetSegment(tv, ra_ctx, packet_length);
                if (new_seg == NULL) {
                    SCLogDebug("segment_pool[%"PRIu16"] is empty", segment_pool_idx[packet_length]);

                    StreamTcpSetEvent(p, STREAM_REASSEMBLY_NO_SEGMENT);
                    SCReturnInt(-1);
                }
                new_seg->payload_len = packet_length;
                new_seg->seq = list_seg->seq + list_seg->payload_len;
                new_seg->next = list_seg->next;
                if (new_seg->next != NULL)
                    new_seg->next->prev = new_seg;
                new_seg->prev = list_seg;
                list_seg->next = new_seg;

                SCLogDebug("new_seg %p, new_seg->next %p, new_seg->prev %p, "
                           "list_seg->next %p new_seg->seq %"PRIu32"", new_seg,
                            new_seg->next, new_seg->prev, list_seg->next,
                            new_seg->seq);

                StreamTcpSegmentDataReplace(new_seg, seg, new_seg->seq,
                                            new_seg->payload_len);

                /* update the stream last_seg in case of removal of list_seg */
                if (stream->seg_list_tail == list_seg)
                    stream->seg_list_tail = new_seg;
            }
        }

        if (check_overlap_different_data &&
                !StreamTcpSegmentDataCompare(list_seg, seg, seg->seq, overlap)) {
            /* interesting, overlap with different data */
            StreamTcpSetEvent(p, STREAM_REASSEMBLY_OVERLAP_DIFFERENT_DATA);
        }

        if (StreamTcpInlineMode()) {
            if (StreamTcpInlineSegmentCompare(list_seg, seg) != 0) {
                StreamTcpInlineSegmentReplacePacket(p, list_seg);
            }
        } else {
            switch (os_policy) {
                case OS_POLICY_SOLARIS:
                case OS_POLICY_HPUX11:
                    if (end_after == TRUE) {
                        StreamTcpSegmentDataReplace(list_seg, seg, seg->seq, overlap);
                    } else {
                        SCLogDebug("using old data in starts beyond list case, "
                                "list_seg->seq %" PRIu32 " policy %" PRIu32 " "
                                "overlap %" PRIu32 "", list_seg->seq, os_policy,
                                overlap);
                    }
                    break;
                case OS_POLICY_LAST:
                    StreamTcpSegmentDataReplace(list_seg, seg, seg->seq, overlap);
                    break;
                case OS_POLICY_BSD:
                case OS_POLICY_HPUX10:
                case OS_POLICY_IRIX:
                case OS_POLICY_WINDOWS:
                case OS_POLICY_WINDOWS2K3:
                case OS_POLICY_VISTA:
                case OS_POLICY_OLD_LINUX:
                case OS_POLICY_LINUX:
                case OS_POLICY_MACOS:
                case OS_POLICY_FIRST:
                default: /* DEFAULT POLICY */
                    SCLogDebug("using old data in starts beyond list case, "
                            "list_seg->seq %" PRIu32 " policy %" PRIu32 " "
                            "overlap %" PRIu32 "", list_seg->seq, os_policy,
                            overlap);
                    break;
            }
        }
        if (end_before == TRUE || end_same == TRUE || handle_beyond == FALSE) {
            SCReturnInt(1);
        }
    }
    SCReturnInt(0);
}

/**
 *  \brief check if stream in pkt direction has depth reached
 *
 *  \param p packet with *LOCKED* flow
 *
 *  \retval 1 stream has depth reached
 *  \retval 0 stream does not have depth reached
 */
int StreamTcpReassembleDepthReached(Packet *p) {
    if (p->flow != NULL && p->flow->protoctx != NULL) {
        TcpSession *ssn = p->flow->protoctx;
        TcpStream *stream;
        if (p->flowflags & FLOW_PKT_TOSERVER) {
            stream = &ssn->client;
        } else {
            stream = &ssn->server;
        }

        return (stream->flags & STREAMTCP_STREAM_FLAG_DEPTH_REACHED) ? 1 : 0;
    }

    return 0;
}

/**
 *  \internal
 *  \brief Function to Check the reassembly depth valuer against the
 *        allowed max depth of the stream reassmbly for TCP streams.
 *
 *  \param stream stream direction
 *  \param seq sequence number where "size" starts
 *  \param size size of the segment that is added
 *
 *  \retval size Part of the size that fits in the depth, 0 if none
 */
static uint32_t StreamTcpReassembleCheckDepth(TcpStream *stream,
        uint32_t seq, uint32_t size)
{
    SCEnter();

    /* if the configured depth value is 0, it means there is no limit on
       reassembly depth. Otherwise carry on my boy ;) */
    if (stream_config.reassembly_depth == 0) {
        SCReturnUInt(size);
    }

    /* if the final flag is set, we're not accepting anymore */
    if (stream->flags & STREAMTCP_STREAM_FLAG_DEPTH_REACHED) {
        SCReturnUInt(0);
    }

    /* if the ra_base_seq has moved passed the depth window we stop
     * checking and just reject the rest of the packets including
     * retransmissions. Saves us the hassle of dealing with sequence
     * wraps as well */
    if (SEQ_GEQ((StreamTcpReassembleGetRaBaseSeq(stream)+1),(stream->isn + stream_config.reassembly_depth))) {
        stream->flags |= STREAMTCP_STREAM_FLAG_DEPTH_REACHED;
        SCReturnUInt(0);
    }

    SCLogDebug("full Depth not yet reached: %"PRIu32" <= %"PRIu32,
            (StreamTcpReassembleGetRaBaseSeq(stream)+1),
            (stream->isn + stream_config.reassembly_depth));

    if (SEQ_GEQ(seq, stream->isn) && SEQ_LT(seq, (stream->isn + stream_config.reassembly_depth))) {
        /* packet (partly?) fits the depth window */

        if (SEQ_LEQ((seq + size),(stream->isn + stream_config.reassembly_depth))) {
            /* complete fit */
            SCReturnUInt(size);
        } else {
            /* partial fit, return only what fits */
            uint32_t part = (stream->isn + stream_config.reassembly_depth) - seq;
#if DEBUG
            BUG_ON(part > size);
#else
            if (part > size)
                part = size;
#endif
            SCReturnUInt(part);
        }
    }

    SCReturnUInt(0);
}

/**
 *  \brief Insert a packets TCP data into the stream reassembly engine.
 *
 *  \retval 0 good segment, as far as we checked.
 *  \retval -1 badness, reason to drop in inline mode
 *
 *  If the retval is 0 the segment is inserted correctly, or overlap is handled,
 *  or it wasn't added because of reassembly depth.
 *
 */
int StreamTcpReassembleHandleSegmentHandleData(ThreadVars *tv, TcpReassemblyThreadCtx *ra_ctx,
                                TcpSession *ssn, TcpStream *stream, Packet *p)
{
    SCEnter();

    /* If we have reached the defined depth for either of the stream, then stop
       reassembling the TCP session */
    uint32_t size = StreamTcpReassembleCheckDepth(stream, TCP_GET_SEQ(p), p->payload_len);
    SCLogDebug("ssn %p: check depth returned %"PRIu32, ssn, size);

    if (stream->flags & STREAMTCP_STREAM_FLAG_DEPTH_REACHED) {
        /* increment stream depth counter */
        SCPerfCounterIncr(ra_ctx->counter_tcp_stream_depth, tv->sc_perf_pca);

        stream->flags |= STREAMTCP_STREAM_FLAG_NOREASSEMBLY;
        SCLogDebug("ssn %p: reassembly depth reached, "
                "STREAMTCP_STREAM_FLAG_NOREASSEMBLY set", ssn);
    }
    if (size == 0) {
        SCLogDebug("ssn %p: depth reached, not reassembling", ssn);
        SCReturnInt(0);
    }

#if DEBUG
    BUG_ON(size > p->payload_len);
#else
    if (size > p->payload_len)
        size = p->payload_len;
#endif

    TcpSegment *seg = StreamTcpGetSegment(tv, ra_ctx, size);
    if (seg == NULL) {
        SCLogDebug("segment_pool[%"PRIu16"] is empty", segment_pool_idx[size]);

        StreamTcpSetEvent(p, STREAM_REASSEMBLY_NO_SEGMENT);
        SCReturnInt(-1);
    }

    memcpy(seg->payload, p->payload, size);
    seg->payload_len = size;
    seg->seq = TCP_GET_SEQ(p);

    if (StreamTcpReassembleInsertSegment(tv, ra_ctx, stream, seg, p) != 0) {
        SCLogDebug("StreamTcpReassembleInsertSegment failed");
        SCReturnInt(-1);
    }

    SCReturnInt(0);
}

#define STREAM_SET_FLAGS(ssn, stream, p, flag) { \
    flag = 0; \
    if (!(ssn->flags & STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED)) {\
        flag |= STREAM_START; \
    } \
    if (stream->flags & STREAMTCP_STREAM_FLAG_CLOSE_INITIATED) {    \
        flag |= STREAM_EOF; \
    } \
    if ((p)->flowflags & FLOW_PKT_TOSERVER) { \
        flag |= STREAM_TOCLIENT; \
    } else { \
        flag |= STREAM_TOSERVER; \
    } \
    if (stream->flags & STREAMTCP_STREAM_FLAG_DEPTH_REACHED) {    \
        flag |= STREAM_DEPTH; \
    } \
}

#define STREAM_SET_INLINE_FLAGS(ssn, stream, p, flag) { \
    flag = 0; \
    if (!(ssn->flags & STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED)) {\
        flag |= STREAM_START; \
    } \
    if (stream->flags & STREAMTCP_STREAM_FLAG_CLOSE_INITIATED) {    \
        flag |= STREAM_EOF; \
    } \
    if ((p)->flowflags & FLOW_PKT_TOSERVER) { \
        flag |= STREAM_TOSERVER; \
    } else { \
        flag |= STREAM_TOCLIENT; \
    } \
    if (stream->flags & STREAMTCP_STREAM_FLAG_DEPTH_REACHED) {    \
        flag |= STREAM_DEPTH; \
    } \
}

static void StreamTcpSetupMsg(TcpSession *ssn, TcpStream *stream, Packet *p,
                              StreamMsg *smsg)
{
    SCEnter();

    smsg->flags = 0;

    if (stream->ra_raw_base_seq == stream->isn) {
        SCLogDebug("setting STREAM_START");
        smsg->flags = STREAM_START;
    }
    if (stream->flags & STREAMTCP_STREAM_FLAG_CLOSE_INITIATED) {
        SCLogDebug("setting STREAM_EOF");
        smsg->flags |= STREAM_EOF;
    }

    if ((!StreamTcpInlineMode() && (p->flowflags & FLOW_PKT_TOSERVER)) ||
        ( StreamTcpInlineMode() && (p->flowflags & FLOW_PKT_TOCLIENT)))
    {
        smsg->flags |= STREAM_TOCLIENT;
        SCLogDebug("stream mesage is to_client");
    } else {
        smsg->flags |= STREAM_TOSERVER;
        SCLogDebug("stream mesage is to_server");
    }

    smsg->data.data_len = 0;
    FlowReference(&smsg->flow, p->flow);
    BUG_ON(smsg->flow == NULL);

    SCLogDebug("smsg %p", smsg);
    SCReturn;
}

/**
 *  \brief Check the minimum size limits for reassembly.
 *
 *  \retval 0 don't reassemble yet
 *  \retval 1 do reassemble
 */
static int StreamTcpReassembleRawCheckLimit(TcpSession *ssn, TcpStream *stream,
                                         Packet *p)
{
    SCEnter();

    if (ssn->flags & STREAMTCP_FLAG_TRIGGER_RAW_REASSEMBLY) {
        SCLogDebug("reassembling now as STREAMTCP_FLAG_TRIGGER_RAW_REASSEMBLY is set");
        ssn->flags &= ~STREAMTCP_FLAG_TRIGGER_RAW_REASSEMBLY;
        SCReturnInt(1);
    }

    /* some states mean we reassemble no matter how much data we have */
    if (ssn->state >= TCP_TIME_WAIT)
        SCReturnInt(1);

    if (p->flags & PKT_PSEUDO_STREAM_END)
        SCReturnInt(1);

    /* check if we have enough data to send to L7 */
    if (p->flowflags & FLOW_PKT_TOCLIENT) {
        SCLogDebug("StreamMsgQueueGetMinChunkLen(STREAM_TOSERVER) %"PRIu32,
                StreamMsgQueueGetMinChunkLen(FLOW_PKT_TOSERVER));

        if (StreamMsgQueueGetMinChunkLen(FLOW_PKT_TOSERVER) >
                (stream->last_ack - stream->ra_raw_base_seq)) {
            SCLogDebug("toserver min chunk len not yet reached: "
                    "last_ack %"PRIu32", ra_raw_base_seq %"PRIu32", %"PRIu32" < "
                    "%"PRIu32"", stream->last_ack, stream->ra_raw_base_seq,
                    (stream->last_ack - stream->ra_raw_base_seq),
                    StreamMsgQueueGetMinChunkLen(FLOW_PKT_TOSERVER));
            SCReturnInt(0);
        }
    } else {
        SCLogDebug("StreamMsgQueueGetMinChunkLen(STREAM_TOCLIENT) %"PRIu32,
                StreamMsgQueueGetMinChunkLen(FLOW_PKT_TOCLIENT));

        if (StreamMsgQueueGetMinChunkLen(FLOW_PKT_TOCLIENT) >
                (stream->last_ack - stream->ra_raw_base_seq)) {
            SCLogDebug("toclient min chunk len not yet reached: "
                    "last_ack %"PRIu32", ra_base_seq %"PRIu32",  %"PRIu32" < "
                    "%"PRIu32"", stream->last_ack, stream->ra_raw_base_seq,
                    (stream->last_ack - stream->ra_raw_base_seq),
                    StreamMsgQueueGetMinChunkLen(FLOW_PKT_TOCLIENT));
            SCReturnInt(0);
        }
    }

    SCReturnInt(1);
}

static void StreamTcpRemoveSegmentFromStream(TcpStream *stream, TcpSegment *seg) {
    if (seg->prev == NULL) {
        stream->seg_list = seg->next;
        if (stream->seg_list != NULL)
            stream->seg_list->prev = NULL;
    } else {
        seg->prev->next = seg->next;
        if (seg->next != NULL)
            seg->next->prev = seg->prev;
    }

    if (stream->seg_list_tail == seg)
        stream->seg_list_tail = seg->prev;
}

/**
 *  \brief see if app layer is done with a segment
 *
 *  \retval 1 app layer is done with this segment
 *  \retval 0 not done yet
 */
#define StreamTcpAppLayerSegmentProcessed(stream, segment) \
    (( ( (stream)->flags & STREAMTCP_STREAM_FLAG_GAP ) || \
       ( (segment)->flags & SEGMENTTCP_FLAG_APPLAYER_PROCESSED ) ? 1 :0 ))

/**
 *  \brief Update the stream reassembly upon receiving a data segment
 *
 *  Reassembly is in the same direction of the packet.
 *
 *  \todo this function is too long, we need to break it up. It needs it BAD
 */
static int StreamTcpReassembleInlineAppLayer (ThreadVars *tv,
        TcpReassemblyThreadCtx *ra_ctx, TcpSession *ssn, TcpStream *stream,
        Packet *p)
{
    SCEnter();

    uint8_t flags = 0;

    SCLogDebug("pcap_cnt %"PRIu64", len %u", p->pcap_cnt, p->payload_len);

    SCLogDebug("stream->seg_list %p", stream->seg_list);
#ifdef DEBUG
    PrintList(stream->seg_list);
    //PrintRawDataFp(stdout, p->payload, p->payload_len);
#endif

    if (stream->seg_list == NULL) {
        /* send an empty EOF msg if we have no segments but TCP state
         * is beyond ESTABLISHED */
        if (ssn->state > TCP_ESTABLISHED) {
            SCLogDebug("sending empty eof message");
            /* send EOF to app layer */
            STREAM_SET_INLINE_FLAGS(ssn, stream, p, flags);
            AppLayerHandleTCPData(&ra_ctx->dp_ctx, p->flow, ssn,
                    NULL, 0, flags);
            PACKET_PROFILING_APP_STORE(&ra_ctx->dp_ctx, p);

        } else {
            SCLogDebug("no segments in the list to reassemble");
        }

        SCReturnInt(0);
    }

    if (stream->flags & STREAMTCP_STREAM_FLAG_GAP) {
        SCReturnInt(0);
    }

    /* stream->ra_app_base_seq remains at stream->isn until protocol is
     * detected. */
    uint32_t ra_base_seq = stream->ra_app_base_seq;
    uint8_t data[4096];
    uint32_t data_len = 0;
    uint16_t payload_offset = 0;
    uint16_t payload_len = 0;
    uint32_t next_seq = ra_base_seq + 1;
    uint32_t data_sent = 0;

    SCLogDebug("ra_base_seq %u", ra_base_seq);

    /* loop through the segments and fill one or more msgs */
    TcpSegment *seg = stream->seg_list;
    SCLogDebug("pre-loop seg %p", seg);
    for (; seg != NULL;) {
        SCLogDebug("seg %p", seg);

        if (p->flow->flags & FLOW_NO_APPLAYER_INSPECTION) {
            if (seg->flags & SEGMENTTCP_FLAG_RAW_PROCESSED) {
                SCLogDebug("removing seg %p seq %"PRIu32
                           " len %"PRIu16"", seg, seg->seq, seg->payload_len);

                TcpSegment *next_seg = seg->next;
                StreamTcpRemoveSegmentFromStream(stream, seg);
                StreamTcpSegmentReturntoPool(seg);
                seg = next_seg;
                continue;
            } else {
                break;
            }

            /* if app layer protocol has been detected, then remove all the segments
             * which has been previously processed and reassembled */
        } else if ((ssn->flags & STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED) &&
                   (seg->flags & SEGMENTTCP_FLAG_RAW_PROCESSED) &&
                   StreamTcpAppLayerSegmentProcessed(stream, seg)) {
            SCLogDebug("segment(%p) of length %"PRIu16" has been processed,"
                    " so return it to pool", seg, seg->payload_len);
            TcpSegment *next_seg = seg->next;
            StreamTcpRemoveSegmentFromStream(stream, seg);
            StreamTcpSegmentReturntoPool(seg);
            seg = next_seg;
            continue;
        }

        /* If packets are fully before ra_base_seq, skip them. We do this
         * because we've reassembled up to the ra_base_seq point already,
         * so we won't do anything with segments before it anyway. */
        SCLogDebug("checking for pre ra_base_seq %"PRIu32" seg %p seq %"PRIu32""
                   " len %"PRIu16", combined %"PRIu32" and stream->last_ack "
                   "%"PRIu32"", ra_base_seq, seg, seg->seq,
                   seg->payload_len, seg->seq+seg->payload_len, stream->last_ack);

        /* we've run into a sequence gap */
        if (SEQ_GT(seg->seq, next_seq)) {

            /* first, pass on data before the gap */
            if (data_len > 0) {
                SCLogDebug("pre GAP data");

                STREAM_SET_INLINE_FLAGS(ssn, stream, p, flags);

                /* process what we have so far */
                AppLayerHandleTCPData(&ra_ctx->dp_ctx, p->flow, ssn,
                        data, data_len, flags);
                PACKET_PROFILING_APP_STORE(&ra_ctx->dp_ctx, p);

                data_sent += data_len;
                data_len = 0;
            }

            /* don't conclude it's a gap straight away. If ra_base_seq is lower
             * than last_ack - the window, we consider it a gap. */
            if (SEQ_GT((stream->last_ack - stream->window), ra_base_seq))
            {
                /* see what the length of the gap is, gap length is seg->seq -
                 * (ra_base_seq +1) */
#ifdef DEBUG
                uint32_t gap_len = seg->seq - next_seq;
                SCLogDebug("expected next_seq %" PRIu32 ", got %" PRIu32 " , "
                        "stream->last_ack %" PRIu32 ". Seq gap %" PRIu32"",
                        next_seq, seg->seq, stream->last_ack, gap_len);
#endif

                /* We have missed the packet and end host has ack'd it, so
                 * IDS should advance it's ra_base_seq and should not consider this
                 * packet any longer, even if it is retransmitted, as end host will
                 * drop it anyway */
                ra_base_seq = seg->seq - 1;

                /* send gap signal */
                STREAM_SET_INLINE_FLAGS(ssn, stream, p, flags);
                AppLayerHandleTCPData(&ra_ctx->dp_ctx, p->flow, ssn,
                        NULL, 0, flags|STREAM_GAP);
                PACKET_PROFILING_APP_STORE(&ra_ctx->dp_ctx, p);
                data_len = 0;

                /* set a GAP flag and make sure not bothering this stream anymore */
                SCLogDebug("set STREAMTCP_STREAM_FLAG_GAP flag");
                stream->flags |= STREAMTCP_STREAM_FLAG_GAP;

                StreamTcpSetEvent(p, STREAM_REASSEMBLY_SEQ_GAP);
                SCPerfCounterIncr(ra_ctx->counter_tcp_reass_gap, tv->sc_perf_pca);
#ifdef DEBUG
                dbg_app_layer_gap++;
#endif
                break;
            } else {
                SCLogDebug("possible GAP, but waiting to see if out of order "
                        "packets might solve that");
#ifdef DEBUG
                dbg_app_layer_gap_candidate++;
#endif
                break;
            }
        }

        /* if the segment ends beyond ra_base_seq we need to consider it */
        if (SEQ_GT((seg->seq + seg->payload_len), (ra_base_seq + 1))) {
            SCLogDebug("seg->seq %" PRIu32 ", seg->payload_len %" PRIu32 ", "
                    "ra_base_seq %" PRIu32 "", seg->seq,
                    seg->payload_len, ra_base_seq);

            /* handle segments partly before ra_base_seq */
            if (SEQ_GT(ra_base_seq, seg->seq)) {
                payload_offset = ra_base_seq - seg->seq - 1;
                payload_len = seg->payload_len - payload_offset;

                if (SCLogDebugEnabled()) {
                    BUG_ON(payload_offset > seg->payload_len);
                    BUG_ON((payload_len + payload_offset) > seg->payload_len);
                }
            } else {
                payload_offset = 0;
                payload_len = seg->payload_len;
            }
            SCLogDebug("payload_offset is %"PRIu16", payload_len is %"PRIu16""
                       " and stream->next_win is %"PRIu32"", payload_offset,
                        payload_len, stream->next_win);

            if (payload_len == 0) {
                SCLogDebug("no payload_len, so bail out");
                break;
            }

            /* copy the data into the smsg */
            uint16_t copy_size = sizeof(data) - data_len;
            if (copy_size > payload_len) {
                copy_size = payload_len;
            }
            if (SCLogDebugEnabled()) {
                BUG_ON(copy_size > sizeof(data));
            }
            SCLogDebug("copy_size is %"PRIu16"", copy_size);
            memcpy(data + data_len, seg->payload + payload_offset, copy_size);
            data_len += copy_size;
            ra_base_seq += copy_size;
            SCLogDebug("ra_base_seq %"PRIu32", data_len %"PRIu32, ra_base_seq, data_len);

            /* queue the smsg if it's full */
            if (data_len == sizeof(data)) {
                /* process what we have so far */
                STREAM_SET_INLINE_FLAGS(ssn, stream, p, flags);
                BUG_ON(data_len > sizeof(data));
                AppLayerHandleTCPData(&ra_ctx->dp_ctx, p->flow, ssn,
                        data, data_len, flags);
                PACKET_PROFILING_APP_STORE(&ra_ctx->dp_ctx, p);
                data_sent += data_len;
                data_len = 0;
            }

            /* if the payload len is bigger than what we copied, we handle the
             * rest of the payload next... */
            if (copy_size < payload_len) {
                SCLogDebug("copy_size %" PRIu32 " < %" PRIu32 "", copy_size,
                            payload_len);

                payload_offset += copy_size;
                payload_len -= copy_size;
                SCLogDebug("payload_offset is %"PRIu16", seg->payload_len is "
                           "%"PRIu16" and stream->last_ack is %"PRIu32"",
                            payload_offset, seg->payload_len, stream->last_ack);
                if (SCLogDebugEnabled()) {
                    BUG_ON(payload_offset > seg->payload_len);
                }

                /* we need a while loop here as the packets theoretically can be
                 * 64k */
                char segment_done = FALSE;
                while (segment_done == FALSE) {
                    SCLogDebug("new msg at offset %" PRIu32 ", payload_len "
                               "%" PRIu32 "", payload_offset, payload_len);
                    data_len = 0;

                    copy_size = sizeof(data) - data_len;
                    if (copy_size > (seg->payload_len - payload_offset)) {
                        copy_size = (seg->payload_len - payload_offset);
                    }
                    if (SCLogDebugEnabled()) {
                        BUG_ON(copy_size > sizeof(data));
                    }

                    SCLogDebug("copy payload_offset %" PRIu32 ", data_len "
                                "%" PRIu32 ", copy_size %" PRIu32 "",
                                payload_offset, data_len, copy_size);
                    memcpy(data + data_len, seg->payload +
                            payload_offset, copy_size);
                    data_len += copy_size;
                    ra_base_seq += copy_size;
                    SCLogDebug("ra_base_seq %"PRIu32, ra_base_seq);
                    SCLogDebug("copied payload_offset %" PRIu32 ", "
                               "data_len %" PRIu32 ", copy_size %" PRIu32 "",
                               payload_offset, data_len, copy_size);

                    if (data_len == sizeof(data)) {
                        /* process what we have so far */
                        STREAM_SET_INLINE_FLAGS(ssn, stream, p, flags);
                        BUG_ON(data_len > sizeof(data));
                        AppLayerHandleTCPData(&ra_ctx->dp_ctx, p->flow, ssn,
                                data, data_len, flags);
                        PACKET_PROFILING_APP_STORE(&ra_ctx->dp_ctx, p);
                        data_sent += data_len;
                        data_len = 0;
                    }

                    /* see if we have segment payload left to process */
                    if ((copy_size + payload_offset) < seg->payload_len) {
                        payload_offset += copy_size;
                        payload_len -= copy_size;

                        if (SCLogDebugEnabled()) {
                            BUG_ON(payload_offset > seg->payload_len);
                        }
                    } else {
                        payload_offset = 0;
                        segment_done = TRUE;
                    }
                }
            }
        }

        /* done with this segment, return it to the pool */
        TcpSegment *next_seg = seg->next;
        next_seq = seg->seq + seg->payload_len;
        seg->flags |= SEGMENTTCP_FLAG_APPLAYER_PROCESSED;
        seg = next_seg;
    }

    /* put the partly filled smsg in the queue to the l7 handler */
    if (data_len > 0) {
        SCLogDebug("data_len > 0, %u", data_len);
        /* process what we have so far */
        STREAM_SET_INLINE_FLAGS(ssn, stream, p, flags);
        BUG_ON(data_len > sizeof(data));
        AppLayerHandleTCPData(&ra_ctx->dp_ctx, p->flow, ssn,
                data, data_len, flags);
        PACKET_PROFILING_APP_STORE(&ra_ctx->dp_ctx, p);
        data_sent += data_len;
    }

    if (data_sent == 0 && ssn->state > TCP_ESTABLISHED) {
        SCLogDebug("sending empty eof message");
        /* send EOF to app layer */
        STREAM_SET_INLINE_FLAGS(ssn, stream, p, flags);
        AppLayerHandleTCPData(&ra_ctx->dp_ctx, p->flow, ssn,
                NULL, 0, flags);
        PACKET_PROFILING_APP_STORE(&ra_ctx->dp_ctx, p);
    }

    /* store ra_base_seq in the stream */
    if ((ssn->flags & STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED)) {
        stream->ra_app_base_seq = ra_base_seq;
    }

    SCLogDebug("stream->ra_app_base_seq %u", stream->ra_app_base_seq);
    SCReturnInt(0);
}

/**
 *  \brief Update the stream reassembly upon receiving a data segment
 *
 *  | left edge        | right edge based on sliding window size
 *  [aaa]
 *  [aaabbb]
 *  ...
 *  [aaabbbcccdddeeefff]
 *  [bbbcccdddeeefffggg] <- cut off aaa to adhere to the window size
 *
 *  GAP situation: each chunk that is uninterrupted has it's own smsg
 *  [aaabbb].[dddeeefff]
 *  [aaa].[ccc].[eeefff]
 *
 *  A flag will be set to indicate where the *NEW* payload starts. This
 *  is to aid the detection code for alert only sigs.
 *
 *  \todo this function is too long, we need to break it up. It needs it BAD
 */
static int StreamTcpReassembleInlineRaw (TcpReassemblyThreadCtx *ra_ctx,
        TcpSession *ssn, TcpStream *stream, Packet *p)
{
    SCEnter();
    SCLogDebug("start p %p, seq %"PRIu32, p, TCP_GET_SEQ(p));

    if (stream->seg_list == NULL) {
        SCReturnInt(0);
    }

    uint32_t ra_base_seq = stream->ra_raw_base_seq;
    StreamMsg *smsg = NULL;
    uint16_t smsg_offset = 0;
    uint16_t payload_offset = 0;
    uint16_t payload_len = 0;
    TcpSegment *seg = stream->seg_list;
    uint32_t next_seq = ra_base_seq + 1;
    int gap = 0;

    uint16_t chunk_size = PKT_IS_TOSERVER(p) ?
        stream_config.reassembly_toserver_chunk_size :
        stream_config.reassembly_toclient_chunk_size;

    /* determine the left edge and right edge */
    uint32_t right_edge = TCP_GET_SEQ(p) + p->payload_len;
    uint32_t left_edge = right_edge - chunk_size;

    /* shift the window to the right if the left edge doesn't cover segments */
    if (SEQ_GT(seg->seq,left_edge)) {
        right_edge += (seg->seq - left_edge);
        left_edge = seg->seq;
    }

    SCLogDebug("left_edge %"PRIu32", right_edge %"PRIu32, left_edge, right_edge);

    /* loop through the segments and fill one or more msgs */
    for (; seg != NULL && SEQ_LT(seg->seq, right_edge); ) {
        SCLogDebug("seg %p", seg);

        /* If packets are fully before ra_base_seq, skip them. We do this
         * because we've reassembled up to the ra_base_seq point already,
         * so we won't do anything with segments before it anyway. */
        SCLogDebug("checking for pre ra_base_seq %"PRIu32" seg %p seq %"PRIu32""
                   " len %"PRIu16", combined %"PRIu32" and right_edge "
                   "%"PRIu32"", ra_base_seq, seg, seg->seq,
                    seg->payload_len, seg->seq+seg->payload_len, right_edge);

        /* Remove the segments which are completely before the ra_base_seq */
        if (SEQ_LT((seg->seq + seg->payload_len), (ra_base_seq - chunk_size)))
        {
            SCLogDebug("removing pre ra_base_seq %"PRIu32" seg %p seq %"PRIu32""
                        " len %"PRIu16"", ra_base_seq, seg, seg->seq,
                        seg->payload_len);

            /* only remove if app layer reassembly is ready too */
            if (StreamTcpAppLayerSegmentProcessed(stream, seg)) {
                TcpSegment *next_seg = seg->next;
                StreamTcpRemoveSegmentFromStream(stream, seg);
                StreamTcpSegmentReturntoPool(seg);
                seg = next_seg;
            /* otherwise, just flag it for removal */
            } else {
                seg->flags |= SEGMENTTCP_FLAG_RAW_PROCESSED;
                seg = seg->next;
            }
            continue;
        }

        /* if app layer protocol has been detected, then remove all the segments
         * which has been previously processed and reassembled
         *
         * If the stream is in GAP state the app layer flag won't be set */
        if ((ssn->flags & STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED) &&
                (seg->flags & SEGMENTTCP_FLAG_RAW_PROCESSED) &&
                StreamTcpAppLayerSegmentProcessed(stream, seg))
        {
            SCLogDebug("segment(%p) of length %"PRIu16" has been processed,"
                    " so return it to pool", seg, seg->payload_len);
            TcpSegment *next_seg = seg->next;
            StreamTcpRemoveSegmentFromStream(stream, seg);
            StreamTcpSegmentReturntoPool(seg);
            seg = next_seg;
            continue;
        }

        /* we've run into a sequence gap, wrap up any existing smsg and
         * queue it so the next chunk (if any) is in a new smsg */
        if (SEQ_GT(seg->seq, next_seq)) {
            /* pass on pre existing smsg (if any) */
            if (smsg != NULL && smsg->data.data_len > 0) {
                StreamMsgPutInQueue(ra_ctx->stream_q, smsg);
                stream->ra_raw_base_seq = ra_base_seq;
                smsg = NULL;
            }

            gap = 1;
        }

        /* if the segment ends beyond left_edge we need to consider it */
        if (SEQ_GT((seg->seq + seg->payload_len), left_edge)) {
            SCLogDebug("seg->seq %" PRIu32 ", seg->payload_len %" PRIu32 ", "
                       "left_edge %" PRIu32 "", seg->seq,
                       seg->payload_len, left_edge);

            /* handle segments partly before ra_base_seq */
            if (SEQ_GT(left_edge, seg->seq)) {
                payload_offset = left_edge - seg->seq;

                if (SEQ_LT(right_edge, (seg->seq + seg->payload_len))) {
                    payload_len = (right_edge - seg->seq) - payload_offset;
                } else {
                    payload_len = seg->payload_len - payload_offset;
                }

                if (SCLogDebugEnabled()) {
                    BUG_ON(payload_offset > seg->payload_len);
                    BUG_ON((payload_len + payload_offset) > seg->payload_len);
                }
            } else {
                payload_offset = 0;

                if (SEQ_LT(right_edge, (seg->seq + seg->payload_len))) {
                    payload_len = right_edge - seg->seq;
                } else {
                    payload_len = seg->payload_len;
                }
            }
            SCLogDebug("payload_offset is %"PRIu16", payload_len is %"PRIu16""
                       " and stream->last_ack is %"PRIu32"", payload_offset,
                        payload_len, stream->last_ack);

            if (payload_len == 0) {
                SCLogDebug("no payload_len, so bail out");
                break;
            }

            if (smsg == NULL) {
                smsg = StreamMsgGetFromPool();
                if (smsg == NULL) {
                    SCLogDebug("stream_msg_pool is empty");
                    return -1;
                }

                smsg_offset = 0;

                StreamTcpSetupMsg(ssn, stream, p, smsg);
            }
            smsg->data.seq = ra_base_seq+1;

            /* copy the data into the smsg */
            uint16_t copy_size = sizeof (smsg->data.data) - smsg_offset;
            if (copy_size > payload_len) {
                copy_size = payload_len;
            }
            if (SCLogDebugEnabled()) {
                BUG_ON(copy_size > sizeof(smsg->data.data));
            }
            SCLogDebug("copy_size is %"PRIu16"", copy_size);
            memcpy(smsg->data.data + smsg_offset, seg->payload + payload_offset,
                    copy_size);
            smsg_offset += copy_size;

            SCLogDebug("seg total %u, seq %u off %u copy %u, ra_base_seq %u",
                    (seg->seq + payload_offset + copy_size), seg->seq,
                    payload_offset, copy_size, ra_base_seq);
            if (gap == 0 && SEQ_GT((seg->seq + payload_offset + copy_size),ra_base_seq+1)) {
                ra_base_seq += copy_size;
            }
            SCLogDebug("ra_base_seq %"PRIu32, ra_base_seq);

            smsg->data.data_len += copy_size;

            /* queue the smsg if it's full */
            if (smsg->data.data_len == sizeof (smsg->data.data)) {
                StreamMsgPutInQueue(ra_ctx->stream_q, smsg);
                stream->ra_raw_base_seq = ra_base_seq;
                smsg = NULL;
            }

            /* if the payload len is bigger than what we copied, we handle the
             * rest of the payload next... */
            if (copy_size < payload_len) {
                SCLogDebug("copy_size %" PRIu32 " < %" PRIu32 "", copy_size,
                            payload_len);
                payload_offset += copy_size;
                payload_len -= copy_size;
                SCLogDebug("payload_offset is %"PRIu16", seg->payload_len is "
                           "%"PRIu16" and stream->last_ack is %"PRIu32"",
                            payload_offset, seg->payload_len, stream->last_ack);
                if (SCLogDebugEnabled()) {
                    BUG_ON(payload_offset > seg->payload_len);
                }

                /* we need a while loop here as the packets theoretically can be
                 * 64k */
                char segment_done = FALSE;
                while (segment_done == FALSE) {
                    SCLogDebug("new msg at offset %" PRIu32 ", payload_len "
                               "%" PRIu32 "", payload_offset, payload_len);

                    /* get a new message
                       XXX we need a setup function */
                    smsg = StreamMsgGetFromPool();
                    if (smsg == NULL) {
                        SCLogDebug("stream_msg_pool is empty");
                        SCReturnInt(-1);
                    }
                    smsg_offset = 0;

                    StreamTcpSetupMsg(ssn, stream,p,smsg);
                    smsg->data.seq = ra_base_seq+1;

                    copy_size = sizeof(smsg->data.data) - smsg_offset;
                    if (copy_size > (seg->payload_len - payload_offset)) {
                        copy_size = (seg->payload_len - payload_offset);
                    }
                    if (SCLogDebugEnabled()) {
                        BUG_ON(copy_size > sizeof(smsg->data.data));
                    }

                    SCLogDebug("copy payload_offset %" PRIu32 ", smsg_offset "
                                "%" PRIu32 ", copy_size %" PRIu32 "",
                                payload_offset, smsg_offset, copy_size);
                    memcpy(smsg->data.data + smsg_offset, seg->payload +
                            payload_offset, copy_size);
                    smsg_offset += copy_size;
                    if (gap == 0 && SEQ_GT((seg->seq + payload_offset + copy_size),ra_base_seq+1)) {
                        ra_base_seq += copy_size;
                    }
                    SCLogDebug("ra_base_seq %"PRIu32, ra_base_seq);
                    smsg->data.data_len += copy_size;
                    SCLogDebug("copied payload_offset %" PRIu32 ", "
                               "smsg_offset %" PRIu32 ", copy_size %" PRIu32 "",
                               payload_offset, smsg_offset, copy_size);
                    if (smsg->data.data_len == sizeof (smsg->data.data)) {
                        StreamMsgPutInQueue(ra_ctx->stream_q, smsg);
                        stream->ra_raw_base_seq = ra_base_seq;
                        smsg = NULL;
                    }

                    /* see if we have segment payload left to process */
                    if ((copy_size + payload_offset) < seg->payload_len) {
                        payload_offset += copy_size;
                        payload_len -= copy_size;

                        if (SCLogDebugEnabled()) {
                            BUG_ON(payload_offset > seg->payload_len);
                        }
                    } else {
                        payload_offset = 0;
                        segment_done = TRUE;
                    }
                }
            }
        }

        /* done with this segment, return it to the pool */
        TcpSegment *next_seg = seg->next;
        next_seq = seg->seq + seg->payload_len;

        if (SEQ_LT((seg->seq + seg->payload_len), (ra_base_seq - chunk_size))) {
            if (seg->flags & SEGMENTTCP_FLAG_APPLAYER_PROCESSED) {
                StreamTcpRemoveSegmentFromStream(stream, seg);
                SCLogDebug("removing seg %p, seg->next %p", seg, seg->next);
                StreamTcpSegmentReturntoPool(seg);
            } else {
                seg->flags |= SEGMENTTCP_FLAG_RAW_PROCESSED;
            }
        }
        seg = next_seg;
    }

    /* put the partly filled smsg in the queue */
    if (smsg != NULL) {
        StreamMsgPutInQueue(ra_ctx->stream_q, smsg);
        smsg = NULL;
        stream->ra_raw_base_seq = ra_base_seq;
    }

    /* see if we can clean up some segments */
    left_edge = (ra_base_seq + 1) - chunk_size;
    SCLogDebug("left_edge %"PRIu32", ra_base_seq %"PRIu32, left_edge, ra_base_seq);

    /* loop through the segments to remove unneeded segments */
    for (seg = stream->seg_list; seg != NULL && SEQ_LEQ((seg->seq + p->payload_len), left_edge); ) {
        SCLogDebug("seg %p seq %"PRIu32", len %"PRIu16", sum %"PRIu32, seg, seg->seq, seg->payload_len, seg->seq+seg->payload_len);

        /* only remove if app layer reassembly is ready too */
        if (StreamTcpAppLayerSegmentProcessed(stream, seg)) {
            TcpSegment *next_seg = seg->next;
            StreamTcpRemoveSegmentFromStream(stream, seg);
            StreamTcpSegmentReturntoPool(seg);
            seg = next_seg;
        } else {
            break;
        }
    }
    SCLogDebug("stream->ra_raw_base_seq %u", stream->ra_raw_base_seq);
    SCReturnInt(0);
}

/** \internal
 *  \brief check if we can remove a segment from our segment list
 *
 *  If a segment is entirely before the oldest smsg, we can discard it. Otherwise
 *  we keep it around to be able to log it.
 *
 *  \retval 1 yes
 *  \retval 0 no
 */
static inline int StreamTcpReturnSegmentCheck(TcpSession *ssn, TcpStream *stream, TcpSegment *seg) {
    if (stream == &ssn->client && ssn->toserver_smsg_head != NULL) {
        /* not (seg is entirely before first smsg, skip) */
        if (!(SEQ_LEQ(seg->seq + seg->payload_len, ssn->toserver_smsg_head->data.seq))) {
            SCReturnInt(0);
        }
    } else if (stream == &ssn->server && ssn->toclient_smsg_head != NULL) {
        /* not (seg is entirely before first smsg, skip) */
        if (!(SEQ_LEQ(seg->seq + seg->payload_len, ssn->toclient_smsg_head->data.seq))) {
            SCReturnInt(0);
        }
    }
    SCReturnInt(1);
}

/** \brief Remove idle TcpSegments from TcpSession
 *
 *  \param f flow
 *  \param flags direction flags
 */
void StreamTcpPruneSession(Flow *f, uint8_t flags) {
    if (f == NULL || f->protoctx == NULL)
        return;

    TcpSession *ssn = f->protoctx;
    TcpStream *stream = NULL;

    if (flags & STREAM_TOSERVER) {
        stream = &ssn->client;
    } else if (flags & STREAM_TOCLIENT) {
        stream = &ssn->server;
    } else {
        return;
    }

    /* loop through the segments and fill one or more msgs */
    TcpSegment *seg = stream->seg_list;
    uint32_t ra_base_seq = stream->ra_app_base_seq;

    for (; seg != NULL && SEQ_LT(seg->seq, stream->last_ack);)
    {
        SCLogDebug("seg %p, SEQ %"PRIu32", LEN %"PRIu16", SUM %"PRIu32,
                seg, seg->seq, seg->payload_len,
                (uint32_t)(seg->seq + seg->payload_len));

        if (SEQ_LEQ((seg->seq + seg->payload_len), (ra_base_seq+1)) &&
                   (seg->flags & SEGMENTTCP_FLAG_RAW_PROCESSED) &&
                   (seg->flags & SEGMENTTCP_FLAG_APPLAYER_PROCESSED)) {
            if (StreamTcpReturnSegmentCheck(ssn, stream, seg) == 0) {
                seg = seg->next;
                break;
            }

            SCLogDebug("removing pre ra_base_seq %"PRIu32" seg %p seq %"PRIu32
                    " len %"PRIu16"", ra_base_seq, seg, seg->seq, seg->payload_len);

            TcpSegment *next_seg = seg->next;
            StreamTcpRemoveSegmentFromStream(stream, seg);
            StreamTcpSegmentReturntoPool(seg);
            seg = next_seg;
            continue;

        } else if ((ssn->flags & STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED) &&
                (seg->flags & SEGMENTTCP_FLAG_RAW_PROCESSED) &&
                (seg->flags & SEGMENTTCP_FLAG_APPLAYER_PROCESSED))
        {
            if (StreamTcpReturnSegmentCheck(ssn, stream, seg) == 0) {
                seg = seg->next;
                break;
            }

            SCLogDebug("segment(%p) of length %"PRIu16" has been processed,"
                    " so return it to pool", seg, seg->payload_len);
            TcpSegment *next_seg = seg->next;
            seg = next_seg;
            continue;
        } else {
            /* give up */
            break;
        }
    }
}

/**
 *  \brief Update the stream reassembly upon receiving an ACK packet.
 *
 *  Stream is in the opposite direction of the packet, as the ACK-packet
 *  is ACK'ing the stream.
 *
 *  \todo this function is too long, we need to break it up. It needs it BAD
 */
static int StreamTcpReassembleAppLayer (ThreadVars *tv,
        TcpReassemblyThreadCtx *ra_ctx, TcpSession *ssn, TcpStream *stream,
        Packet *p)
{
    SCEnter();

    uint8_t flags = 0;

    SCLogDebug("stream->seg_list %p", stream->seg_list);
#ifdef DEBUG
    PrintList(stream->seg_list);
#endif

    /* if no segments are in the list or all are already processed,
     * and state is beyond established, we send an empty msg */
    TcpSegment *seg_tail = stream->seg_list_tail;
    if (seg_tail == NULL ||
            (seg_tail->flags & SEGMENTTCP_FLAG_APPLAYER_PROCESSED))
    {
        /* send an empty EOF msg if we have no segments but TCP state
         * is beyond ESTABLISHED */
        if (ssn->state >= TCP_CLOSING || (p->flags & PKT_PSEUDO_STREAM_END)) {
            SCLogDebug("sending empty eof message");
            /* send EOF to app layer */
            STREAM_SET_FLAGS(ssn, stream, p, flags);
            AppLayerHandleTCPData(&ra_ctx->dp_ctx, p->flow, ssn,
                    NULL, 0, flags);
            PACKET_PROFILING_APP_STORE(&ra_ctx->dp_ctx, p);

            SCReturnInt(0);
        }
    }

    /* no segments, nothing to do */
    if (stream->seg_list == NULL) {
        SCLogDebug("no segments in the list to reassemble");
        SCReturnInt(0);
    }


    if (stream->flags & STREAMTCP_STREAM_FLAG_GAP) {
        SCReturnInt(0);
    }

    /* stream->ra_app_base_seq remains at stream->isn until protocol is
     * detected. */
    uint32_t ra_base_seq = stream->ra_app_base_seq;
    uint8_t data[4096];
    uint32_t data_len = 0;
    uint16_t payload_offset = 0;
    uint16_t payload_len = 0;
    uint32_t next_seq = ra_base_seq + 1;

    SCLogDebug("ra_base_seq %"PRIu32", last_ack %"PRIu32", next_seq %"PRIu32,
            ra_base_seq, stream->last_ack, next_seq);

    /* loop through the segments and fill one or more msgs */
    TcpSegment *seg = stream->seg_list;
    SCLogDebug("pre-loop seg %p", seg);
    for (; seg != NULL && SEQ_LT(seg->seq, stream->last_ack);)
    {
        SCLogDebug("seg %p, SEQ %"PRIu32", LEN %"PRIu16", SUM %"PRIu32,
                seg, seg->seq, seg->payload_len,
                (uint32_t)(seg->seq + seg->payload_len));

        if (p->flow->flags & FLOW_NO_APPLAYER_INSPECTION) {
            if (seg->flags & SEGMENTTCP_FLAG_RAW_PROCESSED) {
                SCLogDebug("removing seg %p seq %"PRIu32
                           " len %"PRIu16"", seg, seg->seq, seg->payload_len);

                TcpSegment *next_seg = seg->next;
                StreamTcpRemoveSegmentFromStream(stream, seg);
                StreamTcpSegmentReturntoPool(seg);
                seg = next_seg;
                continue;
            } else {
                break;
            }

            /* Remove the segments which are either completely before the
             * ra_base_seq and processed by both app layer and raw reassembly. */
        } else if (SEQ_LEQ((seg->seq + seg->payload_len), (ra_base_seq+1)) &&
                   (seg->flags & SEGMENTTCP_FLAG_RAW_PROCESSED) &&
                   (seg->flags & SEGMENTTCP_FLAG_APPLAYER_PROCESSED)) {
            if (StreamTcpReturnSegmentCheck(ssn, stream, seg) == 0) {
                seg = seg->next;
                continue;
            }

            SCLogDebug("removing pre ra_base_seq %"PRIu32" seg %p seq %"PRIu32
                    " len %"PRIu16"", ra_base_seq, seg, seg->seq, seg->payload_len);

            TcpSegment *next_seg = seg->next;
            StreamTcpRemoveSegmentFromStream(stream, seg);
            StreamTcpSegmentReturntoPool(seg);
            seg = next_seg;
            continue;
        }

        /* if app layer protocol has been detected, then remove all the segments
           which has been previously processed and reassembled */
        if ((ssn->flags & STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED) &&
                (seg->flags & SEGMENTTCP_FLAG_RAW_PROCESSED) &&
                (seg->flags & SEGMENTTCP_FLAG_APPLAYER_PROCESSED))
        {
            if (StreamTcpReturnSegmentCheck(ssn, stream, seg) == 0) {
                next_seq = seg->seq + seg->payload_len;
                seg = seg->next;
                continue;
            }

            SCLogDebug("segment(%p) of length %"PRIu16" has been processed,"
                    " so return it to pool", seg, seg->payload_len);
            next_seq = seg->seq + seg->payload_len;
            TcpSegment *next_seg = seg->next;
            seg = next_seg;
            continue;
        }

        /* we've run into a sequence gap */
        if (SEQ_GT(seg->seq, next_seq)) {

            /* first, pass on data before the gap */
            if (data_len > 0) {
                SCLogDebug("pre GAP data");

                STREAM_SET_FLAGS(ssn, stream, p, flags);

                /* process what we have so far */
                AppLayerHandleTCPData(&ra_ctx->dp_ctx, p->flow, ssn,
                        data, data_len, flags);
                PACKET_PROFILING_APP_STORE(&ra_ctx->dp_ctx, p);
                data_len = 0;
            }

            /* don't conclude it's a gap straight away. If ra_base_seq is lower
             * than last_ack - the window, we consider it a gap. */
            if (SEQ_GT((stream->last_ack - stream->window), ra_base_seq) ||
                ssn->state > TCP_ESTABLISHED)
            {
                /* see what the length of the gap is, gap length is seg->seq -
                 * (ra_base_seq +1) */
#ifdef DEBUG
                uint32_t gap_len = seg->seq - next_seq;
                SCLogDebug("expected next_seq %" PRIu32 ", got %" PRIu32 " , "
                        "stream->last_ack %" PRIu32 ". Seq gap %" PRIu32"",
                        next_seq, seg->seq, stream->last_ack, gap_len);
#endif
                /* We have missed the packet and end host has ack'd it, so
                 * IDS should advance it's ra_base_seq and should not consider this
                 * packet any longer, even if it is retransmitted, as end host will
                 * drop it anyway */
                ra_base_seq = seg->seq - 1;

                /* send gap signal */
                STREAM_SET_FLAGS(ssn, stream, p, flags);
                AppLayerHandleTCPData(&ra_ctx->dp_ctx, p->flow, ssn,
                        NULL, 0, flags|STREAM_GAP);
                PACKET_PROFILING_APP_STORE(&ra_ctx->dp_ctx, p);
                data_len = 0;

                /* set a GAP flag and make sure not bothering this stream anymore */
                SCLogDebug("STREAMTCP_STREAM_FLAG_GAP set");
                stream->flags |= STREAMTCP_STREAM_FLAG_GAP;

                StreamTcpSetEvent(p, STREAM_REASSEMBLY_SEQ_GAP);
                SCPerfCounterIncr(ra_ctx->counter_tcp_reass_gap, tv->sc_perf_pca);
#ifdef DEBUG
                dbg_app_layer_gap++;
#endif
                break;
            } else {
                SCLogDebug("possible GAP, but waiting to see if out of order "
                        "packets might solve that");
#ifdef DEBUG
                dbg_app_layer_gap_candidate++;
#endif
                break;
            }
        }

        int partial = FALSE;

        /* if the segment ends beyond ra_base_seq we need to consider it */
        if (SEQ_GT((seg->seq + seg->payload_len), ra_base_seq+1)) {
            SCLogDebug("seg->seq %" PRIu32 ", seg->payload_len %" PRIu32 ", "
                    "ra_base_seq %" PRIu32 ", last_ack %"PRIu32, seg->seq,
                    seg->payload_len, ra_base_seq, stream->last_ack);

            /* handle segments partly before ra_base_seq */
            if (SEQ_GT(ra_base_seq, seg->seq)) {
                payload_offset = (ra_base_seq + 1) - seg->seq;
                SCLogDebug("payload_offset %u", payload_offset);

                if (SEQ_LT(stream->last_ack, (seg->seq + seg->payload_len))) {
                    if (SEQ_LT(stream->last_ack, (ra_base_seq + 1))) {
                        payload_len = (stream->last_ack - seg->seq);
                        SCLogDebug("payload_len %u", payload_len);
                    } else {
                        payload_len = (stream->last_ack - seg->seq) - payload_offset;
                        SCLogDebug("payload_len %u", payload_len);
                    }
                    partial = TRUE;
                } else {
                    payload_len = seg->payload_len - payload_offset;
                    SCLogDebug("payload_len %u", payload_len);
                }

                if (SCLogDebugEnabled()) {
                    BUG_ON(payload_offset > seg->payload_len);
                    BUG_ON((payload_len + payload_offset) > seg->payload_len);
                }
            } else {
                payload_offset = 0;

                if (SEQ_LT(stream->last_ack, (seg->seq + seg->payload_len))) {
                    payload_len = stream->last_ack - seg->seq;
                    SCLogDebug("payload_len %u", payload_len);

                    partial = TRUE;
                } else {
                    payload_len = seg->payload_len;
                    SCLogDebug("payload_len %u", payload_len);
                }
            }
            SCLogDebug("payload_offset is %"PRIu16", payload_len is %"PRIu16""
                       " and stream->last_ack is %"PRIu32"", payload_offset,
                        payload_len, stream->last_ack);

            if (payload_len == 0) {
                SCLogDebug("no payload_len, so bail out");
                break;
            }

            /* copy the data into the smsg */
            uint16_t copy_size = sizeof(data) - data_len;
            if (copy_size > payload_len) {
                copy_size = payload_len;
            }
            if (SCLogDebugEnabled()) {
                BUG_ON(copy_size > sizeof(data));
            }
            SCLogDebug("copy_size is %"PRIu16"", copy_size);
            memcpy(data + data_len, seg->payload + payload_offset, copy_size);
            data_len += copy_size;
            ra_base_seq += copy_size;
            SCLogDebug("ra_base_seq %"PRIu32", data_len %"PRIu32, ra_base_seq, data_len);

            /* queue the smsg if it's full */
            if (data_len == sizeof(data)) {
                /* process what we have so far */
                STREAM_SET_FLAGS(ssn, stream, p, flags);
                BUG_ON(data_len > sizeof(data));
                AppLayerHandleTCPData(&ra_ctx->dp_ctx, p->flow, ssn,
                        data, data_len, flags);
                PACKET_PROFILING_APP_STORE(&ra_ctx->dp_ctx, p);
                data_len = 0;

                /* if after the first data chunk we have no alproto yet,
                 * there is no point in continueing here. */
                if (!(ssn->flags & STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED)) {
                    SCLogDebug("no alproto after first data chunk");
                    break;
                }
            }

            /* if the payload len is bigger than what we copied, we handle the
             * rest of the payload next... */
            if (copy_size < payload_len) {
                SCLogDebug("copy_size %" PRIu32 " < %" PRIu32 "", copy_size,
                            payload_len);

                payload_offset += copy_size;
                payload_len -= copy_size;
                SCLogDebug("payload_offset is %"PRIu16", seg->payload_len is "
                           "%"PRIu16" and stream->last_ack is %"PRIu32"",
                            payload_offset, seg->payload_len, stream->last_ack);
                if (SCLogDebugEnabled()) {
                    BUG_ON(payload_offset > seg->payload_len);
                }

                /* we need a while loop here as the packets theoretically can be
                 * 64k */
                char segment_done = FALSE;
                while (segment_done == FALSE) {
                    SCLogDebug("new msg at offset %" PRIu32 ", payload_len "
                               "%" PRIu32 "", payload_offset, payload_len);
                    data_len = 0;

                    copy_size = sizeof(data) - data_len;
                    if (copy_size > (seg->payload_len - payload_offset)) {
                        copy_size = (seg->payload_len - payload_offset);
                    }
                    if (SCLogDebugEnabled()) {
                        BUG_ON(copy_size > sizeof(data));
                    }

                    SCLogDebug("copy payload_offset %" PRIu32 ", data_len "
                                "%" PRIu32 ", copy_size %" PRIu32 "",
                                payload_offset, data_len, copy_size);
                    memcpy(data + data_len, seg->payload +
                            payload_offset, copy_size);
                    data_len += copy_size;
                    ra_base_seq += copy_size;
                    SCLogDebug("ra_base_seq %"PRIu32, ra_base_seq);
                    SCLogDebug("copied payload_offset %" PRIu32 ", "
                               "data_len %" PRIu32 ", copy_size %" PRIu32 "",
                               payload_offset, data_len, copy_size);

                    if (data_len == sizeof(data)) {
                        /* process what we have so far */
                        STREAM_SET_FLAGS(ssn, stream, p, flags);
                        BUG_ON(data_len > sizeof(data));
                        AppLayerHandleTCPData(&ra_ctx->dp_ctx, p->flow, ssn,
                                data, data_len, flags);
                        PACKET_PROFILING_APP_STORE(&ra_ctx->dp_ctx, p);
                        data_len = 0;

                        /* if after the first data chunk we have no alproto yet,
                         * there is no point in continueing here. */
                        if (!(ssn->flags & STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED)) {
                            SCLogDebug("no alproto after first data chunk");
                            break;
                        }
                    }

                    /* see if we have segment payload left to process */
                    if ((copy_size + payload_offset) < seg->payload_len) {
                        payload_offset += copy_size;
                        payload_len -= copy_size;

                        if (SCLogDebugEnabled()) {
                            BUG_ON(payload_offset > seg->payload_len);
                        }
                    } else {
                        payload_offset = 0;
                        segment_done = TRUE;
                    }
                }
            }
        }

        /* done with this segment, return it to the pool */
        TcpSegment *next_seg = seg->next;
        next_seq = seg->seq + seg->payload_len;
        if (partial == FALSE) {
            SCLogDebug("fully done with segment in app layer reassembly");
            seg->flags |= SEGMENTTCP_FLAG_APPLAYER_PROCESSED;
        } else {
            SCLogDebug("not yet fully done with segment in app layer reassembly");
        }
        seg = next_seg;
    }

    /* put the partly filled smsg in the queue to the l7 handler */
    if (data_len > 0) {
        SCLogDebug("data_len > 0, %u", data_len);
        /* process what we have so far */
        STREAM_SET_FLAGS(ssn, stream, p, flags);
        BUG_ON(data_len > sizeof(data));
        AppLayerHandleTCPData(&ra_ctx->dp_ctx, p->flow, ssn,
                data, data_len, flags);
        PACKET_PROFILING_APP_STORE(&ra_ctx->dp_ctx, p);
    }

    /* store ra_base_seq in the stream */
    if ((ssn->flags & STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED)) {
        stream->ra_app_base_seq = ra_base_seq;
    }
    SCLogDebug("stream->ra_app_base_seq %u", stream->ra_app_base_seq);
    SCReturnInt(0);
}

/**
 *  \brief Update the stream reassembly upon receiving an ACK packet.
 *  \todo this function is too long, we need to break it up. It needs it BAD
 */
static int StreamTcpReassembleRaw (TcpReassemblyThreadCtx *ra_ctx,
        TcpSession *ssn, TcpStream *stream, Packet *p)
{
    SCEnter();
    SCLogDebug("start p %p", p);

    if (stream->seg_list == NULL) {
        /* send an empty EOF msg if we have no segments but TCP state
         * is beyond ESTABLISHED */
        if (ssn->state > TCP_ESTABLISHED) {
            StreamMsg *smsg = StreamMsgGetFromPool();
            if (smsg == NULL) {
                SCLogDebug("stream_msg_pool is empty");
                SCReturnInt(-1);
            }
            StreamTcpSetupMsg(ssn, stream, p, smsg);
            StreamMsgPutInQueue(ra_ctx->stream_q,smsg);

        } else {
            SCLogDebug("no segments in the list to reassemble");
        }

        SCReturnInt(0);
    }

#if 0
    if (ssn->state <= TCP_ESTABLISHED &&
            !(ssn->flags & STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED)) {
        SCLogDebug("only starting raw reassembly after app layer protocol "
                "detection has completed.");
        SCReturnInt(0);
    }
#endif
    /* check if we have enough data */
    if (StreamTcpReassembleRawCheckLimit(ssn,stream,p) == 0) {
        SCLogDebug("not yet reassembling");
        SCReturnInt(0);
    }

    uint32_t ra_base_seq = stream->ra_raw_base_seq;
    StreamMsg *smsg = NULL;
    uint16_t smsg_offset = 0;
    uint16_t payload_offset = 0;
    uint16_t payload_len = 0;
    TcpSegment *seg = stream->seg_list;
    uint32_t next_seq = ra_base_seq + 1;

    SCLogDebug("ra_base_seq %"PRIu32", last_ack %"PRIu32", next_seq %"PRIu32,
            ra_base_seq, stream->last_ack, next_seq);

    /* loop through the segments and fill one or more msgs */
    for (; seg != NULL && SEQ_LT(seg->seq, stream->last_ack);)
    {
        SCLogDebug("seg %p, SEQ %"PRIu32", LEN %"PRIu16", SUM %"PRIu32,
                seg, seg->seq, seg->payload_len,
                (uint32_t)(seg->seq + seg->payload_len));

        /* Remove the segments which are either completely before the
           ra_base_seq or if they are beyond ra_base_seq, but the segment offset
           from which we need to copy in to smsg is beyond the stream->last_ack.
           As we are copying until the stream->last_ack only */
        if (SEQ_LEQ((seg->seq + seg->payload_len), ra_base_seq+1))
        {
            if (StreamTcpReturnSegmentCheck(ssn, stream, seg) == 0) {
                seg = seg->next;
                continue;
            }

            SCLogDebug("removing pre ra_base_seq %"PRIu32" seg %p seq %"PRIu32""
                        " len %"PRIu16"", ra_base_seq, seg, seg->seq,
                        seg->payload_len);

            TcpSegment *next_seg = seg->next;
            StreamTcpRemoveSegmentFromStream(stream, seg);
            StreamTcpSegmentReturntoPool(seg);
            seg = next_seg;
            continue;
        }

        /* if app layer protocol has been detected, then remove all the segments
         * which has been previously processed and reassembled
         *
         * If the stream is in GAP state the app layer flag won't be set */
        if ((ssn->flags & STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED) &&
                (seg->flags & SEGMENTTCP_FLAG_RAW_PROCESSED) &&
                ((seg->flags & SEGMENTTCP_FLAG_APPLAYER_PROCESSED) ||
                 (stream->flags & STREAMTCP_STREAM_FLAG_GAP)))
        {
            if (StreamTcpReturnSegmentCheck(ssn, stream, seg) == 0) {
                seg = seg->next;
                continue;
            }

            SCLogDebug("segment(%p) of length %"PRIu16" has been processed,"
                    " so return it to pool", seg, seg->payload_len);
            TcpSegment *next_seg = seg->next;
            seg = next_seg;
            continue;
        }

        /* we've run into a sequence gap */
        if (SEQ_GT(seg->seq, next_seq)) {

            /* pass on pre existing smsg (if any) */
            if (smsg != NULL && smsg->data.data_len > 0) {
                /* if app layer protocol has not been detected till yet,
                   then check did we have sent message to app layer already
                   or not. If not then sent the message and set flag that first
                   message has been sent. No more data till proto has not
                   been detected */
                StreamMsgPutInQueue(ra_ctx->stream_q, smsg);
                stream->ra_raw_base_seq = ra_base_seq;
                smsg = NULL;
            }

            /* don't conclude it's a gap straight away. If ra_base_seq is lower
             * than last_ack - the window, we consider it a gap. */
            if (SEQ_GT((stream->last_ack - stream->window), ra_base_seq) ||
                ssn->state > TCP_ESTABLISHED)
            {
                /* see what the length of the gap is, gap length is seg->seq -
                 * (ra_base_seq +1) */
                uint32_t gap_len = seg->seq - next_seq;
                SCLogDebug("expected next_seq %" PRIu32 ", got %" PRIu32 " , "
                        "stream->last_ack %" PRIu32 ". Seq gap %" PRIu32"",
                        next_seq, seg->seq, stream->last_ack, gap_len);

                if (smsg == NULL) {
                    smsg = StreamMsgGetFromPool();
                    if (smsg == NULL) {
                        SCLogDebug("stream_msg_pool is empty");
                        return -1;
                    }
                }
                stream->ra_raw_base_seq = ra_base_seq;

                StreamTcpSetupMsg(ssn, stream, p, smsg);

                /* We have missed the packet and end host has ack'd it, so
                 * IDS should advance it's ra_base_seq and should not consider this
                 * packet any longer, even if it is retransmitted, as end host will
                 * drop it anyway */
                ra_base_seq = seg->seq - 1;

                SCLogDebug("setting STREAM_GAP");
                smsg->flags |= STREAM_GAP;
                smsg->gap.gap_size = gap_len;

                StreamMsgPutInQueue(ra_ctx->stream_q,smsg);
                smsg = NULL;
                smsg_offset = 0;
            } else {
                SCLogDebug("possible GAP, but waiting to see if out of order "
                        "packets might solve that");
                break;
            }
        }

        /* if the segment ends beyond ra_base_seq we need to consider it */
        if (SEQ_GT((seg->seq + seg->payload_len), ra_base_seq+1)) {
            SCLogDebug("seg->seq %" PRIu32 ", seg->payload_len %" PRIu32 ", "
                       "ra_base_seq %" PRIu32 "", seg->seq,
                       seg->payload_len, ra_base_seq);

            /* handle segments partly before ra_base_seq */
            if (SEQ_GT(ra_base_seq, seg->seq)) {
                payload_offset = ra_base_seq - seg->seq;

                if (SEQ_LT(stream->last_ack, (seg->seq + seg->payload_len))) {

                    if (SEQ_LT(stream->last_ack, ra_base_seq)) {
                        payload_len = (stream->last_ack - seg->seq);
                    } else {
                        payload_len = (stream->last_ack - seg->seq) - payload_offset;
                    }
                } else {
                    payload_len = seg->payload_len - payload_offset;
                }

                if (SCLogDebugEnabled()) {
                    BUG_ON(payload_offset > seg->payload_len);
                    BUG_ON((payload_len + payload_offset) > seg->payload_len);
                }
            } else {
                payload_offset = 0;

                if (SEQ_LT(stream->last_ack, (seg->seq + seg->payload_len))) {
                    payload_len = stream->last_ack - seg->seq;
                } else {
                    payload_len = seg->payload_len;
                }
            }
            SCLogDebug("payload_offset is %"PRIu16", payload_len is %"PRIu16""
                       " and stream->last_ack is %"PRIu32"", payload_offset,
                        payload_len, stream->last_ack);

            if (payload_len == 0) {
                SCLogDebug("no payload_len, so bail out");
                break;
            }

            if (smsg == NULL) {
                smsg = StreamMsgGetFromPool();
                if (smsg == NULL) {
                    SCLogDebug("stream_msg_pool is empty");
                    return -1;
                }

                smsg_offset = 0;

                StreamTcpSetupMsg(ssn, stream, p, smsg);
            }
            smsg->data.seq = ra_base_seq+1;


            /* copy the data into the smsg */
            uint16_t copy_size = sizeof (smsg->data.data) - smsg_offset;
            if (copy_size > payload_len) {
                copy_size = payload_len;
            }
            if (SCLogDebugEnabled()) {
                BUG_ON(copy_size > sizeof(smsg->data.data));
            }
            SCLogDebug("copy_size is %"PRIu16"", copy_size);
            memcpy(smsg->data.data + smsg_offset, seg->payload + payload_offset,
                    copy_size);
            smsg_offset += copy_size;
            ra_base_seq += copy_size;
            SCLogDebug("ra_base_seq %"PRIu32, ra_base_seq);

            smsg->data.data_len += copy_size;

            /* queue the smsg if it's full */
            if (smsg->data.data_len == sizeof (smsg->data.data)) {
                StreamMsgPutInQueue(ra_ctx->stream_q, smsg);
                stream->ra_raw_base_seq = ra_base_seq;
                smsg = NULL;
            }

            /* if the payload len is bigger than what we copied, we handle the
             * rest of the payload next... */
            if (copy_size < payload_len) {
                SCLogDebug("copy_size %" PRIu32 " < %" PRIu32 "", copy_size,
                            payload_len);

                payload_offset += copy_size;
                payload_len -= copy_size;
                SCLogDebug("payload_offset is %"PRIu16", seg->payload_len is "
                           "%"PRIu16" and stream->last_ack is %"PRIu32"",
                            payload_offset, seg->payload_len, stream->last_ack);
                if (SCLogDebugEnabled()) {
                    BUG_ON(payload_offset > seg->payload_len);
                }

                /* we need a while loop here as the packets theoretically can be
                 * 64k */
                char segment_done = FALSE;
                while (segment_done == FALSE) {
                    SCLogDebug("new msg at offset %" PRIu32 ", payload_len "
                               "%" PRIu32 "", payload_offset, payload_len);

                    /* get a new message
                       XXX we need a setup function */
                    smsg = StreamMsgGetFromPool();
                    if (smsg == NULL) {
                        SCLogDebug("stream_msg_pool is empty");
                        SCReturnInt(-1);
                    }
                    smsg_offset = 0;

                    StreamTcpSetupMsg(ssn, stream,p,smsg);
                    smsg->data.seq = ra_base_seq+1;

                    copy_size = sizeof(smsg->data.data) - smsg_offset;
                    if (copy_size > payload_len) {
                        copy_size = payload_len;
                    }
                    if (SCLogDebugEnabled()) {
                        BUG_ON(copy_size > sizeof(smsg->data.data));
                    }

                    SCLogDebug("copy payload_offset %" PRIu32 ", smsg_offset "
                                "%" PRIu32 ", copy_size %" PRIu32 "",
                                payload_offset, smsg_offset, copy_size);
                    memcpy(smsg->data.data + smsg_offset, seg->payload +
                            payload_offset, copy_size);
                    smsg_offset += copy_size;
                    ra_base_seq += copy_size;
                    SCLogDebug("ra_base_seq %"PRIu32, ra_base_seq);
                    smsg->data.data_len += copy_size;
                    SCLogDebug("copied payload_offset %" PRIu32 ", "
                               "smsg_offset %" PRIu32 ", copy_size %" PRIu32 "",
                               payload_offset, smsg_offset, copy_size);
                    if (smsg->data.data_len == sizeof (smsg->data.data)) {
                        StreamMsgPutInQueue(ra_ctx->stream_q, smsg);
                        stream->ra_raw_base_seq = ra_base_seq;
                        smsg = NULL;
                    }

                    /* see if we have segment payload left to process */
                    if (copy_size < payload_len) {
                        payload_offset += copy_size;
                        payload_len -= copy_size;

                        if (SCLogDebugEnabled()) {
                            BUG_ON(payload_offset > seg->payload_len);
                        }
                    } else {
                        payload_offset = 0;
                        segment_done = TRUE;
                    }
                }
            }
        }

        /* done with this segment, return it to the pool */
        TcpSegment *next_seg = seg->next;
        seg->flags |= SEGMENTTCP_FLAG_RAW_PROCESSED;
        next_seq = seg->seq + seg->payload_len;
        seg = next_seg;
    }

    /* put the partly filled smsg in the queue to the l7 handler */
    if (smsg != NULL) {
        StreamMsgPutInQueue(ra_ctx->stream_q, smsg);
        smsg = NULL;
        stream->ra_raw_base_seq = ra_base_seq;
    }

    SCReturnInt(0);
}

/** \brief update app layer and raw reassembly
 *
 *  \retval r 0 on success, -1 on error
 */
int StreamTcpReassembleHandleSegmentUpdateACK (ThreadVars *tv,
        TcpReassemblyThreadCtx *ra_ctx, TcpSession *ssn, TcpStream *stream, Packet *p)
{
    SCEnter();

    SCLogDebug("stream->seg_list %p", stream->seg_list);

    int r = 0;
    if (!(StreamTcpInlineMode())) {
        if (StreamTcpReassembleAppLayer(tv, ra_ctx, ssn, stream, p) < 0)
            r = -1;
        if (StreamTcpReassembleRaw(ra_ctx, ssn, stream, p) < 0)
            r = -1;
    }

    SCLogDebug("stream->seg_list %p", stream->seg_list);
    SCReturnInt(r);
}

/** \brief Handle the queue'd smsgs containing reassembled app layer data when
 *         we're running the app layer handling as part of the stream threads.
 *
 *  \param ra_ctx Reassembly thread ctx, contains the queue with stream msgs
 *
 *  \todo Currently we process all msgs even if we encounter an error in one
 *        of them. We do this to make sure the thread ctx's queue is emptied.
 *        Maybe we should just clear & return the msgs in case of error.
 *
 *  \retval 0 ok
 *  \retval -1 error
 */
int StreamTcpReassembleProcessAppLayer(TcpReassemblyThreadCtx *ra_ctx)
{
    SCEnter();

    int r = 0;
    if (ra_ctx != NULL && ra_ctx->stream_q && ra_ctx->stream_q->len > 0) {
        StreamMsg *smsg = NULL;
        do {
            smsg = StreamMsgGetFromQueue(ra_ctx->stream_q);
            if (smsg != NULL) {
                SCLogDebug("smsg %p, next %p, prev %p, flow %p, q->len %u, "
                        "smsg->data.datalen %u, direction %s%s",
                        smsg, smsg->next, smsg->prev, smsg->flow,
                        ra_ctx->stream_q->len, smsg->data.data_len,
                        smsg->flags & STREAM_TOSERVER ? "toserver":"",
                        smsg->flags & STREAM_TOCLIENT ? "toclient":"");

                BUG_ON(smsg->flow == NULL);

                //PrintRawDataFp(stderr, smsg->data.data, smsg->data.data_len);

                /* Handle the stream msg. No need to use locking, flow is
                 * already locked at this point. Don't break out of the
                 * loop if we encounter an error. */
                if (AppLayerHandleTCPMsg(&ra_ctx->dp_ctx, smsg) != 0)
                    r = -1;
            }

        } while (ra_ctx->stream_q->len > 0);
    }

    SCReturnInt(r);
}

int StreamTcpReassembleHandleSegment(ThreadVars *tv, TcpReassemblyThreadCtx *ra_ctx,
                                     TcpSession *ssn, TcpStream *stream,
                                     Packet *p, PacketQueue *pq)
{
    SCEnter();
    SCLogDebug("ssn %p, stream %p, p %p, p->payload_len %"PRIu16"",
                ssn, stream, p, p->payload_len);

    /* we need to update the opposing stream in
     * StreamTcpReassembleHandleSegmentUpdateACK */
    TcpStream *opposing_stream = NULL;
    if (stream == &ssn->client) {
        opposing_stream = &ssn->server;
    } else {
        opposing_stream = &ssn->client;
    }

    /* handle ack received */
    if (StreamTcpReassembleHandleSegmentUpdateACK(tv, ra_ctx, ssn, opposing_stream, p) != 0)
    {
        SCLogDebug("StreamTcpReassembleHandleSegmentUpdateACK error");
        SCReturnInt(-1);
    }

    /* If no stream reassembly/application layer protocol inspection, then
       simple return */
    if (p->payload_len > 0 && !(stream->flags & STREAMTCP_STREAM_FLAG_NOREASSEMBLY)) {
        SCLogDebug("calling StreamTcpReassembleHandleSegmentHandleData");

        if (StreamTcpReassembleHandleSegmentHandleData(tv, ra_ctx, ssn, stream, p) != 0) {
            SCLogDebug("StreamTcpReassembleHandleSegmentHandleData error");
            SCReturnInt(-1);
        }

        p->flags |= PKT_STREAM_ADD;
    }

    /* in stream inline mode even if we have no data we call the reassembly
     * functions to handle EOF */
    if (StreamTcpInlineMode()) {
        int r = 0;
        if (StreamTcpReassembleInlineAppLayer(tv, ra_ctx, ssn, stream, p) < 0)
            r = -1;
        if (StreamTcpReassembleInlineRaw(ra_ctx, ssn, stream, p) < 0)
            r = -1;

        if (r < 0) {
            SCReturnInt(-1);
        }
    }

    StreamTcpReassembleMemuseCounter(tv, ra_ctx);
    SCReturnInt(0);
}

/**
 *  \brief  Function to replace the data from a specific point up to given length.
 *
 *  \param  dst_seg     Destination segment to replace the data
 *  \param  src_seg     Source segment of which data is to be written to destination
 *  \param  start_point Starting point to replace the data onwards
 *  \param  len         Length up to which data is need to be replaced
 *
 *  \todo VJ We can remove the abort()s later.
 *  \todo VJ Why not memcpy?
 */
void StreamTcpSegmentDataReplace(TcpSegment *dst_seg, TcpSegment *src_seg,
                                 uint32_t start_point, uint16_t len)
{
    uint32_t seq;
    uint16_t src_pos = 0;
    uint16_t dst_pos = 0;

    SCLogDebug("start_point %u", start_point);

    if (SEQ_GT(start_point, dst_seg->seq)) {
        dst_pos = start_point - dst_seg->seq;
    } else if (SEQ_LT(start_point, dst_seg->seq)) {
        dst_pos = dst_seg->seq - start_point;
    }

    if (SCLogDebugEnabled()) {
        BUG_ON(((len + dst_pos) - 1) > dst_seg->payload_len);
    } else {
        if (((len + dst_pos) - 1) > dst_seg->payload_len)
            return;
    }

    src_pos = (uint16_t)(start_point - src_seg->seq);

    SCLogDebug("Replacing data from dst_pos %"PRIu16"", dst_pos);

    for (seq = start_point; SEQ_LT(seq, (start_point + len)) &&
            src_pos < src_seg->payload_len && dst_pos < dst_seg->payload_len;
            seq++, dst_pos++, src_pos++)
    {
        dst_seg->payload[dst_pos] = src_seg->payload[src_pos];
    }

    SCLogDebug("Replaced data of size %"PRIu16" up to src_pos %"PRIu16
            " dst_pos %"PRIu16, len, src_pos, dst_pos);
}

/**
 *  \brief  Function to compare the data from a specific point up to given length.
 *
 *  \param  dst_seg     Destination segment to compare the data
 *  \param  src_seg     Source segment of which data is to be compared to destination
 *  \param  start_point Starting point to compare the data onwards
 *  \param  len         Length up to which data is need to be compared
 *
 *  \retval 1 same
 *  \retval 0 different
 */
static int StreamTcpSegmentDataCompare(TcpSegment *dst_seg, TcpSegment *src_seg,
                                 uint32_t start_point, uint16_t len)
{
    uint32_t seq;
    uint16_t src_pos = 0;
    uint16_t dst_pos = 0;

    SCLogDebug("start_point %u dst_seg %u src_seg %u", start_point, dst_seg->seq, src_seg->seq);

    if (SEQ_GT(start_point, dst_seg->seq)) {
        SCLogDebug("start_point %u > dst %u", start_point, dst_seg->seq);
        dst_pos = start_point - dst_seg->seq;
    } else if (SEQ_LT(start_point, dst_seg->seq)) {
        SCLogDebug("start_point %u < dst %u", start_point, dst_seg->seq);
        dst_pos = dst_seg->seq - start_point;
    }

    if (SCLogDebugEnabled()) {
        BUG_ON(((len + dst_pos) - 1) > dst_seg->payload_len);
    } else {
        if (((len + dst_pos) - 1) > dst_seg->payload_len)
            return 1;
    }

    src_pos = (uint16_t)(start_point - src_seg->seq);

    SCLogDebug("Comparing data from dst_pos %"PRIu16", src_pos %u", dst_pos, src_pos);

    for (seq = start_point; SEQ_LT(seq, (start_point + len)) &&
            src_pos < src_seg->payload_len && dst_pos < dst_seg->payload_len;
            seq++, dst_pos++, src_pos++)
    {
        if (dst_seg->payload[dst_pos] != src_seg->payload[src_pos]) {
            SCLogDebug("data is different %02x != %02x, dst_pos %u, src_pos %u", dst_seg->payload[dst_pos], src_seg->payload[src_pos], dst_pos, src_pos);
            return 0;
        }
    }

    SCLogDebug("Compared data of size %"PRIu16" up to src_pos %"PRIu16
            " dst_pos %"PRIu16, len, src_pos, dst_pos);
    return 1;
}

/**
 *  \brief  Function to copy the data from src_seg to dst_seg.
 *
 *  \param  dst_seg     Destination segment for copying the contents
 *  \param  src_seg     Source segment to copy its contents
 *
 *  \todo VJ wouldn't a memcpy be more appropriate here?
 *
 *  \warning Both segments need to be properly initialized.
 */

void StreamTcpSegmentDataCopy(TcpSegment *dst_seg, TcpSegment *src_seg)
{
    uint32_t u;
    uint16_t dst_pos = 0;
    uint16_t src_pos = 0;
    uint32_t seq;

    if (SEQ_GT(dst_seg->seq, src_seg->seq)) {
        src_pos = dst_seg->seq - src_seg->seq;
        seq = dst_seg->seq;
    } else {
        dst_pos = src_seg->seq - dst_seg->seq;
        seq = src_seg->seq;
    }

    SCLogDebug("Copying data from seq %"PRIu32"", seq);
    for (u = seq;
            (SEQ_LT(u, (src_seg->seq + src_seg->payload_len)) &&
             SEQ_LT(u, (dst_seg->seq + dst_seg->payload_len))); u++)
    {
        //SCLogDebug("u %"PRIu32, u);

        dst_seg->payload[dst_pos] = src_seg->payload[src_pos];

        dst_pos++;
        src_pos++;
    }
    SCLogDebug("Copyied data of size %"PRIu16" up to dst_pos %"PRIu16"",
                src_pos, dst_pos);
}

/**
 *  \brief   Function to get the segment of required length from the pool.
 *
 *  \param   len    Length which tells the required size of needed segment.
 *
 *  \retval seg Segment from the pool or NULL
 */
TcpSegment* StreamTcpGetSegment(ThreadVars *tv, TcpReassemblyThreadCtx *ra_ctx, uint16_t len)
{
    uint16_t idx = segment_pool_idx[len];
    SCLogDebug("segment_pool_idx %" PRIu32 " for payload_len %" PRIu32 "",
                idx, len);

    SCMutexLock(&segment_pool_mutex[idx]);
    TcpSegment *seg = (TcpSegment *) PoolGet(segment_pool[idx]);

    SCLogDebug("segment_pool[%u]->empty_list_size %u, segment_pool[%u]->alloc_"
               "list_size %u, alloc %u", idx, segment_pool[idx]->empty_list_size,
               idx, segment_pool[idx]->alloc_list_size,
               segment_pool[idx]->allocated);
    SCMutexUnlock(&segment_pool_mutex[idx]);

    SCLogDebug("seg we return is %p", seg);
    if (seg == NULL) {
        SCLogDebug("segment_pool[%u]->empty_list_size %u, "
                   "alloc %u", idx, segment_pool[idx]->empty_list_size,
                   segment_pool[idx]->allocated);
        /* Increment the counter to show that we are not able to serve the
           segment request due to memcap limit */
        SCPerfCounterIncr(ra_ctx->counter_tcp_segment_memcap, tv->sc_perf_pca);
    } else {
        seg->flags = 0;
        seg->next = NULL;
        seg->prev = NULL;
    }

#ifdef DEBUG
    SCMutexLock(&segment_pool_cnt_mutex);
    segment_pool_cnt++;
    SCMutexUnlock(&segment_pool_cnt_mutex);
#endif

    return seg;
}

/**
 *  \brief Trigger RAW stream reassembly
 *
 *  Used by AppLayerTriggerRawStreamReassembly to trigger RAW stream
 *  reassembly from the applayer, for example upon completion of a
 *  HTTP request.
 *
 *  Works by setting a flag in the TcpSession that is unset as soon
 *  as it's checked. Since everything happens when operating under
 *  a single lock period, no side effects are expected.
 *
 *  \param ssn TcpSession
 */
void StreamTcpReassembleTriggerRawReassembly(TcpSession *ssn) {
#ifdef DEBUG
    BUG_ON(ssn == NULL);
#endif

    if (ssn != NULL) {
        SCLogDebug("flagged ssn %p for immediate raw reassembly", ssn);
        ssn->flags |= STREAMTCP_FLAG_TRIGGER_RAW_REASSEMBLY;
    }
}

#ifdef __tilegx__
/* 
 * Remove this temporarily on Tilera
 * Needs a little more work because of the ThreadVars stuff
 */
#undef UNITTESTS
#endif

#ifdef UNITTESTS
/** unit tests and it's support functions below */

/** \brief  The Function tests the reassembly engine working for different
 *          OSes supported. It includes all the OS cases and send
 *          crafted packets to test the reassembly.
 *
 *  \param  stream  The stream which will contain the reassembled segments
 */

static int StreamTcpReassembleStreamTest(TcpStream *stream) {

    TcpSession ssn;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
    return 0;
    Flow f;
    uint8_t payload[4];
    TCPHdr tcph;
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();

    /* prevent L7 from kicking in */
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOSERVER, 4096);
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOCLIENT, 4096);
    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));

    memset(&ssn, 0, sizeof (TcpSession));
    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    memset(&f, 0, sizeof (Flow));
    memset(&tcph, 0, sizeof (TCPHdr));
    ThreadVars tv;
    memset(&tv, 0, sizeof (ThreadVars));
    FLOW_INITIALIZE(&f);
    f.protoctx = &ssn;
    p->src.family = AF_INET;
    p->dst.family = AF_INET;
    p->proto = IPPROTO_TCP;
    p->flow = &f;
    tcph.th_win = 5480;
    tcph.th_flags = TH_PUSH | TH_ACK;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOSERVER;

    StreamTcpCreateTestPacket(payload, 0x41, 3, 4); /*AAA*/
    p->tcph->th_seq = htonl(12);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 3;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x42, 2, 4); /*BB*/
    p->tcph->th_seq = htonl(16);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 2;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x43, 3, 4); /*CCC*/
    p->tcph->th_seq = htonl(18);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 3;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x44, 1, 4); /*D*/
    p->tcph->th_seq = htonl(22);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 1;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x45, 2, 4); /*EE*/
    p->tcph->th_seq = htonl(25);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 2;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x46, 3, 4); /*FFF*/
    p->tcph->th_seq = htonl(27);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 3;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x47, 2, 4); /*GG*/
    p->tcph->th_seq = htonl(30);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 2;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x48, 2, 4); /*HH*/
    p->tcph->th_seq = htonl(32);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 2;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x49, 1, 4); /*I*/
    p->tcph->th_seq = htonl(34);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 1;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x4a, 4, 4); /*JJJJ*/
    p->tcph->th_seq = htonl(13);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 4;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x4b, 3, 4); /*KKK*/
    p->tcph->th_seq = htonl(18);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 3;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x4c, 3, 4); /*LLL*/
    p->tcph->th_seq = htonl(21);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 3;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x4d, 3, 4); /*MMM*/
    p->tcph->th_seq = htonl(24);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 3;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x4e, 1, 4); /*N*/
    p->tcph->th_seq = htonl(28);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 1;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x4f, 1, 4); /*O*/
    p->tcph->th_seq = htonl(31);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 1;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x50, 1, 4); /*P*/
    p->tcph->th_seq = htonl(32);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 1;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x51, 2, 4); /*QQ*/
    p->tcph->th_seq = htonl(34);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 2;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x30, 1, 4); /*0*/
    p->tcph->th_seq = htonl(11);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 1;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpReassembleFreeThreadCtx(ra_ctx);

    SCFree(p);
    return 1;
}

/** \brief  The Function to create the packet with given payload, which is used
 *          to test the reassembly of the engine.
 *
 *  \param  payload     The variable used to store the payload contents of the
 *                      current packet.
 *  \param  value       The value which current payload will have for this packet
 *  \param  payload_len The length of the filed payload for current packet.
 *  \param  len         Length of the payload array
 */

void StreamTcpCreateTestPacket(uint8_t *payload, uint8_t value,
                               uint8_t payload_len, uint8_t len)
{
    uint8_t i;
    for (i = 0; i < payload_len; i++)
        payload[i] = value;
    for (; i < len; i++)
        payload = NULL;
}

/** \brief  The Function Checks the reassembled stream contents against predefined
 *          stream contents according to OS policy used.
 *
 *  \param  stream_policy   Predefined value of stream for different OS policies
 *  \param  stream          Reassembled stream returned from the reassembly functions
 */

int StreamTcpCheckStreamContents(uint8_t *stream_policy, uint16_t sp_size, TcpStream *stream) {
    TcpSegment *temp;
    uint16_t i = 0;
    uint8_t j;

#ifdef DEBUG
    if (SCLogDebugEnabled()) {
        TcpSegment *temp1;
        for (temp1 = stream->seg_list; temp1 != NULL; temp1 = temp1->next)
            PrintRawDataFp(stdout, temp1->payload, temp1->payload_len);

        PrintRawDataFp(stdout, stream_policy, sp_size);
    }
#endif

    for (temp = stream->seg_list; temp != NULL; temp = temp->next) {
        j = 0;
        for (; j < temp->payload_len; j++) {
            SCLogDebug("i %"PRIu16", len %"PRIu32", stream %"PRIx32" and temp is %"PRIx8"",
                i, temp->payload_len, stream_policy[i], temp->payload[j]);

            if (stream_policy[i] == temp->payload[j]) {
                i++;
                continue;
            } else
                return 0;
        }
    }
    return 1;
}

/** \brief  The Function Checks the Stream Queue contents against predefined
 *          stream contents and the gap lentgh.
 *
 *  \param  stream_contents     Predefined value of stream contents
 *  \param  stream              Queue which has the stream contents
 *
 *  \retval On success the function returns 1, on failure 0.
 */
static int StreamTcpCheckQueue (uint8_t *stream_contents, StreamMsgQueue *q, uint8_t test_case) {
    SCEnter();

    StreamMsg *msg;
    uint16_t i = 0;
    uint8_t j;
    uint8_t cnt = 0;

    if (q == NULL) {
        printf("q == NULL, ");
        SCReturnInt(0);
    }

    if (q->len == 0) {
        printf("q->len == 0, ");
        SCReturnInt(0);
    }

    msg = StreamMsgGetFromQueue(q);
    while(msg != NULL) {
        cnt++;
        switch (test_case) {
            /* Gap at start */
            case 1:
                if (cnt == 1 && msg->gap.gap_size != 3) {
                    printf("msg->gap.gap_size %u, msg->flags %02X, ", msg->gap.gap_size, msg->flags);
                    SCReturnInt(0);
                }
                break;
            /* Gap at middle */
            case 2:
                if (cnt == 2 && msg->gap.gap_size != 3) {
                    SCReturnInt(0);
                }
                break;
            /* Gap at end */
            case 3:
                if (cnt == 3 && msg->gap.gap_size != 3 &&
                        msg->flags & STREAM_GAP)
                {
                    SCReturnInt(0);
                }
                break;
        }

        SCLogDebug("gap is %" PRIu32"", msg->gap.gap_size);

        j = 0;
        for (; j < msg->data.data_len; j++) {
            SCLogDebug("i is %" PRIu32 " and len is %" PRIu32 "  and temp is %" PRIx32 "", i, msg->data.data_len, msg->data.data[j]);

            if (stream_contents[i] == msg->data.data[j]) {
                i++;
                continue;
            } else {
                SCReturnInt(0);
            }
        }
        if (q->len > 0) {
            msg = StreamMsgGetFromQueue(q);
        } else {
            SCReturnInt(1);
        }
    }
    SCReturnInt(1);
}

/* \brief           The function craft packets to test the overlapping, where
 *                  new segment stats before the list segment.
 *
 *  \param  stream  The stream which will contain the reassembled segments and
 *                  also tells the OS policy used for reassembling the segments.
 */

static int StreamTcpTestStartsBeforeListSegment(TcpStream *stream) {
    TcpSession ssn;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;
    Flow f;
    uint8_t payload[4];
    TCPHdr tcph;
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();

    /* prevent L7 from kicking in */
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOSERVER, 4096);
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOCLIENT, 4096);
    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));

    memset(&ssn, 0, sizeof (TcpSession));
    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    memset(&f, 0, sizeof (Flow));
    memset(&tcph, 0, sizeof (TCPHdr));

    ThreadVars tv;
    memset(&tv, 0, sizeof (ThreadVars));
    FLOW_INITIALIZE(&f);
    f.protoctx = &ssn;
    p->src.family = AF_INET;
    p->dst.family = AF_INET;
    p->proto = IPPROTO_TCP;
    p->flow = &f;
    tcph.th_win = 5480;
    tcph.th_flags = TH_PUSH | TH_ACK;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOSERVER;

    StreamTcpCreateTestPacket(payload, 0x42, 1, 4); /*B*/
    p->tcph->th_seq = htonl(16);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 1;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x44, 1, 4); /*D*/
    p->tcph->th_seq = htonl(22);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 1;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x45, 2, 4); /*EE*/
    p->tcph->th_seq = htonl(25);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 2;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x41, 2, 4); /*AA*/
    p->tcph->th_seq = htonl(15);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 2;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x4a, 4, 4); /*JJJJ*/
    p->tcph->th_seq = htonl(14);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 4;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    SCLogDebug("sending segment with SEQ 21, len 3");
    StreamTcpCreateTestPacket(payload, 0x4c, 3, 4); /*LLL*/
    p->tcph->th_seq = htonl(21);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 3;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x4d, 3, 4); /*MMM*/
    p->tcph->th_seq = htonl(24);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 3;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    SCFree(p);
    return 1;
}

/* \brief           The function craft packets to test the overlapping, where
 *                  new segment stats at the same seq no. as the list segment.
 *
 *  \param  stream  The stream which will contain the reassembled segments and
 *                  also tells the OS policy used for reassembling the segments.
 */

static int StreamTcpTestStartsAtSameListSegment(TcpStream *stream) {
    TcpSession ssn;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;
    Flow f;
    uint8_t payload[4];
    TCPHdr tcph;
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();
    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));

    /* prevent L7 from kicking in */
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOSERVER, 4096);
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOCLIENT, 4096);

    memset(&ssn, 0, sizeof (TcpSession));
    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    memset(&f, 0, sizeof (Flow));
    memset(&tcph, 0, sizeof (TCPHdr));
    ThreadVars tv;
    memset(&tv, 0, sizeof (ThreadVars));
    FLOW_INITIALIZE(&f);
    f.protoctx = &ssn;
    p->src.family = AF_INET;
    p->dst.family = AF_INET;
    p->proto = IPPROTO_TCP;
    p->flow = &f;
    tcph.th_win = 5480;
    tcph.th_flags = TH_PUSH | TH_ACK;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOSERVER;

    StreamTcpCreateTestPacket(payload, 0x43, 3, 4); /*CCC*/
    p->tcph->th_seq = htonl(18);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 3;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x48, 2, 4); /*HH*/
    p->tcph->th_seq = htonl(32);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 2;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x49, 1, 4); /*I*/
    p->tcph->th_seq = htonl(34);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 1;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x4b, 3, 4); /*KKK*/
    p->tcph->th_seq = htonl(18);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 3;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x4c, 4, 4); /*LLLL*/
    p->tcph->th_seq = htonl(18);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 4;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x50, 1, 4); /*P*/
    p->tcph->th_seq = htonl(32);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 1;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x51, 2, 4); /*QQ*/
    p->tcph->th_seq = htonl(34);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 2;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    SCFree(p);
    return 1;
}

/* \brief           The function craft packets to test the overlapping, where
 *                  new segment stats after the list segment.
 *
 *  \param  stream  The stream which will contain the reassembled segments and
 *                  also tells the OS policy used for reassembling the segments.
 */


static int StreamTcpTestStartsAfterListSegment(TcpStream *stream) {
    TcpSession ssn;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;
    Flow f;
    uint8_t payload[4];
    TCPHdr tcph;
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();
    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));

    /* prevent L7 from kicking in */
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOSERVER, 4096);
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOCLIENT, 4096);

    memset(&ssn, 0, sizeof (TcpSession));
    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    memset(&f, 0, sizeof (Flow));
    memset(&tcph, 0, sizeof (TCPHdr));
    ThreadVars tv;
    memset(&tv, 0, sizeof (ThreadVars));
    FLOW_INITIALIZE(&f);
    f.protoctx = &ssn;
    p->src.family = AF_INET;
    p->dst.family = AF_INET;
    p->proto = IPPROTO_TCP;
    p->flow = &f;
    tcph.th_win = 5480;
    tcph.th_flags = TH_PUSH | TH_ACK;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOSERVER;

    StreamTcpCreateTestPacket(payload, 0x41, 2, 4); /*AA*/
    p->tcph->th_seq = htonl(12);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 2;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x46, 3, 4); /*FFF*/
    p->tcph->th_seq = htonl(27);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 3;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x47, 2, 4); /*GG*/
    p->tcph->th_seq = htonl(30);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 2;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x4a, 2, 4); /*JJ*/
    p->tcph->th_seq = htonl(13);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 2;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x4f, 1, 4); /*O*/
    p->tcph->th_seq = htonl(31);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 1;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpCreateTestPacket(payload, 0x4e, 1, 4); /*N*/
    p->tcph->th_seq = htonl(28);
    p->tcph->th_ack = htonl(31);
    p->payload = payload;
    p->payload_len = 1;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    SCFree(p);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              before the list segment and BSD policy is used to reassemble
 *              segments.
 */

static int StreamTcpReassembleTest01(void) {
    TcpStream stream;
    uint8_t stream_before_bsd[10] = {0x4a, 0x4a, 0x4a, 0x4a, 0x4c, 0x4c,
                                      0x4c, 0x4d, 0x4d, 0x4d};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_BSD;

    StreamTcpInitConfig(TRUE);

    if (StreamTcpTestStartsBeforeListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_before_bsd,sizeof(stream_before_bsd), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }

    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              at the same seq no. as the list segment and BSD policy is used
 *              to reassemble segments.
 */

static int StreamTcpReassembleTest02(void) {
    TcpStream stream;
    uint8_t stream_same_bsd[8] = {0x43, 0x43, 0x43, 0x4c, 0x48, 0x48,
                                    0x49, 0x51};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_BSD;

    StreamTcpInitConfig(TRUE);

    if (StreamTcpTestStartsAtSameListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_same_bsd, sizeof(stream_same_bsd), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              after the list segment and BSD policy is used to reassemble
 *              segments.
 */

static int StreamTcpReassembleTest03(void) {
    TcpStream stream;
    uint8_t stream_after_bsd[8] = {0x41, 0x41, 0x4a, 0x46, 0x46, 0x46,
                                     0x47, 0x47};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_BSD;

    StreamTcpInitConfig(TRUE);

    if (StreamTcpTestStartsAfterListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_after_bsd, sizeof(stream_after_bsd), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly engine for all the case
 *              before, same and after overlapping and BSD policy is used to
 *              reassemble segments.
 */

static int StreamTcpReassembleTest04(void) {
    TcpStream stream;
    uint8_t stream_bsd[25] = {0x30, 0x41, 0x41, 0x41, 0x4a, 0x4a, 0x42, 0x43,
                               0x43, 0x43, 0x4c, 0x4c, 0x4c, 0x4d, 0x4d, 0x4d,
                               0x46, 0x46, 0x46, 0x47, 0x47, 0x48, 0x48, 0x49, 0x51};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_BSD;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpReassembleStreamTest(&stream) == 0) {
        printf("failed in segments reassembly: ");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_bsd, sizeof(stream_bsd), &stream) == 0) {
        printf("failed in stream matching: ");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              before the list segment and VISTA policy is used to reassemble
 *              segments.
 */

static int StreamTcpReassembleTest05(void) {
    TcpStream stream;
    uint8_t stream_before_vista[10] = {0x4a, 0x41, 0x42, 0x4a, 0x4c, 0x44,
                                        0x4c, 0x4d, 0x45, 0x45};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_VISTA;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpTestStartsBeforeListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_before_vista, sizeof(stream_before_vista), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              at the same seq no. as the list segment and VISTA policy is used
 *              to reassemble segments.
 */

static int StreamTcpReassembleTest06(void) {
    TcpStream stream;
    uint8_t stream_same_vista[8] = {0x43, 0x43, 0x43, 0x4c, 0x48, 0x48,
                                     0x49, 0x51};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_VISTA;

    StreamTcpInitConfig(TRUE);

    if (StreamTcpTestStartsAtSameListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_same_vista, sizeof(stream_same_vista), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              after the list segment and BSD policy is used to reassemble
 *              segments.
 */

static int StreamTcpReassembleTest07(void) {
    TcpStream stream;
    uint8_t stream_after_vista[8] = {0x41, 0x41, 0x4a, 0x46, 0x46, 0x46,
                                      0x47, 0x47};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_VISTA;

    StreamTcpInitConfig(TRUE);

    if (StreamTcpTestStartsAfterListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_after_vista, sizeof(stream_after_vista), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly engine for all the case
 *              before, same and after overlapping and VISTA policy is used to
 *              reassemble segments.
 */

static int StreamTcpReassembleTest08(void) {
    TcpStream stream;
    uint8_t stream_vista[25] = {0x30, 0x41, 0x41, 0x41, 0x4a, 0x42, 0x42, 0x43,
                                 0x43, 0x43, 0x4c, 0x44, 0x4c, 0x4d, 0x45, 0x45,
                                 0x46, 0x46, 0x46, 0x47, 0x47, 0x48, 0x48, 0x49, 0x51};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_VISTA;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpReassembleStreamTest(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_vista, sizeof(stream_vista), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              before the list segment and LINUX policy is used to reassemble
 *              segments.
 */

static int StreamTcpReassembleTest09(void) {
    TcpStream stream;
    uint8_t stream_before_linux[10] = {0x4a, 0x4a, 0x4a, 0x4a, 0x4c, 0x4c,
                                        0x4c, 0x4d, 0x4d, 0x4d};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_LINUX;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpTestStartsBeforeListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_before_linux, sizeof(stream_before_linux), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              at the same seq no. as the list segment and LINUX policy is used
 *              to reassemble segments.
 */

static int StreamTcpReassembleTest10(void) {
    TcpStream stream;
    uint8_t stream_same_linux[8] = {0x4c, 0x4c, 0x4c, 0x4c, 0x48, 0x48,
                                     0x51, 0x51};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_LINUX;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpTestStartsAtSameListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_same_linux, sizeof(stream_same_linux), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              after the list segment and LINUX policy is used to reassemble
 *              segments.
 */

static int StreamTcpReassembleTest11(void) {
    TcpStream stream;
    uint8_t stream_after_linux[8] = {0x41, 0x41, 0x4a, 0x46, 0x46, 0x46,
                                      0x47, 0x47};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_LINUX;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpTestStartsAfterListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_after_linux, sizeof(stream_after_linux), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly engine for all the case
 *              before, same and after overlapping and LINUX policy is used to
 *              reassemble segments.
 */

static int StreamTcpReassembleTest12(void) {
    TcpStream stream;
    uint8_t stream_linux[25] = {0x30, 0x41, 0x41, 0x41, 0x4a, 0x4a, 0x42, 0x43,
                                 0x43, 0x43, 0x4c, 0x4c, 0x4c, 0x4d, 0x4d, 0x4d,
                                 0x46, 0x46, 0x46, 0x47, 0x47, 0x48, 0x48, 0x51, 0x51};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_LINUX;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpReassembleStreamTest(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_linux, sizeof(stream_linux), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              before the list segment and OLD_LINUX policy is used to reassemble
 *              segments.
 */

static int StreamTcpReassembleTest13(void) {
    TcpStream stream;
    uint8_t stream_before_old_linux[10] = {0x4a, 0x4a, 0x4a, 0x4a, 0x4c, 0x4c,
                                            0x4c, 0x4d, 0x4d, 0x4d};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_OLD_LINUX;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpTestStartsBeforeListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_before_old_linux, sizeof(stream_before_old_linux), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              at the same seq no. as the list segment and OLD_LINUX policy is
 *              used to reassemble segments.
 */

static int StreamTcpReassembleTest14(void) {
    TcpStream stream;
    uint8_t stream_same_old_linux[8] = {0x4c, 0x4c, 0x4c, 0x4c, 0x48, 0x48,
                                         0x51, 0x51};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_OLD_LINUX;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpTestStartsAtSameListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_same_old_linux, sizeof(stream_same_old_linux), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              after the list segment and OLD_LINUX policy is used to reassemble
 *              segments.
 */

static int StreamTcpReassembleTest15(void) {
    TcpStream stream;
    uint8_t stream_after_old_linux[8] = {0x41, 0x41, 0x4a, 0x46, 0x46, 0x46,
                                          0x47, 0x47};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_OLD_LINUX;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpTestStartsAfterListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_after_old_linux, sizeof(stream_after_old_linux), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly engine for all the case
 *              before, same and after overlapping and OLD_LINUX policy is used to
 *              reassemble segments.
 */

static int StreamTcpReassembleTest16(void) {
    TcpStream stream;
    uint8_t stream_old_linux[25] = {0x30, 0x41, 0x41, 0x41, 0x4a, 0x4a, 0x42, 0x4b,
                                     0x4b, 0x4b, 0x4c, 0x4c, 0x4c, 0x4d, 0x4d, 0x4d,
                                     0x46, 0x46, 0x46, 0x47, 0x47, 0x48, 0x48, 0x51, 0x51};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_OLD_LINUX;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpReassembleStreamTest(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_old_linux, sizeof(stream_old_linux), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              before the list segment and SOLARIS policy is used to reassemble
 *              segments.
 */

static int StreamTcpReassembleTest17(void) {
    TcpStream stream;
    uint8_t stream_before_solaris[10] = {0x4a, 0x4a, 0x4a, 0x4a, 0x4c, 0x4c,
                                          0x4c, 0x4d, 0x4d, 0x4d};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_SOLARIS;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpTestStartsBeforeListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_before_solaris, sizeof(stream_before_solaris), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              at the same seq no. as the list segment and SOLARIS policy is used
 *              to reassemble segments.
 */

static int StreamTcpReassembleTest18(void) {
    TcpStream stream;
    uint8_t stream_same_solaris[8] = {0x4c, 0x4c, 0x4c, 0x4c, 0x48, 0x48,
                                       0x51, 0x51};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_SOLARIS;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpTestStartsAtSameListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_same_solaris, sizeof(stream_same_solaris), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              after the list segment and SOLARIS policy is used to reassemble
 *              segments.
 */

static int StreamTcpReassembleTest19(void) {
    TcpStream stream;
    uint8_t stream_after_solaris[8] = {0x41, 0x4a, 0x4a, 0x46, 0x46, 0x46,
                                        0x47, 0x47};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_SOLARIS;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpTestStartsAfterListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        StreamTcpFreeConfig(TRUE);
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_after_solaris, sizeof(stream_after_solaris), &stream) == 0) {
        printf("failed in stream matching!!\n");
        StreamTcpFreeConfig(TRUE);
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly engine for all the case
 *              before, same and after overlapping and SOLARIS policy is used to
 *              reassemble segments.
 */

static int StreamTcpReassembleTest20(void) {
    TcpStream stream;
    uint8_t stream_solaris[25] = {0x30, 0x41, 0x4a, 0x4a, 0x4a, 0x42, 0x42, 0x4b,
                                   0x4b, 0x4b, 0x4c, 0x4c, 0x4c, 0x4d, 0x4d, 0x4d,
                                   0x46, 0x46, 0x46, 0x47, 0x47, 0x48, 0x48, 0x51, 0x51};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_SOLARIS;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpReassembleStreamTest(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        StreamTcpFreeConfig(TRUE);
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_solaris, sizeof(stream_solaris), &stream) == 0) {
        printf("failed in stream matching!!\n");
        StreamTcpFreeConfig(TRUE);
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              before the list segment and LAST policy is used to reassemble
 *              segments.
 */

static int StreamTcpReassembleTest21(void) {
    TcpStream stream;
    uint8_t stream_before_last[10] = {0x4a, 0x4a, 0x4a, 0x4a, 0x4c, 0x4c,
                                       0x4c, 0x4d, 0x4d, 0x4d};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_LAST;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpTestStartsBeforeListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_before_last, sizeof(stream_before_last), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              at the same seq no. as the list segment and LAST policy is used
 *              to reassemble segments.
 */

static int StreamTcpReassembleTest22(void) {
    TcpStream stream;
    uint8_t stream_same_last[8] = {0x4c, 0x4c, 0x4c, 0x4c, 0x50, 0x48,
                                    0x51, 0x51};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_LAST;
    StreamTcpInitConfig(TRUE);
    if (StreamTcpTestStartsAtSameListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_same_last, sizeof(stream_same_last), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly when new segment starts
 *              after the list segment and LAST policy is used to reassemble
 *              segments.
 */
static int StreamTcpReassembleTest23(void) {
    TcpStream stream;
    uint8_t stream_after_last[8] = {0x41, 0x4a, 0x4a, 0x46, 0x4e, 0x46, 0x47, 0x4f};
    memset(&stream, 0, sizeof (TcpStream));

    stream.os_policy = OS_POLICY_LAST;
    StreamTcpInitConfig(TRUE);

    if (StreamTcpTestStartsAfterListSegment(&stream) == 0) {
        printf("failed in segments reassembly!!\n");
        return 0;
    }
    if (StreamTcpCheckStreamContents(stream_after_last, sizeof(stream_after_last), &stream) == 0) {
        printf("failed in stream matching!!\n");
        return 0;
    }
    StreamTcpFreeConfig(TRUE);
    return 1;
}

/** \brief      The Function to test the reassembly engine for all the case
 *              before, same and after overlapping and LAST policy is used to
 *              reassemble segments.
 */

static int StreamTcpReassembleTest24(void) {
    int ret = 0;
    TcpStream stream;
    uint8_t stream_last[25] = {0x30, 0x41, 0x4a, 0x4a, 0x4a, 0x4a, 0x42, 0x4b,
                               0x4b, 0x4b, 0x4c, 0x4c, 0x4c, 0x4d, 0x4d, 0x4d,
                               0x46, 0x4e, 0x46, 0x47, 0x4f, 0x50, 0x48, 0x51, 0x51};
    memset(&stream, 0, sizeof (TcpStream));

    stream.os_policy = OS_POLICY_LAST;
    StreamTcpInitConfig(TRUE);

    if (StreamTcpReassembleStreamTest(&stream) == 0)  {
        printf("failed in segments reassembly: ");
        goto end;
    }
    if (StreamTcpCheckStreamContents(stream_last, sizeof(stream_last), &stream) == 0) {
        printf("failed in stream matching: ");
        goto end;
    }

    ret = 1;
end:
    StreamTcpFreeConfig(TRUE);
    return ret;
}

/** \brief  The Function to test the missed packets handling with given payload,
 *          which is used to test the reassembly of the engine.
 *
 *  \param  stream      Stream which contain the packets
 *  \param  seq         Sequence number of the packet
 *  \param  ack         Acknowledgment number of the packet
 *  \param  payload     The variable used to store the payload contents of the
 *                      current packet.
 *  \param  len         The length of the payload for current packet.
 *  \param  th_flag     The TCP flags
 *  \param  flowflags   The packet flow direction
 *  \param  state       The TCP session state
 *
 *  \retval On success it returns 0 and on failure it return -1.
 */

static int StreamTcpTestMissedPacket (TcpReassemblyThreadCtx *ra_ctx,
        TcpSession *ssn, uint32_t seq, uint32_t ack, uint8_t *payload,
        uint16_t len, uint8_t th_flags, uint8_t flowflags, uint8_t state)
{
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return -1;
    Flow f;
    TCPHdr tcph;
    Port sp;
    Port dp;
    struct in_addr in;
    ThreadVars tv;
    PacketQueue pq;

    memset(&pq,0,sizeof(PacketQueue));
    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    memset(&f, 0, sizeof (Flow));
    memset(&tcph, 0, sizeof (TCPHdr));
    memset(&tv, 0, sizeof (ThreadVars));

    sp = 200;
    dp = 220;

    FLOW_INITIALIZE(&f);
    if (inet_pton(AF_INET, "1.2.3.4", &in) != 1) {
        SCFree(p);
        return -1;
    }
    f.src.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.5", &in) != 1) {
        SCFree(p);
        return -1;
    }
    f.dst.addr_data32[0] = in.s_addr;
    f.flags |= FLOW_IPV4;
    f.sp = sp;
    f.dp = dp;
    f.protoctx = ssn;
    p->flow = &f;

    tcph.th_win = htons(5480);
    tcph.th_seq = htonl(seq);
    tcph.th_ack = htonl(ack);
    tcph.th_flags = th_flags;
    p->tcph = &tcph;
    p->flowflags = flowflags;

    p->payload = payload;
    p->payload_len = len;
    ssn->state = state;

    TcpStream *s = NULL;
    if (flowflags & FLOW_PKT_TOSERVER) {
        s = &ssn->server;
    } else {
        s = &ssn->client;
    }

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, ssn, s, p, &pq) == -1) {
        SCFree(p);
        return -1;
    }

    SCFree(p);
    return 0;
}

/**
 *  \test   Test the handling of packets missed by both IDS and the end host.
 *          The packet is missed in the starting of the stream.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpReassembleTest25 (void) {
    int ret = 0;
    uint8_t payload[4];
    uint32_t seq;
    uint32_t ack;
    TcpSession ssn;
    uint8_t th_flag;
    uint8_t flowflags;
    uint8_t check_contents[7] = {0x41, 0x41, 0x41, 0x42, 0x42, 0x43, 0x43};

    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();
    memset(&ssn, 0, sizeof (TcpSession));

    flowflags = FLOW_PKT_TOSERVER;
    th_flag = TH_ACK|TH_PUSH;
    ack = 20;
    StreamTcpInitConfig(TRUE);

    StreamTcpCreateTestPacket(payload, 0x42, 2, 4); /*BB*/
    seq = 10;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 2, th_flag, flowflags, TCP_ESTABLISHED) == -1){
        printf("failed in segments reassembly: ");
        goto end;
    }

    StreamTcpCreateTestPacket(payload, 0x43, 2, 4); /*CC*/
    seq = 12;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 2, th_flag, flowflags, TCP_ESTABLISHED) == -1){
        printf("failed in segments reassembly: ");
        goto end;
    }
    ssn.server.next_seq = 14;
    StreamTcpCreateTestPacket(payload, 0x41, 3, 4); /*AAA*/
    seq = 7;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 3, th_flag, flowflags, TCP_ESTABLISHED) == -1) {
        printf("failed in segments reassembly: ");
        goto end;
    }

    if (StreamTcpCheckStreamContents(check_contents, sizeof(check_contents), &ssn.server) == 0) {
        printf("failed in stream matching: ");
        goto end;
    }

    ret = 1;
end:
    StreamTcpReassembleFreeThreadCtx(ra_ctx);
    StreamTcpFreeConfig(TRUE);
    return ret;
}

/**
 *  \test   Test the handling of packets missed by both IDS and the end host.
 *          The packet is missed in the middle of the stream.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpReassembleTest26 (void) {
    int ret = 0;
    uint8_t payload[4];
    uint32_t seq;
    uint32_t ack;
    TcpSession ssn;
    uint8_t th_flag;
    uint8_t flowflags;
    uint8_t check_contents[7] = {0x41, 0x41, 0x41, 0x42, 0x42, 0x43, 0x43};
    memset(&ssn, 0, sizeof (TcpSession));
    flowflags = FLOW_PKT_TOSERVER;
    th_flag = TH_ACK|TH_PUSH;
    ack = 20;
    StreamTcpInitConfig(TRUE);

    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();

    StreamTcpCreateTestPacket(payload, 0x41, 3, 4); /*AAA*/
    seq = 10;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 3, th_flag, flowflags, TCP_ESTABLISHED) == -1){
        printf("failed in segments reassembly: ");
        goto end;
    }

    StreamTcpCreateTestPacket(payload, 0x43, 2, 4); /*CC*/
    seq = 15;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 2, th_flag, flowflags, TCP_ESTABLISHED) == -1){
        printf("failed in segments reassembly: ");
        goto end;
    }

    StreamTcpCreateTestPacket(payload, 0x42, 2, 4); /*BB*/
    seq = 13;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 2, th_flag, flowflags, TCP_ESTABLISHED) == -1) {
        printf("failed in segments reassembly: ");
        goto end;
    }

    if (StreamTcpCheckStreamContents(check_contents, sizeof(check_contents), &ssn.server) == 0) {
        printf("failed in stream matching: ");
        goto end;
    }

    ret = 1;
end:
    StreamTcpReassembleFreeThreadCtx(ra_ctx);
    StreamTcpFreeConfig(TRUE);
    return ret;
}

/**
 *  \test   Test the handling of packets missed by both IDS and the end host.
 *          The packet is missed in the end of the stream.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpReassembleTest27 (void) {
    int ret = 0;
    uint8_t payload[4];
    uint32_t seq;
    uint32_t ack;
    TcpSession ssn;
    uint8_t th_flag;
    uint8_t flowflags;
    uint8_t check_contents[7] = {0x41, 0x41, 0x41, 0x42, 0x42, 0x43, 0x43};
    memset(&ssn, 0, sizeof (TcpSession));
    flowflags = FLOW_PKT_TOSERVER;
    th_flag = TH_ACK|TH_PUSH;
    ack = 20;
    StreamTcpInitConfig(TRUE);

    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();

    StreamTcpCreateTestPacket(payload, 0x41, 3, 4); /*AAA*/
    seq = 10;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 3, th_flag, flowflags, TCP_ESTABLISHED) == -1){
        printf("failed in segments reassembly: ");
        goto end;
    }

    StreamTcpCreateTestPacket(payload, 0x42, 2, 4); /*BB*/
    seq = 13;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 2, th_flag, flowflags, TCP_ESTABLISHED) == -1){
        printf("failed in segments reassembly: ");
        goto end;
    }

    StreamTcpCreateTestPacket(payload, 0x43, 2, 4); /*CC*/
    seq = 15;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 2, th_flag, flowflags, TCP_ESTABLISHED) == -1) {
        printf("failed in segments reassembly: ");
        goto end;
    }

    if (StreamTcpCheckStreamContents(check_contents, sizeof(check_contents), &ssn.server) == 0) {
        printf("failed in stream matching: ");
        goto end;
    }

    ret = 1;
end:
    StreamTcpReassembleFreeThreadCtx(ra_ctx);
    StreamTcpFreeConfig(TRUE);
    return ret;
}

/**
 *  \test   Test the handling of packets missed by IDS, but the end host has
 *          received it and send the acknowledgment of it. The packet is missed
 *          in the starting of the stream.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpReassembleTest28 (void) {
    int ret = 0;
    uint8_t payload[4];
    uint32_t seq;
    uint32_t ack;
    uint8_t th_flag;
    uint8_t th_flags;
    uint8_t flowflags;
    uint8_t check_contents[5] = {0x41, 0x41, 0x42, 0x42, 0x42};
    TcpSession ssn;
    memset(&ssn, 0, sizeof (TcpSession));
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();
    StreamMsgQueue *q = ra_ctx->stream_q;

    StreamTcpInitConfig(TRUE);
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOSERVER, 4096);
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOCLIENT, 4096);

    flowflags = FLOW_PKT_TOSERVER;
    th_flag = TH_ACK|TH_PUSH;
    th_flags = TH_ACK;

    ssn.server.last_ack = 22;
    ssn.server.ra_raw_base_seq = ssn.server.ra_app_base_seq = 6;
    ssn.server.isn = 6;

    StreamTcpCreateTestPacket(payload, 0x41, 2, 4); /*AA*/
    seq = 10;
    ack = 20;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 2, th_flag, flowflags, TCP_ESTABLISHED) == -1) {
        printf("failed in segments reassembly (1): ");
        goto end;
    }

    flowflags = FLOW_PKT_TOCLIENT;
    StreamTcpCreateTestPacket(payload, 0x00, 0, 4);
    seq = 20;
    ack = 12;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 0, th_flags, flowflags, TCP_ESTABLISHED) == -1) {
        printf("failed in segments reassembly (2): ");
        goto end;
    }

    /* Process stream smsgs we may have in queue */
    if (StreamTcpReassembleProcessAppLayer(ra_ctx) < 0) {
        printf("failed in processing stream smsgs (3): ");
        goto end;
    }

    flowflags = FLOW_PKT_TOSERVER;
    StreamTcpCreateTestPacket(payload, 0x42, 3, 4); /*BBB*/
    seq = 12;
    ack = 20;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 3, th_flag, flowflags, TCP_ESTABLISHED) == -1) {
        printf("failed in segments reassembly (4): ");
        goto end;
    }

    flowflags = FLOW_PKT_TOCLIENT;
    StreamTcpCreateTestPacket(payload, 0x00, 0, 4);
    seq = 20;
    ack = 15;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 0, th_flags, flowflags, TCP_TIME_WAIT) == -1) {
        printf("failed in segments reassembly (5): ");
        goto end;
    }

    if (StreamTcpCheckQueue(check_contents, q, 1) == 0) {
        printf("failed in stream matching (6): ");
        goto end;
    }

    ret = 1;
end:
    StreamTcpReassembleFreeThreadCtx(ra_ctx);
    StreamTcpFreeConfig(TRUE);
    return ret;
}

/**
 *  \test   Test the handling of packets missed by IDS, but the end host has
 *          received it and send the acknowledgment of it. The packet is missed
 *          in the middle of the stream.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpReassembleTest29 (void) {
    int ret = 0;
    uint8_t payload[4];
    uint32_t seq;
    uint32_t ack;
    uint8_t th_flag;
    uint8_t th_flags;
    uint8_t flowflags;
    uint8_t check_contents[5] = {0x41, 0x41, 0x42, 0x42, 0x42};
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();
    StreamMsgQueue *q = ra_ctx->stream_q;
    TcpSession ssn;
    memset(&ssn, 0, sizeof (TcpSession));

    flowflags = FLOW_PKT_TOSERVER;
    th_flag = TH_ACK|TH_PUSH;
    th_flags = TH_ACK;

    ssn.server.last_ack = 22;
    ssn.server.ra_raw_base_seq = 9;
    ssn.server.isn = 9;
    StreamTcpInitConfig(TRUE);

    StreamTcpCreateTestPacket(payload, 0x41, 2, 4); /*AA*/
    seq = 10;
    ack = 20;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 2, th_flag, flowflags, TCP_ESTABLISHED) == -1){
        printf("failed in segments reassembly: ");
        goto end;
    }

    flowflags = FLOW_PKT_TOCLIENT;
    StreamTcpCreateTestPacket(payload, 0x00, 0, 4);
    seq = 20;
    ack = 15;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 0, th_flags, flowflags, TCP_ESTABLISHED) == -1){
        printf("failed in segments reassembly: ");
        goto end;
    }

    /* Process stream smsgs we may have in queue */
    if (StreamTcpReassembleProcessAppLayer(ra_ctx) < 0) {
        printf("failed in processing stream smsgs\n");
        goto end;
    }

    flowflags = FLOW_PKT_TOSERVER;
    StreamTcpCreateTestPacket(payload, 0x42, 3, 4); /*BBB*/
    seq = 15;
    ack = 20;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 3, th_flag, flowflags, TCP_ESTABLISHED) == -1) {
        printf("failed in segments reassembly: ");
        goto end;
    }

    flowflags = FLOW_PKT_TOCLIENT;
    StreamTcpCreateTestPacket(payload, 0x00, 0, 4);
    seq = 20;
    ack = 18;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 0, th_flags, flowflags, TCP_TIME_WAIT) == -1) {
        printf("failed in segments reassembly: ");
        goto end;
    }

    if (StreamTcpCheckQueue(check_contents, q, 2) == 0) {
        printf("failed in stream matching: ");
        goto end;
    }

    ret = 1;
end:
    StreamTcpReassembleFreeThreadCtx(ra_ctx);
    StreamTcpFreeConfig(TRUE);
    return ret;
}

/**
 *  \test   Test the handling of packets missed by IDS, but the end host has
 *          received it and send the acknowledgment of it. The packet is missed
 *          at the end of the stream.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpReassembleTest30 (void) {
    int ret = 0;
    uint8_t payload[4];
    uint32_t seq;
    uint32_t ack;
    uint8_t th_flag;
    uint8_t th_flags;
    uint8_t flowflags;
    uint8_t check_contents[6] = {0x41, 0x41, 0x42, 0x42, 0x42, 0x00};
    TcpSession ssn;
    memset(&ssn, 0, sizeof (TcpSession));

    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();
    StreamMsgQueue *q = ra_ctx->stream_q;

    flowflags = FLOW_PKT_TOSERVER;
    th_flag = TH_ACK|TH_PUSH;
    th_flags = TH_ACK;

    ssn.server.last_ack = 22;
    ssn.server.ra_raw_base_seq = ssn.server.ra_app_base_seq = 9;
    ssn.server.isn = 9;

    StreamTcpInitConfig(TRUE);
    StreamTcpCreateTestPacket(payload, 0x41, 2, 4); /*AA*/
    seq = 10;
    ack = 20;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 2, th_flag, flowflags, TCP_ESTABLISHED) == -1){
        printf("failed in segments reassembly: ");
        goto end;
    }

    flowflags = FLOW_PKT_TOCLIENT;
    StreamTcpCreateTestPacket(payload, 0x00, 0, 4);
    seq = 20;
    ack = 12;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 0, th_flags, flowflags, TCP_ESTABLISHED) == -1){
        printf("failed in segments reassembly: ");
        goto end;
    }

    /* Process stream smsgs we may have in queue */
    if (StreamTcpReassembleProcessAppLayer(ra_ctx) < 0) {
        printf("failed in processing stream smsgs\n");
        goto end;
    }

    flowflags = FLOW_PKT_TOSERVER;
    StreamTcpCreateTestPacket(payload, 0x42, 3, 4); /*BBB*/
    seq = 12;
    ack = 20;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 3, th_flag, flowflags, TCP_ESTABLISHED) == -1) {
        printf("failed in segments reassembly: ");
        goto end;
    }

    flowflags = FLOW_PKT_TOCLIENT;
    StreamTcpCreateTestPacket(payload, 0x00, 0, 4);
    seq = 20;
    ack = 18;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 0, th_flags, flowflags, TCP_ESTABLISHED) == -1) {
        printf("failed in segments reassembly: ");
        goto end;
    }

    /* Process stream smsgs we may have in queue */
    if (StreamTcpReassembleProcessAppLayer(ra_ctx) < 0) {
        printf("failed in processing stream smsgs\n");
        goto end;
    }

    th_flag = TH_FIN|TH_ACK;
    seq = 18;
    ack = 20;
    flowflags = FLOW_PKT_TOSERVER;
    StreamTcpCreateTestPacket(payload, 0x00, 1, 4);
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 1, th_flag, flowflags, TCP_ESTABLISHED) == -1) {
        printf("failed in segments reassembly: ");
        goto end;
    }

    flowflags = FLOW_PKT_TOCLIENT;
    StreamTcpCreateTestPacket(payload, 0x00, 0, 4);
    seq = 20;
    ack = 18;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 0, th_flag, flowflags, TCP_TIME_WAIT) == -1) {
        printf("failed in segments reassembly: ");
        goto end;
    }

    if (StreamTcpCheckQueue(check_contents, q, 3) == 0) {
        printf("failed in stream matching: ");
        goto end;
    }

    ret = 1;
end:
    StreamTcpReassembleFreeThreadCtx(ra_ctx);
    StreamTcpFreeConfig(TRUE);
    return ret;
}

/**
 *  \test   Test to reassemble the packets using the fast track method, as most
 *          packets arrives in order.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpReassembleTest31 (void) {
    int ret = 0;
    uint8_t payload[4];
    uint32_t seq;
    uint32_t ack;
    uint8_t th_flag;
    uint8_t flowflags;
    uint8_t check_contents[5] = {0x41, 0x41, 0x42, 0x42, 0x42};
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();
    TcpSession ssn;
    memset(&ssn, 0, sizeof (TcpSession));

    flowflags = FLOW_PKT_TOSERVER;
    th_flag = TH_ACK|TH_PUSH;

    ssn.server.ra_raw_base_seq = 9;
    ssn.server.isn = 9;
    StreamTcpInitConfig(TRUE);

    StreamTcpCreateTestPacket(payload, 0x41, 2, 4); /*AA*/
    seq = 10;
    ack = 20;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 2, th_flag, flowflags, TCP_ESTABLISHED) == -1){
        printf("failed in segments reassembly: ");
        goto end;
    }

    flowflags = FLOW_PKT_TOSERVER;
    StreamTcpCreateTestPacket(payload, 0x42, 1, 4); /*B*/
    seq = 15;
    ack = 20;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 1, th_flag, flowflags, TCP_ESTABLISHED) == -1) {
        printf("failed in segments reassembly: ");
        goto end;
    }

    flowflags = FLOW_PKT_TOSERVER;
    StreamTcpCreateTestPacket(payload, 0x42, 1, 4); /*B*/
    seq = 12;
    ack = 20;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 1, th_flag, flowflags, TCP_ESTABLISHED) == -1) {
        printf("failed in segments reassembly: ");
        goto end;
    }

    flowflags = FLOW_PKT_TOSERVER;
    StreamTcpCreateTestPacket(payload, 0x42, 1, 4); /*B*/
    seq = 16;
    ack = 20;
    if (StreamTcpTestMissedPacket (ra_ctx, &ssn, seq, ack, payload, 1, th_flag, flowflags, TCP_ESTABLISHED) == -1) {
        printf("failed in segments reassembly: ");
        goto end;
    }

    if (StreamTcpCheckStreamContents(check_contents, 5, &ssn.server) == 0) {
        printf("failed in stream matching: ");
        goto end;
    }

    if (ssn.server.seg_list_tail->seq != 16) {
        printf("failed in fast track handling: ");
        goto end;
    }

    ret = 1;
end:
    StreamTcpReassembleFreeThreadCtx(ra_ctx);
    StreamTcpFreeConfig(TRUE);
    return ret;
}

static int StreamTcpReassembleTest32(void) {
    TcpSession ssn;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;
    Flow f;
    TCPHdr tcph;
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();
    TcpStream stream;
    uint8_t ret = 0;
    uint8_t check_contents[35] = {0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
                                 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
                                 0x41, 0x41, 0x41, 0x41, 0x42, 0x42, 0x42, 0x42,
                                 0x42, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43, 0x43,
                                 0x43, 0x43, 0x43};
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_BSD;
    uint8_t payload[20] = "";

    StreamTcpInitConfig(TRUE);

    /* prevent L7 from kicking in */
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOSERVER, 4096);
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOCLIENT, 4096);

    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));
    memset(&ssn, 0, sizeof (TcpSession));
    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    memset(&f, 0, sizeof (Flow));
    memset(&tcph, 0, sizeof (TCPHdr));
    ThreadVars tv;
    memset(&tv, 0, sizeof (ThreadVars));
    FLOW_INITIALIZE(&f);
    f.protoctx = &ssn;
    p->src.family = AF_INET;
    p->dst.family = AF_INET;
    p->proto = IPPROTO_TCP;
    p->flow = &f;
    tcph.th_win = 5480;
    tcph.th_flags = TH_PUSH | TH_ACK;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOSERVER;

    p->tcph->th_seq = htonl(10);
    p->tcph->th_ack = htonl(31);
    p->payload_len = 10;
    StreamTcpCreateTestPacket(payload, 0x41, 10, 20); /*AA*/
    p->payload = payload;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1)
        goto end;

    p->tcph->th_seq = htonl(20);
    p->tcph->th_ack = htonl(31);
    p->payload_len = 10;
    StreamTcpCreateTestPacket(payload, 0x42, 10, 20); /*BB*/
    p->payload = payload;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1)
        goto end;

    p->tcph->th_seq = htonl(40);
    p->tcph->th_ack = htonl(31);
    p->payload_len = 10;
    StreamTcpCreateTestPacket(payload, 0x43, 10, 20); /*CC*/
    p->payload = payload;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1)
        goto end;

    p->tcph->th_seq = htonl(5);
    p->tcph->th_ack = htonl(31);
    p->payload_len = 20;
    StreamTcpCreateTestPacket(payload, 0x41, 20, 20); /*AA*/
    p->payload = payload;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1)
        goto end;

    if (StreamTcpCheckStreamContents(check_contents, 35, &stream) != 0) {
        ret = 1;
    } else {
        printf("failed in stream matching: ");
    }


end:
    StreamTcpFreeConfig(TRUE);
    SCFree(p);
    return ret;
}

static int StreamTcpReassembleTest33(void) {
    TcpSession ssn;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;
    Flow f;
    TCPHdr tcph;
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();
    TcpStream stream;
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_BSD;
    uint8_t packet[1460] = "";

    StreamTcpInitConfig(TRUE);

    /* prevent L7 from kicking in */
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOSERVER, 4096);
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOCLIENT, 4096);

    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));
    memset(&ssn, 0, sizeof (TcpSession));
    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    memset(&f, 0, sizeof (Flow));
    memset(&tcph, 0, sizeof (TCPHdr));
    ThreadVars tv;
    memset(&tv, 0, sizeof (ThreadVars));
    FLOW_INITIALIZE(&f);
    f.protoctx = &ssn;
    p->src.family = AF_INET;
    p->dst.family = AF_INET;
    p->proto = IPPROTO_TCP;
    p->flow = &f;
    tcph.th_win = 5480;
    tcph.th_flags = TH_PUSH | TH_ACK;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload = packet;

    p->tcph->th_seq = htonl(10);
    p->tcph->th_ack = htonl(31);
    p->payload_len = 10;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    p->tcph->th_seq = htonl(20);
    p->tcph->th_ack = htonl(31);
    p->payload_len = 10;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    p->tcph->th_seq = htonl(40);
    p->tcph->th_ack = htonl(31);
    p->payload_len = 10;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    p->tcph->th_seq = htonl(5);
    p->tcph->th_ack = htonl(31);
    p->payload_len = 30;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpFreeConfig(TRUE);
    SCFree(p);
    return 1;
}

static int StreamTcpReassembleTest34(void) {
    TcpSession ssn;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;
    Flow f;
    TCPHdr tcph;
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();
    TcpStream stream;
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_BSD;
    uint8_t packet[1460] = "";

    StreamTcpInitConfig(TRUE);

    /* prevent L7 from kicking in */
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOSERVER, 4096);
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOCLIENT, 4096);

    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));
    memset(&ssn, 0, sizeof (TcpSession));
    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    memset(&f, 0, sizeof (Flow));
    memset(&tcph, 0, sizeof (TCPHdr));
    ThreadVars tv;
    memset(&tv, 0, sizeof (ThreadVars));
    FLOW_INITIALIZE(&f);
    f.protoctx = &ssn;
    p->src.family = AF_INET;
    p->dst.family = AF_INET;
    p->proto = IPPROTO_TCP;
    p->flow = &f;
    tcph.th_win = 5480;
    tcph.th_flags = TH_PUSH | TH_ACK;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload = packet;

    p->tcph->th_seq = htonl(857961230);
    p->tcph->th_ack = htonl(31);
    p->payload_len = 304;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    p->tcph->th_seq = htonl(857961534);
    p->tcph->th_ack = htonl(31);
    p->payload_len = 1460;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    p->tcph->th_seq = htonl(857963582);
    p->tcph->th_ack = htonl(31);
    p->payload_len = 1460;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    p->tcph->th_seq = htonl(857960946);
    p->tcph->th_ack = htonl(31);
    p->payload_len = 1460;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpFreeConfig(TRUE);
    SCFree(p);
    return 1;
}

/** \test Test the bug 56 condition */
static int StreamTcpReassembleTest35(void) {
    TcpSession ssn;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;
    Flow f;
    TCPHdr tcph;
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();
    TcpStream stream;
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_BSD;
    uint8_t packet[1460] = "";

    StreamTcpInitConfig(TRUE);

    /* prevent L7 from kicking in */
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOSERVER, 10);
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOCLIENT, 10);

    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));
    memset(&ssn, 0, sizeof (TcpSession));
    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    memset(&f, 0, sizeof (Flow));
    memset(&tcph, 0, sizeof (TCPHdr));
    ThreadVars tv;
    memset(&tv, 0, sizeof (ThreadVars));
    FLOW_INITIALIZE(&f);
    f.protoctx = &ssn;
    p->src.family = AF_INET;
    p->dst.family = AF_INET;
    p->proto = IPPROTO_TCP;
    p->flow = &f;
    tcph.th_win = 5480;
    tcph.th_flags = TH_PUSH | TH_ACK;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload = packet;

    p->tcph->th_seq = htonl(2257022155UL);
    p->tcph->th_ack = htonl(1374943142);
    p->payload_len = 142;
    stream.last_ack = 2257022285UL;
    stream.ra_raw_base_seq = 2257022172UL;
    stream.ra_app_base_seq = 2257022172UL;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    p->tcph->th_seq = htonl(2257022285UL);
    p->tcph->th_ack = htonl(1374943142);
    p->payload_len = 34;
    stream.last_ack = 2257022285UL;
    stream.ra_raw_base_seq = 2257022172UL;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpFreeConfig(TRUE);
    SCFree(p);
    return 1;
}

/** \test Test the bug 57 condition */
static int StreamTcpReassembleTest36(void) {
    TcpSession ssn;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;
    Flow f;
    TCPHdr tcph;
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();
    TcpStream stream;
    memset(&stream, 0, sizeof (TcpStream));
    stream.os_policy = OS_POLICY_BSD;
    uint8_t packet[1460] = "";

    StreamTcpInitConfig(TRUE);

    /* prevent L7 from kicking in */
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOSERVER, 10);
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOCLIENT, 10);

    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));
    memset(&ssn, 0, sizeof (TcpSession));
    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    memset(&f, 0, sizeof (Flow));
    memset(&tcph, 0, sizeof (TCPHdr));
    ThreadVars tv;
    memset(&tv, 0, sizeof (ThreadVars));
    FLOW_INITIALIZE(&f);
    f.protoctx = &ssn;
    p->src.family = AF_INET;
    p->dst.family = AF_INET;
    p->proto = IPPROTO_TCP;
    p->flow = &f;
    tcph.th_win = 5480;
    tcph.th_flags = TH_PUSH | TH_ACK;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload = packet;

    p->tcph->th_seq = htonl(1549588966);
    p->tcph->th_ack = htonl(4162241372UL);
    p->payload_len = 204;
    stream.last_ack = 1549589007;
    stream.ra_raw_base_seq = 1549589101;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    p->tcph->th_seq = htonl(1549589007);
    p->tcph->th_ack = htonl(4162241372UL);
    p->payload_len = 23;
    stream.last_ack = 1549589007;
    stream.ra_raw_base_seq = 1549589101;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpFreeConfig(TRUE);
    SCFree(p);
    return 1;
}

/** \test Test the bug 76 condition */
static int StreamTcpReassembleTest37(void) {
    TcpSession ssn;
    Flow f;
    TCPHdr tcph;
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();
    TcpStream stream;
    uint8_t packet[1460] = "";
    PacketQueue pq;
    ThreadVars tv;

    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;

    StreamTcpInitConfig(TRUE);

    /* prevent L7 from kicking in */
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOSERVER, 10);
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOCLIENT, 10);

    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);

    memset(&stream, 0, sizeof (TcpStream));
    memset(&pq,0,sizeof(PacketQueue));
    memset(&ssn, 0, sizeof (TcpSession));
    memset(&f, 0, sizeof (Flow));
    memset(&tcph, 0, sizeof (TCPHdr));
    memset(&tv, 0, sizeof (ThreadVars));

    FLOW_INITIALIZE(&f);
    f.protoctx = &ssn;
    p->src.family = AF_INET;
    p->dst.family = AF_INET;
    p->proto = IPPROTO_TCP;
    p->flow = &f;
    tcph.th_win = 5480;
    tcph.th_flags = TH_PUSH | TH_ACK;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload = packet;
    stream.os_policy = OS_POLICY_BSD;

    p->tcph->th_seq = htonl(3061088537UL);
    p->tcph->th_ack = htonl(1729548549UL);
    p->payload_len = 1391;
    stream.last_ack = 3061091137UL;
    stream.ra_raw_base_seq = 3061091309UL;
    stream.ra_app_base_seq = 3061091309UL;

    /* pre base_seq, so should be rejected */
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) != -1) {
        SCFree(p);
        return 0;
    }

    p->tcph->th_seq = htonl(3061089928UL);
    p->tcph->th_ack = htonl(1729548549UL);
    p->payload_len = 1391;
    stream.last_ack = 3061091137UL;
    stream.ra_raw_base_seq = 3061091309UL;
    stream.ra_app_base_seq = 3061091309UL;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    p->tcph->th_seq = htonl(3061091319UL);
    p->tcph->th_ack = htonl(1729548549UL);
    p->payload_len = 1391;
    stream.last_ack = 3061091137UL;
    stream.ra_raw_base_seq = 3061091309UL;
    stream.ra_app_base_seq = 3061091309UL;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx,&ssn, &stream, p, &pq) == -1) {
        SCFree(p);
        return 0;
    }

    StreamTcpFreeConfig(TRUE);
    SCFree(p);
    return 1;
}

/**
 *  \test   Test to make sure we don't send the smsg from toclient to app layer
 *          until the app layer protocol has been detected and one smsg from
 *          toserver side has been sent to app layer.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpReassembleTest38 (void) {
    int ret = 0;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;
    Flow f;
    TCPHdr tcph;
    Port sp;
    Port dp;
    struct in_addr in;
    TcpSession ssn;

    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));
    memset(&f, 0, sizeof (Flow));
    memset(&tcph, 0, sizeof (TCPHdr));
    memset(&ssn, 0, sizeof(TcpSession));
    ThreadVars tv;
    memset(&tv, 0, sizeof (ThreadVars));

    StreamTcpInitConfig(TRUE);
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();

    uint8_t httpbuf1[] = "POST / HTTP/1.0\r\nUser-Agent: Victor/1.0\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */

    uint8_t httpbuf2[] = "HTTP/1.0 200 OK\r\nServer: VictorServer/1.0\r\n\r\n";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    FLOW_INITIALIZE(&f);
    if (inet_pton(AF_INET, "1.2.3.4", &in) != 1)
        goto end;
    f.src.addr_data32[0] = in.s_addr;
    if (inet_pton(AF_INET, "1.2.3.5", &in) != 1)
        goto end;
    f.dst.addr_data32[0] = in.s_addr;
    sp = 200;
    dp = 220;

    ssn.server.ra_raw_base_seq = ssn.server.ra_app_base_seq = 9;
    ssn.server.isn = 9;
    ssn.server.last_ack = 60;
    ssn.client.ra_raw_base_seq = ssn.client.ra_app_base_seq = 9;
    ssn.client.isn = 9;
    ssn.client.last_ack = 60;
    f.alproto = ALPROTO_UNKNOWN;

    f.flags |= FLOW_IPV4;
    f.sp = sp;
    f.dp = dp;
    f.protoctx = &ssn;
    p->flow = &f;

    tcph.th_win = htons(5480);
    tcph.th_seq = htonl(10);
    tcph.th_ack = htonl(20);
    tcph.th_flags = TH_ACK|TH_PUSH;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOCLIENT;

    p->payload = httpbuf2;
    p->payload_len = httplen2;
    ssn.state = TCP_ESTABLISHED;

    TcpStream *s = NULL;
    s = &ssn.server;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (1): ");
        goto end;
    }

    /* Check if we have stream smsgs in queue */
    if (ra_ctx->stream_q->len > 0) {
        printf("there shouldn't be any stream smsgs in the queue (2): ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload = httpbuf1;
    p->payload_len = httplen1;
    tcph.th_seq = htonl(10);
    tcph.th_ack = htonl(55);
    s = &ssn.client;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (3): ");
        goto end;
    }

    /* Check if we have stream smsgs in queue */
    if (ra_ctx->stream_q->len > 0) {
        printf("there shouldn't be any stream smsgs in the queue, as we didn't"
                " processed any smsg from toserver side till yet (4): ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOCLIENT;
    p->payload = httpbuf2;
    p->payload_len = httplen2;
    tcph.th_seq = htonl(55);
    tcph.th_ack = htonl(53);
    s = &ssn.server;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (5): ");
        goto end;
    }

    /* we should now have a smsg as the http request is complete and triggered
     * reassembly */
    if (ra_ctx->stream_q->len != 1) {
        printf("there should one stream smsg in the queue (6): ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload = httpbuf1;
    p->payload_len = httplen1;
    tcph.th_seq = htonl(53);
    tcph.th_ack = htonl(100);
    s = &ssn.client;
    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (8): ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOCLIENT;
    p->payload = NULL;
    p->payload_len = 0;
    tcph.th_seq = htonl(100);
    tcph.th_ack = htonl(53);
    s = &ssn.server;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (9): ");
        goto end;
    }

    ret = 1;
end:
    StreamTcpReassembleFreeThreadCtx(ra_ctx);
    StreamTcpFreeConfig(TRUE);
    SCFree(p);
    return ret;
}

/**
 *  \test   Test to make sure that we don't return the segments until the app
 *          layer proto has been detected and after that remove the processed
 *          segments.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpReassembleTest39 (void) {
    SCEnter();

    int ret = 0;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;
    Flow *f = NULL;
    TCPHdr tcph;
    TcpSession ssn;

    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));
    memset(&tcph, 0, sizeof (TCPHdr));
    memset(&ssn, 0, sizeof(TcpSession));
    ThreadVars tv;
    memset(&tv, 0, sizeof (ThreadVars));

    StreamTcpInitConfig(TRUE);
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOSERVER, 7);
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOCLIENT, 7);

    uint8_t httpbuf1[] = "POST / HTTP/1.0\r\nUser-Agent: Victor/1.0\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */

    uint8_t httpbuf2[] = "HTTP/1.0 200 OK\r\nServer: VictorServer/1.0\r\n\r\n";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    ssn.server.ra_raw_base_seq = ssn.server.ra_app_base_seq = 9;
    ssn.server.isn = 9;
    ssn.server.last_ack = 160;
    ssn.client.ra_raw_base_seq = ssn.client.ra_app_base_seq= 9;
    ssn.client.isn = 9;
    ssn.client.last_ack = 160;

    f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 200, 220);
    if (f == NULL)
        goto end;
    f->protoctx = &ssn;
    p->flow = f;

    SCLogDebug("check client seg list %p", ssn.client.seg_list);
    tcph.th_win = htons(5480);
    tcph.th_seq = htonl(10);
    tcph.th_ack = htonl(20);
    tcph.th_flags = TH_ACK|TH_PUSH;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOCLIENT;

    p->payload = httpbuf2;
    p->payload_len = httplen2;
    ssn.state = TCP_ESTABLISHED;

    TcpStream *s = NULL;
    s = &ssn.server;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (1): ");
        goto end;
    }

    /* Check if we have stream smsgs in queue */
    if (ra_ctx->stream_q->len > 0) {
        printf("there shouldn't be any stream smsgs in the queue: (2): ");
        goto end;
    }

    SCLogDebug("check client seg list %p", ssn.client.seg_list);
    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload = httpbuf1;
    p->payload_len = httplen1;
    tcph.th_seq = htonl(10);
    tcph.th_ack = htonl(55);
    s = &ssn.client;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (3): ");
        goto end;
    }

    /* Check if we have stream smsgs in queue */
    if (ra_ctx->stream_q->len == 0) {
        printf("there should be stream smsgs in the queue (4): ");
        goto end;
    }
    SCLogDebug("check client seg list %p", ssn.client.seg_list);


    p->flowflags = FLOW_PKT_TOCLIENT;
    p->payload = httpbuf2;
    p->payload_len = httplen2;
    tcph.th_seq = htonl(55);
    tcph.th_ack = htonl(53);
    s = &ssn.server;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (5): ");
        goto end;
    }
    SCLogDebug("check client seg list %p", ssn.client.seg_list);


    /* Check if we have stream smsgs in queue */
    SCLogDebug("check if we have stream smsgs in queue");
    if (ra_ctx->stream_q->len == 0) {
        printf("there should be a stream smsgs in the queue (6): ");
        goto end;
    }

    SCLogDebug("check client seg list %p", ssn.client.seg_list);
    /* Process stream smsgs we may have in queue */
    SCLogDebug("process stream smsgs we may have in queue");
    if (StreamTcpReassembleProcessAppLayer(ra_ctx) < 0) {
        printf("failed in processing stream smsgs (7): ");
        goto end;
    }

    SCLogDebug("check client seg list %p", ssn.client.seg_list);

    /* check is have the segment in the list and flagged or not */
/*
    if (ssn.client.seg_list == NULL ||
            !(ssn.client.seg_list->flags & SEGMENTTCP_FLAG_RAW_PROCESSED))
    {
        printf("the list is NULL or the processed segment has not been flaged (8), seg %p, flags %02X: ",
                ssn.client.seg_list, ssn.client.seg_list? ssn.client.seg_list->flags:0);
//abort();
        goto end;
    }
*/
    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload = httpbuf1;
    p->payload_len = httplen1;
    tcph.th_seq = htonl(53);
    tcph.th_ack = htonl(100);
    s = &ssn.client;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (9): ");
        goto end;
    }

    /* Check if we have stream smsgs in queue */
    if (ra_ctx->stream_q->len == 0 &&
            !(ssn.flags & STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED)) {
        printf("there should be a stream smsgs in the queue, as we have detected"
                " the app layer protocol and one smsg from toserver side has "
                "been sent (10): ");
        goto end;
    /* Process stream smsgs we may have in queue */
    } else if (StreamTcpReassembleProcessAppLayer(ra_ctx) < 0) {
        printf("failed in processing stream smsgs (11): ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOCLIENT;
    p->payload = httpbuf2;
    p->payload_len = httplen2;
    tcph.th_seq = htonl(100);
    tcph.th_ack = htonl(96);
    s = &ssn.server;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (12): ");
        goto end;
    }

    SCLogDebug("final check");

    if (!(ssn.flags & STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED)) {
        printf("STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED flag should have been set (13): ");
        goto end;
    }

    /* check if the segment in the list is flagged or not */
    if (ssn.client.seg_list == NULL) {
        printf("segment list should not be empty (14): ");
        goto end;
    }

    SCLogDebug("ssn.client.seg_list->flags %02x, seg %p", ssn.client.seg_list->flags, ssn.client.seg_list);

    if (!(ssn.client.seg_list->flags & SEGMENTTCP_FLAG_APPLAYER_PROCESSED)) {
        printf("segment should have flags SEGMENTTCP_FLAG_APPLAYER_PROCESSED set (15): ");
        goto end;
    }

    if (!(ssn.client.seg_list->flags & SEGMENTTCP_FLAG_RAW_PROCESSED)) {
        printf("segment should have flags SEGMENTTCP_FLAG_RAW_PROCESSED set (16): ");
        goto end;
    }

    ret = 1;
end:
    StreamTcpReassembleFreeThreadCtx(ra_ctx);
    StreamTcpFreeConfig(TRUE);
    SCFree(p);
    UTHFreeFlow(f);
    return ret;
}

/**
 *  \test   Test to make sure that we sent all the segments from the initial
 *          segments to app layer until we have detected the app layer proto.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpReassembleTest40 (void) {
    int ret = 0;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;
    Flow *f = NULL;
    TCPHdr tcph;
    TcpSession ssn;

    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));
    memset(&tcph, 0, sizeof (TCPHdr));
    memset(&ssn, 0, sizeof(TcpSession));
    ThreadVars tv;
    memset(&tv, 0, sizeof (ThreadVars));

    StreamTcpInitConfig(TRUE);
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOSERVER, 130);

    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();

    uint8_t httpbuf1[] = "P";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint8_t httpbuf3[] = "O";
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */
    uint8_t httpbuf4[] = "S";
    uint32_t httplen4 = sizeof(httpbuf4) - 1; /* minus the \0 */
    uint8_t httpbuf5[] = "T \r\n";
    uint32_t httplen5 = sizeof(httpbuf5) - 1; /* minus the \0 */

    uint8_t httpbuf2[] = "HTTP/1.0 200 OK\r\nServer: VictorServer/1.0\r\n\r\n";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    ssn.server.ra_raw_base_seq = ssn.server.ra_app_base_seq = 9;
    ssn.server.isn = 9;
    ssn.server.last_ack = 10;
    ssn.client.ra_raw_base_seq = ssn.client.ra_app_base_seq = 9;
    ssn.client.isn = 9;
    ssn.client.last_ack = 10;

    f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 200, 220);
    if (f == NULL)
        goto end;
    f->protoctx = &ssn;
    p->flow = f;

    tcph.th_win = htons(5480);
    tcph.th_seq = htonl(10);
    tcph.th_ack = htonl(10);
    tcph.th_flags = TH_ACK|TH_PUSH;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOSERVER;

    p->payload = httpbuf1;
    p->payload_len = httplen1;
    ssn.state = TCP_ESTABLISHED;

    TcpStream *s = NULL;
    s = &ssn.client;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (1): ");
        goto end;
    }

    /* Check if we have stream smsgs in queue */
    if (ra_ctx->stream_q->len > 0) {
        printf("there shouldn't be any stream smsgs in the queue, as we didn't"
                " processed any smsg from toserver side till yet (2): ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOCLIENT;
    p->payload = httpbuf2;
    p->payload_len = httplen2;
    tcph.th_seq = htonl(10);
    tcph.th_ack = htonl(11);
    s = &ssn.server;
    ssn.server.last_ack = 11;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (3): ");
        goto end;
    }

    /* Process stream smsgs we may have in queue */
    if (StreamTcpReassembleProcessAppLayer(ra_ctx) < 0) {
        printf("failed in processing stream smsgs (4): ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload = httpbuf3;
    p->payload_len = httplen3;
    tcph.th_seq = htonl(11);
    tcph.th_ack = htonl(55);
    s = &ssn.client;
    ssn.client.last_ack = 55;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (5): ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOCLIENT;
    p->payload = httpbuf2;
    p->payload_len = httplen2;
    tcph.th_seq = htonl(55);
    tcph.th_ack = htonl(12);
    s = &ssn.server;
    ssn.server.last_ack = 12;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (6): ");
        goto end;
    }

    /* check is have the segment in the list and flagged or not */
    if (ssn.client.seg_list == NULL ||
            !(ssn.client.seg_list->flags & SEGMENTTCP_FLAG_APPLAYER_PROCESSED))
    {
        printf("the list is NULL or the processed segment has not been flaged (7): ");
        goto end;
    }

    /* Check if we have stream smsgs in queue */
#if 0
    if (ra_ctx->stream_q->len == 0) {
        printf("there should be a stream smsgs in the queue, as we have detected"
                " the app layer protocol and one smsg from toserver side has "
                "been sent (8): ");
        goto end;
    /* Process stream smsgs we may have in queue */
    } else if (StreamTcpReassembleProcessAppLayer(ra_ctx) < 0) {
        printf("failed in processing stream smsgs (9): ");
        goto end;
    }
#endif
    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload = httpbuf4;
    p->payload_len = httplen4;
    tcph.th_seq = htonl(12);
    tcph.th_ack = htonl(100);
    s = &ssn.client;
    ssn.client.last_ack = 100;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (10): ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOCLIENT;
    p->payload = httpbuf2;
    p->payload_len = httplen2;
    tcph.th_seq = htonl(100);
    tcph.th_ack = htonl(13);
    s = &ssn.server;
    ssn.server.last_ack = 13;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (11): ");
        goto end;
    }

    /* Check if we have stream smsgs in queue */
#if 0
    if (ra_ctx->stream_q->len == 0) {
        printf("there should be a stream smsgs in the queue, as we have detected"
                " the app layer protocol and one smsg from toserver side has "
                "been sent (12): ");
        goto end;
    /* Process stream smsgs we may have in queue */
    } else if (StreamTcpReassembleProcessAppLayer(ra_ctx) < 0) {
        printf("failed in processing stream smsgs (13): ");
        goto end;
    }
#endif
    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload = httpbuf5;
    p->payload_len = httplen5;
    tcph.th_seq = htonl(13);
    tcph.th_ack = htonl(145);
    s = &ssn.client;
    ssn.client.last_ack = 145;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (14): ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOCLIENT;
    p->payload = httpbuf2;
    p->payload_len = httplen2;
    tcph.th_seq = htonl(145);
    tcph.th_ack = htonl(16);
    s = &ssn.server;
    ssn.server.last_ack = 16;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (15): ");
        goto end;
    }

    /* Check if we have stream smsgs in queue */
    if (ra_ctx->stream_q->len == 0) {
        printf("there should be a stream smsgs in the queue, as we have detected"
                " the app layer protocol and one smsg from toserver side has "
                "been sent (16): ");
        goto end;
    /* Process stream smsgs we may have in queue */
    }

    if (StreamTcpReassembleProcessAppLayer(ra_ctx) < 0) {
        printf("failed in processing stream smsgs (17): ");
        goto end;
    }

    if (f->alproto != ALPROTO_HTTP) {
        printf("app layer proto has not been detected (18): ");
        goto end;
    }

    ret = 1;
end:
    StreamTcpReassembleFreeThreadCtx(ra_ctx);
    StreamTcpFreeConfig(TRUE);
    SCFree(p);
    UTHFreeFlow(f);
    return ret;
}

/**
 *  \test   Test to make sure we don't send more than one smsg from toserver to
 *          app layer  until the app layer protocol has not been detected. After
 *          protocol has been detected the processed segments should be returned
 *          to pool.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpReassembleTest41 (void) {
    int ret = 0;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;
    Flow *f = NULL;
    TCPHdr tcph;
    TcpSession ssn;

    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));
    memset(&tcph, 0, sizeof (TCPHdr));
    memset(&ssn, 0, sizeof(TcpSession));
    ThreadVars tv;
    memset(&tv, 0, sizeof (ThreadVars));

    StreamTcpInitConfig(TRUE);
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();

    uint8_t httpbuf1[] = "GET / HTTP/1.0\r\nUser-Agent: Victor/1.0"
                         "W2dyb3VwMV0NCnBob25lMT1wMDB3ODgyMTMxMzAyMTINCmxvZ2lu"
                         "MT0NCnBhc3N3b3JkMT0NCnBob25lMj1wMDB3ODgyMTMxMzAyMTIN"
                         "CmxvZ2luMj0NCnBhc3N3b3JkMj0NCnBob25lMz0NCmxvZ2luMz0N"
                         "CnBhc3N3b3JkMz0NCnBob25lND0NCmxvZ2luND0NCnBhc3N3b3Jk"
                         "ND0NCnBob25lNT0NCmxvZ2luNT0NCnBhc3N3b3JkNT0NCnBob25l"
                         "Nj0NCmxvZ2luNj0NCnBhc3N3b3JkNj0NCmNhbGxfdGltZTE9MzIN"
                         "CmNhbGxfdGltZTI9MjMyDQpkYXlfbGltaXQ9NQ0KbW9udGhfbGlt"
                         "aXQ9MTUNCltncm91cDJdDQpwaG9uZTE9DQpsb2dpbjE9DQpwYXNz"
                         "d29yZDE9DQpwaG9uZTI9DQpsb2dpbjI9DQpwYXNzd29yZDI9DQpw"
                         "aG9uZT";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */

    uint8_t httpbuf3[] = "psb2dpbjM9DQpwYXNzd29yZDM9DQpwaG9uZTQ9DQps"
                         "b2dpbjQ9DQpwYXNzd29yZDQ9DQpwaG9uZTU9DQpsb2dpbjU9DQpw"
                         "YXNzd29yZDU9DQpwaG9uZTY9DQpsb2dpbjY9DQpwYXNzd29yZDY9"
                         "DQpjYWxsX3RpbWUxPQ0KY2FsbF90aW1lMj0NCmRheV9saW1pdD0N"
                         "\r\n\r\n";
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */

    uint8_t httpbuf2[] = "HTTP/1.0 200 OK\r\nServer: VictorServer/1.0\r\n\r\n";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOCLIENT, 100);
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOSERVER, 500);

    ssn.server.ra_raw_base_seq = ssn.server.ra_app_base_seq = 9;
    ssn.server.isn = 9;
    ssn.server.last_ack = 600;
    ssn.client.ra_raw_base_seq = ssn.client.ra_app_base_seq = 9;
    ssn.client.isn = 9;
    ssn.client.last_ack = 600;

    f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 200, 220);
    if (f == NULL)
        goto end;
    f->protoctx = &ssn;
    p->flow = f;

    tcph.th_win = htons(5480);
    tcph.th_seq = htonl(10);
    tcph.th_ack = htonl(20);
    tcph.th_flags = TH_ACK|TH_PUSH;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOCLIENT;

    p->payload = httpbuf2;
    p->payload_len = httplen2;
    ssn.state = TCP_ESTABLISHED;

    TcpStream *s = NULL;
    s = &ssn.server;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet: ");
        goto end;
    }

    /* Check if we have stream smsgs in queue */
    if (ra_ctx->stream_q->len > 0) {
        printf("there shouldn't be any stream smsgs in the queue: ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload = httpbuf1;
    p->payload_len = httplen1;
    tcph.th_seq = htonl(10);
    tcph.th_ack = htonl(55);
    s = &ssn.client;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet: ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload = httpbuf3;
    p->payload_len = httplen3;
    tcph.th_seq = htonl(522);
    tcph.th_ack = htonl(100);
    s = &ssn.client;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet: ");
        goto end;
    }

    /* Check if we have stream smsgs in queue */
    if (ra_ctx->stream_q->len == 0) {
        printf("there should be stream smsgs in the queue: ");
        goto end;
    }

    if (StreamTcpReassembleProcessAppLayer(ra_ctx) < 0) {
        printf("failed in processing stream smsgs: ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOCLIENT;
    p->payload = httpbuf2;
    p->payload_len = httplen2;
    tcph.th_seq = htonl(55);
    tcph.th_ack = htonl(522);
    s = &ssn.server;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet: ");
        goto end;
    }

    /* Check if we have stream smsgs in queue */
    if (ra_ctx->stream_q->len == 0) {
        printf("there should be a stream smsgs in the queue: ");
        goto end;
    /* Process stream smsgs we may have in queue */
    } else if (ra_ctx->stream_q->len > 1) {
        printf("there should be only one stream smsgs in the queue: ");
        goto end;
    } else if (StreamTcpReassembleProcessAppLayer(ra_ctx) < 0) {
        printf("failed in processing stream smsgs: ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOCLIENT;
    p->payload = httpbuf2;
    p->payload_len = httplen2;
    tcph.th_seq = htonl(100);
    tcph.th_ack = htonl(522);
    s = &ssn.server;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet: ");
        goto end;
    }

    /* check if the segment in the list is flagged or not */
    if (ssn.client.seg_list == NULL) {
        printf("segment list should not be empty: ");
        goto end;
    }

    /* last_ack is in the middle of this segment */
    if ((ssn.client.seg_list->flags & SEGMENTTCP_FLAG_APPLAYER_PROCESSED)) {
        printf("segment should not have flags SEGMENTTCP_FLAG_APPLAYER_PROCESSED set: ");
        goto end;
    }

    if (!(ssn.client.seg_list->flags & SEGMENTTCP_FLAG_RAW_PROCESSED)) {
        printf("segment should have flags SEGMENTTCP_FLAG_RAW_PROCESSED set: ");
        goto end;
    }

    ret = 1;
end:
    StreamTcpReassembleFreeThreadCtx(ra_ctx);
    StreamTcpFreeConfig(TRUE);
    SCFree(p);
    UTHFreeFlow(f);
    return ret;
}

/**
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpReassembleTest43 (void) {
    int ret = 0;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;
    Flow *f = NULL;
    TCPHdr tcph;
    TcpSession ssn;

    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));
    memset(&tcph, 0, sizeof (TCPHdr));
    memset(&ssn, 0, sizeof(TcpSession));
    ThreadVars tv;
    memset(&tv, 0, sizeof (ThreadVars));

    StreamTcpInitConfig(TRUE);
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();

    uint8_t httpbuf1[] = "/ HTTP/1.0\r\nUser-Agent: Victor/1.0";

    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */

    uint8_t httpbuf2[] = "HTTP/1.0 200 OK\r\nServer: VictorServer/1.0\r\n\r\n";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    uint8_t httpbuf3[] = "W2dyb3VwMV0NCnBob25lMT1wMDB3ODgyMTMxMzAyMTINCmxvZ2lu"
                         "MT0NCnBhc3N3b3JkMT0NCnBob25lMj1wMDB3ODgyMTMxMzAyMTIN"
                         "CmxvZ2luMj0NCnBhc3N3b3JkMj0NCnBob25lMz0NCmxvZ2luMz0N"
                         "CnBhc3N3b3JkMz0NCnBob25lND0NCmxvZ2luND0NCnBhc3N3b3Jk"
                         "ND0NCnBob25lNT0NCmxvZ2luNT0NCnBhc3N3b3JkNT0NCnBob25l"
                         "Nj0NCmxvZ2luNj0NCnBhc3N3b3JkNj0NCmNhbGxfdGltZTE9MzIN"
                         "CmNhbGxfdGltZTI9MjMyDQpkYXlfbGltaXQ9NQ0KbW9udGhfbGlt"
                         "aXQ9MTUNCltncm91cDJdDQpwaG9uZTE9DQpsb2dpbjE9DQpwYXNz"
                         "d29yZDE9DQpwaG9uZTI9DQpsb2dpbjI9DQpwYXNzd29yZDI9DQpw"
                         "aG9uZT\r\n\r\n";
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */

    ssn.server.ra_raw_base_seq = ssn.server.ra_app_base_seq = 9;
    ssn.server.isn = 9;
    ssn.server.last_ack = 600;
    ssn.client.ra_raw_base_seq = ssn.client.ra_app_base_seq = 9;
    ssn.client.isn = 9;
    ssn.client.last_ack = 600;

    f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 200, 220);
    if (f == NULL)
        goto end;
    f->protoctx = &ssn;
    p->flow = f;

    tcph.th_win = htons(5480);
    tcph.th_seq = htonl(10);
    tcph.th_ack = htonl(10);
    tcph.th_flags = TH_ACK|TH_PUSH;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOCLIENT;

    p->payload = httpbuf2;
    p->payload_len = httplen2;
    ssn.state = TCP_ESTABLISHED;

    TcpStream *s = NULL;
    s = &ssn.server;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (1): ");
        goto end;
    }

    /* Check if we have stream smsgs in queue */
    if (ra_ctx->stream_q->len > 0) {
        printf("there shouldn't be any stream smsgs in the queue (2): ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload = httpbuf1;
    p->payload_len = httplen1;
    tcph.th_seq = htonl(10);
    tcph.th_ack = htonl(55);
    s = &ssn.client;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (3): ");
        goto end;
    }

    /* Check if we have stream smsgs in queue */
    if (ra_ctx->stream_q->len > 0) {
        printf("there shouldn't be any stream smsgs in the queue, as we didn't"
                " processed any smsg from toserver side till yet (4): ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOCLIENT;
    p->payload = httpbuf2;
    p->payload_len = httplen2;
    tcph.th_seq = htonl(55);
    tcph.th_ack = htonl(44);
    s = &ssn.server;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (5): ");
        goto end;
    }
#if 0
    /* Check if we have stream smsgs in queue */
    if (ra_ctx->stream_q->len == 0) {
        printf("there should be a stream smsgs in the queue (6): ");
        goto end;
    /* Process stream smsgs we may have in queue */
    } else if (StreamTcpReassembleProcessAppLayer(ra_ctx) < 0) {
        printf("failed in processing stream smsgs (7): ");
        goto end;
    }
#endif
    if (!(ssn.flags & STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED)) {
        printf("app layer detected flag isn't set, it should be (8): ");
        goto end;
    }

    /* This packets induces a packet gap and also shows why we need to
       process the current segment completely, even if it results in sending more
       than one smsg to the app layer. If we don't send more than one smsg in
       this case, then the first segment of lentgh 34 bytes will be sent to
       app layer and protocol can not be detected in that message and moreover
       the segment lentgh is less than the max. signature size for protocol
       detection, so this will keep looping !! */
    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload = httpbuf3;
    p->payload_len = httplen3;
    tcph.th_seq = htonl(54);
    tcph.th_ack = htonl(100);
    s = &ssn.client;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (9): ");
        goto end;
    }

    /* Check if we have stream smsgs in queue */
    if (ra_ctx->stream_q->len > 0) {
        printf("there shouldn't be any stream smsgs in the queue, as we didn't"
                " detected the app layer protocol till yet (10): ");
        goto end;
    }

    p->flowflags = FLOW_PKT_TOCLIENT;
    p->payload = httpbuf2;
    p->payload_len = httplen2;
    tcph.th_seq = htonl(100);
    tcph.th_ack = htonl(53);
    s = &ssn.server;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet (11): ");
        goto end;
    }
#if 0
    /* Check if we have stream smsgs in queue */
    if (ra_ctx->stream_q->len == 0) {
        printf("there should be a stream smsgs in the queue, as reassembling has"
                " been unpaused now (12): ");
        goto end;
    /* Process stream smsgs we may have in queue */
    } else if (StreamTcpReassembleProcessAppLayer(ra_ctx) < 0) {
        printf("failed in processing stream smsgs (13): ");
        goto end;
    }
#endif
    /* the flag should be set, as the smsg scanned size has crossed the max.
       signature size for app proto detection */
    if (! (ssn.flags & STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED)) {
        printf("app layer detected flag is not set, it should be (14): ");
        goto end;
    }

    ret = 1;
end:
    StreamTcpReassembleFreeThreadCtx(ra_ctx);
    StreamTcpFreeConfig(TRUE);
    SCFree(p);
    UTHFreeFlow(f);
    return ret;
}

/** \test   Test the memcap incrementing/decrementing and memcap check */
static int StreamTcpReassembleTest44(void)
{
    uint8_t ret = 0;
    StreamTcpInitConfig(TRUE);
    uint32_t memuse = SC_ATOMIC_GET(ra_memuse);

    StreamTcpReassembleIncrMemuse(500);
    if (SC_ATOMIC_GET(ra_memuse) != (memuse+500)) {
        printf("failed in incrementing the memory");
        goto end;
    }

    StreamTcpReassembleDecrMemuse(500);
    if (SC_ATOMIC_GET(ra_memuse) != memuse) {
        printf("failed in decrementing the memory");
        goto end;
    }

    if (StreamTcpReassembleCheckMemcap(500) != 1) {
        printf("failed in validating the memcap");
        goto end;
    }

    if (StreamTcpReassembleCheckMemcap((memuse + stream_config.reassembly_memcap)) != 0) {
        printf("failed in validating the memcap");
        goto end;
    }

    StreamTcpFreeConfig(TRUE);

    if (SC_ATOMIC_GET(ra_memuse) != 0) {
        printf("failed in clearing the memory");
        goto end;
    }

    ret = 1;
    return ret;
end:
    StreamTcpFreeConfig(TRUE);
    return ret;
}

/**
 *  \test   Test to make sure that reassembly_depth is enforced.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpReassembleTest45 (void) {
    int ret = 0;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;
    Flow *f = NULL;
    TCPHdr tcph;
    TcpSession ssn;

    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));
    memset(&tcph, 0, sizeof (TCPHdr));
    memset(&ssn, 0, sizeof(TcpSession));
    ThreadVars tv;
    memset(&tv, 0, sizeof (ThreadVars));

    uint8_t httpbuf1[] = "/ HTTP/1.0\r\nUser-Agent: Victor/1.0";

    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */

    StreamTcpInitConfig(TRUE);
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();

    STREAMTCP_SET_RA_BASE_SEQ(&ssn.server, 9);
    ssn.server.isn = 9;
    ssn.server.last_ack = 60;
    STREAMTCP_SET_RA_BASE_SEQ(&ssn.client, 9);
    ssn.client.isn = 9;
    ssn.client.last_ack = 60;

    f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 200, 220);
    if (f == NULL)
        goto end;
    f->protoctx = &ssn;
    p->flow = f;

    tcph.th_win = htons(5480);
    tcph.th_seq = htonl(10);
    tcph.th_ack = htonl(20);
    tcph.th_flags = TH_ACK|TH_PUSH;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOCLIENT;

    p->payload = httpbuf1;
    p->payload_len = httplen1;
    ssn.state = TCP_ESTABLISHED;

    /* set the default value of reassembly depth, as there is no config file */
    stream_config.reassembly_depth = httplen1 + 1;

    TcpStream *s = NULL;
    s = &ssn.server;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toclient packet: ");
        goto end;
    }

    /* Check if we have flags set or not */
    if (s->flags & STREAMTCP_STREAM_FLAG_NOREASSEMBLY) {
        printf("there shouldn't be a noreassembly flag be set: ");
        goto end;
    }
    STREAMTCP_SET_RA_BASE_SEQ(&ssn.server, ssn.server.isn + httplen1);

    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload_len = httplen1;
    s = &ssn.client;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet: ");
        goto end;
    }

    /* Check if we have flags set or not */
    if (s->flags & STREAMTCP_STREAM_FLAG_NOREASSEMBLY) {
        printf("there shouldn't be a noreassembly flag be set: ");
        goto end;
    }
    STREAMTCP_SET_RA_BASE_SEQ(&ssn.client, ssn.client.isn + httplen1);

    p->flowflags = FLOW_PKT_TOCLIENT;
    p->payload_len = httplen1;
    s = &ssn.server;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet: ");
        goto end;
    }

    /* Check if we have flags set or not */
    if (!(s->flags & STREAMTCP_STREAM_FLAG_NOREASSEMBLY)) {
        printf("the noreassembly flags should be set, "
                "p.payload_len %"PRIu16" stream_config.reassembly_"
                "depth %"PRIu32": ", p->payload_len,
                stream_config.reassembly_depth);
        goto end;
    }

    ret = 1;
end:
    StreamTcpReassembleFreeThreadCtx(ra_ctx);
    StreamTcpFreeConfig(TRUE);
    SCFree(p);
    UTHFreeFlow(f);
    return ret;
}

/**
 *  \test   Test the undefined config value of reassembly depth.
 *          the default value of 0 will be loaded and stream will be reassembled
 *          until the session ended
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpReassembleTest46 (void) {
    int ret = 0;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;
    Flow *f = NULL;
    TCPHdr tcph;
    TcpSession ssn;
    ThreadVars tv;

    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));
    memset(&tcph, 0, sizeof (TCPHdr));
    memset(&ssn, 0, sizeof(TcpSession));
    memset(&tv, 0, sizeof (ThreadVars));

    uint8_t httpbuf1[] = "/ HTTP/1.0\r\nUser-Agent: Victor/1.0";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */

    StreamTcpInitConfig(TRUE);
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();

    STREAMTCP_SET_RA_BASE_SEQ(&ssn.server, 9);
    ssn.server.isn = 9;
    ssn.server.last_ack = 60;
    ssn.server.next_seq = ssn.server.isn;
    STREAMTCP_SET_RA_BASE_SEQ(&ssn.client, 9);
    ssn.client.isn = 9;
    ssn.client.last_ack = 60;
    ssn.client.next_seq = ssn.client.isn;

    f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 200, 220);
    if (f == NULL)
        goto end;
    f->protoctx = &ssn;
    p->flow = f;

    tcph.th_win = htons(5480);
    tcph.th_seq = htonl(10);
    tcph.th_ack = htonl(20);
    tcph.th_flags = TH_ACK|TH_PUSH;
    p->tcph = &tcph;
    p->flowflags = FLOW_PKT_TOCLIENT;

    p->payload = httpbuf1;
    p->payload_len = httplen1;
    ssn.state = TCP_ESTABLISHED;

    stream_config.reassembly_depth = 0;

    TcpStream *s = NULL;
    s = &ssn.server;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toclient packet\n");
        goto end;
    }

    /* Check if we have flags set or not */
    if ((ssn.client.flags & STREAMTCP_STREAM_FLAG_NOREASSEMBLY) ||
        (ssn.server.flags & STREAMTCP_STREAM_FLAG_NOREASSEMBLY)) {
        printf("there shouldn't be any no reassembly flag be set \n");
        goto end;
    }
    STREAMTCP_SET_RA_BASE_SEQ(&ssn.server, ssn.server.isn + httplen1);

    p->flowflags = FLOW_PKT_TOSERVER;
    p->payload_len = httplen1;
    s = &ssn.client;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet\n");
        goto end;
    }

    /* Check if we have flags set or not */
    if ((ssn.client.flags & STREAMTCP_STREAM_FLAG_NOREASSEMBLY) ||
        (ssn.server.flags & STREAMTCP_STREAM_FLAG_NOREASSEMBLY)) {
        printf("there shouldn't be any no reassembly flag be set \n");
        goto end;
    }
    STREAMTCP_SET_RA_BASE_SEQ(&ssn.client, ssn.client.isn + httplen1);

    p->flowflags = FLOW_PKT_TOCLIENT;
    p->payload_len = httplen1;
    tcph.th_seq = htonl(10 + httplen1);
    tcph.th_ack = htonl(20 + httplen1);
    s = &ssn.server;

    if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
        printf("failed in segments reassembly, while processing toserver packet\n");
        goto end;
    }

    /* Check if we have flags set or not */
    if ((ssn.client.flags & STREAMTCP_STREAM_FLAG_NOREASSEMBLY) ||
        (ssn.server.flags & STREAMTCP_STREAM_FLAG_NOREASSEMBLY)) {
        printf("the no_reassembly flags should not be set, "
                "p->payload_len %"PRIu16" stream_config.reassembly_"
                "depth %"PRIu32": ", p->payload_len,
                stream_config.reassembly_depth);
        goto end;
    }

    ret = 1;
end:
    StreamTcpReassembleFreeThreadCtx(ra_ctx);
    StreamTcpFreeConfig(TRUE);
    SCFree(p);
    UTHFreeFlow(f);
    return ret;
}

/**
 *  \test   Test to make sure we detect the sequence wrap around and continue
 *          stream reassembly properly.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpReassembleTest47 (void) {
    int ret = 0;
    Packet *p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;
    Flow *f = NULL;
    TCPHdr tcph;
    TcpSession ssn;
    ThreadVars tv;

    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    PacketQueue pq;
    memset(&pq,0,sizeof(PacketQueue));
    memset(&tcph, 0, sizeof (TCPHdr));
    memset(&ssn, 0, sizeof(TcpSession));
    memset(&tv, 0, sizeof (ThreadVars));

    /* prevent L7 from kicking in */
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOSERVER, 0);
    StreamMsgQueueSetMinChunkLen(FLOW_PKT_TOCLIENT, 0);

    StreamTcpInitConfig(TRUE);
    TcpReassemblyThreadCtx *ra_ctx = StreamTcpReassembleInitThreadCtx();

    uint8_t httpbuf1[] = "GET /EVILSUFF HTTP/1.1\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */

    ssn.server.ra_raw_base_seq = ssn.server.ra_app_base_seq = 572799781UL;
    ssn.server.isn = 572799781UL;
    ssn.server.last_ack = 572799782UL;
    ssn.client.ra_raw_base_seq = ssn.client.ra_app_base_seq = 4294967289UL;
    ssn.client.isn = 4294967289UL;
    ssn.client.last_ack = 21;

    f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 200, 220);
    if (f == NULL)
        goto end;
    f->protoctx = &ssn;
    p->flow = f;

    tcph.th_win = htons(5480);
    ssn.state = TCP_ESTABLISHED;
    TcpStream *s = NULL;
    uint8_t cnt = 0;

    for (cnt=0; cnt < httplen1; cnt++) {
        tcph.th_seq = htonl(ssn.client.isn + 1 + cnt);
        tcph.th_ack = htonl(572799782UL);
        tcph.th_flags = TH_ACK|TH_PUSH;
        p->tcph = &tcph;
        p->flowflags = FLOW_PKT_TOSERVER;
        p->payload = &httpbuf1[cnt];
        p->payload_len = 1;
        s = &ssn.client;

        if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
            printf("failed in segments reassembly, while processing toserver "
                    "packet\n");
            goto end;
        }

        p->flowflags = FLOW_PKT_TOCLIENT;
        p->payload = NULL;
        p->payload_len = 0;
        tcph.th_seq = htonl(572799782UL);
        tcph.th_ack = htonl(ssn.client.isn + 1 + cnt);
        tcph.th_flags = TH_ACK;
        p->tcph = &tcph;
        s = &ssn.server;

        if (StreamTcpReassembleHandleSegment(&tv, ra_ctx, &ssn, s, p, &pq) == -1) {
            printf("failed in segments reassembly, while processing toserver "
                    "packet\n");
            goto end;
        }

        /* Process stream smsgs we may have in queue */
        if (StreamTcpReassembleProcessAppLayer(ra_ctx) < 0) {
            printf("failed in processing stream smsgs\n");
            goto end;
        }
    }

    if (f->alproto != ALPROTO_HTTP) {
        printf("App layer protocol (HTTP) should have been detected\n");
        goto end;
    }

    ret = 1;
end:
    StreamTcpReassembleFreeThreadCtx(ra_ctx);
    StreamTcpFreeConfig(TRUE);
    SCFree(p);
    UTHFreeFlow(f);
    return ret;
}

/** \test 3 in order segments in inline reassembly */
static int StreamTcpReassembleInlineTest01(void) {
    int ret = 0;
    TcpReassemblyThreadCtx *ra_ctx = NULL;
    ThreadVars tv;
    TcpSession ssn;
    Flow f;

    memset(&tv, 0x00, sizeof(tv));

    StreamTcpUTInit(&ra_ctx);
    StreamTcpUTSetupSession(&ssn);
    StreamTcpUTSetupStream(&ssn.client, 1);
    FLOW_INITIALIZE(&f);

    uint8_t stream_payload[] = "AAAAABBBBBCCCCC";
    uint8_t payload[] = { 'C', 'C', 'C', 'C', 'C' };
    Packet *p = UTHBuildPacketReal(payload, 5, IPPROTO_TCP, "1.1.1.1", "2.2.2.2", 1024, 80);
    if (p == NULL) {
        printf("couldn't get a packet: ");
        goto end;
    }
    p->tcph->th_seq = htonl(12);
    p->flow = &f;

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  2, 'A', 5) == -1) {
        printf("failed to add segment 1: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  7, 'B', 5) == -1) {
        printf("failed to add segment 2: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 12, 'C', 5) == -1) {
        printf("failed to add segment 3: ");
        goto end;
    }
    ssn.client.next_seq = 17;

    int r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 1) {
        printf("expected a single stream message, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    StreamMsg *smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 15) {
        printf("expected data length to be 15, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload, smsg->data.data, 15) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload, 15);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    ret = 1;
end:
    FLOW_DESTROY(&f);
    UTHFreePacket(p);
    StreamTcpUTClearSession(&ssn);
    StreamTcpUTDeinit(ra_ctx);
    return ret;
}

/** \test 3 in order segments, then reassemble, add one more and reassemble again.
 *        test the sliding window reassembly.
 */
static int StreamTcpReassembleInlineTest02(void) {
    int ret = 0;
    TcpReassemblyThreadCtx *ra_ctx = NULL;
    ThreadVars tv;
    TcpSession ssn;
    Flow f;

    memset(&tv, 0x00, sizeof(tv));

    StreamTcpUTInit(&ra_ctx);
    StreamTcpUTSetupSession(&ssn);
    StreamTcpUTSetupStream(&ssn.client, 1);
    FLOW_INITIALIZE(&f);

    uint8_t stream_payload1[] = "AAAAABBBBBCCCCC";
    uint8_t stream_payload2[] = "AAAAABBBBBCCCCCDDDDD";
    uint8_t payload[] = { 'C', 'C', 'C', 'C', 'C' };
    Packet *p = UTHBuildPacketReal(payload, 5, IPPROTO_TCP, "1.1.1.1", "2.2.2.2", 1024, 80);
    if (p == NULL) {
        printf("couldn't get a packet: ");
        goto end;
    }
    p->tcph->th_seq = htonl(12);
    p->flow = &f;

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  2, 'A', 5) == -1) {
        printf("failed to add segment 1: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  7, 'B', 5) == -1) {
        printf("failed to add segment 2: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 12, 'C', 5) == -1) {
        printf("failed to add segment 3: ");
        goto end;
    }
    ssn.client.next_seq = 17;

    int r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 1) {
        printf("expected a single stream message, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    StreamMsg *smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 15) {
        printf("expected data length to be 15, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload1, smsg->data.data, 15) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload1, 15);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 17, 'D', 5) == -1) {
        printf("failed to add segment 4: ");
        goto end;
    }
    ssn.client.next_seq = 22;

    r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed 2: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 2) {
        printf("expected a single stream message, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 20) {
        printf("expected data length to be 20, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload2, smsg->data.data, 20) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload2, 20);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    ret = 1;
end:
    FLOW_DESTROY(&f);
    UTHFreePacket(p);
    StreamTcpUTClearSession(&ssn);
    StreamTcpUTDeinit(ra_ctx);
    return ret;
}

/** \test 3 in order segments, then reassemble, add one more and reassemble again.
 *        test the sliding window reassembly with a small window size so that we
 *        cutting off at the start (left edge)
 */
static int StreamTcpReassembleInlineTest03(void) {
    int ret = 0;
    TcpReassemblyThreadCtx *ra_ctx = NULL;
    ThreadVars tv;
    TcpSession ssn;
    Flow f;

    memset(&tv, 0x00, sizeof(tv));

    StreamTcpUTInit(&ra_ctx);
    StreamTcpUTSetupSession(&ssn);
    StreamTcpUTSetupStream(&ssn.client, 1);
    FLOW_INITIALIZE(&f);

    stream_config.reassembly_toserver_chunk_size = 15;

    uint8_t stream_payload1[] = "AAAAABBBBBCCCCC";
    uint8_t stream_payload2[] = "BBBBBCCCCCDDDDD";
    uint8_t payload[] = { 'C', 'C', 'C', 'C', 'C' };
    Packet *p = UTHBuildPacketReal(payload, 5, IPPROTO_TCP, "1.1.1.1", "2.2.2.2", 1024, 80);
    if (p == NULL) {
        printf("couldn't get a packet: ");
        goto end;
    }
    p->tcph->th_seq = htonl(12);
    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  2, 'A', 5) == -1) {
        printf("failed to add segment 1: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  7, 'B', 5) == -1) {
        printf("failed to add segment 2: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 12, 'C', 5) == -1) {
        printf("failed to add segment 3: ");
        goto end;
    }
    ssn.client.next_seq = 17;

    int r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 1) {
        printf("expected a single stream message, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    StreamMsg *smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 15) {
        printf("expected data length to be 15, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload1, smsg->data.data, 15) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload1, 15);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 17, 'D', 5) == -1) {
        printf("failed to add segment 4: ");
        goto end;
    }
    ssn.client.next_seq = 22;

    p->tcph->th_seq = htonl(17);

    r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed 2: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 2) {
        printf("expected a single stream message, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 15) {
        printf("expected data length to be 15, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload2, smsg->data.data, 15) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload2, 15);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    ret = 1;
end:
    FLOW_DESTROY(&f);
    UTHFreePacket(p);
    StreamTcpUTClearSession(&ssn);
    StreamTcpUTDeinit(ra_ctx);
    return ret;
}

/** \test 3 in order segments, then reassemble, add one more and reassemble again.
 *        test the sliding window reassembly with a small window size so that we
 *        cutting off at the start (left edge) with small packet overlap.
 */
static int StreamTcpReassembleInlineTest04(void) {
    int ret = 0;
    TcpReassemblyThreadCtx *ra_ctx = NULL;
    ThreadVars tv;
    TcpSession ssn;
    Flow f;

    memset(&tv, 0x00, sizeof(tv));

    StreamTcpUTInit(&ra_ctx);
    StreamTcpUTSetupSession(&ssn);
    StreamTcpUTSetupStream(&ssn.client, 1);
    FLOW_INITIALIZE(&f);

    stream_config.reassembly_toserver_chunk_size = 16;

    uint8_t stream_payload1[] = "AAAAABBBBBCCCCC";
    uint8_t stream_payload2[] = "ABBBBBCCCCCDDDDD";
    uint8_t payload[] = { 'C', 'C', 'C', 'C', 'C' };
    Packet *p = UTHBuildPacketReal(payload, 5, IPPROTO_TCP, "1.1.1.1", "2.2.2.2", 1024, 80);
    if (p == NULL) {
        printf("couldn't get a packet: ");
        goto end;
    }
    p->tcph->th_seq = htonl(12);
    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  2, 'A', 5) == -1) {
        printf("failed to add segment 1: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  7, 'B', 5) == -1) {
        printf("failed to add segment 2: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 12, 'C', 5) == -1) {
        printf("failed to add segment 3: ");
        goto end;
    }
    ssn.client.next_seq = 17;

    int r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 1) {
        printf("expected a single stream message, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    StreamMsg *smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 15) {
        printf("expected data length to be 15, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload1, smsg->data.data, 15) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload1, 15);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 17, 'D', 5) == -1) {
        printf("failed to add segment 4: ");
        goto end;
    }
    ssn.client.next_seq = 22;

    p->tcph->th_seq = htonl(17);

    r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed 2: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 2) {
        printf("expected a single stream message, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 16) {
        printf("expected data length to be 16, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload2, smsg->data.data, 16) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload2, 16);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    ret = 1;
end:
    FLOW_DESTROY(&f);
    UTHFreePacket(p);
    StreamTcpUTClearSession(&ssn);
    StreamTcpUTDeinit(ra_ctx);
    return ret;
}

/** \test with a GAP we should have 2 smsgs */
static int StreamTcpReassembleInlineTest05(void) {
    int ret = 0;
    TcpReassemblyThreadCtx *ra_ctx = NULL;
    ThreadVars tv;
    TcpSession ssn;
    Flow f;

    memset(&tv, 0x00, sizeof(tv));

    StreamTcpUTInit(&ra_ctx);
    StreamTcpUTSetupSession(&ssn);
    StreamTcpUTSetupStream(&ssn.client, 1);
    FLOW_INITIALIZE(&f);

    uint8_t stream_payload1[] = "AAAAABBBBB";
    uint8_t stream_payload2[] = "DDDDD";
    uint8_t payload[] = { 'C', 'C', 'C', 'C', 'C' };
    Packet *p = UTHBuildPacketReal(payload, 5, IPPROTO_TCP, "1.1.1.1", "2.2.2.2", 1024, 80);
    if (p == NULL) {
        printf("couldn't get a packet: ");
        goto end;
    }
    p->tcph->th_seq = htonl(12);
    p->flow = &f;

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  2, 'A', 5) == -1) {
        printf("failed to add segment 1: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  7, 'B', 5) == -1) {
        printf("failed to add segment 2: ");
        goto end;
    }
    ssn.client.next_seq = 12;

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 17, 'D', 5) == -1) {
        printf("failed to add segment 4: ");
        goto end;
    }

    p->tcph->th_seq = htonl(17);

    int r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 2) {
        printf("expected a single stream message, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    StreamMsg *smsg = ra_ctx->stream_q->top->next;
    if (smsg->data.data_len != 10) {
        printf("expected data length to be 10, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload1, smsg->data.data, 10) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload2, 10);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 5) {
        printf("expected data length to be 5, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload2, smsg->data.data, 5) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload2, 5);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    ret = 1;
end:
    FLOW_DESTROY(&f);
    UTHFreePacket(p);
    StreamTcpUTClearSession(&ssn);
    StreamTcpUTDeinit(ra_ctx);
    return ret;
}

/** \test with a GAP we should have 2 smsgs, with filling the GAP later */
static int StreamTcpReassembleInlineTest06(void) {
    int ret = 0;
    TcpReassemblyThreadCtx *ra_ctx = NULL;
    ThreadVars tv;
    TcpSession ssn;
    Flow f;

    memset(&tv, 0x00, sizeof(tv));

    StreamTcpUTInit(&ra_ctx);
    StreamTcpUTSetupSession(&ssn);
    StreamTcpUTSetupStream(&ssn.client, 1);
    FLOW_INITIALIZE(&f);

    uint8_t stream_payload1[] = "AAAAABBBBB";
    uint8_t stream_payload2[] = "DDDDD";
    uint8_t stream_payload3[] = "AAAAABBBBBCCCCCDDDDD";
    uint8_t payload[] = { 'C', 'C', 'C', 'C', 'C' };
    Packet *p = UTHBuildPacketReal(payload, 5, IPPROTO_TCP, "1.1.1.1", "2.2.2.2", 1024, 80);
    if (p == NULL) {
        printf("couldn't get a packet: ");
        goto end;
    }
    p->tcph->th_seq = htonl(12);
    p->flow = &f;

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  2, 'A', 5) == -1) {
        printf("failed to add segment 1: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  7, 'B', 5) == -1) {
        printf("failed to add segment 2: ");
        goto end;
    }
    ssn.client.next_seq = 12;

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 17, 'D', 5) == -1) {
        printf("failed to add segment 4: ");
        goto end;
    }

    p->tcph->th_seq = htonl(17);

    int r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 2) {
        printf("expected a single stream message, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    StreamMsg *smsg = ra_ctx->stream_q->top->next;
    if (smsg->data.data_len != 10) {
        printf("expected data length to be 10, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload1, smsg->data.data, 10) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload2, 10);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 5) {
        printf("expected data length to be 5, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload2, smsg->data.data, 5) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload2, 5);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 12, 'C', 5) == -1) {
        printf("failed to add segment 3: ");
        goto end;
    }
    ssn.client.next_seq = 22;

    p->tcph->th_seq = htonl(12);

    r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 3) {
        printf("expected a single stream message, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 20) {
        printf("expected data length to be 20, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload3, smsg->data.data, 20) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload3, 20);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    ret = 1;
end:
    FLOW_DESTROY(&f);
    UTHFreePacket(p);
    StreamTcpUTClearSession(&ssn);
    StreamTcpUTDeinit(ra_ctx);
    return ret;
}

/** \test with a GAP we should have 2 smsgs, with filling the GAP later, small
 *        window */
static int StreamTcpReassembleInlineTest07(void) {
    int ret = 0;
    TcpReassemblyThreadCtx *ra_ctx = NULL;
    ThreadVars tv;
    TcpSession ssn;
    Flow f;

    memset(&tv, 0x00, sizeof(tv));

    StreamTcpUTInit(&ra_ctx);
    StreamTcpUTSetupSession(&ssn);
    StreamTcpUTSetupStream(&ssn.client, 1);
    FLOW_INITIALIZE(&f);

    stream_config.reassembly_toserver_chunk_size = 16;

    uint8_t stream_payload1[] = "ABBBBB";
    uint8_t stream_payload2[] = "DDDDD";
    uint8_t stream_payload3[] = "AAAAABBBBBCCCCCD";
    uint8_t payload[] = { 'C', 'C', 'C', 'C', 'C' };
    Packet *p = UTHBuildPacketReal(payload, 5, IPPROTO_TCP, "1.1.1.1", "2.2.2.2", 1024, 80);
    if (p == NULL) {
        printf("couldn't get a packet: ");
        goto end;
    }
    p->tcph->th_seq = htonl(12);
    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  2, 'A', 5) == -1) {
        printf("failed to add segment 1: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  7, 'B', 5) == -1) {
        printf("failed to add segment 2: ");
        goto end;
    }
    ssn.client.next_seq = 12;

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 17, 'D', 5) == -1) {
        printf("failed to add segment 4: ");
        goto end;
    }

    p->tcph->th_seq = htonl(17);

    int r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 2) {
        printf("expected a single stream message, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    StreamMsg *smsg = ra_ctx->stream_q->top->next;
    if (smsg->data.data_len != 6) {
        printf("expected data length to be 6, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload1, smsg->data.data, 6) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload1, 6);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 5) {
        printf("expected data length to be 5, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload2, smsg->data.data, 5) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload2, 5);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 12, 'C', 5) == -1) {
        printf("failed to add segment 3: ");
        goto end;
    }
    ssn.client.next_seq = 22;

    p->tcph->th_seq = htonl(12);

    r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 3) {
        printf("expected a single stream message, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 16) {
        printf("expected data length to be 16, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload3, smsg->data.data, 16) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload3, 16);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    ret = 1;
end:
    FLOW_DESTROY(&f);
    UTHFreePacket(p);
    StreamTcpUTClearSession(&ssn);
    StreamTcpUTDeinit(ra_ctx);
    return ret;
}

/** \test 3 in order segments, then reassemble, add one more and reassemble again.
 *        test the sliding window reassembly with a small window size so that we
 *        cutting off at the start (left edge). Test if the first segment is
 *        removed from the list.
 */
static int StreamTcpReassembleInlineTest08(void) {
    int ret = 0;
    TcpReassemblyThreadCtx *ra_ctx = NULL;
    ThreadVars tv;
    TcpSession ssn;
    Flow f;

    memset(&tv, 0x00, sizeof(tv));

    StreamTcpUTInit(&ra_ctx);
    StreamTcpUTSetupSession(&ssn);
    StreamTcpUTSetupStream(&ssn.client, 1);
    FLOW_INITIALIZE(&f);

    stream_config.reassembly_toserver_chunk_size = 15;
    ssn.client.flags |= STREAMTCP_STREAM_FLAG_GAP;

    uint8_t stream_payload1[] = "AAAAABBBBBCCCCC";
    uint8_t stream_payload2[] = "BBBBBCCCCCDDDDD";
    uint8_t payload[] = { 'C', 'C', 'C', 'C', 'C' };
    Packet *p = UTHBuildPacketReal(payload, 5, IPPROTO_TCP, "1.1.1.1", "2.2.2.2", 1024, 80);
    if (p == NULL) {
        printf("couldn't get a packet: ");
        goto end;
    }
    p->tcph->th_seq = htonl(12);
    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  2, 'A', 5) == -1) {
        printf("failed to add segment 1: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  7, 'B', 5) == -1) {
        printf("failed to add segment 2: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 12, 'C', 5) == -1) {
        printf("failed to add segment 3: ");
        goto end;
    }
    ssn.client.next_seq = 17;

    int r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 1) {
        printf("expected a single stream message, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    StreamMsg *smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 15) {
        printf("expected data length to be 15, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload1, smsg->data.data, 15) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload1, 15);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    if (ssn.client.ra_raw_base_seq != 16) {
        printf("ra_raw_base_seq %"PRIu32", expected 16: ", ssn.client.ra_raw_base_seq);
        goto end;
    }

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 17, 'D', 5) == -1) {
        printf("failed to add segment 4: ");
        goto end;
    }
    ssn.client.next_seq = 22;

    p->tcph->th_seq = htonl(17);

    r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed 2: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 2) {
        printf("expected a single stream message, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 15) {
        printf("expected data length to be 15, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload2, smsg->data.data, 15) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload2, 15);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    if (ssn.client.ra_raw_base_seq != 21) {
        printf("ra_raw_base_seq %"PRIu32", expected 21: ", ssn.client.ra_raw_base_seq);
        goto end;
    }

    if (ssn.client.seg_list->seq != 7) {
        printf("expected segment 2 (seq 7) to be first in the list, got seq %"PRIu32": ", ssn.client.seg_list->seq);
        goto end;
    }

    ret = 1;
end:
    FLOW_DESTROY(&f);
    UTHFreePacket(p);
    StreamTcpUTClearSession(&ssn);
    StreamTcpUTDeinit(ra_ctx);
    return ret;
}

/** \test 3 in order segments, then reassemble, add one more and reassemble again.
 *        test the sliding window reassembly with a small window size so that we
 *        cutting off at the start (left edge). Test if the first segment is
 *        removed from the list.
 */
static int StreamTcpReassembleInlineTest09(void) {
    int ret = 0;
    TcpReassemblyThreadCtx *ra_ctx = NULL;
    ThreadVars tv;
    TcpSession ssn;
    Flow f;

    memset(&tv, 0x00, sizeof(tv));

    StreamTcpUTInit(&ra_ctx);
    StreamTcpUTSetupSession(&ssn);
    StreamTcpUTSetupStream(&ssn.client, 1);
    FLOW_INITIALIZE(&f);

    stream_config.reassembly_toserver_chunk_size = 20;
    ssn.client.flags |= STREAMTCP_STREAM_FLAG_GAP;

    uint8_t stream_payload1[] = "AAAAABBBBBCCCCC";
    uint8_t stream_payload2[] = "DDDDD";
    uint8_t stream_payload3[] = "AAAAABBBBBCCCCCDDDDD";
    uint8_t payload[] = { 'C', 'C', 'C', 'C', 'C' };
    Packet *p = UTHBuildPacketReal(payload, 5, IPPROTO_TCP, "1.1.1.1", "2.2.2.2", 1024, 80);
    if (p == NULL) {
        printf("couldn't get a packet: ");
        goto end;
    }
    p->tcph->th_seq = htonl(17);
    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  2, 'A', 5) == -1) {
        printf("failed to add segment 1: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  7, 'B', 5) == -1) {
        printf("failed to add segment 2: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 17, 'D', 5) == -1) {
        printf("failed to add segment 3: ");
        goto end;
    }
    ssn.client.next_seq = 12;

    int r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 2) {
        printf("expected 2 stream message2, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    StreamMsg *smsg = ra_ctx->stream_q->bot;
    if (smsg->data.data_len != 10) {
        printf("expected data length to be 10, got %u (bot): ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload1, smsg->data.data, 10) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload1, 10);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 5) {
        printf("expected data length to be 5, got %u (top): ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload2, smsg->data.data, 5) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload2, 5);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    if (ssn.client.ra_raw_base_seq != 11) {
        printf("ra_raw_base_seq %"PRIu32", expected 11: ", ssn.client.ra_raw_base_seq);
        goto end;
    }

    /* close the GAP and see if we properly reassemble and update ra_base_seq */
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 12, 'C', 5) == -1) {
        printf("failed to add segment 4: ");
        goto end;
    }
    ssn.client.next_seq = 22;

    p->tcph->th_seq = htonl(12);

    r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed 2: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 3) {
        printf("expected 3 stream messages, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 20) {
        printf("expected data length to be 20, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload3, smsg->data.data, 20) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload3, 20);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    if (ssn.client.ra_raw_base_seq != 21) {
        printf("ra_raw_base_seq %"PRIu32", expected 21: ", ssn.client.ra_raw_base_seq);
        goto end;
    }

    if (ssn.client.seg_list->seq != 2) {
        printf("expected segment 1 (seq 2) to be first in the list, got seq %"PRIu32": ", ssn.client.seg_list->seq);
        goto end;
    }

    ret = 1;
end:
    FLOW_DESTROY(&f);
    UTHFreePacket(p);
    StreamTcpUTClearSession(&ssn);
    StreamTcpUTDeinit(ra_ctx);
    return ret;
}

/** \test App Layer reassembly.
 */
static int StreamTcpReassembleInlineTest10(void) {
    int ret = 0;
    TcpReassemblyThreadCtx *ra_ctx = NULL;
    ThreadVars tv;
    TcpSession ssn;
    Flow *f = NULL;

    memset(&tv, 0x00, sizeof(tv));

    StreamTcpUTInit(&ra_ctx);
    StreamTcpUTSetupSession(&ssn);
    StreamTcpUTSetupStream(&ssn.server, 1);

    f = UTHBuildFlow(AF_INET, "1.1.1.1", "2.2.2.2", 1024, 80);
    if (f == NULL)
        goto end;
    f->protoctx = &ssn;

    uint8_t stream_payload1[] = "GE";
    uint8_t stream_payload2[] = "T /";
    uint8_t stream_payload3[] = "HTTP/1.0\r\n\r\n";

    Packet *p = UTHBuildPacketReal(stream_payload3, 12, IPPROTO_TCP, "1.1.1.1", "2.2.2.2", 1024, 80);
    if (p == NULL) {
        printf("couldn't get a packet: ");
        goto end;
    }
    p->tcph->th_seq = htonl(7);
    p->flow = f;
    p->flowflags |= FLOW_PKT_TOSERVER;

    if (StreamTcpUTAddSegmentWithPayload(&tv, ra_ctx, &ssn.server,  2, stream_payload1, 2) == -1) {
        printf("failed to add segment 1: ");
        goto end;
    }
    ssn.server.next_seq = 4;

    int r = StreamTcpReassembleInlineAppLayer(&tv, ra_ctx, &ssn, &ssn.server, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineAppLayer failed: ");
        goto end;
    }

    /* ssn.server.ra_app_base_seq should be isn here. */
    if (ssn.server.ra_app_base_seq != 1 || ssn.server.ra_app_base_seq != ssn.server.isn) {
        printf("expected ra_app_base_seq 1, got %u: ", ssn.server.ra_app_base_seq);
        goto end;
    }

    if (StreamTcpUTAddSegmentWithPayload(&tv, ra_ctx, &ssn.server,  4, stream_payload2, 3) == -1) {
        printf("failed to add segment 2: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithPayload(&tv, ra_ctx, &ssn.server,  7, stream_payload3, 12) == -1) {
        printf("failed to add segment 3: ");
        goto end;
    }
    ssn.server.next_seq = 19;

    r = StreamTcpReassembleInlineAppLayer(&tv, ra_ctx, &ssn, &ssn.server, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineAppLayer failed: ");
        goto end;
    }

    if (ssn.server.ra_app_base_seq != 18) {
        printf("expected ra_app_base_seq 18, got %u: ", ssn.server.ra_app_base_seq);
        goto end;
    }

    ret = 1;
end:
#if 0
    FLOW_DESTROY(&f);
    UTHFreePacket(p);
    StreamTcpUTClearSession(&ssn);
    StreamTcpUTDeinit(ra_ctx);
#endif
    UTHFreeFlow(f);
    return ret;
}

/** \test test insert with overlap
 */
static int StreamTcpReassembleInsertTest01(void) {
    int ret = 0;
    TcpReassemblyThreadCtx *ra_ctx = NULL;
    ThreadVars tv;
    TcpSession ssn;
    Flow f;

    memset(&tv, 0x00, sizeof(tv));

    StreamTcpUTInit(&ra_ctx);
    StreamTcpUTSetupSession(&ssn);
    StreamTcpUTSetupStream(&ssn.client, 1);
    FLOW_INITIALIZE(&f);

    uint8_t stream_payload1[] = "AAAAABBBBBCCCCCDDDDD";
    uint8_t payload[] = { 'C', 'C', 'C', 'C', 'C' };
    Packet *p = UTHBuildPacketReal(payload, 5, IPPROTO_TCP, "1.1.1.1", "2.2.2.2", 1024, 80);
    if (p == NULL) {
        printf("couldn't get a packet: ");
        goto end;
    }
    p->tcph->th_seq = htonl(12);
    p->flow = &f;

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  2, 'A', 5) == -1) {
        printf("failed to add segment 1: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  7, 'B', 5) == -1) {
        printf("failed to add segment 2: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 14, 'D', 2) == -1) {
        printf("failed to add segment 3: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 16, 'D', 6) == -1) {
        printf("failed to add segment 4: ");
        goto end;
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client, 12, 'C', 5) == -1) {
        printf("failed to add segment 5: ");
        goto end;
    }
    ssn.client.next_seq = 21;

    int r = StreamTcpReassembleInlineRaw(ra_ctx, &ssn, &ssn.client, p);
    if (r < 0) {
        printf("StreamTcpReassembleInlineRaw failed: ");
        goto end;
    }

    if (ra_ctx->stream_q->len != 1) {
        printf("expected a single stream message, got %u: ", ra_ctx->stream_q->len);
        goto end;
    }

    StreamMsg *smsg = ra_ctx->stream_q->top;
    if (smsg->data.data_len != 20) {
        printf("expected data length to be 20, got %u: ", smsg->data.data_len);
        goto end;
    }

    if (!(memcmp(stream_payload1, smsg->data.data, 20) == 0)) {
        printf("data is not what we expected:\nExpected:\n");
        PrintRawDataFp(stdout, stream_payload1, 20);
        printf("Got:\n");
        PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
        goto end;
    }

    if (ssn.client.ra_raw_base_seq != 21) {
        printf("ra_raw_base_seq %"PRIu32", expected 21: ", ssn.client.ra_raw_base_seq);
        goto end;
    }
    ret = 1;
end:
    FLOW_DESTROY(&f);
    UTHFreePacket(p);
    StreamTcpUTClearSession(&ssn);
    StreamTcpUTDeinit(ra_ctx);
    return ret;
}

/** \test test insert with overlaps
 */
static int StreamTcpReassembleInsertTest02(void) {
    int ret = 0;
    TcpReassemblyThreadCtx *ra_ctx = NULL;
    ThreadVars tv;
    TcpSession ssn;

    memset(&tv, 0x00, sizeof(tv));

    StreamTcpUTInit(&ra_ctx);
    StreamTcpUTSetupSession(&ssn);
    StreamTcpUTSetupStream(&ssn.client, 1);

    int i;
    for (i = 2; i < 10; i++) {
        int len;
        len = i % 2;
        if (len == 0)
            len = 1;
        int seq;
        seq = i * 10;
        if (seq < 2)
            seq = 2;

        if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  seq, 'A', len) == -1) {
            printf("failed to add segment 1: ");
            goto end;
        }
    }
    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  2, 'B', 1024) == -1) {
        printf("failed to add segment 2: ");
        goto end;
    }

    ret = 1;
end:
    StreamTcpUTClearSession(&ssn);
    StreamTcpUTDeinit(ra_ctx);
    return ret;
}

/** \test test insert with overlaps
 */
static int StreamTcpReassembleInsertTest03(void) {
    int ret = 0;
    TcpReassemblyThreadCtx *ra_ctx = NULL;
    ThreadVars tv;
    TcpSession ssn;

    memset(&tv, 0x00, sizeof(tv));

    StreamTcpUTInit(&ra_ctx);
    StreamTcpUTSetupSession(&ssn);
    StreamTcpUTSetupStream(&ssn.client, 1);

    if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  2, 'A', 1024) == -1) {
        printf("failed to add segment 2: ");
        goto end;
    }

    int i;
    for (i = 2; i < 10; i++) {
        int len;
        len = i % 2;
        if (len == 0)
            len = 1;
        int seq;
        seq = i * 10;
        if (seq < 2)
            seq = 2;

        if (StreamTcpUTAddSegmentWithByte(&tv, ra_ctx, &ssn.client,  seq, 'B', len) == -1) {
            printf("failed to add segment 2: ");
            goto end;
        }
    }
    ret = 1;
end:
    StreamTcpUTClearSession(&ssn);
    StreamTcpUTDeinit(ra_ctx);
    return ret;
}

#endif /* UNITTESTS */

/** \brief  The Function Register the Unit tests to test the reassembly engine
 *          for various OS policies.
 */

void StreamTcpReassembleRegisterTests(void) {
#ifdef UNITTESTS
    UtRegisterTest("StreamTcpReassembleTest01 -- BSD OS Before Reassembly Test", StreamTcpReassembleTest01, 1);
    UtRegisterTest("StreamTcpReassembleTest02 -- BSD OS At Same Reassembly Test", StreamTcpReassembleTest02, 1);
    UtRegisterTest("StreamTcpReassembleTest03 -- BSD OS After Reassembly Test", StreamTcpReassembleTest03, 1);
    UtRegisterTest("StreamTcpReassembleTest04 -- BSD OS Complete Reassembly Test", StreamTcpReassembleTest04, 1);
    UtRegisterTest("StreamTcpReassembleTest05 -- VISTA OS Before Reassembly Test", StreamTcpReassembleTest05, 1);
    UtRegisterTest("StreamTcpReassembleTest06 -- VISTA OS At Same Reassembly Test", StreamTcpReassembleTest06, 1);
    UtRegisterTest("StreamTcpReassembleTest07 -- VISTA OS After Reassembly Test", StreamTcpReassembleTest07, 1);
    UtRegisterTest("StreamTcpReassembleTest08 -- VISTA OS Complete Reassembly Test", StreamTcpReassembleTest08, 1);
    UtRegisterTest("StreamTcpReassembleTest09 -- LINUX OS Before Reassembly Test", StreamTcpReassembleTest09, 1);
    UtRegisterTest("StreamTcpReassembleTest10 -- LINUX OS At Same Reassembly Test", StreamTcpReassembleTest10, 1);
    UtRegisterTest("StreamTcpReassembleTest11 -- LINUX OS After Reassembly Test", StreamTcpReassembleTest11, 1);
    UtRegisterTest("StreamTcpReassembleTest12 -- LINUX OS Complete Reassembly Test", StreamTcpReassembleTest12, 1);
    UtRegisterTest("StreamTcpReassembleTest13 -- LINUX_OLD OS Before Reassembly Test", StreamTcpReassembleTest13, 1);
    UtRegisterTest("StreamTcpReassembleTest14 -- LINUX_OLD At Same Reassembly Test", StreamTcpReassembleTest14, 1);
    UtRegisterTest("StreamTcpReassembleTest15 -- LINUX_OLD OS After Reassembly Test", StreamTcpReassembleTest15, 1);
    UtRegisterTest("StreamTcpReassembleTest16 -- LINUX_OLD OS Complete Reassembly Test", StreamTcpReassembleTest16, 1);
    UtRegisterTest("StreamTcpReassembleTest17 -- SOLARIS OS Before Reassembly Test", StreamTcpReassembleTest17, 1);
    UtRegisterTest("StreamTcpReassembleTest18 -- SOLARIS At Same Reassembly Test", StreamTcpReassembleTest18, 1);
    UtRegisterTest("StreamTcpReassembleTest19 -- SOLARIS OS After Reassembly Test", StreamTcpReassembleTest19, 1);
    UtRegisterTest("StreamTcpReassembleTest20 -- SOLARIS OS Complete Reassembly Test", StreamTcpReassembleTest20, 1);
    UtRegisterTest("StreamTcpReassembleTest21 -- LAST OS Before Reassembly Test", StreamTcpReassembleTest21, 1);
    UtRegisterTest("StreamTcpReassembleTest22 -- LAST OS At Same Reassembly Test", StreamTcpReassembleTest22, 1);
    UtRegisterTest("StreamTcpReassembleTest23 -- LAST OS After Reassembly Test", StreamTcpReassembleTest23, 1);
    UtRegisterTest("StreamTcpReassembleTest24 -- LAST OS Complete Reassembly Test", StreamTcpReassembleTest24, 1);
    UtRegisterTest("StreamTcpReassembleTest25 -- Gap at Start Reassembly Test", StreamTcpReassembleTest25, 1);
    UtRegisterTest("StreamTcpReassembleTest26 -- Gap at middle Reassembly Test", StreamTcpReassembleTest26, 1);
    UtRegisterTest("StreamTcpReassembleTest27 -- Gap at after  Reassembly Test", StreamTcpReassembleTest27, 1);
    UtRegisterTest("StreamTcpReassembleTest28 -- Gap at Start IDS missed packet Reassembly Test", StreamTcpReassembleTest28, 1);
    UtRegisterTest("StreamTcpReassembleTest29 -- Gap at Middle IDS missed packet Reassembly Test", StreamTcpReassembleTest29, 1);
    UtRegisterTest("StreamTcpReassembleTest30 -- Gap at End IDS missed packet Reassembly Test", StreamTcpReassembleTest30, 1);
    UtRegisterTest("StreamTcpReassembleTest31 -- Fast Track Reassembly Test", StreamTcpReassembleTest31, 1);
    UtRegisterTest("StreamTcpReassembleTest32 -- Bug test", StreamTcpReassembleTest32, 1);
    UtRegisterTest("StreamTcpReassembleTest33 -- Bug test", StreamTcpReassembleTest33, 1);
    UtRegisterTest("StreamTcpReassembleTest34 -- Bug test", StreamTcpReassembleTest34, 1);
    UtRegisterTest("StreamTcpReassembleTest35 -- Bug56 test", StreamTcpReassembleTest35, 1);
    UtRegisterTest("StreamTcpReassembleTest36 -- Bug57 test", StreamTcpReassembleTest36, 1);
    UtRegisterTest("StreamTcpReassembleTest37 -- Bug76 test", StreamTcpReassembleTest37, 1);
    UtRegisterTest("StreamTcpReassembleTest38 -- app proto test", StreamTcpReassembleTest38, 1);
    UtRegisterTest("StreamTcpReassembleTest39 -- app proto test", StreamTcpReassembleTest39, 1);
    UtRegisterTest("StreamTcpReassembleTest40 -- app proto test", StreamTcpReassembleTest40, 1);
    UtRegisterTest("StreamTcpReassembleTest41 -- app proto test", StreamTcpReassembleTest41, 1);
    UtRegisterTest("StreamTcpReassembleTest43 -- min smsg size test", StreamTcpReassembleTest43, 1);
    UtRegisterTest("StreamTcpReassembleTest44 -- Memcap Test", StreamTcpReassembleTest44, 1);
    UtRegisterTest("StreamTcpReassembleTest45 -- Depth Test", StreamTcpReassembleTest45, 1);
    UtRegisterTest("StreamTcpReassembleTest46 -- Depth Test", StreamTcpReassembleTest46, 1);
    UtRegisterTest("StreamTcpReassembleTest47 -- TCP Sequence Wraparound Test", StreamTcpReassembleTest47, 1);

    UtRegisterTest("StreamTcpReassembleInlineTest01 -- inline RAW ra", StreamTcpReassembleInlineTest01, 1);
    UtRegisterTest("StreamTcpReassembleInlineTest02 -- inline RAW ra 2", StreamTcpReassembleInlineTest02, 1);
    UtRegisterTest("StreamTcpReassembleInlineTest03 -- inline RAW ra 3", StreamTcpReassembleInlineTest03, 1);
    UtRegisterTest("StreamTcpReassembleInlineTest04 -- inline RAW ra 4", StreamTcpReassembleInlineTest04, 1);
    UtRegisterTest("StreamTcpReassembleInlineTest05 -- inline RAW ra 5 GAP", StreamTcpReassembleInlineTest05, 1);
    UtRegisterTest("StreamTcpReassembleInlineTest06 -- inline RAW ra 6 GAP", StreamTcpReassembleInlineTest06, 1);
    UtRegisterTest("StreamTcpReassembleInlineTest07 -- inline RAW ra 7 GAP", StreamTcpReassembleInlineTest07, 1);
    UtRegisterTest("StreamTcpReassembleInlineTest08 -- inline RAW ra 8 cleanup", StreamTcpReassembleInlineTest08, 1);
    UtRegisterTest("StreamTcpReassembleInlineTest09 -- inline RAW ra 9 GAP cleanup", StreamTcpReassembleInlineTest09, 1);

    UtRegisterTest("StreamTcpReassembleInlineTest10 -- inline APP ra 10", StreamTcpReassembleInlineTest10, 1);

    UtRegisterTest("StreamTcpReassembleInsertTest01 -- insert with overlap", StreamTcpReassembleInsertTest01, 1);
    UtRegisterTest("StreamTcpReassembleInsertTest02 -- insert with overlap", StreamTcpReassembleInsertTest02, 1);
    UtRegisterTest("StreamTcpReassembleInsertTest03 -- insert with overlap", StreamTcpReassembleInsertTest03, 1);

    StreamTcpInlineRegisterTests();
    StreamTcpUtilRegisterTests();
#endif /* UNITTESTS */
}
