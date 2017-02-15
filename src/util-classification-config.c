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
 * Used for parsing a classification.config file
 */

#include "suricata-common.h"
#include "detect.h"
#include "detect-engine.h"
#include "util-hash.h"

#include "conf.h"
#include "util-classification-config.h"
#include "util-unittest.h"
#include "util-error.h"
#include "util-debug.h"
#include "util-fmemopen.h"

/* Regex to parse the classtype argument from a Signature.  The first substring
 * holds the classtype name, the second substring holds the classtype the
 * classtype description, and the third argument holds the priority */
#define DETECT_CLASSCONFIG_REGEX "^\\s*config\\s*classification\\s*:\\s*([a-zA-Z][a-zA-Z0-9-_]*)\\s*,\\s*(.+)\\s*,\\s*(\\d+)\\s*$"

/* Default path for the classification.config file */
#if defined OS_WIN32 || defined __CYGWIN__
#define SC_CLASS_CONF_DEF_CONF_FILEPATH CONFIG_DIR "\\\\classification.config"
#else
#define SC_CLASS_CONF_DEF_CONF_FILEPATH CONFIG_DIR "/classification.config"
#endif

/* Holds a pointer to the default path for the classification.config file */
static const char *default_file_path = SC_CLASS_CONF_DEF_CONF_FILEPATH;
static FILE *fd = NULL;
static pcre *regex = NULL;
static pcre_extra *regex_study = NULL;

uint32_t SCClassConfClasstypeHashFunc(HashTable *ht, void *data, uint16_t datalen);
char SCClassConfClasstypeHashCompareFunc(void *data1, uint16_t datalen1,
                                         void *data2, uint16_t datalen2);
void SCClassConfClasstypeHashFree(void *ch);
static char *SCClassConfGetConfFilename(void);

/**
 * \brief Inits the context to be used by the Classification Config parsing API.
 *
 *        This function initializes the hash table to be used by the Detection
 *        Engine Context to hold the data from the classification.config file,
 *        obtains the file desc to parse the classification.config file, and
 *        inits the regex used to parse the lines from classification.config
 *        file.
 *
 * \param de_ctx Pointer to the Detection Engine Context.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
int SCClassConfInitContextAndLocalResources(DetectEngineCtx *de_ctx)
{
    char *filename = NULL;
    const char *eb = NULL;
    int eo;
    int opts = 0;

    /* init the hash table to be used by the classification config Classtypes */
    de_ctx->class_conf_ht = HashTableInit(4096, SCClassConfClasstypeHashFunc,
                                          SCClassConfClasstypeHashCompareFunc,
                                          SCClassConfClasstypeHashFree);
    if (de_ctx->class_conf_ht == NULL) {
        SCLogError(SC_ERR_HASH_TABLE_INIT, "Error initializing the hash "
                   "table");
        goto error;
    }

    /* if it is not NULL, use the file descriptor.  The hack so that we can
     * avoid using a dummy classification file for testing purposes and
     * instead use an input stream against a buffer containing the
     * classification strings */
    if (fd == NULL) {
        filename = SCClassConfGetConfFilename();
        if ( (fd = fopen(filename, "r")) == NULL) {
            SCLogError(SC_ERR_FOPEN, "Error opening file: \"%s\": %s", filename, strerror(errno));
            goto error;
        }
    }

    regex = pcre_compile(DETECT_CLASSCONFIG_REGEX, opts, &eb, &eo, NULL);
    if (regex == NULL) {
        SCLogDebug("Compile of \"%s\" failed at offset %" PRId32 ": %s",
                   DETECT_CLASSCONFIG_REGEX, eo, eb);
        goto error;
    }

    regex_study = pcre_study(regex, 0, &eb);
    if (eb != NULL) {
        SCLogDebug("pcre study failed: %s", eb);
        goto error;
    }

    return 0;

 error:
    if (de_ctx->class_conf_ht != NULL) {
        HashTableFree(de_ctx->class_conf_ht);
        de_ctx->class_conf_ht = NULL;
    }
    if (fd != NULL) {
        fclose(fd);
        fd = NULL;
    }

    if (regex != NULL) {
        pcre_free(regex);
        regex = NULL;
    }
    if (regex_study != NULL) {
        pcre_free(regex_study);
        regex_study = NULL;
    }

    return -1;
}


