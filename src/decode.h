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

#ifndef __DECODE_H__
#define __DECODE_H__

//#define DBG_THREADS
#define COUNTERS

#include "suricata-common.h"

#include "threadvars.h"

typedef enum {
    CHECKSUM_VALIDATION_DISABLE,
    CHECKSUM_VALIDATION_ENABLE,
    CHECKSUM_VALIDATION_AUTO,
    CHECKSUM_VALIDATION_RXONLY,
    CHECKSUM_VALIDATION_KERNEL,
} ChecksumValidationMode;

enum {
    PKT_SRC_WIRE = 1,
    PKT_SRC_DECODER_GRE,
    PKT_SRC_DECODER_IPV4,
    PKT_SRC_DECODER_IPV6,
    PKT_SRC_DECODER_TEREDO,
    PKT_SRC_DEFRAG,
    PKT_SRC_STREAM_TCP_STREAM_END_PSEUDO,
    PKT_SRC_FFR_V2,
    PKT_SRC_FFR_SHUTDOWN,
};

#include "source-nfq.h"
#include "source-ipfw.h"
#include "source-pcap.h"
#include "source-af-packet.h"
#include "source-mpipe.h"

#include "action-globals.h"

#include "decode-ethernet.h"
#include "decode-gre.h"
#include "decode-ppp.h"
#include "decode-pppoe.h"
#include "decode-sll.h"
#include "decode-ipv4.h"
#include "decode-ipv6.h"
#include "decode-icmpv4.h"
#include "decode-icmpv6.h"
#include "decode-tcp.h"
#include "decode-udp.h"
#include "decode-sctp.h"
#include "decode-raw.h"
#include "decode-vlan.h"

#include "detect-reference.h"

#include "app-layer-protos.h"

#ifdef __tilegx__
#include <gxio/mpipe.h>
#elif defined(__tile__)
#include <netio/netio.h>
#endif

#ifdef __SC_CUDA_SUPPORT__
#define CUDA_MAX_PAYLOAD_SIZE 1500
#endif

/* forward declaration */
struct DetectionEngineThreadCtx_;

/* Address */
typedef struct Address_ {
    char family;
    union {
        uint32_t       address_un_data32[4]; /* type-specific field */
        uint16_t       address_un_data16[8]; /* type-specific field */
        uint8_t        address_un_data8[16]; /* type-specific field */
    } address;
} Address;

#define addr_data32 address.address_un_data32
#define addr_data16 address.address_un_data16
#define addr_data8  address.address_un_data8

#define COPY_ADDRESS(a, b) do {                    \
        (b)->family = (a)->family;                 \
        (b)->addr_data32[0] = (a)->addr_data32[0]; \
        (b)->addr_data32[1] = (a)->addr_data32[1]; \
        (b)->addr_data32[2] = (a)->addr_data32[2]; \
        (b)->addr_data32[3] = (a)->addr_data32[3]; \
    } while (0)

/* Set the IPv4 addressesinto the Addrs of the Packet.
 * Make sure p->ip4h is initialized and validated.
 *
 * We set the rest of the struct to 0 so we can
 * prevent using memset. */
#define SET_IPV4_SRC_ADDR(p, a) do {                              \
        (a)->family = AF_INET;                                    \
        (a)->addr_data32[0] = (uint32_t)(p)->ip4h->s_ip_src.s_addr; \
        (a)->addr_data32[1] = 0;                                  \
        (a)->addr_data32[2] = 0;                                  \
        (a)->addr_data32[3] = 0;                                  \
    } while (0)

#define SET_IPV4_DST_ADDR(p, a) do {                              \
        (a)->family = AF_INET;                                    \
        (a)->addr_data32[0] = (uint32_t)(p)->ip4h->s_ip_dst.s_addr; \
        (a)->addr_data32[1] = 0;                                  \
        (a)->addr_data32[2] = 0;                                  \
        (a)->addr_data32[3] = 0;                                  \
    } while (0)

/* clear the address structure by setting all fields to 0 */
#define CLEAR_ADDR(a) do {       \
        (a)->family = 0;         \
        (a)->addr_data32[0] = 0; \
        (a)->addr_data32[1] = 0; \
        (a)->addr_data32[2] = 0; \
        (a)->addr_data32[3] = 0; \
    } while (0)

/* Set the IPv6 addressesinto the Addrs of the Packet.
 * Make sure p->ip6h is initialized and validated. */
#define SET_IPV6_SRC_ADDR(p, a) do {                    \
        (a)->family = AF_INET6;                         \
        (a)->addr_data32[0] = (p)->ip6h->s_ip6_src[0];  \
        (a)->addr_data32[1] = (p)->ip6h->s_ip6_src[1];  \
        (a)->addr_data32[2] = (p)->ip6h->s_ip6_src[2];  \
        (a)->addr_data32[3] = (p)->ip6h->s_ip6_src[3];  \
    } while (0)

#define SET_IPV6_DST_ADDR(p, a) do {                    \
        (a)->family = AF_INET6;                         \
        (a)->addr_data32[0] = (p)->ip6h->s_ip6_dst[0];  \
        (a)->addr_data32[1] = (p)->ip6h->s_ip6_dst[1];  \
        (a)->addr_data32[2] = (p)->ip6h->s_ip6_dst[2];  \
        (a)->addr_data32[3] = (p)->ip6h->s_ip6_dst[3];  \
    } while (0)

/* Set the TCP ports into the Ports of the Packet.
 * Make sure p->tcph is initialized and validated. */
#define SET_TCP_SRC_PORT(pkt, prt) do {            \
        SET_PORT(TCP_GET_SRC_PORT((pkt)), *(prt)); \
    } while (0)

#define SET_TCP_DST_PORT(pkt, prt) do {            \
        SET_PORT(TCP_GET_DST_PORT((pkt)), *(prt)); \
    } while (0)

/* Set the UDP ports into the Ports of the Packet.
 * Make sure p->udph is initialized and validated. */
#define SET_UDP_SRC_PORT(pkt, prt) do {            \
        SET_PORT(UDP_GET_SRC_PORT((pkt)), *(prt)); \
    } while (0)
