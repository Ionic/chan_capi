/*
 *
  Copyright (c) Dialogic(R), 2010

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

/*! \file
 * \brief Interface to chan_capi
 */

#ifndef __DIVA_STATUS_IFC_H__
#define __DIVA_STATUS_IFC_H__

typedef enum _diva_status_interface_state {
  DivaStatusInterfaceStateNotAvailable = 0,
  DivaStatusInterfaceStateOK           = 1,
  DivaStatusInterfaceStateERROR        = -1,
} diva_status_interface_state_t;

/*!
	\brief Check if Diva interface is available
	*/
int diva_status_available(void);

typedef void (*diva_status_changed_cb_proc_t)(int controller, diva_status_interface_state_t state);

/*!
	\brief activate event based status notifications
	*/
diva_status_interface_state_t diva_status_init_interface(int controller, diva_status_changed_cb_proc_t fn);
/*!
	\brief deactivate event based status notifications
	*/
void diva_status_cleanup_interface(int controller);
/*!
	\brief retrieve file handle to be used in async
	I/O operations
	*/
int  diva_status_get_waitable_object(void);
/*!
	\brief process status change events
	*/
void diva_status_process_events(void);

/*!
	\brief Retrieve state of interface
				 DivaStatusInterfaceStateNotAvailable - ignore state
				 DivaStatusInterfaceStateOK           - interface state verified and OK
				 DivaStatusInterfaceStateERROR        - interface state verified and
                                                can not be used to create calls
	*/
diva_status_interface_state_t diva_status_get_interface_state(int controller);

const char* diva_status_interface_state_name(diva_status_interface_state_t state);



#endif
