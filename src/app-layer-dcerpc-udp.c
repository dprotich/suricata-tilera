/*
 * Copyright (c) 2009, 2010 Open Information Security Foundation
 *
 * \author Kirby Kuehl <kkuehl@gmail.com>
 *
 * \todo Updated by AS: Inspect the possibilities of sending junk start at the
 *       start of udp session to avoid alproto detection.
 */

#include "suricata-common.h"
#include "suricata.h"

#include "debug.h"
#include "decode.h"

#include "flow-util.h"

#include "threads.h"

#include "util-print.h"
#include "util-pool.h"
#include "util-debug.h"

#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"
#include "stream-tcp.h"
#include "stream.h"

#include "app-layer-protos.h"
#include "app-layer-parser.h"

#include "util-spm.h"
#include "util-unittest.h"

#include "app-layer-dcerpc-udp.h"

enum {
	DCERPC_FIELD_NONE = 0,
	DCERPC_PARSE_DCERPC_HEADER,
	DCERPC_PARSE_DCERPC_BIND,
	DCERPC_PARSE_DCERPC_BIND_ACK,
	DCERPC_PARSE_DCERPC_REQUEST,
	/* must be last */
	DCERPC_FIELD_MAX,
};

static uint32_t FragmentDataParser(Flow *f, void *dcerpcudp_state,
		AppLayerParserState *pstate, uint8_t *input, uint32_t input_len,
		AppLayerParserResult *output) {
	SCEnter();
	DCERPCUDPState *sstate = (DCERPCUDPState *) dcerpcudp_state;
    uint8_t **stub_data_buffer = NULL;
    uint32_t *stub_data_buffer_len = NULL;
    uint8_t *stub_data_fresh = NULL;
    uint16_t stub_len = 0;

    /* request PDU.  Retrieve the request stub buffer */
    if (sstate->dcerpc.dcerpchdrudp.type == REQUEST) {
        stub_data_buffer = &sstate->dcerpc.dcerpcrequest.stub_data_buffer;
        stub_data_buffer_len = &sstate->dcerpc.dcerpcrequest.stub_data_buffer_len;
        stub_data_fresh = &sstate->dcerpc.dcerpcrequest.stub_data_fresh;

    /* response PDU.  Retrieve the response stub buffer */
    } else {
        stub_data_buffer = &sstate->dcerpc.dcerpcresponse.stub_data_buffer;
        stub_data_buffer_len = &sstate->dcerpc.dcerpcresponse.stub_data_buffer_len;
        stub_data_fresh = &sstate->dcerpc.dcerpcresponse.stub_data_fresh;
    }

    stub_len = (sstate->dcerpc.fraglenleft < input_len) ? sstate->dcerpc.fraglenleft : input_len;

    if (stub_len == 0) {
        SCReturnUInt(0);
    }
    /* if the frag is the the first frag irrespective of it being a part of
     * a multi frag PDU or not, it indicates the previous PDU's stub would
     * have been buffered and processed and we can use the buffer to hold
     * frags from a fresh request/response */
    if (sstate->dcerpc.dcerpchdrudp.flags1 & PFC_FIRST_FRAG) {
        *stub_data_buffer_len = 0;
    }

    *stub_data_buffer = SCRealloc(*stub_data_buffer, *stub_data_buffer_len + stub_len);
    if (*stub_data_buffer == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        goto end;
    }
    memcpy(*stub_data_buffer + *stub_data_buffer_len, input, stub_len);

    *stub_data_fresh = 1;
    /* length of the buffered stub */
    *stub_data_buffer_len += stub_len;

   sstate->dcerpc.fraglenleft -= stub_len;
   sstate->dcerpc.bytesprocessed += stub_len;

#ifdef DEBUG
    if (SCLogDebugEnabled()) {
        int i = 0;
        for (i = 0; i < stub_len; i++) {
            SCLogDebug("0x%02x ", input[i]);
        }
    }
#endif

end:
    SCReturnUInt((uint32_t)stub_len);
}

/**
 * \brief DCERPCParseHeader parses the 16 byte DCERPC header
 * A fast path for normal decoding is used when there is enough bytes
 * present to parse the entire header. A slow path is used to parse
 * fragmented packets.
 */
