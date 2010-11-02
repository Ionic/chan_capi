/*
 * An implementation of Common ISDN API 2.0 for Asterisk
 *
 * Copyright (C) 2005-2010 Cytronics & Melware
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

#include "chan_capi_platform.h"
#include "chan_capi20.h"
#include "chan_capi.h"
#include "chan_capi_qsig.h"
#include "chan_capi_utils.h"
#include "chan_capi_chat.h"
#include "chan_capi_cli.h"
#ifdef DIVA_STATUS
#include "divastatus_ifc.h"
#endif

/*
	LOCALS
	*/
static int pbx_capi_cli_get_locks(struct capi_pvt *i);

/*
 * usages
 */
static char info_usage[] =
"Usage: " CC_MESSAGE_NAME " info\n"
"       Show info about B channels on controllers.\n";

static char show_channels_usage[] =
"Usage: " CC_MESSAGE_NAME " show channels\n"
"       Show info about B channels.\n";

static char show_resources_usage[] =
"Usage: " CC_MESSAGE_NAME " show ressources\n"
"       Show info about used by channels resources.\n";

static char debug_usage[] =
"Usage: " CC_MESSAGE_NAME " debug\n"
"       Enables dumping of " CC_MESSAGE_BIGNAME " packets for debugging purposes\n";

static char no_debug_usage[] =
"Usage: " CC_MESSAGE_NAME " no debug\n"
"       Disables dumping of " CC_MESSAGE_BIGNAME " packets for debugging purposes\n";

static char qsig_debug_usage[] =
"Usage: " CC_MESSAGE_NAME " qsig debug\n"
"       Enables dumping of QSIG facilities for debugging purposes\n";

static char qsig_no_debug_usage[] =
"Usage: " CC_MESSAGE_NAME " qsig no debug\n"
"       Disables dumping of QSIG facilities for debugging purposes\n";

static char show_exec_usage[] =
"Usage: " CC_MESSAGE_NAME " info\n"
"       Exec chancapi command on selected interface (exec interface command parameters).\n";

#ifndef CC_AST_HAS_VERSION_1_6
static
#endif
char chatinfo_usage[] =
"Usage: " CC_MESSAGE_NAME " chatinfo\n"
"       Show info about chat status.\n";

#define CC_CLI_TEXT_INFO "Show " CC_MESSAGE_BIGNAME " info"
#define CC_CLI_TEXT_SHOW_CHANNELS "Show B-channel info"
#define CC_CLI_TEXT_DEBUG "Enable " CC_MESSAGE_BIGNAME " debugging"
#define CC_CLI_TEXT_NO_DEBUG "Disable " CC_MESSAGE_BIGNAME " debugging"
#define CC_CLI_TEXT_QSIG_DEBUG "Enable QSIG debugging"
#define CC_CLI_TEXT_QSIG_NO_DEBUG "Disable QSIG debugging"
#define CC_CLI_TEXT_CHATINFO "Show " CC_MESSAGE_BIGNAME " chat info"
#define CC_CLI_TEXT_SHOW_RESOURCES "Show used resources"
#define CC_CLI_TEXT_EXEC_CAPICOMMAND "Exec command"

/*
 * helper functions to convert conf value to string
 */
