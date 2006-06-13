/*
 * (CAPI*)
 *
 * An implementation of Common ISDN API 2.0 for
 * Asterisk / OpenPBX.org
 *
 * Copyright (C) 2005-2006 Cytronics & Melware
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
#ifdef PBX_IS_OPBX
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif
#endif

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

#ifdef PBX_IS_OPBX
#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/frame.h" 
#include "openpbx/channel.h"
#include "openpbx/logger.h"
#include "openpbx/module.h"
#include "openpbx/pbx.h"
#include "openpbx/config.h"
#include "openpbx/options.h"
#include "openpbx/features.h"
#include "openpbx/utils.h"
#include "openpbx/cli.h"
#include "openpbx/rtp.h"
#include "openpbx/causes.h"
#include "openpbx/strings.h"
#include "openpbx/devicestate.h"
#include "openpbx/dsp.h"
#include "openpbx/xlaw.h"
#include "openpbx/chan_capi20.h"
#include "openpbx/chan_capi.h"
#include "openpbx/chan_capi_rtp.h"
#else
#include "config.h"

#include <asterisk/lock.h>
#include <asterisk/frame.h> 
#include <asterisk/channel.h>
#ifndef CC_AST_HAVE_TECH_PVT
#include <asterisk/channel_pvt.h>
#endif
#include <asterisk/logger.h>
#include <asterisk/module.h>
#include <asterisk/pbx.h>
#include <asterisk/config.h>
#include <asterisk/options.h>
#include <asterisk/features.h>
#include <asterisk/utils.h>
#include <asterisk/cli.h>
#include <asterisk/rtp.h>
#include <asterisk/causes.h>
#ifndef CC_AST_NO_STRINGS
#include <asterisk/strings.h>
#endif
#include <asterisk/dsp.h>
#ifndef CC_AST_NO_DEVICESTATE
#include <asterisk/devicestate.h>
#endif
#include "xlaw.h"
#include "chan_capi20.h"
#include "chan_capi.h"
#include "chan_capi_rtp.h"
#endif

#ifdef PBX_IS_OPBX
#define CC_VERSION "cm-opbx-0.7"
#else
/* #define CC_VERSION "cm-x.y.z" */
#define CC_VERSION "$Revision$"
#endif

/*
 * personal stuff
 */
#undef   CAPI_APPLID_UNUSED
#define  CAPI_APPLID_UNUSED 0xffffffff
unsigned capi_ApplID = CAPI_APPLID_UNUSED;

static _cword capi_MessageNumber;
#ifdef PBX_IS_OPBX
static char *ccdesc = "Common ISDN API for OpenPBX";
#else
static char *ccdesc = "Common ISDN API for Asterisk";
#endif
#ifdef CC_AST_HAVE_TECH_PVT
static const char tdesc[] = "Common ISDN API Driver (" CC_VERSION ")";
static const char channeltype[] = "CAPI";
static const struct ast_channel_tech capi_tech;
#else
static char *tdesc = "Common ISDN API Driver (" CC_VERSION ")";
static char *channeltype = "CAPI";
#endif

static char *commandtdesc = "CAPI command interface.";
static char *commandapp = "capiCommand";
static char *commandsynopsis = "Execute special CAPI commands";
STANDARD_LOCAL_USER;
LOCAL_USER_DECL;

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
 * 4. cc_mutex_lock(&contrlock);
 *
 * 5. cc_mutex_lock(&messagenumber_lock);
 * 6. cc_mutex_lock(&usecnt_lock);
 * 7. cc_mutex_lock(&capi_put_lock);
 *
 *
 *  ** the PBX will call the callback functions with 
 *     this lock locked. This lock protects the 
 *     structure pointed to by 'i->owner'. Also note
 *     that calling some PBX functions will lock
 *     this lock!
 */

AST_MUTEX_DEFINE_STATIC(messagenumber_lock);
AST_MUTEX_DEFINE_STATIC(usecnt_lock);
AST_MUTEX_DEFINE_STATIC(iflock);
AST_MUTEX_DEFINE_STATIC(contrlock);
AST_MUTEX_DEFINE_STATIC(capi_put_lock);
AST_MUTEX_DEFINE_STATIC(verbose_lock);

static int capi_capability = AST_FORMAT_ALAW;

static pthread_t monitor_thread = (pthread_t)(0-1);

static struct capi_pvt *iflist = NULL;
static struct cc_capi_controller *capi_controllers[CAPI_MAX_CONTROLLERS + 1];
static int capi_num_controllers = 0;
static unsigned int capi_counter = 0;
static unsigned long capi_used_controllers = 0;
static char *emptyid = "\0";
static struct ast_channel *chan_to_hangup = NULL;
static struct ast_channel *chan_to_softhangup = NULL;

static char capi_national_prefix[AST_MAX_EXTENSION];
static char capi_international_prefix[AST_MAX_EXTENSION];

static char default_language[MAX_LANGUAGE] = "";

static int capidebug = 0;

/* local prototypes */
#ifdef CC_AST_HAS_INDICATE_DATA
static int pbx_capi_indicate(struct ast_channel *c, int condition, const void *data, size_t datalen);
#else
static int pbx_capi_indicate(struct ast_channel *c, int condition);
#endif

/* external prototypes */
extern char *capi_info_string(unsigned int info);

/* */
#define return_on_no_interface(x)                                       \
	if (!i) {                                                       \
		cc_verbose(4, 1, "CAPI: %s no interface for PLCI=%#x\n", x, PLCI);   \
		return;                                                 \
	}

/*
 * helper for <pbx>_verbose with different verbose settings
 */
void cc_verbose(int o_v, int c_d, char *text, ...)
{
	char line[4096];
	va_list ap;

	va_start(ap, text);
	vsnprintf(line, sizeof(line), text, ap);
	va_end(ap);

	if ((o_v == 0) || (option_verbose > o_v)) {
		if ((!c_d) || ((c_d) && (capidebug))) {	
			cc_mutex_lock(&verbose_lock);
			cc_pbx_verbose(line);
			cc_mutex_unlock(&verbose_lock);	
		}
	}
}

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
		(_cstruct) "\x04\x01\x00\x00\x02",
		(_cstruct) "\x00"
	}
};

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
 * show the text for a CAPI message info value
 */
static void show_capi_info(_cword info)
{
	char *p;
	
	if (info == 0x0000) {
		/* no error, do nothing */
		return;
	}

	if (!(p = capi_info_string((unsigned int)info))) {
		/* message not available */
		return;
	}
	
	cc_verbose(3, 0, VERBOSE_PREFIX_4 "CAPI INFO 0x%04x: %s\n",
		info, p);
	return;
}

/*
 * get a new capi message number automically
 */
_cword get_capi_MessageNumber(void)
{
	_cword mn;

	cc_mutex_lock(&messagenumber_lock);

	capi_MessageNumber++;
	if (capi_MessageNumber == 0) {
	    /* avoid zero */
	    capi_MessageNumber = 1;
	}

	mn = capi_MessageNumber;

	cc_mutex_unlock(&messagenumber_lock);

	return(mn);
}

/*
 * write a capi message to capi device
 */
MESSAGE_EXCHANGE_ERROR _capi_put_cmsg(_cmsg *CMSG)
{
	MESSAGE_EXCHANGE_ERROR error;
	
	if (cc_mutex_lock(&capi_put_lock)) {
		cc_log(LOG_WARNING, "Unable to lock capi put!\n");
		return -1;
	} 
	
	error = capi20_put_cmsg(CMSG);
	
	if (cc_mutex_unlock(&capi_put_lock)) {
		cc_log(LOG_WARNING, "Unable to unlock capi put!\n");
		return -1;
	}

	if (error) {
		cc_log(LOG_ERROR, "CAPI error sending %s (NCCI=%#x) (error=%#x %s)\n",
			capi_cmsg2str(CMSG), (unsigned int)HEADER_CID(CMSG),
			error, capi_info_string((unsigned int)error));
	} else {
		unsigned short wCmd = HEADER_CMD(CMSG);
		if ((wCmd == CAPI_P_REQ(DATA_B3)) ||
		    (wCmd == CAPI_P_RESP(DATA_B3))) {
			cc_verbose(7, 1, "%s\n", capi_cmsg2str(CMSG));
		} else {
			cc_verbose(4, 1, "%s\n", capi_cmsg2str(CMSG));
		}
	}

	return error;
}

/*
 * write a capi message and wait for CONF
 * i->lock must be held
 */
MESSAGE_EXCHANGE_ERROR _capi_put_cmsg_wait_conf(struct capi_pvt *i, _cmsg *CMSG)
{
	MESSAGE_EXCHANGE_ERROR error;
	struct timespec abstime;

	error = _capi_put_cmsg(CMSG);

	if (!(error)) {
		unsigned short wCmd = (CAPI_CONF << 8)|(CMSG->Command);
		i->waitevent = (unsigned int)wCmd;
		abstime.tv_sec = time(NULL) + 2;
		abstime.tv_nsec = 0;
		cc_verbose(4, 1, "%s: wait for %s\n",
			i->name, capi_cmd2str(CMSG->Command, CAPI_CONF));
		if (ast_cond_timedwait(&i->event_trigger, &i->lock, &abstime) != 0) {
			error = -1;
			cc_log(LOG_WARNING, "%s: timed out waiting for %s\n",
				i->name, capi_cmd2str(CMSG->Command, CAPI_CONF));
		}
	}

	return error;
}

/*
 * wait some time for a new capi message
 */
static MESSAGE_EXCHANGE_ERROR capidev_check_wait_get_cmsg(_cmsg *CMSG)
{
	MESSAGE_EXCHANGE_ERROR Info;
	struct timeval tv;

 repeat:
	Info = capi_get_cmsg(CMSG, capi_ApplID);

#if (CAPI_OS_HINT == 1) || (CAPI_OS_HINT == 2)
	/*
	 * For BSD allow controller 0:
	 */
	if ((HEADER_CID(CMSG) & 0xFF) == 0) {
		HEADER_CID(CMSG) += capi_num_controllers;
 	}
#endif

	/* if queue is empty */
	if (Info == 0x1104) {
		/* try waiting a maximum of 0.100 seconds for a message */
		tv.tv_sec = 0;
		tv.tv_usec = 10000;
		
		Info = capi20_waitformessage(capi_ApplID, &tv);

		if (Info == 0x0000)
			goto repeat;
	}
	
	if ((Info != 0x0000) && (Info != 0x1104)) {
		if (capidebug) {
			cc_log(LOG_DEBUG, "Error waiting for cmsg... INFO = %#x\n", Info);
		}
	}
    
	return Info;
}

/*
 * send Listen to specified controller
 */
static unsigned ListenOnController(unsigned long CIPmask, unsigned controller)
{
	MESSAGE_EXCHANGE_ERROR error;
	_cmsg CMSG;
	int waitcount = 100;

	LISTEN_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(), controller);

	LISTEN_REQ_INFOMASK(&CMSG) = 0xffff; /* lots of info ;) + early B3 connect */
		/* 0x00ff if no early B3 should be done */
		
	LISTEN_REQ_CIPMASK(&CMSG) = CIPmask;
	error = _capi_put_cmsg(&CMSG);

	if (error)
		goto done;

	while (waitcount) {
		error = capidev_check_wait_get_cmsg(&CMSG);

		if (IS_LISTEN_CONF(&CMSG)) {
			error = LISTEN_CONF_INFO(&CMSG);
			break;
		}
		usleep(20000);
		waitcount--;
	}
	if (!waitcount)
		error = 0x100F;

 done:
	return error;
}

#ifdef CC_AST_CHANNEL_HAS_TRANSFERCAP
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
	return 0;
}

static unsigned char tcap_is_digital(unsigned short tcap)
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
#endif /* CC_AST_CHANNEL_HAS_TRANSFERCAP */

/*
 * Echo cancellation is for cards w/ integrated echo cancellation only
 * (i.e. Eicon active cards support it)
 */
#define EC_FUNCTION_ENABLE              1
#define EC_FUNCTION_DISABLE             2
#define EC_FUNCTION_FREEZE              3
#define EC_FUNCTION_RESUME              4
#define EC_FUNCTION_RESET               5
#define EC_OPTION_DISABLE_NEVER         0
#define EC_OPTION_DISABLE_G165          (1<<2)
#define EC_OPTION_DISABLE_G164_OR_G165  (1<<1 | 1<<2)
#define EC_DEFAULT_TAIL                 0 /* maximum */

static void capi_echo_canceller(struct ast_channel *c, int function)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	_cmsg CMSG;
	char buf[10];

	if ((i->isdnstate & CAPI_ISDN_STATE_DISCONNECT))
		return;

	if (((function == EC_FUNCTION_ENABLE) && (i->isdnstate & CAPI_ISDN_STATE_EC)) ||
	    ((function != EC_FUNCTION_ENABLE) && (!(i->isdnstate & CAPI_ISDN_STATE_EC)))) {
		cc_verbose(3, 1, VERBOSE_PREFIX_4 "%s: echo canceller (PLCI=%#x, function=%d) unchanged\n",
			i->name, i->PLCI, function);
		/* nothing to do */
		return;
	}

	/* If echo cancellation is not requested or supported, don't attempt to enable it */
	cc_mutex_lock(&contrlock);
	if (!capi_controllers[i->controller]->echocancel || !i->doEC) {
		cc_mutex_unlock(&contrlock);
		return;
	}
	cc_mutex_unlock(&contrlock);

#ifdef CC_AST_CHANNEL_HAS_TRANSFERCAP
	if (tcap_is_digital(c->transfercapability)) {
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: No echo canceller in digital mode (PLCI=%#x)\n",
			i->name, i->PLCI);
		return;
	}
#endif

	cc_verbose(2, 0, VERBOSE_PREFIX_2 "%s: Setting up echo canceller (PLCI=%#x, function=%d, options=%d, tail=%d)\n",
			i->name, i->PLCI, function, i->ecOption, i->ecTail);

	FACILITY_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(), 0);
	FACILITY_REQ_PLCI(&CMSG) = i->PLCI;
	FACILITY_REQ_FACILITYSELECTOR(&CMSG) = i->ecSelector;

	memset(buf, 0, sizeof(buf));
        buf[0] = 9; /* msg size */
        write_capi_word(&buf[1], function);
	if (function == EC_FUNCTION_ENABLE) {
		buf[3] = 6; /* echo cancel param struct size */
	        write_capi_word(&buf[4], i->ecOption); /* bit field - ignore echo canceller disable tone */
		write_capi_word(&buf[6], i->ecTail);   /* Tail length, ms */
		/* buf 8 and 9 are "pre-delay lenght ms" */
		i->isdnstate |= CAPI_ISDN_STATE_EC;
	} else {
		i->isdnstate &= ~CAPI_ISDN_STATE_EC;
	}

	FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = (_cstruct)buf;
        
	if (_capi_put_cmsg(&CMSG) != 0) {
		return;
	}

	return;
}