#define SET_UDP_DST_PORT(pkt, prt) do {            \
        SET_PORT(UDP_GET_DST_PORT((pkt)), *(prt)); \
    } while (0)

/* Set the SCTP ports into the Ports of the Packet.
 * Make sure p->sctph is initialized and validated. */
#define SET_SCTP_SRC_PORT(pkt, prt) do {            \
        SET_PORT(SCTP_GET_SRC_PORT((pkt)), *(prt)); \
    } while (0)

#define SET_SCTP_DST_PORT(pkt, prt) do {            \
        SET_PORT(SCTP_GET_DST_PORT((pkt)), *(prt)); \
    } while (0)



#define GET_IPV4_SRC_ADDR_U32(p) ((p)->src.addr_data32[0])
#define GET_IPV4_DST_ADDR_U32(p) ((p)->dst.addr_data32[0])
#define GET_IPV4_SRC_ADDR_PTR(p) ((p)->src.addr_data32)
#define GET_IPV4_DST_ADDR_PTR(p) ((p)->dst.addr_data32)

#define GET_IPV6_SRC_ADDR(p) ((p)->src.addr_data32)
#define GET_IPV6_DST_ADDR(p) ((p)->dst.addr_data32)
#define GET_TCP_SRC_PORT(p)  ((p)->sp)
#define GET_TCP_DST_PORT(p)  ((p)->dp)

#define GET_PKT_LEN(p) ((p)->pktlen)
#define GET_PKT_DATA(p) ((((p)->ext_pkt) == NULL ) ? (p)->pkt : (p)->ext_pkt)
#define GET_PKT_DIRECT_DATA(p) ((p)->pkt)
#define GET_PKT_DIRECT_MAX_SIZE(p) (default_packet_size)

#define SET_PKT_LEN(p, len) do { \
    (p)->pktlen = (len); \
    } while (0)


/* Port is just a uint16_t */
typedef uint16_t Port;
#define SET_PORT(v, p) ((p) = (v))
#define COPY_PORT(a,b) ((b) = (a))

#define CMP_ADDR(a1, a2) \
    (((a1)->addr_data32[3] == (a2)->addr_data32[3] && \
      (a1)->addr_data32[2] == (a2)->addr_data32[2] && \
      (a1)->addr_data32[1] == (a2)->addr_data32[1] && \
      (a1)->addr_data32[0] == (a2)->addr_data32[0]))
#define CMP_PORT(p1, p2) \
    ((p1) == (p2))

/*Given a packet pkt offset to the start of the ip header in a packet
 *We determine the ip version. */
#define IP_GET_RAW_VER(pkt) ((((pkt)[0] & 0xf0) >> 4))

#define PKT_IS_IPV4(p)      (((p)->ip4h != NULL))
#define PKT_IS_IPV6(p)      (((p)->ip6h != NULL))
#define PKT_IS_TCP(p)       (((p)->tcph != NULL))
#define PKT_IS_UDP(p)       (((p)->udph != NULL))
#define PKT_IS_ICMPV4(p)    (((p)->icmpv4h != NULL))
#define PKT_IS_ICMPV6(p)    (((p)->icmpv6h != NULL))
#define PKT_IS_TOSERVER(p)  (((p)->flowflags & FLOW_PKT_TOSERVER))
#define PKT_IS_TOCLIENT(p)  (((p)->flowflags & FLOW_PKT_TOCLIENT))

#define IPH_IS_VALID(p) (PKT_IS_IPV4((p)) || PKT_IS_IPV6((p)))

/* Retrieve proto regardless of IP version */
#define IP_GET_IPPROTO(p) \
    (p->proto ? p->proto : \
    (PKT_IS_IPV4((p))? IPV4_GET_IPPROTO((p)) : (PKT_IS_IPV6((p))? IPV6_GET_L4PROTO((p)) : 0)))

/* structure to store the sids/gids/etc the detection engine
 * found in this packet */
typedef struct PacketAlert_ {
    SigIntId num; /* Internal num, used for sorting */
    SigIntId order_id; /* Internal num, used for sorting */
    uint8_t action; /* Internal num, used for sorting */
    uint8_t flags;
    struct Signature_ *s;
} PacketAlert;

/** After processing an alert by the thresholding module, if at
 *  last it gets triggered, we might want to stick the drop action to
 *  the flow on IPS mode */
#define PACKET_ALERT_FLAG_DROP_FLOW     0x01
/** alert was generated based on state */
#define PACKET_ALERT_FLAG_STATE_MATCH   0x02
/** alert was generated based on stream */
#define PACKET_ALERT_FLAG_STREAM_MATCH  0x04

#define PACKET_ALERT_MAX 15

typedef struct PacketAlerts_ {
    uint16_t cnt;
    PacketAlert alerts[PACKET_ALERT_MAX];
} PacketAlerts;

/** number of decoder events we support per packet. Power of 2 minus 1
 *  for memory layout */
#define PACKET_ENGINE_EVENT_MAX 15

/** data structure to store decoder, defrag and stream events */
typedef struct PacketEngineEvents_ {
    uint8_t cnt;                                /**< number of events */
    uint8_t events[PACKET_ENGINE_EVENT_MAX];   /**< array of events */
} PacketEngineEvents;

typedef struct PktVar_ {
    char *name;
    struct PktVar_ *next; /* right now just implement this as a list,
                           * in the long run we have thing of something
                           * faster. */
    uint8_t *value;
    uint16_t value_len;
} PktVar;

#ifdef PROFILING

/** \brief Per TMM stats storage */
typedef struct PktProfilingTmmData_ {
    uint64_t ticks_start;
    uint64_t ticks_end;
#ifdef PROFILE_LOCKING
    uint64_t mutex_lock_cnt;
    uint64_t mutex_lock_wait_ticks;
    uint64_t mutex_lock_contention;
    uint64_t spin_lock_cnt;
    uint64_t spin_lock_wait_ticks;
    uint64_t spin_lock_contention;
    uint64_t rww_lock_cnt;
    uint64_t rww_lock_wait_ticks;
    uint64_t rww_lock_contention;
    uint64_t rwr_lock_cnt;
    uint64_t rwr_lock_wait_ticks;
    uint64_t rwr_lock_contention;
#endif
} PktProfilingTmmData;

