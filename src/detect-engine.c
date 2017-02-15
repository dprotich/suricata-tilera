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

#include "suricata-common.h"
#include "suricata.h"
#include "debug.h"
#include "detect.h"
#include "flow.h"
#include "conf.h"
#include "conf-yaml-loader.h"

#include "app-layer-htp.h"

#include "detect-parse.h"
#include "detect-engine-sigorder.h"

#include "detect-engine-siggroup.h"
#include "detect-engine-address.h"
#include "detect-engine-port.h"
#include "detect-engine-mpm.h"
#include "detect-engine-hcbd.h"
#include "detect-engine-iponly.h"
#include "detect-engine-tag.h"

#include "detect-engine-uri.h"
#include "detect-engine-hcbd.h"
#include "detect-engine-hsbd.h"
#include "detect-engine-hhd.h"
#include "detect-engine-hrhd.h"
#include "detect-engine-hmd.h"
#include "detect-engine-hcd.h"
#include "detect-engine-hrud.h"
#include "detect-engine-hsmd.h"
#include "detect-engine-hscd.h"
#include "detect-engine-hua.h"
#include "detect-engine-hhhd.h"
#include "detect-engine-hrhhd.h"
#include "detect-engine-file.h"

#include "detect-engine.h"
#include "detect-engine-state.h"

#include "detect-byte-extract.h"
#include "detect-content.h"
#include "detect-uricontent.h"
#include "detect-engine-threshold.h"

#include "util-classification-config.h"
#include "util-reference-config.h"
#include "util-threshold-config.h"
#include "util-error.h"
#include "util-hash.h"
#include "util-byte.h"
#include "util-debug.h"
#include "util-unittest.h"
#include "util-action.h"
#include "util-magic.h"
#include "util-signal.h"

#include "util-var-name.h"

#include "tm-threads.h"
#include "runmodes.h"

#ifdef PROFILING
#include "util-profiling.h"
#endif

#include "reputation.h"

#define DETECT_ENGINE_DEFAULT_INSPECTION_RECURSION_LIMIT 3000

static uint32_t detect_engine_ctx_id = 1;

static TmEcode DetectEngineThreadCtxInitForLiveRuleSwap(ThreadVars *, void *, void **);

static uint8_t DetectEngineCtxLoadConf(DetectEngineCtx *);

/* 2 - for each direction */
DetectEngineAppInspectionEngine *app_inspection_engine[ALPROTO_MAX][2];

#if 0

static void DetectEnginePrintAppInspectionEngines(DetectEngineAppInspectionEngine *list[][2])
{
    printf("\n");

    uint16_t alproto = ALPROTO_UNKNOWN + 1;
    for ( ; alproto < ALPROTO_MAX; alproto++) {
        printf("alproto - %d\n", alproto);
        int dir = 0;
        for ( ; dir < 2; dir++) {
            printf("  direction - %d\n", dir);
            DetectEngineAppInspectionEngine *engine = list[alproto][dir];
            while (engine != NULL) {
                printf("    engine->alproto - %"PRIu16"\n", engine->alproto);
                printf("    engine->dir - %"PRIu16"\n", engine->dir);
                printf("    engine->sm_list - %d\n", engine->sm_list);
                printf("    engine->inspect_flags - %"PRIu32"\n", engine->inspect_flags);
                printf("    engine->match_flags - %"PRIu32"\n", engine->match_flags);
                printf("\n");

                engine = engine->next;
            }
        } /* for ( ; dir < 2; dir++) */
    } /* for ( ; alproto < ALPROTO_MAX; alproto++) */

    return;
}

#endif

void DetectEngineRegisterAppInspectionEngines(void)
{
    struct tmp_t {
        uint16_t alproto;
        int32_t sm_list;
        uint32_t inspect_flags;
        uint32_t match_flags;
        uint16_t dir;
        int (*Callback)(ThreadVars *tv,
                        DetectEngineCtx *de_ctx,
                        DetectEngineThreadCtx *det_ctx,
                        Signature *sig, Flow *f,
                        uint8_t flags, void *alstate,
                        int32_t tx_id);

    };

    struct tmp_t data_toserver[] = {
        { ALPROTO_HTTP,
          DETECT_SM_LIST_UMATCH,
          DE_STATE_FLAG_URI_INSPECT,
          DE_STATE_FLAG_URI_MATCH,
          0,
          DetectEngineInspectPacketUris },
        { ALPROTO_HTTP,
          DETECT_SM_LIST_HCBDMATCH,
          DE_STATE_FLAG_HCBD_INSPECT,
          DE_STATE_FLAG_HCBD_MATCH,
          0,
          DetectEngineInspectHttpClientBody },
        { ALPROTO_HTTP,
          DETECT_SM_LIST_HHDMATCH,
          DE_STATE_FLAG_HHD_INSPECT,
          DE_STATE_FLAG_HHD_MATCH,
          0,
          DetectEngineInspectHttpHeader },
        { ALPROTO_HTTP,
          DETECT_SM_LIST_HRHDMATCH,
          DE_STATE_FLAG_HRHD_INSPECT,
          DE_STATE_FLAG_HRHD_MATCH,
          0,
          DetectEngineInspectHttpRawHeader },
        { ALPROTO_HTTP,
          DETECT_SM_LIST_HMDMATCH,
          DE_STATE_FLAG_HMD_INSPECT,
          DE_STATE_FLAG_HMD_MATCH,
          0,
          DetectEngineInspectHttpMethod },
        { ALPROTO_HTTP,
          DETECT_SM_LIST_HCDMATCH,
          DE_STATE_FLAG_HCD_INSPECT,
          DE_STATE_FLAG_HCD_MATCH,
          0,
          DetectEngineInspectHttpCookie },
        { ALPROTO_HTTP,
          DETECT_SM_LIST_HRUDMATCH,
          DE_STATE_FLAG_HRUD_INSPECT,
          DE_STATE_FLAG_HRUD_MATCH,
          0,
          DetectEngineInspectHttpRawUri },
        { ALPROTO_HTTP,
          DETECT_SM_LIST_FILEMATCH,
          DE_STATE_FLAG_FILE_TS_INSPECT,
          DE_STATE_FLAG_FILE_TS_MATCH,
          0,
          DetectFileInspectHttp },
        { ALPROTO_HTTP,
          DETECT_SM_LIST_HUADMATCH,
          DE_STATE_FLAG_HUAD_INSPECT,
          DE_STATE_FLAG_HUAD_MATCH,
          0,
          DetectEngineInspectHttpUA },
        { ALPROTO_HTTP,
          DETECT_SM_LIST_HHHDMATCH,
          DE_STATE_FLAG_HHHD_INSPECT,
          DE_STATE_FLAG_HHHD_MATCH,
          0,
          DetectEngineInspectHttpHH },
        { ALPROTO_HTTP,
          DETECT_SM_LIST_HRHHDMATCH,
          DE_STATE_FLAG_HRHHD_INSPECT,
          DE_STATE_FLAG_HRHHD_MATCH,
          0,
          DetectEngineInspectHttpHRH },
    };

    struct tmp_t data_toclient[] = {
        { ALPROTO_HTTP,
          DETECT_SM_LIST_HSBDMATCH,
          DE_STATE_FLAG_HSBD_INSPECT,
          DE_STATE_FLAG_HSBD_MATCH,
          1,
          DetectEngineInspectHttpServerBody },
        { ALPROTO_HTTP,
          DETECT_SM_LIST_HHDMATCH,
          DE_STATE_FLAG_HHD_INSPECT,
          DE_STATE_FLAG_HHD_MATCH,
          1,
          DetectEngineInspectHttpHeader },
        { ALPROTO_HTTP,
          DETECT_SM_LIST_HRHDMATCH,
          DE_STATE_FLAG_HRHD_INSPECT,
          DE_STATE_FLAG_HRHD_MATCH,
          1,
          DetectEngineInspectHttpRawHeader },
        { ALPROTO_HTTP,
          DETECT_SM_LIST_HCDMATCH,
          DE_STATE_FLAG_HCD_INSPECT,
          DE_STATE_FLAG_HCD_MATCH,
          1,
          DetectEngineInspectHttpCookie },
        { ALPROTO_HTTP,
          DETECT_SM_LIST_FILEMATCH,
          DE_STATE_FLAG_FILE_TC_INSPECT,
          DE_STATE_FLAG_FILE_TC_MATCH,
          1,
          DetectFileInspectHttp },
        { ALPROTO_HTTP,
          DETECT_SM_LIST_HSMDMATCH,
          DE_STATE_FLAG_HSMD_INSPECT,
          DE_STATE_FLAG_HSMD_MATCH,
          1,
          DetectEngineInspectHttpStatMsg },
        { ALPROTO_HTTP,
          DETECT_SM_LIST_HSCDMATCH,
          DE_STATE_FLAG_HSCD_INSPECT,
          DE_STATE_FLAG_HSCD_MATCH,
          1,
          DetectEngineInspectHttpStatCode }
    };

    size_t i;
    for (i = 0 ; i < sizeof(data_toserver) / sizeof(struct tmp_t); i++) {
        DetectEngineRegisterAppInspectionEngine(data_toserver[i].alproto,
                                                data_toserver[i].dir,
                                                data_toserver[i].sm_list,
                                                data_toserver[i].inspect_flags,
                                                data_toserver[i].match_flags,
                                                data_toserver[i].Callback,
                                                app_inspection_engine);
    }

    for (i = 0 ; i < sizeof(data_toclient) / sizeof(struct tmp_t); i++) {
        DetectEngineRegisterAppInspectionEngine(data_toclient[i].alproto,
                                                data_toclient[i].dir,
                                                data_toclient[i].sm_list,
                                                data_toclient[i].inspect_flags,
                                                data_toclient[i].match_flags,
                                                data_toclient[i].Callback,
                                                app_inspection_engine);
    }

#if 0
    DetectEnginePrintAppInspectionEngines(app_inspection_engine);
#endif

    return;
}

