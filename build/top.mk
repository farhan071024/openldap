##
## Copyright 1998 The OpenLDAP Foundation
## COPYING RESTRICTIONS APPLY.  See COPYRIGHT File in top level directory
## of this package for details.
##
@SET_MAKE@

SHELL = /bin/sh

srcdir = @srcdir@
top_srcdir = @top_srcdir@
VPATH = @srcdir@
prefix = @prefix@
exec_prefix = @exec_prefix@

bindir = @bindir@
sbindir = @sbindir@
libexecdir = @libexecdir@
datadir = @datadir@
sysconfdir = @sysconfdir@/ldap
sharedstatedir = @sharedstatedir@
localstatedir = @localstatedir@
libdir = @libdir@
infodir = @infodir@
mandir = @mandir@
includedir = @includedir@

INSTALL = @INSTALL@
INSTALL_PROGRAM = @INSTALL_PROGRAM@
INSTALL_DATA = @INSTALL_DATA@
INSTALL_SCRIPT = @INSTALL_SCRIPT@

LN = ln
LN_S = @LN_S@
RM = rm -f
MAKEINFO = @MAKEINFO@
RANLIB = @RANLIB@
AR = ar

LINT = lint
5LINT = 5lint
MKDEP = mkdep

# Misc UNIX commands used in makefiles
SED = sed
DATE = date
HOSTNAME = uname -n
BASENAME = basename
PWD = pwd
CAT = cat
MKDIR = mkdir
CHMOD = chmod

# Misc UNIX commands used in programs
EDITOR = @EDITOR@
FINGER = @FINGER@
SENDMAIL = @SENDMAIL@

# Version
VERSIONFILE = $(top_srcdir)/build/version

INCLUDEDIR = -I$(top_srcdir)/include $(XINCLUDEDIR)

LDAP_LIBDIR = $(top_srcdir)/libraries
LDAP_LIBS = -lldif -lldap -llber
LDAP_LIBDEPEND = $(LDAP_LIBDIR)/libldap.a

# AutoConfig generated 
AC_CC	= @CC@
AC_DEFS = @CPPFLAGS@ @DEFS@ @LDAP_DEFS@
AC_LIBS = @LDFLAGS@ @LIBS@
AC_CFLAGS = @CFLAGS@
AC_LDFLAGS =

LIBCRYPT = @LIBCRYPT@
LIBTERMCAP = @LIBTERMCAP@
LIBDB = @LIBDB@

# Our Defaults
CC = $(AC_CC)
DEFS = $(AC_DEFS) $(INCLUDEDIR) $(LDAP_DEFS) $(XDEFS)
LIBS = -L$(LDAP_LIBDIR) $(LDAP_LIBS) $(XLIBS) $(AC_LIBS)

CFLAGS = $(AC_CFLAGS) $(DEFS) $(DEFINES)
LDFLAGS = $(AC_LDFLAGS)

all:		all-common FORCE
install:	install-common FORCE
clean:		clean-common FORCE
veryclean:	veryclean-common FORCE
depend:		depend-common FORCE

# empty local rules
all-local:
install-local:
clean-local:
veryclean-local:
depend-local:
lint-local:
lint5-local:

Makefile: Makefile.in ${top_srcdir}/config.status
	@if [ $(top_srcdir) = $(srcdir) ]; then ; \
		./config.status	; \
	else ; \
		echo "Makefile out of date, run config.status from $top_srcdir" ; \
		exit 1 ; \
	fi

# empty rule for forcing rules
FORCE:

##---------------------------------------------------------------------------