static int DCERPCUDPParseHeader(Flow *f, void *dcerpcudp_state,
		AppLayerParserState *pstate, uint8_t *input, uint32_t input_len,
		AppLayerParserResult *output) {
	SCEnter();
	uint8_t *p = input;
	DCERPCUDPState *sstate = (DCERPCUDPState *) dcerpcudp_state;
	if (input_len) {
		switch (sstate->bytesprocessed) {
		case 0:
            // fallthrough
            /* above statement to prevent coverity FPs from the switch
             * fall through */
			if (input_len >= DCERPC_UDP_HDR_LEN) {
				sstate->dcerpc.dcerpchdrudp.rpc_vers = *p;
				if (sstate->dcerpc.dcerpchdrudp.rpc_vers != 4) {
					SCLogDebug("DCERPC UDP Header did not validate");
					SCReturnInt(-1);
				}
				sstate->dcerpc.dcerpchdrudp.type = *(p + 1);
				sstate->dcerpc.dcerpchdrudp.flags1 = *(p + 2);
				sstate->dcerpc.dcerpchdrudp.flags2 = *(p + 3);
				sstate->dcerpc.dcerpchdrudp.drep[0] = *(p + 4);
				sstate->dcerpc.dcerpchdrudp.drep[1] = *(p + 5);
				sstate->dcerpc.dcerpchdrudp.drep[2] = *(p + 6);
				sstate->dcerpc.dcerpchdrudp.serial_hi = *(p + 7);
				sstate->dcerpc.dcerpchdrudp.objectuuid[3] = *(p + 8);
				sstate->dcerpc.dcerpchdrudp.objectuuid[2] = *(p + 9);
				sstate->dcerpc.dcerpchdrudp.objectuuid[1] = *(p + 10);
				sstate->dcerpc.dcerpchdrudp.objectuuid[0] = *(p + 11);
				sstate->dcerpc.dcerpchdrudp.objectuuid[5] = *(p + 12);
				sstate->dcerpc.dcerpchdrudp.objectuuid[4] = *(p + 13);
				sstate->dcerpc.dcerpchdrudp.objectuuid[7] = *(p + 14);
				sstate->dcerpc.dcerpchdrudp.objectuuid[6] = *(p + 15);
				sstate->dcerpc.dcerpchdrudp.objectuuid[8] = *(p + 16);
				sstate->dcerpc.dcerpchdrudp.objectuuid[9] = *(p + 17);
				sstate->dcerpc.dcerpchdrudp.objectuuid[10] = *(p + 18);
				sstate->dcerpc.dcerpchdrudp.objectuuid[11] = *(p + 19);
				sstate->dcerpc.dcerpchdrudp.objectuuid[12] = *(p + 20);
				sstate->dcerpc.dcerpchdrudp.objectuuid[13] = *(p + 21);
				sstate->dcerpc.dcerpchdrudp.objectuuid[14] = *(p + 22);
				sstate->dcerpc.dcerpchdrudp.objectuuid[15] = *(p + 23);
				sstate->dcerpc.dcerpchdrudp.interfaceuuid[3] = *(p + 24);
				sstate->dcerpc.dcerpchdrudp.interfaceuuid[2] = *(p + 25);
				sstate->dcerpc.dcerpchdrudp.interfaceuuid[1] = *(p + 26);
				sstate->dcerpc.dcerpchdrudp.interfaceuuid[0] = *(p + 27);
				sstate->dcerpc.dcerpchdrudp.interfaceuuid[5] = *(p + 28);
				sstate->dcerpc.dcerpchdrudp.interfaceuuid[4] = *(p + 29);
				sstate->dcerpc.dcerpchdrudp.interfaceuuid[7] = *(p + 30);
				sstate->dcerpc.dcerpchdrudp.interfaceuuid[6] = *(p + 31);
				sstate->dcerpc.dcerpchdrudp.interfaceuuid[8] = *(p + 32);
				sstate->dcerpc.dcerpchdrudp.interfaceuuid[9] = *(p + 33);
				sstate->dcerpc.dcerpchdrudp.interfaceuuid[10] = *(p + 34);
				sstate->dcerpc.dcerpchdrudp.interfaceuuid[11] = *(p + 35);
				sstate->dcerpc.dcerpchdrudp.interfaceuuid[12] = *(p + 36);
				sstate->dcerpc.dcerpchdrudp.interfaceuuid[13] = *(p + 37);
				sstate->dcerpc.dcerpchdrudp.interfaceuuid[14] = *(p + 38);
				sstate->dcerpc.dcerpchdrudp.interfaceuuid[15] = *(p + 39);
				sstate->dcerpc.dcerpchdrudp.activityuuid[3] = *(p + 40);
				sstate->dcerpc.dcerpchdrudp.activityuuid[2] = *(p + 41);
				sstate->dcerpc.dcerpchdrudp.activityuuid[1] = *(p + 42);
				sstate->dcerpc.dcerpchdrudp.activityuuid[0] = *(p + 43);
				sstate->dcerpc.dcerpchdrudp.activityuuid[5] = *(p + 44);
				sstate->dcerpc.dcerpchdrudp.activityuuid[4] = *(p + 45);
				sstate->dcerpc.dcerpchdrudp.activityuuid[7] = *(p + 46);
				sstate->dcerpc.dcerpchdrudp.activityuuid[6] = *(p + 47);
				sstate->dcerpc.dcerpchdrudp.activityuuid[8] = *(p + 48);
				sstate->dcerpc.dcerpchdrudp.activityuuid[9] = *(p + 49);
				sstate->dcerpc.dcerpchdrudp.activityuuid[10] = *(p + 50);
				sstate->dcerpc.dcerpchdrudp.activityuuid[11] = *(p + 51);
				sstate->dcerpc.dcerpchdrudp.activityuuid[12] = *(p + 52);
				sstate->dcerpc.dcerpchdrudp.activityuuid[13] = *(p + 53);
				sstate->dcerpc.dcerpchdrudp.activityuuid[14] = *(p + 54);
				sstate->dcerpc.dcerpchdrudp.activityuuid[15] = *(p + 55);
				if (sstate->dcerpc.dcerpchdrudp.drep[0] == 0x10) {
					sstate->dcerpc.dcerpchdrudp.server_boot = *(p + 56);
					sstate->dcerpc.dcerpchdrudp.server_boot |= *(p + 57) << 8;
					sstate->dcerpc.dcerpchdrudp.server_boot |= *(p + 58) << 16;
					sstate->dcerpc.dcerpchdrudp.server_boot |= *(p + 59) << 24;
					sstate->dcerpc.dcerpchdrudp.if_vers = *(p + 60);
					sstate->dcerpc.dcerpchdrudp.if_vers |= *(p + 61) << 8;
					sstate->dcerpc.dcerpchdrudp.if_vers |= *(p + 62) << 16;
					sstate->dcerpc.dcerpchdrudp.if_vers |= *(p + 63) << 24;
					sstate->dcerpc.dcerpchdrudp.seqnum = *(p + 64);
					sstate->dcerpc.dcerpchdrudp.seqnum |= *(p + 65) << 8;
					sstate->dcerpc.dcerpchdrudp.seqnum |= *(p + 66) << 16;
					sstate->dcerpc.dcerpchdrudp.seqnum |= *(p + 67) << 24;
					sstate->dcerpc.dcerpchdrudp.opnum = *(p + 68);
					sstate->dcerpc.dcerpchdrudp.opnum |= *(p + 69) << 8;
					sstate->dcerpc.dcerpchdrudp.ihint = *(p + 70);
					sstate->dcerpc.dcerpchdrudp.ihint |= *(p + 71) << 8;
					sstate->dcerpc.dcerpchdrudp.ahint = *(p + 72);
					sstate->dcerpc.dcerpchdrudp.ahint |= *(p + 73) << 8;
					sstate->dcerpc.dcerpchdrudp.fraglen = *(p + 74);
					sstate->dcerpc.dcerpchdrudp.fraglen |= *(p + 75) << 8;
					sstate->dcerpc.dcerpchdrudp.fragnum = *(p + 76);
					sstate->dcerpc.dcerpchdrudp.fragnum |= *(p + 77) << 8;
				} else {
					sstate->dcerpc.dcerpchdrudp.server_boot = *(p + 56) << 24;
					sstate->dcerpc.dcerpchdrudp.server_boot |= *(p + 57) << 16;
					sstate->dcerpc.dcerpchdrudp.server_boot |= *(p + 58) << 8;
					sstate->dcerpc.dcerpchdrudp.server_boot |= *(p + 59);
					sstate->dcerpc.dcerpchdrudp.if_vers = *(p + 60) << 24;
					sstate->dcerpc.dcerpchdrudp.if_vers |= *(p + 61) << 16;
					sstate->dcerpc.dcerpchdrudp.if_vers |= *(p + 62) << 8;
					sstate->dcerpc.dcerpchdrudp.if_vers |= *(p + 63);
					sstate->dcerpc.dcerpchdrudp.seqnum = *(p + 64) << 24;
					sstate->dcerpc.dcerpchdrudp.seqnum |= *(p + 65) << 16;
					sstate->dcerpc.dcerpchdrudp.seqnum |= *(p + 66) << 8;
					sstate->dcerpc.dcerpchdrudp.seqnum |= *(p + 67);
					sstate->dcerpc.dcerpchdrudp.opnum = *(p + 68) << 24;
					sstate->dcerpc.dcerpchdrudp.opnum |= *(p + 69) << 16;
					sstate->dcerpc.dcerpchdrudp.ihint = *(p + 70) << 8;
					sstate->dcerpc.dcerpchdrudp.ihint |= *(p + 71);
					sstate->dcerpc.dcerpchdrudp.ahint = *(p + 72) << 8;
					sstate->dcerpc.dcerpchdrudp.ahint |= *(p + 73);
					sstate->dcerpc.dcerpchdrudp.fraglen = *(p + 74) << 8;
					sstate->dcerpc.dcerpchdrudp.fraglen |= *(p + 75);
					sstate->dcerpc.dcerpchdrudp.fragnum = *(p + 76) << 8;
					sstate->dcerpc.dcerpchdrudp.fragnum |= *(p + 77);
				}
				sstate->fraglenleft = sstate->dcerpc.dcerpchdrudp.fraglen;
				sstate->dcerpc.dcerpchdrudp.auth_proto = *(p + 78);
				sstate->dcerpc.dcerpchdrudp.serial_lo = *(p + 79);
				sstate->bytesprocessed = DCERPC_UDP_HDR_LEN;
				sstate->uuid_entry = (DCERPCUuidEntry *) SCCalloc(1,
						sizeof(DCERPCUuidEntry));
				if (sstate->uuid_entry == NULL) {
					SCReturnUInt(-1);
				} else {
					memcpy(sstate->uuid_entry->uuid,
							sstate->dcerpc.dcerpchdrudp.activityuuid,
							sizeof(sstate->dcerpc.dcerpchdrudp.activityuuid));
					TAILQ_INSERT_HEAD(&sstate->uuid_list, sstate->uuid_entry,
							next);
#ifdef UNITTESTS
					if (RunmodeIsUnittests()) {
						printUUID("DCERPC UDP", sstate->uuid_entry);

					}
#endif
				}
				SCReturnUInt(80);
				break;
			} else {
				sstate->dcerpc.dcerpchdrudp.rpc_vers = *(p++);
				if (sstate->dcerpc.dcerpchdrudp.rpc_vers != 4) {
					SCLogDebug("DCERPC UDP Header did not validate");
					SCReturnInt(-1);
				}
				if (!(--input_len))
					break;
                /* We fall through to the next case if we still have input.
                 * Same applies for other cases as well */
			}
		case 1:
			sstate->dcerpc.dcerpchdrudp.type = *(p++);
			if (!(--input_len))
				break;
		case 2:
			sstate->dcerpc.dcerpchdrudp.flags1 = *(p++);
			if (!(--input_len))
				break;
		case 3:
			sstate->dcerpc.dcerpchdrudp.flags2 = *(p++);
			if (!(--input_len))
				break;
		case 4:
			sstate->dcerpc.dcerpchdrudp.drep[0] = *(p++);
			if (!(--input_len))
				break;
		case 5:
			sstate->dcerpc.dcerpchdrudp.drep[1] = *(p++);
			if (!(--input_len))
				break;
		case 6:
			sstate->dcerpc.dcerpchdrudp.drep[2] = *(p++);
			if (!(--input_len))
				break;
		case 7:
			sstate->dcerpc.dcerpchdrudp.serial_hi = *(p++);
			if (!(--input_len))
				break;
		case 8:
			sstate->dcerpc.dcerpchdrudp.objectuuid[3] = *(p++);
			if (!(--input_len))
				break;
		case 9:
			sstate->dcerpc.dcerpchdrudp.objectuuid[2] = *(p++);
			if (!(--input_len))
				break;
		case 10:
			sstate->dcerpc.dcerpchdrudp.objectuuid[1] = *(p++);
			if (!(--input_len))
				break;
		case 11:
			sstate->dcerpc.dcerpchdrudp.objectuuid[0] = *(p++);
			if (!(--input_len))
				break;
		case 12:
			sstate->dcerpc.dcerpchdrudp.objectuuid[5] = *(p++);
			if (!(--input_len))
				break;
		case 13:
			sstate->dcerpc.dcerpchdrudp.objectuuid[4] = *(p++);
			if (!(--input_len))
				break;
		case 14:
			sstate->dcerpc.dcerpchdrudp.objectuuid[7] = *(p++);
			if (!(--input_len))
				break;
		case 15:
			sstate->dcerpc.dcerpchdrudp.objectuuid[6] = *(p++);
			if (!(--input_len))
				break;
		case 16:
			sstate->dcerpc.dcerpchdrudp.objectuuid[8] = *(p++);
			if (!(--input_len))
				break;
		case 17:
			sstate->dcerpc.dcerpchdrudp.objectuuid[9] = *(p++);
			if (!(--input_len))
				break;
		case 18:
			sstate->dcerpc.dcerpchdrudp.objectuuid[10] = *(p++);
			if (!(--input_len))
				break;
		case 19:
			sstate->dcerpc.dcerpchdrudp.objectuuid[11] = *(p++);
			if (!(--input_len))
				break;
		case 20:
			sstate->dcerpc.dcerpchdrudp.objectuuid[12] = *(p++);
			if (!(--input_len))
				break;
		case 21:
			sstate->dcerpc.dcerpchdrudp.objectuuid[13] = *(p++);
			if (!(--input_len))
				break;
		case 22:
			sstate->dcerpc.dcerpchdrudp.objectuuid[14] = *(p++);
			if (!(--input_len))
				break;
		case 23:
			sstate->dcerpc.dcerpchdrudp.objectuuid[15] = *(p++);
			if (!(--input_len))
				break;
		case 24:
			sstate->dcerpc.dcerpchdrudp.interfaceuuid[3] = *(p++);
			if (!(--input_len))
				break;
		case 25:
			sstate->dcerpc.dcerpchdrudp.interfaceuuid[2] = *(p++);
			if (!(--input_len))
				break;
		case 26:
			sstate->dcerpc.dcerpchdrudp.interfaceuuid[1] = *(p++);
			if (!(--input_len))
				break;
		case 27:
			sstate->dcerpc.dcerpchdrudp.interfaceuuid[0] = *(p++);
			if (!(--input_len))
				break;
		case 28:
			sstate->dcerpc.dcerpchdrudp.interfaceuuid[5] = *(p++);
			if (!(--input_len))
				break;
		case 29:
			sstate->dcerpc.dcerpchdrudp.interfaceuuid[4] = *(p++);
			if (!(--input_len))
				break;
		case 30:
			sstate->dcerpc.dcerpchdrudp.interfaceuuid[7] = *(p++);
			if (!(--input_len))
				break;
		case 31:
			sstate->dcerpc.dcerpchdrudp.interfaceuuid[6] = *(p++);
			if (!(--input_len))
				break;
		case 32:
			sstate->dcerpc.dcerpchdrudp.interfaceuuid[8] = *(p++);
			if (!(--input_len))
				break;
		case 33:
			sstate->dcerpc.dcerpchdrudp.interfaceuuid[9] = *(p++);
			if (!(--input_len))
				break;
		case 34:
			sstate->dcerpc.dcerpchdrudp.interfaceuuid[10] = *(p++);
			if (!(--input_len))
				break;
		case 35:
			sstate->dcerpc.dcerpchdrudp.interfaceuuid[11] = *(p++);
			if (!(--input_len))
				break;
		case 36:
			sstate->dcerpc.dcerpchdrudp.interfaceuuid[12] = *(p++);
			if (!(--input_len))
				break;
		case 37:
			sstate->dcerpc.dcerpchdrudp.interfaceuuid[13] = *(p++);
			if (!(--input_len))
				break;
		case 38:
			sstate->dcerpc.dcerpchdrudp.interfaceuuid[14] = *(p++);
			if (!(--input_len))
				break;
		case 39:
			sstate->dcerpc.dcerpchdrudp.interfaceuuid[15] = *(p++);
			if (!(--input_len))
				break;
		case 40:
			sstate->dcerpc.dcerpchdrudp.activityuuid[3] = *(p++);
			if (!(--input_len))
				break;
		case 41:
			sstate->dcerpc.dcerpchdrudp.activityuuid[2] = *(p++);
			if (!(--input_len))
				break;
		case 42:
			sstate->dcerpc.dcerpchdrudp.activityuuid[1] = *(p++);
			if (!(--input_len))
				break;
		case 43:
			sstate->dcerpc.dcerpchdrudp.activityuuid[0] = *(p++);
			if (!(--input_len))
				break;
		case 44:
			sstate->dcerpc.dcerpchdrudp.activityuuid[5] = *(p++);
			if (!(--input_len))
				break;
		case 45:
			sstate->dcerpc.dcerpchdrudp.activityuuid[4] = *(p++);
			if (!(--input_len))
				break;
		case 46:
			sstate->dcerpc.dcerpchdrudp.activityuuid[7] = *(p++);
			if (!(--input_len))
				break;
		case 47:
			sstate->dcerpc.dcerpchdrudp.activityuuid[6] = *(p++);
			if (!(--input_len))
				break;
		case 48:
			sstate->dcerpc.dcerpchdrudp.activityuuid[8] = *(p++);
			if (!(--input_len))
				break;
		case 49:
			sstate->dcerpc.dcerpchdrudp.activityuuid[9] = *(p++);
			if (!(--input_len))
				break;
		case 50:
			sstate->dcerpc.dcerpchdrudp.activityuuid[10] = *(p++);
			if (!(--input_len))
				break;
		case 51:
			sstate->dcerpc.dcerpchdrudp.activityuuid[11] = *(p++);
			if (!(--input_len))
				break;
		case 52:
			sstate->dcerpc.dcerpchdrudp.activityuuid[12] = *(p++);
			if (!(--input_len))
				break;
		case 53:
			sstate->dcerpc.dcerpchdrudp.activityuuid[13] = *(p++);
			if (!(--input_len))
				break;
		case 54:
			sstate->dcerpc.dcerpchdrudp.activityuuid[14] = *(p++);
			if (!(--input_len))
				break;
		case 55:
			sstate->dcerpc.dcerpchdrudp.activityuuid[15] = *(p++);
			if (!(--input_len))
				break;
		case 56:
			sstate->dcerpc.dcerpchdrudp.server_boot = *(p++);
			if (!(--input_len))
				break;
		case 57:
			sstate->dcerpc.dcerpchdrudp.server_boot |= *(p++) << 8;
			if (!(--input_len))
				break;
		case 58:
			sstate->dcerpc.dcerpchdrudp.server_boot |= *(p++) << 16;
			if (!(--input_len))
				break;
		case 59:
			sstate->dcerpc.dcerpchdrudp.server_boot |= *(p++) << 24;
			if (!(--input_len))
				break;
		case 60:
			sstate->dcerpc.dcerpchdrudp.if_vers = *(p++);
			if (!(--input_len))
				break;
		case 61:
			sstate->dcerpc.dcerpchdrudp.if_vers |= *(p++) << 8;
			if (!(--input_len))
				break;
		case 62:
			sstate->dcerpc.dcerpchdrudp.if_vers |= *(p++) << 16;
			if (!(--input_len))
				break;
		case 63:
			sstate->dcerpc.dcerpchdrudp.if_vers |= *(p++) << 24;
			if (!(--input_len))
				break;
		case 64:
			sstate->dcerpc.dcerpchdrudp.seqnum = *(p++);
			if (!(--input_len))
				break;
		case 65:
			sstate->dcerpc.dcerpchdrudp.seqnum |= *(p++) << 8;
			if (!(--input_len))
				break;
		case 66:
			sstate->dcerpc.dcerpchdrudp.seqnum |= *(p++) << 16;
			if (!(--input_len))
				break;
		case 67:
			sstate->dcerpc.dcerpchdrudp.seqnum |= *(p++) << 24;
			if (!(--input_len))
				break;
		case 68:
			sstate->dcerpc.dcerpchdrudp.opnum = *(p++);
			if (!(--input_len))
				break;
		case 69:
			sstate->dcerpc.dcerpchdrudp.opnum |= *(p++) << 8;
			if (!(--input_len))
				break;
		case 70:
			sstate->dcerpc.dcerpchdrudp.ihint = *(p++);
			if (!(--input_len))
				break;
		case 71:
			sstate->dcerpc.dcerpchdrudp.ihint |= *(p++) << 8;
			if (!(--input_len))
				break;
		case 72:
			sstate->dcerpc.dcerpchdrudp.ahint = *(p++);
			if (!(--input_len))
				break;
		case 73:
			sstate->dcerpc.dcerpchdrudp.ahint |= *(p++) << 8;
			if (!(--input_len))
				break;
		case 74:
			sstate->dcerpc.dcerpchdrudp.fraglen = *(p++);
			if (!(--input_len))
				break;
		case 75:
			sstate->dcerpc.dcerpchdrudp.fraglen |= *(p++) << 8;
			if (!(--input_len))
				break;
		case 76:
			sstate->dcerpc.dcerpchdrudp.fragnum = *(p++);
			if (!(--input_len))
				break;
		case 77:
			sstate->dcerpc.dcerpchdrudp.fragnum |= *(p++);
			if (!(--input_len))
				break;
		case 78:
			sstate->dcerpc.dcerpchdrudp.auth_proto = *(p++);
			if (!(--input_len))
				break;
		case 79:
			sstate->dcerpc.dcerpchdrudp.serial_lo = *(p++);
			if (sstate->dcerpc.dcerpchdrudp.drep[0] != 0x10) {
				sstate->dcerpc.dcerpchdrudp.server_boot = SCByteSwap32(sstate->dcerpc.dcerpchdrudp.server_boot);
				sstate->dcerpc.dcerpchdrudp.if_vers= SCByteSwap32(sstate->dcerpc.dcerpchdrudp.if_vers);
				sstate->dcerpc.dcerpchdrudp.seqnum= SCByteSwap32(sstate->dcerpc.dcerpchdrudp.seqnum);
				sstate->dcerpc.dcerpchdrudp.opnum = SCByteSwap16(sstate->dcerpc.dcerpchdrudp.opnum);
				sstate->dcerpc.dcerpchdrudp.ihint= SCByteSwap16(sstate->dcerpc.dcerpchdrudp.ihint);
				sstate->dcerpc.dcerpchdrudp.ahint = SCByteSwap16(sstate->dcerpc.dcerpchdrudp.ahint);
				sstate->dcerpc.dcerpchdrudp.fraglen = SCByteSwap16(sstate->dcerpc.dcerpchdrudp.fraglen);
				sstate->dcerpc.dcerpchdrudp.fragnum = SCByteSwap16(sstate->dcerpc.dcerpchdrudp.fragnum);
			}
			sstate->fraglenleft = sstate->dcerpc.dcerpchdrudp.fraglen;
			sstate->uuid_entry = (DCERPCUuidEntry *) SCCalloc(1,
					sizeof(DCERPCUuidEntry));
			if (sstate->uuid_entry == NULL) {
				SCReturnUInt(-1);
			} else {
				memcpy(sstate->uuid_entry->uuid,
						sstate->dcerpc.dcerpchdrudp.activityuuid,
						sizeof(sstate->dcerpc.dcerpchdrudp.activityuuid));
				TAILQ_INSERT_HEAD(&sstate->uuid_list, sstate->uuid_entry,
						next);
#ifdef UNITTESTS
				if (RunmodeIsUnittests()) {
					printUUID("DCERPC UDP", sstate->uuid_entry);
				}
#endif
			}
			--input_len;
			break;
		}
	}
	sstate->bytesprocessed += (p - input);
	SCReturnInt((p - input));
}

