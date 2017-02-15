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
 */

#include "suricata-common.h"
#include "threads.h"
#include "decode.h"

#include "detect.h"
#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-engine-state.h"
#include "detect-content.h"
#include "detect-pcre.h"
#include "detect-bytejump.h"
#include "detect-bytetest.h"
#include "detect-byte-extract.h"
#include "detect-isdataat.h"

#include "app-layer-protos.h"

#include "flow.h"
#include "flow-var.h"
#include "flow-util.h"

#include "util-byte.h"
#include "util-debug.h"
#include "util-unittest.h"
#include "util-unittest-helper.h"
#include "util-spm.h"

/* the default value of endianess to be used, if none's specified */
#define DETECT_BYTE_EXTRACT_ENDIAN_DEFAULT DETECT_BYTE_EXTRACT_ENDIAN_BIG

/* the base to be used if string mode is specified.  These options would be
 * specified in DetectByteParseData->base */
#define DETECT_BYTE_EXTRACT_BASE_NONE 0
#define DETECT_BYTE_EXTRACT_BASE_HEX  16
#define DETECT_BYTE_EXTRACT_BASE_DEC  10
#define DETECT_BYTE_EXTRACT_BASE_OCT   8

/* the default value for multiplier.  Either ways we always store a
 * multiplier, 1 or otherwise, so that we can always multiply the extracted
 * value and store it, instead of checking if a multiplier is set or not */
#define DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT 1
/* the min/max limit for multiplier */
#define DETECT_BYTE_EXTRACT_MULTIPLIER_MIN_LIMIT 1
#define DETECT_BYTE_EXTRACT_MULTIPLIER_MAX_LIMIT 65535

/* the max no of bytes that can be extracted in string mode - (string, hex)
 * (string, oct) or (string, dec) */
#define STRING_MAX_BYTES_TO_EXTRACT_FOR_OCT 23
#define STRING_MAX_BYTES_TO_EXTRACT_FOR_DEC 20
#define STRING_MAX_BYTES_TO_EXTRACT_FOR_HEX 14
/* the max no of bytes that can be extraced in non-string mode */
#define NO_STRING_MAX_BYTES_TO_EXTRACT 8

#define PARSE_REGEX "^"                                                  \
    "\\s*([0-9]+)\\s*"                                                   \
    ",\\s*(-?[0-9]+)\\s*"                                               \
    ",\\s*([^\\s,]+)\\s*"                                                \
    "(?:(?:,\\s*([^\\s,]+)\\s*)|(?:,\\s*([^\\s,]+)\\s+([^\\s,]+)\\s*))?" \
    "(?:(?:,\\s*([^\\s,]+)\\s*)|(?:,\\s*([^\\s,]+)\\s+([^\\s,]+)\\s*))?" \
    "(?:(?:,\\s*([^\\s,]+)\\s*)|(?:,\\s*([^\\s,]+)\\s+([^\\s,]+)\\s*))?" \
    "(?:(?:,\\s*([^\\s,]+)\\s*)|(?:,\\s*([^\\s,]+)\\s+([^\\s,]+)\\s*))?" \
    "(?:(?:,\\s*([^\\s,]+)\\s*)|(?:,\\s*([^\\s,]+)\\s+([^\\s,]+)\\s*))?" \
    "$"

static pcre *parse_regex;
static pcre_extra *parse_regex_study;

int DetectByteExtractMatch(ThreadVars *, DetectEngineThreadCtx *,
                           Packet *, Signature *, SigMatch *);
int DetectByteExtractSetup(DetectEngineCtx *, Signature *, char *);
void DetectByteExtractRegisterTests(void);
void DetectByteExtractFree(void *);

/**
 * \brief Registers the keyword handlers for the "byte_extract" keyword.
 */
void DetectByteExtractRegister(void)
{
    const char *eb;
    int eo;
    int opts = 0;

    sigmatch_table[DETECT_BYTE_EXTRACT].name = "byte_extract";
    sigmatch_table[DETECT_BYTE_EXTRACT].Match = NULL;
    sigmatch_table[DETECT_BYTE_EXTRACT].AppLayerMatch = NULL;
    sigmatch_table[DETECT_BYTE_EXTRACT].Setup = DetectByteExtractSetup;
    sigmatch_table[DETECT_BYTE_EXTRACT].Free = DetectByteExtractFree;
    sigmatch_table[DETECT_BYTE_EXTRACT].RegisterTests = DetectByteExtractRegisterTests;

    sigmatch_table[DETECT_BYTE_EXTRACT].flags |= SIGMATCH_PAYLOAD;

    parse_regex = pcre_compile(PARSE_REGEX, opts, &eb, &eo, NULL);
    if (parse_regex == NULL) {
        SCLogError(SC_ERR_PCRE_COMPILE, "pcre compile of \"%s\" failed "
                   "at offset %" PRId32 ": %s", PARSE_REGEX, eo, eb);
        goto error;
    }

    parse_regex_study = pcre_study(parse_regex, 0, &eb);
    if (eb != NULL) {
        SCLogError(SC_ERR_PCRE_STUDY, "pcre study failed: %s", eb);
        goto error;
    }

    return;
 error:
    return;
}

int DetectByteExtractDoMatch(DetectEngineThreadCtx *det_ctx, SigMatch *sm,
                             Signature *s, uint8_t *payload,
                             uint16_t payload_len, uint64_t *value,
                             uint8_t endian)
{
    DetectByteExtractData *data = (DetectByteExtractData *)sm->ctx;
    uint8_t *ptr = NULL;
    int32_t len = 0;
    uint64_t val = 0;
    int extbytes;

    if (payload_len == 0) {
        return 0;
    }

    /* Calculate the ptr value for the bytetest and length remaining in
     * the packet from that point.
     */
    if (data->flags & DETECT_BYTE_EXTRACT_FLAG_RELATIVE) {
        SCLogDebug("relative, working with det_ctx->buffer_offset %"PRIu32", "
                   "data->offset %"PRIu32"", det_ctx->buffer_offset, data->offset);

        ptr = payload + det_ctx->buffer_offset;
        len = payload_len - det_ctx->buffer_offset;

        /* No match if there is no relative base */
        if (len == 0) {
            return 0;
        }

        ptr += data->offset;
        len -= data->offset;

        //PrintRawDataFp(stdout,ptr,len);
    } else {
        SCLogDebug("absolute, data->offset %"PRIu32"", data->offset);

        ptr = payload + data->offset;
        len = payload_len - data->offset;
    }

    /* Validate that the to-be-extracted is within the packet */
    if (ptr < payload || data->nbytes > len) {
        SCLogDebug("Data not within payload pkt=%p, ptr=%p, len=%"PRIu32", nbytes=%d",
                    payload, ptr, len, data->nbytes);
        return 0;
    }

    /* Extract the byte data */
    if (data->flags & DETECT_BYTE_EXTRACT_FLAG_STRING) {
        extbytes = ByteExtractStringUint64(&val, data->base,
                                           data->nbytes, (const char *)ptr);
        if (extbytes <= 0) {
            /* strtoull() return 0 if there is no numeric value in data string */
            if (val == 0) {
                SCLogDebug("No Numeric value");
                return 0;
            } else {
                SCLogError(SC_ERR_INVALID_NUM_BYTES, "Error extracting %d "
                        "bytes of string data: %d", data->nbytes, extbytes);
                return -1;
            }
        }
    } else {
        int endianness = (endian == DETECT_BYTE_EXTRACT_ENDIAN_BIG) ?
                          BYTE_BIG_ENDIAN : BYTE_LITTLE_ENDIAN;
        extbytes = ByteExtractUint64(&val, endianness, data->nbytes, ptr);
        if (extbytes != data->nbytes) {
            SCLogError(SC_ERR_INVALID_NUM_BYTES, "Error extracting %d bytes "
                   "of numeric data: %d\n", data->nbytes, extbytes);
            return 0;
        }
    }

    /* Adjust the jump value based on flags */
    val *= data->multiplier_value;
    if (data->flags & DETECT_BYTE_EXTRACT_FLAG_ALIGN) {
        if ((val % data->align_value) != 0) {
            val += data->align_value - (val % data->align_value);
        }
    }

    ptr += extbytes;

    det_ctx->buffer_offset = ptr - payload;

    *value = val;

    return 1;
}


int DetectByteExtractMatch(ThreadVars *tv, DetectEngineThreadCtx *det_ctx,
                           Packet *p, Signature *s, SigMatch *m)
{
    goto end;
 end:
    return 1;
}