/*
 * turn on/off DTMF detection
 */
static int capi_detect_dtmf(struct ast_channel *c, int flag)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	MESSAGE_EXCHANGE_ERROR error;
	_cmsg CMSG;
	char buf[9];

	if ((i->isdnstate & CAPI_ISDN_STATE_DISCONNECT))
		return 0;

#ifdef CC_AST_CHANNEL_HAS_TRANSFERCAP
	if (tcap_is_digital(c->transfercapability)) {
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: No dtmf-detect in digital mode (PLCI=%#x)\n",
			i->name, i->PLCI);
		return 0;
	}
#endif
	memset(buf, 0, sizeof(buf));
	
	/* does the controller support dtmf? and do we want to use it? */
	
	cc_mutex_lock(&contrlock);
	
	if ((capi_controllers[i->controller]->dtmf == 1) && (i->doDTMF == 0)) {
		cc_mutex_unlock(&contrlock);
		cc_verbose(2, 0, VERBOSE_PREFIX_2 "%s: Setting up DTMF detector (PLCI=%#x, flag=%d)\n",
			i->name, i->PLCI, flag);
		FACILITY_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(), 0);
		FACILITY_REQ_PLCI(&CMSG) = i->PLCI;
		FACILITY_REQ_FACILITYSELECTOR(&CMSG) = FACILITYSELECTOR_DTMF;
		buf[0] = 8; /* msg length */
		if (flag == 1) {
			write_capi_word(&buf[1], 1); /* start DTMF listen */
		} else {
			write_capi_word(&buf[1], 2); /* stop DTMF listen */
		}
		write_capi_word(&buf[3], CAPI_DTMF_DURATION);
		write_capi_word(&buf[5], CAPI_DTMF_DURATION);
		FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = (_cstruct)buf;
        
		if ((error = _capi_put_cmsg(&CMSG)) != 0) {
			return error;
		}
	} else {
		cc_mutex_unlock(&contrlock);
		/* do software dtmf detection */
		if (i->doDTMF == 0) {
			i->doDTMF = 1;
		}
	}
	return 0;
}

/*
 * queue a frame to PBX
 */
static int local_queue_frame(struct capi_pvt *i, struct ast_frame *f)
{
	struct ast_channel *chan = i->owner;
	unsigned char *wbuf;
	int wbuflen;

	if (chan == NULL) {
		cc_log(LOG_ERROR, "No owner in local_queue_frame for %s\n",
			i->name);
		return -1;
	}

	if (!(i->isdnstate & CAPI_ISDN_STATE_PBX)) {
		/* if there is no PBX running yet,
		   we don't need any frames sent */
		return -1;
	}
	if ((i->state == CAPI_STATE_DISCONNECTING) ||
	    (i->isdnstate & CAPI_ISDN_STATE_HANGUP)) {
		cc_verbose(3, 1, VERBOSE_PREFIX_4 "%s: no queue_frame in state disconnecting for %d/%d\n",
			i->name, f->frametype, f->subclass);
		return 0;
	}

	if ((capidebug) && (f->frametype != AST_FRAME_VOICE)) {
		ast_frame_dump("CAPI", f, VERBOSE_PREFIX_3 "queue frame:");
	}

	if ((f->frametype == AST_FRAME_CONTROL) &&
	    (f->subclass == AST_CONTROL_HANGUP)) {
		i->isdnstate |= CAPI_ISDN_STATE_HANGUP;
	}

	if (i->writerfd == -1) {
		cc_log(LOG_ERROR, "No writerfd in local_queue_frame for %s\n",
			i->name);
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
			i->name);
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
		i->name, i->dnid, capi_counter++);
	ast_change_name(i->owner, name);
	cc_verbose(3, 0, VERBOSE_PREFIX_3 "%s: Updated channel name: %s\n",
			i->name, name);
}

/*
 * send digits via INFO_REQ
 */
static int capi_send_info_digits(struct capi_pvt *i, char *digits, int len)
{
	MESSAGE_EXCHANGE_ERROR error;
	_cmsg CMSG;
	char buf[64];
	int a;
    
	memset(buf, 0, sizeof(buf));

	INFO_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(), 0);
	INFO_REQ_PLCI(&CMSG) = i->PLCI;

	if (len > (sizeof(buf) - 2))
		len = sizeof(buf) - 2;
	
	buf[0] = len + 1;
	buf[1] = 0x80;
	for (a = 0; a < len; a++) {
		buf[a + 2] = digits[a];
	}
	INFO_REQ_CALLEDPARTYNUMBER(&CMSG) = (_cstruct)buf;

	if ((error = _capi_put_cmsg(&CMSG)) != 0) {
		return error;
	}
	cc_verbose(3, 1, VERBOSE_PREFIX_4 "%s: sent CALLEDPARTYNUMBER INFO digits = '%s' (PLCI=%#x)\n",
		i->name, buf + 2, i->PLCI);
	return 0;
}

/*
 * send a DTMF digit
 */
static int pbx_capi_send_digit(struct ast_channel *c, char digit)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	_cmsg CMSG;
	char buf[10];
	char did[2];
	int ret = 0;
    
	if (i == NULL) {
		cc_log(LOG_ERROR, "No interface!\n");
		return -1;
	}

	memset(buf, 0, sizeof(buf));

	cc_mutex_lock(&i->lock);

	if ((c->_state == AST_STATE_DIALING) &&
	    (i->state != CAPI_STATE_DISCONNECTING)) {
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
	}

	if ((i->state == CAPI_STATE_CONNECTED) && (i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
		/* we have a real connection, so send real DTMF */
		cc_mutex_lock(&contrlock);
		if ((capi_controllers[i->controller]->dtmf == 0) || (i->doDTMF > 0)) {
			/* let * fake it */
			cc_mutex_unlock(&contrlock);
			cc_mutex_unlock(&i->lock);
			return -1;
		}
		
		cc_mutex_unlock(&contrlock);
	
		FACILITY_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(), 0);
		FACILITY_REQ_PLCI(&CMSG) = i->NCCI;
	        FACILITY_REQ_FACILITYSELECTOR(&CMSG) = FACILITYSELECTOR_DTMF;
        	buf[0] = 8;
	        write_capi_word(&buf[1], 3); /* send DTMF digit */
	        write_capi_word(&buf[3], CAPI_DTMF_DURATION);
	        write_capi_word(&buf[5], CAPI_DTMF_DURATION);
	        buf[7] = 1;
		buf[8] = digit;
		FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = (_cstruct)buf;
        
		if ((ret = _capi_put_cmsg(&CMSG)) == 0) {
			cc_verbose(3, 0, VERBOSE_PREFIX_4 "%s: sent dtmf '%c'\n",
				i->name, digit);
		}
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
	_cmsg CMSG;

	if ((i->state != CAPI_STATE_INCALL) &&
	    (i->state != CAPI_STATE_DID)) {
		cc_verbose(2, 1, VERBOSE_PREFIX_3 "%s: attempting ALERT in state %d\n",
			i->name, i->state);
		return -1;
	}
	
	ALERT_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(), 0);
	ALERT_REQ_PLCI(&CMSG) = i->PLCI;

	if (_capi_put_cmsg(&CMSG) != 0) {
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
		i->name, i->PLCI);

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

	i->FaxState &= ~CAPI_FAX_STATE_MASK;

	i->PLCI = 0;
	i->MessageNumber = 0;
	i->NCCI = 0;
	i->onholdPLCI = 0;

	memset(i->cid, 0, sizeof(i->cid));
	memset(i->dnid, 0, sizeof(i->dnid));
	i->cid_ton = 0;

	i->rtpcodec = 0;
	if (i->rtp) {
		ast_rtp_destroy(i->rtp);
	}

	i->owner = NULL;
	return;
}

/*
 * disconnect b3 and wait for confirmation 
 */
static void cc_disconnect_b3(struct capi_pvt *i, int wait) 
{
	_cmsg CMSG;
	int waitcount = 200;

	if (!(i->isdnstate & (CAPI_ISDN_STATE_B3_UP | CAPI_ISDN_STATE_B3_PEND)))
		return;

	cc_mutex_lock(&i->lock);
	DISCONNECT_B3_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(), 0);
	DISCONNECT_B3_REQ_NCCI(&CMSG) = i->NCCI;
	_capi_put_cmsg_wait_conf(i, &CMSG);
	cc_mutex_unlock(&i->lock);

	if (!wait)
		return;
	
	/* wait for the B3 layer to go down */
	while ((waitcount > 0) &&
	       ((i->isdnstate & (CAPI_ISDN_STATE_B3_UP | CAPI_ISDN_STATE_B3_PEND)))) {
		usleep(10000);
		waitcount--;
	}
	if ((i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
		cc_log(LOG_ERROR, "capi disconnect b3: didn't disconnect NCCI=0x%08x\n",
			i->NCCI);
	}
	return;
}

/*
 * send CONNECT_B3_REQ
 */
static void cc_start_b3(struct capi_pvt *i)
{
	_cmsg CMSG;

	if (!(i->isdnstate & (CAPI_ISDN_STATE_B3_UP | CAPI_ISDN_STATE_B3_PEND))) {
		i->isdnstate |= CAPI_ISDN_STATE_B3_PEND;
		CONNECT_B3_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(), 0);
		CONNECT_B3_REQ_PLCI(&CMSG) = i->PLCI;
		CONNECT_B3_REQ_NCPI(&CMSG) = capi_rtp_ncpi(i);
		_capi_put_cmsg(&CMSG);
		cc_verbose(4, 1, VERBOSE_PREFIX_3 "%s: sent CONNECT_B3_REQ PLCI=%#x\n",
			i->name, i->PLCI);
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
 * hangup a line (CAPI messages)
 */
static void capi_activehangup(struct ast_channel *c)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	_cmsg CMSG;
	int state;
	const char *cause;

	if (i == NULL) {
		cc_log(LOG_WARNING, "No interface!\n");
		return;
	}

	state = i->state;
	i->state = CAPI_STATE_DISCONNECTING;

	i->cause = c->hangupcause;
	if ((cause = pbx_builtin_getvar_helper(c, "PRI_CAUSE"))) {
		i->cause = atoi(cause);
	}
	
	if ((i->isdnstate & CAPI_ISDN_STATE_ECT)) {
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: activehangup ECT call\n",
			i->name);
		/* we do nothing, just wait for DISCONNECT_IND */
		return;
	}

	cc_verbose(2, 1, VERBOSE_PREFIX_3 "%s: activehangingup (cause=%d)\n",
		i->name, i->cause);


	if ((state == CAPI_STATE_ALERTING) ||
	    (state == CAPI_STATE_DID) || (state == CAPI_STATE_INCALL)) {
		CONNECT_RESP_HEADER(&CMSG, capi_ApplID, i->MessageNumber, 0);
		CONNECT_RESP_PLCI(&CMSG) = i->PLCI;
		CONNECT_RESP_REJECT(&CMSG) = (i->cause) ? (0x3480 | (i->cause & 0x7f)) : 2;
		_capi_put_cmsg(&CMSG);
		return;
	}

	/* active disconnect */
	if ((i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
		cc_disconnect_b3(i, 0);
		return;
	}
	
	if ((state == CAPI_STATE_CONNECTED) || (state == CAPI_STATE_CONNECTPENDING) ||
	    (state == CAPI_STATE_ANSWERING) || (state == CAPI_STATE_ONHOLD)) {
		cc_mutex_lock(&i->lock);
		DISCONNECT_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(), 0);
		DISCONNECT_REQ_PLCI(&CMSG) = i->PLCI;
		_capi_put_cmsg_wait_conf(i, &CMSG);
		cc_mutex_unlock(&i->lock);
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

	/*
	 * hmm....ok...this is called to free the capi interface (passive disconnect)
	 * or to bring down the channel (active disconnect)
	 */

	if (i == NULL) {
		cc_log(LOG_ERROR, "channel has no interface!\n");
		return -1;
	}

	cc_verbose(3, 0, VERBOSE_PREFIX_2 "%s: CAPI Hangingup\n",
		i->name);
  
	/* are we down, yet? */
	if (i->state != CAPI_STATE_DISCONNECTED) {
		/* no */
		capi_activehangup(c);
	} else {
		cleanup = 1;
	}
	
	if ((i->doDTMF > 0) && (i->vad != NULL)) {
		ast_dsp_free(i->vad);
	}
	
	cc_mutex_lock(&usecnt_lock);
	usecnt--;
	cc_mutex_unlock(&usecnt_lock);
	
	ast_update_use_count();
	
	cc_mutex_lock(&i->lock);

	CC_CHANNEL_PVT(c) = NULL;
	ast_setstate(c, AST_STATE_DOWN);

	if (cleanup) {
		/* disconnect already done, so cleanup */
		interface_cleanup(i);
	}

	cc_mutex_unlock(&i->lock);

	return 0;
}

/*
 * convert a number
 */
static char *capi_number_func(unsigned char *data, unsigned int strip, char *buf)
{
	unsigned int len;

	if (data[0] == 0xff) {
		len = read_capi_word(&data[1]);
		data += 2;
	} else {
		len = data[0];
		data += 1;
	}
	if (len > (AST_MAX_EXTENSION - 1))
		len = (AST_MAX_EXTENSION - 1);
	
	/* convert a capi struct to a \0 terminated string */
	if ((!len) || (len < strip))
		return NULL;
		
	len = len - strip;
	data += strip;

	memcpy(buf, data, len);
	buf[len] = '\0';
	
	return buf;
}
#define capi_number(data, strip) \
  capi_number_func(data, strip, alloca(AST_MAX_EXTENSION))

/*
 * parse the dialstring
 */
