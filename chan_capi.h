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

#define CAPI_MAX_CONTROLLERS             16
#define CAPI_MAX_B3_BLOCKS                7

/* was : 130 bytes Alaw = 16.25 ms audio not suitable for VoIP */
/* now : 160 bytes Alaw = 20 ms audio */
/* you can tune this to your need. higher value == more latency */
#define CAPI_MAX_B3_BLOCK_SIZE          160

#define CAPI_BCHANS                     120
#define ALL_SERVICES             0x1FFF03FF

#define CAPI_ISDNMODE_MSN                 0
#define CAPI_ISDNMODE_DID                 1

/*
 * helper for ast_verbose with different verbose settings
 */
#define cc_verbose(o_v, c_d, text...)					\
	do { 								\
		if ((o_v == 0) || (option_verbose > o_v)) {		\
			if ((!c_d) || ((c_d) && (capidebug))) {		\
				ast_mutex_lock(&verbose_lock);		\
				ast_verbose(text);			\
				ast_mutex_unlock(&verbose_lock);	\
			}						\
		}							\
	} while(0)

/* some helper functions */
static inline void write_capi_word(void *m, unsigned short val)
{
	((unsigned char *)m)[0] = val & 0xff;
	((unsigned char *)m)[1] = (val >> 8) & 0xff;
}
static inline unsigned short read_capi_word(void *m)
{
	unsigned short val;

	val = ((unsigned char *)m)[0] | (((unsigned char *)m)[1] << 8);	
	return (val);
}
static inline void write_capi_dword(void *m, unsigned int val)
{
	((unsigned char *)m)[0] = val & 0xff;
	((unsigned char *)m)[1] = (val >> 8) & 0xff;
	((unsigned char *)m)[2] = (val >> 16) & 0xff;
	((unsigned char *)m)[3] = (val >> 24) & 0xff;
}
static inline unsigned int read_capi_dword(void *m)
{
	unsigned int val;

	val = ((unsigned char *)m)[0] | (((unsigned char *)m)[1] << 8) |	
	      (((unsigned char *)m)[2] << 16) | (((unsigned char *)m)[3] << 24);	
	return (val);
}

/*
 * PBX defines
 */
#define cc_mutex_lock(x) ast_mutex_lock(x)
#define cc_mutex_unlock(x) ast_mutex_unlock(x)
#define cc_log(x...) ast_log(x)
#define cc_copy_string(dst, src, size) ast_copy_string(dst, src, size)

/*
 * definitions for compatibility with older versions of ast*
 */
#ifdef CC_AST_HAVE_TECH_PVT
#define CC_CHANNEL_PVT(c) (c)->tech_pvt
#else
#define CC_CHANNEL_PVT(c) (c)->pvt->pvt
#endif

#ifdef CC_AST_HAS_BRIDGED_CHANNEL
#define CC_AST_BRIDGED_CHANNEL(x) ast_bridged_channel(x)
#else
#define CC_AST_BRIDGED_CHANNEL(x) (x)->bridge
#endif

#ifdef CC_AST_HAS_BRIDGE_RESULT
#define CC_BRIDGE_RETURN enum ast_bridge_result
#else
#define CC_BRIDGE_RETURN int
#define AST_BRIDGE_COMPLETE        0
#define AST_BRIDGE_FAILED         -1
#define AST_BRIDGE_FAILED_NOWARN  -2
#define AST_BRIDGE_RETRY          -3
#endif

#ifndef AST_MUTEX_DEFINE_STATIC
#define AST_MUTEX_DEFINE_STATIC(mutex)		\
	static ast_mutex_t mutex = AST_MUTEX_INITIALIZER
#endif

/* FAX Resolutions */
#define FAX_STANDARD_RESOLUTION         0
#define FAX_HIGH_RESOLUTION             1

/* FAX Formats */
#define FAX_SFF_FORMAT                  0
#define FAX_PLAIN_FORMAT                1
#define FAX_PCX_FORMAT                  2
#define FAX_DCX_FORMAT                  3
#define FAX_TIFF_FORMAT                 4
#define FAX_ASCII_FORMAT                5
#define FAX_EXTENDED_ASCII_FORMAT       6
#define FAX_BINARY_FILE_TRANSFER_FORMAT 7

/* Fax struct */
struct fax3proto3 {
	unsigned char len;
	unsigned short resolution;
	unsigned short format;
	unsigned char Infos[100];
} __attribute__((__packed__));

