/* $Id: fuse_fs.c
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

#ifdef HAVE_CONFIG_H
#	include "../config.h"
#endif

#include <fuse.h>
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

#include <stdio.h>
#include <stddef.h>
#include <limits.h>
#include <sys/param.h>

#include "talloc_util.h"
#include "device_list.h"
#include "log.h"
#include "upnp_util.h"
#include "string_util.h"
#include "content_dir.h"
#include "charset.h"
#include "minmax.h"
#include "clientmgr.h"
#include "stream.h"
#include "djfs.h"

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

#define USE_FUSE_SETUP 1

/*****************************************************************************
 * Global avmount settings
 *****************************************************************************/

#if USE_FUSE_SETUP
static char *mountpoint = NULL;
static struct fuse *fuse = NULL;
#endif
static void *context = NULL;;
static VFS* g_djfs = NULL;
static char* fuse_argv[32] = { NULL };
static int fuse_argc = 0;



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


static int
fs_truncate (const char* path, off_t size)
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


/**
 * Fuse_SetOpt() -- Set FUSE option.
 */
int
FuseFS_SetOpt(const char *opt)
{
	if (fuse_argc >= 31) {
		return -ENOMEM;
	}
	fuse_argv[fuse_argc++] = (char*) opt;
	Log_Printf(LOG_DEBUG, "Fuse option = %s", opt);
	return 0;
}


/**
 * Fuse_RunLoop() -- Main FUSE Loop
 */
int
FuseFS_Run(DJFS_Flags djfs_flags, size_t search_history_size, char *charset)
{
	int rc;

	/* allocate talloc context */
	context = talloc_new(NULL);
	if (context == NULL) {
		Log_Print(LOG_ERROR, "Fuse_Run(): Out of memory");
		return -1;
	}
#ifdef DEBUG
	talloc_set_name(context, "FUSE");
#endif

	// Force Read-only (write operations not implemented yet)
	FuseFS_SetOpt("-r");
	FuseFS_SetOpt("-f");

#if HAVE_FUSE_O_READDIR_INO
	// try to fill in d_ino in readdir
	FuseFS_SetOpt("-o");
	FuseFS_SetOpt("readdir_ino");
#endif
#if !HAVE_FUSE_FILE_INFO_DIRECT_IO
	// Set global "direct_io" option, if not available per open file,
	// because we are not sure that every open file can be opened
	// without this mode : see comment in fs_open() function.
	FuseFS_SetOpt("-o");
	FuseFS_SetOpt("direct_io");
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
	g_djfs = DJFS_ToVFS (DJFS_Create(context, djfs_flags,
					  search_history_size));
	if (g_djfs == NULL) {
		Log_Printf (LOG_ERROR, "Failed to create virtual file system");
		exit (EXIT_FAILURE); // ---------->
	}

#if USE_FUSE_SETUP
	int multithreaded;
	fuse = fuse_setup(fuse_argc, fuse_argv, &fs_oper, sizeof(struct fuse_operations),
		&mountpoint, &multithreaded, NULL);
	if (fuse == NULL) {
		return 1;
	}

	if (multithreaded) {
		rc = fuse_loop_mt(fuse);
	} else {
		rc = fuse_loop(fuse);
	}

	if (rc == -1)
		rc = 1;
#else
	fuse_argv[fuse_argc] = NULL; // End FUSE arguments list
	rc = fuse_main(fuse_argc, fuse_argv, &fs_oper);
	if (rc != 0) {
		Log_Printf (LOG_ERROR, "Error in FUSE main loop = %d", rc);
	}
#endif
	return rc;
}


/**
 * Fuse_Destroy() -- Free FUSE memory
 */
void
FuseFS_Destroy(int teardown)
{
	Log_Print(LOG_DEBUG, "FuseFS_Destroy() running");
#if USE_FUSE_SETUP
	if (fuse != NULL) {
		if (teardown) {
			fuse_teardown(fuse, 0, mountpoint);
		} else {
			fuse_destroy(fuse);
		}
	}
#else
	(void) teardown;
#endif
	if (context != NULL) {
		talloc_free(context);
		context = NULL;
	}
}

