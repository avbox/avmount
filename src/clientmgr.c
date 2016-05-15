/*
 * clientmgr.c : Monitors network interfaces and manages children
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
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <upnp/upnp.h>

#ifdef HAVE_MALLOC_TRIM
#include <malloc.h>
#endif

#include "log.h"
#include "device_list.h"
#include "content_dir.h"
#include "xml_util.h"
#include "talloc_util.h"
#include "stream.h"
#include "fuse_fs.h"
#include "linkedlist.h"
#include "string_util.h"

/*
 * Structure used to represent a network interface
 */
LISTABLE_TYPE(iface_t,
	char *name;
	int keep;
	int eventsfd;
	int infd;
	int outfd;
#if defined(ENABLE_IPV6) && defined(HAVE_UPNPINIT2)
	int disable_ipv6;
#endif
	pid_t pid;
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_mutex_t event_lock;
	UpnpClient_Handle handle;
);

typedef enum
{
	CMD_UPNP_SUBSCRIBE,
	CMD_UPNP_SEND_ACTION,
	CMD_UPNP_UNSUBSCRIBE,
#ifdef DEBUG
	CMD_TALLOC_REPORT,
	CMD_TALLOC_REPORT_FULL,
#endif
	CMD_EXIT
}
command_t;

static void *context = NULL;
static int upnp_port = 0;
static int abort_mon = 0;
static pid_t mainpid = 0;
static pthread_t monthread;
static DIR* dir;

LIST_DECLARE_STATIC(ifaces);
static pthread_mutex_t list_lock = PTHREAD_MUTEX_INITIALIZER;

int
DeviceList_EventHandlerCallback(
	const char* iface_name, Upnp_EventType event_type,
	void* event, void* cookie);

/*
 * Maximum permissible content-length for SOAP messages, in bytes
 * (taking into account that "Browse" answers can be very large
 * if contain lot of objects).
 */
#define MAX_CONTENT_LENGTH      (1024 * 1024)

#define POLL_INTERVAL	(5)

#define PROCESS_EVENT(iface, event_type, event_data, handle) \
	DeviceList_EventHandlerCallback(iface->name, event_type, event_data, handle);

#define FIND_INTERFACE(iface, name, locked, onerror) \
	iface = ClientManager_FindInterface(name, locked); \
	if (iface == NULL) { \
		Log_Printf(LOG_ERROR, "Cannot find interface: %s", name); \
		onerror; \
	}

#define LOCK_INTERFACE(iface, name, list_locked, onerror) \
	pthread_mutex_lock(&iface->mutex); \
	if (ClientManager_FindInterface(name, list_locked) != iface) { \
		pthread_mutex_unlock(&iface->mutex); \
		onerror; \
	}

#define UNLOCK_INTERFACE(iface) \
	pthread_mutex_unlock(&iface->mutex);

#define PIPE_WRITE_VALUE(fd, value) write_or_die(fd, &value, sizeof(value))

#define PIPE_WRITE_STRING(fd, str) \
{ \
	size_t __left = strlen(str) + 1; \
	PIPE_WRITE_VALUE(fd, __left); \
	write_or_die(fd, str, __left); \
}

#define PIPE_WRITE_SID(fd, sid) write_or_die(fd, sid, sizeof(Upnp_SID))

#define PIPE_WRITE_XML(fd, xmldoc) \
{ \
	if (xmldoc != NULL) { \
		char *__xml = XMLUtil_GetDocumentString(NULL, xmldoc); \
		if (__xml == NULL) { \
			Log_Printf(LOG_ERROR, "PIPE_WRITE_XML: " \
				"XMLUtil_GetDocumentString() returned NULL!"); \
			__xml = talloc_strdup(NULL, ""); \
			if (__xml == NULL) { \
				Log_Printf(LOG_ERROR, "Out of memory!"); \
				abort(); \
			} \
		} \
		PIPE_WRITE_STRING(fd, __xml); \
		talloc_free(__xml); \
	} else { \
		PIPE_WRITE_STRING(fd, ""); \
	} \
}

#define PIPE_READ_VALUE(fd, value) read_or_die(fd, &value, sizeof(value))

#define PIPE_READ_STRING(fd, str) \
{ \
	size_t __left; \
	PIPE_READ_VALUE(fd, __left); \
	str = talloc_size(NULL, __left); \
	if (str == NULL) { \
		Log_Printf(LOG_ERROR, "Out of memory!"); \
		abort(); \
	} \
	read_or_die(fd, str, __left); \
}

#define PIPE_READ_XML(fd, xmldoc) \
{ \
	char *__xml; \
	PIPE_READ_STRING(fd, __xml); \
	if (*__xml != '\0') { \
		int __rc = ixmlParseBufferEx(__xml, &xmldoc); \
		if (__rc != IXML_SUCCESS) { \
			Log_Printf(LOG_ERROR, "Bad XML!\n%s", __xml); \
			abort(); \
		} \
	} else { \
		xmldoc = NULL; \
	} \
	talloc_free(__xml); \
}