typedef struct PktProfilingDetectData_ {
    uint64_t ticks_start;
    uint64_t ticks_end;
    uint64_t ticks_spent;
} PktProfilingDetectData;

typedef struct PktProfilingAppData_ {
    uint64_t ticks_spent;
} PktProfilingAppData;

/** \brief Per pkt stats storage */
typedef struct PktProfiling_ {
    uint64_t ticks_start;
    uint64_t ticks_end;

    PktProfilingTmmData tmm[TMM_SIZE];
    PktProfilingAppData app[ALPROTO_MAX];
    PktProfilingDetectData detect[PROF_DETECT_SIZE];
    uint64_t proto_detect;
} PktProfiling;

#endif /* PROFILING */

/* forward declartion since Packet struct definition requires this */
struct PacketQueue_;

/* sizes of the members:
 * src: 17 bytes
 * dst: 17 bytes
 * sp/type: 1 byte
 * dp/code: 1 byte
 * proto: 1 byte
 * recurs: 1 byte
 *
 * sum of above: 38 bytes
 *
 * flow ptr: 4/8 bytes
 * flags: 1 byte
 * flowflags: 1 byte
 *
 * sum of above 44/48 bytes
 */
#ifndef __tile__

typedef struct Packet_
{
    /* Addresses, Ports and protocol
     * these are on top so we can use
     * the Packet as a hash key */
    Address src;
    Address dst;
    union {
        Port sp;
        uint8_t type;
    };
    union {
        Port dp;
        uint8_t code;
    };
    uint8_t proto;
    /* make sure we can't be attacked on when the tunneled packet
     * has the exact same tuple as the lower levels */
    uint8_t recursion_level;

    /* Pkt Flags */
    uint32_t flags;

    /* flow */
    uint8_t flowflags;

    uint8_t pkt_src;

    struct Flow_ *flow;

    struct timeval ts;

    union {
        /* nfq stuff */
#ifdef NFQ
        NFQPacketVars nfq_v;
#endif /* NFQ */
#ifdef IPFW
        IPFWPacketVars ipfw_v;
#endif /* IPFW */
#ifdef AF_PACKET
        AFPPacketVars afp_v;
#endif

        /** libpcap vars: shared by Pcap Live mode and Pcap File mode */
        PcapPacketVars pcap_v;
    };

    /** data linktype in host order */
    int datalink;

    /* IPS action to take */
    uint8_t action;

    /* used to hold flowbits only if debuglog is enabled */
    int debuglog_flowbits_names_len;
    const char **debuglog_flowbits_names;

    /** The release function for packet data */
    TmEcode (*ReleaseData)(ThreadVars *, struct Packet_ *);

    /* pkt vars */
    PktVar *pktvar;

    /* header pointers */
    EthernetHdr *ethh;

    IPV4Hdr *ip4h;
    IPV4Vars ip4vars;

    IPV6Hdr *ip6h;
    IPV6Vars ip6vars;
    IPV6ExtHdrs ip6eh;

    TCPHdr *tcph;
    TCPVars tcpvars;

    UDPHdr *udph;
    UDPVars udpvars;

    SCTPHdr *sctph;

    ICMPV4Hdr *icmpv4h;
    ICMPV4Vars icmpv4vars;

    ICMPV6Hdr *icmpv6h;
    ICMPV6Vars icmpv6vars;

    PPPHdr *ppph;
    PPPOESessionHdr *pppoesh;
    PPPOEDiscoveryHdr *pppoedh;

    GREHdr *greh;

    VLANHdr *vlanh;

    /* ptr to the payload of the packet
     * with it's length. */
    uint8_t *payload;
    uint16_t payload_len;

    /* storage: set to pointer to heap and extended via allocation if necessary */
    uint8_t *pkt;
    uint8_t *ext_pkt;
    uint32_t pktlen;

    /* Incoming interface */
    struct LiveDevice_ *livedev;

    PacketAlerts alerts;

    struct Host_ *host_src;
    struct Host_ *host_dst;

    /** packet number in the pcap file, matches wireshark */
    uint64_t pcap_cnt;

    /** mutex to protect access to:
     *  - tunnel_rtv_cnt
     *  - tunnel_tpr_cnt
     */
    SCMutex tunnel_mutex;
    /* ready to set verdict counter, only set in root */
    uint16_t tunnel_rtv_cnt;
    /* tunnel packet ref count */
    uint16_t tunnel_tpr_cnt;

    /* engine events */
    PacketEngineEvents events;

    /* double linked list ptrs */
    struct Packet_ *next;
    struct Packet_ *prev;

    /* tunnel/encapsulation handling */
    struct Packet_ *root; /* in case of tunnel this is a ptr
                           * to the 'real' packet, the one we
                           * need to set the verdict on --
                           * It should always point to the lowest
                           * packet in a encapsulated packet */

    /* required for cuda support */
#ifdef __SC_CUDA_SUPPORT__
    /* indicates if the cuda mpm would be conducted or a normal cpu mpm would
     * be conduced on this packet.  If it is set to 0, the cpu mpm; else cuda mpm */
    uint8_t cuda_mpm_enabled;
    /* indicates if the cuda mpm has finished running the mpm and processed the
     * results for this packet, assuming if cuda_mpm_enabled has been set for this
     * packet */
    uint16_t cuda_done;
    /* used by the detect thread and the cuda mpm dispatcher thread.  The detect
     * thread would wait on this cond var, if the cuda mpm dispatcher thread
     * still hasn't processed the packet.  The dispatcher would use this cond
     * to inform the detect thread(in case it is waiting on this packet), once
     * the dispatcher is done processing the packet results */
    SCMutex cuda_mutex;
    SCCondT cuda_cond;
    /* the extra 1 in the 1481, is to hold the no_of_matches from the mpm run */
    uint16_t mpm_offsets[CUDA_MAX_PAYLOAD_SIZE + 1];
#endif

#ifdef PROFILING
    PktProfiling profile;
#endif
} Packet;

