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
 *
 * \todo figure out a way to (thread) safely print detection engine info
 * \todo maybe by having a log queue in the packet
 * \todo maybe by accessing it just and hoping threading doesn't hurt
 */

#include "suricata-common.h"
#include "suricata.h"

#include "debug.h"
#include "detect.h"
#include "flow.h"
#include "conf.h"
#include "stream.h"
#include "app-layer-protos.h"

#include "threads.h"
#include "threadvars.h"
#include "tm-threads.h"

#include "util-print.h"

#include "pkt-var.h"

#include "util-unittest.h"

#include "util-debug.h"
#include "util-buffer.h"

#include "output.h"
#include "alert-debuglog.h"
#include "util-privs.h"
#include "flow-var.h"
#include "flow-bit.h"
#include "util-var-name.h"
#include "util-optimize.h"
#include "util-logopenfile.h"

#define DEFAULT_LOG_FILENAME "alert-debug.log"

#define MODULE_NAME "AlertDebugLog"

TmEcode AlertDebugLog (ThreadVars *, Packet *, void *, PacketQueue *, PacketQueue *);
TmEcode AlertDebugLogIPv4(ThreadVars *, Packet *, void *, PacketQueue *, PacketQueue *);
TmEcode AlertDebugLogIPv6(ThreadVars *, Packet *, void *, PacketQueue *, PacketQueue *);
TmEcode AlertDebugLogThreadInit(ThreadVars *, void*, void **);
TmEcode AlertDebugLogThreadDeinit(ThreadVars *, void *);
void AlertDebugLogExitPrintStats(ThreadVars *, void *);

void TmModuleAlertDebugLogRegister (void) {
    tmm_modules[TMM_ALERTDEBUGLOG].name = MODULE_NAME;
    tmm_modules[TMM_ALERTDEBUGLOG].ThreadInit = AlertDebugLogThreadInit;
    tmm_modules[TMM_ALERTDEBUGLOG].Func = AlertDebugLog;
    tmm_modules[TMM_ALERTDEBUGLOG].ThreadExitPrintStats = AlertDebugLogExitPrintStats;
    tmm_modules[TMM_ALERTDEBUGLOG].ThreadDeinit = AlertDebugLogThreadDeinit;
    tmm_modules[TMM_ALERTDEBUGLOG].RegisterTests = NULL;
    tmm_modules[TMM_ALERTDEBUGLOG].cap_flags = 0;

    OutputRegisterModule(MODULE_NAME, "alert-debug", AlertDebugLogInitCtx);
}

typedef struct AlertDebugLogThread_ {
    LogFileCtx *file_ctx;
    /** LogFileCtx has the pointer to the file and a mutex to allow multithreading */
    MemBuffer *buffer;
} AlertDebugLogThread;

static void CreateTimeString (const struct timeval *ts, char *str, size_t size) {
    time_t time = ts->tv_sec;
    struct tm local_tm;
    struct tm *t = (struct tm*)SCLocalTime(time, &local_tm);

    snprintf(str, size, "%02d/%02d/%02d-%02d:%02d:%02d.%06u",
        t->tm_mon + 1, t->tm_mday, t->tm_year + 1900, t->tm_hour,
            t->tm_min, t->tm_sec, (uint32_t) ts->tv_usec);
}

/**
 *  \brief Function to log the FlowVars in to alert-debug.log
 *
 *  \param aft Pointer to AltertDebugLog Thread
 *  \param p Pointer to the packet
 *
 */
