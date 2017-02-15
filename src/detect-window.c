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
 * \author Pablo Rincon Crespo <pablo.rincon.crespo@gmail.com>
 *
 * Implements the window keyword.
 */

#include "suricata-common.h"
#include "debug.h"
#include "decode.h"

#include "detect.h"
#include "detect-parse.h"

#include "detect-window.h"
#include "flow.h"
#include "flow-var.h"

#include "util-debug.h"
#include "util-unittest.h"
#include "util-unittest-helper.h"
#include "util-byte.h"

/**
 * \brief Regex for parsing our window option
 */
#define PARSE_REGEX  "^\\s*([!])?\\s*([0-9]{1,9}+)\\s*$"

static pcre *parse_regex;
static pcre_extra *parse_regex_study;

int DetectWindowMatch(ThreadVars *, DetectEngineThreadCtx *, Packet *, Signature *, SigMatch *);
int DetectWindowSetup(DetectEngineCtx *, Signature *, char *);
void DetectWindowRegisterTests(void);
void DetectWindowFree(void *);

/**
 * \brief Registration function for window: keyword
 */
void DetectWindowRegister (void) {
    sigmatch_table[DETECT_WINDOW].name = "window";
    sigmatch_table[DETECT_WINDOW].desc = "check for a specific TCP window size";
    sigmatch_table[DETECT_WINDOW].url = "https://redmine.openinfosecfoundation.org/projects/suricata/wiki/Header_keywords#Window";
    sigmatch_table[DETECT_WINDOW].Match = DetectWindowMatch;
    sigmatch_table[DETECT_WINDOW].Setup = DetectWindowSetup;
    sigmatch_table[DETECT_WINDOW].Free  = DetectWindowFree;
    sigmatch_table[DETECT_WINDOW].RegisterTests = DetectWindowRegisterTests;

    const char *eb;
    int eo;
    int opts = 0;

	#ifdef WINDOW_DEBUG
	printf("detect-window: Registering window rule option\n");
	#endif

    parse_regex = pcre_compile(PARSE_REGEX, opts, &eb, &eo, NULL);
    if(parse_regex == NULL)
    {
        SCLogError(SC_ERR_PCRE_COMPILE, "pcre compile of \"%s\" failed at offset %" PRId32 ": %s", PARSE_REGEX, eo, eb);
        goto error;
    }

    parse_regex_study = pcre_study(parse_regex, 0, &eb);
    if(eb != NULL)
    {
        SCLogError(SC_ERR_PCRE_STUDY, "pcre study failed: %s", eb);
        goto error;
    }
    return;

error:
    /* XXX */
    return;
}

/**
 * \brief This function is used to match the window size on a packet
 *
 * \param t pointer to thread vars
 * \param det_ctx pointer to the pattern matcher thread
 * \param p pointer to the current packet
 * \param m pointer to the sigmatch that we will cast into DetectWindowData
 *
 * \retval 0 no match
 * \retval 1 match
 */
int DetectWindowMatch(ThreadVars *t, DetectEngineThreadCtx *det_ctx, Packet *p, Signature *s, SigMatch *m) {
    DetectWindowData *wd = (DetectWindowData *)m->ctx;

    if ( !(PKT_IS_TCP(p)) || wd == NULL || PKT_IS_PSEUDOPKT(p)) {
        return 0;
    }

    if ( (!wd->negated && wd->size == TCP_GET_WINDOW(p)) || (wd->negated && wd->size != TCP_GET_WINDOW(p))) {
        return 1;
    }

    return 0;
}

/**
 * \brief This function is used to parse window options passed via window: keyword
 *
 * \param windowstr Pointer to the user provided window options (negation! and size)
 *
 * \retval wd pointer to DetectWindowData on success
 * \retval NULL on failure
 */
DetectWindowData *DetectWindowParse(char *windowstr) {
    DetectWindowData *wd = NULL;
    char *args[3] = {NULL,NULL,NULL}; /* PR: Why PCRE MAX_SUBSTRING must be multiple of 3? */
	#define MAX_SUBSTRINGS 30

    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];


    ret = pcre_exec(parse_regex, parse_regex_study, windowstr, strlen(windowstr), 0, 0, ov, MAX_SUBSTRINGS);

    if (ret < 1 || ret > 3) {
        SCLogError(SC_ERR_PCRE_MATCH, "pcre_exec parse error, ret %" PRId32 ", string %s", ret, windowstr);
        goto error;
    }

    wd = SCMalloc(sizeof(DetectWindowData));
    if (unlikely(wd == NULL))
        goto error;

    if (ret > 1) {
        const char *str_ptr;
        res = pcre_get_substring((char *)windowstr, ov, MAX_SUBSTRINGS, 1, &str_ptr);
        if (res < 0) {
            SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
            goto error;
        }
        args[0] = (char *)str_ptr;
        /* Detect if it's negated */
        if (args[0][0] == '!')
            wd->negated = 1;
        else
            wd->negated = 0;

        if (ret > 2) {
            res = pcre_get_substring((char *)windowstr, ov, MAX_SUBSTRINGS, 2, &str_ptr);
            if (res < 0) {
                SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
                goto error;
            }

            /* Get the window size if it's a valid value (in packets, we should alert if this doesn't happend from decode) */
            if (-1 == ByteExtractStringUint16(&wd->size, 10, 0, str_ptr)) {
                goto error;
            }
        }
    }

	int i = 0;
    for (i = 0; i < (ret -1); i++){
        if (args[i] != NULL)
            SCFree(args[i]);
    }
    return wd;