#define PIPE_READ_SID(fd, sid) read_or_die(fd, sid, sizeof(Upnp_SID))

#define PIPE_FREE_STRING(str) talloc_free(str)
#define PIPE_FREE_XML(xml) ixmlDocument_free(xml)

/**
 * write_or_die() -- Like write but it guarantees that it will
 * write the requested amount of data and will crash the program
 * on any error condition, including EOF
 */
static void
write_or_die(int fd, const void *buf, size_t len)
{
	ssize_t ret;
	size_t written = 0;
	while ((ret = write(fd, buf + written, len)) != len) {
		if (ret == 0) {
			Log_Printf(LOG_ERROR, "write_or_die: EOF!");
			abort();

		} else if (ret == -1) {
			if (errno == EINTR) {
				continue;
			}
			Log_Printf(LOG_ERROR, "write_or_die: "
				"write() returned %zd (errno=%i,len=%zu,written=%zu)",
				ret, errno, len, written);
			abort();
		}
		len -= ret;
		written += ret;
	}
}

/**
 * read_or_die() -- Like read() but it guarantees that it will
 * return the requested amount of data and will crash the program
 * on any error condition, including EOF.
 */
static void
read_or_die(int fd, void *buf, size_t length)
{
	ssize_t ret;
	size_t bytes_read = 0;
	while ((ret = read(fd, buf + bytes_read, length)) != length) {
		if (ret == 0) {
			Log_Printf(LOG_ERROR, "read_or_die: EOF!");
			abort();

		} else if (ret == -1) {
			if (errno == EINTR) {
				continue;
			}
			Log_Printf(LOG_ERROR, "read_or_die: "
				"read() returned %zd (errno=%i,length=%zu,bytes_read=%zu)",
				ret, errno, length, bytes_read);
			abort();
		}
		bytes_read += ret;
		length -= ret;
	}
}

/**
 * read_or_eof() -- Like read() but it will either successfuly read
 * the amount requested, return EOF, or crash the program on any other
 * error condition
 */
static int
read_or_eof(int fd, void *buf, size_t length)
{
	ssize_t ret;
	size_t bytes_read = 0;
	while ((ret = read(fd, buf + bytes_read, length)) != length) {
		if (ret == 0) {
			/* EOF after some bytes read should never happen */
			if (bytes_read != 0) {
				Log_Printf(LOG_DEBUG, "read_or_eof(): EOF after %zd bytes read.",
					bytes_read);
				abort();
			}
			return 0; /* eof */
		} else if (ret == -1) {
			if (errno == EINTR) {
				continue;
			}
			Log_Printf(LOG_ERROR, "read_or_die: "
				"read() returned %zd (errno=%i,length=%zu,bytes_read=%zu)",
				ret, errno, length, bytes_read);
			abort();
		}
		bytes_read += ret;
		length -= ret;
	}
	return (bytes_read + length);
}


/**
 * EventHandlerCallback() -- Receives events from
 * libupnp and forwards them to the parent process
 */
static int
EventHandlerCallback (Upnp_EventType event_type,
	void* event, void* cookie)
{
	iface_t *iface = (iface_t*) cookie;

	pthread_mutex_lock(&iface->event_lock);

	PIPE_WRITE_VALUE(iface->eventsfd, event_type);
	PIPE_WRITE_VALUE(iface->eventsfd, iface->handle);

	switch (event_type) {
	case UPNP_DISCOVERY_ADVERTISEMENT_ALIVE:
	case UPNP_DISCOVERY_SEARCH_RESULT:
	case UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE:
	{
		int has_discovery = (event != NULL);
		PIPE_WRITE_VALUE(iface->eventsfd, has_discovery);
		if (has_discovery) {
			struct Upnp_Discovery *discovery = (struct Upnp_Discovery*) event;
			PIPE_WRITE_VALUE(iface->eventsfd, *discovery);
		}
		break;
	}
	case UPNP_CONTROL_ACTION_COMPLETE:
	{
		struct Upnp_Action_Complete *action_complete =
			(struct Upnp_Action_Complete*) event;
		PIPE_WRITE_VALUE(iface->eventsfd, *action_complete);
		PIPE_WRITE_XML(iface->eventsfd, action_complete->ActionRequest);
		PIPE_WRITE_XML(iface->eventsfd, action_complete->ActionResult);
		break;
	}
	case UPNP_EVENT_RECEIVED:
	{
		struct Upnp_Event *e = event;
		PIPE_WRITE_VALUE(iface->eventsfd, *e);
		PIPE_WRITE_XML(iface->eventsfd, e->ChangedVariables);
		/* ixmlDocument_free(e->ChangedVariables); <-- done by libupnp */
		break;
	}
	case UPNP_EVENT_SUBSCRIBE_COMPLETE:
	case UPNP_EVENT_UNSUBSCRIBE_COMPLETE:
	case UPNP_EVENT_RENEWAL_COMPLETE:
	case UPNP_EVENT_AUTORENEWAL_FAILED:
	case UPNP_EVENT_SUBSCRIPTION_EXPIRED:
	{
		struct Upnp_Event_Subscribe *subs = event;
		PIPE_WRITE_VALUE(iface->eventsfd, *subs);
		break;
	}
	default:
		break;
	}
	pthread_mutex_unlock(&iface->event_lock);
	return 0;
}

