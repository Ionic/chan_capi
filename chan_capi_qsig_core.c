/*
 * (CAPI*)
 *
 * An implementation of Common ISDN API 2.0 for Asterisk
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
#include "chan_capi_utils.h"
#include "chan_capi_qsig.h"
#include "chan_capi_qsig_asn197ade.h"
#include "chan_capi_qsig_asn197no.h"

/*
 * Encodes an ASN.1 string
 */
unsigned int cc_qsig_asn1_add_string(unsigned char *buf, int *idx, char *data, int datalen)
{
	int myidx=*idx;
	
	if ((1 + datalen + (*idx) ) > sizeof(*buf)) {
		/* String exceeds buffer size */
		return -1;
	}
	
	buf[myidx++] = datalen;
	memcpy(&buf[myidx], data, datalen);
	myidx += 1 + datalen;
	
	*idx = myidx;
	return 0;
}

/*
 * Returns an string from ASN.1 encoded string
 */
unsigned int cc_qsig_asn1_get_string(unsigned char *buf, int buflen, unsigned char *data)
{
	int strsize;
	int myidx=0;
	
	strsize = data[myidx++];
	if (strsize > buflen)
		strsize = buflen - 1;
	memcpy(buf, &data[myidx], strsize);
	buf[strsize] = 0;
/*	cc_verbose(1, 1, VERBOSE_PREFIX_4 " get_string length %i\n", strsize); */
	return strsize;
}

/*
 * Encode ASN.1 Integer
 */
unsigned int cc_qsig_asn1_add_integer(unsigned char *buf, int *idx, int value)
{
	int myidx = *idx;
	int intlen = 1;
	
	if ((unsigned int)value > (unsigned int)0xFFFF)
		return -1;	/* no support at the moment */
	
	if (value > 255)
		intlen++;	/* we need 2 bytes */
	
	buf[myidx++] = ASN1_INTEGER;
	buf[myidx++] = intlen;
	if (intlen > 1)	{
		buf[myidx++] = (unsigned char)(value >> 8);
		buf[myidx++] = (unsigned char)(value - 0xff00);
	} else {
		buf[myidx++] = (unsigned char)value;
	}
	
	*idx = myidx;
	return 0;
}

/*
 * Returns an Integer from ASN.1 Encoded Integer
 */
unsigned int cc_qsig_asn1_get_integer(unsigned char *data, int *idx)
{	/* TODO: not conform with negative integers */
	int myidx = *idx;
	int intlen;
	int temp;
	
	intlen = data[myidx++];
	if ((intlen < 1) || (intlen > 2)) {  /* i don't know if there's a bigger Integer as 16bit -> read specs */
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "ASN1Decode: Size of ASN.1 Integer not supported: %i\n", intlen);
		*idx = myidx + intlen;
		return 0;
	}
	
	temp = (char)data[myidx++];
	if (intlen == 2) {
		temp=(temp << 8) + data[myidx++];
	}
	
	*idx = myidx;
	return temp;
}

/*
 * Returns an Human Readable OID from ASN.1 Encoded OID
 */
unsigned char *cc_qsig_asn1_oid2str(unsigned char *data, int size)
{
	/* TODO: Add code */
	
	
	return 0;
}


/*
 * Check if OID is ECMA-ISDN (1.3.12.9.*)
 */
signed int cc_qsig_asn1_check_ecma_isdn_oid(unsigned char *data, int len)
{
	/*	1.3			.12		.9 */
	if ((data[0] == 0x2B) && (data[1] == 0x0C) && (data[2] == 0x09)) 
		return 0;
	return -1;
}


/*
 * This function simply updates the length informations of the facility struct
 */
void cc_qsig_update_facility_length(unsigned char * buf, unsigned int idx)
{
	buf[0] = idx;
	buf[2] = idx-2;
}

/*
 * Create Invoke Struct
 */
int cc_qsig_build_facility_struct(unsigned char * buf, unsigned int *idx, int apdu_interpr, struct cc_qsig_nfe *nfe)
{
	int myidx = *idx;	/* we start with Index 1 - Byte 0 is Length of Facilitydataarray */
	if (!myidx)
		myidx++;
	
	buf[myidx++] = 0x1c;
	buf[myidx++] = 0;		/* Byte 2 length of Facilitydataarray */
	buf[myidx++] = COMP_TYPE_DISCR_SS;	/* QSIG Facility */
	/* TODO: Outsource following struct to an separate function */
	buf[myidx++] = COMP_TYPE_NFE;		/* Network Facility Extension */
	buf[myidx++] = 6;				/* NFE Size hardcoded - not good */
	buf[myidx++] = 0x80;			/* Source Entity */
	buf[myidx++] = 0x01;
	buf[myidx++] = 0x00;			/* End PINX hardcoded */
	buf[myidx++] = 0x82;			/* Dest. Entity */
	buf[myidx++] = 0x01;
	buf[myidx++] = 0x00;			/* End PINX hardcoded */
	buf[myidx++] = COMP_TYPE_APDU_INTERP;	/* How to interpret this APDU */
	buf[myidx++] = 0x01;			/* Length */
	buf[myidx++] = apdu_interpr;
						/* Here will follow now the Invoke */
	*idx = myidx;
	cc_qsig_update_facility_length(buf, myidx);
	return 0;
}


