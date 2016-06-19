/*
 * ifaceutil.c : Utility functions to enumerate and get interface info
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

#ifdef HAVE_CONFIG_H
#	include "../config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <dirent.h>

#include "iface_util.h"

/*
 * ifaceutil_getip() -- Gets the IP address of a network
 * interface
 */
char*
ifaceutil_getip(char *iface_name)
{

	int fd;
	struct ifreq ifr;

	ifr.ifr_addr.sa_family = AF_INET;
	strncpy(ifr.ifr_name, iface_name, IFNAMSIZ - 1);

	if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
		return NULL;
	}
	if (ioctl(fd, SIOCGIFADDR, &ifr) == -1) {
		return NULL;
	}

	close(fd);

	return strdup(inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
}



int
ifaceutil_enumifaces(ifaceutil_enum_callback callback, void *data)
{
	const char * const sysfs_net = "/sys/class/net";
	DIR *dir;
	struct dirent *dp;

	if ((dir = opendir(sysfs_net)) == NULL) {
		return -1;
	}

	while ((dp = readdir(dir)) != NULL) {
		int fd;
		char name[PATH_MAX], *ip;

		if (dp->d_name[0] == '.') {
			continue;
		}

		/* check operstate */
		(void) snprintf(name, PATH_MAX, "/sys/class/net/%s/operstate", dp->d_name);
		if ((fd = open(name, O_RDONLY)) == -1) {
			closedir(dir);
			return -1;
		}
		if (read(fd, name, PATH_MAX) == -1) {
			close(fd);
			closedir(dir);
			return -1;
		}
		close(fd);

		/* operstate is "unkown" on tunnel interfaces */
		if (memcmp(name, "up", sizeof("up") - 1) &&
			memcmp(name, "unknown", sizeof("unknown") - 1)) {
			continue;
		}

		/* check carrier */
		(void) snprintf(name, PATH_MAX, "/sys/class/net/%s/carrier", dp->d_name);
		if ((fd = open(name, O_RDONLY)) == -1) {
			closedir(dir);
			return -1;
		}
		if (read(fd, name, PATH_MAX) == -1) {
			close(fd);
			closedir(dir);
			return -1;
		}
		close(fd);
		if (name[0] != '1') {
			continue;
		}

		/* check that the interface has an ip */
		if ((ip = ifaceutil_getip(dp->d_name)) == NULL) {
			continue;
		}

		free(ip);
		callback(dp->d_name, data);
	}
	closedir(dir);
	return 0;
}

#if 0
static int
callback(const char * const iface_name, void *data)
{
	fprintf(stderr, "%s\n", iface_name);
}

int
main()
{
	return ifaceutil_enumifaces(callback, NULL)
}
#endif

