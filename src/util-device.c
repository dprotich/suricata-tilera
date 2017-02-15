/* Copyright (C) 2011-2012 Open Information Security Foundation
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

#include "suricata-common.h"
#include "conf.h"
#include "util-device.h"

/**
 * \file
 *
 * \author Eric Leblond <eric@regit.org>
 *
 *  \brief Utility functions to handle device list
 */

/** private device list */
static TAILQ_HEAD(, LiveDevice_) live_devices =
    TAILQ_HEAD_INITIALIZER(live_devices);


/**
 *  \brief Add a pcap device for monitoring
 *
 *  \param dev string with the device name
 *
 *  \retval 0 on success.
 *  \retval -1 on failure.
 */
int LiveRegisterDevice(char *dev)
{
    LiveDevice *pd = SCMalloc(sizeof(LiveDevice));
    if (unlikely(pd == NULL)) {
        return -1;
    }

    pd->dev = SCStrdup(dev);
    SC_ATOMIC_INIT(pd->pkts);
    SC_ATOMIC_INIT(pd->drop);
    SC_ATOMIC_INIT(pd->invalid_checksums);
    pd->ignore_checksum = 0;
    TAILQ_INSERT_TAIL(&live_devices, pd, next);

    SCLogDebug("Pcap device \"%s\" registered.", dev);
    return 0;
}

/**
 *  \brief Get the number of registered devices
 *
 *  \retval cnt the number of registered devices
 */
int LiveGetDeviceCount(void) {
    int i = 0;
    LiveDevice *pd;

    TAILQ_FOREACH(pd, &live_devices, next) {
        i++;
    }

    return i;
}

/**
 *  \brief Get a pointer to the device name at idx
 *
 *  \param number idx of the device in our list
 *
 *  \retval ptr pointer to the string containing the device
 *  \retval NULL on error
 */
char *LiveGetDeviceName(int number) {
    int i = 0;
    LiveDevice *pd;

    TAILQ_FOREACH(pd, &live_devices, next) {
        if (i == number) {
            return pd->dev;
        }

        i++;
    }

    return NULL;
}

/**
 *  \brief Get a pointer to the device at idx
 *
 *  \param number idx of the device in our list
 *
 *  \retval ptr pointer to the string containing the device
 *  \retval NULL on error
 */
LiveDevice *LiveGetDevice(char *name) {
    int i = 0;
    LiveDevice *pd;

    if (name == NULL) {
        SCLogWarning(SC_ERR_INVALID_VALUE, "Name of device should not be null");
        return NULL;
    }

    TAILQ_FOREACH(pd, &live_devices, next) {
        if (!strcmp(name, pd->dev)) {
            return pd;
        }

        i++;
    }

    return NULL;
}



int LiveBuildDeviceList(char * runmode)
{
    ConfNode *base = ConfGetNode(runmode);
    ConfNode *child;
    int i = 0;

    if (base == NULL)
        return 0;

    TAILQ_FOREACH(child, &base->head, next) {
        if (!strcmp(child->val, "interface")) {
            ConfNode *subchild;
            TAILQ_FOREACH(subchild, &child->head, next) {
                if ((!strcmp(subchild->name, "interface"))) {
                    if (!strcmp(subchild->val, "default"))
                        break;
                    SCLogInfo("Adding interface %s from config file",
                              subchild->val);
                    LiveRegisterDevice(subchild->val);
                    i++;
                }
            }
        }
    }

    return i;
}

#ifdef BUILD_UNIX_SOCKET
TmEcode LiveDeviceIfaceStat(json_t *cmd, json_t *answer, void *data)
{
    SCEnter();
    LiveDevice *pd;
    const char * name = NULL;
    json_t *jarg = json_object_get(cmd, "iface");
    if(!json_is_string(jarg)) {
        json_object_set_new(answer, "message", json_string("Iface is not a string"));
        SCReturnInt(TM_ECODE_FAILED);
    }
    name = json_string_value(jarg);
    if (name == NULL) {
        json_object_set_new(answer, "message", json_string("Iface name is NULL"));
        SCReturnInt(TM_ECODE_FAILED);
    }

    TAILQ_FOREACH(pd, &live_devices, next) {
        if (!strcmp(name, pd->dev)) {
            json_t *jdata = json_object();
            if (jdata == NULL) {
                json_object_set_new(answer, "message",
                        json_string("internal error at json object creation"));
                SCReturnInt(TM_ECODE_FAILED);
            }
            json_object_set_new(jdata, "pkts",
                                json_integer(SC_ATOMIC_GET(pd->pkts)));
            json_object_set_new(jdata, "invalid-checksums",
                                json_integer(SC_ATOMIC_GET(pd->invalid_checksums)));
            json_object_set_new(jdata, "drop",
                                json_integer(SC_ATOMIC_GET(pd->drop)));
            json_object_set_new(answer, "message", jdata);
            SCReturnInt(TM_ECODE_OK);
        }
    }
    json_object_set_new(answer, "message", json_string("Iface does not exist"));
    SCReturnInt(TM_ECODE_FAILED);
}

TmEcode LiveDeviceIfaceList(json_t *cmd, json_t *answer, void *data)
{
    SCEnter();
    json_t *jdata;
    json_t *jarray;
    LiveDevice *pd;
    int i = 0;

    jdata = json_object();
    if (jdata == NULL) {
        json_object_set_new(answer, "message",
                            json_string("internal error at json object creation"));
        return TM_ECODE_FAILED;
    }
    jarray = json_array();
    if (jarray == NULL) {
        json_object_set_new(answer, "message",
                            json_string("internal error at json object creation"));
        return TM_ECODE_FAILED;
    }
    TAILQ_FOREACH(pd, &live_devices, next) {
        json_array_append(jarray, json_string(pd->dev));
        i++;
    }

    json_object_set_new(jdata, "count", json_integer(i));
    json_object_set_new(jdata, "ifaces", jarray);
    json_object_set_new(answer, "message", jdata);
    SCReturnInt(TM_ECODE_OK);
}
#endif /* BUILD_UNIX_SOCKET */
