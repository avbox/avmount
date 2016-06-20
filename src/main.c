/* $Id: main.c
 *
 * Program entry point.
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

#ifdef HAVE_CONFIG_H
#	include "../config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/statfs.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curl/curl.h>
#include <fuse.h>
#include <libgen.h>

#include "talloc_util.h"
#include "device_list.h"
#include "log.h"
#include "upnp_util.h"
#include "string_util.h"
#include "djfs.h"
#include "content_dir.h"
#include "charset.h"
#include "minmax.h"
#include "clientmgr.h"
#include "stream.h"
#include "fuse_fs.h"

#define DEFAULT_SEARCH_HISTORY_SIZE  (100)

#define STRINGIZE2(x) #x
#define STRINGIZE(x) STRINGIZE2(x)


/*****************************************************************************
 * @fn 		stdout_print
 * @brief 	Output log messages.
 *
 * Parameters:
 * 	See Log_PrintFunction prototype.
 *
 *****************************************************************************/

static FILE *logf = NULL;

static void
__stdout_print (FILE* f, Log_Level level, const char* msg)
{
	Log_BeginColor (level, f);
	switch (level) {
	case LOG_ERROR:		fprintf (f, "[E] "); break;
	case LOG_WARNING:	fprintf (f, "[W] "); break;
	case LOG_INFO:		fprintf (f, "[I] "); break;
	case LOG_DEBUG:		fprintf (f, "[D] "); break;
	default:
		fprintf (f, "[%d] ", (int) level);
		break;
	}

	// Convert message to display charset, and print
	Charset_PrintString (CHARSET_FROM_UTF8, msg, f);
	Log_EndColor (level, f);
	fprintf (f, "\n");
	fflush(f);
}

static void
stdout_print(Log_Level level, const char *msg)
{
	__stdout_print(logf, level, msg);
	if (logf != stdout && level <= LOG_INFO) {
		__stdout_print(stdout, level, msg);
	}
}

/*****************************************************************************
 * Usage
 *****************************************************************************/

#ifdef HAVE_UPNP_SETLOGFILENAMES
#    define DEBUG_DEFAULT_LEVELS	"upnpall,debug,fuse,leak"
#else
#    define DEBUG_DEFAULT_LEVELS	"debug,fuse,leak"
#endif

static const char* const FUSE_ALLOWED_OPTIONS = \
	"    default_permissions    enable permission checking by kernel\n"
	"    allow_other            allow access to other users\n"
	"    allow_root             allow access to root\n"
	"    kernel_cache           cache files in kernel\n"
#if HAVE_FUSE_O_NONEMPTY
	"    nonempty               allow mounts over non-empty file/dir\n"
#endif
	"    fsname=NAME            set filesystem name in mtab\n";

static void
usage (FILE* stream, const char* progname)
{
  fprintf(stream, "usage: %s [options] mountpoint\n", progname);
  fprintf(stream,
     "\n"
     "Options:\n"
     "    -h or --help           print this help, then exit\n"
     "    --version              print version number, then exit\n"
     "    --lobind               bind UPnP clients to all interfaces, including loopback\n"
     "    -o [options]           mount options (see below)\n"
     "    -p [port]              specify port number\n"
     "    -d[levels]             enable debug output (implies -f)\n"
     "    -f                     foreground operation (default: daemonized)\n"
     "\n"
     "Mount options (one or more comma separated options) :\n"
#if HAVE_CHARSET
     "    iocharset=<charset>    filenames encoding (default: environment)\n"
#endif
     "    playlists              use playlists for AV files, instead of plain files\n"
     "    search_history=<size>  number of remembered searches (default: %i)\n"
     "                           (set to 0 to disable search)\n"
     "\n", DEFAULT_SEARCH_HISTORY_SIZE);
  fprintf(stream,
     "See FUSE documentation for the following mount options:\n%s",
     FUSE_ALLOWED_OPTIONS);
  fprintf(stream,
     "\nDebug levels are one or more comma separated words :\n"
#ifdef HAVE_UPNP_SETLOGFILENAMES
     "    upnperr, upnpall : increasing level of UPnP traces\n"
#endif
     "    error, warn, info, debug : increasing level of " PACKAGE " traces\n"
     "    fuse : activates FUSE traces\n"
     "    leak, leakfull : enable talloc leak reports at exit\n"
     "'-d' alone defaults to '" DEBUG_DEFAULT_LEVELS "' i.e. all traces.\n"
     "\n"
     "Report bugs to <" PACKAGE_BUGREPORT ">.\n");

  exit (EXIT_SUCCESS); // ---------->
}


