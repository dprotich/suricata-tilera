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
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 */

#ifndef __DECODE_EVENTS_H__
#define __DECODE_EVENTS_H__

enum {
    /* IPV4 EVENTS */
    IPV4_PKT_TOO_SMALL = 1,         /**< ipv4 pkt smaller than minimum header size */
    IPV4_HLEN_TOO_SMALL,            /**< ipv4 header smaller than minimum size */
    IPV4_IPLEN_SMALLER_THAN_HLEN,   /**< ipv4 pkt len smaller than ip header size */
    IPV4_TRUNC_PKT,                 /**< truncated ipv4 packet */

    /* IPV4 OPTIONS */
    IPV4_OPT_INVALID,               /**< invalid ip options */
    IPV4_OPT_INVALID_LEN,           /**< ip options with invalid len */
    IPV4_OPT_MALFORMED,             /**< malformed ip options */
    IPV4_OPT_PAD_REQUIRED,          /**< pad bytes are needed in ip options */
    IPV4_OPT_EOL_REQUIRED,          /**< "end of list" needed in ip options */
    IPV4_OPT_DUPLICATE,             /**< duplicated ip option */
    IPV4_OPT_UNKNOWN,               /**< unknown ip option */
    IPV4_WRONG_IP_VER,              /**< wrong ip version in ip options */

    /* ICMP EVENTS */
    ICMPV4_PKT_TOO_SMALL,           /**< icmpv4 packet smaller than minimum size */
    ICMPV4_UNKNOWN_TYPE,            /**< icmpv4 unknown type */
    ICMPV4_UNKNOWN_CODE,            /**< icmpv4 unknown code */
    ICMPV4_IPV4_TRUNC_PKT,          /**< truncated icmpv4 packet */
    ICMPV4_IPV4_UNKNOWN_VER,        /**< unknown version in icmpv4 packet*/

    /* ICMPv6 EVENTS */
    ICMPV6_UNKNOWN_TYPE,            /**< icmpv6 unknown type */
    ICMPV6_UNKNOWN_CODE,            /**< icmpv6 unknown code */
    ICMPV6_PKT_TOO_SMALL,           /**< icmpv6 smaller than minimum size */
    ICMPV6_IPV6_UNKNOWN_VER,        /**< unknown version in icmpv6 packet */
    ICMPV6_IPV6_TRUNC_PKT,          /**< truncated icmpv6 packet */

    /* IPV6 EVENTS */
    IPV6_PKT_TOO_SMALL,             /**< ipv6 packet smaller than minimum size */
    IPV6_TRUNC_PKT,                 /**< truncated ipv6 packet */
    IPV6_TRUNC_EXTHDR,              /**< truncated ipv6 extension header */
    IPV6_EXTHDR_DUPL_FH,            /**< duplicated "fragment" header in ipv6 extension headers */
    IPV6_EXTHDR_USELESS_FH,         /**< useless FH: offset 0 + no more fragments */
    IPV6_EXTHDR_DUPL_RH,            /**< duplicated "routing" header in ipv6 extension headers */
    IPV6_EXTHDR_DUPL_HH,            /**< duplicated "hop-by-hop" header in ipv6 extension headers */
    IPV6_EXTHDR_DUPL_DH,            /**< duplicated "destination" header in ipv6 extension headers */
    IPV6_EXTHDR_DUPL_AH,            /**< duplicated "authentication" header in ipv6 extension headers */
    IPV6_EXTHDR_DUPL_EH,            /**< duplicated "ESP" header in ipv6 extension headers */

    IPV6_EXTHDR_INVALID_OPTLEN,     /**< the opt len in an hop or dst hdr is invalid. */
    IPV6_WRONG_IP_VER,              /**< wrong version in ipv6 */
    IPV6_EXTHDR_AH_RES_NOT_NULL,    /**< AH hdr reserved fields not null (rfc 4302) */

    IPV6_HOPOPTS_UNKNOWN_OPT,       /**< unknown HOP opt */
    IPV6_HOPOPTS_ONLY_PADDING,      /**< all options in HOP opts are padding */
    IPV6_DSTOPTS_UNKNOWN_OPT,       /**< unknown DST opt */
    IPV6_DSTOPTS_ONLY_PADDING,      /**< all options in DST opts are padding */

    IPV6_WITH_ICMPV4,               /**< IPv6 packet with ICMPv4 header */

    /* TCP EVENTS */
    TCP_PKT_TOO_SMALL,              /**< tcp packet smaller than minimum size */
    TCP_HLEN_TOO_SMALL,             /**< tcp header smaller than minimum size */
    TCP_INVALID_OPTLEN,             /**< invalid len in tcp options */

