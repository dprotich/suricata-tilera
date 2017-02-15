/* Copyright (C) 2011 Open Information Security Foundation
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
 * \author Eric Leblond <eric@regit.org>
 *
 * Replace part of the detection engine.
 *
 * If previous filter is of content type, replace can be used to change
 * the matched part to a new value.
 */

#include "suricata-common.h"

#include "runmodes.h"

extern int run_mode;

#include "decode.h"

#include "detect.h"
#include "detect-parse.h"
#include "detect-content.h"
#include "detect-uricontent.h"
#include "detect-byte-extract.h"
#include "detect-replace.h"
#include "app-layer.h"

#include "detect-engine-mpm.h"
#include "detect-engine.h"
#include "detect-engine-state.h"

#include "util-checksum.h"

#include "util-unittest.h"
#include "util-unittest-helper.h"

#include "flow-var.h"

#include "util-debug.h"

static int DetectReplaceSetup (DetectEngineCtx *, Signature *, char *);
void DetectReplaceRegisterTests(void);

void DetectReplaceRegister (void) {
    sigmatch_table[DETECT_REPLACE].name = "replace";
    sigmatch_table[DETECT_REPLACE].Match = NULL;
    sigmatch_table[DETECT_REPLACE].Setup = DetectReplaceSetup;
    sigmatch_table[DETECT_REPLACE].Free  = NULL;
    sigmatch_table[DETECT_REPLACE].RegisterTests = DetectReplaceRegisterTests;

    sigmatch_table[DETECT_REPLACE].flags |= SIGMATCH_PAYLOAD;
}

int DetectReplaceSetup(DetectEngineCtx *de_ctx, Signature *s, char *replacestr)
{
    uint8_t *content = NULL;
    uint16_t len = 0;
    uint32_t flags = 0;
    SigMatch *pm = NULL;
    DetectContentData *ud = NULL;

    int ret = DetectContentDataParse("replace", replacestr, &content, &len, &flags);
    if (ret == -1)
        goto error;

    if (flags & DETECT_CONTENT_NEGATED) {
        SCLogError(SC_ERR_INVALID_VALUE, "Can't negate replacement string: %s",
                   replacestr);
        goto error;
    }

    switch (run_mode) {
        case RUNMODE_NFQ:
        case RUNMODE_IPFW:
            break;
        default:
            SCLogWarning(SC_ERR_RUNMODE,
                         "Can't use 'replace' keyword in non IPS mode: %s",
                         s->sig_str);
            /* this is a success, having the alert is interesting */
            return 0;
    }

    /* add to the latest "content" keyword from either dmatch or pmatch */
    pm =  SigMatchGetLastSMFromLists(s, 2,
            DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_PMATCH]);
    if (pm == NULL) {
        SCLogError(SC_ERR_WITHIN_MISSING_CONTENT, "replace needs"
                "preceding content option for raw sig");
        SCFree(content);
        return -1;
    }

    /* we can remove this switch now with the unified structure */
    ud = (DetectContentData *)pm->ctx;
    if (ud == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "invalid argument");
        SCFree(content);
        return -1;
    }
    if (ud->flags & DETECT_CONTENT_NEGATED) {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "can't have a relative "
                "negated keyword set along with a replacement");
        goto error;
    }
    if (ud->content_len != len) {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "can't have a content "
                "length different from replace length");
        goto error;
    }

    ud->replace = SCMalloc(len);
    if (ud->replace == NULL) {
        goto error;
    }
    memcpy(ud->replace, content, len);
    ud->replace_len = len;
    ud->flags |= DETECT_CONTENT_REPLACE;
    /* want packet matching only won't be able to replace data with
     * a flow.
     */
    s->flags |= SIG_FLAG_REQUIRE_PACKET;
    SCFree(content);

    return 0;

error:
    SCFree(content);
    return -1;
}

DetectReplaceList * DetectReplaceAddToList(DetectReplaceList *replist, uint8_t *found, DetectContentData *cd)
{
    DetectReplaceList *newlist;

    if (cd->content_len != cd->replace_len)
        return NULL;
    SCLogDebug("replace: Adding match");

    newlist = SCMalloc(sizeof(DetectReplaceList));
    if (unlikely(newlist == NULL))
        return NULL;
    newlist->found = found;
    newlist->cd = cd;
    newlist->next = NULL;

    if (replist) {
        replist->next = newlist;
        return replist;
    } else
        return newlist;
}


