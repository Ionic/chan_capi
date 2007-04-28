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
#include "chan_capi_chat.h"
#include "chan_capi_utils.h"

#define CAPI_MAX_MEETME_NAME 32

struct capichat_s {
	char name[CAPI_MAX_MEETME_NAME];
	unsigned int number;
	struct ast_channel *chan;
	struct capi_pvt *i;
	_cdword plci;
	struct capichat_s *next;
};

static struct capichat_s *chat_list = NULL;
AST_MUTEX_DEFINE_STATIC(chat_lock);

/*
 * update the capi mixer for the given char room
 */
static void update_capi_mixer(int remove, unsigned int roomnumber, _cdword plci)
{
	struct capichat_s *room;
	unsigned char p_list[360];
	_cdword dest;
	_cdword datapath;
	capi_prestruct_t p_struct;
	unsigned int found = 0;
	_cword j = 0;

	cc_mutex_lock(&chat_lock);
	room = chat_list;
	while (room) {
		if ((room->number == roomnumber) &&
		    (room->plci != plci)) {
			found++;
			if (j + 9 > sizeof(p_list)) {
				/* maybe we need to split capi messages here */
				break;
			}
			p_list[j++] = 8;
			p_list[j++] = (_cbyte)(room->plci);
			p_list[j++] = (_cbyte)(room->plci >> 8);
			p_list[j++] = (_cbyte)(room->plci >> 16);
			p_list[j++] = (_cbyte)(room->plci >> 24);
			dest = (remove) ? 0x00000000 : 0x00000003;
			p_list[j++] = (_cbyte)(dest);
			p_list[j++] = (_cbyte)(dest >> 8);
			p_list[j++] = (_cbyte)(dest >> 16);
			p_list[j++] = (_cbyte)(dest >> 24);
		}
		room = room->next;
	}
	cc_mutex_unlock(&chat_lock);

	if (found) {
		p_struct.wLen = j;
		p_struct.info = p_list;

		/* don't send DATA_B3 to me */
		datapath = 0x00000000;
		if (remove) {
			/* now we need DATA_B3 again */
			datapath = 0x000000c0;
			if (found == 1) {
				/* only one left, enable DATA_B3 too */
				p_list[5] |= 0x30;
			}
		}

		capi_sendf(NULL, 0, CAPI_FACILITY_REQ, plci, get_capi_MessageNumber(),
			"w(w(dc))",
			FACILITYSELECTOR_LINE_INTERCONNECT,
			0x0001, /* CONNECT */
			datapath,
			&p_struct
		);
	}
}

/*
 * delete a chat member
 */
static void del_chat_member(struct capichat_s *room)
{
	struct capichat_s *tmproom;
	struct capichat_s *tmproom2 = NULL;
	unsigned int roomnumber = room->number;
	_cdword plci = room->plci;

	cc_mutex_lock(&chat_lock);
	tmproom = chat_list;
	while (tmproom) {
		if (tmproom == room) {
			if (!tmproom2) {
				chat_list = tmproom->next;
			} else {
				tmproom2->next = tmproom->next;
			}
			cc_verbose(3, 1, VERBOSE_PREFIX_3 "capi chat: removed member from room %s (%d)\n",
				room->name, room->number);
			free(room);
		}
		tmproom2 = tmproom;
		tmproom = tmproom->next;
	}
	cc_mutex_unlock(&chat_lock);

	update_capi_mixer(1, roomnumber, plci);
}

/*
 * add a new chat member
 */
static struct capichat_s *add_chat_member(char *roomname,
	struct ast_channel *chan, struct capi_pvt *i)
{
	struct capichat_s *room = NULL;
	struct capichat_s *tmproom;
	unsigned int roomnumber = 1;

	room = malloc(sizeof(struct capichat_s));
	if (room == NULL) {
		cc_log(LOG_ERROR, "Unable to allocate capi chat struct.\n");
		return NULL;
	}
	memset(room, 0, sizeof(struct capichat_s));
	
	strncpy(room->name, roomname, sizeof(room->name));
	room->name[sizeof(room->name) - 1] = 0;
	room->chan = chan;
	room->i = i;
	room->plci = i->PLCI;
	
	cc_mutex_lock(&chat_lock);

	tmproom = chat_list;
	while (tmproom) {
		if (!strcmp(tmproom->name, roomname)) {
			roomnumber = tmproom->number;
			break;
		} else {
			if (tmproom->number == roomnumber) {
				roomnumber++;
			}
		}
		tmproom = tmproom->next;
	}

	room->number = roomnumber;
	
	room->next = chat_list;
	chat_list = room;

	cc_mutex_unlock(&chat_lock);

	cc_verbose(3, 1, VERBOSE_PREFIX_3 "capi chat: added new member to room %s (%d)\n",
		roomname, roomnumber);

	update_capi_mixer(0, roomnumber, room->plci);

	return room;
}

/*
 * start the chat
 */
int pbx_capi_chat(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = NULL; 
	char *roomname, *controller, *options;
	struct capichat_s *room;
	struct ast_frame *f;
	int state;
	unsigned int contr = 1;

	roomname = strsep(&param, "|");
	controller = strsep(&param, "|");
	options = param;

	if (!roomname) {
		cc_log(LOG_WARNING, "capi chat requires room name.\n");
		return -1;
	}
	
	cc_verbose(3, 1, VERBOSE_PREFIX_3 "capi chat: %s: roomname=%s "
		"controller=%s options=%s\n",
		c->name, roomname, controller, options);

	if (controller) {
		contr = (unsigned int)strtoul(controller, NULL, 0);
	}

	if (c->tech == &capi_tech) {
		i = CC_CHANNEL_PVT(c); 
	} else {
		/* virtual CAPI channel */
		i = mknullif(contr);
		if (!i) {
			return -1;
		}
	}

	if (c->_state != AST_STATE_UP)
		ast_answer(c);

	capi_wait_for_answered(i);
	if (!(capi_wait_for_b3_up(i))) {
		goto out;
	}

	room = add_chat_member(roomname, c, i);
	if (!room) {
		cc_log(LOG_WARNING, "Unable to open capi chat room.\n");
		return -1;
	}

	while (ast_waitfor(c, 500) >= 0) {
		f = ast_read(c);
		if (f) {
			ast_frfree(f);
		} else {
			/* channel was hung up or something else happened */
			break;
		}
	}

	del_chat_member(room);

out:
	if (i->channeltype == CAPI_CHANNELTYPE_NULL) {
		cc_mutex_lock(&i->lock);
		state = i->state;
		i->state = CAPI_STATE_DISCONNECTING;
		capi_activehangup(i, state);
		cc_mutex_unlock(&i->lock);
	}

	return 0;
}