error:
    for (i = 0; i < (ret -1) && i < 3; i++){
        if (args[i] != NULL)
            SCFree(args[i]);
    }
    if (wd != NULL)
        DetectWindowFree(wd);
    return NULL;

}

/**
 * \brief this function is used to add the parsed window sizedata into the current signature
 *
 * \param de_ctx pointer to the Detection Engine Context
 * \param s pointer to the Current Signature
 * \param windowstr pointer to the user provided window options
 *
 * \retval 0 on Success
 * \retval -1 on Failure
 */
int DetectWindowSetup (DetectEngineCtx *de_ctx, Signature *s, char *windowstr)
{
    DetectWindowData *wd = NULL;
    SigMatch *sm = NULL;

    wd = DetectWindowParse(windowstr);
    if (wd == NULL) goto error;

    /* Okay so far so good, lets get this into a SigMatch
     * and put it in the Signature. */
    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_WINDOW;
    sm->ctx = (void *)wd;

    SigMatchAppendSMToList(s, sm, DETECT_SM_LIST_MATCH);
    s->flags |= SIG_FLAG_REQUIRE_PACKET;

    return 0;

error:
    if (wd != NULL) DetectWindowFree(wd);
    if (sm != NULL) SCFree(sm);
    return -1;

}

/**
 * \brief this function will free memory associated with DetectWindowData
 *
 * \param wd pointer to DetectWindowData
 */
void DetectWindowFree(void *ptr) {
    DetectWindowData *wd = (DetectWindowData *)ptr;
    SCFree(wd);
}

#ifdef UNITTESTS /* UNITTESTS */

/**
 * \test DetectWindowTestParse01 is a test to make sure that we set the size correctly
 *  when given valid window opt
 */
int DetectWindowTestParse01 (void) {
    int result = 0;
    DetectWindowData *wd = NULL;
    wd = DetectWindowParse("35402");
    if (wd != NULL &&wd->size==35402) {
        DetectWindowFree(wd);
        result = 1;
    }

    return result;
}

/**
 * \test DetectWindowTestParse02 is a test for setting the window opt negated
 */
int DetectWindowTestParse02 (void) {
    int result = 0;
    DetectWindowData *wd = NULL;
    wd = DetectWindowParse("!35402");
    if (wd != NULL) {
        if (wd->negated == 1 && wd->size==35402) {
            result = 1;
        } else {
            printf("expected wd->negated=1 and wd->size=35402\n");
        }
        DetectWindowFree(wd);
    }

    return result;
}

/**
 * \test DetectWindowTestParse03 is a test to check for an empty value
 */
int DetectWindowTestParse03 (void) {
    int result = 0;
    DetectWindowData *wd = NULL;
    wd = DetectWindowParse("");
    if (wd == NULL) {
        result = 1;
    } else {
        printf("expected a NULL pointer (It was an empty string)\n");
    }
    DetectWindowFree(wd);

    return result;
}

/**
 * \test DetectWindowTestParse03 is a test to check for a big value
 */
int DetectWindowTestParse04 (void) {
    int result = 0;
    DetectWindowData *wd = NULL;
    wd = DetectWindowParse("1235402");
    if (wd != NULL) {
        printf("expected a NULL pointer (It was exceeding the MAX window size)\n");
        DetectWindowFree(wd);
    }else
        result=1;

    return result;
}

/**
 * \test DetectWindowTestPacket01 is a test to check window with constructed packets
 */
int DetectWindowTestPacket01 (void) {
    int result = 0;
    uint8_t *buf = (uint8_t *)"Hi all!";
    uint16_t buflen = strlen((char *)buf);
    Packet *p[3];
    p[0] = UTHBuildPacket((uint8_t *)buf, buflen, IPPROTO_TCP);
    p[1] = UTHBuildPacket((uint8_t *)buf, buflen, IPPROTO_TCP);
    p[2] = UTHBuildPacket((uint8_t *)buf, buflen, IPPROTO_ICMP);

    if (p[0] == NULL || p[1] == NULL ||p[2] == NULL)
        goto end;

    /* TCP wwindow = 40 */
    p[0]->tcph->th_win = htons(40);

    /* TCP window = 41 */
    p[1]->tcph->th_win = htons(41);

    char *sigs[2];
    sigs[0]= "alert tcp any any -> any any (msg:\"Testing window 1\"; window:40; sid:1;)";
    sigs[1]= "alert tcp any any -> any any (msg:\"Testing window 2\"; window:41; sid:2;)";

    uint32_t sid[2] = {1, 2};

    uint32_t results[3][2] = {
                              /* packet 0 match sid 1 but should not match sid 2 */
                              {1, 0},
                              /* packet 1 should not match */
                              {0, 1},
                              /* packet 2 should not match */
                              {0, 0} };
    result = UTHGenericTest(p, 3, sigs, sid, (uint32_t *) results, 2);

    UTHFreePackets(p, 3);
end:
    return result;
}

#endif /* UNITTESTS */

/**
 * \brief this function registers unit tests for DetectWindow
 */
void DetectWindowRegisterTests(void) {
    #ifdef UNITTESTS /* UNITTESTS */
    UtRegisterTest("DetectWindowTestParse01", DetectWindowTestParse01, 1);
    UtRegisterTest("DetectWindowTestParse02", DetectWindowTestParse02, 1);
    UtRegisterTest("DetectWindowTestParse03", DetectWindowTestParse03, 1);
    UtRegisterTest("DetectWindowTestParse04", DetectWindowTestParse04, 1);
    UtRegisterTest("DetectWindowTestPacket01"  , DetectWindowTestPacket01  , 1);
    #endif /* UNITTESTS */
}
