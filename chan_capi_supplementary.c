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

static struct ccbsnr_s *ccbsnr_list = NULL;
AST_MUTEX_DEFINE_STATIC(ccbsnr_lock);

/*
 * a new CCBS/CCNR id was received
 */
static void new_ccbsnr_id(ccbsnrtype_t type, unsigned int plci,
	_cword id, struct capi_pvt *i)
{
	char buffer[CAPI_MAX_STRING];
	struct ccbsnr_s *ccbsnr;

	ccbsnr = malloc(sizeof(struct ccbsnr_s));
	if (ccbsnr == NULL) {
		cc_log(LOG_ERROR, "Unable to allocate CCBS/CCNR struct.\n");
		return;
	}
	memset(ccbsnr, 0, sizeof(struct ccbsnr_s));

    ccbsnr->type = type;
    ccbsnr->id = id;
    ccbsnr->plci = plci;
    ccbsnr->state = CCBSNR_AVAILABLE;
    ccbsnr->handle = (id | ((plci & 0xff) << 16));

	if (i->peer) {
		snprintf(buffer, CAPI_MAX_STRING-1, "%u", ccbsnr->handle);
		pbx_builtin_setvar_helper(i->peer, "CCLINKAGEID", buffer);
	} else {
		cc_log(LOG_NOTICE, "No peerlink found to set CCBS/CCNR linkage ID.\n");
	}

	cc_mutex_lock(&ccbsnr_lock);
	ccbsnr->next = ccbsnr_list;
	ccbsnr_list = ccbsnr;
	cc_mutex_unlock(&ccbsnr_lock);

	cc_verbose(1, 1, VERBOSE_PREFIX_3
		"%s: PLCI=%#x CCBS/CCNR new id=0x%04x handle=%d\n",
		i->vname, plci, id, ccbsnr->handle);
}

/*
 * return the pointer to ccbsnr structure by handle
 */
static struct ccbsnr_s *get_ccbsnr_link(unsigned int handle, unsigned int *state)
{
	struct ccbsnr_s *ret;
	
	cc_mutex_lock(&ccbsnr_lock);
	ret = ccbsnr_list;
	while (ret) {
		if (ret->handle == handle) {
			if (state) {
				*state = ret->state;
			}
			break;
		}
		ret = ret->next;
	}
	cc_mutex_unlock(&ccbsnr_lock);

	return ret;
}

/*
 * return the pointer to ccbsnr structure by ref
 */
static struct ccbsnr_s *get_ccbsnr_linkref(_cword ref, char *busy)
{
	struct ccbsnr_s *ret;
	
	cc_mutex_lock(&ccbsnr_lock);
	ret = ccbsnr_list;
	while (ret) {
		if (ret->rbref == ref) {
			if (busy) {
				*busy = ret->partybusy;
			}
			break;
		}
		ret = ret->next;
	}
	cc_mutex_unlock(&ccbsnr_lock);

	return ret;
}

/*
 * function to tell if CCBSNR is activated
 */
static int ccbsnr_tell_activated(void *data)
{
	unsigned int handle = (unsigned int)data;
	int ret = 0;
	unsigned int state;

	if (get_ccbsnr_link(handle, &state) != NULL) {
		if (state == CCBSNR_REQUESTED) {
			ret = 1;
		}
	}

	return ret;
}

/*
 * select CCBS/CCNR id
 */
static unsigned int select_ccbsnr_id(unsigned int id, ccbsnrtype_t type,
	char *context, char *exten, int priority)
{
	struct ccbsnr_s *ccbsnr;
	int ret = 0;
	
	cc_mutex_lock(&ccbsnr_lock);
	ccbsnr = ccbsnr_list;
	while (ccbsnr) {
		if (((ccbsnr->plci & 0xff) == ((id >> 16) & 0xff)) &&
		   (ccbsnr->id == (id & 0xffff)) &&
		   (ccbsnr->type == type) &&
		   (ccbsnr->state == CCBSNR_AVAILABLE)) {
			strncpy(ccbsnr->context, context, sizeof(ccbsnr->context) - 1);
			strncpy(ccbsnr->exten, exten, sizeof(ccbsnr->exten) - 1);
			ccbsnr->priority = priority;
			ccbsnr->state = CCBSNR_REQUESTED;
			ret = ccbsnr->handle;
			cc_verbose(1, 1, VERBOSE_PREFIX_3 "CAPI: request CCBS/NR id=0x%x handle=%d (%s,%s,%d)\n",
				id, ret, context, exten, priority);
			break;
		}
		ccbsnr = ccbsnr->next;
	}
	cc_mutex_unlock(&ccbsnr_lock);
	
	return ret;
}

/*
 * a CCBS/CCNR ref was removed 
 */
static void del_ccbsnr_ref(unsigned int plci, _cword ref)
{
	struct ccbsnr_s *ccbsnr;
	struct ccbsnr_s *tmp = NULL;

	cc_mutex_lock(&ccbsnr_lock);
	ccbsnr = ccbsnr_list;
	while (ccbsnr) {
		if (((ccbsnr->plci & 0xff) == (plci & 0xff)) &&
		   (ccbsnr->rbref == ref)) {
			if (!tmp) {
				ccbsnr_list = ccbsnr->next;
			} else {
				tmp->next = ccbsnr->next;
			}
			free(ccbsnr);
			cc_verbose(1, 1, VERBOSE_PREFIX_3 "CAPI: PLCI=%#x CCBS/CCNR deactivated "
				"ref=0x%04x\n",	plci, ref);
			break;
		}
		tmp = ccbsnr;
		ccbsnr = ccbsnr->next;
	}
	cc_mutex_unlock(&ccbsnr_lock);
}

