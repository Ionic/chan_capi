/*
 * (CAPI*)
 *
 * An implementation of Common ISDN API 2.0 for Asterisk
 *
 * Disable echo suppression (useful for fax and voicemail!)
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



static char *tdesc = "(CAPI*) No Echo Suppression.";
static char *app = "capiNoES";
static char *synopsis = "Disable Echo Suppression";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int capiNoES_exec(struct ast_channel *chan, void *data)
{
	int res=0;
	struct localuser *u;
	
	LOCAL_USER_ADD(u);

	if (strcasecmp("CAPI", chan->type) == 0) {
		struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(chan);
		if (i->doES == 1) {
			i->doES = 0;
		}
	} else {
		ast_log(LOG_WARNING, "capiNoES only works on CAPI channels, check your extensions.conf!\n");
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
	return ast_register_application(app, capiNoES_exec, synopsis, tdesc);
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

