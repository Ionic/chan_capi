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

#define CCBSNR_TYPE_CCBS 1
#define CCBSNR_TYPE_CCNR 2

#define CCBSNR_AVAILABLE  1
#define CCBSNR_REQUESTED  2
#define CCBSNR_ACTIVATED  3

struct ccbsnr_s {
	char type;
	_cword id;
	unsigned int plci;
	unsigned int state;
	unsigned int handle;
	_cword mode;
	_cword rbref;
	char partybusy;
	char context[AST_MAX_CONTEXT];
	char exten[AST_MAX_EXTENSION];
	int priority;
	time_t age;
	struct ccbsnr_s *next;
};

/*
 * prototypes
 */
extern void ListenOnSupplementary(unsigned controller);
extern int handle_facility_indication_supplementary(
	_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i);
extern void handle_facility_confirmation_supplementary(
	_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i);
extern int pbx_capi_ccbs(struct ast_channel *c, char *data);
extern int pbx_capi_ccbsstop(struct ast_channel *c, char *data);
extern int pbx_capi_ccpartybusy(struct ast_channel *c, char *data);
extern void cleanup_ccbsnr(void);

#endif
