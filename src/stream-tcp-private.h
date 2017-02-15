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
 * \author Victor Julien <victor@inliniac.net>
 */

#ifndef __STREAM_TCP_PRIVATE_H__
#define __STREAM_TCP_PRIVATE_H__

#include "decode.h"

#define STREAMTCP_QUEUE_FLAG_TS     0x01
#define STREAMTCP_QUEUE_FLAG_WS     0x02
#define STREAMTCP_QUEUE_FLAG_SACK   0x04

/** currently only SYN/ACK */
typedef struct TcpStateQueue_ {
    uint8_t flags;
    uint8_t wscale;
    uint16_t win;
    uint32_t seq;
    uint32_t ack;
    uint32_t ts;
    uint32_t pkt_ts;
    struct TcpStateQueue_ *next;
} TcpStateQueue;

typedef struct StreamTcpSackRecord_ {
    uint32_t le;    /**< left edge, host order */
    uint32_t re;    /**< right edge, host order */
    struct StreamTcpSackRecord_ *next;
} StreamTcpSackRecord;

typedef struct TcpSegment_ {
    uint8_t *payload;
    uint16_t payload_len;       /**< actual size of the payload */
    uint16_t pool_size;         /**< size of the memory */
    uint32_t seq;
    struct TcpSegment_ *next;
    struct TcpSegment_ *prev;
    uint8_t flags;
} TcpSegment;

typedef struct TcpStream_ {
    uint16_t flags;                 /**< Flag specific to the stream e.g. Timestamp */
    uint8_t wscale;                 /**< wscale setting in this direction */
    uint8_t os_policy;              /**< target based OS policy used for reassembly and handling packets*/

    uint32_t isn;                   /**< initial sequence number */
    uint32_t next_seq;              /**< next expected sequence number */
    uint32_t last_ack;              /**< last ack'd sequence number in this stream */
    uint32_t next_win;              /**< next max seq within window */
    uint32_t window;                /**< current window setting, after wscale is applied */

    uint32_t last_ts;               /**< Time stamp (TSVAL) of the last seen packet for this stream*/
    uint32_t last_pkt_ts;           /**< Time of last seen packet for this stream (needed for PAWS update)
                                         This will be used to validate the last_ts, when connection has been idle for
                                         longer time.(RFC 1323)*/
    /* reassembly */
    uint32_t ra_app_base_seq;       /**< reassembled seq. We've reassembled up to this point. */
    uint32_t ra_raw_base_seq;       /**< reassembled seq. We've reassembled up to this point. */

    TcpSegment *seg_list;           /**< list of TCP segments that are not yet (fully) used in reassembly */
    TcpSegment *seg_list_tail;      /**< Last segment in the reassembled stream seg list*/

    StreamTcpSackRecord *sack_head; /**< head of list of SACK records */
    StreamTcpSackRecord *sack_tail; /**< tail of list of SACK records */
} TcpStream;

/* from /usr/include/netinet/tcp.h */
enum
{
    TCP_NONE,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECV,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_TIME_WAIT,
    TCP_LAST_ACK,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_CLOSED,
};

/*
 * Per SESSION flags
 */

/** Flag for mid stream session */
#define STREAMTCP_FLAG_MIDSTREAM                    0x0001
/** Flag for mid stream established session */
#define STREAMTCP_FLAG_MIDSTREAM_ESTABLISHED        0x0002
/** Flag for mid session when syn/ack is received */
#define STREAMTCP_FLAG_MIDSTREAM_SYNACK             0x0004
/** Flag for TCP Timestamp option */
#define STREAMTCP_FLAG_TIMESTAMP                    0x0008
/** Server supports wscale (even though it can be 0) */
#define STREAMTCP_FLAG_SERVER_WSCALE                0x0010

/** vacancy at 0x0020 */

/** Flag to indicate that the session is handling asynchronous stream.*/
#define STREAMTCP_FLAG_ASYNC                        0x0040
/** Flag to indicate we're dealing with 4WHS: SYN, SYN, SYN/ACK, ACK
 * (http://www.breakingpointsystems.com/community/blog/tcp-portals-the-three-way-handshake-is-a-lie) */
#define STREAMTCP_FLAG_4WHS                         0x0080
/** Flag to indicate the app layer has detected the app layer protocol on
 *  the current TCP session */
#define STREAMTCP_FLAG_APPPROTO_DETECTION_COMPLETED 0x0100
/** Flag to indicate that this session is possible trying to evade the detection
 *  (http://www.packetstan.com/2010/06/recently-ive-been-on-campaign-to-make.html) */