static void parse_dialstring(char *buffer, char **interface, char **dest, char **param, char **ocid)
{
	int cp = 0;
	char *buffer_p = buffer;
	char *oc;

	/* interface is the first part of the string */
	*interface = buffer;

	*dest = emptyid;
	*param = emptyid;
	*ocid = NULL;

	while (*buffer_p) {
		if (*buffer_p == '/') {
			*buffer_p = 0;
			buffer_p++;
			if (cp == 0) {
				*dest = buffer_p;
				cp++;
			} else if (cp == 1) {
				*param = buffer_p;
				cp++;
			} else {
				cc_log(LOG_WARNING, "Too many parts in dialstring '%s'\n",
					buffer);
			}
			continue;
		}
		buffer_p++;
	}
	if ((oc = strchr(*dest, ':')) != NULL) {
		*ocid = *dest;
		*oc = '\0';
		*dest = oc + 1;
	}
	cc_verbose(3, 1, VERBOSE_PREFIX_4 "parsed dialstring: '%s' '%s' '%s' '%s'\n",
		*interface, (*ocid) ? *ocid : "NULL", *dest, *param);
	return;
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
	char bchaninfo[3];
	int CLIR;
	int callernplan = 0;
	int use_defaultcid = 0;
	const char *ton, *p;
	char *osa = NULL;
	char *dsa = NULL;
	char callingsubaddress[AST_MAX_EXTENSION];
	char calledsubaddress[AST_MAX_EXTENSION];
	
	_cmsg CMSG;
	MESSAGE_EXCHANGE_ERROR  error;

	cc_copy_string(buffer, idest, sizeof(buffer));
	parse_dialstring(buffer, &interface, &dest, &param, &ocid);

	cc_mutex_lock(&i->lock);

	/* init param settings */
	i->doB3 = CAPI_B3_DONT;
	i->doOverlap = 0;
	memset(i->overlapdigits, 0, sizeof(i->overlapdigits));

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
		default:
			cc_log(LOG_WARNING, "Unknown parameter '%c' in '%s', ignoring.\n",
				*param, idest);
		}
		param++;
	}
	if (((!dest) || (!dest[0])) && (i->doB3 != CAPI_B3_ALWAYS)) {
		cc_log(LOG_ERROR, "No destination or dialtone requested in '%s'\n", idest);
		cc_mutex_unlock(&i->lock);
		return -1;
	}

#ifdef CC_AST_CHANNEL_HAS_CID
	CLIR = c->cid.cid_pres;
	callernplan = c->cid.cid_ton & 0x7f;
#else    
	CLIR = c->callingpres;
#endif
	if ((ton = pbx_builtin_getvar_helper(c, "CALLERTON"))) {
		callernplan = atoi(ton) & 0x7f;
	}
	cc_verbose(1, 1, VERBOSE_PREFIX_2 "%s: Call %s %s%s (pres=0x%02x, ton=0x%02x)\n",
		i->name, c->name, i->doB3 ? "with B3 ":" ",
		i->doOverlap ? "overlap":"", CLIR, callernplan);

	i->outgoing = 1;
	i->isdnstate |= CAPI_ISDN_STATE_PBX;
	
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

	i->MessageNumber = get_capi_MessageNumber();
	CONNECT_REQ_HEADER(&CMSG, capi_ApplID, i->MessageNumber, i->controller);
	CONNECT_REQ_CONTROLLER(&CMSG) = i->controller;
#ifdef CC_AST_CHANNEL_HAS_TRANSFERCAP
	CONNECT_REQ_CIPVALUE(&CMSG) = tcap2cip(c->transfercapability);
	if (tcap_is_digital(c->transfercapability)) {
		i->bproto = CC_BPROTO_TRANSPARENT;
		cc_verbose(4, 0, VERBOSE_PREFIX_2 "%s: is digital call, set proto to TRANSPARENT\n",
			i->name);
	}
#else
	CONNECT_REQ_CIPVALUE(&CMSG) = 0x10; /* Telephony */
#endif
	if ((i->doOverlap) && (strlen(dest))) {
		cc_copy_string(i->overlapdigits, dest, sizeof(i->overlapdigits));
		called[0] = 1;
	} else {
		i->doOverlap = 0;
		called[0] = strlen(dest) + 1;
	}
	called[1] = 0x80;
	strncpy(&called[2], dest, sizeof(called) - 3);
	CONNECT_REQ_CALLEDPARTYNUMBER(&CMSG) = (_cstruct)called;
	CONNECT_REQ_CALLEDPARTYSUBADDRESS(&CMSG) = (_cstruct)dsa;

#ifdef CC_AST_CHANNEL_HAS_CID
	if (c->cid.cid_num) 
		cc_copy_string(callerid, c->cid.cid_num, sizeof(callerid));
#else
	if (c->callerid) 
		cc_copy_string(callerid, c->callerid, sizeof(callerid));
#endif
	else
		memset(callerid, 0, sizeof(callerid));

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

	CONNECT_REQ_CALLINGPARTYNUMBER(&CMSG) = (_cstruct)calling;
	CONNECT_REQ_CALLINGPARTYSUBADDRESS(&CMSG) = (_cstruct)osa;

	CONNECT_REQ_B1PROTOCOL(&CMSG) = b_protocol_table[i->bproto].b1protocol;
	CONNECT_REQ_B2PROTOCOL(&CMSG) = b_protocol_table[i->bproto].b2protocol;
	CONNECT_REQ_B3PROTOCOL(&CMSG) = b_protocol_table[i->bproto].b3protocol;
	CONNECT_REQ_B1CONFIGURATION(&CMSG) = b_protocol_table[i->bproto].b1configuration;
	CONNECT_REQ_B2CONFIGURATION(&CMSG) = b_protocol_table[i->bproto].b2configuration;
	CONNECT_REQ_B3CONFIGURATION(&CMSG) = b_protocol_table[i->bproto].b3configuration;

	bchaninfo[0] = 2;
	bchaninfo[1] = 0x0;
	bchaninfo[2] = 0x0;
	CONNECT_REQ_BCHANNELINFORMATION(&CMSG) = (_cstruct)bchaninfo; /* 0 */

        if ((error = _capi_put_cmsg_wait_conf(i, &CMSG))) {
		interface_cleanup(i);
		cc_mutex_unlock(&i->lock);
		return error;
	}

	i->state = CAPI_STATE_CONNECTPENDING;
	ast_setstate(c, AST_STATE_DIALING);

	/* now we shall return .... the rest has to be done by handle_msg */
	cc_mutex_unlock(&i->lock);
	return 0;
}

/*
 * answer a capi call
 */
static int capi_send_answer(struct ast_channel *c, _cstruct b3conf)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	_cmsg CMSG;
	char buf[CAPI_MAX_STRING];
	const char *dnid;
	const char *connectednumber;
    
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

	CONNECT_RESP_HEADER(&CMSG, capi_ApplID, i->MessageNumber, 0);
	CONNECT_RESP_PLCI(&CMSG) = i->PLCI;
	CONNECT_RESP_REJECT(&CMSG) = 0;
	if (strlen(dnid)) {
		buf[0] = strlen(dnid) + 2;
		buf[1] = 0x00;
		buf[2] = 0x80;
		strncpy(&buf[3], dnid, sizeof(buf) - 4);
		CONNECT_RESP_CONNECTEDNUMBER(&CMSG) = (_cstruct)buf;
	}
	CONNECT_RESP_B1PROTOCOL(&CMSG) = b_protocol_table[i->bproto].b1protocol;
	CONNECT_RESP_B2PROTOCOL(&CMSG) = b_protocol_table[i->bproto].b2protocol;
	CONNECT_RESP_B3PROTOCOL(&CMSG) = b_protocol_table[i->bproto].b3protocol;
	CONNECT_RESP_B1CONFIGURATION(&CMSG) = b_protocol_table[i->bproto].b1configuration;
	CONNECT_RESP_B2CONFIGURATION(&CMSG) = b_protocol_table[i->bproto].b2configuration;
	if (!b3conf)
		b3conf = b_protocol_table[i->bproto].b3configuration;
	CONNECT_RESP_B3CONFIGURATION(&CMSG) = b3conf;

	cc_verbose(3, 0, VERBOSE_PREFIX_2 "%s: Answering for %s\n",
		i->name, dnid);
		
	if (_capi_put_cmsg(&CMSG) != 0) {
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
#ifdef CC_AST_CHANNEL_HAS_TRANSFERCAP	
		if (!tcap_is_digital(c->transfercapability))
#endif
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
	int readsize;

	if (i == NULL) {
		cc_log(LOG_ERROR, "channel has no interface\n");
		return NULL;
	}
	if (i->readerfd == -1) {
		cc_log(LOG_ERROR, "no readerfd\n");
		return NULL;
	}

	f = &i->f;
	f->frametype = AST_FRAME_NULL;
	f->subclass = 0;

	readsize = read(i->readerfd, f, sizeof(struct ast_frame));
	if (readsize != sizeof(struct ast_frame)) {
		cc_log(LOG_ERROR, "did not read a whole frame\n");
	}
	
	f->mallocd = 0;
	f->data = NULL;

	if ((f->frametype == AST_FRAME_CONTROL) && (f->subclass == AST_CONTROL_HANGUP)) {
		return NULL;
	}

	if ((f->frametype == AST_FRAME_VOICE) && (f->datalen > 0)) {
		if (f->datalen > sizeof(i->frame_data)) {
			cc_log(LOG_ERROR, "f.datalen(%d) greater than space of frame_data(%d)\n",
				f->datalen, sizeof(i->frame_data));
			f->datalen = sizeof(i->frame_data);
		}
		readsize = read(i->readerfd, i->frame_data + AST_FRIENDLY_OFFSET, f->datalen);
		if (readsize != f->datalen) {
			cc_log(LOG_ERROR, "did not read whole frame data\n");
		}
		f->data = i->frame_data + AST_FRIENDLY_OFFSET;
		if ((i->doDTMF > 0) && (i->vad != NULL) ) {
#ifdef CC_AST_DSP_PROCESS_NEEDLOCK 
			f = ast_dsp_process(c, i->vad, f, 0);
#else
			f = ast_dsp_process(c, i->vad, f);
#endif
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
	MESSAGE_EXCHANGE_ERROR error;
	_cmsg CMSG;
	int j = 0;
	unsigned char *buf;
	struct ast_frame *fsmooth;
	int txavg=0;
	int ret = 0;

	if (!i) {
		cc_log(LOG_ERROR, "channel has no interface\n");
		return -1;
	}
	 
	cc_mutex_lock(&i->lock);

	if ((!(i->isdnstate & CAPI_ISDN_STATE_B3_UP)) || (!i->NCCI) ||
	    ((i->isdnstate & (CAPI_ISDN_STATE_B3_CHANGE | CAPI_ISDN_STATE_LI)))) {
		cc_mutex_unlock(&i->lock);
		return 0;
	}

	if ((!(i->ntmode)) && (i->state != CAPI_STATE_CONNECTED)) {
		cc_mutex_unlock(&i->lock);
		return 0;
	}

	if (f->frametype == AST_FRAME_NULL) {
		cc_mutex_unlock(&i->lock);
		return 0;
	}
	if (f->frametype == AST_FRAME_DTMF) {
		cc_log(LOG_ERROR, "dtmf frame should be written\n");
		cc_mutex_unlock(&i->lock);
		return 0;
	}
	if (f->frametype != AST_FRAME_VOICE) {
		cc_log(LOG_ERROR,"not a voice frame\n");
		cc_mutex_unlock(&i->lock);
		return 0;
	}
	if (i->FaxState & CAPI_FAX_STATE_ACTIVE) {
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: write on fax_receive?\n",
			i->name);
		cc_mutex_unlock(&i->lock);
		return 0;
	}
	if ((!f->data) || (!f->datalen)) {
		cc_log(LOG_DEBUG, "No data for FRAME_VOICE %s\n", c->name);
		cc_mutex_unlock(&i->lock);
		return 0;
	}
	if (i->isdnstate & CAPI_ISDN_STATE_RTP) {
		if ((!(f->subclass & i->codec)) &&
		    (f->subclass != capi_capability)) {
			cc_log(LOG_ERROR, "don't know how to write subclass %s(%d)\n",
				ast_getformatname(f->subclass), f->subclass);
			cc_mutex_unlock(&i->lock);
			return 0;
		}
	}

	if ((!i->smoother) || (ast_smoother_feed(i->smoother, f) != 0)) {
		cc_log(LOG_ERROR, "%s: failed to fill smoother\n", i->name);
		cc_mutex_unlock(&i->lock);
		return 0;
	}

	for (fsmooth = ast_smoother_read(i->smoother);
	     fsmooth != NULL;
	     fsmooth = ast_smoother_read(i->smoother)) {
		if (i->isdnstate & CAPI_ISDN_STATE_RTP) {
			ret = capi_write_rtp(c, fsmooth);
			continue;
		}
		DATA_B3_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(), 0);
		DATA_B3_REQ_NCCI(&CMSG) = i->NCCI;
		DATA_B3_REQ_DATALENGTH(&CMSG) = fsmooth->datalen;
		DATA_B3_REQ_FLAGS(&CMSG) = 0; 

		DATA_B3_REQ_DATAHANDLE(&CMSG) = i->send_buffer_handle;
		buf = &(i->send_buffer[(i->send_buffer_handle % CAPI_MAX_B3_BLOCKS) *
			(CAPI_MAX_B3_BLOCK_SIZE + AST_FRIENDLY_OFFSET)]);
		DATA_B3_REQ_DATA(&CMSG) = buf;
		i->send_buffer_handle++;

#ifdef CC_AST_CHANNEL_HAS_TRANSFERCAP	
		if ((i->doES == 1) && (!tcap_is_digital(c->transfercapability))) {
#else
		if ((i->doES == 1)) {
#endif
			for (j = 0; j < fsmooth->datalen; j++) {
				buf[j] = reversebits[ ((unsigned char *)fsmooth->data)[j] ]; 
				if (capi_capability == AST_FORMAT_ULAW) {
					txavg += abs( capiULAW2INT[reversebits[ ((unsigned char*)fsmooth->data)[j]]] );
				} else {
					txavg += abs( capiALAW2INT[reversebits[ ((unsigned char*)fsmooth->data)[j]]] );
				}
			}
			txavg = txavg / j;
			for(j = 0; j < ECHO_TX_COUNT - 1; j++) {
				i->txavg[j] = i->txavg[j+1];
			}
			i->txavg[ECHO_TX_COUNT - 1] = txavg;
		} else {
#ifdef CC_AST_CHANNEL_HAS_TRANSFERCAP	
			if ((i->txgain == 1.0) || (!tcap_is_digital(c->transfercapability))) {
#else
			if (i->txgain == 1.0) {
#endif
				for (j = 0; j < fsmooth->datalen; j++) {
					buf[j] = reversebits[((unsigned char *)fsmooth->data)[j]];
				}
			} else {
				for (j = 0; j < fsmooth->datalen; j++) {
					buf[j] = i->g.txgains[reversebits[((unsigned char *)fsmooth->data)[j]]];
				}
			}
		}
   
   		error = 1; 
		if (i->B3q > 0) {
			error = _capi_put_cmsg(&CMSG);
		} else {
			cc_verbose(3, 1, VERBOSE_PREFIX_4 "%s: too much voice to send for NCCI=%#x\n",
				i->name, i->NCCI);
		}

		if (!error) {
			i->B3q -= fsmooth->datalen;
			if (i->B3q < 0)
				i->B3q = 0;
		}
	}
	cc_mutex_unlock(&i->lock);
	return ret;
}

/*
 * new channel
 */
static int pbx_capi_fixup(struct ast_channel *oldchan, struct ast_channel *newchan)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(newchan);

	cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: %s fixup now %s\n",
		i->name, oldchan->name, newchan->name);

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
	_cmsg CMSG;

	SELECT_B_PROTOCOL_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(), 0);
	SELECT_B_PROTOCOL_REQ_PLCI(&CMSG) = i->PLCI;
	SELECT_B_PROTOCOL_REQ_B1PROTOCOL(&CMSG) = b_protocol_table[i->bproto].b1protocol;
	SELECT_B_PROTOCOL_REQ_B2PROTOCOL(&CMSG) = b_protocol_table[i->bproto].b2protocol;
	SELECT_B_PROTOCOL_REQ_B3PROTOCOL(&CMSG) = b_protocol_table[i->bproto].b3protocol;
	SELECT_B_PROTOCOL_REQ_B1CONFIGURATION(&CMSG) = b_protocol_table[i->bproto].b1configuration;
	SELECT_B_PROTOCOL_REQ_B2CONFIGURATION(&CMSG) = b_protocol_table[i->bproto].b2configuration;
	if (!b3conf)
		b3conf = b_protocol_table[i->bproto].b3configuration;
	SELECT_B_PROTOCOL_REQ_B3CONFIGURATION(&CMSG) = b3conf;

	_capi_put_cmsg(&CMSG);
}

