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
 * \author Endace Technology Limited, Jason Ish <jason.ish@endace.com>
 */

#ifndef __DEFRAG_H__
#define __DEFRAG_H__

#include "util-pool.h"

/**
 * A context for an instance of a fragmentation re-assembler, in case
 * we ever need more than one.
 */
typedef struct DefragContext_ {
    Pool *frag_pool; /**< Pool of fragments. */
    SCMutex frag_pool_lock;

    time_t timeout; /**< Default timeout. */
} DefragContext;

/**
 * Storage for an individual fragment.
 */
typedef struct Frag_ {
    uint16_t offset;            /**< The offset of this fragment, already
                                 *   multiplied by 8. */

    uint16_t len;               /**< The length of this fragment. */

    uint8_t hlen;               /**< The length of this fragments IP header. */

    uint8_t more_frags:4;       /**< More frags? */
    uint8_t skip:4;             /**< Skip this fragment during re-assembly. */

    uint16_t ip_hdr_offset;     /**< Offset in the packet where the IP
                                 * header starts. */
    uint16_t frag_hdr_offset;   /**< Offset in the packet where the frag
                                 * header starts. */

    uint16_t data_offset;       /**< Offset to the packet data. */
    uint16_t data_len;          /**< Length of data. */

    uint16_t ltrim;             /**< Number of leading bytes to trim when
                                 * re-assembling the packet. */

    uint8_t *pkt;               /**< The actual packet. */

#ifdef DEBUG
    uint64_t pcap_cnt;          /**< pcap_cnt of original packet */
#endif

    TAILQ_ENTRY(Frag_) next;    /**< Pointer to next fragment for tailq. */
} Frag;

/** \brief Reset tracker fields except "lock" */
#define DEFRAG_TRACKER_RESET(t) { \
    (t)->timeout = 0; \
    (t)->id = 0; \
    (t)->policy = 0; \
    (t)->af = 0; \
    (t)->seen_last = 0; \
    (t)->remove = 0; \
    CLEAR_ADDR(&(t)->src_addr); \
    CLEAR_ADDR(&(t)->dst_addr); \
    (t)->frags.tqh_first = NULL; \
    (t)->frags.tqh_last = NULL; \
}

/**
 * A defragmentation tracker.  Used to track fragments that make up a
 * single packet.
 */
typedef struct DefragTracker_ {
    SCMutex lock; /**< Mutex for locking list operations on
                           * this tracker. */

    uint32_t id; /**< IP ID for this tracker.  32 bits for IPv6, 16
                  * for IPv4. */

    uint8_t policy; /**< Reassembly policy this tracker will use. */

    uint8_t af; /**< Address family for this tracker, AF_INET or
                 * AF_INET6. */

    uint8_t seen_last; /**< Has this tracker seen the last fragment? */

    uint8_t remove; /**< remove */

    Address src_addr; /**< Source address for this tracker. */
    Address dst_addr; /**< Destination address for this tracker. */

    uint32_t timeout; /**< When this tracker will timeout. */

    /** use cnt, reference counter */
#ifdef __tile__
    SC_ATOMIC_DECLARE(unsigned int, use_cnt);
#else
    SC_ATOMIC_DECLARE(unsigned short, use_cnt);
#endif

    TAILQ_HEAD(frag_tailq, Frag_) frags; /**< Head of list of fragments. */

    /** hash pointers, protected by hash row mutex/spin */
    struct DefragTracker_ *hnext;
    struct DefragTracker_ *hprev;

    /** list pointers, protected by tracker-queue mutex/spin */
    struct DefragTracker_ *lnext;
    struct DefragTracker_ *lprev;
} DefragTracker;

void DefragInit(void);
void DefragDestroy(void);
void DefragReload(void); /**< use only in unittests */

uint8_t DefragGetOsPolicy(Packet *);
void DefragTrackerFreeFrags(DefragTracker *);
Packet *Defrag(ThreadVars *, DecodeThreadVars *, Packet *);
void DefragRegisterTests(void);

#endif /* __DEFRAG_H__ */