#else

/* __tile__ */
typedef struct Packet_
{
    /* Addresses, Ports and protocol
     * these are on top so we can use
     * the Packet as a hash key */
    Address src;
    Address dst;
    union {
        Port sp;
        uint8_t type;
    };
    union {
        Port dp;
        uint8_t code;
    };
    uint8_t proto;
    /* make sure we can't be attacked on when the tunneled packet
     * has the exact same tuple as the lower levels */
    uint8_t recursion_level;

    /* Pkt Flags */
    uint32_t flags;

    /* flow */
    uint8_t flowflags;

    uint8_t pkt_src;

    struct Flow_ *flow;

    /* data linktype in host order */
    uint16_t datalink;

    /* IPS action to take */
    uint8_t action;

    /* double linked list ptrs */
    struct Packet_ *next;
    struct Packet_ *prev;

    union {
        struct timespec ts_nsec; /* time from mpipe */
        struct timeval ts;
    };

    /* used to hold flowbits only if debuglog is enabled */
    int debuglog_flowbits_names_len;
    const char **debuglog_flowbits_names;

    /** The release function for packet data */
    TmEcode (*ReleaseData)(ThreadVars *, struct Packet_ *);

    EthernetHdr *ethh;

    /* pkt vars */
    PktVar *pktvar;

    /* header pointers */
    IPV4Hdr *ip4h;
    IPV6Hdr *ip6h;
    TCPHdr *tcph;
    UDPHdr *udph;
    SCTPHdr *sctph;
    ICMPV4Hdr *icmpv4h;
    ICMPV6Hdr *icmpv6h;

    PPPHdr *ppph;
    PPPOESessionHdr *pppoesh;
    PPPOEDiscoveryHdr *pppoedh;

    GREHdr *greh;

    VLANHdr *vlanh;

    /* ptr to the payload of the packet
     * with it's length. */
    uint8_t *payload;
    uint16_t payload_len;

    /* storage: set to pointer to heap and extended via allocation if necessary */
    uint8_t *pkt;
    uint8_t *ext_pkt;
    uint32_t pktlen;

    /* Incoming interface */
    struct LiveDevice_ *livedev;

#if 1
    union {
        IPV4Vars ip4vars;
        struct {
            IPV6Vars ip6vars;
            IPV6ExtHdrs ip6eh;
        };
    };
    union {
        TCPVars tcpvars;
        UDPVars udpvars;
        ICMPV4Vars icmpv4vars;
        ICMPV6Vars icmpv6vars;
    };
#else
    IPV4Vars ip4vars /*__attribute__((aligned(64)))*/;
    IPV6Vars ip6vars /*__attribute__((aligned(64)))*/;
    IPV6ExtHdrs ip6eh /*__attribute__((aligned(64)))*/;
    TCPVars tcpvars /*__attribute__((aligned(64)))*/;
    UDPVars udpvars /*__attribute__((aligned(64)))*/;
    ICMPV4Vars icmpv4vars /*__attribute__((aligned(64)))*/;
    ICMPV6Vars icmpv6vars /*__attribute__((aligned(64)))*/;
#endif

    /** packet number in the pcap file, matches wireshark */
    uint64_t pcap_cnt;

    /** mutex to protect access to:
     *  - tunnel_rtv_cnt
     *  - tunnel_tpr_cnt
     */
    SCMutex tunnel_mutex;
    /* ready to set verdict counter, only set in root */
    uint16_t tunnel_rtv_cnt;
    /* tunnel packet ref count */
    uint16_t tunnel_tpr_cnt;

    /* tunnel/encapsulation handling */
    struct Packet_ *root; /* in case of tunnel this is a ptr
                           * to the 'real' packet, the one we
                           * need to set the verdict on --
                           * It should always point to the lowest
                           * packet in a encapsulated packet */

    /* engine events */
    PacketEngineEvents events;
    PacketAlerts alerts;

    struct Host_ *host_src;
    struct Host_ *host_dst;

    /* required for cuda support */
#ifdef __SC_CUDA_SUPPORT__
    /* indicates if the cuda mpm would be conducted or a normal cpu mpm would
     * be conduced on this packet.  If it is set to 0, the cpu mpm; else cuda mpm */
    uint8_t cuda_mpm_enabled;
    /* indicates if the cuda mpm has finished running the mpm and processed the
     * results for this packet, assuming if cuda_mpm_enabled has been set for this
     * packet */
    uint16_t cuda_done;
    /* used by the detect thread and the cuda mpm dispatcher thread.  The detect
     * thread would wait on this cond var, if the cuda mpm dispatcher thread
     * still hasn't processed the packet.  The dispatcher would use this cond
     * to inform the detect thread(in case it is waiting on this packet), once
     * the dispatcher is done processing the packet results */
    SCMutex cuda_mutex;
    SCCondT cuda_cond;
    /* the extra 1 in the 1481, is to hold the no_of_matches from the mpm run */
    uint16_t mpm_offsets[CUDA_MAX_PAYLOAD_SIZE + 1];
#endif

#ifdef PROFILING
    PktProfiling profile;
#endif
    union {
        /* nfq stuff */
#ifdef NFQ
        NFQPacketVars nfq_v;
#endif /* NFQ */

#ifdef IPFW
        IPFWPacketVars ipfw_v;
#endif /* IPFW */

        /** libpcap vars: shared by Pcap Live mode and Pcap File mode */
        PcapPacketVars pcap_v;

#ifdef __tilegx__
        /* tilegx mpipe stuff */
        MpipePacketVars mpipe_v;
#else
        /* Tile64 netio stuff */
    	netio_pkt_t netio_packet;
#endif
    };

} __attribute__((aligned(128))) Packet;

#endif

#define DEFAULT_PACKET_SIZE (1500 + ETHERNET_HEADER_LEN)
/* storage: maximum ip packet size + link header */
#define MAX_PAYLOAD_SIZE (IPV6_HEADER_LEN + 65536 + 28)
uint32_t default_packet_size;
#define SIZE_OF_PACKET (default_packet_size + sizeof(Packet))

