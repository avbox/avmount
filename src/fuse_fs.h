/* $Id: fuse_util.c 223 2006-07-25 19:43:02Z r3mi $
 *
 * main FUSE interface.
 * This file is part of avmount.
 *
 * (C) Copyright 2016 Fernando Rodriguez
 * (C) Copyright 2005 Rémi Turboult <r3mi@users.sourceforge.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef __FUSE_UTIL_H__
#define __FUSE_UTIL_H__

#include <stdio.h>
#include "djfs.h"

int
FuseFS_SetOpt(const char *opt);

int
FuseFS_Run(DJFS_Flags djfs_flags,
	size_t search_history_size,
	char *charset);

void
FuseFS_PrintVersionString(FILE *f);

void
FuseFS_Destroy(int teardown);

#endif