typedef struct fax3proto3 B3_PROTO_FAXG3;

/* duration in ms for sending and detecting dtmfs */
#define CAPI_DTMF_DURATION              0x40

#define CAPI_NATIONAL_PREF               "0"
#define CAPI_INTERNAT_PREF              "00"

#define ECHO_TX_COUNT                   5 /* 5 x 20ms = 100ms */
#define ECHO_EFFECTIVE_TX_COUNT         3 /* 2 x 20ms = 40ms == 40-100ms  ... ignore first 40ms */
#define ECHO_TXRX_RATIO                 2.3 /* if( rx < (txavg/ECHO_TXRX_RATIO) ) rx=0; */

#define FACILITYSELECTOR_DTMF              1
#define FACILITYSELECTOR_SUPPLEMENTARY     3
#define FACILITYSELECTOR_LINE_INTERCONNECT 5
#define FACILITYSELECTOR_ECHO_CANCEL       8

#define CC_HOLDTYPE_LOCAL               0
#define CC_HOLDTYPE_HOLD                1
#define CC_HOLDTYPE_NOTIFY              2

/*
 * state combination for a normal incoming call:
 * DIS -> ALERT -> CON -> DIS
 *
 * outgoing call:
 * DIS -> CONP -> CON -> DIS
 */

#define CAPI_STATE_ALERTING             1
#define CAPI_STATE_CONNECTED            2

#define CAPI_STATE_DISCONNECTING        3
#define CAPI_STATE_DISCONNECTED         4

#define CAPI_STATE_CONNECTPENDING       5
#define CAPI_STATE_ANSWERING            6
#define CAPI_STATE_DID                  7
#define CAPI_STATE_INCALL               8

#define CAPI_STATE_ONHOLD              10

#define CAPI_B3_DONT                    0
#define CAPI_B3_ALWAYS                  1
#define CAPI_B3_ON_SUCCESS              2

#define CAPI_MAX_STRING              2048

struct cc_capi_gains {
	unsigned char txgains[256];
	unsigned char rxgains[256];
};

#define CAPI_ISDN_STATE_SETUP         0x00000001
#define CAPI_ISDN_STATE_SETUP_ACK     0x00000002
#define CAPI_ISDN_STATE_HOLD          0x00000004
#define CAPI_ISDN_STATE_ECT           0x00000008
#define CAPI_ISDN_STATE_PROGRESS      0x00000010
#define CAPI_ISDN_STATE_LI            0x00000020
#define CAPI_ISDN_STATE_DISCONNECT    0x00000040
#define CAPI_ISDN_STATE_DID           0x00000080
#define CAPI_ISDN_STATE_B3_PEND       0x00000100
#define CAPI_ISDN_STATE_B3_UP         0x00000200
#define CAPI_ISDN_STATE_PBX           0x80000000

#define CAPI_CHANNELTYPE_B            0
#define CAPI_CHANNELTYPE_D            1

/* ! Private data for a capi device */
struct capi_pvt {
	ast_mutex_t lock;
	int fd;
	int fd2;

	char name[CAPI_MAX_STRING];

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
	unsigned char send_buffer[CAPI_MAX_B3_BLOCKS * CAPI_MAX_B3_BLOCK_SIZE];
	unsigned short send_buffer_handle;

	/* receive buffer */
	unsigned char rec_buffer[CAPI_MAX_B3_BLOCK_SIZE + AST_FRIENDLY_OFFSET];

	/* current state */
	int state;

	/* the state of the line */
	unsigned int isdnstate;
	int cause;
	
	char context[AST_MAX_EXTENSION];
	/*! Multiple Subscriber Number we listen to (, seperated list) */
	char incomingmsn[CAPI_MAX_STRING];	
	/*! Prefix to Build CID */
	char prefix[AST_MAX_EXTENSION];	
	/* the default caller id */
	char defaultcid[CAPI_MAX_STRING];

	/*! Caller ID if available */
	char cid[AST_MAX_EXTENSION];	
	/*! Dialed Number if available */
	char dnid[AST_MAX_EXTENSION];
	/* callerid type of number */
	int cid_ton;

	char accountcode[20];	

	unsigned int callgroup;
	unsigned int group;
	
	/* language */
	char language[MAX_LANGUAGE];	

	/* additional numbers to dial */
	int doOverlap;
	char overlapdigits[AST_MAX_EXTENSION];

