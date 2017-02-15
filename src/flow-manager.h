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
 */

#ifndef __FLOW_MANAGER_H__
#define __FLOW_MANAGER_H__

/** flow manager scheduling condition */
SCPtCondT flow_manager_cond;
SCPtMutex flow_manager_mutex;

//SCCondT flow_manager_cond;
//SCMutex flow_manager_mutex;
#define FlowWakeupFlowManagerThread() SCCondSignal(&flow_manager_cond)

void FlowManagerThreadSpawn(void);
void FlowKillFlowManagerThread(void);
void FlowMgrRegisterTests (void);

#endif /* __FLOW_MANAGER_H__ */
