# Kyronix Kernel — SMP Bug Report

## Critical Bugs

### BUG-1: `g_current_space` is a single global (DATA RACE)
- **File:** `kernel/syscall/syscall.c:65`
- **Impact:** Two CPUs switching to different processes simultaneously overwrite each other's address space pointer. `uptr_ok()` / `uptr_ok_w()` (syscall.h:11-22) validate userspace pointers against this global — CPU 0 could validate against B's page table, leading to arbitrary memory access.
- **Fix:** Move to `cpu_local_t.current_space`, access via `%gs` segment register.

### BUG-2: `g_fds` is a single global (DATA RACE)
- **File:** `kernel/fs/vfs.c:24`
- **Impact:** Two CPUs switching to different processes simultaneously clobber each other's file descriptor tables. A `read()` on CPU 0 could use file descriptors from the process on CPU 1.
- **Fix:** Move to `cpu_local_t.g_fds`, access via per-CPU helpers.

### BUG-3: `g_reap_thread` is a single global (DATA RACE)
- **File:** `kernel/proc/proc.c:106`
- **Impact:** Two CPUs calling `proc_reap_pending()` simultaneously both call `proc_kstack_free()` on the same process — double-free of physical pages. Two calls to `proc_defer_thread_reap()` on different CPUs: one's deferred reap is silently lost, leaking the dead process's kstack forever.
- **Fix:** Move to per-CPU, or protect with a lock.

### BUG-4: Timer ISR scheduler operates WITHOUT `g_sched_lock` (RACE CONDITION)
- **File:** `kernel/arch/x86_64/idt.c:285-299` (PIC IRQ0), `idt.c:309-327` (LAPIC timer)
- **Impact:** Two CPUs' timer ISRs running simultaneously both call `proc_next_ready()`, get the SAME process, both set it to `PROC_RUNNING`, both call `sched_switch()` — catastrophic kernel stack corruption.
- **Fix:** Wrap the scheduler path in the ISR with `spin_lock(&g_sched_lock)`.

### BUG-5: No TLB shootdown on `vmm_unmap()` (USE-AFTER-FREE)
- **File:** `kernel/mm/vmm.c:142-157`
- **Impact:** `invlpg` only flushes the local CPU's TLB. `proc_kstack_free()` frees physical pages while other CPUs may still have cached translations → use-after-free, data corruption.
- **Fix:** Send IPI to all CPUs for TLB shootdown, or use full CR3 reload.

### BUG-6: `proc_send_signal()` is non-atomic (LOST SIGNALS)
- **File:** `kernel/proc/signal.c:26-34`
- **Impact:** `pending_sigs |=` is a non-atomic RMW. Two CPUs sending signals simultaneously lose one. State transition not atomic with signal delivery.
- **Fix:** Use `__atomic_fetch_or` for `pending_sigs` update.

### BUG-7: `proc_create_idle` missing FPU state initialization
- **File:** `kernel/proc/proc.c:206-257`
- **Impact:** `sched_switch` does `fxrstor64` with zeroed FCW → all x87 exceptions unmasked → spurious #MF faults during context switches on ALL idle processes (BSP + all APs).
- **Fix:** Add FCW=0x037F and MXCSR=0x1F80 initialization like `proc_alloc()` does.

---

## High-Severity Bugs

### BUG-8: `proc_next_ready()` always returns same process (NO FAIRNESS)
- **File:** `kernel/proc/proc.c:140-147`
- **Impact:** Linear first-fit scan always returns the lowest-indexed READY process. Under load, higher-indexed processes starve. Combined with TTY wakeup_tick causing pid=3 to wake every tick, creates scheduler starvation loop.
- **Fix:** Track last-scheduled index per-CPU for round-robin.

### BUG-9: LAPIC ICR write not interrupt-safe
- **File:** `kernel/arch/x86_64/lapic.c:80-96`
- **Impact:** Interrupt between ICR_HI and ICR_LO writes corrupts IPI destination — IPI sent to wrong CPU or broadcast.
- **Fix:** Disable interrupts (`cli`) around the ICR write sequence.

### BUG-10: AP signals "ready" before loading GDT/IDT
- **File:** `kernel/proc/smp.c:141-151`
- **Impact:** `g_aps_ready` incremented before `gdt_ap_load()` and `idt_load_ap()`. BSP returns from spin-wait while AP still has Limine's tables — interrupt on AP jumps to wrong handler.
- **Fix:** Move `g_aps_ready++` and `cpu->online = 1` AFTER `gdt_ap_load()` and `idt_load_ap()`.

### BUG-11: `gdt_ap_load` missing FS/GS reset and `ltr`
- **File:** `kernel/arch/x86_64/gdt.c:99-121`
- **Impact:** AP has no TSS loaded (`ltr` missing) — double-fault or NMI → #GP. FS/GS selectors not zeroed (though base is set via MSR).
- **Fix:** Add `xor ax,ax; mov fs,ax; mov gs,ax` and `mov $0x28,ax; ltr ax` to `gdt_ap_load`.

### BUG-12: Shared TSS `rsp0` — all CPUs write to same field
- **File:** `kernel/arch/x86_64/gdt.c:123`
- **Impact:** `gdt_set_kernel_stack()` writes to shared `g_tss.rsp0`. Ring-3 interrupt on CPU 0 loads rsp0 set by CPU 1 → uses wrong kernel stack → corruption.
- **Fix:** Per-CPU TSS, or disable TSS rsp0 switching for SMP (use per-CPU kernel_rsp from GS instead).

