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
#include <config.h>
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

/*
 * Structure used to represent a network interface
 */
struct iface_entry
{
	char *name;
	int keep;
	int eventsfd;
	int infd;
	int outfd;
	pid_t pid;
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_mutex_t event_lock;
	UpnpClient_Handle handle;
	struct iface_entry *prev;
	struct iface_entry *next;
};

struct iface_list
{
	struct iface_entry *first;
	struct iface_entry *last;
};

typedef enum
{
	CMD_UPNP_SUBSCRIBE,
	CMD_UPNP_SEND_ACTION,
	CMD_UPNP_UNSUBSCRIBE,
	CMD_UPNP_EXIT
}
command_t;

static void *context = NULL;
static int abort_mon = 0;
static pid_t mainpid = 0;
static struct iface_list ifaces = { NULL };
static pthread_t monthread;

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

#define FIND_INTERFACE(iface, name, onerror) \
	iface = ClientManager_FindInterface(name); \
	if (iface == NULL) { \
		Log_Printf(LOG_ERROR, "Cannot find interface: %s", name); \
		onerror; \
	}

#define LOCK_INTERFACE(iface, name, onerror) \
	pthread_mutex_lock(&iface->mutex); \
	if (ClientManager_FindInterface(name) != iface) { \
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
 * EventHandlerCallback() -- Receives events from
 * libupnp and forwards them to the parent process
 */
static int
EventHandlerCallback (Upnp_EventType event_type,
	void* event, void* cookie)
{
	struct iface_entry *iface = (struct iface_entry*) cookie;

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
 * This runs the UPnP client on a child process
 */
static void
ClientManager_ClientLoop(struct iface_entry *iface, int eventsfd, int infd, int outfd)
{
	int rc;
	command_t cmd;

	/* ignore SIGTERM */
	/* (void) signal(SIGTERM, SIG_IGN); */

	iface->eventsfd = eventsfd;

	Log_Printf (LOG_INFO, "Intializing UPnP client on interface %s",
		iface->name);

	/* Initialize libupnp */
	rc = UpnpInit2(iface->name, 0);
	if (UPNP_E_SUCCESS != rc) {
		Log_Printf (LOG_ERROR, "UpnpInit2() Error: %d", rc);
		UpnpFinish();
		if (rc == UPNP_E_SOCKET_ERROR) {
			Log_Printf (LOG_ERROR, "Check network configuration, "
				"in particular that a multicast route "
				"is set for the default network "
				"interface");
		}
		exit(1);
	}

	/* register UPnP client */
	rc = UpnpRegisterClient(EventHandlerCallback,
		iface, &iface->handle);
	if (rc != UPNP_E_SUCCESS) {
		Log_Printf (LOG_ERROR, "Error registering CP: %d", rc);
		UpnpFinish();
		exit(1);
	}

	Log_Printf(LOG_INFO, "UPnP Initialized (pid=%i if=%s ip=%s port=%d handle=%lx)",
		getpid(), iface->name, UpnpGetServerIpAddress(), UpnpGetServerPort(),
		(unsigned long) iface->handle);

	/*
	 * Increase maximum permissible content-length for SOAP
	 * messages, because "Browse" answers can be very large
	 * if contain lot of objects.
	 */
	UpnpSetMaxContentLength(MAX_CONTENT_LENGTH);

	/* process commands from main process */
	while (1) {
		ssize_t ret;
		while ((ret = read(infd, &cmd, sizeof(command_t))) > 0) {

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

				rc = UpnpSubscribe(handle, eventURL, &timeout, sid);

				PIPE_WRITE_VALUE(outfd, rc);
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

				rc = UpnpUnSubscribe(handle, sid);

				PIPE_WRITE_VALUE(outfd, rc);
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

				rc = UpnpSendAction(handle, actionURL, serviceType, NULL,
					doc, &res);

				PIPE_WRITE_VALUE(outfd, rc);
				PIPE_WRITE_XML(outfd, res);
				PIPE_FREE_XML(doc);
				PIPE_FREE_STRING(serviceType);
				PIPE_FREE_STRING(actionURL);
				ixmlDocument_free(res);
				break;
			}
			case CMD_UPNP_EXIT:
			{
				Log_Printf(LOG_DEBUG, "Client(%i): Exit command received",
					getpid());
				goto CLIENT_EXIT;
			}
			default:
				Log_Printf(LOG_ERROR, "Client(%i): Unknown command received %i",
					getpid(), cmd);
				abort();
			}
		}
		if (ret == -1) {
			if (errno == EINTR) {
				continue;
			}
			Log_Printf(LOG_ERROR, "Client(%i): read returned %zd (errno=%i)",
				getpid(), ret, errno);
			abort();
		}
		break;
	}

CLIENT_EXIT:
	/* Unregister client and shutdown UPnP SDK */
	UpnpUnRegisterClient(iface->handle);
	UpnpFinish();
}

