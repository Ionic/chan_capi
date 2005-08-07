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
 
#ifndef _ASTERISK_CAPI_H
#define _ASTERISK_CAPI_H

#define AST_CAPI_MAX_CONTROLLERS        16
#define AST_CAPI_MAX_DEVICES            30
#define AST_CAPI_MAX_BUF                160

#define AST_CAPI_MAX_B3_BLOCKS          7

/* was : 130 bytes Alaw = 16.25 ms audio not suitable for VoIP */
/* now : 160 bytes Alaw = 20 ms audio */
/* you can tune this to your need. higher value == more latency */
#define AST_CAPI_MAX_B3_BLOCK_SIZE      160

#define AST_CAPI_BCHANS                 120
#define ALL_SERVICES                    0x1FFF03FF

#define AST_CAPI_ISDNMODE_PTMP          0
#define AST_CAPI_ISDNMODE_PTP           1

/* some helper functions */
static inline void write_capi_word(void *m, unsigned short val)
{
	((unsigned char *)m)[0] = val & 0xff;
	((unsigned char *)m)[1] = (val >> 8) & 0xff;
}

/*
 * definitions for compatibility with older versions of ast*
 */
#ifdef CC_AST_HAVE_TECH_PVT
#define CC_AST_CHANNEL_PVT(c) c->tech_pvt
#else
#define CC_AST_CHANNEL_PVT(c) c->pvt->pvt
#endif

#ifndef AST_MUTEX_DEFINE_STATIC
#define AST_MUTEX_DEFINE_STATIC(mutex)		\
	static ast_mutex_t mutex = AST_MUTEX_INITIALIZER
#endif


/* duration in ms for sending and detecting dtmfs */
#define AST_CAPI_DTMF_DURATION          0x40

#define AST_CAPI_NATIONAL_PREF          "0"
#define AST_CAPI_INTERNAT_PREF          "00"

#define ECHO_TX_COUNT                   5 /* 5 x 20ms = 100ms */
#define ECHO_EFFECTIVE_TX_COUNT         3 /* 2 x 20ms = 40ms == 40-100ms  ... ignore first 40ms */
#define ECHO_TXRX_RATIO                 2.3 /* if( rx < (txavg/ECHO_TXRX_RATIO) ) rx=0; */

#define FACILITYSELECTOR_DTMF           1
#define FACILITYSELECTOR_SUPPLEMENTARY  3
#define FACILITYSELECTOR_ECHO_CANCEL    6

/*
 * state combination for a normal incoming call:
 * DIS -> ALERT -> CON -> BCON -> CON -> DIS
 *
 * outgoing call:
 * DIS -> CONP -> BCONNECTED -> CON -> DIS
 */

#define CAPI_STATE_ALERTING             1
#define CAPI_STATE_CONNECTED            2
#define CAPI_STATE_BCONNECTED           3

#define CAPI_STATE_DISCONNECTING        4
#define CAPI_STATE_DISCONNECTED         5

#define CAPI_STATE_CONNECTPENDING       6
#define CAPI_STATE_ANSWERING            7
#define CAPI_STATE_DID                  8
#define CAPI_STATE_INCALL               9

#define CAPI_STATE_PUTTINGONHOLD        10
#define CAPI_STATE_RETRIEVING           11
#define CAPI_STATE_ONHOLD               12


#define AST_CAPI_B3_DONT                0
#define AST_CAPI_B3_ALWAYS              1
#define AST_CAPI_B3_ON_SUCCESS          2

#define AST_CAPI_MAX_STRING             2048

struct ast_capi_gains {
	unsigned char txgains[256];
	unsigned char rxgains[256];
};

#define CAPI_ISDN_STATE_SETUP_ACK     0x01

/* ! Private data for a capi device */
struct ast_capi_pvt {
	ast_mutex_t lock;
	int fd;
	int fd2;

	char name[AST_CAPI_MAX_STRING];	

	/*! Channel we belong to, possibly NULL */
	struct ast_channel *owner;		
	/*! Frame */
	struct ast_frame fr;			
	
	/* capi message number */
	_cword MessageNumber;	
	unsigned int NCCI;
	unsigned int PLCI;
	/* on which controller we do live */
	int controller;
	
	/* we could live on those */
	unsigned long controllers;

	/* send buffer */
	unsigned char send_buffer[AST_CAPI_MAX_B3_BLOCKS * AST_CAPI_MAX_B3_BLOCK_SIZE];
	unsigned short send_buffer_handle;

	/* receive buffer */
	unsigned char rec_buffer[AST_CAPI_MAX_BUF + AST_FRIENDLY_OFFSET];

	/* current state */
	int state;

	unsigned int isdnstate;
	int cause;
	
	char context[AST_MAX_EXTENSION];
	/*! Multiple Subscriber Number we listen to (, seperated list) */
	char incomingmsn[AST_CAPI_MAX_STRING];	
	/*! Prefix to Build CID */
	char prefix[AST_MAX_EXTENSION];	

	/*! Caller ID if available */
	char cid[AST_MAX_EXTENSION];	
	/*! Dialed Number if available */
	char dnid[AST_MAX_EXTENSION];
	/* callerid type of number */
	int cid_ton;

	char accountcode[20];	

	unsigned int callgroup;
	unsigned int group;
	
	/*! default language */
	char language[MAX_LANGUAGE];	

	/* additional numbers to dial */
	int doOverlap;
	char overlapdigits[AST_MAX_EXTENSION];

