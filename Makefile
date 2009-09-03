##
# Building apk-tools

PACKAGE := apk-tools
VERSION := 2.0_rc5

##
# Default directories

DESTDIR		:=
SBINDIR		:= /sbin
CONFDIR		:= /etc/apk
MANDIR		:= /usr/share/man
DOCDIR		:= /usr/share/doc/apk

export DESTDIR SBINDIR CONFDIR MANDIR DOCDIR

##
# Top-level rules and targets

targets		:= src/

##
# Include all rules and stuff

include Make.rules

##
# Top-level targets

install:
	$(INSTALLDIR) $(DESTDIR)$(DOCDIR)
	$(INSTALL) README $(DESTDIR)$(DOCDIR)

static:
	$(Q)$(MAKE) STATIC=y
