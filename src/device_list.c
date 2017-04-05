/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* $Id: device_list.c 265 2006-08-27 17:53:14Z r3mi $
 *
 * DeviceList : List of UPnP Devices
 * This file is part of avmount.
 *
 * (C) Copyright 2016 Fernando Rodriguez
 * (C) Copyright 2005 Rémi Turboult <r3mi@users.sourceforge.net>
 *
 * Part derived from libupnp example (upnp/sample/tvctrlpt/upnp_tv_ctrlpt.c)
 * Copyright (c) 2000-2003 Intel Corporation
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

#include "device_list.h"
#include "content_dir.h"
#include "device.h"
#include "upnp_util.h"
#include "log.h"
#include "service.h"
#include "talloc_util.h"
#include "linkedlist.h"

#include <stdbool.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <upnp/upnp.h>
#include <upnp/upnptools.h>

#ifdef HAVE_MALLOC_TRIM
#	include <malloc.h>
#endif

// How often to check advertisement and subscription timeouts for devices
static const unsigned int CHECK_SUBSCRIPTIONS_TIMEOUT = 30; // in seconds


static int WorkerThreadAbort = 0;
static pthread_t WorkerThread;
static pthread_cond_t WorkerThreadSignal;
static pthread_mutex_t WorkerThreadSignalMutex;

static char* g_ssdp_target = CONTENT_DIR_SERVICE_TYPE;


/*
 * Mutex for protecting the global device list
 * in a multi-threaded, asynchronous environment.
 * All functions should lock this mutex before reading
 * or writing the device list. 
 */
static pthread_mutex_t DeviceListMutex = PTHREAD_MUTEX_INITIALIZER;


/*
 * The global device list
 */
LISTABLE_TYPE(DeviceNode,
	char*    deviceId; // as reported by the discovery callback
	char*    descLocation;
	Device*  d;
	int      expires;
);


//
// TBD XXX TBD
// TBD to replace with Hash Table for better performances XXX
// TBD XXX TBD
//
LIST_DECLARE_STATIC(GlobalDeviceList);


/*****************************************************************************
 * GetDeviceNodeFromName
 *
 * Description: 
 *       Given a device name, returns the pointer to the device node.
 *       Note that this function is not thread safe.  It must be called 
 *       from a function that has locked the global device list.
 *
 * @param name 	the device name
 *
 *****************************************************************************/

static DeviceNode*
GetDeviceNodeFromName (const char* name, bool log_error)
{
	if (name) {
		DeviceNode* devnode;
		LIST_FOREACH(DeviceNode*, devnode, &GlobalDeviceList) {
			if (devnode && devnode->d) {
				if (!strcmp (talloc_get_name (devnode->d), name)) {
					return devnode;
				}
			}
		}
	}
	if (log_error)
		Log_Printf (LOG_ERROR, "Error finding Device named %s", NN(name));
	return NULL;
}

static DeviceNode*
GetDeviceListNodeFromId (const char* deviceId)
{
	if (deviceId) {
		DeviceNode *devnode;
		LIST_FOREACH(DeviceNode*, devnode, &GlobalDeviceList) {
			if (devnode && devnode->deviceId && 
				strcmp (devnode->deviceId, deviceId) == 0) {
				return devnode; // ---------->
			}
		}
	}
	return 0;
}

static Service*
GetService (const char* s, enum GetFrom from) 
{
	DeviceNode *devnode;
	LIST_FOREACH(DeviceNode*, devnode, &GlobalDeviceList) {
		if (devnode) {
			Service* const serv = Device_GetServiceFrom 
				(devnode->d, s, from, false);
			if (serv) 
				return serv; // ---------->
		}
	}
	Log_Printf (LOG_ERROR, "Can't find service matching %s in device list",
		    NN(s));
	return NULL;
}


/*****************************************************************************
 * make_device_name
 *
 * Generate a unique device name.
 * Must be deallocated with "talloc_free".
 *****************************************************************************/
static char*
make_device_name (void* talloc_context, const char* base)
{
  if (base == 0 || *base == '\0')
    base = "unnamed";
  char* name = String_CleanFileName (talloc_context, base);
  
  char* res = name;
  int i = 1;
  while (GetDeviceNodeFromName (res, false)) {
    if (res != name)
      talloc_free (res);
    res = talloc_asprintf (talloc_context, "%s_%d", name, ++i);
  }
  if (res != name)
    talloc_free (name);
  
  return res;
}