typedef struct PacketQueue_ {
    Packet *top;
    Packet *bot;
#ifdef __tile__
    int32_t len;    /* -1 signals termination */
#else
    uint32_t len;
#endif
#ifdef DBG_PERF
    uint32_t dbg_maxlen;
#endif /* DBG_PERF */
#ifdef __tile__
    volatile uint32_t cond_q;
    SCMutex mutex_q /*__attribute__((aligned(64)))*/;
} __attribute__((aligned(64))) PacketQueue;
#else
    SCMutex mutex_q;
    SCCondT cond_q;
} PacketQueue;
#endif

/** \brief Specific ctx for AL proto detection */
typedef struct AlpProtoDetectDirectionThread_ {
    MpmThreadCtx mpm_ctx;
    PatternMatcherQueue pmq;
} AlpProtoDetectDirectionThread;

/** \brief Specific ctx for AL proto detection */
typedef struct AlpProtoDetectThreadCtx_ {
    AlpProtoDetectDirectionThread toserver;
    AlpProtoDetectDirectionThread toclient;

    void *alproto_local_storage[ALPROTO_MAX];

#ifdef PROFILING
    uint64_t ticks_start;
    uint64_t ticks_end;
    uint64_t ticks_spent;
    uint16_t alproto;
    uint64_t proto_detect_ticks_start;
    uint64_t proto_detect_ticks_end;
    uint64_t proto_detect_ticks_spent;
#endif
} AlpProtoDetectThreadCtx;

/** \brief Structure to hold thread specific data for all decode modules */
typedef struct DecodeThreadVars_
{
    /** Specific context for udp protocol detection (here atm) */
    AlpProtoDetectThreadCtx udp_dp_ctx;

    /** stats/counters */
    uint16_t counter_pkts;
    uint16_t counter_pkts_per_sec;
    uint16_t counter_bytes;
    uint16_t counter_bytes_per_sec;
    uint16_t counter_mbit_per_sec;
    uint16_t counter_ipv4;
    uint16_t counter_ipv6;
    uint16_t counter_eth;
    uint16_t counter_sll;
    uint16_t counter_raw;
    uint16_t counter_tcp;
    uint16_t counter_udp;
    uint16_t counter_sctp;
    uint16_t counter_icmpv4;
    uint16_t counter_icmpv6;
    uint16_t counter_ppp;
    uint16_t counter_gre;
    uint16_t counter_vlan;
    uint16_t counter_pppoe;
    uint16_t counter_teredo;
    uint16_t counter_ipv4inipv6;
    uint16_t counter_ipv6inipv6;
    uint16_t counter_avg_pkt_size;
    uint16_t counter_max_pkt_size;

    /** frag stats - defrag runs in the context of the decoder. */
    uint16_t counter_defrag_ipv4_fragments;
    uint16_t counter_defrag_ipv4_reassembled;
    uint16_t counter_defrag_ipv4_timeouts;
    uint16_t counter_defrag_ipv6_fragments;
    uint16_t counter_defrag_ipv6_reassembled;
    uint16_t counter_defrag_ipv6_timeouts;
    uint16_t counter_defrag_max_hit;
} DecodeThreadVars;

/**
 *  \brief reset these to -1(indicates that the packet is fresh from the queue)
 */
#define PACKET_RESET_CHECKSUMS(p) do { \
        (p)->ip4vars.comp_csum = -1;   \
        (p)->tcpvars.comp_csum = -1;      \
        (p)->udpvars.comp_csum = -1;      \
        (p)->icmpv4vars.comp_csum = -1;   \
        (p)->icmpv6vars.comp_csum = -1;   \
    } while (0)

/**
 *  \brief Initialize a packet structure for use.
 */
#ifndef __SC_CUDA_SUPPORT__
#ifdef __tile__
#define PACKET_INITIALIZE(p) { \
    memset((p), 0x00, sizeof(Packet)); \
    SCMutexInit(&(p)->tunnel_mutex, NULL); \
    PACKET_RESET_CHECKSUMS((p)); \
    (p)->pkt = ((uint8_t *)(p)) + sizeof(Packet); \
    (p)->livedev = NULL; \
}
#else
#define PACKET_INITIALIZE(p) { \
    memset((p), 0x00, SIZE_OF_PACKET); \
    SCMutexInit(&(p)->tunnel_mutex, NULL); \
    PACKET_RESET_CHECKSUMS((p)); \
    (p)->pkt = ((uint8_t *)(p)) + sizeof(Packet); \
    (p)->livedev = NULL; \
}
#endif
#else
#ifdef __tile__
#define PACKET_INITIALIZE(p) { \
    memset((p), 0x00, sizeof(Packet)); \
    SCMutexInit(&(p)->tunnel_mutex, NULL); \
    PACKET_RESET_CHECKSUMS((p)); \
    SCMutexInit(&(p)->cuda_mutex, NULL); \
    SCCondInit(&(p)->cuda_cond, NULL); \
    (p)->pkt = ((uint8_t *)(p)) + sizeof(Packet); \
    (p)->livedev = NULL; \
}
#else
#define PACKET_INITIALIZE(p) { \
    memset((p), 0x00, SIZE_OF_PACKET); \
    SCMutexInit(&(p)->tunnel_mutex, NULL); \
    PACKET_RESET_CHECKSUMS((p)); \
    SCMutexInit(&(p)->cuda_mutex, NULL); \
    SCCondInit(&(p)->cuda_cond, NULL); \
    (p)->pkt = ((uint8_t *)(p)) + sizeof(Packet); \
    (p)->livedev = NULL; \
}
#endif
#endif

/**
 *  \brief Recycle a packet structure for reuse.
 *  \todo the mutex destroy & init is necessary because of the memset, reconsider
 */
#ifndef __tile__

