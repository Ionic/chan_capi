/*
 *
  Copyright (c) Dialogic (R) 2009 - 2010
 *
  This source file is supplied for the use with
  Eicon Networks range of DIVA Server Adapters.
 *
  Dialogic (R) File Revision :    1.9
 *
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2, or (at your option)
  any later version.
 *
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY OF ANY KIND WHATSOEVER INCLUDING ANY
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU General Public License for more details.
 *
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 * Based on apps/app_meetme.c
 *
 */
#ifdef CC_AST_HAS_VERSION_1_6
#include "chan_capi_platform.h"
#include "chan_capi20.h"
#include "chan_capi.h"
#include "chan_capi_qsig.h"
#include "chan_capi_utils.h"
#include "chan_capi_chat.h"
#include "asterisk/manager.h"


#define CC_AMI_ACTION_NAME_CHATLIST "CapichatList"

/*
	LOCALS
	*/
static int pbx_capi_ami_capichat_list(struct mansession *s, const struct message *m);
static int capiChatListRegistered;

static char mandescr_capichatlist[] =
"Description: Lists all users in a particular CapiChat conference.\n"
"CapichatList will follow as separate events, followed by a final event called\n"
"CapichatListComplete.\n"
"Variables:\n"
"    *ActionId: <id>\n"
"    *Conference: <confname>\n";


void pbx_capi_ami_register(void)
{
	capiChatListRegistered = ast_manager_register2(CC_AMI_ACTION_NAME_CHATLIST,
																								EVENT_FLAG_REPORTING,
																								pbx_capi_ami_capichat_list,
																								"List participants in a conference",
																								mandescr_capichatlist) == 0;
}

void pbx_capi_ami_unregister(void)
{
	if (capiChatListRegistered != 0)
		ast_manager_unregister(CC_AMI_ACTION_NAME_CHATLIST);
}

static int pbx_capi_ami_capichat_list(struct mansession *s, const struct message *m) {
	const char *actionid = astman_get_header(m, "ActionID");
	const char *conference = astman_get_header(m, "Conference");
	char idText[80] = "";
	int total = 0;
	const struct capichat_s *capiChatRoom;

	if (!ast_strlen_zero(actionid))
		snprintf(idText, sizeof(idText), "ActionID: %s\r\n", actionid);

	if (pbx_capi_chat_get_room_c(NULL) == NULL) {
		astman_send_error(s, m, "No active conferences.");
		return 0;
	}

	astman_send_listack(s, m, CC_AMI_ACTION_NAME_CHATLIST" user list will follow", "start");

	/* Find the right conference */
	pbx_capi_lock_chat_rooms();

	for (capiChatRoom = pbx_capi_chat_get_room_c(NULL), total = 0;
			 capiChatRoom != NULL;
			 capiChatRoom = pbx_capi_chat_get_room_c(capiChatRoom)) {
		const char*  roomName = pbx_capi_chat_get_room_name(capiChatRoom);
		/* If we ask for one particular, and this isn't it, skip it */
		if (!ast_strlen_zero(conference) && strcmp(roomName, conference))
			continue;

		{
			unsigned int roomNumber      = pbx_capi_chat_get_room_number(capiChatRoom);
			struct ast_channel *c        = pbx_capi_chat_get_room_channel(capiChatRoom);
			int isMemberOperator         = pbx_capi_chat_is_member_operator(capiChatRoom);
			int isCapiChatRoomMuted      = pbx_capi_chat_is_room_muted(capiChatRoom);
			int isCapiChatMemberMuted    = pbx_capi_chat_is_member_muted(capiChatRoom);
			int isCapiChatMemberListener = pbx_capi_chat_is_member_listener(capiChatRoom);
			int isCapiChatMostRecentMember = pbx_capi_chat_is_most_recent_user(capiChatRoom);
			const char* mutedVisualName = "No";
			char* cidVisual;
			char* callerNameVisual;

			ast_channel_lock(c);
			cidVisual        = ast_strdup(pbx_capi_get_cid (c, "<unknown>"));
			callerNameVisual = ast_strdup(pbx_capi_get_callername (c, "<no name>"));
			ast_channel_unlock(c);

			if (isCapiChatMemberListener || isCapiChatRoomMuted || isCapiChatMemberMuted) {
				if (isMemberOperator) {
					if (isCapiChatMemberMuted)
						mutedVisualName = "By self";
				} else if (isCapiChatMemberListener || isCapiChatRoomMuted) {
					mutedVisualName = "By admin";
				} else {
					mutedVisualName = "By self";
				}
			}

			total++;
			astman_append(s,
				"Event: "CC_AMI_ACTION_NAME_CHATLIST"\r\n"
				"%s"
				"Conference: %s/%u\r\n"
				"UserNumber: %d\r\n"
				"CallerIDNum: %s\r\n"
				"CallerIDName: %s\r\n"
				"Channel: %s\r\n"
				"Admin: %s\r\n"
				"Role: %s\r\n"
				"MarkedUser: %s\r\n"
				"Muted: %s\r\n"
				"Talking: %s\r\n"
				"\r\n",
				idText,
				roomName,
				roomNumber,
				total,
				(cidVisual != 0) ? cidVisual : "?",
				(callerNameVisual != 0) ? callerNameVisual : "?",
				c->name,
				(isMemberOperator != 0) ? "Yes" : "No",
				(isCapiChatMemberListener != 0) ? "Listen only" : "Talk and listen" /* "Talk only" */,
				(isCapiChatMostRecentMember != 0) ? "Yes" : "No",
				mutedVisualName,
				/* "Yes" "No" */ "Not monitored");

				ast_free (cidVisual);
				ast_free (callerNameVisual);
		}
	}
	pbx_capi_unlock_chat_rooms();
	/* Send final confirmation */
	astman_append(s,
	"Event: "CC_AMI_ACTION_NAME_CHATLIST"Complete\r\n"
	"EventList: Complete\r\n"
	"ListItems: %d\r\n"
	"%s"
	"\r\n", total, idText);
	return 0;
}
#else
void pbx_capi_ami_register(void)
{
}
void pbx_capi_ami_unregister(void)
{
}
#endif
