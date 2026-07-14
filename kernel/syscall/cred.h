#pragma once

#include <stdint.h>

int64_t sys_getpid(void);
int64_t sys_getppid(void);
int64_t sys_getuid(void);
int64_t sys_getgid(void);
int64_t sys_geteuid(void);
int64_t sys_getegid(void);
int64_t sys_setfsuid(uint32_t uid);
int64_t sys_setfsgid(uint32_t gid);
int64_t sys_setuid(uint32_t uid);
int64_t sys_setgid(uint32_t gid);
int64_t sys_setreuid(uint32_t ruid, uint32_t euid);
int64_t sys_setregid(uint32_t rgid, uint32_t egid);
int64_t sys_setresuid(uint32_t ruid, uint32_t euid, uint32_t suid);
int64_t sys_setresgid(uint32_t rgid, uint32_t egid, uint32_t sgid);
int64_t sys_getresuid(uint32_t *ruid, uint32_t *euid, uint32_t *suid);
int64_t sys_getresgid(uint32_t *rgid, uint32_t *egid, uint32_t *sgid);
int64_t sys_getpgrp(void);
int64_t sys_getpgid(uint64_t pid);
int64_t sys_setsid(void);
int64_t sys_setpgid(uint64_t pid, uint64_t pgid);
int64_t sys_gettid(void);
int64_t sys_getgroups(int size, uint32_t *list);
int64_t sys_setgroups(int size, const uint32_t *list);