/*****************************************************************************
 * DeviceList_RemoveDevice
 *
 * Description: 
 *       Remove a device from the global device list.
 *
 * Parameters:
 *   deviceId -- The Unique Device Name for the device to remove
 *
 *****************************************************************************/
int
DeviceList_RemoveDevice (const char* deviceId)
{
	int rc = UPNP_E_SUCCESS;

	pthread_mutex_lock(&DeviceListMutex);
	
	DeviceNode* const devnode = GetDeviceListNodeFromId (deviceId);
	if (devnode) {
		LIST_REMOVE(devnode);
		talloc_free (devnode);
	} else {
		rc = UPNP_E_INVALID_DEVICE;
	}
	
	pthread_mutex_unlock(&DeviceListMutex);
	
	return rc;
}


/*****************************************************************************
 * DeviceList_RemoveAll
 *
 * Description: 
 *       Remove all devices from the global device list.
 *
 * Parameters:
 *   None
 *
 *****************************************************************************/
static int
DeviceList_RemoveAll (void)
{
	pthread_mutex_lock( &DeviceListMutex );
	DeviceNode *devnode;
	LIST_FOREACH_SAFE(DeviceNode*, devnode, &GlobalDeviceList, {
		LIST_REMOVE(devnode);
		talloc_free(devnode);
	});
	pthread_mutex_unlock( &DeviceListMutex );
	return UPNP_E_SUCCESS;
}


/*****************************************************************************
 * HandleEvent
 *
 * Description: 
 *       Handle a UPnP event that was received.  Process the event and update
 *       the appropriate service state table.
 *
 * Parameters:
 *   sid -- The subscription id for the event
 *   eventkey -- The eventkey number for the event
 *   changes -- The DOM document representing the changes
 *
 *****************************************************************************/
static void
HandleEvent (Upnp_SID sid,
	     int eventkey,
	     IXML_Document* changes )
{
  pthread_mutex_lock( &DeviceListMutex );
  
  Log_Printf (LOG_DEBUG, "Received Event: %d for SID %s", eventkey, NN(sid));
  Service* const serv = GetService (sid, FROM_SID);
  if (serv) 
    Service_UpdateState (serv, changes);
  
  pthread_mutex_unlock( &DeviceListMutex );
}


/*****************************************************************************
 * AddDevice
 *
 * Description: 
 *       If the device is not already included in the global device list,
 *       add it.  Otherwise, update its advertisement expiration timeout.
 *
 * Parameters:
 *   descLocation -- The location of the description document URL
 *   expires -- The expiration time for this advertisement
 *
 *****************************************************************************/


