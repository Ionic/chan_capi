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
extern void handle_facility_indication_supplementary(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i);

#endif
