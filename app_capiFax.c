/*
 * (CAPI*)
 *
 * Receive a fax with CAPI API.
 * Usage : capiAnswerFax(path_output_file.SFF)
 *
 * This function can be called even after a regular Answer (voice mode),
 * the channel will be changed to Fax Mode.
 * 
 * Example of use :
 * line number 123, play something, if a fax tone is detected, handle it
 * line number 124, answer directly in fax mode
 *
 * [incoming]
 * exten => 123,1,Answer()
 * exten => 123,2,BackGround(jpop)
 * exten => 124,1,Goto(handle_fax,s,1)
 * exten => fax,1,Goto(handle_fax,s,1)
 *
 * [handle_fax]
 * exten => s,1,capiAnswerFax(/tmp/${UNIQUEID})
 * exten => s,2,Hangup()
 * exten => h,1,deadagi,fax.php // Run SFF2TIFF and mail it.
 *
 * This program is free software and may be modified and 
 * distributed under the terms of the GNU Public License.
 * 
 * (c) 2004,2005 by Carl Sempla, Cedrik Hans, Frank Sautter
 * 
 */

#include "config.h"

#include <asterisk/file.h>
#include <asterisk/logger.h>
#include <asterisk/channel.h>
#ifndef CC_AST_HAVE_TECH_PVT
#include <asterisk/channel_pvt.h>
#endif
#include <asterisk/logger.h>
#include <asterisk/options.h>
#include <asterisk/pbx.h>
#include <asterisk/module.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include <pthread.h>
#include <capi20.h>
#include "chan_capi_pvt.h"
#include "chan_capi_app.h"

/* FAX Resolutions */
#define FAX_STANDARD_RESOLUTION		0
#define FAX_HIGH_RESOLUTION		1

/* FAX Formats */
#define FAX_SFF_FORMAT			0
#define FAX_PLAIN_FORMAT		1
#define FAX_PCX_FORMAT			2
#define FAX_DCX_FORMAT			3
#define FAX_TIFF_FORMAT			4
#define FAX_ASCII_FORMAT		5
#define FAX_EXTENDED_ASCII_FORMAT	6
#define FAX_BINARY_FILE_TRANSFER_FORMAT	7

typedef struct fax3proto3 {
	unsigned char len;
	unsigned short resolution __attribute__ ((packed));
	unsigned short format __attribute__ ((packed));
	unsigned char Infos[100] __attribute__ ((packed));
} B3_PROTO_FAXG3;

static char *tdesc = "(CAPI*) Receive Faxes.";
static char *app = "capiAnswerFax";
static char *synopsis = "Answer Fax with CAPI";
STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

void SetupB3Config(B3_PROTO_FAXG3  *B3conf, int FAX_Format)
{
	int len1;
	int len2;
	char *stationID = "00000000";
	char *headLine  = "CAPI FAXServer";

	B3conf->resolution = 0;
	B3conf->format = (unsigned short)FAX_Format;
	len1 = strlen(stationID);
	B3conf->Infos[0] = (unsigned char)len1;
	strcpy((char *)&B3conf->Infos[1], stationID);
	len2 = strlen(headLine);
	B3conf->Infos[len1 + 1] = (unsigned char)len2;
	strcpy((char *)&B3conf->Infos[len1 + 2], headLine);
	B3conf->len = (unsigned char)(2 * sizeof(unsigned short) + len1 + len2 + 2);
}

static int capi_change_bchan_fax(struct ast_channel *c) 
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(c);
	_cmsg CMSG;
	B3_PROTO_FAXG3  B3conf;

	SetupB3Config(&B3conf, FAX_SFF_FORMAT); /* Format ignored by eicon cards */
	
	DISCONNECT_B3_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
	DISCONNECT_B3_REQ_NCCI(&CMSG) = i->NCCI;
	_capi_put_cmsg(&CMSG);
		
	/* wait for the B3 layer to go down */
	while (i->state != CAPI_STATE_CONNECTED) {
		usleep(10000);
	}

	/* TODO: if state != BCONNECTED */

	SELECT_B_PROTOCOL_REQ_HEADER(&CMSG, ast_capi_ApplID, get_ast_capi_MessageNumber(), 0);
	SELECT_B_PROTOCOL_REQ_PLCI(&CMSG) = i->PLCI;
	SELECT_B_PROTOCOL_REQ_B1PROTOCOL(&CMSG) = 4;
	SELECT_B_PROTOCOL_REQ_B2PROTOCOL(&CMSG) = 4;
	SELECT_B_PROTOCOL_REQ_B3PROTOCOL(&CMSG) = 4;
	SELECT_B_PROTOCOL_REQ_B1CONFIGURATION(&CMSG) = NULL;
	SELECT_B_PROTOCOL_REQ_B2CONFIGURATION(&CMSG) = NULL;
	SELECT_B_PROTOCOL_REQ_B3CONFIGURATION(&CMSG) = (_cstruct)&B3conf;
	_capi_put_cmsg(&CMSG);

	return 0;
}