static void
bad_usage (const char* progname, ...)
{
	fprintf(stderr, "%s: ", progname);
	va_list ap;
	va_start (ap, progname);
	const char* const format = va_arg (ap, const char*);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fprintf (stderr, "\nTry '%s --help' for more information.\n",
		 progname);
	exit (EXIT_FAILURE); // ---------->
}


static void
version (FILE* stream, const char* progname)
{
	char * const avmount = "avmount";
	char *pn = strdup(progname);
	if (pn == NULL) {
		pn = avmount;
	}
#ifdef GIT_COMMIT
	char commit[13];
	commit[(sizeof(commit) / sizeof(char)) - 1] = '\0';
	memcpy(commit, STRINGIZE(GIT_COMMIT), sizeof(commit) - sizeof(char));
	fprintf(stream,
		"%s (" PACKAGE ") " VERSION "-%s (" STRINGIZE(GIT_BRANCH) " branch)\n",
		basename(pn), commit);
#else
	fprintf(stream,
		 "%s (" PACKAGE ") " VERSION "\n",
		 basename(pn));
#endif

	fprintf (stream, "Copyright (C) 2016 Fernando Rodriguez\n");
	fprintf (stream, "Copyright (C) 2005 Rémi Turboult\n\n");

	/* Build options */
#ifdef DEBUG
#	define OPT_DEBUG "Yes"
#else
#	define OPT_DEBUG "No"
#endif
#ifdef ENABLE_IPV6
#	define OPT_IPV6 "Yes"
#else
#	define OPT_IPV6 "No"
#endif

	fprintf(stream, "Compile Options\n");
	fprintf(stream, "===============\n\n");
	fprintf(stream, "FUSE version......%i.%i\n",
		FUSE_MAJOR_VERSION,
		FUSE_MINOR_VERSION);

#ifdef UPNP_VERSION_STRING
	fprintf(stream, "libupnp version...%s\n", UPNP_VERSION_STRING);
#endif

	fprintf(stream, "libcurl version...%i.%i.%i\n",
		LIBCURL_VERSION_MAJOR,
		LIBCURL_VERSION_MINOR,
		LIBCURL_VERSION_PATCH);
	fprintf(stream, "Debug.............%s\n", OPT_DEBUG);
	fprintf(stream, "IPv6..............%s\n\n", OPT_IPV6);

	fputs ("\
This is free software. You may redistribute copies of it under the terms of\n\
the GNU General Public License <http://www.gnu.org/licenses/gpl.html>.\n\
There is NO WARRANTY, to the extent permitted by law.\n\
\n", stream);

	if (pn != avmount) {
		free(pn);
	}

	exit(EXIT_SUCCESS); // ---------->
}


/*****************************************************************************
 * Main
 *****************************************************************************/

