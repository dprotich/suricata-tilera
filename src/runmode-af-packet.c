/* Copyright (C) 2011,2012 Open Information Security Foundation
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
 * \ingroup afppacket
 *
 * @{
 */

/**
 * \file
 *
 * \author Eric Leblond <eric@regit.org>
 *
 * AF_PACKET socket runmode
 *
 */


#include "suricata-common.h"
#include "config.h"
#include "tm-threads.h"
#include "conf.h"
#include "runmodes.h"
#include "runmode-af-packet.h"
#include "log-httplog.h"
#include "output.h"
#include "cuda-packet-batcher.h"
#include "detect-engine-mpm.h"

#include "alert-fastlog.h"
#include "alert-prelude.h"
#include "alert-unified2-alert.h"
#include "alert-debuglog.h"

#include "util-debug.h"
#include "util-time.h"
#include "util-cpu.h"
#include "util-affinity.h"
#include "util-device.h"
#include "util-runmodes.h"

#include "source-af-packet.h"

extern int max_pending_packets;

static const char *default_mode_auto = NULL;
static const char *default_mode_autofp = NULL;

const char *RunModeAFPGetDefaultMode(void)
{
    return default_mode_autofp;
}

void RunModeIdsAFPRegister(void)
{
    default_mode_auto = "autofp";
    RunModeRegisterNewRunMode(RUNMODE_AFP_DEV, "auto",
                              "Multi threaded af-packet mode",
                              RunModeIdsAFPAuto);
    RunModeRegisterNewRunMode(RUNMODE_AFP_DEV, "single",
                              "Single threaded af-packet mode",
                              RunModeIdsAFPSingle);
    RunModeRegisterNewRunMode(RUNMODE_AFP_DEV, "workers",
                              "Workers af-packet mode, each thread does all"
                              " tasks from acquisition to logging",
                              RunModeIdsAFPWorkers);
    default_mode_autofp = "autofp";
    RunModeRegisterNewRunMode(RUNMODE_AFP_DEV, "autofp",
                              "Multi socket AF_PACKET mode.  Packets from "
                              "each flow are assigned to a single detect "
                              "thread.",
                              RunModeIdsAFPAutoFp);
    return;
}

void AFPDerefConfig(void *conf)
{
    AFPIfaceConfig *pfp = (AFPIfaceConfig *)conf;
    /* Pcap config is used only once but cost of this low. */
    if (SC_ATOMIC_SUB(pfp->ref, 1) == 0) {
        SCFree(pfp);
    }
}

/**
 * \brief extract information from config file
 *
 * The returned structure will be freed by the thread init function.
 * This is thus necessary to or copy the structure before giving it
 * to thread or to reparse the file for each thread (and thus have
 * new structure.
 *
 * \return a AFPIfaceConfig corresponding to the interface name
 */
