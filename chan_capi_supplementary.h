/*
 * (CAPI*)
 *
 * An implementation of Common ISDN API 2.0 for Asterisk
 *
 * Copyright (C) 2006-2007 Cytronics & Melware
 *
 * Armin Schindler <armin@melware.de>
 * 
 * This program is free software and may be modified and 
 * distributed under the terms of the GNU Public License.
 */
 
#ifndef _PBX_CAPI_SUPP_H
#define _PBX_CAPI_SUPP_H

/*
 * prototypes
 */
extern void ListenOnSupplementary(unsigned controller);
extern void handle_facility_indication_supplementary(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i);

typedef enum {
	CCBSNR_TYPE_NULL = 0,
	CCBSNR_TYPE_CCBS,
	CCBSNR_TYPE_CCNR
} ccbsnrtype_t;

#define CCBSNR_AVAILABLE  1

struct ccbsnr_s {
	ccbsnrtype_t type;
	_cword id;
	unsigned int plci;
	unsigned int state;
	char context[AST_MAX_CONTEXT];
	char exten[AST_MAX_EXTENSION];
	int priority;
};

#endif
