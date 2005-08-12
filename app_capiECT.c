/*
 * (CAPI*)
 *
 * An implementation of Common ISDN API 2.0 for Asterisk
 *
 * ECT transfer the held call 
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
#include <asterisk/say.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include <pthread.h>
#include <capi20.h>
#include "chan_capi_pvt.h"
#include "chan_capi_app.h"


static char *tdesc = "(CAPI*) ECT";
static char *app = "capiECT";
static char *synopsis = "transfer the call that is on hold";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;


static int capiECT_exec(struct ast_channel *chan, void *data)
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(chan);
	MESSAGE_EXCHANGE_ERROR Info;
	_cmsg	CMSG;
	char	fac[8];
	int res = 0;
	struct localuser *u;
	char *ecodes = "*#";

	if (!data) {
		ast_log(LOG_WARNING, "ECT requires an argument (destination phone number)\n");
		return -1;
	}
	
	if (i->onholdPLCI <= 0) {
		ast_log(LOG_WARNING, "no call on hold that could be transfered\n");
		return -1;
	}

	LOCAL_USER_ADD(u);
	
	/* Do our thing here */
	
	ast_log(LOG_NOTICE, "ECT to %s\n", (char *)data);
	capi_call(chan, data, 0);

	while ((i->state != CAPI_STATE_BCONNECTED) && (i->onholdPLCI != 0)) {
		usleep(10000);
	}

	if (i->state == CAPI_STATE_BCONNECTED) {
		ast_log(LOG_NOTICE,"call was answered\n");

		capi_detect_dtmf(chan, 1);

		/* put the stuff to play announcement message here --->   <----- */
		res = ast_say_digit_str(chan, i->cid, ecodes, chan->language);
		if ( res == '#') {
			ast_log(LOG_NOTICE, "res = %d\n", res);
			/* user pressed #, hangup */
			/* first the holded user */
			/* ast_exec("capi RETRIEVE",chan); */

			DISCONNECT_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
			DISCONNECT_REQ_PLCI(&CMSG) = i->onholdPLCI;

			if ((Info = _capi_put_cmsg(&CMSG)) == 0) {
				ast_log(LOG_NOTICE, "sent DISCONNECT_REQ PLCI=%#x\n",
					i->onholdPLCI);
			}
		
			/* then the destination */

			DISCONNECT_B3_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
			DISCONNECT_B3_REQ_NCCI(&CMSG) = i->NCCI;

			if ((Info = _capi_put_cmsg(&CMSG)) == 0) {
				ast_log(LOG_NOTICE, "sent DISCONNECT_B3_REQ NCCI=%#x\n",
					i->NCCI);
			}

			/* wait for the B3 layer to go down */
			while (i->state != CAPI_STATE_CONNECTED) {
				usleep(10000);
			}

			DISCONNECT_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
			DISCONNECT_REQ_PLCI(&CMSG) = i->PLCI;

			if ((Info = _capi_put_cmsg(&CMSG)) == 0) {
				ast_log(LOG_NOTICE, "sent DISCONNECT_REQ PLCI=%#x\n",
					i->PLCI);
			}
		
			LOCAL_USER_REMOVE(u);
			return -1;
		} else {
			/* now drop the bchannel */
			DISCONNECT_B3_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
			DISCONNECT_B3_REQ_NCCI(&CMSG) = i->NCCI;

			if ((Info = _capi_put_cmsg(&CMSG)) == 0) {
				ast_log(LOG_NOTICE, "sent DISCONNECT_B3_REQ NCCI=%#x\n",i->NCCI);
			}

			/* wait for the B3 layer to go down */
			while (i->state != CAPI_STATE_CONNECTED) {
				usleep(10000);
			} 
		}
	}

	/* the caller onhold hungup or died away, drop the answered call */
	if (i->onholdPLCI == 0) {
		DISCONNECT_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
		DISCONNECT_REQ_PLCI(&CMSG) = i->PLCI;

		if ((Info = _capi_put_cmsg(&CMSG)) == 0) {
			ast_log(LOG_NOTICE, "sent DISCONNECT_REQ PLCI=%#x\n",
				i->PLCI);
		}
		return -1;
	}

	ast_log(LOG_NOTICE, "onholdPLCI = %d\n", i->onholdPLCI);

	fac[0] = 7;	/* len */
	fac[1] = 0x06;	/* ECT (function) */
	fac[2] = 0x00;
	fac[3] = 4;	/* len / sservice specific parameter , cstruct */
	fac[4] = (i->onholdPLCI << 8 ) >> 8;
	fac[5] = i->onholdPLCI >> 8;
	fac[6] = 0;
	fac[7] = 0;

	FACILITY_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(),0);
	FACILITY_REQ_CONTROLLER(&CMSG) = i->controller;
	FACILITY_REQ_PLCI(&CMSG) = i->onholdPLCI;
	FACILITY_REQ_FACILITYSELECTOR(&CMSG) = 0x0003;	/* sservices */
	FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = (char *)&fac;

	if ((Info = _capi_put_cmsg(&CMSG)) != 0) {
		res = (int)Info;
	} else {
		ast_log(LOG_NOTICE, "sent FACILITY_REQ PLCI = %#x (%#x %#x) onholdPLCI = %#x\n ",
			i->PLCI, fac[4], fac[5], i->onholdPLCI);
			ast_log(LOG_NOTICE,"%s\n",capi_cmsg2str(&CMSG));
	}

	/*   i->outgoing = -1; / incoming + outgoing, this is a magic channel :) */

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
	return ast_register_application(app, capiECT_exec, synopsis, tdesc);
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

