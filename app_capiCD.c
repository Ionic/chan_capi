/*
 * (CAPI*)
 *
 * An implementation of Common ISDN API 2.0 for Asterisk
 *
 * Call Deflection, inspired by capircvd by Alexander Brickwedde
 *
 * Copyright (C) 2005 Cytronics & Melware
 *
 * Armin Schindler <armin@melware.de>
 * 
 * Reworked, but based on the work of
 * Copyright (C) 2002-2005 Junghanns.NET GmbH
 *
 * Klaus-Peter Junghanns <kapejod@ns1.jnetdns.de>
 *
 * This program is free software and may be modified and 
 * distributed under the terms of the GNU Public License.
 */

#include "config.h"

#include <asterisk/file.h>
#include <asterisk/logger.h>
#include <asterisk/channel.h>
#ifndef CC_AST_HAVE_TECH_PVT
#include <asterisk/channel_pvt.h>
#endif
#include <asterisk/pbx.h>
#include <asterisk/module.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include <pthread.h>
#include <linux/capi.h>
#include <capi20.h>
#include "chan_capi_pvt.h"
#include "chan_capi_app.h"


static char *tdesc = "(CAPI*) Call Deflection, the magic thing.";
static char *app = "capiCD";
static char *synopsis = "call deflection";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int capiCD_exec(struct ast_channel *chan, void *data)
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(chan);
	MESSAGE_EXCHANGE_ERROR Info;
	_cmsg	CMSG;
	char	bchaninfo[1];
	char	fac[60];
	int	res = 0;
	int	ms = 3000;
	struct localuser *u;

	if (!data) {
		ast_log(LOG_WARNING, "cd requires an argument (destination phone number)\n");
		return -1;
	}
	
	LOCAL_USER_ADD(u);
	
	/* Do our thing here */

	if ((i->state == CAPI_STATE_CONNECTED) ||
	    (i->state == CAPI_STATE_BCONNECTED)) {
		ast_log(LOG_ERROR, "call deflection does not work with calls that are already connected!\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}
	
	/* wait until the channel is alerting, so we dont drop the call and interfer with msgs */
	while ((ms > 0) && (i->state != CAPI_STATE_ALERTING)) {
		sleep(100);
		ms -= 100;
	}

	/* make sure we hang up correctly */
	i->state = CAPI_STATE_CONNECTPENDING;

	fac[0] = 0;	/* len */
	fac[1] = 0;	/* len */
	fac[2] = 0x01;	/* Use D-Chan */
	fac[3] = 0;	/* Keypad len */
	fac[4] = 31;	/* user user data? len = 31 = 29 + 2 */
	fac[5] = 0x1c;	/* magic? */
	fac[6] = 0x1d;	/* strlen destination + 18 = 29 */
	fac[7] = 0x91;	/* .. */
	fac[8] = 0xA1;
	fac[9] = 0x1A;	/* strlen destination + 15 = 26 */
	fac[10] = 0x02;
	fac[11] = 0x01;
	fac[12] = 0x70;
	fac[13] = 0x02;
	fac[14] = 0x01;
	fac[15] = 0x0d;
	fac[16] = 0x30;
	fac[17] = 0x12;	/* strlen destination + 7 = 18 */
	fac[18] = 0x30;	/* ...hm 0x30 */
	fac[19] = 0x0d;	/* strlen destination + 2 */
	fac[20] = 0x80;	/* CLIP */
	fac[21] = 0x0b;	/* strlen destination */
	fac[22] = 0x01;	/* destination start */
	fac[23] = 0x01;	/* */  
	fac[24] = 0x01;	/* */  
	fac[25] = 0x01;	/* */  
	fac[26] = 0x01;	/* */
	fac[27] = 0x01;	/* */
	fac[28] = 0x01;	/* */
	fac[29] = 0x01;	/* */
	fac[30] = 0x01;	/* */
	fac[31] = 0x01;	/* */
	fac[32] = 0x01;	/* */
	fac[33] = 0x01;	/* 0x01 = sending complete */
	fac[34] = 0x01;
	fac[35] = 0x01;
				   
	memcpy((unsigned char *)fac + 22, data, strlen(data));
	
	fac[22 + strlen(data)] = 0x01;	/* fill with 0x01 if number is only 6 numbers (local call) */
	fac[23 + strlen(data)] = 0x01;
	fac[24 + strlen(data)] = 0x01;
	fac[25 + strlen(data)] = 0x01;
	fac[26 + strlen(data)] = 0x01;
     
	fac[6] = 18 + strlen(data);
	fac[9] = 15 + strlen(data);
	fac[17] = 7 + strlen(data);
	fac[19] = 2 + strlen(data);
	fac[21] = strlen(data);

	bchaninfo[0] = 0x1;
	
	INFO_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
	INFO_REQ_CONTROLLER(&CMSG) = i->controller;
	INFO_REQ_PLCI(&CMSG) = i->PLCI;
	INFO_REQ_BCHANNELINFORMATION(&CMSG) = (unsigned char*)bchaninfo; /* use D-Channel */
	INFO_REQ_KEYPADFACILITY(&CMSG) = 0;
	INFO_REQ_USERUSERDATA(&CMSG) = 0;
	INFO_REQ_FACILITYDATAARRAY(&CMSG) = (unsigned char*)fac + 4;

	if ((Info = _capi_put_cmsg(&CMSG)) != 0) {
		res = (int)Info;
	} else {
		if (capidebug) {
			ast_log(LOG_NOTICE, "sent INFO_REQ PLCI = %#x\n",
				i->PLCI);
		}
	}

	LOCAL_USER_REMOVE(u);
	return res;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	return ast_unregister_application(app);
}

int load_module(void)
{
	return ast_register_application(app, capiCD_exec, synopsis, tdesc);
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	int res;
	STANDARD_USECOUNT(res);
	return res;
}

char *key()
{
	return ASTERISK_GPL_KEY;
}
