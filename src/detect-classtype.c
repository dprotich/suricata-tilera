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
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 *
 * Implements classtype keyword.
 */

#include "suricata-common.h"
#include "decode.h"

#include "detect.h"
#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-classtype.h"
#include "flow-var.h"
#include "util-classification-config.h"
#include "util-error.h"
#include "util-debug.h"
#include "util-unittest.h"

#define DETECT_CLASSTYPE_REGEX "^\\s*([a-zA-Z][a-zA-Z0-9-_]*)\\s*$"

static pcre *regex = NULL;
static pcre_extra *regex_study = NULL;

static int DetectClasstypeSetup(DetectEngineCtx *, Signature *, char *);
void DetectClasstypeRegisterTests(void);

/**
 * \brief Registers the handler functions for the "Classtype" keyword.
 */
void DetectClasstypeRegister(void)
{
    const char *eb = NULL;
    int eo;
    int opts = 0;

    SCLogDebug("Registering the Classtype keyword handler");

    sigmatch_table[DETECT_CLASSTYPE].name = "classtype";
    sigmatch_table[DETECT_CLASSTYPE].desc = "information about the classification of rules and alerts";
    sigmatch_table[DETECT_CLASSTYPE].url = "https://redmine.openinfosecfoundation.org/projects/suricata/wiki/Meta-settings#Classtype";
    sigmatch_table[DETECT_CLASSTYPE].Match = NULL;
    sigmatch_table[DETECT_CLASSTYPE].Setup = DetectClasstypeSetup;
    sigmatch_table[DETECT_CLASSTYPE].Free  = NULL;
    sigmatch_table[DETECT_CLASSTYPE].RegisterTests = DetectClasstypeRegisterTests;

    regex = pcre_compile(DETECT_CLASSTYPE_REGEX, opts, &eb, &eo, NULL);
    if (regex == NULL) {
        SCLogError(SC_ERR_PCRE_COMPILE, "Compile of \"%s\" failed at offset %" PRId32 ": %s",
                   DETECT_CLASSTYPE_REGEX, eo, eb);
        goto end;
    }

    regex_study = pcre_study(regex, 0, &eb);
    if (eb != NULL) {
        SCLogError(SC_ERR_PCRE_STUDY, "pcre study failed: %s", eb);
        goto end;
    }

 end:
    return;
}

/**
 * \brief Parses the raw string supplied with the "Classtype" keyword.
 *
 * \param Pointer to the string to be parsed.
 *
 * \retval ct_name Pointer to the parsed string on Success; NULL on failure.
 */
static inline const char *DetectClasstypeParseRawString(char *rawstr)
{
    const char *ct_name = NULL;
#define MAX_SUBSTRINGS 30
    int ret = 0;
    int ov[MAX_SUBSTRINGS];

    /* get rid of the double quotes if present */
    if (rawstr[0] == '\"' && rawstr[strlen(rawstr) - 1] == '\"') {
        if ( (rawstr = SCStrdup(rawstr + 1)) == NULL) {
            SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
            goto end;
        }
        rawstr[strlen(rawstr) - 1] = '\0';
    }

    ret = pcre_exec(regex, regex_study, rawstr, strlen(rawstr), 0, 0, ov, 30);
    if (ret < 0) {
        SCLogError(SC_ERR_PCRE_MATCH, "Invalid Classtype in Signature");
        goto end;
    }

    ret = pcre_get_substring((char *)rawstr, ov, 30, 1, &ct_name);
    if (ret < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
        goto end;
    }

 end:
    return ct_name;
}

/**
 * \brief The setup function that would be called when the Signature parsing
 *        module encounters the "Classtype" keyword.
 *
 * \param de_ctx Pointer to the Detection Engine Context.
 * \param s      Pointer the current Signature instance that is being parsed.
 * \param rawstr Pointer to the argument supplied to the classtype keyword.
 *
 * \retval  0 On success
 * \retval -1 On failure
 */
static int DetectClasstypeSetup(DetectEngineCtx *de_ctx, Signature *s, char *rawstr)
{
    const char *parsed_ct_name = NULL;
    SCClassConfClasstype *ct = NULL;

    if ( (parsed_ct_name = DetectClasstypeParseRawString(rawstr)) == NULL) {
        SCLogError(SC_ERR_PCRE_PARSE, "Error parsing classtype argument supplied with the "
                   "classtype keyword");
        goto error;
    }

    ct = SCClassConfGetClasstype(parsed_ct_name, de_ctx);
    if (ct == NULL) {
        SCLogError(SC_ERR_UNKNOWN_VALUE, "Unknown Classtype: \"%s\".  Invalidating the Signature",
                   parsed_ct_name);
        goto error;
    }

    /* if we have retrieved the classtype, assign the message to be displayed
     * for this Signature by fast.log, if a Packet matches this Signature */
    s->class = ct->classtype_id;
    s->class_msg = ct->classtype_desc;

    /* if a priority keyword has appeared before the classtype, s->prio would
     * hold a value which is != -1, in which case we don't overwrite the value.
     * Otherwise, overwrite the value */
    if (s->prio == -1)
        s->prio = ct->priority;

    pcre_free_substring(parsed_ct_name);
    return 0;

 error:
    if (parsed_ct_name != NULL)
        pcre_free_substring(parsed_ct_name);
    return -1;
}

