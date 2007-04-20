/*
 * (CAPI*)
 *
 * An implementation of Common ISDN API 2.0 for Asterisk
 *
 * Copyright (C) 2005-2007 Cytronics & Melware
 *
 * Armin Schindler <armin@melware.de>
 * 
 * This program is free software and may be modified and 
 * distributed under the terms of the GNU Public License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "chan_capi20.h"
#include "chan_capi.h"
#include "chan_capi_supplementary.h"
#include "chan_capi_utils.h"

#define CCBSNR_MAX_LIST_ENTRIES 32
static struct ccbsnr_s ccbsnr_list[CCBSNR_MAX_LIST_ENTRIES];
AST_MUTEX_DEFINE_STATIC(ccbsnr_lock);

/*
 * a new CCBS/CCNR id was received
 */
static void new_ccbsnr_id(ccbsnrtype_t type, unsigned int plci,
	_cword id, struct capi_pvt *i)
{
	int a;
	char buffer[CAPI_MAX_STRING];

	cc_mutex_lock(&ccbsnr_lock);
	for (a = 0; a < CCBSNR_MAX_LIST_ENTRIES; a++) {
		if (ccbsnr_list[a].type == CCBSNR_TYPE_NULL) {
			ccbsnr_list[a].type = type;
			ccbsnr_list[a].id = id;
			ccbsnr_list[a].plci = plci;
			ccbsnr_list[a].state = CCBSNR_AVAILABLE;
			ccbsnr_list[a].handle = (id | ((plci & 0xff) << 16));

			if (i->peer) {
				snprintf(buffer, CAPI_MAX_STRING-1, "%u", ccbsnr_list[a].handle);
				pbx_builtin_setvar_helper(i->peer, "CCLINKAGEID", buffer);
			}
			cc_verbose(1, 1, VERBOSE_PREFIX_3 "%s: PLCI=%#x CCBS/CCNR new id=0x%04x handle=%d\n",
				i->vname, plci, id, ccbsnr_list[a].handle);
			break;
		}
	}
	cc_mutex_unlock(&ccbsnr_lock);
	
	if (a == CCBSNR_MAX_LIST_ENTRIES) {
		cc_log(LOG_ERROR, "No free entry for new CCBS/CCNR ID\n");
	}
}

/*
 * function to tell if CCBSNR is activated
 */
static int ccbsnr_tell_activated(void *data)
{
	unsigned int handle = (unsigned int)data;
	int a;
	int ret = 0;

	cc_mutex_lock(&ccbsnr_lock);
	for (a = 0; a < CCBSNR_MAX_LIST_ENTRIES; a++) {
		if (ccbsnr_list[a].handle == handle) {
			if (ccbsnr_list[a].state == CCBSNR_REQUESTED) {
				ret = 1;
			}
			break;
		}
	}
	cc_mutex_unlock(&ccbsnr_lock);

	return ret;
}

/*
 * return the pointer to ccbsnr structure
 */
static struct ccbsnr_s *get_ccbsnr_link(unsigned int handle)
{
	struct ccbsnr_s *ret = NULL;
	int a;
	
	for (a = 0; a < CCBSNR_MAX_LIST_ENTRIES; a++) {
		if (ccbsnr_list[a].handle == handle) {
			ret = &ccbsnr_list[a];
			break;
		}
	}

	return ret;
}

/*
 * select CCBS/CCNR id
 */
static int select_ccbsnr_id(unsigned int id, ccbsnrtype_t type,
	char *context, char *exten, int priority)
{
	int ret = 0;
	int a;
	
	cc_mutex_lock(&ccbsnr_lock);
	for (a = 0; a < CCBSNR_MAX_LIST_ENTRIES; a++) {
		if (((ccbsnr_list[a].plci & 0xff) == ((id >> 16) & 0xff)) &&
		   (ccbsnr_list[a].id == (id & 0xffff)) &&
		   (ccbsnr_list[a].type == type) &&
		   (ccbsnr_list[a].state == CCBSNR_AVAILABLE)) {
			strncpy(ccbsnr_list[a].context, context, sizeof(ccbsnr_list[a].context) - 1);
			strncpy(ccbsnr_list[a].exten, exten, sizeof(ccbsnr_list[a].exten) - 1);
			ccbsnr_list[a].priority = priority;
			ccbsnr_list[a].state = CCBSNR_REQUESTED;
			ret = ccbsnr_list[a].handle;
			cc_verbose(1, 1, VERBOSE_PREFIX_3 "CAPI: request CCBS/NR id=0x%x handle=%d (%s,%s,%d)\n",
				id, ret, context, exten, priority);
			break;
		}
	}
	cc_mutex_unlock(&ccbsnr_lock);
	return ret;
}