static char *show_bproto(int bproto)
{
	switch(bproto) {
	case CC_BPROTO_TRANSPARENT:
		return "trans";
	case CC_BPROTO_FAXG3:
	case CC_BPROTO_FAX3_BASIC:
		return " fax ";
	case CC_BPROTO_RTP:
		return " rtp ";
	case CC_BPROTO_VOCODER:
		return " vocoder ";
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
#ifdef CC_AST_HAS_VERSION_1_6
static char *pbxcli_capi_show_channels(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
#else
static int pbxcli_capi_show_channels(int fd, int argc, char *argv[])
#endif
{
	struct capi_pvt *i;
	char iochar;
	char i_state[80];
	char b3q[32];
	int required_args;
	int provided_args;
	const char* required_channel_name = NULL;

#ifdef CC_AST_HAS_VERSION_1_6
	int fd = a->fd;

	if (cmd == CLI_INIT) {
		e->command = CC_MESSAGE_NAME " show channels";
		e->usage = show_channels_usage;
		return NULL;
	} else if (cmd == CLI_GENERATE)
		return NULL;
	required_args = e->args;
	provided_args = a->argc;
	if (required_args < provided_args) {
		required_channel_name = a->argv[required_args];
	}
#else
	required_args = 3;
	provided_args = argc;
	if (required_args < provided_args) {
		required_channel_name = argv[required_args];
	}
#endif

	ast_cli(fd, CC_MESSAGE_BIGNAME " B-channel information:\n");
	ast_cli(fd, "Line-Name       NTmode state i/o bproto isdnstate   ton  number\n");
	ast_cli(fd, "----------------------------------------------------------------\n");

	pbx_capi_lock_interfaces();

	for (i = capi_iflist; i; i = i->next) {
		if (i->channeltype != CAPI_CHANNELTYPE_B)
			continue;
		if ((required_channel_name != NULL) && (strcmp(required_channel_name, i->vname) != 0))
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

	pbx_capi_unlock_interfaces();

#ifdef CC_AST_HAS_VERSION_1_6
	return CLI_SUCCESS;
#else
	return RESULT_SUCCESS;
#endif
}

/*
 * do command capi show resources
 */
#ifdef CC_AST_HAS_VERSION_1_6
static char *pbxcli_capi_show_resources(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
#else
static int pbxcli_capi_show_resources(int fd, int argc, char *argv[])
#endif
{
	int ifc_type;
	const struct capi_pvt *i;
	int required_args;
	int provided_args;
	const char* required_channel_name = NULL;
	struct {
		const struct capi_pvt *head;
		void (*lock_proc)(void);
		void (*unlock_proc)(void);
	} data[2];

	data[0].head        = capi_iflist;
	data[0].lock_proc   = pbx_capi_lock_interfaces;
	data[0].unlock_proc = pbx_capi_unlock_interfaces;
	data[1].head        = pbx_capi_get_nulliflist();
	data[1].lock_proc   = pbx_capi_nulliflist_lock;
	data[1].unlock_proc = pbx_capi_nulliflist_unlock;

#ifdef CC_AST_HAS_VERSION_1_6
	int fd = a->fd;

	if (cmd == CLI_INIT) {
		e->command = CC_MESSAGE_NAME " show resources";
		e->usage = show_resources_usage;
		return NULL;
	} else if (cmd == CLI_GENERATE)
		return NULL;
	required_args = e->args;
	provided_args = a->argc;
	if (required_args < provided_args) {
		required_channel_name = a->argv[required_args];
	}
#else
	required_args = 3;
	provided_args = argc;
	if (required_args < provided_args) {
		required_channel_name = argv[required_args];
	}
#endif

	ast_cli(fd, CC_MESSAGE_BIGNAME " resources in use:\n");
	ast_cli(fd, "%-40s %-6s %-4s %-10s %-9s %-5s %-5s %-6s %-6s\n",
							"Line-Name", "Domain", "DTMF", "EchoCancel", "NoiseSupp", "RxAGC", "TxAGC", "RxGain", "TxGain");
	ast_cli(fd, "------------------------------------------------------------------------------------------------------\n");

	for (ifc_type = 0; ifc_type < sizeof(data)/sizeof(data[0]); ifc_type++) {
		data[ifc_type].lock_proc();

		for (i = data[ifc_type].head; i; i = i->next) {
			char* name;

			if ((i->used == 0) || ((i->channeltype != CAPI_CHANNELTYPE_B) &&
				(i->channeltype != CAPI_CHANNELTYPE_NULL)))
				continue;
			if (i->data_plci != 0)
				continue;

			name = ast_strdup(i->vname);
			if ((i->channeltype == CAPI_CHANNELTYPE_NULL) && (name != NULL)) {
				char* p = strstr(name, "-DATAPLCI");
				if (p != NULL)
					*p = 0;
			}

			if ((required_channel_name != NULL) &&
					(strcmp(required_channel_name, (name == 0) ? i->vname : name) != 0)) {
				ast_free(name);
				continue;
			}

			ast_cli(fd, "%-40s %-6s %-4s %-10s %-9s %-5s %-5s %-.1f%-3s %-.1f%-3s\n",
							(name == 0) ? i->vname : name,
							(i->channeltype == CAPI_CHANNELTYPE_B) ? "TDM" : "IP",
							(i->isdnstate & CAPI_ISDN_STATE_DTMF) ? "Y" : "N",
							(i->isdnstate & CAPI_ISDN_STATE_EC)   ? "Y" : "N",
							(i->divaAudioFlags & 0x0080) ? "Y" : "N", /* Noise supression */
							(i->divaAudioFlags & 0x0008) ? "Y" : "N", /* Rx AGC */
							(i->divaAudioFlags & 0x0004) ? "Y" : "N", /* Tx AGC */
							i->divaDigitalRxGainDB, "dB",
							i->divaDigitalTxGainDB, "dB");
			ast_free (name);
		}

		data[ifc_type].unlock_proc();
	}

#ifdef CC_AST_HAS_VERSION_1_6
	return CLI_SUCCESS;
#else
	return RESULT_SUCCESS;
#endif
}

/*
 * do command capi info
 */
#ifdef CC_AST_HAS_VERSION_1_6
static char *pbxcli_capi_info(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
#else
static int pbxcli_capi_info(int fd, int argc, char *argv[])
#endif
{
	int i = 0, capi_num_controllers = pbx_capi_get_num_controllers();
#ifdef CC_AST_HAS_VERSION_1_6
	int fd = a->fd;

	if (cmd == CLI_INIT) {
		e->command = CC_MESSAGE_NAME " info";
		e->usage = info_usage;
		return NULL;
	} else if (cmd == CLI_GENERATE)
		return NULL;
	if (a->argc != e->args)
		return CLI_SHOWUSAGE;
#else
	
	if (argc != 2)
		return RESULT_SHOWUSAGE;
#endif
	
	ast_cli(fd, "%s www.chan-capi.org\n", pbx_capi_get_module_description());
		
	for (i = 1; i <= capi_num_controllers; i++) {
		const struct cc_capi_controller *capiController = pbx_capi_get_controller(i);
		if (capiController != NULL) {
			ast_cli(fd, "Contr%d: %d B channels total, %d B channels free.%s\n",
				i, capiController->nbchannels,
				capiController->nfreebchannels,
				(capiController->used) ? "":" (unused)");
		}
	}
#ifdef CC_AST_HAS_VERSION_1_6
	return CLI_SUCCESS;
#else
	return RESULT_SUCCESS;
#endif
}

/*
 * enable debugging
 */
#ifdef CC_AST_HAS_VERSION_1_6
static char *pbxcli_capi_do_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
#else
static int pbxcli_capi_do_debug(int fd, int argc, char *argv[])
#endif
{
#ifdef CC_AST_HAS_VERSION_1_6
	int fd = a->fd;

	if (cmd == CLI_INIT) {
		e->command = CC_MESSAGE_NAME " debug";
		e->usage = debug_usage;
		return NULL;
	} else if (cmd == CLI_GENERATE)
		return NULL;
	if (a->argc != e->args)
		return CLI_SHOWUSAGE;
#else
	if (argc != 2)
		return RESULT_SHOWUSAGE;
#endif
		
	capidebug = 1;
	ast_cli(fd, CC_MESSAGE_BIGNAME " Message Debugging Enabled\n");
	
#ifdef CC_AST_HAS_VERSION_1_6
	return CLI_SUCCESS;
#else
	return RESULT_SUCCESS;
#endif
}

/*
 * disable debugging
 */
#ifdef CC_AST_HAS_VERSION_1_6
static char *pbxcli_capi_no_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
#else
static int pbxcli_capi_no_debug(int fd, int argc, char *argv[])
#endif
{
#ifdef CC_AST_HAS_VERSION_1_6
	int fd = a->fd;

	if (cmd == CLI_INIT) {
		e->command = CC_MESSAGE_NAME " no debug";
		e->usage = no_debug_usage;
		return NULL;
	} else if (cmd == CLI_GENERATE)
		return NULL;
	if (a->argc != e->args)
		return CLI_SHOWUSAGE;
#else
	if (argc != 3)
		return RESULT_SHOWUSAGE;
#endif

	capidebug = 0;
	ast_cli(fd, CC_MESSAGE_BIGNAME " Message Debugging Disabled\n");
	
#ifdef CC_AST_HAS_VERSION_1_6
	return CLI_SUCCESS;
#else
	return RESULT_SUCCESS;
#endif
}

/*
 * enable QSIG debugging
 */
#ifdef CC_AST_HAS_VERSION_1_6
static char *pbxcli_capi_qsig_do_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
#else
static int pbxcli_capi_qsig_do_debug(int fd, int argc, char *argv[])
#endif
{
#ifdef CC_AST_HAS_VERSION_1_6
	int fd = a->fd;

	if (cmd == CLI_INIT) {
		e->command = CC_MESSAGE_NAME " qsig debug";
		e->usage = qsig_debug_usage;
		return NULL;
	} else if (cmd == CLI_GENERATE)
		return NULL;
	if (a->argc != e->args)
		return CLI_SHOWUSAGE;
#else
	if (argc != 3)
		return RESULT_SHOWUSAGE;
#endif
		
	capiqsigdebug = 1;
	ast_cli(fd, "QSIG Debugging Enabled\n");
	
#ifdef CC_AST_HAS_VERSION_1_6
	return CLI_SUCCESS;
#else
	return RESULT_SUCCESS;
#endif
}

/*
 * disable QSIG debugging
 */
#ifdef CC_AST_HAS_VERSION_1_6
static char *pbxcli_capi_qsig_no_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
#else
static int pbxcli_capi_qsig_no_debug(int fd, int argc, char *argv[])
#endif
{
#ifdef CC_AST_HAS_VERSION_1_6
	int fd = a->fd;

	if (cmd == CLI_INIT) {
		e->command = CC_MESSAGE_NAME " qsig no debug";
		e->usage = qsig_no_debug_usage;
		return NULL;
	} else if (cmd == CLI_GENERATE)
		return NULL;
	if (a->argc != e->args)
		return CLI_SHOWUSAGE;
#else
	if (argc != 4)
		return RESULT_SHOWUSAGE;
#endif

	capiqsigdebug = 0;
	ast_cli(fd, "QSIG Debugging Disabled\n");
	
#ifdef CC_AST_HAS_VERSION_1_6
	return CLI_SUCCESS;
#else
	return RESULT_SUCCESS;
#endif
}

/*
 * exec capi command
 */
#ifdef CC_AST_HAS_VERSION_1_6
static char *pbxcli_capi_exec_capicommand(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
#else
static int pbxcli_capi_exec_capicommand(int fd, int argc, char *argv[])
#endif
{
	int ifc_type, found, retry_search, search_loops = 10;
	struct capi_pvt *i;
	int required_args = 4;
	int provided_args;
	const char* requiredChannelName = NULL;
#ifdef CC_AST_HAS_VERSION_1_6
	const char * const *cli_argv;
#else
	char * const *cli_argv;
#endif
	int ret = -1;
	struct {
		struct capi_pvt *head;
		void (*lock_proc)(void);
		void (*unlock_proc)(void);
	} data[2];

	data[0].head        = capi_iflist;
	data[0].lock_proc   = pbx_capi_lock_interfaces;
	data[0].unlock_proc = pbx_capi_unlock_interfaces;
	data[1].head        = (struct capi_pvt*)pbx_capi_get_nulliflist();
	data[1].lock_proc   = pbx_capi_nulliflist_lock;
	data[1].unlock_proc = pbx_capi_nulliflist_unlock;

#ifdef CC_AST_HAS_VERSION_1_6
	if (cmd == CLI_INIT) {
		e->command = CC_MESSAGE_NAME " exec";
		e->usage = show_exec_usage;
		return NULL;
	} else if (cmd == CLI_GENERATE) {
		return NULL;
	}
	provided_args = a->argc;
	cli_argv = a->argv;
	if (provided_args < required_args) {
		return CLI_SHOWUSAGE;
	}
#else
	provided_args = argc;
	cli_argv = argv;
	if (provided_args < required_args) {
		return RESULT_SHOWUSAGE;
	}
#endif
	requiredChannelName = cli_argv[2];

	do {
		retry_search = 0;

		for (ifc_type = 0, found = 0; (found == 0) && (ifc_type < sizeof(data)/sizeof(data[0])); ifc_type++) {
			data[ifc_type].lock_proc();
			for (i = data[ifc_type].head; (i != 0); i = i->next) {
				char* name;

				if ((i->used == 0) || ((i->channeltype != CAPI_CHANNELTYPE_B) &&
						(i->channeltype != CAPI_CHANNELTYPE_NULL)))
					continue;
				if (i->data_plci != 0)
					continue;

				name = ast_strdup(i->vname);
				if ((i->channeltype == CAPI_CHANNELTYPE_NULL) && (name != NULL)) {
					char* p = strstr(name, "-DATAPLCI");
					if (p != NULL)
						*p = 0;
				}

				if (strcmp(requiredChannelName, (name == 0) ? i->vname : name) == 0) {
					found = 1;
					ast_free(name);
					retry_search = pbx_capi_cli_get_locks(i);
					break;
				}

				ast_free(name);
			}
			data[ifc_type].unlock_proc();
		}

		if (retry_search != 0) {
			usleep (100);
			i = 0;
		}
	} while ((retry_search != 0) && (search_loops-- > 0));

	if (i != NULL) {
		if (i->channeltype != CAPI_CHANNELTYPE_NULL) {
			struct ast_channel* c = i->owner;
			ret = pbx_capi_cli_exec_capicommand(c, cli_argv[3]);
			cc_mutex_unlock(&i->lock);
			if (c)
				ast_channel_unlock (c);
		} else {
			ret = pbx_capi_cli_exec_capicommand(i->used, cli_argv[3]);
			cc_mutex_unlock(&i->lock);
		}
	}

#ifdef CC_AST_HAS_VERSION_1_6
	return ((ret == 0) ? CLI_SUCCESS : CLI_FAILURE);
#else
	return ((ret == 0) ? RESULT_SUCCESS : RESULT_FAILURE);
#endif
}

/*
 * define commands
 */
#ifdef CC_AST_HAS_VERSION_1_6
static struct ast_cli_entry cc_cli_cmd[] = {
	AST_CLI_DEFINE(pbxcli_capi_info, CC_CLI_TEXT_INFO),
	AST_CLI_DEFINE(pbxcli_capi_show_channels, CC_CLI_TEXT_SHOW_CHANNELS),
	AST_CLI_DEFINE(pbxcli_capi_do_debug, CC_CLI_TEXT_DEBUG),
	AST_CLI_DEFINE(pbxcli_capi_no_debug, CC_CLI_TEXT_NO_DEBUG),
	AST_CLI_DEFINE(pbxcli_capi_qsig_do_debug, CC_CLI_TEXT_QSIG_DEBUG),
	AST_CLI_DEFINE(pbxcli_capi_qsig_no_debug, CC_CLI_TEXT_QSIG_NO_DEBUG),
	AST_CLI_DEFINE(pbxcli_capi_chatinfo, CC_CLI_TEXT_CHATINFO),
#ifdef DIVA_STATUS
	AST_CLI_DEFINE(pbxcli_capi_ifc_status, CC_CLI_TEXT_IFC_STATUSINFO),
#endif
	AST_CLI_DEFINE(pbxcli_capi_show_resources, CC_CLI_TEXT_SHOW_RESOURCES),
	AST_CLI_DEFINE(pbxcli_capi_exec_capicommand, CC_CLI_TEXT_EXEC_CAPICOMMAND),
};
#else
static struct ast_cli_entry  cli_info =
	{ { CC_MESSAGE_NAME, "info", NULL }, pbxcli_capi_info, CC_CLI_TEXT_INFO, info_usage };
static struct ast_cli_entry  cli_show_channels =
	{ { CC_MESSAGE_NAME, "show", "channels", NULL }, pbxcli_capi_show_channels, CC_CLI_TEXT_SHOW_CHANNELS, show_channels_usage };
static struct ast_cli_entry  cli_debug =
	{ { CC_MESSAGE_NAME, "debug", NULL }, pbxcli_capi_do_debug, CC_CLI_TEXT_DEBUG, debug_usage };
static struct ast_cli_entry  cli_no_debug =
	{ { CC_MESSAGE_NAME, "no", "debug", NULL }, pbxcli_capi_no_debug, CC_CLI_TEXT_NO_DEBUG, no_debug_usage };
static struct ast_cli_entry  cli_qsig_debug =
	{ { CC_MESSAGE_NAME, "qsig", "debug", NULL }, pbxcli_capi_qsig_do_debug, CC_CLI_TEXT_QSIG_DEBUG, qsig_debug_usage };
static struct ast_cli_entry  cli_qsig_no_debug =
	{ { CC_MESSAGE_NAME, "qsig", "no", "debug", NULL }, pbxcli_capi_qsig_no_debug, CC_CLI_TEXT_QSIG_NO_DEBUG, qsig_no_debug_usage };
static struct ast_cli_entry  cli_chatinfo =
	{ { CC_MESSAGE_NAME, "chatinfo", NULL }, pbxcli_capi_chatinfo, CC_CLI_TEXT_CHATINFO, chatinfo_usage };
#ifdef DIVA_STATUS
static struct ast_cli_entry  cli_ifcstate =
	{ { CC_MESSAGE_NAME, "ifcstate", NULL }, pbxcli_capi_ifc_status, CC_CLI_TEXT_IFC_STATUSINFO, diva_status_ifc_state_usage };
#endif
static struct ast_cli_entry  cli_show_resources =
	{ { CC_MESSAGE_NAME, "show", "resources", NULL }, pbxcli_capi_show_resources, CC_CLI_TEXT_SHOW_RESOURCES, show_resources_usage };
static struct ast_cli_entry  cli_exec_capicommand =
	{ { CC_MESSAGE_NAME, "exec", NULL }, pbxcli_capi_exec_capicommand, CC_CLI_TEXT_EXEC_CAPICOMMAND, show_exec_usage };
#endif



void pbx_capi_cli_register(void)
{
#ifdef CC_AST_HAS_VERSION_1_6
	ast_cli_register_multiple(cc_cli_cmd, sizeof(cc_cli_cmd)/ sizeof(struct ast_cli_entry));
#else
	ast_cli_register(&cli_info);
	ast_cli_register(&cli_show_channels);
	ast_cli_register(&cli_debug);
	ast_cli_register(&cli_no_debug);
	ast_cli_register(&cli_qsig_debug);
	ast_cli_register(&cli_qsig_no_debug);
	ast_cli_register(&cli_chatinfo);
#ifdef DIVA_STATUS
	ast_cli_register(&cli_ifcstate);
#endif
	ast_cli_register(&cli_show_resources);
	ast_cli_register(&cli_exec_capicommand);
#endif
}

void pbx_capi_cli_unregister(void)
{
#ifdef CC_AST_HAS_VERSION_1_6
	ast_cli_unregister_multiple(cc_cli_cmd, sizeof(cc_cli_cmd)/ sizeof(struct ast_cli_entry));
#else
	ast_cli_unregister(&cli_info);
	ast_cli_unregister(&cli_show_channels);
	ast_cli_unregister(&cli_debug);
	ast_cli_unregister(&cli_no_debug);
	ast_cli_unregister(&cli_qsig_debug);
	ast_cli_unregister(&cli_qsig_no_debug);
	ast_cli_unregister(&cli_chatinfo);
#ifdef DIVA_STATUS
	ast_cli_unregister(&cli_ifcstate);
#endif
	ast_cli_unregister(&cli_show_resources);
	ast_cli_unregister(&cli_exec_capicommand);
#endif
}

static int pbx_capi_cli_get_locks(struct capi_pvt *i)
{
	if (i->channeltype != CAPI_CHANNELTYPE_NULL) {
		struct ast_channel* c = i->owner;
		if (c != 0) {
			if (ast_channel_trylock(c) == 0) {
				if (ast_mutex_trylock(&i->lock) == 0) {
					if (i->owner == c) {
						return (0);
					} else {
						ast_mutex_unlock(&i->lock);
						ast_channel_unlock (c);
					}
				} else {
					ast_channel_unlock (c);
				}
			}
		}
	} else {
		if (ast_mutex_trylock(&i->lock) == 0) {
			return (0);
		}
	}

	return (-1);
}

