/*
 * An implementation of Common ISDN API 2.0 for Asterisk
 *
 * Copyright (C) 2005-2009 Cytronics & Melware
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

#include "chan_capi_platform.h"
#include "chan_capi20.h"
#include "chan_capi.h"
#include "chan_capi_chat.h"
#include "chan_capi_utils.h"

#define CHAT_FLAG_MOH      0x0001

typedef enum {
	RoomMemberDefault  = 0, /* Rx/Tx by default, muted by operator */
	RoomMemberListener = 1, /* Rx only, always muted */
	RoomMemberOperator = 2  /* Rx/Tx, newer muted */
} room_member_type_t;

typedef enum {
	RoomModeDefault = 0,
	RoomModeMuted   = 1
} room_mode_t;

#define PLCI_PER_LX_REQUEST 8

struct capichat_s {
	char name[16];
	unsigned int number;
	int active;
	room_member_type_t room_member_type;
	room_mode_t        room_mode;
	struct capi_pvt *i;
	struct capichat_s *next;
};

struct _deffered_chat_capi_message;
typedef struct _deffered_chat_capi_message {
	int busy;
	_cdword datapath;
	capi_prestruct_t p_struct;
	unsigned char p_list[254];
} deffered_chat_capi_message_t;

static struct capichat_s *chat_list = NULL;
AST_MUTEX_DEFINE_STATIC(chat_lock);

/*
 * LOCALS
 */
static const char* room_member_type_2_name(room_member_type_t room_member_type);

/*
 * partial update the capi mixer for the given char room
 */
static struct capichat_s* update_capi_mixer_part(
	struct capichat_s *chat_start,
	int overall_found,
	deffered_chat_capi_message_t* capi_msg,
	int remove,
	unsigned int roomnumber,
	struct capi_pvt *i)
{
	struct capi_pvt *ii, *ii_last = NULL;
	struct capichat_s *room;
	unsigned char* p_list = &capi_msg->p_list[0];
	_cdword dest;
	_cdword datapath;
	capi_prestruct_t* p_struct = &capi_msg->p_struct;
	unsigned int found = 0;
	_cword j = 0;
	struct capichat_s *new_chat_start = NULL;
	room_member_type_t main_member_type = RoomMemberDefault;
	room_mode_t room_mode = RoomModeDefault;

	room = chat_start;
	while (room != 0) {
		if (room->i == i) {
			main_member_type = room->room_member_type;
			room_mode        = room->room_mode;
			break;
		}
		room = room->next;
	}

	if ((room_mode == RoomModeMuted) && (main_member_type == RoomMemberDefault)) {
		main_member_type = RoomMemberListener;
	}