#define PACKET_DO_RECYCLE(p) do {               \
        CLEAR_ADDR(&(p)->src);                  \
        CLEAR_ADDR(&(p)->dst);                  \
        (p)->sp = 0;                            \
        (p)->dp = 0;                            \
        (p)->proto = 0;                         \
        (p)->recursion_level = 0;               \
        (p)->flags = 0;                         \
        (p)->flowflags = 0;                     \
        (p)->pkt_src = 0;                       \
        FlowDeReference(&((p)->flow));          \
        (p)->ts.tv_sec = 0;                     \
        (p)->ts.tv_usec = 0;                    \
        (p)->datalink = 0;                      \
        (p)->action = 0;                        \
        if ((p)->pktvar != NULL) {              \
            PktVarFree((p)->pktvar);            \
            (p)->pktvar = NULL;                 \
        }                                       \
        (p)->ethh = NULL;                       \
        if ((p)->ip4h != NULL) {                \
            CLEAR_IPV4_PACKET((p));             \
        }                                       \
        if ((p)->ip6h != NULL) {                \
            CLEAR_IPV6_PACKET((p));             \
        }                                       \
        if ((p)->tcph != NULL) {                \
            CLEAR_TCP_PACKET((p));              \
        }                                       \
        if ((p)->udph != NULL) {                \
            CLEAR_UDP_PACKET((p));              \
        }                                       \
        if ((p)->sctph != NULL) {               \
            CLEAR_SCTP_PACKET((p));             \
        }                                       \
        if ((p)->icmpv4h != NULL) {             \
            CLEAR_ICMPV4_PACKET((p));           \
        }                                       \
        if ((p)->icmpv6h != NULL) {             \
            CLEAR_ICMPV6_PACKET((p));           \
        }                                       \
        (p)->ppph = NULL;                       \
        (p)->pppoesh = NULL;                    \
        (p)->pppoedh = NULL;                    \
        (p)->greh = NULL;                       \
        (p)->vlanh = NULL;                      \
        (p)->payload = NULL;                    \
        (p)->payload_len = 0;                   \
        (p)->pktlen = 0;                        \
        (p)->alerts.cnt = 0;                    \
        HostDeReference(&((p)->host_src));      \
        HostDeReference(&((p)->host_dst));      \
        (p)->pcap_cnt = 0;                      \
        (p)->tunnel_rtv_cnt = 0;                \
        (p)->tunnel_tpr_cnt = 0;                \
        SCMutexDestroy(&(p)->tunnel_mutex);     \
        SCMutexInit(&(p)->tunnel_mutex, NULL);  \
        (p)->events.cnt = 0;                    \
        (p)->next = NULL;                       \
        (p)->prev = NULL;                       \
        (p)->root = NULL;                       \
        (p)->livedev = NULL;                    \
        (p)->ReleaseData = NULL;                \
        PACKET_RESET_CHECKSUMS((p));            \
        PACKET_PROFILING_RESET((p));            \
    } while (0)

#else

/* __tile__ */
#define PACKET_DO_RECYCLE(p) do {               \
        __insn_prefetch(&(p)->pktvar);		\
        __insn_prefetch(&(p)->alerts.cnt); \
        __insn_prefetch(&(p)->events.cnt); \
	uint64_t *llp = (uint64_t *)(p);	\
        __insn_wh64(llp);		\
        __insn_wh64(&llp[8]);		\
	llp[0] = 0;				\
	llp[1] = 0;				\
	llp[2] = 0;				\
	llp[3] = 0;				\
	llp[4] = 0;				\
	llp[5] = 0;				\
	llp[6] = 0;				\
	llp[7] = 0;				\
	llp[8] = 0;				\
	llp[9] = 0;				\
	llp[10] = 0;				\
	llp[11] = 0;				\
	llp[12] = 0;				\
	llp[13] = 0;				\
	llp[14] = 0;				\
	llp[15] = 0;				\
        /*CLEAR_ADDR(&(p)->src);*/                  \
        /*CLEAR_ADDR(&(p)->dst);*/                  \
        /*(p)->sp = 0;*/                            \
        /*(p)->dp = 0;*/                            \
        /*(p)->proto = 0;*/                         \
        /*(p)->recursion_level = 0;*/               \
        /*(p)->flags = 0;*/                         \
        /*(p)->flowflags = 0;*/                     \
        /* (p)->pkt_src = 0; */                     \
        /*(p)->flow = NULL;*/                       \
        /*(p)->ts.tv_sec = 0;*/                     \
        /*(p)->ts.tv_usec = 0;*/                    \
        /*(p)->datalink = 0;*/                      \
        /*(p)->action = 0;*/                        \
        FlowDeReference(&((p)->flow));          \
        if ((p)->pktvar != NULL) {              \
            PktVarFree((p)->pktvar);            \
            (p)->pktvar = NULL;                 \
        }                                       \
        /*(p)->ethh = NULL;*/                       \
        if ((p)->ip4h != NULL) {                \
            CLEAR_IPV4_PACKET((p));             \
        }                                       \
        if ((p)->ip6h != NULL) {                \
            CLEAR_IPV6_PACKET((p));             \
        }                                       \
        if ((p)->tcph != NULL) {                \
            CLEAR_TCP_PACKET((p));              \
        }                                       \
        if ((p)->udph != NULL) {                \
            CLEAR_UDP_PACKET((p));              \
        }                                       \
        if ((p)->sctph != NULL) {                \
            CLEAR_SCTP_PACKET((p));              \
        }                                       \
        if ((p)->icmpv4h != NULL) {             \
            CLEAR_ICMPV4_PACKET((p));           \
        }                                       \
        if ((p)->icmpv6h != NULL) {             \
            CLEAR_ICMPV6_PACKET((p));           \
        }                                       \
        (p)->ppph = NULL;                       \
        (p)->pppoesh = NULL;                    \
        (p)->pppoedh = NULL;                    \
        (p)->greh = NULL;                       \
        (p)->vlanh = NULL;                      \
        (p)->payload = NULL;                    \
        (p)->payload_len = 0;                   \
        (p)->pktlen = 0;                        \
        if (unlikely((p)->alerts.cnt)) (p)->alerts.cnt = 0; \
        HostDeReference(&((p)->host_src));      \
        HostDeReference(&((p)->host_dst));      \
        (p)->pcap_cnt = 0;                      \
        (p)->tunnel_rtv_cnt = 0;                \
        (p)->tunnel_tpr_cnt = 0;                \
        SCMutexDestroy(&(p)->tunnel_mutex);     \
        SCMutexInit(&(p)->tunnel_mutex, NULL);  \
        if ((p)->events.cnt) (p)->events.cnt = 0; \
        /*(p)->next = NULL;*/                   \
        /*(p)->prev = NULL;*/                   \
        (p)->root = NULL;                       \
        (p)->livedev = NULL;                    \
        PACKET_RESET_CHECKSUMS((p));            \
        PACKET_PROFILING_RESET((p));            \
    } while (0)