/*------------------------------Unittests-------------------------------------*/

#ifdef UNITTESTS

/**
 * \test Check that supplying an invalid classtype in the rule, results in the
 *       rule being invalidated.
 */
int DetectClasstypeTest01()
{
    int result = 0;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    SCClassConfGenerateValidDummyClassConfigFD01();
    SCClassConfLoadClassficationConfigFile(de_ctx);
    SCClassConfDeleteDummyClassificationConfigFD();

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(msg:\"Classtype test\"; "
                               "Classtype:not_available; sid:1;)");
    if (de_ctx->sig_list == NULL)
        result = 1;

    DetectEngineCtxFree(de_ctx);

end:
    return result;
}

/**
 * \test Check that both valid and invalid classtypes in a rule are handled
 *       properly, with rules containing invalid classtypes being rejected
 *       and the ones containing valid classtypes parsed and returned.
 */
int DetectClasstypeTest02()
{
    int result = 0;
    Signature *last = NULL;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    SCClassConfGenerateValidDummyClassConfigFD01();
    SCClassConfLoadClassficationConfigFile(de_ctx);
    SCClassConfDeleteDummyClassificationConfigFD();

    sig = SigInit(de_ctx, "alert tcp any any -> any any "
                  "(msg:\"Classtype test\"; Classtype:bad-unknown; sid:1;)");
    if (sig == NULL) {
        printf("first sig failed to parse: ");
        result = 0;
        goto end;
    }
    de_ctx->sig_list = last = sig;
    result = (sig != NULL);

    sig = SigInit(de_ctx, "alert tcp any any -> any any "
                  "(msg:\"Classtype test\"; Classtype:not-there; sid:1;)");
    last->next = sig;
    result &= (sig == NULL);

    sig = SigInit(de_ctx, "alert tcp any any -> any any "
                  "(msg:\"Classtype test\"; Classtype:Bad-UnkNown; sid:1;)");
    if (sig == NULL) {
        printf("second sig failed to parse: ");
        result = 0;
        goto end;
    }
    last->next = sig;
    last = sig;
    result &= (sig != NULL);

    sig = SigInit(de_ctx, "alert tcp any any -> any any "
                  "(msg:\"Classtype test\"; Classtype:nothing-wrong; sid:1;)");
    if (sig == NULL) {
        result = 0;
        goto end;
    }
    last->next = sig;
    last = sig;
    result &= (sig != NULL);

    sig = SigInit(de_ctx, "alert tcp any any -> any any "
                  "(msg:\"Classtype test\"; Classtype:attempted_dos; sid:1;)");
    last->next = sig;
    result &= (sig == NULL);

    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

end:
    return result;
}

/**
 * \test Check that the signatures are assigned priority based on classtype they
 *       are given.
 */
int DetectClasstypeTest03()
{
    int result = 0;
    Signature *last = NULL;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    SCClassConfGenerateValidDummyClassConfigFD01();
    SCClassConfLoadClassficationConfigFile(de_ctx);
    SCClassConfDeleteDummyClassificationConfigFD();

    sig = SigInit(de_ctx, "alert tcp any any -> any any "
                  "(msg:\"Classtype test\"; Classtype:bad-unknown; priority:1; sid:1;)");
    if (sig == NULL) {
        result = 0;
        goto end;
    }
    de_ctx->sig_list = last = sig;
    result = (sig != NULL);
    result &= (sig->prio == 1);

    sig = SigInit(de_ctx, "alert tcp any any -> any any "
                  "(msg:\"Classtype test\"; Classtype:unKnoWn; "
                  "priority:3; sid:1;)");
    if (sig == NULL) {
        result = 0;
        goto end;
    }
    last->next = sig;
    last = sig;
    result &= (sig != NULL);
    result &= (sig->prio == 3);

    sig = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"Classtype test\"; "
                  "Classtype:nothing-wrong; priority:1; sid:1;)");
    if (sig == NULL) {
        result = 0;
        goto end;
    }
    last->next = sig;
    last = sig;
    result &= (sig != NULL);
    result &= (sig->prio == 1);


    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

end:
    return result;
}

#endif /* UNITTESTS */

/**
 * \brief This function registers unit tests for Classification Config API.
 */
void DetectClasstypeRegisterTests(void)
{

#ifdef UNITTESTS

    UtRegisterTest("DetectClasstypeTest01", DetectClasstypeTest01, 1);
    UtRegisterTest("DetectClasstypeTest02", DetectClasstypeTest02, 1);
    UtRegisterTest("DetectClasstypeTest03", DetectClasstypeTest03, 1);

#endif /* UNITTESTS */

}