	int calledPartyIsISDN;
	/* this is an outgoing channel */
	int outgoing;
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
	int ecSelector;
	/* isdnmode MSN or DID */
	int isdnmode;
	/* NT-mode */
	int ntmode;
	/* Answer before getting digits? */
	int immediate;
	/* which holdtype */
	int holdtype;
	int doholdtype;
	/* line interconnect allowed */
	int bridge;
	/* channeltype */
	int channeltype;

	/* Common ISDN Profile (CIP) */
	int cip;
	
	/* if not null, receiving a fax */
	FILE *fFax;
	/* Has a fax tone already been handled? */
	int faxhandled;
	/* Fax ready ? */
	int FaxState;

	/* not all codecs supply frames in nice 160 byte chunks */
	struct ast_smoother *smoother;
	/* ok, we stop to be nice and give them the lowest possible latency 130 samples * 2 = 260 bytes */

	/* outgoing queue count */
	int B3q;

	/* do ECHO SURPRESSION */
	int ES;
	int doES;
	short txavg[ECHO_TX_COUNT];
	float rxmin;
	float txmin;
	
	struct cc_capi_gains g;

	float txgain;
	float rxgain;
	struct ast_dsp *vad;

	unsigned int reason;
	unsigned int reasonb3;

	/*! Next channel in list */
	struct capi_pvt *next;
};

struct cc_capi_profile {
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
} __attribute__((__packed__));

struct cc_capi_conf {
	char name[CAPI_MAX_STRING];	
	char language[MAX_LANGUAGE];
	char incomingmsn[CAPI_MAX_STRING];
	char defaultcid[CAPI_MAX_STRING];
	char context[AST_MAX_EXTENSION];
	char controllerstr[CAPI_MAX_STRING];
	char prefix[AST_MAX_EXTENSION];
	char accountcode[20];
	int devices;
	int softdtmf;
	int echocancel;
	int ecoption;
	int ectail;
	int ecSelector;
	int isdnmode;
	int ntmode;
	int immediate;
	int holdtype;
	int es;
	int bridge;
	unsigned int callgroup;
	unsigned int group;
	float rxgain;
	float txgain;
};

struct cc_capi_controller {
	/* which controller is this? */
	int controller;
	/* how many bchans? */
	int nbchannels;
	/* free bchans */
	int nfreebchannels;
	/* features: */
	int dtmf;
	int echocancel;
	int sservices;	/* supplementray services */
	int lineinterconnect;
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
#define CAPI_CIPI_SPEECH                        0x01
#define CAPI_CIPI_DIGITAL                       0x02
#define CAPI_CIPI_RESTRICTED_DIGITAL            0x03
#define CAPI_CIPI_3K1AUDIO                      0x04
#define CAPI_CIPI_7KAUDIO                       0x05
#define CAPI_CIPI_VIDEO                         0x06
#define CAPI_CIPI_PACKET_MODE                   0x07
#define CAPI_CIPI_56KBIT_RATE_ADAPTION          0x08
#define CAPI_CIPI_DIGITAL_W_TONES               0x09
#define CAPI_CIPI_TELEPHONY                     0x10
#define CAPI_CIPI_FAX_G2_3                      0x11
#define CAPI_CIPI_FAX_G4C1                      0x12
#define CAPI_CIPI_FAX_G4C2_3                    0x13
#define CAPI_CIPI_TELETEX_PROCESSABLE           0x14
#define CAPI_CIPI_TELETEX_BASIC                 0x15
#define CAPI_CIPI_VIDEOTEX                      0x16
#define CAPI_CIPI_TELEX                         0x17
#define CAPI_CIPI_X400                          0x18
#define CAPI_CIPI_X200                          0x19
#define CAPI_CIPI_7K_TELEPHONY                  0x1a
#define CAPI_CIPI_VIDEO_TELEPHONY_C1            0x1b
#define CAPI_CIPI_VIDEO_TELEPHONY_C2            0x1c

/* Transfer capabilities */
#define PRI_TRANS_CAP_SPEECH                    0x00
#define PRI_TRANS_CAP_DIGITAL                   0x08
#define PRI_TRANS_CAP_RESTRICTED_DIGITAL        0x09
#define PRI_TRANS_CAP_3K1AUDIO                  0x10
#define PRI_TRANS_CAP_DIGITAL_W_TONES           0x11
#define PRI_TRANS_CAP_VIDEO                     0x18

#endif