/*
 * a CCBS/CCNR ref was removed 
 */
static void del_ccbsnr_ref(unsigned int plci, _cword ref)
{
	int a;

	cc_mutex_lock(&ccbsnr_lock);
	for (a = 0; a < CCBSNR_MAX_LIST_ENTRIES; a++) {
		if (((ccbsnr_list[a].plci & 0xff) == (plci & 0xff)) &&
		   (ccbsnr_list[a].rbref == ref)) {
			ccbsnr_list[a].id = 0;
			ccbsnr_list[a].state = 0;
			ccbsnr_list[a].handle = 0;
			ccbsnr_list[a].rbref = 0;
			ccbsnr_list[a].plci = 0;
			ccbsnr_list[a].type = CCBSNR_TYPE_NULL;
			cc_verbose(1, 1, VERBOSE_PREFIX_3 "CAPI: PLCI=%#x CCBS/CCNR deactivated "
				"ref=0x%04x\n",	plci, ref);
			break;
		}
	}
	cc_mutex_unlock(&ccbsnr_lock);
}

/*
 * a CCBS/CCNR id was removed 
 */
static void del_ccbsnr_id(unsigned int plci, _cword id)
{
	int a;

	cc_mutex_lock(&ccbsnr_lock);
	for (a = 0; a < CCBSNR_MAX_LIST_ENTRIES; a++) {
		if (((ccbsnr_list[a].plci & 0xff) == (plci & 0xff)) &&
		   (ccbsnr_list[a].id == id)) {
			ccbsnr_list[a].id = 0;
			if ((ccbsnr_list[a].state == CCBSNR_AVAILABLE) ||
			    (ccbsnr_list[a].rbref == 0)) {
				ccbsnr_list[a].state = 0;
				ccbsnr_list[a].handle = 0;
				ccbsnr_list[a].rbref = 0;
				ccbsnr_list[a].plci = 0;
				ccbsnr_list[a].type = CCBSNR_TYPE_NULL;
			}
			cc_verbose(1, 1, VERBOSE_PREFIX_3 "CAPI: PLCI=%#x CCBS/CCNR deleted id=0x%04x "
				"handle=%d status=%d\n", plci, id,
				ccbsnr_list[a].handle, ccbsnr_list[a].state);
			break;
		}
	}
	cc_mutex_unlock(&ccbsnr_lock);
}

/*
 * send Listen for supplementary to specified controller
 */
void ListenOnSupplementary(unsigned controller)
{
	_cmsg	CMSG;
	MESSAGE_EXCHANGE_ERROR error;
	int waitcount = 50;

	error = capi_sendf(NULL, 0, CAPI_FACILITY_REQ, controller, get_capi_MessageNumber(),
		"w(w(d))",
		FACILITYSELECTOR_SUPPLEMENTARY,
		0x0001,  /* LISTEN */
		0x0000079f
	);

	while (waitcount) {
		error = capidev_check_wait_get_cmsg(&CMSG);

		if (IS_FACILITY_CONF(&CMSG)) {
			break;
		}
		usleep(30000);
		waitcount--;
	}
	if (!waitcount) {
		cc_log(LOG_ERROR,"Unable to supplementary-listen on contr%d (error=0x%x)\n",
			controller, error);
	}
}

/*
 * CAPI FACILITY_IND supplementary services 
 */