#endif

#ifndef __SC_CUDA_SUPPORT__
#define PACKET_RECYCLE(p) PACKET_DO_RECYCLE((p))
#else
#define PACKET_RECYCLE(p) do {                  \
    PACKET_DO_RECYCLE((p));                     \
    SCMutexDestroy(&(p)->cuda_mutex);           \
    SCCondDestroy(&(p)->cuda_cond);             \
    SCMutexInit(&(p)->cuda_mutex, NULL);        \
    SCCondInit(&(p)->cuda_cond, NULL);          \
    PACKET_RESET_CHECKSUMS((p));                \
} while(0)
#endif

/**
 *  \brief Cleanup a packet so that we can free it. No memset needed..
 */
#ifndef __SC_CUDA_SUPPORT__
#define PACKET_CLEANUP(p) do {                  \
        if ((p)->pktvar != NULL) {              \
            PktVarFree((p)->pktvar);            \
        }                                       \
        SCMutexDestroy(&(p)->tunnel_mutex);     \
    } while (0)
#else
#define PACKET_CLEANUP(p) do {                  \
    if ((p)->pktvar != NULL) {                  \
        PktVarFree((p)->pktvar);                \
    }                                           \
    SCMutexDestroy(&(p)->tunnel_mutex);         \
    SCMutexDestroy(&(p)->cuda_mutex);           \
    SCCondDestroy(&(p)->cuda_cond);             \
} while(0)
#endif


/* macro's for setting the action
 * handle the case of a root packet
 * for tunnels */
#define ALERT_PACKET(p) do { \
    ((p)->root ? \
     ((p)->root->action = ACTION_ALERT) : \
     ((p)->action = ACTION_ALERT)); \
} while (0)

#define ACCEPT_PACKET(p) do { \
    ((p)->root ? \
     ((p)->root->action = ACTION_ACCEPT) : \
     ((p)->action = ACTION_ACCEPT)); \
} while (0)

#define DROP_PACKET(p) do { \
    ((p)->root ? \
     ((p)->root->action = ACTION_DROP) : \
     ((p)->action = ACTION_DROP)); \
} while (0)

#define REJECT_PACKET(p) do { \
    ((p)->root ? \
     ((p)->root->action = (ACTION_REJECT|ACTION_DROP)) : \
     ((p)->action = (ACTION_REJECT|ACTION_DROP))); \
} while (0)

#define REJECT_PACKET_DST(p) do { \
    ((p)->root ? \
     ((p)->root->action = (ACTION_REJECT_DST|ACTION_DROP)) : \
     ((p)->action = (ACTION_REJECT_DST|ACTION_DROP))); \
} while (0)

#define REJECT_PACKET_BOTH(p) do { \
    ((p)->root ? \
     ((p)->root->action = (ACTION_REJECT_BOTH|ACTION_DROP)) : \
     ((p)->action = (ACTION_REJECT_BOTH|ACTION_DROP))); \
} while (0)

#define PASS_PACKET(p) do { \
    ((p)->root ? \
     ((p)->root->action = ACTION_PASS) : \
     ((p)->action = ACTION_PASS)); \
} while (0)

#define TUNNEL_INCR_PKT_RTV(p) do {                                                 \
        SCMutexLock((p)->root ? &(p)->root->tunnel_mutex : &(p)->tunnel_mutex);     \
        ((p)->root ? (p)->root->tunnel_rtv_cnt++ : (p)->tunnel_rtv_cnt++);          \
        SCMutexUnlock((p)->root ? &(p)->root->tunnel_mutex : &(p)->tunnel_mutex);   \
    } while (0)

#define TUNNEL_INCR_PKT_TPR(p) do {                                                 \
        SCMutexLock((p)->root ? &(p)->root->tunnel_mutex : &(p)->tunnel_mutex);     \
        ((p)->root ? (p)->root->tunnel_tpr_cnt++ : (p)->tunnel_tpr_cnt++);          \
        SCMutexUnlock((p)->root ? &(p)->root->tunnel_mutex : &(p)->tunnel_mutex);   \
    } while (0)

#define TUNNEL_DECR_PKT_TPR(p) do {                                                 \
        SCMutexLock((p)->root ? &(p)->root->tunnel_mutex : &(p)->tunnel_mutex);     \
        ((p)->root ? (p)->root->tunnel_tpr_cnt-- : (p)->tunnel_tpr_cnt--);          \
        SCMutexUnlock((p)->root ? &(p)->root->tunnel_mutex : &(p)->tunnel_mutex);   \
    } while (0)

#define TUNNEL_DECR_PKT_TPR_NOLOCK(p) do {                                          \
        ((p)->root ? (p)->root->tunnel_tpr_cnt-- : (p)->tunnel_tpr_cnt--);          \
    } while (0)

#define TUNNEL_PKT_RTV(p) ((p)->root ? (p)->root->tunnel_rtv_cnt : (p)->tunnel_rtv_cnt)
#define TUNNEL_PKT_TPR(p) ((p)->root ? (p)->root->tunnel_tpr_cnt : (p)->tunnel_tpr_cnt)

#define IS_TUNNEL_PKT(p)            (((p)->flags & PKT_TUNNEL))
#define SET_TUNNEL_PKT(p)           ((p)->flags |= PKT_TUNNEL)
#define IS_TUNNEL_ROOT_PKT(p)       (IS_TUNNEL_PKT(p) && (p)->root == NULL)

#define IS_TUNNEL_PKT_VERDICTED(p)  (((p)->flags & PKT_TUNNEL_VERDICTED))
#define SET_TUNNEL_PKT_VERDICTED(p) ((p)->flags |= PKT_TUNNEL_VERDICTED)


