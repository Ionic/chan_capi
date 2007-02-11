/*
 *  (QSIG)
 *
 *  Implementation of QSIG extensions for CHAN_CAPI
 *  
 *  Copyright 2006-2007 (c) Mario Goegel
 *
 *  Mario Goegel <m.goegel@gmx.de>
 *
 * This program is free software and may be modified and
 * distributed under the terms of the GNU Public License.
 */

#ifndef PBX_QSIG_H
#define PBX_QSIG_H

#define QSIG_DISABLED		0x00
#define QSIG_ENABLED		0x01

#define QSIG_TYPE_DEFAULT	0x00		/* use only common features */
#define QSIG_TYPE_ALCATEL	0x01		/* use additional Alcatel features */
#define QSIG_TYPE_HICOM		0x02		/* use additional Hicom features */

#define Q932_PROTOCOL_ROSE			0x11	/* X.219 & X.229 */
#define Q932_PROTOCOL_CMIP			0x12	/* Q.941 */
#define Q932_PROTOCOL_ACSE			0x13	/* X.217 & X.227 */
#define Q932_PROTOCOL_GAT			0x16
#define Q932_PROTOCOL_EXTENSIONS	0x1F

#define COMP_TYPE_INVOKE	0xa1		/* Invoke component */
#define COMP_TYPE_DISCR_SS	0x91		/* Supplementary service descriptor - ROSE PROTOCOL */
#define COMP_TYPE_NFE		0xaa		/* Network Facility Extensions (ECMA-165) */
#define COMP_TYPE_APDU_INTERP	0x8b		/* APDU Interpration Type (0 DISCARD, 1 CLEARCALL-IF-UNKNOWN, 2 REJECT-APDU) */
#define COMP_TYPE_RETURN_RESULT	0xA2
#define COMP_TYPE_RETURN_ERROR	0xA3
#define COMP_TYPE_REJECT	0xA4
		
#define APDUINTERPRETATION_IGNORE	0x00
#define APDUINTERPRETATION_CLEARCALL	0x01
#define APDUINTERPRETATION_REJECT	0x02

		/* ASN.1 Identifier Octet - Data types */
#define ASN1_TYPE_MASK			0x1f
#define ASN1_BOOLEAN			0x01
#define ASN1_INTEGER			0x02
#define ASN1_BITSTRING			0x03
#define ASN1_OCTETSTRING		0x04
#define ASN1_NULL				0x05
#define ASN1_OBJECTIDENTIFIER	0x06
#define ASN1_OBJECTDESCRIPTOR	0x07
#define ASN1_EXTERN				0x08
#define ASN1_REAL				0x09
#define ASN1_ENUMERATED			0x0a
#define ASN1_EMBEDDEDPDV		0x0b
#define ASN1_UTF8STRING			0x0c
#define ASN1_RELATIVEOBJECTID	0x0d
		/* 0x0e & 0x0f are reserved for future ASN.1 editions */
#define ASN1_SEQUENCE			0x10
#define ASN1_SET				0x11
#define ASN1_NUMERICSTRING		0x12
#define ASN1_PRINTABLESTRING	0x13
#define ASN1_TELETEXSTRING		0x14
#define ASN1_IA5STRING			0x16
#define ASN1_UTCTIME			0x17
#define ASN1_GENERALIZEDTIME	0x18


#define CNIP_CALLINGNAME	0x00		/* Name-Types defined in ECMA-164 */
#define CNIP_CALLEDNAME		0x01
#define CNIP_CONNECTEDNAME	0x02
#define CNIP_BUSYNAME		0x03


#define CNIP_NAMEPRESALLOW	0x80
#define CNIP_NAMEPRESRESTR	0xA0
#define CNIP_NAMEPRESUNAVAIL	0xC0

#define CNIP_NAMEUSERPROVIDED	0x00		/* Name is User-provided, unvalidated */
#define CNIP_NAMEUSERPROVIDEDV	0x01		/* Name is User-provided and validated */
		
#define CCQSIG__ECMA__NAMEPRES	1000		/* Setting an own constant for ECMA Operation/Namepresentation, others will follow */

/*
 * INVOKE Data struct, contains data for further operations
 */