/*
 * do line initerconnect
 */
static int line_interconnect(struct capi_pvt *i0, struct capi_pvt *i1, int start)
{
	_cmsg CMSG;
	char buf[20];

	if ((i0->isdnstate & CAPI_ISDN_STATE_DISCONNECT) ||
	    (i1->isdnstate & CAPI_ISDN_STATE_DISCONNECT))
		return -1;

	if ((!(i0->isdnstate & CAPI_ISDN_STATE_B3_UP)) || 
	    (!(i1->isdnstate & CAPI_ISDN_STATE_B3_UP))) {
		cc_verbose(3, 1, VERBOSE_PREFIX_2
			"%s:%s line interconnect aborted, at least "
			"one channel is not connected.\n",
			i0->name, i1->name);
		return -1;
	}

	FACILITY_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(), 0);
	FACILITY_REQ_PLCI(&CMSG) = i0->PLCI;
	FACILITY_REQ_FACILITYSELECTOR(&CMSG) = FACILITYSELECTOR_LINE_INTERCONNECT;

	memset(buf, 0, sizeof(buf));

	if (start) {
		/* connect */
		buf[0] = 17; /* msg size */
		write_capi_word(&buf[1], 0x0001);
		buf[3] = 14; /* struct size LI Request Parameter */
		write_capi_dword(&buf[4], 0x00000000); /* Data Path */
		buf[8] = 9; /* struct size */
		buf[9] = 8; /* struct size LI Request Connect Participant */
		write_capi_dword(&buf[10], i1->PLCI);
		write_capi_dword(&buf[14], 0x00000003); /* Data Path Participant */
	} else {
		/* disconnect */
		buf[0] = 7; /* msg size */
		write_capi_word(&buf[1], 0x0002);
		buf[3] = 4; /* struct size */
		write_capi_dword(&buf[4], i1->PLCI);
	}

	FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = (_cstruct)buf;
        
	_capi_put_cmsg(&CMSG);

	if (start) {
		i0->isdnstate |= CAPI_ISDN_STATE_LI;
		i1->isdnstate |= CAPI_ISDN_STATE_LI;
	} else {
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
 * native bridging / line interconnect
 */
static CC_BRIDGE_RETURN pbx_capi_bridge(struct ast_channel *c0,
                                    struct ast_channel *c1,
                                    int flags, struct ast_frame **fo,
				    struct ast_channel **rc
#ifdef CC_AST_BRIDGE_WITH_TIMEOUTMS
                                    , int timeoutms
#endif
				    )
{
	struct capi_pvt *i0 = CC_CHANNEL_PVT(c0);
	struct capi_pvt *i1 = CC_CHANNEL_PVT(c1);
	CC_BRIDGE_RETURN ret = AST_BRIDGE_COMPLETE;
	int waitcount = 20;

	cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s:%s Requested native bridge for %s and %s\n",
		i0->name, i1->name, c0->name, c1->name);

	if ((!i0->bridge) || (!i1->bridge))
		return AST_BRIDGE_FAILED_NOWARN;

	cc_mutex_lock(&contrlock);
	if ((!capi_controllers[i0->controller]->lineinterconnect) ||
	    (!capi_controllers[i1->controller]->lineinterconnect)) {
		cc_mutex_unlock(&contrlock);
		return AST_BRIDGE_FAILED_NOWARN;
	}
	cc_mutex_unlock(&contrlock);

	while (((!(i0->isdnstate & CAPI_ISDN_STATE_B3_UP)) || 
	       (!(i1->isdnstate & CAPI_ISDN_STATE_B3_UP))) &&
	       (waitcount > 0)) {
		/* wait a moment for B to come up */
		usleep(20000);
		waitcount--;
	}

	if (!(flags & AST_BRIDGE_DTMF_CHANNEL_0))
		capi_detect_dtmf(i0->owner, 0);

	if (!(flags & AST_BRIDGE_DTMF_CHANNEL_1))
		capi_detect_dtmf(i1->owner, 0);

	capi_echo_canceller(i0->owner, EC_FUNCTION_DISABLE);
	capi_echo_canceller(i1->owner, EC_FUNCTION_DISABLE);

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
#ifndef CC_AST_BRIDGE_WITH_TIMEOUTMS
		int timeoutms;

		timeoutms = -1;
#endif
		who = ast_waitfor_n(priority ? c0_priority : c1_priority, 2, &timeoutms);
		if (!who) {
#ifdef CC_AST_BRIDGE_WITH_TIMEOUTMS
			if (!timeoutms) {
				ret = AST_BRIDGE_RETRY;
				break;
			}
#else
			cc_log(LOG_DEBUG, "Ooh, empty read...\n");
#endif
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
		capi_detect_dtmf(i0->owner, 1);

	if (!(flags & AST_BRIDGE_DTMF_CHANNEL_1))
		capi_detect_dtmf(i1->owner, 1);

	capi_echo_canceller(i0->owner, EC_FUNCTION_ENABLE);
	capi_echo_canceller(i1->owner, EC_FUNCTION_ENABLE);

	return ret;
}

/*
 * a new channel is needed
 */
static struct ast_channel *capi_new(struct capi_pvt *i, int state)
{
	struct ast_channel *tmp;
	int fmt;
	int fds[2];
	int flags;

	tmp = ast_channel_alloc(0);
	
	if (tmp == NULL) {
		cc_log(LOG_ERROR,"Unable to allocate channel!\n");
		return(NULL);
	}
#ifndef CC_AST_HAVE_TECH_PVT
	if (tmp->pvt == NULL) {
	    	cc_log(LOG_ERROR, "CAPI: pvt structure not allocated.\n");
		ast_channel_free(tmp);
		return NULL;
	}
#endif

#ifdef CC_AST_HAS_STRINGFIELD_IN_CHANNEL
	ast_string_field_build(tmp, name, "CAPI/%s/%s-%x",
		i->name, i->dnid, capi_counter++);
#else
	snprintf(tmp->name, sizeof(tmp->name) - 1, "CAPI/%s/%s-%x",
		i->name, i->dnid, capi_counter++);
#endif
#ifdef CC_AST_HAS_TYPE_IN_CHANNEL
	tmp->type = channeltype;
#endif

	if (pipe(fds) != 0) {
		cc_log(LOG_ERROR, "%s: unable to create pipe.\n",
			i->name);
		ast_channel_free(tmp);
		return NULL;
	}
	i->readerfd = fds[0];
	i->writerfd = fds[1];
	flags = fcntl(i->readerfd, F_GETFL);
	fcntl(i->readerfd, F_SETFL, flags | O_NONBLOCK);
	flags = fcntl(i->writerfd, F_GETFL);
	fcntl(i->writerfd, F_SETFL, flags | O_NONBLOCK);

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

#ifdef CC_AST_HAVE_TECH_PVT
	tmp->tech = &capi_tech;
	tmp->rawreadformat = fmt;
	tmp->rawwriteformat = fmt;
#else
	tmp->pvt->call = pbx_capi_call;
	tmp->pvt->fixup = pbx_capi_fixup;
	tmp->pvt->indicate = pbx_capi_indicate;
	tmp->pvt->bridge = pbx_capi_bridge;
	tmp->pvt->answer = pbx_capi_answer;
	tmp->pvt->hangup = pbx_capi_hangup;
	tmp->pvt->read = pbx_capi_read;
	tmp->pvt->write = pbx_capi_write;
	tmp->pvt->send_digit = pbx_capi_send_digit;
	tmp->pvt->rawreadformat = fmt;
	tmp->pvt->rawwriteformat = fmt;
#endif
	cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: setting format %s - %s%s\n",
		i->name, ast_getformatname(fmt),
		ast_getformatname_multiple(alloca(80), 80,
		tmp->nativeformats),
		(i->rtp) ? " (RTP)" : "");
	cc_copy_string(tmp->context, i->context, sizeof(tmp->context));
#ifdef CC_AST_CHANNEL_HAS_CID
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
#else
	if (!ast_strlen_zero(i->cid)) {
		if (tmp->callerid) {
			free(tmp->callerid);
		}
		tmp->callerid = strdup(i->cid);
	}
	if (!ast_strlen_zero(i->dnid)) {
		if (tmp->dnid) {
			free(tmp->dnid);
		}
		tmp->dnid = strdup(i->dnid);
	}
#endif
	
	cc_copy_string(tmp->exten, i->dnid, sizeof(tmp->exten));
#ifdef CC_AST_HAS_STRINGFIELD_IN_CHANNEL
	ast_string_field_set(tmp, accountcode, i->accountcode);
	ast_string_field_set(tmp, language, i->language);
#else
	cc_copy_string(tmp->accountcode, i->accountcode, sizeof(tmp->accountcode));
	cc_copy_string(tmp->language, i->language, sizeof(tmp->language));
#endif
	i->owner = tmp;
	cc_mutex_lock(&usecnt_lock);
	usecnt++;
	cc_mutex_unlock(&usecnt_lock);
	ast_update_use_count();
	
	ast_setstate(tmp, state);

	return tmp;
}

/*
 * PBX wants us to dial ...
 */
static struct ast_channel *

#ifdef CC_AST_HAVE_TECH_PVT
pbx_capi_request(const char *type, int format, void *data, int *cause)
#else
pbx_capi_request(char *type, int format, void *data)
#endif
{
	struct capi_pvt *i;
	struct ast_channel *tmp = NULL;
	char *dest, *interface, *param, *ocid;
	char buffer[CAPI_MAX_STRING];
	ast_group_t capigroup = 0;
	unsigned int controller = 0;
	unsigned int foundcontroller;
	int notfound = 1;

	cc_verbose(1, 1, VERBOSE_PREFIX_4 "data = %s format=%d\n", (char *)data, format);

	cc_copy_string(buffer, (char *)data, sizeof(buffer));
	parse_dialstring(buffer, &interface, &dest, &param, &ocid);

	if ((!interface) || (!dest)) {
		cc_log(LOG_ERROR, "Syntax error in dialstring. Read the docs!\n");
#ifdef CC_AST_HAVE_TECH_PVT
		*cause = AST_CAUSE_INVALID_NUMBER_FORMAT;
#endif
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
	} else {
		cc_verbose(1, 1, VERBOSE_PREFIX_4 "capi request for interface '%s'\n",
				interface);
 	}

	cc_mutex_lock(&iflock);
	
	for (i = iflist; (i && notfound); i = i->next) {
		if ((i->owner) || (i->channeltype != CAPI_CHANNELTYPE_B)) {
			/* if already in use or no real channel */
			continue;
		}
		/* unused channel */
		cc_mutex_lock(&contrlock);
		if (controller) {
			/* DIAL(CAPI/contrX/...) */
			if ((!(i->controllers & (1 << controller))) ||
			    (capi_controllers[controller]->nfreebchannels < 1)) {
				/* keep on running! */
				cc_mutex_unlock(&contrlock);
				continue;
			}
			foundcontroller = controller;
		} else {
			/* DIAL(CAPI/gX/...) */
			if ((interface[0] == 'g') && (!(i->group & capigroup))) {
				/* keep on running! */
				cc_mutex_unlock(&contrlock);
				continue;
			}
			/* DIAL(CAPI/<interface-name>/...) */
			if ((interface[0] != 'g') && (strcmp(interface, i->name))) {
				/* keep on running! */
				cc_mutex_unlock(&contrlock);
				continue;
			}
			for (foundcontroller = 1; foundcontroller <= capi_num_controllers; foundcontroller++) {
				if ((i->controllers & (1 << foundcontroller)) &&
				    (capi_controllers[foundcontroller]->nfreebchannels > 0)) {
						break;
				}
			}
			if (foundcontroller > capi_num_controllers) {
				/* keep on running! */
				cc_mutex_unlock(&contrlock);
				continue;
			}
		}
		/* when we come here, we found a free controller match */
		cc_copy_string(i->dnid, dest, sizeof(i->dnid));
		i->controller = foundcontroller;
		tmp = capi_new(i, AST_STATE_RESERVED);
		if (!tmp) {
			cc_log(LOG_ERROR, "cannot create new capi channel\n");
			interface_cleanup(i);
		}
		i->PLCI = 0;
		i->outgoing = 1;	/* this is an outgoing line */
		cc_mutex_unlock(&contrlock);
		cc_mutex_unlock(&iflock);
		return tmp;
	}
	cc_mutex_unlock(&iflock);
	cc_verbose(2, 0, VERBOSE_PREFIX_3 "didn't find capi device for interface '%s'\n",
		interface);
#ifdef CC_AST_HAVE_TECH_PVT
	*cause = AST_CAUSE_REQUESTED_CHAN_UNAVAIL;
#endif
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
	char *filename, *stationid, *headline;
	B3_PROTO_FAXG3 b3conf;

	if (!data) { /* no data implies no filename or anything is present */
		cc_log(LOG_WARNING, "capi receivefax requires a filename\n");
		return -1;
	}

	cc_mutex_lock(&i->lock);

	filename = strsep(&data, "|");
	stationid = strsep(&data, "|");
	headline = data;

	if (!stationid)
		stationid = emptyid;
	if (!headline)
		headline = emptyid;

	if ((i->fFax = fopen(filename, "wb")) == NULL) {
		cc_mutex_unlock(&i->lock);
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
		cc_mutex_unlock(&i->lock);
		capi_send_answer(c, (_cstruct)&b3conf);
		break;
	case CAPI_STATE_CONNECTED:
		cc_mutex_unlock(&i->lock);
		capi_change_bchan_fax(c, &b3conf);
		break;
	default:
		i->FaxState &= ~CAPI_FAX_STATE_ACTIVE;
		cc_mutex_unlock(&i->lock);
		cc_log(LOG_WARNING, "capi receive fax in wrong state (%d)\n",
			i->state);
		return -1;
	}

	while (i->FaxState & CAPI_FAX_STATE_ACTIVE) {
		usleep(10000);
	}

	res = (i->FaxState & CAPI_FAX_STATE_ERROR) ? -1 : 0;
	i->FaxState &= ~(CAPI_FAX_STATE_ACTIVE | CAPI_FAX_STATE_ERROR);

	/* if the file has zero length */
	if (ftell(i->fFax) == 0L) {
		res = -1;
	}
			
	cc_verbose(2, 1, VERBOSE_PREFIX_3 "Closing fax file...\n");
	fclose(i->fFax);
	i->fFax = NULL;

	if (res != 0) {
		cc_verbose(2, 0,
			VERBOSE_PREFIX_1 "capi receivefax: fax receive failed reason=0x%04x reasonB3=0x%04x\n",
				i->reason, i->reasonb3);
		unlink(filename);
	} else {
		cc_verbose(2, 0,
			VERBOSE_PREFIX_1 "capi receivefax: fax receive successful.\n");
	}
	
	return res;
}