	int calledPartyIsISDN;
	/* this is an outgoing channel */
	int outgoing;
	/* are we doing early B3 connect on this interface? */
	int earlyB3;
	/* should we do early B3 on this interface? */
	int doB3;
	/* store plci here for the call that is onhold */
	unsigned int onholdPLCI;
	/* do software dtmf detection */
	int doDTMF;
	/* CAPI echo cancellation */
	int doEC;
	int ecOption;
	int ecTail;
	/* isdnmode ptp or ptm */
	int isdnmode;

	/* Common ISDN Profile (CIP) */
	int cip;
	
	/* if not null, receiving a fax */
	FILE *fFax;
	/* Has a fax tone already been handled? */
	int faxhandled;
	/* Fax ready ? */
	int FaxState;

	/* deflect on circuitbusy */
	char deflect2[AST_MAX_EXTENSION];
	
	/* not all codecs supply frames in nice 160 byte chunks */
	struct ast_smoother *smoother;
	/* ok, we stop to be nice and give them the lowest possible latency 130 samples * 2 = 260 bytes */

	/* outgoing queue count */
	int B3q;
	ast_mutex_t lockB3q;

	/* do ECHO SURPRESSION */
	int doES;
	short txavg[ECHO_TX_COUNT];
	float rxmin;
	float txmin;
	
	struct ast_capi_gains g;

	float txgain;
	float rxgain;
	struct ast_dsp *vad;

	unsigned int reason;
	unsigned int reasonb3;
	
	/*! Next channel in list */
	struct ast_capi_pvt *next;			
};

struct ast_capi_profile {
	unsigned short ncontrollers;
	unsigned short nbchannels;
	unsigned char globaloptions;
	unsigned char globaloptions2;
	unsigned char globaloptions3;
	unsigned char globaloptions4;
	unsigned int b1protocols;
	unsigned int b2protocols;
	unsigned int b3protocols;
	unsigned int reserved3[6];
	unsigned int manufacturer[5];
};

struct ast_capi_conf {
	char name[AST_CAPI_MAX_STRING];	
	char incomingmsn[AST_CAPI_MAX_STRING];
	char context[AST_MAX_EXTENSION];
	char controllerstr[AST_CAPI_MAX_STRING];
	char prefix[AST_MAX_EXTENSION];
	char deflect2[AST_MAX_EXTENSION];
	char accountcode[20];
	int devices;
	int softdtmf;
	int echocancel;
	int ecoption;
	int ectail;
	int isdnmode;
	int es;
	unsigned int callgroup;
	unsigned int group;
	float rxgain;
	float txgain;
};

struct ast_capi_controller {
	/* which controller is this? */
	int controller;
	/* how many bchans? */
	int nbchannels;
	/* free bchans */
	int nfreebchannels;
	/* DID */
	int isdnmode;
	/* features: */
	int dtmf;
	int echocancel;
	int sservices;	/* supplementray services */
	/* supported sservices: */
	int holdretrieve;
	int terminalportability;
	int ECT;
	int threePTY;
	int CF;
	int CD;
	int MCID;
	int CCBS;
	int MWI;
	int CCNR;
	int CONF;
};


/* ETSI 300 102-1 information element identifiers */
#define CAPI_ETSI_IE_CAUSE                      0x08
#define CAPI_ETSI_IE_PROGRESS_INDICATOR         0x1e
#define CAPI_ETSI_IE_CALLED_PARTY_NUMBER        0x70

/* ETIS 300 102-1 message types */
#define CAPI_ETSI_ALERTING                      0x01
#define CAPI_ETSI_SETUP_ACKKNOWLEDGE            0x0d
#define CAPI_ETSI_DISCONNECT                    0x45

/* ETSI 300 102-1 Numbering Plans */
#define CAPI_ETSI_NPLAN_NATIONAL                0x20
#define CAPI_ETSI_NPLAN_INTERNAT                0x10

/* Common ISDN Profiles (CIP) */
#define CAPI_CIP_SPEECH                         0x01
#define CAPI_CIP_DIGITAL                        0x02
#define CAPI_CIP_RESTRICTED_DIGITAL             0x03
#define CAPI_CIP_3K1AUDIO                       0x04
#define CAPI_CIP_7KAUDIO                        0x05
#define CAPI_CIP_VIDEO                          0x06
#define CAPI_CIP_PACKET_MODE                    0x07
#define CAPI_CIP_56KBIT_RATE_ADAPTION           0x08
#define CAPI_CIP_DIGITAL_W_TONES                0x09
#define CAPI_CIP_TELEPHONY                      0x10
#define CAPI_CIP_FAX_G2_3                       0x11
#define CAPI_CIP_FAX_G4C1                       0x12
#define CAPI_CIP_FAX_G4C2_3                     0x13
#define CAPI_CIP_TELETEX_PROCESSABLE            0x14
#define CAPI_CIP_TELETEX_BASIC                  0x15
#define CAPI_CIP_VIDEOTEX                       0x16
#define CAPI_CIP_TELEX                          0x17
#define CAPI_CIP_X400                           0x18
#define CAPI_CIP_X200                           0x19
#define CAPI_CIP_7K_TELEPHONY                   0x1a
#define CAPI_CIP_VIDEO_TELEPHONY_C1             0x1b
#define CAPI_CIP_VIDEO_TELEPHONY_C2             0x1c

/* Transfer capabilities */
#define PRI_TRANS_CAP_SPEECH                    0x00
#define PRI_TRANS_CAP_DIGITAL                   0x08
#define PRI_TRANS_CAP_RESTRICTED_DIGITAL        0x09
#define PRI_TRANS_CAP_3K1AUDIO                  0x10
#define PRI_TRANS_CAP_DIGITAL_W_TONES           0x11
#define PRI_TRANS_CAP_VIDEO                     0x18

#endif