void DetectReplaceExecute(Packet *p, DetectReplaceList *replist)
{
    DetectReplaceList *tlist = NULL;

    if (p == NULL)
        return;

    SCLogDebug("replace: Executing match");
    while(replist) {
        memcpy(replist->found, replist->cd->replace, replist->cd->replace_len);
        SCLogDebug("replace: injecting '%s'", replist->cd->replace);
        p->flags |= PKT_STREAM_MODIFIED;
        ReCalculateChecksum(p);
        tlist = replist;
        replist = replist->next;
        SCFree(tlist);
    }
}


void DetectReplaceFree(DetectReplaceList *replist)
{
    DetectReplaceList *tlist = NULL;
    while(replist) {
        SCLogDebug("replace: Freing match");
        tlist = replist;
        replist = replist->next;
        SCFree(tlist);
    }
}

#ifdef UNITTESTS /* UNITTESTS */

/**
 * \test Test packet Matches
 * \param raw_eth_pkt pointer to the ethernet packet
 * \param pktsize size of the packet
 * \param sig pointer to the signature to test
 * \param sid sid number of the signature
 * \retval return 1 if match
 * \retval return 0 if not
 */
int DetectReplaceLongPatternMatchTest(uint8_t *raw_eth_pkt, uint16_t pktsize, char *sig,
                                      uint32_t sid, uint8_t *pp, uint16_t *len)
{
    int result = 0;

    Packet *p = NULL;
    p = SCMalloc(SIZE_OF_PACKET);
    if (unlikely(p == NULL))
        return 0;

    DecodeThreadVars dtv;

    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;

    if (pp == NULL) {
        SCLogDebug("replace: looks like a second run");
    }

    memset(p, 0, SIZE_OF_PACKET);
    p->pkt = (uint8_t *)(p + 1);
    PacketCopyData(p, raw_eth_pkt, pktsize);
    memset(&dtv, 0, sizeof(DecodeThreadVars));
    memset(&th_v, 0, sizeof(th_v));


    FlowInitConfig(FLOW_QUIET);
    DecodeEthernet(&th_v, &dtv, p, GET_PKT_DATA(p), pktsize, NULL);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }
    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx, sig);
    if (de_ctx->sig_list == NULL) {
        goto end;
    }
    de_ctx->sig_list->next = NULL;

    if (de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->type == DETECT_CONTENT) {
        DetectContentData *co = (DetectContentData *)de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx;
        if (co->flags & DETECT_CONTENT_RELATIVE_NEXT) {
            printf("relative next flag set on final match which is content: ");
            goto end;
        }
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    if (PacketAlertCheck(p, sid) != 1) {
        SCLogDebug("replace: no alert on sig %d", sid);
        goto end;
    }

    if (pp) {
        memcpy(pp, GET_PKT_DATA(p), GET_PKT_LEN(p));
        *len = pktsize;
        SCLogDebug("replace: copying %d on %p", *len, pp);
    }


    result = 1;
end:
    if (de_ctx != NULL)
    {
        SigGroupCleanup(de_ctx);
        SigCleanSignatures(de_ctx);
        if (det_ctx != NULL)
            DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
        DetectEngineCtxFree(de_ctx);
    }
    FlowShutdown();
    SCFree(p);


    return result;
}


/**
 * \brief Wrapper for DetectContentLongPatternMatchTest
 */