static void AppendAppInspectionEngine(DetectEngineAppInspectionEngine *engine,
                                      DetectEngineAppInspectionEngine *list[][2])
{
    /* append to the list */
    DetectEngineAppInspectionEngine *tmp = list[engine->alproto][engine->dir];
    DetectEngineAppInspectionEngine *insert = NULL;
    while (tmp != NULL) {
        if (tmp->dir == engine->dir &&
            (tmp->sm_list == engine->sm_list ||
             tmp->inspect_flags == engine->inspect_flags ||
             tmp->match_flags == engine->match_flags)) {
            SCLogError(SC_ERR_DETECT_PREPARE, "App Inspection Engine already "
                       "registered for this direction(%"PRIu16") ||"
                       "sm_list(%d) || "
                       "[match(%"PRIu32")|inspect(%"PRIu32")]_flags",
                       tmp->dir, tmp->sm_list, tmp->inspect_flags,
                       tmp->match_flags);
            exit(EXIT_FAILURE);
        }
        insert = tmp;
        tmp = tmp->next;
    }
    if (insert == NULL)
        list[engine->alproto][engine->dir] = engine;
    else
        insert->next = engine;

    return;
}

void DetectEngineRegisterAppInspectionEngine(uint16_t alproto,
                                             uint16_t dir,
                                             int32_t sm_list,
                                             uint32_t inspect_flags,
                                             uint32_t match_flags,
                                             int (*Callback)(ThreadVars *tv,
                                                             DetectEngineCtx *de_ctx,
                                                             DetectEngineThreadCtx *det_ctx,
                                                             Signature *sig, Flow *f,
                                                             uint8_t flags, void *alstate,
                                                             int32_t tx_id),
                                             DetectEngineAppInspectionEngine *list[][2])
{
    if ((list == NULL) ||
        (alproto <= ALPROTO_UNKNOWN && alproto >= ALPROTO_FAILED) ||
        (dir > 1) ||
        (sm_list < DETECT_SM_LIST_MATCH || sm_list >= DETECT_SM_LIST_MAX) ||
        (Callback == NULL)) {
        SCLogError(SC_ERR_INVALID_ARGUMENTS, "Invalid arguments");
        exit(EXIT_FAILURE);
    }

    DetectEngineAppInspectionEngine *new_engine = SCMalloc(sizeof(DetectEngineAppInspectionEngine));
    if (unlikely(new_engine == NULL)) {
        exit(EXIT_FAILURE);
    }
    memset(new_engine, 0, sizeof(*new_engine));
    new_engine->alproto = alproto;
    new_engine->dir = dir;
    new_engine->sm_list = sm_list;
    new_engine->inspect_flags = inspect_flags;
    new_engine->match_flags = match_flags;
    new_engine->Callback = Callback;

    AppendAppInspectionEngine(new_engine, list);

    return;
}

static void *DetectEngineLiveRuleSwap(void *arg)
{
    SCEnter();

    if (SCSetThreadName("LiveRuleSwap") < 0) {
        SCLogWarning(SC_ERR_THREAD_INIT, "Unable to set thread name");
    }

    SCLogInfo("===== Starting live rule swap triggered by user signal USR2 =====");

    ThreadVars *tv_local = (ThreadVars *)arg;

    /* block usr2.  usr2 to be handled by the main thread only */
    UtilSignalBlock(SIGUSR2);

    if (tv_local->thread_setup_flags != 0)
        TmThreadSetupOptions(tv_local);

    /* release TmThreadSpawn */
    TmThreadsSetFlag(tv_local, THV_INIT_DONE);

    ConfDeInit();
    ConfInit();

    /* re-load the yaml file */
    if (conf_filename != NULL) {
        if (ConfYamlLoadFile(conf_filename) != 0) {
            /* Error already displayed. */
            exit(EXIT_FAILURE);
        }

        ConfNode *file;
        ConfNode *includes = ConfGetNode("include");
        if (includes != NULL) {
            TAILQ_FOREACH(file, &includes->head, next) {
                char *ifile = ConfLoadCompleteIncludePath(file->val);
                SCLogInfo("Live Rule Swap: Including: %s", ifile);

                if (ConfYamlLoadFile(ifile) != 0) {
                    /* Error already displayed. */
                    exit(EXIT_FAILURE);
                }
            }
        }
    } /* if (conf_filename != NULL) */

#if 0
    ConfDump();
#endif

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();

    SCClassConfLoadClassficationConfigFile(de_ctx);
    SCRConfLoadReferenceConfigFile(de_ctx);

    if (ActionInitConfig() < 0) {
        exit(EXIT_FAILURE);
    }

    if (SigLoadSignatures(de_ctx, NULL, FALSE) < 0) {
        SCLogError(SC_ERR_NO_RULES_LOADED, "Loading signatures failed.");
        if (de_ctx->failure_fatal)
            exit(EXIT_FAILURE);
        DetectEngineCtxFree(de_ctx);
        SCLogError(SC_ERR_LIVE_RULE_SWAP,  "Failure encountered while "
                   "loading new ruleset with live swap.");
        SCLogInfo("===== Live rule swap DONE =====");
        TmThreadsSetFlag(tv_local, THV_CLOSED);
        pthread_exit(NULL);
        return NULL;
    }

    SCThresholdConfInitContext(de_ctx, NULL);

    /* start the process of swapping detect threads ctxs */

    SCMutexLock(&tv_root_lock);

    int no_of_detect_tvs = 0;
    ThreadVars *tv = tv_root[TVT_PPT];
    while (tv) {
        /* obtain the slots for this TV */
        TmSlot *slots = tv->tm_slots;
        while (slots != NULL) {
            TmModule *tm = TmModuleGetById(slots->tm_id);

            if (suricata_ctl_flags != 0) {
                TmThreadsSetFlag(tv_local, THV_CLOSED);

                SCLogInfo("===== Live rule swap premature exit, since "
                          "engine is in shutdown phase =====");

                UtilSignalHandlerSetup(SIGUSR2, SignalHandlerSigusr2);
                SCMutexUnlock(&tv_root_lock);
                pthread_exit(NULL);
            }

            if (!(tm->flags & TM_FLAG_DETECT_TM)) {
                slots = slots->slot_next;
                continue;
            }

            no_of_detect_tvs++;

            slots = slots->slot_next;
        }

        tv = tv->next;
    }

    DetectEngineThreadCtx *old_det_ctx[no_of_detect_tvs];
    DetectEngineThreadCtx *new_det_ctx[no_of_detect_tvs];
    ThreadVars *detect_tvs[no_of_detect_tvs];

    /* all receive threads are part of packet processing threads */
    tv = tv_root[TVT_PPT];
    int i = 0;
    while (tv) {
        /* obtain the slots for this TV */
        TmSlot *slots = tv->tm_slots;
        while (slots != NULL) {
            TmModule *tm = TmModuleGetById(slots->tm_id);

            if (!(tm->flags & TM_FLAG_DETECT_TM)) {
                slots = slots->slot_next;
                continue;
            }

            old_det_ctx[i] = SC_ATOMIC_GET(slots->slot_data);
            detect_tvs[i] = tv;

            DetectEngineThreadCtx *det_ctx = NULL;
            DetectEngineThreadCtxInitForLiveRuleSwap(tv, (void *)de_ctx,
                                                     (void **)&det_ctx);
            SCLogDebug("live rule swap done with new det_ctx - %p and de_ctx "
                       "- %p\n", det_ctx, de_ctx);

            new_det_ctx[i] = det_ctx;
            i++;

            SCLogDebug("swapping new det_ctx - %p with older one - %p", det_ctx,
                       SC_ATOMIC_GET(slots->slot_data));
            (void)SC_ATOMIC_SET(slots->slot_data, det_ctx);

            slots = slots->slot_next;
        }

        tv = tv->next;
    }

    SCMutexUnlock(&tv_root_lock);

    SCLogInfo("Live rule swap has swapped %d old det_ctx's with new ones, "
              "along with the new de_ctx", no_of_detect_tvs);

    for (i = 0; i < no_of_detect_tvs; i++) {
        int break_out = 0;
        int pseudo_pkt_inserted = 0;
        usleep(1000);
        while (SC_ATOMIC_GET(new_det_ctx[i]->so_far_used_by_detect) != 1) {
            if (suricata_ctl_flags != 0) {
                break_out = 1;
                break;
            }

            if (pseudo_pkt_inserted == 0) {
                pseudo_pkt_inserted = 1;
                if (detect_tvs[i]->inq != NULL) {
                    Packet *p = PacketGetFromAlloc();
                    if (p != NULL) {
                        p->flags |= PKT_PSEUDO_STREAM_END;
                        PacketQueue *q = &trans_q[detect_tvs[i]->inq->id];
                        SCMutexLock(&q->mutex_q);

                        PacketEnqueue(q, p);
#ifdef __tile__
#else
                        SCCondSignal(&q->cond_q);
#endif
                        SCMutexUnlock(&q->mutex_q);
                    }
                }
            }
            usleep(1000);
        }
        if (break_out)
            break;
        SCLogDebug("new_det_ctx - %p used by detect engine", new_det_ctx[i]);
    }

    /* this is to make sure that if someone initiated shutdown during a live
     * rule swap, the live rule swap won't clean up the old det_ctx and
     * de_ctx, till all detect threads have stopped working and sitting
     * silently after setting RUNNING_DONE flag and while waiting for
     * THV_DEINIT flag */
    if (i != no_of_detect_tvs) {
        ThreadVars *tv = tv_root[TVT_PPT];
        while (tv) {
            /* obtain the slots for this TV */
            TmSlot *slots = tv->tm_slots;
            while (slots != NULL) {
                TmModule *tm = TmModuleGetById(slots->tm_id);

                if (!(tm->flags & TM_FLAG_DETECT_TM)) {
                    slots = slots->slot_next;
                    continue;
                }

                while (!TmThreadsCheckFlag(tv, THV_RUNNING_DONE)) {
                    usleep(100);
                }

                slots = slots->slot_next;
            }

            tv = tv->next;
        }
    }

    /* free all the ctxs */
    DetectEngineCtx *old_de_ctx = old_det_ctx[0]->de_ctx;
    for (i = 0; i < no_of_detect_tvs; i++) {
        SCLogDebug("Freeing old_det_ctx - %p used by detect",
                   old_det_ctx[i]);
        DetectEngineThreadCtxDeinit(NULL, old_det_ctx[i]);
    }
    DetectEngineCtxFree(old_de_ctx);

    SRepReloadComplete();

    /* reset the handler */
    UtilSignalHandlerSetup(SIGUSR2, SignalHandlerSigusr2);

    TmThreadsSetFlag(tv_local, THV_CLOSED);

    SCLogInfo("===== Live rule swap DONE =====");

    pthread_exit(NULL);
    return NULL;
}