void handle_facility_indication_supplementary(
	_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	_cword function;
	_cword infoword = 0xffff;
	unsigned char length;
	_cdword handle;
	_cword mode;
	_cword rbref;
	struct ccbsnr_s *ccbsnrlink;

	function = read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[1]);
	length = FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[3];

	if (length >= 2) {
		infoword = read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[4]);
	}

	/* first check functions without interface needed */
	switch (function) {
	case 0x000f: /* CCBS request */
		handle = read_capi_dword(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[6]);
		mode = read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[10]);
		rbref = read_capi_dword(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[12]);
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "contr%d: PLCI=%#x CCBS request reason=0x%04x "
			"handle=%d mode=0x%x rbref=0x%x\n",
			PLCI & 0xff, PLCI, infoword, handle, mode, rbref);
		if ((ccbsnrlink = get_ccbsnr_link(handle)) == NULL) {
			cc_log(LOG_WARNING, "capi ccbs request indication without request!\n");
			break;
		}
		if (infoword == 0) {
			/* success */
			ccbsnrlink->state = CCBSNR_ACTIVATED;
			ccbsnrlink->rbref = rbref;
			ccbsnrlink->mode = mode;
		} else {
			/* error */
			ccbsnrlink->state = CCBSNR_AVAILABLE;
		}
		show_capi_info(NULL, infoword);
		break;
	case 0x800e: /* CCBS status */
		rbref = read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[6]);
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "contr%d: PLCI=%#x CCBS status ref=0x%04x mode=0x%x\n",
			PLCI & 0xff, PLCI, rbref, infoword);
		/* XXX report we are free */
		break;
	case 0x800d: /* CCBS erase call linkage ID */
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "contr%d: PLCI=%#x CCBS/CCNR erase id=0x%04x\n",
			PLCI & 0xff, PLCI, infoword);
		del_ccbsnr_id(PLCI, infoword);
		break;
	case 0x800f: /* CCBS remote user free */
		rbref = read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[6]);
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "contr%d: PLCI=%#x CCBS status ref=0x%04x mode=0x%x\n",
			PLCI & 0xff, PLCI, rbref, infoword);
		/* XXX start alerting */
		break;
	case 0x8010: /* CCBS B-free */
		rbref = read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[6]);
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "contr%d: PLCI=%#x CCBS B-free ref=0x%04x mode=0x%x\n",
			PLCI & 0xff, PLCI, rbref, infoword);
		break;
	case 0x8011: /* CCBS erase (ref), deactivated by network */
		rbref = read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[6]);
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "contr%d: PLCI=%#x CCBS deactivate ref=0x%04x mode=0x%x\n",
			PLCI & 0xff, PLCI, rbref, infoword);
		del_ccbsnr_ref(PLCI, rbref);
		break;
	case 0x8012: /* CCBS stop alerting */
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "contr%d: PLCI=%#x CCBS B-free ref=0x%04x\n",
			PLCI & 0xff, PLCI, infoword);
		break;
	}

	return_on_no_interface("FACILITY_IND SUPPLEMENTARY");

	/* now functions bound to interface */
	switch (function) {
	case 0x0002: /* HOLD */
		if (infoword != 0) {
			/* reason != 0x0000 == problem */
			i->onholdPLCI = 0;
			cc_log(LOG_WARNING, "%s: unable to put PLCI=%#x onhold, REASON = 0x%04x, maybe you need to subscribe for this...\n",
				i->vname, PLCI, infoword);
			show_capi_info(i, infoword);
		} else {
			/* reason = 0x0000 == call on hold */
			i->state = CAPI_STATE_ONHOLD;
			cc_verbose(1, 1, VERBOSE_PREFIX_3 "%s: PLCI=%#x put onhold\n",
				i->vname, PLCI);
		}
		break;
	case 0x0003: /* RETRIEVE */
		if (infoword != 0) {
			cc_log(LOG_WARNING, "%s: unable to retrieve PLCI=%#x, REASON = 0x%04x\n",
				i->vname, PLCI, infoword);
			show_capi_info(i, infoword);
		} else {
			i->state = CAPI_STATE_CONNECTED;
			i->PLCI = i->onholdPLCI;
			i->onholdPLCI = 0;
			cc_verbose(1, 1, VERBOSE_PREFIX_3 "%s: PLCI=%#x retrieved\n",
				i->vname, PLCI);
			cc_start_b3(i);
		}
		break;
	case 0x0006:	/* ECT */
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "%s: PLCI=%#x ECT  Reason=0x%04x\n",
			i->vname, PLCI, infoword);
		show_capi_info(i, infoword);
		break;
	case 0x0007: /* 3PTY begin */
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "%s: PLCI=%#x 3PTY begin Reason=0x%04x\n",
			i->vname, PLCI, infoword);
		show_capi_info(i, infoword);
		break;
	case 0x0008: /* 3PTY end */
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "%s: PLCI=%#x 3PTY end Reason=0x%04x\n",
			i->vname, PLCI, infoword);
		show_capi_info(i, infoword);
		break;
	case 0x8013: /* CCBS info retain */
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "%s: PLCI=%#x CCBS unique id=0x%04x\n",
			i->vname, PLCI, infoword);
		new_ccbsnr_id(CCBSNR_TYPE_CCBS, PLCI, infoword, i);
		break;
	case 0x8015: /* CCNR info retain */
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "%s: PLCI=%#x CCNR unique id=0x%04x\n",
			i->vname, PLCI, infoword);
		new_ccbsnr_id(CCBSNR_TYPE_CCNR, PLCI, infoword, i);
		break;
	case 0x000e: /* CCBS status */
	case 0x000f: /* CCBS request */
	case 0x800f: /* CCBS remote user free */
	case 0x800d: /* CCBS erase call linkage ID */
	case 0x8010: /* CCBS B-free */
	case 0x8011: /* CCBS erase (ref), deactivated by network */
	case 0x8012: /* CCBS stop alerting */
		/* handled above */
		break;
	default:
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: unhandled FACILITY_IND supplementary function %04x\n",
			i->vname, function);
	}
}