void DecodeRegisterPerfCounters(DecodeThreadVars *, ThreadVars *);
Packet *PacketPseudoPktSetup(Packet *parent, uint8_t *pkt, uint16_t len, uint8_t proto);
Packet *PacketDefragPktSetup(Packet *parent, uint8_t *pkt, uint16_t len, uint8_t proto);
#ifdef __tile__
Packet *PacketGetFromQueueOrAlloc(int pool);
#else
Packet *PacketGetFromQueueOrAlloc(void);
#endif
Packet *PacketGetFromAlloc(void);
int PacketCopyData(Packet *p, uint8_t *pktdata, int pktlen);
int PacketSetData(Packet *p, uint8_t *pktdata, int pktlen);
int PacketCopyDataOffset(Packet *p, int offset, uint8_t *data, int datalen);

DecodeThreadVars *DecodeThreadVarsAlloc(ThreadVars *tv);

/* decoder functions */
void DecodeEthernet(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeSll(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodePPP(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodePPPOESession(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodePPPOEDiscovery(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeTunnel(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *, uint8_t);
void DecodeRaw(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeIPV4(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeIPV6(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeICMPV4(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeICMPV6(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeTCP(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeUDP(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeSCTP(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeGRE(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeVLAN(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);

void AddressDebugPrint(Address *);

/** \brief Set the No payload inspection Flag for the packet.
 *
 * \param p Packet to set the flag in
 */
#define DecodeSetNoPayloadInspectionFlag(p) do { \
        (p)->flags |= PKT_NOPAYLOAD_INSPECTION;  \
    } while (0)

/** \brief Set the No packet inspection Flag for the packet.
 *
 * \param p Packet to set the flag in
 */
#define DecodeSetNoPacketInspectionFlag(p) do { \
        (p)->flags |= PKT_NOPACKET_INSPECTION;  \
    } while (0)


#define ENGINE_SET_EVENT(p, e) do { \
    SCLogDebug("p %p event %d", (p), e); \
    if ((p)->events.cnt < PACKET_ENGINE_EVENT_MAX) { \
        (p)->events.events[(p)->events.cnt] = e; \
        (p)->events.cnt++; \
    } \
} while(0)

#define ENGINE_ISSET_EVENT(p, e) ({ \
    int r = 0; \
    uint8_t u; \
    for (u = 0; u < (p)->events.cnt; u++) { \
        if ((p)->events.events[u] == (e)) { \
            r = 1; \
            break; \
        } \
    } \
    r; \
})

/* older libcs don't contain a def for IPPROTO_DCCP
 * inside of <netinet/in.h>
 * if it isn't defined let's define it here.
 */
#ifndef IPPROTO_DCCP
#define IPPROTO_DCCP 33
#endif

/* older libcs don't contain a def for IPPROTO_SCTP
 * inside of <netinet/in.h>
 * if it isn't defined let's define it here.
 */
#ifndef IPPROTO_SCTP
#define IPPROTO_SCTP 132
#endif

/* pcap provides this, but we don't want to depend on libpcap */
#ifndef DLT_EN10MB
#define DLT_EN10MB 1
#endif

/* taken from pcap's bpf.h */
#ifndef DLT_RAW
#ifdef __OpenBSD__
#define DLT_RAW     14  /* raw IP */
#else
#define DLT_RAW     12  /* raw IP */
#endif
#endif

/** libpcap shows us the way to linktype codes
 * \todo we need more & maybe put them in a separate file? */
#define LINKTYPE_ETHERNET   DLT_EN10MB
#define LINKTYPE_LINUX_SLL  113
#define LINKTYPE_PPP        9
#define LINKTYPE_RAW        DLT_RAW
#define PPP_OVER_GRE        11
#define VLAN_OVER_GRE       13

/*Packet Flags*/
#define PKT_NOPACKET_INSPECTION         (1)         /**< Flag to indicate that packet header or contents should not be inspected*/
#define PKT_NOPAYLOAD_INSPECTION        (1<<2)      /**< Flag to indicate that packet contents should not be inspected*/
#define PKT_ALLOC                       (1<<3)      /**< Packet was alloc'd this run, needs to be freed */
#define PKT_HAS_TAG                     (1<<4)      /**< Packet has matched a tag */
#define PKT_STREAM_ADD                  (1<<5)      /**< Packet payload was added to reassembled stream */
#define PKT_STREAM_EST                  (1<<6)      /**< Packet is part of establised stream */
#define PKT_STREAM_EOF                  (1<<7)      /**< Stream is in eof state */
#define PKT_HAS_FLOW                    (1<<8)
#define PKT_PSEUDO_STREAM_END           (1<<9)      /**< Pseudo packet to end the stream */
#define PKT_STREAM_MODIFIED             (1<<10)     /**< Packet is modified by the stream engine, we need to recalc the csum and reinject/replace */
#define PKT_MARK_MODIFIED               (1<<11)     /**< Packet mark is modified */
#define PKT_STREAM_NOPCAPLOG            (1<<12)     /**< Exclude packet from pcap logging as it's part of a stream that has reassembly depth reached. */

#define PKT_TUNNEL                      (1<<13)
#define PKT_TUNNEL_VERDICTED            (1<<14)

#define PKT_IGNORE_CHECKSUM             (1<<15)     /**< Packet checksum is not computed (TX packet for example) */
#define PKT_ZERO_COPY                   (1<<16)     /**< Packet comes from zero copy (ext_pkt must not be freed) */
#define PKT_NETIO                       (1<<17)     /**< Packet payload from netio */
#define PKT_MPIPE                       (1<<18)     /**< Packet payload from mpipe */

#define PKT_HOST_SRC_LOOKED_UP          (1<<19)
#define PKT_HOST_DST_LOOKED_UP          (1<<20)

/** \brief return 1 if the packet is a pseudo packet */
#define PKT_IS_PSEUDOPKT(p) ((p)->flags & PKT_PSEUDO_STREAM_END)

#define PKT_SET_SRC(p, src_val) ((p)->pkt_src = src_val)

#endif /* __DECODE_H__ */