int
main (int argc, char *argv[])
{
	int rc, lobind = 0;
	bool background = true;

	/* Create a working context for temporary strings */
	void* const tmp_ctx = talloc_autofree_context();

	/*
	 * Check if the -l argument was provided and if so, then open
	 * the log file for appending
	 */
	for (rc = 0; rc < argc; rc++) {
		if (!strcmp(argv[rc], "-l")) {
			if ((rc + 1) >= argc) {
				bad_usage(argv[0], "option -l must be followed by a filename");
				return -1;
			}
			logf = fopen(argv[rc + 1], "a+");
			if (logf == NULL) {
				fprintf(stderr, "Could not open %s", argv[rc + 1]);
				return -1;
			}
			fprintf(logf, "Begin logging...\n");
			fflush(logf);
		}
	}

	/*
	 * if no log file was provided then use stdout
	 */
	if (logf == NULL) {
		logf = stdout;
	}

	rc = Log_Initialize (stdout_print);
	if (rc != 0) {
		fprintf (stderr, "%s : Error initialising Logger", argv[0]);
		exit (rc); // ---------->
	}
	Log_Colorize (true);
#ifdef HAVE_UPNP_SETLOGFILENAMES
	SetLogFileNames ("/dev/null", "/dev/null");
#endif

	/*
	 * Handle options
	 */
	int port = 0;
	char* charset = NULL;
	DJFS_Flags djfs_flags = DJFS_SHOW_METADATA;
	size_t search_history_size = DEFAULT_SEARCH_HISTORY_SIZE;

#ifdef DEBUG
	djfs_flags |= DJFS_SHOW_DEBUG;
#endif

	FuseFS_SetOpt(argv[0]);

	int opt = 1;
	char* o;
	while ((o = argv[opt++])) {
		if (strcmp (o, "-h") == 0 || strcmp (o, "--help") == 0) {
			usage (stdout, argv[0]); // ---------->

		} else if (strcmp (o, "-l") == 0) {
			opt++;
			continue;

		} else if (strcmp (o, "--version") == 0) {
			version (stdout, argv[0]); // ---------->

		} else if (strcmp(o, "--lobind") == 0) {
			lobind = 1;

		} else if (strcmp(o, "-f") == 0) {
			background = false;

		} else if (*o != '-') {
			// mount point
			FuseFS_SetOpt(o);

		} else if ( strcmp (o, "-o") == 0 && argv[opt] ) {
			// Parse mount options
			const char* const options = argv[opt++];
			char* options_copy = strdup (options);
			char* tokptr = 0;
			char* s;
			for (s = strtok_r (options_copy, ",", &tokptr);
			     s != NULL;
			     s = strtok_r (NULL, ",", &tokptr)) {
				if (strncmp (s,"playlists", 5) == 0) {
					djfs_flags |= DJFS_USE_PLAYLISTS;
#if HAVE_CHARSET
				} else if (strncmp(s, "iocharset=", 10) == 0) {
					charset = talloc_strdup(tmp_ctx, s+10);
#endif
				} else if (strncmp(s, "search_history=", 15)
					   == 0) {
					search_history_size = atoi (s+15);
				} else if (strncmp(s, "fsname=", 7) == 0 ||
					   strstr (FUSE_ALLOWED_OPTIONS, s)) {
					FuseFS_SetOpt("-o");
					FuseFS_SetOpt(talloc_strdup (tmp_ctx, s));
				} else {
					bad_usage (argv[0],
						   "unknown mount option '%s'",
						   s); // ---------->
				}
			}
			free (options_copy);
			Log_Printf (LOG_INFO, "  Mount options = %s", options);

		} else if (strncmp (o, "-d", 2) == 0) {
			background = false;

			// Parse debug levels
			const char* const levels =
				(o[2] ? o+2 : DEBUG_DEFAULT_LEVELS);
			char* levels_copy = strdup (levels);
			char* tokptr = 0;
			char* s;
			for (s = strtok_r (levels_copy, ",", &tokptr);
			     s != NULL;
			     s = strtok_r (NULL, ",", &tokptr)) {
				if (strcmp (s, "leak") == 0) {
#ifdef DEBUG
					talloc_enable_leak_report();
#endif
				} else if (strcmp (s, "leakfull") == 0) {
#ifdef DEBUG
					talloc_enable_leak_report_full();
#endif
				} else if (strcmp (s, "fuse") == 0) {
					/* FUSE_ARG ("-d"); */
				} else if (strcmp (s, "debug") == 0) {
					Log_SetMaxLevel (LOG_DEBUG);
				} else if (strcmp (s, "info") == 0) {
					Log_SetMaxLevel (LOG_INFO);
				} else if (strncmp (s, "warn", 4) == 0) {
					Log_SetMaxLevel (LOG_WARNING);
				} else if (strncmp (s, "error", 3) == 0) {
					Log_SetMaxLevel (LOG_ERROR);
#ifdef HAVE_UPNP_SETLOGFILENAMES
				} else if (strcmp (s, "upnperr") == 0) {
					SetLogFileNames ("/dev/stdout",
							 "/dev/null");
				} else if (strcmp (s, "upnpall") == 0) {
					SetLogFileNames ("/dev/stdout",
							 "/dev/stdout");
#endif
				} else {
					bad_usage (argv[0],
						   "unknown debug level '%s'",
						   s); // ---------->
				}
			}
			free (levels_copy);
			Log_Printf (LOG_DEBUG, "  Debug options = %s", levels);

		} else if (!strncmp(o, "-p", 2) || !strncmp(o, "--port", 6)) {
			char *pps, *port_string = (o[1] == 'p') ? o + 2 : o + 6;
			/*
			 * if the argument is not immediately followed by
			 * the value then the value is on the next argument
			 */
			if (*port_string == '\0') {
				if (opt >= argc) {
					bad_usage(argv[0],
						"port number not specified");
				} else {
					port_string = argv[opt++];
				}
			}

			/* check that the provided port number is numeric */
			for (pps = port_string; *pps != '\0'; pps++) {
				if (!isdigit(*pps)) {
					bad_usage(argv[0],
						"invalid port number: '%s'", port_string);
				}
			}

			/* convert port number to integer */
			port = atoi(port_string);

			/* check that it is a valid port number */
			if (port < 1 || port > 65535) {
				bad_usage(argv[0], "invalid port number: %i", port);
			}

			/* if a priviledged port was specified check that we
			 * have superuser rights */
			if (port <= 1024) {
				if (getuid()) {
					bad_usage(argv[0],
						"Superuser required for binding to priviledged port %i",
						port);
				}
			}

		} else {
			bad_usage (argv[0], "unrecognized option '%s'",
				   o); // ---------->
		}
	}

	/*
	 * Set charset encoding
	 */
	rc = Charset_Initialize (charset);
	if (rc) {
		Log_Printf (LOG_ERROR, "Error initialising charset='%s'",
			    NN(charset));
	}

	/*
	 * Daemonize process if necessary (must be done before UPnP
	 * initialisation, so not relying on fuse_main function).
	 */
	if (background) {
		// Avoid chdir, else a relative mountpoint given as
		// argument to FUSE won't work.
		//  TBD FIXME  close stdout/stderr : how do we see errors
		//  TBD FIXME  if UPnP or FUSE fails in background mode ?
	        rc = daemon (/* nochdir => */ 1, /* noclose => */ 0);
		if (rc == -1) {
			int const err = errno;
			Log_Printf (LOG_ERROR,
				    "Failed to daemonize program : %s",
				    strerror (err));
			exit (err); // ---------->
		}
	}

	/*
	 * Initialize the device list
	 */
	if (DeviceList_Init() == -1) {
		return 1;
	}

	/*
	 * Initialie stream engine
	 */
	Stream_Init();

	/*
	 * Initialize client manager
	 */
	ClientManager_Init(port, lobind);

	/*
	 * Initialize and run fuse fs
	 */
	if ((rc = FuseFS_Run(djfs_flags, search_history_size, charset))) {
		Log_Printf (LOG_ERROR, "Error in FUSE main loop = %d", rc);
	}

	/* ignore SIGINT during shutdown */
	(void) signal(SIGINT, SIG_IGN);

	/*
	 * Cleanup and exit
	 */
	Log_Print(LOG_DEBUG, "Shutting down ...");
	ClientManager_Stop();
	FuseFS_Destroy(1);
	Stream_Destroy();
	DeviceList_Destroy();
	ClientManager_Destroy();
	Charset_Finish();
	Log_Finish();
	return rc;
}

