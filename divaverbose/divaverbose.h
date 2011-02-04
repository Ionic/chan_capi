#ifndef __DIVA_VERBOSER_H__
#define __DIVA_VERBOSER_H__

#ifdef DIVA_VERBOSE

void diva_verbose_load(void);
void diva_verbose_unload(void);

#ifdef CC_AST_HAS_VERSION_1_6
char *pbxcli_capi_do_verbose(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a);
char *pbxcli_capi_no_verbose(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a);
#else
int pbxcli_capi_do_verbose(int fd, int argc, char *argv[]);
int pbxcli_capi_no_verbose(int fd, int argc, char *argv[]);
#endif

#define CC_CLI_TEXT_CAPI_DO_VERBOSE "Connect " CC_MESSAGE_BIGNAME " to debug driver"
#define CC_CLI_TEXT_CAPI_NO_VERBOSE "Disconnect " CC_MESSAGE_BIGNAME " from debug driver"


#ifndef CC_AST_HAS_VERSION_1_6
extern char capi_do_verbose_usage[];
extern char capi_no_verbose_usage[];
#endif

#else

#define diva_verbose_load() do{}while(0)
#define diva_verbose_unload() do{}while(0)

#endif

#endif

