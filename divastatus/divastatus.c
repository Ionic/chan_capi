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
 * \brief Implements to access Diva management file system
 *
 * \par Access to Diva hardware state
 * divalogd captures information from Diva management and exports
 * it CVS file format.
 *
 */
#include "divastreaming/platform.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "dlist.h"
#include "divastatus_parameters.h"
#include "divastatus_ifc.h"
#include "divastatus.h"
#ifdef CC_USE_INOTIFY
#include <sys/inotify.h>
#endif

typedef const char* pcchar;

static pcchar DIVA_STATUS_PATH       = "/usr/lib/eicon/divas/registry/ifc";
static pcchar DIVA_STATUS_FILE       = "ifcstate";
static pcchar DIVA_CONFIG_FILE       = "info/Config";
static pcchar DIVA_SERIAL_FILE       = "serial";
static pcchar DIVA_READ_ALARM_FILE   = "info/Red Alarm";
static pcchar DIVA_YELLOW_ALARM_FILE = "info/Yellow Alarm";
static pcchar DIVA_BLUE_ALARM_FILE   = "info/Blue Alarm";

/*
	LOCALS
	*/
static int diva_status_active(void);
static int diva_status_get_controller_state(int controller, diva_status_ifc_state_t *state);
static char* diva_status_read_file(unsigned int controller, const char* fileName);

/*!
	\brief Check divalogd is available
	*/
static int diva_status_active(void)
{
	struct stat v;

	return ((stat(DIVA_STATUS_PATH, &v) == 0 && S_ISDIR(v.st_mode) != 0) ? 0 : -1);
}

static char* diva_status_read_file(unsigned int controller, const char* fileName)
{
	int name_len = strlen(DIVA_STATUS_PATH) + strlen(fileName) + 32;
	char name[name_len];
	struct stat v;
	int fd;
	int length;
	char *data, *p;

	snprintf(name, name_len, "%s/adapter%u/%s", DIVA_STATUS_PATH, controller, fileName);
	name[name_len-1] = 0;

	fd = open(name, O_RDONLY);
	if (fd < 0)
		return 0;

	if (fstat(fd, &v) != 0 || v.st_size == 0) {
    close(fd);
    return 0;
  }

	length = MIN(v.st_size, 16U*1024U);

	data = diva_os_malloc(0, length+1);
	if (data == 0) {
		close (fd);
		return 0;
	}

	if (read(fd, data, length) != length) {
		diva_os_free(0, data);
		close(fd);
		return (0);
	}

	data[length] = 0;

	while (((p = strchr(data, '\n')) != 0) || ((p = strchr(data, '\r')))) {
		*p = 0;
	}

	return (data);
}