void DetectEngineSpawnLiveRuleSwapMgmtThread(void)
{
    SCEnter();

    SCLogDebug("Spawning mgmt thread for live rule swap");

    ThreadVars *tv = TmThreadCreateMgmtThread("DetectEngineLiveRuleSwap",
                                              DetectEngineLiveRuleSwap, 0);
    if (tv == NULL) {
        SCLogError(SC_ERR_THREAD_CREATE, "Live rule swap thread spawn failed");
        exit(EXIT_FAILURE);
    }

    TmThreadSetCPU(tv, MANAGEMENT_CPU_SET);

    if (TmThreadSpawn(tv) != 0) {
        SCLogError(SC_ERR_THREAD_SPAWN, "TmThreadSpawn failed for "
                   "DetectEngineLiveRuleSwap");
        exit(EXIT_FAILURE);
    }

    SCReturn;
}

DetectEngineCtx *DetectEngineGetGlobalDeCtx(void)
{
    DetectEngineCtx *de_ctx = NULL;

    SCMutexLock(&tv_root_lock);

    ThreadVars *tv = tv_root[TVT_PPT];
    while (tv) {
        /* obtain the slots for this TV */
        TmSlot *slots = tv->tm_slots;
        while (slots != NULL) {
            TmModule *tm = TmModuleGetById(slots->tm_id);

            if (tm->flags & TM_FLAG_DETECT_TM) {
                DetectEngineThreadCtx *det_ctx = SC_ATOMIC_GET(slots->slot_data);
                de_ctx = det_ctx->de_ctx;
                SCMutexUnlock(&tv_root_lock);
                return de_ctx;
            }

            slots = slots->slot_next;
        }

        tv = tv->next;
    }

    SCMutexUnlock(&tv_root_lock);
    return NULL;
}

DetectEngineCtx *DetectEngineCtxInit(void) {
    DetectEngineCtx *de_ctx;

    ConfNode *seq_node = NULL;
    ConfNode *insp_recursion_limit_node = NULL;
    ConfNode *de_engine_node = NULL;
    char *insp_recursion_limit = NULL;

    de_ctx = SCMalloc(sizeof(DetectEngineCtx));
    if (unlikely(de_ctx == NULL))
        goto error;

    memset(de_ctx,0,sizeof(DetectEngineCtx));

    if (ConfGetBool("engine.init-failure-fatal", (int *)&(de_ctx->failure_fatal)) != 1) {
        SCLogDebug("ConfGetBool could not load the value.");
    }

    de_engine_node = ConfGetNode("detect-engine");
    if (de_engine_node != NULL) {
        TAILQ_FOREACH(seq_node, &de_engine_node->head, next) {
            if (strcmp(seq_node->val, "inspection-recursion-limit") != 0)
                continue;

            insp_recursion_limit_node = ConfNodeLookupChild(seq_node, seq_node->val);
            if (insp_recursion_limit_node == NULL) {
                SCLogError(SC_ERR_INVALID_YAML_CONF_ENTRY, "Error retrieving conf "
                           "entry for detect-engine:inspection-recursion-limit");
                break;
            }
            insp_recursion_limit = insp_recursion_limit_node->val;
            SCLogDebug("Found detect-engine:inspection-recursion-limit - %s:%s",
                       insp_recursion_limit_node->name, insp_recursion_limit_node->val);

            break;
        }
    }

    if (insp_recursion_limit != NULL) {
        de_ctx->inspection_recursion_limit = atoi(insp_recursion_limit);
    } else {
        de_ctx->inspection_recursion_limit =
            DETECT_ENGINE_DEFAULT_INSPECTION_RECURSION_LIMIT;
    }

    if (de_ctx->inspection_recursion_limit == 0)
        de_ctx->inspection_recursion_limit = -1;

    SCLogDebug("de_ctx->inspection_recursion_limit: %d",
               de_ctx->inspection_recursion_limit);

    de_ctx->mpm_matcher = PatternMatchDefaultMatcher();
    DetectEngineCtxLoadConf(de_ctx);

    SigGroupHeadHashInit(de_ctx);
    SigGroupHeadMpmHashInit(de_ctx);
    SigGroupHeadMpmUriHashInit(de_ctx);
    SigGroupHeadSPortHashInit(de_ctx);
    SigGroupHeadDPortHashInit(de_ctx);
    DetectPortSpHashInit(de_ctx);
    DetectPortDpHashInit(de_ctx);
    ThresholdHashInit(de_ctx);
    VariableNameInitHash(de_ctx);
    DetectParseDupSigHashInit(de_ctx);

    de_ctx->mpm_pattern_id_store = MpmPatternIdTableInitHash();
    if (de_ctx->mpm_pattern_id_store == NULL) {
        goto error;
    }

    de_ctx->id = detect_engine_ctx_id++;

    /* init iprep... ignore errors for now */
    (void)SRepInit(de_ctx);

    return de_ctx;
error:
    return NULL;
}

