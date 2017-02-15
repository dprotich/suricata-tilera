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

#ifndef __UTIL_VAR_NAME_H__
#define __UTIL_VAR_NAME_H__

int VariableNameInitHash(DetectEngineCtx *);
void VariableNameFreeHash(DetectEngineCtx *);

uint16_t VariableNameGetIdx(DetectEngineCtx *, char *, uint8_t);
char * VariableIdxGetName(DetectEngineCtx *, uint16_t , uint8_t);

#endif