/**
 * \brief Returns the path for the Classification Config file.  We check if we
 *        can retrieve the path from the yaml conf file.  If it is not present,
 *        return the default path for the classification file which is
 *        "./classification.config".
 *
 * \retval log_filename Pointer to a string containing the path for the
 *                      Classification Config file.
 */
static char *SCClassConfGetConfFilename(void)
{
    char *log_filename = NULL;

    if (ConfGet("classification-file", &log_filename) != 1) {
        log_filename = (char *)default_file_path;
    }

    return log_filename;
}

/**
 * \brief Releases resources used by the Classification Config API.
 */
static void SCClassConfDeInitLocalResources(DetectEngineCtx *de_ctx)
{

    fclose(fd);
    default_file_path = SC_CLASS_CONF_DEF_CONF_FILEPATH;
    fd = NULL;
    if (regex != NULL) {
        pcre_free(regex);
        regex = NULL;
    }
    if (regex_study != NULL) {
        pcre_free(regex_study);
        regex_study = NULL;
    }

    return;
}

/**
 * \brief Releases resources used by the Classification Config API.
 */
void SCClassConfDeInitContext(DetectEngineCtx *de_ctx)
{
    if (de_ctx->class_conf_ht != NULL)
        HashTableFree(de_ctx->class_conf_ht);

    de_ctx->class_conf_ht = NULL;

    return;
}

/**
 * \brief Converts a string to lowercase.
 *
 * \param str Pointer to the string to be converted.
 */
static char *SCClassConfStringToLowercase(const char *str)
{
    char *new_str = NULL;
    char *temp_str = NULL;

    if ( (new_str = SCStrdup(str)) == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        exit(EXIT_FAILURE);
    }

    temp_str = new_str;
    while (*temp_str != '\0') {
        *temp_str = tolower((unsigned char)*temp_str);
        temp_str++;
    }

    return new_str;
}

/**
 * \brief Parses a line from the classification file and adds it to Classtype
 *        hash table in DetectEngineCtx, i.e. DetectEngineCtx->class_conf_ht.
 *
 * \param rawstr Pointer to the string to be parsed.
 * \param index  Relative index of the string to be parsed.
 * \param de_ctx Pointer to the Detection Engine Context.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
int SCClassConfAddClasstype(char *rawstr, uint8_t index, DetectEngineCtx *de_ctx)
{
    const char *ct_name = NULL;
    const char *ct_desc = NULL;
    const char *ct_priority_str = NULL;
    int ct_priority = 0;
    uint8_t ct_id = index;

    SCClassConfClasstype *ct_new = NULL;
    SCClassConfClasstype *ct_lookup = NULL;

#define MAX_SUBSTRINGS 30
    int ret = 0;
    int ov[MAX_SUBSTRINGS];

    ret = pcre_exec(regex, regex_study, rawstr, strlen(rawstr), 0, 0, ov, 30);
    if (ret < 0) {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "Invalid Classtype in "
                   "classification.config file");
        goto error;
    }

    /* retrieve the classtype name */
    ret = pcre_get_substring((char *)rawstr, ov, 30, 1, &ct_name);
    if (ret < 0) {
        SCLogInfo("pcre_get_substring() failed");
        goto error;
    }

    /* retrieve the classtype description */
    ret = pcre_get_substring((char *)rawstr, ov, 30, 2, &ct_desc);
    if (ret < 0) {
        SCLogInfo("pcre_get_substring() failed");
        goto error;
    }

    /* retrieve the classtype priority */
    ret = pcre_get_substring((char *)rawstr, ov, 30, 3, &ct_priority_str);
    if (ret < 0) {
        SCLogInfo("pcre_get_substring() failed");
        goto error;
    }
    if (ct_priority_str == NULL) {
        goto error;
    }

    ct_priority = atoi(ct_priority_str);

    /* Create a new instance of the parsed Classtype string */
    ct_new = SCClassConfAllocClasstype(ct_id, ct_name, ct_desc, ct_priority);
    if (ct_new == NULL)
        goto error;

    /* Check if the Classtype is present in the HashTable.  In case it's present
     * ignore it, as it is a duplicate.  If not present, add it to the table */
    ct_lookup = HashTableLookup(de_ctx->class_conf_ht, ct_new, 0);
    if (ct_lookup == NULL) {
        if (HashTableAdd(de_ctx->class_conf_ht, ct_new, 0) < 0)
            SCLogDebug("HashTable Add failed");
    } else {
        SCLogDebug("Duplicate classtype found inside classification.config");
        if (ct_new->classtype_desc) SCFree(ct_new->classtype_desc);
        if (ct_new->classtype) SCFree(ct_new->classtype);
        SCFree(ct_new);
    }

    if (ct_name) SCFree((char *)ct_name);
    if (ct_desc) SCFree((char *)ct_desc);
    if (ct_priority_str) SCFree((char *)ct_priority_str);
    return 0;

 error:
    if (ct_name) SCFree((char *)ct_name);
    if (ct_desc) SCFree((char *)ct_desc);
    if (ct_priority_str) SCFree((char *)ct_priority_str);

    return -1;
}

