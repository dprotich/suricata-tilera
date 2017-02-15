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
 * \author Pablo Rincon <pablo.rincon.crespo@gmail.com>
 */

#ifndef __APP_LAYER_SSH_H__
#define __APP_LAYER_SSH_H__

#define SSH_FLAG_SERVER_CHANGE_CIPHER_SPEC   0x01    /**< Flag to indicate that
                                                     server will now on sends
                                                     encrypted msgs. */
#define SSH_FLAG_CLIENT_CHANGE_CIPHER_SPEC   0x02    /**< Flag to indicate that
                                                     client will now on sends
                                                     encrypted msgs. */

#define SSH_FLAG_CLIENT_VERSION_PARSED       0x01
#define SSH_FLAG_SERVER_VERSION_PARSED       0x02

/* This flags indicate that the rest of the communication
 * must be ciphered, so the parsing finish here */
#define SSH_FLAG_PARSER_DONE                 0x04

/* MSG_CODE */
#define SSH_MSG_NEWKEYS             21

enum {
    SSH_FIELD_NONE = 0,
    SSH_FIELD_SERVER_VER_STATE_LINE,
    SSH_FIELD_CLIENT_VER_STATE_LINE,
    SSH_FIELD_SERVER_PKT_LENGTH,
    SSH_FIELD_CLIENT_PKT_LENGTH,
    SSH_FIELD_SERVER_PADDING_LENGTH,
    SSH_FIELD_CLIENT_PADDING_LENGTH,
    SSH_FIELD_SERVER_PAYLOAD,
    SSH_FIELD_CLIENT_PAYLOAD,

    /* must be last */
    SSH_FIELD_MAX,
};

/** From SSH-TRANSP rfc

    SSH Bunary packet structure:
      uint32    packet_length
      byte      padding_length
      byte[n1]  payload; n1 = packet_length - padding_length - 1
      byte[n2]  random padding; n2 = padding_length
      byte[m]   mac (Message Authentication Code - MAC); m = mac_length

    So we are going to do a header struct to store
    the lenghts and msg_code (inside payload, if any)
*/

typedef struct SSHHeader_ {
    uint32_t pkt_len;
    uint8_t padding_len;
    uint8_t msg_code;
} SshHeader;

/** structure to store the SSH state values */
typedef struct SshState_ {
    uint8_t flags;                  /**< Flags to indicate the current SSH
                                         sessoin state */
    uint8_t client_msg_code;    /**< Client content type storage field */
    uint8_t server_msg_code;    /**< Server content type storage field */

    uint8_t *client_proto_version;        /**< Client SSH version storage field */
    uint8_t *client_software_version;        /**< Client SSH version storage field */

    uint8_t *server_proto_version;        /**< Server SSH version storage field */
    uint8_t *server_software_version;        /**< Server SSH version storage field */

    SshHeader srv_hdr;
    SshHeader cli_hdr;
} SshState;

void RegisterSSHParsers(void);
void SSHParserRegisterTests(void);

#endif /* __APP_LAYER_SSH_H__ */