/*
 * CAPI FACILITY_CONF supplementary
 */
void handle_facility_confirmation_supplementary(
	_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	_cword function;
	_cword serviceinfo;
	char name[64];

	if (i) {
		strncpy(name, i->vname, sizeof(name) - 1);
	} else {
		snprintf(name, sizeof(name) - 1, "contr%d", PLCI & 0xff);
	}

	function = read_capi_word(&FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[1]);
	serviceinfo = read_capi_word(&FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[4]);

	switch(function) {
	case 0x0002: /* HOLD */
		if (serviceinfo == 0) {
			cc_verbose(2, 0, VERBOSE_PREFIX_3 "%s: Call on hold (PLCI=%#x)\n",
				name, PLCI);
		}
		break;
	case 0x000f: /* CCBS request */
		cc_verbose(2, 0, VERBOSE_PREFIX_3 "%s: CCBS request info=0x%04x (PLCI=%#x)\n",
			name, serviceinfo, PLCI);
		break;
	default:
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: unhandled FACILITY_CONF supplementary function %04x\n",
			name, function);
	}
}

/*
 * capicommand 'ccbs'
 */
int pbx_capi_ccbs(struct ast_channel *c, char *data)
{
	char *slinkageid, *context, *exten, *priority;
	unsigned int linkid = 0;
	int handle, a;
	char *result = "ERROR";
	char *goodresult = "ACTIVATED";
	MESSAGE_EXCHANGE_ERROR error;
	struct ccbsnr_s *ccbsnrlink;

	slinkageid = strsep(&data, "|");
	context = strsep(&data, "|");
	exten = strsep(&data, "|");
	priority = data;

	if (slinkageid) {
		linkid = (unsigned int)strtoul(slinkageid, NULL, 0);
	}

	if ((!context) || (!exten) || (!priority)) {
		cc_log(LOG_WARNING, "capi ccbs requires <context>|<exten>|<priority>\n");
		return -1;
	}

	cc_verbose(3, 1, VERBOSE_PREFIX_3 "capi ccbs: '%d' '%s' '%s' '%s'\n",
		linkid, context, exten, priority);

	handle = select_ccbsnr_id(linkid, CCBSNR_TYPE_CCBS,
		context, exten, (int)strtol(priority, NULL, 0));

	if (handle > 0) {
	 	error = capi_sendf(NULL, 0, CAPI_FACILITY_REQ, (linkid >> 16) & 0xff,
			get_capi_MessageNumber(),
			"w(w(dw))",
			FACILITYSELECTOR_SUPPLEMENTARY,
			0x000f,  /* CCBS request */
			handle, /* handle */
			(linkid & 0xffff) /* CCBS linkage ID */
		);

		for (a = 0; a < 5; a++) {
		/* Wait for CCBS request indication */
			if (ast_safe_sleep_conditional(c, 500, ccbsnr_tell_activated,
			   (void *)handle) != 0) {
				/* we got a hangup */
				cc_verbose(3, 1,
					VERBOSE_PREFIX_3 "capi ccbs: hangup.\n");
				break;
			}
		}
		if ((ccbsnrlink = get_ccbsnr_link(handle)) != NULL) {
			if (ccbsnrlink->state == CCBSNR_ACTIVATED) {
				result = goodresult;
			}
		}
	}

	pbx_builtin_setvar_helper(c, "CCBSSTATUS", result);

	return 0;
}

