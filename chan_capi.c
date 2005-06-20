/*
 * (CAPI*)
 *
 * An implementation of Common ISDN API 2.0 for Asterisk
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
#include <sys/time.h>
#include <sys/signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined( __NetBSD__ ) || defined(__APPLE__)
#include <capi_bsd.h>
#else
#include <linux/capi.h>
#endif
#include <capi20.h>
#include <asterisk/dsp.h>
#include "xlaw.h"
#include "chan_capi_app.h"
#include "chan_capi_pvt.h"

#define CC_VERSION "cm-0.6dev"

#ifdef CAPI_ULAW
#define LAW_STRING "uLaw"
#else
#define LAW_STRING "aLaw"
#endif

/*
 * personal stuff
 */
unsigned ast_capi_ApplID;

static _cword ast_capi_MessageNumber = 1;
static char *desc = "Common ISDN API for Asterisk";
#ifdef CC_AST_HAVE_TECH_PVT
static const char tdesc[] = "Common ISDN API Driver (" CC_VERSION ") " LAW_STRING " " ASTERISKVERSION;
static const char type[] = "CAPI";
#else
static char *tdesc = "Common ISDN API Driver (" CC_VERSION ") " LAW_STRING " "ASTERISKVERSION;
static char *type = "CAPI";
#endif

static int usecnt;

AST_MUTEX_DEFINE_STATIC(messagenumber_lock);
AST_MUTEX_DEFINE_STATIC(usecnt_lock);
AST_MUTEX_DEFINE_STATIC(iflock);
AST_MUTEX_DEFINE_STATIC(pipelock);
AST_MUTEX_DEFINE_STATIC(monlock);
AST_MUTEX_DEFINE_STATIC(contrlock);
AST_MUTEX_DEFINE_STATIC(capi_put_lock);

#ifdef CAPI_ULAW
static int capi_capability = AST_FORMAT_ULAW;
#else
static int capi_capability = AST_FORMAT_ALAW;
#endif

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined( __NetBSD__ ) || defined(__APPLE__)
static CAPIProfileBuffer_t profile;
#else
static struct ast_capi_profile profile;
#endif

static pthread_t monitor_thread = -1;

static struct ast_capi_pvt *iflist = NULL;
static struct capi_pipe *pipelist = NULL;
static struct ast_capi_controller *capi_controllers[AST_CAPI_MAX_CONTROLLERS];
static int capi_num_controllers = 0;
static int capi_counter = 0;
static unsigned long capi_used_controllers = 0;

char capi_national_prefix[AST_MAX_EXTENSION];
char capi_international_prefix[AST_MAX_EXTENSION];

int capidebug = 0;

/* */
#define return_on_no_pipe(x)						\
	if (!p) {							\
		cc_ast_verbose(4, 1, "CAPI: %s no pipe PLCI=%#x\n", x, PLCI);	\
		return;							\
	}

/*
 * get a new capi message number atomically
 */
_cword get_ast_capi_MessageNumber(void)
{
	_cword mn;

	ast_mutex_lock(&messagenumber_lock);
	mn = ast_capi_MessageNumber;
	ast_capi_MessageNumber++;
	ast_mutex_unlock(&messagenumber_lock);

	return(mn);
}

/*
 * write a capi message to capi device
 */
MESSAGE_EXCHANGE_ERROR _capi_put_cmsg(_cmsg *CMSG)
{
	MESSAGE_EXCHANGE_ERROR error;
	
	if (ast_mutex_lock(&capi_put_lock)) {
		ast_log(LOG_WARNING, "Unable to lock capi put!\n");
		return -1;
	} 
	
	error = capi20_put_cmsg(CMSG);
	
	if (ast_mutex_unlock(&capi_put_lock)) {
		ast_log(LOG_WARNING, "Unable to unlock capi put!\n");
		return -1;
	}

	if (error) {
		ast_log(LOG_ERROR, "CAPI error sending %s (NCCI=%#x) (error=%#x)\n",
			capi_cmsg2str(CMSG), CMSG->adr.adrNCCI, error);
	} else {
		if (CMSG->Command == CAPI_DATA_B3) {
			cc_ast_verbose(7, 1, "%s\n", capi_cmsg2str(CMSG));
		} else {
			cc_ast_verbose(4, 1, "%s\n", capi_cmsg2str(CMSG));
		}
	}
	return error;
}

/*
 * wait some time for a new capi message
 */
MESSAGE_EXCHANGE_ERROR check_wait_get_cmsg(_cmsg *CMSG)
{
	MESSAGE_EXCHANGE_ERROR Info;
	struct timeval tv;
	
	tv.tv_sec = 0;
	tv.tv_usec = 10000;
	
	Info = capi20_waitformessage(ast_capi_ApplID, &tv);
	if ((Info != 0x0000) && (Info != 0x1104)) {
		if (capidebug) {
			ast_log(LOG_DEBUG, "Error waiting for cmsg... INFO = %#x\n", Info);
		}
		return Info;
	}
    
	if (Info == 0x0000) {
		Info = capi_get_cmsg(CMSG,ast_capi_ApplID);
	}
	return Info;
}

/*
 * send Listen to specified controller
 */
static unsigned ListenOnController(unsigned long CIPmask, unsigned controller)
{
	MESSAGE_EXCHANGE_ERROR error;
	_cmsg                  CMSG,CMSG2;

	LISTEN_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), controller);

	LISTEN_REQ_INFOMASK(&CMSG) = 0x03ff; /* lots of info ;) + early B3 connect */
		/* 0x00ff if no early B3 should be done */
		
	LISTEN_REQ_CIPMASK(&CMSG) = CIPmask;
	if ((error = _capi_put_cmsg(&CMSG)) != 0) {
		return error;
	}
	while (!IS_LISTEN_CONF(&CMSG2)) {
		error = check_wait_get_cmsg(&CMSG2);
	}
	return 0;
}

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
#define EC_OPTION_DISABLE_G165          (1<<1)
#define EC_OPTION_DISABLE_G164_OR_G165  (1<<1 | 1<<2)
#define EC_DEFAULT_TAIL                 64

#ifdef CC_AST_CHANNEL_HAS_TRANSFERCAP
/*
 *  TCAP -> CIP Translation Table (TransferCapability->CommonIsdnProfile)
 */
static struct {
	unsigned short tcap;
	unsigned short cip;
} translate_tcap2cip[] = {
	{ PRI_TRANS_CAP_SPEECH,                 CAPI_CIP_SPEECH },
	{ PRI_TRANS_CAP_DIGITAL,                CAPI_CIP_DIGITAL },
	{ PRI_TRANS_CAP_RESTRICTED_DIGITAL,     CAPI_CIP_RESTRICTED_DIGITAL },
	{ PRI_TRANS_CAP_3K1AUDIO,               CAPI_CIP_3K1AUDIO },
	{ PRI_TRANS_CAP_DIGITAL_W_TONES,        CAPI_CIP_DIGITAL_W_TONES },
	{ PRI_TRANS_CAP_VIDEO,                  CAPI_CIP_VIDEO }
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

/*
 *  CIP -> TCAP Translation Table (CommonIsdnProfile->TransferCapability)
 */
static struct {
	unsigned short cip;
	unsigned short tcap;
} translate_cip2tcap[] = {
	{ CAPI_CIP_SPEECH,                  PRI_TRANS_CAP_SPEECH },
	{ CAPI_CIP_DIGITAL,                 PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIP_RESTRICTED_DIGITAL,      PRI_TRANS_CAP_RESTRICTED_DIGITAL },
	{ CAPI_CIP_3K1AUDIO,                PRI_TRANS_CAP_3K1AUDIO },
	{ CAPI_CIP_7KAUDIO,                 PRI_TRANS_CAP_DIGITAL_W_TONES },
	{ CAPI_CIP_VIDEO,                   PRI_TRANS_CAP_VIDEO },
	{ CAPI_CIP_PACKET_MODE,             PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIP_56KBIT_RATE_ADAPTION,    PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIP_DIGITAL_W_TONES,         PRI_TRANS_CAP_DIGITAL_W_TONES },
	{ CAPI_CIP_TELEPHONY,               PRI_TRANS_CAP_SPEECH },
	{ CAPI_CIP_FAX_G2_3,                PRI_TRANS_CAP_3K1AUDIO },
	{ CAPI_CIP_FAX_G4C1,                PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIP_FAX_G4C2_3,              PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIP_TELETEX_PROCESSABLE,     PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIP_TELETEX_BASIC,           PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIP_VIDEOTEX,                PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIP_TELEX,                   PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIP_X400,                    PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIP_X200,                    PRI_TRANS_CAP_DIGITAL },
	{ CAPI_CIP_7K_TELEPHONY,            PRI_TRANS_CAP_DIGITAL_W_TONES },
	{ CAPI_CIP_VIDEO_TELEPHONY_C1,      PRI_TRANS_CAP_DIGITAL_W_TONES },
	{ CAPI_CIP_VIDEO_TELEPHONY_C2,      PRI_TRANS_CAP_DIGITAL }
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

static void capi_echo_canceller(struct ast_channel *c, int function)
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(c);
	_cmsg CMSG;
	char buf[7];

	/* If echo cancellation is not requested or supported, don't attempt to enable it */
	ast_mutex_lock(&contrlock);
	if (!capi_controllers[i->controller]->echocancel || !i->doEC) {
		ast_mutex_unlock(&contrlock);
		return;
	}
	ast_mutex_unlock(&contrlock);

	cc_ast_verbose(2, 0, VERBOSE_PREFIX_3 "Setting up echo canceller (PLCI=%#x, function=%d, options=%d, tail=%d)\n",
			i->PLCI, function, i->ecOption, i->ecTail);

	FACILITY_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
	FACILITY_REQ_PLCI(&CMSG) = i->PLCI;
	FACILITY_REQ_FACILITYSELECTOR(&CMSG) = 6; /* Echo canceller */

	memset(buf, 0, sizeof(buf));
        buf[0] = 6; /* msg size */
        write_capi_word(&buf[1], function);
	if (function == EC_FUNCTION_ENABLE) {
	        write_capi_word(&buf[3], i->ecOption); /* bit field - ignore echo canceller disable tone */
		write_capi_word(&buf[5], i->ecTail);   /* Tail length, ms */
	}

	FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = buf;
        
	if (_capi_put_cmsg(&CMSG) != 0) {
		return;
	}

	return;
}

/*
 * turn on/off DTMF detection
 */
int capi_detect_dtmf(struct ast_channel *c, int flag)
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(c);
	MESSAGE_EXCHANGE_ERROR error;
	_cmsg CMSG;
	char buf[9];

	memset(buf, 0, sizeof(buf));
	
	/* does the controller support dtmf? and do we want to use it? */
	
	ast_mutex_lock(&contrlock);
	
	if ((capi_controllers[i->controller]->dtmf == 1) && (i->doDTMF == 0)) {
		ast_mutex_unlock(&contrlock);
		FACILITY_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
		FACILITY_REQ_PLCI(&CMSG) = i->PLCI;
		FACILITY_REQ_FACILITYSELECTOR(&CMSG) = 1;
		buf[0] = 8; /* msg length */
		if (flag == 1) {
			write_capi_word(&buf[1], 1); /* start DTMF listen */
		} else {
			write_capi_word(&buf[1], 2); /* stop DTMF listen */
		}
		write_capi_word(&buf[3], AST_CAPI_DTMF_DURATION);
		write_capi_word(&buf[5], AST_CAPI_DTMF_DURATION);
		FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = buf;
        
		if ((error = _capi_put_cmsg(&CMSG)) != 0) {
			return error;
		}
	} else {
		ast_mutex_unlock(&contrlock);
		/* do software dtmf detection */
		i->doDTMF = 1; /* just being paranoid again... */
	}
	return 0;
}

/*
 * send a DTMF digit
 */
static int capi_send_digit(struct ast_channel *c, char digit)
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(c);
	MESSAGE_EXCHANGE_ERROR error;
	_cmsg CMSG;
	char buf[10];
    
	if (i->state != CAPI_STATE_BCONNECTED) {
		return 0;
	}

	memset(buf, 0, sizeof(buf));
	
	if (i->earlyB3 == 1) {
	/*
	 * we should really test for the network saying the number is incomplete
	 * since i'm only doing a test and this is true at the right time
	 * i'm going with this
	 */

		INFO_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
		INFO_REQ_PLCI(&CMSG) = i->PLCI;
		buf[0] = 2;
		buf[1] = 0x80;
		buf[2] = digit;
		INFO_REQ_CALLEDPARTYNUMBER(&CMSG) = buf;

		if ((error = _capi_put_cmsg(&CMSG)) != 0) {
			return error;
		}
		cc_ast_verbose(3, 0, VERBOSE_PREFIX_4 "sent CALLEDPARTYNUMBER INFO digit = %c (PLCI=%#x)\n",
			digit, i->PLCI);
	} else {
		ast_mutex_lock(&contrlock);
		if ((capi_controllers[i->controller]->dtmf == 0) || (i->doDTMF == 1)) {
			/* let * fake it */
			ast_mutex_unlock(&contrlock);
			return -1;
		}
		
		ast_mutex_unlock(&contrlock);
	
		FACILITY_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
		FACILITY_REQ_PLCI(&CMSG) = i->NCCI;
	        FACILITY_REQ_FACILITYSELECTOR(&CMSG) = 1;
        	buf[0] = 8;
	        write_capi_word(&buf[1], 3); /* send DTMF digit */
	        write_capi_word(&buf[3], AST_CAPI_DTMF_DURATION);
	        write_capi_word(&buf[5], AST_CAPI_DTMF_DURATION);
	        buf[7] = 1;
		buf[8] = digit;
		FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = buf;
        
		if ((error = _capi_put_cmsg(&CMSG)) != 0) {
			return error;
		}
		cc_ast_verbose(3, 0, VERBOSE_PREFIX_3 "sent dtmf '%c'\n", digit);
	}
	return 0;
}

/*
 * send ALERT to ISDN line
 */
static int capi_alert(struct ast_channel *c)
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(c);
	_cmsg CMSG;

	if ((i->state != CAPI_STATE_DISCONNECTED) &&
	    (i->state != CAPI_STATE_DID)) {
		ast_log(LOG_WARNING, "CAPI attempting ALERT in state %d\n",
			i->state);
		return -1;
	}
	
	ALERT_REQ_HEADER(&CMSG, ast_capi_ApplID, i->MessageNumber, 0);
	ALERT_REQ_PLCI(&CMSG) = i->PLCI;

	if (_capi_put_cmsg(&CMSG) != 0) {
		return -1;
	}

	i->state = CAPI_STATE_ALERTING;
	
	return 0;
}