static void AlertDebugLogFlowVars(AlertDebugLogThread *aft, Packet *p)
{
    GenericVar *gv = p->flow->flowvar;
    uint16_t i;
    while (gv != NULL) {
        if (gv->type == DETECT_FLOWVAR || gv->type == DETECT_FLOWINT) {
            FlowVar *fv = (FlowVar *) gv;

            if (fv->datatype == FLOWVAR_TYPE_STR) {
                MemBufferWriteString(aft->buffer, "FLOWVAR idx(%"PRIu32"):    ",
                                     fv->idx);
                for (i = 0; i < fv->data.fv_str.value_len; i++) {
                    if (isprint(fv->data.fv_str.value[i])) {
                        MemBufferWriteString(aft->buffer, "%c",
                                             fv->data.fv_str.value[i]);
                    } else {
                        MemBufferWriteString(aft->buffer, "\\%02X",
                                             fv->data.fv_str.value[i]);
                    }
                }
            } else if (fv->datatype == FLOWVAR_TYPE_INT) {
                MemBufferWriteString(aft->buffer, "FLOWVAR idx(%"PRIu32"):   "
                        " %" PRIu32 "\"", fv->idx, fv->data.fv_int.value);
            }
        }
        gv = gv->next;
    }
}

/**
 *  \brief Function to log the FlowBits in to alert-debug.log
 *
 *  \param aft Pointer to AltertDebugLog Thread
 *  \param p Pointer to the packet
 *
 */
static void AlertDebugLogFlowBits(AlertDebugLogThread *aft, Packet *p)
{
    int i;
    for (i = 0; i < p->debuglog_flowbits_names_len; i++) {
        if (p->debuglog_flowbits_names[i] != NULL) {
            MemBufferWriteString(aft->buffer, "FLOWBIT:           %s\n",
                                 p->debuglog_flowbits_names[i]);
        }
    }

    SCFree(p->debuglog_flowbits_names);
    p->debuglog_flowbits_names = NULL;
    p->debuglog_flowbits_names_len = 0;

    return;
}

/**
 *  \brief Function to log the PktVars in to alert-debug.log
 *
 *  \param aft Pointer to AltertDebugLog Thread
 *  \param p Pointer to the packet
 *
 */
static void AlertDebugLogPktVars(AlertDebugLogThread *aft, Packet *p)
{
    PktVar *pv = p->pktvar;

    while(pv != NULL) {
        MemBufferWriteString(aft->buffer, "PKTVAR:            %s\n", pv->name);
        PrintRawDataToBuffer(aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                             pv->value, pv->value_len);
        pv = pv->next;
    }
}

/** \todo doc
 * assume we have aft lock */
static int AlertDebugPrintStreamSegmentCallback(Packet *p, void *data, uint8_t *buf, uint32_t buflen)
{
    AlertDebugLogThread *aft = (AlertDebugLogThread *)data;

    MemBufferWriteString(aft->buffer, "STREAM DATA LEN:     %"PRIu32"\n", buflen);
    MemBufferWriteString(aft->buffer, "STREAM DATA:\n");

    PrintRawDataToBuffer(aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                         buf, buflen);

    return 1;
}