/**
 * ClientManager_GetInterfaceIp() -- Gets the IP address of a network
 * interface
 */
static char*
ClientManager_GetInterfaceIp(char *iface_name)
{

	int fd;
	struct ifreq ifr;

	ifr.ifr_addr.sa_family = AF_INET;
	strncpy(ifr.ifr_name, iface_name, IFNAMSIZ-1);

	if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
		Log_Printf(LOG_ERROR, "ClientManager_GetInterfaceIp(): socket() failed (errno=%i)",
			errno);
		return NULL;
	}
	if (ioctl(fd, SIOCGIFADDR, &ifr) == -1) {
		Log_Printf(LOG_ERROR, "ClientManager_GetInterfaceIp(): ioctl() failed (errno=%i)",
			errno);
		return NULL;
	}

	close(fd);

	return talloc_strdup(context,
		inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
}

/**
 * This runs the UPnP client on a child process
 */
static void
ClientManager_ClientLoop(iface_t *iface, int eventsfd, int infd, int outfd)
{
	int rc = 0, iret, initfailed = 0;
	ssize_t ret;
	command_t cmd = -1;

	/* ignore SIGTERM */
	(void) signal(SIGTERM, SIG_IGN);
	(void) signal(SIGINT, SIG_IGN);

	iface->eventsfd = eventsfd;

	Log_Printf (LOG_INFO, "Client[%i]: Intializing UPnP client on interface %s",
		getpid(), iface->name);

	/* Initialize libupnp */
#if defined(ENABLE_IPV6) && defined(HAVE_UPNPINIT2)
	/* if we have UpnpInit2() try it first */
	if (!iface->disable_ipv6) {
		rc = UpnpInit2(iface->name, upnp_port);
		if (rc == UPNP_E_SUCCESS) {
			goto UPNP_READY;	/* UpnpInit2() working! */
		}
		Log_Printf(LOG_ERROR, "Client[%i]: UpnpInit2(\"%s\", %i) returned %i. "
			"IP Version 6 support will not be available!",
			getpid(), iface->name, upnp_port, rc);
		initfailed = 1;
	} else {
		Log_Printf(LOG_WARNING, "Client[%i]: The last attempt to spawn an IP Version 6 "
			"enabled client failed. Launching IPv4-only client.", getpid());
	}
#endif

	/* Either we don't have UpnpInit2() or it's not working,
	 * so let's try UpnpInit() */
	char *ip = ClientManager_GetInterfaceIp(iface->name);
	if (ip == NULL) {
		Log_Printf(LOG_ERROR, "ClientManager_ClientLoop(): "
			"Could not get IP for interface '%s'", iface->name);
		rc = -1;
		goto CLIENT_EXIT;
	}
	rc = UpnpInit(ip, upnp_port);
	talloc_free(ip);

	if (rc != UPNP_E_SUCCESS) {
		Log_Printf(LOG_ERROR, "Client[%i]: UpnpInit() Error: %d",
			getpid(), rc);
		if (rc/100 == UPNP_E_SOCKET_ERROR/100) {
			Log_Printf(LOG_ERROR, "Client[%i]: Check network configuration, "
				"in particular that a multicast route "
				"is set for the default network "
				"interface", getpid());
		}
		goto CLIENT_EXIT;
	}

#if defined(ENABLE_IPV6) && defined(HAVE_UPNPINIT2)
UPNP_READY:
#endif
	/* register UPnP client */
	rc = UpnpRegisterClient(EventHandlerCallback,
		iface, &iface->handle);
	if (rc != UPNP_E_SUCCESS) {
		Log_Printf(LOG_ERROR, "Client[%i]: Error registering CP: %d",
			getpid(), rc);
		goto CLIENT_EXIT;
	}

	Log_Printf(LOG_INFO, "Client[%i]: UPnP Initialized (if=%s ip=%s port=%d handle=%lx)",
		getpid(), iface->name, UpnpGetServerIpAddress(), UpnpGetServerPort(),
		(unsigned long) iface->handle);

	/*
	 * Increase maximum permissible content-length for SOAP
	 * messages, because "Browse" answers can be very large
	 * if contain lot of objects.
	 */
	UpnpSetMaxContentLength(MAX_CONTENT_LENGTH);

	/* process commands from main process */
	while ((ret = read_or_eof(infd, &cmd, sizeof(command_t)))) {

		assert(ret == sizeof(command_t));

		switch (cmd) {
		case CMD_UPNP_SUBSCRIBE:
		{
			UpnpClient_Handle handle;
			char *eventURL;
			int timeout;
			Upnp_SID sid;

			PIPE_READ_VALUE(infd, handle);
			PIPE_READ_STRING(infd, eventURL);
			PIPE_READ_VALUE(infd, timeout);

			iret = UpnpSubscribe(handle, eventURL, &timeout, sid);

			PIPE_WRITE_VALUE(outfd, iret);
			PIPE_WRITE_VALUE(outfd, timeout);
			PIPE_WRITE_SID(outfd, sid);
			PIPE_FREE_STRING(eventURL);
			break;
		}
		case CMD_UPNP_UNSUBSCRIBE:
		{
			UpnpClient_Handle handle;
			Upnp_SID sid;

			PIPE_READ_VALUE(infd, handle);
			PIPE_READ_SID(infd, sid);

			iret = UpnpUnSubscribe(handle, sid);

			PIPE_WRITE_VALUE(outfd, iret);
			break;
		}
		case CMD_UPNP_SEND_ACTION:
		{
			char *actionURL;
			char *serviceType;
			UpnpClient_Handle handle;
			IXML_Document *doc = NULL;
			IXML_Document *res;

			PIPE_READ_VALUE(infd, handle);
			PIPE_READ_STRING(infd, actionURL);
			PIPE_READ_STRING(infd, serviceType);
			PIPE_READ_XML(infd, doc);

			iret = UpnpSendAction(handle, actionURL, serviceType, NULL,
				doc, &res);

			PIPE_WRITE_VALUE(outfd, iret);
			PIPE_WRITE_XML(outfd, res);
			PIPE_FREE_XML(doc);
			PIPE_FREE_STRING(serviceType);
			PIPE_FREE_STRING(actionURL);
			ixmlDocument_free(res);
			break;
		}
#ifdef DEBUG
		case CMD_TALLOC_REPORT:
		{
			void *tmp_ctx = talloc_new(context);
			if (tmp_ctx == NULL) {
				PIPE_WRITE_STRING(outfd, "");
			} else {
				talloc_set_name(tmp_ctx, "talloc_report");
				StringStream* const ss = StringStream_Create (tmp_ctx);
				FILE* const file = StringStream_GetFile (ss);
				talloc_report(NULL, file);
				const char* const str = StringStream_GetSnapshot
					(ss, tmp_ctx, NULL);
				PIPE_WRITE_STRING(outfd, str);
				talloc_free(tmp_ctx);
			}
			break;
		}
		case CMD_TALLOC_REPORT_FULL:
		{
			void *tmp_ctx = talloc_new(context);
			if (tmp_ctx == NULL) {
				PIPE_WRITE_STRING(outfd, "");
			} else {
				talloc_set_name(tmp_ctx, "talloc_report_full");
				StringStream* const ss = StringStream_Create (tmp_ctx);
				FILE* const file = StringStream_GetFile (ss);
				talloc_report_full(NULL, file);
				const char* const str = StringStream_GetSnapshot
					(ss, tmp_ctx, NULL);
				PIPE_WRITE_STRING(outfd, str);
				talloc_free(tmp_ctx);
			}
			break;
		}
#endif
		case CMD_EXIT:
		{
			Log_Printf(LOG_DEBUG, "Client[%i]: Exit command received",
				getpid());

			/*
			 * Wait for any events being handled to complete
			 * and then exit
			 */
			pthread_mutex_lock(&iface->event_lock);
			goto CLIENT_EXIT;
		}
		default:
			Log_Printf(LOG_ERROR, "Client[%i]: Unknown command received %i",
				getpid(), cmd);
			abort();
		}
	}

CLIENT_EXIT:
	/* Unregister client */
	if (rc == UPNP_E_SUCCESS) {
		UpnpUnRegisterClient(iface->handle);
	}

	/* Shutdown UPnP SDK */
	UpnpFinish();

	if (cmd == CMD_EXIT) {
		pthread_mutex_unlock(&iface->event_lock);
	}

	Log_Printf(LOG_DEBUG, "Client[%i]: Exiting", getpid());

	/* re-enable SIGTERM */
	(void) signal(SIGTERM, SIG_DFL);

	/* If there was an error exit with failure status */
	if (rc != UPNP_E_SUCCESS) {
		if (initfailed) {
			exit(2);
		}
		exit(1);
	}
}