/*
 * deflect a call
 */
static int capi_deflect(struct ast_channel *chan, void *data)
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(chan);
	MESSAGE_EXCHANGE_ERROR Info;
	_cmsg CMSG;
	char bchaninfo[1];
	char fac[60];
	int res = 0;
	int ms = 3000;

	if (!data) {
		ast_log(LOG_WARNING, "cd requires an argument (destination phone number)\n");
		return -1;
	}

	if ((i->state == CAPI_STATE_CONNECTED) || (i->state == CAPI_STATE_BCONNECTED)) {
		ast_log(LOG_ERROR, "call deflection does not work with calls that are already connected!\n");
		return -1;
	}
	
	/* wait until the channel is alerting, so we dont drop the call and interfer with msgs */
	while ((ms > 0) && (i->state != CAPI_STATE_ALERTING)) {
		sleep(100);
		ms -= 100;
	}

	/* make sure we hang up correctly */
	i->state = CAPI_STATE_CONNECTPENDING;

	fac[0] = 0;     /* len */
	fac[1] = 0;     /* len */
	fac[2] = 0x01;  /* Use D-Chan */
	fac[3] = 0;     /* Keypad len */
	fac[4] = 31;    /* user user data? len = 31 = 29 + 2 */
	fac[5] = 0x1c;  /* magic? */
	fac[6] = 0x1d;  /* strlen destination + 18 = 29 */
	fac[7] = 0x91;  /* .. */
	fac[8] = 0xA1;
	fac[9] = 0x1A;  /* strlen destination + 15 = 26 */
	fac[10] = 0x02;
	fac[11] = 0x01;
	fac[12] = 0x70;
	fac[13] = 0x02;
	fac[14] = 0x01;
	fac[15] = 0x0d;
	fac[16] = 0x30;
	fac[17] = 0x12; /* strlen destination + 7 = 18 */
	fac[18] = 0x30; /* ...hm 0x30 */
	fac[19] = 0x0d; /* strlen destination + 2 */
	fac[20] = 0x80; /* CLIP */
	fac[21] = 0x0b; /*  strlen destination */
	fac[22] = 0x01; /*  destination start */
	fac[23] = 0x01;  
	fac[24] = 0x01;  
	fac[25] = 0x01;  
	fac[26] = 0x01;  
	fac[27] = 0x01;  
	fac[28] = 0x01;  
	fac[29] = 0x01;  
	fac[30] = 0x01; 
	fac[31] = 0x01;  
	fac[32] = 0x01;  
	fac[33] = 0x01; /* 0x1 = sending complete */
	fac[34] = 0x01;
	fac[35] = 0x01;
				   
	memcpy((unsigned char *)fac + 22, data, strlen(data));
	fac[22 + strlen(data)] = 0x01; /* fill with 0x01 if number is only 6 numbers (local call) */
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
	INFO_REQ_FACILITYDATAARRAY(&CMSG) = (unsigned char*) fac + 4;

	if ((Info = _capi_put_cmsg(&CMSG)) != 0) {
		return Info;
	}

	return res;
}

/*
 * remove a pipe from the list
 */
static void remove_pipe(unsigned int PLCI)
{
	struct capi_pipe *p,*ptmp;

	ast_mutex_lock(&pipelock);
	p = pipelist;
	ptmp = NULL;
	
	while (p) {
		if (p->PLCI != PLCI) {
			ptmp = p;
			p = p->next;
			continue;
		}
		if (ptmp == NULL) {
			/* mypipe == head of pipelist */
			pipelist = p->next;
		} else {
			/* somehwere inbetween or at the end */
			ptmp->next = p->next;
		}
		if(p->fd > -1)
			close(p->fd);
		if(p->i != NULL && p->i->fd > -1)
			close(p->i->fd);
		free(p);
		cc_ast_verbose(3, 0, VERBOSE_PREFIX_3 "removed pipe for PLCI = %#x\n", PLCI);
		break;
	}
	ast_mutex_unlock(&pipelock);
}

/*
 * hangup a line (CAPI messages)
 */
static void capi_activehangup(struct ast_channel *c)
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(c);
	_cmsg CMSG;

	cc_ast_verbose(2, 1, VERBOSE_PREFIX_4 "activehangingup\n");

	if (i == NULL) {
		return;
	}

	if (c->_state == AST_STATE_RING) {
		CONNECT_RESP_HEADER(&CMSG, ast_capi_ApplID, i->MessageNumber, 0);
		CONNECT_RESP_PLCI(&CMSG) = i->PLCI;
		CONNECT_RESP_REJECT(&CMSG) = 2;
		_capi_put_cmsg(&CMSG);
		return;
	}

	/* active disconnect */
	if (i->state == CAPI_STATE_BCONNECTED) {
		DISCONNECT_B3_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
		DISCONNECT_B3_REQ_NCCI(&CMSG) = i->NCCI;
		_capi_put_cmsg(&CMSG);
		return;
	}
	
	if ((i->state == CAPI_STATE_CONNECTED) || (i->state == CAPI_STATE_CONNECTPENDING)) {
		DISCONNECT_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
		DISCONNECT_REQ_PLCI(&CMSG) = i->PLCI;
		_capi_put_cmsg(&CMSG);
	}
	return;
}

/*
 * Asterisk tells us to hangup a line
 */
static int capi_hangup(struct ast_channel *c)
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(c);
	int removepipe = 0;

	/*
	 * hmm....ok...this is called to free the capi interface (passive disconnect)
	 * or to bring down the channel (active disconnect)
	 */

	cc_ast_verbose(3, 0, VERBOSE_PREFIX_3 "CAPI Hangingup\n");

	if (i == NULL) {
		ast_log(LOG_ERROR, "channel has no interface!\n");
		return -1;
	}
    
	/* are we down, yet? */
	if (i->state != CAPI_STATE_DISCONNECTED) {
		/* no */
		capi_activehangup(c);
		i->state = CAPI_STATE_DISCONNECTING;
	} else {
		removepipe = 1;
	}
	
	if ((i->doDTMF == 1) && (i->vad != NULL)) {
		ast_dsp_free(i->vad);
	}
	
	memset(i->cid, 0, sizeof(i->cid));
	i->owner = NULL;
	
	ast_mutex_lock(&usecnt_lock);
	usecnt--;
	ast_mutex_unlock(&usecnt_lock);
	
	ast_update_use_count();
	
	i->mypipe = NULL;
	CC_AST_CHANNEL_PVT(c) = NULL;
	ast_setstate(c, AST_STATE_DOWN);

	if (removepipe) {
		/* disconnect already done, so cleanup */
		if (!i->PLCI) {
			ast_log(LOG_ERROR, "no PLCI for pipe removal\n");
		} else {
			remove_pipe(i->PLCI);
			i->PLCI = 0;
			i->NCCI = 0;
		}
	}

	return 0;
}

/*
 * convert a number
 */
static char *capi_number(char *data, int strip)
{
	unsigned len = *data;
	
	/* XXX fix me */
	/* convert a capi struct to a \0 terminated string */
	if ((!len) || (len < (unsigned int) strip))
		return NULL;
		
	len = len - strip;
	data = (char *)(data + 1 + strip);
	
	return strndup((char *)data, len);
}

/*
 * Asterisk tells us to make a call
 */
int capi_call(struct ast_channel *c, char *idest, int timeout)
{
	struct ast_capi_pvt *i;
	struct capi_pipe *p = NULL;
	char *dest, *interface;
	char buffer[AST_MAX_EXTENSION];
	char buffer_r[AST_CAPI_MAX_STRING];
	char *buffer_rp = buffer_r;
	char called[AST_MAX_EXTENSION], calling[AST_MAX_EXTENSION];
	char bchaninfo[3];
	
	_cmsg CMSG;
	MESSAGE_EXCHANGE_ERROR  error;
	
	strncpy(buffer, (char *)idest, sizeof(buffer) - 1);
	interface = strtok_r(buffer, "/", &buffer_rp);
	dest = strtok_r(NULL, "/", &buffer_rp);


	if ((!dest) || (!dest[0])) {
		ast_log(LOG_WARNING, "Destination %s requres a real destination\n", idest);
		return -1;
	}

	i = CC_AST_CHANNEL_PVT(c);

	/* first character of destination tells special treatment */
	switch(dest[0]) {
	case 'b':	/* always B3 */
		i->doB3 = AST_CAPI_B3_ALWAYS;
		dest++;
		break;
	case 'B':	/* only do B3 on successfull calls */
		i->doB3 = AST_CAPI_B3_ON_SUCCESS;
		dest++;
		break;
	default:
		i->doB3 = AST_CAPI_B3_DONT;
	}

	cc_ast_verbose(1, 1, VERBOSE_PREFIX_2 "CAPI Call %s %s\n", c->name, i->doB3?"with B3":"");
    
#ifdef CC_AST_CHANNEL_HAS_CID
	switch (c->cid.cid_pres) {
#else    
	switch (c->callingpres) {
#endif
	case PRES_ALLOWED_USER_NUMBER_NOT_SCREENED:
	case PRES_ALLOWED_USER_NUMBER_PASSED_SCREEN:
	case PRES_ALLOWED_USER_NUMBER_FAILED_SCREEN:
	case PRES_ALLOWED_NETWORK_NUMBER:
	case PRES_NUMBER_NOT_AVAILABLE:
		i->CLIR = 0;
		break;
	case PRES_PROHIB_USER_NUMBER_NOT_SCREENED:
	case PRES_PROHIB_USER_NUMBER_PASSED_SCREEN:
	case PRES_PROHIB_USER_NUMBER_FAILED_SCREEN:
	case PRES_PROHIB_NETWORK_NUMBER:
		i->CLIR = 1;
		break;
	default:
		i->CLIR = 0;
	}

	p = malloc(sizeof(struct capi_pipe));
	if (!p) {
		ast_log(LOG_ERROR, "Error allocating capi_pipe.\n");
		return -1;
	}
	memset(p, 0, sizeof(struct capi_pipe));

	/* set pipe and FDs */
	p->fd = i->fd2;
	c->fds[0] = i->fd;
	p->i = i;
	p->c = c;
	i->mypipe = p;

	ast_mutex_lock(&pipelock);
	p->next = pipelist;
	pipelist = p;
	ast_mutex_unlock(&pipelock);
		
	i->outgoing = 1;
	
	i->MessageNumber = get_ast_capi_MessageNumber();
	CONNECT_REQ_HEADER(&CMSG, ast_capi_ApplID, i->MessageNumber, i->controller);
	CONNECT_REQ_CONTROLLER(&CMSG) = i->controller;
#ifdef CC_AST_CHANNEL_HAS_TRANSFERCAP
	CONNECT_REQ_CIPVALUE(&CMSG) = tcap2cip(c->transfercapability);
#else
	CONNECT_REQ_CIPVALUE(&CMSG) = 0x10; /* Telephony */
#endif
	called[0] = strlen(dest) + 1;
	called[1] = 0x80;
	strncpy(&called[2], dest, sizeof(called) - 2);
	CONNECT_REQ_CALLEDPARTYNUMBER(&CMSG) = called;
	CONNECT_REQ_CALLEDPARTYSUBADDRESS(&CMSG) = NULL;

#ifdef CC_AST_CHANNEL_HAS_CID
	calling[0] = strlen(c->cid.cid_num) + 2;
#else
	calling[0] = strlen(c->callerid) + 2;
#endif

	calling[1] = 0x00;
	if (i->CLIR == 1) {
		calling[2] = 0xA0; /* CLIR */
	} else {
		calling[2] = 0x80; /* CLIP */
	}
#ifdef CC_AST_CHANNEL_HAS_CID
	strncpy(&calling[3], c->cid.cid_num, sizeof(calling) - 3);
#else
	strncpy(&calling[3], c->callerid, sizeof(calling) - 3);
#endif

	CONNECT_REQ_CALLINGPARTYNUMBER(&CMSG) = calling;
	CONNECT_REQ_CALLINGPARTYSUBADDRESS(&CMSG) = NULL;

	CONNECT_REQ_B1PROTOCOL(&CMSG) = 1;
	CONNECT_REQ_B2PROTOCOL(&CMSG) = 1;
	CONNECT_REQ_B3PROTOCOL(&CMSG) = 0;

	bchaninfo[0] = 2;
	bchaninfo[1] = 0x0;
	bchaninfo[2] = 0x0;
	CONNECT_REQ_BCHANNELINFORMATION(&CMSG) = bchaninfo; /* 0 */

        if ((error = _capi_put_cmsg(&CMSG))) {
		return error;
	}
	i->state = CAPI_STATE_CONNECTPENDING;

	ast_setstate(c, AST_STATE_DIALING);

#if 0
	/* XXX fixme, not nice: */
	if (i->controller > 0) {
		capi_controllers[i->controller]->nfreebchannels--;
	}
#endif

	/* now we shall return .... the rest has to be done by handle_msg */
	return 0;
}

/*
 * Asterisk tells us to answer a call
 */
static int capi_answer(struct ast_channel *c)
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(c);
	_cmsg CMSG;
	char buf[AST_CAPI_MAX_STRING];
	char *dnid;
    
	if ((i->isdnmode == AST_CAPI_ISDNMODE_PTP) &&
	    (strlen(i->incomingmsn) < strlen(i->dnid))) {
		dnid = i->dnid + strlen(i->incomingmsn);
	} else {
		dnid = i->dnid;
	}

	i->fFax = NULL;
	i->faxhandled = 0;
	i->FaxState = 0;

	CONNECT_RESP_HEADER(&CMSG, ast_capi_ApplID, i->MessageNumber, 0);
	CONNECT_RESP_PLCI(&CMSG) = i->PLCI;
	CONNECT_RESP_REJECT(&CMSG) = 0;
	buf[0] = strlen(dnid) + 2;
	buf[1] = 0x00;
	buf[2] = 0x80;
	strncpy(&buf[3], dnid, sizeof(buf) - 4);
	CONNECT_RESP_CONNECTEDNUMBER(&CMSG) = buf;
	CONNECT_RESP_CONNECTEDSUBADDRESS(&CMSG) = NULL;
	CONNECT_RESP_LLC(&CMSG) = NULL;
	CONNECT_RESP_B1PROTOCOL(&CMSG) = 1;
	CONNECT_RESP_B2PROTOCOL(&CMSG) = 1;
	CONNECT_RESP_B3PROTOCOL(&CMSG) = 0;

	cc_ast_verbose(3, 0, VERBOSE_PREFIX_3 "CAPI Answering for MSN %s\n", dnid);
		
	if (_capi_put_cmsg(&CMSG) != 0) {
		return -1;	
	}
    
	i->state = CAPI_STATE_ANSWERING;
	i->doB3 = AST_CAPI_B3_DONT;
	i->outgoing = 0;
	i->earlyB3 = -1;

	return 0;
}

