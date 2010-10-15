#ifndef __CHAN_CAPI_MWI_H__
#define __CHAN_CAPI_MWI_H__

extern int pbx_capi_mwi(struct ast_channel *c, char *info);
extern int pbx_capi_xmit_mwi(
	struct cc_capi_controller *controller,
	unsigned short basicService, 
	unsigned int   numberOfMessages,
	unsigned short messageStatus,
	unsigned short messageReference,
	unsigned short invocationMode,
	const unsigned char* receivingUserNumber,
	const unsigned char* controllingUserNumber,
	const unsigned char* controllingUserProvidedNumber,
	const unsigned char* timeX208);
extern int pbx_capi_xmit_mwi_deactivate(
	struct cc_capi_controller *controller,
	unsigned short basicService,
	unsigned short invocationMode,
	const unsigned char* receivingUserNumber,
	const unsigned char* controllingUserNumber);
extern void pbx_capi_register_mwi(struct cc_capi_controller *controller);
extern void pbx_capi_refresh_mwi(struct cc_capi_controller *controller);
extern void pbx_capi_unregister_mwi(struct cc_capi_controller *controller);
extern void pbx_capi_cleanup_mwi(struct cc_capi_controller *controller);
extern unsigned char* pbx_capi_build_facility_number(
	unsigned char mwifacptynrtype,
	unsigned char mwifacptynrton,
	unsigned char mwifacptynrpres,
	const char* number);

void pbx_capi_init_mwi_server (
	struct cc_capi_controller *mwiController,
	const struct cc_capi_conf *conf);

#endif