static void DetectEngineCtxFreeThreadKeywordData(DetectEngineCtx *de_ctx) {
    DetectEngineThreadKeywordCtxItem *item = de_ctx->keyword_list;
    while (item) {
        DetectEngineThreadKeywordCtxItem *next = item->next;
        SCFree(item);
        item = next;
    }
    de_ctx->keyword_list = NULL;
}

void DetectEngineCtxFree(DetectEngineCtx *de_ctx) {

    if (de_ctx == NULL)
        return;

#ifdef PROFILING
    if (de_ctx->profile_ctx != NULL) {
        SCProfilingRuleDestroyCtx(de_ctx->profile_ctx);
        de_ctx->profile_ctx = NULL;
    }
#endif

    /* Normally the hashes are freed elsewhere, but
     * to be sure look at them again here.
     */
    MpmPatternIdTableFreeHash(de_ctx->mpm_pattern_id_store); /* normally cleaned up in SigGroupBuild */

    SigGroupHeadHashFree(de_ctx);
    SigGroupHeadMpmHashFree(de_ctx);
    SigGroupHeadMpmUriHashFree(de_ctx);
    SigGroupHeadSPortHashFree(de_ctx);
    SigGroupHeadDPortHashFree(de_ctx);
    DetectParseDupSigHashFree(de_ctx);
    SCSigSignatureOrderingModuleCleanup(de_ctx);
    DetectPortSpHashFree(de_ctx);
    DetectPortDpHashFree(de_ctx);
    ThresholdContextDestroy(de_ctx);
    SigCleanSignatures(de_ctx);

    VariableNameFreeHash(de_ctx);
    if (de_ctx->sig_array)
        SCFree(de_ctx->sig_array);

    SCClassConfDeInitContext(de_ctx);
    SCRConfDeInitContext(de_ctx);

    SigGroupCleanup(de_ctx);

    if (de_ctx->sgh_mpm_context == ENGINE_SGH_MPM_FACTORY_CONTEXT_SINGLE) {
        MpmFactoryDeRegisterAllMpmCtxProfiles(de_ctx);
    }

    DetectEngineCtxFreeThreadKeywordData(de_ctx);
    SCFree(de_ctx);
    //DetectAddressGroupPrintMemory();
    //DetectSigGroupPrintMemory();
    //DetectPortPrintMemory();
}

/** \brief  Function that load DetectEngineCtx config for grouping sigs
 *          used by the engine
 *  \retval 0 if no config provided, 1 if config was provided
 *          and loaded successfuly
 */
static uint8_t DetectEngineCtxLoadConf(DetectEngineCtx *de_ctx) {
    uint8_t profile = ENGINE_PROFILE_UNKNOWN;
    char *de_ctx_profile = NULL;

    const char *max_uniq_toclient_src_groups_str = NULL;
    const char *max_uniq_toclient_dst_groups_str = NULL;
    const char *max_uniq_toclient_sp_groups_str = NULL;
    const char *max_uniq_toclient_dp_groups_str = NULL;

    const char *max_uniq_toserver_src_groups_str = NULL;
    const char *max_uniq_toserver_dst_groups_str = NULL;
    const char *max_uniq_toserver_sp_groups_str = NULL;
    const char *max_uniq_toserver_dp_groups_str = NULL;

    char *sgh_mpm_context = NULL;

    ConfNode *de_ctx_custom = ConfGetNode("detect-engine");
    ConfNode *opt = NULL;

    if (de_ctx_custom != NULL) {
        TAILQ_FOREACH(opt, &de_ctx_custom->head, next) {
            if (strcmp(opt->val, "profile") == 0) {
                de_ctx_profile = opt->head.tqh_first->val;
            } else if (strcmp(opt->val, "sgh-mpm-context") == 0) {
                sgh_mpm_context = opt->head.tqh_first->val;
            }
        }
    }

    if (de_ctx_profile != NULL) {
        if (strcmp(de_ctx_profile, "low") == 0) {
            profile = ENGINE_PROFILE_LOW;
        } else if (strcmp(de_ctx_profile, "medium") == 0) {
            profile = ENGINE_PROFILE_MEDIUM;
        } else if (strcmp(de_ctx_profile, "high") == 0) {
            profile = ENGINE_PROFILE_HIGH;
        } else if (strcmp(de_ctx_profile, "custom") == 0) {
            profile = ENGINE_PROFILE_CUSTOM;
        }

        SCLogDebug("Profile for detection engine groups is \"%s\"", de_ctx_profile);
    } else {
        SCLogDebug("Profile for detection engine groups not provided "
                   "at suricata.yaml. Using default (\"medium\").");
    }

    /* detect-engine.sgh-mpm-context option parsing */
    if (sgh_mpm_context == NULL || strcmp(sgh_mpm_context, "auto") == 0) {
        /* for now, since we still haven't implemented any intelligence into
         * understanding the patterns and distributing mpm_ctx across sgh */
        if (de_ctx->mpm_matcher == MPM_AC ||
            de_ctx->mpm_matcher == MPM_ACC ||
            de_ctx->mpm_matcher == MPM_AC_GFBS ||
            de_ctx->mpm_matcher == MPM_AC_BS) {
            de_ctx->sgh_mpm_context = ENGINE_SGH_MPM_FACTORY_CONTEXT_SINGLE;
        } else {
            de_ctx->sgh_mpm_context = ENGINE_SGH_MPM_FACTORY_CONTEXT_FULL;
        }
    } else {
        if (strcmp(sgh_mpm_context, "single") == 0) {
            de_ctx->sgh_mpm_context = ENGINE_SGH_MPM_FACTORY_CONTEXT_SINGLE;
        } else if (strcmp(sgh_mpm_context, "full") == 0) {
            de_ctx->sgh_mpm_context = ENGINE_SGH_MPM_FACTORY_CONTEXT_FULL;
        } else {
           SCLogError(SC_ERR_INVALID_YAML_CONF_ENTRY, "You have supplied an "
                      "invalid conf value for detect-engine.sgh-mpm-context-"
                      "%s", sgh_mpm_context);
           exit(EXIT_FAILURE);
        }
    }

    if (run_mode == RUNMODE_UNITTEST) {
        de_ctx->sgh_mpm_context = ENGINE_SGH_MPM_FACTORY_CONTEXT_FULL;
    }

    opt = NULL;
    switch (profile) {
        case ENGINE_PROFILE_LOW:
            de_ctx->max_uniq_toclient_src_groups = 2;
            de_ctx->max_uniq_toclient_dst_groups = 2;
            de_ctx->max_uniq_toclient_sp_groups = 2;
            de_ctx->max_uniq_toclient_dp_groups = 3;
            de_ctx->max_uniq_toserver_src_groups = 2;
            de_ctx->max_uniq_toserver_dst_groups = 2;
            de_ctx->max_uniq_toserver_sp_groups = 2;
            de_ctx->max_uniq_toserver_dp_groups = 3;
            break;

        case ENGINE_PROFILE_HIGH:
            de_ctx->max_uniq_toclient_src_groups = 15;
            de_ctx->max_uniq_toclient_dst_groups = 15;
            de_ctx->max_uniq_toclient_sp_groups = 15;
            de_ctx->max_uniq_toclient_dp_groups = 20;
            de_ctx->max_uniq_toserver_src_groups = 15;
            de_ctx->max_uniq_toserver_dst_groups = 15;
            de_ctx->max_uniq_toserver_sp_groups = 15;
            de_ctx->max_uniq_toserver_dp_groups = 40;
            break;

        case ENGINE_PROFILE_CUSTOM:
            TAILQ_FOREACH(opt, &de_ctx_custom->head, next) {
                if (strncmp(opt->val, "custom-values", 3) == 0) {
                    max_uniq_toclient_src_groups_str = ConfNodeLookupChildValue
                            (opt->head.tqh_first, "toclient_src_groups");
                    max_uniq_toclient_dst_groups_str = ConfNodeLookupChildValue
                            (opt->head.tqh_first, "toclient_dst_groups");
                    max_uniq_toclient_sp_groups_str = ConfNodeLookupChildValue
                            (opt->head.tqh_first, "toclient_sp_groups");
                    max_uniq_toclient_dp_groups_str = ConfNodeLookupChildValue
                            (opt->head.tqh_first, "toclient_dp_groups");
                    max_uniq_toserver_src_groups_str = ConfNodeLookupChildValue
                            (opt->head.tqh_first, "toserver_src_groups");
                    max_uniq_toserver_dst_groups_str = ConfNodeLookupChildValue
                            (opt->head.tqh_first, "toserver_dst_groups");
                    max_uniq_toserver_sp_groups_str = ConfNodeLookupChildValue
                            (opt->head.tqh_first, "toserver_sp_groups");
                    max_uniq_toserver_dp_groups_str = ConfNodeLookupChildValue
                            (opt->head.tqh_first, "toserver_dp_groups");
                }
            }
            if (max_uniq_toclient_src_groups_str != NULL) {
                if (ByteExtractStringUint16(&de_ctx->max_uniq_toclient_src_groups, 10,
                    strlen(max_uniq_toclient_src_groups_str),
                    (const char *)max_uniq_toclient_src_groups_str) <= 0)
                        de_ctx->max_uniq_toclient_src_groups = 2;
            } else {
                de_ctx->max_uniq_toclient_src_groups = 2;
            }
            if (max_uniq_toclient_dst_groups_str != NULL) {
                if (ByteExtractStringUint16(&de_ctx->max_uniq_toclient_dst_groups, 10,
                    strlen(max_uniq_toclient_dst_groups_str),
                    (const char *)max_uniq_toclient_dst_groups_str) <= 0)
                        de_ctx->max_uniq_toclient_dst_groups = 2;
            } else {
                de_ctx->max_uniq_toclient_dst_groups = 2;
            }
            if (max_uniq_toclient_sp_groups_str != NULL) {
                if (ByteExtractStringUint16(&de_ctx->max_uniq_toclient_sp_groups, 10,
                    strlen(max_uniq_toclient_sp_groups_str),
                    (const char *)max_uniq_toclient_sp_groups_str) <= 0)
                        de_ctx->max_uniq_toclient_sp_groups = 2;
            } else {
                de_ctx->max_uniq_toclient_sp_groups = 2;
            }
            if (max_uniq_toclient_dp_groups_str != NULL) {
                if (ByteExtractStringUint16(&de_ctx->max_uniq_toclient_dp_groups, 10,
                    strlen(max_uniq_toclient_dp_groups_str),
                    (const char *)max_uniq_toclient_dp_groups_str) <= 0)
                        de_ctx->max_uniq_toclient_dp_groups = 2;
            } else {
                de_ctx->max_uniq_toclient_dp_groups = 2;
            }
            if (max_uniq_toserver_src_groups_str != NULL) {
                if (ByteExtractStringUint16(&de_ctx->max_uniq_toserver_src_groups, 10,
                    strlen(max_uniq_toserver_src_groups_str),
                    (const char *)max_uniq_toserver_src_groups_str) <= 0)
                        de_ctx->max_uniq_toserver_src_groups = 2;
            } else {
                de_ctx->max_uniq_toserver_src_groups = 2;
            }
            if (max_uniq_toserver_dst_groups_str != NULL) {
                if (ByteExtractStringUint16(&de_ctx->max_uniq_toserver_dst_groups, 10,
                    strlen(max_uniq_toserver_dst_groups_str),
                    (const char *)max_uniq_toserver_dst_groups_str) <= 0)
                        de_ctx->max_uniq_toserver_dst_groups = 2;
            } else {
                de_ctx->max_uniq_toserver_dst_groups = 2;
            }
            if (max_uniq_toserver_sp_groups_str != NULL) {
                if (ByteExtractStringUint16(&de_ctx->max_uniq_toserver_sp_groups, 10,
                    strlen(max_uniq_toserver_sp_groups_str),
                    (const char *)max_uniq_toserver_sp_groups_str) <= 0)
                        de_ctx->max_uniq_toserver_sp_groups = 2;
            } else {
                de_ctx->max_uniq_toserver_sp_groups = 2;
            }
            if (max_uniq_toserver_dp_groups_str != NULL) {
                if (ByteExtractStringUint16(&de_ctx->max_uniq_toserver_dp_groups, 10,
                    strlen(max_uniq_toserver_dp_groups_str),
                    (const char *)max_uniq_toserver_dp_groups_str) <= 0)
                        de_ctx->max_uniq_toserver_dp_groups = 2;
            } else {
                de_ctx->max_uniq_toserver_dp_groups = 2;
            }
            break;

        /* Default (or no config provided) is profile medium */
        case ENGINE_PROFILE_MEDIUM:
        case ENGINE_PROFILE_UNKNOWN:
        default:
            de_ctx->max_uniq_toclient_src_groups = 4;
            de_ctx->max_uniq_toclient_dst_groups = 4;
            de_ctx->max_uniq_toclient_sp_groups = 4;
            de_ctx->max_uniq_toclient_dp_groups = 6;

            de_ctx->max_uniq_toserver_src_groups = 4;
            de_ctx->max_uniq_toserver_dst_groups = 8;
            de_ctx->max_uniq_toserver_sp_groups = 4;
            de_ctx->max_uniq_toserver_dp_groups = 30;
            break;
    }

    if (profile == ENGINE_PROFILE_UNKNOWN)
        return 0;
    return 1;
}