static int capi_answer_fax(struct ast_channel *c) 
{
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(c); 
	MESSAGE_EXCHANGE_ERROR  error;
	_cmsg CMSG;
	char buf[AST_CAPI_MAX_STRING];
	char *dnid;
	B3_PROTO_FAXG3 B3conf;

	if (i->isdnmode && (strlen(i->incomingmsn) < strlen(i->dnid))) {
		dnid = i->dnid + strlen(i->incomingmsn);
	} else {
		dnid = i->dnid;
	}

	SetupB3Config(&B3conf, FAX_SFF_FORMAT); /* Format ignored by eicon cards */

	CONNECT_RESP_HEADER(&CMSG, ast_capi_ApplID, i->MessageNumber, 0);
	CONNECT_RESP_PLCI(&CMSG) = i->PLCI;
	CONNECT_RESP_REJECT(&CMSG) = 0;
	buf[0] = strlen(dnid)+2;
	buf[1] = 0x0;
	buf[2] = 0x80;
	strncpy(&buf[3],dnid,sizeof(buf)-4);
	CONNECT_RESP_CONNECTEDNUMBER(&CMSG) = buf;
	CONNECT_RESP_CONNECTEDSUBADDRESS(&CMSG) = NULL;
	CONNECT_RESP_LLC(&CMSG) = NULL;
	CONNECT_RESP_B1PROTOCOL(&CMSG) = 4; /* T.30 modem for Group 3 fax */
	CONNECT_RESP_B2PROTOCOL(&CMSG) = 4; /* T.30 for Group 3 fax */
	CONNECT_RESP_B3PROTOCOL(&CMSG) = 4; /* T.30 for Group 3 fax */
	CONNECT_RESP_B3CONFIGURATION(&CMSG) = (_cstruct)&B3conf;
 
	cc_ast_verbose(3, 0, VERBOSE_PREFIX_3 "CAPI Answering in fax mode for MSN %s\n", dnid);

	if ((error = _capi_put_cmsg(&CMSG)) != 0) {
		return -1;
	}

	i->state = CAPI_STATE_ANSWERING;
	i->doB3 = AST_CAPI_B3_DONT;
	i->outgoing = 0;
	i->earlyB3 = -1;

	return 0;
}

static int capianswerfax_exec(struct ast_channel *chan, void *data)
{
	int res = 0;
	struct localuser *u;
	char *vdata;
	struct ast_capi_pvt *i = CC_AST_CHANNEL_PVT(chan);

	if (!data) { /* no data implies no filename or anything is present */
		ast_log(LOG_WARNING, "capiAnswerFax requires an argument (filename)\n");
		return -1;
	}
	vdata = ast_strdupa(data);

	LOCAL_USER_ADD(u);

	if (!strcasecmp("CAPI", chan->type) == 0) {
		ast_log(LOG_WARNING, "capiFax only works on CAPI channels, check your extensions.conf!\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	i->fFax = fopen(vdata, "wb");

	if (i->fFax == NULL) {
		ast_log(LOG_WARNING, "capiAnswerFax: can't create the output file (%s)\n", strerror(errno));
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	i->FaxState = 1;
	if (i->state != CAPI_STATE_BCONNECTED) {
		capi_answer_fax(chan);
	} else {
		capi_change_bchan_fax(chan);
	}
	while (i->FaxState) {
		sleep(1);
	}

	/* if the file has zero length */
	if (ftell(i->fFax) == 0L)
		res = -1;
	
	cc_ast_verbose(2, 1, VERBOSE_PREFIX_3 "Closing fax file...\n");
	fclose(i->fFax);
	i->fFax = NULL;

	switch (i->reason) {
	case 0x3490:
	case 0x349f:
		res = (i->reasonb3 == 0) ? 0 : -1;
		break;
	default:
		res = -1;
	}
			
	if (res != 0) {
		cc_ast_verbose(2, 0,
			VERBOSE_PREFIX_1 "capiAnswerFax: fax receive failed.\n");
		unlink(vdata);
	} else {
		cc_ast_verbose(2, 0,
			VERBOSE_PREFIX_1 "capiAnswerFax: fax received.\n");
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
	return ast_register_application(app, capianswerfax_exec, synopsis, tdesc);
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