/*
 * Add invoke to buf
 */
int cc_qsig_add_invoke(unsigned char * buf, unsigned int *idx, struct cc_qsig_invokedata *invoke, struct capi_pvt *i)
{
	unsigned char oid1[] = {0x2b,0x0c,0x09,0x00};
	int myidx = *idx;
	int invlenidx;
	int result;
	
	buf[myidx++] = COMP_TYPE_INVOKE;
	invlenidx = myidx;	/* save the Invoke length index for later */
	buf[myidx++] = 0;
	
	result = cc_qsig_asn1_add_integer(buf, &myidx, invoke->id);
	if (result) {
		cc_log(LOG_ERROR, "QSIG: Cannot add invoke, identifier is not encoded!\n");
		return -1;
	}
	
	if (invoke->descr_type == -1) {
		switch (i->qsigfeat) {
			case QSIG_TYPE_ALCATEL_ECMA:
				invoke->descr_type = ASN1_OBJECTIDENTIFIER;
				/* Set ECMA/ETSI OID */
				oid1[3] = (unsigned char)invoke->type;
				invoke->oid_len = sizeof(oid1);
				memcpy(invoke->oid_bin, oid1, sizeof(oid1));
				break;
			case QSIG_TYPE_HICOM_ECMAV2:
				invoke->descr_type = ASN1_INTEGER;
				/* Leave type as it is */
				break;
			default: 
				/* INVOKE is not encoded */
				break;
		}
	}
		
		
	switch (invoke->descr_type) {
		case ASN1_INTEGER:
			result = cc_qsig_asn1_add_integer(buf, &myidx, invoke->type);
			if (result) {
				cc_log(LOG_ERROR, "QSIG: Cannot add invoke, identifier is not encoded!\n");
				return -1;
			}
			break;
		case ASN1_OBJECTIDENTIFIER:
			if ((invoke->oid_len < 1) || (invoke->oid_len > 20)) {
				cc_log(LOG_ERROR, "QSIG: Cannot add invoke, OID is too big!\n");
				return -1;
			}
			buf[myidx++] = ASN1_OBJECTIDENTIFIER;
			buf[myidx++] = invoke->oid_len;
			memcpy(&buf[myidx], invoke->oid_bin, invoke->oid_len);
			myidx += invoke->oid_len;
			break;
		default:
			cc_verbose(1, 1, VERBOSE_PREFIX_3 "QSIG: Unknown Invoke Type, not encoded (%i)\n", invoke->descr_type);
			return -1;
			break;
	}
	if (invoke->datalen > 0) {	/* may be no error, if there's no data */
		memcpy(&buf[myidx], invoke->data, invoke->datalen);
		myidx += invoke->datalen;
	}
	
	buf[invlenidx] = myidx - invlenidx - 1;
	cc_qsig_update_facility_length(buf, myidx - 1);
	*idx = myidx;

	return 0;
}


		
/*
 * Valid QSIG-Facility?
 * Returns 0 if not
 */
unsigned int cc_qsig_check_facility(unsigned char *data, int *idx, int *apduval, int protocol)
{
	int myidx = *idx;
	
	/* First byte after Facility Length */ 
	if (data[myidx] == (unsigned char)(0x80 | protocol)) {
		myidx++;
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "QSIG: Supplementary Services");
		if (data[myidx] == (unsigned char)COMP_TYPE_NFE) {
			myidx++;
			/* TODO: Check Entities? */
			myidx += data[myidx] + 1;
			*idx = myidx;
			/* cc_verbose(1, 1, VERBOSE_PREFIX_3 "CONNECT_IND (idc #1 %i)\n",idx); */
			cc_verbose(1, 1, ", has NFE struct");
		}
		if ((data[myidx] == (unsigned char)COMP_TYPE_APDU_INTERP)) {
			myidx++;
			myidx += data[myidx];
			*apduval = data[myidx++];
			/* TODO: implement real reject or clear call ? */
			*idx = myidx;
			/* cc_verbose(1, 1, ", has APDU %s", APDU_STR[*apduval]); */
		}
		cc_verbose(1, 1, ".\n");
		return 1;
	} else {
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "QSIG: received protocol 0x%#x not configured!\n", (data[myidx] ^= 0x80));
	}
	return 0;
}


/*
 * Is this an INVOKE component?
 * when not return -1, set idx to next byte (length of component?)
 *		*** check idx in this case, that we are not out of range - maybe we got an unknown component then
 * when it is an invoke, return invoke length and set idx to first byte of component
 *
 */
