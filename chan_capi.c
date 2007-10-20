/*
 * (CAPI*)
 *
 * An implementation of Common ISDN API 2.0 for Asterisk
 *
 * Copyright (C) 2005-2007 Cytronics & Melware
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

#include <sys/time.h>
#include <sys/signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include "xlaw.h"
#include "chan_capi20.h"
#include "chan_capi.h"
#include "chan_capi_rtp.h"
#include "chan_capi_qsig.h"
#include "chan_capi_qsig_ecma.h"
#include "chan_capi_qsig_asn197ade.h"
#include "chan_capi_qsig_asn197no.h"
#include "chan_capi_utils.h"
#include "chan_capi_supplementary.h"
#include "chan_capi_chat.h"

/* #define CC_VERSION "x.y.z" */
#define CC_VERSION "$Revision$"

/*
 * personal stuff
 */
#undef   CAPI_APPLID_UNUSED
#define  CAPI_APPLID_UNUSED 0xffffffff
unsigned capi_ApplID = CAPI_APPLID_UNUSED;

static const char tdesc[] = "Common ISDN API Driver (" CC_VERSION ")";
static const char channeltype[] = "CAPI";
#ifdef CC_AST_HAS_VERSION_1_4
#define AST_MODULE "chan_capi"
#else
static char *ccdesc = "Common ISDN API for Asterisk";
#endif

static char *commandtdesc = "CAPI command interface.\n"
"The dial command:\n"
"Dial(CAPI/g<group>/[<callerid>:]<destination>[/<params>])\n"
"Dial(CAPI/contr<controller>/[<callerid>:]<destination>[/<params>])\n"
"Dial(CAPI/<interface-name>/[<callerid>:]<destination>[/<params>])\n"
"\"params\" can be:\n"
"early B3:\"b\"=always, \"B\"=on successful calls only\n"
"\"d\":use callerID from capi.conf, \"o\":overlap sending number\n"
"\n\"q\":disable QSIG functions on outgoing call\n"
"\n"
"capicommand() where () can be:\n"
"\"progress\" send progress (for NT mode)\n"
"\"deflect|to_number\" forwards an unanswered call to number\n"
"\"malicous\" report a call of malicious nature\n"
"\"echocancel|<yes> or <no>\" echo-cancel provided by driver/hardware\n"
"\"echosquelch|<yes> or <no>\" very primitive echo-squelch by chan-capi\n"
"\"holdtype|<local> or <hold>\" set type of 'hold'\n"
"\"hold[|MYHOLDVAR]\" puts an answered call on hold\n"
"\"retrieve|${MYHOLDVAR}\" gets back the held call\n"
"\"ect|${MYHOLDVAR})\" explicit call transfer of call on hold\n"
"\"3pty_begin|${MYHOLDVAR})\" Three-Party-Conference (3PTY) with active and held call\n"
"\"receivefax|filename|stationID|headline\" receive a CAPIfax\n"
"\"sendfax|filename.sff|stationID|headline\" send a CAPIfax\n"
"\"qsig_ssct|cidsrc|ciddst\" QSIG single step call transfer\n"
"\"qsig_ct|cidsrc|ciddst|marker|waitconnect\" QSIG call transfer\n"
"\"qsig_callmark|marker\" marks a QSIG call for later identification\n"
"Variables set after fax receive:\n"
"FAXSTATUS     :0=OK, 1=Error\n"
"FAXREASON     :B3 disconnect reason\n"
"FAXREASONTEXT :FAXREASON as text\n"
"FAXRATE       :baud rate of fax connection\n"
"FAXRESOLUTION :0=standard, 1=high\n"
"FAXFORMAT     :0=SFF\n"
"FAXPAGES      :Number of pages received\n"
"FAXID         :ID of the remote fax machine\n"
"Asterisk variables used/set by chan_capi:\n"
"BCHANNELINFO,CALLEDTON,_CALLERHOLDID,CALLINGSUBADDRESS,CALLEDSUBADDRESS\n"
"CONNECTEDNUMBER,FAXEXTEN,PRI_CAUSE,REDIRECTINGNUMBER,REDIRECTREASON\n"
"!!! for more details and samples, check the README of chan-capi !!!\n";

static char *commandapp = "capiCommand";
static char *commandsynopsis = "Execute special CAPI commands";
#ifndef CC_AST_HAS_VERSION_1_4
STANDARD_LOCAL_USER;
LOCAL_USER_DECL;
#endif

static int usecnt;

/*
 * LOCKING RULES
 * =============
 *
 * This channel driver uses several locks. One must be 
 * careful not to reverse the locking order, which will
 * lead to a so called deadlock. Here is the locking order
 * that must be followed:
 *
 * struct capi_pvt *i;
 *
 * 1. cc_mutex_lock(&i->owner->lock); **
 *
 * 2. cc_mutex_lock(&i->lock);
 *
 * 3. cc_mutex_lock(&iflock);
 * 4. cc_mutex_lock(&messagenumber_lock);
 * 5. cc_mutex_lock(&usecnt_lock);
 * 6. cc_mutex_lock(&capi_put_lock);
 *
 *
 *  ** the PBX will call the callback functions with 
 *     this lock locked. This lock protects the 
 *     structure pointed to by 'i->owner'. Also note
 *     that calling some PBX functions will lock
 *     this lock!
 */

#ifndef CC_AST_HAS_VERSION_1_4
AST_MUTEX_DEFINE_STATIC(usecnt_lock);
#endif
AST_MUTEX_DEFINE_STATIC(iflock);

static pthread_t monitor_thread = (pthread_t)(0-1);

struct capi_pvt *capi_iflist = NULL;

static struct cc_capi_controller *capi_controllers[CAPI_MAX_CONTROLLERS + 1];
static int capi_num_controllers = 0;
static unsigned int capi_counter = 0;
static unsigned long capi_used_controllers = 0;

static struct ast_channel *chan_for_task;
static int channel_task;
#define CAPI_CHANNEL_TASK_NONE             0
#define CAPI_CHANNEL_TASK_HANGUP           1
#define CAPI_CHANNEL_TASK_SOFTHANGUP       2
#define CAPI_CHANNEL_TASK_PICKUP           3
#define CAPI_CHANNEL_TASK_GOTOFAX          4

static struct capi_pvt *interface_for_task;
static int interface_task;
#define CAPI_INTERFACE_TASK_NONE           0
#define CAPI_INTERFACE_TASK_NULLIFREMOVE   1

static char capi_national_prefix[AST_MAX_EXTENSION];
static char capi_international_prefix[AST_MAX_EXTENSION];

static char default_language[MAX_LANGUAGE] = "";

int capi_capability = AST_FORMAT_ALAW;

#ifdef CC_AST_HAS_VERSION_1_4
/* Global jitterbuffer configuration - by default, jb is disabled */
static struct ast_jb_conf default_jbconf =
{
	.flags = 0,
	.max_size = -1,
	.resync_threshold = -1,
	.impl = ""
};
static struct ast_jb_conf global_jbconf;
static char global_mohinterpret[MAX_MUSICCLASS] = "default";
#endif

/* local prototypes */
static int pbx_capi_hold(struct ast_channel *c, char *param);
#ifdef CC_AST_HAS_INDICATE_DATA
static int pbx_capi_indicate(struct ast_channel *c, int condition, const void *data, size_t datalen);
#else
static int pbx_capi_indicate(struct ast_channel *c, int condition);
#endif

/*
 * B protocol settings
 */
static struct {
	_cword b1protocol;
	_cword b2protocol;
	_cword b3protocol;
	_cstruct b1configuration;
	_cstruct b2configuration;
	_cstruct b3configuration;
} b_protocol_table[] =
{
	{ 0x01, 0x01, 0x00,	/* 0 */
		NULL,
		NULL,
		NULL
	},
	{ 0x04, 0x04, 0x04,	/* 1 */
		NULL,
		NULL,
		NULL
	},
	{ 0x1f, 0x1f, 0x1f,	/* 2 */
		(_cstruct) "\x00",
		/* (_cstruct) "\x04\x01\x00\x00\x02", */
		(_cstruct) "\x06\x01\x00\x58\x02\x32\x00",
		(_cstruct) "\x00"
	}
};

/*
 * set the global-configuration (b-channel operation)
 */
static _cstruct capi_set_global_configuration(struct capi_pvt *i)
{
	unsigned short dtedce = 0;
	unsigned char *buf = i->tmpbuf;

	buf[0] = 2; /* len */

	if (i->FaxState & CAPI_FAX_STATE_ACTIVE) {
		if ((i->outgoing) && (!(i->FaxState & CAPI_FAX_STATE_SENDMODE)))
			dtedce = 2;
		if ((!(i->outgoing)) && ((i->FaxState & CAPI_FAX_STATE_SENDMODE)))
			dtedce = 1;
	}
	write_capi_word(&buf[1], dtedce);
	if (dtedce == 0)
		buf = NULL;
	return (_cstruct)buf;
}

/*
 * command to string function
 */
static const char * capi_command_to_string(unsigned short wCmd)
{
	enum { lowest_value = CAPI_P_MIN,
	       end_value = CAPI_P_MAX,
	       range = end_value - lowest_value,
	};

#undef  CHAN_CAPI_COMMAND_DESC
#define CHAN_CAPI_COMMAND_DESC(n, ENUM, value)		\
	[CAPI_P_REQ(ENUM)-(n)]  = #ENUM "_REQ",		\
	[CAPI_P_CONF(ENUM)-(n)] = #ENUM "_CONF",	\
	[CAPI_P_IND(ENUM)-(n)]  = #ENUM "_IND",		\
	[CAPI_P_RESP(ENUM)-(n)] = #ENUM "_RESP",

	static const char * const table[range] = {
	    CAPI_COMMANDS(CHAN_CAPI_COMMAND_DESC, lowest_value)
	};

	wCmd -= lowest_value;

	if (wCmd >= range) {
	    goto error;
	}

	if (table[wCmd] == NULL) {
	    goto error;
	}
	return table[wCmd];

 error:
	return "UNDEFINED";
}

/*
 * wait for B3 up
 */
int capi_wait_for_b3_up(struct capi_pvt *i)
{
	struct timespec abstime;
	int ret = 1;

	cc_mutex_lock(&i->lock);
	if (!(i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
		i->waitevent = CAPI_WAITEVENT_B3_UP;
		abstime.tv_sec = time(NULL) + 2;
		abstime.tv_nsec = 0;
		cc_verbose(4, 1, "%s: wait for b3 up.\n",
			i->vname);
		if (ast_cond_timedwait(&i->event_trigger, &i->lock, &abstime) != 0) {
			cc_log(LOG_WARNING, "%s: timed out waiting for b3 up.\n",
				i->vname);
			ret = 0;
		} else {
			cc_verbose(4, 1, "%s: cond signal received for b3 up.\n",
				i->vname);
		}
	}
	cc_mutex_unlock(&i->lock);

	return ret;
}

/*
 * wait for finishing answering state
 */
void capi_wait_for_answered(struct capi_pvt *i)
{
	struct timespec abstime;

	cc_mutex_lock(&i->lock);
	if (i->state == CAPI_STATE_ANSWERING) {
		i->waitevent = CAPI_WAITEVENT_ANSWER_FINISH;
		abstime.tv_sec = time(NULL) + 2;
		abstime.tv_nsec = 0;
		cc_verbose(4, 1, "%s: wait for finish answer.\n",
			i->vname);
		if (ast_cond_timedwait(&i->event_trigger, &i->lock, &abstime) != 0) {
			cc_log(LOG_WARNING, "%s: timed out waiting for finish answer.\n",
				i->vname);
		} else {
			cc_verbose(4, 1, "%s: cond signal received for finish answer.\n",
				i->vname);
		}
	}
	cc_mutex_unlock(&i->lock);
}

/*
 * function to tell if fax activity has finished
 */
static int capi_tell_fax_finish(void *data)
{
	struct capi_pvt *i = (struct capi_pvt *)data;

	if (i->FaxState & CAPI_FAX_STATE_ACTIVE) {
		return 1;
	}

	return 0;
}

/*
 *  TCAP -> CIP Translation Table (TransferCapability->CommonIsdnProfile)
 */
static struct {
	unsigned short tcap;
	unsigned short cip;
	unsigned char digital;
} translate_tcap2cip[] = {
	{ PRI_TRANS_CAP_SPEECH,                 CAPI_CIPI_SPEECH,		0 },
	{ PRI_TRANS_CAP_DIGITAL,                CAPI_CIPI_DIGITAL,		1 },
	{ PRI_TRANS_CAP_RESTRICTED_DIGITAL,     CAPI_CIPI_RESTRICTED_DIGITAL,	1 },
	{ PRI_TRANS_CAP_3K1AUDIO,               CAPI_CIPI_3K1AUDIO,		0 },
	{ PRI_TRANS_CAP_DIGITAL_W_TONES,        CAPI_CIPI_DIGITAL_W_TONES,	1 },
	{ PRI_TRANS_CAP_VIDEO,                  CAPI_CIPI_VIDEO,		1 }
};

static int tcap2cip(unsigned short tcap)
{
	int x;
	
	for (x = 0; x < sizeof(translate_tcap2cip) / sizeof(translate_tcap2cip[0]); x++) {
		if (translate_tcap2cip[x].tcap == tcap)
			return (int)translate_tcap2cip[x].cip;
	}
	return CAPI_CIPI_SPEECH;
}

unsigned char capi_tcap_is_digital(unsigned short tcap)
{
	int x;
	
	for (x = 0; x < sizeof(translate_tcap2cip) / sizeof(translate_tcap2cip[0]); x++) {
		if (translate_tcap2cip[x].tcap == tcap)
			return translate_tcap2cip[x].digital;
	}
	return 0;
}

/*
 *  CIP -> TCAP Translation Table (CommonIsdnProfile->TransferCapability)
 */
static struct {
	unsigned short cip;
	unsigned short tcap;
} translate_cip2tcap[] = {
	{ CAPI_CIPI_SPEECH,                  PRI_TRANS_CAP_SPEECH },
	{ CAPI_CIPI_DIGITAL,                 PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIPI_RESTRICTED_DIGITAL,      PRI_TRANS_CAP_RESTRICTED_DIGITAL },
	{ CAPI_CIPI_3K1AUDIO,                PRI_TRANS_CAP_3K1AUDIO },
	{ CAPI_CIPI_7KAUDIO,                 PRI_TRANS_CAP_DIGITAL_W_TONES },
	{ CAPI_CIPI_VIDEO,                   PRI_TRANS_CAP_VIDEO },
	{ CAPI_CIPI_PACKET_MODE,             PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIPI_56KBIT_RATE_ADAPTION,    PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIPI_DIGITAL_W_TONES,         PRI_TRANS_CAP_DIGITAL_W_TONES },
	{ CAPI_CIPI_TELEPHONY,               PRI_TRANS_CAP_SPEECH },
	{ CAPI_CIPI_FAX_G2_3,                PRI_TRANS_CAP_3K1AUDIO },
	{ CAPI_CIPI_FAX_G4C1,                PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIPI_FAX_G4C2_3,              PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIPI_TELETEX_PROCESSABLE,     PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIPI_TELETEX_BASIC,           PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIPI_VIDEOTEX,                PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIPI_TELEX,                   PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIPI_X400,                    PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIPI_X200,                    PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIPI_7K_TELEPHONY,            PRI_TRANS_CAP_DIGITAL_W_TONES },
	{ CAPI_CIPI_VIDEO_TELEPHONY_C1,      PRI_TRANS_CAP_DIGITAL_W_TONES },
	{ CAPI_CIPI_VIDEO_TELEPHONY_C2,      PRI_TRANS_CAP_DIGITAL }
};

static unsigned short cip2tcap(int cip)
{
	int x;
	
	for (x = 0;x < sizeof(translate_cip2tcap) / sizeof(translate_cip2tcap[0]); x++) {
		if (translate_cip2tcap[x].cip == (unsigned short)cip)
			return translate_cip2tcap[x].tcap;
	}
	return 0;
}

/*
 *  TransferCapability to String conversion
 */
static char *transfercapability2str(int transfercapability)
{
	switch(transfercapability) {
	case PRI_TRANS_CAP_SPEECH:
		return "SPEECH";
	case PRI_TRANS_CAP_DIGITAL:
		return "DIGITAL";
	case PRI_TRANS_CAP_RESTRICTED_DIGITAL:
		return "RESTRICTED_DIGITAL";
	case PRI_TRANS_CAP_3K1AUDIO:
		return "3K1AUDIO";
	case PRI_TRANS_CAP_DIGITAL_W_TONES:
		return "DIGITAL_W_TONES";
	case PRI_TRANS_CAP_VIDEO:
		return "VIDEO";
	default:
		return "UNKNOWN";
	}
}

/*
 * set task for an interface which need to be done out of lock
 * ( after the capi thread loop )
 */
static void capi_interface_task(struct capi_pvt *i, int task)
{
	interface_for_task = i;
	interface_task = task;

	cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: set interface task to %d\n",
		i->name, task);
}

/*
 * set task for a channel which need to be done out of lock
 * ( after the capi thread loop )
 */
static void capi_channel_task(struct ast_channel *c, int task)
{
	chan_for_task = c;
	channel_task = task;

	cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: set channel task to %d\n",
		c->name, task);
}

/*
 * Added date/time IE to facility structure
 */
static void capi_facility_add_datetime(unsigned char *facilityarray)
{
	unsigned int idx;
	time_t current_time;
	struct tm *time_local;
	unsigned char year;

	if (!facilityarray)
		return;

	current_time = time(NULL);
	time_local = localtime(&current_time);
	year = time_local->tm_year;

	while (year > 99) {
		year -= 100;
	}
	
	idx = facilityarray[0] + 1;

	facilityarray[idx++] = 0x29;	/* date/time IE */
	facilityarray[idx++] = 5;		/* length */
	facilityarray[idx++] = year;
	facilityarray[idx++] = time_local->tm_mon + 1;
	facilityarray[idx++] = time_local->tm_mday;
	facilityarray[idx++] = time_local->tm_hour;
	facilityarray[idx++] = time_local->tm_min;

	facilityarray[0] = idx - 1;

	return;
}

/*
 * Echo cancellation is for cards w/ integrated echo cancellation only
 * (i.e. Eicon active cards support it)
 */
static void capi_echo_canceller(struct capi_pvt *i, int function)
{
	int ecAvail = 0;

	if ((i->isdnstate & CAPI_ISDN_STATE_DISCONNECT))
		return;

	if (i->channeltype == CAPI_CHANNELTYPE_NULL) {
		return;
	}

	if (((function == EC_FUNCTION_ENABLE) && (i->isdnstate & CAPI_ISDN_STATE_EC)) ||
	    ((function != EC_FUNCTION_ENABLE) && (!(i->isdnstate & CAPI_ISDN_STATE_EC)))) {
		cc_verbose(3, 1, VERBOSE_PREFIX_4 "%s: echo canceller (PLCI=%#x, function=%d) unchanged\n",
			i->vname, i->PLCI, function);
		/* nothing to do */
		return;
	}

	/* check for old echo-cancel configuration */
	if ((i->ecSelector != FACILITYSELECTOR_ECHO_CANCEL) &&
	    (capi_controllers[i->controller]->broadband)) {
		ecAvail = 1;
	}
	if ((i->ecSelector == FACILITYSELECTOR_ECHO_CANCEL) &&
	    (capi_controllers[i->controller]->echocancel)) {
		ecAvail = 1;
	}

	/* If echo cancellation is not requested or supported, don't attempt to enable it */
	if (!ecAvail || !i->doEC) {
		return;
	}

	if (capi_tcap_is_digital(i->transfercapability)) {
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: No echo canceller in digital mode (PLCI=%#x)\n",
			i->vname, i->PLCI);
		return;
	}

	cc_verbose(3, 0, VERBOSE_PREFIX_2 "%s: Setting up echo canceller (PLCI=%#x, function=%d, options=%d, tail=%d)\n",
			i->vname, i->PLCI, function, i->ecOption, i->ecTail);

	if (function == EC_FUNCTION_ENABLE) {
		i->isdnstate |= CAPI_ISDN_STATE_EC;
	} else {
		i->isdnstate &= ~CAPI_ISDN_STATE_EC;
	}

	capi_sendf(i, 0, CAPI_FACILITY_REQ, i->PLCI, get_capi_MessageNumber(),
		"w(w(www))",
		i->ecSelector,
		function,
		i->ecOption,  /* bit field - ignore echo canceller disable tone */
		i->ecTail,    /* Tail length, ms */
		0
	);

	return;
}

/*
 * turn on/off DTMF detection
 */
static int capi_detect_dtmf(struct capi_pvt *i, int flag)
{
	MESSAGE_EXCHANGE_ERROR error;

	if ((i->isdnstate & CAPI_ISDN_STATE_DISCONNECT))
		return 0;

	if (i->channeltype == CAPI_CHANNELTYPE_NULL) {
		return 0;
	}

	if (capi_tcap_is_digital(i->transfercapability)) {
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: No dtmf-detect in digital mode (PLCI=%#x)\n",
			i->vname, i->PLCI);
		return 0;
	}

	if (((flag == 1) && (i->isdnstate & CAPI_ISDN_STATE_DTMF)) ||
	    ((flag == 0) && (!(i->isdnstate & CAPI_ISDN_STATE_DTMF)))) {
		cc_verbose(3, 1, VERBOSE_PREFIX_4 "%s: dtmf (PLCI=%#x, flag=%d) unchanged\n",
			i->vname, i->PLCI, flag);
		/* nothing to do */
		return 0;
	}
	
	/* does the controller support dtmf? and do we want to use it? */
	if ((capi_controllers[i->controller]->dtmf != 1) || (i->doDTMF != 0))
		return 0;
	
	cc_verbose(3, 0, VERBOSE_PREFIX_2 "%s: Setting up DTMF detector (PLCI=%#x, flag=%d)\n",
		i->vname, i->PLCI, flag);

	error = capi_sendf(i, 0, CAPI_FACILITY_REQ, i->PLCI, get_capi_MessageNumber(),
		"w(www()())",
		FACILITYSELECTOR_DTMF,
		(flag == 1) ? 1:2,  /* start/stop DTMF listen */
		CAPI_DTMF_DURATION,
		CAPI_DTMF_DURATION
	);

	if (error != 0) {
		return error;
	}
	if (flag == 1) {
		i->isdnstate |= CAPI_ISDN_STATE_DTMF;
	} else {
		i->isdnstate &= ~CAPI_ISDN_STATE_DTMF;
	}
	return 0;
}

/*
 * queue a frame to PBX
 */
static int local_queue_frame(struct capi_pvt *i, struct ast_frame *f)
{
	unsigned char *wbuf;
	int wbuflen;

	if (!(i->isdnstate & CAPI_ISDN_STATE_PBX)) {
		/* if there is no PBX running yet,
		   we don't need any frames sent */
		return -1;
	}
	if ((i->state == CAPI_STATE_DISCONNECTING) ||
	    (i->isdnstate & CAPI_ISDN_STATE_HANGUP)) {
		cc_verbose(3, 1, VERBOSE_PREFIX_4 "%s: no queue_frame in state disconnecting for %d/%d\n",
			i->vname, f->frametype, f->subclass);
		return 0;
	}

	if ((capidebug) && (f->frametype != AST_FRAME_VOICE)) {
		ast_frame_dump(i->vname, f, VERBOSE_PREFIX_3 "CAPI queue frame:");
	}

	if ((f->frametype == AST_FRAME_CONTROL) &&
	    (f->subclass == AST_CONTROL_HANGUP)) {
		i->isdnstate |= CAPI_ISDN_STATE_HANGUP;
	}

	if (i->writerfd == -1) {
		cc_log(LOG_ERROR, "No writerfd in local_queue_frame for %s\n",
			i->vname);
		return -1;
	}

	if (f->frametype != AST_FRAME_VOICE)
		f->datalen = 0;

	wbuflen = sizeof(struct ast_frame) + f->datalen;
	wbuf = alloca(wbuflen);
	memcpy(wbuf, f, sizeof(struct ast_frame));
	if (f->datalen)
		memcpy(wbuf + sizeof(struct ast_frame), f->data, f->datalen);

	if (write(i->writerfd, wbuf, wbuflen) != wbuflen) {
		cc_log(LOG_ERROR, "Could not write to pipe for %s\n",
			i->vname);
	}
	return 0;
}

/*
 * set a new name for this channel
 */
static void update_channel_name(struct capi_pvt *i)
{
	char name[AST_CHANNEL_NAME];

	snprintf(name, sizeof(name) - 1, "CAPI/%s/%s-%x",
		i->vname, i->dnid, capi_counter++);
	if (i->owner) {
		ast_change_name(i->owner, name);
	}
	cc_verbose(3, 0, VERBOSE_PREFIX_3 "%s: Updated channel name: %s\n",
			i->vname, name);
}

/*
 * send digits via INFO_REQ
 */
static int capi_send_info_digits(struct capi_pvt *i, char *digits, int len)
{
	MESSAGE_EXCHANGE_ERROR error;
	char buf[64];
	int a;
    
	memset(buf, 0, sizeof(buf));

	if (len > (sizeof(buf) - 2))
		len = sizeof(buf) - 2;
	
	buf[0] = len + 1;
	buf[1] = 0x80;
	for (a = 0; a < len; a++) {
		buf[a + 2] = digits[a];
	}

	error = capi_sendf(NULL, 0, CAPI_INFO_REQ, i->PLCI, get_capi_MessageNumber(),
		"s()",
		buf
	);
	if (error != 0) {
		return error;
	}
	cc_verbose(3, 1, VERBOSE_PREFIX_4 "%s: sent CALLEDPARTYNUMBER INFO digits = '%s' (PLCI=%#x)\n",
		i->vname, buf + 2, i->PLCI);
	return 0;
}

#ifdef CC_AST_HAS_VERSION_1_4
/*
 *  begin send DMTF
 */
static int pbx_capi_send_digit_begin(struct ast_channel *c, char digit)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	
	if ((i->state == CAPI_STATE_CONNECTED) && (i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
		/* we have a real connection, so send real DTMF */
		if ((capi_controllers[i->controller]->dtmf == 0) || (i->doDTMF > 0)) {
			/* let * fake it */
			return -1;
		}
	}
	
	return 0;
}
#endif

/*
 * send DTMF digit
 */
