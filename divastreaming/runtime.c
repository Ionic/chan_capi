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
#include "platform.h"
#include <malloc.h>

void* diva_os_malloc (unsigned long flags, unsigned long size) {
	void* ret = 0;

	if (size != 0) {
		ret = malloc (size);
	}

	return (ret);
}

void diva_os_free (unsigned long flags, void* ptr) {
	if (ptr != 0) {
		free (ptr);
	}
}

void diva_runtime_error_message (const char* fmt, ...) {

}

void diva_runtime_log_message (const char* fmt, ...) {

}

void diva_runtime_trace_message (const char* fmt, ...) {

}

