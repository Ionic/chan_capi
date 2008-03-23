/*
 * An implementation of Common ISDN API 2.0 for Asterisk
 *
 * Copyright (C) 2005-2008 Cytronics & Melware
 *
 * Armin Schindler <armin@melware.de>
 * 
 * This program is free software and may be modified and 
 * distributed under the terms of the GNU Public License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/signal.h>

#include "chan_capi20.h"
#include "chan_capi.h"
#include "chan_capi_chat.h"
#include "chan_capi_utils.h"

#define CHAT_FLAG_MOH      0x0001

struct capichat_s {
	char name[16];
	unsigned int number;
	int active;
	struct capi_pvt *i;
	struct capichat_s *next;
};

static struct capichat_s *chat_list = NULL;
AST_MUTEX_DEFINE_STATIC(chat_lock);

/*
 * update the capi mixer for the given char room
 */
static void update_capi_mixer(int remove, unsigned int roomnumber, struct capi_pvt *i)
{
	struct capi_pvt *ii;
	struct capichat_s *room;
	unsigned char p_list[360];
	_cdword dest;
	_cdword datapath;
	capi_prestruct_t p_struct;
	unsigned int found = 0;
	_cword j = 0;

	if (i->PLCI == 0) {
		cc_verbose(2, 0, VERBOSE_PREFIX_3 CC_MESSAGE_NAME
			" mixer: %s: PLCI is unset, abort.\n", i->vname);
		return;
	}

	cc_mutex_lock(&chat_lock);
	room = chat_list;
	while (room) {
		if ((room->number == roomnumber) &&
		    (room->i != i)) {
			found++;
			if (j + 9 > sizeof(p_list)) {
				/* maybe we need to split capi messages here */
				break;
			}
			ii = room->i;
			p_list[j++] = 8;
			p_list[j++] = (_cbyte)(ii->PLCI);
			p_list[j++] = (_cbyte)(ii->PLCI >> 8);
			p_list[j++] = (_cbyte)(ii->PLCI >> 16);
			p_list[j++] = (_cbyte)(ii->PLCI >> 24);
			dest = (remove) ? 0x00000000 : 0x00000003;
			if (ii->channeltype == CAPI_CHANNELTYPE_NULL) {
				dest |= 0x00000030;
			}
			p_list[j++] = (_cbyte)(dest);
			p_list[j++] = (_cbyte)(dest >> 8);
			p_list[j++] = (_cbyte)(dest >> 16);
			p_list[j++] = (_cbyte)(dest >> 24);
			cc_verbose(3, 1, VERBOSE_PREFIX_3 CC_MESSAGE_NAME
				" mixer: listed %s PLCI=0x%04x LI=0x%x\n", ii->vname, ii->PLCI, dest);
		}
		room = room->next;
	}
	room = chat_list;
	while (room) {
		if (room->number == roomnumber) {
			room->active = found + ((remove) ? 0 : 1);
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
			datapath = 0x0000000c;
			if (found == 1) {
				/* only one left, enable DATA_B3 too */
				p_list[5] |= 0x0c;
			}
		}
		if (i->channeltype == CAPI_CHANNELTYPE_NULL) {
			if (!remove) {
				datapath |= 0x00000030;
			}
		}

		cc_verbose(3, 1, VERBOSE_PREFIX_3 CC_MESSAGE_NAME
			" mixer: %s PLCI=0x%04x LI=0x%x\n", i->vname, i->PLCI, datapath);

		capi_sendf(NULL, 0, CAPI_FACILITY_REQ, i->PLCI, get_capi_MessageNumber(),
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
	struct capi_pvt *i = room->i;

	cc_mutex_lock(&chat_lock);
	tmproom = chat_list;
	while (tmproom) {
		if (tmproom == room) {
			if (!tmproom2) {
				chat_list = tmproom->next;
			} else {
				tmproom2->next = tmproom->next;
			}
			cc_verbose(3, 0, VERBOSE_PREFIX_3 "%s: removed chat member from room '%s' (%d)\n",
				room->i->vname, room->name, room->number);
			free(room);
		}
		tmproom2 = tmproom;
		tmproom = tmproom->next;
	}
	cc_mutex_unlock(&chat_lock);

	update_capi_mixer(1, roomnumber, i);
}

/*
 * add a new chat member
 */
static struct capichat_s *add_chat_member(char *roomname, struct capi_pvt *i)
{
	struct capichat_s *room = NULL;
	struct capichat_s *tmproom;
	unsigned int roomnumber = 1;

	room = malloc(sizeof(struct capichat_s));
	if (room == NULL) {
		cc_log(LOG_ERROR, "Unable to allocate chan_capi chat struct.\n");
		return NULL;
	}
	memset(room, 0, sizeof(struct capichat_s));
	
	strncpy(room->name, roomname, sizeof(room->name));
	room->name[sizeof(room->name) - 1] = 0;
	room->i = i;
	
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

	cc_verbose(3, 0, VERBOSE_PREFIX_3 "%s: added new chat member to room '%s' (%d)\n",
		i->vname, roomname, roomnumber);

	update_capi_mixer(0, roomnumber, i);

	return room;
}

/*
 * loop during chat
 */
static void chat_handle_events(struct ast_channel *c, struct capi_pvt *i,
	struct capichat_s *room, unsigned int flags)
{
	struct ast_frame *f;
	int ms;
	int exception;
	int ready_fd;
	int waitfd;
	int nfds = 0;
	struct ast_channel *rchan;
	struct ast_channel *chan = c;
	int moh_active = 0;

	ast_indicate(chan, -1);

	waitfd = i->readerfd;
	if (i->channeltype == CAPI_CHANNELTYPE_NULL) {
		nfds = 1;
		ast_set_read_format(chan, capi_capability);
		ast_set_write_format(chan, capi_capability);
	}

	if ((flags & CHAT_FLAG_MOH) && (room->active < 2)) {
#if defined(CC_AST_HAS_VERSION_1_6) || defined(CC_AST_HAS_VERSION_1_4)
		ast_moh_start(chan, NULL, NULL);
#else
		ast_moh_start(chan, NULL);
#endif
		moh_active = 1;
	}

	while (1) {
		ready_fd = 0;
		ms = 100;
		errno = 0;
		exception = 0;

		rchan = ast_waitfor_nandfds(&chan, 1, &waitfd, nfds, &exception, &ready_fd, &ms);

		if (rchan) {
			f = ast_read(chan);
			if (!f) {
				cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: chat: no frame, hangup.\n",
					i->vname);
				break;
			}
			if ((f->frametype == AST_FRAME_CONTROL) && (f->subclass == AST_CONTROL_HANGUP)) {
				cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: chat: hangup frame.\n",
					i->vname);
				ast_frfree(f);
				break;
			} else if (f->frametype == AST_FRAME_VOICE) {
				cc_verbose(5, 1, VERBOSE_PREFIX_3 "%s: chat: voice frame.\n",
					i->vname);
				if (i->channeltype == CAPI_CHANNELTYPE_NULL) {
					capi_write_frame(i, f);
				}
			} else if (f->frametype == AST_FRAME_NULL) {
				/* ignore NULL frame */
				cc_verbose(5, 1, VERBOSE_PREFIX_3 "%s: chat: NULL frame, ignoring.\n",
					i->vname);
			} else {
				cc_verbose(3, 1, VERBOSE_PREFIX_3 "%s: chat: unhandled frame %d/%d.\n",
					i->vname, f->frametype, f->subclass);
			}
			ast_frfree(f);
		} else if (ready_fd == i->readerfd) {
			if (exception) {
				cc_verbose(1, 0, VERBOSE_PREFIX_3 "%s: chat: exception on readerfd\n",
					i->vname);
				break;
			}
			f = capi_read_pipeframe(i);
			if (f->frametype == AST_FRAME_VOICE) {
				ast_write(chan, f);
			}
			/* ignore other nullplci frames */
		} else {
			if ((ready_fd < 0) && ms) { 
				if (errno == 0 || errno == EINTR)
					continue;
				cc_log(LOG_WARNING, "%s: Wait failed (%s).\n",
					chan->name, strerror(errno));
				break;
			}
		}
		if ((moh_active) && (room->active > 1)) {
			ast_moh_stop(chan);
			moh_active = 0;
		}
	}
}

