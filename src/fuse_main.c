/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* $Id: fuse_main.c 223 2006-07-25 19:43:02Z r3mi $
 *
 * main FUSE interface.
 * This file is part of djmount.
 *
 * (C) Copyright 2005 R�mi Turboult <r3mi@users.sourceforge.net>
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
#	include <config.h>
#endif

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/statfs.h>
#ifdef HAVE_SETXATTR
#	include <sys/xattr.h>
#endif
#include <stdarg.h>	
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

#if USE_CURL
#include "curl_util.h"
#endif

/*****************************************************************************
 * Configuration related to specific FUSE versions
 *****************************************************************************/

// Missing in earlier FUSE versions e.g. 2.2
#ifndef FUSE_VERSION
#	define FUSE_VERSION	(FUSE_MAJOR_VERSION * 10 + FUSE_MINOR_VERSION)
#endif

// "-o readdir_ino" option available ?
#if FUSE_VERSION >= 23
#	define HAVE_FUSE_O_READDIR_INO	1
#endif

// "-o nonempty" option available ?
#if FUSE_VERSION >= 24
#	define HAVE_FUSE_O_NONEMPTY	1
#endif

// per-file direct_io flag ?
#if FUSE_VERSION >= 24
#	define HAVE_FUSE_FILE_INFO_DIRECT_IO	1
#endif



/*****************************************************************************
 * Global djmount settings
 *****************************************************************************/

static const DJFS_Flags DEFAULT_DJFS_FLAGS = 
	DJFS_SHOW_METADATA
#if DEBUG
	| DJFS_SHOW_DEBUG
#endif
	;
 
// set to 0 to disable "search" sub-directories
static const size_t DEFAULT_SEARCH_HISTORY_SIZE = 100;


static VFS* g_djfs = NULL;



/*****************************************************************************
 * Charset conversions (display <-> UTF-8) for filesystem
 *****************************************************************************/

typedef struct {
	fuse_dirh_t    h;
	fuse_dirfil_t  filler;
} my_dir_handle;

static int filler_from_utf8 (fuse_dirh_t h, const char *name, 
			     int type, ino_t ino)
{
	// Convert filename to display charset
	char buffer [NAME_MAX + 1];
	char* display_name = Charset_ConvertString (CHARSET_FROM_UTF8, 
						    name, 
						    buffer, sizeof (buffer),
						    NULL);
	my_dir_handle* const my_h = (my_dir_handle*) h;
	int rc = my_h->filler (my_h->h, display_name, type, ino);
	if (display_name != buffer && display_name != name)
		talloc_free (display_name);
	return rc;
}

static int
Browse (const VFS_Query* query)
{
	int rc = -EIO;
	if (! Charset_IsConverting()) {
		rc = VFS_Browse (g_djfs, query);
	} else {
		VFS_Query utfq = *query;
		// Convert filename from display charset 
		char buffer [PATH_MAX];
		char* const utf_path = Charset_ConvertString 
			(CHARSET_TO_UTF8, query->path, buffer, sizeof (buffer),
			 NULL);
		utfq.path = utf_path;
		my_dir_handle my_h = { .h = query->h, .filler = query->filler};
		if (query->filler) {
			utfq.h = (void*) &my_h;
			utfq.filler = filler_from_utf8;
		}
		rc = VFS_Browse (g_djfs, &utfq);
		if (utf_path != buffer && utf_path != query->path)
			talloc_free (utf_path);
		// Convert symlink content (if any) to display charset
		if (query->lnk_buf) {
			char* const display_name = Charset_ConvertString 
				(CHARSET_FROM_UTF8, query->lnk_buf,
				 buffer, sizeof (buffer), NULL);
			if (display_name && display_name != query->lnk_buf) 
				strncpy (query->lnk_buf, display_name,
					 query->lnk_bufsiz);
			if (display_name != buffer && 
			    display_name != query->lnk_buf)
				talloc_free (display_name);
		}
	}
	return rc;
}

/*****************************************************************************
 * FUSE Operations
 *****************************************************************************/

static int 
fs_getattr (const char* path, struct stat* stbuf)
{
	*stbuf = (struct stat) { .st_mode = 0 };
	const VFS_Query q = { .path = path, .stbuf = stbuf };
	int rc = Browse (&q);
	
	return rc;
}

static int 
fs_readlink (const char *path, char *buf, size_t size)
{
	VFS_Query const q = { .path = path,
			      .lnk_buf = buf, .lnk_bufsiz = size };
	int rc = Browse (&q);
	return rc;
}

static int 
fs_getdir (const char* path, fuse_dirh_t h, fuse_dirfil_t filler)
{
	const VFS_Query q = { .path = path, .h = h, .filler = filler };
	int rc = Browse (&q);
	return rc;
}  