signed int cc_qsig_check_invoke(unsigned char *data, int *idx)
{
	int myidx = *idx;
	
	if (data[myidx++] == (unsigned char)COMP_TYPE_INVOKE) {
		/* is an INVOKE */
		*idx = myidx;		/* Set index to length byte of component */
/*		cc_verbose(1, 1, VERBOSE_PREFIX_4 "CONNECT_IND (Invoke Length %i)\n", data[myidx+1]); */
		return data[myidx + 1];	/* return component length */
	}
	*idx += data[myidx];	/* we can end here, if it is an Invoke Result or Error */
	return -1;			/* what to do now? got no Invoke */
}


/*
 * Get Invoke ID
 *	returns current index
 *	idx points to next byte in array
 */
signed int cc_qsig_get_invokeid(unsigned char *data, int *idx, struct cc_qsig_invokedata *invoke)
{
	int myidx;
	int invidtype = 0;
	int invlen = 0;
	int invoffset;
	int temp = 0;
	
	myidx = *idx;
	invoffset = myidx;
	invlen = data[myidx++];
	if (invlen > 0) {
		invoke->len = invlen;		/* set Length of Invoke struct */
		invoke->offset = invoffset;	/* offset in Facility Array, where the Invoke Data starts */
		invidtype = data[myidx++];	/* Get INVOKE Id Type */
		if (invidtype != ASN1_INTEGER) {
			cc_verbose(1, 1, VERBOSE_PREFIX_3 "QSIG: Unknown Invoke Identifier Type 0x%#x\n", invidtype);
			return -1;
		}
		temp = cc_qsig_asn1_get_integer(data, &myidx);
		invoke->id = temp;
 		*idx = myidx; 
	}
	return 0;
}


/*
 * fill the Invoke struct with all the invoke data
 */
signed int cc_qsig_fill_invokestruct(unsigned char *data, int *idx, struct cc_qsig_invokedata *invoke, int apduval)
{
	int myidx = *idx;
	int invoptyp;
	int temp;
	int temp2;
	int datalen;
	
	invoptyp = data[myidx++];		/* Invoke Operation Type 0x02=INTEGER, 0x06=OID */
	switch (invoptyp) {
		case ASN1_INTEGER:
			invoke->apdu_interpr = apduval;
			temp = cc_qsig_asn1_get_integer(data, &myidx);
			invoke->descr_type = ASN1_INTEGER;
			invoke->type = temp;
			temp2 = (invoke->len) + (invoke->offset) + 1;	/* Array End = Invoke Length + Invoke Offset +1 */
			datalen = temp2 - myidx;
					
			if (datalen > 255) {
				cc_verbose(1, 1, VERBOSE_PREFIX_3 "QSIG: Unsupported INVOKE Operation Size (max 255 Bytes): %i\n", datalen);
				datalen = 255;
			}
			
			invoke->datalen = datalen;
			memcpy(invoke->data, &data[myidx], datalen);	/* copy data of Invoke Operation */
			myidx = myidx + datalen;		/* points to next INVOKE component, if there's any */
			*idx = myidx;
			
			break;
			
		case ASN1_OBJECTIDENTIFIER:
			invoke->apdu_interpr = apduval;
			invoke->descr_type = ASN1_OBJECTIDENTIFIER;
			temp = data[myidx++];		/* Length of OID */
			if (temp > 20)  {
				cc_verbose(1, 1, VERBOSE_PREFIX_3 "QSIG: Unsupported INVOKE Operation OID Size (max 20 Bytes): %i\n", temp);
				temp = 20;
			}
			
			/* TODO: Maybe we decode the OID here and be verbose - have to write cc_qsig_asn1get_oid */
			
/*			cc_verbose(1, 1, VERBOSE_PREFIX_3 "CONNECT_IND (OID, Length %i)\n", temp); */
			invoke->oid_len = temp;
			memcpy(invoke->oid_bin, &data[myidx], temp);	/* Copy OID to separate array */
			myidx = myidx + temp;				/* Set index to next information */
			
			temp2 = (invoke->len) + (invoke->offset) + 1;	/* Array End = Invoke Length + Invoke Offset +1 */
			datalen = temp2 - myidx;
					
			if (datalen > 255) {
				cc_verbose(1, 1, VERBOSE_PREFIX_3 "QSIG: Unsupported INVOKE Operation Size (max 255 Bytes): %i\n", datalen);
				datalen = 255;
			}
			
/*			cc_verbose(1, 1, VERBOSE_PREFIX_3 "CONNECT_IND (OID, Datalength %i)\n",datalen); */
			invoke->datalen = datalen;
			memcpy(invoke->data, &data[myidx], datalen);	/* copy data of Invoke Operation */
			myidx = myidx + datalen;		/* points to next INVOKE component, if there's any */
			*idx = myidx;

			break;
			
		default:
			cc_verbose(1, 1, VERBOSE_PREFIX_3 "QSIG: Unknown INVOKE Operation Type: %i\n", invoptyp);
			
			temp2 = (invoke->len) + (invoke->offset) + 1;	/* Array End = Invoke Length + Invoke Offset +1 */
			datalen = temp2 - myidx;
					
			if (datalen > 255) {
				cc_verbose(1, 1, VERBOSE_PREFIX_3 "QSIG: Unsupported INVOKE Operation Size (max 255 Bytes): %i\n", datalen);
				datalen = 255;
			}
			
			*idx = datalen;	/* Set index to next INVOKE, if there's any */
			return -1;
			break;
	}
	return 0;	/* No problems */
	
}