/*
 * Fax guard tone -- Handle and return NULL
 */
static void capi_handle_dtmf_fax(struct capi_pvt *i)
{
	struct ast_channel *c = i->owner;

	if (!c) {
		cc_log(LOG_ERROR, "No channel!\n");
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
			i->name);
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
		i->name, c->name);
			
	/* Save the DID/DNIS when we transfer the fax call to a "fax" extension */
	pbx_builtin_setvar_helper(c, "FAXEXTEN", c->exten);
	
	if (ast_async_goto(c, c->context, "fax", 1))
		cc_log(LOG_WARNING, "Failed to async goto '%s' into fax of '%s'\n", c->name, c->context);
	return;
}

/*
 * find the interface (pvt) the PLCI belongs to
 */
static struct capi_pvt *find_interface_by_plci(unsigned int plci)
{
	struct capi_pvt *i;

	if (plci == 0)
		return NULL;

	cc_mutex_lock(&iflock);
	for (i = iflist; i; i = i->next) {
		if (i->PLCI == plci)
			break;
	}
	cc_mutex_unlock(&iflock);

	return i;
}

/*
 * find the interface (pvt) the messagenumber belongs to
 */
static struct capi_pvt *find_interface_by_msgnum(unsigned short msgnum)
{
	struct capi_pvt *i;

	if (msgnum == 0x0000)
		return NULL;

	cc_mutex_lock(&iflock);
	for (i = iflist; i; i = i->next) {
		    if ((i->PLCI == 0) && (i->MessageNumber == msgnum))
			break;
	}
	cc_mutex_unlock(&iflock);

	return i;
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
			i->name, c->name, exten, c->context);
	} else {
		if (strlen(i->dnid) < strlen(i->incomingmsn))
			return 0;
		exten = i->dnid;
	}

	if (ast_exists_extension(NULL, c->context, exten, 1, i->cid)) {
		c->priority = 1;
		cc_copy_string(c->exten, exten, sizeof(c->exten));
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: %s: %s matches in context %s\n",
			i->name, c->name, exten, c->context);
		return 0;
	}

	if (ast_canmatch_extension(NULL, c->context, exten, 1, i->cid)) {
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: %s: %s would possibly match in context %s\n",
			i->name, c->name, exten, c->context);
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
			i->name);
		return;
	}

	switch(INFO_IND_INFOELEMENT(CMSG)[2] & 0x7f) {
	case 0x01:
		cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: Not end-to-end ISDN\n",
			i->name);
		break;
	case 0x02:
		cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: Destination is non ISDN\n",
			i->name);
		i->calledPartyIsISDN = 0;
		break;
	case 0x03:
		cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: Origination is non ISDN\n",
			i->name);
		break;
	case 0x04:
		cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: Call returned to ISDN\n",
			i->name);
		break;
	case 0x05:
		cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: Interworking occured\n",
			i->name);
		break;
	case 0x08:
		cc_verbose(4, 1, VERBOSE_PREFIX_4 "%s: In-band information available\n",
			i->name);
		break;
	default:
		cc_verbose(3, 1, VERBOSE_PREFIX_4 "%s: Unknown progress description %02x\n",
			i->name, INFO_IND_INFOELEMENT(CMSG)[2]);
	}
	send_progress(i);
	return;
}

/*
 * if the dnid matches, start the pbx
 */
static void start_pbx_on_match(struct capi_pvt *i, unsigned int PLCI, _cword MessageNumber)
{
	_cmsg CMSG2;

	if ((i->isdnstate & CAPI_ISDN_STATE_PBX)) {
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: pbx already started on channel %s\n",
			i->name, i->owner->name);
		return;
	}

	switch(search_did(i->owner)) {
	case 0: /* match */
		i->isdnstate |= CAPI_ISDN_STATE_PBX;
		ast_setstate(i->owner, AST_STATE_RING);
		if (ast_pbx_start(i->owner)) {
			cc_log(LOG_ERROR, "%s: Unable to start pbx on channel!\n",
				i->name);
			chan_to_hangup = i->owner;
		} else {
			cc_verbose(2, 1, VERBOSE_PREFIX_2 "Started pbx on channel %s\n",
				i->owner->name);
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
		i->isdnstate |= CAPI_ISDN_STATE_PBX; /* don't try again */
		cc_log(LOG_NOTICE, "%s: did not find exten for '%s', ignoring call.\n",
			i->name, i->dnid);
		CONNECT_RESP_HEADER(&CMSG2, capi_ApplID, MessageNumber, 0);
		CONNECT_RESP_PLCI(&CMSG2) = PLCI;
		CONNECT_RESP_REJECT(&CMSG2) = 1; /* ignore */
		_capi_put_cmsg(&CMSG2);
	}
	return;
}

/*
 * Called Party Number via INFO_IND
 */
static void capidev_handle_did_digits(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
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
			i->name);
		return;
	}

	did = capi_number(INFO_IND_INFOELEMENT(CMSG), 1);

	if ((!(i->isdnstate & CAPI_ISDN_STATE_DID)) && 
	    (strlen(i->dnid) && !strcasecmp(i->dnid, did))) {
		did = NULL;
	}

	if ((did) && (strlen(i->dnid) < (sizeof(i->dnid) - 1)))
		strcat(i->dnid, did);

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
static void queue_cause_control(struct capi_pvt *i, int control)
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
	_cmsg CMSG2;

	i->isdnstate |= CAPI_ISDN_STATE_DISCONNECT;

	if ((i->isdnstate & CAPI_ISDN_STATE_ECT)) {
		cc_verbose(4, 1, VERBOSE_PREFIX_3 "%s: Disconnect ECT call\n",
			i->name);
		/* we do nothing, just wait for DISCONNECT_IND */
		return;
	}

	if (PLCI == i->onholdPLCI) {
		cc_verbose(4, 1, VERBOSE_PREFIX_3 "%s: Disconnect onhold call\n",
			i->name);
		/* the caller onhold hung up (or ECTed away) */
		/* send a disconnect_req , we cannot hangup the channel here!!! */
		DISCONNECT_REQ_HEADER(&CMSG2, capi_ApplID, get_capi_MessageNumber(), 0);
		DISCONNECT_REQ_PLCI(&CMSG2) = i->onholdPLCI;
		_capi_put_cmsg(&CMSG2);
		return;
	}

	/* case 1: B3 on success or no B3 at all */
	if ((i->doB3 != CAPI_B3_ALWAYS) && (i->outgoing == 1)) {
		cc_verbose(4, 1, VERBOSE_PREFIX_3 "%s: Disconnect case 1\n",
			i->name);
		queue_cause_control(i, 1);
		return;
	}
	
	/* case 2: we are doing B3, and receive the 0x8045 after a successful call */
	if ((i->doB3 != CAPI_B3_DONT) &&
	    (i->state == CAPI_STATE_CONNECTED) && (i->outgoing == 1)) {
		cc_verbose(4, 1, VERBOSE_PREFIX_3 "%s: Disconnect case 2\n",
			i->name);
		queue_cause_control(i, 1);
		return;
	}

	/*
	 * case 3: this channel is an incoming channel! the user hung up!
	 * it is much better to hangup now instead of waiting for a timeout and
	 * network caused DISCONNECT_IND!
	 */
	if (i->outgoing == 0) {
		cc_verbose(4, 1, VERBOSE_PREFIX_3 "%s: Disconnect case 3\n",
			i->name);
		if (i->FaxState & CAPI_FAX_STATE_ACTIVE) {
			/* in fax mode, we just hangup */
			DISCONNECT_REQ_HEADER(&CMSG2, capi_ApplID, get_capi_MessageNumber(), 0);
			DISCONNECT_REQ_PLCI(&CMSG2) = i->PLCI;
			_capi_put_cmsg(&CMSG2);
			return;
		}
		queue_cause_control(i, 0);
		return;
	}
	
	/* case 4 (a.k.a. the italian case): B3 always. call is unsuccessful */
	if ((i->doB3 == CAPI_B3_ALWAYS) && (i->outgoing == 1)) {
		cc_verbose(4, 1, VERBOSE_PREFIX_3 "%s: Disconnect case 4\n",
			i->name);
		if ((i->state == CAPI_STATE_CONNECTED) &&
		    (i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
			queue_cause_control(i, 1);
			return;
		}
		/* wait for the 0x001e (PROGRESS), play audio and wait for a timeout from the network */
		return;
	}
	cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: Other case DISCONNECT INFO_IND\n",
		i->name);
	return;
}

/*
 * incoming call SETUP
 */
static void capidev_handle_setup_element(_cmsg *CMSG, unsigned int PLCI, struct capi_pvt *i)
{
	if ((i->isdnstate & CAPI_ISDN_STATE_SETUP)) {
		cc_verbose(3, 1, VERBOSE_PREFIX_4 "%s: IE SETUP / SENDING-COMPLETE already received.\n",
			i->name);
		return;
	}

	i->isdnstate |= CAPI_ISDN_STATE_SETUP;

	if (!i->owner) {
		cc_log(LOG_ERROR, "No channel for interface!\n");
		return;
	}

	if (i->isdnmode == CAPI_ISDNMODE_DID) {
		if (!strlen(i->dnid) && (i->immediate)) {
			start_pbx_on_match(i, PLCI, HEADER_MSGNUM(CMSG));
		}
	} else {
		start_pbx_on_match(i, PLCI, HEADER_MSGNUM(CMSG));
	}
	return;
}

/*
 * CAPI INFO_IND
 */
static void capidev_handle_info_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	_cmsg CMSG2;
	struct ast_frame fr = { AST_FRAME_NULL, };
	char *p = NULL;
	int val = 0;

	INFO_RESP_HEADER(&CMSG2, capi_ApplID, HEADER_MSGNUM(CMSG), PLCI);
	_capi_put_cmsg(&CMSG2);

	return_on_no_interface("INFO_IND");

	switch(INFO_IND_INFONUMBER(CMSG)) {
	case 0x0008:	/* Cause */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CAUSE %02x %02x\n",
			i->name, INFO_IND_INFOELEMENT(CMSG)[1], INFO_IND_INFOELEMENT(CMSG)[2]);
		if (i->owner) {
			i->owner->hangupcause = INFO_IND_INFOELEMENT(CMSG)[2] & 0x7f;
		}
		break;
	case 0x0014:	/* Call State */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CALL STATE %02x\n",
			i->name, INFO_IND_INFOELEMENT(CMSG)[1]);
		break;
	case 0x0018:	/* Channel Identification */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CHANNEL IDENTIFICATION %02x\n",
			i->name, INFO_IND_INFOELEMENT(CMSG)[1]);
		break;
	case 0x001c:	/*  Facility Q.932 */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element FACILITY\n",
			i->name);
		break;
	case 0x001e:	/* Progress Indicator */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element PI %02x %02x\n",
			i->name, INFO_IND_INFOELEMENT(CMSG)[1], INFO_IND_INFOELEMENT(CMSG)[2]);
		handle_progress_indicator(CMSG, PLCI, i);
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
			}
		}
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element NOTIFICATION INDICATOR '%s' (0x%02x)\n",
			i->name, desc, INFO_IND_INFOELEMENT(CMSG)[1]);
		break;
	}
	case 0x0028:	/* DSP */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element DSP\n",
			i->name);
		break;
	case 0x0029:	/* Date/Time */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element Date/Time %02d/%02d/%02d %02d:%02d\n",
			i->name,
			INFO_IND_INFOELEMENT(CMSG)[1], INFO_IND_INFOELEMENT(CMSG)[2],
			INFO_IND_INFOELEMENT(CMSG)[3], INFO_IND_INFOELEMENT(CMSG)[4],
			INFO_IND_INFOELEMENT(CMSG)[5]);
		break;
	case 0x0070:	/* Called Party Number */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CALLED PARTY NUMBER\n",
			i->name);
		capidev_handle_did_digits(CMSG, PLCI, NCCI, i);
		break;
	case 0x0074:	/* Redirecting Number */
		p = capi_number(INFO_IND_INFOELEMENT(CMSG), 3);
		if (INFO_IND_INFOELEMENT(CMSG)[0] > 2) {
			val = INFO_IND_INFOELEMENT(CMSG)[3] & 0x0f;
		}
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element REDIRECTING NUMBER '%s' Reason=0x%02x\n",
			i->name, p, val);
		if (i->owner) {
			char reasonbuf[16];
			snprintf(reasonbuf, sizeof(reasonbuf) - 1, "%d", val); 
			pbx_builtin_setvar_helper(i->owner, "REDIRECTINGNUMBER", p);
			pbx_builtin_setvar_helper(i->owner, "REDIRECTREASON", reasonbuf);
#ifdef CC_AST_CHANNEL_HAS_CID
			if (i->owner->cid.cid_rdnis) {
				free(i->owner->cid.cid_rdnis);
			}
			i->owner->cid.cid_rdnis = strdup(p);
#else
			if (i->owner->rdnis) {
				free(i->owner->rdnis);
			}
			i->owner->rdnis = strdup(p);
#endif
		}
		break;
	case 0x00a1:	/* Sending Complete */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element Sending Complete\n",
			i->name);
		capidev_handle_setup_element(CMSG, PLCI, i);
		break;
	case 0x4000:	/* CHARGE in UNITS */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CHARGE in UNITS\n",
			i->name);
		break;
	case 0x4001:	/* CHARGE in CURRENCY */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CHARGE in CURRENCY\n",
			i->name);
		break;
	case 0x8001:	/* ALERTING */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element ALERTING\n",
			i->name);
		send_progress(i);
		fr.frametype = AST_FRAME_CONTROL;
		fr.subclass = AST_CONTROL_RINGING;
		local_queue_frame(i, &fr);
		break;
	case 0x8002:	/* CALL PROCEEDING */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CALL PROCEEDING\n",
			i->name);
		fr.frametype = AST_FRAME_CONTROL;
		fr.subclass = AST_CONTROL_PROCEEDING;
		local_queue_frame(i, &fr);
		break;
	case 0x8003:	/* PROGRESS */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element PROGRESS\n",
			i->name);
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
				queue_cause_control(i, 1);
				break;
			}
		}
		send_progress(i);
		break;
	case 0x8005:	/* SETUP */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element SETUP\n",
			i->name);
		capidev_handle_setup_element(CMSG, PLCI, i);
		break;
	case 0x8007:	/* CONNECT */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element CONNECT\n",
			i->name);
		break;
	case 0x800d:	/* SETUP ACK */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element SETUP ACK\n",
			i->name);
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
			i->name);
		break;
	case 0x8045:	/* DISCONNECT */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element DISCONNECT\n",
			i->name);
		capidev_handle_info_disconnect(CMSG, PLCI, NCCI, i);
		break;
	case 0x804d:	/* RELEASE */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element RELEASE\n",
			i->name);
		break;
	case 0x805a:	/* RELEASE COMPLETE */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element RELEASE COMPLETE\n",
			i->name);
		break;
	case 0x8062:	/* FACILITY */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element FACILITY\n",
			i->name);
		break;
	case 0x806e:	/* NOTIFY */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element NOTIFY\n",
			i->name);
		break;
	case 0x807b:	/* INFORMATION */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element INFORMATION\n",
			i->name);
		break;
	case 0x807d:	/* STATUS */
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: info element STATUS\n",
			i->name);
		break;
	default:
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: unhandled INFO_IND %#x (PLCI=%#x)\n",
			i->name, INFO_IND_INFONUMBER(CMSG), PLCI);
		break;
	}
	return;
}