/*
 * getting & (re)setting the internal sig i
 */

//inline uint32_t DetectEngineGetMaxSigId(DetectEngineCtx *de_ctx) {
//    return de_ctx->signum;
//}

void DetectEngineResetMaxSigId(DetectEngineCtx *de_ctx) {
    de_ctx->signum = 0;
}

static int DetectEngineThreadCtxInitKeywords(DetectEngineCtx *de_ctx, DetectEngineThreadCtx *det_ctx) {
    if (de_ctx->keyword_id > 0) {
        det_ctx->keyword_ctxs_array = SCMalloc(de_ctx->keyword_id * sizeof(void *));
        if (det_ctx->keyword_ctxs_array == NULL) {
            SCLogError(SC_ERR_DETECT_PREPARE, "setting up thread local detect ctx");
            return TM_ECODE_FAILED;
        }

        memset(det_ctx->keyword_ctxs_array, 0x00, de_ctx->keyword_id * sizeof(void *));

        det_ctx->keyword_ctxs_size = de_ctx->keyword_id;

        DetectEngineThreadKeywordCtxItem *item = de_ctx->keyword_list;
        while (item) {
            det_ctx->keyword_ctxs_array[item->id] = item->InitFunc(item->data);
            if (det_ctx->keyword_ctxs_array[item->id] == NULL) {
                SCLogError(SC_ERR_DETECT_PREPARE, "setting up thread local detect ctx "
                        "for keyword \"%s\" failed", item->name);
                return TM_ECODE_FAILED;
            }
            item = item->next;
        }
    }
    return TM_ECODE_OK;
}

static void DetectEngineThreadCtxDeinitKeywords(DetectEngineCtx *de_ctx, DetectEngineThreadCtx *det_ctx) {
    if (de_ctx->keyword_id > 0) {
        DetectEngineThreadKeywordCtxItem *item = de_ctx->keyword_list;
        while (item) {
            if (det_ctx->keyword_ctxs_array[item->id] != NULL)
                item->FreeFunc(det_ctx->keyword_ctxs_array[item->id]);

            item = item->next;
        }
        det_ctx->keyword_ctxs_size = 0;
        SCFree(det_ctx->keyword_ctxs_array);
        det_ctx->keyword_ctxs_array = NULL;
    }
}

/** \internal
 *  \brief Helper for DetectThread setup functions
 */
