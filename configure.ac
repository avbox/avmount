# -*- Autoconf -*-
# $Id: configure.ac 266 2006-08-27 20:07:58Z r3mi $
#
# Top-level configure.ac file for avmount
#
# Process this file with autoconf to produce a configure script.
#
# (C) Copyright 2016 Fernando Rodriguez
# (C) Copyright 2005 Rémi Turboult <r3mi@users.sourceforge.net>
#

AC_PREREQ([2.69])
AC_INIT([avmount],[0.8],[https://github.com/fernando-rodriguez/avmount])

AC_CONFIG_MACRO_DIR(gl/m4)

AC_CONFIG_SRCDIR(src/djfs.c)

AM_INIT_AUTOMAKE([1.9 -Wall std-options gnu])

AC_CONFIG_HEADERS(config.h)

AC_COPYRIGHT([Copyright (C) 2005 Rémi Turboult])
AC_REVISION([$Revision: 266 $])

#
# --enable-FEATURE flags
# ----------------------
#

AC_ARG_ENABLE([debug], [extra debugging code])
AM_CONDITIONAL(ENABLE_DEBUG, test x"$enable_debug" = xyes)
if test x"$enable_debug" = xyes; then
	AC_DEFINE([DEBUG],1,[Define to 1 to compile debug code])
	DAEMON_ARGS="-d -l /var/log/avmount.log"
else
	AC_DEFINE([NDEBUG],1,[Define to 1  to disable debug code])
	DAEMON_ARGS=""
fi
AC_SUBST(DAEMON_ARGS)

AC_ARG_ENABLE([charset],
	[charset conversion code and -o iocharset option (assume all UTF-8 when disabled)])
AM_CONDITIONAL(ENABLE_CHARSET, test x"$enable_charset" = xyes)
if test x"$enable_charset" = xyes; then
	AC_DEFINE([HAVE_CHARSET],1,
		  [Define to 1 to have charset conversion code])
fi

AC_ARG_ENABLE([ipv6], [IPv6 support])
if test x"$enable_ipv6" = xyes; then
	AC_DEFINE([ENABLE_IPV6],1,[Define to 1 to enable IPv6 support])
fi

AC_ARG_WITH([systemd], [Install systemd unit])
AM_CONDITIONAL([WITH_SYSTEMD], test x"$with_systemd" = xyes)


#
# Checks for programs
#
AC_PROG_CC
gl_EARLY

LT_INIT([disable-shared])

AC_PROG_INSTALL
AC_PROG_MAKE_SET
AC_PROG_LN_S
AC_PROG_MKDIR_P

AC_PATH_PROG(PERL,perl)
AC_PATH_PROG(GIT,git)

# for pkg_config
export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/usr/local/lib/pkgconfig



#
# Default compilation flags
#
if test x"$enable_debug" = xyes; then
	# AC_PROG_CC already sets CFLAGS to "-g -O2" by default
	CFLAGS="$CFLAGS -O0 -ggdb"
else
	# add optimise for size
	CFLAGS="$CFLAGS -Os"
fi
CFLAGS="$CFLAGS -Wall"

# Arrange for large-file support / large off_t
# Define _FILE_OFFSET_BITS and _LARGE_FILES if necessary
# See also http://unix.freshmeat.net/articles/view/709/
AC_SYS_LARGEFILE


#
# Checks for header files
#
AC_HEADER_DIRENT
AC_HEADER_STDC
AC_CHECK_HEADERS([inttypes.h stddef.h stdint.h stdarg.h])
if test x"$enable_charset" = xyes; then
	AC_CHECK_HEADERS([locale.h langinfo.h])
fi


#
# Checks for typedefs, structures, and compiler characteristics
#
AC_HEADER_STDBOOL
AC_C_CONST
AC_C_INLINE


#
# Checks for library functions
#
AC_CHECK_FUNCS([setxattr mktime])
if test x"$enable_charset" = xyes; then
	AC_CHECK_FUNCS([setlocale])
fi
AC_FUNC_FSEEKO

# Optional GNU extensions
AH_TEMPLATE([_GNU_SOURCE],[Define to 1 to get GNU extensions])
AC_CHECK_FUNCS([open_memstream], [AC_DEFINE([_GNU_SOURCE])])


#
# gnulib 
# ------
# Configuration for imported modules.
# Use "gnulib-tool --import" to update the modules : 
# see http://lists.gnu.org/archive/html/bug-gnulib/2005-08/msg00241.html
gl_INIT



#
# talloc 
# ------
#
PKG_CHECK_MODULES(TALLOC, [talloc >= 2.1.5], ,
	AC_MSG_ERROR('Unable to find libtalloc. Please make sure library and header files are installed.'))
CFLAGS="$CFLAGS $TALLOC_CFLAGS"
LDFLAGS="$LDFLAGS $TALLOC_LIBS"

if false; then
AC_MSG_CHECKING([whether talloc works])
AC_LANG([C])
AC_LINK_IFELSE([
	AC_LANG_SOURCE([[
/* those include are currently missing from "talloc.h" */
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_STDARG_H
#  include <stdarg.h>
#else
#  include <varargs.h>
#endif
#include "talloc.h"

int main()
{
	/* test for recent talloc functions */
	(void) talloc_get_size (talloc_autofree_context());
	return 0;
}
	]])],
	[AC_MSG_RESULT([yes])],
	[AC_MSG_ERROR([no])])
fi

#
# readline (option)
# -----------------
# Checks for readline and history compatible libraries
#
AX_LIB_READLINE


#
# iconv & langinfo
# ----------------
#

if test x"$enable_charset" = xyes; then
	# Checks for function in C library or libiconv 
	# (AM macros from "gettext" package)
	AM_ICONV
	AM_LANGINFO_CODESET
	# TBD print warning if no setlocale or nl_langinfo(CODESET) implemented
fi
AM_CONDITIONAL(WANT_ICONV, 
   	  test x"$enable_charset" = xyes && test x"$am_cv_func_iconv" = xyes)


#
# POSIX Threads
# -------------
#
AX_PTHREAD([],[AC_MSG_ERROR([POSIX threads are required to build this program])])

# We are only building threads programs so we add the pthread flags directly
# to the default compilation variables. This will also allow the tests below
# (fuse, libupnp, ...) to pick these flags.
LIBS="$PTHREAD_LIBS $LIBS"

CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
CC="$PTHREAD_CC"


#
# FUSE 
# ----
#
PKG_CHECK_MODULES(FUSE, [fuse >= 2.2], ,
	AC_MSG_ERROR('Unable to find libfuse. Please make sure library and header files are installed.'))
CFLAGS="$CFLAGS $FUSE_CFLAGS -DFUSE_USE_VERSION=22"
LDFLAGS="$LDFLAGS $FUSE_LIBS"

if false; then
AC_MSG_CHECKING([whether libfuse works])
AC_LANG([C])
AC_LINK_IFELSE([
	AC_LANG_SOURCE([[
/* must request latest API (25) else dont work when using FUSE 2.5 */
#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 25
#endif
#include <fuse.h> 
/* Missing in earlier FUSE versions e.g. 2.2 */
#ifndef FUSE_VERSION
#	define FUSE_VERSION	(FUSE_MAJOR_VERSION * 10 + FUSE_MINOR_VERSION)
#endif
#if FUSE_VERSION < 22
#	error FUSE version is too old
#endif
int main() { fuse_destroy(0); return 0; }
	]])],
	[AC_MSG_RESULT([yes])],
	[AC_MSG_ERROR([no])])
fi

#
# libupnp
#
PKG_CHECK_MODULES(LIBUPNP, [libupnp >= 1.3.1], ,
	AC_MSG_ERROR('Unable to find libfuse. Please make sure library and header files are installed.'))
CFLAGS="$CFLAGS $LIBUPNP_CFLAGS"
LDFLAGS="$LDFLAGS $LIBUPNP_LIBS"

if false; then
AC_MSG_CHECKING([whether libupnp works])
AC_LANG([C])
AC_LINK_IFELSE([
	AC_LANG_SOURCE([[
#include <upnp/ixml.h>
#include <upnp/upnp.h>
#include <upnp/upnptools.h>
#if (!defined UPNP_VERSION) || (UPNP_VERSION < 10301)
#	error libupnp version is too old
#endif
int main()
{
	UpnpResolveURL (NULL, NULL, NULL);
	/* test for functions added after libupnp-1.2.1a */
	ixmlRelaxParser (0);
	UpnpSetMaxContentLength (0);
	return 0;
}
	]])],
	[AC_MSG_RESULT([yes])],
	[AC_MSG_ERROR([no])])
fi

#
# Check for differences accross supported libupnp versions
#
CFLAGS_save="$CFLAGS"
CFLAGS="$CFLAGS -Werror"
AC_MSG_CHECKING([whether Upnp_Event_Subscribe.PublisherUrl is char*])
AC_LANG([C])
AC_COMPILE_IFELSE([
        AC_LANG_SOURCE([[
#include <stdlib.h>
#include <stdio.h>
#include <upnp/upnp.h>
#if (!defined UPNP_VERSION) || (UPNP_VERSION < 10301)
#       error libupnp version is too old
#endif
void
StringTest(const char *s)
{
	printf("%s", s);
}

int main()
{
	struct Upnp_Event_Subscribe e;
	StringTest(e.PublisherUrl);
	return 0;
}
	]])],
	[AC_MSG_RESULT([yes])],
	[
		AC_MSG_RESULT([no])
		AC_DEFINE([HAVE_UPNPSTRING_PUBLISHERURL], 1,
			[Define to 1 if Upnp_Event_Subscribe.PublisherUrl is char*])
	])

AC_MSG_CHECKING([whether Upnp_Action_Complete.CtrlUrl is char*])
AC_LANG([C])
AC_COMPILE_IFELSE([
        AC_LANG_SOURCE([[
#include <stdlib.h>
#include <stdio.h>
#include <upnp/upnp.h>
#if (!defined UPNP_VERSION) || (UPNP_VERSION < 10301)
#       error libupnp version is too old
#endif
void
StringTest(const char *s)
{
	printf("%s", s);
}

int main()
{
	struct Upnp_Action_Complete e;
	StringTest(e.CtrlUrl);
	return 0;
}
	]])],
	[AC_MSG_RESULT([yes])],
	[
		AC_MSG_RESULT([no])
		AC_DEFINE([HAVE_UPNPSTRING_ACTIONCOMPLETE_CTRLURL], 1,
			[Define to 1 if Upnp_Action_Complete.CtrlUrl is char*])
	])

AC_MSG_CHECKING([whether Upnp_State_Var_Complete.CtrlUrl is char*])
AC_LANG([C])
AC_COMPILE_IFELSE([
        AC_LANG_SOURCE([[
#include <stdlib.h>
#include <stdio.h>
#include <upnp/upnp.h>
#if (!defined UPNP_VERSION) || (UPNP_VERSION < 10301)
#       error libupnp version is too old
#endif
void
StringTest(const char *s)
{
	printf("%s", s);
}

int main()
{
	struct Upnp_State_Var_Complete e;
	StringTest(e.CtrlUrl);
	return 0;
}
	]])],
	[AC_MSG_RESULT([yes])],
	[
		AC_MSG_RESULT([no])
		AC_DEFINE([HAVE_UPNPSTRING_STATEVARCOMPLETE_CTRLURL], 1,
			[Define to 1 if Upnp_State_Var_Complete.CtrlUrl is char*])
	])



CFLAGS="$CFLAGS_save"


#
# Check if we have CheckLogFileNames in libupnp
#
AC_CHECK_LIB([upnp], [SetLogFileNames],
	[AC_DEFINE([HAVE_UPNP_SETLOGFILENAMES], 1, [Define to 1 if we have SetLogFileNames])])

#
# If IPv6 is enabled make sure that libupnp is ipv6 enabled
#
if test x"$enable_ipv6" = xyes; then
	AC_CHECK_LIB([upnp], [UpnpInit2],
		[AC_DEFINE([HAVE_UPNPINIT2], 1, [Define to 1 if we have UpnpInit2])],
		[AC_MSG_ERROR([It appears that libupnp has been compiled without IPv6 support!])])
fi


#
# libcurl
#
AX_LIB_CURL([7.45.0],,
	[AC_MSG_ERROR([Your system lacks libcurl >= 7.45.0])])
LDFLAGS="$LDFLAGS -lcurl"
AC_DEFINE([USE_CURL], 1, [Define to 1 to use libcurl])

#
# malloc_trim()
#
AC_CHECK_FUNCS([malloc_trim])

#
# git commit hash
#
if test x"$GIT" != x; then
	have_repo=$($GIT log -1 2>/dev/null)
	if test x"$have_repo" != x; then
		AC_MSG_CHECKING([git commit hash])
		commit_hash="$($GIT log -1 | head -n1 | cut -d' ' -f2)"
		if test x"$commit_hash" != x; then
			AC_MSG_RESULT([$commit_hash])
			AC_DEFINE_UNQUOTED([GIT_COMMIT], [$commit_hash], [Git commit hash])
		else
			AC_MSG_RESULT([no])
		fi
		AC_MSG_CHECKING([git branch])
		git_branch="$($GIT status | head -n1 | cut -d' ' -f3)"
		if test x"$git_branch" != x; then
			AC_MSG_RESULT([$git_branch])
			AC_DEFINE_UNQUOTED([GIT_BRANCH], [$git_branch], [Git branch])
		else
			AC_MSG_RESULT([no])
		fi
	fi
fi

#
# Output Makefiles
#

AC_CONFIG_FILES([Makefile
		 gl/Makefile
                 src/Makefile
		 src/avmount.service
                 ])

AC_OUTPUT

