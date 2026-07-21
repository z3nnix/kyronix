#pragma once

#include <stdint.h>

#include "proc/signal.h"
#include "syscall.h"

int64_t sys_kill(int64_t pid, int sig);
int64_t sys_tgkill(int tgid, int tid, int sig);
int64_t sys_rt_sigaction(int sig, const k_sigaction_t *act, k_sigaction_t *oldact,
                         uint64_t sigsetsize);
int64_t sys_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset, uint64_t sigsetsize);
int64_t sys_rt_sigreturn(syscall_frame_t *f);
int64_t sys_pause(void);
int64_t sys_rt_sigsuspend(const uint64_t *mask, uint64_t sigsetsize);
int64_t sys_rt_sigtimedwait(const uint64_t *set, void *info, const void *timeout,
                            uint64_t sigsetsize);
int64_t sys_sigaltstack(const void *ss, void *oss);