static int DCERPCUDPParse(Flow *f, void *dcerpc_state,
                          AppLayerParserState *pstate,
                          uint8_t *input, uint32_t input_len,
                          void *local_data, AppLayerParserResult *output)
{
	uint32_t retval = 0;
	uint32_t parsed = 0;
	int hdrretval = 0;
	SCEnter();

	DCERPCUDPState *sstate = (DCERPCUDPState *) dcerpc_state;
	while (sstate->bytesprocessed < DCERPC_UDP_HDR_LEN && input_len) {
		hdrretval = DCERPCUDPParseHeader(f, dcerpc_state, pstate, input,
				input_len, output);
		if (hdrretval == -1 || hdrretval > (int32_t)input_len) {
			sstate->bytesprocessed = 0;
			SCReturnInt(hdrretval);
		} else {
			parsed += hdrretval;
			input_len -= hdrretval;
		}
	}

#if 0
	printf("Done with DCERPCUDPParseHeader bytesprocessed %u/%u left %u\n",
			sstate->bytesprocessed, sstate->dcerpc.dcerpchdrudp.fraglen, input_len);
	printf("\nDCERPC Version:\t%u\n", sstate->dcerpc.dcerpchdrudp.rpc_vers);
	printf("DCERPC Type:\t%u\n", sstate->dcerpc.dcerpchdrudp.ptype);
	printf("DCERPC Flags1:\t0x%02x\n", sstate->dcerpc.dcerpchdrudp.flags1);
	printf("DCERPC Flags2:\t0x%02x\n", sstate->dcerpc.dcerpchdrudp.flags2);
	printf("DCERPC Packed Drep:\t%02x %02x %02x\n",
			sstate->dcerpc.dcerpchdrudp.drep[0], sstate->dcerpc.dcerpchdrudp.drep[1],
			sstate->dcerpc.dcerpchdrudp.drep[2]);
	printf("DCERPC Frag Length:\t0x%04x %u\n", sstate->dcerpc.dcerpchdrudp.fraglen,
			sstate->dcerpc.dcerpchdrudp.fraglen);
	printf("DCERPC Frag Number:\t0x%04x\n", sstate->dcerpc.dcerpchdrudp.fragnum);
	printf("DCERPC OpNum:\t0x%04x\n", sstate->dcerpc.dcerpchdrudp.opnum);
#endif

	while (sstate->bytesprocessed >= DCERPC_UDP_HDR_LEN
			&& sstate->bytesprocessed < sstate->dcerpc.dcerpchdrudp.fraglen
			&& input_len) {
		retval = FragmentDataParser(f, dcerpc_state, pstate, input + parsed,
				input_len, output);
		if (retval || retval > input_len) {
			parsed += retval;
			input_len -= retval;
		} else if (input_len) {
			SCLogDebug("Error parsing DCERPC UDP Fragment Data");
			parsed -= input_len;
			input_len = 0;
			sstate->bytesprocessed = 0;
		}
	}

	if (sstate->bytesprocessed == sstate->dcerpc.dcerpchdrudp.fraglen) {
		sstate->bytesprocessed = 0;
	}
	if (pstate == NULL)
		SCReturnInt(-1);

	pstate->parse_field = 0;

	SCReturnInt(1);
}