static int capi_send_dtmf_digits(struct capi_pvt *i, char digit)
{
	int ret;

	if (!(i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
		cc_verbose(2, 1, VERBOSE_PREFIX_3 "%s: send DTMF: B-channel not connected.\n",
			i->vname);
		return -1;
	}

	cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: send DTMF '%c'.\n",
		i->vname, digit);

	if ((capi_controllers[i->controller]->dtmf == 0) || (i->doDTMF > 0)) {
		/* let * fake it */
		cc_mutex_unlock(&i->lock);
		return -1;
	}

	ret = capi_sendf(i, 0, CAPI_FACILITY_REQ, i->NCCI, get_capi_MessageNumber(),
		"w(www(b)())",
		FACILITYSELECTOR_DTMF,
		3,	/* send DTMF digit */
		CAPI_DTMF_DURATION,	/* XXX: duration comes from asterisk in 1.4 */
		CAPI_DTMF_DURATION,
		digit
	);
		
	if (ret == 0) {
		cc_verbose(3, 0, VERBOSE_PREFIX_4 "%s: sent dtmf '%c'\n",
			i->vname, digit);
	}
	return ret;
}

/*
 * send a digit
 */
#if defined(CC_AST_HAS_VERSION_1_4) && defined(CC_AST_HAS_SEND_DIGIT_END_DURATION)
static int pbx_capi_send_digit(struct ast_channel *c, char digit, unsigned int duration)
#else
static int pbx_capi_send_digit(struct ast_channel *c, char digit)
#endif
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	char did[2];
	int ret = 0;
    
	if (i == NULL) {
		cc_log(LOG_ERROR, "No interface!\n");
		return -1;
	}

	cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: send_digit '%c' in state %d(%d)\n",
		i->vname, digit, i->state, c->_state);

	cc_mutex_lock(&i->lock);

	if ((c->_state == AST_STATE_DIALING) &&
	    (i->state != CAPI_STATE_DISCONNECTING)) {
		if (!(i->isdnstate & CAPI_ISDN_STATE_ISDNPROGRESS)) {
			did[0] = digit;
			did[1] = 0;
			strncat(i->dnid, did, sizeof(i->dnid) - 1);
			update_channel_name(i);	
			if ((i->isdnstate & CAPI_ISDN_STATE_SETUP_ACK) &&
			    (i->doOverlap == 0)) {
				ret = capi_send_info_digits(i, &digit, 1);
			} else {
				/* if no SETUP-ACK yet, add it to the overlap list */
				strncat(i->overlapdigits, &digit, 1);
				i->doOverlap = 1;
			}
			cc_mutex_unlock(&i->lock);
			return ret;
		} else {
			/* if PROGRESS arrived, we sent as DTMF */
			ret = capi_send_dtmf_digits(i, digit);
			cc_mutex_unlock(&i->lock);
			return ret;
		}
	}

	if (i->state == CAPI_STATE_CONNECTED) {
		/* we have a real connection, so send real DTMF */
		ret = capi_send_dtmf_digits(i, digit);
	}
	cc_mutex_unlock(&i->lock);
	return ret;
}

/*
 * send ALERT to ISDN line
 */
static int pbx_capi_alert(struct ast_channel *c)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	unsigned char *facilityarray = NULL;

	if ((i->state != CAPI_STATE_INCALL) &&
	    (i->state != CAPI_STATE_DID)) {
		cc_verbose(2, 1, VERBOSE_PREFIX_3 "%s: attempting ALERT in state %d\n",
			i->vname, i->state);
		return -1;
	}

	facilityarray = alloca(CAPI_MAX_FACILITYDATAARRAY_SIZE);
	cc_qsig_add_call_alert_data(facilityarray, i, c);

	if (capi_sendf(NULL, 0, CAPI_ALERT_REQ, i->PLCI, get_capi_MessageNumber(),
	    "(()()()s())", facilityarray) != 0) {
		return -1;
	}

	i->state = CAPI_STATE_ALERTING;
	ast_setstate(c, AST_STATE_RING);
	
	return 0;
}

/*
 * cleanup the interface
 */
static void interface_cleanup(struct capi_pvt *i)
{
	if (!i)
		return;

	cc_verbose(2, 1, VERBOSE_PREFIX_2 "%s: Interface cleanup PLCI=%#x\n",
		i->vname, i->PLCI);

	if (i->readerfd != -1) {
		close(i->readerfd);
		i->readerfd = -1;
	}
	if (i->writerfd != -1) {
		close(i->writerfd);
		i->writerfd = -1;
	}

	i->isdnstate = 0;
	i->cause = 0;

	i->whentohangup = 0;
	i->whentoqueuehangup = 0;

	i->FaxState &= ~CAPI_FAX_STATE_MASK;

	i->PLCI = 0;
	i->MessageNumber = 0;
	i->NCCI = 0;
	i->onholdPLCI = 0;
	i->doEC = i->doEC_global;
	i->ccbsnrhandle = 0;

	memset(i->cid, 0, sizeof(i->cid));
	memset(i->dnid, 0, sizeof(i->dnid));
	i->cid_ton = 0;

	i->rtpcodec = 0;
	if (i->rtp) {
		ast_rtp_destroy(i->rtp);
		i->rtp = NULL;
	}

	interface_cleanup_qsig(i);

	i->peer = NULL;	
	i->owner = NULL;
	i->used = NULL;

	if (i->channeltype == CAPI_CHANNELTYPE_NULL) {
		capi_interface_task(i, CAPI_INTERFACE_TASK_NULLIFREMOVE);
	}

	return;
}

/*
 * disconnect b3 and wait for confirmation 
 */
static void cc_disconnect_b3(struct capi_pvt *i, int wait) 
{
	struct timespec abstime;

	if (!(i->isdnstate & (CAPI_ISDN_STATE_B3_UP | CAPI_ISDN_STATE_B3_PEND)))
		return;

	if (wait) {
		cc_mutex_lock(&i->lock);
		capi_sendf(i, 1, CAPI_DISCONNECT_B3_REQ, i->NCCI, get_capi_MessageNumber(), "()");
	} else {
		capi_sendf(NULL, 0, CAPI_DISCONNECT_B3_REQ, i->NCCI, get_capi_MessageNumber(), "()");
		return;
	}

	/* wait for the B3 layer to go down */
	if ((i->isdnstate & (CAPI_ISDN_STATE_B3_UP | CAPI_ISDN_STATE_B3_PEND))) {
		i->waitevent = CAPI_WAITEVENT_B3_DOWN;
		abstime.tv_sec = time(NULL) + 2;
		abstime.tv_nsec = 0;
		cc_verbose(4, 1, "%s: wait for b3 down.\n",
			i->vname);
		if (ast_cond_timedwait(&i->event_trigger, &i->lock, &abstime) != 0) {
			cc_log(LOG_WARNING, "%s: timed out waiting for b3 down.\n",
				i->vname);
		} else {
			cc_verbose(4, 1, "%s: cond signal received for b3 down.\n",
				i->vname);
		}
	}
	cc_mutex_unlock(&i->lock);
	if ((i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
		cc_log(LOG_ERROR, "capi disconnect b3: didn't disconnect NCCI=0x%08x\n",
			i->NCCI);
	}
	return;
}

/*
 * send CONNECT_B3_REQ
 */
void cc_start_b3(struct capi_pvt *i)
{
	if (!(i->isdnstate & (CAPI_ISDN_STATE_B3_UP | CAPI_ISDN_STATE_B3_PEND))) {
		i->isdnstate |= CAPI_ISDN_STATE_B3_PEND;
		capi_sendf(NULL, 0, CAPI_CONNECT_B3_REQ, i->PLCI, get_capi_MessageNumber(),
			"s", capi_rtp_ncpi(i));
		cc_verbose(4, 1, VERBOSE_PREFIX_3 "%s: sent CONNECT_B3_REQ PLCI=%#x\n",
			i->vname, i->PLCI);
	}
}

/*
 * start early B3
 */
static void start_early_b3(struct capi_pvt *i)
{
	if (i->doB3 != CAPI_B3_DONT) { 
		/* we do early B3 Connect */
		cc_start_b3(i);
	}
}

/*
 * signal 'progress' to PBX 
 */
static void send_progress(struct capi_pvt *i)
{
	struct ast_frame fr = { AST_FRAME_CONTROL, };

	start_early_b3(i);

	if (!(i->isdnstate & CAPI_ISDN_STATE_PROGRESS)) {
		i->isdnstate |= CAPI_ISDN_STATE_PROGRESS;
		fr.subclass = AST_CONTROL_PROGRESS;
		local_queue_frame(i, &fr);
	}
	return;
}

/*
 * send disconnect_req
 */
static void capi_send_disconnect(unsigned int PLCI, struct capi_pvt *i)
{
	if (PLCI == 0) {
		return;
	}
	if (i) {
		capi_sendf(i, 1, CAPI_DISCONNECT_REQ, PLCI, get_capi_MessageNumber(), "()");
	} else {
		capi_sendf(NULL, 0, CAPI_DISCONNECT_REQ, PLCI, get_capi_MessageNumber(), "()");
	}
}

/*
 * hangup a line (CAPI messages)
 * (this must be called with i->lock held)
 */
void capi_activehangup(struct capi_pvt *i, int state)
{
	struct ast_channel *c = i->owner;
	const char *cause;

	if (c) {
		i->cause = c->hangupcause;
		if ((cause = pbx_builtin_getvar_helper(c, "PRI_CAUSE"))) {
			i->cause = atoi(cause);
		}
	
		if ((i->isdnstate & CAPI_ISDN_STATE_ECT)) {
			cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: activehangup ECT call\n",
				i->vname);
		}
	}

	cc_verbose(2, 1, VERBOSE_PREFIX_3 "%s: activehangingup (cause=%d) for PLCI=%#x\n",
		i->vname, i->cause, i->PLCI);


	if ((state == CAPI_STATE_ALERTING) ||
	    (state == CAPI_STATE_DID) || (state == CAPI_STATE_INCALL)) {
		capi_sendf(NULL, 0, CAPI_CONNECT_RESP, i->PLCI, i->MessageNumber,
			"w()()()()()",
			(i->cause) ? (0x3480 | (i->cause & 0x7f)) : 2);
		return;
	}
	
	if ((i->isdnstate & CAPI_ISDN_STATE_STAYONLINE)) {
		/* user has requested to leave channel online for further actions
		   like CCBS */
		cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: disconnect deferred, stay-online mode PLCI=%#x\n",
			i->vname, i->PLCI);
		i->whentohangup = time(NULL) + 18; /* timeout 18 seconds */
		return;
	}

	/* active disconnect */
	if ((i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
		cc_disconnect_b3(i, 0);
		return;
	}

	if (i->channeltype == CAPI_CHANNELTYPE_NULL) {
		if (i->PLCI == 0) {
			interface_cleanup(i);
			return;
		}
	}
	
	if ((state == CAPI_STATE_CONNECTED) || (state == CAPI_STATE_CONNECTPENDING) ||
	    (state == CAPI_STATE_ANSWERING) || (state == CAPI_STATE_ONHOLD)) {
		if (i->PLCI == 0) {
			/* CONNECT_CONF not received yet? */
			capi_wait_conf(i, CAPI_CONNECT_CONF);
		}
		capi_send_disconnect(i->PLCI, i);
	}
	return;
}

/*
 * PBX tells us to hangup a line
 */
static int pbx_capi_hangup(struct ast_channel *c)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	int cleanup = 0;
	int state;

	/*
	 * hmm....ok...this is called to free the capi interface (passive disconnect)
	 * or to bring down the channel (active disconnect)
	 */

	if (i == NULL) {
		cc_log(LOG_ERROR, "channel has no interface!\n");
		return -1;
	}

	cc_mutex_lock(&i->lock);

	state = i->state;

	cc_verbose(3, 0, VERBOSE_PREFIX_2 "%s: CAPI Hangingup for PLCI=%#x in state %d\n",
		i->vname, i->PLCI, state);
 
	/* are we down, yet? */
	if (state != CAPI_STATE_DISCONNECTED) {
		/* no */
		i->state = CAPI_STATE_DISCONNECTING;
	} else {
		cleanup = 1;
	}
	
	if ((i->doDTMF > 0) && (i->vad != NULL)) {
		ast_dsp_free(i->vad);
		i->vad = NULL;
	}
	
	if (cleanup) {
		/* disconnect already done, so cleanup */
		interface_cleanup(i);
	} else {
		/* not disconnected yet, we must actively do it */
		capi_activehangup(i, state);
	}

	i->owner = NULL;
	CC_CHANNEL_PVT(c) = NULL;

	cc_mutex_unlock(&i->lock);

	ast_setstate(c, AST_STATE_DOWN);

#ifdef CC_AST_HAS_VERSION_1_4
	ast_atomic_fetchadd_int(&usecnt, -1);
#else
	cc_mutex_lock(&usecnt_lock);
	usecnt--;
	cc_mutex_unlock(&usecnt_lock);
#endif
	ast_update_use_count();
	
	return 0;
}

/*
 * PBX tells us to make a call
 */
static int pbx_capi_call(struct ast_channel *c, char *idest, int timeout)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	char *dest, *interface, *param, *ocid;
	char buffer[AST_MAX_EXTENSION];
	char called[AST_MAX_EXTENSION], calling[AST_MAX_EXTENSION];
	char callerid[AST_MAX_EXTENSION];
	int CLIR;
	int callernplan = 0;
	int use_defaultcid = 0;
	const char *ton, *p;
	char *osa = NULL;
	char *dsa = NULL;
	char callingsubaddress[AST_MAX_EXTENSION];
	char calledsubaddress[AST_MAX_EXTENSION];
	int doqsig;
	unsigned char *facilityarray = NULL;
	
	MESSAGE_EXCHANGE_ERROR  error;

	cc_copy_string(buffer, idest, sizeof(buffer));
	capi_parse_dialstring(buffer, &interface, &dest, &param, &ocid);

	/* init param settings */
	i->doB3 = CAPI_B3_DONT;
	i->doOverlap = 0;
	memset(i->overlapdigits, 0, sizeof(i->overlapdigits));
	doqsig = i->qsigfeat;

	/* parse the parameters */
	while ((param) && (*param)) {
		switch (*param) {
		case 'b':	/* always B3 */
			if (i->doB3 != CAPI_B3_DONT)
				cc_log(LOG_WARNING, "B3 already set in '%s'\n", idest);
			i->doB3 = CAPI_B3_ALWAYS;
			break;
		case 'B':	/* only do B3 on successfull calls */
			if (i->doB3 != CAPI_B3_DONT)
				cc_log(LOG_WARNING, "B3 already set in '%s'\n", idest);
			i->doB3 = CAPI_B3_ON_SUCCESS;
			break;
		case 'o':	/* overlap sending of digits */
			if (i->doOverlap)
				cc_log(LOG_WARNING, "Overlap already set in '%s'\n", idest);
			i->doOverlap = 1;
			break;
		case 'd':	/* use default cid */
			if (use_defaultcid)
				cc_log(LOG_WARNING, "Default CID already set in '%s'\n", idest);
			use_defaultcid = 1;
			break;
		case 's':	/* stay online */
			if ((i->isdnstate & CAPI_ISDN_STATE_STAYONLINE))
				cc_log(LOG_WARNING, "'stay-online' already set in '%s'\n", idest);
			i->isdnstate |= CAPI_ISDN_STATE_STAYONLINE;
			break;
		case 'q':	/* disable QSIG */
			cc_verbose(4, 0, VERBOSE_PREFIX_4 "%s: QSIG extensions for this call disabled\n",
				i->vname);
			doqsig = 0;
			break;
		default:
			cc_log(LOG_WARNING, "Unknown parameter '%c' in '%s', ignoring.\n",
				*param, idest);
		}
		param++;
	}
	if (((!dest) || (!dest[0])) && (i->doB3 != CAPI_B3_ALWAYS)) {
		cc_log(LOG_ERROR, "No destination or dialtone requested in '%s'\n", idest);
		return -1;
	}

	i->peer = cc_get_peer_link_id(pbx_builtin_getvar_helper(c, "CAPIPEERLINKID"));
	i->outgoing = 1;
	i->transfercapability = c->transfercapability;
	i->isdnstate |= CAPI_ISDN_STATE_PBX;
	i->state = CAPI_STATE_CONNECTPENDING;
	ast_setstate(c, AST_STATE_DIALING);
	i->MessageNumber = get_capi_MessageNumber();

	/* if this is a CCBS/CCNR callback call */
	if (i->ccbsnrhandle) {
		_cword cip = (_cword)tcap2cip(i->transfercapability);
		_cword rbref;

		i->doOverlap = 0;
		rbref = capi_ccbsnr_take_ref(i->ccbsnrhandle);

		if ((rbref == 0xdead) ||
		    ((capi_sendf(NULL, 0, CAPI_FACILITY_REQ, i->controller, i->MessageNumber,
			"w(w(www(wwwsss())()()()()))",
			FACILITYSELECTOR_SUPPLEMENTARY,
			0x0012,  /* CCBS call */
			rbref,   /* reference */
			cip,     /* CIP */
			0, /* reserved */
			 /* B protocol */
			b_protocol_table[i->bproto].b1protocol,
			b_protocol_table[i->bproto].b2protocol,
			b_protocol_table[i->bproto].b3protocol,
			b_protocol_table[i->bproto].b1configuration,
			b_protocol_table[i->bproto].b2configuration,
			b_protocol_table[i->bproto].b3configuration
			/* */ /* BC */
			/* */ /* LLC */
			/* */  /* HLC */
			/* */  /* Additional Info */
			)))) {
			i->state = CAPI_STATE_DISCONNECTED;
			ast_setstate(c, AST_STATE_RESERVED);
			return 1;
		}
		return 0;
	}

	CLIR = c->cid.cid_pres;
	callernplan = c->cid.cid_ton & 0x7f;

	if ((ton = pbx_builtin_getvar_helper(c, "CALLERTON"))) {
		callernplan = atoi(ton) & 0x7f;
	}
	i->cid_ton = callernplan;

	cc_verbose(1, 1, VERBOSE_PREFIX_2 "%s: Call %s %s%s (pres=0x%02x, ton=0x%02x)\n",
		i->vname, c->name, i->doB3 ? "with B3 ":" ",
		i->doOverlap ? "overlap":"", CLIR, callernplan);

	if ((p = pbx_builtin_getvar_helper(c, "CALLINGSUBADDRESS"))) {
		callingsubaddress[0] = strlen(p) + 1;
		callingsubaddress[1] = 0x80;
		strncpy(&callingsubaddress[2], p, sizeof(callingsubaddress) - 3);
		osa = callingsubaddress;
	}
	if ((p = pbx_builtin_getvar_helper(c, "CALLEDSUBADDRESS"))) {
		calledsubaddress[0] = strlen(p) + 1;
		calledsubaddress[1] = 0x80;
		strncpy(&calledsubaddress[2], p, sizeof(calledsubaddress) - 3);
		dsa = calledsubaddress;
	}

	if (capi_tcap_is_digital(i->transfercapability)) {
		i->bproto = CC_BPROTO_TRANSPARENT;
		cc_verbose(4, 0, VERBOSE_PREFIX_2 "%s: is digital call, set proto to TRANSPARENT\n",
			i->vname);
	}
	if ((i->doOverlap) && (strlen(dest))) {
		cc_copy_string(i->overlapdigits, dest, sizeof(i->overlapdigits));
		called[0] = 1;
	} else {
		i->doOverlap = 0;
		called[0] = strlen(dest) + 1;
	}
	called[1] = 0x80;
	strncpy(&called[2], dest, sizeof(called) - 3);

	if (c->cid.cid_num) {
		cc_copy_string(callerid, c->cid.cid_num, sizeof(callerid));
	} else {
		memset(callerid, 0, sizeof(callerid));
	}

	if (use_defaultcid) {
		cc_copy_string(callerid, i->defaultcid, sizeof(callerid));
	} else if (ocid) {
		cc_copy_string(callerid, ocid, sizeof(callerid));
	}
	cc_copy_string(i->cid, callerid, sizeof(i->cid));

	calling[0] = strlen(callerid) + 2;
	calling[1] = callernplan;
	calling[2] = 0x80 | (CLIR & 0x63);
	strncpy(&calling[3], callerid, sizeof(calling) - 4);

	if (doqsig) {
		facilityarray = alloca(CAPI_MAX_FACILITYDATAARRAY_SIZE);
		cc_qsig_add_call_setup_data(facilityarray, i, c);
	}

	error = capi_sendf(NULL, 0, CAPI_CONNECT_REQ, i->controller, i->MessageNumber,
		"wssss(wwwsss())()()()((w)()()s())",
		tcap2cip(i->transfercapability), /* CIP value */
		called, /* called party number */
		calling, /* calling party number */
		dsa, /* called party subaddress */
		osa, /* calling party subaddress */
		 /* B protocol */
		b_protocol_table[i->bproto].b1protocol,
		b_protocol_table[i->bproto].b2protocol,
		b_protocol_table[i->bproto].b3protocol,
		b_protocol_table[i->bproto].b1configuration,
		b_protocol_table[i->bproto].b2configuration,
		b_protocol_table[i->bproto].b3configuration,
		 /* BC */
		 /* LLC */
		 /* HLC */
		 /* Additional Info */
		  0x0000, /* B channel info */
		   /* Keypad facility */
		   /* User-User data */
		  facilityarray /* Facility data array */
	);

	if (error) {
		i->state = CAPI_STATE_DISCONNECTED;
		ast_setstate(c, AST_STATE_RESERVED);
		return error;
	}

	/* now we shall return .... the rest has to be done by handle_msg */
	return 0;
}

/*
 * answer a capi call
 */
static int capi_send_answer(struct ast_channel *c, _cstruct b3conf)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	char buf[CAPI_MAX_STRING];
	const char *dnid;
	const char *connectednumber;
	unsigned char *facilityarray = NULL;
    
	if (i->state == CAPI_STATE_DISCONNECTED) {
		cc_verbose(3, 0, VERBOSE_PREFIX_2 "%s: Not answering disconnected call.\n",
			i->vname);
		return -1;	
	}

	if ((i->isdnmode == CAPI_ISDNMODE_DID) &&
	    ((strlen(i->incomingmsn) < strlen(i->dnid)) && 
	    (strcmp(i->incomingmsn, "*")))) {
		dnid = i->dnid + strlen(i->incomingmsn);
	} else {
		dnid = i->dnid;
	}
	if ((connectednumber = pbx_builtin_getvar_helper(c, "CONNECTEDNUMBER"))) {
		dnid = connectednumber;
	}

	if (strlen(dnid)) {
		buf[0] = strlen(dnid) + 2;
		buf[1] = 0x00;
		buf[2] = 0x80;
		strncpy(&buf[3], dnid, sizeof(buf) - 4);
	} else {
		buf[0] = 0x00;
	}
	if (!b3conf) {
		b3conf = b_protocol_table[i->bproto].b3configuration;
	}

	cc_verbose(3, 0, VERBOSE_PREFIX_2 "%s: Answering for %s\n",
		i->vname, dnid);
		
	facilityarray = alloca(CAPI_MAX_FACILITYDATAARRAY_SIZE);
	cc_qsig_add_call_answer_data(facilityarray, i, c);

	if (i->ntmode) {
		/* in NT-mode we send the current local time to device */
		capi_facility_add_datetime(facilityarray);
	}

	if (capi_sendf(NULL, 0, CAPI_CONNECT_RESP, i->PLCI, i->MessageNumber,
	    "w(wwwssss)s()()(()()()s())",
		0, /* accept call */
		/* B protocol */
		b_protocol_table[i->bproto].b1protocol,
		b_protocol_table[i->bproto].b2protocol,
		b_protocol_table[i->bproto].b3protocol,
		b_protocol_table[i->bproto].b1configuration,
		b_protocol_table[i->bproto].b2configuration,
		b3conf,
		capi_set_global_configuration(i),
		buf, /* connected number */
		/* connected subaddress */
		/* LLC */
		/* Additional info */
		facilityarray		
		) != 0) {
		return -1;	
	}
    
	i->state = CAPI_STATE_ANSWERING;
	i->doB3 = CAPI_B3_DONT;
	i->outgoing = 0;

	return 0;
}

/*
 * PBX tells us to answer a call
 */
static int pbx_capi_answer(struct ast_channel *c)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	int ret;

	i->bproto = CC_BPROTO_TRANSPARENT;

	if (i->rtp) {
		if (!capi_tcap_is_digital(i->transfercapability))
			i->bproto = CC_BPROTO_RTP;
	}

	ret = capi_send_answer(c, NULL);
	return ret;
}

/*
 * read for a channel
 */
static struct ast_frame *pbx_capi_read(struct ast_channel *c) 
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	struct ast_frame *f;

	f = capi_read_pipeframe(i);

	if ((f) && (f->frametype == AST_FRAME_VOICE) && (f->datalen > 0)) {
		if ((i->doDTMF > 0) && (i->vad != NULL) ) {
			f = ast_dsp_process(c, i->vad, f);
		}
	}
	return f;
}

/*
 * PBX tells us to write for a channel
 */
static int pbx_capi_write(struct ast_channel *c, struct ast_frame *f)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	int ret = 0;

	ret = capi_write_frame(i, f);

	return ret;
}

/*
 * new channel (masq)
 */
static int pbx_capi_fixup(struct ast_channel *oldchan, struct ast_channel *newchan)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(newchan);

	cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: %s fixup now %s\n",
		i->vname, oldchan->name, newchan->name);

	cc_mutex_lock(&i->lock);
	i->owner = newchan;
	cc_mutex_unlock(&i->lock);
	return 0;
}

/*
 * activate (another B protocol)
 */
static void cc_select_b(struct capi_pvt *i, _cstruct b3conf)
{
	if (!b3conf) {
		b3conf = b_protocol_table[i->bproto].b3configuration;
	}

	capi_sendf(NULL, 0, CAPI_SELECT_B_PROTOCOL_REQ, i->PLCI, get_capi_MessageNumber(),
		"(wwwssss)",
		b_protocol_table[i->bproto].b1protocol,
		b_protocol_table[i->bproto].b2protocol,
		b_protocol_table[i->bproto].b3protocol,
		b_protocol_table[i->bproto].b1configuration,
		b_protocol_table[i->bproto].b2configuration,
		b3conf,
		capi_set_global_configuration(i)
	);
}

/*
 * do line initerconnect
 */
