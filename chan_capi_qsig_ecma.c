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
#include <asterisk/pbx.h>
#include "chan_capi20.h"
#include "chan_capi.h"
#include "chan_capi_qsig.h"
#include "chan_capi_qsig_asn197ade.h"
#include "chan_capi_qsig_asn197no.h"


/* 
 * Handle Operation: 1.3.12.9.0-3		ECMA/ISDN/NAMEPRESENTATION 
 * 
 * This function decodes the namepresentation facility
 * The name will be copied in the cid.cid_name field of the asterisk channel struct
 *
 * parameters
 *	invoke	struct, which contains encoded data from facility
 *	i	is pointer to capi channel
 * returns
 * 	nothing
 */
void cc_qsig_op_ecma_isdn_namepres(struct cc_qsig_invokedata *invoke, struct capi_pvt *i)
{
	char callername[51];	/* ECMA defines max length to 50 */
	unsigned int namelength = 0;
	unsigned int datalength;
	int myidx = 0;
	
	cc_verbose(1, 1, VERBOSE_PREFIX_4 "Handling Name Operation (id# %#x)\n", invoke->id);

	datalength = invoke->datalen;
	
	myidx = cc_qsig_asn197no_get_name(callername, ASN197NO_NAME_STRSIZE, &namelength, &myidx, invoke->data );
	
	if (namelength > 0) {
		/* TODO: Maybe we do some charset conversions */
		i->owner->cid.cid_name = strdup(callername);
		cc_verbose(1, 1, VERBOSE_PREFIX_4 "  * received name (%i byte(s)): \"%s\"\n", namelength, callername); 
	}

	/* if there was an sequence tag, we have more informations here, but we will ignore it at the moment */
	
}

/* 
 * Encode Operation: 1.3.12.9.0-3		ECMA/ISDN/NAMEPRESENTATION 
 * 
 * This function encodes the namepresentation facility
 * The name will be copied from the cid.cid_name field of the asterisk channel struct.
 * We create an invoke struct with the complete encoded invoke.
 *
 * parameters
 *	buf 	is pointer to facility array, not used now
 *	idx	current idx in facility array, not used now
 *	invoke	struct, which contains encoded data for facility
 *	i	is pointer to capi channel
 * returns
 * 	always 0
 */
