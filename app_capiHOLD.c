/*
 * (CAPI*)
 *
 * An implementation of Common ISDN API 2.0 for Asterisk
 *
 * HOLD ... stop right there...that's close enough
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



static char *tdesc = "(CAPI*) HOLD";
static char *app = "capiHOLD";
static char *synopsis = "put the call on hold";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int capiHOLD_exec(struct ast_channel *chan, void *data)
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(chan);
	MESSAGE_EXCHANGE_ERROR Info;
	_cmsg	CMSG;
	char	fac[4];
	int	res = 0;
	struct localuser *u;

	LOCAL_USER_ADD(u);
	
	/* Do our thing here */
	
	while (i->state != CAPI_STATE_BCONNECTED) {
		usleep(10000);
	}

	fac[0] = 3;	/* len */
	fac[1] = 0x02;	/* this is a HOLD up */
	fac[2] = 0;
	fac[3] = 0;	

	FACILITY_REQ_HEADER(&CMSG,ast_capi_ApplID, get_ast_capi_MessageNumber(),0);
	FACILITY_REQ_PLCI(&CMSG) = i->PLCI;
	FACILITY_REQ_FACILITYSELECTOR(&CMSG) = 0x0003; /* sservices */
	FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = (char *)&fac;

	if ((Info = _capi_put_cmsg(&CMSG)) != 0) {
		LOCAL_USER_REMOVE(u);
		return Info;
	} else {
		ast_log(LOG_NOTICE,"sent FACILITY_REQ PLCI = %#x\n",
			i->PLCI);
	}

	i->state = CAPI_STATE_PUTTINGONHOLD;
	i->onholdPLCI= i->PLCI;

	while (i->state == CAPI_STATE_PUTTINGONHOLD) {
		usleep(10000);
	}

	if (i->onholdPLCI != 0) {
		ast_log(LOG_NOTICE,"PLCI = %#x is on hold now\n",
			i->onholdPLCI);
	} else {
		i->state = CAPI_STATE_BCONNECTED;
		ast_log(LOG_NOTICE,"PLCI = %#x did not go on hold. going on!\n",
			i->PLCI);
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
	return ast_register_application(app, capiHOLD_exec, synopsis, tdesc);
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

