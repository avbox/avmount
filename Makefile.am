# $Id: Makefile.am 216 2006-07-09 17:32:37Z r3mi $
#
# Top-level Makefile.am for djmount
#
# Copyright (C) 2005 Rémi Turboult <r3mi@users.sourceforge.net>
#

ACLOCAL_AMFLAGS = -I m4 -I gl/m4

SUBDIRS 	= gl
SUBDIRS 	+= src

EXTRA_DIST	= search_help.txt


# 
# Note : 'libupnp' has a complete autoconf/automake system, which also handles
# an 'install' target. But we don't want to install libupnp, and we don't want
# to modify its build system either, so we just forward all 'install*' targets
# to './djmount' only.
#
AUTOMAKE_OPTIONS = -Wno-override

# note: do not try to declare all targets together (install install-data ...)
# or automake will not detect the override.
install:
	$(MAKE) $(AM_MAKEFLAGS) -C src $@
install-data:
	$(MAKE) $(AM_MAKEFLAGS) -C src $@
install-exec:
	$(MAKE) $(AM_MAKEFLAGS) -C src $@
install-info:
	$(MAKE) $(AM_MAKEFLAGS) -C src $@
installdirs:
	$(MAKE) $(AM_MAKEFLAGS) -C src $@
uninstall:
	$(MAKE) $(AM_MAKEFLAGS) -C src $@
uninstall-info :
	$(MAKE) $(AM_MAKEFLAGS) -C src $@


