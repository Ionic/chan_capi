#ifndef __CC_AMI_INTERFACE_H__
#define __CC_AMI_INTERFACE_H__

void pbx_capi_ami_register(void);
void pbx_capi_ami_unregister(void);
struct capichat_s;
void pbx_capi_chat_join_event(struct ast_channel* c, const struct capichat_s * room);
void pbx_capi_chat_leave_event(struct ast_channel* c,
															 const struct capichat_s *room,
															 long duration);
void pbx_capi_chat_conference_end_event(const char* roomName);


#endif