/**
 * \brief Checks if a string is a comment or a blank line.
 *
 *        Comments lines are lines of the following format -
 *        "# This is a comment string" or
 *        "   # This is a comment string".
 *
 * \param line String that has to be checked
 *
 * \retval 1 On the argument string being a comment or blank line
 * \retval 0 Otherwise
 */
static int SCClassConfIsLineBlankOrComment(char *line)
{
    while (*line != '\0') {
        /* we have a comment */
        if (*line == '#')
            return 1;

        /* this line is neither a comment line, nor a blank line */
        if (!isspace((unsigned char)*line))
            return 0;

        line++;
    }

    /* we have a blank line */
    return 1;
}

/**
 * \brief Parses the Classification Config file and updates the
 *        DetectionEngineCtx->class_conf_ht with the Classtype information.
 *
 * \param de_ctx Pointer to the Detection Engine Context.
 */
void SCClassConfParseFile(DetectEngineCtx *de_ctx)
{
    char line[1024];
    uint8_t i = 1;

    while (fgets(line, sizeof(line), fd) != NULL) {
        if (SCClassConfIsLineBlankOrComment(line))
            continue;

        SCClassConfAddClasstype(line, i, de_ctx);
        i++;
    }

#ifdef UNITTESTS
    SCLogInfo("Added \"%d\" classification types from the classification file",
              de_ctx->class_conf_ht->count);
#endif

    return;
}

/**
 * \brief Returns a new SCClassConfClasstype instance.  The classtype string
 *        is converted into lowercase, before being assigned to the instance.
 *
 * \param classtype      Pointer to the classification type.
 * \param classtype_desc Pointer to the classification type description.
 * \param priority       Holds the priority for the classification type.
 *
 * \retval ct Pointer to the new instance of SCClassConfClasstype on success;
 *            NULL on failure.
 */
SCClassConfClasstype *SCClassConfAllocClasstype(uint8_t classtype_id,
                                                const char *classtype,
                                                const char *classtype_desc,
                                                int priority)
{
    SCClassConfClasstype *ct = NULL;

    if (classtype == NULL)
        return NULL;

    if ( (ct = SCMalloc(sizeof(SCClassConfClasstype))) == NULL)
        return NULL;
    memset(ct, 0, sizeof(SCClassConfClasstype));

    if ( (ct->classtype = SCClassConfStringToLowercase(classtype)) == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        exit(EXIT_FAILURE);
    }

    if (classtype_desc != NULL &&
        (ct->classtype_desc = SCStrdup(classtype_desc)) == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        exit(EXIT_FAILURE);
    }

    ct->classtype_id = classtype_id;
    ct->priority = priority;

    return ct;
}

/**
 * \brief Frees a SCClassConfClasstype instance
 *
 * \param Pointer to the SCClassConfClasstype instance that has to be freed
 */
void SCClassConfDeAllocClasstype(SCClassConfClasstype *ct)
{
    if (ct != NULL) {
        if (ct->classtype != NULL)
            SCFree(ct->classtype);

        if (ct->classtype_desc != NULL)
            SCFree(ct->classtype_desc);

        SCFree(ct);
    }

    return;
}

/**
 * \brief Hashing function to be used to hash the Classtype name.  Would be
 *        supplied as an argument to the HashTableInit function for
 *        DetectEngineCtx->class_conf_ht.
 *
 * \param ht      Pointer to the HashTable.
 * \param data    Pointer to the data to be hashed.  In this case, the data
 *                would be a pointer to a SCClassConfClasstype instance.
 * \param datalen Not used by this function.
 */