static int diva_status_get_controller_state(int controller, diva_status_ifc_state_t *state)
{
	char *data, *p;
	int i, pri;
	const char* v;

	if (diva_status_active() != 0)
		return -1;

	if ((data = diva_status_read_file(controller, DIVA_CONFIG_FILE)) == 0)
		return -1;

	for (i = 0, pri = 0, p = data, v = strsep(&p, ",");
			 v != 0 && i < DivaStateIfcConfig_Max;
			 v = strsep(&p, ","), i++) {
		switch ((diva_state_ifc_config_parameters_t)i) {
			case DivaStateIfcConfig_TYPE:
				pri += (strcmp ("PRI", v) == 0);
				break;

			case DivaStateIfcConfig_PRI:
				pri += (strcmp ("'YES'", v) == 0);
				break;

			default:
				break;
		}
	}
	diva_os_free(0, data);

	if ((data = diva_status_read_file(controller, DIVA_STATUS_FILE)) == 0)
		return (-1);

	memset (state, 0x00, sizeof(*state));
	state->ifcType    = (pri == 2) ? DivaStatusIfcPri : DivaStatusIfcNotPri;
	state->hwState    = DivaStatusHwStateUnknown;
	state->ifcL1State = DivaStatusIfcL2DoNotApply;
	state->ifcL2State = DivaStatusIfcL2DoNotApply;

	for (i = 0, p = data, v = strsep (&p, ","); v != 0 && i < (int)DivaStateIfcState_Max; v = strsep (&p, ","), i++) {
		switch ((diva_state_ifcstate_parameters_t)i) {

			case DivaStateIfcState_LAYER1_STATE:
				if (state->ifcType == DivaStatusIfcPri) {
					state->ifcL1State = (strcmp ("'Activated'", v) == 0) ? DivaStatusIfcL1OK : DivaStatusIfcL1Error;
				}
				break;

			case DivaStateIfcState_LAYER2_STATE:
				if (state->ifcType == DivaStatusIfcPri) {
					state->ifcL1State = (strcmp ("'Layer2 UP'", v) == 0) ? DivaStatusIfcL2OK : DivaStatusIfcL2Error;
				}
				break;

			case DivaStateIfcState_D2_X_FRAMES:
				state->ifcTxDStatistics.Frames = (unsigned int)atol(data);
				break;
			case DivaStateIfcState_D2_X_BYTES:
				state->ifcTxDStatistics.Bytes = (unsigned int)atol(data);
				break;
			case DivaStateIfcState_D2_X_ERRORS:
				state->ifcTxDStatistics.Errors = (unsigned int)atol(data);
				break;
			case DivaStateIfcState_D2_R_FRAMES:
				state->ifcRxDStatistics.Frames = (unsigned int)atol(data);
				break;
			case DivaStateIfcState_D2_R_BYTES:
				state->ifcRxDStatistics.Bytes = (unsigned int)atol(data);
				break;
			case DivaStateIfcState_D2_R_ERRORS:
				state->ifcRxDStatistics.Errors = (unsigned int)atol(data);
				break;

			case DivaStateIfcState_MAX_TEMPERATURE:
				state->maxTemperature = (unsigned int)atoi(data);
				break;
			case DivaStateIfcState_TEMPERATURE:
				state->currentTemperature = (unsigned int)atoi(data);
				break;

			case DivaStateIfcState_HARDWARE_STATE:
				if (strcmp("'Active'", data) == 0)
					state->hwState = DivaStateHwStateActive;
				else if (strcmp("'Inactive'", data) == 0)
					state->hwState = DivaStateHwStateInactive;
				break;

			default:
				break;
		}
	}
	diva_os_free (0, data);

	if ((data = diva_status_read_file(controller, DIVA_READ_ALARM_FILE)) != 0) {
		state->ifcAlarms.Red = strcmp("TRUE", data);
		diva_os_free(0, data);
	}
	if ((data = diva_status_read_file(controller, DIVA_YELLOW_ALARM_FILE)) != 0) {
		state->ifcAlarms.Yellow = strcmp("TRUE", data);
		diva_os_free(0, data);
	}
	if ((data = diva_status_read_file(controller, DIVA_BLUE_ALARM_FILE)) != 0) {
		state->ifcAlarms.Blue = strcmp("TRUE", data);
		diva_os_free(0, data);
	}
	if ((data = diva_status_read_file(controller, DIVA_SERIAL_FILE)) != 0) {
		state->serialNumber = (unsigned int)(atol(data)) & 0x00ffffff;
		diva_os_free(0, data);
	}

	return (0);
}

/*
	chan_capi interface
	*/
diva_status_interface_state_t diva_status_get_interface_state(int controller)
{
	diva_status_ifc_state_t state;
	int ret;

	ret = diva_status_get_controller_state(controller, &state);

	if ((ret != 0) ||
			(state.ifcType != DivaStatusIfcPri) ||
			(state.ifcL1State == DivaStatusIfcL2DoNotApply) ||
			(state.hwState    == DivaStatusHwStateUnknown)) {
		return (DivaStatusInterfaceStateNotAvailable);
	}

	if ((state.ifcAlarms.Red    == 0) &&
			(state.ifcAlarms.Yellow == 0) &&
			(state.ifcAlarms.Blue   == 0) &&
			(state.hwState == DivaStateHwStateActive) &&
			(state.ifcL1State == DivaStatusIfcL1OK)) {
		return DivaStatusInterfaceStateOK;
	}

	return DivaStatusInterfaceStateERROR;
}