int DetectReplaceLongPatternMatchTestWrp(char *sig, uint32_t sid, char *sig_rep,  uint32_t sid_rep) {
    int ret;
    /** Real packet with the following tcp data:
     * "Hi, this is a big test to check content matches of splitted"
     * "patterns between multiple chunks!"
     * (without quotes! :) )
     */
    uint8_t raw_eth_pkt[] = {
        0xff,0xff,0xff,0xff,0xff,0xff,0x00,0x00,
        0x00,0x00,0x00,0x00,0x08,0x00,0x45,0x00,
        0x00,0x85,0x00,0x01,0x00,0x00,0x40,0x06,
        0x7c,0x70,0x7f,0x00,0x00,0x01,0x7f,0x00,
        0x00,0x01,0x00,0x14,0x00,0x50,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x50,0x02,
        0x20,0x00,0xc9,0xad,0x00,0x00,0x48,0x69,
        0x2c,0x20,0x74,0x68,0x69,0x73,0x20,0x69,
        0x73,0x20,0x61,0x20,0x62,0x69,0x67,0x20,
        0x74,0x65,0x73,0x74,0x20,0x74,0x6f,0x20,
        0x63,0x68,0x65,0x63,0x6b,0x20,0x63,0x6f,
        0x6e,0x74,0x65,0x6e,0x74,0x20,0x6d,0x61,
        0x74,0x63,0x68,0x65,0x73,0x20,0x6f,0x66,
        0x20,0x73,0x70,0x6c,0x69,0x74,0x74,0x65,
        0x64,0x20,0x70,0x61,0x74,0x74,0x65,0x72,
        0x6e,0x73,0x20,0x62,0x65,0x74,0x77,0x65,
        0x65,0x6e,0x20,0x6d,0x75,0x6c,0x74,0x69,
        0x70,0x6c,0x65,0x20,0x63,0x68,0x75,0x6e,
        0x6b,0x73,0x21 }; /* end raw_eth_pkt */
    uint8_t p[sizeof(raw_eth_pkt)];
    uint16_t psize = sizeof(raw_eth_pkt);

    /* would be unittest */
    int run_mode_backup = run_mode;
    run_mode = RUNMODE_NFQ;
    ret = DetectReplaceLongPatternMatchTest(raw_eth_pkt, (uint16_t)sizeof(raw_eth_pkt),
                             sig, sid, p, &psize);
    if (ret == 1) {
        SCLogDebug("replace: test1 phase1");
        ret = DetectReplaceLongPatternMatchTest(p, psize, sig_rep, sid_rep, NULL, NULL);
    }
    run_mode = run_mode_backup;
    return ret;
}


/**
 * \brief Wrapper for DetectContentLongPatternMatchTest
 */
int DetectReplaceLongPatternMatchTestUDPWrp(char *sig, uint32_t sid, char *sig_rep,  uint32_t sid_rep) {
    int ret;
    /** Real UDP DNS packet with a request A to a1.twimg.com
     */
    uint8_t raw_eth_pkt[] = {
        0x8c, 0xa9, 0x82, 0x75, 0x5d, 0x62, 0xb4, 0x07, 
        0xf9, 0xf3, 0xc7, 0x0a, 0x08, 0x00, 0x45, 0x00, 
        0x00, 0x3a, 0x92, 0x4f, 0x40, 0x00, 0x40, 0x11, 
        0x31, 0x1a, 0xc0, 0xa8, 0x00, 0x02, 0xc1, 0xbd, 
        0xf4, 0xe1, 0x3b, 0x7e, 0x00, 0x35, 0x00, 0x26, 
        0xcb, 0x81, 0x37, 0x62, 0x01, 0x00, 0x00, 0x01, 
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x61, 
        0x31, 0x05, 0x74, 0x77, 0x69, 0x6d, 0x67, 0x03, 
        0x63, 0x6f, 0x6d, 0x00, 0x00, 0x01, 0x00, 0x01 };

    uint8_t p[sizeof(raw_eth_pkt)];
    uint16_t psize = sizeof(raw_eth_pkt);

    int run_mode_backup = run_mode;
    run_mode = RUNMODE_NFQ;
    ret = DetectReplaceLongPatternMatchTest(raw_eth_pkt, (uint16_t)sizeof(raw_eth_pkt),
                             sig, sid, p, &psize);
    if (ret == 1) {
        SCLogDebug("replace: test1 phase1 ok: %" PRIuMAX" vs %d",(uintmax_t)sizeof(raw_eth_pkt),psize);
        ret = DetectReplaceLongPatternMatchTest(p, psize, sig_rep, sid_rep, NULL, NULL);
    }
    run_mode = run_mode_backup;
    return ret;
}

/**
 * \test Check if replace is working
 */