void *ParseAFPConfig(const char *iface)
{
    char *threadsstr = NULL;
    ConfNode *if_root;
    ConfNode *if_default = NULL;
    ConfNode *af_packet_node;
    AFPIfaceConfig *aconf = SCMalloc(sizeof(*aconf));
    char *tmpclusterid;
    char *tmpctype;
    char *copymodestr;
    intmax_t value;
    int boolval;
    char *bpf_filter = NULL;
    char *out_iface = NULL;

    if (unlikely(aconf == NULL)) {
        return NULL;
    }

    if (iface == NULL) {
        SCFree(aconf);
        return NULL;
    }

    strlcpy(aconf->iface, iface, sizeof(aconf->iface));
    aconf->threads = 1;
    SC_ATOMIC_INIT(aconf->ref);
    (void) SC_ATOMIC_ADD(aconf->ref, 1);
    aconf->buffer_size = 0;
    aconf->cluster_id = 1;
    aconf->cluster_type = PACKET_FANOUT_HASH;
    aconf->promisc = 1;
    aconf->checksum_mode = CHECKSUM_VALIDATION_KERNEL;
    aconf->DerefFunc = AFPDerefConfig;
    aconf->flags = 0;
    aconf->bpf_filter = NULL;
    aconf->out_iface = NULL;

    if (ConfGet("bpf-filter", &bpf_filter) == 1) {
        if (strlen(bpf_filter) > 0) {
            aconf->bpf_filter = bpf_filter;
            SCLogInfo("Going to use command-line provided bpf filter '%s'",
                       aconf->bpf_filter);
        }
    }

    /* Find initial node */
    af_packet_node = ConfGetNode("af-packet");
    if (af_packet_node == NULL) {
        SCLogInfo("Unable to find af-packet config using default value");
        return aconf;
    }

    if_root = ConfNodeLookupKeyValue(af_packet_node, "interface", iface);

    if_default = ConfNodeLookupKeyValue(af_packet_node, "interface", "default");

    if (if_root == NULL && if_default == NULL) {
        SCLogInfo("Unable to find af-packet config for "
                  "interface \"%s\" or \"default\", using default value",
                  iface);
        return aconf;
    }

    /* If there is no setting for current interface use default one as main iface */
    if (if_root == NULL) {
        if_root = if_default;
        if_default = NULL;
    }

    if (ConfGetChildValueWithDefault(if_root, if_default, "threads", &threadsstr) != 1) {
        aconf->threads = 1;
    } else {
        if (threadsstr != NULL) {
            aconf->threads = (uint8_t)atoi(threadsstr);
        }
    }
    if (aconf->threads == 0) {
        aconf->threads = 1;
    }

    if (ConfGetChildValueWithDefault(if_root, if_default, "copy-iface", &out_iface) == 1) {
        if (strlen(out_iface) > 0) {
            aconf->out_iface = out_iface;
        }
    }

    (void)ConfGetChildValueBoolWithDefault(if_root, if_default, "use-mmap", (int *)&boolval);
    if (boolval) {
        SCLogInfo("Enabling mmaped capture on iface %s",
                aconf->iface);
        aconf->flags |= AFP_RING_MODE;
    }
    (void)ConfGetChildValueBoolWithDefault(if_root, if_default, "use-emergency-flush", (int *)&boolval);
    if (boolval) {
        SCLogInfo("Enabling ring emergency flush on iface %s",
                aconf->iface);
        aconf->flags |= AFP_EMERGENCY_MODE;
    }


    aconf->copy_mode = AFP_COPY_MODE_NONE;
    if (ConfGetChildValueWithDefault(if_root, if_default, "copy-mode", &copymodestr) == 1) {
        if (aconf->out_iface == NULL) {
            SCLogInfo("Copy mode activated but no destination"
                      " iface. Disabling feature");
        } else if (!(aconf->flags & AFP_RING_MODE)) {
            SCLogInfo("Copy mode activated but use-mmap "
                      "set to no. Disabling feature");
        } else if (strlen(copymodestr) <= 0) {
            aconf->out_iface = NULL;
        } else if (strcmp(copymodestr, "ips") == 0) {
            SCLogInfo("AF_PACKET IPS mode activated %s->%s",
                    iface,
                    aconf->out_iface);
            aconf->copy_mode = AFP_COPY_MODE_IPS;
        } else if (strcmp(copymodestr, "tap") == 0) {
            SCLogInfo("AF_PACKET TAP mode activated %s->%s",
                    iface,
                    aconf->out_iface);
            aconf->copy_mode = AFP_COPY_MODE_TAP;
        } else {
            SCLogInfo("Invalid mode (not in tap, ips)");
        }
    }

    SC_ATOMIC_RESET(aconf->ref);
    (void) SC_ATOMIC_ADD(aconf->ref, aconf->threads);

    if (ConfGetChildValueWithDefault(if_root, if_default, "cluster-id", &tmpclusterid) != 1) {
        SCLogError(SC_ERR_INVALID_ARGUMENT,"Could not get cluster-id from config");
    } else {
        aconf->cluster_id = (uint16_t)atoi(tmpclusterid);
        SCLogDebug("Going to use cluster-id %" PRId32, aconf->cluster_id);
    }

    if (ConfGetChildValueWithDefault(if_root, if_default, "cluster-type", &tmpctype) != 1) {
        SCLogError(SC_ERR_GET_CLUSTER_TYPE_FAILED,"Could not get cluster-type from config");
    } else if (strcmp(tmpctype, "cluster_round_robin") == 0) {
        SCLogInfo("Using round-robin cluster mode for AF_PACKET (iface %s)",
                aconf->iface);
        aconf->cluster_type = PACKET_FANOUT_LB;
    } else if (strcmp(tmpctype, "cluster_flow") == 0) {
        /* In hash mode, we also ask for defragmentation needed to
         * compute the hash */
        uint16_t defrag = 0;
        SCLogInfo("Using flow cluster mode for AF_PACKET (iface %s)",
                aconf->iface);
        ConfGetChildValueBoolWithDefault(if_root, if_default, "defrag", (int *)&defrag);
        if (defrag) {
            SCLogInfo("Using defrag kernel functionality for AF_PACKET (iface %s)",
                    aconf->iface);
            defrag = PACKET_FANOUT_FLAG_DEFRAG;
        }
        aconf->cluster_type = PACKET_FANOUT_HASH | defrag;
    } else if (strcmp(tmpctype, "cluster_cpu") == 0) {
        SCLogInfo("Using cpu cluster mode for AF_PACKET (iface %s)",
                aconf->iface);
        aconf->cluster_type = PACKET_FANOUT_CPU;
    } else {
        SCLogError(SC_ERR_INVALID_CLUSTER_TYPE,"invalid cluster-type %s",tmpctype);
        SCFree(aconf);
        return NULL;
    }

    /*load af_packet bpf filter*/
    /* command line value has precedence */
    if (ConfGet("bpf-filter", &bpf_filter) != 1) {
        if (ConfGetChildValueWithDefault(if_root, if_default, "bpf-filter", &bpf_filter) == 1) {
            if (strlen(bpf_filter) > 0) {
                aconf->bpf_filter = bpf_filter;
                SCLogInfo("Going to use bpf filter %s", aconf->bpf_filter);
            }
        }
    }

    if ((ConfGetChildValueIntWithDefault(if_root, if_default, "buffer-size", &value)) == 1) {
        aconf->buffer_size = value;
    } else {
        aconf->buffer_size = 0;
    }
    if ((ConfGetChildValueIntWithDefault(if_root, if_default, "ring-size", &value)) == 1) {
        aconf->ring_size = value;
        if (value * aconf->threads < max_pending_packets) {
            aconf->ring_size = max_pending_packets / aconf->threads + 1;
            SCLogWarning(SC_ERR_AFP_CREATE, "Inefficient setup: ring-size < max_pending_packets. "
                         "Resetting to decent value %d.", aconf->ring_size);
            /* We want at least that max_pending_packets packets can be handled by the
             * interface. This is generous if we have multiple interfaces listening. */
        }
    } else {
        /* We want that max_pending_packets packets can be handled by suricata
         * for this interface. To take burst into account we multiply the obtained
         * size by 2. */
        aconf->ring_size = max_pending_packets * 2 / aconf->threads;
    }

    (void)ConfGetChildValueBoolWithDefault(if_root, if_default, "disable-promisc", (int *)&boolval);
    if (boolval) {
        SCLogInfo("Disabling promiscuous mode on iface %s",
                aconf->iface);
        aconf->promisc = 0;
    }

    if (ConfGetChildValueWithDefault(if_root, if_default, "checksum-checks", &tmpctype) == 1) {
        if (strcmp(tmpctype, "auto") == 0) {
            aconf->checksum_mode = CHECKSUM_VALIDATION_AUTO;
        } else if (strcmp(tmpctype, "yes") == 0) {
            aconf->checksum_mode = CHECKSUM_VALIDATION_ENABLE;
        } else if (strcmp(tmpctype, "no") == 0) {
            aconf->checksum_mode = CHECKSUM_VALIDATION_DISABLE;
        } else if (strcmp(tmpctype, "kernel") == 0) {
            aconf->checksum_mode = CHECKSUM_VALIDATION_KERNEL;
        } else {
            SCLogError(SC_ERR_INVALID_ARGUMENT, "Invalid value for checksum-checks for %s", aconf->iface);
        }
    }

    return aconf;
}