/*
 * start the chat
 */
int pbx_capi_chat(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = NULL; 
	char *roomname, *controller, *options;
	char *p;
	struct capichat_s *room;
	ast_group_t tmpcntr;
	unsigned long long contr = 0;
	unsigned int flags = 0;

	roomname = strsep(&param, "|");
	options = strsep(&param, "|");
	controller = param;

	if (!roomname) {
		cc_log(LOG_WARNING, CC_MESSAGE_NAME " chat requires room name.\n");
		return -1;
	}
	
	if (controller) {
		for (p = controller; p && *p; p++) {
			if (*p == '|') *p = ',';
		}
		tmpcntr = ast_get_group(controller);
		contr = (unsigned long long)(tmpcntr >> 1);
	}

	while ((options) && (*options)) {
		switch (*options) {
		case 'm':
			flags |= CHAT_FLAG_MOH;
			break;
		default:
			cc_log(LOG_WARNING, "Unknown chat option '%c'.\n",
				*options);
			break;
		}
		options++;
	}

	cc_verbose(3, 1, VERBOSE_PREFIX_3 CC_MESSAGE_NAME " chat: %s: roomname=%s "
		"options=%s controller=%s (0x%llx)\n",
		c->name, roomname, options, controller, contr);

	if (c->tech == &capi_tech) {
		i = CC_CHANNEL_PVT(c); 
	} else {
		/* virtual CAPI channel */
		i = capi_mknullif(c, contr);
		if (!i) {
			return -1;
		}
	}

	if (c->_state != AST_STATE_UP) {
		ast_answer(c);
	}

	capi_wait_for_answered(i);
	if (!(capi_wait_for_b3_up(i))) {
		goto out;
	}

	room = add_chat_member(roomname, i);
	if (!room) {
		cc_log(LOG_WARNING, "Unable to open " CC_MESSAGE_NAME " chat room.\n");
		return -1;
	}

	/* main loop */
	chat_handle_events(c, i, room, flags);

	del_chat_member(room);

out:
	capi_remove_nullif(i);

	return 0;
}