/*
 * Asterisk tells us to read for a channel
 */
struct ast_frame *capi_read(struct ast_channel *c) 
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(c);
	int readsize = 0;
	
	if (i == NULL) {
		ast_log(LOG_ERROR, "channel has no interface\n");
		return NULL;
	}

	if (i->state == CAPI_STATE_ONHOLD) {
		i->fr.frametype = AST_FRAME_NULL;
		return &i->fr;
	}
	
	i->fr.frametype = AST_FRAME_NULL;
	i->fr.subclass = 0;

#ifdef CC_AST_FRAME_HAS_TIMEVAL
	i->fr.delivery.tv_sec = 0;
	i->fr.delivery.tv_usec = 0;
#endif	
	readsize = read(i->fd, &i->fr, sizeof(struct ast_frame));
	if (readsize != sizeof(struct ast_frame)) {
		ast_log(LOG_ERROR, "did not read a whole frame\n");
	}
	if (i->fr.frametype == AST_FRAME_VOICE) {
		readsize = read(i->fd, i->fr.data, i->fr.datalen);
		if (readsize != i->fr.datalen) {
			ast_log(LOG_ERROR, "did not read whole frame data\n");
		}
	}
	
	i->fr.mallocd = 0;	
	
	if (i->fr.frametype == AST_FRAME_NULL) {
		return NULL;
	}
	if ((i->fr.frametype == AST_FRAME_DTMF) && (i->fr.subclass == 'f')) {
		if (strcmp(c->exten, "fax")) {
#ifdef CC_AST_CHANNEL_HAS_CID
			if (ast_exists_extension(c, ast_strlen_zero(c->macrocontext) ? c->context : c->macrocontext, "fax", 1, c->cid.cid_num)) {
#else
			if (ast_exists_extension(c, ast_strlen_zero(c->macrocontext) ? c->context : c->macrocontext, "fax", 1, c->callerid)) {
#endif
				cc_ast_verbose(2, 0, VERBOSE_PREFIX_3 "Redirecting %s to fax extension\n", c->name);
				/* Save the DID/DNIS when we transfer the fax call to a "fax" extension */
				pbx_builtin_setvar_helper(c, "FAXEXTEN", c->exten);
				if (ast_async_goto(c, c->context, "fax", 1))
					ast_log(LOG_WARNING, "Failed to async goto '%s' into fax of '%s'\n", c->name, c->context);
			} else {
				ast_log(LOG_NOTICE, "Fax detected, but no fax extension\n");
			}
		} else {
			ast_log(LOG_DEBUG, "Already in a fax extension, not redirecting\n");
		}
	}
	return &i->fr;
}

/*
 * Asterisk tells us to write for a channel
 */
int capi_write(struct ast_channel *c, struct ast_frame *f)
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(c);
	MESSAGE_EXCHANGE_ERROR error = 1;
	_cmsg CMSG;
	int j = 0;
	unsigned char *buf;
	struct ast_frame *fsmooth;
	int txavg=0;

	if (!i) {
		ast_log(LOG_ERROR, "channel has no interface\n");
		return -1;
	} 

	/* dont send audio to the local exchange! */
	if ((i->earlyB3 == 1) || (!i->NCCI)) {
		return 0;
	}

	if (f->frametype == AST_FRAME_NULL) {
		return 0;
	}
	if (f->frametype == AST_FRAME_DTMF) {
		ast_log(LOG_ERROR, "dtmf frame should be written\n");
		return 0;
	}
	if (f->frametype != AST_FRAME_VOICE) {
		ast_log(LOG_ERROR,"not a voice frame\n");
		return -1;
	}
	if (f->subclass != capi_capability) {
		ast_log(LOG_ERROR, "dont know how to write subclass %d\n", f->subclass);
		return -1;
	}
	if ((!f->data) || (!f->datalen) || (!i->smoother)) {
		return 0;
	}

	if (ast_smoother_feed(i->smoother, f) != 0) {
		ast_log(LOG_ERROR, "CAPI: failed to fill smoother\n");
		return -1;
	}

	fsmooth = ast_smoother_read(i->smoother);
	while(fsmooth != NULL) {
		DATA_B3_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
		DATA_B3_REQ_NCCI(&CMSG) = i->NCCI;
		DATA_B3_REQ_DATALENGTH(&CMSG) = fsmooth->datalen;
		DATA_B3_REQ_FLAGS(&CMSG) = 0; 

		DATA_B3_REQ_DATAHANDLE(&CMSG) = i->send_buffer_handle;
		buf = &(i->send_buffer[(i->send_buffer_handle % AST_CAPI_MAX_B3_BLOCKS) * AST_CAPI_MAX_B3_BLOCK_SIZE]);
		DATA_B3_REQ_DATA(&CMSG) = buf;
		i->send_buffer_handle++;

		if ((i->doES == 1)) {
			for (j = 0; j < fsmooth->datalen; j++) {
				buf[j] = reversebits[ ((unsigned char *)fsmooth->data)[j] ]; 
				txavg += abs( capiXLAW2INT(reversebits[ ((unsigned char*)fsmooth->data)[j]]) );
			}
			txavg = txavg / j;
			for(j = 0; j < ECHO_TX_COUNT - 1; j++) {
				i->txavg[j] = i->txavg[j+1];
			}
			i->txavg[ECHO_TX_COUNT - 1] = txavg;
		} else {
			for (j = 0; j < fsmooth->datalen; j++) {
				buf[j] = i->g.txgains[reversebits[((unsigned char *)fsmooth->data)[j]]]; 
			}
		}
    
		if (i->B3q < AST_CAPI_MAX_B3_BLOCKS) {
			error = _capi_put_cmsg(&CMSG);
		}

		if (!error) {
			ast_mutex_lock(&i->lockB3q);
			i->B3q++;
			ast_mutex_unlock(&i->lockB3q);
		}

	        fsmooth = ast_smoother_read(i->smoother);
	}
	return 0;
}

/*
 * new channel
 */
static int capi_fixup(struct ast_channel *oldchan, struct ast_channel *newchan)
{
	struct ast_capi_pvt *p = CC_AST_CHANNEL_PVT(newchan);

	p->owner = newchan;
	return 0;
}

/*
 * we don't support own indications
 */
static int capi_indicate(struct ast_channel *c, int condition)
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(c);
	_cmsg CMSG;
	int ret = -1;

	if (i == NULL) {
		return -1;
	}

	switch (condition) {
	case AST_CONTROL_RINGING:
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "Requested RINGING-Indication for %s\n",
			c->name);
		break;
	case AST_CONTROL_BUSY:
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "Requested BUSY-Indication for %s\n",
			c->name);
		if (c->_state == AST_STATE_RING) {
			CONNECT_RESP_HEADER(&CMSG, ast_capi_ApplID, i->MessageNumber, 0);
			CONNECT_RESP_PLCI(&CMSG) = i->PLCI;
			CONNECT_RESP_REJECT(&CMSG) = 3;
			_capi_put_cmsg(&CMSG);
			ret = 0;
		}
		break;
	case AST_CONTROL_CONGESTION:
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "Requested CONGESTION-Indication for %s\n",
			c->name);
		if (c->_state == AST_STATE_RING) {
			CONNECT_RESP_HEADER(&CMSG, ast_capi_ApplID, i->MessageNumber, 0);
			CONNECT_RESP_PLCI(&CMSG) = i->PLCI;
			CONNECT_RESP_REJECT(&CMSG) = 4;
			_capi_put_cmsg(&CMSG);
			ret = 0;
		}
		break;
	case AST_CONTROL_PROGRESS:
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "Requested PROGRESS-Indication for %s\n",
			c->name);
		break;
	case AST_CONTROL_PROCEEDING:
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "Requested PROCEEDING-Indication for %s\n",
			c->name);
		break;
	case -1: /* stop indications */
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "Requested Indication-STOP for %s\n",
			c->name);
		break;
	default:
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "Requested unknown Indication %d for %s\n",
			condition, c->name);
		break;
	}
	return(ret);
}

/*
 * native bridging: connect to channels directly
 */
static int capi_bridge(struct ast_channel *c0, struct ast_channel *c1,
		int flags, struct ast_frame **fo, struct ast_channel **rc)
{
	cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "Requested bridge for %s and %s\n",
		c0->name, c1->name);
	
	return -1; /* failed, not supported */
	return -2; /* we dont want private bridge, no error message */
	return -3; /* no success, but try me again */
	return 0; /* success and end of private bridging */
}

/*
 * a new channel is needed
 */
static struct ast_channel *capi_new(struct ast_capi_pvt *i, int state)
{
	struct ast_channel *tmp;
	int fmt;
	int fds[2];

	tmp = ast_channel_alloc(0);
	
	if (tmp == NULL) {
		ast_log(LOG_ERROR,"Unable to allocate channel!\n");
		return(NULL);
	}

	/* ast_log(LOG_NOTICE,"allocated channel for PLCI=%#x!\n", i->PLCI); */
	snprintf(tmp->name, sizeof(tmp->name), "CAPI/contr%d/%s-%d",
		i->controller, i->dnid, capi_counter++);
	tmp->type = type;

	if (pipe(fds) != 0) {
	    	ast_log(LOG_ERROR, "CAPI: unable to create pipe.\n");
		ast_channel_free(tmp);
		return NULL;
	}
	
	cc_ast_verbose(4, 0, VERBOSE_PREFIX_3 "creating pipe for PLCI=%#x\n",
		i->PLCI);

	i->fd = fds[0];
	i->fd2 = fds[1];
	
	ast_setstate(tmp, state);
	tmp->fds[0] = i->fd;
	if (i->smoother != NULL) {
		ast_smoother_reset(i->smoother, AST_CAPI_MAX_B3_BLOCK_SIZE);
	}
	i->fr.frametype = 0;
	i->fr.subclass = 0;
#ifdef CC_AST_FRAME_HAS_TIMEVAL
	i->fr.delivery.tv_sec = 0;
	i->fr.delivery.tv_usec = 0;
#endif
	i->state = CAPI_STATE_DISCONNECTED;
	i->CLIR = 0;
	i->calledPartyIsISDN = 1;
	i->earlyB3 = -1;
	i->doB3 = AST_CAPI_B3_DONT;
	i->outgoing = 0;
	i->onholdPLCI = 0;
#ifdef CAPI_SYNC
	i->B3q = 0;
	ast_mutex_init(&i->lockB3q);
#endif		
	memset(i->txavg, 0, ECHO_TX_COUNT);

	if (i->doDTMF == 1) {
		i->vad = ast_dsp_new();
		ast_dsp_set_features(i->vad, DSP_FEATURE_DTMF_DETECT);
	}

	if (tmp->pvt == NULL) {
		free(tmp);
		return NULL;
	}

	CC_AST_CHANNEL_PVT(tmp) = i;

	tmp->callgroup = i->callgroup;
#ifndef CC_AST_HAVE_TECH_PVT
	tmp->pvt->call = capi_call;
	tmp->pvt->fixup = capi_fixup;
	tmp->pvt->indicate = capi_indicate;
	tmp->pvt->bridge = capi_bridge;
	tmp->pvt->answer = capi_answer;
	tmp->pvt->hangup = capi_hangup;
	tmp->pvt->read = capi_read;
	tmp->pvt->write = capi_write;
	tmp->pvt->send_digit = capi_send_digit;
#endif
	tmp->nativeformats = capi_capability;
	fmt = ast_best_codec(tmp->nativeformats);
	/* fmt = capi_capability; */
	tmp->readformat = fmt;
	tmp->writeformat = fmt;
#ifdef CC_AST_HAVE_TECH_PVT
	tmp->rawreadformat = fmt;
	tmp->rawwriteformat = fmt;
#else
	tmp->pvt->rawreadformat = fmt;
	tmp->pvt->rawwriteformat = fmt;
#endif
	strncpy(tmp->context, i->context, sizeof(tmp->context) - 1);
#ifdef CC_AST_CHANNEL_HAS_CID
	tmp->cid.cid_num = strdup(i->cid);
	tmp->cid.cid_dnid = strdup(i->dnid);
#else
	tmp->callerid = strdup(i->cid);
#endif
	
#ifdef CC_AST_CHANNEL_HAS_TRANSFERCAP
	tmp->transfercapability = cip2tcap(i->cip);
	pbx_builtin_setvar_helper(tmp, "TRANSFERCAPABILITY", transfercapability2str(tmp->transfercapability));
#endif
	
	strncpy(tmp->exten, i->dnid, sizeof(tmp->exten) - 1);
	strncpy(tmp->accountcode, i->accountcode, sizeof(tmp->accountcode) - 1);
	i->owner = tmp;
	ast_mutex_lock(&usecnt_lock);
	usecnt++;
	ast_mutex_unlock(&usecnt_lock);
	ast_update_use_count();
	if (state != AST_STATE_DOWN) {
		/* we are alerting (phones ringing) */
		if (state == AST_STATE_RING)
			capi_alert(tmp);
		if (ast_pbx_start(tmp)) {
			ast_log(LOG_ERROR, "Unable to start pbx on channel!\n");
			ast_hangup(tmp);
			tmp = NULL;
		} else {
			cc_ast_verbose(2, 0, VERBOSE_PREFIX_3 "started pbx on channel (callgroup=%d)!\n",
				tmp->callgroup);
		}
	}
	return tmp;
}

/*
 * Asterisk wants us to dial ...
 */