/*
 * a CCBS/CCNR id was removed 
 */
static void del_ccbsnr_id(unsigned int plci, _cword id)
{
	struct ccbsnr_s *ccbsnr;
	struct ccbsnr_s *tmp = NULL;

	cc_mutex_lock(&ccbsnr_lock);
	ccbsnr = ccbsnr_list;
	while (ccbsnr) {
		if (((ccbsnr->plci & 0xff) == (plci & 0xff)) &&
		    (ccbsnr->id == id)) {
			cc_verbose(1, 1, VERBOSE_PREFIX_3 "CAPI: PLCI=%#x CCBS/CCNR deleted id=0x%04x "
				"handle=%d status=%d\n", plci, id,
				ccbsnr->handle, ccbsnr->state);
			if ((ccbsnr->state == CCBSNR_AVAILABLE) ||
			    (ccbsnr->rbref == 0)) {
				if (!tmp) {
					ccbsnr_list = ccbsnr->next;
				} else {
					tmp->next = ccbsnr->next;
				}
				free(ccbsnr);
			} else {
				/* just deactivate the linkage id */
				ccbsnr->id = 0xdead;
			}
			break;
		}
		tmp = ccbsnr;
		ccbsnr = ccbsnr->next;
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
int handle_facility_indication_supplementary(
	_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	_cword function;
	_cword infoword = 0xffff;
	unsigned char length;
	_cdword handle;
	_cword mode;
	_cword rbref;
	struct ccbsnr_s *ccbsnrlink;
	char partybusy = 0;
	int ret = 0;

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
		if ((ccbsnrlink = get_ccbsnr_link(handle, NULL)) == NULL) {
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
	case 0x800d: /* CCBS erase call linkage ID */
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "contr%d: PLCI=%#x CCBS/CCNR erase id=0x%04x\n",
			PLCI & 0xff, PLCI, infoword);
		del_ccbsnr_id(PLCI, infoword);
		break;
	case 0x800e: /* CCBS status */
		rbref = read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[6]);
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "contr%d: PLCI=%#x CCBS status ref=0x%04x mode=0x%x\n",
			PLCI & 0xff, PLCI, rbref, infoword);
		if (get_ccbsnr_linkref(rbref, &partybusy) == NULL) {
			cc_log(LOG_WARNING, "capi CCBS status reference not found!\n");
		}
		capi_sendf(NULL, 0, CAPI_FACILITY_RESP, PLCI, HEADER_MSGNUM(CMSG),
			"w(w(w))",
			FACILITYSELECTOR_SUPPLEMENTARY,
			0x800e,  /* CCBS status */
			(partybusy) ? 0x0000 : 0x0001
		);
		ret = 1;
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

	if (!i) {
		cc_verbose(4, 1, "CAPI: FACILITY_IND SUPPLEMENTARY " 
			"no interface for PLCI=%#x\n", PLCI);
		return ret;
	}

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
	return ret;
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
 * capicommand 'ccpartybusy'
 */
int pbx_capi_ccpartybusy(struct ast_channel *c, char *data)
{
	char *slinkageid, *yesno;
	unsigned int linkid = 0;
	struct ccbsnr_s *ccbsnr;
	char partybusy = 0;

	slinkageid = strsep(&data, "|");
	yesno = data;
	
	if (slinkageid) {
		linkid = (unsigned int)strtoul(slinkageid, NULL, 0);
	}

	if ((yesno) && ast_true(yesno)) {
		partybusy = 1;
	}

	cc_mutex_lock(&ccbsnr_lock);
	ccbsnr = ccbsnr_list;
	while (ccbsnr) {
		if (((ccbsnr->plci & 0xff) == ((linkid >> 16) & 0xff)) &&
		   (ccbsnr->id == (linkid & 0xffff))) {
			ccbsnr->partybusy = partybusy;
			cc_verbose(1, 1, VERBOSE_PREFIX_3 "CAPI: CCBS/NR id=0x%x busy set to %d\n",
				linkid, partybusy);
			break;
		}
		ccbsnr = ccbsnr->next;
	}
	cc_mutex_unlock(&ccbsnr_lock);

	return 0;
}

/*
 * capicommand 'ccbs'
 */
int pbx_capi_ccbs(struct ast_channel *c, char *data)
{
	char *slinkageid, *context, *exten, *priority;
	unsigned int linkid = 0;
	unsigned int handle, a;
	char *result = "ERROR";
	char *goodresult = "ACTIVATED";
	MESSAGE_EXCHANGE_ERROR error;
	unsigned int ccbsnrstate;

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
		if (get_ccbsnr_link(handle, &ccbsnrstate) != NULL) {
			if (ccbsnrstate == CCBSNR_ACTIVATED) {
				result = goodresult;
			}
		}
	}

	pbx_builtin_setvar_helper(c, "CCBSSTATUS", result);

	return 0;
}

