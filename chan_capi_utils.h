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
 
#ifndef _PBX_CAPI_UTILS_H
#define _PBX_CAPI_UTILS_H

/*
 * prototypes
 */
extern char *capi_info_string(unsigned int info);
extern void show_capi_info(struct capi_pvt *i, _cword info);

#endif