TmEcode AlertDebugLogger(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    AlertDebugLogThread *aft = (AlertDebugLogThread *)data;
    int i;
    char timebuf[64];

    if (p->alerts.cnt == 0)
        return TM_ECODE_OK;

    MemBufferReset(aft->buffer);

    CreateTimeString(&p->ts, timebuf, sizeof(timebuf));

    MemBufferWriteString(aft->buffer, "+================\n"
                         "TIME:              %s\n", timebuf);
    if (p->pcap_cnt > 0) {
        MemBufferWriteString(aft->buffer, "PCAP PKT NUM:      %"PRIu64"\n", p->pcap_cnt);
    }

    char srcip[46], dstip[46];
    if (PKT_IS_IPV4(p)) {
        PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p), srcip, sizeof(srcip));
        PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p), dstip, sizeof(dstip));
    } else if (PKT_IS_IPV6(p)) {
        PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p), srcip, sizeof(srcip));
        PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p), dstip, sizeof(dstip));
    }

    MemBufferWriteString(aft->buffer, "SRC IP:            %s\n"
                         "DST IP:            %s\n"
                         "PROTO:             %" PRIu32 "\n",
                         srcip, dstip, p->proto);
    if (PKT_IS_TCP(p) || PKT_IS_UDP(p)) {
        MemBufferWriteString(aft->buffer, "SRC PORT:          %" PRIu32 "\n"
                             "DST PORT:          %" PRIu32 "\n",
                             p->sp, p->dp);
        if (PKT_IS_TCP(p)) {
            MemBufferWriteString(aft->buffer, "TCP SEQ:           %"PRIu32"\n"
                                 "TCP ACK:           %"PRIu32"\n",
                                 TCP_GET_SEQ(p), TCP_GET_ACK(p));
        }
    }

    /* flow stuff */
    MemBufferWriteString(aft->buffer, "FLOW:              to_server: %s, "
                         "to_client: %s\n",
                         p->flowflags & FLOW_PKT_TOSERVER ? "TRUE" : "FALSE",
                         p->flowflags & FLOW_PKT_TOCLIENT ? "TRUE" : "FALSE");

    if (p->flow != NULL) {
        FLOWLOCK_RDLOCK(p->flow);
        CreateTimeString(&p->flow->startts, timebuf, sizeof(timebuf));
        MemBufferWriteString(aft->buffer, "FLOW Start TS:     %s\n", timebuf);
#ifdef DEBUG
        MemBufferWriteString(aft->buffer, "FLOW PKTS TODST:   %"PRIu32"\n"
                             "FLOW PKTS TOSRC:   %"PRIu32"\n"
                             "FLOW Total Bytes:  %"PRIu64"\n",
                             p->flow->todstpktcnt, p->flow->tosrcpktcnt,
                             p->flow->bytecnt);
#endif
        MemBufferWriteString(aft->buffer,
                             "FLOW IPONLY SET:   TOSERVER: %s, TOCLIENT: %s\n"
                             "FLOW ACTION:       DROP: %s\n"
                             "FLOW NOINSPECTION: PACKET: %s, PAYLOAD: %s, APP_LAYER: %s\n"
                             "FLOW APP_LAYER:    DETECTED: %s, PROTO %"PRIu16"\n",
                             p->flow->flags & FLOW_TOSERVER_IPONLY_SET ? "TRUE" : "FALSE",
                             p->flow->flags & FLOW_TOCLIENT_IPONLY_SET ? "TRUE" : "FALSE",
                             p->flow->flags & FLOW_ACTION_DROP ? "TRUE" : "FALSE",
                             p->flow->flags & FLOW_NOPACKET_INSPECTION ? "TRUE" : "FALSE",
                             p->flow->flags & FLOW_NOPAYLOAD_INSPECTION ? "TRUE" : "FALSE",
                             p->flow->flags & FLOW_NO_APPLAYER_INSPECTION ? "TRUE" : "FALSE",
                             (p->flow->alproto != ALPROTO_UNKNOWN) ? "TRUE" : "FALSE", p->flow->alproto);
        AlertDebugLogFlowVars(aft, p);
        AlertDebugLogFlowBits(aft, p);
        FLOWLOCK_UNLOCK(p->flow);
    }

    AlertDebugLogPktVars(aft, p);