#ifdef CC_AST_HAVE_TECH_PVT
struct ast_channel *capi_request(const char *type, int format, void *data, int *cause)
#else
struct ast_channel *capi_request(char *type, int format, void *data)
#endif
{
	struct ast_capi_pvt *i;
	struct ast_channel *tmp = NULL;
	char *dest, *interface;
	char buffer[AST_CAPI_MAX_STRING];
	char buffer_r[AST_CAPI_MAX_STRING];
	char *buffer_rp = buffer_r;
	unsigned int capigroup = 0, controller = 0;
	unsigned int foundcontroller;
	int notfound = 1;

	cc_ast_verbose(1, 1, VERBOSE_PREFIX_3 "data = %s\n", (char *)data);

	strncpy(buffer, (char *)data, sizeof(buffer) - 1);

	interface = strtok_r(buffer, "/", &buffer_rp);
	dest = strtok_r(NULL, "/", &buffer_rp);

	if (((char *)interface)[0] == 'g') {
		interface++;
		capigroup = atoi(interface);
		cc_ast_verbose(1, 1, VERBOSE_PREFIX_3 "capi request group = %d\n",
				capigroup);
	} else if (!strncmp(interface, "contr", 5)) {
		interface += 5;
		controller = atoi(interface);
		cc_ast_verbose(1, 1, VERBOSE_PREFIX_3 "capi request controller = %d\n",
				controller);
	} else {
		ast_log(LOG_ERROR,"Syntax error in dialstring. read the docs!\n");
		return NULL;
 	}

	ast_mutex_lock(&iflock);
	
	for (i = iflist; (i && notfound); i = i->next) {
		if (i->owner) {
			continue;
		}
		/* unused channel */
		ast_mutex_lock(&contrlock);
		if (controller) {
			/* DIAL(CAPI/contrX/...) */
			if ((!(i->controllers & (1 << controller))) ||
			    (capi_controllers[controller]->nfreebchannels < 1)) {
				/* keep on running! */
				ast_mutex_unlock(&contrlock);
				continue;
			}
			foundcontroller = controller;
		} else {
			/* DIAL(CAPI/gX/...) */
			if (!(i->group & (1 << capigroup))) {
				/* keep on running! */
				ast_mutex_unlock(&contrlock);
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
				ast_mutex_unlock(&contrlock);
				continue;
			}
		}
		/* when we come here, we found a free controller match */
		strncpy(i->dnid, dest, sizeof(i->dnid) - 1);
		i->controller = foundcontroller;
		tmp = capi_new(i, AST_STATE_DOWN);
		i->PLCI = 0;
		i->outgoing = 1;	/* this is an outgoing line */
		i->earlyB3 = -1;
		ast_mutex_unlock(&contrlock);
		ast_mutex_unlock(&iflock);
		return tmp;
	}
	ast_mutex_unlock(&iflock);
	ast_log(LOG_NOTICE, "didn't find capi device with controller = %d or group = %d.\n",
		controller, capigroup);
	return NULL;
}

/*
 * Fax guard tone -- Handle and return NULL
 */
static void capi_handle_dtmf_fax(struct ast_channel *ast)
{
	struct ast_capi_pvt *p = CC_AST_CHANNEL_PVT(ast);
	char *cid;

	if (p->faxhandled) {
		ast_log(LOG_DEBUG, "Fax already handled\n");
		return;
	}
	
	p->faxhandled++;
	
	if (!strcmp(ast->exten, "fax")) {
		ast_log(LOG_DEBUG, "Already in a fax extension, not redirecting\n");
		return;
	}
#ifdef CC_AST_CHANNEL_HAS_CID
	cid = ast->cid.cid_num;
#else
	cid = ast->callerid;
#endif
	if (!ast_exists_extension(ast, ast->context, "fax", 1, cid)) {
		ast_log(LOG_NOTICE, "Fax detected, but no fax extension\n");
		return;
	}

	cc_ast_verbose(2, 0, VERBOSE_PREFIX_3 "Redirecting %s to fax extension\n", ast->name);
			
	/* Save the DID/DNIS when we transfer the fax call to a "fax" extension */
	pbx_builtin_setvar_helper(ast, "FAXEXTEN", ast->exten);
	
	if (ast_async_goto(ast, ast->context, "fax", 1))
		ast_log(LOG_WARNING, "Failed to async goto '%s' into fax of '%s'\n", ast->name, ast->context);
}

/*
 * find a pipe in our pipe-list
 */
struct capi_pipe *find_pipe(_cmsg *CMSG)
{
	struct capi_pipe *p;
	unsigned int NCCI = CMSG->adr.adrNCCI;
	unsigned int PLCI = (NCCI & 0xffff);
	int MN = CMSG->Messagenumber;
	
	/* find a pipe by PLCI or by MessageNumber (in case this is a CONNECT_CONF) */
	
	ast_mutex_lock(&pipelock);
	
	p = pipelist;
	while(p != NULL) {
		if ((p->PLCI == PLCI) ||
		   ((p->PLCI == 0) && (p->i->MessageNumber == MN))) {
			ast_mutex_unlock(&pipelock);
			return p;
		}
		p = p->next;
	}
	ast_mutex_unlock(&pipelock);

	cc_ast_verbose(2, 1, VERBOSE_PREFIX_1
		"CAPI: unable to find a pipe for PLCI = %#x MN = %#x\n",
		PLCI, MN);

	return NULL;
}

/*
 * send a frame to Asterisk via pipe
 */
static int pipe_frame(struct capi_pipe *p, struct ast_frame *f)
{
	fd_set wfds;
	int written = 0;
	struct timeval tv;
	
	FD_ZERO(&wfds);
	FD_SET(p->fd, &wfds);
	tv.tv_sec = 0;
	tv.tv_usec = 10;
	
	if ((f->frametype == AST_FRAME_VOICE) &&
	    (p->i->doDTMF == 1) &&
	    (p->i->vad != NULL)) {
#ifdef CC_AST_DSP_PROCESS_NEEDLOCK 
		f = ast_dsp_process(p->c, p->i->vad, f, 0);
#else
		f = ast_dsp_process(p->c, p->i->vad, f);
#endif
		if (f->frametype == AST_FRAME_NULL) {
			return 0;
		}
	}
	
	/* we dont want the monitor thread to block */
	if (select(p->fd + 1, NULL, &wfds, NULL, &tv) == 1) {
		written = write(p->fd, f, sizeof(struct ast_frame));
		if (written < (signed int) sizeof(struct ast_frame)) {
			ast_log(LOG_ERROR, "wrote %d bytes instead of %d\n",
				written, sizeof(struct ast_frame));
			return -1;
		}
		if (f->frametype == AST_FRAME_VOICE) {
			written = write(p->fd, f->data, f->datalen);
			if (written < f->datalen) {
				ast_log(LOG_ERROR,"wrote %d bytes instead of %d\n",
					written, f->datalen);
				return -1;
			}
		}
		return 0;
	}
	return -1;
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
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(c);
	char *exten;
    
	if (strlen(i->dnid) < strlen(i->incomingmsn))
		return -1;
	
	/* exten = i->dnid + strlen(i->incomingmsn); */
	exten = i->dnid;

	if (ast_exists_extension(NULL, c->context, exten, 1, NULL)) {
		c->priority = 1;
		strncpy(c->exten, exten, sizeof(c->exten) - 1);
		return 0;
	}

	if (ast_canmatch_extension(NULL, c->context, exten, 1, NULL)) {
		return 1;
	}

	return -1;
}

/*
 * Progress Indicator
 */
static void handle_progress_indicator(_cmsg *CMSG, unsigned int PLCI, struct capi_pipe *p)
{
	_cmsg CMSG2;

	if (INFO_IND_INFOELEMENT(CMSG)[0] < 2) {
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_1 "Progress description missing\n");
		return;
	}

	switch(INFO_IND_INFOELEMENT(CMSG)[2] & 0x7f) {
	case 0x01:
		cc_ast_verbose(4, 1, VERBOSE_PREFIX_1 "Not end-to-end ISDN\n");
		break;
	case 0x02:
		cc_ast_verbose(4, 1, VERBOSE_PREFIX_1 "Destination is non ISDN\n");
		p->i->calledPartyIsISDN = 0;
		break;
	case 0x03:
		cc_ast_verbose(4, 1, VERBOSE_PREFIX_1 "Origination is non ISDN\n");
		break;
	case 0x04:
		cc_ast_verbose(4, 1, VERBOSE_PREFIX_1 "Call returned to ISDN\n");
		break;
	case 0x05:
		cc_ast_verbose(4, 1, VERBOSE_PREFIX_1 "Interworking occured\n");
		break;
	case 0x08:
		cc_ast_verbose(4, 1, VERBOSE_PREFIX_1 "In-band information available\n");
		if ((p->i->doB3 != AST_CAPI_B3_DONT) &&
		    (p->i->earlyB3 == -1) &&
		    (p->i->state != CAPI_STATE_BCONNECTED)) {
			/* we do early B3 Connect */
			p->i->earlyB3 = 1;
			memset(&CMSG2, 0, sizeof(_cmsg));
			CONNECT_B3_REQ_HEADER(&CMSG2, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
			CONNECT_B3_REQ_PLCI(&CMSG2) = PLCI;
			_capi_put_cmsg(&CMSG2);
		}
		break;
	default:
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_1 "Unknown progress description %02x\n",
			INFO_IND_INFOELEMENT(CMSG)[2]);
	}
}

/*
 * Called Party Number via INFO_IND
 */
static void handle_did_digits(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pipe *p)
{
	_cmsg CMSG2;
	char name[AST_CHANNEL_NAME] = "";
	char *did;

	if(p->i->isdnmode != AST_CAPI_ISDNMODE_PTP) {
		cc_ast_verbose(4, 1, VERBOSE_PREFIX_4 "INFO_IND DID digits in non PtP mode\n");
		return;
	}

	if (p->c == NULL) {
		ast_log(LOG_ERROR, "No channel for INFO_IND DID digits\n");
		return;
	}

	did = capi_number(INFO_IND_INFOELEMENT(CMSG), 1);
	if (strcasecmp(p->i->dnid, did)) {
		strncat(p->i->dnid, did, sizeof(p->i->dnid) - 1);
	}
				
	snprintf(name, sizeof(name), "CAPI/contr%d/%s/-%d",
		p->i->controller, p->i->dnid, capi_counter++);
	ast_change_name(p->c, name);
				
	switch(search_did(p->c)) {
	case 0: /* match */
		ast_setstate(p->c, AST_STATE_RING);
		/* we are alerting (phones ringing) */
		capi_alert(p->c); /* Do this here after pbx_start the Channel can be destroyed */
		if (ast_pbx_start(p->c)) {
			ast_log(LOG_ERROR, "Unable to start pbx on channel!\n");
			ast_hangup(p->c);
		} else {
			cc_ast_verbose(2, 1, VERBOSE_PREFIX_3 "started pbx on channel!\n");
		}
		break;
	case 1:
		/* would possibly match */
		break;
	case -1:
	default:
		/* doesn't match */
		ast_log(LOG_ERROR, "did not find device for msn = %s\n",
			p->i->dnid);
		CONNECT_RESP_HEADER(&CMSG2, ast_capi_ApplID, CMSG->Messagenumber, 0);
		CONNECT_RESP_PLCI(&CMSG2) = PLCI;
		CONNECT_RESP_REJECT(&CMSG2) = 1; /* ignore */
		_capi_put_cmsg(&CMSG2);
	}
}

/*
 * Disconnect via INFO_IND
 */
static void handle_info_disconnect(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pipe *p)
{
	_cmsg CMSG2;
	struct ast_frame fr;

	if (PLCI == p->i->onholdPLCI) {
		cc_ast_verbose(4, 1, VERBOSE_PREFIX_1 "Disconnect onhold call\n");
		/* the caller onhold hung up (or ECTed away) */
		/* send a disconnect_req , we cannot hangup the channel here!!! */
		memset(&CMSG2, 0, sizeof(_cmsg));
		DISCONNECT_REQ_HEADER(&CMSG2, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
		DISCONNECT_REQ_PLCI(&CMSG2) = p->i->onholdPLCI;
		_capi_put_cmsg(&CMSG2);
		return;
	}

	/* case 1: B3 on success or no B3 at all */
	if ((p->i->doB3 != AST_CAPI_B3_ALWAYS) && (p->i->outgoing == 1)) {
		cc_ast_verbose(4, 1, VERBOSE_PREFIX_1 "Disconnect case 1\n");
		p->i->earlyB3 = 0; /* !!! */
		fr.frametype = AST_FRAME_NULL;
		fr.datalen = 0;
		pipe_frame(p, (struct ast_frame *)&fr);
		return;
	}
	
	/* case 2: we are doing B3, and receive the 0x8045 after a successful call */
	if ((p->i->doB3 != AST_CAPI_B3_DONT) &&
	    (p->i->earlyB3 == 0) && (p->i->outgoing == 1)) {
		cc_ast_verbose(4, 1, VERBOSE_PREFIX_1 "Disconnect case 2\n");
		fr.frametype = AST_FRAME_NULL;
		fr.datalen = 0;
		pipe_frame(p, (struct ast_frame *)&fr);
		return;
	}

	/*
	 * case 3: this channel is an incoming channel! the user hung up!
	 * it is much better to hangup now instead of waiting for a timeout and
	 * network caused DISCONNECT_IND!
	 */
	if (p->i->outgoing == 0) {
		cc_ast_verbose(4, 1, VERBOSE_PREFIX_1 "Disconnect case 3\n");
		if (p->i->FaxState) {
			/* in capiFax */
			p->i->FaxState = 0;
			return;
		}
		fr.frametype = AST_FRAME_NULL;
		fr.datalen = 0;
		pipe_frame(p, (struct ast_frame *)&fr);
		return;
	}
	
	/* case 4 (a.k.a. the italian case): B3 always. call is unsuccessful */
	if ((p->i->doB3 == AST_CAPI_B3_ALWAYS) &&
	    (p->i->earlyB3 == -1) && (p->i->outgoing == 1)) {
		cc_ast_verbose(4, 1, VERBOSE_PREFIX_1 "Disconnect case 4\n");
		/* wait for the 0x001e (PROGRESS), play audio and wait for a timeout from the network */
		return;
	}
	cc_ast_verbose(3, 1, VERBOSE_PREFIX_1 "Unhandled case DISCONNECT INFO_IND\n");
}

/*
 * CAPI INFO_IND
 */
static void capi_handle_info_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pipe *p)
{
	_cmsg CMSG2;
	struct ast_frame fr;
	int handled = 0;

	memset(&CMSG2,0,sizeof(_cmsg));
	INFO_RESP_HEADER(&CMSG2, ast_capi_ApplID, CMSG->Messagenumber, PLCI);
	_capi_put_cmsg(&CMSG2);

	return_on_no_pipe("INFO_IND");

	switch(INFO_IND_INFONUMBER(CMSG)) {
	case 0x8045:	/* DISCONNECT */
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "info element DISCONNECT\n");
		handle_info_disconnect(CMSG, PLCI, NCCI, p);
		handled = 1;
		break;
	case 0x001e:	/* Progress Indicator */
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "info element PI %02x %02x\n",
			INFO_IND_INFOELEMENT(CMSG)[1], INFO_IND_INFOELEMENT(CMSG)[2]);
		handle_progress_indicator(CMSG, PLCI, p);
		handled = 1;
		break;
	case 0x0070:	/* Called Party Number */
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "info element CALLED PARTY NUMBER\n");
		handle_did_digits(CMSG, PLCI, NCCI, p);
		handled = 1;
		break;
	case 0x0074:	/* Redirecting Number */
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "info element REDIRECTING NUMBER\n");
		/*
		strncpy(p->i->owner->exten, capi_number(INFO_IND_INFOELEMENT(CMSG), 3),
			sizeof(p->i->owner->exten) - 1);
		*/
		handled = 1;
		break;
	case 0x0008:	/* Cause */
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "info element CAUSE %02x %02x\n",
			INFO_IND_INFOELEMENT(CMSG)[1], INFO_IND_INFOELEMENT(CMSG)[2]);
		handled = 1;
		break;
	case 0x0018:	/* Channel Identifikation */
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "info element CHANNEL IDENTIFIKATION %02x\n",
			INFO_IND_INFOELEMENT(CMSG)[1]);
		handled = 1;
		break;
	case 0x0028:	/* DSP */
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "info element DSP\n");
		#if 0
		struct ast_frame ft = { AST_FRAME_TEXT, capi_number(INFO_IND_INFOELEMENT(CMSG),0), };
		ast_sendtext(p->i->owner,capi_number(INFO_IND_INFOELEMENT(CMSG), 0));
		ast_queue_frame(p->i->owner, &ft);
		ast_log(LOG_NOTICE,"%s\n",capi_number(INFO_IND_INFOELEMENT(CMSG),0));
		#endif
		handled = 1;
		break;
	case 0x0029:	/* Date/Time */
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "info element Date/Time %02d/%02d/%02d %02d:%02d\n",
			INFO_IND_INFOELEMENT(CMSG)[1], INFO_IND_INFOELEMENT(CMSG)[2],
			INFO_IND_INFOELEMENT(CMSG)[3], INFO_IND_INFOELEMENT(CMSG)[4],
			INFO_IND_INFOELEMENT(CMSG)[5]);
		handled = 1;
		break;
	case 0x8005:	/* SETUP */
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "info element SETUP\n");
		handled = 1;
		break;
	case 0x8007:	/* CONNECT */
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "info element CONNECT\n");
		handled = 1;
		break;
	case 0x800f:	/* CONNECT ACK */
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "info element CONNECT ACK\n");
		handled = 1;
		break;
	case 0x800d:	/* SETUP ACK */
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "info element SETUP ACK\n");
		fr.frametype = AST_FRAME_CONTROL;
		fr.subclass = AST_CONTROL_PROGRESS;
		pipe_frame(p, (struct ast_frame *)&fr);
		handled = 1;
		break;
	case 0x8001:	/* ALERTING */
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "info element ALERTING\n");
		fr.frametype = AST_FRAME_CONTROL;
		fr.subclass = AST_CONTROL_RINGING;
		pipe_frame(p, (struct ast_frame *)&fr);
		handled = 1;
		break;
	case 0x805a:	/* RELEASE COMPLETE */
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "info element RELEASE COMPLETE\n");
		handled = 1;
		break;
	case 0x804d:	/* RELEASE */
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "info element RELEASE\n");
		handled = 1;
		break;
	default:
		/* */
		break;
	}
	if (!handled) {
		cc_ast_verbose(3, 1, VERBOSE_PREFIX_2 "CAPI: unhandled INFO_IND %#x (PLCI=%#x)\n",
			INFO_IND_INFONUMBER(CMSG), PLCI);
	}
}