struct cc_qsig_invokedata {
	int len;		/* invoke length */
	int offset;		/* where does the invoke start in facility array */
	int id;			/* id from sent Invoke Number */
	int apdu_interpr;	/* What To Do with unknown Operation? */
	int descr_type;		/* component descriptor is of ASN.1 Datatype (0x02 Integer, 0x06 Object Identifier) */
	int type;		/* when component is Integer */
	int oid_len;
	unsigned char oid_bin[20];	/* when component is Object Identifier then save here the binary oid */
	int datalen;			/* invoke struct len */
	unsigned char data[255];	/* invoke */
};

/*
 * NFE Entity Address - contains destination informations for following INVOKE's
 */
struct cc_qsig_entityaddr {	/* In case of AnyPINX */
	int partynum;		/* private,public,etc a5=private */
	int ton;		/* Type of Number */
	unsigned char *num;		/* EntityNumber */
};
	
/*
 * Network Facility Extensions struct - to which pbx does the INVOKE belong to
 */
struct cc_qsig_nfe {
	int src_entity;		/* Call is coming from PBX (End|Any) */
	int dst_entity;		/* Call destination is */
	struct cc_qsig_entityaddr src_addr;	/* additional infos (PBX identifier) */
	struct cc_qsig_entityaddr dst_addr;	/* same here for destination */
};



/*****************  QSIG Core Functions */

/* Create an default QSIG Facility Array */
extern int cc_qsig_build_facility_struct(unsigned char * buf, unsigned int *idx, int apdu_interpr, struct cc_qsig_nfe *nfe);

extern int cc_qsig_add_invoke(unsigned char * buf, unsigned int *idx, struct cc_qsig_invokedata *invoke);

/* Returns an String from ASN.1 Encoded String */
extern unsigned int cc_qsig_asn1_get_string(unsigned char *buf, int buflen, unsigned char *data);

/* Returns an Integer from ASN.1 Encoded Integer */
extern unsigned int cc_qsig_asn1_get_integer(unsigned char *data, int *idx);

/* Returns an Human Readable OID from ASN.1 Encoded OID */
extern unsigned char cc_qsig_asn1_get_oid(unsigned char *data, int *idx);


/* Check if OID is ECMA-ISDN (1.3.12.9.*) */
extern signed int cc_qsig_asn1_check_ecma_isdn_oid(unsigned char *data, int len);

		
/* 
 * Valid QSIG-Facility?
 * Returns 0 if not 
 */
extern unsigned int cc_qsig_check_facility(unsigned char *data, int *idx, int *apduval);

/*
 * Is this an INVOKE component?
 * when not return -1, set idx to next byte (length of component?)
 *		*** check idx in this case, that we are not out of range - maybe we got an unknown component then
 * when it is an invoke, return invoke length and set idx to first byte of component
 */
extern signed int cc_qsig_check_invoke(unsigned char *data, int *idx);

/*
 * Get Invoke ID
 *	returns current index
 *	idx points to next byte in array
 */
extern signed int cc_qsig_get_invokeid(unsigned char *data, int *idx, struct cc_qsig_invokedata *invoke);

/* fill the Invoke struct with all the invoke data */
extern signed int cc_qsig_fill_invokestruct(unsigned char *data, int *idx, struct cc_qsig_invokedata *invoke, int apduval);

/*
 * Handles incoming Facilities on CAPI Indications
 */
extern unsigned int cc_qsig_handle_capiind(unsigned char *data, struct capi_pvt *i);

/*
 * Handles outgoing Facilies on Call SETUP
 */
extern unsigned int cc_qsig_add_call_setup_data(unsigned char *data, struct capi_pvt *i);

/* Identify an INVOKE and return our own Ident Integer (CCQSIG__*) */
extern signed int cc_qsig_identifyinvoke(struct cc_qsig_invokedata *invoke);

extern unsigned int cc_qsig_handle_invokeoperation(int invokeident, struct cc_qsig_invokedata *invoke, struct capi_pvt *i);



/* ECMA QSIG Functions */

/* Handle Operation: 1.3.12.9.0-3		ECMA/ISDN/NAMEPRESENTATION */
extern void cc_qsig_op_ecma_isdn_namepres(struct cc_qsig_invokedata *invoke, struct capi_pvt *i);

extern int cc_qsig_encode_ecma_name_invoke(unsigned char * buf, unsigned int *idx, struct cc_qsig_invokedata *invoke, struct capi_pvt *i);

#endif