/*
 * Identify an INVOKE and return our own Ident Integer (CCQSIG__*)
 */
signed int cc_qsig_identifyinvoke(struct cc_qsig_invokedata *invoke, int protocol)
{
	int invokedescrtype = 0;
	int datalen;
	
/*	cc_verbose(1, 1, VERBOSE_PREFIX_4 "CONNECT_IND (Ident Invoke %i)\n", invoke->descr_type); */

	switch (protocol) {
		case QSIG_TYPE_ALCATEL_ECMA:
			switch (invoke->descr_type) {
				case ASN1_INTEGER:
					invokedescrtype = 1;
					switch (invoke->type) {
						case 0:
							return CCQSIG__ECMA__NAMEPRES;
						case 4:
							return CCQSIG__ECMA__PRPROPOSE;
						case 21:
							return CCQSIG__ECMA__LEGINFO2;
						default:
							cc_verbose(1, 1, VERBOSE_PREFIX_4 "QSIG: Unhandled ECMA-ISDN QSIG INVOKE (%i)\n", invoke->type);
							return 0;
					}
					break;
				case ASN1_OBJECTIDENTIFIER:
					invokedescrtype = 2;
					datalen = invoke->oid_len;
					if ((datalen) == 4) {
						if (!cc_qsig_asn1_check_ecma_isdn_oid(invoke->oid_bin, datalen)) {
							switch (invoke->oid_bin[3]) {
								case 0:		/* ECMA QSIG Name Presentation */
									return CCQSIG__ECMA__NAMEPRES;
								case 4:
									return CCQSIG__ECMA__PRPROPOSE;
								case 21:
									return CCQSIG__ECMA__LEGINFO2;
								default:	/* Unknown Operation */
									cc_verbose(1, 1, VERBOSE_PREFIX_4 "QSIG: Unhandled ECMA-ISDN QSIG INVOKE (%i)\n", invoke->oid_bin[3]);
									return 0;
							}
						}
					}
					
					break;
				default:
					cc_verbose(1, 1, VERBOSE_PREFIX_3 "QSIG: Unidentified INVOKE OP\n");
					break;
			}
			break;
		case QSIG_TYPE_HICOM_ECMAV2:
			switch (invoke->descr_type) {
				case ASN1_INTEGER:
					invokedescrtype = 1;
					switch (invoke->type) {
						case 0:
							return CCQSIG__ECMA__NAMEPRES;
						case 4:
							return CCQSIG__ECMA__PRPROPOSE;
						case 21:
							return CCQSIG__ECMA__LEGINFO2;
						default:
							cc_verbose(1, 1, VERBOSE_PREFIX_4 "QSIG: Unhandled ISO QSIG INVOKE (%i)\n", invoke->type);
							return 0;
					}
					break;
				case ASN1_OBJECTIDENTIFIER:
					invokedescrtype = 2;
					datalen = invoke->oid_len;
					if ((datalen) == 4) {
						if (!cc_qsig_asn1_check_ecma_isdn_oid(invoke->oid_bin, datalen)) {
							switch (invoke->oid_bin[3]) {
								case 0:		/* ECMA QSIG Name Presentation */
									return CCQSIG__ECMA__NAMEPRES;
								case 4:
									return CCQSIG__ECMA__PRPROPOSE;
								case 21:
									return CCQSIG__ECMA__LEGINFO2;
									default:	/* Unknown Operation */
										cc_verbose(1, 1, VERBOSE_PREFIX_4 "QSIG: Unhandled ISO QSIG INVOKE (%i)\n", invoke->oid_bin[3]);
										return 0;
							}
						}
					}
					break;
				default:
					cc_verbose(1, 1, VERBOSE_PREFIX_3 "QSIG: Unidentified INVOKE OP\n");
					break;
			}
			break;
		default:
			break;
	}
	return 0;
	
}


/*
 *
 */
unsigned int cc_qsig_handle_invokeoperation(int invokeident, struct cc_qsig_invokedata *invoke, struct capi_pvt *i)
{
	switch (invokeident) {
		case CCQSIG__ECMA__NAMEPRES:
			cc_qsig_op_ecma_isdn_namepres(invoke, i);
			break;
		case CCQSIG__ECMA__PRPROPOSE:
			cc_qsig_op_ecma_isdn_prpropose(invoke, i);
			break;
		case CCQSIG__ECMA__LEGINFO2:
			cc_qsig_op_ecma_isdn_leginfo2(invoke, i);
			break;
		default:
			break;
	}
	return 0;
}

