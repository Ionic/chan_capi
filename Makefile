#
# (CAPI*)
#
# An implementation of Common ISDN API 2.0 for
# Asterisk/OpenPBX.org
#
# Makefile, based on the Asterisk Makefile, Coypright (C) 1999, Mark Spencer
#
# Copyright (C) 2005-2006 Cytronics & Melware
# 
# Armin Schindler <armin@melware.de>
# 
# Reworked, but based on the work of
# Copyright (C) 2002-2005 Junghanns.NET GmbH
#
# Klaus-Peter Junghanns <kapejod@ns1.jnetdns.de>
#
# This program is free software and may be modified and 
# distributed under the terms of the GNU Public License.
#

OSNAME=${shell uname}

.EXPORT_ALL_VARIABLES:

.PHONY: openpbx

INSTALL_PREFIX=

ASTERISK_HEADER_DIR=$(INSTALL_PREFIX)/usr/include

ifeq (${OSNAME},FreeBSD)
ASTERISK_HEADER_DIR=$(INSTALL_PREFIX)/usr/local/include
endif

ifeq (${OSNAME},NetBSD)
ASTERISK_HEADER_DIR=$(INSTALL_PREFIX)/usr/pkg/include
endif

ASTERISKVERSION=$(shell if [ -f .version ]; then cat .version; else if [ -d CVS ]; then if [ -f CVS/Tag ] ; then echo "CVS-`sed 's/^T//g' CVS/Tag`-`date +"%D-%T"`"; else echo "CVS-HEAD-`date +"%D-%T"`"; fi; fi; fi)

MODULES_DIR=$(INSTALL_PREFIX)/usr/lib/asterisk/modules

ifeq (${OSNAME},FreeBSD)
MODULES_DIR=$(INSTALL_PREFIX)/usr/local/lib/asterisk/modules
endif

ifeq (${OSNAME},NetBSD)
MODULES_DIR=$(INSTALL_PREFIX)/usr/pkg/lib/asterisk/modules
endif

CONFIG_DIR=$(INSTALL_PREFIX)/etc/asterisk

ifeq (${OSNAME},FreeBSD)
CONFIG_DIR=$(INSTALL_PREFIX)/usr/local/etc/asterisk
endif

ifeq (${OSNAME},NetBSD)
CONFIG_DIR=$(INSTALL_PREFIX)/usr/pkg/etc/asterisk
endif

PROC=$(shell uname -m)

LIBLINUX=
DEBUG=-g #-pg
INCLUDE=-I$(ASTERISK_HEADER_DIR)
ifndef C4B
ifeq (${OSNAME},FreeBSD)
INCLUDE+=$(shell [ -d /usr/include/i4b/include ] && \
	echo -n -I/usr/include/i4b/include)
endif
ifeq (${OSNAME},NetBSD)
INCLUDE+=$(shell [ -d /usr/include/i4b/include ] && \
        echo -n -I/usr/include/i4b/include)
endif
endif #C4B

ifdef C4B
LIBLINUX=-L/usr/local/lib -llinuxcapi20
endif

CFLAGS=-pipe -fPIC -Wall -Wmissing-prototypes -Wmissing-declarations $(DEBUG) $(INCLUDE) -D_REENTRANT -D_GNU_SOURCE
CFLAGS+=$(OPTIMIZE)
CFLAGS+=-O6
CFLAGS+=$(shell if $(CC) -march=$(PROC) -S -o /dev/null -xc /dev/null >/dev/null 2>&1; then echo "-march=$(PROC)"; fi)
CFLAGS+=$(shell if uname -m | grep -q ppc; then echo "-fsigned-char"; fi)

CFLAGS+=-DASTERISKVERSION=\"$(ASTERISKVERSION)\"

LIBS=-ldl -lpthread -lm
CC=gcc
INSTALL=install

SHAREDOS=chan_capi.so

OBJECTS=chan_capi.o c20msg.o chan_capi_rtp.o

CFLAGS+=-Wno-missing-prototypes -Wno-missing-declarations

CFLAGS+=-DCRYPTO

all: config.h $(SHAREDOS)

clean:
	rm -f config.h
	rm -f *.so *.o

config.h:
	./create_config.sh "$(ASTERISK_HEADER_DIR)"

chan_capi.so: $(OBJECTS)
	$(CC) -shared -Xlinker -x -o $@ $^ $(LIBLINUX) -lcapi20

install: all
	$(INSTALL) -d -m 755 $(MODULES_DIR)
	for x in $(SHAREDOS); do $(INSTALL) -m 755 $$x $(MODULES_DIR) ; done

install_config: capi.conf
	$(INSTALL) -d -m 755 ${CONFIG_DIR}
	$(INSTALL) -m 644 capi.conf ${CONFIG_DIR}

samples: install_config

openpbx:
	@rm -rf openpbx
	@mkdir -p openpbx/channels
	@mkdir -p openpbx/include/openpbx
	@mkdir -p openpbx/doc
	@mkdir -p openpbx/configs
	@(	\
	 ./preparser -c openpbx.ctrl chan_capi.c openpbx/channels/chan_capi.c;	\
	 ./preparser -c openpbx.ctrl chan_capi_rtp.c openpbx/channels/chan_capi_rtp.c;	\
	 ./preparser -c openpbx.ctrl c20msg.c openpbx/channels/c20msg.c;	\
	 ./preparser -c openpbx.ctrl chan_capi.h openpbx/include/openpbx/chan_capi.h;	\
	 ./preparser -c openpbx.ctrl chan_capi_rtp.h openpbx/include/openpbx/chan_capi_rtp.h;	\
	 ./preparser -c openpbx.ctrl chan_capi20.h openpbx/include/openpbx/chan_capi20.h;	\
	 ./preparser -c openpbx.ctrl xlaw.h openpbx/include/openpbx/xlaw.h;	\
	 ./preparser -c openpbx.ctrl README openpbx/doc/README.chan_capi;	\
	 ./preparser -c openpbx.ctrl capi.conf openpbx/configs/capi.conf.sample;	\
	 true;	\
	)