/*
 * CAPI FACILITY_IND
 */
static void capidev_handle_facility_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	_cmsg CMSG2;
	struct ast_frame fr = { AST_FRAME_NULL, };
	char dtmf;
	unsigned dtmflen;
	unsigned dtmfpos = 0;

	FACILITY_RESP_HEADER(&CMSG2, capi_ApplID, HEADER_MSGNUM(CMSG), PLCI);
	FACILITY_RESP_FACILITYSELECTOR(&CMSG2) = FACILITY_IND_FACILITYSELECTOR(CMSG);
	FACILITY_RESP_FACILITYRESPONSEPARAMETERS(&CMSG2) = FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG);
	_capi_put_cmsg(&CMSG2);
	
	return_on_no_interface("FACILITY_IND");

	if (FACILITY_IND_FACILITYSELECTOR(CMSG) == FACILITYSELECTOR_LINE_INTERCONNECT) {
		/* line interconnect */
		if ((FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[1] == 0x01) &&
		    (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[2] == 0x00)) {
			cc_verbose(3, 0, VERBOSE_PREFIX_3 "%s: Line Interconnect activated\n",
				i->name);
		}
		if ((FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[1] == 0x02) &&
		    (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[2] == 0x00) &&
		    (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[0] > 8)) {
			show_capi_info(read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[8]));
		}
	}
	
	if (FACILITY_IND_FACILITYSELECTOR(CMSG) == FACILITYSELECTOR_DTMF) {
		/* DTMF received */
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
				i->name, dtmf);
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
	}
	
	if (FACILITY_IND_FACILITYSELECTOR(CMSG) == FACILITYSELECTOR_SUPPLEMENTARY) {
		/* supplementary sservices */
		/* ECT */
		if ( (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[1] == 0x6) &&
		     (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[3] == 0x2) ) {
			cc_verbose(1, 1, VERBOSE_PREFIX_3 "%s: PLCI=%#x ECT  Reason=0x%02x%02x\n",
				i->name, PLCI,
				FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[5],
				FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[4]);
			show_capi_info(read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[4]));
		}

		/* RETRIEVE */
		if ( (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[1] == 0x3) &&
		     (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[3] == 0x2) ) {
			if ((FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[5] != 0) || 
			    (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[4] != 0)) { 
				cc_log(LOG_WARNING, "%s: unable to retrieve PLCI=%#x, REASON = 0x%02x%02x\n",
					i->name, PLCI,
					FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[5],
					FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[4]);
				show_capi_info(read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[4]));
			} else {
				/* reason != 0x0000 == problem */
				i->state = CAPI_STATE_CONNECTED;
				i->PLCI = i->onholdPLCI;
				i->onholdPLCI = 0;
				cc_verbose(1, 1, VERBOSE_PREFIX_3 "%s: PLCI=%#x retrieved\n",
					i->name, PLCI);
				cc_start_b3(i);
			}
		}
		
		/* HOLD */
		if ( (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[1] == 0x2) &&
		     (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[3] == 0x2) ) {
			if ((FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[5] != 0) || 
			    (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[4] != 0)) { 
				/* reason != 0x0000 == problem */
				i->onholdPLCI = 0;
				cc_log(LOG_WARNING, "%s: unable to put PLCI=%#x onhold, REASON = 0x%02x%02x, maybe you need to subscribe for this...\n",
					i->name, PLCI,
					FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[5],
					FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[4]);
				show_capi_info(read_capi_word(&FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[4]));
			} else {
				/* reason = 0x0000 == call on hold */
				i->state = CAPI_STATE_ONHOLD;
				cc_verbose(1, 1, VERBOSE_PREFIX_3 "%s: PLCI=%#x put onhold\n",
					i->name, PLCI);
			}
		}
	}
	return;
}

/*
 * CAPI DATA_B3_IND
 */