/*
 * CAPI FACILITY_IND
 */
static void capi_handle_facility_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pipe *p)
{
	_cmsg CMSG2;
	struct ast_frame fr;
	char dtmf;
	unsigned dtmflen;
	unsigned dtmfpos = 0;

	FACILITY_RESP_HEADER(&CMSG2, ast_capi_ApplID, CMSG->Messagenumber, PLCI);
	FACILITY_RESP_FACILITYSELECTOR(&CMSG2) = FACILITY_IND_FACILITYSELECTOR(CMSG);
	CMSG2.FacilityResponseParameters = FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG);
	_capi_put_cmsg(&CMSG2);
	
	return_on_no_pipe("FACILITY_IND");

	if (FACILITY_IND_FACILITYSELECTOR(CMSG) == 0x0001) {
		/* DTMF received */
		if (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[0] != (0xff)) {
			dtmflen = FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[0];
			FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG) += 1;
		} else {
			dtmflen = ((__u16 *) (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG) + 1))[0];
			FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG) += 3;
		}
		while (dtmflen) {
			dtmf = (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG))[dtmfpos];
			cc_ast_verbose(1, 1, VERBOSE_PREFIX_3 "c_dtmf = %c\n", dtmf);
			if ((dtmf == 'X') || (dtmf == 'Y'))
				capi_handle_dtmf_fax(p->c);
			fr.frametype = AST_FRAME_DTMF;
			fr.subclass = dtmf;
			pipe_frame(p, (struct ast_frame *)&fr);
			dtmflen--;
			dtmfpos++;
		} 
	}
	
	if (FACILITY_IND_FACILITYSELECTOR(CMSG) == 0x0003) {
		/* supplementary sservices */
#if 0
		ast_log(LOG_NOTICE,"FACILITY_IND PLCI = %#x\n",PLCI);
		ast_log(LOG_NOTICE,"%#x\n",FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[0]);
		ast_log(LOG_NOTICE,"%#x\n",FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[1]);
		ast_log(LOG_NOTICE,"%#x\n",FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[2]);
		ast_log(LOG_NOTICE,"%#x\n",FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[3]);
		ast_log(LOG_NOTICE,"%#x\n",FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[4]);
		ast_log(LOG_NOTICE,"%#x\n",FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[5]);
#endif
		/* RETRIEVE */
		if ( (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[1] == 0x3) &&
		     (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[3] == 0x2) ) {
			p->i->state = CAPI_STATE_CONNECTED;
			p->i->PLCI = p->i->onholdPLCI;
			p->i->onholdPLCI = 0;
		}
		
		/* HOLD */
		if ( (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[1] == 0x2) &&
		     (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[3] == 0x2) ) {
			if ((FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[5] != 0) &&
			    (FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[4] != 0)) { 
				/* reason != 0x0000 == problem */
				p->i->onholdPLCI = 0;
				p->i->state = CAPI_STATE_ONHOLD;
				ast_log(LOG_WARNING, "unable to put PLCI=%#x onhold, REASON = %#x%#x, maybe you need to subscribe for this...\n",
					PLCI, FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[5],
					FACILITY_IND_FACILITYINDICATIONPARAMETER(CMSG)[4]);
			} else {
				/* reason = 0x0000 == call on hold */
				p->i->state = CAPI_STATE_ONHOLD;
				if (capidebug) 
					ast_log(LOG_NOTICE, "PLCI=%#x put onhold\n", PLCI);
			}
		}
	}
}

/*
 * CAPI DATA_B3_IND
 */
static void capi_handle_data_b3_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pipe *p)
{
	_cmsg CMSG2;
	struct ast_frame fr;
	unsigned char *b3buf = NULL;
	int b3len = 0;
	int j;
	int rxavg = 0;
	int txavg = 0;

	b3len = DATA_B3_IND_DATALENGTH(CMSG);
	if ((p) && (p->i)) {
		b3buf = &(p->i->rec_buffer[AST_FRIENDLY_OFFSET]);
		memcpy(b3buf, (char *)DATA_B3_IND_DATA(CMSG), b3len);
	}
	
	/* send a DATA_B3_RESP very quickly to free the buffer in capi */
	DATA_B3_RESP_HEADER(&CMSG2, ast_capi_ApplID, CMSG->Messagenumber, 0);
	DATA_B3_RESP_NCCI(&CMSG2) = NCCI;
	DATA_B3_RESP_DATAHANDLE(&CMSG2) = DATA_B3_IND_DATAHANDLE(CMSG);
	_capi_put_cmsg(&CMSG2);

	return_on_no_pipe("DATA_B3_IND");
	
	if (p->i->fFax) {
		/* we are in fax-receive and have a file open */
		cc_ast_verbose(6, 1, VERBOSE_PREFIX_3 "DATA_B3_IND (len=%d) Fax\n",
			b3len);
		if (fwrite(b3buf, 1, b3len, p->i->fFax) != b3len)
			ast_log(LOG_WARNING, "capiAnswerFax : error writing output file (%s)\n", strerror(errno));
		return;
	}
	    
	if ((p->i->doES == 1)) {
		for (j = 0; j < b3len; j++) {
			*(b3buf + j) = reversebits[*(b3buf + j)]; 
			rxavg += abs(capiXLAW2INT( reversebits[*(b3buf + j)]));
		}
		rxavg = rxavg / j;
		for (j = 0; j < ECHO_EFFECTIVE_TX_COUNT; j++) {
			txavg += p->i->txavg[j];
		}
		txavg = txavg / j;
			    
		if ( (txavg / ECHO_TXRX_RATIO) > rxavg) { 
#ifdef CAPI_ULAW
			memset(b3buf, 255, b3len);
#else
			memset(b3buf, 84, b3len);
#endif
			if (capidebug) {
				ast_log(LOG_NOTICE, "SUPPRESSING ECHO rx=%d, tx=%d\n",
					rxavg, txavg);
			}
		}
	} else {
		for (j = 0; j < b3len; j++) {
			*(b3buf + j) = reversebits[p->i->g.rxgains[*(b3buf + j)]]; 
		}
	}

	/* just being paranoid ... */
	/*
	if (p->c->_state != AST_STATE_UP) {
		ast_setstate(p->c, AST_STATE_UP);
	}
	*/
			    
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
	cc_ast_verbose(8, 1, VERBOSE_PREFIX_3 "DATA_B3_IND (len=%d) fr.datalen=%d fr.subclass=%d\n",
		b3len, fr.datalen, fr.subclass);
	pipe_frame(p, &fr);
}

/*
 * CAPI CONNECT_ACTIVE_IND
 */
static void capi_handle_connect_active_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pipe *p)
{
	_cmsg CMSG2;
	struct ast_frame fr;
	
	CONNECT_ACTIVE_RESP_HEADER(&CMSG2, ast_capi_ApplID, CMSG->Messagenumber, 0);
	CONNECT_ACTIVE_RESP_PLCI(&CMSG2) = PLCI;
	
	if (_capi_put_cmsg(&CMSG2) != 0) {
		return;
	}
	
	return_on_no_pipe("CONNECT_ACTIVE_IND");

	if (p->i->FaxState) {
		p->i->state = CAPI_STATE_CONNECTED;
		ast_setstate(p->c, AST_STATE_UP);
		fr.frametype = AST_FRAME_CONTROL;
		fr.subclass = AST_CONTROL_ANSWER;
		fr.datalen = 0;
		pipe_frame(p,(struct ast_frame *)&fr);
		return;
	}
	
	/* normal processing */
	if (p->i->earlyB3 != 1) {
		p->i->state = CAPI_STATE_CONNECTED;
			    
		/* send a CONNECT_B3_REQ */
		if (p->i->outgoing == 1) {
			/* outgoing call */
			memset(&CMSG2, 0, sizeof(_cmsg));
			CONNECT_B3_REQ_HEADER(&CMSG2, ast_capi_ApplID, get_ast_capi_MessageNumber(),0);
			CONNECT_B3_REQ_PLCI(&CMSG2) = PLCI;
			if (_capi_put_cmsg(&CMSG2) != 0) {
				return;
			}
		} else {
			/* incoming call */
			/* RESP already sent ... wait for CONNECT_B3_IND */
		}
	} else {
		/* special treatment for early B3 connects */
		p->i->state = CAPI_STATE_BCONNECTED;
		if (p->c->_state != AST_STATE_UP) {
			ast_setstate(p->c, AST_STATE_UP);
		}
		p->i->earlyB3 = 0; /* not early anymore */
		fr.frametype = AST_FRAME_CONTROL;
		fr.subclass = AST_CONTROL_ANSWER;
		fr.datalen = 0;
		pipe_frame(p,(struct ast_frame *)&fr);
	}
}

/*
 * CAPI CONNECT_B3_ACTIVE_IND
 */
static void capi_handle_connect_b3_active_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pipe *p)
{
	_cmsg CMSG2;
	struct ast_frame fr;

	/* then send a CONNECT_B3_ACTIVE_RESP */
	CONNECT_B3_ACTIVE_RESP_HEADER(&CMSG2, ast_capi_ApplID, CMSG->Messagenumber, 0);
	CONNECT_B3_ACTIVE_RESP_NCCI(&CMSG2) = NCCI;

	if (_capi_put_cmsg(&CMSG2) != 0) {
		return;
	}
	
	return_on_no_pipe("CONNECT_ACTIVE_B3_IND");

	ast_mutex_lock(&contrlock);
	if (p->i->controller > 0) {
		capi_controllers[p->i->controller]->nfreebchannels--;
	}
	ast_mutex_unlock(&contrlock);

	p->i->state = CAPI_STATE_BCONNECTED;

	if (p->i->FaxState) {
		cc_ast_verbose(6, 0, VERBOSE_PREFIX_3 "Fax connection, no EC/DTMF\n");
	} else {
		capi_echo_canceller(p->c, EC_FUNCTION_ENABLE);
		capi_detect_dtmf(p->c, 1);
	}

	if (p->i->earlyB3 != 1) {
		ast_setstate(p->c, AST_STATE_UP);
		fr.frametype = AST_FRAME_CONTROL;
		fr.subclass = AST_CONTROL_ANSWER;
		fr.datalen = 0;
		pipe_frame(p, (struct ast_frame *)&fr);
	}
}

/*
 * CAPI DISCONNECT_B3_IND
 */