/**
 * This runs on the main process and handles events
 * sent by the client process.
 */
static void
ClientManager_ProxyLoop(struct iface_entry *iface, int eventsfd)
{
	ssize_t ret;
	Upnp_EventType event_type;
	UpnpClient_Handle handle;

	while (1) {
		while ((ret = read(eventsfd, &event_type, sizeof(Upnp_EventType))) > 0) {

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
		if (ret == -1) {
			if (errno == EINTR) {
				continue;
			}
			Log_Printf(LOG_ERROR, "ClientManager_ProxyLoop: "
				"read() returned %zd (errno=%i)",
				ret, errno);
			abort();
		}
		break;
	}
}

/**
 * Launches a new process for the UPnP client and
 * a proxy to forward events from the client to the
 * child.
 */
static void*
ClientManager_ProxyThread(void *data)
{
	struct iface_entry *iface = (struct iface_entry*) data;

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

	if ((pid = fork()) == -1) {
		Log_Printf(LOG_ERROR, "ClientManager_AddInterface() -- fork() failed");
		return NULL;

	} else if (pid == 0) { /* CHILD */

		/* close other end of pipes */
		close(eventsfd[0]);
		close(readfd[0]);
		close(writefd[1]);

		/*
		 * Free the device list, stream buffers,
		 * and all other memory not needed by the child
		 */
		Stream_Destroy();
		DeviceList_Destroy();

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

		if (ret) {
			Log_Printf(LOG_ERROR, "Child exited with %i", ret);
		}
		iface->pid = -1;

		/* close pipes */
		close(eventsfd[0]);
		close(readfd[0]);
		close(writefd[1]);
	}
	return NULL;
}

/**
 * ClientManager_RunClient() -- Runs a client on interface
 */
static void
ClientManager_RunClient(struct iface_entry *iface)
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
	struct iface_entry *iface;

	Log_Printf(LOG_INFO, "ClientManager: Adding interface: %s", name);

	iface = talloc(context, struct iface_entry);
	if (iface == NULL) {
		Log_Printf(LOG_ERROR, "ClientManager_AddInterface() -- Out of memory");
		return -1;
	}

#ifdef DEBUG
	talloc_set_name(iface, "NetworkInterface: %s", name);
#endif

	iface->name = talloc_strdup(iface, name);
	iface->keep = 1;
	iface->prev = NULL;
	iface->next = NULL;

	if (iface->name == NULL) {
		talloc_free(iface);
		return -1;
	}

	/* start the client */
	ClientManager_RunClient(iface);

	/* add interface to list */
	if (ifaces.first == NULL || ifaces.last == NULL) {
		assert(ifaces.first == ifaces.last);
		ifaces.first = ifaces.last = iface;
	} else {
		iface->prev = ifaces.last;
		ifaces.last->next = iface;
		ifaces.last = iface;
	}

	return 0;
}

/**
 * ClientManager_RemoveInterface() -- Removes an interface from
 * the list and shuts down it's child process.
 */
static void
ClientManager_RemoveInterface(struct iface_entry *entry)
{
	Log_Printf(LOG_INFO, "ClientManager: Removing interface: %s",
		entry->name);

	pthread_mutex_lock(&entry->mutex);

	if (entry->pid != -1) {
		command_t cmd = CMD_UPNP_EXIT;
		PIPE_WRITE_VALUE(entry->infd, cmd);
		pthread_join(entry->thread, NULL);
	}

	if (entry->prev == NULL) {
		assert(ifaces.first == entry);
		ifaces.first = entry->next;
	} else {
		entry->prev->next = entry->next;
	}
	if (entry->next == NULL) {
		assert(ifaces.last == entry);
		ifaces.last = entry->prev;
	} else {
		entry->next->prev = entry->prev;
	}
	talloc_free(entry);

	pthread_mutex_unlock(&entry->mutex);
}