    /* TCP OPTIONS */
    TCP_OPT_INVALID_LEN,            /**< tcp option with invalid len */
    TCP_OPT_DUPLICATE,              /**< duplicated tcp option */

    /* UDP EVENTS */
    UDP_PKT_TOO_SMALL,              /**< udp packet smaller than minimum size */
    UDP_HLEN_TOO_SMALL,             /**< udp header smaller than minimum size */
    UDP_HLEN_INVALID,               /**< invalid len of upd header */

    /* SLL EVENTS */
    SLL_PKT_TOO_SMALL,              /**< sll packet smaller than minimum size */

    /* ETHERNET EVENTS */
    ETHERNET_PKT_TOO_SMALL,         /**< ethernet packet smaller than minimum size */

    /* PPP EVENTS */
    PPP_PKT_TOO_SMALL,              /**< ppp packet smaller than minimum size */
    PPPVJU_PKT_TOO_SMALL,           /**< ppp vj uncompressed packet smaller than minimum size */
    PPPIPV4_PKT_TOO_SMALL,          /**< ppp ipv4 packet smaller than minimum size */
    PPPIPV6_PKT_TOO_SMALL,          /**< ppp ipv6 packet smaller than minimum size */
    PPP_WRONG_TYPE,                 /**< wrong type in ppp frame */
    PPP_UNSUP_PROTO,                /**< protocol not supported for ppp */

    /* PPPOE EVENTS */
    PPPOE_PKT_TOO_SMALL,            /**< pppoe packet smaller than minimum size */
    PPPOE_WRONG_CODE,               /**< wrong code for pppoe */
    PPPOE_MALFORMED_TAGS,           /**< malformed tags in pppoe */

    /* GRE EVENTS */
    GRE_PKT_TOO_SMALL,              /**< gre packet smaller than minimum size */
    GRE_WRONG_VERSION,              /**< wrong version in gre header */
    GRE_VERSION0_RECUR,             /**< gre v0 recursion control */
    GRE_VERSION0_FLAGS,             /**< gre v0 flags */
    GRE_VERSION0_HDR_TOO_BIG,       /**< gre v0 header bigger than maximum size */
    GRE_VERSION0_MALFORMED_SRE_HDR, /**< gre v0 malformed source route entry header */
    GRE_VERSION1_CHKSUM,            /**< gre v1 checksum */
    GRE_VERSION1_ROUTE,             /**< gre v1 routing */
    GRE_VERSION1_SSR,               /**< gre v1 strict source route */
    GRE_VERSION1_RECUR,             /**< gre v1 recursion control */
    GRE_VERSION1_FLAGS,             /**< gre v1 flags */
    GRE_VERSION1_NO_KEY,            /**< gre v1 no key present in header */
    GRE_VERSION1_WRONG_PROTOCOL,    /**< gre v1 wrong protocol */
    GRE_VERSION1_MALFORMED_SRE_HDR, /**< gre v1 malformed source route entry header */
    GRE_VERSION1_HDR_TOO_BIG,       /**< gre v1 header too big */

    /* VLAN EVENTS */
    VLAN_HEADER_TOO_SMALL,          /**< vlan header smaller than minimum size */
    VLAN_UNKNOWN_TYPE,              /**< vlan unknown type */

    /* RAW EVENTS */
    IPRAW_INVALID_IPV,              /**< invalid ip version in ip raw */