static void capi_handle_disconnect_b3_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pipe *p)
{
	_cmsg CMSG2;

	DISCONNECT_B3_RESP_HEADER(&CMSG2, ast_capi_ApplID, CMSG->Messagenumber, 0);
	DISCONNECT_B3_RESP_NCCI(&CMSG2) = NCCI;

	_capi_put_cmsg(&CMSG2);

	return_on_no_pipe("DISCONNECT_B3_IND");

	p->i->reasonb3 = DISCONNECT_B3_IND_REASON_B3(CMSG);

	switch(p->i->state) {
	case CAPI_STATE_BCONNECTED:
		/* passive disconnect */
		p->i->state = CAPI_STATE_CONNECTED;
		break;
	case CAPI_STATE_DISCONNECTING:
		/* active disconnect */
		memset(&CMSG2, 0, sizeof(_cmsg));
		DISCONNECT_REQ_HEADER(&CMSG2, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
		DISCONNECT_REQ_PLCI(&CMSG2) = PLCI;
		_capi_put_cmsg(&CMSG2);
		break;
	case CAPI_STATE_ONHOLD:
		/* no hangup */
		break;
	}

	ast_mutex_lock(&contrlock);
	if (p->i->controller > 0) {
		capi_controllers[p->i->controller]->nfreebchannels++;
	}
	ast_mutex_unlock(&contrlock);
}

/*
 * CAPI CONNECT_B3_IND
 */
static void capi_handle_connect_b3_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pipe *p)
{
	_cmsg CMSG2;

	/* then send a CONNECT_B3_RESP */
	memset(&CMSG2, 0, sizeof(_cmsg));
	CONNECT_B3_RESP_HEADER(&CMSG2, ast_capi_ApplID, CMSG->Messagenumber, 0);
	CONNECT_B3_RESP_NCCI(&CMSG2) = NCCI;
	CONNECT_B3_RESP_REJECT(&CMSG2) = 0;

	if (_capi_put_cmsg(&CMSG2) != 0) {
		return;
	}
		
	return_on_no_pipe("CONNECT_B3_IND");

	p->i->NCCI = NCCI;
	
	/*
	if (p->i->controller > 0) {
		capi_controllers[p->i->controller]->nfreebchannels--;
	}
	*/
}

/*
 * CAPI DISCONNECT_IND
 */
static void capi_handle_disconnect_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pipe *p)
{
	_cmsg CMSG2;
	struct ast_frame fr;
	struct ast_capi_pvt *i;

	DISCONNECT_RESP_HEADER(&CMSG2, ast_capi_ApplID, CMSG->Messagenumber , 0);
	DISCONNECT_RESP_PLCI(&CMSG2) = PLCI;
	_capi_put_cmsg(&CMSG2);
	
	/*
	if (p->i->controller > 0) {
		capi_controllers[p->i->controller]->nfreebchannels++;
	}
	*/

	return_on_no_pipe("DISCONNECT_IND");

	i = p->i;

	i->reason = DISCONNECT_IND_REASON(CMSG);
	
	if (p->c) {
		p->c->hangupcause = DISCONNECT_IND_REASON(CMSG) - 0x3480;
	}
			    
	if (PLCI == i->onholdPLCI) {
		/* the caller onhold hung up (or ECTed away) */
		i->onholdPLCI = 0;
		remove_pipe(PLCI);
		return;
	}
			    
	if (i->state == CAPI_STATE_DID) {
		if ((p->c) != NULL) {
			ast_hangup(p->c);
		} else {    
			ast_log(LOG_WARNING, "unable to hangup channel on DID. Channel is NULL.\n");
		}
		return;				
	}
			    
	i->faxhandled = 0;
	i->FaxState = 0;

	fr.frametype = AST_FRAME_CONTROL;
	if (DISCONNECT_IND_REASON(CMSG) == 0x34a2) {
		fr.subclass = AST_CONTROL_BUSY;
	} else {
		fr.frametype = AST_FRAME_NULL;
	}
	fr.datalen = 0;
	
	if (pipe_frame(p, (struct ast_frame *)&fr) == -1) {
		/*
		 * in this case * did not read our hangup control frame
		 * so we must hangup the channel!
		 */
		if ( (p->i->state != CAPI_STATE_DISCONNECTED) && (p->i->state != CAPI_STATE_DISCONNECTING) &&
		     (ast_check_hangup(p->c) == 0)) {
			cc_ast_verbose(1, 0, VERBOSE_PREFIX_3 "soft hangup by capi\n");
			ast_softhangup(p->c, AST_SOFTHANGUP_DEV);
		} else {
			/* dont ever hangup while hanging up! */
			/* ast_log(LOG_NOTICE,"no soft hangup by capi\n"); */
		}
	}

	if (i->state == CAPI_STATE_DISCONNECTING) {
		i->PLCI = 0;
		i->NCCI = 0;
		remove_pipe(PLCI);
	}
	i->state = CAPI_STATE_DISCONNECTED;
}

/*
 * CAPI CONNECT_IND
 */
static void capi_handle_connect_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI)
{
	struct ast_capi_pvt *i;
	struct capi_pipe *p;
	_cmsg CMSG2;
	char *DNID;
	char *CID;
	int NPLAN=0;
	int controller = 0;
	char *msn;
	char buffer[AST_CAPI_MAX_STRING];
	char buffer_r[AST_CAPI_MAX_STRING];
	char *buffer_rp = buffer_r;
	char *magicmsn = "*\0";
	char *emptyid = "\0";
	char *emptydnid = "s\0";
	int deflect = 0;

	DNID = capi_number(CONNECT_IND_CALLEDPARTYNUMBER(CMSG),1);
	if ((DNID && *DNID == 0) || !DNID) {
		DNID = emptydnid;
	}
	
	NPLAN = (CONNECT_IND_CALLINGPARTYNUMBER(CMSG)[1] & 0x70);
	CID = capi_number(CONNECT_IND_CALLINGPARTYNUMBER(CMSG), 2);
	controller = PLCI & 0xff;
	
	cc_ast_verbose(1, 1, VERBOSE_PREFIX_2 "CONNECT_IND (PLCI=%#x,DID=%s,CID=%s,CIP=%#x,CONTROLLER=%#x)\n",
		PLCI, DNID, CID, CONNECT_IND_CIPVALUE(CMSG), controller);

	if ((CONNECT_IND_BCHANNELINFORMATION(CMSG)) &&
	    ((CONNECT_IND_BCHANNELINFORMATION(CMSG)[1] == 0x02) &&
	    (capi_controllers[controller]->isdnmode != AST_CAPI_ISDNMODE_PTP))) {
		/*
		 * this is a call waiting CONNECT_IND with BChannelinformation[1] == 0x02
		 * meaning "no B or D channel for this call", since we can't do anything with call waiting now
		 * just reject it with "user busy"
		 * however...if we are a p2p BRI then the telco switch will allow us to choose the b channel
		 * so it will look like a callwaiting connect_ind to us
		 */
		ast_log(LOG_ERROR, "received a call waiting CONNECT_IND\n");
		deflect = 1;
	}

	/* well...somebody is calling us. let's set up a channel */
	ast_mutex_lock(&iflock);
	for (i = iflist; i; i = i->next) {
		if ((i->owner) || (i->incomingmsn == NULL)) {
			/* has already owner */
			continue;
		}
		if (!(i->controllers & (1 << controller))) {
			continue;
		}
		strncpy(buffer, i->incomingmsn, sizeof(buffer) - 1);
		for (msn = strtok_r(buffer, ",", &buffer_rp); msn; msn = strtok_r(NULL, ",", &buffer_rp)) {
			if (!DNID) {
				/* if no DNID, only accept if '*' was specified */
				if (strncasecmp(msn, magicmsn, strlen(msn))) {
					continue;
				}
				strncpy(i->dnid, emptydnid, sizeof(i->dnid) - 1);
			} else {
				/* make sure the number match exactly or may match on ptp mode */
				if ((strcasecmp(msn, DNID)) &&
				   ((i->isdnmode == AST_CAPI_ISDNMODE_PTMP) ||
				    (strlen(msn) >= strlen(DNID)) ||
				    (strncasecmp(msn, DNID, strlen(msn)))) &&
				   (strncasecmp(msn, magicmsn, strlen(msn)))) {
					continue;
				}
				strncpy(i->dnid, DNID, sizeof(i->dnid)-1);
			}
			if (CID != NULL) {
				if (NPLAN == CAPI_ETSI_NPLAN_NATIONAL)
					snprintf(i->cid, (sizeof(i->cid)-1), "%s%s%s",
						i->prefix, capi_national_prefix, CID);
				else if (NPLAN == CAPI_ETSI_NPLAN_INTERNAT)
					snprintf(i->cid, (sizeof(i->cid)-1), "%s%s%s",
						i->prefix, capi_international_prefix, CID);
				else
					snprintf(i->cid, (sizeof(i->cid)-1), "%s%s",
						i->prefix, CID);
			} else
				strncpy(i->cid, emptyid, sizeof(i->cid) - 1);

			i->cip = CONNECT_IND_CIPVALUE(CMSG);
			i->controller = controller;
			i->PLCI = PLCI;
			i->MessageNumber = CMSG->Messagenumber;

			p = malloc(sizeof(struct capi_pipe));
			if (!p) {
				ast_log(LOG_ERROR, "couldn't alloc capi_pipe\n");
				i = NULL;
				break;
			}
			memset(p, 0, sizeof(struct capi_pipe));
			p->PLCI = PLCI;
			p->i = i;

			ast_pthread_mutex_init(&(p->lock), NULL);
			i->mypipe = p;

			if (i->isdnmode == AST_CAPI_ISDNMODE_PTP) {
				p->c = capi_new(i,AST_STATE_DOWN);
				i->state = CAPI_STATE_DID;
			} else {
				p->c = capi_new(i, AST_STATE_RING);
			}
			p->fd = i->fd2;

			ast_mutex_lock(&pipelock);
			p->next = pipelist;
			pipelist = p;
			ast_mutex_unlock(&pipelock);
				
			ast_mutex_unlock(&iflock);

			if (deflect == 1) {
				if (i->deflect2) {
					capi_deflect(p->c, i->deflect2);
				} else
					break;
			}
			return;
		}
	}
	ast_mutex_unlock(&iflock);

	/* obviously we are not called...so tell capi to ignore this call */

	if (capidebug) {
		ast_log(LOG_ERROR, "did not find device for msn = %s\n", DNID);
	}
	
	CONNECT_RESP_HEADER(&CMSG2, ast_capi_ApplID, CMSG->Messagenumber, 0);
	CONNECT_RESP_PLCI(&CMSG2) = CONNECT_IND_PLCI(CMSG);
	CONNECT_RESP_REJECT(&CMSG2) = (deflect == 1) ? 3 : 1; /* busy : ignore */
	_capi_put_cmsg(&CMSG2);
}

/*
 * CAPI *_IND
 */
static void capi_handle_indication(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI)
{
	struct capi_pipe *p;

	if (CMSG->Command == CAPI_CONNECT) { /* only connect_ind are global (not channel specific) */
		capi_handle_connect_indication(CMSG, PLCI, NCCI);
		return;
	}

	p = find_pipe(CMSG);

	switch (CMSG->Command) {
	case CAPI_DATA_B3:
		capi_handle_data_b3_indication(CMSG, PLCI, NCCI, p);
		break;
	case CAPI_CONNECT_B3:
		capi_handle_connect_b3_indication(CMSG, PLCI, NCCI, p);
		break;
	case CAPI_CONNECT_B3_ACTIVE:
		capi_handle_connect_b3_active_indication(CMSG, PLCI, NCCI, p);
		break;
	case CAPI_DISCONNECT_B3:
		capi_handle_disconnect_b3_indication(CMSG, PLCI, NCCI, p);
		break;
	case CAPI_DISCONNECT:
		capi_handle_disconnect_indication(CMSG, PLCI, NCCI, p);
		break;
	case CAPI_FACILITY:
		capi_handle_facility_indication(CMSG, PLCI, NCCI, p);
		break;
	case CAPI_INFO:
		capi_handle_info_indication(CMSG, PLCI, NCCI, p);
		break;
	case CAPI_CONNECT_ACTIVE:
		capi_handle_connect_active_indication(CMSG, PLCI, NCCI, p);
		break;
	default:
		ast_log(LOG_ERROR, "Command.Subcommand = %#x.%#x\n",
			CMSG->Command, CMSG->Subcommand);
	}
}

/*
 * CAPI FACILITY_IND
 */
static void capi_handle_facility_confirmation(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI, struct capi_pipe *p)
{
	switch (FACILITY_CONF_FACILITYSELECTOR(CMSG)) {
	case FACILITYSELECTOR_DTMF:
		cc_ast_verbose(2, 1, VERBOSE_PREFIX_3 "DTMF conf(PLCI=%#x)\n",
			PLCI);
		break;
	case FACILITYSELECTOR_ECHO_CANCEL:
		if (FACILITY_CONF_INFO(CMSG)) {
			cc_ast_verbose(2, 0, VERBOSE_PREFIX_3 "Error setting up echo canceller (PLCI=%#x, Info=%#04x)\n",
				PLCI, FACILITY_CONF_INFO(CMSG));
			break;
		}
		if (FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[1] == EC_FUNCTION_DISABLE) {
			cc_ast_verbose(2, 0, VERBOSE_PREFIX_3 "Echo canceller successfully disabled (PLCI=%#x)\n",
				PLCI);
		} else {
			cc_ast_verbose(2, 0, VERBOSE_PREFIX_3 "Echo canceller successfully set up (PLCI=%#x)\n",
				PLCI);
		}
		break;

	case FACILITYSELECTOR_SUPPLEMENTARY:
		/* HOLD */
		if ((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[1] == 0x2) &&
		    (FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[2] == 0x0) &&
		    ((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[4] != 0x0) ||
		     (FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(CMSG)[5] != 0x0))) {
			if (p) {
				p->i->state = CAPI_STATE_BCONNECTED;
			}
		}
		break;
	default:
    		ast_log(LOG_ERROR, "CAPI: unhandled FACILITY_CONF 0x%x\n",
			FACILITY_CONF_FACILITYSELECTOR(CMSG));
	}
}

/*
 * CAPI *_CONF
 */