static TmEcode ThreadCtxDoInit (DetectEngineCtx *de_ctx, DetectEngineThreadCtx *det_ctx) {
    ThreadVars *tv = det_ctx->tv;
    int i;

    /** \todo we still depend on the global mpm_ctx here
     *
     * Initialize the thread pattern match ctx with the max size
     * of the content and uricontent id's so our match lookup
     * table is always big enough
     */
    PatternMatchThreadPrepare(tv, &det_ctx->mtc, de_ctx->mpm_matcher, DetectContentMaxId(de_ctx));
    PatternMatchThreadPrepare(tv, &det_ctx->mtcs, de_ctx->mpm_matcher, DetectContentMaxId(de_ctx));
    PatternMatchThreadPrepare(tv, &det_ctx->mtcu, de_ctx->mpm_matcher, DetectUricontentMaxId(de_ctx));

    PmqSetup(tv, &det_ctx->pmq, 0, de_ctx->max_fp_id);
    for (i = 0; i < DETECT_SMSG_PMQ_NUM; i++) {
        PmqSetup(tv, &det_ctx->smsg_pmq[i], 0, de_ctx->max_fp_id);
    }

    /* IP-ONLY */
    DetectEngineIPOnlyThreadInit(de_ctx,&det_ctx->io_ctx);

    /* DeState */
    if (de_ctx->sig_array_len > 0) {
        det_ctx->de_state_sig_array_len = de_ctx->sig_array_len;
        det_ctx->de_state_sig_array = SCThreadMalloc(tv, det_ctx->de_state_sig_array_len * sizeof(uint8_t));
        if (det_ctx->de_state_sig_array == NULL) {
            return TM_ECODE_FAILED;
        }
        memset(det_ctx->de_state_sig_array, 0,
               det_ctx->de_state_sig_array_len * sizeof(uint8_t));

        det_ctx->match_array_len = de_ctx->sig_array_len;
        det_ctx->match_array = SCThreadMalloc(tv, det_ctx->match_array_len * sizeof(Signature *));
        if (det_ctx->match_array == NULL) {
            return TM_ECODE_FAILED;
        }
        memset(det_ctx->match_array, 0,
               det_ctx->match_array_len * sizeof(Signature *));
    }

    /* byte_extract storage */
    det_ctx->bj_values = SCMalloc(sizeof(*det_ctx->bj_values) *
                                  (de_ctx->byte_extract_max_local_id + 1));
    if (det_ctx->bj_values == NULL) {
        return TM_ECODE_FAILED;
    }

    DetectEngineThreadCtxInitKeywords(de_ctx, det_ctx);
#ifdef PROFILING
    SCProfilingRuleThreadSetup(de_ctx->profile_ctx, det_ctx);
#endif
    SC_ATOMIC_INIT(det_ctx->so_far_used_by_detect);

    return TM_ECODE_OK;
}

TmEcode DetectEngineThreadCtxInit(ThreadVars *tv, void *initdata, void **data)
{
    DetectEngineCtx *de_ctx = (DetectEngineCtx *)initdata;
    if (de_ctx == NULL)
        return TM_ECODE_FAILED;

    DetectEngineThreadCtx *det_ctx = SCMalloc(sizeof(DetectEngineThreadCtx));
    if (unlikely(det_ctx == NULL))
        return TM_ECODE_FAILED;
    memset(det_ctx, 0, sizeof(DetectEngineThreadCtx));

    det_ctx->tv = tv;
    det_ctx->de_ctx = de_ctx;

    if (ThreadCtxDoInit(de_ctx, det_ctx) != TM_ECODE_OK)
        return TM_ECODE_FAILED;

    /** alert counter setup */
    det_ctx->counter_alerts = SCPerfTVRegisterCounter("detect.alert", tv,
                                                      SC_PERF_TYPE_UINT64, "NULL");
    tv->sc_perf_pca = SCPerfGetAllCountersArray(tv, &tv->sc_perf_pctx);
    SCPerfAddToClubbedTMTable((tv->thread_group_name != NULL) ? tv->thread_group_name : tv->name,
                              &tv->sc_perf_pctx);

    /* pass thread data back to caller */
    *data = (void *)det_ctx;

    return TM_ECODE_OK;
}

/**
 * \internal
 * \brief This thread is an exact duplicate of DetectEngineThreadCtxInit(),
 *        except that the counters API 2 calls doesn't let us use the same
 *        init function.  Once we have the new counters API it should let
 *        us use the same init function.
 */
static TmEcode DetectEngineThreadCtxInitForLiveRuleSwap(ThreadVars *tv, void *initdata, void **data)
{
    DetectEngineCtx *de_ctx = (DetectEngineCtx *)initdata;
    if (de_ctx == NULL)
        return TM_ECODE_FAILED;

    DetectEngineThreadCtx *det_ctx = SCMalloc(sizeof(DetectEngineThreadCtx));
    if (unlikely(det_ctx == NULL))
        return TM_ECODE_FAILED;
    memset(det_ctx, 0, sizeof(DetectEngineThreadCtx));

    det_ctx->tv = tv;
    det_ctx->de_ctx = de_ctx;

    if (ThreadCtxDoInit(de_ctx, det_ctx) != TM_ECODE_OK)
        return TM_ECODE_FAILED;

    /** alert counter setup */
    det_ctx->counter_alerts = SCPerfTVRegisterCounter("detect.alert", tv,
                                                      SC_PERF_TYPE_UINT64, "NULL");
    /* no counter creation here */

    /* pass thread data back to caller */
    *data = (void *)det_ctx;

    return TM_ECODE_OK;
}

TmEcode DetectEngineThreadCtxDeinit(ThreadVars *tv, void *data) {
    DetectEngineThreadCtx *det_ctx = (DetectEngineThreadCtx *)data;

    if (det_ctx == NULL) {
        SCLogWarning(SC_ERR_INVALID_ARGUMENTS, "argument \"data\" NULL");
        return TM_ECODE_OK;
    }

#ifdef PROFILING
    SCProfilingRuleThreadCleanup(det_ctx);
#endif

    DetectEngineIPOnlyThreadDeinit(&det_ctx->io_ctx);

    /** \todo get rid of this static */
    PatternMatchThreadDestroy(&det_ctx->mtc, det_ctx->de_ctx->mpm_matcher);
    PatternMatchThreadDestroy(&det_ctx->mtcu, det_ctx->de_ctx->mpm_matcher);

    PmqFree(&det_ctx->pmq);
    int i;
    for (i = 0; i < DETECT_SMSG_PMQ_NUM; i++) {
        PmqFree(&det_ctx->smsg_pmq[i]);
    }

    if (det_ctx->de_state_sig_array != NULL)
        SCFree(det_ctx->de_state_sig_array);
    if (det_ctx->match_array != NULL)
        SCFree(det_ctx->match_array);

    if (det_ctx->bj_values != NULL)
        SCFree(det_ctx->bj_values);

    if (det_ctx->hsbd != NULL) {
        SCLogDebug("det_ctx hsbd %u", det_ctx->hsbd_buffers_list_len);
        for (i = 0; i < det_ctx->hsbd_buffers_list_len; i++) {
            if (det_ctx->hsbd[i].buffer != NULL)
                SCFree(det_ctx->hsbd[i].buffer);
        }
        SCFree(det_ctx->hsbd);
    }

    if (det_ctx->hcbd != NULL) {
        SCLogDebug("det_ctx hcbd %u", det_ctx->hcbd_buffers_list_len);
        for (i = 0; i < det_ctx->hcbd_buffers_list_len; i++) {
            if (det_ctx->hcbd[i].buffer != NULL)
                SCFree(det_ctx->hcbd[i].buffer);
            SCLogDebug("det_ctx->hcbd[i].buffer_size %u", det_ctx->hcbd[i].buffer_size);
        }
        SCFree(det_ctx->hcbd);
    }

    DetectEngineThreadCtxDeinitKeywords(det_ctx->de_ctx, det_ctx);
    SCFree(det_ctx);

    return TM_ECODE_OK;
}

void DetectEngineThreadCtxInfo(ThreadVars *t, DetectEngineThreadCtx *det_ctx) {
    /* XXX */
    PatternMatchThreadPrint(&det_ctx->mtc, det_ctx->de_ctx->mpm_matcher);
    PatternMatchThreadPrint(&det_ctx->mtcu, det_ctx->de_ctx->mpm_matcher);
}

/** \brief Register Thread keyword context Funcs
 *
 *  \param de_ctx detection engine to register in
 *  \param name keyword name for error printing
 *  \param InitFunc function ptr
 *  \param data keyword init data to pass to Func
 *  \param FreeFunc function ptr
 *  \param mode 0 normal (ctx per keyword instance) 1 shared (one ctx per det_ct)
 *
 *  \retval id for retrieval of ctx at runtime
 *  \retval -1 on error
 *
 *  \note make sure "data" remains valid and it free'd elsewhere. It's
 *        recommended to store it in the keywords global ctx so that
 *        it's freed when the de_ctx is freed.
 */
int DetectRegisterThreadCtxFuncs(DetectEngineCtx *de_ctx, const char *name, void *(*InitFunc)(void *), void *data, void (*FreeFunc)(void *), int mode) {
    BUG_ON(de_ctx == NULL || InitFunc == NULL || FreeFunc == NULL || data == NULL);

    if (mode) {
        DetectEngineThreadKeywordCtxItem *item = de_ctx->keyword_list;
        while (item != NULL) {
            if (strcmp(name, item->name) == 0) {
                return item->id;
            }

            item = item->next;
        }
    }

    DetectEngineThreadKeywordCtxItem *item = SCMalloc(sizeof(DetectEngineThreadKeywordCtxItem));
    if (unlikely(item == NULL))
        return -1;
    memset(item, 0x00, sizeof(DetectEngineThreadKeywordCtxItem));

    item->InitFunc = InitFunc;
    item->FreeFunc = FreeFunc;
    item->data = data;
    item->name = name;

    item->next = de_ctx->keyword_list;
    de_ctx->keyword_list = item;
    item->id = de_ctx->keyword_id++;

    return item->id;
}