int cc_qsig_encode_ecma_name_invoke(unsigned char * buf, unsigned int *idx, struct cc_qsig_invokedata *invoke, struct capi_pvt *i, int nametype)
{
	const unsigned char oid[] = {0x2b,0x0c,0x09,0x00};	/* 1.3.12.9.0 */
	int oid_len = sizeof(oid);
	unsigned char namebuf[51];
	unsigned char data[255];
	int dataidx = 0;
	int namelen = 0;
	
	if (i->owner->cid.cid_name)
		namelen = strlen(i->owner->cid.cid_name);
	
	if (namelen < 1) {	/* There's no name available, try to take Interface-Name */
		if (i->name) {
			if (strlen(i->name) >= 1) {
				if (namelen > 50)
					namelen = 50;
				namelen = strlen(i->name);
				memcpy(namebuf, i->name, namelen);
			}
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
	
	/* HACK: */
	if (nametype)
		invoke->oid_bin[3] = 2;
	
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


/* 
 * Handle Operation: 1.3.12.9.21		ECMA/ISDN/LEG_INFORMATION2
 * 
 * This function decodes the LEG INFORMATION2 facility
 * The datas will be copied in the some Asterisk channel variables -> see README.qsig
 *
 * parameters
 *	invoke	struct, which contains encoded data from facility
 *	i	is pointer to capi channel
 * returns
 * 	nothing
 */
void cc_qsig_op_ecma_isdn_leginfo2(struct cc_qsig_invokedata *invoke, struct capi_pvt *i)
{
	
	unsigned int datalength;
	unsigned int seqlength = 0;
	int myidx = 0;
	
	unsigned int parameter = 0;
	unsigned int divCount = 0;
	unsigned int divReason = 0;
	unsigned int orgDivReason = 0;
	char tempstr[5];
	char divertNum[ASN197ADE_NUMDIGITS_STRSIZE+1];
	char origCalledNum[ASN197ADE_NUMDIGITS_STRSIZE+1];
	char divertName[ASN197NO_NAME_STRSIZE+1];
	char origCalledName[ASN197NO_NAME_STRSIZE+1];
	unsigned int temp = 0;
	unsigned int temp2 = 0;
	
	divertNum[0] = 0;
	origCalledNum[0] = 0;
	divertNum[0] = 0;
	divertName[0] = 0;
	origCalledName[0] = 0;
	
	cc_verbose(1, 1, VERBOSE_PREFIX_4 "Handling QSIG LEG INFO2 (id# %#x)\n", invoke->id);

	if (invoke->data[myidx++] != (ASN1_SEQUENCE | ASN1_TC_UNIVERSAL | ASN1_TF_CONSTRUCTED)) { /* 0x30 */
		/* We do not handle this, because it should start with an sequence tag */
		cc_verbose(1, 1, VERBOSE_PREFIX_4 "  * not Handling QSIG LEG INFO2 - not a sequence\n");
		return;
	}
	
	/* This facility is encoded as SEQUENCE */
	seqlength = invoke->data[myidx++];
	datalength = invoke->datalen;
	if (datalength < (seqlength+1)) {
		cc_verbose(1, 1, VERBOSE_PREFIX_4 "  * not Handling QSIG LEG INFO2 - buffer error\n");
		return;
	}
	
	if (invoke->data[myidx++] == ASN1_INTEGER) 
		divCount = cc_qsig_asn1_get_integer(invoke->data, &myidx);
	
	if (invoke->data[myidx++] == ASN1_ENUMERATED) 
		divReason = cc_qsig_asn1_get_integer(invoke->data, &myidx);
	
	while (myidx < datalength) {
		parameter = (invoke->data[myidx++] & 0x0f);
		cc_verbose(1, 1, VERBOSE_PREFIX_4 "  * Found parameter %i\n", parameter);
		switch (parameter) {
			case 0:
				myidx++;	/* Ignore Length of enumeration tag*/
				if (invoke->data[myidx++] == ASN1_ENUMERATED)
					orgDivReason = cc_qsig_asn1_get_integer(invoke->data, &myidx);
				break;
			case 1:
				temp = cc_qsig_asn197ade_get_partynumber(divertNum, ASN197ADE_NUMDIGITS_STRSIZE, &myidx, invoke->data);
				if (temp) {
					myidx += temp;
				}
				break;
			case 2:
				temp = cc_qsig_asn197ade_get_partynumber(origCalledNum, ASN197ADE_NUMDIGITS_STRSIZE, &myidx, invoke->data);
				if (temp) {
					myidx += temp;
				}
				break;
			case 3:
				/* Redirecting Name */
				myidx++;
				temp = cc_qsig_asn197no_get_name(divertName, ASN197NO_NAME_STRSIZE, &temp2, &myidx, invoke->data);
				if (temp) {
					myidx += temp;
				}
				break;
			case 4:
				/* origCalled Name */
				myidx++;
				temp = cc_qsig_asn197no_get_name(origCalledName, ASN197NO_NAME_STRSIZE, &temp2, &myidx, invoke->data);
				if (temp) {
					myidx += temp;
				}
				break;
		}
	}

	snprintf(tempstr, 5, "%i", divReason);
	pbx_builtin_setvar_helper(i->owner, "_QSIG_LI2_DIVREASON", tempstr);
	snprintf(tempstr, 5, "%i", orgDivReason);
	pbx_builtin_setvar_helper(i->owner, "_QSIG_LI2_ODIVREASON", tempstr);
	snprintf(tempstr, 5, "%i", divCount);
	pbx_builtin_setvar_helper(i->owner, "_QSIG_LI2_DIVCOUNT", tempstr);
	
	pbx_builtin_setvar_helper(i->owner, "_QSIG_LI2_DIVNUM", divertNum);
	pbx_builtin_setvar_helper(i->owner, "_QSIG_LI2_ODIVNUM", origCalledNum);
	pbx_builtin_setvar_helper(i->owner, "_QSIG_LI2_DIVNAME", divertName);
	pbx_builtin_setvar_helper(i->owner, "_QSIG_LI2_ODIVNAME", origCalledName);

	cc_verbose(1, 1, VERBOSE_PREFIX_4 "  * QSIG_LEG_INFO2: %i(%i), %ix %s->%s, %s->%s\n", divReason, orgDivReason, divCount, origCalledNum, divertNum, origCalledName, divertName);
	
	return;
	
}


/* 
 * Encode Operation: 1.3.12.9.12		ECMA/ISDN/CALLTRANSFER
 * 
 * This function encodes the call transfer facility
 *
 * We create an invoke struct with the complete encoded invoke.
 *
 * parameters
 *	buf 	is pointer to facility array, not used now
 *	idx	current idx in facility array, not used now
 *	invoke	struct, which contains encoded data for facility
 *	i	is pointer to capi channel
 *	param	is parameter from capicommand
 *	info	this facility is part of 2, 0 is facility 1, 1 is facility 2
 * returns
 * 	always 0
 */
void cc_qsig_encode_ecma_calltransfer(unsigned char * buf, unsigned int *idx, struct cc_qsig_invokedata *invoke, struct capi_pvt *i, char *param, int info)
{
	const unsigned char oid[] = {0x2b,0x0c,0x09,0xc};	/* 1.3.12.9.12 */
	char *cid, *ccanswer;
	int icanswer = 0;
	int cidlen = 0;
	int seqlen = 13;
	char c[255];
	int ix = 0;

	if (info) {
		cid = strsep(&param, "|");
		cidlen = strlen(cid);
		if (cidlen > 20)	/* HACK: stop action here, maybe we have invalid data */
			cidlen = 20;
	} else {
		char *tmp = strsep(&param, "|");
		tmp = NULL;
		cid = strsep(&param, "|");
		cidlen = strlen(cid);
		if (cidlen > 20)	/* HACK: stop action here, maybe we have invalid data */
			cidlen = 20;
		
		ccanswer = strsep(&param, "|");
		if (ccanswer[0])
			icanswer = ccanswer[0] - 0x30;
	}
	
	seqlen += cidlen;
	
	
	c[ix++] = ASN1_SEQUENCE | ASN1_TF_CONSTRUCTED;	/* start of SEQUENCE */
	c[ix++] = seqlen;
		
	c[ix++] = ASN1_ENUMERATED;					/* End Designation */
	c[ix++] = 1; /* length */
	c[ix++] = info;

	c[ix++] = ASN1_TC_CONTEXTSPEC | ASN1_TF_CONSTRUCTED;	/* val 2 - Source Caller ID struct */
	c[ix++] = 5 + cidlen;
	c[ix++] = ASN1_TC_CONTEXTSPEC;				/* CallerID */
	c[ix++] = cidlen;
	memcpy(&c[ix], cid, cidlen);
	ix += cidlen;
	c[ix++] = ASN1_ENUMERATED;					/* Screening Indicator */
	c[ix++] = 1; /* length */
	c[ix++] = 1; /* 01 = userProvidedVerifiedAndPassed    ...we hope so */
	
	c[ix++] = ASN1_ENUMERATED;			/* val 3 - wait for connect ? */
	c[ix++] = 1;
	c[ix++] = icanswer;
	
					/* end of SEQUENCE */
	/* there are optional data possible here */
	
	invoke->id = 12;
	invoke->descr_type = ASN1_OBJECTIDENTIFIER;
	invoke->oid_len = sizeof(oid);
	memcpy(invoke->oid_bin, oid, sizeof(oid));
	
	invoke->datalen = ix;
	memcpy(invoke->data, c, ix);
	cc_verbose(1, 1, VERBOSE_PREFIX_4 "  * QSIG_CT: %i->%s\n", info, cid);
	
}

/* 
 * Encode Operation: 1.3.12.9.99		ECMA/ISDN/SINGLESTEPCALLTRANSFER
 * 
 * This function encodes the single step call transfer facility
 *
 * We create an invoke struct with the complete encoded invoke.
 *
 * parameters
 *	buf 	is pointer to facility array, not used now
 *	idx	current idx in facility array, not used now
 *	invoke	struct, which contains encoded data for facility
 *	i	is pointer to capi channel
 *	param	is parameter from capicommand
 * returns
 * 	always 0
 */
void cc_qsig_encode_ecma_sscalltransfer(unsigned char * buf, unsigned int *idx, struct cc_qsig_invokedata *invoke, struct capi_pvt *i, char *param)
{
	const unsigned char oid[] = {0x2b,0x0c,0x09,0x63};	/* 1.3.12.9.99 */
	char *cidsrc, *ciddst;
	int srclen, dstlen;
	int seqlen = 12;
	char c[255];
	int ix = 0;

	cidsrc = strsep(&param, "|");
	srclen = strlen(cidsrc);
	if (srclen > 20)	/* HACK: stop action here, maybe we have invalid data */
		srclen = 20;
	
	ciddst = strsep(&param, "|");
	dstlen = strlen(ciddst);
	if (dstlen > 20)	/* HACK: stop action here, maybe we have invalid data */
		dstlen = 20;
	
	seqlen += srclen + dstlen;
	
	
	c[ix++] = ASN1_SEQUENCE | ASN1_TF_CONSTRUCTED;	/* start of SEQUENCE */
	c[ix++] = seqlen;
		
	c[ix++] = ASN1_TC_CONTEXTSPEC;		/* val 1 - Destination CallerID */
	c[ix++] = dstlen;
	memcpy(&c[ix], ciddst, dstlen);
	ix += dstlen;
	
	c[ix++] = ASN1_TC_CONTEXTSPEC | ASN1_TF_CONSTRUCTED;	/* val 2 - Source Caller ID struct */
	c[ix++] = 5 + srclen;
	c[ix++] = ASN1_TC_CONTEXTSPEC;				/* CallerID */
	c[ix++] = srclen;
	memcpy(&c[ix], cidsrc, srclen);
	ix += srclen;
	c[ix++] = ASN1_ENUMERATED;					/* Screening Indicator */
	c[ix++] = 1; /* length */
	c[ix++] = 1; /* 01 = userProvidedVerifiedAndPassed    ...we hope so */
	
	c[ix++] = ASN1_BOOLEAN;			/* val 3 - wait for connect ? */
	c[ix++] = 1;
	c[ix++] = 0;
	
	/* end of SEQUENCE */
	/* there are optional data possible here */
	
	invoke->id = 99;
	invoke->descr_type = ASN1_OBJECTIDENTIFIER;
	invoke->oid_len = sizeof(oid);
	memcpy(invoke->oid_bin, oid, sizeof(oid));
	
	invoke->datalen = ix;
	memcpy(invoke->data, c, ix);
	cc_verbose(1, 1, VERBOSE_PREFIX_4 "  * QSIG_SSCT: %s->%s\n", cidsrc, ciddst);
	
}

/* 
 * Handle Operation: 1.3.12.9.19		ECMA/ISDN/PATH REPLACEMENT PROPOSE
 * 
 * This function decodes the PATH REPLACEMENT PROPOSE facility
 * The datas will be copied in the some capi_pvt channel variables 
 *
 * parameters
 *	invoke	struct, which contains encoded data from facility
 *	i	is pointer to capi channel
 * returns
 * 	nothing
 */
void cc_qsig_op_ecma_isdn_prpropose(struct cc_qsig_invokedata *invoke, struct capi_pvt *i)
{
	
	unsigned int datalength;
	unsigned int seqlength = 0;
	int myidx = 0;
	/* TODO: write more code */
	
	char callid[4+1];
	char reroutingnr[ASN197ADE_NUMDIGITS_STRSIZE+1];
	int temp = 0;
	
	callid[0] = 0;
	reroutingnr[0] = 0;
	
	cc_verbose(1, 1, VERBOSE_PREFIX_4 "Handling QSIG PATH REPLACEMENT PROPOSE (id# %#x)\n", invoke->id);

	if (invoke->data[myidx++] != (ASN1_SEQUENCE | ASN1_TC_UNIVERSAL | ASN1_TF_CONSTRUCTED)) { /* 0x30 */
		/* We do not handle this, because it should start with an sequence tag */
		cc_verbose(1, 1, VERBOSE_PREFIX_4 "  * not Handling QSIG REPLACEMENT PROPOSE - not a sequence\n");
		return;
	}
	
	/* This facility is encoded as SEQUENCE */
	seqlength = invoke->data[myidx++];
	datalength = invoke->datalen;
	if (datalength < (seqlength+1)) {
		cc_verbose(1, 1, VERBOSE_PREFIX_4 "  * not Handling QSIG REPLACEMENT PROPOSE - buffer error\n");
		return;
	}
	
	if (invoke->data[myidx++] == ASN1_NUMERICSTRING) {
		int strsize;
		strsize = cc_qsig_asn1_get_string((unsigned char*)&callid, sizeof(callid), &invoke->data[myidx]);
		myidx += strsize +1;
	} else {
		cc_verbose(1, 1, VERBOSE_PREFIX_4 "  * not Handling QSIG REPLACEMENT PROPOSE - NUMERICSTRING expected\n");
		return;
	}
	
 	if (invoke->data[myidx++] == ASN1_TC_CONTEXTSPEC)
		temp = cc_qsig_asn1_get_string((unsigned char*)&reroutingnr, sizeof(reroutingnr), &invoke->data[myidx]);
	
	if (temp) {
		myidx += temp;
	} else {
		cc_verbose(1, 1, VERBOSE_PREFIX_4 "  * not Handling QSIG REPLACEMENT PROPOSE - partyNumber expected (%i)\n", myidx);
		return;
	}
	

	i->qsig_data.pr_propose_cid  = strdup(callid);
	i->qsig_data.pr_propose_pn = strdup(reroutingnr);
	
	cc_verbose(1, 1, VERBOSE_PREFIX_4 "  * QSIG_PATHREPLACEMENT_PROPOSE Call identity: %s, Party number: %s (%i)\n", callid, reroutingnr, temp);
	
	return;
}

/* 
 * Encode Operation: 1.3.12.9.19		ECMA/ISDN/PATH REPLACEMENT PROPOSE
 * 
 * This function encodes the path replacement propose
 *
 * We create an invoke struct with the complete encoded invoke.
 *
 * parameters
 *	buf 	is pointer to facility array, not used now
 *	idx	current idx in facility array, not used now
 *	invoke	struct, which contains encoded data for facility
 *	i	is pointer to capi channel
 *	param	is parameter from capicommand
 * returns
 * 	always 0
 */
void cc_qsig_encode_ecma_prpropose(unsigned char * buf, unsigned int *idx, struct cc_qsig_invokedata *invoke, struct capi_pvt *i, char *param)
{
	/* TODO: write code */
	const unsigned char oid[] = {0x2b,0x0c,0x09,0x13};	/* 1.3.12.9.99 */
	
	return;
}
