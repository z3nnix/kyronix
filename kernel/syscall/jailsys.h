#pragma once

#include <stdint.h>

#include "proc/jail.h"

int64_t sys_jail_create(const kjail_conf_t *ucfg);
int64_t sys_jail_attach(uint32_t jid);
int64_t sys_jail_get(uint32_t jid, kjail_info_t *uout);
int64_t sys_jail_list(uint32_t *uids, int max);
int64_t sys_jail_remove(uint32_t jid);
int64_t sys_jail_self(void);
int64_t sys_jail_set_auto(int on);