static int line_interconnect(struct capi_pvt *i0, struct capi_pvt *i1, int start)
{
	if ((i0->isdnstate & CAPI_ISDN_STATE_DISCONNECT) ||
	    (i1->isdnstate & CAPI_ISDN_STATE_DISCONNECT))
		return -1;

	if ((!(i0->isdnstate & CAPI_ISDN_STATE_B3_UP)) || 
	    (!(i1->isdnstate & CAPI_ISDN_STATE_B3_UP))) {
		cc_verbose(3, 1, VERBOSE_PREFIX_2
			"%s:%s line interconnect aborted, at least "
			"one channel is not connected.\n",
			i0->vname, i1->vname);
		return -1;
	}

	if (start) {
		/* connect */
		capi_sendf(i1, 0, CAPI_FACILITY_REQ, i0->PLCI, get_capi_MessageNumber(),
			"w(w(d((dd))))",
			FACILITYSELECTOR_LINE_INTERCONNECT,
			0x0001,
			/* struct LI Request Parameter */
			0x00000000, /* Data Path */
			/* struct */
			/* struct LI Request Connect Participant */
			i1->PLCI,
			0x00000003 /* Data Path Participant */
		);
		i0->isdnstate |= CAPI_ISDN_STATE_LI;
		i1->isdnstate |= CAPI_ISDN_STATE_LI;
	} else {
		/* disconnect */
		capi_sendf(i1, 0, CAPI_FACILITY_REQ, i0->PLCI, get_capi_MessageNumber(),
			"w(w(d))",
			FACILITYSELECTOR_LINE_INTERCONNECT,
			0x0002,
			i1->PLCI
		);
		i0->isdnstate &= ~CAPI_ISDN_STATE_LI;
		i1->isdnstate &= ~CAPI_ISDN_STATE_LI;
	}

	return 0;
}

#if 0
/*
 * disconnect b3 and bring it up with another protocol
 */
static void cc_switch_b_protocol(struct capi_pvt *i)
{
	int waitcount = 200;

	cc_disconnect_b3(i, 1);

	i->isdnstate |= CAPI_ISDN_STATE_B3_CHANGE;
	cc_select_b(i, NULL);

	if (i->outgoing) {
		/* on outgoing call we must do the connect-b3 request */
		cc_start_b3(i);
	}

	/* wait for the B3 layer to come up */
	while ((waitcount > 0) &&
	       (!(i->isdnstate & CAPI_ISDN_STATE_B3_UP))) {
		usleep(10000);
		waitcount--;
	}
	if (!(i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
		cc_log(LOG_ERROR, "capi switch b3: no b3 up\n");
	}
}

/*
 * set the b3 protocol to transparent
 */
static int cc_set_transparent(struct capi_pvt *i)
{
	if (i->bproto != CC_BPROTO_RTP) {
		/* nothing to do */
		return 0;
	}

	i->bproto = CC_BPROTO_TRANSPARENT;
	cc_switch_b_protocol(i);

	return 1;
}

/*
 * set the b3 protocol to RTP (if wanted)
 */
static void cc_unset_transparent(struct capi_pvt *i, int rtpwanted)
{
	if ((!rtpwanted) ||
	    (i->isdnstate & CAPI_ISDN_STATE_DISCONNECT))
		return;

	i->bproto = CC_BPROTO_RTP;
	cc_switch_b_protocol(i);

	return;
}
#endif

/*
 * try call transfer instead of bridge
 */
static int pbx_capi_bridge_transfer(
	struct ast_channel *c0,
	struct ast_channel *c1,
	struct capi_pvt *i0,
	struct capi_pvt *i1)
{
	int ret = 0;
	ast_group_t tgroup0 = i0->transfergroup;
	ast_group_t tgroup1 = i1->transfergroup;
	struct timespec abstime;
	const char* var;

	/* variable may override config */
	if ((var = pbx_builtin_getvar_helper(c0, "TRANSFERGROUP")) != NULL) {
		tgroup0 = ast_get_group((char *)var);
	}
	if ((var = pbx_builtin_getvar_helper(c1, "TRANSFERGROUP")) != NULL) {
		tgroup1 = ast_get_group((char *)var);
	}

	if ((!((1 << i0->controller) & tgroup1)) ||
		(!((1 << i1->controller) & tgroup0))) {
		/* transfer between those controllers is not allowed */
		cc_verbose(4, 1, "%s: transfergroup mismatch %d(0x%x),%d(0x%x)\n",
			i0->vname, i0->controller, tgroup1, i1->controller, tgroup0);
		return 0;
	}

	if ((i0->qsigfeat) && (i1->qsigfeat)) {
		/* QSIG */
		ret = pbx_capi_qsig_bridge(i0, i1);
	
		if (ret == 2) {	
			/* don't do bridge - call transfer is active */
			cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s:%s cancelled bridge (path replacement was sent) for %s and %s\n",
				   i0->vname, i1->vname, c0->name, c1->name);
		}
	} else {
		/* standard ECT */

		/* put first call on hold */
		pbx_capi_hold(c1, NULL);
		cc_mutex_lock(&i1->lock);
		if ((i1->onholdPLCI != 0) && (i1->state != CAPI_STATE_ONHOLD)) {
			i1->waitevent = CAPI_WAITEVENT_HOLD_IND;
			abstime.tv_sec = time(NULL) + 2;
			abstime.tv_nsec = 0;
			if (ast_cond_timedwait(&i1->event_trigger, &i1->lock, &abstime) != 0) {
				cc_log(LOG_WARNING, "%s: timed out waiting for HOLD.\n", i1->vname);
			} else {
				cc_verbose(4, 1, "%s: cond signal received for HOLD.\n", i1->vname);
			}
		}
		cc_mutex_unlock(&i1->lock);

		if (i1->state != CAPI_STATE_ONHOLD) {
			cc_verbose(4, 1, "%s: HOLD impossible, transfer aborted.\n", i1->vname);
			return 0;
		}

		/* start the ECT */
		cc_disconnect_b3(i0, 1);

		cc_mutex_lock(&i0->lock);

		/* ECT */
		capi_sendf(i0, 1, CAPI_FACILITY_REQ, i0->PLCI, get_capi_MessageNumber(),
			"w(w(d))",
			FACILITYSELECTOR_SUPPLEMENTARY,
			0x0006,  /* ECT */
			i1->PLCI
		);

		i1->isdnstate &= ~CAPI_ISDN_STATE_HOLD;
		i1->isdnstate |= CAPI_ISDN_STATE_ECT;
		i0->isdnstate |= CAPI_ISDN_STATE_ECT;
	
		i0->waitevent = CAPI_WAITEVENT_ECT_IND;
		abstime.tv_sec = time(NULL) + 2;
		abstime.tv_nsec = 0;
		if (ast_cond_timedwait(&i0->event_trigger, &i0->lock, &abstime) != 0) {
			cc_log(LOG_WARNING, "%s: timed out waiting for ECT.\n", i0->vname);
		} else {
			cc_verbose(4, 1, "%s: cond signal received for ECT.\n", i0->vname);
		}

		cc_mutex_unlock(&i0->lock);

		cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: sent ECT for PLCI=%#x to PLCI=%#x\n",
			i1->vname, i1->PLCI, i0->PLCI);

		ret = 1;
	}

	return ret;
}

/*
 * native bridging / line interconnect
 */
static CC_BRIDGE_RETURN pbx_capi_bridge(
	struct ast_channel *c0,
	struct ast_channel *c1,
	int flags, struct ast_frame **fo,
	struct ast_channel **rc,
	int timeoutms)
{
	struct capi_pvt *i0 = CC_CHANNEL_PVT(c0);
	struct capi_pvt *i1 = CC_CHANNEL_PVT(c1);
	CC_BRIDGE_RETURN ret = AST_BRIDGE_COMPLETE;

	cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s:%s Requested native bridge for %s and %s\n",
		i0->vname, i1->vname, c0->name, c1->name);

	if ((i0->isdnstate & CAPI_ISDN_STATE_ECT) ||
	    (i1->isdnstate & CAPI_ISDN_STATE_ECT)) {
		/* If an ECT is already in progress, forget about the bridge */
		return AST_BRIDGE_FAILED;
	}

	switch(pbx_capi_bridge_transfer(c0, c1, i0, i1)) {
	case 1: /* transfer was sucessful */
		return ret;
	case 2: /* not successful, abort */
		return AST_BRIDGE_FAILED_NOWARN;
	default: /* go on with line-interconnect */
		break;
	}

	/* do bridge aka line-interconnect here */
	
	if ((!i0->bridge) || (!i1->bridge))
		return AST_BRIDGE_FAILED_NOWARN;
	
	if ((!capi_controllers[i0->controller]->lineinterconnect) ||
	    (!capi_controllers[i1->controller]->lineinterconnect)) {
		return AST_BRIDGE_FAILED_NOWARN;
	}

	capi_wait_for_b3_up(i0);
	capi_wait_for_b3_up(i1);
	
	if (!(flags & AST_BRIDGE_DTMF_CHANNEL_0))
		capi_detect_dtmf(i0, 0);

	if (!(flags & AST_BRIDGE_DTMF_CHANNEL_1))
		capi_detect_dtmf(i1, 0);

	capi_echo_canceller(i0, EC_FUNCTION_DISABLE);
	capi_echo_canceller(i1, EC_FUNCTION_DISABLE);

	if (line_interconnect(i0, i1, 1)) {
		ret = AST_BRIDGE_FAILED;
		goto return_from_bridge;
	}

	for (;;) {
		struct ast_channel *c0_priority[2] = {c0, c1};
		struct ast_channel *c1_priority[2] = {c1, c0};
		int priority = 0;
		struct ast_frame *f;
		struct ast_channel *who;

		who = ast_waitfor_n(priority ? c0_priority : c1_priority, 2, &timeoutms);
		if (!who) {
			if (!timeoutms) {
				ret = AST_BRIDGE_RETRY;
				break;
			}
			continue;
		}
		f = ast_read(who);
		if (!f || (f->frametype == AST_FRAME_CONTROL)
		       || (f->frametype == AST_FRAME_DTMF)) {
			*fo = f;
			*rc = who;
			ret = AST_BRIDGE_COMPLETE;
			break;
		}
		if (who == c0) {
			ast_write(c1, f);
		} else {
			ast_write(c0, f);
		}
		ast_frfree(f);

		/* Swap who gets priority */
		priority = !priority;
	}

	line_interconnect(i0, i1, 0);

return_from_bridge:

	if (!(flags & AST_BRIDGE_DTMF_CHANNEL_0))
		capi_detect_dtmf(i0, 1);

	if (!(flags & AST_BRIDGE_DTMF_CHANNEL_1))
		capi_detect_dtmf(i1, 1);

	capi_echo_canceller(i0, EC_FUNCTION_ENABLE);
	capi_echo_canceller(i1, EC_FUNCTION_ENABLE);

	return ret;
}

/*
 * a new channel is needed
 */
static struct ast_channel *capi_new(struct capi_pvt *i, int state)
{
	struct ast_channel *tmp;
	int fmt;

#ifdef CC_AST_HAS_EXT_CHAN_ALLOC
	tmp = ast_channel_alloc(0, state, i->cid, NULL,
#ifdef CC_AST_HAS_EXT2_CHAN_ALLOC
		i->accountcode, i->dnid, i->context, i->amaflags,
#endif
		"CAPI/%s/%s-%x", i->vname, i->dnid, capi_counter++);
#else
	tmp = ast_channel_alloc(0);
#endif
	
	if (tmp == NULL) {
		cc_log(LOG_ERROR, "Unable to allocate channel!\n");
		return NULL;
	}

#ifndef CC_AST_HAS_EXT_CHAN_ALLOC
#ifdef CC_AST_HAS_STRINGFIELD_IN_CHANNEL
	ast_string_field_build(tmp, name, "CAPI/%s/%s-%x",
		i->vname, i->dnid, capi_counter++);
#else
	snprintf(tmp->name, sizeof(tmp->name) - 1, "CAPI/%s/%s-%x",
		i->vname, i->dnid, capi_counter++);
#endif
#endif
#ifndef CC_AST_HAS_VERSION_1_4
	tmp->type = channeltype;
#endif

	if (!(capi_create_reader_writer_pipe(i))) {
		ast_channel_free(tmp);
		return NULL;
	}
	tmp->fds[0] = i->readerfd;

	if (i->smoother != NULL) {
		ast_smoother_reset(i->smoother, CAPI_MAX_B3_BLOCK_SIZE);
	}

	i->state = CAPI_STATE_DISCONNECTED;
	i->calledPartyIsISDN = 1;
	i->doB3 = CAPI_B3_DONT;
	i->doES = i->ES;
	i->outgoing = 0;
	i->onholdPLCI = 0;
	i->doholdtype = i->holdtype;
	i->B3q = 0;
	i->B3count = 0;
	memset(i->txavg, 0, ECHO_TX_COUNT);

	if (i->doDTMF > 0) {
		i->vad = ast_dsp_new();
		ast_dsp_set_features(i->vad, DSP_FEATURE_DTMF_DETECT);
		if (i->doDTMF > 1) {
			ast_dsp_digitmode(i->vad, DSP_DIGITMODE_DTMF | DSP_DIGITMODE_RELAXDTMF);
		}
	}

	CC_CHANNEL_PVT(tmp) = i;

	tmp->callgroup = i->callgroup;
	tmp->pickupgroup = i->pickupgroup;
	tmp->nativeformats = capi_capability;
	i->bproto = CC_BPROTO_TRANSPARENT;
	if ((i->rtpcodec = (capi_controllers[i->controller]->rtpcodec & i->capability))) {
		if (capi_alloc_rtp(i)) {
			/* error on rtp alloc */
			i->rtpcodec = 0;
		} else {
			/* start with rtp */
			tmp->nativeformats = i->rtpcodec;
			i->bproto = CC_BPROTO_RTP;
		}
	}
	fmt = ast_best_codec(tmp->nativeformats);
	i->codec = fmt;
	tmp->readformat = fmt;
	tmp->writeformat = fmt;

	tmp->tech = &capi_tech;
	tmp->rawreadformat = fmt;
	tmp->rawwriteformat = fmt;

	cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: setting format %s - %s%s\n",
		i->vname, ast_getformatname(fmt),
		ast_getformatname_multiple(alloca(80), 80,
		tmp->nativeformats),
		(i->rtp) ? " (RTP)" : "");

	if (!ast_strlen_zero(i->cid)) {
		if (tmp->cid.cid_num) {
			free(tmp->cid.cid_num);
		}
		tmp->cid.cid_num = strdup(i->cid);
	}
	if (!ast_strlen_zero(i->dnid)) {
		if (tmp->cid.cid_dnid) {
			free(tmp->cid.cid_dnid);
		}
		tmp->cid.cid_dnid = strdup(i->dnid);
	}
	tmp->cid.cid_ton = i->cid_ton;

#ifndef CC_AST_HAS_EXT2_CHAN_ALLOC
	if (i->amaflags)
		tmp->amaflags = i->amaflags;
	
	cc_copy_string(tmp->context, i->context, sizeof(tmp->context));
	cc_copy_string(tmp->exten, i->dnid, sizeof(tmp->exten));
#ifdef CC_AST_HAS_STRINGFIELD_IN_CHANNEL
	ast_string_field_set(tmp, accountcode, i->accountcode);
	ast_string_field_set(tmp, language, i->language);
#else
	cc_copy_string(tmp->accountcode, i->accountcode, sizeof(tmp->accountcode));
	cc_copy_string(tmp->language, i->language, sizeof(tmp->language));
#endif
#endif

#ifdef CC_AST_HAS_STRINGFIELD_IN_CHANNEL
	ast_string_field_set(tmp, language, i->language);
#else
	cc_copy_string(tmp->language, i->language, sizeof(tmp->language));
#endif

	i->owner = tmp;
	i->used = tmp;

#ifdef CC_AST_HAS_VERSION_1_4
	ast_atomic_fetchadd_int(&usecnt, 1);
	ast_jb_configure(tmp, &i->jbconf);
#else
	cc_mutex_lock(&usecnt_lock);
	usecnt++;
	cc_mutex_unlock(&usecnt_lock);
#endif
	ast_update_use_count();
	
#ifndef CC_AST_HAS_EXT_CHAN_ALLOC
	ast_setstate(tmp, state);
#endif

	return tmp;
}

/*
 * PBX wants us to dial ...
 */
static struct ast_channel *
pbx_capi_request(const char *type, int format, void *data, int *cause)
{
	struct capi_pvt *i;
	struct ast_channel *tmp = NULL;
	char *dest, *interface, *param, *ocid;
	char buffer[CAPI_MAX_STRING];
	ast_group_t capigroup = 0;
	unsigned int controller = 0;
	unsigned int ccbsnrhandle = 0;

	cc_verbose(1, 1, VERBOSE_PREFIX_4 "data = %s format=%d\n", (char *)data, format);

	cc_copy_string(buffer, (char *)data, sizeof(buffer));
	capi_parse_dialstring(buffer, &interface, &dest, &param, &ocid);

	if ((!interface) || (!dest)) {
		cc_log(LOG_ERROR, "Syntax error in dialstring. Read the docs!\n");
		*cause = AST_CAUSE_INVALID_NUMBER_FORMAT;
		return NULL;
	}

	if (interface[0] == 'g') {
		capigroup = ast_get_group(interface + 1);
		cc_verbose(1, 1, VERBOSE_PREFIX_4 "capi request group = %d\n",
				(unsigned int)capigroup);
	} else if (!strncmp(interface, "contr", 5)) {
		controller = atoi(interface + 5);
		cc_verbose(1, 1, VERBOSE_PREFIX_4 "capi request controller = %d\n",
				controller);
	} else if (!strncmp(interface, "ccbs", 4)) {
		ccbsnrhandle = (unsigned int)strtoul(dest, NULL, 0);
		cc_verbose(1, 1, VERBOSE_PREFIX_4 "capi request ccbs handle = %u\n",
				ccbsnrhandle);
		if ((controller = capi_get_ccbsnrcontroller(ccbsnrhandle)) == 0) {
			cc_verbose(2, 0, VERBOSE_PREFIX_3 "didn't find CCBS handle %u\n",
				ccbsnrhandle);
			*cause = AST_CAUSE_REQUESTED_CHAN_UNAVAIL;
			return NULL;
		}
	} else {
		cc_verbose(1, 1, VERBOSE_PREFIX_4 "capi request for interface '%s'\n",
				interface);
 	}

	cc_mutex_lock(&iflock);
	
	for (i = capi_iflist; i; i = i->next) {
		if ((i->used) || (i->channeltype != CAPI_CHANNELTYPE_B)) {
			/* if already in use or no real channel */
			continue;
		}
		/* unused channel */
		if (controller) {
			/* DIAL(CAPI/contrX/...) */
			if (i->controller != controller) {
				/* keep on running! */
				continue;
			}
		} else {
			/* DIAL(CAPI/gX/...) */
			if ((interface[0] == 'g') && (!(i->group & capigroup))) {
				/* keep on running! */
				continue;
			}
			/* DIAL(CAPI/<interface-name>/...) */
			if ((interface[0] != 'g') && (strcmp(interface, i->name))) {
				/* keep on running! */
				continue;
			}
		}
		/* when we come here, we found a free controller match */
		cc_copy_string(i->dnid, dest, sizeof(i->dnid));
		tmp = capi_new(i, AST_STATE_RESERVED);
		if (!tmp) {
			cc_log(LOG_ERROR, "cannot create new capi channel\n");
			interface_cleanup(i);
		}
		i->PLCI = 0;
		i->outgoing = 1;	/* this is an outgoing line */
		i->ccbsnrhandle = ccbsnrhandle;
		cc_mutex_unlock(&iflock);
		return tmp;
	}
	cc_mutex_unlock(&iflock);
	cc_verbose(2, 0, VERBOSE_PREFIX_3 "didn't find capi device for interface '%s'\n",
		interface);
	*cause = AST_CAUSE_REQUESTED_CHAN_UNAVAIL;
	return NULL;
}

/*
 * fill out fax conf struct
 */
static void setup_b3_fax_config(B3_PROTO_FAXG3 *b3conf, int fax_format, char *stationid, char *headline)
{
	int len1;
	int len2;

	cc_verbose(3, 1, VERBOSE_PREFIX_3 "Setup fax b3conf fmt=%d, stationid='%s' headline='%s'\n",
		fax_format, stationid, headline);
	b3conf->resolution = 0;
	b3conf->format = (unsigned short)fax_format;
	len1 = strlen(stationid);
	b3conf->Infos[0] = (unsigned char)len1;
	strcpy((char *)&b3conf->Infos[1], stationid);
	len2 = strlen(headline);
	b3conf->Infos[len1 + 1] = (unsigned char)len2;
	strcpy((char *)&b3conf->Infos[len1 + 2], headline);
	b3conf->len = (unsigned char)(2 * sizeof(unsigned short) + len1 + len2 + 2);
	return;
}

/*
 * change b protocol to fax
 */
static void capi_change_bchan_fax(struct ast_channel *c, B3_PROTO_FAXG3 *b3conf) 
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);

	i->isdnstate |= CAPI_ISDN_STATE_B3_SELECT;
	cc_disconnect_b3(i, 1);
	cc_select_b(i, (_cstruct)b3conf);
	return;
}

/*
 * capicommand 'receivefax'
 */
static int pbx_capi_receive_fax(struct ast_channel *c, char *data)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	int res = 0;
	int keepbadfax = 0;
	char *filename, *stationid, *headline, *options;
	B3_PROTO_FAXG3 b3conf;
	char buffer[CAPI_MAX_STRING];

	if (!data) { /* no data implies no filename or anything is present */
		cc_log(LOG_WARNING, "capi receivefax requires a filename\n");
		return -1;
	}

	filename = strsep(&data, "|");
	stationid = strsep(&data, "|");
	headline = strsep(&data, "|");
	options = data;

	if (!stationid)
		stationid = emptyid;
	if (!headline)
		headline = emptyid;
	if (!options)
		options = emptyid;

	cc_verbose(3, 1, VERBOSE_PREFIX_3 "capi receivefax: '%s' '%s' '%s' '%s'\n",
		filename, stationid, headline, options);

	/* parse the options */
	while ((options) && (*options)) {
		switch (*options) {
		case 'k':	/* keepbadfax */
			cc_verbose(3, 1,
				VERBOSE_PREFIX_3 "capi receivefax: if fax is bad, "
				"file won't be deleted.\n");
			keepbadfax = 1;
			break;
		default:
			cc_log(LOG_WARNING, "Unknown option '%c' for receivefax.\n",
				*options);
		}
		options++;
	}

	capi_wait_for_answered(i);

	i->FaxState &= ~CAPI_FAX_STATE_CONN;
	if ((i->fFax = fopen(filename, "wb")) == NULL) {
		cc_log(LOG_WARNING, "can't create fax output file (%s)\n", strerror(errno));
		return -1;
	}

	i->FaxState |= CAPI_FAX_STATE_ACTIVE;
	setup_b3_fax_config(&b3conf, FAX_SFF_FORMAT, stationid, headline);

	i->bproto = CC_BPROTO_FAXG3;

	switch (i->state) {
	case CAPI_STATE_ALERTING:
	case CAPI_STATE_DID:
	case CAPI_STATE_INCALL:
		capi_send_answer(c, (_cstruct)&b3conf);
		break;
	case CAPI_STATE_CONNECTED:
		capi_change_bchan_fax(c, &b3conf);
		break;
	default:
		i->FaxState &= ~CAPI_FAX_STATE_ACTIVE;
		cc_log(LOG_WARNING, "capi receive fax in wrong state (%d)\n",
			i->state);
		return -1;
	}

	while (capi_tell_fax_finish(i)) {
		if (ast_safe_sleep_conditional(c, 1000, capi_tell_fax_finish, i) != 0) {
			/* we got a hangup */
			cc_verbose(3, 1,
				VERBOSE_PREFIX_3 "capi receivefax: hangup.\n");
			break;
		}
	}

	cc_mutex_lock(&i->lock);

	res = (i->FaxState & CAPI_FAX_STATE_ERROR) ? 1 : 0;
	i->FaxState &= ~(CAPI_FAX_STATE_ACTIVE | CAPI_FAX_STATE_ERROR);

	/* if the file has zero length */
	if (ftell(i->fFax) == 0L) {
		res = 1;
	}
			
	cc_verbose(2, 1, VERBOSE_PREFIX_3 "Closing fax file...\n");
	fclose(i->fFax);
	i->fFax = NULL;

	cc_mutex_unlock(&i->lock);

	if (res != 0) {
		cc_verbose(2, 0,
			VERBOSE_PREFIX_1 "capi receivefax: fax receive failed reason=0x%04x reasonB3=0x%04x\n",
				i->reason, i->reasonb3);
		if (!keepbadfax) {
			cc_verbose(3, 1,
				VERBOSE_PREFIX_3 "capi receivefax: removing fax file.\n");
			unlink(filename);
		}
	} else {
		cc_verbose(2, 0,
			VERBOSE_PREFIX_1 "capi receivefax: fax receive successful.\n");
	}
	snprintf(buffer, CAPI_MAX_STRING-1, "%d", res);
	pbx_builtin_setvar_helper(c, "FAXSTATUS", buffer);
	
	return 0;
}

/*
 * capicommand 'sendfax'
 */
static int pbx_capi_send_fax(struct ast_channel *c, char *data)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	int res = 0;
	char *filename, *stationid, *headline;
	B3_PROTO_FAXG3 b3conf;
	char buffer[CAPI_MAX_STRING];

	if (!data) { /* no data implies no filename or anything is present */
		cc_log(LOG_WARNING, "capi sendfax requires a filename\n");
		return -1;
	}

	filename = strsep(&data, "|");
	stationid = strsep(&data, "|");
	headline = data;

	if (!stationid)
		stationid = emptyid;
	if (!headline)
		headline = emptyid;

	cc_verbose(3, 1, VERBOSE_PREFIX_3 "capi sendfax: '%s' '%s' '%s'\n",
		filename, stationid, headline);

	capi_wait_for_answered(i);

	if ((i->fFax = fopen(filename, "rb")) == NULL) {
		cc_log(LOG_WARNING, "can't open fax file (%s)\n", strerror(errno));
		return -1;
	}

	i->FaxState |= (CAPI_FAX_STATE_ACTIVE | CAPI_FAX_STATE_SENDMODE);
	setup_b3_fax_config(&b3conf, FAX_SFF_FORMAT, stationid, headline);

	i->bproto = CC_BPROTO_FAXG3;

	switch (i->state) {
	case CAPI_STATE_ALERTING:
	case CAPI_STATE_DID:
	case CAPI_STATE_INCALL:
		capi_send_answer(c, (_cstruct)&b3conf);
		break;
	case CAPI_STATE_CONNECTED:
		capi_change_bchan_fax(c, &b3conf);
		break;
	default:
		i->FaxState &= ~CAPI_FAX_STATE_ACTIVE;
		cc_log(LOG_WARNING, "capi send fax in wrong state (%d)\n",
			i->state);
		return -1;
	}
	while (capi_tell_fax_finish(i)) {
		if (ast_safe_sleep_conditional(c, 1000, capi_tell_fax_finish, i) != 0) {
			/* we got a hangup */
			cc_verbose(3, 1,
				VERBOSE_PREFIX_3 "capi sendfax: hangup.\n");
			break;
		}
	}

	cc_mutex_lock(&i->lock);

	res = (i->FaxState & CAPI_FAX_STATE_ERROR) ? 1 : 0;
	i->FaxState &= ~(CAPI_FAX_STATE_ACTIVE | CAPI_FAX_STATE_ERROR);

	cc_verbose(2, 1, VERBOSE_PREFIX_3 "Closing fax file...\n");
	fclose(i->fFax);
	i->fFax = NULL;

	cc_mutex_unlock(&i->lock);

	if (res != 0) {
		cc_verbose(2, 0,
			VERBOSE_PREFIX_1 "capi sendfax: fax send failed reason=0x%04x reasonB3=0x%04x\n",
				i->reason, i->reasonb3);
	} else {
		cc_verbose(2, 0,
			VERBOSE_PREFIX_1 "capi sendfax: fax sent successful.\n");
	}
	snprintf(buffer, CAPI_MAX_STRING-1, "%d", res);
	pbx_builtin_setvar_helper(c, "FAXSTATUS", buffer);
	
	return 0;
}

