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


/*
 * send Listen for supplementary to specified controller
 */
void ListenOnSupplementary(unsigned controller)
{
	_cmsg	CMSG;
	char	fac[8];
	MESSAGE_EXCHANGE_ERROR error;
	int waitcount = 50;

	fac[0] = 7;	/* len */
	fac[1] = 0x01;	/* listen */
	fac[2] = 0x00;
	fac[3] = 4;	/* len / sservice specific parameter , cstruct */
	write_capi_dword(&(fac[4]), 0x0000079f);

	FACILITY_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(), 0);
	FACILITY_REQ_CONTROLLER(&CMSG) = controller;
	FACILITY_REQ_FACILITYSELECTOR(&CMSG) = FACILITYSELECTOR_SUPPLEMENTARY;
	FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = (_cstruct)&fac;

	error = _capi_put_cmsg(&CMSG);

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

	function = read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[1]);
	length = FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[3];

	if (length >= 2) {
		infoword = read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[4]);
	}

	/* first check functions without interface needed */
	switch (function) {
	case 0x800d: /* CCBS erase call linkage ID */
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "contr%d: PLCI=%#x CCBS/CCNR erase id=0x%04x\n",
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
		break;
	case 0x8015: /* CCNR info retain */
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "%s: PLCI=%#x CCNR unique id=0x%04x\n",
			i->vname, PLCI, infoword);
		break;
	case 0x800d: /* CCBS erase call linkage ID */
		/* handled above */
		break;
	default:
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: unhandled FACILITY_IND supplementary function %04x\n",
			i->vname, function);
	}
}