/**
 * \internal
 * \brief Used to parse byte_extract arg.
 *
 * \arg The argument to parse.
 *
 * \param bed On success an instance containing the parsed data.
 *            On failure, NULL.
 */
static inline DetectByteExtractData *DetectByteExtractParse(char *arg)
{
    DetectByteExtractData *bed = NULL;
#define MAX_SUBSTRINGS 100
    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];
    char *str_ptr;
    int i = 0;

    ret = pcre_exec(parse_regex, parse_regex_study, arg,
                    strlen(arg), 0, 0, ov, MAX_SUBSTRINGS);
    if (ret < 3 || ret > 19) {
        SCLogError(SC_ERR_PCRE_PARSE, "parse error, ret %" PRId32
                   ", string \"%s\"", ret, arg);
        SCLogError(SC_ERR_INVALID_SIGNATURE, "Invalid arg to byte_extract : %s "
                   "for byte_extract", arg);
        goto error;
    }

    bed = SCMalloc(sizeof(DetectByteExtractData));
    if (unlikely(bed == NULL))
        goto error;
    memset(bed, 0, sizeof(DetectByteExtractData));

    /* no of bytes to extract */
    res = pcre_get_substring((char *)arg, ov,
                             MAX_SUBSTRINGS, 1, (const char **)&str_ptr);
    if (res < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed "
                   "for arg 1 for byte_extract");
        goto error;
    }
    bed->nbytes = atoi(str_ptr);

    /* offset */
    res = pcre_get_substring((char *)arg, ov,
                             MAX_SUBSTRINGS, 2, (const char **)&str_ptr);
    if (res < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed "
                   "for arg 2 for byte_extract");
        goto error;
    }
    int offset = atoi(str_ptr);
    if (offset < -65535 || offset > 65535) {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "byte_extract offset invalid - %d.  "
                   "The right offset range is -65535 to 65535", offset);
        goto error;
    }
    bed->offset = offset;

    /* var name */
    res = pcre_get_substring((char *)arg, ov,
                             MAX_SUBSTRINGS, 3, (const char **)&str_ptr);
    if (res < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed "
                   "for arg 3 for byte_extract");
        goto error;
    }
    bed->name = SCStrdup(str_ptr);
    if (bed->name == NULL)
        goto error;

    /* check out other optional args */
    for (i = 4; i < ret; i++) {
        res = pcre_get_substring((char *)arg, ov,
                                 MAX_SUBSTRINGS, i, (const char **)&str_ptr);
        if (res < 0) {
            SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed "
                       "for arg %d for byte_extract", i);
            goto error;
        }

        if (strcmp("relative", str_ptr) == 0) {
            if (bed->flags & DETECT_BYTE_EXTRACT_FLAG_RELATIVE) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "relative specified more "
                           "than once for byte_extract");
                goto error;
            }
            bed->flags |= DETECT_BYTE_EXTRACT_FLAG_RELATIVE;
        } else if (strcmp("multiplier", str_ptr) == 0) {
            if (bed->flags & DETECT_BYTE_EXTRACT_FLAG_MULTIPLIER) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "multiplier specified more "
                           "than once for byte_extract");
                goto error;
            }
            bed->flags |= DETECT_BYTE_EXTRACT_FLAG_MULTIPLIER;
            i++;
            res = pcre_get_substring((char *)arg, ov,
                                     MAX_SUBSTRINGS, i, (const char **)&str_ptr);
            if (res < 0) {
                SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed "
                           "for arg %d for byte_extract", i);
                goto error;
            }
            int multiplier = atoi(str_ptr);
            if (multiplier < DETECT_BYTE_EXTRACT_MULTIPLIER_MIN_LIMIT ||
                multiplier > DETECT_BYTE_EXTRACT_MULTIPLIER_MAX_LIMIT) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "multipiler_value invalid "
                           "- %d.  The range is %d-%d",
                           multiplier,
                           DETECT_BYTE_EXTRACT_MULTIPLIER_MIN_LIMIT,
                           DETECT_BYTE_EXTRACT_MULTIPLIER_MAX_LIMIT);
                goto error;
            }
            bed->multiplier_value = multiplier;
        } else if (strcmp("big", str_ptr) == 0) {
            if (bed->flags & DETECT_BYTE_EXTRACT_FLAG_ENDIAN) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "endian option specified "
                           "more than once for byte_extract");
                goto error;
            }
            bed->flags |= DETECT_BYTE_EXTRACT_FLAG_ENDIAN;
            bed->endian = DETECT_BYTE_EXTRACT_ENDIAN_BIG;
        } else if (strcmp("little", str_ptr) == 0) {
            if (bed->flags & DETECT_BYTE_EXTRACT_FLAG_ENDIAN) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "endian option specified "
                           "more than once for byte_extract");
                goto error;
            }
            bed->flags |= DETECT_BYTE_EXTRACT_FLAG_ENDIAN;
            bed->endian = DETECT_BYTE_EXTRACT_ENDIAN_LITTLE;
        } else if (strcmp("dce", str_ptr) == 0) {
            if (bed->flags & DETECT_BYTE_EXTRACT_FLAG_ENDIAN) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "endian option specified "
                           "more than once for byte_extract");
                goto error;
            }
            bed->flags |= DETECT_BYTE_EXTRACT_FLAG_ENDIAN;
            bed->endian = DETECT_BYTE_EXTRACT_ENDIAN_DCE;
        } else if (strcmp("string", str_ptr) == 0) {
            if (bed->flags & DETECT_BYTE_EXTRACT_FLAG_STRING) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "string specified more "
                           "than once for byte_extract");
                goto error;
            }
            if (bed->base != DETECT_BYTE_EXTRACT_BASE_NONE) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "The right way to specify "
                           "base is (string, base) and not (base, string) "
                           "for byte_extract");
                goto error;
            }
            bed->flags |= DETECT_BYTE_EXTRACT_FLAG_STRING;
        } else if (strcmp("hex", str_ptr) == 0) {
            if (!(bed->flags & DETECT_BYTE_EXTRACT_FLAG_STRING)) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "Base(hex) specified "
                           "without specifying string.  The right way is "
                           "(string, base) and not (base, string)");
                goto error;
            }
            if (bed->base != DETECT_BYTE_EXTRACT_BASE_NONE) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "More than one base "
                           "specified for byte_extract");
                goto error;
            }
            bed->base = DETECT_BYTE_EXTRACT_BASE_HEX;
        } else if (strcmp("oct", str_ptr) == 0) {
            if (!(bed->flags & DETECT_BYTE_EXTRACT_FLAG_STRING)) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "Base(oct) specified "
                           "without specifying string.  The right way is "
                           "(string, base) and not (base, string)");
                goto error;
            }
            if (bed->base != DETECT_BYTE_EXTRACT_BASE_NONE) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "More than one base "
                           "specified for byte_extract");
                goto error;
            }
            bed->base = DETECT_BYTE_EXTRACT_BASE_OCT;
        } else if (strcmp("dec", str_ptr) == 0) {
            if (!(bed->flags & DETECT_BYTE_EXTRACT_FLAG_STRING)) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "Base(dec) specified "
                           "without specifying string.  The right way is "
                           "(string, base) and not (base, string)");
                goto error;
            }
            if (bed->base != DETECT_BYTE_EXTRACT_BASE_NONE) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "More than one base "
                           "specified for byte_extract");
                goto error;
            }
            bed->base = DETECT_BYTE_EXTRACT_BASE_DEC;
        } else if (strcmp("align", str_ptr) == 0) {
            if (bed->flags & DETECT_BYTE_EXTRACT_FLAG_ALIGN) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "Align specified more "
                           "than once for byte_extract");
                goto error;
            }
            bed->flags |= DETECT_BYTE_EXTRACT_FLAG_ALIGN;
            i++;
            res = pcre_get_substring((char *)arg, ov,
                                     MAX_SUBSTRINGS, i, (const char **)&str_ptr);
            if (res < 0) {
                SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed "
                           "for arg %d in byte_extract", i);
                goto error;
            }
            bed->align_value = atoi(str_ptr);
            if (!(bed->align_value == 2 || bed->align_value == 4)) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "Invalid align_value for "
                           "byte_extract - \"%d\"", bed->align_value);
                goto error;
            }
        } else if (strcmp("", str_ptr) == 0) {
            ;
        } else {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "Invalid option - \"%s\" "
                       "specified in byte_extract", str_ptr);
            goto error;
        }
    } /* for (i = 4; i < ret; i++) */

    /* validation */
    if (!(bed->flags & DETECT_BYTE_EXTRACT_FLAG_MULTIPLIER)) {
        /* default value */
        bed->multiplier_value = DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT;
    }

    if (bed->flags & DETECT_BYTE_EXTRACT_FLAG_STRING) {
        if (bed->base == DETECT_BYTE_EXTRACT_BASE_NONE) {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "Base not specified for "
                       "byte_extract, though string was specified.  "
                       "The right options are (string, hex), (string, oct) "
                       "or (string, dec)");
            goto error;
        }
        if (bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE) {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "byte_extract can't have "
                       "endian \"big\" or \"little\" specified along with "
                       "\"string\"");
            goto error;
        }
        if (bed->base == DETECT_BYTE_EXTRACT_BASE_OCT) {
            /* if are dealing with octal nos, the max no that can fit in a 8
             * byte value is 01777777777777777777777 */
            if (bed->nbytes > STRING_MAX_BYTES_TO_EXTRACT_FOR_OCT) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "byte_extract can't process "
                           "more than %d bytes in \"string\" extraction",
                           STRING_MAX_BYTES_TO_EXTRACT_FOR_OCT);
                goto error;
            }
        } else if (bed->base == DETECT_BYTE_EXTRACT_BASE_DEC) {
            /* if are dealing with decimal nos, the max no that can fit in a 8
             * byte value is 18446744073709551615 */
            if (bed->nbytes > STRING_MAX_BYTES_TO_EXTRACT_FOR_DEC) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "byte_extract can't process "
                           "more than %d bytes in \"string\" extraction",
                           STRING_MAX_BYTES_TO_EXTRACT_FOR_DEC);
                goto error;
            }
        } else if (bed->base == DETECT_BYTE_EXTRACT_BASE_HEX) {
            /* if are dealing with hex nos, the max no that can fit in a 8
             * byte value is 0xFFFFFFFFFFFFFFFF */
            if (bed->nbytes > STRING_MAX_BYTES_TO_EXTRACT_FOR_HEX) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "byte_extract can't process "
                           "more than %d bytes in \"string\" extraction",
                           STRING_MAX_BYTES_TO_EXTRACT_FOR_HEX);
                goto error;
            }
        } else {
            ; // just a placeholder.  we won't reach here.
        }
    } else {
        if (bed->nbytes > NO_STRING_MAX_BYTES_TO_EXTRACT) {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "byte_extract can't process "
                       "more than %d bytes in \"non-string\" extraction",
                       NO_STRING_MAX_BYTES_TO_EXTRACT);
            goto error;
        }
        /* if string has not been specified and no endian option has been
         * specified, then set the default endian level of BIG */
        if (!(bed->flags & DETECT_BYTE_EXTRACT_FLAG_ENDIAN))
            bed->endian = DETECT_BYTE_EXTRACT_ENDIAN_DEFAULT;
    }

    return bed;
 error:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return NULL;
}

