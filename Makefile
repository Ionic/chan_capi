#
# (CAPI*)
#
# An implementation of Common ISDN API 2.0 for Asterisk
#
# Makefile, based on the Asterisk Makefile, Coypright (C) 1999, Mark Spencer
#
# Copyright (C) 2005 Cytronics & Melware
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

.EXPORT_ALL_VARIABLES:

INSTALL_PREFIX=
ASTERISK_HEADER_DIR=$(INSTALL_PREFIX)/usr/include
ASTERISKVERSION=$(shell if [ -f .version ]; then cat .version; else if [ -d CVS ]; then if [ -f CVS/Tag ] ; then echo "CVS-`sed 's/^T//g' CVS/Tag`-`date +"%D-%T"`"; else echo "CVS-HEAD-`date +"%D-%T"`"; fi; fi; fi)


MODULES_DIR=$(INSTALL_PREFIX)/usr/lib/asterisk/modules

PROC=$(shell uname -m)

DEBUG=-g #-pg
INCLUDE=-I$(ASTERISK_HEADER_DIR)
CFLAGS=-pipe -fPIC -Wall -Wmissing-prototypes -Wmissing-declarations $(DEBUG) $(INCLUDE) -D_REENTRANT -D_GNU_SOURCE
CFLAGS+=-O6
CFLAGS+=$(shell if $(CC) -march=$(PROC) -S -o /dev/null -xc /dev/null >/dev/null 2>&1; then echo "-march=$(PROC)"; fi)
CFLAGS+=$(shell if uname -m | grep -q ppc; then echo "-fsigned-char"; fi)

CFLAGS+=-DASTERISKVERSION=\"$(ASTERISKVERSION)\"

LIBS=-ldl -lpthread -lm
CC=gcc
INSTALL=install

SHAREDOS=chan_capi.so app_capiHOLD.so app_capiRETRIEVE.so \
	app_capiECT.so app_capiMCID.so app_capiNoES.so app_capiFax.so

CFLAGS+=-Wno-missing-prototypes -Wno-missing-declarations

CFLAGS+=-DCRYPTO

all: config.h $(SHAREDOS)

clean:
	rm -f config.h
	rm -f *.so *.o

%.so : %.o
	$(CC) -shared -Xlinker -x -o $@ $<

config.h:
	./create_config.sh "$(ASTERISK_HEADER_DIR)"

chan_capi.so: chan_capi.o
	$(CC) -shared -Xlinker -x -o $@ chan_capi.o -lcapi20

install: all
	for x in $(SHAREDOS); do $(INSTALL) -m 755 $$x $(MODULES_DIR) ; done

config: all
	cp capi.conf $(INSTALL_PREFIX)/etc/asterisk/

samples: config

