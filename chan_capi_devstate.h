#ifndef __CC_DEVICE_STATE_PROVIDER_INTERFACE_H__
#define __CC_DEVICE_STATE_PROVIDER_INTERFACE_H__

void pbx_capi_register_device_state_providers(void);
void pbx_capi_unregister_device_state_providers(void);
void pbx_capi_chat_room_state_event(const char* roomName, int inUse);


#endif