/*
 * Fax guard tone -- Handle and return NULL
 */
static void capi_handle_dtmf_fax(struct capi_pvt *i)
{
	struct ast_channel *c = i->owner;

	if (!c) {
		/* no channel, ignore */
		return;
	}
	
	if (i->FaxState & CAPI_FAX_STATE_HANDLED) {
		cc_log(LOG_DEBUG, "Fax already handled\n");
		return;
	}
	i->FaxState |= CAPI_FAX_STATE_HANDLED;

	if (((i->outgoing == 1) && (!(i->FaxState & CAPI_FAX_DETECT_OUTGOING))) ||
	    ((i->outgoing == 0) && (!(i->FaxState & CAPI_FAX_DETECT_INCOMING)))) {
		cc_verbose(3, 0, VERBOSE_PREFIX_3 "%s: Fax detected, but not configured for redirection\n",
			i->vname);
		return;
	}
	
	if (!strcmp(c->exten, "fax")) {
		cc_log(LOG_DEBUG, "Already in a fax extension, not redirecting\n");
		return;
	}

	if (!ast_exists_extension(c, c->context, "fax", 1, i->cid)) {
		cc_verbose(3, 0, VERBOSE_PREFIX_3 "Fax tone detected, but no fax extension for %s\n", c->name);
		return;
	}

	cc_verbose(2, 0, VERBOSE_PREFIX_3 "%s: Redirecting %s to fax extension\n",
		i->vname, c->name);
			
	capi_channel_task(c, CAPI_CHANNEL_TASK_GOTOFAX);

	return;
}

/*
 * see if did matches
 */
static int search_did(struct ast_channel *c)
{
	/*
	 * Returns 
	 * -1 = Failure 
	 *  0 = Match
	 *  1 = possible match 
	 */
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	char *exten;
    
	if (!strlen(i->dnid) && (i->immediate)) {
		exten = "s";
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: %s: %s matches in context %s for immediate\n",
			i->vname, c->name, exten, c->context);
	} else {
		if (strlen(i->dnid) < strlen(i->incomingmsn))
			return 0;
		exten = i->dnid;
	}

	if (ast_exists_extension(NULL, c->context, exten, 1, i->cid)) {
		c->priority = 1;
		cc_copy_string(c->exten, exten, sizeof(c->exten));
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: %s: %s matches in context %s\n",
			i->vname, c->name, exten, c->context);
		return 0;
	}

	if (ast_canmatch_extension(NULL, c->context, exten, 1, i->cid)) {
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: %s: %s would possibly match in context %s\n",
			i->vname, c->name, exten, c->context);
		return 1;
	}

	return -1;
}

/*
 * Progress Indicator
 */
static void handle_progress_indicator(_cmsg *CMSG, unsigned int PLCI, struct capi_pvt *i)
{
	if (INFO_IND_INFOELEMENT(CMSG)[0] < 2) {
		cc_verbose(3, 1, VERBOSE_PREFIX_4 "%s: Progress description missing\n",
			i->vname);
		return;
	}

	switch(INFO_IND_INFOELEMENT(CMSG)[2] & 0x7f) {
	case 0x01:
		cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: Not end-to-end ISDN\n",
			i->vname);
		break;
	case 0x02:
		cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: Destination is non ISDN\n",
			i->vname);
		i->calledPartyIsISDN = 0;
		break;
	case 0x03:
		cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: Origination is non ISDN\n",
			i->vname);
		break;
	case 0x04:
		cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: Call returned to ISDN\n",
			i->vname);
		break;
	case 0x05:
		cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: Interworking occured\n",
			i->vname);
		break;
	case 0x08:
		cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: In-band information available\n",
			i->vname);
		break;
	default:
		cc_verbose(3, 1, VERBOSE_PREFIX_4 "%s: Unknown progress description %02x\n",
			i->vname, INFO_IND_INFOELEMENT(CMSG)[2]);
	}
	send_progress(i);
	return;
}

/*
 * if the dnid matches, start the pbx
 */
static void start_pbx_on_match(struct capi_pvt *i, unsigned int PLCI, _cword MessageNumber)
{
	struct ast_channel *c;

	c = i->owner;

	if ((i->isdnstate & CAPI_ISDN_STATE_PBX_DONT)) {
		/* we already found non-match here */
		return;
	}
	if ((i->isdnstate & CAPI_ISDN_STATE_PBX)) {
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: pbx already started on channel %s\n",
			i->vname, c->name);
		return;
	}

	/* check for internal pickup extension first */
	if (!strcmp(i->dnid, ast_pickup_ext())) {
		i->isdnstate |= CAPI_ISDN_STATE_PBX;
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Pickup extension '%s' found.\n",
			i->vname, i->dnid);
		cc_copy_string(c->exten, i->dnid, sizeof(c->exten));
		pbx_capi_alert(c);
		capi_channel_task(c, CAPI_CHANNEL_TASK_PICKUP);
		return;
	}

	switch(search_did(c)) {
	case 0: /* match */
		i->isdnstate |= CAPI_ISDN_STATE_PBX;
		ast_setstate(c, AST_STATE_RING);
		if (ast_pbx_start(c)) {
			cc_log(LOG_ERROR, "%s: Unable to start pbx on channel!\n",
				i->vname);
			capi_channel_task(c, CAPI_CHANNEL_TASK_HANGUP); 
		} else {
			cc_verbose(2, 1, VERBOSE_PREFIX_2 "Started pbx on channel %s\n",
				c->name);
		}
		break;
	case 1:
		/* would possibly match */
		if (i->isdnmode == CAPI_ISDNMODE_DID)
			break;
		/* fall through for MSN mode, because there won't be a longer msn */
	case -1:
	default:
		/* doesn't match */
		i->isdnstate |= CAPI_ISDN_STATE_PBX_DONT; /* don't try again */
		cc_log(LOG_NOTICE, "%s: did not find exten for '%s', ignoring call.\n",
			i->vname, i->dnid);
		capi_sendf(NULL, 0, CAPI_CONNECT_RESP, PLCI, MessageNumber,
			"w()()()()()", 1 /* ignore */);
	}
	return;
}

/*
 * Called Party Number via INFO_IND
 */
static void capidev_handle_did_digits(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI,
	struct capi_pvt *i, unsigned int skip)
{
	char *did;
	struct ast_frame fr = { AST_FRAME_NULL, };
	int a;

	if (!i->owner) {
		cc_log(LOG_ERROR, "No channel for interface!\n");
		return;
	}

	if (i->state != CAPI_STATE_DID) {
		cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: INFO_IND DID digits not used in this state.\n",
			i->vname);
		return;
	}

	did = capi_number(INFO_IND_INFOELEMENT(CMSG), skip);

	if ((!(i->isdnstate & CAPI_ISDN_STATE_DID)) && 
	    (strlen(i->dnid) && !strcasecmp(i->dnid, did))) {
		did = NULL;
	}

	if ((did) && (strlen(i->dnid) < (sizeof(i->dnid) - 1))) {
		if ((!strlen(i->dnid)) && (INFO_IND_INFONUMBER(CMSG) = 0x002c)) {
			/* start of keypad */
			strcat(i->dnid, "K");
		}
		strcat(i->dnid, did);
	}

	i->isdnstate |= CAPI_ISDN_STATE_DID;
	
	update_channel_name(i);	
	
	if (i->owner->pbx != NULL) {
		/* we are already in pbx, so we send the digits as dtmf */
		for (a = 0; a < strlen(did); a++) {
			fr.frametype = AST_FRAME_DTMF;
			fr.subclass = did[a];
			local_queue_frame(i, &fr);
		} 
		return;
	}

	start_pbx_on_match(i, PLCI, HEADER_MSGNUM(CMSG));
	return;
}

/*
 * send control according to cause code
 */
void capi_queue_cause_control(struct capi_pvt *i, int control)
{
	struct ast_frame fr = { AST_FRAME_CONTROL, AST_CONTROL_HANGUP, };
	
	if ((i->owner) && (control)) {
		int cause = i->owner->hangupcause;
		if (cause == AST_CAUSE_NORMAL_CIRCUIT_CONGESTION) {
			fr.subclass = AST_CONTROL_CONGESTION;
		} else if ((cause != AST_CAUSE_NO_USER_RESPONSE) &&
		           (cause != AST_CAUSE_NO_ANSWER)) {
			/* not NOANSWER */
			fr.subclass = AST_CONTROL_BUSY;
		}
	}
	local_queue_frame(i, &fr);
	return;
}

/*
 * Disconnect via INFO_IND
 */
static void capidev_handle_info_disconnect(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	i->isdnstate |= CAPI_ISDN_STATE_DISCONNECT;

	if ((PLCI == i->onholdPLCI) || (i->isdnstate & CAPI_ISDN_STATE_ECT)) {
		cc_verbose(4, 1, VERBOSE_PREFIX_3 "%s: Disconnect onhold/ECT call\n",
			i->vname);
		/* the caller onhold hung up (or ECTed away) */
		/* send a disconnect_req , we cannot hangup the channel here!!! */
		capi_send_disconnect(PLCI, NULL);
		return;
	}

	/* case 1: B3 on success or no B3 at all */
	if ((i->doB3 != CAPI_B3_ALWAYS) && (i->outgoing == 1)) {
		cc_verbose(4, 1, VERBOSE_PREFIX_3 "%s: Disconnect case 1\n",
			i->vname);
		if (i->state == CAPI_STATE_CONNECTED) {
			capi_queue_cause_control(i, 0);
		} else {
			if ((i->isdnstate & CAPI_ISDN_STATE_STAYONLINE)) {
				cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: stay-online hangup frame queued.\n",
					i->vname);
				i->whentoqueuehangup = time(NULL) + 1;
			} else {
				capi_queue_cause_control(i, 1);
			}
		}
		return;
	}
	
	/* case 2: we are doing B3, and receive the 0x8045 after a successful call */
	if ((i->doB3 != CAPI_B3_DONT) &&
	    (i->state == CAPI_STATE_CONNECTED) && (i->outgoing == 1)) {
		cc_verbose(4, 1, VERBOSE_PREFIX_3 "%s: Disconnect case 2\n",
			i->vname);
		capi_queue_cause_control(i, 1);
		return;
	}

	/*
	 * case 3: this channel is an incoming channel! the user hung up!
	 * it is much better to hangup now instead of waiting for a timeout and
	 * network caused DISCONNECT_IND!
	 */
	if (i->outgoing == 0) {
		cc_verbose(4, 1, VERBOSE_PREFIX_3 "%s: Disconnect case 3\n",
			i->vname);
		if (i->FaxState & CAPI_FAX_STATE_ACTIVE) {
			/* in fax mode, we just hangup */
			capi_send_disconnect(i->PLCI, NULL);
			return;
		}
		capi_queue_cause_control(i, 0);
		return;
	}
	
	/* case 4 (a.k.a. the italian case): B3 always. call is unsuccessful */
	if ((i->doB3 == CAPI_B3_ALWAYS) && (i->outgoing == 1)) {
		cc_verbose(4, 1, VERBOSE_PREFIX_3 "%s: Disconnect case 4\n",
			i->vname);
		if ((i->state == CAPI_STATE_CONNECTED) &&
		    (i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
			capi_queue_cause_control(i, 1);
			return;
		}
		/* wait for the 0x001e (PROGRESS), play audio and wait for a timeout from the network */
		return;
	}
	cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: Other case DISCONNECT INFO_IND\n",
		i->vname);
	return;
}

/*
 * incoming call SETUP
 */
static void capidev_handle_setup_element(_cmsg *CMSG, unsigned int PLCI, struct capi_pvt *i)
{
	if ((i->isdnstate & CAPI_ISDN_STATE_SETUP)) {
		cc_verbose(3, 1, VERBOSE_PREFIX_4 "%s: IE SETUP / SENDING-COMPLETE already received.\n",
			i->vname);
		return;
	}

	i->isdnstate |= CAPI_ISDN_STATE_SETUP;

	if (!i->owner) {
		cc_log(LOG_ERROR, "No channel for interface!\n");
		return;
	}

	if (i->isdnmode == CAPI_ISDNMODE_DID) {
		if (strlen(i->dnid) || (i->immediate)) {
			start_pbx_on_match(i, PLCI, HEADER_MSGNUM(CMSG));
		}
	} else {
		start_pbx_on_match(i, PLCI, HEADER_MSGNUM(CMSG));
	}
	return;
}

/*
 * Send info elements back to calling channel if in NT-mode
 * (this works with peerlink only)
 */
static void capidev_sendback_info(struct capi_pvt *i, _cmsg *CMSG)
{
	struct capi_pvt *i2;
	unsigned char fac[CAPI_MAX_FACILITYDATAARRAY_SIZE];
	unsigned char length;

	if (!(i->peer))
		return;

	if (i->peer->tech != &capi_tech)
		return;

	i2 = CC_CHANNEL_PVT(i->peer);

	if (!(i2->ntmode))
		return;

	length = INFO_IND_INFOELEMENT(CMSG)[0];

	fac[0] = length + 2;
	fac[1] = (unsigned char) INFO_IND_INFONUMBER(CMSG) & 0xff;
	memcpy(&fac[2], &INFO_IND_INFOELEMENT(CMSG)[0], length + 1);

	capi_sendf(NULL, 0, CAPI_INFO_REQ, i2->PLCI, get_capi_MessageNumber(),
		"()(()()()s())",
		fac
	);
}

/*
 * CAPI INFO_IND
 */
static void capidev_handle_info_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	struct ast_frame fr = { AST_FRAME_NULL, };
	char *p = NULL;
	char *p2 = NULL;
	int val = 0;

	capi_sendf(NULL, 0, CAPI_INFO_RESP, PLCI, HEADER_MSGNUM(CMSG), "");

	return_on_no_interface("INFO_IND");

	switch(INFO_IND_INFONUMBER(CMSG)) {
	case 0x0008:	/* Cause */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CAUSE %02x %02x\n",
			i->vname, INFO_IND_INFOELEMENT(CMSG)[1], INFO_IND_INFOELEMENT(CMSG)[2]);
		if (i->owner) {
			i->owner->hangupcause = INFO_IND_INFOELEMENT(CMSG)[2] & 0x7f;
		}
		capidev_sendback_info(i, CMSG);
		break;
	case 0x0014:	/* Call State */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CALL STATE %02x\n",
			i->vname, INFO_IND_INFOELEMENT(CMSG)[1]);
		capidev_sendback_info(i, CMSG);
		break;
	case 0x0018:	/* Channel Identification */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CHANNEL IDENTIFICATION %02x\n",
			i->vname, INFO_IND_INFOELEMENT(CMSG)[1]);
		if (i->doB3 == CAPI_B3_ON_SUCCESS) { 
			/* try early B3 Connect */
			cc_start_b3(i);
		}
		break;
	case 0x001c:	/*  Facility Q.932 */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element FACILITY\n",
			i->vname);
		break;
	case 0x001e:	/* Progress Indicator */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element PI %02x %02x\n",
			i->vname, INFO_IND_INFOELEMENT(CMSG)[1], INFO_IND_INFOELEMENT(CMSG)[2]);
		handle_progress_indicator(CMSG, PLCI, i);
		capidev_sendback_info(i, CMSG);
		break;
	case 0x0027: {	/*  Notification Indicator */
		char *desc = "?";
		if (INFO_IND_INFOELEMENT(CMSG)[0] > 0) {
			switch (INFO_IND_INFOELEMENT(CMSG)[1]) {
			case 0:
				desc = "User suspended";
				break;
			case 1:
				desc = "User resumed";
				break;
			case 2:
				desc = "Bearer service changed";
				break;
			case 0xf9:
				desc = "User put on hold";
				break;
			case 0xfa:
				desc = "User retrieved from hold";
				break;
			}
		}
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element NOTIFICATION INDICATOR '%s' (0x%02x)\n",
			i->vname, desc, INFO_IND_INFOELEMENT(CMSG)[1]);
		capidev_sendback_info(i, CMSG);
		break;
	}
	case 0x0028:	/* DSP */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element DSP\n",
			i->vname);
		capidev_sendback_info(i, CMSG);
		break;
	case 0x0029:	/* Date/Time */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element Date/Time %02d/%02d/%02d %02d:%02d\n",
			i->vname,
			INFO_IND_INFOELEMENT(CMSG)[1], INFO_IND_INFOELEMENT(CMSG)[2],
			INFO_IND_INFOELEMENT(CMSG)[3], INFO_IND_INFOELEMENT(CMSG)[4],
			INFO_IND_INFOELEMENT(CMSG)[5]);
		break;
	case 0x002c:	/* Keypad facility */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element KEYPAD FACILITY\n",
			i->vname);
		/* we handle keypad digits as normal digits */
		capidev_handle_did_digits(CMSG, PLCI, NCCI, i, 0);
		break;
	case 0x0070:	/* Called Party Number */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CALLED PARTY NUMBER\n",
			i->vname);
		capidev_handle_did_digits(CMSG, PLCI, NCCI, i, 1);
		break;
	case 0x0074:	/* Redirecting Number */
		p = capi_number(INFO_IND_INFOELEMENT(CMSG), 3);
		if (INFO_IND_INFOELEMENT(CMSG)[0] > 2) {
			val = INFO_IND_INFOELEMENT(CMSG)[3] & 0x0f;
		}
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element REDIRECTING NUMBER '%s' Reason=0x%02x\n",
			i->vname, p, val);
		if (i->owner) {
			char reasonbuf[16];
			snprintf(reasonbuf, sizeof(reasonbuf) - 1, "%d", val); 
			pbx_builtin_setvar_helper(i->owner, "REDIRECTINGNUMBER", p);
			pbx_builtin_setvar_helper(i->owner, "REDIRECTREASON", reasonbuf);
			if (i->owner->cid.cid_rdnis) {
				free(i->owner->cid.cid_rdnis);
			}
			i->owner->cid.cid_rdnis = strdup(p);
		}
		capidev_sendback_info(i, CMSG);
		break;
	case 0x0076:	/* Redirection Number */
		p = capi_number(INFO_IND_INFOELEMENT(CMSG), 2);
		p2 = emptyid;
		if (INFO_IND_INFOELEMENT(CMSG)[0] > 1) {
			val = INFO_IND_INFOELEMENT(CMSG)[1] & 0x70;
			if (val == CAPI_ETSI_NPLAN_NATIONAL) {
				p2 = capi_national_prefix;
			} else if (val == CAPI_ETSI_NPLAN_INTERNAT) {
				p2 = capi_international_prefix;
			}
		}
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element REDIRECTION NUMBER '(%s)%s'\n",
			i->vname, p2, p);
		if (i->owner) {
			char numberbuf[64];
			snprintf(numberbuf, sizeof(numberbuf) - 1, "%s%s", p2, p);
			pbx_builtin_setvar_helper(i->owner, "REDIRECTIONNUMBER", numberbuf);
		}
		capidev_sendback_info(i, CMSG);
		break;
	case 0x00a1:	/* Sending Complete */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element Sending Complete\n",
			i->vname);
		capidev_handle_setup_element(CMSG, PLCI, i);
		break;
	case 0x4000:	/* CHARGE in UNITS */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CHARGE in UNITS\n",
			i->vname);
		break;
	case 0x4001:	/* CHARGE in CURRENCY */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CHARGE in CURRENCY\n",
			i->vname);
		break;
	case 0x8001:	/* ALERTING */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element ALERTING\n",
			i->vname);
		send_progress(i);
		fr.frametype = AST_FRAME_CONTROL;
		fr.subclass = AST_CONTROL_RINGING;
		local_queue_frame(i, &fr);
		if (i->owner)
			ast_setstate(i->owner, AST_STATE_RINGING);
		
		break;
	case 0x8002:	/* CALL PROCEEDING */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CALL PROCEEDING\n",
			i->vname);
		fr.frametype = AST_FRAME_CONTROL;
		fr.subclass = AST_CONTROL_PROCEEDING;
		local_queue_frame(i, &fr);
		break;
	case 0x8003:	/* PROGRESS */
		i->isdnstate |= CAPI_ISDN_STATE_ISDNPROGRESS;
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element PROGRESS\n",
			i->vname);
		/*
		 * rain - some networks will indicate a USER BUSY cause, send
		 * PROGRESS message, and then send audio for a busy signal for
		 * a moment before dropping the line.  This delays sending the
		 * busy to the end user, so we explicitly check for it here.
		 *
		 * FIXME: should have better CAUSE handling so that we can
		 * distinguish things like status responses and invalid IE
		 * content messages (from bad SetCallerID) from errors actually
		 * related to the call setup; then, we could always abort if we
		 * get a PROGRESS with a hangupcause set (safer?)
		 */
		if (i->doB3 == CAPI_B3_DONT) {
			if ((i->owner) &&
			    (i->owner->hangupcause == AST_CAUSE_USER_BUSY)) {
				capi_queue_cause_control(i, 1);
				break;
			}
		}
		send_progress(i);
		break;
	case 0x8005:	/* SETUP */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element SETUP\n",
			i->vname);
		capidev_handle_setup_element(CMSG, PLCI, i);
		break;
	case 0x8007:	/* CONNECT */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CONNECT\n",
			i->vname);
		break;
	case 0x800d:	/* SETUP ACK */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element SETUP ACK\n",
			i->vname);
		i->isdnstate |= CAPI_ISDN_STATE_SETUP_ACK;
		/* if some digits of initial CONNECT_REQ are left to dial */
		if (strlen(i->overlapdigits)) {
			capi_send_info_digits(i, i->overlapdigits,
				strlen(i->overlapdigits));
			i->overlapdigits[0] = 0;
			i->doOverlap = 0;
		}
		break;
	case 0x800f:	/* CONNECT ACK */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CONNECT ACK\n",
			i->vname);
		break;
	case 0x8045:	/* DISCONNECT */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element DISCONNECT\n",
			i->vname);
		capidev_handle_info_disconnect(CMSG, PLCI, NCCI, i);
		break;
	case 0x804d:	/* RELEASE */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element RELEASE\n",
			i->vname);
		break;
	case 0x805a:	/* RELEASE COMPLETE */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element RELEASE COMPLETE\n",
			i->vname);
		break;
	case 0x8062:	/* FACILITY */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element FACILITY\n",
			i->vname);
		break;
	case 0x806e:	/* NOTIFY */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element NOTIFY\n",
			i->vname);
		break;
	case 0x807b:	/* INFORMATION */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element INFORMATION\n",
			i->vname);
		break;
	case 0x807d:	/* STATUS */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element STATUS\n",
			i->vname);
		break;
	default:
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: unhandled INFO_IND %#x (PLCI=%#x)\n",
			i->vname, INFO_IND_INFONUMBER(CMSG), PLCI);
		break;
	}
	
	/* QSIG worker - is only executed, if QSIG is enabled */
	pbx_capi_qsig_handle_info_indication(CMSG, PLCI, NCCI, i);
	
	return;
}

/*
 * CAPI FACILITY_IND line interconnect
 */
static int handle_facility_indication_line_interconnect(
	_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	if ((FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[1] == 0x01) &&
	    (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[2] == 0x00)) {
		cc_verbose(3, 0, VERBOSE_PREFIX_3 "%s: Line Interconnect activated\n",
			i->vname);
	}
	if ((FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[1] == 0x02) &&
	    (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[2] == 0x00) &&
	    (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[0] > 8)) {
		show_capi_info(i, read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[8]));
	}
	return 0;
}

/*
 * CAPI FACILITY_IND dtmf received 
 */
static int handle_facility_indication_dtmf(
	_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	struct ast_frame fr = { AST_FRAME_NULL, };
	char dtmf;
	unsigned dtmflen = 0;
	unsigned dtmfpos = 0;

	if (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[0] != (0xff)) {
		dtmflen = FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[0];
		FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG) += 1;
	} else {
		dtmflen = read_capi_word(FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG) + 1);
		FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG) += 3;
	}
	while (dtmflen) {
		dtmf = (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG))[dtmfpos];
		cc_verbose(1, 1, VERBOSE_PREFIX_4 "%s: c_dtmf = %c\n",
			i->vname, dtmf);
		if ((!(i->ntmode)) || (i->state == CAPI_STATE_CONNECTED)) {
			if ((dtmf == 'X') || (dtmf == 'Y')) {
				capi_handle_dtmf_fax(i);
			} else {
				fr.frametype = AST_FRAME_DTMF;
				fr.subclass = dtmf;
				local_queue_frame(i, &fr);
			}
		}
		dtmflen--;
		dtmfpos++;
	}
	return 0;
}

/*
 * CAPI FACILITY_IND
 */
static void capidev_handle_facility_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	int resp_done = 0;

	switch (FACILITY_IND_FACILITYSELECTOR(CMSG)) {
	case FACILITYSELECTOR_LINE_INTERCONNECT:
		return_on_no_interface("FACILITY_IND LI");
		resp_done = handle_facility_indication_line_interconnect(CMSG, PLCI, NCCI, i);
		break;
	case FACILITYSELECTOR_DTMF:
		return_on_no_interface("FACILITY_IND DTMF");
		resp_done = handle_facility_indication_dtmf(CMSG, PLCI, NCCI, i);
		break;
	case FACILITYSELECTOR_SUPPLEMENTARY:
		resp_done = handle_facility_indication_supplementary(CMSG, PLCI, NCCI, i);
		break;
	default:
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: unhandled FACILITY_IND selector %d\n",
			(i) ? i->vname:"???", FACILITY_IND_FACILITYSELECTOR(CMSG));
	}

	if (!resp_done) {
		capi_sendf(NULL, 0, CAPI_FACILITY_RESP, PLCI, HEADER_MSGNUM(CMSG),
			"w()",
			FACILITY_IND_FACILITYSELECTOR(CMSG)
		);
	}
}