static void capi_handle_confirmation(_cmsg *CMSG, unsigned int PLCI, unsigned int NCCI)
{
	struct capi_pipe *p;

	p = find_pipe(CMSG);

	switch (CMSG->Command) {
	case CAPI_FACILITY:
		capi_handle_facility_confirmation(CMSG, PLCI, NCCI, p);
		break;
	case CAPI_CONNECT:
		cc_ast_verbose(1, 1, VERBOSE_PREFIX_2 "received CONNECT_CONF PLCI = %#x INFO = %#x\n",
			PLCI, CONNECT_CONF_INFO(CMSG));
		if (!p)
			break;
		if (CONNECT_CONF_INFO(CMSG) == 0) {
			p->i->PLCI = PLCI;
			p->PLCI = PLCI;
			ast_setstate(p->c, AST_STATE_DIALING);
		} else {
			/* here, something has to be done --> */
			struct ast_frame fr;
			fr.frametype = AST_FRAME_CONTROL;
			fr.subclass = AST_CONTROL_BUSY;
			fr.datalen = 0;
			pipe_frame(p, (struct ast_frame *)&fr);
		}
		break;
	case CAPI_CONNECT_B3:
		if (!p)
			break;
		if (CONNECT_B3_CONF_INFO(CMSG) == 0) {
			p->i->NCCI = NCCI;
		} else {
			p->i->earlyB3 = -1;
			p->i->doB3 = AST_CAPI_B3_DONT;
		}
		break;
	case CAPI_ALERT:
		if (!p)
			break;
		if (ALERT_CONF_INFO(CMSG) == 0) {
			p->i->state = CAPI_STATE_ALERTING;
			if (p->c->_state == AST_STATE_RING) {
				p->c->rings = 1;
			}
		} else {
			ast_log(LOG_ERROR, "CAPI: ALERT conf_error 0x%x PLCI=0x%x Command.Subcommand = %#x.%#x\n",
				CMSG->Info, PLCI, CMSG->Command, CMSG->Subcommand);
		}
		break;	    
	case CAPI_SELECT_B_PROTOCOL:
		if (CMSG->Info) {
			ast_log(LOG_ERROR, "CAPI: conf_error 0x%x PLCI=0x%x Command.Subcommand = %#x.%#x\n",
				CMSG->Info, PLCI, CMSG->Command, CMSG->Subcommand);
		} else {
			if (p->i->FaxState) {
				capi_echo_canceller(p->c, EC_FUNCTION_DISABLE);
				capi_detect_dtmf(p->c, 0);
			}
		}
		break;
	case CAPI_DATA_B3:
		if (CMSG->Info) {
			ast_log(LOG_ERROR, "CAPI: conf_error 0x%x PLCI=0x%x Command.Subcommand = %#x.%#x\n",
				CMSG->Info, PLCI, CMSG->Command, CMSG->Subcommand);
		}
		if (p) {
			ast_mutex_lock(&p->i->lockB3q);
			if (p->i->B3q > 0) {
				p->i->B3q--;
			}
			ast_mutex_unlock(&p->i->lockB3q);
		}
		break;
	case CAPI_DISCONNECT:
	case CAPI_DISCONNECT_B3:
	case CAPI_LISTEN:
	case CAPI_INFO:
		if (CMSG->Info) {
			ast_log(LOG_ERROR, "CAPI: conf_error 0x%x PLCI=0x%x Command.Subcommand = %#x.%#x\n",
				CMSG->Info, PLCI, CMSG->Command, CMSG->Subcommand);
		}
		break;
	default:
		ast_log(LOG_ERROR,"CAPI: Command.Subcommand = %#x.%#x\n",
			CMSG->Command, CMSG->Subcommand);
	}
}

/*
 * handle CAPI msg
 */
static void capi_handle_msg(_cmsg *CMSG)
{
	unsigned int NCCI = CMSG->adr.adrNCCI;
	unsigned int PLCI = (NCCI & 0xffff);

	if ((CMSG->Subcommand != CAPI_IND) &&
	    (CMSG->Subcommand != CAPI_CONF)) {
		ast_log(LOG_ERROR, "CAPI: unknown Command.Subcommand = %#x.%#x\n",
			CMSG->Command, CMSG->Subcommand);
		return;
	}

	if (CMSG->Command == CAPI_DATA_B3) {
		cc_ast_verbose(7, 1, "%s\n", capi_cmsg2str(CMSG));
	} else {
		cc_ast_verbose(4, 1, "%s\n", capi_cmsg2str(CMSG));
	}

	switch (CMSG->Subcommand) {
	case CAPI_IND:
		capi_handle_indication(CMSG, PLCI, NCCI);
		break;
	case CAPI_CONF:
		capi_handle_confirmation(CMSG, PLCI, NCCI);
		break;
	}
}


/*
 * module stuff, monitor...
 */

static void *do_monitor(void *data)
{
	unsigned int Info;
	_cmsg monCMSG;
	
	for (/* for ever */;;) {
#if 0
		if (ast_mutex_lock(&monlock)) {
			ast_log(LOG_ERROR, "Unable to get monitor lock!\n");
			return NULL;
		}
		/* do some nifty stuff */
	
		ast_mutex_unlock(&monlock);
#endif
	
		memset(&monCMSG, 0, sizeof(_cmsg));
	
		switch(Info = check_wait_get_cmsg(&monCMSG)) {
		case 0x0000:
			capi_handle_msg(&monCMSG);
			break;
		case 0x1104:
			/* CAPI queue is empty */
			break;
		default:
			/* something is wrong! */
			break;
		} /* switch */
	} /* for */
	
	/* never reached */
	return NULL;
}

/*
 * start monitor thread
 */
static int restart_monitor(void)
{
	/* stay stopped if wanted */
	if (ast_mutex_lock(&monlock)) {
		ast_log(LOG_WARNING, "Unable to get monitor lock!\n");
		return -1;
	}
	
	if (monitor_thread == pthread_self()) {
		ast_mutex_unlock(&monlock);
		ast_log(LOG_WARNING, "Unable to kill myself!\n");
		return -1;
	}
    
	/* restart */
	if (ast_pthread_create(&monitor_thread, NULL, do_monitor, NULL) < 0) {
		ast_mutex_unlock(&monlock);
		ast_log(LOG_ERROR, "Unable to start monitor thread!\n");
		return -1;
	}

	return 0;
}

/*
 * GAIN
 */
static void capi_gains(struct ast_capi_gains *g, float rxgain, float txgain)
{
	int i = 0;
	int x = 0;
	
	if (rxgain != 1.0) {
		for (i = 0; i < 256; i++) {
			x = (int)(((float)capiXLAW2INT(i)) * rxgain);
			if (x > 32767)
				x = 32767;
			if (x < -32767)
				x = -32767;
			g->rxgains[i] = capiINT2XLAW(x);
		}
	} else {
		for (i = 0; i < 256; i++) {
			g->rxgains[i] = i;
		}
	}
	
	if (txgain != 1.0) {
		for (i = 0; i < 256; i++) {
			x = (int)(((float)capiXLAW2INT(i)) * txgain);
			if (x > 32767)
				x = 32767;
			if (x < -32767)
				x = -32767;
			g->txgains[i] = capiINT2XLAW(x);
		}
	} else {
		for (i = 0; i < 256; i++) {
			g->txgains[i] = i;
		}
	}
}

/*
 * create new interface
 */
int mkif(char *incomingmsn, char *context, char *controllerstr, int devices,
	int softdtmf, int echocancel, int ecoption, int ectail, char *prefix,
	int isdnmode, int es, float rxgain, float txgain, char *deflect2,
	char *accountcode, unsigned int callgroup, unsigned int group)
{
	struct ast_capi_pvt *tmp;
	int i = 0;
	char buffer[AST_CAPI_MAX_STRING];
	char buffer_r[AST_CAPI_MAX_STRING];
	char *buffer_rp = buffer_r;
	char *contr;
	unsigned long contrmap = 0;

	for (i = 0; i < devices; i++) {
		tmp = malloc(sizeof(struct ast_capi_pvt));
		if (!tmp) {
			return -1;
		}
		memset(tmp, 0, sizeof(struct ast_capi_pvt));
		
		ast_pthread_mutex_init(&(tmp->lock),NULL);
		
		strncpy(tmp->context, context, sizeof(tmp->context) - 1);
		strncpy(tmp->incomingmsn, incomingmsn, sizeof(tmp->incomingmsn)-1);
		strncpy(tmp->prefix, prefix, sizeof(tmp->prefix)-1);
		strncpy(tmp->accountcode, accountcode, sizeof(tmp->accountcode)-1);
	    
		strncpy(buffer, controllerstr, sizeof(buffer) - 1);
		contr = strtok_r(buffer, ",", &buffer_rp);
		while (contr != NULL) {
			contrmap |= (1 << atoi(contr));
			if (capi_controllers[atoi(contr)]) {
				capi_controllers[atoi(contr)]->isdnmode = isdnmode;
				/* ast_log(LOG_NOTICE, "contr %d isdnmode %d\n",
					atoi(contr), isdnmode); */
			}
			contr = strtok_r(NULL, ",", &buffer_rp);
		}
		
		tmp->controllers = contrmap;
		capi_used_controllers |= contrmap;
		tmp->controller = 0;
		tmp->CLIR = 0;
		tmp->earlyB3 = -1;
		tmp->onholdPLCI = 0;
		tmp->doEC = echocancel;
		tmp->ecOption = ecoption;
		tmp->ecTail = ectail;
		tmp->isdnmode = isdnmode;
		tmp->doES = es;
		tmp->callgroup = callgroup;
		tmp->group = group;
		
		tmp->smoother = ast_smoother_new(AST_CAPI_MAX_B3_BLOCK_SIZE);

		tmp->rxgain = rxgain;
		tmp->txgain = txgain;
		capi_gains(&tmp->g, rxgain, txgain);

		strncpy(tmp->deflect2, deflect2, sizeof(tmp->deflect2) - 1);

		if (softdtmf == 1) {
			tmp->doDTMF = 1;
		} else {
			tmp->doDTMF = 0;
		}

		tmp->next = iflist; /* prepend */
		iflist = tmp;
		/*
		  ast_log(LOG_NOTICE, "ast_capi_pvt(%s,%s,%#x,%d) (%d,%d,%d) (%d)(%f/%f) %d\n",
		  	tmp->incomingmsn, tmp->context, (int)tmp->controllers, devices,
			tmp->doEC, tmp->ecOption, tmp->ecTail, tmp->doES, tmp->rxgain,
			tmp->txgain, callgroup);
		 */
		cc_ast_verbose(2, 0, VERBOSE_PREFIX_2 "ast_capi_pvt(%s,%s,%d,%d) (%d,%d,%d)\n",
			tmp->incomingmsn, tmp->context, tmp->controller, devices, tmp->doEC,
			tmp->ecOption, tmp->ecTail);
	}
	return 0;
}

/*
 * eval supported services
 */
static void supported_sservices(struct ast_capi_controller *cp)
{
	MESSAGE_EXCHANGE_ERROR error;
	_cmsg CMSG, CMSG2;
	struct timeval tv;
	char fac[20];

	memset(fac, 0, sizeof(fac));
	FACILITY_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
	FACILITY_REQ_CONTROLLER(&CMSG) = cp->controller;
	FACILITY_REQ_FACILITYSELECTOR(&CMSG) = 0x0003; /* sservices */
	fac[0] = 3;
	FACILITY_REQ_FACILITYREQUESTPARAMETER(&CMSG) = (char *)&fac;
	_capi_put_cmsg(&CMSG);

	tv.tv_sec = 1;
	tv.tv_usec = 0;
	
	for (/* for ever */;;) {
		error = capi20_waitformessage(ast_capi_ApplID, &tv);
		error = capi_get_cmsg(&CMSG2, ast_capi_ApplID); 
		if (error == 0) {
			if (IS_FACILITY_CONF(&CMSG2)) {
				cc_ast_verbose(5, 0, VERBOSE_PREFIX_4 "FACILITY_CONF INFO = %#x\n",
					FACILITY_CONF_INFO(&CMSG2));
				break;
			}
		}
	} 

	/* preset all zero */
	cp->holdretrieve = 0;	
	cp->terminalportability = 0;
	cp->ECT = 0;
	cp->threePTY = 0;
	cp->CF = 0;
	cp->CD = 0;
	cp->MCID = 0;
	cp->CCBS = 0;
	cp->MWI = 0;
	cp->CCNR = 0;
	cp->CONF = 0;

	/* parse supported sservices */
	if (FACILITY_CONF_FACILITYSELECTOR(&CMSG2) != 0x0003) {
		ast_log(LOG_NOTICE, "unexpected FACILITY_SELECTOR = %#x\n",
			FACILITY_CONF_FACILITYSELECTOR(&CMSG2));
		return;
	}

	if (FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(&CMSG2)[4] != 0) {
		ast_log(LOG_NOTICE, "supplementary services info  = %#x\n",
			(short)FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(&CMSG2)[1]);
		return;
	}
	
	/* success, so set the features we have */
	if ((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(&CMSG2)[6] & 1) == 1) {
		cp->holdretrieve = 1;
		cc_ast_verbose(3, 0, VERBOSE_PREFIX_4 "HOLD/RETRIEVE\n");
	}
	if (((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(&CMSG2)[6] & 2) >> 1) == 1) {
		cp->terminalportability = 1;
		cc_ast_verbose(3, 0, VERBOSE_PREFIX_4 "TERMINAL PORTABILITY\n");
	}
	if (((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(&CMSG2)[6] & 4) >> 2) == 1) {
		cp->ECT = 1;
		cc_ast_verbose(3, 0, VERBOSE_PREFIX_4 "ECT\n");
	}
	if (((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(&CMSG2)[6] & 8) >> 3) == 1) {
		cp->threePTY = 1;
		cc_ast_verbose(3, 0, VERBOSE_PREFIX_4 "3PTY\n");
	}
	if (((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(&CMSG2)[6] & 16) >> 4) == 1) {
		cp->CF = 1;
		cc_ast_verbose(3, 0, VERBOSE_PREFIX_4 "CF\n");
	}
	if (((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(&CMSG2)[6] & 32) >> 5) == 1) {
		cp->CD = 1;
		cc_ast_verbose(3, 0, VERBOSE_PREFIX_4 "CD\n");
	}
	if (((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(&CMSG2)[6] & 64) >> 6) == 1) {
		cp->MCID = 1;
		cc_ast_verbose(3, 0, VERBOSE_PREFIX_4 "MCID\n");
	}
	if (((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(&CMSG2)[6] & 128) >> 7) == 1) {
		cp->CCBS = 1;
		cc_ast_verbose(3, 0, VERBOSE_PREFIX_4 "CCBS\n");
	}
	if ((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(&CMSG2)[7] & 1) == 1) {
		cp->MWI = 1;
		cc_ast_verbose(3, 0, VERBOSE_PREFIX_4 "MWI\n");
	}
	if (((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(&CMSG2)[7] & 2) >> 1) == 1) {
		cp->CCNR = 1;
		cc_ast_verbose(3, 0, VERBOSE_PREFIX_4 "CCNR\n");
	}
	if (((FACILITY_CONF_FACILITYCONFIRMATIONPARAMETER(&CMSG2)[7] & 4) >> 2) == 1) {
		cp->CONF = 1;
		cc_ast_verbose(3, 0, VERBOSE_PREFIX_4 "CONF\n");
	}
}

/*
 * do command capi info
 */