/**
 * \brief The setup function for the byte_extract keyword for a signature.
 *
 * \param de_ctx Pointer to the detection engine context.
 * \param s      Pointer to signature for the current Signature being parsed
 *               from the rules.
 * \param m      Pointer to the head of the SigMatch for the current rule
 *               being parsed.
 * \param arg    Pointer to the string holding the keyword value.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
int DetectByteExtractSetup(DetectEngineCtx *de_ctx, Signature *s, char *arg)
{
    SigMatch *sm = NULL;
    SigMatch *prev_pm = NULL;
    DetectByteExtractData *data = NULL;
    int ret = -1;

    data = DetectByteExtractParse(arg);
    if (data == NULL)
        goto error;

    int sm_list;
    if (s->init_flags & (SIG_FLAG_INIT_FILE_DATA | SIG_FLAG_INIT_DCE_STUB_DATA)) {
        if (s->init_flags & SIG_FLAG_INIT_FILE_DATA) {
            if (data->endian == DETECT_BYTE_EXTRACT_ENDIAN_DCE) {
                SCLogError(SC_ERR_INVALID_SIGNATURE, "dce byte_extract specified "
                           "with file_data option set.");
                goto error;
            }
            AppLayerHtpEnableResponseBodyCallback();
            sm_list = DETECT_SM_LIST_HSBDMATCH;
        } else {
            sm_list = DETECT_SM_LIST_DMATCH;
        }
        s->flags |= SIG_FLAG_APPLAYER;
        if (data->flags & DETECT_BYTE_EXTRACT_FLAG_RELATIVE) {
            prev_pm = SigMatchGetLastSMFromLists(s, 4,
                                                 DETECT_CONTENT, s->sm_lists_tail[sm_list],
                                                 DETECT_PCRE, s->sm_lists_tail[sm_list]);
        }
    } else if (data->endian == DETECT_BYTE_EXTRACT_ENDIAN_DCE) {
        if (data->flags & DETECT_BYTE_EXTRACT_FLAG_RELATIVE) {
            prev_pm = SigMatchGetLastSMFromLists(s, 12,
                                                 DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                                 DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                                 DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                                 DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                                 DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                                 DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_PMATCH]);
            if (prev_pm == NULL) {
                sm_list = DETECT_SM_LIST_PMATCH;
            } else {
                sm_list = SigMatchListSMBelongsTo(s, prev_pm);
            }
        } else {
            sm_list = DETECT_SM_LIST_PMATCH;
        }

        s->alproto = ALPROTO_DCERPC;
        s->flags |= SIG_FLAG_APPLAYER;

    } else if (data->flags & DETECT_BYTE_EXTRACT_FLAG_RELATIVE) {
        prev_pm = SigMatchGetLastSMFromLists(s, 168,
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_UMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HSBDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HHDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HMDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HCDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HRUDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HSMDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HSCDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HUADMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HHHDMATCH],
                                             DETECT_CONTENT, s->sm_lists_tail[DETECT_SM_LIST_HRHHDMATCH],

                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_UMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HSBDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HHDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HMDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HCDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HRUDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HSMDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HSCDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HUADMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HHHDMATCH],
                                             DETECT_PCRE, s->sm_lists_tail[DETECT_SM_LIST_HRHHDMATCH],

                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_UMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HSBDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HHDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HMDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HCDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HRUDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HSMDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HSCDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HUADMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HHHDMATCH],
                                             DETECT_BYTETEST, s->sm_lists_tail[DETECT_SM_LIST_HRHHDMATCH],

                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_UMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HSBDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HHDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HMDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HCDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HRUDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HSMDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HSCDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HUADMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HHHDMATCH],
                                             DETECT_BYTEJUMP, s->sm_lists_tail[DETECT_SM_LIST_HRHHDMATCH],

                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_UMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HSBDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HHDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HMDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HCDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HRUDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HSMDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HSCDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HUADMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HHHDMATCH],
                                             DETECT_BYTE_EXTRACT, s->sm_lists_tail[DETECT_SM_LIST_HRHHDMATCH],

                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_PMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_UMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HCBDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HSBDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HHDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HRHDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HMDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HCDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HRUDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HSMDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HSCDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HUADMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HHHDMATCH],
                                             DETECT_ISDATAAT, s->sm_lists_tail[DETECT_SM_LIST_HRHHDMATCH]);
        if (prev_pm == NULL) {
            sm_list = DETECT_SM_LIST_PMATCH;
        } else {
            sm_list = SigMatchListSMBelongsTo(s, prev_pm);
        }

    } else {
        sm_list = DETECT_SM_LIST_PMATCH;
    }

    if (data->endian == DETECT_BYTE_EXTRACT_ENDIAN_DCE) {
        if (s->alproto != ALPROTO_UNKNOWN && s->alproto != ALPROTO_DCERPC) {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "Non dce alproto sig has "
                       "byte_extract with dce enabled");
            goto error;
        }
        s->alproto = ALPROTO_DCERPC;
        if ((data->flags & DETECT_BYTE_EXTRACT_FLAG_STRING) ||
            (data->base == DETECT_BYTE_EXTRACT_BASE_DEC) ||
            (data->base == DETECT_BYTE_EXTRACT_BASE_HEX) ||
            (data->base == DETECT_BYTE_EXTRACT_BASE_OCT) ) {
            SCLogError(SC_ERR_CONFLICTING_RULE_KEYWORDS, "Invalid option. "
                       "A byte_jump keyword with dce holds other invalid modifiers.");
            goto error;
        }
    }

    SigMatch *prev_bed_sm = SigMatchGetLastSMFromLists(s, 2,
                                                       DETECT_BYTE_EXTRACT, s->sm_lists_tail[sm_list]);
    if (prev_bed_sm == NULL)
        data->local_id = 0;
    else
        data->local_id = ((DetectByteExtractData *)prev_bed_sm->ctx)->local_id + 1;
    if (data->local_id > de_ctx->byte_extract_max_local_id)
        de_ctx->byte_extract_max_local_id = data->local_id;


    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;
    sm->type = DETECT_BYTE_EXTRACT;
    sm->ctx = (void *)data;
    SigMatchAppendSMToList(s, sm, sm_list);


    if (!(data->flags & DETECT_BYTE_EXTRACT_FLAG_RELATIVE))
        goto okay;

    if (prev_pm == NULL)
        goto okay;

    if (prev_pm->type == DETECT_CONTENT) {
        DetectContentData *cd = (DetectContentData *)prev_pm->ctx;
        cd->flags |= DETECT_CONTENT_RELATIVE_NEXT;
    } else if (prev_pm->type == DETECT_PCRE) {
        DetectPcreData *pd = (DetectPcreData *)prev_pm->ctx;
        pd->flags |= DETECT_PCRE_RELATIVE_NEXT;
    }

 okay:
    ret = 0;
    return ret;
 error:
    DetectByteExtractFree(data);
    return ret;
}

/**
 * \brief Used to free instances of DetectByteExtractData.
 *
 * \param ptr Instance of DetectByteExtractData to be freed.
 */