static void
AddDevice (const char *iface, const char* deviceId,
	   const char* descLocation,
	   int expires, UpnpClient_Handle client_handle)
{
	pthread_mutex_lock (&DeviceListMutex);

	DeviceNode* devnode = GetDeviceListNodeFromId(deviceId);
	
	if (devnode) {
		// The device is already there, so just update 
		// the advertisement timeout field
		Log_Printf (LOG_DEBUG, 
			    "AddDevice Id=%s already exists, update only",
			    NN(deviceId));
		devnode->expires = expires;

		/* if this is a local service but we're connected to it through
		 * a non loopback interface remove the old one and add this one.
		 * In other words, give preference to local connections. */
		if (strstr(descLocation, "127.0.0.1")) {
			if (strcmp(descLocation, devnode->descLocation)) {
				pthread_mutex_unlock(&DeviceListMutex);
				DeviceList_RemoveDevice(devnode->deviceId);
				devnode = NULL;
				pthread_mutex_lock(&DeviceListMutex);
			}
		}
	}

	if (devnode == NULL) {
		// Else create a new device
		Log_Printf (LOG_DEBUG, "AddDevice try new device Id=%s", 
			    NN(deviceId));
		
		// *unlock* before trying to download the Device Description 
		// Document, which can take a long time in some error cases 
		// (e.g. timeout if network problems)
		pthread_mutex_unlock (&DeviceListMutex);

		if (descLocation == NULL) {
			Log_Printf (LOG_ERROR, 
				    "NULL description doc. URL device Id=%s", 
				    NN(deviceId));
			return; // ---------->
		}

		char* descDocText = NULL;
		char content_type [LINE_SIZE] = "";
		int rc = UpnpDownloadUrlItem (descLocation, &descDocText, 
					      content_type);
		if (rc != UPNP_E_SUCCESS) {
			Log_Printf (LOG_ERROR,
				    "Error obtaining device description from "
				    "url '%s' : %d (%s)", descLocation, rc, 
				    UpnpGetErrorMessage (rc));
			if (rc/100 == UPNP_E_NETWORK_ERROR/100) {
				Log_Printf (LOG_ERROR, "Check device network "
					    "configuration (firewall ?)");
			}
			return; // ---------->
		} 
		if (strncasecmp (content_type, "text/xml", 8)) {
			// "text/xml" is specified in UPnP Device Architecture
			// v1.0 -- however don't abort if incorrect because
			// some broken UPnP device send other MIME types 
			// (e.g. application/octet-stream).
			Log_Printf (LOG_ERROR, "Device description at url '%s'"
				    " has MIME '%s' instead of XML ! "
				    "Trying to parse anyway ...", 
				    descLocation, content_type);
		}

		void* context = NULL; // TBD should be parent talloc TBD XXX
		
		devnode = talloc (context, DeviceNode);
		if (devnode == NULL) {
			Log_Printf(LOG_ERROR, "Could not allocate device node");
			return;
		}

		devnode->d = Device_Create (devnode, client_handle,
					    descLocation, deviceId,
					    descDocText,
							iface);
		free (descDocText);
		descDocText = NULL;

		if (devnode->d == NULL) {
			Log_Printf (LOG_ERROR, "Can't create Device Id=%s", 
				    NN(deviceId));
			talloc_free (devnode);
			return; // ---------->
		} else {
			// If SSDP target specified, check that the device
			// matches it.
			if (strstr (g_ssdp_target, ":service:")) {
				const Service* serv = Device_GetServiceFrom 
					(devnode->d, g_ssdp_target, 
					 FROM_SERVICE_TYPE, false);
				if (serv == NULL) {
					Log_Printf (LOG_DEBUG,
						    "Discovered device Id=%s "
						    "has no '%s' service : "
						    "forgetting", NN(deviceId),
						    g_ssdp_target);
					talloc_free (devnode);
					return; // ---------->
				}
			}

			// Relock the device list (and check that the same
			// device has not already been added by another thread
			// while the list was unlocked)
			pthread_mutex_lock (&DeviceListMutex);
			if (GetDeviceListNodeFromId(deviceId) != NULL) {
				// Delete extraneous device descriptor. Note:
				// service subscription is not yet done, so 
				// the Service destructors will not unsubscribe
				talloc_free (devnode);
			} else {
				devnode->deviceId = talloc_strdup(devnode, deviceId);
				devnode->descLocation = talloc_strdup(devnode, descLocation);
				if (devnode->deviceId == NULL || devnode->descLocation == NULL) {
					Log_Printf(LOG_ERROR, "AddDevice() failed. Out of memory");
					pthread_mutex_unlock (&DeviceListMutex);
					talloc_free (devnode);
					return;
				}
				devnode->expires = expires;
				
				// Generate a unique, friendly, name 
				// for this device
				const char* base = Device_GetDescDocItem 
					(devnode->d, "friendlyName", true);
				char* name = make_device_name (NULL, base);
				talloc_set_name (devnode->d, "%s", name);
				talloc_free (name);
				
				Log_Printf (LOG_INFO, 
					    "Add new device : Name='%s' "
					    "Id='%s' descURL='%s'", 
					    NN(talloc_get_name (devnode->d)), 
					    NN(deviceId), descLocation);

				// Insert the new device node in the list
				LIST_ADD(&GlobalDeviceList, devnode);

				Device_SusbcribeAllEvents (iface, devnode->d);
			}
		}
	}
	pthread_mutex_unlock (&DeviceListMutex);
}
  


/*****************************************************************************
 * EventHandlerCallback
 *
 * Description: 
 *       The callback handler registered with the SDK while registering
 *       the control point.  Detects the type of callback, and passes the 
 *       request on to the appropriate function.
 *
 * Parameters:
 *   event_type -- The type of callback event
 *   event -- Data structure containing event data
 *   cookie -- Optional data specified during callback registration
 *
 *****************************************************************************/