#define STREAMTCP_FLAG_DETECTION_EVASION_ATTEMPT    0x0200
/** Flag to indicate the client (SYN pkt) permits SACK */
#define STREAMTCP_FLAG_CLIENT_SACKOK                0x0400
/** Flag to indicate both sides of the session permit SACK (SYN + SYN/ACK) */
#define STREAMTCP_FLAG_SACKOK                       0x0800
/** Flag for triggering RAW reassembly before the size limit is reached or
    the stream reaches EOF. */
#define STREAMTCP_FLAG_TRIGGER_RAW_REASSEMBLY       0x1000
/** 3WHS confirmed by server -- if suri sees 3whs ACK but server doesn't (pkt
 *  is lost on the way to server), SYN/ACK is retransmitted. If server sends
 *  normal packet we assume 3whs to be completed. Only used for SYN/ACK resend
 *  event. */
#define STREAMTCP_FLAG_3WHS_CONFIRMED               0x2000

/*
 * Per STREAM flags
 */

/** stream is in a gap state */
#define STREAMTCP_STREAM_FLAG_GAP               0x01
/** Flag to avoid stream reassembly/app layer inspection for the stream */
#define STREAMTCP_STREAM_FLAG_NOREASSEMBLY      0x02

/** vacancy at 0x04 */

/** Stream has reached it's reassembly depth, all further packets are ignored */
#define STREAMTCP_STREAM_FLAG_DEPTH_REACHED     0x08
/** Stream has sent a FIN/RST */
#define STREAMTCP_STREAM_FLAG_CLOSE_INITIATED   0x10
/** Stream supports TIMESTAMP -- used to set ssn STREAMTCP_FLAG_TIMESTAMP
 *  flag. */
#define STREAMTCP_STREAM_FLAG_TIMESTAMP         0x20
/** Flag to indicate the zero value of timestamp */
#define STREAMTCP_STREAM_FLAG_ZERO_TIMESTAMP    0x40

/*
 * Per SEGMENT flags
 */
/** Flag to indicate that the current segment has been processed by the
 *  reassembly code and should be deleted after app layer protocol has been
 *  detected. */
#define SEGMENTTCP_FLAG_RAW_PROCESSED       0x01
/** App Layer reassembly code is done with this segment */
#define SEGMENTTCP_FLAG_APPLAYER_PROCESSED  0x02

#define PAWS_24DAYS         2073600         /**< 24 days in seconds */

#define PKT_IS_IN_RIGHT_DIR(ssn, p)        ((ssn)->flags & STREAMTCP_FLAG_MIDSTREAM_SYNACK ? \
                                            PKT_IS_TOSERVER(p) ? (p)->flowflags &= ~FLOW_PKT_TOSERVER \
                                            (p)->flowflags |= FLOW_PKT_TOCLIENT : (p)->flowflags &= ~FLOW_PKT_TOCLIENT \
                                            (p)->flowflags |= FLOW_PKT_TOSERVER : 0)

/* Macro's for comparing Sequence numbers
 * Page 810 from TCP/IP Illustrated, Volume 2. */
#define SEQ_EQ(a,b)  ((int32_t)((a) - (b)) == 0)
#define SEQ_LT(a,b)  ((int32_t)((a) - (b)) <  0)
#define SEQ_LEQ(a,b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a,b)  ((int32_t)((a) - (b)) >  0)
#define SEQ_GEQ(a,b) ((int32_t)((a) - (b)) >= 0)

#define STREAMTCP_SET_RA_BASE_SEQ(stream, seq) { \
    do { \
        (stream)->ra_raw_base_seq = (seq); \
        (stream)->ra_app_base_seq = (seq); \
    } while(0); \
}

#define StreamTcpSetEvent(p, e) { \
    SCLogDebug("setting event %"PRIu8" on pkt %p (%"PRIu64")", (e), p, (p)->pcap_cnt); \
    ENGINE_SET_EVENT((p), (e)); \
}

typedef struct TcpSession_ {
    uint8_t state;
    uint8_t queue_len;                      /**< length of queue list below */
    uint16_t flags;
    TcpStream server;
    TcpStream client;
    struct StreamMsg_ *toserver_smsg_head;  /**< list of stream msgs (for detection inspection) */
    struct StreamMsg_ *toserver_smsg_tail;  /**< list of stream msgs (for detection inspection) */
    struct StreamMsg_ *toclient_smsg_head;  /**< list of stream msgs (for detection inspection) */
    struct StreamMsg_ *toclient_smsg_tail;  /**< list of stream msgs (for detection inspection) */

    TcpStateQueue *queue;                   /**< list of SYN/ACK candidates */
} TcpSession;

#endif /* __STREAM_TCP_PRIVATE_H__ */