uint32_t SCClassConfClasstypeHashFunc(HashTable *ht, void *data, uint16_t datalen)
{
    SCClassConfClasstype *ct = (SCClassConfClasstype *)data;
    uint32_t hash = 0;
    int i = 0;

    int len = strlen(ct->classtype);

    for (i = 0; i < len; i++)
        hash += tolower((unsigned char)(ct->classtype)[i]);

    hash = hash % ht->array_size;

    return hash;
}

/**
 * \brief Used to compare two Classtypes that have been stored in the HashTable.
 *        This function is supplied as an argument to the HashTableInit function
 *        for DetectionEngineCtx->class_conf_ct.
 *
 * \param data1 Pointer to the first SCClassConfClasstype to be compared.
 * \param len1  Not used by this function.
 * \param data2 Pointer to the second SCClassConfClasstype to be compared.
 * \param len2  Not used by this function.
 *
 * \retval 1 On data1 and data2 being equal.
 * \retval 0 On data1 and data2 not being equal.
 */
char SCClassConfClasstypeHashCompareFunc(void *data1, uint16_t datalen1,
                                         void *data2, uint16_t datalen2)
{
    SCClassConfClasstype *ct1 = (SCClassConfClasstype *)data1;
    SCClassConfClasstype *ct2 = (SCClassConfClasstype *)data2;
    int len1 = 0;
    int len2 = 0;

    if (ct1 == NULL || ct2 == NULL)
        return 0;

    if (ct1->classtype == NULL || ct2->classtype == NULL)
        return 0;

    len1 = strlen(ct1->classtype);
    len2 = strlen(ct2->classtype);

    if (len1 == len2 && memcmp(ct1->classtype, ct2->classtype, len1) == 0) {
        SCLogDebug("Match found inside Classification-Config hash function");
        return 1;
    }

    return 0;
}

/**
 * \brief Used to free the Classification Config Hash Data that was stored in
 *        DetectEngineCtx->class_conf_ht Hashtable.
 *
 * \param ch Pointer to the data that has to be freed.
 */
void SCClassConfClasstypeHashFree(void *ch)
{
    SCClassConfDeAllocClasstype(ch);

    return;
}

/**
 * \brief Loads the Classtype info from the classification.config file.
 *
 *        The classification.config file contains the different classtypes,
 *        that can be used to label Signatures.  Each line of the file should
 *        have the following format -
 *        classtype_name, classtype_description, priority
 *        None of the above parameters should hold a quote inside the file.
 *
 * \param de_ctx Pointer to the Detection Engine Context that should be updated
 *               with Classtype information.
 */
void SCClassConfLoadClassficationConfigFile(DetectEngineCtx *de_ctx)
{
    if (SCClassConfInitContextAndLocalResources(de_ctx) == -1) {
        SCLogInfo("Please check the \"classification-file\" option in your suricata.yaml file");
        exit(EXIT_FAILURE);
    }

    SCClassConfParseFile(de_ctx);
    SCClassConfDeInitLocalResources(de_ctx);

    return;
}

/**
 * \brief Gets the classtype from the corresponding hash table stored
 *        in the Detection Engine Context's class conf ht, given the
 *        classtype name.
 *
 * \param ct_name Pointer to the classtype name that has to be looked up.
 * \param de_ctx  Pointer to the Detection Engine Context.
 *
 * \retval lookup_ct_info Pointer to the SCClassConfClasstype instance from
 *                        the hash table on success; NULL on failure.
 */
SCClassConfClasstype *SCClassConfGetClasstype(const char *ct_name,
                                              DetectEngineCtx *de_ctx)
{
    SCClassConfClasstype *ct_info = SCClassConfAllocClasstype(0, ct_name, NULL,
                                                              0);
    if (ct_info == NULL)
        return NULL;
    SCClassConfClasstype *lookup_ct_info = HashTableLookup(de_ctx->class_conf_ht,
                                                           ct_info, 0);

    SCClassConfDeAllocClasstype(ct_info);
    return lookup_ct_info;
}

/*----------------------------------Unittests---------------------------------*/


#ifdef UNITTESTS