/* any stuff */
/* Sig details? */

    MemBufferWriteString(aft->buffer,
                         "PACKET LEN:        %" PRIu32 "\n"
                         "PACKET:\n",
                         GET_PKT_LEN(p));
    PrintRawDataToBuffer(aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                         GET_PKT_DATA(p), GET_PKT_LEN(p));

    MemBufferWriteString(aft->buffer, "ALERT CNT:           %" PRIu32 "\n",
                         p->alerts.cnt);

    for (i = 0; i < p->alerts.cnt; i++) {
        PacketAlert *pa = &p->alerts.alerts[i];
        if (unlikely(pa->s == NULL)) {
            continue;
        }

        MemBufferWriteString(aft->buffer,
                             "ALERT MSG [%02d]:      %s\n"
                             "ALERT GID [%02d]:      %" PRIu32 "\n"
                             "ALERT SID [%02d]:      %" PRIu32 "\n"
                             "ALERT REV [%02d]:      %" PRIu32 "\n"
                             "ALERT CLASS [%02d]:    %s\n"
                             "ALERT PRIO [%02d]:     %" PRIu32 "\n"
                             "ALERT FOUND IN [%02d]: %s\n",
                             i, pa->s->msg,
                             i, pa->s->gid,
                             i, pa->s->id,
                             i, pa->s->rev,
                             i, pa->s->class_msg ? pa->s->class_msg : "<none>",
                             i, pa->s->prio,
                             i,
                             pa->flags & PACKET_ALERT_FLAG_STREAM_MATCH  ? "STREAM" :
                             (pa->flags & PACKET_ALERT_FLAG_STATE_MATCH ? "STATE" : "PACKET"));
        if (p->payload_len > 0) {
            MemBufferWriteString(aft->buffer,
                                 "PAYLOAD LEN:         %" PRIu32 "\n"
                                 "PAYLOAD:\n",
                                 p->payload_len);
            PrintRawDataToBuffer(aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                                 p->payload, p->payload_len);
        }
        if ((pa->flags & PACKET_ALERT_FLAG_STATE_MATCH) ||
            (pa->flags & PACKET_ALERT_FLAG_STREAM_MATCH)) {
            /* This is an app layer or stream alert */
            int ret;
            uint8_t flag;
            if (!(PKT_IS_TCP(p)) || p->flow == NULL ||
                    p->flow->protoctx == NULL) {
                return TM_ECODE_OK;
            }
            /* IDS mode reverse the data */
            /** \todo improve the order selection policy */
            if (p->flowflags & FLOW_PKT_TOSERVER) {
                flag = FLOW_PKT_TOCLIENT;
            } else {
                flag = FLOW_PKT_TOSERVER;
            }
            ret = StreamSegmentForEach(p, flag,
                                 AlertDebugPrintStreamSegmentCallback,
                                 (void *)aft);
            if (ret < 0) {
                return TM_ECODE_FAILED;
            }
        }
    }

    SCMutexLock(&aft->file_ctx->fp_mutex);
    (void)MemBufferPrintToFPAsString(aft->buffer, aft->file_ctx->fp);
    fflush(aft->file_ctx->fp);
    aft->file_ctx->alerts += p->alerts.cnt;
    SCMutexUnlock(&aft->file_ctx->fp_mutex);

    return TM_ECODE_OK;
}

TmEcode AlertDebugLogDecoderEvent(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    AlertDebugLogThread *aft = (AlertDebugLogThread *)data;
    int i;
    char timebuf[64];

    if (p->alerts.cnt == 0)
        return TM_ECODE_OK;

    MemBufferReset(aft->buffer);

    CreateTimeString(&p->ts, timebuf, sizeof(timebuf));

    MemBufferWriteString(aft->buffer,
                         "+================\n"
                         "TIME:              %s\n", timebuf);
    if (p->pcap_cnt > 0) {
        MemBufferWriteString(aft->buffer,
                             "PCAP PKT NUM:      %"PRIu64"\n", p->pcap_cnt);
    }
    MemBufferWriteString(aft->buffer,
                         "ALERT CNT:         %" PRIu32 "\n", p->alerts.cnt);

    for (i = 0; i < p->alerts.cnt; i++) {
        PacketAlert *pa = &p->alerts.alerts[i];
        if (unlikely(pa->s == NULL)) {
            continue;
        }

        MemBufferWriteString(aft->buffer,
                             "ALERT MSG [%02d]:    %s\n"
                             "ALERT GID [%02d]:    %" PRIu32 "\n"
                             "ALERT SID [%02d]:    %" PRIu32 "\n"
                             "ALERT REV [%02d]:    %" PRIu32 "\n"
                             "ALERT CLASS [%02d]:  %s\n"
                             "ALERT PRIO [%02d]:   %" PRIu32 "\n",
                             i, pa->s->msg,
                             i, pa->s->gid,
                             i, pa->s->id,
                             i, pa->s->rev,
                             i, pa->s->class_msg,
                             i, pa->s->prio);
    }

    MemBufferWriteString(aft->buffer,
                         "PACKET LEN:        %" PRIu32 "\n"
                         "PACKET:\n",
                         GET_PKT_LEN(p));
    PrintRawDataToBuffer(aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                         GET_PKT_DATA(p), GET_PKT_LEN(p));

    SCMutexLock(&aft->file_ctx->fp_mutex);
    (void)MemBufferPrintToFPAsString(aft->buffer, aft->file_ctx->fp);
    fflush(aft->file_ctx->fp);
    aft->file_ctx->alerts += p->alerts.cnt;
    SCMutexUnlock(&aft->file_ctx->fp_mutex);

    return TM_ECODE_OK;
}