/*
 * Handles incoming Indications from CAPI
 */
unsigned int cc_qsig_handle_capiind(unsigned char *data, struct capi_pvt *i)
{
	int faclen0 = 0;
	int faclen = 0;
	int facidx = 2;
	int action_unkn_apdu;		/* What to do with unknown Invoke-APDUs (0=Ignore, 1=clear call, 2=reject APDU) */
	
	int invoke_len;			/* Length of Invoke APDU */
	unsigned int invoke_op = 0;	/* Invoke Operation ID */
	struct cc_qsig_invokedata invoke;
	int invoketmp1;
	
	
	if (!data) {
		return 0;
	}

	faclen0 = data[facidx-2];	/* Length of facility array - there may be more facilities encoded in this struct */
	faclen = data[facidx++];
	faclen += facidx;
// 	facidx++;
	while (facidx < faclen0) {
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "Checking Facility at index %i\n", facidx);
		switch (i->qsigfeat) {
			case QSIG_TYPE_ALCATEL_ECMA:
				if (cc_qsig_check_facility(data, &facidx, &action_unkn_apdu, Q932_PROTOCOL_ROSE)) {
					while (facidx < faclen) {
						cc_verbose(1, 1, VERBOSE_PREFIX_3 "Checking INVOKE at index %i\n", facidx);
						invoke_len=cc_qsig_check_invoke(data, &facidx);
						if (invoke_len>0) {
							if (cc_qsig_get_invokeid(data, &facidx, &invoke)==0) {
								invoketmp1=cc_qsig_fill_invokestruct(data, &facidx, &invoke, action_unkn_apdu);
								invoke_op=cc_qsig_identifyinvoke(&invoke, i->qsigfeat);
								cc_qsig_handle_invokeoperation(invoke_op, &invoke, i);
							}
						} else {
								/* Not an Invoke */
						}
					}
				}
				break;
			case QSIG_TYPE_HICOM_ECMAV2:
				if (cc_qsig_check_facility(data, &facidx, &action_unkn_apdu, Q932_PROTOCOL_EXTENSIONS)) {
					while (facidx < faclen) {
						invoke_len=cc_qsig_check_invoke(data, &facidx);
						if (invoke_len>0) {
							if (cc_qsig_get_invokeid(data, &facidx, &invoke)==0) {
								invoketmp1=cc_qsig_fill_invokestruct(data, &facidx, &invoke, action_unkn_apdu);
								invoke_op=cc_qsig_identifyinvoke(&invoke, i->qsigfeat);
								cc_qsig_handle_invokeoperation(invoke_op, &invoke, i);
							}
						} else {
							/* Not an Invoke */
						}
					}
				}
				break;
			default:
				cc_verbose(1, 1, VERBOSE_PREFIX_3 "Unknown QSIG protocol configured (%i)\n", i->qsigfeat);
				break;
		}
		
		if (facidx < faclen0) {	/* there may follow a new facility */
			if (data[facidx] == 0x1c) {
				cc_verbose(1, 1, VERBOSE_PREFIX_3 "Found another facility at index %i\n", facidx);
				facidx++;
				faclen = data[facidx++];
				faclen += facidx;
			} else {
				cc_verbose(1, 1, VERBOSE_PREFIX_3 "More data found in facility at index %i, but this is not an facility (%#x)\n", facidx, data[facidx]);
				facidx++; /* don't start an endlessloop */
			}
		}
	}
	cc_verbose(1, 1, VERBOSE_PREFIX_3 "Facility done at index %i from %i\n", facidx, faclen);
	return invoke_op;
}

/*
 * Handles incoming Facility Indications from CAPI
 */
