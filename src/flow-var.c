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
 * \author Victor Julien <victor@inliniac.net>
 * \author Pablo Rincon <pablo.rincon.crespo@gmail.com>
 *
 * Flow level variable support for complex detection rules
 * Supported types atm are String and Integers
 */

#include "suricata-common.h"
#include "threads.h"
#include "flow-var.h"
#include "flow.h"
#include "detect.h"
#include "util-debug.h"

/* puts a new value into a flowvar */
void FlowVarUpdateStr(FlowVar *fv, uint8_t *value, uint16_t size) {
    if (fv->data.fv_str.value) SCFree(fv->data.fv_str.value);
    fv->data.fv_str.value = value;
    fv->data.fv_str.value_len = size;
}

/* puts a new value into a flowvar */
void FlowVarUpdateInt(FlowVar *fv, uint32_t value) {
    fv->data.fv_int.value = value;
}

/* get the flowvar with name 'name' from the flow
 *
 * name is a normal string*/
FlowVar *FlowVarGet(Flow *f, uint16_t idx) {
    GenericVar *gv = f->flowvar;

    for ( ; gv != NULL; gv = gv->next) {
        if (gv->type == DETECT_FLOWVAR && gv->idx == idx)
            return (FlowVar *)gv;
    }

    return NULL;
}

/* add a flowvar to the flow, or update it */
void FlowVarAddStr(Flow *f, uint16_t idx, uint8_t *value, uint16_t size) {
    FLOWLOCK_WRLOCK(f);

    FlowVar *fv = FlowVarGet(f, idx);
    if (fv == NULL) {
        fv = SCMalloc(sizeof(FlowVar));
        if (unlikely(fv == NULL))
            goto out;

        fv->type = DETECT_FLOWVAR;
        fv->datatype = FLOWVAR_TYPE_STR;
        fv->idx = idx;
        fv->data.fv_str.value = value;
        fv->data.fv_str.value_len = size;
        fv->next = NULL;

        GenericVarAppend(&f->flowvar, (GenericVar *)fv);
    } else {
        FlowVarUpdateStr(fv, value, size);
    }

out:
    FLOWLOCK_UNLOCK(f);
}

/* add a flowvar to the flow, or update it */
void FlowVarAddInt(Flow *f, uint16_t idx, uint32_t value) {
    FLOWLOCK_WRLOCK(f);

    FlowVar *fv = FlowVarGet(f, idx);
    if (fv == NULL) {
        fv = SCMalloc(sizeof(FlowVar));
        if (unlikely(fv == NULL))
            goto out;

        fv->type = DETECT_FLOWVAR;
        fv->datatype = FLOWVAR_TYPE_INT;
        fv->idx = idx;
        fv->data.fv_int.value= value;
        fv->next = NULL;

        GenericVarAppend(&f->flowvar, (GenericVar *)fv);
    } else {
        FlowVarUpdateInt(fv, value);
    }

out:
    FLOWLOCK_UNLOCK(f);
}

void FlowVarFree(FlowVar *fv) {
    if (fv == NULL)
        return;

    if (fv->datatype == FLOWVAR_TYPE_STR) {
        if (fv->data.fv_str.value != NULL)
            SCFree(fv->data.fv_str.value);
    }
    SCFree(fv);
}

void FlowVarPrint(GenericVar *gv) {
    uint16_t u;

    if (!SCLogDebugEnabled())
        return;

    if (gv == NULL)
        return;

    if (gv->type == DETECT_FLOWVAR || gv->type == DETECT_FLOWINT) {
        FlowVar *fv = (FlowVar *)gv;

        if (fv->datatype == FLOWVAR_TYPE_STR) {
            SCLogDebug("Name idx \"%" PRIu16 "\", Value \"", fv->idx);
            for (u = 0; u < fv->data.fv_str.value_len; u++) {
                if (isprint(fv->data.fv_str.value[u]))
                    SCLogDebug("%c", fv->data.fv_str.value[u]);
                else
                    SCLogDebug("\\%02X", fv->data.fv_str.value[u]);
            }
            SCLogDebug("\", Len \"%" PRIu16 "\"\n", fv->data.fv_str.value_len);
        } else if (fv->datatype == FLOWVAR_TYPE_INT) {
            SCLogDebug("Name idx \"%" PRIu16 "\", Value \"%" PRIu16 "\"", fv->idx,
                    fv->data.fv_int.value);
        } else {
            SCLogDebug("Unknown data type at flowvars\n");
        }
    }
    FlowVarPrint(gv->next);
}