/*
 * CAPI DATA_B3_IND
 */
static void capidev_handle_data_b3_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	struct ast_frame fr = { AST_FRAME_NULL, };
	unsigned char *b3buf = NULL;
	int b3len = 0;
	int j;
	int rxavg = 0;
	int txavg = 0;
	int rtpoffset = 0;

	if (i != NULL) {
		if ((i->isdnstate & CAPI_ISDN_STATE_RTP)) rtpoffset = RTP_HEADER_SIZE;
		b3len = DATA_B3_IND_DATALENGTH(CMSG);
		b3buf = &(i->rec_buffer[AST_FRIENDLY_OFFSET - rtpoffset]);
		memcpy(b3buf, (char *)DATA_B3_IND_DATA(CMSG), b3len);
	}
	
	/* send a DATA_B3_RESP very quickly to free the buffer in capi */
	capi_sendf(NULL, 0, CAPI_DATA_B3_RESP, NCCI, HEADER_MSGNUM(CMSG),
		"w", DATA_B3_IND_DATAHANDLE(CMSG));

	return_on_no_interface("DATA_B3_IND");

	if (i->fFax) {
		/* we are in fax mode and have a file open */
		cc_verbose(6, 1, VERBOSE_PREFIX_3 "%s: DATA_B3_IND (len=%d) Fax\n",
			i->vname, b3len);
		if ((!(i->FaxState & CAPI_FAX_STATE_SENDMODE)) &&
			(i->FaxState & CAPI_FAX_STATE_CONN)) {
			if (fwrite(b3buf, 1, b3len, i->fFax) != b3len)
				cc_log(LOG_WARNING, "%s : error writing output file (%s)\n",
					i->vname, strerror(errno));
		}
#ifndef CC_AST_HAS_VERSION_1_4
		fr.frametype = AST_FRAME_CONTROL;
		fr.subclass = AST_CONTROL_PROGRESS;
		local_queue_frame(i, &fr);
#endif
		return;
	}

	if (((i->isdnstate &
	    (CAPI_ISDN_STATE_B3_CHANGE | CAPI_ISDN_STATE_LI | CAPI_ISDN_STATE_HANGUP))) ||
	    (i->state == CAPI_STATE_DISCONNECTING)) {
		/* drop voice frames when we don't want them */
		return;
	}

	if ((i->isdnstate & CAPI_ISDN_STATE_RTP)) {
		struct ast_frame *f = capi_read_rtp(i, b3buf, b3len);
		if (f)
			local_queue_frame(i, f);
		return;
	}

	if (i->B3q < (((CAPI_MAX_B3_BLOCKS - 1) * CAPI_MAX_B3_BLOCK_SIZE) + 1)) {
		i->B3q += b3len;
	}

	if ((i->doES == 1) && (!capi_tcap_is_digital(i->transfercapability))) {
		for (j = 0; j < b3len; j++) {
			*(b3buf + j) = capi_reversebits[*(b3buf + j)]; 
			if (capi_capability == AST_FORMAT_ULAW) {
				rxavg += abs(capiULAW2INT[ capi_reversebits[*(b3buf + j)]]);
			} else {
				rxavg += abs(capiALAW2INT[ capi_reversebits[*(b3buf + j)]]);
			}
		}
		rxavg = rxavg / j;
		for (j = 0; j < ECHO_EFFECTIVE_TX_COUNT; j++) {
			txavg += i->txavg[j];
		}
		txavg = txavg / j;
			    
		if ( (txavg / ECHO_TXRX_RATIO) > rxavg) {
			if (capi_capability == AST_FORMAT_ULAW) {
				memset(b3buf, 255, b3len);
			} else {
				memset(b3buf, 85, b3len);
			}
			cc_verbose(6, 1, VERBOSE_PREFIX_3 "%s: SUPPRESSING ECHO rx=%d, tx=%d\n",
					i->vname, rxavg, txavg);
		}
	} else {
		if ((i->rxgain == 1.0) || (capi_tcap_is_digital(i->transfercapability))) {
			for (j = 0; j < b3len; j++) {
				*(b3buf + j) = capi_reversebits[*(b3buf + j)];
			}
		} else {
			for (j = 0; j < b3len; j++) {
				*(b3buf + j) = capi_reversebits[i->g.rxgains[*(b3buf + j)]];
			}
		}
	}

	fr.frametype = AST_FRAME_VOICE;
	fr.subclass = capi_capability;
	fr.data = b3buf;
	fr.datalen = b3len;
	fr.samples = b3len;
	fr.offset = AST_FRIENDLY_OFFSET;
	fr.mallocd = 0;
	fr.delivery = ast_tv(0,0);
	fr.src = NULL;
	cc_verbose(8, 1, VERBOSE_PREFIX_3 "%s: DATA_B3_IND (len=%d) fr.datalen=%d fr.subclass=%d\n",
		i->vname, b3len, fr.datalen, fr.subclass);
	local_queue_frame(i, &fr);
	return;
}

/*
 * signal 'answer' to PBX
 */
static void capi_signal_answer(struct capi_pvt *i)
{
	struct ast_frame fr = { AST_FRAME_CONTROL, AST_CONTROL_ANSWER, };

	if (i->outgoing == 1) {
		local_queue_frame(i, &fr);
	}
}

/*
 * send the next data
 */
static void capidev_send_faxdata(struct capi_pvt *i)
{
#ifndef CC_AST_HAS_VERSION_1_4
	struct ast_frame fr = { AST_FRAME_CONTROL, AST_CONTROL_PROGRESS, };
#endif
	unsigned char faxdata[CAPI_MAX_B3_BLOCK_SIZE];
	size_t len;

	if (i->NCCI == 0) {
		cc_verbose(3, 0, VERBOSE_PREFIX_3 "%s: send_faxdata on NCCI = 0.\n",
			i->vname);
		return;
	}

	if (i->state == CAPI_STATE_DISCONNECTING) {
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: send_faxdata in DISCONNECTING.\n",
			i->vname);
		return;
	}

	if ((i->fFax) && (!(feof(i->fFax)))) {
		len = fread(faxdata, 1, CAPI_MAX_B3_BLOCK_SIZE, i->fFax);
		if (len > 0) {
			i->send_buffer_handle++;
			capi_sendf(NULL, 0, CAPI_DATA_B3_REQ, i->NCCI, get_capi_MessageNumber(),
				"dwww", faxdata, len, i->send_buffer_handle, 0);
			cc_verbose(5, 1, VERBOSE_PREFIX_3 "%s: send %d fax bytes.\n",
				i->vname, len);
#ifndef CC_AST_HAS_VERSION_1_4
			local_queue_frame(i, &fr);
#endif
			return;
		}
	}
	/* finished send fax, so we hangup */
	cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: completed faxsend.\n",
		i->vname);
	capi_sendf(NULL, 0, CAPI_DISCONNECT_B3_REQ, i->NCCI, get_capi_MessageNumber(),
		"()");
}

/*
 * CAPI MANUFACTURER_IND
 */
static void capidev_handle_manufacturer_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	capi_sendf(NULL, 0, CAPI_MANUFACTURER_RESP, MANUFACTURER_IND_CONTROLLER(CMSG), HEADER_MSGNUM(CMSG),
		"d", MANUFACTURER_IND_MANUID(CMSG));
	
	return_on_no_interface("MANUFACTURER_IND");

	cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: Ignored MANUFACTURER_IND Id=0x%x \n",
		i->vname, MANUFACTURER_IND_MANUID(CMSG));

	return;
}

/*
 * CAPI CONNECT_ACTIVE_IND
 */
static void capidev_handle_connect_active_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	capi_sendf(NULL, 0, CAPI_CONNECT_ACTIVE_RESP, PLCI, HEADER_MSGNUM(CMSG), "");
	
	return_on_no_interface("CONNECT_ACTIVE_IND");

	if (i->state == CAPI_STATE_DISCONNECTING) {
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: CONNECT_ACTIVE in DISCONNECTING.\n",
			i->vname);
		return;
	}

	i->state = CAPI_STATE_CONNECTED;

	if ((i->FaxState & CAPI_FAX_STATE_SENDMODE)) {
		cc_start_b3(i);
		return;
	}

	if ((i->owner) && (i->FaxState & CAPI_FAX_STATE_ACTIVE)) {
		ast_setstate(i->owner, AST_STATE_UP);
		if (i->owner->cdr)
			ast_cdr_answer(i->owner->cdr);
		return;
	}
	
	/* normal processing */
			    
	if (!(i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
		/* send a CONNECT_B3_REQ */
		if (i->outgoing == 1) {
			/* outgoing call */
			if (i->channeltype == CAPI_CHANNELTYPE_NULL) {
				/* NULL-PLCI needs a virtual connection */
				capi_sendf(NULL, 0, CAPI_FACILITY_REQ, PLCI, get_capi_MessageNumber(),
					"w(w(d()))",
					FACILITYSELECTOR_LINE_INTERCONNECT,
					0x0001, /* CONNECT */
					0x0000000c /* mask */
				);
			}
			cc_start_b3(i);
		} else {
			/* incoming call */
			/* RESP already sent ... wait for CONNECT_B3_IND */
		}
	} else {
		capi_signal_answer(i);
	}
	return;
}

/*
 * CAPI CONNECT_B3_ACTIVE_IND
 */
static void capidev_handle_connect_b3_active_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	capi_sendf(NULL, 0, CAPI_CONNECT_B3_ACTIVE_RESP, NCCI, HEADER_MSGNUM(CMSG), "");

	return_on_no_interface("CONNECT_ACTIVE_B3_IND");

	if (i->channeltype != CAPI_CHANNELTYPE_NULL) {
		capi_controllers[i->controller]->nfreebchannels--;
	}

	if (i->state == CAPI_STATE_DISCONNECTING) {
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: CONNECT_B3_ACTIVE_IND during disconnect for NCCI %#x\n",
			i->vname, NCCI);
		return;
	}

	i->isdnstate |= CAPI_ISDN_STATE_B3_UP;
	i->isdnstate &= ~CAPI_ISDN_STATE_B3_PEND;

	if (i->bproto == CC_BPROTO_RTP) {
		i->isdnstate |= CAPI_ISDN_STATE_RTP;
	} else {
		i->isdnstate &= ~CAPI_ISDN_STATE_RTP;
	}

	i->B3q = (CAPI_MAX_B3_BLOCK_SIZE * 3);

	if ((i->FaxState & CAPI_FAX_STATE_SENDMODE)) {
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: Start sending fax.\n",
			i->vname);
		capidev_send_faxdata(i);
	}

	if ((i->isdnstate & CAPI_ISDN_STATE_B3_CHANGE)) {
		i->isdnstate &= ~CAPI_ISDN_STATE_B3_CHANGE;
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: B3 protocol changed.\n",
			i->vname);
		return;
	}

	if (i->FaxState & CAPI_FAX_STATE_ACTIVE) {
		i->FaxState |= CAPI_FAX_STATE_CONN;
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: Fax connection, no EC/DTMF\n",
			i->vname);
	} else {
		capi_echo_canceller(i, EC_FUNCTION_ENABLE);
		capi_detect_dtmf(i, 1);
	}

	if (i->state == CAPI_STATE_CONNECTED) {
		capi_signal_answer(i);
	}
	return;
}

/*
 * CAPI DISCONNECT_B3_IND
 */
static void capidev_handle_disconnect_b3_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	capi_sendf(NULL, 0, CAPI_DISCONNECT_B3_RESP, NCCI, HEADER_MSGNUM(CMSG), "");

	return_on_no_interface("DISCONNECT_B3_IND");

	i->isdnstate &= ~(CAPI_ISDN_STATE_B3_UP | CAPI_ISDN_STATE_B3_PEND);

	i->reasonb3 = DISCONNECT_B3_IND_REASON_B3(CMSG);
	i->NCCI = 0;

	if ((i->FaxState & CAPI_FAX_STATE_ACTIVE) && (i->owner)) {
		char buffer[CAPI_MAX_STRING];
		char *infostring;
		unsigned char *ncpi = (unsigned char *)DISCONNECT_B3_IND_NCPI(CMSG);
		/* if we have fax infos, set them as variables */
		snprintf(buffer, CAPI_MAX_STRING-1, "%d", i->reasonb3);
		pbx_builtin_setvar_helper(i->owner, "FAXREASON", buffer);
		if (i->reasonb3 == 0) {
			pbx_builtin_setvar_helper(i->owner, "FAXREASONTEXT", "OK");
		} else if ((infostring = capi_info_string(i->reasonb3)) != NULL) {
			pbx_builtin_setvar_helper(i->owner, "FAXREASONTEXT", infostring);
		} else {
			pbx_builtin_setvar_helper(i->owner, "FAXREASONTEXT", "");
		}
		if (ncpi) {
			snprintf(buffer, CAPI_MAX_STRING-1, "%d", read_capi_word(&ncpi[1]));
			pbx_builtin_setvar_helper(i->owner, "FAXRATE", buffer);
			snprintf(buffer, CAPI_MAX_STRING-1, "%d", read_capi_word(&ncpi[3]));
			pbx_builtin_setvar_helper(i->owner, "FAXRESOLUTION", buffer);
			snprintf(buffer, CAPI_MAX_STRING-1, "%d", read_capi_word(&ncpi[5]));
			pbx_builtin_setvar_helper(i->owner, "FAXFORMAT", buffer);
			snprintf(buffer, CAPI_MAX_STRING-1, "%d", read_capi_word(&ncpi[7]));
			pbx_builtin_setvar_helper(i->owner, "FAXPAGES", buffer);
			memcpy(buffer, &ncpi[10], ncpi[9]);
			buffer[ncpi[9]] = 0;
			pbx_builtin_setvar_helper(i->owner, "FAXID", buffer);
		}
	}

	if ((i->state == CAPI_STATE_DISCONNECTING)) {
		if (!(i->isdnstate & CAPI_ISDN_STATE_STAYONLINE)) {
			/* active disconnect */
			capi_send_disconnect(PLCI, NULL);
		}
	} else if ((!(i->isdnstate & CAPI_ISDN_STATE_B3_SELECT)) &&
	           (i->FaxState & CAPI_FAX_STATE_SENDMODE)) {
		capi_send_disconnect(PLCI, NULL);
	}

	if (i->channeltype != CAPI_CHANNELTYPE_NULL) {
		capi_controllers[i->controller]->nfreebchannels++;
	}
}

/*
 * CAPI CONNECT_B3_IND
 */
static void capidev_handle_connect_b3_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	capi_sendf(NULL, 0, CAPI_CONNECT_B3_RESP, NCCI, HEADER_MSGNUM(CMSG),
		"ws",
		0x0000, /* accept */
		capi_rtp_ncpi(i));

	return_on_no_interface("CONNECT_B3_IND");

	i->NCCI = NCCI;
	i->B3count = 0;

	return;
}

/*
 * CAPI DISCONNECT_IND
 */
static void capidev_handle_disconnect_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	struct ast_frame fr = { AST_FRAME_CONTROL, AST_CONTROL_HANGUP, };
	int state;

	capi_sendf(NULL, 0, CAPI_DISCONNECT_RESP, PLCI, HEADER_MSGNUM(CMSG), "");
	
	show_capi_info(i, DISCONNECT_IND_REASON(CMSG));

	return_on_no_interface("DISCONNECT_IND");

	state = i->state;
	i->state = CAPI_STATE_DISCONNECTED;

	i->reason = DISCONNECT_IND_REASON(CMSG);

	if ((i->owner) && (i->owner->hangupcause == 0)) {
		/* set hangupcause, in case there is no 
		 * "cause" information element:
		 */
		i->owner->hangupcause =
			((i->reason & 0xFF00) == 0x3400) ?
			i->reason & 0x7F : AST_CAUSE_NORMAL_CLEARING;
	}

	if (i->FaxState & CAPI_FAX_STATE_ACTIVE) {
		/* in capiFax */
		switch (i->reason) {
		case 0x3400:
		case 0x3490:
		case 0x349f:
			if (i->reasonb3 != 0)
				i->FaxState |= CAPI_FAX_STATE_ERROR;
			break;
		default:
			i->FaxState |= CAPI_FAX_STATE_ERROR;
		}
		i->FaxState &= ~CAPI_FAX_STATE_ACTIVE;
	}

	if ((i->owner) &&
	    ((state == CAPI_STATE_DID) || (state == CAPI_STATE_INCALL)) &&
	    (!(i->isdnstate & CAPI_ISDN_STATE_PBX))) {
		/* the pbx was not started yet */
		cc_verbose(4, 1, VERBOSE_PREFIX_3 "%s: DISCONNECT_IND on incoming without pbx, doing hangup.\n",
			i->vname);
		capi_channel_task(i->owner, CAPI_CHANNEL_TASK_HANGUP); 
		return;
	}

	if (DISCONNECT_IND_REASON(CMSG) == 0x34a2) {
		fr.subclass = AST_CONTROL_CONGESTION;
	}

	if (state == CAPI_STATE_DISCONNECTING) {
		interface_cleanup(i);
	} else {
		local_queue_frame(i, &fr);
		/* PLCI is now removed, make sure it doesn't match with new one */
		i->PLCI = 0xdead0000;
	}
	return;
}

/*
 * CAPI CONNECT_IND
 */
static void capidev_handle_connect_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt **interface)
{
	struct capi_pvt *i;
	char *DNID;
	char *CID;
	char *KEYPAD = NULL;
	int callernplan = 0, callednplan = 0;
	int controller = 0;
	char *msn;
	char buffer[CAPI_MAX_STRING];
	char buffer_r[CAPI_MAX_STRING];
	char *buffer_rp = buffer_r;
	char *magicmsn = "*\0";
	char *emptydnid = "\0";
	int callpres = 0;
	char bchannelinfo[2] = { '0', 0 };

	if (*interface) {
	    /* chan_capi does not support 
	     * double connect indications !
	     * (This is used to update 
	     *  telephone numbers and 
	     *  other information)
	     */
		return;
	}

	if (CONNECT_IND_KEYPADFACILITY(CMSG)) {
		KEYPAD = capi_number(CONNECT_IND_KEYPADFACILITY(CMSG), 0);
	}
	DNID = capi_number(CONNECT_IND_CALLEDPARTYNUMBER(CMSG), 1);
	if (!DNID) {
		if (!KEYPAD) {
			DNID = emptydnid;
		} else {
			/* if keypad is signaled instead, use it as DID with 'K' */
			DNID = alloca(AST_MAX_EXTENSION);
			snprintf(DNID, AST_MAX_EXTENSION -1, "K%s", KEYPAD);
		}
	}

	if (CONNECT_IND_CALLEDPARTYNUMBER(CMSG)[0] > 1) {
		callednplan = (CONNECT_IND_CALLEDPARTYNUMBER(CMSG)[1] & 0x7f);
	}

	CID = capi_number(CONNECT_IND_CALLINGPARTYNUMBER(CMSG), 2);
	if (CONNECT_IND_CALLINGPARTYNUMBER(CMSG)[0] > 1) {
		callernplan = (CONNECT_IND_CALLINGPARTYNUMBER(CMSG)[1] & 0x7f);
		callpres = (CONNECT_IND_CALLINGPARTYNUMBER(CMSG)[2] & 0x63);
	}
	controller = PLCI & 0xff;
	
	cc_verbose(1, 1, VERBOSE_PREFIX_3 "CONNECT_IND (PLCI=%#x,DID=%s,CID=%s,CIP=%#x,CONTROLLER=%#x)\n",
		PLCI, DNID, CID, CONNECT_IND_CIPVALUE(CMSG), controller);

	if (CONNECT_IND_BCHANNELINFORMATION(CMSG) && (CONNECT_IND_BCHANNELINFORMATION(CMSG)[0] > 0)) {
		bchannelinfo[0] = CONNECT_IND_BCHANNELINFORMATION(CMSG)[1] + '0';
	}

	/* well...somebody is calling us. let's set up a channel */
	cc_mutex_lock(&iflock);
	for (i = capi_iflist; i; i = i->next) {
		if (i->used) {
			/* is already used */
			continue;
		}
		if (i->controller != controller) {
			continue;
		}
		if (i->channeltype == CAPI_CHANNELTYPE_B) {
			if (bchannelinfo[0] != '0')
				continue;
		} else {
			if (bchannelinfo[0] == '0')
				continue;
		}
		cc_copy_string(buffer, i->incomingmsn, sizeof(buffer));
		for (msn = strtok_r(buffer, ",", &buffer_rp); msn; msn = strtok_r(NULL, ",", &buffer_rp)) {
			if (!strlen(DNID)) {
				/* if no DNID, only accept if '*' was specified */
				if (strncasecmp(msn, magicmsn, strlen(msn))) {
					continue;
				}
				cc_copy_string(i->dnid, emptydnid, sizeof(i->dnid));
			} else {
				/* make sure the number match exactly or may match on ptp mode */
				cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: msn='%s' DNID='%s' %s\n",
					i->vname, msn, DNID,
					(i->isdnmode == CAPI_ISDNMODE_MSN)?"MSN":"DID");
				if ((strcasecmp(msn, DNID)) &&
				   ((i->isdnmode == CAPI_ISDNMODE_MSN) ||
				    (strlen(msn) >= strlen(DNID)) ||
				    (strncasecmp(msn, DNID, strlen(msn)))) &&
				   (strncasecmp(msn, magicmsn, strlen(msn)))) {
					continue;
				}
				cc_copy_string(i->dnid, DNID, sizeof(i->dnid));
			}
			if (CID != NULL) {
				if ((callernplan & 0x70) == CAPI_ETSI_NPLAN_NATIONAL)
					snprintf(i->cid, (sizeof(i->cid)-1), "%s%s%s",
						i->prefix, capi_national_prefix, CID);
				else if ((callernplan & 0x70) == CAPI_ETSI_NPLAN_INTERNAT)
					snprintf(i->cid, (sizeof(i->cid)-1), "%s%s%s",
						i->prefix, capi_international_prefix, CID);
				else
					snprintf(i->cid, (sizeof(i->cid)-1), "%s%s",
						i->prefix, CID);
			} else {
				cc_copy_string(i->cid, emptyid, sizeof(i->cid));
			}
			i->cip = CONNECT_IND_CIPVALUE(CMSG);
			i->PLCI = PLCI;
			i->MessageNumber = HEADER_MSGNUM(CMSG);
			i->cid_ton = callernplan;

			capi_new(i, AST_STATE_DOWN);
			if (i->isdnmode == CAPI_ISDNMODE_DID) {
				i->state = CAPI_STATE_DID;
			} else {
				i->state = CAPI_STATE_INCALL;
			}

			if (!i->owner) {
				interface_cleanup(i);
				break;
			}
			i->transfercapability = cip2tcap(i->cip);
 			i->owner->transfercapability = i->transfercapability;
			if (capi_tcap_is_digital(i->transfercapability)) {
				i->bproto = CC_BPROTO_TRANSPARENT;
			}
			i->owner->cid.cid_pres = callpres;
			cc_verbose(3, 0, VERBOSE_PREFIX_2 "%s: Incoming call '%s' -> '%s'\n",
				i->vname, i->cid, i->dnid);

			*interface = i;
			cc_mutex_unlock(&iflock);
			cc_mutex_lock(&i->lock);
		
			pbx_builtin_setvar_helper(i->owner, "TRANSFERCAPABILITY", transfercapability2str(i->transfercapability));
			pbx_builtin_setvar_helper(i->owner, "BCHANNELINFO", bchannelinfo);
			sprintf(buffer, "%d", callednplan);
			pbx_builtin_setvar_helper(i->owner, "CALLEDTON", buffer);
			/*
			pbx_builtin_setvar_helper(i->owner, "CALLINGSUBADDRESS",
				CONNECT_IND_CALLINGPARTYSUBADDRESS(CMSG));
			pbx_builtin_setvar_helper(i->owner, "CALLEDSUBADDRESS",
				CONNECT_IND_CALLEDPARTYSUBADDRESS(CMSG));
			pbx_builtin_setvar_helper(i->owner, "USERUSERINFO",
				CONNECT_IND_USERUSERDATA(CMSG));
			*/
			/* TODO : set some more variables on incoming call */
			/*
			pbx_builtin_setvar_helper(i->owner, "ANI2", buffer);
			pbx_builtin_setvar_helper(i->owner, "SECONDCALLERID", buffer);
			*/

			/* Handle QSIG informations, if any */
			cc_qsig_handle_capiind(CONNECT_IND_FACILITYDATAARRAY(CMSG), i);
			
			if ((i->isdnmode == CAPI_ISDNMODE_MSN) && (i->immediate)) {
				/* if we don't want to wait for SETUP/SENDING-COMPLETE in MSN mode */
				start_pbx_on_match(i, PLCI, HEADER_MSGNUM(CMSG));
			}
			return;
		}
	}
	cc_mutex_unlock(&iflock);

	/* obviously we are not called...so tell capi to ignore this call */

	if (capidebug) {
		cc_log(LOG_WARNING, "did not find device for msn = %s\n", DNID);
	}
	
	capi_sendf(NULL, 0, CAPI_CONNECT_RESP, CONNECT_IND_PLCI(CMSG), HEADER_MSGNUM(CMSG),
			"w()()()()()", 1 /* ignore */);
	return;
}

/*
 * CAPI FACILITY_CONF
 */