unsigned int cc_qsig_handle_capi_facilityind(unsigned char *data, struct capi_pvt *i)
{
	int faclen = 0;
	int facidx = 0;
	int action_unkn_apdu;		/* What to do with unknown Invoke-APDUs (0=Ignore, 1=clear call, 2=reject APDU) */
	
	int invoke_len;			/* Length of Invoke APDU */
	unsigned int invoke_op;		/* Invoke Operation ID */
	struct cc_qsig_invokedata invoke;
	int invoketmp1;
	
	
	if (!data) {
		return 0;
	}
	faclen = data[facidx++];
	/*					cc_verbose(1, 1, VERBOSE_PREFIX_3 "CONNECT_IND (Got Facility IE, Length=%#x)\n", faclen); */
	while (facidx < faclen) {
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "Checking Facility at index %i\n", facidx);
		switch (i->qsigfeat) {
			case QSIG_TYPE_ALCATEL_ECMA:
				if (cc_qsig_check_facility(data, &facidx, &action_unkn_apdu, Q932_PROTOCOL_ROSE)) {
					/*						cc_verbose(1, 1, VERBOSE_PREFIX_3 "CONNECT_IND ROSE Supplementary Services (APDU Interpretation:  %i)\n", action_unkn_apdu); */
					while ((facidx-1)<faclen) {
						cc_verbose(1, 1, VERBOSE_PREFIX_3 "Checking INVOKE at index %i\n", facidx);
						invoke_len=cc_qsig_check_invoke(data, &facidx);
						if (invoke_len>0) {
							if (cc_qsig_get_invokeid(data, &facidx, &invoke)==0) {
								invoketmp1=cc_qsig_fill_invokestruct(data, &facidx, &invoke, action_unkn_apdu);
								invoke_op=cc_qsig_identifyinvoke(&invoke, i->qsigfeat);
								if (invoke_op) {
									cc_qsig_handle_invokeoperation(invoke_op, &invoke, i);
								} else {
// 									facidx += invoke_len;
									cc_verbose(1, 1, VERBOSE_PREFIX_3 "Invoke not identified!\n");
								}
							}
						} else {
							/* Not an Invoke */
						}
					}
				} else { /* kill endlessloop */
					facidx += faclen;
				}
				break;
			case QSIG_TYPE_HICOM_ECMAV2:
				if (cc_qsig_check_facility(data, &facidx, &action_unkn_apdu, Q932_PROTOCOL_EXTENSIONS)) {
					/*						cc_verbose(1, 1, VERBOSE_PREFIX_3 "CONNECT_IND ROSE Supplementary Services (APDU Interpretation:  %i)\n", action_unkn_apdu); */
					while ((facidx-1)<faclen) {
						invoke_len=cc_qsig_check_invoke(data, &facidx);
						if (invoke_len>0) {
							if (cc_qsig_get_invokeid(data, &facidx, &invoke)==0) {
								invoketmp1=cc_qsig_fill_invokestruct(data, &facidx, &invoke, action_unkn_apdu);
								invoke_op=cc_qsig_identifyinvoke(&invoke, i->qsigfeat);
								if (invoke_op) {
									cc_qsig_handle_invokeoperation(invoke_op, &invoke, i);
								} else {
// 									facidx += invoke_len;
									cc_verbose(1, 1, VERBOSE_PREFIX_3 "Invoke not identified!\n");
								}
							}	
						} else {
							/* Not an Invoke */
						}
					}
				} else { /* kill endlessloop */
					facidx += faclen;
				}
				break;
			default:
				cc_verbose(1, 1, VERBOSE_PREFIX_3 "Unknown QSIG protocol configured (%i)\n", i->qsigfeat);
				break;
		}
	}
	cc_verbose(1, 1, VERBOSE_PREFIX_3 "Facility done at index %i from %i\n", facidx, faclen);
	return 0;
}

static int identify_qsig_setup_callfeature(char *param)
{
	char *p = param;
	switch (*p) {
		case 't':
			cc_verbose(1, 1, "Call Transfer");
			p++;
			if (*p == 'r') {
				cc_verbose(1, 1, " on ALERT");
				return 2;
			} else {
				return 1;
			}
		default:
			cc_verbose(1, 1, "unknown (%c)\n", *p);
			break;
	}
	
	return 0;
}

/*
 * Handles outgoing Facilies on Call SETUP
 */
unsigned int cc_qsig_add_call_setup_data(unsigned char *data, struct capi_pvt *i, struct  ast_channel *c)
{
	/* TODO: Check buffers */
	struct cc_qsig_invokedata invoke;
	struct cc_qsig_nfe nfe;
	unsigned int dataidx = 0;
	
	const unsigned char xprogress[] = {0x1e,0x02,0xa0,0x90};
	char *p = NULL;
	char *pp = NULL;
	int add_externalinfo = 0;
			
	if ((p = pbx_builtin_getvar_helper(c, "QSIG_SETUP"))) {
		/* some special dial parameters */
		/* parse the parameters */
		while ((p) && (*p)) {
			switch (*p) {
				case 'X':	/* add PROGRESS INDICATOR for external calls*/
					cc_verbose(1, 1, VERBOSE_PREFIX_4 "Sending QSIG external PROGRESS IE.\n");
					add_externalinfo = 1;
 					pp = strsep (&p, "/");
 					pp = NULL;
					break;
				case 'C':
					cc_verbose(1, 1, VERBOSE_PREFIX_4 "QSIG Call Feature requested: ");
					p++;
					switch(identify_qsig_setup_callfeature(p)) {
						case 1: /* Call transfer */
							p++;
 							pp = strsep(&p, "/");
							if (!pp) {
								cc_log(LOG_WARNING, "QSIG Call Feature needs plci as parameter!\n");
							} else {
								i->qsig_data.calltransfer = 1;
								i->qsig_data.partner_plci = atoi(pp);
								cc_verbose(1, 1, " for plci %#x\n", i->qsig_data.partner_plci);
							}
							break;
						case 2: /* Call transfer on ring */
							p += 2;
 							pp = strsep(&p, "/");
							if (!pp) {
								cc_log(LOG_WARNING, "QSIG Call Feature needs plci as parameter!\n");
							} else {
								i->qsig_data.calltransfer_onring = 1;
								i->qsig_data.partner_plci = atoi(pp);
								cc_verbose(1, 1, " for plci %#x\n", i->qsig_data.partner_plci);
							}
							break;
						default:
 							pp = strsep(&p, "/");
							break;
					}
					pp = NULL;
					break;
				default:
					cc_log(LOG_WARNING, "Unknown parameter '%c' in QSIG_SETUP, ignoring.\n", *p);
					p++;
			}
		}
	}
	
/*mg:remember me	switch (i->doqsig) {*/
	cc_qsig_build_facility_struct(data, &dataidx, APDUINTERPRETATION_IGNORE, &nfe);
	cc_qsig_encode_ecma_name_invoke(data, &dataidx, &invoke, i, 0);
	cc_qsig_add_invoke(data, &dataidx, &invoke, i);
	
	if (add_externalinfo) {
		/* add PROGRESS INDICATOR for external calls*/
		memcpy(&data[dataidx], xprogress, sizeof(xprogress));
		data[0] += data[0] + sizeof(xprogress);
	}
/*	}*/
	return 0;
}


