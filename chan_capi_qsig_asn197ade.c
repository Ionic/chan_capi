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

/*
 *	Decoding of addressing-data-elements from asn1-97
 */
 
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
		
#include <asterisk/channel.h>
#include <asterisk/options.h>
#include "chan_capi20.h"
#include "chan_capi.h"
#include "chan_capi_qsig.h"
#include "chan_capi_qsig_asn197ade.h"

/*
 * Returns an "Party Number" from an string, encoded as in addressing-data-elements-asn1-97
 *	data should be a buffer with max. 20 bytes, according to spec
 *  return:
 *	index counter
 */
unsigned int cc_qsig_asn197ade_get_partynumber(char *buf, int buflen, int *idx, unsigned char *data)
{
	int myidx = *idx;
	int datalength;
	int numtype;
	
	datalength = data[myidx++];
	
	if (!datalength) {
		return 0;
	}
	
	numtype = (data[myidx++] & 0x0F);	/* defines type of Number: numDigits, publicPartyNum, nsapEncNum, dataNumDigits */
	
	/* cc_verbose(1, 1, VERBOSE_PREFIX_4 " * num type %i\n", numtype);  */
	switch (numtype){
		case 0:
			if (data[myidx++] > 0)	/* length of this context data */
				if (data[myidx++] == ASN1_TC_CONTEXTSPEC)
					myidx += cc_qsig_asn197ade_get_numdigits(buf, buflen, &myidx, data) + 1;
			break;
		case 1:			/* publicPartyNumber (E.164) not supported yet */
			return 0;
			break;
		case 2:			/* NsapEncodedNumber (some ATM stuff) not supported yet */
			return 0;
			break;
		case 3:
			if (data[myidx++] > 0)	/* length of this context data */
				if (data[myidx++] == ASN1_TC_CONTEXTSPEC)
					myidx += cc_qsig_asn197ade_get_numdigits(buf, buflen, &myidx, data) + 1;
			break;
	};
	return myidx - *idx;
}

/*
 * Returns an string from ASN.1 encoded string
 */
unsigned int cc_qsig_asn197ade_get_numdigits(char *buf, int buflen, int *idx, unsigned char *data)
{
	int strsize;
	int myidx = *idx;
	
	strsize = data[myidx++];
	if (strsize > buflen)	
		strsize = buflen;
	memcpy(buf, &data[myidx], strsize);
	buf[strsize] = 0;
	
/*	cc_verbose(1, 1, VERBOSE_PREFIX_4 " * string length %i\n", strsize); */
	return strsize;
}