/**
 * \brief Creates a dummy classification file, with all valid Classtypes, for
 *        testing purposes.
 *
 * \file_path Pointer to the file_path for the dummy classification file.
 */
void SCClassConfGenerateValidDummyClassConfigFD01(void)
{
    const char *buffer =
        "config classification: nothing-wrong,Nothing Wrong With Us,3\n"
        "config classification: unknown,Unknown are we,3\n"
        "config classification: bad-unknown,We think it's bad, 2\n";

    fd = SCFmemopen((void *)buffer, strlen(buffer), "r");
    if (fd == NULL)
        SCLogDebug("Error with SCFmemopen() called by Classifiation Config test code");

    return;
}

/**
 * \brief Creates a dummy classification file, with some valid Classtypes and a
 *        couple of invalid Classtypes, for testing purposes.
 *
 * \file_path Pointer to the file_path for the dummy classification file.
 */
void SCClassConfGenerateInValidDummyClassConfigFD02(void)
{
    const char *buffer =
        "config classification: not-suspicious,Not Suspicious Traffic,3\n"
        "onfig classification: unknown,Unknown Traffic,3\n"
        "config classification: _badunknown,Potentially Bad Traffic, 2\n"
        "config classification: bamboola1,Unknown Traffic,3\n"
        "config classification: misc-activity,Misc activity,-1\n"
        "config classification: policy-violation,Potential Corporate "
        "config classification: bamboola,Unknown Traffic,3\n";

    fd = SCFmemopen((void *)buffer, strlen(buffer), "r");
    if (fd == NULL)
        SCLogDebug("Error with SCFmemopen() called by Classifiation Config test code");

    return;
}

/**
 * \brief Creates a dummy classification file, with all invalid Classtypes, for
 *        testing purposes.
 *
 * \file_path Pointer to the file_path for the dummy classification file.
 */
void SCClassConfGenerateInValidDummyClassConfigFD03(void)
{
    const char *buffer =
        "conig classification: not-suspicious,Not Suspicious Traffic,3\n"
        "onfig classification: unknown,Unknown Traffic,3\n"
        "config classification: _badunknown,Potentially Bad Traffic, 2\n"
        "config classification: misc-activity,Misc activity,-1\n";

    fd = SCFmemopen((void *)buffer, strlen(buffer), "r");
    if (fd == NULL)
        SCLogDebug("Error with SCFmemopen() called by Classifiation Config test code");

    return;
}

/**
 * \brief Deletes a file, whose path is specified as the argument.
 *
 * \file_path Pointer to the file_path that has to be deleted.
 */
void SCClassConfDeleteDummyClassificationConfigFD(void)
{
    if (fd != NULL) {
        fclose(fd);
        fd = NULL;
    }

    return;
}

/**
 * \test Check that the classification file is loaded and the detection engine
 *       content class_conf_hash_table loaded with the classtype data.
 */