int DetectReplaceMatchTest01()
{
    char *sig = "alert tcp any any -> any any (msg:\"Nothing..\";"
                " content:\"big\"; replace:\"pig\"; sid:1;)";
    char *sig_rep = "alert tcp any any -> any any (msg:\"replace worked\";"
                " content:\"this is a pig test\"; sid:2;)";
    return DetectReplaceLongPatternMatchTestWrp(sig, 1, sig_rep, 2);
}

/**
 * \test Check if replace is working with offset
 */
int DetectReplaceMatchTest02()
{
    char *sig = "alert tcp any any -> any any (msg:\"Nothing..\";"
                " content:\"th\"; offset: 4; replace:\"TH\"; sid:1;)";
    char *sig_rep = "alert tcp any any -> any any (msg:\"replace worked\";"
                " content:\"THis\"; offset:4; sid:2;)";
    return DetectReplaceLongPatternMatchTestWrp(sig, 1, sig_rep, 2);
}

/**
 * \test Check if replace is working with offset and keyword inversion
 */
int DetectReplaceMatchTest03()
{
    char *sig = "alert tcp any any -> any any (msg:\"Nothing..\";"
                " content:\"th\"; replace:\"TH\"; offset: 4; sid:1;)";
    char *sig_rep = "alert tcp any any -> any any (msg:\"replace worked\";"
                " content:\"THis\"; offset:4; sid:2;)";
    return DetectReplaceLongPatternMatchTestWrp(sig, 1, sig_rep, 2);
}

/**
 * \test Check if replace is working with second content
 */
int DetectReplaceMatchTest04()
{
    char *sig = "alert tcp any any -> any any (msg:\"Nothing..\";"
                " content:\"th\"; replace:\"TH\"; content:\"patter\"; replace:\"matter\"; sid:1;)";
    char *sig_rep = "alert tcp any any -> any any (msg:\"replace worked\";"
                " content:\"THis\"; content:\"matterns\"; sid:2;)";
    return DetectReplaceLongPatternMatchTestWrp(sig, 1, sig_rep, 2);
}

/**
 * \test Check if replace is not done when second content don't match
 */
int DetectReplaceMatchTest05()
{
    char *sig = "alert tcp any any -> any any (msg:\"Nothing..\";"
                " content:\"th\"; replace:\"TH\"; content:\"nutella\"; sid:1;)";
    char *sig_rep = "alert tcp any any -> any any (msg:\"replace worked\";"
                " content:\"TH\"; sid:2;)";
    return DetectReplaceLongPatternMatchTestWrp(sig, 1, sig_rep, 2);
}

/**
 * \test Check if replace is not done when second content match and not
 * first
 */
int DetectReplaceMatchTest06()
{
    char *sig = "alert tcp any any -> any any (msg:\"Nothing..\";"
                " content:\"nutella\"; replace:\"commode\"; content:\"this is\"; sid:1;)";
    char *sig_rep = "alert tcp any any -> any any (msg:\"replace worked\";"
                " content:\"commode\"; sid:2;)";
    return DetectReplaceLongPatternMatchTestWrp(sig, 1, sig_rep, 2);
}

/**
 * \test Check if replace is working when nocase used
 */
int DetectReplaceMatchTest07()
{
    char *sig = "alert tcp any any -> any any (msg:\"Nothing..\";"
                " content:\"BiG\"; nocase; replace:\"pig\"; sid:1;)";
    char *sig_rep = "alert tcp any any -> any any (msg:\"replace worked\";"
                " content:\"this is a pig test\"; sid:2;)";
    return DetectReplaceLongPatternMatchTestWrp(sig, 1, sig_rep, 2);
}

/**
 * \test Check if replace is working when depth is used
 */
int DetectReplaceMatchTest08()
{
    char *sig = "alert tcp any any -> any any (msg:\"Nothing..\";"
                " content:\"big\"; depth:17; replace:\"pig\"; sid:1;)";
    char *sig_rep = "alert tcp any any -> any any (msg:\"replace worked\";"
                " content:\"this is a pig test\"; sid:2;)";
    return DetectReplaceLongPatternMatchTestWrp(sig, 1, sig_rep, 2);
}

/**
 * \test Check if replace is working when depth block match used
 */