static void *DCERPCUDPStateAlloc(void) {
	void *s = SCMalloc(sizeof(DCERPCUDPState));
	if (unlikely(s == NULL))
		return NULL;

	memset(s, 0, sizeof(DCERPCUDPState));
	return s;
}

static void DCERPCUDPStateFree(void *s) {
	DCERPCUDPState *sstate = (DCERPCUDPState *) s;

	DCERPCUuidEntry *item;

	while ((item = TAILQ_FIRST(&sstate->uuid_list))) {
		//printUUID("Free", item);
		TAILQ_REMOVE(&sstate->uuid_list, item, next);
		SCFree(item);
	}
    if (sstate->dcerpc.dcerpcrequest.stub_data_buffer != NULL) {
        SCFree(sstate->dcerpc.dcerpcrequest.stub_data_buffer);
        sstate->dcerpc.dcerpcrequest.stub_data_buffer = NULL;
        sstate->dcerpc.dcerpcrequest.stub_data_buffer_len = 0;
    }
    if (sstate->dcerpc.dcerpcresponse.stub_data_buffer != NULL) {
        SCFree(sstate->dcerpc.dcerpcresponse.stub_data_buffer);
        sstate->dcerpc.dcerpcresponse.stub_data_buffer = NULL;
        sstate->dcerpc.dcerpcresponse.stub_data_buffer_len = 0;
    }
	if (s) {
		SCFree(s);
		s = NULL;
	}
}