int SCClassConfTest01(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    int result = 0;

    if (de_ctx == NULL)
        return result;

    SCClassConfGenerateValidDummyClassConfigFD01();
    SCClassConfLoadClassficationConfigFile(de_ctx);
    SCClassConfDeleteDummyClassificationConfigFD();

    if (de_ctx->class_conf_ht == NULL)
        return result;

    result = (de_ctx->class_conf_ht->count == 3);
    if (result == 0) printf("de_ctx->class_conf_ht->count %u: ", de_ctx->class_conf_ht->count);

    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Check that invalid classtypes present in the classification config file
 *       aren't loaded.
 */
int SCClassConfTest02(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    int result = 0;

    if (de_ctx == NULL)
        return result;

    SCClassConfGenerateInValidDummyClassConfigFD03();
    SCClassConfLoadClassficationConfigFile(de_ctx);
    SCClassConfDeleteDummyClassificationConfigFD();

    if (de_ctx->class_conf_ht == NULL)
        return result;

    result = (de_ctx->class_conf_ht->count == 0);

    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Check that only valid classtypes are loaded into the hash table from
 *       the classfication.config file.
 */
int SCClassConfTest03(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    int result = 0;

    if (de_ctx == NULL)
        return result;

    SCClassConfGenerateInValidDummyClassConfigFD02();
    SCClassConfLoadClassficationConfigFile(de_ctx);
    SCClassConfDeleteDummyClassificationConfigFD();

    if (de_ctx->class_conf_ht == NULL)
        return result;

    result = (de_ctx->class_conf_ht->count == 3);

    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Check if the classtype info from the classification.config file have
 *       been loaded into the hash table.
 */
int SCClassConfTest04(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    int result = 1;

    if (de_ctx == NULL)
        return 0;

    SCClassConfGenerateValidDummyClassConfigFD01();
    SCClassConfLoadClassficationConfigFile(de_ctx);
    SCClassConfDeleteDummyClassificationConfigFD();

    if (de_ctx->class_conf_ht == NULL)
        return 0;

    result = (de_ctx->class_conf_ht->count == 3);

    result &= (SCClassConfGetClasstype("unknown", de_ctx) != NULL);
    result &= (SCClassConfGetClasstype("unKnoWn", de_ctx) != NULL);
    result &= (SCClassConfGetClasstype("bamboo", de_ctx) == NULL);
    result &= (SCClassConfGetClasstype("bad-unknown", de_ctx) != NULL);
    result &= (SCClassConfGetClasstype("BAD-UNKnOWN", de_ctx) != NULL);
    result &= (SCClassConfGetClasstype("bed-unknown", de_ctx) == NULL);

    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Check if the classtype info from the invalid classification.config file
 *       have not been loaded into the hash table, and cross verify to check
 *       that the hash table contains no classtype data.
 */
int SCClassConfTest05(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    int result = 1;

    if (de_ctx == NULL)
        return 0;

    SCClassConfGenerateInValidDummyClassConfigFD03();
    SCClassConfLoadClassficationConfigFile(de_ctx);
    SCClassConfDeleteDummyClassificationConfigFD();

    if (de_ctx->class_conf_ht == NULL)
        return 0;

    result = (de_ctx->class_conf_ht->count == 0);

    result &= (SCClassConfGetClasstype("unknown", de_ctx) == NULL);
    result &= (SCClassConfGetClasstype("unKnoWn", de_ctx) == NULL);
    result &= (SCClassConfGetClasstype("bamboo", de_ctx) == NULL);
    result &= (SCClassConfGetClasstype("bad-unknown", de_ctx) == NULL);
    result &= (SCClassConfGetClasstype("BAD-UNKnOWN", de_ctx) == NULL);
    result &= (SCClassConfGetClasstype("bed-unknown", de_ctx) == NULL);

    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Check if the classtype info from the classification.config file have
 *       been loaded into the hash table.
 */
int SCClassConfTest06(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    int result = 1;

    if (de_ctx == NULL)
        return 0;

    SCClassConfGenerateInValidDummyClassConfigFD02();
    SCClassConfLoadClassficationConfigFile(de_ctx);
    SCClassConfDeleteDummyClassificationConfigFD();

    if (de_ctx->class_conf_ht == NULL)
        return 0;

    result = (de_ctx->class_conf_ht->count == 3);

    result &= (SCClassConfGetClasstype("unknown", de_ctx) == NULL);
    result &= (SCClassConfGetClasstype("not-suspicious", de_ctx) != NULL);
    result &= (SCClassConfGetClasstype("bamboola1", de_ctx) != NULL);
    result &= (SCClassConfGetClasstype("bamboola1", de_ctx) != NULL);
    result &= (SCClassConfGetClasstype("BAMBOolA1", de_ctx) != NULL);
    result &= (SCClassConfGetClasstype("unkNOwn", de_ctx) == NULL);

    DetectEngineCtxFree(de_ctx);

    return result;
}

#endif /* UNITTESTS */

/**
 * \brief This function registers unit tests for Classification Config API.
 */
void SCClassConfRegisterTests(void)
{

#ifdef UNITTESTS

    UtRegisterTest("SCClassConfTest01", SCClassConfTest01, 1);
    UtRegisterTest("SCClassConfTest02", SCClassConfTest02, 1);
    UtRegisterTest("SCClassConfTest03", SCClassConfTest03, 1);
    UtRegisterTest("SCClassConfTest04", SCClassConfTest04, 1);
    UtRegisterTest("SCClassConfTest05", SCClassConfTest05, 1);
    UtRegisterTest("SCClassConfTest06", SCClassConfTest06, 1);

#endif /* UNITTESTS */

}
