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
 * \author Gurvinder Singh <gurvindersinghdahiya@gmail.com>
 *
 * File to provide the protocol names based on protocol numbers defined in the
 * specified protocol file.
 */

#include "suricata-common.h"
#include "util-proto-name.h"

/**
 *  \brief  Function to load the protocol names from the specified protocol
 *          file.
 */
void SCProtoNameInit()
{
    /* Load the known protocols name from the /etc/protocols file */
    FILE *fp = fopen(PROTO_FILE,"r");
    if (fp != NULL) {
        char line[200];
#if !defined(__WIN32) && !defined(_WIN32)
        char *ptr = NULL;
#endif /* __WIN32 */

        while(fgets(line, sizeof(line), fp) != NULL) {
            if (line[0] == '#')
                continue;

#if defined(__WIN32) || defined(_WIN32)
                char *name = strtok(line," \t");
#else
                char *name = strtok_r(line," \t", &ptr);
#endif /* __WIN32 */
                if (name == NULL)
                continue;

#if defined(__WIN32) || defined(_WIN32)
                char *proto_ch = strtok(NULL," \t");
#else
                char *proto_ch = strtok_r(NULL," \t", &ptr);
#endif /* __WIN32 */
            if (proto_ch == NULL)
                continue;

            int proto = atoi(proto_ch);
            if (proto >= 255)
                continue;

#if defined(__WIN32) || defined(_WIN32)
                char *cname = strtok(NULL, " \t");
#else
                char *cname = strtok_r(NULL, " \t", &ptr);
#endif /* __WIN32 */

            if (cname != NULL) {
                known_proto[proto] = SCStrdup(cname);
            } else {
                known_proto[proto] = SCStrdup(name);
            }
            int proto_len = strlen(known_proto[proto]);
            if (proto_len > 0 && known_proto[proto][proto_len - 1] == '\n')
                known_proto[proto][proto_len - 1] = '\0';
        }
        fclose(fp);
    }
}

/**
 * \brief   Function to check if the received protocol number is valid and do
 *          we have corresponding name entry for this number or not.
 *
 * \param proto Protocol number to be validated
 * \retval ret On success returns TRUE otherwise FALSE
 */
uint8_t SCProtoNameValid(uint16_t proto)
{
    uint8_t ret = FALSE;

    if (proto <= 255 && known_proto[proto] != NULL) {
        ret = TRUE;
    }

    return ret;
}

/**
 *  \brief  Function to clears the memory used in storing the protocol names.
 */
void SCProtoNameDeInit()
{
    /* clears the memory of loaded protocol names */
    for (uint8_t cnt=0;cnt < 255;cnt++) {
        if(known_proto[cnt] != NULL)
            SCFree(known_proto[cnt]);
    }
}