static int capi_info(int fd, int argc, char *argv[])
{
	int i=0;
	
	if (argc != 2)
		return RESULT_SHOWUSAGE;
		
	for (i = 1; i <= capi_num_controllers; i++) {
		ast_mutex_lock(&contrlock);
		if (capi_controllers[i] != NULL) {
			ast_cli(fd, "Contr%d: %d B channels total, %d B channels free.\n",
				i, capi_controllers[i]->nbchannels, capi_controllers[i]->nfreebchannels);
		}
		ast_mutex_unlock(&contrlock);
	}
	return RESULT_SUCCESS;
}

/*
 * enable debugging
 */
static int capi_do_debug(int fd, int argc, char *argv[])
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
static int capi_no_debug(int fd, int argc, char *argv[])
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
	{ { "capi", "info", NULL }, capi_info, "Show CAPI info", info_usage };
static struct ast_cli_entry  cli_debug =
	{ { "capi", "debug", NULL }, capi_do_debug, "Enable CAPI debugging", debug_usage };
static struct ast_cli_entry  cli_no_debug =
	{ { "capi", "no", "debug", NULL }, capi_no_debug, "Disable CAPI debugging", no_debug_usage };

/*
 * calling pres
 */
static char *synopsis_callingpres = "Change the presentation for the callerid";
static char *descrip_callingpres = "Callingpres(number): Changes the presentation for the callerid. Should be called before placing an outgoing call\n";
static char *app_callingpres = "CallingPres";

static int change_callingpres(struct ast_channel *chan, void *data)
{
	int mode = 0;
	
	if (data) {
		mode = atoi((char *)data);
#ifdef CC_AST_CHANNEL_HAS_CID 
		chan->cid.cid_pres = mode;
#else
		chan->callingpres = mode;
#endif
	} else {
		ast_log(LOG_NOTICE, "Application %s requres an argument: %s(number)\n",
			app_callingpres, app_callingpres);
	}
	return 0;
}

#ifdef CC_AST_HAVE_TECH_PVT
static const struct ast_channel_tech capi_tech = {
	.type = type,
	.description = tdesc,
#ifdef CAPI_ULAW
	.capabilities = AST_FORMAT_ULAW,
#else
	.capabilities = AST_FORMAT_ALAW,
#endif
	.requester = capi_request,
	.send_digit = capi_send_digit,
	.send_text = NULL,
	.call = capi_call,
	.hangup = capi_hangup,
	.answer = capi_answer,
	.read = capi_read,
	.write = capi_write,
	.bridge = capi_bridge,
	.exception = NULL,
	.indicate = capi_indicate,
	.fixup = capi_fixup,
	.setoption = NULL,
};
#endif								

/*
 * main: load the module
 */
int load_module(void)
{
	struct ast_config *cfg;
	struct ast_variable *v;
	char *config = "capi.conf";
	char incomingmsn[AST_CAPI_MAX_STRING] = "";
	char context[AST_MAX_EXTENSION] = "";
	char prefix[AST_MAX_EXTENSION] = "";
	char accountcode[20] = "";
	char *empty = "\0";
	char deflect2[AST_MAX_EXTENSION] = "";
	char controllerstr[AST_CAPI_MAX_STRING] = "";
	int res = 0;
	int controller = 0;
	int softdtmf = 0;
	int echocancel = 1;
	int ecoption = EC_OPTION_DISABLE_G165;
	int ectail = EC_DEFAULT_TAIL;
	int es = 0;
	float rxgain = 1.0;
	float txgain = 1.0;
	int isdnmode = 0;
	unsigned int callgroup = 0;
	unsigned int group = 0;
	struct ast_capi_controller *cp;

	cfg = ast_config_load(config);

	/* We *must* have a config file otherwise stop immediately, well no */
	if (!cfg) {
		ast_log(LOG_ERROR, "Unable to load config %s, CAPI disabled\n", config);
		return 0;
	}
	if (ast_mutex_lock(&iflock)) {
		ast_log(LOG_ERROR, "Unable to lock interface list???\n");
		return -1;
	}

	strncpy(capi_national_prefix, AST_CAPI_NATIONAL_PREF, sizeof(capi_national_prefix)-1);
	strncpy(capi_international_prefix, AST_CAPI_NATIONAL_PREF, sizeof(capi_national_prefix)-1);
	v = ast_variable_browse(cfg, "general");
	while(v) {
		if (!strcasecmp(v->name, "nationalprefix")) {
			strncpy(capi_national_prefix, v->value, sizeof(capi_national_prefix)-1);
		} else if (!strcasecmp(v->name, "internationalprefix")) {
			strncpy(capi_international_prefix, v->value, sizeof(capi_international_prefix)-1);
		} else if (!strcasecmp(v->name, "rxgain")) {
			if (sscanf(v->value,"%f",&rxgain) != 1) {
				ast_log(LOG_ERROR,"invalid rxgain\n");
			}
		} else if (!strcasecmp(v->name, "txgain")) {
			if (sscanf(v->value,"%f",&txgain) != 1) {
				ast_log(LOG_ERROR,"invalid txgain\n");
			}
		}
		v = v->next;
	}
	
	if (capi20_isinstalled() != 0) {
		ast_mutex_unlock(&iflock);
		ast_log(LOG_WARNING, "CAPI not installed, CAPI disabled!\n");
		return 0;
	}

	if (capi20_register(AST_CAPI_BCHANS, AST_CAPI_MAX_B3_BLOCKS,
			AST_CAPI_MAX_B3_BLOCK_SIZE, &ast_capi_ApplID) != 0) {
		ast_mutex_unlock(&iflock);
		ast_log(LOG_NOTICE,"unable to register application at CAPI!\n");
		return -1;
	}

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined( __NetBSD__ ) || defined(__APPLE__)
	if (capi20_get_profile(0, &profile) != 0) {
#else
	if (capi20_get_profile(0, (char *)&profile) != 0) {
#endif
		ast_mutex_unlock(&iflock);
		ast_log(LOG_NOTICE,"unable to get CAPI profile!\n");
		return -1;
	} 

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined( __NetBSD__ ) || defined(__APPLE__)
	capi_num_controllers = profile.wCtlr;
#else
	capi_num_controllers = profile.ncontrollers;
#endif

	cc_ast_verbose(3, 0, VERBOSE_PREFIX_3 "This box has %d capi controller(s).\n",
		capi_num_controllers);
	
	for (controller = 1 ;controller <= capi_num_controllers; controller++) {

		memset(&profile, 0, sizeof(profile));
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined( __NetBSD__ ) || defined(__APPLE__)
		capi20_get_profile(controller, &profile);
#else
		capi20_get_profile(controller, (char *)&profile);
#endif
		cp = malloc(sizeof(struct ast_capi_controller));
		if (!cp) {
			ast_mutex_unlock(&iflock);
			ast_log(LOG_ERROR, "Error allocating memory for struct capi_controller\n");
			return -1;
		}
		memset(cp, 0, sizeof(struct ast_capi_controller));
		cp->controller = controller;
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined( __NetBSD__ ) || defined(__APPLE__)
		cp->nbchannels = profile.wNumBChannels;
		cp->nfreebchannels = profile.wNumBChannels;
		if (profile.dwGlobalOptions & CAPI_PROFILE_DTMF_SUPPORT) {
#else
		cp->nbchannels = profile.nbchannels;
		cp->nfreebchannels = profile.nbchannels;
		if ((profile.globaloptions & 8) >> 3 == 1) {
#endif
			cc_ast_verbose(3, 0, VERBOSE_PREFIX_3 "CAPI/contr%d supports DTMF\n",
				controller);
			cp->dtmf = 1;
		}
		
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined( __NetBSD__ ) || defined(__APPLE__)
		if (profile.dwGlobalOptions & CAPI_PROFILE_ECHO_CANCELLATION) {
#else
		if (profile.globaloptions2 & 1) {
#endif
			cc_ast_verbose(3, 0, VERBOSE_PREFIX_3 "CAPI/contr%d supports echo cancellation\n",
				controller);
			cp->echocancel = 1;
		}
		
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined( __NetBSD__ ) || defined(__APPLE__)
		if (profile.dwGlobalOptions & CAPI_PROFILE_SUPPLEMENTARY_SERVICES)  {
#else
		if ((profile.globaloptions & 16) >> 4 == 1) {
#endif
			cp->sservices = 1;
		}
		
		if (cp->sservices == 1) {
			cc_ast_verbose(3, 0, VERBOSE_PREFIX_3 "CAPI/contr%d supports supplementary services\n",
				controller);
			supported_sservices(cp);
		}

		capi_controllers[controller] = cp;
	}

	v = ast_variable_browse(cfg, "interfaces");
	while(v) {
		/* Create the interface list */
		if (!strcasecmp(v->name, "devices")) {
			if (mkif(incomingmsn, context, controllerstr, atoi(v->value),
			    softdtmf, echocancel, ecoption, ectail, prefix, isdnmode,
			    es, rxgain, txgain, deflect2, accountcode, callgroup, group)) {
				ast_mutex_unlock(&iflock);
				ast_log(LOG_ERROR,"Error creating interface list\n");
				return -1;
			}
			es = 0; 
			strncpy(deflect2, empty, sizeof(deflect2)-1);
		} else if (!strcasecmp(v->name, "context")) {
			strncpy(context, v->value, sizeof(context)-1);
		} else if (!strcasecmp(v->name, "incomingmsn")) {
			strncpy(incomingmsn, v->value, sizeof(incomingmsn)-1);
		} else if (!strcasecmp(v->name, "controller")) {
			strncpy(controllerstr, v->value, sizeof(controllerstr)-1);
		} else if (!strcasecmp(v->name, "softdtmf")) {
			softdtmf = atoi(v->value);
		} else if (!strcasecmp(v->name, "echosquelch")) {
			es = atoi(v->value);
		} else if (!strcasecmp(v->name, "callgroup")) {
			callgroup = ast_get_group(v->value);
		} else if (!strcasecmp(v->name, "group")) {
			group = ast_get_group(v->value);
		} else if (!strcasecmp(v->name, "deflect")) {
			strncpy(deflect2, v->value, sizeof(deflect2)-1);
		} else if (!strcasecmp(v->name, "rxgain")) {
			if (sscanf(v->value,"%f",&rxgain) != 1) {
			    ast_log(LOG_ERROR,"invalid rxgain\n");
			}
		} else if (!strcasecmp(v->name, "txgain")) {
			if (sscanf(v->value,"%f",&txgain) != 1) {
			    ast_log(LOG_ERROR,"invalid txgain\n");
			}
		} else if (!strcasecmp(v->name, "echocancel")) {
			if (!strcasecmp(v->value, "yes") || !strcasecmp(v->value, "1") || !strcasecmp(v->value, "on")) {
				echocancel=1;
				ecoption=EC_OPTION_DISABLE_G165;
			}	
			else if (!strcasecmp(v->value, "no") || !strcasecmp(v->value, "0") || !strcasecmp(v->value, "off")) {
				echocancel=0;
				ecoption=0;
			}	
			else if (!strcasecmp(v->value, "g165") || !strcasecmp(v->value, "g.165")) {
				echocancel=1;
				ecoption=EC_OPTION_DISABLE_G165;
			}	
			else if (!strcasecmp(v->value, "g164") || !strcasecmp(v->value, "g.164")) {
				echocancel=1;
				ecoption=EC_OPTION_DISABLE_G164_OR_G165;
			}	
			else if (!strcasecmp(v->value, "force")) {
				echocancel=1;
				ecoption=EC_OPTION_DISABLE_NEVER;
			}
			else {
				ast_log(LOG_ERROR,"Unknown echocancel parameter \"%s\" -- ignoring\n",v->value);
			}
		} else if (!strcasecmp(v->name, "echotail")) {
			ectail = atoi(v->value);
			if (ectail > 255)
				ectail = 255;
		} else if (!strcasecmp(v->name, "prefix")) {
			strncpy(prefix, v->value, sizeof(prefix)-1);
		} else if (!strcasecmp(v->name, "accountcode")) {
			strncpy(accountcode, v->value, sizeof(accountcode)-1);
		} else if (!strcasecmp(v->name, "isdnmode")) {
			if (!strcasecmp(v->value, "ptp") || !strcasecmp(v->value, "1"))
			    isdnmode = AST_CAPI_ISDNMODE_PTP;
			else if (!strcasecmp(v->value, "ptm") ||
				 !strcasecmp(v->value, "0") ||
				 !strcasecmp(v->value, "ptmp"))
			    isdnmode = AST_CAPI_ISDNMODE_PTMP;
			else
			    ast_log(LOG_ERROR,"Unknown isdnmode parameter \"%s\" -- ignoring\n",
			    	v->value);

		}
		v = v->next;
	}
	ast_config_destroy(cfg);

	for (controller = 1; controller <= capi_num_controllers; controller++) {
		if (capi_used_controllers & (1 << controller)) {
			if (ListenOnController(ALL_SERVICES, controller) != 0) {
				ast_log(LOG_ERROR,"Unable to listen on contr%d\n", controller);
			} else {
				cc_ast_verbose(2, 0, VERBOSE_PREFIX_3 "listening on contr%d CIPmask = %#x\n",
					controller, ALL_SERVICES);
			}
		} else {
			ast_log(LOG_WARNING,"Unused contr%d\n",controller);
		}
	}

	ast_mutex_unlock(&iflock);
	
#ifdef CC_AST_HAVE_TECH_PVT
	if (ast_channel_register(&capi_tech)) {
#else	
	if (ast_channel_register(type, tdesc, capi_capability, capi_request)) {
#endif
		ast_log(LOG_ERROR, "Unable to register channel class %s\n", type);
		unload_module();
		return -1;
	}

	ast_cli_register(&cli_info);
	ast_cli_register(&cli_debug);
	ast_cli_register(&cli_no_debug);
	
	ast_register_application(app_callingpres, change_callingpres, synopsis_callingpres, descrip_callingpres);

	restart_monitor();

	return res;
}

/*
 * unload the module
 */
int unload_module()
{
	if (capi20_release(ast_capi_ApplID) != 0)
		ast_log(LOG_WARNING,"Unable to unregister from CAPI!\n");

	ast_unregister_application(app_callingpres);
	
#ifdef CC_AST_HAVE_TECH_PVT
	ast_channel_unregister(&capi_tech);
#else
	ast_channel_unregister(type);
#endif
	
	return 0;
}

int usecount()
{
	int res;
	
	ast_mutex_lock(&usecnt_lock);
	res = usecnt;
	ast_mutex_unlock(&usecnt_lock);

	return res;
}

char *description()
{
	return desc;
}


char *key()
{
	return ASTERISK_GPL_KEY;
}