/**
 * This runs on the main process and handles events
 * sent by the client process.
 */
static void
ClientManager_ProxyLoop(iface_t *iface, int eventsfd)
{
	ssize_t ret;
	Upnp_EventType event_type;
	UpnpClient_Handle handle;

	while ((ret = read_or_eof(eventsfd, &event_type, sizeof(Upnp_EventType)))) {

		assert(ret == sizeof(Upnp_EventType));

		PIPE_READ_VALUE(eventsfd, handle);

		switch (event_type) {
		case UPNP_DISCOVERY_ADVERTISEMENT_ALIVE:
		case UPNP_DISCOVERY_SEARCH_RESULT:
		case UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE:
		{
			struct Upnp_Discovery disc;
			int has_discovery;
			PIPE_READ_VALUE(eventsfd, has_discovery);
			if (has_discovery) {
				PIPE_READ_VALUE(eventsfd, disc);
			}
			PROCESS_EVENT(iface, event_type, &disc, &handle);
			break;
		}
		case UPNP_CONTROL_ACTION_COMPLETE:
		{
			struct Upnp_Action_Complete action;
			PIPE_READ_VALUE(eventsfd, action);
			PIPE_READ_XML(eventsfd, action.ActionRequest);
			PIPE_READ_XML(eventsfd, action.ActionResult);
			PROCESS_EVENT(iface, event_type, &action, &handle);
			PIPE_FREE_XML(action.ActionResult);
			PIPE_FREE_XML(action.ActionRequest);
			break;
		}
		case UPNP_EVENT_RECEIVED:
		{
			struct Upnp_Event event;
			PIPE_READ_VALUE(eventsfd, event);
			PIPE_READ_XML(eventsfd, event.ChangedVariables);
			PROCESS_EVENT(iface, event_type, &event, &handle);
			ixmlDocument_free(event.ChangedVariables);
			break;
		}
		case UPNP_EVENT_SUBSCRIBE_COMPLETE:
		case UPNP_EVENT_UNSUBSCRIBE_COMPLETE:
		case UPNP_EVENT_RENEWAL_COMPLETE:
		case UPNP_EVENT_AUTORENEWAL_FAILED:
		case UPNP_EVENT_SUBSCRIPTION_EXPIRED:
		{
			struct Upnp_Event_Subscribe event_subscribe;
			PIPE_READ_VALUE(eventsfd, event_subscribe);
			PROCESS_EVENT(iface, event_type, &event_subscribe, &handle);
			break;
		}
		default:
			PROCESS_EVENT(iface, event_type, NULL, &handle);
		}
	}
	Log_Printf(LOG_DEBUG, "ClientManager_ProxyLoop() -- Exiting...");
}