void RegisterDCERPCUDPParsers(void) {
    char *proto_name = "dcerpcudp";

    /** DCERPC */
    AlpProtoAdd(&alp_proto_ctx, proto_name, IPPROTO_UDP, ALPROTO_DCERPC_UDP, "|04 00|", 2, 0, STREAM_TOSERVER);

	AppLayerRegisterProto(proto_name, ALPROTO_DCERPC_UDP, STREAM_TOSERVER,
			DCERPCUDPParse);
	AppLayerRegisterProto(proto_name, ALPROTO_DCERPC_UDP, STREAM_TOCLIENT,
			DCERPCUDPParse);
	AppLayerRegisterStateFuncs(ALPROTO_DCERPC_UDP, DCERPCUDPStateAlloc,
			DCERPCUDPStateFree);
}

/* UNITTESTS */
#ifdef UNITTESTS
/** \test DCERPC UDP Header Parsing and UUID handling
 */

int DCERPCUDPParserTest01(void) {
	int result = 1;
	Flow f;
	uint8_t dcerpcrequest[] = {
		0x04, 0x00, 0x2c, 0x00, 0x10, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xa0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
		0x3f, 0x98, 0xf0, 0x5c, 0xd9, 0x63, 0xcc, 0x46,
		0xc2, 0x74, 0x51, 0x6c, 0x8a, 0x53, 0x7d, 0x6f,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0xff, 0xff,
		0xff, 0xff, 0x70, 0x05, 0x00, 0x00, 0x00, 0x00,
		0x05, 0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x32, 0x24, 0x58, 0xfd,
		0xcc, 0x45, 0x64, 0x49, 0xb0, 0x70, 0xdd, 0xae,
		0x74, 0x2c, 0x96, 0xd2, 0x60, 0x5e, 0x0d, 0x00,
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x70, 0x5e, 0x0d, 0x00, 0x02, 0x00, 0x00, 0x00,
		0x7c, 0x5e, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x10, 0x00, 0x00, 0x00, 0x80, 0x96, 0xf1, 0xf1,
		0x2a, 0x4d, 0xce, 0x11, 0xa6, 0x6a, 0x00, 0x20,
		0xaf, 0x6e, 0x72, 0xf4, 0x0c, 0x00, 0x00, 0x00,
		0x4d, 0x41, 0x52, 0x42, 0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x0d, 0xf0, 0xad, 0xba,
		0x00, 0x00, 0x00, 0x00, 0xa8, 0xf4, 0x0b, 0x00,
		0x10, 0x09, 0x00, 0x00, 0x10, 0x09, 0x00, 0x00,
		0x4d, 0x45, 0x4f, 0x57, 0x04, 0x00, 0x00, 0x00,
		0xa2, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
		0x38, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
		0x00, 0x00, 0x00, 0x00, 0xe0, 0x08, 0x00, 0x00,
		0xd8, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x10, 0x08, 0x00, 0xcc, 0xcc, 0xcc, 0xcc,
		0xc8, 0x00, 0x00, 0x00, 0x4d, 0x45, 0x4f, 0x57,
		0xd8, 0x08, 0x00, 0x00, 0xd8, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
		0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xc4, 0x28, 0xcd, 0x00,
		0x64, 0x29, 0xcd, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x07, 0x00, 0x00, 0x00, 0xb9, 0x01, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x46, 0xab, 0x01, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x46, 0xa5, 0x01, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x46, 0xa6, 0x01, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x46, 0xa4, 0x01, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x46, 0xad, 0x01, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x46, 0xaa, 0x01, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x46, 0x07, 0x00, 0x00, 0x00,
		0x60, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00,
		0x90, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
		0x20, 0x00, 0x00, 0x00, 0x28, 0x06, 0x00, 0x00,
		0x30, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x10, 0x08, 0x00, 0xcc, 0xcc, 0xcc, 0xcc,
		0x50, 0x00, 0x00, 0x00, 0x4f, 0xb6, 0x88, 0x20,
		0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x10, 0x08, 0x00, 0xcc, 0xcc, 0xcc, 0xcc,
		0x48, 0x00, 0x00, 0x00, 0x07, 0x00, 0x66, 0x00,
		0x06, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
		0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x78, 0x19, 0x0c, 0x00,
		0x58, 0x00, 0x00, 0x00, 0x05, 0x00, 0x06, 0x00,
		0x01, 0x00, 0x00, 0x00, 0x70, 0xd8, 0x98, 0x93,
		0x98, 0x4f, 0xd2, 0x11, 0xa9, 0x3d, 0xbe, 0x57,
		0xb2, 0x00, 0x00, 0x00, 0x32, 0x00, 0x31, 0x00,
		0x01, 0x10, 0x08, 0x00, 0xcc, 0xcc, 0xcc, 0xcc,
		0x80, 0x00, 0x00, 0x00, 0x0d, 0xf0, 0xad, 0xba,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x18, 0x43, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x60, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00,
		0x4d, 0x45, 0x4f, 0x57, 0x04, 0x00, 0x00, 0x00,
		0xc0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
		0x3b, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
		0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x01, 0x00, 0x81, 0xc5, 0x17, 0x03,
		0x80, 0x0e, 0xe9, 0x4a, 0x99, 0x99, 0xf1, 0x8a,
		0x50, 0x6f, 0x7a, 0x85, 0x02, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x10, 0x08, 0x00, 0xcc, 0xcc, 0xcc, 0xcc,
		0x30, 0x00, 0x00, 0x00, 0x78, 0x00, 0x6e, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xd8, 0xda, 0x0d, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x20, 0x2f, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
		0x46, 0x00, 0x58, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x10, 0x08, 0x00, 0xcc, 0xcc, 0xcc, 0xcc,
		0x10, 0x00, 0x00, 0x00, 0x30, 0x00, 0x2e, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x10, 0x08, 0x00, 0xcc, 0xcc, 0xcc, 0xcc,
		0x68, 0x00, 0x00, 0x00, 0x0e, 0x00, 0xff, 0xff,
		0x68, 0x8b, 0x0b, 0x00, 0x02, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xfe, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xfe, 0x02, 0x00, 0x00, 0x5c, 0x00, 0x5c, 0x00,
		0x31, 0x00, 0x31, 0x00, 0x31, 0x00, 0x31, 0x00,
		0x31, 0x00, 0x31, 0x00, 0x31, 0x00, 0x31, 0x00,
		0x31, 0x00, 0x31, 0x00, 0x31, 0x00, 0x31, 0x00,
		0x31, 0x00, 0x31, 0x00, 0x31, 0x00, 0x31, 0x00,
		0x31, 0x00, 0x31, 0x00, 0x9d, 0x13, 0x00, 0x01,
		0xcc, 0xe0, 0xfd, 0x7f, 0xcc, 0xe0, 0xfd, 0x7f,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
	uint32_t requestlen = sizeof(dcerpcrequest);

	TcpSession ssn;
	DCERPCUuidEntry *uuid_entry;

	memset(&f, 0, sizeof(f));
	memset(&ssn, 0, sizeof(ssn));
	f.protoctx = (void *)&ssn;
    FLOW_INITIALIZE(&f);

	StreamTcpInitConfig(TRUE);

	int r = AppLayerParse(NULL, &f, ALPROTO_DCERPC_UDP, STREAM_TOSERVER|STREAM_START, dcerpcrequest, requestlen);
	if (r != 0) {
		printf("dcerpc header check returned %" PRId32 ", expected 0: ", r);
		result = 0;
		goto end;
	}

	DCERPCUDPState *dcerpc_state = f.alstate;
	if (dcerpc_state == NULL) {
		printf("no dcerpc state: ");
		result = 0;
		goto end;
	}

	if (dcerpc_state->dcerpc.dcerpchdrudp.rpc_vers != 4) {
		printf("expected dcerpc version 0x04, got 0x%02x : ",
				dcerpc_state->dcerpc.dcerpchdrudp.rpc_vers);
		result = 0;
		goto end;
	}

	if (dcerpc_state->dcerpc.dcerpchdrudp.fraglen != 1392) {
		printf("expected dcerpc fraglen 0x%02x , got 0x%02x : ", 1392, dcerpc_state->dcerpc.dcerpchdrudp.fraglen);
		result = 0;
		goto end;
	}

	if (dcerpc_state->dcerpc.dcerpchdrudp.opnum != 4) {
		printf("expected dcerpc opnum 0x%02x , got 0x%02x : ", 4, dcerpc_state->dcerpc.dcerpchdrudp.opnum);
		result = 0;
		goto end;
	}

	TAILQ_FOREACH(uuid_entry, &dcerpc_state->uuid_list, next) {
		printUUID("REQUEST", uuid_entry);
	}

end:
	StreamTcpFreeConfig(TRUE);
	return result;
}

void DCERPCUDPParserRegisterTests(void) {
	UtRegisterTest("DCERPCUDPParserTest01", DCERPCUDPParserTest01, 1);
}
#endif
