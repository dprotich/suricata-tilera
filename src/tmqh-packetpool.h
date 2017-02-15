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
 */

#ifndef __TMQH_PACKETPOOL_H__
#define __TMQH_PACKETPOOL_H__

Packet *TmqhInputPacketpool(ThreadVars *);
void TmqhOutputPacketpool(ThreadVars *, Packet *);
void TmqhReleasePacketsToPacketPool(PacketQueue *);
void TmqhPacketpoolRegister (void);
void TmqhPacketpoolDestroy (void);
#ifdef __tile__
Packet *PacketPoolGetPacket(int pool);
uint16_t PacketPoolSize(int pool);
void PacketPoolWait(int pool);
#else
Packet *PacketPoolGetPacket(void);
uint16_t PacketPoolSize(void);
void PacketPoolWait(void);
#endif
void PacketPoolStorePacket(Packet *);

void PacketPoolInit(intmax_t max_pending_packets);
void PacketPoolDestroy(void);

#endif /* __TMQH_PACKETPOOL_H__ */
