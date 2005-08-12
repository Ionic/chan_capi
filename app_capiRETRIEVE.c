/*
 * (CAPI*)
 *
 * An implementation of Common ISDN API 2.0 for Asterisk
 *
 * RETRIEVE
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
#include <capi20.h>
#include "chan_capi_pvt.h"
#include "chan_capi_app.h"



static char *tdesc = "(CAPI*) RETRIEVE";
static char *app = "capiRETRIEVE";
static char *synopsis = "retrieve the call that is on hold";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int capiRETRIEVE_exec(struct ast_channel *chan, void *data)
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(chan);
	MESSAGE_EXCHANGE_ERROR Info;
	_cmsg	CMSG;
	char	fac[4];
	int	res = 0;
	struct localuser *u;

	if (i->onholdPLCI <= 0) {
		ast_log(LOG_WARNING, "no call on hold to retrieve!\n");
		return -1;
	}

	LOCAL_USER_ADD(u);
	
	/* Do our thing here */

	fac[0] = 3;	/* len */
	fac[1] = 0x03;	/* retrieve */
	fac[2] = 0x00;
	fac[3] = 0;	

	FACILITY_REQ_HEADER(&CMSG,ast_capi_ApplID, get_ast_capi_MessageNumber(),0);
	FACILITY_REQ_PLCI(&CMSG) = i->onholdPLCI;
	FACILITY_REQ_FACILITYSELECTOR(&CMSG) = 0x0003; /* sservices */
	FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = (char *)&fac;

	if ((Info = _capi_put_cmsg(&CMSG)) != 0) {
    		LOCAL_USER_REMOVE(u);
		return Info;
	} else {
		i->state = CAPI_STATE_RETRIEVING;
		ast_log(LOG_NOTICE,"sent FACILITY_REQ PLCI = %#x\n",i->onholdPLCI);
	}

	while (i->state == CAPI_STATE_RETRIEVING) {
		usleep(10000);
	}

	/* send a CONNECT_B3_REQ */
	memset(&CMSG, 0, sizeof(_cmsg));
	CONNECT_B3_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(),0);
	CONNECT_B3_REQ_PLCI(&CMSG) = i->PLCI;
	if ((Info = _capi_put_cmsg(&CMSG)) == 0) {
		ast_log(LOG_NOTICE,"sent CONNECT_B3_REQ (PLCI=%#x)\n",
			i->PLCI);
	}

	while (i->state == CAPI_STATE_CONNECTED) {
		usleep(10000);
	}
	ast_log(LOG_NOTICE,"retrieved PLCI = %#x\n", i->PLCI);
    
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
	return ast_register_application(app, capiRETRIEVE_exec, synopsis, tdesc);
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