int
DeviceList_EventHandlerCallback (const char *iface_name, Upnp_EventType event_type,
		      void* event, void* cookie)
{

	// Create a working context for temporary strings
	void* const tmp_ctx = talloc_new (NULL);
	if (tmp_ctx == NULL) {
		Log_Printf(LOG_ERROR,  "DeviceList_EventHandlerCallback() -- "
			"Could not allocate temp talloc context!");
		return 0;
	}

	UpnpClient_Handle handle = *((UpnpClient_Handle*) cookie);

	Log_Printf(LOG_DEBUG, "Created temp context %p", tmp_ctx);
	Log_Printf(LOG_DEBUG, "Received event from %s (handle=%lu)",
		iface_name, (unsigned long) handle);
	Log_Print (LOG_DEBUG, UpnpUtil_GetEventString (tmp_ctx, event_type, 
						       event));
	
	switch ( event_type ) {
		/*
		 * SSDP Stuff 
		 */
	case UPNP_DISCOVERY_ADVERTISEMENT_ALIVE:
	case UPNP_DISCOVERY_SEARCH_RESULT:
	{
		const struct Upnp_Discovery* const e =
			(struct Upnp_Discovery*) event;
		
		if (e->ErrCode != UPNP_E_SUCCESS) {
			Log_Printf (LOG_ERROR, 
				    "Error in Discovery Callback -- %d", 
				    e->ErrCode);	
		} else if (e->ServiceType != NULL) {
			/* If this is an adverstisement for a device with a
			 * ContentDirectory service then attempt to add the
			 * device.
			 */
			const char * const type = "urn:schemas-upnp-org:service:ContentDirectory";
			if (!strncmp(type, e->ServiceType, sizeof(type) - 1)) {
				if (e->DeviceId && e->DeviceId[0]) {
					Log_Printf (LOG_DEBUG,
						"Discovery : device type '%s' "
						"OS '%s' at URL '%s'", NN(e->DeviceType),
						NN(e->Os), NN(e->Location));
					AddDevice (iface_name, e->DeviceId, e->Location, e->Expires, handle);
					/* Log_Printf (LOG_DEBUG, "Discovery: "
						"DeviceList after AddDevice = \n%s",
						DeviceList_GetStatusString (tmp_ctx)); */
				}
			}
		}
		break;
	}
    
	case UPNP_DISCOVERY_SEARCH_TIMEOUT:
		/*
		 * Nothing to do here... 
		 */
		break;
		
	case UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE:
    	{
		struct Upnp_Discovery* e = (struct Upnp_Discovery*) event;
      
		if (e->ErrCode != UPNP_E_SUCCESS ) {
			Log_Printf (LOG_ERROR,
				    "Error in Discovery ByeBye Callback -- %d",
				    e->ErrCode );
		}
		
		Log_Printf (LOG_DEBUG, "Received ByeBye for Device: %s",
			    e->DeviceId );
		DeviceList_RemoveDevice (e->DeviceId);
		
		Log_Printf (LOG_DEBUG, "DeviceList after byebye: \n%s",
			    DeviceList_GetStatusString (tmp_ctx));
		break;
	}
    
	/*
	 * SOAP Stuff 
	 */
	case UPNP_CONTROL_ACTION_COMPLETE:
    	{
		struct Upnp_Action_Complete* e = 
			(struct Upnp_Action_Complete*) event;
      
		if (e->ErrCode != UPNP_E_SUCCESS ) {
			Log_Printf (LOG_ERROR,
				    "Error in  Action Complete Callback -- %d",
				    e->ErrCode );
		}
		
		/*
		 * No need for any processing here, just print out results.  
		 * Service state table updates are handled by events. 
		 */
		
		break;
	}
	
	case UPNP_CONTROL_GET_VAR_COMPLETE:
		/*
		 * Not used : deprecated
		 */
		Log_Printf (LOG_WARNING, 
			    "Deprecated Get Var Complete Callback");
		break;
    
		/*
		 * GENA Stuff 
		 */
	case UPNP_EVENT_RECEIVED:
	{
		struct Upnp_Event* e = (struct Upnp_Event*) event;
		HandleEvent (e->Sid, e->EventKey, e->ChangedVariables);
		break;
	}
	
	case UPNP_EVENT_SUBSCRIBE_COMPLETE:
	case UPNP_EVENT_UNSUBSCRIBE_COMPLETE:
	case UPNP_EVENT_RENEWAL_COMPLETE:
	{
		struct Upnp_Event_Subscribe* e =
			(struct Upnp_Event_Subscribe*) event;
      
		if (e->ErrCode != UPNP_E_SUCCESS ) {
			Log_Printf (LOG_ERROR,
				    "Error in Event Subscribe Callback -- %d",
				    e->ErrCode );
		} else {
#ifdef HAVE_UPNPSTRING_PUBLISHERURL
			Log_Printf (LOG_DEBUG,
				    "Received Event Renewal for eventURL %s",
				    NN(UpnpString_get_String(e->PublisherUrl)));
#else
			Log_Printf (LOG_DEBUG,
				    "Received Event Renewal for eventURL %s",
				    NN(e->PublisherUrl));
#endif

			pthread_mutex_lock (&DeviceListMutex);

#ifdef HAVE_UPNPSTRING_PUBLISHERURL
			Service* const serv = GetService (UpnpString_get_String(e->PublisherUrl),
							  FROM_EVENT_URL);
#else
			Service* const serv = GetService (e->PublisherUrl,
							  FROM_EVENT_URL);
#endif
			if (serv) {
				if (event_type == 
				    UPNP_EVENT_UNSUBSCRIBE_COMPLETE)
					Service_SetSid (serv, NULL);
				else			      
					Service_SetSid (serv, e->Sid);
			}
			pthread_mutex_unlock (&DeviceListMutex);
		}
		break;
	}
    
	case UPNP_EVENT_AUTORENEWAL_FAILED:
	case UPNP_EVENT_SUBSCRIPTION_EXPIRED:
    	{
		struct Upnp_Event_Subscribe* e = 
			(struct Upnp_Event_Subscribe*) event;

#ifdef HAVE_UPNPSTRING_PUBLISHERURL
		Log_Printf (LOG_DEBUG, "Renewing subscription for eventURL %s",
			    NN(UpnpString_get_String(e->PublisherUrl)));
#else
		Log_Printf (LOG_DEBUG, "Renewing subscription for eventURL %s",
			    NN(e->PublisherUrl));
#endif
     
		pthread_mutex_lock (&DeviceListMutex);
      
#ifdef HAVE_UPNPSTRING_PUBLISHERURL
		Service* const serv = GetService (UpnpString_get_String(e->PublisherUrl),
						  FROM_EVENT_URL);
#else
		Service* const serv = GetService (e->PublisherUrl,
						  FROM_EVENT_URL);
#endif
		if (serv) 
			Service_SubscribeEventURL (iface_name, serv);
		
		pthread_mutex_unlock (&DeviceListMutex);
		
		break;
	}
    
	/*
	 * ignore these cases, since this is not a device 
	 */
	case UPNP_EVENT_SUBSCRIPTION_REQUEST:
	case UPNP_CONTROL_GET_VAR_REQUEST:
	case UPNP_CONTROL_ACTION_REQUEST:
		break;
	}
  	
  	// Delete all temporary strings
	Log_Printf(LOG_DEBUG, "Deleting temp context: %p",
		tmp_ctx);
	talloc_free (tmp_ctx);

	return 0;
}