    STREAM_3WHS_ACK_IN_WRONG_DIR,
    STREAM_3WHS_ASYNC_WRONG_SEQ,
    STREAM_3WHS_RIGHT_SEQ_WRONG_ACK_EVASION,
    STREAM_3WHS_SYNACK_IN_WRONG_DIRECTION,
    STREAM_3WHS_SYNACK_RESEND_WITH_DIFFERENT_ACK,
    STREAM_3WHS_SYNACK_RESEND_WITH_DIFF_SEQ,
    STREAM_3WHS_SYNACK_TOSERVER_ON_SYN_RECV,
    STREAM_3WHS_SYNACK_WITH_WRONG_ACK,
    STREAM_3WHS_SYNACK_FLOOD,
    STREAM_3WHS_SYN_RESEND_DIFF_SEQ_ON_SYN_RECV,
    STREAM_3WHS_SYN_TOCLIENT_ON_SYN_RECV,
    STREAM_3WHS_WRONG_SEQ_WRONG_ACK,
    STREAM_4WHS_SYNACK_WITH_WRONG_ACK,
    STREAM_4WHS_SYNACK_WITH_WRONG_SYN,
    STREAM_4WHS_WRONG_SEQ,
    STREAM_4WHS_INVALID_ACK,
    STREAM_CLOSEWAIT_ACK_OUT_OF_WINDOW,
    STREAM_CLOSEWAIT_FIN_OUT_OF_WINDOW,
    STREAM_CLOSEWAIT_PKT_BEFORE_LAST_ACK,
    STREAM_CLOSEWAIT_INVALID_ACK,
    STREAM_CLOSING_ACK_WRONG_SEQ,
    STREAM_CLOSING_INVALID_ACK,
    STREAM_EST_PACKET_OUT_OF_WINDOW,
    STREAM_EST_PKT_BEFORE_LAST_ACK,
    STREAM_EST_SYNACK_RESEND,
    STREAM_EST_SYNACK_RESEND_WITH_DIFFERENT_ACK,
    STREAM_EST_SYNACK_RESEND_WITH_DIFF_SEQ,
    STREAM_EST_SYNACK_TOSERVER,
    STREAM_EST_SYN_RESEND,
    STREAM_EST_SYN_RESEND_DIFF_SEQ,
    STREAM_EST_SYN_TOCLIENT,
    STREAM_EST_INVALID_ACK,
    STREAM_FIN_INVALID_ACK,
    STREAM_FIN1_ACK_WRONG_SEQ,
    STREAM_FIN1_FIN_WRONG_SEQ,
    STREAM_FIN1_INVALID_ACK,
    STREAM_FIN2_ACK_WRONG_SEQ,
    STREAM_FIN2_FIN_WRONG_SEQ,
    STREAM_FIN2_INVALID_ACK,
    STREAM_FIN_BUT_NO_SESSION,
    STREAM_FIN_OUT_OF_WINDOW,
    STREAM_LASTACK_ACK_WRONG_SEQ,
    STREAM_LASTACK_INVALID_ACK,
    STREAM_RST_BUT_NO_SESSION,
    STREAM_TIMEWAIT_ACK_WRONG_SEQ,
    STREAM_TIMEWAIT_INVALID_ACK,
    STREAM_SHUTDOWN_SYN_RESEND,
    STREAM_PKT_INVALID_TIMESTAMP,
    STREAM_PKT_INVALID_ACK,
    STREAM_PKT_BROKEN_ACK,
    STREAM_RST_INVALID_ACK,
    STREAM_PKT_RETRANSMISSION,

    STREAM_REASSEMBLY_SEGMENT_BEFORE_BASE_SEQ,
    STREAM_REASSEMBLY_NO_SEGMENT,

    STREAM_REASSEMBLY_SEQ_GAP,

    STREAM_REASSEMBLY_OVERLAP_DIFFERENT_DATA,

    /* SCTP EVENTS */
    SCTP_PKT_TOO_SMALL,              /**< sctp packet smaller than minimum size */

    /* Fragmentation reasembly events. */
    IPV4_FRAG_PKT_TOO_LARGE,
    IPV4_FRAG_OVERLAP,
    IPV6_FRAG_PKT_TOO_LARGE,
    IPV6_FRAG_OVERLAP,
    IPV4_FRAG_TOO_LARGE,
    IPV6_FRAG_TOO_LARGE,
    /* Fragment ignored due to internal error */
    IPV4_FRAG_IGNORED,
    IPV6_FRAG_IGNORED,

    /* IPv4 in IPv6 events */
    IPV4_IN_IPV6_PKT_TOO_SMALL,
    IPV4_IN_IPV6_WRONG_IP_VER,
    /* IPv6 in IPv6 events */
    IPV6_IN_IPV6_PKT_TOO_SMALL,
    IPV6_IN_IPV6_WRONG_IP_VER,

    /* should always be last! */
    DECODE_EVENT_MAX,
};

#define DECODER_EVENTS_BUFFER_STEPS 5

/**
 * \brief Data structure to store app layer decoder events.
 */
typedef struct AppLayerDecoderEvents_ {
    /* array of events */
    uint8_t *events;
    /* number of events in the above buffer */
    uint8_t cnt;
    /* current event buffer size */
    uint8_t events_buffer_size;
} AppLayerDecoderEvents;

/**
 * \brief Store decoder event module
 */
typedef struct AppLayerDecoderEventsModule_ {
    /* the alproto module for which we are storing the event table */
    uint16_t alproto;
    /* the event table map */
    SCEnumCharMap *table;

    struct AppLayerDecoderEventsModule_ *next;
} AppLayerDecoderEventsModule;

#if 0