	room = chat_start;
	while (room) {
		if ((room->number == roomnumber) &&
		    (room->i != i)) {
			if ((found >= PLCI_PER_LX_REQUEST) || ((j + 9) > sizeof(capi_msg->p_list))) {
				/* maybe we need to split capi messages here */
				new_chat_start = room;
				break;
			}
			found++;
			ii = room->i;
			ii_last = ii;
			p_list[j++] = 8;
			p_list[j++] = (_cbyte)(ii->PLCI);
			p_list[j++] = (_cbyte)(ii->PLCI >> 8);
			p_list[j++] = (_cbyte)(ii->PLCI >> 16);
			p_list[j++] = (_cbyte)(ii->PLCI >> 24);
			dest = (remove) ? 0x00000000 : 0x00000003;
			if (ii->channeltype == CAPI_CHANNELTYPE_NULL && ii->line_plci == 0) {
				dest |= 0x00000030;
			}
			if (remove == 0) {
				room_member_type_t room_member_type = room->room_member_type;

				if ((room_mode == RoomModeMuted) && (room_member_type == RoomMemberDefault)) {
					room_member_type = RoomMemberListener;
				}

				if ((main_member_type == RoomMemberListener) && (room_member_type == RoomMemberListener)) {
					dest &= ~3U; /* Disable data transmission between two listener */
				} else if ((main_member_type == RoomMemberListener) && (room_member_type != RoomMemberListener)) {
					dest &= ~1U; /* Disable data transmission from main PLCI to member PLCI */
				} else if ((main_member_type != RoomMemberListener) && (room_member_type == RoomMemberListener)) {
					dest &= ~2U; /* Disable data transmission from member PLCI to main PLCI */
				}
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

	if (found != 0) {
		p_struct->wLen = j;
		p_struct->info = p_list;

		/* don't send DATA_B3 to me */
		datapath = 0x00000000;
		if (remove) {
			/* now we need DATA_B3 again */
			if (i->line_plci == 0) {
				if (i->channeltype != CAPI_CHANNELTYPE_NULL) {
					datapath = 0x0000000c;
				} else {
					datapath = 0x00000030;
				}
			}

			if (overall_found == 1) {
				/* only one left, enable DATA_B3 too */
				if (ii_last->line_plci == 0) {
	        if (ii_last->channeltype != CAPI_CHANNELTYPE_NULL) {
						p_list[5] |= 0x0c;
					} else {
						p_list[5] |= 0x30;
					}
				}
			}
		}
		if ((i->channeltype == CAPI_CHANNELTYPE_NULL) && (i->line_plci == 0)) {
			if (!remove) {
				datapath |= 0x00000030;
			}
		}

		capi_msg->busy = 1;
		capi_msg->datapath = datapath;
	}

	return (new_chat_start);
}

static void update_capi_mixer(int remove, unsigned int roomnumber, struct capi_pvt *i, deffered_chat_capi_message_t* update_segment)
{
	struct capichat_s *room;
	unsigned int overall_found;
	unsigned int nr_segments;

	if (i->PLCI == 0) {
		cc_verbose(2, 0, VERBOSE_PREFIX_3 CC_MESSAGE_NAME
			" mixer: %s: PLCI is unset, abort.\n", i->vname);
		return;
	}

	if (update_segment == 0) {
		cc_mutex_lock(&chat_lock);
	}

	/*
		Get overall amount of parties
	*/
	for (room = chat_list, overall_found = 0; room != 0; room = room->next) {
		overall_found += ((room->number == roomnumber) && (room->i != i));
	}

	room = chat_list;
	while (room != 0) {
		if (room->number == roomnumber) {
			room->active = overall_found + ((remove != 0) ? 0 : 1);
		}
		room = room->next;
	}

	nr_segments = overall_found/PLCI_PER_LX_REQUEST + (overall_found%PLCI_PER_LX_REQUEST != 0);
	if (nr_segments != 0) {
		deffered_chat_capi_message_t __segments[nr_segments];
		deffered_chat_capi_message_t* segments = update_segment == 0 ? __segments : update_segment;
		struct capichat_s *chat_start;
		int segment_nr, nr;

		for (segment_nr = 0, chat_start = chat_list; segment_nr < nr_segments && chat_start != 0; segment_nr++) {
			segments[segment_nr].busy = 0;
			chat_start = update_capi_mixer_part(chat_start, overall_found, &segments[segment_nr], remove, roomnumber, i);
		}

		if (update_segment == 0) {
			cc_mutex_unlock(&chat_lock);
		}

		if (chat_start != 0) {
			cc_log(LOG_ERROR, "%s:%s at %d.\n", __FILE__, __FUNCTION__, __LINE__);
		}

		if (update_segment == 0) {
			for (nr = 0; nr < segment_nr; nr++) {
				if (segments[nr].busy != 0) {
					cc_verbose(3, 1, VERBOSE_PREFIX_3 CC_MESSAGE_NAME
						" mixer: %s PLCI=0x%04x LI=0x%x\n", i->vname, i->PLCI, segments[nr].datapath);

					capi_sendf(NULL, 0, CAPI_FACILITY_REQ, i->PLCI, get_capi_MessageNumber(),
						"w(w(dc))",
						FACILITYSELECTOR_LINE_INTERCONNECT,
						0x0001, /* CONNECT */
						segments[nr].datapath,
						&segments[nr].p_struct);
				}
			}
		}

		return;
	}

	if (update_segment == 0) {
		cc_mutex_unlock(&chat_lock);
	}
}

static void update_all_capi_mixers(unsigned int roomnumber) {
	struct capichat_s *room;
	unsigned int overall_found;
	unsigned int nr_segments;

	for (room = chat_list, overall_found = 0; room != 0; room = room->next) {
		overall_found += (room->number == roomnumber);
	}

	nr_segments = overall_found/PLCI_PER_LX_REQUEST + (overall_found%PLCI_PER_LX_REQUEST != 0);

	{
		deffered_chat_capi_message_t *segments, *segment;
		unsigned int PLCIS[overall_found];
		int i, j, nr;

		segments = malloc (sizeof(*segments)*overall_found*nr_segments);
		if (segments == 0) {
			cc_mutex_unlock(&chat_lock);
			return;
		}

		for (room = chat_list, i = 0; room != 0; room = room->next) {
			if (room->number == roomnumber && room->i && room->i->PLCI != 0) {
			  segment = segments + i*nr_segments;
				for (nr = 0; nr < nr_segments; nr++) {
					segment[nr].busy = 0;
				}
				update_capi_mixer(0, roomnumber, room->i, segment);
				if (segment[0].busy != 0) {
					PLCIS[i++] = room->i->PLCI;
				}
			}
		}

		cc_mutex_unlock(&chat_lock);

		for (j = 0; j < i; j++) {
			segment = segments + j*nr_segments;
			for (nr = 0; nr < nr_segments; nr++) {
				if (segment[nr].busy != 0) {
					cc_verbose(3, 1, VERBOSE_PREFIX_3 CC_MESSAGE_NAME
						" mixer: PLCI=0x%04x LI=0x%x\n", PLCIS[j], segment[nr].datapath);
					capi_sendf(NULL, 0, CAPI_FACILITY_REQ, PLCIS[j], get_capi_MessageNumber(),
						"w(w(dc))",
						FACILITYSELECTOR_LINE_INTERCONNECT,
						0x0001, /* CONNECT */
						segment[nr].datapath,
						&segment[nr].p_struct);
				}
			}
		}

		free (segments);
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

	update_capi_mixer(1, roomnumber, i, 0);
}

/*
 * add a new chat member
 */
static struct capichat_s *add_chat_member(char *roomname, struct capi_pvt *i, room_member_type_t room_member_type)
{
	struct capichat_s *room = NULL;
	struct capichat_s *tmproom;
	unsigned int roomnumber = 1;
	room_mode_t room_mode = RoomModeDefault;

	room = malloc(sizeof(struct capichat_s));
	if (room == NULL) {
		cc_log(LOG_ERROR, "Unable to allocate chan_capi chat struct.\n");
		return NULL;
	}
	memset(room, 0, sizeof(struct capichat_s));
	
	strncpy(room->name, roomname, sizeof(room->name));
	room->name[sizeof(room->name) - 1] = 0;
	room->i = i;
	room->room_member_type = room_member_type;

	cc_mutex_lock(&chat_lock);

	tmproom = chat_list;
	while (tmproom) {
		if (!strcmp(tmproom->name, roomname)) {
			roomnumber = tmproom->number;
			room_mode  = tmproom->room_mode;
			break;
		} else {
			if (tmproom->number == roomnumber) {
				roomnumber++;
			}
		}
		tmproom = tmproom->next;
	}

	room->number = roomnumber;
	room->room_mode = room_mode;
	
	room->next = chat_list;
	chat_list = room;

	cc_mutex_unlock(&chat_lock);

	cc_verbose(3, 0, VERBOSE_PREFIX_3 "%s: added new chat member to room '%s' %s(%d)\n",
		i->vname, roomname, room_member_type_2_name(room_member_type), roomnumber);

	update_capi_mixer(0, roomnumber, i, 0);

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
	room_member_type_t room_member_type = RoomMemberDefault;

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
		case 'l':
			room_member_type = RoomMemberListener;
			break;
		case 'o':
			room_member_type = RoomMemberOperator;
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
		i = pbx_check_resource_plci(c);

		if (i == NULL) {
			i = capi_mknullif(c, contr);
		}
		if (i == NULL) {
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

	room = add_chat_member(roomname, i, room_member_type);
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

struct capi_pvt* pbx_check_resource_plci(struct ast_channel *c)
{
	struct capi_pvt *i = NULL; 
	const char* id = pbx_builtin_getvar_helper(c, "RESOURCEPLCI");

	if (id != 0) {
		i = (struct capi_pvt*)strtoul(id, NULL, 0);
		if (i != 0 && capi_verify_resource_plci(i) != 0) {
			cc_log(LOG_ERROR, "resource PLCI lost\n");
			i = 0;
		}
	}

	return i;
}

int pbx_capi_chat_associate_resource_plci(struct ast_channel *c, char *param)
{
	struct capi_pvt *i = NULL; 
	char *controller;
	char *p;
	ast_group_t tmpcntr;
	unsigned long long contr = 0;

	controller = param;

	if (controller) {
		for (p = controller; p && *p; p++) {
			if (*p == '|') *p = ',';
		}
		tmpcntr = ast_get_group(controller);
		contr = (unsigned long long)(tmpcntr >> 1);
	}

	if (c->tech != &capi_tech) {
		i = capi_mkresourceif(c, contr, 0);
		if (i != NULL) {
			char buffer[24];
			snprintf(buffer, sizeof(buffer)-1, "%p", i);
			/**
				Not sure ast_channel pointer does not change across the
				use of resource PLCI. For this reason use variable to provide
				the pointer to resource PLCI to resource PLCI user

				\todo This is still possible that resource PLCI will be lost.
							In case this happens this will be necessary to maintain one
							live time stamp on resource PLCI and automatically remove
							resource LCI if time stamp exceeds certail limit.
				*/
			pbx_builtin_setvar_helper(c, "RESOURCEPLCI", buffer);

			capi_mkresourceif(c, contr, i);
		}
	}

	return 0; /* Always return success in case c->tech == &capi_tech or to fallback to NULL PLCI */
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

static const char* room_member_type_2_name (room_member_type_t room_member_type)
{
	switch (room_member_type) {
	case RoomMemberListener:
		return "in listener mode ";
	case RoomMemberOperator:
		return "in operator mode ";

	default:
		return "";
	}
}

int pbx_capi_chat_mute(struct ast_channel *c, char *param)
{
	struct capichat_s *room;
	unsigned int roomnumber;
	room_mode_t room_mode;
	const char* roommode = strsep(&param, "|");
	const char* roomname  = param;
	struct capi_pvt *i;

	if (roommode == 0 || *roommode == 0) {
		cc_log(LOG_WARNING, CC_MESSAGE_NAME " chat_mute requires room mode.\n");
		return -1;
	}

	if (ast_true(roommode)) {
		room_mode = RoomModeMuted;
	} else if (ast_false(roommode)) {
		room_mode = RoomModeDefault;
	} else {
		cc_log(LOG_WARNING, CC_MESSAGE_NAME " false parameter for chat_mute.\n");
		cc_log(LOG_WARNING, "Parameter for chat_mute invalid.\n");
		return -1;
	}

	if (roomname == 0 && *roomname == 0) {
		roomname = 0;
	}

	i = pbx_check_resource_plci(c);

	cc_mutex_lock(&chat_lock);

	for (room = chat_list; room != 0; room = room->next) {
		if ((roomname != 0 && strcmp(room->name, roomname) == 0) ||
				(i != 0 && room->i == i) ||
				(room->i != 0 && (room->i->used == c || room->i->peer == c))) {
			roomnumber = room->number;
			cc_verbose(3, 0, VERBOSE_PREFIX_3 "%s: change mode to %s (%d)\n",
									room->name, room_mode == RoomModeDefault ? "full duplex" : "half duplex", roomnumber);
			for (room = chat_list; room != 0; room = room->next) {
				if (room->number == roomnumber) {
					room->room_mode = room_mode;
				}
			}
			update_all_capi_mixers(roomnumber);
			return 0;
		}
	}

	cc_mutex_unlock(&chat_lock);

	return -1;
}