static void capidev_handle_data_b3_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	_cmsg CMSG2;
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
	DATA_B3_RESP_HEADER(&CMSG2, capi_ApplID, HEADER_MSGNUM(CMSG), 0);
	DATA_B3_RESP_NCCI(&CMSG2) = NCCI;
	DATA_B3_RESP_DATAHANDLE(&CMSG2) = DATA_B3_IND_DATAHANDLE(CMSG);
	_capi_put_cmsg(&CMSG2);

	return_on_no_interface("DATA_B3_IND");

	if (i->fFax) {
		/* we are in fax-receive and have a file open */
		cc_verbose(6, 1, VERBOSE_PREFIX_3 "%s: DATA_B3_IND (len=%d) Fax\n",
			i->name, b3len);
		if (fwrite(b3buf, 1, b3len, i->fFax) != b3len)
			cc_log(LOG_WARNING, "%s : error writing output file (%s)\n",
				i->name, strerror(errno));
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

	if ((i->doES == 1)) {
		for (j = 0; j < b3len; j++) {
			*(b3buf + j) = reversebits[*(b3buf + j)]; 
			if (capi_capability == AST_FORMAT_ULAW) {
				rxavg += abs(capiULAW2INT[ reversebits[*(b3buf + j)]]);
			} else {
				rxavg += abs(capiALAW2INT[ reversebits[*(b3buf + j)]]);
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
					i->name, rxavg, txavg);
		}
	} else {
		if (i->rxgain == 1.0) {
			for (j = 0; j < b3len; j++) {
				*(b3buf + j) = reversebits[*(b3buf + j)];
			}
		} else {
			for (j = 0; j < b3len; j++) {
				*(b3buf + j) = reversebits[i->g.rxgains[*(b3buf + j)]];
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
#ifdef CC_AST_FRAME_HAS_TIMEVAL
	fr.delivery.tv_sec = 0;
	fr.delivery.tv_usec = 0;
#endif
	fr.src = NULL;
	cc_verbose(8, 1, VERBOSE_PREFIX_3 "%s: DATA_B3_IND (len=%d) fr.datalen=%d fr.subclass=%d\n",
		i->name, b3len, fr.datalen, fr.subclass);
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
 * CAPI CONNECT_ACTIVE_IND
 */
static void capidev_handle_connect_active_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	_cmsg CMSG2;
	
	CONNECT_ACTIVE_RESP_HEADER(&CMSG2, capi_ApplID, HEADER_MSGNUM(CMSG), 0);
	CONNECT_ACTIVE_RESP_PLCI(&CMSG2) = PLCI;
	_capi_put_cmsg(&CMSG2);
	
	return_on_no_interface("CONNECT_ACTIVE_IND");

	if (i->state == CAPI_STATE_DISCONNECTING) {
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: CONNECT_ACTIVE in DISCONNECTING.\n",
			i->name);
		return;
	}

	i->state = CAPI_STATE_CONNECTED;

	if ((i->owner) && (i->FaxState & CAPI_FAX_STATE_ACTIVE)) {
		capi_signal_answer(i);
		return;
	}
	
	/* normal processing */
			    
	if (!(i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
		/* send a CONNECT_B3_REQ */
		if (i->outgoing == 1) {
			/* outgoing call */
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
	_cmsg CMSG2;

	/* then send a CONNECT_B3_ACTIVE_RESP */
	CONNECT_B3_ACTIVE_RESP_HEADER(&CMSG2, capi_ApplID, HEADER_MSGNUM(CMSG), 0);
	CONNECT_B3_ACTIVE_RESP_NCCI(&CMSG2) = NCCI;
	_capi_put_cmsg(&CMSG2);

	return_on_no_interface("CONNECT_ACTIVE_B3_IND");

	cc_mutex_lock(&contrlock);
	if (i->controller > 0) {
		capi_controllers[i->controller]->nfreebchannels--;
	}
	cc_mutex_unlock(&contrlock);

	if (i->state == CAPI_STATE_DISCONNECTING) {
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: CONNECT_B3_ACTIVE_IND during disconnect for NCCI %#x\n",
			i->name, NCCI);
		return;
	}

	i->isdnstate |= CAPI_ISDN_STATE_B3_UP;
	i->isdnstate &= ~CAPI_ISDN_STATE_B3_PEND;

	if (i->bproto == CC_BPROTO_RTP) {
		i->isdnstate |= CAPI_ISDN_STATE_RTP;
	} else {
		i->isdnstate &= ~CAPI_ISDN_STATE_RTP;
	}

	if ((i->isdnstate & CAPI_ISDN_STATE_B3_CHANGE)) {
		i->isdnstate &= ~CAPI_ISDN_STATE_B3_CHANGE;
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: B3 protocol changed.\n",
			i->name);
		return;
	}

	if (!i->owner) {
		cc_log(LOG_ERROR, "%s: No channel for interface!\n",
			i->name);
		return;
	}

	if (i->FaxState & CAPI_FAX_STATE_ACTIVE) {
		cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: Fax connection, no EC/DTMF\n",
			i->name);
	} else {
		capi_echo_canceller(i->owner, EC_FUNCTION_ENABLE);
		capi_detect_dtmf(i->owner, 1);
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
	_cmsg CMSG2;

	DISCONNECT_B3_RESP_HEADER(&CMSG2, capi_ApplID, HEADER_MSGNUM(CMSG), 0);
	DISCONNECT_B3_RESP_NCCI(&CMSG2) = NCCI;
	_capi_put_cmsg(&CMSG2);

	return_on_no_interface("DISCONNECT_B3_IND");

	i->isdnstate &= ~(CAPI_ISDN_STATE_B3_UP | CAPI_ISDN_STATE_B3_PEND);

	i->reasonb3 = DISCONNECT_B3_IND_REASON_B3(CMSG);
	i->NCCI = 0;

	if ((i->FaxState & CAPI_FAX_STATE_ACTIVE) && (i->owner)) {
		char buffer[CAPI_MAX_STRING];
		unsigned char *ncpi = (unsigned char *)DISCONNECT_B3_IND_NCPI(CMSG);
		/* if we have fax infos, set them as variables */
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

	if (i->state == CAPI_STATE_DISCONNECTING) {
		/* active disconnect */
		DISCONNECT_REQ_HEADER(&CMSG2, capi_ApplID, get_capi_MessageNumber(), 0);
		DISCONNECT_REQ_PLCI(&CMSG2) = PLCI;
		_capi_put_cmsg(&CMSG2);
	}

	cc_mutex_lock(&contrlock);
	if (i->controller > 0) {
		capi_controllers[i->controller]->nfreebchannels++;
	}
	cc_mutex_unlock(&contrlock);
}

/*
 * CAPI CONNECT_B3_IND
 */
static void capidev_handle_connect_b3_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	_cmsg CMSG2;

	/* then send a CONNECT_B3_RESP */
	CONNECT_B3_RESP_HEADER(&CMSG2, capi_ApplID, HEADER_MSGNUM(CMSG), 0);
	CONNECT_B3_RESP_NCCI(&CMSG2) = NCCI;
	CONNECT_B3_RESP_REJECT(&CMSG2) = 0;
	CONNECT_B3_RESP_NCPI(&CMSG2) = capi_rtp_ncpi(i);
	_capi_put_cmsg(&CMSG2);

	return_on_no_interface("CONNECT_B3_IND");

	i->NCCI = NCCI;

	return;
}

/*
 * CAPI DISCONNECT_IND
 */
static void capidev_handle_disconnect_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	_cmsg CMSG2;
	struct ast_frame fr = { AST_FRAME_CONTROL, AST_CONTROL_HANGUP, };
	int state;

	DISCONNECT_RESP_HEADER(&CMSG2, capi_ApplID, HEADER_MSGNUM(CMSG) , 0);
	DISCONNECT_RESP_PLCI(&CMSG2) = PLCI;
	_capi_put_cmsg(&CMSG2);
	
	show_capi_info(DISCONNECT_IND_REASON(CMSG));

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
	    (i->owner->pbx == NULL)) {
		/* the pbx was not started yet */
		cc_verbose(4, 1, VERBOSE_PREFIX_3 "%s: DISCONNECT_IND on incoming without pbx, doing hangup.\n",
			i->name);
		chan_to_hangup = i->owner;
		return;
	}

	if (DISCONNECT_IND_REASON(CMSG) == 0x34a2) {
		fr.subclass = AST_CONTROL_CONGESTION;
	}

	if (state == CAPI_STATE_DISCONNECTING) {
		interface_cleanup(i);
	} else {
		local_queue_frame(i, &fr);
	}
	return;
}

/*
 * CAPI CONNECT_IND
 */
static void capidev_handle_connect_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt **interface)
{
	struct capi_pvt *i;
	_cmsg CMSG2;
	char *DNID;
	char *CID;
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

	DNID = capi_number(CONNECT_IND_CALLEDPARTYNUMBER(CMSG), 1);
	if (!DNID) {
		DNID = emptydnid;
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

	if (CONNECT_IND_BCHANNELINFORMATION(CMSG)) {
		bchannelinfo[0] = CONNECT_IND_BCHANNELINFORMATION(CMSG)[1] + '0';
	}

	/* well...somebody is calling us. let's set up a channel */
	cc_mutex_lock(&iflock);
	for (i = iflist; i; i = i->next) {
		if (i->owner) {
			/* has already owner */
			continue;
		}
		if (!(i->controllers & (1 << controller))) {
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
					i->name, msn, DNID,
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
			i->controller = controller;
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
#ifdef CC_AST_CHANNEL_HAS_TRANSFERCAP	
 			i->owner->transfercapability = cip2tcap(i->cip);
			if (tcap_is_digital(i->owner->transfercapability)) {
				i->bproto = CC_BPROTO_TRANSPARENT;
			}
#else
#endif
#ifdef CC_AST_CHANNEL_HAS_CID
			i->owner->cid.cid_pres = callpres;
#else    
			i->owner->callingpres = callpres;
#endif
			cc_verbose(3, 0, VERBOSE_PREFIX_2 "%s: Incoming call '%s' -> '%s'\n",
				i->name, i->cid, i->dnid);

			*interface = i;
			cc_mutex_unlock(&iflock);
			cc_mutex_lock(&i->lock);
		
#ifdef CC_AST_CHANNEL_HAS_TRANSFERCAP	
			pbx_builtin_setvar_helper(i->owner, "TRANSFERCAPABILITY", transfercapability2str(i->owner->transfercapability));
#endif
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
	
	CONNECT_RESP_HEADER(&CMSG2, capi_ApplID, HEADER_MSGNUM(CMSG), 0);
	CONNECT_RESP_PLCI(&CMSG2) = CONNECT_IND_PLCI(CMSG);
	CONNECT_RESP_REJECT(&CMSG2) = 1; /* ignore */
	_capi_put_cmsg(&CMSG2);
	return;
}

/*
 * CAPI FACILITY_CONF
 */
static void capidev_handle_facility_confirmation(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pvt *i)
{
	int selector;

	if (i == NULL)
		return;

	selector = FACILITY_CONF_FACILITYSELECTOR(CMSG);

	if (selector == FACILITYSELECTOR_DTMF) {
		cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: DTMF conf(PLCI=%#x)\n",
			i->name, PLCI);
		return;
	}
	if (selector == i->ecSelector) {
		if (FACILITY_CONF_INFO(CMSG)) {
			cc_verbose(2, 0, VERBOSE_PREFIX_3 "%s: Error setting up echo canceller (PLCI=%#x)\n",
				i->name, PLCI);
			return;
		}
		if (FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[1] == EC_FUNCTION_DISABLE) {
			cc_verbose(2, 0, VERBOSE_PREFIX_3 "%s: Echo canceller successfully disabled (PLCI=%#x)\n",
				i->name, PLCI);
		} else {
			cc_verbose(2, 0, VERBOSE_PREFIX_3 "%s: Echo canceller successfully set up (PLCI=%#x)\n",
				i->name, PLCI);
		}
		return;
	}
	if (selector == FACILITYSELECTOR_SUPPLEMENTARY) {
		/* HOLD */
		if ((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[1] == 0x2) &&
		    (FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[2] == 0x0) &&
		    ((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[4] != 0x0) ||
		     (FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[5] != 0x0))) {
			cc_verbose(2, 0, VERBOSE_PREFIX_3 "%s: Call on hold (PLCI=%#x)\n",
				i->name, PLCI);
		}
		return;
	}
	if (selector == FACILITYSELECTOR_LINE_INTERCONNECT) {
		if ((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[1] == 0x1) &&
		    (FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[2] == 0x0)) {
			/* enable */
			if (FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[0] > 12) {
				show_capi_info(read_capi_word(&FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[12]));
			}
		} else {
			/* disable */
			if (FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[0] > 12) {
				show_capi_info(read_capi_word(&FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[12]));
			}
		}
		return;
	}
	cc_log(LOG_ERROR, "%s: unhandled FACILITY_CONF 0x%x\n",
		i->name, FACILITY_CONF_FACILITYSELECTOR(CMSG));
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
		name = i->name;
	
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
	unsigned short capicommand = ((CMSG->Subcommand << 8)|(CMSG->Command));

	if (i->waitevent == capicommand) {
		i->waitevent = 0;
		ast_cond_signal(&i->event_trigger);
		cc_verbose(4, 1, "%s: found and signal for %s\n",
			i->name, capi_cmd2str(CMSG->Command, CMSG->Subcommand));
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
	struct capi_pvt *i = find_interface_by_plci(PLCI);

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

	  /*
	   * CAPI confirmations
	   */

	case CAPI_P_CONF(FACILITY):
		wInfo = FACILITY_CONF_INFO(CMSG);
		capidev_handle_facility_confirmation(CMSG, PLCI, NCCI, i);
		break;
	case CAPI_P_CONF(CONNECT):
		wInfo = CONNECT_CONF_INFO(CMSG);
		if (i) {
			cc_log(LOG_ERROR, "CAPI: CONNECT_CONF for already "
				"defined interface received\n");
			break;
		}
		i = find_interface_by_msgnum(wMsgNum);
		if ((i == NULL) || (!i->owner))
			break;
		cc_verbose(1, 1, VERBOSE_PREFIX_3 "%s: received CONNECT_CONF PLCI = %#x\n",
			i->name, PLCI);
		if (wInfo == 0) {
			i->PLCI = PLCI;
		} else {
			/* here, something has to be done --> */
			struct ast_frame fr = { AST_FRAME_CONTROL, AST_CONTROL_BUSY, };
			local_queue_frame(i, &fr);
		}
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
			if ((i->owner) && (i->FaxState & CAPI_FAX_STATE_ACTIVE)) {
				capi_echo_canceller(i->owner, EC_FUNCTION_DISABLE);
				capi_detect_dtmf(i->owner, 0);
			}
		}
		break;
	case CAPI_P_CONF(DATA_B3):
		wInfo = DATA_B3_CONF_INFO(CMSG);
		if ((i) && (i->B3q > 0) && (i->isdnstate & CAPI_ISDN_STATE_RTP)) {
			i->B3q--;
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
		show_capi_info(wInfo);
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
	_cmsg	CMSG;
	char	fac[64];
	int	res = 0;
	char *number;
	int numberlen;

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
			i->name, c->name);
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
	
	fac[0] = 0x0a + numberlen; /* length */
	fac[1] = 0x0d; /* call deflection */
	fac[2] = 0x00;
	fac[3] = 0x07 + numberlen; /* struct len */
	fac[4] = 0x01; /* display of own address allowed */
	fac[5] = 0x00;
	fac[6] = 0x03 + numberlen;
	fac[7] = 0x00; /* type of facility number */
	fac[8] = 0x00; /* number plan */
	fac[9] = 0x00; /* presentation allowed */
	fac[10 + numberlen] = 0x00; /* subaddress len */

	memcpy((unsigned char *)fac + 10, number, numberlen);

	FACILITY_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(),0);
	FACILITY_REQ_PLCI(&CMSG) = i->PLCI;
	FACILITY_REQ_FACILITYSELECTOR(&CMSG) = FACILITYSELECTOR_SUPPLEMENTARY;
	FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = (_cstruct)&fac;
	
	_capi_put_cmsg_wait_conf(i, &CMSG);

	cc_mutex_unlock(&i->lock);

	cc_verbose(2, 1, VERBOSE_PREFIX_3 "%s: sent FACILITY_REQ for CD PLCI = %#x\n",
		i->name, i->PLCI);

	return(res);
}

/*
 * retrieve a hold on call
 */
static int pbx_capi_retrieve(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c); 
	_cmsg	CMSG;
	char	fac[4];
	unsigned int plci = 0;

#ifdef CC_AST_HAVE_TECH_PVT
	if (c->tech->type == channeltype) {
#else
	if (!(strcmp(c->type, "CAPI"))) {
#endif
		plci = i->onholdPLCI;
	} else {
		i = NULL;
	}

	if (param) {
		plci = (unsigned int)strtoul(param, NULL, 0);
		cc_mutex_lock(&iflock);
		for (i = iflist; i; i = i->next) {
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
			i->name, plci);
		return 0;
	}
	cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: using PLCI=%#x for retrieve\n",
		i->name, plci);

	if (!(capi_controllers[i->controller]->holdretrieve)) {
		cc_log(LOG_NOTICE,"%s: RETRIEVE for %s not supported by controller.\n",
			i->name, c->name);
		return -1;
	}

	fac[0] = 3;	/* len */
	fac[1] = 0x03;	/* retrieve */
	fac[2] = 0x00;
	fac[3] = 0;	

	FACILITY_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(),0);
	FACILITY_REQ_PLCI(&CMSG) = plci;
	FACILITY_REQ_FACILITYSELECTOR(&CMSG) = FACILITYSELECTOR_SUPPLEMENTARY;
	FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = (_cstruct)&fac;

	_capi_put_cmsg(&CMSG);
	cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: sent RETRIEVE for PLCI=%#x\n",
		i->name, plci);

	i->isdnstate &= ~CAPI_ISDN_STATE_HOLD;
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
	_cmsg CMSG;
	char fac[8];
	const char *id;
	unsigned int plci = 0;

	if ((id = pbx_builtin_getvar_helper(c, "CALLERHOLDID"))) {
		plci = (unsigned int)strtoul(id, NULL, 0);
	}
	
	if (param) {
		plci = (unsigned int)strtoul(param, NULL, 0);
	}

	if (!plci) {
		cc_log(LOG_WARNING, "%s: No id for ECT !\n", i->name);
		return -1;
	}

	cc_mutex_lock(&iflock);
	for (ii = iflist; ii; ii = ii->next) {
		if (ii->onholdPLCI == plci)
			break;
	}
	cc_mutex_unlock(&iflock);

	if (!ii) {
		cc_log(LOG_WARNING, "%s: 0x%x is not on hold !\n",
			i->name, plci);
		return -1;
	}

	cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: using PLCI=%#x for ECT\n",
		i->name, plci);

	if (!(capi_controllers[i->controller]->ECT)) {
		cc_log(LOG_WARNING, "%s: ECT for %s not supported by controller.\n",
			i->name, c->name);
		return -1;
	}

	if (!(ii->isdnstate & CAPI_ISDN_STATE_HOLD)) {
		cc_log(LOG_WARNING, "%s: PLCI %#x (%s) is not on hold for ECT\n",
			i->name, plci, ii->name);
		return -1;
	}

	cc_mutex_lock(&i->lock);

	cc_disconnect_b3(i, 1);

	if (i->state != CAPI_STATE_CONNECTED) {
		cc_log(LOG_WARNING, "%s: destination not connected for ECT\n",
			i->name);
		cc_mutex_unlock(&i->lock);
		return -1;
	}

	fac[0] = 7;	/* len */
	fac[1] = 0x06;	/* ECT (function) */
	fac[2] = 0x00;
	fac[3] = 4;	/* len / sservice specific parameter , cstruct */
	write_capi_dword(&(fac[4]), plci);

	FACILITY_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(), 0);
	FACILITY_REQ_CONTROLLER(&CMSG) = i->controller;
	FACILITY_REQ_PLCI(&CMSG) = plci; /* implicit ECT */
	FACILITY_REQ_FACILITYSELECTOR(&CMSG) = FACILITYSELECTOR_SUPPLEMENTARY;
	FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = (_cstruct)&fac;

	_capi_put_cmsg_wait_conf(i, &CMSG);
	
	ii->isdnstate &= ~CAPI_ISDN_STATE_HOLD;
	ii->isdnstate |= CAPI_ISDN_STATE_ECT;
	i->isdnstate |= CAPI_ISDN_STATE_ECT;
	
	cc_mutex_unlock(&i->lock);

	cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: sent ECT for PLCI=%#x to PLCI=%#x\n",
		i->name, plci, i->PLCI);

	return 0;
}

/*
 * hold a call
 */
static int pbx_capi_hold(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);
	_cmsg	CMSG;
	char buffer[16];
	char	fac[4];

	/*  TODO: support holdtype notify */

	if ((i->isdnstate & CAPI_ISDN_STATE_HOLD)) {
		cc_log(LOG_NOTICE,"%s: %s already on hold.\n",
			i->name, c->name);
		return 0;
	}

	if (!(i->isdnstate & CAPI_ISDN_STATE_B3_UP)) {
		cc_log(LOG_NOTICE,"%s: Cannot put on hold %s while not connected.\n",
			i->name, c->name);
		return 0;
	}
	if (!(capi_controllers[i->controller]->holdretrieve)) {
		cc_log(LOG_NOTICE,"%s: HOLD for %s not supported by controller.\n",
			i->name, c->name);
		return 0;
	}

	fac[0] = 3;	/* len */
	fac[1] = 0x02;	/* this is a HOLD up */
	fac[2] = 0x00;
	fac[3] = 0;	

	FACILITY_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(),0);
	FACILITY_REQ_PLCI(&CMSG) = i->PLCI;
	FACILITY_REQ_FACILITYSELECTOR(&CMSG) = FACILITYSELECTOR_SUPPLEMENTARY;
	FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = (_cstruct)&fac;

	_capi_put_cmsg(&CMSG);
	cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: sent HOLD for PLCI=%#x\n",
		i->name, i->PLCI);

	i->onholdPLCI = i->PLCI;
	i->isdnstate |= CAPI_ISDN_STATE_HOLD;

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
	_cmsg	CMSG;
	char	fac[4];

	if (!(capi_controllers[i->controller]->MCID)) {
		cc_log(LOG_NOTICE, "%s: MCID for %s not supported by controller.\n",
			i->name, c->name);
		return -1;
	}

	fac[0] = 3;      /* len */
	fac[1] = 0x0e;   /* MCID */
	fac[2] = 0x00;
	fac[3] = 0;	

	FACILITY_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(),0);
	FACILITY_REQ_PLCI(&CMSG) = i->PLCI;
	FACILITY_REQ_FACILITYSELECTOR(&CMSG) = FACILITYSELECTOR_SUPPLEMENTARY;
	FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = (_cstruct)&fac;

	cc_mutex_lock(&i->lock);
	_capi_put_cmsg_wait_conf(i, &CMSG);
	cc_mutex_unlock(&i->lock);

	cc_verbose(2, 1, VERBOSE_PREFIX_4 "%s: sent MCID for PLCI=%#x\n",
		i->name, i->PLCI);

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
		capi_echo_canceller(c, EC_FUNCTION_ENABLE);
	} else if (ast_false(param)) {
		capi_echo_canceller(c, EC_FUNCTION_DISABLE);
		i->doEC = 0;
	} else {
		cc_log(LOG_WARNING, "Parameter for echocancel invalid.\n");
		return -1;
	}
	cc_verbose(2, 0, VERBOSE_PREFIX_4 "%s: echocancel switched %s\n",
		i->name, i->doEC ? "ON":"OFF");
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
		i->name, i->doES ? "ON":"OFF");
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
		i->name, param);
	return 0;
}

/*
 * set early-B3 (progress) for incoming connections
 * (only for NT mode)
 */