#define AppLayerDecoderEventsSetEvent(module_id, devents_head, event)   \
    do {                                                                \
        DecoderEvents devents = *devents_head;                          \
        while (devents != NULL && devents->module_id != module_id) {    \
            devents = devents->next;                                    \
        }                                                               \
        if (devents == NULL) {                                          \
            DecoderEvents new_devents = SCMalloc(sizeof(DecoderEvents));\
            if (new_devents == NULL)                                    \
                return;                                                 \
            memset(new_devents, 0, sizeof(DecoderEvents));              \
            devents_head = new_devents;                                 \
        }                                                               \
        if ((devents)->cnt == events_buffer_size) {                     \
            devents->events = SCRealloc(devents->events,                \
                                        (devents->cnt +                 \
                                         DECODER_EVENTS_BUFFER_STEPS) * \
                                         sizeof(uint8_t));              \
            if (devents->events == NULL) {                              \
                devents->events_buffer_size = 0;                        \
                devents->cnt = 0;                                       \
                break;                                                  \
            }                                                           \
            devents->events_buffer_size += DECODER_EVENTS_BUFFER_STEPS; \
        }                                                               \
        devents->events[devents->cnt++] = event;                        \
    } while (0)

static inline int AppLayerDecoderEventsIsEventSet(int module_id,
                                                  DecoderEvents *devents,
                                                  uint8_t event)
{
    while (devents != NULL && devents->module_id != module_id) {
        devents = devents->next;
    }

    if (devents == NULL)
        return 0;

    int i;
    int cnt = devents->cnt;
    for (i = 0; i < cnt; i++) {
        if (devents->events[i] == event)
            return 1;
    }

    return 0;
}

#define DecoderEventsFreeEvents(devents)                    \
    do {                                                    \
        while ((devents) != NULL) {                         \
            if ((devents)->events != NULL)                  \
                SCFree((devents)->events);                  \
            (devents) = (devents)->next;                    \
        }                                                   \
    } while (0)


#endif /* #if 0 */

/**
 * \brief Set an app layer decoder event.
 *
 * \param devents_head Pointer to a DecoderEvents pointer head.  If
 *                     the head points to a DecoderEvents instance, a
 *                     new instance would be created and the pointer head would
 *                     would be updated with this new instance
 * \param event        The event to be stored.
 */
#define AppLayerDecoderEventsSetEvent(f, event)                         \
    do {                                                                \
        AppLayerParserStateStore *parser_state_store =                  \
            (AppLayerParserStateStore *)(f)->alparser;                  \
        AppLayerDecoderEvents *devents =                                \
            parser_state_store->decoder_events;                         \
        if (devents == NULL) {                                          \
            AppLayerDecoderEvents *new_devents =                        \
                SCMalloc(sizeof(AppLayerDecoderEvents));                \
            if (new_devents == NULL)                                    \
                break;                                                  \
            memset(new_devents, 0, sizeof(AppLayerDecoderEvents));      \
            parser_state_store->decoder_events = new_devents;           \
            devents = new_devents;                                      \
        }                                                               \
        if (devents->cnt == devents->events_buffer_size) {              \
            devents->events = SCRealloc(devents->events,                \
                                        (devents->cnt +                 \
                                         DECODER_EVENTS_BUFFER_STEPS) * \
                                         sizeof(uint8_t));              \
            if (devents->events == NULL) {                              \
                devents->events_buffer_size = 0;                        \
                devents->cnt = 0;                                       \
                break;                                                  \
            }                                                           \
            devents->events_buffer_size += DECODER_EVENTS_BUFFER_STEPS; \
        }                                                               \
        devents->events[devents->cnt++] = (event);                      \
        SCLogDebug("setting app-layer-event %u", (event));              \
    } while (0)

static inline int AppLayerDecoderEventsIsEventSet(AppLayerDecoderEvents *devents,
                                                  uint8_t event)
{
    if (devents == NULL)
        return 0;

    int i;
    int cnt = devents->cnt;
    for (i = 0; i < cnt; i++) {
        if (devents->events[i] == event)
            return 1;
    }

    return 0;
}

#define AppLayerDecoderEventsFreeEvents(devents)            \
    do {                                                    \
        if ((devents) != NULL) {                            \
            if ((devents)->events != NULL)                  \
                SCFree((devents)->events);                  \
        }                                                   \
        SCFree((devents));                                  \
    } while (0)

void AppLayerDecoderEventsModuleRegister(uint16_t, SCEnumCharMap *);
uint16_t AppLayerDecoderEventsModuleGetAlproto(const char *);
int AppLayerDecoderEventsModuleGetEventId(uint16_t, const char *);
void AppLayerDecodeEventsModuleDeRegister(void);

/***** Unittest helper functions *****/
void AppLayerDecoderEventsModuleCreateBackup(void);
void AppLayerDecoderEventsModuleRestoreBackup(void);

#endif /* __DECODE_EVENTS_H__ */