/** \brief Retrieve thread local keyword ctx by id
 *
 *  \param det_ctx detection engine thread ctx to retrieve the ctx from
 *  \param id id of the ctx returned by DetectRegisterThreadCtxInitFunc at
 *            keyword init.
 *
 *  \retval ctx or NULL on error
 */
void *DetectThreadCtxGetKeywordThreadCtx(DetectEngineThreadCtx *det_ctx, int id) {
    if (id < 0 || id > det_ctx->keyword_ctxs_size || det_ctx->keyword_ctxs_array == NULL)
        return NULL;

    return det_ctx->keyword_ctxs_array[id];
}


/*************************************Unittest*********************************/

#ifdef UNITTESTS

static int DetectEngineInitYamlConf(char *conf)
{
    ConfCreateContextBackup();
    ConfInit();
    return ConfYamlLoadString(conf, strlen(conf));
}

static void DetectEngineDeInitYamlConf(void)
{
    ConfDeInit();
    ConfRestoreContextBackup();

    return;
}

static int DetectEngineTest01(void)
{
    char *conf =
        "%YAML 1.1\n"
        "---\n"
        "detect-engine:\n"
        "  - profile: medium\n"
        "  - custom-values:\n"
        "      toclient_src_groups: 2\n"
        "      toclient_dst_groups: 2\n"
        "      toclient_sp_groups: 2\n"
        "      toclient_dp_groups: 3\n"
        "      toserver_src_groups: 2\n"
        "      toserver_dst_groups: 4\n"
        "      toserver_sp_groups: 2\n"
        "      toserver_dp_groups: 25\n"
        "  - inspection-recursion-limit: 0\n";

    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if (DetectEngineInitYamlConf(conf) == -1)
        return 0;
    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    result = (de_ctx->inspection_recursion_limit == -1);

 end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);

    DetectEngineDeInitYamlConf();

    return result;
}

static int DetectEngineTest02(void)
{
    char *conf =
        "%YAML 1.1\n"
        "---\n"
        "detect-engine:\n"
        "  - profile: medium\n"
        "  - custom-values:\n"
        "      toclient_src_groups: 2\n"
        "      toclient_dst_groups: 2\n"
        "      toclient_sp_groups: 2\n"
        "      toclient_dp_groups: 3\n"
        "      toserver_src_groups: 2\n"
        "      toserver_dst_groups: 4\n"
        "      toserver_sp_groups: 2\n"
        "      toserver_dp_groups: 25\n"
        "  - inspection-recursion-limit:\n";

    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if (DetectEngineInitYamlConf(conf) == -1)
        return 0;
    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    result = (de_ctx->inspection_recursion_limit == -1);

 end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);

    DetectEngineDeInitYamlConf();

    return result;
}

static int DetectEngineTest03(void)
{
    char *conf =
        "%YAML 1.1\n"
        "---\n"
        "detect-engine:\n"
        "  - profile: medium\n"
        "  - custom-values:\n"
        "      toclient_src_groups: 2\n"
        "      toclient_dst_groups: 2\n"
        "      toclient_sp_groups: 2\n"
        "      toclient_dp_groups: 3\n"
        "      toserver_src_groups: 2\n"
        "      toserver_dst_groups: 4\n"
        "      toserver_sp_groups: 2\n"
        "      toserver_dp_groups: 25\n";

    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if (DetectEngineInitYamlConf(conf) == -1)
        return 0;
    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    result = (de_ctx->inspection_recursion_limit ==
              DETECT_ENGINE_DEFAULT_INSPECTION_RECURSION_LIMIT);

 end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);

    DetectEngineDeInitYamlConf();

    return result;
}

static int DetectEngineTest04(void)
{
    char *conf =
        "%YAML 1.1\n"
        "---\n"
        "detect-engine:\n"
        "  - profile: medium\n"
        "  - custom-values:\n"
        "      toclient_src_groups: 2\n"
        "      toclient_dst_groups: 2\n"
        "      toclient_sp_groups: 2\n"
        "      toclient_dp_groups: 3\n"
        "      toserver_src_groups: 2\n"
        "      toserver_dst_groups: 4\n"
        "      toserver_sp_groups: 2\n"
        "      toserver_dp_groups: 25\n"
        "  - inspection-recursion-limit: 10\n";

    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if (DetectEngineInitYamlConf(conf) == -1)
        return 0;
    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    result = (de_ctx->inspection_recursion_limit == 10);

 end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);

    DetectEngineDeInitYamlConf();

    return result;
}

int DummyTestAppInspectionEngine01(ThreadVars *tv,
                                   DetectEngineCtx *de_ctx,
                                   DetectEngineThreadCtx *det_ctx,
                                   Signature *sig,
                                   Flow *f,
                                   uint8_t flags,
                                   void *alstate,
                                   int32_t tx_id)
{
    return 0;
}

int DummyTestAppInspectionEngine02(ThreadVars *tv,
                                   DetectEngineCtx *de_ctx,
                                   DetectEngineThreadCtx *det_ctx,
                                   Signature *sig,
                                   Flow *f,
                                   uint8_t flags,
                                   void *alstate,
                                   int32_t tx_id)
{
    return 0;
}

int DetectEngineTest05(void)
{
    int result = 0;

    DetectEngineAppInspectionEngine *engine_list[ALPROTO_MAX][2];
    memset(engine_list, 0, sizeof(engine_list));

    DetectEngineRegisterAppInspectionEngine(ALPROTO_HTTP,
                                            0 /* STREAM_TOSERVER */,
                                            DETECT_SM_LIST_UMATCH,
                                            DE_STATE_FLAG_URI_INSPECT,
                                            DE_STATE_FLAG_URI_MATCH,
                                            DummyTestAppInspectionEngine01,
                                            engine_list);

    int alproto = ALPROTO_UNKNOWN + 1;
    for ( ; alproto < ALPROTO_FAILED; alproto++) {
        int dir = 0;
        for ( ; dir < 2; dir++) {
            if (alproto == ALPROTO_HTTP && dir == 0) {
                if (engine_list[alproto][dir]->next != NULL) {
                    printf("more than one entry found\n");
                    goto end;
                }

                DetectEngineAppInspectionEngine *engine = engine_list[alproto][dir];

                if (engine->alproto != alproto ||
                    engine->dir != dir ||
                    engine->sm_list != DETECT_SM_LIST_UMATCH ||
                    engine->inspect_flags != DE_STATE_FLAG_URI_INSPECT ||
                    engine->match_flags != DE_STATE_FLAG_URI_MATCH ||
                    engine->Callback != DummyTestAppInspectionEngine01) {
                    printf("failed for http and dir(0-toserver)\n");
                    goto end;
                }
            } /* if (alproto == ALPROTO_HTTP && dir == 0) */

            if (alproto == ALPROTO_HTTP && dir == 1) {
                if (engine_list[alproto][dir] != NULL) {
                    printf("failed for http and dir(1-toclient)\n");
                    goto end;
                }
            }

            if (alproto != ALPROTO_HTTP &&
                engine_list[alproto][0] != NULL &&
                engine_list[alproto][1] != NULL) {
                printf("failed for protocol %d\n", alproto);
                goto end;
            }
        } /* for ( ; dir < 2 ..)*/
    } /* for ( ; alproto < ALPROTO_FAILED; ..) */

    result = 1;
 end:
    return result;
}