/**
 * Launches a new process for the UPnP client and
 * a proxy to forward events from the client to the
 * child.
 */
static void*
ClientManager_ProxyThread(void *data)
{
	iface_t *iface = (iface_t*) data;

	pid_t pid;
	int eventsfd[2];	/* used to forward events to main process */
	int readfd[2]; 		/* used to read from child process */
	int writefd[2];		/* used to write to child process */

	/* create pipes for ipc */
	if (pipe(eventsfd) == -1 || pipe(readfd) == -1 || pipe(writefd) == -1) {
		Log_Printf(LOG_ERROR, "ClientManager_ProxyThread: pipe() returned %i",
			errno);
		abort();
	}

	/* Make sure the device list is in a known state */
	DeviceList_Suspend();

	if ((pid = fork()) == -1) {
		Log_Printf(LOG_ERROR, "ClientManager_AddInterface() -- fork() failed");
		DeviceList_Resume();
		return NULL;

	} else if (pid == 0) { /* CHILD */

		/* close other end of pipes */
		close(eventsfd[0]);
		close(readfd[0]);
		close(writefd[1]);

		/* close open procfs directory */
		/* closedir(dir); */

		/*
		 * Free the device list, stream buffers,
		 * and all other memory not needed by the child
		 */
		DeviceList_Resume();
		Stream_Destroy();
		DeviceList_Destroy();
		FuseFS_Destroy(0);

		/*
		 * If malloc_trim() is available then trim the heap
		 */
#ifdef HAVE_MALLOC_TRIM
		malloc_trim(0);
#endif

		/* run the client */
		ClientManager_ClientLoop(iface, eventsfd[1], writefd[0], readfd[1]);

		/* free stuff */
		talloc_free(context);

		/* close pipes and exit */
		close(eventsfd[1]);
		close(writefd[0]);
		close(readfd[1]);
		exit(0);

	} else { /* PARENT */
		int ret;

		/* close other end of pipes */
		close(eventsfd[1]);
		close(readfd[1]);
		close(writefd[0]);

		/* unlock the device list */
		DeviceList_Resume();

		iface->pid = pid;
		iface->eventsfd = eventsfd[0];
		iface->infd = writefd[1];
		iface->outfd = readfd[0];

		/* run proxy */
		ClientManager_ProxyLoop(iface, eventsfd[0]);

		/* wait for child to exit */
		while (waitpid(pid, &ret, 0) == -1) {
			Log_Printf(LOG_ERROR, "ClientManager_ProxyThread: "
				"waitpid() return -1 (errno=%i)", errno);
			if (errno == ECHILD) {
				ret = -ECHILD;
				break;
			} else if (errno == EINTR) {
				continue;
			}
			abort();
		}

#if defined(ENABLE_IPV6) && defined(HAVE_UPNPINIT2)
		if (WEXITSTATUS(ret) == 2) {
			Log_Printf(LOG_ERROR, "ClientManager: Child exited with 2 "
				"(UpnpInit2() failure)");
			iface->disable_ipv6 = 1;
		}
		else /* ... */
#endif
		if (ret) {
			Log_Printf(LOG_ERROR, "ClientManager: Child exited with %i",
				WEXITSTATUS(ret));
		}
		iface->pid = -1;

		/* close pipes */
		close(eventsfd[0]);
		close(readfd[0]);
		close(writefd[1]);
	}
	Log_Printf(LOG_DEBUG, "ClientManager_ProxyThread() -- Exiting.");
	pthread_exit(0);
	return NULL;
}