static void capidev_handle_facility_confirmation(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt **i)
{
	int selector;

	selector = FACILITY_CONF_FACILITYSELECTOR(CMSG);

	if (selector == FACILITYSELECTOR_SUPPLEMENTARY) {
		handle_facility_confirmation_supplementary(CMSG, PLCI, NCCI, i);
		return;
	}
	
	if (*i == NULL)
		return;

	if (selector == FACILITYSELECTOR_DTMF) {
		cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: DTMF conf(PLCI=%#x)\n",
			(*i)->vname, PLCI);
		return;
	}
	if (selector == (*i)->ecSelector) {
		if (FACILITY_CONF_INFO(CMSG)) {
			cc_verbose(2, 0, VERBOSE_PREFIX_3 "%s: Error setting up echo canceller (PLCI=%#x)\n",
				(*i)->vname, PLCI);
			return;
		}
		if (FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[1] == EC_FUNCTION_DISABLE) {
			cc_verbose(3, 0, VERBOSE_PREFIX_3 "%s: Echo canceller successfully disabled (PLCI=%#x)\n",
				(*i)->vname, PLCI);
		} else {
			cc_verbose(3, 0, VERBOSE_PREFIX_3 "%s: Echo canceller successfully set up (PLCI=%#x)\n",
				(*i)->vname, PLCI);
		}
		return;
	}
	if (selector == FACILITYSELECTOR_LINE_INTERCONNECT) {
		if ((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[1] == 0x1) &&
		    (FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[2] == 0x0)) {
			/* enable */
			if (FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[0] > 12) {
				show_capi_info(*i, read_capi_word(&FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[12]));
			}
		} else {
			/* disable */
			if (FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[0] > 12) {
				show_capi_info(*i, read_capi_word(&FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[12]));
			}
		}
		return;
	}
	cc_log(LOG_ERROR, "%s: unhandled FACILITY_CONF 0x%x\n",
		(*i)->vname, FACILITY_CONF_FACILITYSELECTOR(CMSG));
}

/*
 * show error in confirmation
 */
static void show_capi_conf_error(struct capi_pvt *i, 
				 unsigned int PLCI, u_int16_t wInfo, 
				 u_int16_t wCmd)
{
	const char *name = channeltype;

	if (i)
		name = i->vname;
	
	if ((wCmd == CAPI_P_CONF(ALERT)) && (wInfo == 0x0003)) {
		/* Alert already sent by another application */
		return;
	}
		
	if (wInfo == 0x2002) {
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "%s: "
			       "0x%x (wrong state) PLCI=0x%x "
			       "Command=%s,0x%04x\n",
			       name, wInfo, PLCI, capi_command_to_string(wCmd), wCmd);
	} else {
		cc_log(LOG_WARNING, "%s: conf_error 0x%04x "
			"PLCI=0x%x Command=%s,0x%04x\n",
			name, wInfo, PLCI, capi_command_to_string(wCmd), wCmd);
	}
	return;
}

/*
 * check special conditions, wake waiting threads and send outstanding commands
 * for the given interface
 */
static void capidev_post_handling(struct capi_pvt *i, _cmsg *CMSG)
{
	unsigned short capicommand = CAPICMD(CMSG->Command, CMSG->Subcommand);

	if ((i->waitevent == CAPI_WAITEVENT_B3_UP) &&
	    ((i->isdnstate & CAPI_ISDN_STATE_B3_UP))) {
		i->waitevent = 0;
		ast_cond_signal(&i->event_trigger);
		cc_verbose(4, 1, "%s: found and signal for b3 up state.\n",
			i->vname);
		return;
	}
	if ((i->waitevent == CAPI_WAITEVENT_B3_DOWN) &&
	    (!(i->isdnstate & (CAPI_ISDN_STATE_B3_UP | CAPI_ISDN_STATE_B3_PEND)))) {
		i->waitevent = 0;
		ast_cond_signal(&i->event_trigger);
		cc_verbose(4, 1, "%s: found and signal for b3 down state.\n",
			i->vname);
		return;
	}
	if ((i->waitevent == CAPI_WAITEVENT_ANSWER_FINISH) &&
	    (i->state != CAPI_STATE_ANSWERING)) {
		i->waitevent = 0;
		ast_cond_signal(&i->event_trigger);
		cc_verbose(4, 1, "%s: found and signal for finished ANSWER state.\n",
			i->vname);
		return;
	}
	if ((i->waitevent == CAPI_WAITEVENT_HOLD_IND) &&
	    (HEADER_CMD(CMSG) == CAPI_P_IND(FACILITY)) &&
		(FACILITY_IND_FACILITYSELECTOR(CMSG) == FACILITYSELECTOR_SUPPLEMENTARY) &&
		(read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[1]) == 0x0002)) {
		i->waitevent = 0;
		ast_cond_signal(&i->event_trigger);
		cc_verbose(4, 1, "%s: found and signal for HOLD indication.\n",
			i->vname);
		return;
	}
	if ((i->waitevent == CAPI_WAITEVENT_ECT_IND) &&
	    (HEADER_CMD(CMSG) == CAPI_P_IND(FACILITY)) &&
		(FACILITY_IND_FACILITYSELECTOR(CMSG) == FACILITYSELECTOR_SUPPLEMENTARY) &&
		(read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[1]) == 0x0006)) {
		i->waitevent = 0;
		ast_cond_signal(&i->event_trigger);
		cc_verbose(4, 1, "%s: found and signal for ECT indication.\n",
			i->vname);
		return;
	}
	if (i->waitevent == capicommand) {
		i->waitevent = 0;
		ast_cond_signal(&i->event_trigger);
		cc_verbose(4, 1, "%s: found and signal for %s\n",
			i->vname, capi_cmd2str(CMSG->Command, CMSG->Subcommand));
		return;
	}
}

/*
 * handle CONNECT_CONF or FACILITY_CONF(CCBS call)
 */
void capidev_handle_connection_conf(struct capi_pvt **i, unsigned int PLCI,
	unsigned short wInfo, unsigned short wMsgNum)
{
	struct capi_pvt *ii;
	struct ast_frame fr = { AST_FRAME_CONTROL, AST_CONTROL_BUSY, };

	if (*i) {
		cc_log(LOG_ERROR, "CAPI: CONNECT_CONF for already "
			"defined interface received\n");
		return;
	}
	*i = capi_find_interface_by_msgnum(wMsgNum);
	ii = *i;
	if (ii == NULL) {
		return;
	}
	cc_verbose(1, 1, VERBOSE_PREFIX_3 "%s: received CONNECT_CONF PLCI = %#x\n",
		ii->vname, PLCI);
	cc_mutex_lock(&ii->lock);
	if (wInfo == 0) {
		ii->PLCI = PLCI;
	} else {
		/* error in connect, so set correct state and signal busy */
		ii->state = CAPI_STATE_DISCONNECTED;
		if (ii->owner) {
			local_queue_frame(ii, &fr);
		}
	}
}

/*
 * handle CAPI msg
 */
static void capidev_handle_msg(_cmsg *CMSG)
{
	unsigned int NCCI = HEADER_CID(CMSG);
	unsigned int PLCI = (NCCI & 0xffff);
	unsigned short wCmd = HEADER_CMD(CMSG);
	unsigned short wMsgNum = HEADER_MSGNUM(CMSG);
	unsigned short wInfo = 0xffff;
	struct capi_pvt *i = capi_find_interface_by_plci(PLCI);

	if ((wCmd == CAPI_P_IND(DATA_B3)) ||
	    (wCmd == CAPI_P_CONF(DATA_B3))) {
		cc_verbose(7, 1, "%s\n", capi_cmsg2str(CMSG));
	} else {
		cc_verbose(4, 1, "%s\n", capi_cmsg2str(CMSG));
	}

	if (i != NULL)
		cc_mutex_lock(&i->lock);

	/* main switch table */

	switch (wCmd) {

	  /*
	   * CAPI indications
	   */
	case CAPI_P_IND(CONNECT):
		capidev_handle_connect_indication(CMSG, PLCI, NCCI, &i);
		break;
	case CAPI_P_IND(DATA_B3):
		capidev_handle_data_b3_indication(CMSG, PLCI, NCCI, i);
		break;
	case CAPI_P_IND(CONNECT_B3):
		capidev_handle_connect_b3_indication(CMSG, PLCI, NCCI, i);
		break;
	case CAPI_P_IND(CONNECT_B3_ACTIVE):
		capidev_handle_connect_b3_active_indication(CMSG, PLCI, NCCI, i);
		break;
	case CAPI_P_IND(DISCONNECT_B3):
		capidev_handle_disconnect_b3_indication(CMSG, PLCI, NCCI, i);
		break;
	case CAPI_P_IND(DISCONNECT):
		capidev_handle_disconnect_indication(CMSG, PLCI, NCCI, i);
		break;
	case CAPI_P_IND(FACILITY):
		capidev_handle_facility_indication(CMSG, PLCI, NCCI, i);
		break;
	case CAPI_P_IND(INFO):
		capidev_handle_info_indication(CMSG, PLCI, NCCI, i);
		break;
	case CAPI_P_IND(CONNECT_ACTIVE):
		capidev_handle_connect_active_indication(CMSG, PLCI, NCCI, i);
		break;
	case CAPI_P_IND(MANUFACTURER):
		capidev_handle_manufacturer_indication(CMSG, PLCI, NCCI, i);
		break;

	  /*
	   * CAPI confirmations
	   */

	case CAPI_P_CONF(FACILITY):
		wInfo = FACILITY_CONF_INFO(CMSG);
		capidev_handle_facility_confirmation(CMSG, PLCI, NCCI, &i);
		break;
	case CAPI_P_CONF(CONNECT):
		wInfo = CONNECT_CONF_INFO(CMSG);
		capidev_handle_connection_conf(&i, PLCI, wInfo, wMsgNum);
		break;
	case CAPI_P_CONF(CONNECT_B3):
		wInfo = CONNECT_B3_CONF_INFO(CMSG);
		if(i == NULL) break;
		if (wInfo == 0) {
			i->NCCI = NCCI;
		} else {
			i->isdnstate &= ~(CAPI_ISDN_STATE_B3_UP | CAPI_ISDN_STATE_B3_PEND);
		}
		break;
	case CAPI_P_CONF(ALERT):
		wInfo = ALERT_CONF_INFO(CMSG);
		if(i == NULL) break;
		if (!i->owner) break;
		if ((wInfo & 0xff00) == 0) {
			if (i->state != CAPI_STATE_DISCONNECTING) {
				i->state = CAPI_STATE_ALERTING;
				if (i->owner->_state == AST_STATE_RING) {
					i->owner->rings = 1;
				}
			}
		}
		break;	    
	case CAPI_P_CONF(SELECT_B_PROTOCOL):
		wInfo = SELECT_B_PROTOCOL_CONF_INFO(CMSG);
		if(i == NULL) break;
		if (!wInfo) {
			i->isdnstate &= ~CAPI_ISDN_STATE_B3_SELECT;
			if ((i->outgoing) && (i->FaxState & CAPI_FAX_STATE_SENDMODE)) {
				cc_start_b3(i);
			}
			if ((i->owner) && (i->FaxState & CAPI_FAX_STATE_ACTIVE)) {
				capi_echo_canceller(i, EC_FUNCTION_DISABLE);
				capi_detect_dtmf(i, 0);
			}
		} else {
			i->isdnstate &= ~CAPI_ISDN_STATE_B3_PEND;
		}
		break;
	case CAPI_P_CONF(DATA_B3):
		wInfo = DATA_B3_CONF_INFO(CMSG);
		if ((i) && (i->B3count > 0)) {
			i->B3count--;
		}
		if ((i) && (i->FaxState & CAPI_FAX_STATE_SENDMODE)) {
			capidev_send_faxdata(i);
		}
		break;
 
	case CAPI_P_CONF(DISCONNECT):
		wInfo = DISCONNECT_CONF_INFO(CMSG);
		break;

	case CAPI_P_CONF(DISCONNECT_B3):
		wInfo = DISCONNECT_B3_CONF_INFO(CMSG);
		break;

	case CAPI_P_CONF(LISTEN):
		wInfo = LISTEN_CONF_INFO(CMSG);
		break;

	case CAPI_P_CONF(INFO):
		wInfo = INFO_CONF_INFO(CMSG);
		break;

	default:
		cc_log(LOG_ERROR, "CAPI: Command=%s,0x%04x",
			capi_command_to_string(wCmd), wCmd);
		break;
	}

	if (wInfo != 0xffff) {
		if (wInfo) {
			show_capi_conf_error(i, PLCI, wInfo, wCmd);
		}
		show_capi_info(i, wInfo);
	}

	if (i == NULL) {
		cc_verbose(2, 1, VERBOSE_PREFIX_4
			"CAPI: Command=%s,0x%04x: no interface for PLCI="
			"%#x, MSGNUM=%#x!\n", capi_command_to_string(wCmd),
			wCmd, PLCI, wMsgNum);
	} else {
		capidev_post_handling(i, CMSG);
		cc_mutex_unlock(&i->lock);
	}

	return;
}

/*
 * deflect a call
 */
static int pbx_capi_call_deflect(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	char *number;
	int numberlen;
	char facnumber[32];

	if (!param) {
		cc_log(LOG_WARNING, "capi deflection requires an argument (destination phone number)\n");
		return -1;
	}
	number = strsep(&param, "|");
	numberlen = strlen(number);

	if (!numberlen) {
		cc_log(LOG_WARNING, "capi deflection requires an argument (destination phone number)\n");
		return -1;
	}
	if (numberlen > 35) {
		cc_log(LOG_WARNING, "capi deflection does only support phone number up to 35 digits\n");
		return -1;
	}
	if (!(capi_controllers[i->controller]->CD)) {
		cc_log(LOG_NOTICE,"%s: CALL DEFLECT for %s not supported by controller.\n",
			i->vname, c->name);
		return -1;
	}

	cc_mutex_lock(&i->lock);

	if ((i->state != CAPI_STATE_INCALL) &&
	    (i->state != CAPI_STATE_DID) &&
	    (i->state != CAPI_STATE_ALERTING)) {
		cc_mutex_unlock(&i->lock);
		cc_log(LOG_WARNING, "wrong state of call for call deflection\n");
		return -1;
	}
	if (i->state != CAPI_STATE_ALERTING) {
		pbx_capi_alert(c);
	}

	facnumber[0] = 0x03 + numberlen; 
	facnumber[1] = 0x00; /* type of facility number */
	facnumber[2] = 0x00; /* number plan */
	facnumber[3] = 0x00; /* presentation allowed */
	memcpy(&facnumber[4], number, numberlen);
	
	capi_sendf(i, 1, CAPI_FACILITY_REQ, i->PLCI, get_capi_MessageNumber(),
		"w(w(ws()))",
		FACILITYSELECTOR_SUPPLEMENTARY,
		0x000d,  /* call deflection */
		0x0001,  /* display of own address allowed */
		&facnumber[0]
	);

	cc_mutex_unlock(&i->lock);

	cc_verbose(2, 1, VERBOSE_PREFIX_3 "%s: sent FACILITY_REQ for CD PLCI = %#x\n",
		i->vname, i->PLCI);

	return 0;
}

/*
 * store the peer for future actions 
 */
static int pbx_capi_peer_link(struct ast_channel *c, char *param)
{
	char buffer[32];
	int id;

	id = cc_add_peer_link_id(c);

	if (id >= 0) {
		snprintf(buffer, sizeof(buffer) - 1, "%d", id);
		pbx_builtin_setvar_helper(c, "_CAPIPEERLINKID", buffer);
	}

	cc_verbose(2, 1, VERBOSE_PREFIX_3 "Added %s as CAPI peer link.\n",
		c->name);

	return 0;
}

/*
 * retrieve a hold on call
 */
static int pbx_capi_retrieve(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c); 
	unsigned int plci = 0;

	if (c->tech == &capi_tech) {
		plci = i->onholdPLCI;
	} else {
		i = NULL;
	}

	if (param) {
		plci = (unsigned int)strtoul(param, NULL, 0);
		cc_mutex_lock(&iflock);
		for (i = capi_iflist; i; i = i->next) {
			if (i->onholdPLCI == plci)
				break;
		}
		cc_mutex_unlock(&iflock);
		if (!i) {
			plci = 0;
		}
	}

	if (!i) {
		cc_log(LOG_WARNING, "%s is not valid or not on capi hold to retrieve!\n",
			c->name);
		return 0;
	}

	if ((i->state != CAPI_STATE_ONHOLD) &&
	    (i->isdnstate & CAPI_ISDN_STATE_HOLD)) {
		int waitcount = 20;
		while ((waitcount > 0) && (i->state != CAPI_STATE_ONHOLD)) {
			usleep(10000);
			waitcount--;
		}
	}

	if ((!plci) || (i->state != CAPI_STATE_ONHOLD)) {
		cc_log(LOG_WARNING, "%s: 0x%x is not valid or not on hold to retrieve!\n",
			i->vname, plci);
		return 0;
	}
	cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: using PLCI=%#x for retrieve\n",
		i->vname, plci);

	if (!(capi_controllers[i->controller]->holdretrieve)) {
		cc_log(LOG_NOTICE,"%s: RETRIEVE for %s not supported by controller.\n",
			i->vname, c->name);
		return -1;
	}

	if (param != NULL)
		cc_mutex_lock(&i->lock);

	capi_sendf(i, (param == NULL) ? 0:1, CAPI_FACILITY_REQ, plci, get_capi_MessageNumber(),
		"w(w())",
		FACILITYSELECTOR_SUPPLEMENTARY,
		0x0003  /* retrieve */
	);
	
	i->isdnstate &= ~CAPI_ISDN_STATE_HOLD;

	if (param != NULL)
		cc_mutex_unlock(&i->lock);

	cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: sent RETRIEVE for PLCI=%#x\n",
		i->vname, plci);

	pbx_builtin_setvar_helper(i->owner, "_CALLERHOLDID", NULL);

	return 0;
}

/*
 * explicit transfer a held call
 */
static int pbx_capi_ect(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	struct capi_pvt *ii = NULL;
	const char *id;
	unsigned int plci = 0;
	unsigned int ectplci;
	char *holdid;

	if ((id = pbx_builtin_getvar_helper(c, "CALLERHOLDID"))) {
		plci = (unsigned int)strtoul(id, NULL, 0);
	}
	
	holdid = strsep(&param, "|");

	if (holdid) {
		plci = (unsigned int)strtoul(holdid, NULL, 0);
	}

	if (!plci) {
		cc_log(LOG_WARNING, "%s: No id for ECT !\n", i->vname);
		return -1;
	}

	cc_mutex_lock(&iflock);
	for (ii = capi_iflist; ii; ii = ii->next) {
		if (ii->onholdPLCI == plci)
			break;
	}
	cc_mutex_unlock(&iflock);

	if (!ii) {
		cc_log(LOG_WARNING, "%s: 0x%x is not on hold !\n",
			i->vname, plci);
		return -1;
	}

	ectplci = plci;
	if ((param) && (*param == 'x')) {
		ectplci = i->PLCI;
	}

	cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: using PLCI=%#x for ECT\n",
		i->vname, ectplci);

	if (!(capi_controllers[i->controller]->ECT)) {
		cc_log(LOG_WARNING, "%s: ECT for %s not supported by controller.\n",
			i->vname, c->name);
		return -1;
	}

	if (!(ii->isdnstate & CAPI_ISDN_STATE_HOLD)) {
		cc_log(LOG_WARNING, "%s: PLCI %#x (%s) is not on hold for ECT\n",
			i->vname, plci, ii->vname);
		return -1;
	}

	cc_disconnect_b3(i, 1);

	if (i->state != CAPI_STATE_CONNECTED) {
		cc_log(LOG_WARNING, "%s: destination not connected for ECT\n",
			i->vname);
		return -1;
	}

	cc_mutex_lock(&ii->lock);

	/* implicit ECT */
	capi_sendf(ii, 1, CAPI_FACILITY_REQ, ectplci, get_capi_MessageNumber(),
		"w(w(d))",
		FACILITYSELECTOR_SUPPLEMENTARY,
		0x0006,  /* ECT */
		plci
	);

	ii->isdnstate &= ~CAPI_ISDN_STATE_HOLD;
	ii->isdnstate |= CAPI_ISDN_STATE_ECT;
	i->isdnstate |= CAPI_ISDN_STATE_ECT;
	
	cc_mutex_unlock(&ii->lock);

	cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: sent ECT for PLCI=%#x to PLCI=%#x\n",
		i->vname, plci, ectplci);

	return 0;
}

/*
 * hold a call
 */
static int pbx_capi_hold(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	char buffer[16];

	/*  TODO: support holdtype notify */

	if ((i->isdnstate & CAPI_ISDN_STATE_HOLD)) {
		cc_log(LOG_NOTICE,"%s: %s already on hold.\n",
			i->vname, c->name);
		return 0;
	}

	if (!(i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
		cc_log(LOG_NOTICE,"%s: Cannot put on hold %s while not connected.\n",
			i->vname, c->name);
		return 0;
	}
	if (!(capi_controllers[i->controller]->holdretrieve)) {
		cc_log(LOG_NOTICE,"%s: HOLD for %s not supported by controller.\n",
			i->vname, c->name);
		return 0;
	}

	cc_mutex_lock(&i->lock);

	capi_sendf(i, 1, CAPI_FACILITY_REQ, i->PLCI, get_capi_MessageNumber(),
		"w(w())",
		FACILITYSELECTOR_SUPPLEMENTARY,
		0x0002  /* hold */
	);

	i->onholdPLCI = i->PLCI;
	i->isdnstate |= CAPI_ISDN_STATE_HOLD;

	cc_mutex_unlock(&i->lock);

	cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: sent HOLD for PLCI=%#x\n",
		i->vname, i->PLCI);

	snprintf(buffer, sizeof(buffer) - 1, "%d", i->PLCI);
	if (param) {
		pbx_builtin_setvar_helper(i->owner, param, buffer);
	}
	pbx_builtin_setvar_helper(i->owner, "_CALLERHOLDID", buffer);

	return 0;
}

/*
 * report malicious call
 */
static int pbx_capi_malicious(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);

	if (!(capi_controllers[i->controller]->MCID)) {
		cc_log(LOG_NOTICE, "%s: MCID for %s not supported by controller.\n",
			i->vname, c->name);
		return -1;
	}

	cc_mutex_lock(&i->lock);

	capi_sendf(i, 1, CAPI_FACILITY_REQ, i->PLCI, get_capi_MessageNumber(),
		"w(w())",
		FACILITYSELECTOR_SUPPLEMENTARY,
		0x000e  /* MCID */
	);

	cc_mutex_unlock(&i->lock);

	cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: sent MCID for PLCI=%#x\n",
		i->vname, i->PLCI);

	return 0;
}

/*
 * set echo cancel
 */
static int pbx_capi_echocancel(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);

	if (!param) {
		cc_log(LOG_WARNING, "Parameter for echocancel missing.\n");
		return -1;
	}
	if (ast_true(param)) {
		i->doEC = 1;
		capi_echo_canceller(i, EC_FUNCTION_ENABLE);
	} else if (ast_false(param)) {
		capi_echo_canceller(i, EC_FUNCTION_DISABLE);
		i->doEC = 0;
	} else {
		cc_log(LOG_WARNING, "Parameter for echocancel invalid.\n");
		return -1;
	}
	cc_verbose(2, 0, VERBOSE_PREFIX_4 "%s: echocancel switched %s\n",
		i->vname, i->doEC ? "ON":"OFF");
	return 0;
}

/*
 * set echo squelch
 */
static int pbx_capi_echosquelch(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);

	if (!param) {
		cc_log(LOG_WARNING, "Parameter for echosquelch missing.\n");
		return -1;
	}
	if (ast_true(param)) {
		i->doES = 1;
	} else if (ast_false(param)) {
		i->doES = 0;
	} else {
		cc_log(LOG_WARNING, "Parameter for echosquelch invalid.\n");
		return -1;
	}
	cc_verbose(2, 0, VERBOSE_PREFIX_4 "%s: echosquelch switched %s\n",
		i->vname, i->doES ? "ON":"OFF");
	return 0;
}

/*
 * set holdtype
 */
static int pbx_capi_holdtype(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);

	if (!param) {
		cc_log(LOG_WARNING, "Parameter for holdtype missing.\n");
		return -1;
	}
	if (!strcasecmp(param, "hold")) {
		i->doholdtype = CC_HOLDTYPE_HOLD;
	} else if (!strcasecmp(param, "notify")) {
		i->doholdtype = CC_HOLDTYPE_NOTIFY;
	} else if (!strcasecmp(param, "local")) {
		i->doholdtype = CC_HOLDTYPE_LOCAL;
	} else {
		cc_log(LOG_WARNING, "Parameter for holdtype invalid.\n");
		return -1;
	}
	cc_verbose(2, 0, VERBOSE_PREFIX_4 "%s: holdtype switched to %s\n",
		i->vname, param);
	return 0;
}

/*
 * send the disconnect commands to capi
 */
static void capi_disconnect(struct capi_pvt *i)
{
	cc_mutex_lock(&i->lock);

	i->isdnstate &= ~CAPI_ISDN_STATE_STAYONLINE;
	if ((i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
		cc_disconnect_b3(i, 0);
	} else {
		capi_send_disconnect(i->PLCI, NULL);
	}
	
	cc_mutex_unlock(&i->lock);
}

/*
 * really hangup a channel if the stay-online mode was activated
 */
static int pbx_capi_realhangup(struct ast_channel *c, char *param)
{
	struct capi_pvt *i;

	cc_mutex_lock(&iflock);
	for (i = capi_iflist; i; i = i->next) {
		if (i->peer == c)
			break;
	}
	cc_mutex_unlock(&iflock);

	if ((i) && (i->state == CAPI_STATE_DISCONNECTING)) {
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: capi command hangup PLCI=0x%#x.\n",
			i->vname, i->PLCI);
		capi_disconnect(i);
	}

	return 0;
}

/*
 * set early-B3 (progress) for incoming connections
 * (only for NT mode)
 */
static int pbx_capi_signal_progress(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	unsigned char fac[] = "\x04\x1e\x02\x82\x88"; /* In-Band info available */

	if ((i->state != CAPI_STATE_DID) && (i->state != CAPI_STATE_INCALL)) {
		cc_log(LOG_DEBUG, "wrong channel state to signal PROGRESS\n");
		return 0;
	}
	if (!(i->ntmode)) {
		cc_log(LOG_WARNING, "PROGRESS sending for non NT-mode not possible\n");
		return 0;
	}
	if ((i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
		cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: signal_progress in NT: B-channel already up\n",
			i->vname);
		return 0;
	}
	if ((i->isdnstate & CAPI_ISDN_STATE_B3_PEND)) {
		cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: signal_progress in NT: B-channel already pending\n",
			i->vname);
		return 0;
	}
	i->isdnstate |= CAPI_ISDN_STATE_B3_PEND;

	cc_select_b(i, NULL);

	/* send facility for Progress 'In-Band info available' */
	capi_sendf(NULL, 0, CAPI_INFO_REQ, i->PLCI, get_capi_MessageNumber(),
		"()(()()()s())",
		fac
	);

	return 0;
}

/*
 * Initiate a Three-Party-Conference (3PTY) with one active and one
 * held call
 */