int DetectEngineTest06(void)
{
    int result = 0;

    DetectEngineAppInspectionEngine *engine_list[ALPROTO_MAX][2];
    memset(engine_list, 0, sizeof(engine_list));

    DetectEngineRegisterAppInspectionEngine(ALPROTO_HTTP,
                                            0 /* STREAM_TOSERVER */,
                                            DETECT_SM_LIST_UMATCH,
                                            DE_STATE_FLAG_URI_INSPECT,
                                            DE_STATE_FLAG_URI_MATCH,
                                            DummyTestAppInspectionEngine01,
                                            engine_list);
    DetectEngineRegisterAppInspectionEngine(ALPROTO_HTTP,
                                            1 /* STREAM_TOCLIENT */,
                                            DETECT_SM_LIST_UMATCH,
                                            DE_STATE_FLAG_URI_INSPECT,
                                            DE_STATE_FLAG_URI_MATCH,
                                            DummyTestAppInspectionEngine02,
                                            engine_list);

    int alproto = ALPROTO_UNKNOWN + 1;
    for ( ; alproto < ALPROTO_FAILED; alproto++) {
        int dir = 0;
        for ( ; dir < 2; dir++) {
            if (alproto == ALPROTO_HTTP && dir == 0) {
                if (engine_list[alproto][dir]->next != NULL) {
                    printf("more than one entry found\n");
                    goto end;
                }

                DetectEngineAppInspectionEngine *engine = engine_list[alproto][dir];

                if (engine->alproto != alproto ||
                    engine->dir != dir ||
                    engine->sm_list != DETECT_SM_LIST_UMATCH ||
                    engine->inspect_flags != DE_STATE_FLAG_URI_INSPECT ||
                    engine->match_flags != DE_STATE_FLAG_URI_MATCH ||
                    engine->Callback != DummyTestAppInspectionEngine01) {
                    printf("failed for http and dir(0-toserver)\n");
                    goto end;
                }
            } /* if (alproto == ALPROTO_HTTP && dir == 0) */

            if (alproto == ALPROTO_HTTP && dir == 1) {
                if (engine_list[alproto][dir]->next != NULL) {
                    printf("more than one entry found\n");
                    goto end;
                }

                DetectEngineAppInspectionEngine *engine = engine_list[alproto][dir];

                if (engine->alproto != alproto ||
                    engine->dir != dir ||
                    engine->sm_list != DETECT_SM_LIST_UMATCH ||
                    engine->inspect_flags != DE_STATE_FLAG_URI_INSPECT ||
                    engine->match_flags != DE_STATE_FLAG_URI_MATCH ||
                    engine->Callback != DummyTestAppInspectionEngine02) {
                    printf("failed for http and dir(0-toclient)\n");
                    goto end;
                }
            } /* if (alproto == ALPROTO_HTTP && dir == 1) */

            if (alproto != ALPROTO_HTTP &&
                engine_list[alproto][0] != NULL &&
                engine_list[alproto][1] != NULL) {
                printf("failed for protocol %d\n", alproto);
                goto end;
            }
        } /* for ( ; dir < 2 ..)*/
    } /* for ( ; alproto < ALPROTO_FAILED; ..) */

    result = 1;
 end:
    return result;
}

int DetectEngineTest07(void)
{
    int result = 0;

    DetectEngineAppInspectionEngine *engine_list[ALPROTO_MAX][2];
    memset(engine_list, 0, sizeof(engine_list));

    struct test_data_t {
        int32_t sm_list;
        uint32_t inspect_flags;
        uint32_t match_flags;
        uint16_t dir;
        int (*Callback)(ThreadVars *tv,
                        DetectEngineCtx *de_ctx,
                        DetectEngineThreadCtx *det_ctx,
                        Signature *sig, Flow *f,
                        uint8_t flags, void *alstate,
                        int32_t tx_id);

    };

    struct test_data_t data[] = {
        { DETECT_SM_LIST_UMATCH,
          DE_STATE_FLAG_URI_INSPECT,
          DE_STATE_FLAG_URI_MATCH,
          0,
          DummyTestAppInspectionEngine01 },
        { DETECT_SM_LIST_HCBDMATCH,
          DE_STATE_FLAG_HCBD_INSPECT,
          DE_STATE_FLAG_HCBD_MATCH,
          0,
          DummyTestAppInspectionEngine02 },
        { DETECT_SM_LIST_HSBDMATCH,
          DE_STATE_FLAG_HSBD_INSPECT,
          DE_STATE_FLAG_HSBD_MATCH,
          1,
          DummyTestAppInspectionEngine02 },
        { DETECT_SM_LIST_HHDMATCH,
          DE_STATE_FLAG_HHD_INSPECT,
          DE_STATE_FLAG_HHD_MATCH,
          0,
          DummyTestAppInspectionEngine01 },
        { DETECT_SM_LIST_HRHDMATCH,
          DE_STATE_FLAG_HRHD_INSPECT,
          DE_STATE_FLAG_HRHD_MATCH,
          0,
          DummyTestAppInspectionEngine01 },
        { DETECT_SM_LIST_HMDMATCH,
          DE_STATE_FLAG_HMD_INSPECT,
          DE_STATE_FLAG_HMD_MATCH,
          0,
          DummyTestAppInspectionEngine02 },
        { DETECT_SM_LIST_HCDMATCH,
          DE_STATE_FLAG_HCD_INSPECT,
          DE_STATE_FLAG_HCD_MATCH,
          0,
          DummyTestAppInspectionEngine01 },
        { DETECT_SM_LIST_HRUDMATCH,
          DE_STATE_FLAG_HRUD_INSPECT,
          DE_STATE_FLAG_HRUD_MATCH,
          0,
          DummyTestAppInspectionEngine01 },
        { DETECT_SM_LIST_FILEMATCH,
          DE_STATE_FLAG_FILE_TS_INSPECT,
          DE_STATE_FLAG_FILE_TS_MATCH,
          0,
          DummyTestAppInspectionEngine02 },
        { DETECT_SM_LIST_FILEMATCH,
          DE_STATE_FLAG_FILE_TC_INSPECT,
          DE_STATE_FLAG_FILE_TC_MATCH,
          1,
          DummyTestAppInspectionEngine02 },
        { DETECT_SM_LIST_HSMDMATCH,
          DE_STATE_FLAG_HSMD_INSPECT,
          DE_STATE_FLAG_HSMD_MATCH,
          0,
          DummyTestAppInspectionEngine01 },
        { DETECT_SM_LIST_HSCDMATCH,
          DE_STATE_FLAG_HSCD_INSPECT,
          DE_STATE_FLAG_HSCD_MATCH,
          0,
          DummyTestAppInspectionEngine01 },
        { DETECT_SM_LIST_HUADMATCH,
          DE_STATE_FLAG_HUAD_INSPECT,
          DE_STATE_FLAG_HUAD_MATCH,
          0,
          DummyTestAppInspectionEngine02 },
    };

    size_t i = 0;
    for ( ; i < sizeof(data) / sizeof(struct test_data_t); i++) {
        DetectEngineRegisterAppInspectionEngine(ALPROTO_HTTP,
                                                data[i].dir /* STREAM_TOCLIENT */,
                                                data[i].sm_list,
                                                data[i].inspect_flags,
                                                data[i].match_flags,
                                                data[i].Callback,
                                                engine_list);
    }

#if 0
    DetectEnginePrintAppInspectionEngines(engine_list);
#endif

    int alproto = ALPROTO_UNKNOWN + 1;
    for ( ; alproto < ALPROTO_FAILED; alproto++) {
        int dir = 0;
        for ( ; dir < 2; dir++) {
            if (alproto == ALPROTO_HTTP) {
                DetectEngineAppInspectionEngine *engine = engine_list[alproto][dir];

                size_t i = 0;
                for ( ; i < (sizeof(data) / sizeof(struct test_data_t)); i++) {
                    if (data[i].dir != dir)
                        continue;

                    if (engine->alproto != ALPROTO_HTTP ||
                        engine->dir != data[i].dir ||
                        engine->sm_list != data[i].sm_list ||
                        engine->inspect_flags != data[i].inspect_flags ||
                        engine->match_flags != data[i].match_flags ||
                        engine->Callback != data[i].Callback) {
                        printf("failed for http\n");
                        goto end;
                    }
                    engine = engine->next;
                }
            } else {
                if (engine_list[alproto][0] != NULL &&
                    engine_list[alproto][1] != NULL) {
                    printf("failed for protocol %d\n", alproto);
                    goto end;
                }
            } /* else */
        } /* for ( ; dir < 2; dir++) */
    } /* for ( ; alproto < ALPROTO_FAILED; ..) */

    result = 1;
 end:
    return result;
}

#endif

void DetectEngineRegisterTests()
{

#ifdef UNITTESTS
    UtRegisterTest("DetectEngineTest01", DetectEngineTest01, 1);
    UtRegisterTest("DetectEngineTest02", DetectEngineTest02, 1);
    UtRegisterTest("DetectEngineTest03", DetectEngineTest03, 1);
    UtRegisterTest("DetectEngineTest04", DetectEngineTest04, 1);
    UtRegisterTest("DetectEngineTest05", DetectEngineTest05, 1);
    UtRegisterTest("DetectEngineTest06", DetectEngineTest06, 1);
    UtRegisterTest("DetectEngineTest07", DetectEngineTest07, 1);
#endif

    return;
}