/*
 * Handles outgoing Facilies on capicommand
 */
unsigned int cc_qsig_do_facility(unsigned char *fac, struct  ast_channel *c, char *param, unsigned int factype, int info1)
{
	struct cc_qsig_invokedata invoke;
	struct cc_qsig_nfe nfe;
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	/* struct capi_pvt *ii = NULL; */
	unsigned int facidx = 0;
	
	cc_qsig_build_facility_struct(fac, &facidx, APDUINTERPRETATION_REJECT, &nfe);
	switch (factype) {
		case 12: /* ECMA-178 callTransfer */
			cc_qsig_encode_ecma_calltransfer(fac, &facidx, &invoke, i, param, info1);
			cc_qsig_add_invoke(fac, &facidx, &invoke, i);
			
			break;
		case 99: /* ECMA-300 simpleCallTransfer */
			cc_qsig_encode_ecma_sscalltransfer(fac, &facidx, &invoke, i, param);
			cc_qsig_add_invoke(fac, &facidx, &invoke, i);
			break;
		default:
			break;
	}
	
	return 0;
}

/*
 * Initiate a QSIG Call Transfer
 */
int pbx_capi_qsig_getplci(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	char buffer[10];

	snprintf(buffer, sizeof(buffer)-1, "%d", i->PLCI);
	cc_verbose(4, 1, VERBOSE_PREFIX_4 "QSIG_GETPLCI: %s\n", buffer);
	pbx_builtin_setvar_helper(c, "QSIG_PLCI", buffer);
	
	return 0;
}

/*
 * Initiate a QSIG Single Step Call Transfer
 */
int pbx_capi_qsig_ssct(struct ast_channel *c, char *param)
{
	unsigned char fac[CAPI_MAX_FACILITYDATAARRAY_SIZE];
	_cmsg		CMSG;
	struct capi_pvt *i = CC_CHANNEL_PVT(c);

	if (!param) { /* no data implies no Calling Number and Destination Number */
		cc_log(LOG_WARNING, "capi qsig_ssct requires source number and destination number\n");
		return -1;
	}

	cc_qsig_do_facility(fac, c, param, 99, 0);
	
	INFO_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(),0);
	INFO_REQ_PLCI(&CMSG) = i->PLCI;
	INFO_REQ_FACILITYDATAARRAY(&CMSG) = fac;
		
	_capi_put_cmsg(&CMSG);

	return 0;
}

/*
 * Initiate a QSIG Call Transfer
 */
int pbx_capi_qsig_ct(struct ast_channel *c, char *param)
{
	unsigned char fac[CAPI_MAX_FACILITYDATAARRAY_SIZE];
	_cmsg		CMSG;
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	struct capi_pvt *ii = NULL;
	unsigned int callmark;
	char *marker;

	if (!param) { /* no data implies no Calling Number and Destination Number */
		cc_log(LOG_WARNING, "capi qsig_ct requires call marker, source number, destination number and await_connect info\n");
		return -1;
	}

	marker = strsep(&param, "|");
	
	callmark = atoi(marker);
	cc_verbose(1, 1, VERBOSE_PREFIX_4 "  * QSIG_CT: using call marker %i(%s)\n", callmark, marker);
	
	for (ii = iflist; ii; ii = ii->next) {
		if (ii->qsig_data.callmark == callmark)
			break;
	}
	
	if (!ii) {
		cc_log(LOG_WARNING, "capi qsig_ct call marker not found!\n");
		return -1;
	}
	
	cc_qsig_do_facility(fac, c, param, 12, 0);
	
	INFO_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(),0);
	INFO_REQ_PLCI(&CMSG) = ii->PLCI;
	INFO_REQ_FACILITYDATAARRAY(&CMSG) = fac;
		
	_capi_put_cmsg(&CMSG);
	
	cc_qsig_do_facility(fac, c, param, 12, 1);
	
	INFO_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(),0);
	INFO_REQ_PLCI(&CMSG) = i->PLCI;
	INFO_REQ_FACILITYDATAARRAY(&CMSG) = fac;
		
	_capi_put_cmsg(&CMSG);

	return 0;
}