static int pbx_capi_signal_progress(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = CC_CHANNEL_PVT(c);

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
			i->name);
		return 0;
	}

	cc_select_b(i, NULL);

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
	{ "progress",     pbx_capi_signal_progress, 1 },
	{ "deflect",      pbx_capi_call_deflect,    1 },
	{ "receivefax",   pbx_capi_receive_fax,     1 },
	{ "echosquelch",  pbx_capi_echosquelch,     1 },
	{ "echocancel",   pbx_capi_echocancel,      1 },
	{ "malicious",    pbx_capi_malicious,       1 },
	{ "hold",         pbx_capi_hold,            1 },
	{ "holdtype",     pbx_capi_holdtype,        1 },
	{ "retrieve",     pbx_capi_retrieve,        0 },
	{ "ect",          pbx_capi_ect,             1 },
	{ NULL, NULL, 0 }
};

/*
 * capi command interface
 */
static int pbx_capicommand_exec(struct ast_channel *chan, void *data)
{
	int res = 0;
	struct localuser *u;
	char *s;
	char *stringp;
	char *command, *params;
	struct capicommands_s *capicmd = &capicommands[0];

	if (!data) {
		cc_log(LOG_WARNING, "capiCommand requires arguments\n");
		return -1;
	}

	LOCAL_USER_ADD(u);

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
		LOCAL_USER_REMOVE(u);
		cc_log(LOG_WARNING, "Unknown command '%s' for capiCommand\n",
			command);
		return -1;
	}

#ifdef CC_AST_HAVE_TECH_PVT
	if ((capicmd->capionly) && (chan->tech->type != channeltype)) {
#else
	if ((capicmd->capionly) && (strcmp(chan->type, "CAPI"))) {
#endif
		LOCAL_USER_REMOVE(u);
		cc_log(LOG_WARNING, "capiCommand works on CAPI channels only, check your extensions.conf!\n");
		return -1;
	}

	res = (capicmd->cmd)(chan, params);
	
	LOCAL_USER_REMOVE(u);
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
	_cmsg CMSG;
	int ret = -1;

	if (i == NULL) {
		return -1;
	}

	cc_mutex_lock(&i->lock);

	switch (condition) {
	case AST_CONTROL_RINGING:
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested RINGING-Indication for %s\n",
			i->name, c->name);
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
			i->name, c->name);
		if ((i->state == CAPI_STATE_ALERTING) ||
		    (i->state == CAPI_STATE_DID) || (i->state == CAPI_STATE_INCALL)) {
			CONNECT_RESP_HEADER(&CMSG, capi_ApplID, i->MessageNumber, 0);
			CONNECT_RESP_PLCI(&CMSG) = i->PLCI;
			CONNECT_RESP_REJECT(&CMSG) = 3;
			_capi_put_cmsg(&CMSG);
			ret = 0;
		}
		if ((i->isdnstate & CAPI_ISDN_STATE_HOLD))
			pbx_capi_retrieve(c, NULL);
		break;
	case AST_CONTROL_CONGESTION:
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested CONGESTION-Indication for %s\n",
			i->name, c->name);
		if ((i->state == CAPI_STATE_ALERTING) ||
		    (i->state == CAPI_STATE_DID) || (i->state == CAPI_STATE_INCALL)) {
			CONNECT_RESP_HEADER(&CMSG, capi_ApplID, i->MessageNumber, 0);
			CONNECT_RESP_PLCI(&CMSG) = i->PLCI;
			CONNECT_RESP_REJECT(&CMSG) = 4;
			_capi_put_cmsg(&CMSG);
			ret = 0;
		}
		if ((i->isdnstate & CAPI_ISDN_STATE_HOLD))
			pbx_capi_retrieve(c, NULL);
		break;
	case AST_CONTROL_PROGRESS:
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested PROGRESS-Indication for %s\n",
			i->name, c->name);
		if (i->ntmode) pbx_capi_signal_progress(c, NULL);
		break;
	case AST_CONTROL_PROCEEDING:
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested PROCEEDING-Indication for %s\n",
			i->name, c->name);
		if (i->ntmode) pbx_capi_signal_progress(c, NULL);
		break;
#ifdef CC_AST_CONTROL_HOLD
	case AST_CONTROL_HOLD:
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested HOLD-Indication for %s\n",
			i->name, c->name);
		if (i->doholdtype != CC_HOLDTYPE_LOCAL) {
			ret = pbx_capi_hold(c, NULL);
		}
		break;
	case AST_CONTROL_UNHOLD:
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested UNHOLD-Indication for %s\n",
			i->name, c->name);
		if (i->doholdtype != CC_HOLDTYPE_LOCAL) {
			ret = pbx_capi_retrieve(c, NULL);
		}
		break;
#endif
	case -1: /* stop indications */
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested Indication-STOP for %s\n",
			i->name, c->name);
		if ((i->isdnstate & CAPI_ISDN_STATE_HOLD))
			pbx_capi_retrieve(c, NULL);
		break;
	default:
		cc_verbose(3, 1, VERBOSE_PREFIX_2 "%s: Requested unknown Indication %d for %s\n",
			i->name, condition, c->name);
		break;
	}
	cc_mutex_unlock(&i->lock);
	return(ret);
}

#ifndef CC_AST_NO_DEVICESTATE
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
#endif

/*
 * module stuff, monitor...
 */
static void *capidev_loop(void *data)
{
	unsigned int Info;
	_cmsg monCMSG;
	
	for (/* for ever */;;) {
		switch(Info = capidev_check_wait_get_cmsg(&monCMSG)) {
		case 0x0000:
			capidev_handle_msg(&monCMSG);
			if (chan_to_hangup != NULL) {
				/* deferred (out of lock) hangup */
				ast_hangup(chan_to_hangup);
				chan_to_hangup = NULL;
			}
			if (chan_to_softhangup != NULL) {
				/* deferred (out of lock) soft-hangup */
				ast_softhangup(chan_to_softhangup, AST_SOFTHANGUP_DEV);
				chan_to_softhangup = NULL;
			}
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
	} /* for */
	
	/* never reached */
	return NULL;
}

/*
 * GAIN
 */
static void capi_gains(struct cc_capi_gains *g, float rxgain, float txgain)
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
	char buffer[CAPI_MAX_STRING];
	char buffer_r[CAPI_MAX_STRING];
	char *buffer_rp = buffer_r;
	char *contr;
	unsigned long contrmap = 0;

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
		cc_copy_string(tmp->context, conf->context, sizeof(tmp->context));
		cc_copy_string(tmp->incomingmsn, conf->incomingmsn, sizeof(tmp->incomingmsn));
		cc_copy_string(tmp->defaultcid, conf->defaultcid, sizeof(tmp->defaultcid));
		cc_copy_string(tmp->prefix, conf->prefix, sizeof(tmp->prefix));
		cc_copy_string(tmp->accountcode, conf->accountcode, sizeof(tmp->accountcode));
		cc_copy_string(tmp->language, conf->language, sizeof(tmp->language));

		cc_copy_string(buffer, conf->controllerstr, sizeof(buffer));
		contr = strtok_r(buffer, ",", &buffer_rp);
		while (contr != NULL) {
			u_int16_t unit = atoi(contr);
 
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

			/* always range check user input */
 
			if (unit >= CAPI_MAX_CONTROLLERS)
				unit = CAPI_MAX_CONTROLLERS - 1;

			contrmap |= (1 << unit);
			contr = strtok_r(NULL, ",", &buffer_rp);
		}
		
		tmp->controllers = contrmap;
		capi_used_controllers |= contrmap;
		tmp->doEC = conf->echocancel;
		tmp->ecOption = conf->ecoption;
		if (conf->ecnlp) tmp->ecOption |= 0x01; /* bit 0 of ec-option is NLP */
		tmp->ecTail = conf->ectail;
		tmp->isdnmode = conf->isdnmode;
		tmp->ntmode = conf->ntmode;
		tmp->ES = conf->es;
		tmp->callgroup = conf->callgroup;
		tmp->group = conf->group;
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

		tmp->next = iflist; /* prepend */
		iflist = tmp;
		cc_verbose(2, 0, VERBOSE_PREFIX_3 "capi_pvt %s (%s,%s,%lu,%d) (%d,%d,%d)\n",
			tmp->name, tmp->incomingmsn, tmp->context, tmp->controllers,
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
	_cmsg CMSG, CMSG2;
	struct timeval tv;
	unsigned char fac[20];
	unsigned int services;

	memset(fac, 0, sizeof(fac));
	FACILITY_REQ_HEADER(&CMSG, capi_ApplID, get_capi_MessageNumber(), 0);
	FACILITY_REQ_CONTROLLER(&CMSG) = cp->controller;
	FACILITY_REQ_FACILITYSELECTOR(&CMSG) = FACILITYSELECTOR_SUPPLEMENTARY;
	fac[0] = 3;
	FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = (_cstruct)&fac;
	_capi_put_cmsg(&CMSG);

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
	
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	
	ast_cli(fd, "CAPI B-channel information:\n");
	ast_cli(fd, "Line-Name       NTmode state i/o bproto isdnstate   ton  number\n");
	ast_cli(fd, "----------------------------------------------------------------\n");

	cc_mutex_lock(&iflock);

	for (i = iflist; i; i = i->next) {
		if (i->channeltype != CAPI_CHANNELTYPE_B)
			continue;

		if ((i->state == 0) || (i->state == CAPI_STATE_DISCONNECTED))
			iochar = '-';
		else if (i->outgoing)
			iochar = 'O';
		else
			iochar = 'I';

		ast_cli(fd,
			"%-16s %s   %s  %c  %s  %-10s  0x%02x '%s'->'%s'\n",
			i->name,
			i->ntmode ? "yes":"no ",
			show_state(i->state),
			iochar,
			show_bproto(i->bproto),
			show_isdnstate(i->isdnstate, i_state),
			i->cid_ton,
			i->cid,
			i->dnid
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
		
	for (i = 1; i <= capi_num_controllers; i++) {
		cc_mutex_lock(&contrlock);
		if (capi_controllers[i] != NULL) {
			ast_cli(fd, "Contr%d: %d B channels total, %d B channels free.\n",
				i, capi_controllers[i]->nbchannels,
				capi_controllers[i]->nfreebchannels);
		}
		cc_mutex_unlock(&contrlock);
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

#ifdef CC_AST_HAVE_TECH_PVT
static const struct ast_channel_tech capi_tech = {
	.type = channeltype,
	.description = tdesc,
	.capabilities = AST_FORMAT_ALAW,
	.requester = pbx_capi_request,
	.send_digit = pbx_capi_send_digit,
	.send_text = NULL,
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
#ifndef CC_AST_NO_DEVICESTATE
	.devicestate = pbx_capi_devicestate,
#endif
};
#endif

/*
 * register at CAPI interface
 */
static int cc_register_capi(unsigned blocksize)
{
	u_int16_t error = 0;

	if (capi_ApplID != CAPI_APPLID_UNUSED) {
		if (capi20_release(capi_ApplID) != 0)
			cc_log(LOG_WARNING,"Unable to unregister from CAPI!\n");
	}
	cc_verbose(3, 0, VERBOSE_PREFIX_3 "Registering at CAPI "
		   "(blocksize=%d)\n", blocksize);

#if (CAPI_OS_HINT == 2)
	error = capi20_register(CAPI_BCHANS, CAPI_MAX_B3_BLOCKS, 
				blocksize, &capi_ApplID, CAPI_STACK_VERSION);
#else
	error = capi20_register(CAPI_BCHANS, CAPI_MAX_B3_BLOCKS, 
				blocksize, &capi_ApplID);
#endif
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

	if (cc_register_capi(CAPI_MAX_B3_BLOCK_SIZE))
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
			cc_verbose(3, 0, VERBOSE_PREFIX_4 "T.38 is supported\n");
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
	int use_rtp = 0;

	for (i = iflist; i && !use_rtp; i = i->next) {
		/* if at least one line wants RTP, we need to re-register with
		   bigger block size for RTP-header */
		for (controller = 1; controller <= capi_num_controllers && !use_rtp; controller++) {
			if (((i->controllers & (1 << controller))) &&
			     (capi_controllers[controller]->rtpcodec & i->capability)) {
				cc_verbose(3, 0, VERBOSE_PREFIX_4 "at least one CAPI controller wants RTP.\n");
				use_rtp = 1;
			}
		}
	}
	if (use_rtp) {
		if (cc_register_capi(CAPI_MAX_B3_BLOCK_SIZE + RTP_HEADER_SIZE))
			return -1;
	}

	for (controller = 1; controller <= capi_num_controllers; controller++) {
		if (capi_used_controllers & (1 << controller)) {
			if ((error = ListenOnController(ALL_SERVICES, controller)) != 0) {
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
		if (!strcasecmp(v->name, "group")) {
			conf->group = ast_get_group(v->value);
			continue;
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

	/* read the general section */
	for (v = ast_variable_browse(cfg, "general"); v; v = v->next) {
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

/*
 * unload the module
 */
int unload_module()
{
	struct capi_pvt *i, *itmp;
	int controller;

	ast_unregister_application(commandapp);

	ast_cli_unregister(&cli_info);
	ast_cli_unregister(&cli_show_channels);
	ast_cli_unregister(&cli_debug);
	ast_cli_unregister(&cli_no_debug);

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
	
	i = iflist;
	while (i) {
		if (i->owner)
			cc_log(LOG_WARNING, "On unload, interface still has owner.\n");
		if (i->smoother)
			ast_smoother_free(i->smoother);
		cc_mutex_destroy(&i->lock);
		ast_cond_destroy(&i->event_trigger);
		itmp = i;
		i = i->next;
		free(itmp);
	}

	cc_mutex_unlock(&iflock);
	
#ifdef CC_AST_HAVE_TECH_PVT
	ast_channel_unregister(&capi_tech);
#else
	ast_channel_unregister(channeltype);
#endif
	
	return 0;
}

/*
 * main: load the module
 */
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
	
#ifdef CC_AST_HAVE_TECH_PVT
	if (ast_channel_register(&capi_tech)) {
#else	
	if (ast_channel_register(channeltype, tdesc, capi_capability, pbx_capi_request)) {
#endif
		cc_log(LOG_ERROR, "Unable to register channel class %s\n", channeltype);
		unload_module();
		return -1;
	}

	ast_cli_register(&cli_info);
	ast_cli_register(&cli_show_channels);
	ast_cli_register(&cli_debug);
	ast_cli_register(&cli_no_debug);
	
	ast_register_application(commandapp, pbx_capicommand_exec, commandsynopsis, commandtdesc);

	if (ast_pthread_create(&monitor_thread, NULL, capidev_loop, NULL) < 0) {
		monitor_thread = (pthread_t)(0-1);
		cc_log(LOG_ERROR, "Unable to start monitor thread!\n");
		return -1;
	}

	return 0;
}

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

#ifndef PBX_IS_OPBX
char *key()
{
	return ASTERISK_GPL_KEY;
}
#endif