void DetectByteExtractFree(void *ptr)
{
    if (ptr != NULL) {
        DetectByteExtractData *bed = ptr;
        if (bed->name != NULL)
            SCFree((void *)bed->name);
        SCFree(bed);
    }

    return;
}

SigMatch *DetectByteExtractRetrieveSMVar(const char *arg, Signature *s, int list)
{
    if (list == -1)
        return NULL;

    DetectByteExtractData *bed = NULL;
    SigMatch *sm = s->sm_lists[list];

    while (sm != NULL) {
        if (sm->type == DETECT_BYTE_EXTRACT) {
            bed = (DetectByteExtractData *)sm->ctx;
            if (strcmp(bed->name, arg) == 0) {
                return sm;
            }
        }
        sm = sm->next;
    }

    return NULL;
}

/*************************************Unittests********************************/

#ifdef UNITTESTS

int DetectByteExtractTest01(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != 0 ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_DEFAULT ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_NONE ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest02(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, relative");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_RELATIVE ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_DEFAULT ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_NONE ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest03(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, multiplier 10");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_MULTIPLIER ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_DEFAULT ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_NONE ||
        bed->align_value != 0 ||
        bed->multiplier_value != 10) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest04(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, relative, multiplier 10");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != (DETECT_BYTE_EXTRACT_FLAG_RELATIVE |
                       DETECT_BYTE_EXTRACT_FLAG_MULTIPLIER) ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_DEFAULT ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_NONE ||
        bed->align_value != 0 ||
        bed->multiplier_value != 10) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest05(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, big");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_ENDIAN ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_BIG ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_NONE ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest06(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, little");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_ENDIAN ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_LITTLE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_NONE ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest07(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, dce");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_ENDIAN ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_DCE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_NONE ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest08(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, string, hex");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest09(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, string, oct");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_OCT ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest10(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, string, dec");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_DEC ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest11(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, align 4");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_ALIGN ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_DEFAULT ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_NONE ||
        bed->align_value != 4 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest12(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, align 4, relative");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != (DETECT_BYTE_EXTRACT_FLAG_ALIGN |
                       DETECT_BYTE_EXTRACT_FLAG_RELATIVE) ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_DEFAULT ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_NONE ||
        bed->align_value != 4 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest13(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, align 4, relative, big");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != (DETECT_BYTE_EXTRACT_FLAG_ALIGN |
                       DETECT_BYTE_EXTRACT_FLAG_ENDIAN |
                       DETECT_BYTE_EXTRACT_FLAG_RELATIVE) ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_BIG ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_NONE ||
        bed->align_value != 4 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest14(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, align 4, relative, dce");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != (DETECT_BYTE_EXTRACT_FLAG_ALIGN |
                       DETECT_BYTE_EXTRACT_FLAG_ENDIAN |
                       DETECT_BYTE_EXTRACT_FLAG_RELATIVE) ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_DCE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_NONE ||
        bed->align_value != 4 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest15(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, align 4, relative, little");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != (DETECT_BYTE_EXTRACT_FLAG_ALIGN |
                       DETECT_BYTE_EXTRACT_FLAG_ENDIAN |
                       DETECT_BYTE_EXTRACT_FLAG_RELATIVE) ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_LITTLE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_NONE ||
        bed->align_value != 4 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest16(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, align 4, relative, little, multiplier 2");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != (DETECT_BYTE_EXTRACT_FLAG_ALIGN |
                       DETECT_BYTE_EXTRACT_FLAG_RELATIVE |
                       DETECT_BYTE_EXTRACT_FLAG_ENDIAN |
                       DETECT_BYTE_EXTRACT_FLAG_MULTIPLIER) ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_LITTLE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_NONE ||
        bed->align_value != 4 ||
        bed->multiplier_value != 2) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest17(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, align 4, "
                                                        "relative, little, "
                                                        "multiplier 2, string hex");
    if (bed != NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest18(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, align 4, "
                                                        "relative, little, "
                                                        "multiplier 2, "
                                                        "relative");
    if (bed != NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest19(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, align 4, "
                                                        "relative, little, "
                                                        "multiplier 2, "
                                                        "little");
    if (bed != NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest20(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, align 4, "
                                                        "relative, "
                                                        "multiplier 2, "
                                                        "align 2");
    if (bed != NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest21(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, align 4, "
                                                        "multiplier 2, "
                                                        "relative, "
                                                        "multiplier 2");
    if (bed != NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest22(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, align 4, "
                                                        "string hex, "
                                                        "relative, "
                                                        "string hex");
    if (bed != NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest23(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, align 4, "
                                                        "string hex, "
                                                        "relative, "
                                                        "string oct");
    if (bed != NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest24(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("24, 2, one, align 4, "
                                                        "string hex, "
                                                        "relative");
    if (bed != NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest25(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("9, 2, one, align 4, "
                                                        "little, "
                                                        "relative");
    if (bed != NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest26(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, align 4, "
                                                        "little, "
                                                        "relative, "
                                                        "multiplier 65536");
    if (bed != NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest27(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, 2, one, align 4, "
                                                        "little, "
                                                        "relative, "
                                                        "multiplier 0");
    if (bed != NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest28(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("23, 2, one, string, oct");
    if (bed == NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest29(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("24, 2, one, string, oct");
    if (bed != NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest30(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("20, 2, one, string, dec");
    if (bed == NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest31(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("21, 2, one, string, dec");
    if (bed != NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest32(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("14, 2, one, string, hex");
    if (bed == NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest33(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("15, 2, one, string, hex");
    if (bed != NULL)
        goto end;

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

int DetectByteExtractTest34(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,2,two,relative,string,hex; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        !(cd->flags & DETECT_CONTENT_RELATIVE_NEXT) ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strncmp(bed->name, "two", cd->content_len) != 0 ||
        bed->flags != (DETECT_BYTE_EXTRACT_FLAG_RELATIVE |
                       DETECT_BYTE_EXTRACT_FLAG_STRING) ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest35(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectPcreData *pd = NULL;
    DetectByteExtractData *bed = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; pcre:/asf/; "
                                   "byte_extract:4,0,two,relative,string,hex; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_PCRE) {
        result = 0;
        goto end;
    }
    pd = (DetectPcreData *)sm->ctx;
    if (pd->flags != DETECT_PCRE_RELATIVE_NEXT) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "two") != 0 ||
        bed->flags != (DETECT_BYTE_EXTRACT_FLAG_RELATIVE |
                       DETECT_BYTE_EXTRACT_FLAG_STRING) ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest36(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectBytejumpData *bjd = NULL;
    DetectByteExtractData *bed = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; byte_jump:1,13; "
                                   "byte_extract:4,0,two,relative,string,hex; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTEJUMP) {
        result = 0;
        goto end;
    }
    bjd = (DetectBytejumpData *)sm->ctx;
    if (bjd->flags != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "two") != 0 ||
        bed->flags != (DETECT_BYTE_EXTRACT_FLAG_RELATIVE |
                       DETECT_BYTE_EXTRACT_FLAG_STRING) ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest37(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectContentData *ud = NULL;
    DetectByteExtractData *bed = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; uricontent:\"two\"; "
                                   "byte_extract:4,0,two,relative,string,hex; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_UMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    ud = (DetectContentData *)sm->ctx;
    if (ud->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)ud->content, "two", cd->content_len) != 0 ||
        ud->flags & DETECT_CONTENT_NOCASE ||
        ud->flags & DETECT_CONTENT_WITHIN ||
        ud->flags & DETECT_CONTENT_DISTANCE ||
        ud->flags & DETECT_CONTENT_FAST_PATTERN ||
        !(ud->flags & DETECT_CONTENT_RELATIVE_NEXT) ||
        ud->flags & DETECT_CONTENT_NEGATED ) {
        printf("two failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "two") != 0 ||
        bed->flags != (DETECT_BYTE_EXTRACT_FLAG_RELATIVE |
                       DETECT_BYTE_EXTRACT_FLAG_STRING) ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest38(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectContentData *ud = NULL;
    DetectByteExtractData *bed = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; uricontent:\"two\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "two") != 0 ||
        bed->flags !=DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_UMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    ud = (DetectContentData *)sm->ctx;
    if (ud->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)ud->content, "two", cd->content_len) != 0 ||
        ud->flags & DETECT_CONTENT_NOCASE ||
        ud->flags & DETECT_CONTENT_WITHIN ||
        ud->flags & DETECT_CONTENT_DISTANCE ||
        ud->flags & DETECT_CONTENT_FAST_PATTERN ||
        ud->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        ud->flags & DETECT_CONTENT_NEGATED ) {
        printf("two failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL) {
        result = 0;
        goto end;
    }

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest39(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectContentData *ud = NULL;
    DetectByteExtractData *bed = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; content:\"two\"; http_uri; "
                                   "byte_extract:4,0,two,relative,string,hex; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_UMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    ud = (DetectContentData *)sm->ctx;
    if (ud->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)ud->content, "two", cd->content_len) != 0 ||
        ud->flags & DETECT_CONTENT_NOCASE ||
        ud->flags & DETECT_CONTENT_WITHIN ||
        ud->flags & DETECT_CONTENT_DISTANCE ||
        ud->flags & DETECT_CONTENT_FAST_PATTERN ||
        !(ud->flags & DETECT_CONTENT_RELATIVE_NEXT) ||
        ud->flags & DETECT_CONTENT_NEGATED ) {
        printf("two failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "two") != 0 ||
        bed->flags != (DETECT_BYTE_EXTRACT_FLAG_RELATIVE |
                       DETECT_BYTE_EXTRACT_FLAG_STRING) ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest40(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectContentData *ud = NULL;
    DetectByteExtractData *bed = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; content:\"two\"; http_uri; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "two") != 0 ||
        bed->flags !=DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_UMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    ud = (DetectContentData *)sm->ctx;
    if (ud->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)ud->content, "two", cd->content_len) != 0 ||
        ud->flags & DETECT_CONTENT_NOCASE ||
        ud->flags & DETECT_CONTENT_WITHIN ||
        ud->flags & DETECT_CONTENT_DISTANCE ||
        ud->flags & DETECT_CONTENT_FAST_PATTERN ||
        ud->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        ud->flags & DETECT_CONTENT_NEGATED ) {
        printf("two failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL) {
        result = 0;
        goto end;
    }

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest41(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "byte_extract:4,0,three,string,hex; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "two") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "three") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed->local_id != 1) {
        result = 0;
        goto end;
    }

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest42(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectContentData *ud = NULL;
    DetectByteExtractData *bed = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "uricontent: \"three\"; "
                                   "byte_extract:4,0,four,string,hex,relative; "
                                   "byte_extract:4,0,five,string,hex; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "two") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "five") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed->local_id != 1) {
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    sm = s->sm_lists[DETECT_SM_LIST_UMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    ud = (DetectContentData *)sm->ctx;
    if (ud->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)ud->content, "three", cd->content_len) != 0 ||
        ud->flags & DETECT_CONTENT_NOCASE ||
        ud->flags & DETECT_CONTENT_WITHIN ||
        ud->flags & DETECT_CONTENT_DISTANCE ||
        ud->flags & DETECT_CONTENT_FAST_PATTERN ||
        !(ud->flags & DETECT_CONTENT_RELATIVE_NEXT) ||
        ud->flags & DETECT_CONTENT_NEGATED ) {
        printf("two failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "four") != 0 ||
        bed->flags != (DETECT_BYTE_EXTRACT_FLAG_RELATIVE |
                       DETECT_BYTE_EXTRACT_FLAG_STRING) ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed->local_id != 0) {
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest43(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "content: \"three\"; offset:two; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "two") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (strncmp((char *)cd->content, "three", cd->content_len) != 0 ||
        cd->flags != (DETECT_CONTENT_OFFSET_BE |
                      DETECT_CONTENT_OFFSET) ||
        cd->offset != bed->local_id) {
        printf("three failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest44(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed1 = NULL;
    DetectByteExtractData *bed2 = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "byte_extract:4,0,three,string,hex; "
                                   "content: \"four\"; offset:two; "
                                   "content: \"five\"; offset:three; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed1 = (DetectByteExtractData *)sm->ctx;
    if (bed1->nbytes != 4 ||
        bed1->offset != 0 ||
        strcmp(bed1->name, "two") != 0 ||
        bed1->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed1->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed1->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed1->align_value != 0 ||
        bed1->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed1->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed2 = (DetectByteExtractData *)sm->ctx;

    sm = sm->next;
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (strncmp((char *)cd->content, "four", cd->content_len) != 0 ||
        cd->flags != (DETECT_CONTENT_OFFSET_BE |
                      DETECT_CONTENT_OFFSET) ||
        cd->offset != bed1->local_id) {
        printf("four failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (strncmp((char *)cd->content, "five", cd->content_len) != 0 ||
        cd->flags != (DETECT_CONTENT_OFFSET_BE |
                      DETECT_CONTENT_OFFSET) ||
        cd->offset != bed2->local_id) {
        printf("five failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest45(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "content: \"three\"; depth:two; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "two") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (strncmp((char *)cd->content, "three", cd->content_len) != 0 ||
        cd->flags != (DETECT_CONTENT_DEPTH_BE |
                      DETECT_CONTENT_DEPTH) ||
        cd->depth != bed->local_id ||
        cd->offset != 0) {
        printf("three failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest46(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed1 = NULL;
    DetectByteExtractData *bed2 = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "byte_extract:4,0,three,string,hex; "
                                   "content: \"four\"; depth:two; "
                                   "content: \"five\"; depth:three; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed1 = (DetectByteExtractData *)sm->ctx;
    if (bed1->nbytes != 4 ||
        bed1->offset != 0 ||
        strcmp(bed1->name, "two") != 0 ||
        bed1->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed1->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed1->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed1->align_value != 0 ||
        bed1->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed1->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed2 = (DetectByteExtractData *)sm->ctx;

    sm = sm->next;
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (strncmp((char *)cd->content, "four", cd->content_len) != 0 ||
        cd->flags != (DETECT_CONTENT_DEPTH_BE |
                      DETECT_CONTENT_DEPTH) ||
        cd->depth != bed1->local_id) {
        printf("four failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (strncmp((char *)cd->content, "five", cd->content_len) != 0 ||
        cd->flags != (DETECT_CONTENT_DEPTH_BE |
                      DETECT_CONTENT_DEPTH) ||
        cd->depth != bed2->local_id) {
        printf("five failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest47(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "content: \"three\"; distance:two; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        !(cd->flags & DETECT_CONTENT_RELATIVE_NEXT) ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "two") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (strncmp((char *)cd->content, "three", cd->content_len) != 0 ||
        cd->flags != (DETECT_CONTENT_DISTANCE_BE |
                      DETECT_CONTENT_DISTANCE) ||
        cd->distance != bed->local_id ||
        cd->offset != 0 ||
        cd->depth != 0) {
        printf("three failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest48(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed1 = NULL;
    DetectByteExtractData *bed2 = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "byte_extract:4,0,three,string,hex; "
                                   "content: \"four\"; distance:two; "
                                   "content: \"five\"; distance:three; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        !(cd->flags & DETECT_CONTENT_RELATIVE_NEXT) ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed1 = (DetectByteExtractData *)sm->ctx;
    if (bed1->nbytes != 4 ||
        bed1->offset != 0 ||
        strcmp(bed1->name, "two") != 0 ||
        bed1->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed1->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed1->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed1->align_value != 0 ||
        bed1->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed1->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed2 = (DetectByteExtractData *)sm->ctx;

    sm = sm->next;
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (strncmp((char *)cd->content, "four", cd->content_len) != 0 ||
        cd->flags != (DETECT_CONTENT_DISTANCE_BE |
                      DETECT_CONTENT_DISTANCE |
                      DETECT_CONTENT_RELATIVE_NEXT) ||
        cd->distance != bed1->local_id ||
        cd->depth != 0 ||
        cd->offset != 0) {
        printf("four failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (strncmp((char *)cd->content, "five", cd->content_len) != 0 ||
        cd->flags != (DETECT_CONTENT_DISTANCE_BE |
                      DETECT_CONTENT_DISTANCE) ||
        cd->distance != bed2->local_id ||
        cd->depth != 0 ||
        cd->offset != 0) {
        printf("five failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest49(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "content: \"three\"; within:two; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        !(cd->flags & DETECT_CONTENT_RELATIVE_NEXT) ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "two") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (strncmp((char *)cd->content, "three", cd->content_len) != 0 ||
        cd->flags != (DETECT_CONTENT_WITHIN_BE |
                      DETECT_CONTENT_WITHIN) ||
        cd->within != bed->local_id ||
        cd->offset != 0 ||
        cd->depth != 0 ||
        cd->distance != 0) {
        printf("three failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest50(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed1 = NULL;
    DetectByteExtractData *bed2 = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "byte_extract:4,0,three,string,hex; "
                                   "content: \"four\"; within:two; "
                                   "content: \"five\"; within:three; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        !(cd->flags & DETECT_CONTENT_RELATIVE_NEXT) ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed1 = (DetectByteExtractData *)sm->ctx;
    if (bed1->nbytes != 4 ||
        bed1->offset != 0 ||
        strcmp(bed1->name, "two") != 0 ||
        bed1->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed1->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed1->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed1->align_value != 0 ||
        bed1->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed1->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed2 = (DetectByteExtractData *)sm->ctx;

    sm = sm->next;
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (strncmp((char *)cd->content, "four", cd->content_len) != 0 ||
        cd->flags != (DETECT_CONTENT_WITHIN_BE |
                      DETECT_CONTENT_WITHIN|
                      DETECT_CONTENT_RELATIVE_NEXT) ||
        cd->within != bed1->local_id ||
        cd->depth != 0 ||
        cd->offset != 0 ||
        cd->distance != 0) {
        printf("four failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (strncmp((char *)cd->content, "five", cd->content_len) != 0 ||
        cd->flags != (DETECT_CONTENT_WITHIN_BE |
                      DETECT_CONTENT_WITHIN) ||
        cd->within != bed2->local_id ||
        cd->depth != 0 ||
        cd->offset != 0 ||
        cd->distance != 0) {
        printf("five failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest51(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed = NULL;
    DetectBytetestData *btd = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "byte_test: 2,=,10, two; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "two") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTETEST) {
        result = 0;
        goto end;
    }
    btd = (DetectBytetestData *)sm->ctx;
    if (btd->flags != DETECT_BYTETEST_OFFSET_BE ||
        btd->value != 10 ||
        btd->offset != 0) {
        printf("three failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest52(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed1 = NULL;
    DetectBytetestData *btd = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "byte_extract:4,0,three,string,hex; "
                                   "byte_test: 2,=,two,three; "
                                   "byte_test: 3,=,10,three; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed1 = (DetectByteExtractData *)sm->ctx;
    if (bed1->nbytes != 4 ||
        bed1->offset != 0 ||
        strcmp(bed1->name, "two") != 0 ||
        bed1->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed1->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed1->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed1->align_value != 0 ||
        bed1->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed1->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTETEST) {
        result = 0;
        goto end;
    }
    btd = (DetectBytetestData *)sm->ctx;
    if (btd->flags != (DETECT_BYTETEST_OFFSET_BE |
                       DETECT_BYTETEST_VALUE_BE) ||
        btd->value != 0 ||
        btd->offset != 1) {
        printf("three failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTETEST) {
        result = 0;
        goto end;
    }
    btd = (DetectBytetestData *)sm->ctx;
    if (btd->flags != DETECT_BYTETEST_OFFSET_BE ||
        btd->value != 10 ||
        btd->offset != 1) {
        printf("four failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest53(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed = NULL;
    DetectBytejumpData *bjd = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "byte_jump: 2,two; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 0 ||
        strcmp(bed->name, "two") != 0 ||
        bed->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTEJUMP) {
        result = 0;
        goto end;
    }
    bjd = (DetectBytejumpData *)sm->ctx;
    if (bjd->flags != DETECT_BYTEJUMP_OFFSET_BE ||
        bjd->offset != 0) {
        printf("three failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest54(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed1 = NULL;
    DetectBytejumpData *bjd = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "byte_extract:4,0,three,string,hex; "
                                   "byte_jump: 2,two; "
                                   "byte_jump: 3,three; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed1 = (DetectByteExtractData *)sm->ctx;
    if (bed1->nbytes != 4 ||
        bed1->offset != 0 ||
        strcmp(bed1->name, "two") != 0 ||
        bed1->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed1->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed1->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed1->align_value != 0 ||
        bed1->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed1->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTEJUMP) {
        result = 0;
        goto end;
    }
    bjd = (DetectBytejumpData *)sm->ctx;
    if (bjd->flags != DETECT_BYTEJUMP_OFFSET_BE ||
        bjd->offset != 0) {
        printf("three failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTEJUMP) {
        result = 0;
        goto end;
    }
    bjd = (DetectBytejumpData *)sm->ctx;
    if (bjd->flags != DETECT_BYTEJUMP_OFFSET_BE ||
        bjd->offset != 1) {
        printf("four failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest55(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed1 = NULL;
    DetectByteExtractData *bed2 = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing byte_extract\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "byte_extract:4,0,three,string,hex; "
                                   "byte_extract:4,0,four,string,hex; "
                                   "byte_extract:4,0,five,string,hex; "
                                   "content: \"four\"; within:two; distance:three; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        !(cd->flags & DETECT_CONTENT_RELATIVE_NEXT) ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed: ");
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        goto end;
    }
    bed1 = (DetectByteExtractData *)sm->ctx;
    if (bed1->nbytes != 4 ||
        bed1->offset != 0 ||
        strcmp(bed1->name, "two") != 0 ||
        bed1->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed1->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed1->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed1->align_value != 0 ||
        bed1->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed1->local_id != 0) {
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        goto end;
    }
    bed2 = (DetectByteExtractData *)sm->ctx;

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_CONTENT) {
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (strncmp((char *)cd->content, "four", cd->content_len) != 0 ||
        cd->flags != (DETECT_CONTENT_DISTANCE_BE |
                      DETECT_CONTENT_WITHIN_BE |
                      DETECT_CONTENT_DISTANCE |
                      DETECT_CONTENT_WITHIN) ||
        cd->within != bed1->local_id ||
        cd->distance != bed2->local_id) {
        printf("four failed: ");
        goto end;
    }

    if (sm->next != NULL) {
        goto end;
    }

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest56(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed1 = NULL;
    DetectByteExtractData *bed2 = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "uricontent:\"urione\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "byte_extract:4,0,three,string,hex; "
                                   "byte_extract:4,0,four,string,hex; "
                                   "byte_extract:4,0,five,string,hex; "
                                   "content: \"four\"; within:two; distance:three; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_UMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "urione", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        !(cd->flags & DETECT_CONTENT_RELATIVE_NEXT) ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed1 = (DetectByteExtractData *)sm->ctx;
    if (bed1->nbytes != 4 ||
        bed1->offset != 0 ||
        strcmp(bed1->name, "two") != 0 ||
        bed1->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed1->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed1->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed1->align_value != 0 ||
        bed1->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed1->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed2 = (DetectByteExtractData *)sm->ctx;

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (strncmp((char *)cd->content, "four", cd->content_len) != 0 ||
        cd->flags != (DETECT_CONTENT_DISTANCE_BE |
                      DETECT_CONTENT_WITHIN_BE |
                      DETECT_CONTENT_DISTANCE |
                      DETECT_CONTENT_WITHIN) ||
        cd->within != bed1->local_id ||
        cd->distance != bed2->local_id ) {
        printf("four failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL) {
        goto end;
    }

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest57(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed1 = NULL;
    DetectByteExtractData *bed2 = NULL;
    DetectByteExtractData *bed3 = NULL;
    DetectByteExtractData *bed4 = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "uricontent: \"urione\"; "
                                   "byte_extract:4,0,two,string,hex,relative; "
                                   "byte_extract:4,0,three,string,hex,relative; "
                                   "byte_extract:4,0,four,string,hex,relative; "
                                   "byte_extract:4,0,five,string,hex,relative; "
                                   "uricontent: \"four\"; within:two; distance:three; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    sm = s->sm_lists[DETECT_SM_LIST_UMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "urione", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        !(cd->flags & DETECT_CONTENT_RELATIVE_NEXT) ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed1 = (DetectByteExtractData *)sm->ctx;
    if (bed1->nbytes != 4 ||
        bed1->offset != 0 ||
        strcmp(bed1->name, "two") != 0 ||
        bed1->flags != (DETECT_BYTE_EXTRACT_FLAG_STRING |
                        DETECT_BYTE_EXTRACT_FLAG_RELATIVE) ||
        bed1->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed1->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed1->align_value != 0 ||
        bed1->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed1->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed2 = (DetectByteExtractData *)sm->ctx;
    if (bed2->local_id != 1) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed3 = (DetectByteExtractData *)sm->ctx;
    if (bed3->local_id != 2) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed4 = (DetectByteExtractData *)sm->ctx;
    if (bed4->local_id != 3) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (strncmp((char *)cd->content, "four", cd->content_len) != 0 ||
        cd->flags != (DETECT_CONTENT_DISTANCE_BE |
                      DETECT_CONTENT_WITHIN_BE |
                      DETECT_CONTENT_DISTANCE |
                      DETECT_CONTENT_WITHIN) ||
        cd->within != bed1->local_id ||
        cd->distance != bed2->local_id)  {
        printf("four failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL) {
        goto end;
    }

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest58(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed1 = NULL;
    DetectBytejumpData *bjd = NULL;
    DetectIsdataatData *isdd = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "byte_extract:4,0,three,string,hex; "
                                   "byte_jump: 2,two; "
                                   "byte_jump: 3,three; "
                                   "isdataat: three; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed1 = (DetectByteExtractData *)sm->ctx;
    if (bed1->nbytes != 4 ||
        bed1->offset != 0 ||
        strcmp(bed1->name, "two") != 0 ||
        bed1->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed1->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed1->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed1->align_value != 0 ||
        bed1->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed1->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTEJUMP) {
        result = 0;
        goto end;
    }
    bjd = (DetectBytejumpData *)sm->ctx;
    if (bjd->flags != DETECT_BYTEJUMP_OFFSET_BE ||
        bjd->offset != 0) {
        printf("three failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTEJUMP) {
        result = 0;
        goto end;
    }
    bjd = (DetectBytejumpData *)sm->ctx;
    if (bjd->flags != DETECT_BYTEJUMP_OFFSET_BE ||
        bjd->offset != 1) {
        printf("four failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_ISDATAAT) {
        result = 0;
        goto end;
    }
    isdd = (DetectIsdataatData *)sm->ctx;
    if (isdd->flags != ISDATAAT_OFFSET_BE ||
        isdd->dataat != 1) {
        printf("isdataat failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest59(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed1 = NULL;
    DetectBytejumpData *bjd = NULL;
    DetectIsdataatData *isdd = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex; "
                                   "byte_extract:4,0,three,string,hex; "
                                   "byte_jump: 2,two; "
                                   "byte_jump: 3,three; "
                                   "isdataat: three,relative; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        cd->flags & DETECT_CONTENT_RELATIVE_NEXT ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed1 = (DetectByteExtractData *)sm->ctx;
    if (bed1->nbytes != 4 ||
        bed1->offset != 0 ||
        strcmp(bed1->name, "two") != 0 ||
        bed1->flags != DETECT_BYTE_EXTRACT_FLAG_STRING ||
        bed1->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed1->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed1->align_value != 0 ||
        bed1->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed1->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTEJUMP) {
        result = 0;
        goto end;
    }
    bjd = (DetectBytejumpData *)sm->ctx;
    if (bjd->flags != DETECT_BYTEJUMP_OFFSET_BE ||
        bjd->offset != 0) {
        printf("three failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTEJUMP) {
        result = 0;
        goto end;
    }
    bjd = (DetectBytejumpData *)sm->ctx;
    if (bjd->flags != DETECT_BYTEJUMP_OFFSET_BE ||
        bjd->offset != 1) {
        printf("four failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_ISDATAAT) {
        result = 0;
        goto end;
    }
    isdd = (DetectIsdataatData *)sm->ctx;
    if (isdd->flags != (ISDATAAT_OFFSET_BE |
                        ISDATAAT_RELATIVE) ||
        isdd->dataat != 1) {
        printf("isdataat failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest60(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed1 = NULL;
    DetectIsdataatData *isdd = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex,relative; "
                                   "uricontent: \"three\"; "
                                   "byte_extract:4,0,four,string,hex,relative; "
                                   "isdataat: two; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        !(cd->flags & DETECT_CONTENT_RELATIVE_NEXT) ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed1 = (DetectByteExtractData *)sm->ctx;
    if (bed1->nbytes != 4 ||
        bed1->offset != 0 ||
        strcmp(bed1->name, "two") != 0 ||
        bed1->flags != (DETECT_BYTE_EXTRACT_FLAG_STRING |
                        DETECT_BYTE_EXTRACT_FLAG_RELATIVE) ||
        bed1->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed1->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed1->align_value != 0 ||
        bed1->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed1->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_ISDATAAT) {
        result = 0;
        goto end;
    }
    isdd = (DetectIsdataatData *)sm->ctx;
    if (isdd->flags != (ISDATAAT_OFFSET_BE) ||
        isdd->dataat != bed1->local_id) {
        printf("isdataat failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    if (s->sm_lists_tail[DETECT_SM_LIST_UMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_UMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags != DETECT_CONTENT_RELATIVE_NEXT ||
        strncmp((char *)cd->content, "three", cd->content_len) != 0) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed1 = (DetectByteExtractData *)sm->ctx;
    if (bed1->nbytes != 4 ||
        bed1->offset != 0 ||
        strcmp(bed1->name, "four") != 0 ||
        bed1->flags != (DETECT_BYTE_EXTRACT_FLAG_STRING |
                        DETECT_BYTE_EXTRACT_FLAG_RELATIVE) ||
        bed1->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed1->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed1->align_value != 0 ||
        bed1->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed1->local_id != 0) {
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest61(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectContentData *cd = NULL;
    DetectByteExtractData *bed1 = NULL;
    DetectIsdataatData *isdd = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(msg:\"Testing bytejump_body\"; "
                                   "content:\"one\"; "
                                   "byte_extract:4,0,two,string,hex,relative; "
                                   "uricontent: \"three\"; "
                                   "byte_extract:4,0,four,string,hex,relative; "
                                   "isdataat: four, relative; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_PMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_PMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES ||
        strncmp((char *)cd->content, "one", cd->content_len) != 0 ||
        cd->flags & DETECT_CONTENT_NOCASE ||
        cd->flags & DETECT_CONTENT_WITHIN ||
        cd->flags & DETECT_CONTENT_DISTANCE ||
        cd->flags & DETECT_CONTENT_FAST_PATTERN ||
        !(cd->flags & DETECT_CONTENT_RELATIVE_NEXT) ||
        cd->flags & DETECT_CONTENT_NEGATED ) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed1 = (DetectByteExtractData *)sm->ctx;
    if (bed1->nbytes != 4 ||
        bed1->offset != 0 ||
        strcmp(bed1->name, "two") != 0 ||
        bed1->flags != (DETECT_BYTE_EXTRACT_FLAG_STRING |
                        DETECT_BYTE_EXTRACT_FLAG_RELATIVE) ||
        bed1->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed1->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed1->align_value != 0 ||
        bed1->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed1->local_id != 0) {
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    if (s->sm_lists_tail[DETECT_SM_LIST_UMATCH] == NULL) {
        result = 0;
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_UMATCH];
    if (sm->type != DETECT_CONTENT) {
        result = 0;
        goto end;
    }
    cd = (DetectContentData *)sm->ctx;
    if (cd->flags != DETECT_CONTENT_RELATIVE_NEXT ||
        strncmp((char *)cd->content, "three", cd->content_len) != 0) {
        printf("one failed\n");
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed1 = (DetectByteExtractData *)sm->ctx;
    if (bed1->nbytes != 4 ||
        bed1->offset != 0 ||
        strcmp(bed1->name, "four") != 0 ||
        bed1->flags != (DETECT_BYTE_EXTRACT_FLAG_STRING |
                        DETECT_BYTE_EXTRACT_FLAG_RELATIVE) ||
        bed1->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed1->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed1->align_value != 0 ||
        bed1->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }
    if (bed1->local_id != 0) {
        result = 0;
        goto end;
    }

    sm = sm->next;
    if (sm->type != DETECT_ISDATAAT) {
        result = 0;
        goto end;
    }
    isdd = (DetectIsdataatData *)sm->ctx;
    if (isdd->flags != (ISDATAAT_OFFSET_BE |
                        ISDATAAT_RELATIVE) ||
        isdd->dataat != bed1->local_id) {
        printf("isdataat failed\n");
        result = 0;
        goto end;
    }

    if (sm->next != NULL)
        goto end;

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

static int DetectByteExtractTest62(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Signature *s = NULL;
    SigMatch *sm = NULL;
    DetectByteExtractData *bed = NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                                   "(file_data; byte_extract:4,2,two,relative,string,hex; "
                                   "sid:1;)");
    if (de_ctx->sig_list == NULL) {
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_HSBDMATCH] == NULL) {
        goto end;
    }

    sm = s->sm_lists[DETECT_SM_LIST_HSBDMATCH];
    if (sm->type != DETECT_BYTE_EXTRACT) {
        result = 0;
        goto end;
    }
    bed = (DetectByteExtractData *)sm->ctx;
    if (bed->nbytes != 4 ||
        bed->offset != 2 ||
        strncmp(bed->name, "two", 3) != 0 ||
        bed->flags != (DETECT_BYTE_EXTRACT_FLAG_STRING | DETECT_BYTE_EXTRACT_FLAG_RELATIVE) ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_NONE ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_HEX ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectByteExtractTest63(void)
{
    int result = 0;

    DetectByteExtractData *bed = DetectByteExtractParse("4, -2, one");
    if (bed == NULL)
        goto end;

    if (bed->nbytes != 4 ||
        bed->offset != -2 ||
        strcmp(bed->name, "one") != 0 ||
        bed->flags != 0 ||
        bed->endian != DETECT_BYTE_EXTRACT_ENDIAN_DEFAULT ||
        bed->base != DETECT_BYTE_EXTRACT_BASE_NONE ||
        bed->align_value != 0 ||
        bed->multiplier_value != DETECT_BYTE_EXTRACT_MULTIPLIER_DEFAULT) {
        goto end;
    }

    result = 1;
 end:
    if (bed != NULL)
        DetectByteExtractFree(bed);
    return result;
}

#endif /* UNITTESTS */

void DetectByteExtractRegisterTests(void)
{
#ifdef UNITTESTS
    UtRegisterTest("DetectByteExtractTest01", DetectByteExtractTest01, 1);
    UtRegisterTest("DetectByteExtractTest02", DetectByteExtractTest02, 1);
    UtRegisterTest("DetectByteExtractTest03", DetectByteExtractTest03, 1);
    UtRegisterTest("DetectByteExtractTest04", DetectByteExtractTest04, 1);
    UtRegisterTest("DetectByteExtractTest05", DetectByteExtractTest05, 1);
    UtRegisterTest("DetectByteExtractTest06", DetectByteExtractTest06, 1);
    UtRegisterTest("DetectByteExtractTest07", DetectByteExtractTest07, 1);
    UtRegisterTest("DetectByteExtractTest08", DetectByteExtractTest08, 1);
    UtRegisterTest("DetectByteExtractTest09", DetectByteExtractTest09, 1);
    UtRegisterTest("DetectByteExtractTest10", DetectByteExtractTest10, 1);
    UtRegisterTest("DetectByteExtractTest11", DetectByteExtractTest11, 1);
    UtRegisterTest("DetectByteExtractTest12", DetectByteExtractTest12, 1);
    UtRegisterTest("DetectByteExtractTest13", DetectByteExtractTest13, 1);
    UtRegisterTest("DetectByteExtractTest14", DetectByteExtractTest14, 1);
    UtRegisterTest("DetectByteExtractTest15", DetectByteExtractTest15, 1);
    UtRegisterTest("DetectByteExtractTest16", DetectByteExtractTest16, 1);
    UtRegisterTest("DetectByteExtractTest17", DetectByteExtractTest17, 1);
    UtRegisterTest("DetectByteExtractTest18", DetectByteExtractTest18, 1);
    UtRegisterTest("DetectByteExtractTest19", DetectByteExtractTest19, 1);
    UtRegisterTest("DetectByteExtractTest20", DetectByteExtractTest20, 1);
    UtRegisterTest("DetectByteExtractTest21", DetectByteExtractTest21, 1);
    UtRegisterTest("DetectByteExtractTest22", DetectByteExtractTest22, 1);
    UtRegisterTest("DetectByteExtractTest23", DetectByteExtractTest23, 1);
    UtRegisterTest("DetectByteExtractTest24", DetectByteExtractTest24, 1);
    UtRegisterTest("DetectByteExtractTest25", DetectByteExtractTest25, 1);
    UtRegisterTest("DetectByteExtractTest26", DetectByteExtractTest26, 1);
    UtRegisterTest("DetectByteExtractTest27", DetectByteExtractTest27, 1);
    UtRegisterTest("DetectByteExtractTest28", DetectByteExtractTest28, 1);
    UtRegisterTest("DetectByteExtractTest29", DetectByteExtractTest29, 1);
    UtRegisterTest("DetectByteExtractTest30", DetectByteExtractTest30, 1);
    UtRegisterTest("DetectByteExtractTest31", DetectByteExtractTest31, 1);
    UtRegisterTest("DetectByteExtractTest32", DetectByteExtractTest32, 1);
    UtRegisterTest("DetectByteExtractTest33", DetectByteExtractTest33, 1);
    UtRegisterTest("DetectByteExtractTest34", DetectByteExtractTest34, 1);
    UtRegisterTest("DetectByteExtractTest35", DetectByteExtractTest35, 1);
    UtRegisterTest("DetectByteExtractTest36", DetectByteExtractTest36, 1);
    UtRegisterTest("DetectByteExtractTest37", DetectByteExtractTest37, 1);
    UtRegisterTest("DetectByteExtractTest38", DetectByteExtractTest38, 1);
    UtRegisterTest("DetectByteExtractTest39", DetectByteExtractTest39, 1);
    UtRegisterTest("DetectByteExtractTest40", DetectByteExtractTest40, 1);
    UtRegisterTest("DetectByteExtractTest41", DetectByteExtractTest41, 1);
    UtRegisterTest("DetectByteExtractTest42", DetectByteExtractTest42, 1);

    UtRegisterTest("DetectByteExtractTest43", DetectByteExtractTest43, 1);
    UtRegisterTest("DetectByteExtractTest44", DetectByteExtractTest44, 1);

    UtRegisterTest("DetectByteExtractTest45", DetectByteExtractTest45, 1);
    UtRegisterTest("DetectByteExtractTest46", DetectByteExtractTest46, 1);

    UtRegisterTest("DetectByteExtractTest47", DetectByteExtractTest47, 1);
    UtRegisterTest("DetectByteExtractTest48", DetectByteExtractTest48, 1);

    UtRegisterTest("DetectByteExtractTest49", DetectByteExtractTest49, 1);
    UtRegisterTest("DetectByteExtractTest50", DetectByteExtractTest50, 1);

    UtRegisterTest("DetectByteExtractTest51", DetectByteExtractTest51, 1);
    UtRegisterTest("DetectByteExtractTest52", DetectByteExtractTest52, 1);

    UtRegisterTest("DetectByteExtractTest53", DetectByteExtractTest53, 1);
    UtRegisterTest("DetectByteExtractTest54", DetectByteExtractTest54, 1);

    UtRegisterTest("DetectByteExtractTest55", DetectByteExtractTest55, 1);
    UtRegisterTest("DetectByteExtractTest56", DetectByteExtractTest56, 1);
    UtRegisterTest("DetectByteExtractTest57", DetectByteExtractTest57, 1);

    UtRegisterTest("DetectByteExtractTest58", DetectByteExtractTest58, 1);
    UtRegisterTest("DetectByteExtractTest59", DetectByteExtractTest59, 1);
    UtRegisterTest("DetectByteExtractTest60", DetectByteExtractTest60, 1);
    UtRegisterTest("DetectByteExtractTest61", DetectByteExtractTest61, 1);
    UtRegisterTest("DetectByteExtractTest62", DetectByteExtractTest62, 1);
    UtRegisterTest("DetectByteExtractTest63", DetectByteExtractTest63, 1);
#endif /* UNITTESTS */

    return;
}
