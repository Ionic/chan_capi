/*
 * (CAPI*)
 *
 * An implementation of Common ISDN API 2.0 for Asterisk
 *
 * Copyright (C) 2006-2007 Cytronics & Melware
 *
 * Armin Schindler <armin@melware.de>
 * 
 * This program is free software and may be modified and 
 * distributed under the terms of the GNU Public License.
 */
 
#ifndef _PBX_CAPI_CHAT_H
#define _PBX_CAPI_CHAT_H

/*
 * prototypes
 */
extern int pbx_capi_chat(struct ast_channel *c, char *param);
#ifdef CC_AST_HAS_VERSION_1_6
extern char *pbxcli_capi_chatinfo(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a);
#else
extern int pbxcli_capi_chatinfo(int fd, int argc, char *argv[]);
#endif

#endif
