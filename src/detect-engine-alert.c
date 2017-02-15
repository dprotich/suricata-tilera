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

#include "suricata-common.h"

#include "detect.h"
#include "detect-engine-alert.h"
#include "detect-engine-threshold.h"
#include "detect-engine-tag.h"

#include "decode.h"

#include "flow.h"
#include "flow-private.h"

/** tag signature we use for tag alerts */
static Signature g_tag_signature;
/** tag packet alert structure for tag alerts */
static PacketAlert g_tag_pa;

void PacketAlertTagInit(void) {
    memset(&g_tag_signature, 0x00, sizeof(g_tag_signature));

    g_tag_signature.id = TAG_SIG_ID;
    g_tag_signature.gid = TAG_SIG_GEN;
    g_tag_signature.num = TAG_SIG_ID;
    g_tag_signature.rev = 1;
    g_tag_signature.prio = 2;

    memset(&g_tag_pa, 0x00, sizeof(g_tag_pa));

    g_tag_pa.order_id = 1000;
    g_tag_pa.action = ACTION_ALERT;
    g_tag_pa.s = &g_tag_signature;
}

PacketAlert *PacketAlertGetTag(void) {
    return &g_tag_pa;
}

/**
 * \brief Handle a packet and check if needs a threshold logic
 *        Also apply rule action if necessary.
 *
 * \param de_ctx Detection Context
 * \param sig Signature pointer
 * \param p Packet structure
 *
 * \retval 1 alert is not suppressed
 * \retval 0 alert is suppressed
 */
static int PacketAlertHandle(DetectEngineCtx *de_ctx, DetectEngineThreadCtx *det_ctx,
                       Signature *s, Packet *p, uint16_t pos)
{
    SCEnter();
    int ret = 1;
    DetectThresholdData *td = NULL;
    SigMatch *sm = NULL;

    if (!(PKT_IS_IPV4(p) || PKT_IS_IPV6(p))) {
        SCReturnInt(1);
    }

    do {
        td = SigGetThresholdTypeIter(s, p, &sm);
        if (td != NULL) {
            SCLogDebug("td %p", td);

            /* PacketAlertThreshold returns 2 if the alert is suppressed but
             * we do need to apply rule actions to the packet. */
            ret = PacketAlertThreshold(de_ctx, det_ctx, td, p, s);
            if (ret == 0 || ret == 2) {
                /* It doesn't match threshold, remove it */
                SCReturnInt(ret);
            }
        }
    } while (sm != NULL);

    SCReturnInt(1);
}


/**
 * \brief Check if a certain sid alerted, this is used in the test functions
 *
 * \param p   Packet on which we want to check if the signature alerted or not
 * \param sid Signature id of the signature that thas to be checked for a match
 *
 * \retval match A value > 0 on a match; 0 on no match
 */
int PacketAlertCheck(Packet *p, uint32_t sid)
{
    uint16_t i = 0;
    int match = 0;

    for (i = 0; i < p->alerts.cnt; i++) {
        if (p->alerts.alerts[i].s == NULL)
            continue;

        if (p->alerts.alerts[i].s->id == sid)
            match++;
    }

    return match;
}

/**
 * \brief Remove alert from the p->alerts.alerts array at pos
 * \param p Pointer to the Packet
 * \param pos Position in the array
 * \retval 0 if the number of alerts is less than pos
 *         1 if all goes well
 */
int PacketAlertRemove(Packet *p, uint16_t pos)
{
    uint16_t i = 0;
    int match = 0;

    if (pos > p->alerts.cnt) {
        SCLogDebug("removing %u failed, pos > cnt %u", pos, p->alerts.cnt);
        return 0;
    }

    for (i = pos; i <= p->alerts.cnt - 1; i++) {
        memcpy(&p->alerts.alerts[i], &p->alerts.alerts[i + 1], sizeof(PacketAlert));
    }

    // Update it, since we removed 1
    p->alerts.cnt--;

    return match;
}

/** \brief append a signature match to a packet
 *
 *  \param det_ctx thread detection engine ctx
 *  \param s the signature that matched
 *  \param p packet
 *  \param flags alert flags
 *  \param alert_msg ptr to StreamMsg object that the signature matched on
 */