/**
 * ClientManager_RunClient() -- Runs a client on interface
 */
static void
ClientManager_RunClient(iface_t *iface)
{
	if (pthread_mutex_init(&iface->event_lock, NULL) != 0) {
		Log_Printf(LOG_ERROR, "ClientManager: Mutex initialization failed!");
		iface->pid = -1;
	}
		
	if (pthread_mutex_init(&iface->mutex, NULL) != 0) {
		Log_Printf(LOG_ERROR, "ClientManager: Mutex initialization failed!");
		iface->pid = -1;
	}

	/* fire the proxy thread */
	if (pthread_create(&iface->thread, NULL, ClientManager_ProxyThread, (void*) iface) == -1) {
		Log_Printf(LOG_ERROR, "ClientManager: pthread_create() failed");
		iface->pid = -1;
	}
}

/**
 * ClientManager_AddInterface() -- Adds an interface to the
 * list and launches a child process.
 */
static int
ClientManager_AddInterface(char *name)
{
	iface_t *iface;

	Log_Printf(LOG_INFO, "ClientManager: Starting UPnP client on interface: %s", name);

	iface = talloc_zero(context, iface_t);
	if (iface == NULL) {
		Log_Printf(LOG_ERROR, "ClientManager_AddInterface() -- Out of memory");
		return -1;
	}

#ifdef DEBUG
	talloc_set_name(iface, "NetworkInterface: %s", name);
#endif

	iface->name = talloc_strdup(iface, name);
	iface->keep = 1;

	if (iface->name == NULL) {
		talloc_free(iface);
		return -1;
	}

	/* start the client */
	ClientManager_RunClient(iface);

	/* add interface to list */
	LIST_ADD(&ifaces, iface);

	return 0;
}

/**
 * ClientManager_RemoveInterface() -- Removes an interface from
 * the list and shuts down it's child process.
 */
static void
ClientManager_RemoveInterface(iface_t *entry)
{
	Log_Printf(LOG_INFO, "ClientManager: Shutting down UPnP client on interface: %s",
		entry->name);

	/* remove from list and destroy */
	pthread_mutex_lock(&entry->mutex);
	LIST_REMOVE(entry);
	pthread_mutex_unlock(&entry->mutex);
	talloc_free(entry);
}

/**
 * ClientManager_FindInterface() -- Finds an interface
 * entry on the list by it's name
 */
static iface_t*
ClientManager_FindInterface(const char *name, int locked)
{
	iface_t *ent;
	if (!locked) {
#if 0
		int ret, i = 0;
		struct timespec tv;
		tv.tv_sec = 0;
		tv.tv_nsec = 100 * 1000000L;
		while (i++ < 15 && (ret = pthread_mutex_trylock(&list_lock)) != 0) {
			nanosleep(&tv, NULL);
		}
		if (ret != 0) {
			return NULL;
		}
#else
		pthread_mutex_lock(&list_lock);
#endif
	}
	LIST_FOREACH(iface_t*, ent, &ifaces) {
		if (!strcmp(name, ent->name)) {
			if (!locked) {
				pthread_mutex_unlock(&list_lock);
			}
			return ent;
		}
	}
	if (!locked) {
		pthread_mutex_unlock(&list_lock);
	}
	return NULL;
}

/**
 * ClientManager_CleanupInit() -- Marks all interfaces
 * for deletion
 */
static void
ClientManager_CleanupInit()
{
	iface_t *ent;
	pthread_mutex_lock(&list_lock);
	LIST_FOREACH(iface_t*, ent, &ifaces) {
		ent->keep = 0;
	}
	pthread_mutex_unlock(&list_lock);
}

/**
 * ClientManager_Cleanup() -- Removes all interfaces
 * that are marked for deletion from the list
 */
static void
ClientManager_Cleanup()
{
	iface_t *ent;
	command_t cmd = CMD_EXIT;
CLEANUP_RESTART:
	pthread_mutex_lock(&list_lock);
	LIST_FOREACH_SAFE(iface_t*, ent, &ifaces, {
		if (!ent->keep) {
			ClientManager_RemoveInterface(ent);

			/*
			 * if the child process is running send it the EXIT command
			 * and wait for it to exit (we'll know it exited when it's
			 * proxy thread exits).
			 */
			if (ent->pid != -1) {
				pthread_mutex_unlock(&list_lock);
				pthread_mutex_lock(&ent->mutex);
				if (ent->pid != -1) {
					PIPE_WRITE_VALUE(ent->infd, cmd);
					pthread_mutex_unlock(&ent->mutex);
					pthread_join(ent->thread, NULL);
					Log_Printf(LOG_DEBUG, "ClientManager_RemoveInterface() -- Client thread joined.");
				} else {
					pthread_mutex_unlock(&ent->mutex);
				}
				goto CLEANUP_RESTART;
			}
		}
	});
	pthread_mutex_unlock(&list_lock);
}