/*
 * do command capi chatinfo
 */
#ifdef CC_AST_HAS_VERSION_1_6
char *pbxcli_capi_chatinfo(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
#else
int pbxcli_capi_chatinfo(int fd, int argc, char *argv[])
#endif
{
	struct capichat_s *room = NULL;
	struct ast_channel *c;
#ifdef CC_AST_HAS_VERSION_1_6
	int fd = a->fd;

	if (cmd == CLI_INIT) {
		e->command = CC_MESSAGE_NAME " chatinfo";
		e->usage = chatinfo_usage;
		return NULL;
	} else if (cmd == CLI_GENERATE)
		return NULL;
	if (a->argc != e->args)
		return CLI_SHOWUSAGE;
#else
	
	if (argc != 2)
		return RESULT_SHOWUSAGE;
#endif

	if (chat_list == NULL) {
		ast_cli(fd, "There are no members in " CC_MESSAGE_NAME " chat.\n");
		return RESULT_SUCCESS;
	}

	ast_cli(fd, CC_MESSAGE_NAME " chat\n");
	ast_cli(fd, "Room# Roomname    Member                        Caller\n");

	cc_mutex_lock(&chat_lock);
	room = chat_list;
	while (room) {
		c = room->i->owner;
		if (!c) {
			c = room->i->used;
		}
		if (!c) {
			ast_cli(fd, "%3d   %-12s%-30s\"%s\" <%s>\n",
				room->number, room->name, room->i->vname,
				"?", "?");
		} else {
			ast_cli(fd, "%3d   %-12s%-30s\"%s\" <%s>\n",
				room->number, room->name, c->name,
				(c->cid.cid_name) ? c->cid.cid_name:"", c->cid.cid_num);
		}
		room = room->next;
	}
	cc_mutex_unlock(&chat_lock);

#ifdef CC_AST_HAS_VERSION_1_6
	return CLI_SUCCESS;
#else
	return RESULT_SUCCESS;
#endif
}