int AFPConfigGeThreadsCount(void *conf)
{
    AFPIfaceConfig *afp = (AFPIfaceConfig *)conf;
    return afp->threads;
}

/**
 * \brief RunModeIdsAFPAuto set up the following thread packet handlers:
 *        - Receive thread (from live iface)
 *        - Decode thread
 *        - Stream thread
 *        - Detect: If we have only 1 cpu, it will setup one Detect thread
 *                  If we have more than one, it will setup num_cpus - 1
 *                  starting from the second cpu available.
 *        - Respond/Reject thread
 *        - Outputs thread
 *        By default the threads will use the first cpu available
 *        except the Detection threads if we have more than one cpu.
 *
 * \param de_ctx Pointer to the Detection Engine.
 *
 * \retval 0 If all goes well. (If any problem is detected the engine will
 *           exit()).
 */
int RunModeIdsAFPAuto(DetectEngineCtx *de_ctx)
{
    SCEnter();

#ifdef HAVE_AF_PACKET
    int ret;
    char *live_dev = NULL;

    RunModeInitialize();

    TimeModeSetLive();

    (void)ConfGet("af-packet.live-interface", &live_dev);

    if (AFPPeersListInit() != TM_ECODE_OK) {
        SCLogError(SC_ERR_RUNMODE, "Unable to init peers list.");
        exit(EXIT_FAILURE);
    }

    ret = RunModeSetLiveCaptureAuto(de_ctx,
                                    ParseAFPConfig,
                                    AFPConfigGeThreadsCount,
                                    "ReceiveAFP",
                                    "DecodeAFP", "RecvAFP",
                                    live_dev);
    if (ret != 0) {
        SCLogError(SC_ERR_RUNMODE, "Unable to start runmode");
        exit(EXIT_FAILURE);
    }

    /* In IPS mode each threads must have a peer */
    if (AFPPeersListCheck() != TM_ECODE_OK) {
        SCLogError(SC_ERR_RUNMODE, "Some IPS capture threads did not peer.");
        exit(EXIT_FAILURE);
    }

    SCLogInfo("RunModeIdsAFPAuto initialised");
#endif
    SCReturnInt(0);
}