/**
 * ClientManager_CheckClients() -- Checks that all children
 * are running properly
 */
static void
ClientManager_CheckClients()
{
	iface_t *ent;
	pthread_mutex_lock(&list_lock);
	LIST_FOREACH(iface_t*, ent, &ifaces) {
		if (ent->pid == -1) {
			Log_Printf(LOG_ERROR, "ClientManager: "
				"Client for interface %s died. Restarting",
				ent->name);
			ClientManager_RunClient(ent);
		}
	}
	pthread_mutex_unlock(&list_lock);
}

/**
 * ClientManager_MonitorInterfaces() -- Monitors network interfaces and
 * launches and monitors a child process to listen
 * for UPnP advertisements on that interface
 */
static void*
ClientManager_MonitorInterfaces(void *arg)
{
	const char * const sysfs_net = "/sys/class/net";

	(void) arg;

	while (abort_mon == 0) {
		dir = opendir(sysfs_net);
		if (dir == NULL) {
			sleep(2);
			continue;
		}
		ClientManager_CleanupInit();
		struct dirent *dp;
		while ((dp = readdir(dir)) != NULL) {
			int fd;
			char name[PATH_MAX];
			iface_t *ent;

			/* ignore loopback interface and dot files */
			if (dp->d_name[0] == '.' || !strcmp("lo", dp->d_name)) {
				continue;
			}

			/*
			 * Ignore sit interfaces.
			 * TODO: Find a better way to detect usable
			 * interfaces
			 */
			if (!memcmp("sit", dp->d_name, 3 * sizeof(char))) {
				continue;
			}

			/* check operstate */
			(void) snprintf(name, PATH_MAX, "/sys/class/net/%s/operstate", dp->d_name);
			if ((fd = open(name, O_RDONLY)) == -1) {
				Log_Printf(LOG_ERROR, "ClientManager: Could not open %s", name);
				continue;
			}
			if (read(fd, name, PATH_MAX) == -1) {
				Log_Printf(LOG_ERROR, "ClientManager: Could not operstate");
				close(fd);
				continue;
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
				Log_Printf(LOG_ERROR, "ClientManager: Could not open %s", name);
				continue;
			}
			if (read(fd, name, PATH_MAX) == -1) {
				Log_Printf(LOG_ERROR, "ClientManager: Could not operstate");
				close(fd);
				continue;
			}
			close(fd);
			if (name[0] != '1') {
				continue;
			}

			if ((ent = ClientManager_FindInterface(dp->d_name, 0)) == NULL) {
				pthread_mutex_lock(&list_lock);
				if ((ent = ClientManager_FindInterface(dp->d_name, 1)) == NULL) {
					ClientManager_AddInterface(dp->d_name);
				} else {
					ent->keep = 1;
				}
				pthread_mutex_unlock(&list_lock);
			} else {
				ent->keep = 1;
			}
		}
		closedir(dir);
		ClientManager_Cleanup();
		sleep(POLL_INTERVAL);
		ClientManager_CheckClients();
	}
	return NULL;
}

/**
 * ClientManager_Init() -- Initialize the client manager
 */
void
ClientManager_Init(const int port)
{

	LIST_INIT(&ifaces);

	mainpid = getpid();
	context = talloc_new(NULL);
	upnp_port = port;

	if (context == NULL) {
		Log_Printf(LOG_ERROR, "ClientManager: talloc_new() failed!");
		return;
	}
#ifdef DEBUG
	talloc_set_name(context, "ClientManager");
#endif

	abort_mon = 0;
	if (pthread_create(&monthread, NULL, ClientManager_MonitorInterfaces, NULL) != 0) {
		Log_Printf(LOG_ERROR, "ClientManager: pthread_create() failed");
		abort();
	}
}

void
ClientManager_Stop()
{
	Log_Print(LOG_DEBUG, "ClientManager_Stop() -- Stopping");

	/* wait for ClientManager to exit */
	abort_mon = 1;
	pthread_join(monthread, NULL);
}

/**
 * ClientManager_Destroy() -- Shutdown the client manager
 */
void
ClientManager_Destroy()
{
	Log_Print(LOG_DEBUG, "ClientManager_Destroy() running");

	/* Shutdown all clients */
	ClientManager_CleanupInit();
	ClientManager_Cleanup();

	/* free stuff */
	talloc_free(context);
}

/**
 * ClientManager_UpnpSubscribe() -- Calls UpnpSubscribe() on
 * behalf of the parent process.
 */