int DetectReplaceMatchTest09()
{
    char *sig = "alert tcp any any -> any any (msg:\"Nothing..\";"
                " content:\"big\"; depth:16; replace:\"pig\"; sid:1;)";
    char *sig_rep = "alert tcp any any -> any any (msg:\"replace worked\";"
                " content:\"this is a pig test\"; sid:2;)";
    return DetectReplaceLongPatternMatchTestWrp(sig, 1, sig_rep, 2);
}

/**
 * \test Check if replace is working when depth block match used
 */
int DetectReplaceMatchTest10()
{
    char *sig = "alert tcp any any -> any any (msg:\"Nothing..\";"
                " content:\"big\"; depth:17; replace:\"pig\"; offset: 14; sid:1;)";
    char *sig_rep = "alert tcp any any -> any any (msg:\"replace worked\";"
                " content:\"pig\"; depth:17; offset:14; sid:2;)";
    return DetectReplaceLongPatternMatchTestWrp(sig, 1, sig_rep, 2);
}

/**
 * \test Check if replace is working with within
 */
int DetectReplaceMatchTest11()
{
    char *sig = "alert tcp any any -> any any (msg:\"Nothing..\";"
                " content:\"big\"; replace:\"pig\"; content:\"to\"; within: 11; sid:1;)";
    char *sig_rep = "alert tcp any any -> any any (msg:\"replace worked\";"
                " content:\"pig\"; depth:17; offset:14; sid:2;)";
    return DetectReplaceLongPatternMatchTestWrp(sig, 1, sig_rep, 2);
}

/**
 * \test Check if replace is working with within
 */
int DetectReplaceMatchTest12()
{
    char *sig = "alert tcp any any -> any any (msg:\"Nothing..\";"
                " content:\"big\"; replace:\"pig\"; content:\"to\"; within: 4; sid:1;)";
    char *sig_rep = "alert tcp any any -> any any (msg:\"replace worked\";"
                " content:\"pig\"; depth:17; offset:14; sid:2;)";
    return DetectReplaceLongPatternMatchTestWrp(sig, 1, sig_rep, 2);
}

/**
 * \test Check if replace is working with within
 */
int DetectReplaceMatchTest13()
{
    char *sig = "alert tcp any any -> any any (msg:\"Nothing..\";"
                " content:\"big\"; replace:\"pig\"; content:\"test\"; distance: 1; sid:1;)";
    char *sig_rep = "alert tcp any any -> any any (msg:\"replace worked\";"
                " content:\"pig\"; depth:17; offset:14; sid:2;)";
    return DetectReplaceLongPatternMatchTestWrp(sig, 1, sig_rep, 2);
}

/**
 * \test Check if replace is working with within
 */
int DetectReplaceMatchTest14()
{
    char *sig = "alert tcp any any -> any any (msg:\"Nothing..\";"
                " content:\"big\"; replace:\"pig\"; content:\"test\"; distance: 2; sid:1;)";
    char *sig_rep = "alert tcp any any -> any any (msg:\"replace worked\";"
                " content:\"pig\"; depth:17; offset:14; sid:2;)";
    return DetectReplaceLongPatternMatchTestWrp(sig, 1, sig_rep, 2);
}

/**
 * \test Check if replace is working with within
 */
int DetectReplaceMatchTest15()
{
    char *sig = "alert udp any any -> any any (msg:\"Nothing..\";"
                " content:\"com\"; replace:\"org\"; sid:1;)";
    char *sig_rep = "alert udp any any -> any any (msg:\"replace worked\";"
                " content:\"twimg|03|org\"; sid:2;)";
    return DetectReplaceLongPatternMatchTestUDPWrp(sig, 1, sig_rep, 2);
}


/**
 * \test Parsing test
 */