### BUG-13: BSP idle process `current` never initialized
- **File:** `kernel/kernel.c:231-236`
- **Impact:** `g_cpu_local[0].current` is NULL until `process_exec()` runs. Timer ISR between `sti()` (kernel.c:263) and `process_exec` dereferences NULL.
- **Fix:** Set `g_cpu_local[0].current = bsp_idle` after creating BSP idle.

---

## Medium-Severity Bugs

### BUG-14: `g_kstack_va_bump` and `proc_alloc` not locked
- **File:** `kernel/proc/proc.c:22,31-45`
- **Impact:** Concurrent `fork()` on two CPUs → both map kernel stacks to the same virtual address.
- **Fix:** Protect with `g_sched_lock` or a dedicated lock.

### BUG-15: `vmm_space_new()` pool not locked
- **File:** `kernel/mm/vmm.c:159-182`
- **Impact:** Concurrent `fork()` → two processes share the same address space.
- **Fix:** Protect with a spinlock.

### BUG-16: `vmm_switch()` copies kernel PML4 entries without barrier
- **File:** `kernel/mm/vmm.c:220-228`
- **Impact:** If kernel page tables are modified concurrently, the copy might miss new mappings.
- **Fix:** Low priority — x86 store ordering makes this mostly benign.

### BUG-17: `sched.S` does not save/restore RFLAGS
- **File:** `kernel/proc/sched.S:17-77`
- **Impact:** DF (Direction Flag) preserved across context switch if set by kernel code. IOPL/IF not saved.
- **Fix:** Pushf/popf around the callee-saved register save/restore.

### BUG-18: Thread exit path has no scheduler lock
- **File:** `kernel/syscall/syscall.c:839-853`
- **Impact:** `proc_idle_until_ready` and `nt->state = PROC_RUNNING` without `g_sched_lock`.
- **Fix:** Acquire `g_sched_lock` in the thread exit scheduling path.

### BUG-19: `sched_yield_blocking` drops/re-acquires BKL
- **File:** `kernel/proc/proc.c:159-203`
- **Impact:** After `sched_switch(next)` returns, re-acquiring BKL may spin while other CPUs modify data.
- **Fix:** Design-level issue — needs BKL protocol redesign.

### BUG-20: AP boot infinite hang on failure
- **File:** `kernel/proc/smp.c:197-200`
- **Impact:** If any AP faults during `ap_entry()`, BSP spins forever.
- **Fix:** Add timeout with fallback to mark failed AP offline.

### BUG-21: Signal `pending_sigs` + `state` not atomically ordered
- **File:** `kernel/proc/signal.c:31-33`
- **Impact:** Scheduler on another CPU could see WAITING state but miss the signal.
- **Fix:** Memory barrier between `pending_sigs` update and state transition.

---

## Low-Severity / Design Issues

### BUG-22: `resp->cpu_count` vs `g_cpu_count` inconsistency
- **File:** `kernel/proc/smp.c:41,65,172`
- **Impact:** `smp_boot_aps` iterates original count, not capped count. Relies on `extra_argument` safety check.
- **Fix:** Use `g_cpu_count` consistently.

### BUG-23: BSP `current` unset before APs released
- **File:** `kernel/kernel.c:231-237`
- **Impact:** Same as BUG-13, window between `smp_boot_aps` return and `process_exec`.
- **Fix:** Same as BUG-13.

### BUG-24: `gdt_set_kernel_stack` writes shared TSS
- **File:** `kernel/arch/x86_64/syscall_setup.c:60`
- **Impact:** All CPUs write to same `g_tss.rsp0` — last writer wins.
- **Fix:** Same as BUG-12.

### BUG-25: Idle entry RSP alignment (16-byte vs 8-mod-16)
- **File:** `kernel/proc/proc.c:245-253`
- **Impact:** Entry function sees RSP `0 mod 16` instead of `8 mod 16` per SysV ABI. Low risk with current `-mno-sse` compilation.
- **Fix:** Adjust ksp calculation to maintain ABI alignment.

### BUG-26: `kernel_backtrace` uses global `in_bt` flag
- **File:** `kernel/arch/x86_64/idt.c:177-178`
- **Impact:** Two CPUs faulting simultaneously: second one skips backtrace. Intentional anti-recursion but loses diagnostics.
- **Fix:** Make per-CPU.

### BUG-27: `ap_sched_loop` leaves idle state as PROC_READY
- **File:** `kernel/proc/smp.c:83-109`
- **Impact:** After `sched_switch(next)` returns, idle is left as READY instead of RUNNING. Safe because `proc_next_ready` skips pid=0, but fragile.
- **Fix:** Set `idle->state = PROC_RUNNING` after returning from switch.

### BUG-28: PIC IRQ0 handler modifies process states without lock
- **File:** `kernel/arch/x86_64/idt.c:266-284`
- **Impact:** BSP's timer ISR modifies process states while APs may be scheduling the same processes.
- **Fix:** Hold `g_sched_lock` around state modifications.

### BUG-29: `proc_ptrace_stop` has no locking on ptrace fields
- **File:** `kernel/proc/proc.c:260-291`
- **Impact:** Tracer on another CPU reads partially-updated ptrace fields.
- **Fix:** Add memory barriers.

### BUG-30: Duplicate of BUG-7
- **File:** `kernel/proc/proc.c:206-257`
- **Impact:** Same as BUG-7.
- **Fix:** Same as BUG-7.