/*****************************************************************************
 * _DeviceList_LockDevice
 *****************************************************************************/
Device*
_DeviceList_LockDevice (const char* deviceName)
{
	Device* dev = NULL;

	Log_Printf (LOG_DEBUG, "LockDevice : device '%s'", NN(deviceName));

	// XXX coarse implementation : lock the whole device list, 
	// XXX not only the device.
	pthread_mutex_lock (&DeviceListMutex);
	
	const DeviceNode* devnode = GetDeviceNodeFromName (deviceName, true);
	if (devnode) 
		dev = devnode->d;
	if (dev == NULL)
		pthread_mutex_unlock (&DeviceListMutex);
	return dev;
}


/*****************************************************************************
 * _DeviceList_UnlockDevice
 *****************************************************************************/
inline void
_DeviceList_UnlockDevice (Device* dev)
{
	if (dev)
		pthread_mutex_unlock (&DeviceListMutex);
}

/*****************************************************************************
 * _DeviceList_LockService
 *****************************************************************************/
Service*
_DeviceList_LockService (const char* deviceName, const char* serviceType)
{
	Service* serv = NULL;

	Log_Printf (LOG_DEBUG, "LockService : device '%s' service '%s'",
		    NN(deviceName), NN(serviceType));

	// XXX coarse implementation : lock the whole device list, 
	// XXX not only the service.
	pthread_mutex_lock (&DeviceListMutex);
	
	const DeviceNode* devnode = GetDeviceNodeFromName (deviceName, true);
	if (devnode) 
		serv = Device_GetServiceFrom (devnode->d, serviceType, 
					      FROM_SERVICE_TYPE, true);
	if (serv == NULL)
		pthread_mutex_unlock (&DeviceListMutex);
	return serv;
}