int DetectReplaceParseTest01(void)
{
    int run_mode_backup = run_mode;
    run_mode = RUNMODE_NFQ;

    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert udp any any -> any any "
                               "(msg:\"test\"; content:\"doh\"; replace:\"; sid:238012;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    run_mode = run_mode_backup;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test: non valid because of http protocol
 */
int DetectReplaceParseTest02(void)
{
    int run_mode_backup = run_mode;
    run_mode = RUNMODE_NFQ;

    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert http any any -> any any "
                               "(msg:\"test\"; content:\"doh\"; replace:\"bon\"; sid:238012;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

 end:
    run_mode = run_mode_backup;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test: non valid because of http_header on same content
 * as replace keyword
 */
int DetectReplaceParseTest03(void)
{
    int run_mode_backup = run_mode;
    run_mode = RUNMODE_NFQ;

    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert tcp any any -> any any "
                               "(msg:\"test\"; content:\"doh\"; replace:\"don\"; http_header; sid:238012;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    run_mode = run_mode_backup;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test no content
 */
int DetectReplaceParseTest04(void)
{
    int run_mode_backup = run_mode;
    run_mode = RUNMODE_NFQ;

    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert tcp any any -> any any "
                               "(msg:\"test\"; replace:\"don\"; sid:238012;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    run_mode = run_mode_backup;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test content after replace
 */
int DetectReplaceParseTest05(void)
{
    int run_mode_backup = run_mode;
    run_mode = RUNMODE_NFQ;

    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert tcp any any -> any any "
                               "(msg:\"test\"; replace:\"don\"; content:\"doh\"; sid:238012;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    run_mode = run_mode_backup;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test content and replace length differ
 */
int DetectReplaceParseTest06(void)
{
    int run_mode_backup = run_mode;
    run_mode = RUNMODE_NFQ;

    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert tcp any any -> any any "
                               "(msg:\"test\"; content:\"don\"; replace:\"donut\"; sid:238012;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    run_mode = run_mode_backup;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test content and replace length differ
 */
int DetectReplaceParseTest07(void)
{
    int run_mode_backup = run_mode;
    run_mode = RUNMODE_NFQ;

    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert tcp any any -> any any "
                               "(msg:\"test\"; content:\"don\"; replace:\"dou\"; content:\"jpg\"; http_header; sid:238012;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    run_mode = run_mode_backup;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}



#endif /* UNITTESTS */

/**
 * \brief this function registers unit tests for DetectContent
 */
void DetectReplaceRegisterTests(void)
{
#ifdef UNITTESTS /* UNITTESTS */
/* matching */
    UtRegisterTest("DetectReplaceMatchTest01", DetectReplaceMatchTest01, 1);
    UtRegisterTest("DetectReplaceMatchTest02", DetectReplaceMatchTest02, 1);
    UtRegisterTest("DetectReplaceMatchTest03", DetectReplaceMatchTest03, 1);
    UtRegisterTest("DetectReplaceMatchTest04", DetectReplaceMatchTest04, 1);
    UtRegisterTest("DetectReplaceMatchTest05", DetectReplaceMatchTest05, 0);
    UtRegisterTest("DetectReplaceMatchTest06", DetectReplaceMatchTest06, 0);
    UtRegisterTest("DetectReplaceMatchTest07", DetectReplaceMatchTest07, 1);
    UtRegisterTest("DetectReplaceMatchTest08", DetectReplaceMatchTest08, 1);
    UtRegisterTest("DetectReplaceMatchTest09", DetectReplaceMatchTest09, 0);
    UtRegisterTest("DetectReplaceMatchTest10", DetectReplaceMatchTest10, 1);
    UtRegisterTest("DetectReplaceMatchTest11", DetectReplaceMatchTest11, 1);
    UtRegisterTest("DetectReplaceMatchTest12", DetectReplaceMatchTest12, 0);
    UtRegisterTest("DetectReplaceMatchTest13", DetectReplaceMatchTest13, 1);
    UtRegisterTest("DetectReplaceMatchTest14", DetectReplaceMatchTest14, 0);
    UtRegisterTest("DetectReplaceMatchTest15", DetectReplaceMatchTest15, 1);
/* parsing */
    UtRegisterTest("DetectReplaceParseTest01", DetectReplaceParseTest01, 1);
    UtRegisterTest("DetectReplaceParseTest02", DetectReplaceParseTest02, 1);
    UtRegisterTest("DetectReplaceParseTest03", DetectReplaceParseTest03, 1);
    UtRegisterTest("DetectReplaceParseTest04", DetectReplaceParseTest04, 1);
    UtRegisterTest("DetectReplaceParseTest05", DetectReplaceParseTest05, 1);
    UtRegisterTest("DetectReplaceParseTest06", DetectReplaceParseTest06, 1);
    UtRegisterTest("DetectReplaceParseTest07", DetectReplaceParseTest07, 1);
#endif /* UNITTESTS */
}
