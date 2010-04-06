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
 */
#ifndef __DIVA_LINK_H__
#define __DIVA_LINK_H__

struct _diva_entity_link;
typedef struct _diva_entity_link {
	struct _diva_entity_link* prev;
	struct _diva_entity_link* next;
} diva_entity_link_t;

typedef struct _diva_entity_queue {
	diva_entity_link_t* head;
	diva_entity_link_t* tail;
} diva_entity_queue_t;


typedef int (*diva_q_cmp_fn_t)(const void* what,
                               const diva_entity_link_t*);

#if defined(__cplusplus)
extern "C" {
#endif

void diva_q_remove   (diva_entity_queue_t* q, diva_entity_link_t* what);
void diva_q_add_tail (diva_entity_queue_t* q, diva_entity_link_t* what);
void diva_q_insert_after (diva_entity_queue_t* q,
                             diva_entity_link_t* prev,
                             diva_entity_link_t* what);
void diva_q_insert_before (diva_entity_queue_t* q,
                             diva_entity_link_t* next,
                             diva_entity_link_t* what);
diva_entity_link_t* diva_q_find (const diva_entity_queue_t* q,
                                 const void* what, diva_q_cmp_fn_t cmp_fn);

diva_entity_link_t* diva_q_get_head	(const diva_entity_queue_t* q);
diva_entity_link_t* diva_q_get_tail	(const diva_entity_queue_t* q);
diva_entity_link_t* diva_q_get_next	(const diva_entity_link_t* what);
diva_entity_link_t* diva_q_get_prev	(const diva_entity_link_t* what);
int diva_q_get_nr_of_entries (const diva_entity_queue_t* q);
void diva_q_init (diva_entity_queue_t* q);

#if defined(__cplusplus)
}
#endif

#if defined(__cplusplus) /* { */

template<class T> class CDivaListEntry {
  public:
    T* prev;
    T* next;
};

template<class T, typename C> class CDivaListQueue {
	public:
		CDivaListQueue() { head = 0; tail = 0; }
		~CDivaListQueue() { }

	void remove (T* what) {
		if (what->prev == 0) {
			if ((head = what->next) != 0) {
				head->prev = 0;
			} else {
				tail = 0;
			}
		} else if (what->next == 0) {
			tail       = what->prev;
			tail->next = 0;
		} else {
			what->prev->next = what->next;
			what->next->prev = what->prev;
		}
		what->prev = what->next = 0;
	}

	void add_tail (T* what) {
		what->next = 0;
		if (head == 0) {
			what->prev = 0;
			head = tail = what;
		} else {
			what->prev = tail;
			tail->next = what;
			tail = what;
		}
	}

	T* get_head () { return (head); }

	static T* next	(T* what) { return ((what) ? what->next : 0); }

	static T* prev	(T* what) { return ((what) ? what->prev : 0); }

	dword nr_of_entries () const {
		const T* diva_q_current = head;
		dword i = 0;

		while (diva_q_current != 0) {
			i++;
			diva_q_current = diva_q_current->next;
		}

		return (i);
	}

	T* find (const C& what) {
		T* diva_q_current = head;

		while (diva_q_current != 0) {
			if (*diva_q_current == what) {
				break;
			}
			diva_q_current = diva_q_current->next;
		}

		return (diva_q_current);
	}

	private:
		T* head;
		T* tail;
};

#endif /* } */


#endif