/**
 * ClientManager_FindInterface() -- Finds an interface
 * entry on the list by it's name
 */
static struct iface_entry*
ClientManager_FindInterface(const char *name)
{
	struct iface_entry *ent = ifaces.first;
	while (ent != NULL) {
		if (!strcmp(name, ent->name)) {
			return ent;
		}
		ent = ent->next;
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
	struct iface_entry *ent = ifaces.first;
	while (ent != NULL) {
		ent->keep = 0;
		ent = ent->next;
	}
}

/**
 * ClientManager_Cleanup() -- Removes all interfaces
 * that are marked for deletion from the list
 */
static void
ClientManager_Cleanup()
{
	struct iface_entry *ent = ifaces.first;
	while (ent != NULL) {
		struct iface_entry *next = ent->next;
		if (!ent->keep) {
			ClientManager_RemoveInterface(ent);
		}
		ent = next;
	}
}

/**
 * ClientManager_CheckClients() -- Checks that all children
 * are running properly
 */
static void
ClientManager_CheckClients()
{
	struct iface_entry *ent = ifaces.first;
	while (ent != NULL) {
		if (ent->pid == -1) {
			Log_Printf(LOG_ERROR, "ClientManager: "
				"Client for interface %s died. Restarting",
				ent->name);
			ClientManager_RunClient(ent);
		}
		ent = ent->next;
	}
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

	ifaces.first = NULL;
	ifaces.last = NULL;

	while (abort_mon == 0) {
		DIR *dir = opendir(sysfs_net);
		if (dir == NULL) {
			sleep(2);
			continue;
		}
		ClientManager_CleanupInit();
		struct dirent *dp;
		while ((dp = readdir(dir)) != NULL) {
			int fd;
			char name[PATH_MAX];
			struct iface_entry *ent;

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

			if ((ent = ClientManager_FindInterface(dp->d_name)) == NULL) {
				ClientManager_AddInterface(dp->d_name);
			} else {
				ent->keep = 1;
			}
		}
		closedir(dir);
		ClientManager_Cleanup();
		sleep(POLL_INTERVAL);
		ClientManager_CheckClients();
	}
	ClientManager_CleanupInit();
	ClientManager_Cleanup();
	return NULL;
}

/**
 * ClientManager_Start() -- Starts the network monitoring
 * thread
 */
void
ClientManager_Start()
{
	mainpid = getpid();
	context = talloc_new(NULL);
	if (context == NULL) {
		Log_Printf(LOG_ERROR, "ClientManager: talloc_new() failed!");
		return;
	}
#ifdef DEBUG
	talloc_set_name(context, "ClientManager");
#endif

	abort_mon = 0;

	DeviceList_Init();

	if (pthread_create(&monthread, NULL, ClientManager_MonitorInterfaces, NULL) != 0) {
		Log_Printf(LOG_ERROR, "ClientManager: pthread_create() failed");
	}
}

/**
 * ClientManager_Stop() -- Stops the network monitoring thread
 */
void
ClientManager_Stop()
{
	/* wait for ClientManager to exit */
	abort_mon = 1;
	pthread_join(monthread, NULL);

	/* free device list */
	DeviceList_Destroy();

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
	struct iface_entry *iface;
	FIND_INTERFACE(iface, iface_name, return -1);
	LOCK_INTERFACE(iface, iface_name, return -1);
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
	struct iface_entry *iface;

	/*
	 * this function may be called by the destructors
	 * when destroying the device list on a child process
	 * so we just return success on that case
	 */
	if (getpid() != mainpid) {
		return 0;
	}

	FIND_INTERFACE(iface, iface_name, return 0);
	LOCK_INTERFACE(iface, iface_name, return 0);
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
	struct iface_entry *iface;
	IXML_Document *doc = NULL;
	FIND_INTERFACE(iface, iface_name, return -1);
	LOCK_INTERFACE(iface, iface_name, return -1);
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