static int pbx_capi_3pty_begin(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	struct capi_pvt *ii = NULL;
	const char	*id;
	unsigned int	plci = 0;

	if ((id = pbx_builtin_getvar_helper(c, "CALLERHOLDID"))) {
		plci = (unsigned int)strtoul(id, NULL, 0);
	}
	
	if (param) {
		plci = (unsigned int)strtoul(param, NULL, 0);
	}

	if (!plci) {
		cc_log(LOG_WARNING, "%s: No id for 3PTY !\n", i->vname);
		return -1;
	}

	cc_mutex_lock(&iflock);
	for (ii = capi_iflist; ii; ii = ii->next) {
		if (ii->onholdPLCI == plci)
			break;
	}
	cc_mutex_unlock(&iflock);

	if (!ii) {
		cc_log(LOG_WARNING, "%s: 0x%x is not on hold !\n",
			i->vname, plci);
		return -1;
	}

	cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: using PLCI=%#x for 3PTY\n",
		i->vname, plci);

	if (!(ii->isdnstate & CAPI_ISDN_STATE_HOLD)) {
		cc_log(LOG_WARNING, "%s: PLCI %#x (%s) is not on hold for 3PTY\n",
			i->vname, plci, ii->vname);
		return -1;
	}
	if (!(i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
		cc_log(LOG_NOTICE,"%s: Cannot initiate conference %s while not connected.\n",
			i->vname, c->name);
		return 0;
	}
	if (!(capi_controllers[i->controller]->threePTY)) {
	        cc_log(LOG_NOTICE,"%s: 3PTY for %s not supported by controller.\n",
			i->vname, c->name);
		return 0;
	}

	cc_mutex_lock(&ii->lock);

	capi_sendf(ii, 1, CAPI_FACILITY_REQ, plci, get_capi_MessageNumber(),
		"w(w(d))",
		FACILITYSELECTOR_SUPPLEMENTARY,
		0x0007,  /* 3PTY begin */
		plci
	);

	ii->isdnstate &= ~CAPI_ISDN_STATE_HOLD;
	ii->isdnstate |= CAPI_ISDN_STATE_3PTY;
	i->isdnstate |= CAPI_ISDN_STATE_3PTY;

	cc_mutex_unlock(&ii->lock);

	cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: sent 3PTY for PLCI=%#x to PLCI=%#x\n",
		i->vname, plci, i->PLCI);

	return 0;
}

/*
 * struct of capi commands
 */
static struct capicommands_s {
	char *cmdname;
	int (*cmd)(struct ast_channel *, char *);
	int capionly;
} capicommands[] = {
	{ "peerlink",     pbx_capi_peer_link,       0 },
	{ "progress",     pbx_capi_signal_progress, 1 },
	{ "deflect",      pbx_capi_call_deflect,    1 },
	{ "receivefax",   pbx_capi_receive_fax,     1 },
	{ "sendfax",      pbx_capi_send_fax,        1 },
	{ "echosquelch",  pbx_capi_echosquelch,     1 },
	{ "echocancel",   pbx_capi_echocancel,      1 },
	{ "malicious",    pbx_capi_malicious,       1 },
	{ "hold",         pbx_capi_hold,            1 },
	{ "holdtype",     pbx_capi_holdtype,        1 },
	{ "retrieve",     pbx_capi_retrieve,        0 },
	{ "ect",          pbx_capi_ect,             1 },
	{ "3pty_begin",   pbx_capi_3pty_begin,      1 },
	{ "ccbs",         pbx_capi_ccbs,            0 },
	{ "ccbsstop",     pbx_capi_ccbsstop,        0 },
	{ "ccpartybusy",  pbx_capi_ccpartybusy,     0 },
	{ "chat",         pbx_capi_chat,            0 },
	{ "hangup",       pbx_capi_realhangup,      0 },
 	{ "qsig_ssct",	  pbx_capi_qsig_ssct,	    1 },
  	{ "qsig_ct",      pbx_capi_qsig_ct,         1 },
   	{ "qsig_callmark",pbx_capi_qsig_callmark,   1 },
	{ "qsig_getplci", pbx_capi_qsig_getplci,    1 },
  	{ NULL, NULL, 0 }
};

/*
 * capi command interface
 */
static int pbx_capicommand_exec(struct ast_channel *chan, void *data)
{
	int res = 0;
#ifdef CC_AST_HAS_VERSION_1_4
	struct ast_module_user *u;
#else
	struct localuser *u;
#endif
	char *s;
	char *stringp;
	char *command, *params;
	struct capicommands_s *capicmd = &capicommands[0];

	if (!data) {
		cc_log(LOG_WARNING, "capiCommand requires arguments\n");
		return -1;
	}

#ifdef CC_AST_HAS_VERSION_1_4
	u = ast_module_user_add(chan);
#else
	LOCAL_USER_ADD(u);
#endif

	s = ast_strdupa(data);
	stringp = s;
	command = strsep(&stringp, "|");
	params = stringp;
	cc_verbose(2, 1, VERBOSE_PREFIX_3 "capiCommand: '%s' '%s'\n",
		command, params);

	while(capicmd->cmd) {
		if (!strcasecmp(capicmd->cmdname, command))
			break;
		capicmd++;
	}
	if (!capicmd->cmd) {
#ifdef CC_AST_HAS_VERSION_1_4
	ast_module_user_remove(u);
#else
		LOCAL_USER_REMOVE(u);
#endif
		cc_log(LOG_WARNING, "Unknown command '%s' for capiCommand\n",
			command);
		return -1;
	}

	if ((capicmd->capionly) && (chan->tech != &capi_tech)) {
#ifdef CC_AST_HAS_VERSION_1_4
	ast_module_user_remove(u);
#else
		LOCAL_USER_REMOVE(u);
#endif
		cc_log(LOG_WARNING, "capiCommand works on CAPI channels only, check your extensions.conf!\n");
		return -1;
	}

	res = (capicmd->cmd)(chan, params);
	
#ifdef CC_AST_HAS_VERSION_1_4
	ast_module_user_remove(u);
#else
	LOCAL_USER_REMOVE(u);
#endif
	return(res);
}

/*
 * we don't support own indications
 */
#ifdef CC_AST_HAS_INDICATE_DATA
static int pbx_capi_indicate(struct ast_channel *c, int condition, const void *data, size_t datalen)
#else
static int pbx_capi_indicate(struct ast_channel *c, int condition)
#endif
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	int ret = -1;

	if (i == NULL) {
		return -1;
	}

	cc_mutex_lock(&i->lock);

	switch (condition) {
	case AST_CONTROL_RINGING:
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested RINGING-Indication for %s\n",
			i->vname, c->name);
		/* TODO somehow enable unhold on ringing, but when wanted only */
		/* 
		if (i->isdnstate & CAPI_ISDN_STATE_HOLD)
			pbx_capi_retrieve(c, NULL);
		*/
		if (i->ntmode) {
			if ((i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
				ret = 0;
			}
			pbx_capi_signal_progress(c, NULL);
			pbx_capi_alert(c);
		} else {
			ret = pbx_capi_alert(c);
		}
		break;
	case AST_CONTROL_BUSY:
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested BUSY-Indication for %s\n",
			i->vname, c->name);
		if ((i->state == CAPI_STATE_ALERTING) ||
		    (i->state == CAPI_STATE_DID) || (i->state == CAPI_STATE_INCALL)) {
			capi_sendf(NULL, 0, CAPI_CONNECT_RESP, i->PLCI, i->MessageNumber,
				"w()()()()()", 3);
			ret = 0;
		}
		if ((i->isdnstate & CAPI_ISDN_STATE_HOLD))
			pbx_capi_retrieve(c, NULL);
		break;
	case AST_CONTROL_CONGESTION:
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested CONGESTION-Indication for %s\n",
			i->vname, c->name);
		if ((i->state == CAPI_STATE_ALERTING) ||
		    (i->state == CAPI_STATE_DID) || (i->state == CAPI_STATE_INCALL)) {
			capi_sendf(NULL, 0, CAPI_CONNECT_RESP, i->PLCI, i->MessageNumber,
				"w()()()()()", 4);
			ret = 0;
		}
		if ((i->isdnstate & CAPI_ISDN_STATE_HOLD))
			pbx_capi_retrieve(c, NULL);
		break;
	case AST_CONTROL_PROGRESS:
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested PROGRESS-Indication for %s\n",
			i->vname, c->name);
		if (i->ntmode) pbx_capi_signal_progress(c, NULL);
		break;
	case AST_CONTROL_PROCEEDING:
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested PROCEEDING-Indication for %s\n",
			i->vname, c->name);
		if (i->ntmode) pbx_capi_signal_progress(c, NULL);
		break;
	case AST_CONTROL_HOLD:
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested HOLD-Indication for %s\n",
			i->vname, c->name);
		if (i->doholdtype != CC_HOLDTYPE_LOCAL) {
			ret = pbx_capi_hold(c, NULL);
		}
#ifdef CC_AST_HAS_VERSION_1_4
		else {
			ast_moh_start(c, data, i->mohinterpret);
		}
#endif
		break;
	case AST_CONTROL_UNHOLD:
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested UNHOLD-Indication for %s\n",
			i->vname, c->name);
		if (i->doholdtype != CC_HOLDTYPE_LOCAL) {
			ret = pbx_capi_retrieve(c, NULL);
		}
#ifdef CC_AST_HAS_VERSION_1_4
		else {
			ast_moh_stop(c);
		}
#endif
		break;
	case -1: /* stop indications */
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested Indication-STOP for %s\n",
			i->vname, c->name);
		if ((i->isdnstate & CAPI_ISDN_STATE_HOLD))
			pbx_capi_retrieve(c, NULL);
		break;
	default:
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested unknown Indication %d for %s\n",
			i->vname, condition, c->name);
		break;
	}
	cc_mutex_unlock(&i->lock);
	return(ret);
}

/*
 * PBX wants to know the state for a specific device
 */
static int pbx_capi_devicestate(void *data)
{
	int res = AST_DEVICE_UNKNOWN;

	if (!data) {
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "No data for capi_devicestate\n");
		return res;
	}

	cc_verbose(3, 1, VERBOSE_PREFIX_4 "CAPI devicestate requested for %s\n",
		(char *)data);

	return res;
}

static void capi_do_interface_task(void)
{
	if (interface_for_task == NULL)
		return;

	switch (interface_task) {
	case CAPI_INTERFACE_TASK_NULLIFREMOVE:
		/* remove an old null-plci interface */
		capi_remove_nullif(interface_for_task);
		break;
	default:
		/* nothing to do */
		break;
	}

	interface_for_task = NULL;
	interface_task = CAPI_INTERFACE_TASK_NONE;
}

static void capi_do_channel_task(void)
{
	if (chan_for_task == NULL)
		return;

	switch (channel_task) {
	case CAPI_CHANNEL_TASK_HANGUP:
		/* deferred (out of lock) hangup */
		ast_hangup(chan_for_task);
		break;
	case CAPI_CHANNEL_TASK_SOFTHANGUP:
		/* deferred (out of lock) soft-hangup */
		ast_softhangup(chan_for_task, AST_SOFTHANGUP_DEV);
		break;
	case CAPI_CHANNEL_TASK_PICKUP:
		if (ast_pickup_call(chan_for_task)) {
			cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Pickup not possible.\n",
				chan_for_task->name);
		}
		ast_hangup(chan_for_task);
		break;
	case CAPI_CHANNEL_TASK_GOTOFAX:
		/* deferred (out of lock) async goto fax extension */
		/* Save the DID/DNIS when we transfer the fax call to a "fax" extension */
		pbx_builtin_setvar_helper(chan_for_task, "FAXEXTEN", chan_for_task->exten);
	
		if (ast_async_goto(chan_for_task, chan_for_task->context, "fax", 1)) {
			cc_log(LOG_WARNING, "Failed to async goto '%s' into fax of '%s'\n",
				chan_for_task->name, chan_for_task->context);
		}
		break;
	default:
		/* nothing to do */
		break;
	}
	chan_for_task = NULL;
	channel_task = CAPI_CHANNEL_TASK_NONE;
}

/*
 * check for tasks every second
 */
static void capidev_run_secondly(time_t now)
{
	struct capi_pvt *i;

	/* check for channels to hangup (timeout) */
	cc_mutex_lock(&iflock);
	for (i = capi_iflist; i; i = i->next) {
		if (i->used == NULL) {
			continue;
		}
		if ((i->whentohangup) && (i->whentohangup < now)) {
			cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: stay-online timeout, hanging up.\n",
				i->vname);
			i->whentohangup = 0;
			capi_disconnect(i);
		}
		if ((i->whentoqueuehangup) && (i->whentoqueuehangup < now)) {
			cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: stay-online queue-hangup.\n",
				i->vname);
			capi_queue_cause_control(i, 1);
			i->whentoqueuehangup = 0;
		}
	}
	cc_mutex_unlock(&iflock);
}

/*
 * module stuff, monitor...
 */
static void *capidev_loop(void *data)
{
	unsigned int Info;
	_cmsg monCMSG;
	time_t lastcall = 0;
	time_t newtime;
	
	cc_log(LOG_NOTICE, "Started CAPI monitor-thread.\n");

	for (/* for ever */;;) {
		switch(Info = capidev_check_wait_get_cmsg(&monCMSG)) {
		case 0x0000:
			capidev_handle_msg(&monCMSG);
			capi_do_channel_task();
			capi_do_interface_task();
			break;
		case 0x1104:
			/* CAPI queue is empty */
			break;
		case 0x1101:
			/* The application ID is no longer valid.
			 * This error is fatal, and "chan_capi" 
			 * should restart.
			 */
			cc_log(LOG_ERROR, "CAPI reports application ID no longer valid, PANIC\n");
			return NULL;
		default:
			/* something is wrong! */
			break;
		} /* switch */
		newtime = time(NULL);
		if (lastcall != newtime) {
			lastcall = newtime;
			capidev_run_secondly(newtime);
		}
	} /* for */
	
	/* never reached */
	return NULL;
}

/*
 * GAIN
 */
void capi_gains(struct cc_capi_gains *g, float rxgain, float txgain)
{
	int i = 0;
	int x = 0;
	
	if (rxgain != 1.0) {
		for (i = 0; i < 256; i++) {
			if (capi_capability == AST_FORMAT_ULAW) {
				x = (int)(((float)capiULAW2INT[i]) * rxgain);
			} else {
				x = (int)(((float)capiALAW2INT[i]) * rxgain);
			}
			if (x > 32767)
				x = 32767;
			if (x < -32767)
				x = -32767;
			if (capi_capability == AST_FORMAT_ULAW) {
				g->rxgains[i] = capi_int2ulaw(x);
			} else {
				g->rxgains[i] = capi_int2alaw(x);
			}
		}
	}
	
	if (txgain != 1.0) {
		for (i = 0; i < 256; i++) {
			if (capi_capability == AST_FORMAT_ULAW) {
				x = (int)(((float)capiULAW2INT[i]) * txgain);
			} else {
				x = (int)(((float)capiALAW2INT[i]) * txgain);
			}
			if (x > 32767)
				x = 32767;
			if (x < -32767)
				x = -32767;
			if (capi_capability == AST_FORMAT_ULAW) {
				g->txgains[i] = capi_int2ulaw(x);
			} else {
				g->txgains[i] = capi_int2alaw(x);
			}
		}
	}
}

/*
 * create new interface
 */
int mkif(struct cc_capi_conf *conf)
{
	struct capi_pvt *tmp;
	int i = 0;
	u_int16_t unit;

	for (i = 0; i <= conf->devices; i++) {
		tmp = malloc(sizeof(struct capi_pvt));
		if (!tmp) {
			return -1;
		}
		memset(tmp, 0, sizeof(struct capi_pvt));
	
		tmp->readerfd = -1;
		tmp->writerfd = -1;
		
		cc_mutex_init(&tmp->lock);
		ast_cond_init(&tmp->event_trigger, NULL);
	
		if (i == 0) {
			snprintf(tmp->name, sizeof(tmp->name) - 1, "%s-pseudo-D", conf->name);
			tmp->channeltype = CAPI_CHANNELTYPE_D;
		} else {
			cc_copy_string(tmp->name, conf->name, sizeof(tmp->name));
			tmp->channeltype = CAPI_CHANNELTYPE_B;
		}
		snprintf(tmp->vname, sizeof(tmp->vname) - 1, "%s#%02d", conf->name, i);
		cc_copy_string(tmp->context, conf->context, sizeof(tmp->context));
		cc_copy_string(tmp->incomingmsn, conf->incomingmsn, sizeof(tmp->incomingmsn));
		cc_copy_string(tmp->defaultcid, conf->defaultcid, sizeof(tmp->defaultcid));
		cc_copy_string(tmp->prefix, conf->prefix, sizeof(tmp->prefix));
		cc_copy_string(tmp->accountcode, conf->accountcode, sizeof(tmp->accountcode));
		cc_copy_string(tmp->language, conf->language, sizeof(tmp->language));
#ifdef CC_AST_HAS_VERSION_1_4
		cc_copy_string(tmp->mohinterpret, conf->mohinterpret, sizeof(tmp->mohinterpret));
		memcpy(&tmp->jbconf, &conf->jbconf, sizeof(struct ast_jb_conf));
#endif

		unit = atoi(conf->controllerstr);
			/* There is no reason not to
			 * allow controller 0 !
			 *
			 * Hide problem from user:
			 */
			if (unit == 0) {
				/* The ISDN4BSD kernel will modulo
				 * the controller number by 
				 * "capi_num_controllers", so this
				 * is equivalent to "0":
				 */
				unit = capi_num_controllers;
			}

		if (unit > capi_num_controllers){
			free(tmp);
			cc_verbose(2, 0, VERBOSE_PREFIX_3 "controller %d invalid, ignoring interface.\n",
				unit);
			return 0;
		}

		/* always range check user input */
		if (unit > CAPI_MAX_CONTROLLERS)
			unit = CAPI_MAX_CONTROLLERS;

		tmp->controller = unit;
		capi_used_controllers |= (1 << unit);
		tmp->doEC = conf->echocancel;
		tmp->doEC_global = conf->echocancel;
		tmp->ecOption = conf->ecoption;
		if (conf->ecnlp) tmp->ecOption |= 0x01; /* bit 0 of ec-option is NLP */
		tmp->ecTail = conf->ectail;
		tmp->isdnmode = conf->isdnmode;
		tmp->ntmode = conf->ntmode;
		tmp->ES = conf->es;
		tmp->callgroup = conf->callgroup;
		tmp->pickupgroup = conf->pickupgroup;
		tmp->group = conf->group;
		tmp->transfergroup = conf->transfergroup;
		tmp->amaflags = conf->amaflags;
		tmp->immediate = conf->immediate;
		tmp->holdtype = conf->holdtype;
		tmp->ecSelector = conf->ecSelector;
		tmp->bridge = conf->bridge;
		tmp->FaxState = conf->faxsetting;
		
		tmp->smoother = ast_smoother_new(CAPI_MAX_B3_BLOCK_SIZE);

		tmp->rxgain = conf->rxgain;
		tmp->txgain = conf->txgain;
		capi_gains(&tmp->g, conf->rxgain, conf->txgain);

		tmp->doDTMF = conf->softdtmf;
		tmp->capability = conf->capability;

		/* Initialize QSIG code */
		cc_qsig_interface_init(conf, tmp);
		
		tmp->next = capi_iflist; /* prepend */
		capi_iflist = tmp;
		cc_verbose(2, 0, VERBOSE_PREFIX_3 "capi %c %s (%s:%s) contr=%d devs=%d EC=%d,opt=%d,tail=%d\n",
			(tmp->channeltype == CAPI_CHANNELTYPE_B)? 'B' : 'D',
			tmp->vname, tmp->incomingmsn, tmp->context, tmp->controller,
			conf->devices, tmp->doEC, tmp->ecOption, tmp->ecTail);
	}
	return 0;
}

/*
 * eval supported services
 */
static void supported_sservices(struct cc_capi_controller *cp)
{
	MESSAGE_EXCHANGE_ERROR error;
	_cmsg CMSG2;
	struct timeval tv;
	unsigned int services;

	capi_sendf(NULL, 0, CAPI_FACILITY_REQ, cp->controller, get_capi_MessageNumber(),
		"w(w())",
		FACILITYSELECTOR_SUPPLEMENTARY,
		0x0000  /* get supported services */
	);
	
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	
	for (/* for ever */;;) {
		error = capi20_waitformessage(capi_ApplID, &tv);
		error = capi_get_cmsg(&CMSG2, capi_ApplID); 
		if (error == 0) {
			if (IS_FACILITY_CONF(&CMSG2)) {
				cc_verbose(5, 0, VERBOSE_PREFIX_4 "FACILITY_CONF INFO = %#x\n",
					FACILITY_CONF_INFO(&CMSG2));
				break;
			}
		}
	} 

	/* parse supported sservices */
	if (FACILITY_CONF_FACILITYSELECTOR(&CMSG2) != FACILITYSELECTOR_SUPPLEMENTARY) {
		cc_log(LOG_NOTICE, "unexpected FACILITY_SELECTOR = %#x\n",
			FACILITY_CONF_FACILITYSELECTOR(&CMSG2));
		return;
	}

	if (FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(&CMSG2)[4] != 0) {
		cc_log(LOG_NOTICE, "supplementary services info  = %#x\n",
			(short)FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(&CMSG2)[1]);
		return;
	}
	services = read_capi_dword(&(FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(&CMSG2)[6]));
	cc_verbose(3, 0, VERBOSE_PREFIX_4 "supplementary services : 0x%08x\n",
		services);
	
	/* success, so set the features we have */
	cc_verbose(3, 0, VERBOSE_PREFIX_4 " ");
	if (services & 0x0001) {
		cp->holdretrieve = 1;
		cc_verbose(3, 0, "HOLD/RETRIEVE ");
	}
	if (services & 0x0002) {
		cp->terminalportability = 1;
		cc_verbose(3, 0, "TERMINAL-PORTABILITY ");
	}
	if (services & 0x0004) {
		cp->ECT = 1;
		cc_verbose(3, 0, "ECT ");
	}
	if (services & 0x0008) {
		cp->threePTY = 1;
		cc_verbose(3, 0, "3PTY ");
	}
	if (services & 0x0010) {
		cp->CF = 1;
		cc_verbose(3, 0, "CF ");
	}
	if (services & 0x0020) {
		cp->CD = 1;
		cc_verbose(3, 0, "CD ");
	}
	if (services & 0x0040) {
		cp->MCID = 1;
		cc_verbose(3, 0, "MCID ");
	}
	if (services & 0x0080) {
		cp->CCBS = 1;
		cc_verbose(3, 0, "CCBS ");
	}
	if (services & 0x0100) {
		cp->MWI = 1;
		cc_verbose(3, 0, "MWI ");
	}
	if (services & 0x0200) {
		cp->CCNR = 1;
		cc_verbose(3, 0, "CCNR ");
	}
	if (services & 0x0400) {
		cp->CONF = 1;
		cc_verbose(3, 0, "CONF");
	}
	cc_verbose(3, 0, "\n");
	return;
}

/*
 * helper functions to convert conf value to string
 */
static char *show_bproto(int bproto)
{
	switch(bproto) {
	case CC_BPROTO_TRANSPARENT:
		return "trans";
	case CC_BPROTO_FAXG3:
		return " fax ";
	case CC_BPROTO_RTP:
		return " rtp ";
	}
	return " ??? ";
}
static char *show_state(int state)
{
	switch(state) {
	case CAPI_STATE_ALERTING:
		return "Ring ";
	case CAPI_STATE_CONNECTED:
		return "Conn ";
	case CAPI_STATE_DISCONNECTING:
		return "discP";
	case CAPI_STATE_DISCONNECTED:
		return "Disc ";
	case CAPI_STATE_CONNECTPENDING:
		return "Dial ";
	case CAPI_STATE_ANSWERING:
		return "Answ ";
	case CAPI_STATE_DID:
		return "DIDin";
	case CAPI_STATE_INCALL:
		return "icall";
	case CAPI_STATE_ONHOLD:
		return "Hold ";
	}
	return "-----";
}
static char *show_isdnstate(unsigned int isdnstate, char *str)
{
	str[0] = '\0';

	if (isdnstate & CAPI_ISDN_STATE_PBX)
		strcat(str, "*");
	if (isdnstate & CAPI_ISDN_STATE_LI)
		strcat(str, "G");
	if (isdnstate & CAPI_ISDN_STATE_B3_UP)
		strcat(str, "B");
	if (isdnstate & CAPI_ISDN_STATE_B3_PEND)
		strcat(str, "b");
	if (isdnstate & CAPI_ISDN_STATE_PROGRESS)
		strcat(str, "P");
	if (isdnstate & CAPI_ISDN_STATE_HOLD)
		strcat(str, "H");
	if (isdnstate & CAPI_ISDN_STATE_ECT)
		strcat(str, "T");
	if (isdnstate & CAPI_ISDN_STATE_3PTY)
	        strcat(str, "3");
	if (isdnstate & (CAPI_ISDN_STATE_SETUP | CAPI_ISDN_STATE_SETUP_ACK))
		strcat(str, "S");

	return str;
}

/*
 * do command capi show channels
 */
static int pbxcli_capi_show_channels(int fd, int argc, char *argv[])
{
	struct capi_pvt *i;
	char iochar;
	char i_state[80];
	char b3q[32];
	
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	
	ast_cli(fd, "CAPI B-channel information:\n");
	ast_cli(fd, "Line-Name       NTmode state i/o bproto isdnstate   ton  number\n");
	ast_cli(fd, "----------------------------------------------------------------\n");

	cc_mutex_lock(&iflock);

	for (i = capi_iflist; i; i = i->next) {
		if (i->channeltype != CAPI_CHANNELTYPE_B)
			continue;

		if ((i->state == 0) || (i->state == CAPI_STATE_DISCONNECTED))
			iochar = '-';
		else if (i->outgoing)
			iochar = 'O';
		else
			iochar = 'I';

		if (capidebug) {
			snprintf(b3q, sizeof(b3q), "  B3q=%d B3count=%d",
				i->B3q, i->B3count);
		} else {
			b3q[0] = '\0';
		}

		ast_cli(fd,
			"%-16s %s   %s  %c  %s  %-10s  0x%02x '%s'->'%s'%s\n",
			i->vname,
			i->ntmode ? "yes":"no ",
			show_state(i->state),
			iochar,
			show_bproto(i->bproto),
			show_isdnstate(i->isdnstate, i_state),
			i->cid_ton,
			i->cid,
			i->dnid,
			b3q
		);
	}

	cc_mutex_unlock(&iflock);
		
	return RESULT_SUCCESS;
}

