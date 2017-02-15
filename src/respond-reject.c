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
 * \author William Metcalf <william.metcalf@gmail.com>
 *
 * RespondReject is a threaded wrapper for sending Rejects
 *
 * \todo RespondRejectFunc returns 1 on error, 0 on ok... why? For now it should
 *   just return 0 always, error handling is a TODO in the threading model (VJ)
 */

#include "suricata-common.h"
#include "decode.h"
#include "packet-queue.h"
#include "threads.h"
#include "threadvars.h"
#include "tm-queuehandlers.h"
#include "tm-threads.h"
#include "action-globals.h"

#include "respond-reject.h"
#include "respond-reject-libnet11.h"

#include "util-debug.h"
#include "util-privs.h"

int RejectSendIPv4TCP(ThreadVars *, Packet *, void *);
int RejectSendIPv4ICMP(ThreadVars *, Packet *, void *);
int RejectSendIPv6TCP(ThreadVars *, Packet *, void *);
int RejectSendIPv6ICMP(ThreadVars *, Packet *, void *);

void TmModuleRespondRejectRegister (void) {

    tmm_modules[TMM_RESPONDREJECT].name = "RespondReject";
    tmm_modules[TMM_RESPONDREJECT].ThreadInit = NULL;
    tmm_modules[TMM_RESPONDREJECT].Func = RespondRejectFunc;
    tmm_modules[TMM_RESPONDREJECT].ThreadDeinit = NULL;
    tmm_modules[TMM_RESPONDREJECT].RegisterTests = NULL;
    tmm_modules[TMM_RESPONDREJECT].cap_flags = 0; /* libnet is not compat with caps */
}

TmEcode RespondRejectFunc(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq) {
    int ret = 0;

    /* ACTION_REJECT defaults to rejecting the SRC */
    if (!(p->action & ACTION_REJECT) &&
        !(p->action & ACTION_REJECT_DST) &&
        !(p->action & ACTION_REJECT_BOTH)) {
        return TM_ECODE_OK;
    }

    if (PKT_IS_IPV4(p)) {
        if (PKT_IS_TCP(p)) {
            ret = RejectSendIPv4TCP(tv, p, data);
        } else if(PKT_IS_UDP(p)) {
            ret = RejectSendIPv4ICMP(tv, p, data);
        } else {
            return TM_ECODE_OK;
        }
    } else if (PKT_IS_IPV6(p)) {
        if (PKT_IS_TCP(p)) {
            ret = RejectSendIPv6TCP(tv, p, data);
        } else if(PKT_IS_UDP(p)){
            ret = RejectSendIPv6ICMP(tv, p, data);
        } else {
            return TM_ECODE_OK;
        }
    } else {
        /* we're only supporting IPv4 and IPv6 */
        return TM_ECODE_OK;
    }

    if (ret)
        return TM_ECODE_FAILED;
    else
        return TM_ECODE_OK;
}

int RejectSendIPv4TCP(ThreadVars *tv, Packet *p, void *data) {
    if (p->action & ACTION_REJECT) {
        return RejectSendLibnet11L3IPv4TCP(tv, p, data, REJECT_DIR_SRC);
    } else if (p->action & ACTION_REJECT_DST) {
        return RejectSendLibnet11L3IPv4TCP(tv, p, data, REJECT_DIR_DST);
    } else if(p->action & ACTION_REJECT_BOTH) {
        if (RejectSendLibnet11L3IPv4TCP(tv, p, data, REJECT_DIR_SRC) == 0 &&
            RejectSendLibnet11L3IPv4TCP(tv, p, data, REJECT_DIR_DST) == 0) {
            return 0;
        } else {
            return 1;
        }
    }
    return 0;
}

int RejectSendIPv4ICMP(ThreadVars *tv, Packet *p, void *data) {
    if (p->action & ACTION_REJECT) {
        return RejectSendLibnet11L3IPv4ICMP(tv, p, data, REJECT_DIR_SRC);
    } else if (p->action & ACTION_REJECT_DST) {
        return RejectSendLibnet11L3IPv4ICMP(tv, p, data, REJECT_DIR_DST);
    } else if(p->action & ACTION_REJECT_BOTH) {
        if (RejectSendLibnet11L3IPv4ICMP(tv, p, data, REJECT_DIR_SRC) == 0 &&
            RejectSendLibnet11L3IPv4ICMP(tv, p, data, REJECT_DIR_DST) == 0) {
            return 0;
        } else {
            return 1;
        }
    }
    return 0;
}

/** \todo implement */
int RejectSendIPv6TCP(ThreadVars *tv, Packet *p, void *data) {
    SCEnter();
    SCLogDebug("we would send a ipv6 tcp reset here");
    SCReturnInt(0);
}

/** \todo implement */
int RejectSendIPv6ICMP(ThreadVars *tv, Packet *p, void *data) {
    SCEnter();
    SCLogDebug("we would send a ipv6 icmp reset here");
    SCReturnInt(0);
}