TmEcode AlertDebugLog (ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    if (PKT_IS_IPV4(p)) {
        return AlertDebugLogger(tv, p, data, pq, postpq);
    } else if (PKT_IS_IPV6(p)) {
        return AlertDebugLogger(tv, p, data, pq, postpq);
    } else if (p->events.cnt > 0) {
        return AlertDebugLogDecoderEvent(tv, p, data, pq, postpq);
    }

    return TM_ECODE_OK;
}

TmEcode AlertDebugLogThreadInit(ThreadVars *t, void *initdata, void **data)
{
    AlertDebugLogThread *aft = SCMalloc(sizeof(AlertDebugLogThread));
    if (unlikely(aft == NULL))
        return TM_ECODE_FAILED;
    memset(aft, 0, sizeof(AlertDebugLogThread));

    if(initdata == NULL)
    {
        SCLogDebug("Error getting context for DebugLog.  \"initdata\" argument NULL");
        SCFree(aft);
        return TM_ECODE_FAILED;
    }
    /** Use the Ouptut Context (file pointer and mutex) */
    aft->file_ctx = ((OutputCtx *)initdata)->data;

    /* 1 mb seems sufficient enough */
    aft->buffer = MemBufferCreateNew(1 * 1024 * 1024);
    if (aft->buffer == NULL) {
        SCFree(aft);
        return TM_ECODE_FAILED;
    }

    *data = (void *)aft;
    return TM_ECODE_OK;
}

TmEcode AlertDebugLogThreadDeinit(ThreadVars *t, void *data)
{
    AlertDebugLogThread *aft = (AlertDebugLogThread *)data;
    if (aft == NULL) {
        return TM_ECODE_OK;
    }

    MemBufferFree(aft->buffer);
    /* clear memory */
    memset(aft, 0, sizeof(AlertDebugLogThread));

    SCFree(aft);
    return TM_ECODE_OK;
}

void AlertDebugLogExitPrintStats(ThreadVars *tv, void *data) {
    AlertDebugLogThread *aft = (AlertDebugLogThread *)data;
    if (aft == NULL) {
        return;
    }

    SCLogInfo("(%s) Alerts %" PRIu64 "", tv->name, aft->file_ctx->alerts);
}

static void AlertDebugLogDeInitCtx(OutputCtx *output_ctx)
{
    if (output_ctx != NULL) {
        LogFileCtx *logfile_ctx = (LogFileCtx *)output_ctx->data;
        if (logfile_ctx != NULL) {
            LogFileFreeCtx(logfile_ctx);
        }
        SCFree(output_ctx);
    }
}

/**
 *  \brief Create a new LogFileCtx for alert debug logging.
 *
 *  \param ConfNode containing configuration for this logger.
 *
 *  \return output_ctx if succesful, NULL otherwise
 */
OutputCtx *AlertDebugLogInitCtx(ConfNode *conf)
{
    LogFileCtx *file_ctx = NULL;

    file_ctx = LogFileNewCtx();
    if (file_ctx == NULL) {
        SCLogDebug("couldn't create new file_ctx");
        goto error;
    }

    if (SCConfLogOpenGeneric(conf, file_ctx, DEFAULT_LOG_FILENAME) < 0) {
        goto error;
    }

    OutputCtx *output_ctx = SCMalloc(sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL))
        goto error;

    memset(output_ctx, 0x00, sizeof(OutputCtx));
    output_ctx->data = file_ctx;
    output_ctx->DeInit = AlertDebugLogDeInitCtx;

    SCLogDebug("Alert debug log output initialized");
    return output_ctx;

error:
    if (file_ctx != NULL) {
        LogFileFreeCtx(file_ctx);
    }

    return NULL;
}