int
ClientManager_UpnpSubscribe(const char *iface_name,
	UpnpClient_Handle ctrlpt_handle, char *eventURL, int *timeout, Upnp_SID sid)
{
	int ret;
	const command_t cmd = CMD_UPNP_SUBSCRIBE;
	iface_t *iface;
	FIND_INTERFACE(iface, iface_name, 0, return -1);
	LOCK_INTERFACE(iface, iface_name, 0, return -1);
	PIPE_WRITE_VALUE(iface->infd, cmd);
	PIPE_WRITE_VALUE(iface->infd, ctrlpt_handle);
	PIPE_WRITE_STRING(iface->infd, eventURL);
	PIPE_WRITE_VALUE(iface->infd, *timeout);
	PIPE_READ_VALUE(iface->outfd, ret);
	PIPE_READ_VALUE(iface->outfd, *timeout);
	PIPE_READ_SID(iface->outfd, sid);
	UNLOCK_INTERFACE(iface);
	return ret;
}

/**
 * ClientManager_UpnpUnSubscribe() -- Calls UpnpUnSubscribe()
 * on behalf of the parent process
 */
int
ClientManager_UpnpUnSubscribe(const char *iface_name,
	UpnpClient_Handle handle, Upnp_SID sid)
{
	int ret;
	const command_t cmd = CMD_UPNP_UNSUBSCRIBE;
	iface_t *iface;

	/*
	 * this function may be called by the destructors
	 * when destroying the device list on a child process
	 * so we just return success on that case
	 */
	if (getpid() != mainpid) {
		return 0;
	}

	FIND_INTERFACE(iface, iface_name, 0, return 0);
	LOCK_INTERFACE(iface, iface_name, 0, return 0);
	PIPE_WRITE_VALUE(iface->infd, cmd);
	PIPE_WRITE_VALUE(iface->infd, handle);
	PIPE_WRITE_SID(iface->infd, sid);
	PIPE_READ_VALUE(iface->outfd, ret);
	UNLOCK_INTERFACE(iface);

	return ret;
}

/**
 * ClientManager_UpnpSendAction() -- Calls UpnpSendAction() on
 * behalf of the parent process.
 */
int
ClientManager_UpnpSendAction(const char *iface_name,
	UpnpClient_Handle handle,
	const char *actionURL,
	const char *serviceType,
	const char *devUDN,
	IXML_Document *action,
	IXML_Document **resp)
{
	int ret;
	const command_t cmd = CMD_UPNP_SEND_ACTION;
	iface_t *iface;
	IXML_Document *doc = NULL;
	FIND_INTERFACE(iface, iface_name, 0, return -1);
	LOCK_INTERFACE(iface, iface_name, 0, return -1);
	PIPE_WRITE_VALUE(iface->infd, cmd);
	PIPE_WRITE_VALUE(iface->infd, handle);
	PIPE_WRITE_STRING(iface->infd, actionURL);
	PIPE_WRITE_STRING(iface->infd, serviceType);
	PIPE_WRITE_XML(iface->infd, action);
	PIPE_READ_VALUE(iface->outfd, ret);
	PIPE_READ_XML(iface->outfd, doc);
	UNLOCK_INTERFACE(iface);
	*resp = doc;
	return ret;
}

#ifdef DEBUG
/**
 * ClientManager_Talloc_Report() -- Prints the talloc_report() of
 * each child to the stream.
 */
void
ClientManager_Talloc_Report(FILE *file)
{
	iface_t *iface;
	char *str;
	const command_t cmd = CMD_TALLOC_REPORT;
	pthread_mutex_lock(&list_lock);
	LIST_FOREACH(iface_t*, iface, &ifaces) {
		LOCK_INTERFACE(iface, iface->name, 1, return);
		PIPE_WRITE_VALUE(iface->infd, cmd);
		PIPE_READ_STRING(iface->outfd, str);

		fprintf(file, "\n==========================================\n");
		fprintf(file, "talloc report for process: %i (%s)",
			getpid(), iface->name);
		fprintf(file, "\n==========================================\n\n");
		fprintf(file, "%s", str);

		PIPE_FREE_STRING(str);
		UNLOCK_INTERFACE(iface);
	}
	pthread_mutex_unlock(&list_lock);
}

/**
 * ClientManager_Talloc_Report_Full() -- Prints the talloc_report_full()
 * of each child to the stream.
 */
void
ClientManager_Talloc_Report_Full(FILE *file)
{
	iface_t *iface;
	char *str;
	const command_t cmd = CMD_TALLOC_REPORT_FULL;
	pthread_mutex_lock(&list_lock);
	LIST_FOREACH(iface_t*, iface, &ifaces) {
		LOCK_INTERFACE(iface, iface->name, 1, return);
		PIPE_WRITE_VALUE(iface->infd, cmd);
		PIPE_READ_STRING(iface->outfd, str);

		fprintf(file, "\n==========================================\n");
		fprintf(file, "talloc report for process: %i (%s)",
			getpid(), iface->name);
		fprintf(file, "\n==========================================\n\n");
		fprintf(file, "%s", str);

		PIPE_FREE_STRING(str);
		UNLOCK_INTERFACE(iface);
	}
	pthread_mutex_unlock(&list_lock);
}
#endif

