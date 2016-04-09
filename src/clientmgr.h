/*
 * clientmgr.h : Monitors network interfaces and manages children
 * This file is part of avmount.
 *
 * (C) Copyright 2016 Fernando Rodriguez
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

#ifndef __NETMON_H__
#define __NETMON_H__

#ifdef DEBUG
#	include <stdio.h>
#endif

void
ClientManager_Start();

void
ClientManager_Stop();

int
ClientManager_UpnpSubscribe(const char *iface_name,
	UpnpClient_Handle ctrlpt_handle, char *eventURL, int *timeout, Upnp_SID sid);

int
ClientManager_UpnpUnSubscribe(const char *iface_name,
	UpnpClient_Handle handle, Upnp_SID sid);

int
ClientManager_UpnpSendAction(const char *iface_name,
	UpnpClient_Handle handle,
	const char *actionURL,
	const char *serviceType,
	const char *devUDN,
	IXML_Document *action,
	IXML_Document **resp);

int
ClientManager_UpnpSetMaxContentLength(const char *ifname, size_t contentLength);

#ifdef DEBUG
void
ClientManager_Talloc_Report(FILE* file);

void
ClientManager_Talloc_Report_Full(FILE* file);
#endif

#endif