/*
 * do command capi info
 */
static int pbxcli_capi_info(int fd, int argc, char *argv[])
{
	int i = 0;
	
	if (argc != 2)
		return RESULT_SHOWUSAGE;
	
	ast_cli(fd, "%s www.chan-capi.org\n", tdesc);
		
	for (i = 1; i <= capi_num_controllers; i++) {
		if (capi_controllers[i] != NULL) {
			ast_cli(fd, "Contr%d: %d B channels total, %d B channels free.\n",
				i, capi_controllers[i]->nbchannels,
				capi_controllers[i]->nfreebchannels);
		}
	}
	return RESULT_SUCCESS;
}

/*
 * enable debugging
 */
static int pbxcli_capi_do_debug(int fd, int argc, char *argv[])
{
	if (argc != 2)
		return RESULT_SHOWUSAGE;
		
	capidebug = 1;
	ast_cli(fd, "CAPI Debugging Enabled\n");
	
	return RESULT_SUCCESS;
}

/*
 * disable debugging
 */
static int pbxcli_capi_no_debug(int fd, int argc, char *argv[])
{
	if (argc != 3)
		return RESULT_SHOWUSAGE;

	capidebug = 0;
	ast_cli(fd, "CAPI Debugging Disabled\n");
	
	return RESULT_SUCCESS;
}

/*
 * usages
 */
static char info_usage[] = 
"Usage: capi info\n"
"       Show info about B channels on controllers.\n";

static char show_channels_usage[] = 
"Usage: capi show channels\n"
"       Show info about B channels.\n";

static char debug_usage[] = 
"Usage: capi debug\n"
"       Enables dumping of CAPI packets for debugging purposes\n";

static char no_debug_usage[] = 
"Usage: capi no debug\n"
"       Disables dumping of CAPI packets for debugging purposes\n";

static char chatinfo_usage[] = 
"Usage: capi chatinfo\n"
"       Show info about chat status.\n";

/*
 * define commands
 */
static struct ast_cli_entry  cli_info =
	{ { "capi", "info", NULL }, pbxcli_capi_info, "Show CAPI info", info_usage };
static struct ast_cli_entry  cli_show_channels =
	{ { "capi", "show", "channels", NULL }, pbxcli_capi_show_channels, "Show B-channel info", show_channels_usage };
static struct ast_cli_entry  cli_debug =
	{ { "capi", "debug", NULL }, pbxcli_capi_do_debug, "Enable CAPI debugging", debug_usage };
static struct ast_cli_entry  cli_no_debug =
	{ { "capi", "no", "debug", NULL }, pbxcli_capi_no_debug, "Disable CAPI debugging", no_debug_usage };
static struct ast_cli_entry  cli_chatinfo =
	{ { "capi", "chatinfo", NULL }, pbxcli_capi_chatinfo, "Show CAPI chat info", chatinfo_usage };

const struct ast_channel_tech capi_tech = {
	.type = channeltype,
	.description = tdesc,
	.capabilities = AST_FORMAT_ALAW,
	.requester = pbx_capi_request,
#ifdef CC_AST_HAS_VERSION_1_4
	.send_digit_begin = pbx_capi_send_digit_begin,
	.send_digit_end = pbx_capi_send_digit,
#else
	.send_digit = pbx_capi_send_digit,
#endif
	.send_text = pbx_capi_qsig_sendtext,
	.call = pbx_capi_call,
	.hangup = pbx_capi_hangup,
	.answer = pbx_capi_answer,
	.read = pbx_capi_read,
	.write = pbx_capi_write,
	.bridge = pbx_capi_bridge,
	.exception = NULL,
	.indicate = pbx_capi_indicate,
	.fixup = pbx_capi_fixup,
	.setoption = NULL,
	.devicestate = pbx_capi_devicestate,
};

/*
 * register at CAPI interface
 */
static int cc_register_capi(unsigned blocksize, unsigned connections)
{
	u_int16_t error = 0;
	unsigned capi_ApplID_old = capi_ApplID;

	cc_verbose(3, 0, VERBOSE_PREFIX_3 "Registering at CAPI "
		   "(blocksize=%d maxlogicalchannels=%d)\n", blocksize, connections);

#if (CAPI_OS_HINT == 2)
	error = capi20_register(connections, CAPI_MAX_B3_BLOCKS, 
				blocksize, &capi_ApplID, CAPI_STACK_VERSION);
#else
	error = capi20_register(connections, CAPI_MAX_B3_BLOCKS, 
				blocksize, &capi_ApplID);
#endif
	if (capi_ApplID_old != CAPI_APPLID_UNUSED) {
		if (capi20_release(capi_ApplID_old) != 0)
			cc_log(LOG_WARNING,"Unable to unregister from CAPI!\n");
	}
	if (error != 0) {
		capi_ApplID = CAPI_APPLID_UNUSED;
		cc_log(LOG_NOTICE,"unable to register application at CAPI!\n");
		return -1;
	}
	return 0;
}

/*
 * init capi stuff
 */
static int cc_init_capi(void)
{
#if (CAPI_OS_HINT == 1)
	CAPIProfileBuffer_t profile;
#else
	struct cc_capi_profile profile;
#endif
	struct cc_capi_controller *cp;
	int controller;
	unsigned int privateoptions;

	if (capi20_isinstalled() != 0) {
		cc_log(LOG_WARNING, "CAPI not installed, CAPI disabled!\n");
		return -1;
	}

	if (cc_register_capi(CAPI_MAX_B3_BLOCK_SIZE, 2))
		return -1;

#if (CAPI_OS_HINT == 1)
	if (capi20_get_profile(0, &profile) != 0) {
#elif (CAPI_OS_HINT == 2)
	if (capi20_get_profile(0, &profile, sizeof(profile)) != 0) {
#else
	if (capi20_get_profile(0, (unsigned char *)&profile) != 0) {
#endif
		cc_log(LOG_NOTICE,"unable to get CAPI profile!\n");
		return -1;
	} 

#if (CAPI_OS_HINT == 1)
	capi_num_controllers = profile.wCtlr;
#else
	capi_num_controllers = profile.ncontrollers;
#endif

	cc_verbose(3, 0, VERBOSE_PREFIX_2 "This box has %d capi controller(s).\n",
		capi_num_controllers);
	
	for (controller = 1 ;controller <= capi_num_controllers; controller++) {

		memset(&profile, 0, sizeof(profile));
#if (CAPI_OS_HINT == 1)
		capi20_get_profile(controller, &profile);
#elif (CAPI_OS_HINT == 2)
		capi20_get_profile(controller, &profile, sizeof(profile));
#else
		capi20_get_profile(controller, (unsigned char *)&profile);
#endif
		cp = malloc(sizeof(struct cc_capi_controller));
		if (!cp) {
			cc_log(LOG_ERROR, "Error allocating memory for struct cc_capi_controller\n");
			return -1;
		}
		memset(cp, 0, sizeof(struct cc_capi_controller));
		cp->controller = controller;
#if (CAPI_OS_HINT == 1)
		cp->nbchannels = profile.wNumBChannels;
		cp->nfreebchannels = profile.wNumBChannels;
		if (profile.dwGlobalOptions & CAPI_PROFILE_DTMF_SUPPORT) {
#else
		cp->nbchannels = profile.nbchannels;
		cp->nfreebchannels = profile.nbchannels;
		if (profile.globaloptions & 0x08) {
#endif
			cc_verbose(3, 0, VERBOSE_PREFIX_3 "CAPI/contr%d supports DTMF\n",
				controller);
			cp->dtmf = 1;
		}
		
	
#if (CAPI_OS_HINT == 1)
		if (profile.dwGlobalOptions & 0x01) {
#else
		if (profile.globaloptions2 & 0x01) {
#endif
			cc_verbose(3, 0, VERBOSE_PREFIX_3 "CAPI/contr%d supports broadband (or old echo-cancel)\n",
				controller);
			cp->broadband = 1;
		}

#if (CAPI_OS_HINT == 1)
		if (profile.dwGlobalOptions & CAPI_PROFILE_ECHO_CANCELLATION) {
#else
		if (profile.globaloptions2 & 0x02) {
#endif
			cc_verbose(3, 0, VERBOSE_PREFIX_3 "CAPI/contr%d supports echo cancellation\n",
				controller);
			cp->echocancel = 1;
		}
		
#if (CAPI_OS_HINT == 1)
		if (profile.dwGlobalOptions & CAPI_PROFILE_SUPPLEMENTARY_SERVICES)  {
#else
		if (profile.globaloptions & 0x10) {
#endif
			cp->sservices = 1;
		}

#if (CAPI_OS_HINT == 1)
		if (profile.dwGlobalOptions & 0x80)  {
#else
		if (profile.globaloptions & 0x80) {
#endif
			cc_verbose(3, 0, VERBOSE_PREFIX_3 "CAPI/contr%d supports line interconnect\n",
				controller);
			cp->lineinterconnect = 1;
		}
		
		if (cp->sservices == 1) {
			cc_verbose(3, 0, VERBOSE_PREFIX_3 "CAPI/contr%d supports supplementary services\n",
				controller);
			supported_sservices(cp);
		}

		/* New profile options for e.g. RTP with Eicon DIVA */
		privateoptions = read_capi_dword(&profile.manufacturer[0]);
		cc_verbose(3, 0, VERBOSE_PREFIX_3 "CAPI/contr%d private options=0x%08x\n",
			controller, privateoptions);
		if (privateoptions & 0x02) {
			cc_verbose(3, 0, VERBOSE_PREFIX_4 "VoIP/RTP is supported\n");
			voice_over_ip_profile(cp);
		}
		if (privateoptions & 0x04) {
			cc_verbose(3, 0, VERBOSE_PREFIX_4 "T.38 is supported (not implemented yet)\n");
		}

		capi_controllers[controller] = cp;
	}
	return 0;
}

/*
 * final capi init
 */
static int cc_post_init_capi(void)
{
	struct capi_pvt *i;
	int controller;
	unsigned error;
	int rtp_ext_size = 0;
	unsigned needchannels = 0;

	for (i = capi_iflist; i && !rtp_ext_size; i = i->next) {
		/* if at least one line wants RTP, we need to re-register with
		   bigger block size for RTP-header */
		if (capi_controllers[i->controller]->rtpcodec & i->capability) {
			cc_verbose(3, 0, VERBOSE_PREFIX_4 "at least one CAPI controller wants RTP.\n");
			rtp_ext_size = RTP_HEADER_SIZE;
		}
	}
	for (controller = 1; controller <= capi_num_controllers; controller++) {
		if (capi_controllers[controller] != NULL) {
			needchannels += (capi_controllers[controller]->nbchannels + 1);
		}
	}
	if (cc_register_capi(CAPI_MAX_B3_BLOCK_SIZE + rtp_ext_size, needchannels))
		return -1;

	for (controller = 1; controller <= capi_num_controllers; controller++) {
		if (capi_used_controllers & (1 << controller)) {
			if ((error = capi_ListenOnController(ALL_SERVICES, controller)) != 0) {
				cc_log(LOG_ERROR,"Unable to listen on contr%d (error=0x%x)\n",
					controller, error);
			} else {
				cc_verbose(2, 0, VERBOSE_PREFIX_3 "listening on contr%d CIPmask = %#x\n",
					controller, ALL_SERVICES);
			}
		} else {
			cc_log(LOG_NOTICE, "Unused contr%d\n",controller);
		}
	}

	return 0;
}

/*
 * build the interface according to configs
 */
static int conf_interface(struct cc_capi_conf *conf, struct ast_variable *v)
{
	int y;

#define CONF_STRING(var, token)            \
	if (!strcasecmp(v->name, token)) { \
		cc_copy_string(var, v->value, sizeof(var)); \
		continue;                  \
	} else
#define CONF_INTEGER(var, token)           \
	if (!strcasecmp(v->name, token)) { \
		var = atoi(v->value);      \
		continue;                  \
	} else
#define CONF_TRUE(var, token, val)         \
	if (!strcasecmp(v->name, token)) { \
		if (ast_true(v->value))    \
			var = val;         \
		continue;                  \
	} else

	for (; v; v = v->next) {
#ifdef CC_AST_HAS_VERSION_1_4
		/* handle jb conf */
		if (!ast_jb_read_conf(&conf->jbconf, v->name, v->value)) {
			continue;
		}
		CONF_STRING(conf->mohinterpret, "mohinterpret")
#endif
		CONF_INTEGER(conf->devices, "devices")
		CONF_STRING(conf->context, "context")
		CONF_STRING(conf->incomingmsn, "incomingmsn")
		CONF_STRING(conf->defaultcid, "defaultcid")
		CONF_STRING(conf->controllerstr, "controller")
		CONF_STRING(conf->prefix, "prefix")
		CONF_STRING(conf->accountcode, "accountcode")
		CONF_STRING(conf->language, "language")

		if (!strcasecmp(v->name, "softdtmf")) {
			if ((!conf->softdtmf) && (ast_true(v->value))) {
				conf->softdtmf = 1;
			}
			continue;
		} else
		CONF_TRUE(conf->softdtmf, "relaxdtmf", 2)
		if (!strcasecmp(v->name, "holdtype")) {
			if (!strcasecmp(v->value, "hold")) {
				conf->holdtype = CC_HOLDTYPE_HOLD;
			} else if (!strcasecmp(v->value, "notify")) {
				conf->holdtype = CC_HOLDTYPE_NOTIFY;
			} else {
				conf->holdtype = CC_HOLDTYPE_LOCAL;
			}
			continue;
		} else
		CONF_TRUE(conf->immediate, "immediate", 1)
		CONF_TRUE(conf->es, "echosquelch", 1)
		CONF_TRUE(conf->bridge, "bridge", 1)
		CONF_TRUE(conf->ntmode, "ntmode", 1)
		if (!strcasecmp(v->name, "callgroup")) {
			conf->callgroup = ast_get_group(v->value);
			continue;
		} else
		if (!strcasecmp(v->name, "pickupgroup")) {
			conf->pickupgroup = ast_get_group(v->value);
			continue;
		} else
		if (!strcasecmp(v->name, "group")) {
			conf->group = ast_get_group(v->value);
			continue;
		} else
		if (!strcasecmp(v->name, "transfergroup")) {
			conf->transfergroup = ast_get_group(v->value);
			continue;
		} else
		if (!strcasecmp(v->name, "amaflags")) {
			y = ast_cdr_amaflags2int(v->value);
			if (y < 0) {
				ast_log(LOG_WARNING, "Invalid AMA flags: %s at line %d\n",
					v->value, v->lineno);
			} else {
				conf->amaflags = y;
			}
		} else
		if (!strcasecmp(v->name, "rxgain")) {
			if (sscanf(v->value, "%f", &conf->rxgain) != 1) {
				cc_log(LOG_ERROR,"invalid rxgain\n");
			}
			continue;
		} else
		if (!strcasecmp(v->name, "txgain")) {
			if (sscanf(v->value, "%f", &conf->txgain) != 1) {
				cc_log(LOG_ERROR, "invalid txgain\n");
			}
			continue;
		} else
		if (!strcasecmp(v->name, "echocancelold")) {
			if (ast_true(v->value)) {
				conf->ecSelector = 6;
			}
			continue;
		} else
		if (!strcasecmp(v->name, "faxdetect")) {
			if (!strcasecmp(v->value, "incoming")) {
				conf->faxsetting |= CAPI_FAX_DETECT_INCOMING;
				conf->faxsetting &= ~CAPI_FAX_DETECT_OUTGOING;
			} else if (!strcasecmp(v->value, "outgoing")) {
				conf->faxsetting |= CAPI_FAX_DETECT_OUTGOING;
				conf->faxsetting &= ~CAPI_FAX_DETECT_INCOMING;
			} else if (!strcasecmp(v->value, "both") || ast_true(v->value))
				conf->faxsetting |= (CAPI_FAX_DETECT_OUTGOING | CAPI_FAX_DETECT_INCOMING);
			else
				conf->faxsetting &= ~(CAPI_FAX_DETECT_OUTGOING | CAPI_FAX_DETECT_INCOMING);
		} else
		if (!strcasecmp(v->name, "echocancel")) {
			if (ast_true(v->value)) {
				conf->echocancel = 1;
				conf->ecoption = EC_OPTION_DISABLE_G165;
			}	
			else if (ast_false(v->value)) {
				conf->echocancel = 0;
				conf->ecoption = 0;
			}	
			else if (!strcasecmp(v->value, "g165") || !strcasecmp(v->value, "g.165")) {
				conf->echocancel = 1;
				conf->ecoption = EC_OPTION_DISABLE_G165;
			}	
			else if (!strcasecmp(v->value, "g164") || !strcasecmp(v->value, "g.164")) {
				conf->echocancel = 1;
				conf->ecoption = EC_OPTION_DISABLE_G164_OR_G165;
			}	
			else if (!strcasecmp(v->value, "force")) {
				conf->echocancel = 1;
				conf->ecoption = EC_OPTION_DISABLE_NEVER;
			}
			else {
				cc_log(LOG_ERROR,"Unknown echocancel parameter \"%s\" -- ignoring\n",v->value);
			}
			continue;
		} else
		CONF_TRUE(conf->ecnlp, "echocancelnlp", 1)
		if (!strcasecmp(v->name, "echotail")) {
			conf->ectail = atoi(v->value);
			if (conf->ectail > 255) {
				conf->ectail = 255;
			} 
			continue;
		} else
		if (!strcasecmp(v->name, "isdnmode")) {
			if (!strcasecmp(v->value, "did"))
			    conf->isdnmode = CAPI_ISDNMODE_DID;
			else if (!strcasecmp(v->value, "msn"))
			    conf->isdnmode = CAPI_ISDNMODE_MSN;
			else
			    cc_log(LOG_ERROR,"Unknown isdnmode parameter \"%s\" -- ignoring\n",
			    	v->value);
		} else
		if (!strcasecmp(v->name, "allow")) {
			ast_parse_allow_disallow(&conf->prefs, &conf->capability, v->value, 1);
		} else
		if (!strcasecmp(v->name, "disallow")) {
			ast_parse_allow_disallow(&conf->prefs, &conf->capability, v->value, 0);
		}
		cc_pbx_qsig_conf_interface_value(conf, v);
	}
#undef CONF_STRING
#undef CONF_INTEGER
#undef CONF_TRUE
	return 0;
}

/*
 * load the config
 */
static int capi_eval_config(struct ast_config *cfg)
{
	struct cc_capi_conf conf;
	struct ast_variable *v;
	char *cat = NULL;
	float rxgain = 1.0;
	float txgain = 1.0;

	/* prefix defaults */
	cc_copy_string(capi_national_prefix, CAPI_NATIONAL_PREF, sizeof(capi_national_prefix));
	cc_copy_string(capi_international_prefix, CAPI_INTERNAT_PREF, sizeof(capi_international_prefix));

#ifdef CC_AST_HAS_VERSION_1_4
	/* Copy the default jb config over global_jbconf */
	memcpy(&global_jbconf, &default_jbconf, sizeof(struct ast_jb_conf));
#endif

	/* read the general section */
	for (v = ast_variable_browse(cfg, "general"); v; v = v->next) {
#ifdef CC_AST_HAS_VERSION_1_4
		/* handle global jb conf */
		if (!ast_jb_read_conf(&global_jbconf, v->name, v->value)) {
			continue;
		}
		if (!strcasecmp(v->name, "mohinterpret")) {
			cc_copy_string(global_mohinterpret, v->value, sizeof(global_mohinterpret));
		} else
#endif
		if (!strcasecmp(v->name, "nationalprefix")) {
			cc_copy_string(capi_national_prefix, v->value, sizeof(capi_national_prefix));
		} else if (!strcasecmp(v->name, "internationalprefix")) {
			cc_copy_string(capi_international_prefix, v->value, sizeof(capi_international_prefix));
		} else if (!strcasecmp(v->name, "language")) {
			cc_copy_string(default_language, v->value, sizeof(default_language));
		} else if (!strcasecmp(v->name, "rxgain")) {
			if (sscanf(v->value,"%f",&rxgain) != 1) {
				cc_log(LOG_ERROR,"invalid rxgain\n");
			}
		} else if (!strcasecmp(v->name, "txgain")) {
			if (sscanf(v->value,"%f",&txgain) != 1) {
				cc_log(LOG_ERROR,"invalid txgain\n");
			}
		} else if (!strcasecmp(v->name, "ulaw")) {
			if (ast_true(v->value)) {
				capi_capability = AST_FORMAT_ULAW;
			}
		}
	}

	/* go through all other sections, which are our interfaces */
	for (cat = ast_category_browse(cfg, NULL); cat; cat = ast_category_browse(cfg, cat)) {
		if (!strcasecmp(cat, "general"))
			continue;
			
		if (!strcasecmp(cat, "interfaces")) {
			cc_log(LOG_WARNING, "Config file syntax has changed! Don't use 'interfaces'\n");
			return -1;
		}
		cc_verbose(4, 0, VERBOSE_PREFIX_2 "Reading config for %s\n",
			cat);
		
		/* init the conf struct */
		memset(&conf, 0, sizeof(conf));
		conf.rxgain = rxgain;
		conf.txgain = txgain;
		conf.ecoption = EC_OPTION_DISABLE_G165;
		conf.ectail = EC_DEFAULT_TAIL;
		conf.ecSelector = FACILITYSELECTOR_ECHO_CANCEL;
		cc_copy_string(conf.name, cat, sizeof(conf.name));
		cc_copy_string(conf.language, default_language, sizeof(conf.language));
#ifdef CC_AST_HAS_VERSION_1_4
		cc_copy_string(conf.mohinterpret, global_mohinterpret, sizeof(conf.mohinterpret));
		/* Copy the global jb config into interface conf */
		memcpy(&conf.jbconf, &global_jbconf, sizeof(struct ast_jb_conf));
#endif

		if (conf_interface(&conf, ast_variable_browse(cfg, cat))) {
			cc_log(LOG_ERROR, "Error interface config.\n");
			return -1;
		}

		if (mkif(&conf)) {
			cc_log(LOG_ERROR,"Error creating interface list\n");
			return -1;
		}
	}
	return 0;
}

#ifdef CC_AST_HAS_VERSION_1_4
static int reload(void)
{
	cc_log(LOG_WARNING, "config reload is not supported yet.\n");

	return 0;
}
#endif
/*
 * unload the module
 */
#ifdef CC_AST_HAS_VERSION_1_4
static
#endif
int unload_module(void)
{
	struct capi_pvt *i, *itmp;
	int controller;

	ast_unregister_application(commandapp);

	ast_cli_unregister(&cli_info);
	ast_cli_unregister(&cli_show_channels);
	ast_cli_unregister(&cli_debug);
	ast_cli_unregister(&cli_no_debug);
	ast_cli_unregister(&cli_chatinfo);

#ifdef CC_AST_HAS_VERSION_1_4
	ast_module_user_hangup_all();
#endif

	if (monitor_thread != (pthread_t)(0-1)) {
		pthread_cancel(monitor_thread);
		pthread_kill(monitor_thread, SIGURG);
		pthread_join(monitor_thread, NULL);
	}

	cc_mutex_lock(&iflock);

	if (capi_ApplID != CAPI_APPLID_UNUSED) {
		if (capi20_release(capi_ApplID) != 0)
			cc_log(LOG_WARNING,"Unable to unregister from CAPI!\n");
	}

	for (controller = 1; controller <= capi_num_controllers; controller++) {
		if (capi_used_controllers & (1 << controller)) {
			if (capi_controllers[controller])
				free(capi_controllers[controller]);
		}
	}
	
	i = capi_iflist;
	while (i) {
		if ((i->owner) || (i->used))
			cc_log(LOG_WARNING, "On unload, interface still has owner or is used.\n");
		if (i->smoother)
			ast_smoother_free(i->smoother);
		
		pbx_capi_qsig_unload_module(i);
		
		cc_mutex_destroy(&i->lock);
		ast_cond_destroy(&i->event_trigger);
		itmp = i;
		i = i->next;
		free(itmp);
	}

	cc_mutex_unlock(&iflock);
	
	ast_channel_unregister(&capi_tech);

	cleanup_ccbsnr();
	
	return 0;
}

/*
 * main: load the module
 */
#ifdef CC_AST_HAS_VERSION_1_4
static
#endif
int load_module(void)
{
	struct ast_config *cfg;
	char *config = "capi.conf";
	int res = 0;

	cfg = ast_config_load(config);

	/* We *must* have a config file otherwise stop immediately, well no */
	if (!cfg) {
		cc_log(LOG_ERROR, "Unable to load config %s, CAPI disabled\n", config);
		return 0;
	}

	if (cc_mutex_lock(&iflock)) {
		cc_log(LOG_ERROR, "Unable to lock interface list???\n");
		return -1;
	}

	if ((res = cc_init_capi()) != 0) {
		cc_mutex_unlock(&iflock);
		return(res);
	}

	res = capi_eval_config(cfg);
	ast_config_destroy(cfg);

	if (res != 0) {
		cc_mutex_unlock(&iflock);
		return(res);
	}

	if ((res = cc_post_init_capi()) != 0) {
		cc_mutex_unlock(&iflock);
		unload_module();
		return(res);
	}
	
	cc_mutex_unlock(&iflock);
	
	if (ast_channel_register(&capi_tech)) {
		cc_log(LOG_ERROR, "Unable to register channel class %s\n", channeltype);
		unload_module();
		return -1;
	}

	ast_cli_register(&cli_info);
	ast_cli_register(&cli_show_channels);
	ast_cli_register(&cli_debug);
	ast_cli_register(&cli_no_debug);
	ast_cli_register(&cli_chatinfo);
	
	ast_register_application(commandapp, pbx_capicommand_exec, commandsynopsis, commandtdesc);

	if (ast_pthread_create(&monitor_thread, NULL, capidev_loop, NULL) < 0) {
		monitor_thread = (pthread_t)(0-1);
		cc_log(LOG_ERROR, "Unable to start monitor thread!\n");
		return -1;
	}

	return 0;
}

#ifdef CC_AST_HAS_VERSION_1_4
AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, tdesc,
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
#else
int usecount()
{
	int res;
	
	cc_mutex_lock(&usecnt_lock);
	res = usecnt;
	cc_mutex_unlock(&usecnt_lock);

	return res;
}

char *description()
{
	return ccdesc;
}

char *key()
{
	return ASTERISK_GPL_KEY;
}
#endif /* CC_AST_HAS_VERSION_1_4 */