static int 
fs_mknod (const char* path, mode_t mode, dev_t rdev)
{
	// Permission denied : not allowed in this filesystem
	return -EPERM;
}


static int 
fs_mkdir (const char* path, mode_t mode)
{
	// Permission denied : not allowed in this filesystem
	return -EPERM;
}


static int 
fs_unlink (const char* path)
{
	// Permission denied : not allowed in this filesystem
	return -EPERM;
}


static int 
fs_rmdir (const char* path)
{
	// Permission denied : not allowed in this filesystem
	return -EPERM;
}


static int 
fs_symlink (const char* from, const char* to)
{
	// Permission denied : not allowed in this filesystem
	return -EPERM;
}


static int 
fs_rename (const char* from, const char* to)
{
	// Permission denied : not allowed in this filesystem
	return -EPERM;
}


static int 
fs_link (const char* from, const char* to)
{
	// Permission denied : not allowed in this filesystem
	return -EPERM;
}


static int 
fs_chmod (const char* path, mode_t mode)
{
	// Permission denied : not allowed in this filesystem
	return -EPERM;
}


static int 
fs_chown (const char* path, uid_t uid, gid_t gid)
{
	// Permission denied : not allowed in this filesystem
	return -EPERM;
}


static int fs_truncate (const char* path, off_t size)
{
	// Permission denied : not allowed in this filesystem
	return -EPERM;
}


static int 
fs_utime (const char *path, struct utimbuf *buf)
{
	// Permission denied : not allowed in this filesystem
	return -EPERM;
}


static int 
fs_open (const char* path, struct fuse_file_info* fi)
{
	if (fi == NULL) {
		return -EIO; // ---------->
	} 
	if ((fi->flags & O_ACCMODE) != O_RDONLY) {
		return -EACCES; // ---------->
	} 
	
	void* context = NULL; // TBD
	FileBuffer* file = NULL;
	const VFS_Query q = { .path = path, .talloc_context = context, 
			      .file = &file };
	int rc = Browse (&q);
	if (rc) {
		talloc_free (file);
		file = NULL;
	}
	fi->fh = (intptr_t) file;

#if HAVE_FUSE_FILE_INFO_DIRECT_IO	
	/*
	 * Whenever possible, do not set the 'direct_io' flag on files : 
	 * this allow the 'mmap' operation to succeed on these files.
	 * However, in some case, we have to set the 'direct_io' :
	 * a) if the size of the file if not known in advance 
	 *    (e.g. if the DIDL-Lite attribute <res@size> is not set)
	 * b) or if the buffer does not guaranty to return exactly the
	 *    number of bytes requested in a read (except on EOF or error)
	 */
	fi->direct_io = ( FileBuffer_GetSize (file) < 0 ||
			  ! FileBuffer_HasExactRead (file) );
#endif

	return rc;
}


static int
fs_read (const char* path, char* buf, size_t size, off_t offset,
	 struct fuse_file_info* fi)
{
	FileBuffer* const file = (FileBuffer*) fi->fh;
	int rc = FileBuffer_Read (file, buf, size, offset);
	return rc;
}


static int 
fs_write (const char* path, const char* buf, size_t size,
	  off_t offset, struct fuse_file_info* fi)
{
	// Permission denied : not allowed in this filesystem
	return -EPERM;
}


#if 1
#  define fs_statfs	NULL
#else
static int 
fs_statfs (const char* path, struct statfs* stbuf)
{
  int res = -ENOSYS; // not supported
  
  *stbuf = (struct statfs) { }; // TBD
  
  return res;
}
#endif


static int 
fs_release (const char* path, struct fuse_file_info* fi)
{
	FileBuffer* const file = (FileBuffer*) fi->fh;
	
	if (file) {
		FileBuffer_Close (file);
		talloc_free (file);
		fi->fh = (intptr_t) NULL;
	}
	return 0;
}


#if 1
#	define fs_fsync		NULL
#else
static int 
fs_fsync (const char* path, int isdatasync, struct fuse_file_info* fi)
{
  /* Just a stub.  This method is optional and can safely be left
     unimplemented */

  (void) path;
  (void) isdatasync;
  (void) fi;
  return 0;
}
#endif


#ifdef HAVE_SETXATTR
/*
 * xattr operations are optional and can safely be left unimplemented 
 */

static int 
fs_setxattr (const char *path, const char *name, const char *value,
	     size_t size, int flags)
{
	// Permission denied : not allowed in this filesystem
	return -EPERM;
}


