/*
 * (CAPI*)
 *
 * An implementation of Common ISDN API 2.0 for
 * Asterisk / OpenPBX.org
 *
 * Copyright (C) 2005-2007 Cytronics & Melware
 * Copyright (C) 2007 Mario Goegel
 *
 * Armin Schindler <armin@melware.de>
 * Mario Goegel <m.goegel@gmx.de>
 *
 * This program is free software and may be modified and 
 * distributed under the terms of the GNU Public License.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
		
#include <asterisk/channel.h>
#include <asterisk/options.h>
#include "chan_capi20.h"
#include "chan_capi.h"
#include "chan_capi_qsig.h"

/* Handle Operation: 1.3.12.9.0-3		ECMA/ISDN/NAMEPRESENTATION */
void cc_qsig_op_ecma_isdn_namepres(struct cc_qsig_invokedata *invoke, struct capi_pvt *i)
{
	char callername[50];	/* ECMA defines max length to 50 */
	unsigned int namelength = 0;
	unsigned int namesetlength = 0;
	unsigned int charset = 1;
	unsigned int namepres;
	unsigned int nametype;
	unsigned int datalength;
	int myidx = 0;
	
	cc_verbose(1, 1, VERBOSE_PREFIX_4 "Handling Name Operation (id# %#x)\n", invoke->id);
	
	datalength = invoke->datalen;
	namepres = (invoke->data[myidx] & 0xF0);		/*	Name Presentation or Restriction */
	nametype = (invoke->data[myidx++] & 0x0F);		/*	Type of Name-Struct */
	
	switch (nametype) {
		case 0:		/* Simple Name */
		case 2:		/* [LENGTH] [STRING] */
			namelength = cc_qsig_asn1_get_string((unsigned char *)callername, sizeof(callername), &invoke->data[myidx]);
			callername[namelength] = 0;
			break;
		case 1:		/* Nameset */
		case 3:		/*  [LENGTH] [BIT-STRING] [LENGTH] [STRING] [INTEGER] [LENGTH] [VALUE] */
			namesetlength = invoke->data[myidx++];
			if (invoke->data[myidx++] == ASN1_OCTETSTRING) {
				/* should be so */
				namelength = cc_qsig_asn1_get_string((unsigned char *)callername, sizeof(callername), &invoke->data[myidx]);
				callername[namelength] = 0;
				myidx += invoke->data[myidx-1];		/* is this safe? */
			} else {
				cc_verbose(1, 1, VERBOSE_PREFIX_4 " Namestruct not ECMA conform (String expected)\n");
				break;
			}
			if (invoke->data[myidx++] == ASN1_INTEGER) {
				charset=cc_qsig_asn1_get_integer(invoke->data, &myidx);
			} else {
				cc_verbose(1, 1, VERBOSE_PREFIX_4 " Namestruct not ECMA conform (Integer expected)\n");
			}
			break;
		case 4:		/* Name not available */
			break;
		case 7:		/* Namepres. restricted NULL  - don't understand ECMA-164, Page 5 */
			break;
	}
	
	if (namelength > 0) {
		/* TODO: Maybe we do some charset conversions */
		i->owner->cid.cid_name = strdup(callername);
/*		cc_verbose(1, 1, VERBOSE_PREFIX_4 " callers name length: %i, \"%s\"\n", namelength, callername); */
	}
	
}

int cc_qsig_encode_ecma_name_invoke(unsigned char * buf, unsigned int *idx, struct cc_qsig_invokedata *invoke, struct capi_pvt *i)
{
	const unsigned char oid[] = {0x2b,0x0c,0x09,0x00};	/* 1.3.12.9.0 */
	int oid_len = sizeof(oid);
	unsigned char namebuf[50];
	unsigned char data[255];
	int dataidx = 0;
	int namelen = 0;
	/*TODO: write something */
	
	namelen = strlen(i->owner->cid.cid_name);
	
	if (namelen < 1) {	/* There's no name available, try to take Interface-Name */
		if (strlen(i->name) >= 1) {
			if (namelen > 50)
				namelen = 50;
			namelen = strlen(i->name);
			memcpy(namebuf, i->name, namelen);
		}
	} else {
		if (namelen > 50)
			namelen = 50;
		memcpy(namebuf, i->owner->cid.cid_name, namelen);
	}
	
	invoke->id = 1;
	invoke->descr_type = ASN1_OBJECTIDENTIFIER;
	invoke->oid_len = oid_len;
	memcpy(invoke->oid_bin, oid, oid_len);
	
	if (namelen>0) {
		data[dataidx++] = 0x80;	/* We send only simple Name, Namepresentation allowed */
		data[dataidx++] = namelen;
		memcpy(&data[dataidx], namebuf, namelen);
		dataidx += namelen;
	} else {
		data[dataidx++] = 0x84;	/* Name not available */
		data[dataidx++] = 0;
	}
	
	invoke->datalen = dataidx;
	memcpy(invoke->data, data, dataidx);
	
/*	qsig_add_invoke(buf, idx, invoke); */
			
	return 0;
}
