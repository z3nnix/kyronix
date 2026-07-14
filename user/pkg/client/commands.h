#ifndef PKG_COMMANDS_H
#define PKG_COMMANDS_H

void cmd_health(void);
void cmd_set(const char *endpoint);
void cmd_get(const char *name);
void cmd_list(void);
void cmd_remove(const char *name);

#endif
