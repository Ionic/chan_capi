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
unsigned char cc_qsig_asn1_get_oid(unsigned char *data, int *idx)
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
	int myidx = 1;	/* we start with Index 1 - Byte 0 is Length of Facilitydataarray */
	
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
int cc_qsig_add_invoke(unsigned char * buf, unsigned int *idx, struct cc_qsig_invokedata *invoke)
{
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
	
	buf[invlenidx] = myidx-1;
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
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "QSIG: Supplementary Services\n");
		if (data[myidx++] == (unsigned char)COMP_TYPE_NFE) {
			/* Todo: Check Entities? */
			myidx = myidx + data[myidx] + 1;
			/* cc_verbose(1, 1, VERBOSE_PREFIX_3 "CONNECT_IND (idc #1 %i)\n",idx); */
			if ((data[myidx++] == (unsigned char)COMP_TYPE_APDU_INTERP)) {
				myidx = myidx + data[myidx];
				*apduval = data[myidx];
				/* ToDo: implement real reject or clear call ? */
				*idx = ++myidx;
				return 1;
			}
		}
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
	
	if (data[myidx] == (unsigned char)COMP_TYPE_INVOKE) {
		/* is an INVOKE_IDENT */
		*idx = myidx + 1;		/* Set index to length byte of component */
/*		cc_verbose(1, 1, VERBOSE_PREFIX_4 "CONNECT_IND (Invoke Length %i)\n", data[myidx+1]); */
		return data[myidx + 1];	/* return component length */
	}
	*idx = ++myidx;
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
/*		*idx += invlen + 1; */
/*		cc_verbose(1, 1, VERBOSE_PREFIX_3 "CONNECT_IND (Invoke Identifier %#x)\n", temp); */
/*		*idx=myidx; 	/* Set by cc_qsig_asn1get_integer */
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
			/*myidx++;*/				/* component length */
			/*datalen=data[myidx++];		/* maybe correct, better we calculate the datalength */
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
					break;
				case ASN1_OBJECTIDENTIFIER:
					invokedescrtype = 2;
					datalen = invoke->oid_len;
					if ((datalen) == 4) {
						if (!cc_qsig_asn1_check_ecma_isdn_oid(invoke->oid_bin, datalen)) {
							switch (invoke->oid_bin[3]) {
								case 0:		/* ECMA QSIG Name Presentation */
									return CCQSIG__ECMA__NAMEPRES;
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
						case 21:
							return CCQSIG__ECMA__LEGINFO2;
						default:
							cc_verbose(1, 1, VERBOSE_PREFIX_4 "QSIG: Unhandled QSIG INVOKE (%i)\n", invoke->type);
							return 0;
					}
					break;
				case ASN1_OBJECTIDENTIFIER:
					invokedescrtype = 2;
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
	int faclen = 0;
	int facidx = 2;
	int action_unkn_apdu;		/* What to do with unknown Invoke-APDUs (0=Ignore, 1=clear call, 2=reject APDU) */
	
	int invoke_len;			/* Length of Invoke APDU */
	unsigned int invoke_op;		/* Invoke Operation ID */
	struct cc_qsig_invokedata invoke;
	int invoketmp1;
	
	
	if (data) {
		faclen=data[facidx-2];
/*					cc_verbose(1, 1, VERBOSE_PREFIX_3 "CONNECT_IND (Got Facility IE, Length=%#x)\n", faclen); */
		facidx++;
		while (facidx < faclen) {
			cc_verbose(1, 1, VERBOSE_PREFIX_3 "Checking Facility at index %i\n", facidx);
			switch (i->qsigfeat) {
				case QSIG_TYPE_ALCATEL_ECMA:
					if (cc_qsig_check_facility(data, &facidx, &action_unkn_apdu, Q932_PROTOCOL_ROSE)) {
	/*						cc_verbose(1, 1, VERBOSE_PREFIX_3 "CONNECT_IND ROSE Supplementary Services (APDU Interpretation:  %i)\n", action_unkn_apdu); */
						while ((facidx-1)<faclen) {
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
						/*						cc_verbose(1, 1, VERBOSE_PREFIX_3 "CONNECT_IND ROSE Supplementary Services (APDU Interpretation:  %i)\n", action_unkn_apdu); */
						while ((facidx-1)<faclen) {
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
		}
	}
	cc_verbose(1, 1, VERBOSE_PREFIX_3 "Facility done at index %i from %i\n", facidx, faclen);
	return 0;
}

/*
 * Handles outgoing Facilies on Call SETUP
 */
unsigned int cc_qsig_add_call_setup_data(unsigned char *data, struct capi_pvt *i)
{
	/* TODO: Check buffers */
	struct cc_qsig_invokedata invoke;
	struct cc_qsig_nfe nfe;
	unsigned int dataidx;
	
/*mg:remember me	switch (i->doqsig) {*/
	cc_qsig_build_facility_struct(data, &dataidx, APDUINTERPRETATION_IGNORE, &nfe);
	cc_qsig_encode_ecma_name_invoke(data, &dataidx, &invoke, i);
	cc_qsig_add_invoke(data, &dataidx, &invoke);
/*	}*/
	return 0;
}