/*****************************************************************************
 * _DeviceList_UnlockService
 *****************************************************************************/
inline void
_DeviceList_UnlockService (Service* serv)
{
	if (serv)
		pthread_mutex_unlock (&DeviceListMutex);
}

/*****************************************************************************
 * GetDevicesNames
 *****************************************************************************/
PtrArray*
DeviceList_GetDevicesNames (void* context)
{
	pthread_mutex_lock (&DeviceListMutex);

	Log_Printf (LOG_DEBUG, "GetDevicesNames");
	PtrArray* const a = PtrArray_CreateWithCapacity 
		(context, LIST_SIZE(&GlobalDeviceList));
	if (a) {
		DeviceNode *devnode;
		LIST_FOREACH(DeviceNode*, devnode, &GlobalDeviceList) {
			if (devnode) {
				const char* const name = 
					(devnode->d ? 
					 talloc_get_name (devnode->d) : NULL);
				// add pointer directly 
				// TBD no need to copy ??? XXX
				PtrArray_Append (a, (char*) NN(name)); 
			}
		}
	}
  
	pthread_mutex_unlock (&DeviceListMutex);
	
	return a;
}



/*****************************************************************************
 * DeviceList_GetStatusString
 *****************************************************************************/
char*
DeviceList_GetStatusString (void* context)
{
	char* ret = talloc_strdup (context, "");
	if (ret) {
		pthread_mutex_lock (&DeviceListMutex);
    
		// Print the universal device names for each device
		// in the global device list
		DeviceNode *devnode;
		LIST_FOREACH(DeviceNode*, devnode, &GlobalDeviceList) {
			if (devnode) {
				const char* const name =
					(devnode->d ? talloc_get_name(devnode->d) : 0);
				ret = talloc_asprintf_append (ret, " %-20s -- %s\n",
					NN(name), NN(devnode->deviceId));
			}
		}
		pthread_mutex_unlock (&DeviceListMutex);
	}
	return ret;
}



/*****************************************************************************
 * DeviceList_GetDeviceStatusString
 *****************************************************************************/
char*
DeviceList_GetDeviceStatusString (void* context, const char* deviceName,
				  bool debug) 
{
  char* ret = NULL;
  
  pthread_mutex_lock (&DeviceListMutex);
  
  DeviceNode* devnode = GetDeviceNodeFromName (deviceName, true);
  if (devnode) { 
	  char* s = Device_GetStatusString (devnode->d, NULL, debug);
    ret = talloc_asprintf (context, 
			   "Device \"%s\" (expires in %d seconds)\n%s",
			   deviceName, devnode->expires, s);
    talloc_free (s);
  } 
  
  pthread_mutex_unlock (&DeviceListMutex);
  
  return ret;
}


/*****************************************************************************
 * DeviceList_VerifyTimeouts
 *
 * Description: 
 *       Checks the advertisement  each device
 *        in the global device list.  If an advertisement expires,
 *       the device is removed from the list.  If an advertisement is about to
 *       expire, a search request is sent for that device.  
 *
 * Parameters:
 *    incr -- The increment to subtract from the timeouts each time the
 *            function is called.
 *
 *****************************************************************************/