/*
 * Initiate a QSIG Call Transfer
 */
int pbx_capi_qsig_callmark(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);

	if (!param) { /* no data implies no Calling Number and Destination Number */
		cc_log(LOG_WARNING, "capi qsig_callmark requires an call identifier\n");
		return -1;
	}

	i->qsig_data.callmark = atoi(param);

	return 0;
}

/*
 * cleanup QSIG stuff on interface
 */
void interface_cleanup_qsig(struct capi_pvt *i)
{
	if (i->qsigfeat) {
		i->qsig_data.callmark = 0;
		i->qsig_data.partner_ch = NULL;
		i->qsig_data.calltransfer_active = 0;
		i->qsig_data.calltransfer_onring = 0;
		if (i->qsig_data.pr_propose_cid)
			free(i->qsig_data.pr_propose_cid);
		if (i->qsig_data.pr_propose_pn)
			free(i->qsig_data.pr_propose_pn);
		
	}
}

/*
 *  CAPI INFO_IND (QSIG part)
 */
void pbx_capi_qsig_handle_info_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	if (!i->qsigfeat)	/* Run only, if QSIG enabled */
		return;
	
	switch(INFO_IND_INFONUMBER(CMSG)) {
		case 0x0008:	/* Cause */
			break;
		case 0x0014:	/* Call State */
			break;
		case 0x0018:	/* Channel Identification */
			break;
		case 0x001c:	/*  Facility Q.932 */
			{
				unsigned int qsiginvoke;
				qsiginvoke = cc_qsig_handle_capi_facilityind( (unsigned char*) INFO_IND_INFOELEMENT(CMSG), i);
			}
			break;
		case 0x001e:	/* Progress Indicator */
			break;
		case 0x0027:	/*  Notification Indicator */
			break;
		case 0x0028:	/* DSP */
			break;
		case 0x0029:	/* Date/Time */
			break;
		case 0x0070:	/* Called Party Number */
			break;
		case 0x0074:	/* Redirecting Number */
			break;
		case 0x0076:	/* Redirection Number */
			break;
		case 0x00a1:	/* Sending Complete */
			break;
		case 0x4000:	/* CHARGE in UNITS */
			break;
		case 0x4001:	/* CHARGE in CURRENCY */
			break;
		case 0x8001:	/* ALERTING */
			/* TODO: some checks, if there's any work here */
			if (i->qsig_data.calltransfer_onring) {
				unsigned char fac[CAPI_MAX_FACILITYDATAARRAY_SIZE];
				struct capi_pvt *ii = find_interface_by_plci(i->qsig_data.partner_plci);

				_cmsg		CMSG3;

				i->qsig_data.calltransfer_onring = 0;

				if (ii) {
					cc_qsig_do_facility(fac, ii->owner, NULL, 12, 0);

					INFO_REQ_HEADER(&CMSG3, capi_ApplID, get_capi_MessageNumber(),0);
					INFO_REQ_PLCI(&CMSG3) = ii->PLCI;
					INFO_REQ_FACILITYDATAARRAY(&CMSG3) = fac;
					_capi_put_cmsg(&CMSG3);

					cc_qsig_do_facility(fac, i->owner, NULL, 12, 1);
		
					INFO_REQ_HEADER(&CMSG3, capi_ApplID, get_capi_MessageNumber(),0);
					INFO_REQ_PLCI(&CMSG3) = i->PLCI;
					INFO_REQ_FACILITYDATAARRAY(&CMSG3) = fac;
					_capi_put_cmsg(&CMSG3);
				} else {
					cc_log(LOG_WARNING, "Call Transfer failed - second channel not found (PLCI %#x)!\n", i->qsig_data.partner_plci);
				}
			}
			break;
		case 0x8002:	/* CALL PROCEEDING */
			break;
		case 0x8003:	/* PROGRESS */
			break;
		case 0x8005:	/* SETUP */
			break;
		case 0x8007:	/* CONNECT */
			break;
		case 0x800d:	/* SETUP ACK */
			break;
		case 0x800f:	/* CONNECT ACK */
			break;
		case 0x8045:	/* DISCONNECT */
			break;
		case 0x804d:	/* RELEASE */
			break;
		case 0x805a:	/* RELEASE COMPLETE */
			break;
		case 0x8062:	/* FACILITY */
			break;
		case 0x806e:	/* NOTIFY */
			break;
		case 0x807b:	/* INFORMATION */
			break;
		case 0x807d:	/* STATUS */
			break;
		default:
			break;
	}
	return;

}