#if 1
#	define fs_getxattr	NULL
#else
static int 
fs_getxattr (const char *path, const char *name, char *value, size_t size)
{
    int res = lgetxattr(path, name, value, size);
    if(res == -1)
        return -errno;
    return res;
}
#endif


#if 1
#	define fs_listxattr	NULL
#else
static int 
fs_listxattr (const char *path, char *list, size_t size)
{
    int res = llistxattr(path, list, size);
    if(res == -1)
        return -errno;
    return res;
}
#endif


static int 
fs_removexattr (const char *path, const char *name)
{
	// Permission denied : not allowed in this filesystem
	return -EPERM;
}


#endif /* HAVE_SETXATTR */


static struct fuse_operations fs_oper = {
	.getattr	= fs_getattr,
	.readlink	= fs_readlink,
	.getdir		= fs_getdir,
	.mknod		= fs_mknod,
	.mkdir		= fs_mkdir,
	.symlink	= fs_symlink,
	.unlink		= fs_unlink,
	.rmdir		= fs_rmdir,
	.rename		= fs_rename,
	.link		= fs_link,
	.chmod		= fs_chmod,
	.chown		= fs_chown,
	.truncate	= fs_truncate,
	.utime		= fs_utime,
	.open		= fs_open,
	.read		= fs_read,
	.write		= fs_write,
	.statfs		= fs_statfs,
	.flush      	= NULL,
	.release	= fs_release,
	.fsync		= fs_fsync,
#ifdef HAVE_SETXATTR
	.setxattr	= fs_setxattr,
	.getxattr	= fs_getxattr,
	.listxattr	= fs_listxattr,
	.removexattr	= fs_removexattr,
#endif
};


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
stdout_print (Log_Level level, const char* msg)
{
	Log_BeginColor (level, logf);
	switch (level) {
	case LOG_ERROR:		fprintf (logf, "[E] "); break;
	case LOG_WARNING:	fprintf (logf, "[W] "); break;
	case LOG_INFO:		fprintf (logf, "[I] "); break;
	case LOG_DEBUG:		fprintf (logf, "[D] "); break;
	default:
		fprintf (logf, "[%d] ", (int) level);
		break;
	}
	
	// Convert message to display charset, and print
	Charset_PrintString (CHARSET_FROM_UTF8, msg, logf);
	Log_EndColor (level, logf);
	fprintf (logf, "\n");
	fflush(logf);
}


/*****************************************************************************
 * Usage
 *****************************************************************************/

#if UPNP_HAVE_DEBUG
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
  fprintf (stream, "usage: %s [options] mountpoint\n", progname);
  fprintf 
    (stream,
     "\n"
     "Options:\n"
     "    -h or --help           print this help, then exit\n"
     "    --version              print version number, then exit\n"
     "    -o [options]           mount options (see below)\n"
     "    -d[levels]             enable debug output (implies -f)\n"
     "    -f                     foreground operation (default: daemonized)\n"
     "\n"
     "Mount options (one or more comma separated options) :\n"
#if HAVE_CHARSET
     "    iocharset=<charset>    filenames encoding (default: environment)\n"
#endif
     "    playlists              use playlists for AV files, instead of plain files\n"
     "    search_history=<size>  number of remembered searches (default: %zd)\n"
     "                           (set to 0 to disable search)\n"
     "\n", DEFAULT_SEARCH_HISTORY_SIZE);
  fprintf 
    (stream,
     "See FUSE documentation for the following mount options:\n%s",
     FUSE_ALLOWED_OPTIONS);
  fprintf 
    (stream,
     "\nDebug levels are one or more comma separated words :\n"
#if UPNP_HAVE_DEBUG
     "    upnperr, upnpall : increasing level of UPnP traces\n"
#endif
     "    error, warn, info, debug : increasing level of djmount traces\n"
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
	fprintf (stderr, "%s: ", progname);
	va_list ap;
	va_start (ap, progname);
	const char* const format = va_arg (ap, const char*);
	vfprintf (stderr, format, ap);
	va_end (ap);
	fprintf (stderr, "\nTry '%s --help' for more information.\n",
		 progname);
	exit (EXIT_FAILURE); // ---------->
}


static void
version (FILE* stream, const char* progname)
{
	fprintf (stream, 
		 "%s (" PACKAGE ") " VERSION "\n", progname);
	fprintf (stream, "Copyright (C) 2005 R�mi Turboult\n");
	fprintf (stream, "Compiled against: ");
	fprintf (stream, "FUSE %d.%d", FUSE_MAJOR_VERSION, 
		 FUSE_MINOR_VERSION);
#ifdef UPNP_VERSION_STRING
	fprintf (stream, ", libupnp %s", UPNP_VERSION_STRING);
#endif
	fputs ("\n\
This is free software. You may redistribute copies of it under the terms of\n\
the GNU General Public License <http://www.gnu.org/licenses/gpl.html>.\n\
There is NO WARRANTY, to the extent permitted by law.\n\
\n", stream);
	exit (EXIT_SUCCESS); // ---------->
}