static void
DeviceList_VerifyTimeouts (int incr)
{
	pthread_mutex_lock (&DeviceListMutex);

	DeviceNode *devnode;
	LIST_FOREACH_SAFE(DeviceNode*, devnode, &GlobalDeviceList, {
		devnode->expires -= incr;
		if (devnode->expires <= 0) {
			char *descDocText;
			const char *descDocURL = Device_GetDescDocURL(devnode->d);
			char content_type[LINE_SIZE];
			int rc = UpnpDownloadUrlItem (descDocURL, &descDocText,
				content_type);
			if (rc != UPNP_E_SUCCESS) {
				/*
				 * This advertisement has expired, so we should remove the device
				 * from the list
				 */
				Log_Printf (LOG_DEBUG, "Remove expired device Id=%s", devnode->deviceId);
				LIST_REMOVE(devnode);
				talloc_free(devnode);
			} else {
				/* TODO: Should we check that descDocText hasn't changed
				 * and update it if it has?
				 */
				free(descDocText);
				devnode->expires = 60;
			}
		}
	});
	pthread_mutex_unlock (&DeviceListMutex);
}


/**
 * DeviceList_GC() -- Garbage Collect Caches
 */
static void
DeviceList_GC()
{
	pthread_mutex_lock (&DeviceListMutex);
	DeviceNode *devnode;
	LIST_FOREACH(DeviceNode*, devnode, &GlobalDeviceList) {
		Device_GC(devnode->d);
	}
	pthread_mutex_unlock (&DeviceListMutex);
#ifdef HAVE_MALLOC_TRIM
	malloc_trim(4096);
#endif
}


/*****************************************************************************
 * DeviceList_BackgroundWorker()
 *
 * Description: 
 *       Function that runs in its own thread and performs the following
 *       background tasks:
 *
 *       - Monitor subscription timeouts for devices in the global device list
 *       - Periodically garbage collect caches
 *
 * Parameters:
 *    None
 *
 *****************************************************************************/
static void*
DeviceList_BackgroundWorker(void* arg)
{
	struct timespec ts;

	while (!WorkerThreadAbort) {
		pthread_mutex_lock(&WorkerThreadSignalMutex);
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += CHECK_SUBSCRIPTIONS_TIMEOUT;
		pthread_cond_timedwait(&WorkerThreadSignal,
			&WorkerThreadSignalMutex, &ts);
		pthread_mutex_unlock(&WorkerThreadSignalMutex);

		if (!WorkerThreadAbort) {
			DeviceList_VerifyTimeouts(CHECK_SUBSCRIPTIONS_TIMEOUT);
			DeviceList_GC();
		}
	}
	return NULL;
}


/*****************************************************************************
 * DeviceList_Init
 *****************************************************************************/
int
DeviceList_Init()
{
	/* Initialize linked list */
	LIST_INIT(&GlobalDeviceList);

	/* Makes the XML parser more tolerant to malformed text */
	ixmlRelaxParser ('?');

	pthread_mutex_init(&WorkerThreadSignalMutex, NULL);
	pthread_cond_init(&WorkerThreadSignal, NULL);

	/* Start background worker */
	if (pthread_create(&WorkerThread, NULL, DeviceList_BackgroundWorker, NULL) == -1) {
		Log_Print(LOG_ERROR, "DeviceList_Init() -- pthread_create() failed.");
		return -1;
	}

	return 0;
}

void
DeviceList_Destroy()
{
	Log_Printf(LOG_DEBUG, "DeviceList_Destroy() running");

	/* Abort and wait for the worker thread to exit */
	pthread_mutex_lock(&WorkerThreadSignalMutex);
	WorkerThreadAbort = 1;
	pthread_cond_signal(&WorkerThreadSignal);
	pthread_mutex_unlock(&WorkerThreadSignalMutex);
	pthread_join(WorkerThread, NULL);

	/* destroy all devices */
	DeviceList_RemoveAll();
}

/**
 * DeviceList_Suspend() -- Locks all mutexes. This is used by clientmgr.c
 * to make sure that the device list is in a known state before forking.
 * Otherwise if we fork while the worker thread mutex is locked the child
 * will deadlock when it tries to destroy the device list during startup.
 */
void
DeviceList_Suspend()
{
	pthread_mutex_lock(&WorkerThreadSignalMutex);
	pthread_mutex_lock(&DeviceListMutex);
}

/**
 * DeviceList_Resume() -- Unlocks all mutexes.
 */
void
DeviceList_Resume()
{
	pthread_mutex_unlock(&WorkerThreadSignalMutex);
	pthread_mutex_unlock(&DeviceListMutex);
}