int RunModeIdsAFPAutoFp(DetectEngineCtx *de_ctx)
{
    SCEnter();

/* We include only if AF_PACKET is enabled */
#ifdef HAVE_AF_PACKET
    int ret;
    char *live_dev = NULL;

    RunModeInitialize();

    TimeModeSetLive();

    (void)ConfGet("af-packet.live-interface", &live_dev);

    SCLogDebug("live_dev %s", live_dev);

    if (AFPPeersListInit() != TM_ECODE_OK) {
        SCLogError(SC_ERR_RUNMODE, "Unable to init peers list.");
        exit(EXIT_FAILURE);
    }

    ret = RunModeSetLiveCaptureAutoFp(de_ctx,
                              ParseAFPConfig,
                              AFPConfigGeThreadsCount,
                              "ReceiveAFP",
                              "DecodeAFP", "RxAFP",
                              live_dev);
    if (ret != 0) {
        SCLogError(SC_ERR_RUNMODE, "Unable to start runmode");
        exit(EXIT_FAILURE);
    }

    /* In IPS mode each threads must have a peer */
    if (AFPPeersListCheck() != TM_ECODE_OK) {
        SCLogError(SC_ERR_RUNMODE, "Some IPS capture threads did not peer.");
        exit(EXIT_FAILURE);
    }

    SCLogInfo("RunModeIdsAFPAutoFp initialised");
#endif /* HAVE_AF_PACKET */

    SCReturnInt(0);
}

/**
 * \brief Single thread version of the AF_PACKET processing.
 */
int RunModeIdsAFPSingle(DetectEngineCtx *de_ctx)
{
#ifdef HAVE_AF_PACKET
    int ret;
    char *live_dev = NULL;
#endif
    SCEnter();
#ifdef HAVE_AF_PACKET

    RunModeInitialize();
    TimeModeSetLive();

    (void)ConfGet("af-packet.live-interface", &live_dev);

    if (AFPPeersListInit() != TM_ECODE_OK) {
        SCLogError(SC_ERR_RUNMODE, "Unable to init peers list.");
        exit(EXIT_FAILURE);
    }

    ret = RunModeSetLiveCaptureSingle(de_ctx,
                                    ParseAFPConfig,
                                    AFPConfigGeThreadsCount,
                                    "ReceiveAFP",
                                    "DecodeAFP", "AFPacket",
                                    live_dev);
    if (ret != 0) {
        SCLogError(SC_ERR_RUNMODE, "Unable to start runmode");
        exit(EXIT_FAILURE);
    }

    /* In IPS mode each threads must have a peer */
    if (AFPPeersListCheck() != TM_ECODE_OK) {
        SCLogError(SC_ERR_RUNMODE, "Some IPS capture threads did not peer.");
        exit(EXIT_FAILURE);
    }

    SCLogInfo("RunModeIdsAFPSingle initialised");

#endif /* HAVE_AF_PACKET */
    SCReturnInt(0);
}

/**
 * \brief Workers version of the AF_PACKET processing.
 *
 * Start N threads with each thread doing all the work.
 *
 */
int RunModeIdsAFPWorkers(DetectEngineCtx *de_ctx)
{
#ifdef HAVE_AF_PACKET
    int ret;
    char *live_dev = NULL;
#endif
    SCEnter();
#ifdef HAVE_AF_PACKET

    RunModeInitialize();
    TimeModeSetLive();

    (void)ConfGet("af-packet.live-interface", &live_dev);

    if (AFPPeersListInit() != TM_ECODE_OK) {
        SCLogError(SC_ERR_RUNMODE, "Unable to init peers list.");
        exit(EXIT_FAILURE);
    }

    ret = RunModeSetLiveCaptureWorkers(de_ctx,
                                    ParseAFPConfig,
                                    AFPConfigGeThreadsCount,
                                    "ReceiveAFP",
                                    "DecodeAFP", "AFPacket",
                                    live_dev);
    if (ret != 0) {
        SCLogError(SC_ERR_RUNMODE, "Unable to start runmode");
        exit(EXIT_FAILURE);
    }

    /* In IPS mode each threads must have a peer */
    if (AFPPeersListCheck() != TM_ECODE_OK) {
        SCLogError(SC_ERR_RUNMODE, "Some IPS capture threads did not peer.");
        exit(EXIT_FAILURE);
    }

    SCLogInfo("RunModeIdsAFPWorkers initialised");

#endif /* HAVE_AF_PACKET */
    SCReturnInt(0);
}

/**
 * @}
 */