/*****************************************************************************
 * Main
 *****************************************************************************/

int 
main (int argc, char *argv[])
{
	int rc;
	bool background = true;

	// Create a working context for temporary strings
	void* const tmp_ctx = talloc_autofree_context();

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
	if (logf == NULL) {
		logf = stdout;
	}

	rc = Log_Initialize (stdout_print);
	if (rc != 0) {
		fprintf (stderr, "%s : Error initialising Logger", argv[0]);
		exit (rc); // ---------->
	}  
	Log_Colorize (true);
#if UPNP_HAVE_DEBUG
	SetLogFileNames ("/dev/null", "/dev/null");
#endif
	
	/*
	 * Handle options
	 */
	char* charset = NULL;
	DJFS_Flags djfs_flags = DEFAULT_DJFS_FLAGS;
	size_t search_history_size = DEFAULT_SEARCH_HISTORY_SIZE;

	char* fuse_argv[32] = { argv[0] };
	int fuse_argc = 1;
	
#define FUSE_ARG(OPT)							\
	if (fuse_argc >= 31) bad_usage (argv[0], "too many args");	\
	fuse_argv[fuse_argc] = OPT;					\
	Log_Printf (LOG_DEBUG, "  Fuse option = %s", fuse_argv[fuse_argc]); \
	fuse_argc++

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
			
		} else if (strcmp(o, "-f") == 0) {
			background = false;

		} else if (*o != '-') { 
			// mount point
			FUSE_ARG (o);
			
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
					FUSE_ARG ("-o");
					FUSE_ARG (talloc_strdup (tmp_ctx, s));
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
					talloc_enable_leak_report();
				} else if (strcmp (s, "leakfull") == 0) {
					talloc_enable_leak_report_full();
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
#if UPNP_HAVE_DEBUG
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
			
		} else {
			bad_usage (argv[0], "unrecognized option '%s'", 
				   o); // ---------->
		}
	}
	
	// Force Read-only (write operations not implemented yet)
	FUSE_ARG ("-r"); 

#if HAVE_FUSE_O_READDIR_INO
	// try to fill in d_ino in readdir
	FUSE_ARG ("-o");
	FUSE_ARG ("readdir_ino");
#endif
#if !HAVE_FUSE_FILE_INFO_DIRECT_IO	
	// Set global "direct_io" option, if not available per open file,
	// because we are not sure that every open file can be opened 
	// without this mode : see comment in fs_open() function.
	FUSE_ARG ("-o");
	FUSE_ARG ("direct_io");
#endif

	/*
	 * Set charset encoding
	 */
	rc = Charset_Initialize (charset);
	if (rc) {
		Log_Printf (LOG_ERROR, "Error initialising charset='%s'",
			    NN(charset));
	}

	/* 
	 * Create virtual file system
	 */
	g_djfs = DJFS_ToVFS (DJFS_Create (tmp_ctx, djfs_flags,
					  search_history_size));
	if (g_djfs == NULL) {
		Log_Printf (LOG_ERROR, "Failed to create virtual file system");
		exit (EXIT_FAILURE); // ---------->
	}

	/*
	 * Daemonize process if necessary (must be done before UPnP
	 * initialisation, so not relying on fuse_main function).
	 */
	FUSE_ARG ("-f");
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
	
#if USE_CURL
	/*
	 * Initialie cURL
	 */
	CurlUtil_Init();
#endif

	/*
	 * Initialise UPnP Control point and starts FUSE file system
	 */

#if 1
	ClientManager_Start();
#else
	rc = DeviceList_Start (CONTENT_DIR_SERVICE_TYPE, NULL);
	if (rc != UPNP_E_SUCCESS) {
		Log_Printf (LOG_ERROR, 
			    "Error starting UPnP Control Point : %d (%s)",
			    rc, UpnpGetErrorMessage (rc));
		exit (rc); // ---------->
	}
#endif

	fuse_argv[fuse_argc] = NULL; // End FUSE arguments list
	rc = fuse_main (fuse_argc, fuse_argv, &fs_oper);
	if (rc != 0) {
		Log_Printf (LOG_ERROR, "Error in FUSE main loop = %d", rc);
	}
	
	Log_Printf (LOG_DEBUG, "Shutting down ...");
#if 1
	ClientManager_Stop();
#else
	DeviceList_Stop();
#endif
	
	(void) Charset_Finish();
	Log_Finish();

	return rc; 
}