int PacketAlertAppend(DetectEngineThreadCtx *det_ctx, Signature *s, Packet *p, uint8_t flags)
{
    int i = 0;

    if (p->alerts.cnt == PACKET_ALERT_MAX)
        return 0;

    SCLogDebug("sid %"PRIu32"", s->id);

    /* It should be usually the last, so check it before iterating */
    if (p->alerts.cnt == 0 || (p->alerts.cnt > 0 &&
                               p->alerts.alerts[p->alerts.cnt - 1].order_id < s->order_id)) {
        /* We just add it */
        p->alerts.alerts[p->alerts.cnt].num = s->num;
        p->alerts.alerts[p->alerts.cnt].order_id = s->order_id;
        p->alerts.alerts[p->alerts.cnt].action = s->action;
        p->alerts.alerts[p->alerts.cnt].flags = flags;
        p->alerts.alerts[p->alerts.cnt].s = s;
    } else {
        /* We need to make room for this s->num
         (a bit ugly with memcpy but we are planning changes here)*/
        for (i = p->alerts.cnt - 1; i >= 0 && p->alerts.alerts[i].order_id > s->order_id; i--) {
            memcpy(&p->alerts.alerts[i + 1], &p->alerts.alerts[i], sizeof(PacketAlert));
        }

        i++; /* The right place to store the alert */

        p->alerts.alerts[i].num = s->num;
        p->alerts.alerts[i].order_id = s->order_id;
        p->alerts.alerts[i].action = s->action;
        p->alerts.alerts[i].flags = flags;
        p->alerts.alerts[i].s = s;
    }

    /* Update the count */
    p->alerts.cnt++;

    return 0;
}

/**
 * \brief Check the threshold of the sigs that match, set actions, break on pass action
 *        This function iterate the packet alerts array, removing those that didn't match
 *        the threshold, and those that match after a signature with the action "pass".
 *        The array is sorted by action priority/order
 * \param de_ctx detection engine context
 * \param det_ctx detection engine thread context
 * \param p pointer to the packet
 */
void PacketAlertFinalize(DetectEngineCtx *de_ctx, DetectEngineThreadCtx *det_ctx, Packet *p) {
    SCEnter();
    int i = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;

    while (i < p->alerts.cnt) {
        SCLogDebug("Sig->num: %"PRIu16, p->alerts.alerts[i].num);
        s = de_ctx->sig_array[p->alerts.alerts[i].num];

        int res = PacketAlertHandle(de_ctx, det_ctx, s, p, i);
        if (res > 0) {
            /* Now, if we have an alert, we have to check if we want
             * to tag this session or src/dst host */
            sm = s->sm_lists[DETECT_SM_LIST_TMATCH];
            while (sm) {
                /* tags are set only for alerts */
                sigmatch_table[sm->type].Match(NULL, det_ctx, p, s, sm);
                sm = sm->next;
            }

            if (s->flags & SIG_FLAG_IPONLY) {
                if (((p->flowflags & FLOW_PKT_TOSERVER) && !(p->flowflags & FLOW_PKT_TOSERVER_IPONLY_SET)) ||
                    ((p->flowflags & FLOW_PKT_TOCLIENT) && !(p->flowflags & FLOW_PKT_TOCLIENT_IPONLY_SET))) {
                    SCLogDebug("testing against \"ip-only\" signatures");

                    if (p->flow != NULL) {
                        /* Update flow flags for iponly */
                        FLOWLOCK_WRLOCK(p->flow);
                        FlowSetIPOnlyFlagNoLock(p->flow, p->flowflags & FLOW_PKT_TOSERVER ? 1 : 0);

                        if (s->action & ACTION_DROP)
                            p->flow->flags |= FLOW_ACTION_DROP;
                        if (s->action & ACTION_REJECT)
                            p->flow->flags |= FLOW_ACTION_DROP;
                        if (s->action & ACTION_REJECT_DST)
                            p->flow->flags |= FLOW_ACTION_DROP;
                        if (s->action & ACTION_REJECT_BOTH)
                            p->flow->flags |= FLOW_ACTION_DROP;
                        if (s->action & ACTION_PASS) {
                            FlowSetNoPacketInspectionFlag(p->flow);
                        }
                        FLOWLOCK_UNLOCK(p->flow);
                    }
                }
            }

            /* set verdict on packet */
            p->action |= p->alerts.alerts[i].action;

            if (p->action & ACTION_PASS) {
                /* Ok, reset the alert cnt to end in the previous of pass
                 * so we ignore the rest with less prio */
                p->alerts.cnt = i;
                break;
            /* if the signature wants to drop, check if the
             * PACKET_ALERT_FLAG_DROP_FLOW flag is set. */
            } else if ((p->action & ACTION_DROP) &&
                    ((p->alerts.alerts[i].flags & PACKET_ALERT_FLAG_DROP_FLOW) ||
                         (s->flags & SIG_FLAG_APPLAYER))
                       && p->flow != NULL)
            {
                FLOWLOCK_WRLOCK(p->flow);
                /* This will apply only on IPS mode (check StreamTcpPacket) */
                p->flow->flags |= FLOW_ACTION_DROP;
                FLOWLOCK_UNLOCK(p->flow);
            }
        }

        /* Thresholding removes this alert */
        if (res == 0 || res == 2) {
            PacketAlertRemove(p, i);

            if (p->alerts.cnt == 0)
                break;
        } else {
            i++;
        }
    }

    /* At this point, we should have all the new alerts. Now check the tag
     * keyword context for sessions and hosts */
    TagHandlePacket(de_ctx, det_ctx, p);
}


