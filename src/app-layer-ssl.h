/* Copyright (C) 2007-2012 Open Information Security Foundation
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
 * \author Pierre Chifflier <pierre.chifflier@ssi.gouv.fr>
 *
 */

#ifndef __APP_LAYER_SSL_H__
#define __APP_LAYER_SSL_H__

#include "decode-events.h"
#include "queue.h"

enum {
    /* TLS protocol messages */
    TLS_DECODER_EVENT_INVALID_SSLV2_HEADER,
    TLS_DECODER_EVENT_INVALID_TLS_HEADER,
    TLS_DECODER_EVENT_INVALID_RECORD_TYPE,
    TLS_DECODER_EVENT_INVALID_HANDSHAKE_MESSAGE,
    /* Certificates decoding messages */
    TLS_DECODER_EVENT_INVALID_CERTIFICATE,
    TLS_DECODER_EVENT_CERTIFICATE_MISSING_ELEMENT,
    TLS_DECODER_EVENT_CERTIFICATE_UNKNOWN_ELEMENT,
    TLS_DECODER_EVENT_CERTIFICATE_INVALID_LENGTH,
    TLS_DECODER_EVENT_CERTIFICATE_INVALID_STRING,
    TLS_DECODER_EVENT_ERROR_MSG_ENCOUNTERED,
};

/* Flag to indicate that server will now on send encrypted msgs */
#define SSL_AL_FLAG_SERVER_CHANGE_CIPHER_SPEC   0x0001
/* Flag to indicate that client will now on send encrypted msgs */
#define SSL_AL_FLAG_CLIENT_CHANGE_CIPHER_SPEC   0x0002
#define SSL_AL_FLAG_CHANGE_CIPHER_SPEC          0x0004

/* SSL related flags */
#define SSL_AL_FLAG_SSL_CLIENT_HS               0x0008
#define SSL_AL_FLAG_SSL_SERVER_HS               0x0010
#define SSL_AL_FLAG_SSL_CLIENT_MASTER_KEY       0x0020
#define SSL_AL_FLAG_SSL_CLIENT_SSN_ENCRYPTED    0x0040
#define SSL_AL_FLAG_SSL_SERVER_SSN_ENCRYPTED    0x0080
#define SSL_AL_FLAG_SSL_NO_SESSION_ID           0x0100

/* flags specific to detect-ssl-state keyword */
#define SSL_AL_FLAG_STATE_CLIENT_HELLO          0x0200
#define SSL_AL_FLAG_STATE_SERVER_HELLO          0x0400
#define SSL_AL_FLAG_STATE_CLIENT_KEYX           0x0800
#define SSL_AL_FLAG_STATE_SERVER_KEYX           0x1000
#define SSL_AL_FLAG_STATE_UNKNOWN               0x2000

#define SSL_TLS_LOG_PEM                         (1 << 0)



/* SSL versions.  We'll use a unified format for all, with the top byte
 * holding the major version and the lower byte the minor version */
enum {
    TLS_VERSION_UNKNOWN = 0x0000,
    SSL_VERSION_2 = 0x0200,
    SSL_VERSION_3 = 0x0300,
    TLS_VERSION_10 = 0x0301,
    TLS_VERSION_11 = 0x0302,
    TLS_VERSION_12 = 0x0303,
};

typedef struct SSLCertsChain_ {
    uint8_t *cert_data;
    uint32_t cert_len;
    TAILQ_ENTRY(SSLCertsChain_) next;
} SSLCertsChain;


typedef struct SSLStateConnp_ {
    /* record length */
    uint32_t record_length;
    /* record length's length for SSLv2 */
    uint32_t record_lengths_length;

    /* offset of the beginning of the current message (including header) */
    uint32_t message_start;
    uint32_t message_length;

    uint16_t version;
    uint8_t content_type;

    uint8_t handshake_type;
    uint32_t handshake_length;

    /* the no of bytes processed in the currently parsed record */
    uint16_t bytes_processed;
    /* the no of bytes processed in the currently parsed handshake */
    uint16_t hs_bytes_processed;

    /* sslv2 client hello session id length */
    uint16_t session_id_length;

    char *cert0_subject;
    char *cert0_issuerdn;
    char *cert0_fingerprint;

    uint8_t *cert_input;
    uint32_t cert_input_len;

    TAILQ_HEAD(, SSLCertsChain_) certs;

    uint32_t cert_log_flag;

    /* buffer for the tls record.
     * We use a malloced buffer, if the record is fragmented */
    uint8_t *trec;
    uint32_t trec_len;
    uint32_t trec_pos;
} SSLStateConnp;

/**
 * \brief SSLv[2.0|3.[0|1|2|3]] state structure.
 *
 *        Structure to store the SSL state values.
 */
typedef struct SSLState_ {
    Flow *f;

    /* holds some state flags we need */
    uint32_t flags;

    SSLStateConnp *curr_connp;

    SSLStateConnp client_connp;
    SSLStateConnp server_connp;
} SSLState;

void RegisterSSLParsers(void);
void SSLParserRegisterTests(void);

#endif /* __APP_LAYER_SSL_H__ */
