/* Copyright (C) 2007-2011 Open Information Security Foundation
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

#ifndef __TM_THREADS_COMMON_H__
#define __TM_THREADS_COMMON_H__

/** \brief Thread Model Module id's.
 *
 *  \note anything added here should also be added to TmModuleTmmIdToString
 *        in tm-modules.c
 */
typedef enum {
    TMM_DECODENFQ,
    TMM_VERDICTNFQ,
    TMM_RECEIVENFQ,
    TMM_RECEIVEPCAP,
    TMM_RECEIVEPCAPFILE,
#ifdef __tilegx__
    TMM_RECEIVEMPIPE,
#elif defined(__tile__)
    TMM_RECEIVENETIO,
#endif
    TMM_DECODEPCAP,
    TMM_DECODEPCAPFILE,
    TMM_RECEIVEPFRING,
    TMM_DECODEPFRING,
#ifdef __tilegx__
    TMM_DECODEMPIPE,
#elif defined(__tile__)
    TMM_DECODENETIO,
#endif
    TMM_DETECT,
    TMM_ALERTFASTLOG,
    TMM_ALERTFASTLOG4,
    TMM_ALERTFASTLOG6,
    TMM_ALERTUNIFIED2ALERT,
    TMM_ALERTPRELUDE,
    TMM_ALERTDEBUGLOG,
    TMM_ALERTSYSLOG,
    TMM_LOGDROPLOG,
    TMM_ALERTSYSLOG4,
    TMM_ALERTSYSLOG6,
    TMM_RESPONDREJECT,
    TMM_LOGHTTPLOG,
    TMM_LOGHTTPLOG4,
    TMM_LOGHTTPLOG6,
    TMM_LOGTLSLOG,
    TMM_LOGTLSLOG4,
    TMM_LOGTLSLOG6,
    TMM_PCAPLOG,
    TMM_FILELOG,
    TMM_FILESTORE,
    TMM_STREAMTCP,
    TMM_DECODEIPFW,
    TMM_VERDICTIPFW,
    TMM_RECEIVEIPFW,
#ifdef __SC_CUDA_SUPPORT__
    TMM_CUDA_MPM_B2G,
    TMM_CUDA_PACKET_BATCHER,
#endif
    TMM_RECEIVEERFFILE,
    TMM_DECODEERFFILE,
    TMM_RECEIVEERFDAG,
    TMM_DECODEERFDAG,
    TMM_RECEIVEAFP,
    TMM_DECODEAFP,
    TMM_ALERTPCAPINFO,
    TMM_RECEIVENAPATECH,
    TMM_DECODENAPATECH,
    TMM_SIZE,
} TmmId;

/*Error codes for the thread modules*/
typedef enum {
    TM_ECODE_OK = 0,    /**< Thread module exits OK*/
    TM_ECODE_FAILED,    /**< Thread module exits due to failure*/
    TM_ECODE_DONE,    /**< Thread module task is finished*/
} TmEcode;

/* ThreadVars type */
enum {
    TVT_PPT,
    TVT_MGMT,
    TVT_CMD,
    TVT_MAX,
};

#endif /* __TM_THREADS_COMMON_H__ */

