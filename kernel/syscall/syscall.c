#include "syscall.h"
#include "version.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/pit.h"
#include "arch/x86_64/spinlock.h"
#include "arch/x86_64/syscall_setup.h"
#include "crypto/chacha20.h"
#include "epoll.h"
#include "exec/elf.h"
#include "exec/process.h"
#include "file.h"
#include "fs/inet_socket.h"
#include "fs/pipe.h"
#include "fs/ext2.h"
#include "fs/vfs.h"
#include "fs/vfs_internal.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/shm.h"
#include "mm/vma.h"
#include "mm/vmm.h"
#include "poll.h"
#include "proc/jail.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "proc/smp.h"
#include "socket.h"
#include "time.h"

/* linuh errno values */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EIO 5
#define EINTR 4
#define EBADF 9
#define EACCES 13
#define EEXIST 17
#define ENOSPC 28
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EFAULT 14
#define ENOEXEC 8
#define ENOTDIR 20
#define EINVAL 22
#define EMFILE 24
#define ENOSYS 38
#define EISDIR 21
#define ENOSPC 28
#define ETIMEDOUT 110
#define ENOTEMPTY 39
#define ENAMETOOLONG 36
#define ENOTSUP 95
#define ELOOP 40
#define EOVERFLOW 75
#define ECONNREFUSED 111
#define ENOTCONN 107
#define EISCONN 106
#define EPROTONOSUPPORT 93

static void cleartid_wake(uint32_t *addr);

static inline proc_t *cur(void) { return g_current_proc; }

static void proc_release_fdtable(proc_t *p) {
    if (!p || !p->fds) return;
    if (p->fds_refcnt && *p->fds_refcnt > 1) {
        (*p->fds_refcnt)--;
    } else {
        vfs_free_fdtable(p->fds);
        if (p->fds_refcnt) kfree(p->fds_refcnt);
    }
    p->fds = NULL;
    p->fds_refcnt = NULL;
}

void syscall_set_brk(uint64_t brk_base) {
    proc_t *p = cur();
    if (p) p->brk = p->brk_base = PAGE_ALIGN_UP(brk_base);
}

static int64_t sys_brk(uint64_t addr) {
    proc_t *p = cur();
    if (!p || !p->space) return -(int64_t) ENOMEM;
    if (addr == 0) return (int64_t) p->brk;
    if (addr < p->brk_base) return (int64_t) p->brk;

    if (addr <= p->brk) {
        uint64_t new_end = PAGE_ALIGN_UP(addr);
        uint64_t old_end = PAGE_ALIGN_UP(p->brk);
        uint64_t nfreed = (old_end - new_end) / PAGE_SIZE;
        for (uint64_t va = new_end; va < old_end; va += PAGE_SIZE) {
            uint64_t phys = vmm_virt_to_phys(p->space, va);
            vmm_unmap(p->space, va);
            if (phys) pmm_free((void *) phys);
        }
        p->pages_freed += nfreed;
        if (old_end > p->brk_base)
            vma_remove_overlaps(p->space, p->brk_base, old_end - p->brk_base);
        if (new_end > p->brk_base)
            vma_add(p->space, p->brk_base, new_end - p->brk_base, PROT_READ | PROT_WRITE, 0, true);
        p->brk = addr;
        return (int64_t) p->brk;
    }

    uint64_t old = PAGE_ALIGN_UP(p->brk);
    uint64_t new = PAGE_ALIGN_UP(addr);
    uint64_t nallocd = 0;
    for (uint64_t va = old; va < new; va += PAGE_SIZE) {
        void *ph = pmm_alloc_zeroed();
        if (!ph) {
            for (uint64_t r = old; r < va; r += PAGE_SIZE) {
                uint64_t phys = vmm_virt_to_phys(p->space, r);
                vmm_unmap(p->space, r);
                if (phys) pmm_free((void *) phys);
            }
            return (int64_t) p->brk;
        }
        if (vmm_map(p->space, va, (uint64_t) ph, VMM_UDATA) < 0) {
            pmm_free(ph);
            for (uint64_t r = old; r < va; r += PAGE_SIZE) {
                uint64_t phys = vmm_virt_to_phys(p->space, r);
                vmm_unmap(p->space, r);
                if (phys) pmm_free((void *) phys);
            }
            return (int64_t) p->brk;
        }
        nallocd++;
    }
    p->pages_alloc += nallocd;
    if (old > p->brk_base) vma_remove_overlaps(p->space, p->brk_base, old - p->brk_base);
    vma_add(p->space, p->brk_base, new - p->brk_base, PROT_READ | PROT_WRITE, 0, true);
    p->brk = addr;
    return (int64_t) addr;
}

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define MAP_ANON 0x20
#define MAP_FIXED 0x10
#define MAP_PRIVATE 0x02
#define MAP_SHARED 0x01

static bool user_map_range_ok(uint64_t addr, uint64_t len) {
    if (!len || (addr & (PAGE_SIZE - 1))) return false;
    if (addr >= USER_LIMIT || len > USER_LIMIT - addr) return false;
    return true;
}

static bool prot_valid(uint64_t prot) {
    return (prot & ~(uint64_t) (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0;
}

static bool mmap_flags_valid(uint64_t flags) {
    if (flags & ~(uint64_t) (MAP_ANON | MAP_FIXED | MAP_PRIVATE | MAP_SHARED)) return false;
    if (!(flags & MAP_ANON) && !(flags & (MAP_PRIVATE | MAP_SHARED))) return false;
    return true;
}

static int mmap_pick_addr(proc_t *p, uint64_t addr, uint64_t length, uint64_t flags,
                          uint64_t *out) {
    if ((flags & MAP_FIXED) && addr) {
        uint64_t va = PAGE_ALIGN_DOWN(addr);
        if (!user_map_range_ok(va, length)) return -EINVAL;
        for (uint64_t o = 0; o < length; o += PAGE_SIZE) {
            uint64_t page = va + o;
            if (!vma_page_mapped(p->space, page) && vmm_virt_to_phys(p->space, page))
                return -EINVAL;
        }
        *out = va;
        return 0;
    }

    uint64_t va = PAGE_ALIGN_UP(p->mmap_bump);
    for (int tries = 0; tries < VMM_VMA_MAX + 16; tries++) {
        if (!user_map_range_ok(va, length)) return -ENOMEM;
        if (!vma_conflicts(p->space, va, length)) {
            p->mmap_bump = va + length;
            *out = va;
            return 0;
        }
        va += length;
    }
    return -ENOMEM;
}

static void unmap_owned_pages(proc_t *p, uint64_t addr, uint64_t len) {
    uint64_t nfreed = 0;
    for (uint64_t o = 0; o < len; o += PAGE_SIZE) {
        uint64_t va = addr + o;
        bool tracked = vma_page_mapped(p->space, va);
        bool owned = vma_page_owned(p->space, va);
        uint64_t phys = vmm_virt_to_phys(p->space, va);
        if (tracked) vmm_unmap(p->space, va);
        if (owned && phys) {
            pmm_free((void *) phys);
            nfreed++;
        }
    }
    p->pages_freed += nfreed;
}

static void rollback_new_mapping(proc_t *p, uint64_t addr, uint64_t mapped_len, uint64_t vma_len) {
    if (mapped_len) unmap_owned_pages(p, addr, mapped_len);
    vma_remove(p->space, addr, vma_len);
}

static int mmap_fixed_replace(proc_t *p, uint64_t addr, uint64_t len, uint64_t flags) {
    if (!(flags & MAP_FIXED)) return 0;
    unmap_owned_pages(p, addr, len);
    return vma_remove_overlaps(p->space, addr, len);
}

static int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd,
                        uint64_t off) {
    (void) fd;
    (void) off;
    proc_t *p = cur();
    if (!p || !p->space) return -(int64_t) ENOMEM;
    if (!length) return -(int64_t) EINVAL;
    length = PAGE_ALIGN_UP(length);
    if (!prot_valid(prot) || !mmap_flags_valid(flags)) return -(int64_t) EINVAL;
    if ((off & (PAGE_SIZE - 1)) != 0) return -(int64_t) EINVAL;

    uint64_t va = 0;
    int pick = mmap_pick_addr(p, addr, length, flags, &va);
    if (pick < 0) return pick;
    int fixed_rc = mmap_fixed_replace(p, va, length, flags);
    if (fixed_rc < 0) return fixed_rc;

    uint64_t vf = VMM_UDATA;
    if (!(prot & PROT_WRITE)) vf &= ~(uint64_t) VMM_WRITE;
    if (prot & PROT_EXEC) vf &= ~(uint64_t) VMM_NX;
    bool reserve_only = (prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0;

    if (!(flags & MAP_ANON)) {
        vfs_node_t *fn = fd_get_node((int) fd);
        if (!fn) return -(int64_t) EBADF;

        /* chr-dev with custom mmap (e.g. UIO physical BAR mapping) */
        if (fn->type == VFS_TYPE_CHR && fn->chr_mmap) {
            int rc = vma_add(p->space, va, length, (uint32_t) prot, (uint32_t) flags, false);
            if (rc < 0) return rc;
            if (reserve_only) return (int64_t) va;
            int64_t mr = fn->chr_mmap(fn, off, length, va, vf);
            if (mr < 0) {
                vma_remove(p->space, va, length);
                return mr;
            }
            return mr;
        }

        if (fn->type != VFS_TYPE_REG) return -(int64_t) EBADF;

        int rc = vma_add(p->space, va, length, (uint32_t) prot, (uint32_t) flags, true);
        if (rc < 0) return rc;
        if (reserve_only) return (int64_t) va;

        /* Lazily load file data if filesystem hasn't populated fn->data yet (ext2) */
        if (!fn->data && fn->fs_ops && fn->fs_ops->read)
            fn->fs_ops->read(fn, NULL, 0, 0);

        /* file-backed: MAP_PRIVATE - allocate pages and copy file content */
        uint64_t file_size = fn->size;
        uint64_t nallocd_file = 0;
        for (uint64_t o = 0; o < length; o += PAGE_SIZE) {
            void *ph = pmm_alloc_zeroed();
            if (!ph) {
                rollback_new_mapping(p, va, o, length);
                return -(int64_t) ENOMEM;
            }
            if (vmm_map(p->space, va + o, (uint64_t) ph, vf) < 0) {
                pmm_free(ph);
                rollback_new_mapping(p, va, o, length);
                return -(int64_t) ENOMEM;
            }
            if (fn->data && off + o < file_size) {
                uint64_t copy = file_size - (off + o);
                if (copy > PAGE_SIZE) copy = PAGE_SIZE;
                memcpy(phys_to_virt((uint64_t) ph), fn->data + off + o, copy);
            }
            nallocd_file++;
        }
        p->pages_alloc += nallocd_file;
        return (int64_t) va;
    }

    int rc = vma_add(p->space, va, length, (uint32_t) prot, (uint32_t) flags, true);
    if (rc < 0) return rc;
    if (reserve_only) return (int64_t) va;

    uint64_t nallocd = 0;
    for (uint64_t o = 0; o < length; o += PAGE_SIZE) {
        void *ph = pmm_alloc_zeroed();
        if (!ph) {
            rollback_new_mapping(p, va, o, length);
            return -(int64_t) ENOMEM;
        }
        if (vmm_map(p->space, va + o, (uint64_t) ph, vf) < 0) {
            pmm_free(ph);
            rollback_new_mapping(p, va, o, length);
            return -(int64_t) ENOMEM;
        }
        nallocd++;
    }
    p->pages_alloc += nallocd;
    return (int64_t) va;
}

static int64_t sys_munmap(uint64_t addr, uint64_t len) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EINVAL;
    if ((addr & (PAGE_SIZE - 1)) || !len) return -(int64_t) EINVAL;
    len = PAGE_ALIGN_UP(len);
    if (!user_map_range_ok(addr, len)) return -(int64_t) EINVAL;
    unmap_owned_pages(p, addr, len);
    vma_remove_overlaps(p->space, addr, len);
    return 0;
}

static int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
    proc_t *p = cur();
    if (!p || !len) return 0;
    if (addr & (PAGE_SIZE - 1)) return -(int64_t) EINVAL;
    if (!prot_valid(prot)) return -(int64_t) EINVAL;
    addr = PAGE_ALIGN_DOWN(addr);
    len = PAGE_ALIGN_UP(len);
    if (!user_map_range_ok(addr, len)) return -(int64_t) EINVAL;
    bool tracked = vma_range_ok(p->space, addr, len);
    if (!tracked && !vmm_user_range_ok(p->space, addr, len, false)) return -(int64_t) EINVAL;
    if (tracked) {
        int rc = vma_protect(p->space, addr, len, (uint32_t) prot);
        if (rc < 0) return rc;
    }
    uint64_t flags = VMM_USER | VMM_NX;
    if (prot & PROT_WRITE) flags |= VMM_WRITE;
    if (prot & PROT_EXEC) flags &= ~(uint64_t) VMM_NX;
    for (uint64_t o = 0; o < len; o += PAGE_SIZE) {
        uint64_t va = addr + o;
        if (!vmm_virt_to_phys(p->space, va)) {
            if (tracked) continue;
            return -(int64_t) EINVAL;
        }
        if (vmm_protect(p->space, va, flags) < 0) return -(int64_t) EINVAL;
    }
    return 0;
}

static int64_t sys_fork_at(syscall_frame_t *f, uint64_t child_stack) {
    proc_t *parent = cur();
    if (!parent) return -(int64_t) ENOMEM;
    if (!jail_can_fork(parent->jail_id)) return -(int64_t) EAGAIN;

    proc_t *child = proc_alloc(parent->pid);
    if (!child) return -(int64_t) ENOMEM;

    child->space = vmm_space_new();
    if (!child->space) goto fail_space;

    if (vmm_fork_user(child->space, parent->space) < 0) goto fail_fork;

    vfs_copy_fdtable(child->fds, parent->fds);
    if (child->fds_refcnt) *child->fds_refcnt = 1;

    child->brk = parent->brk;
    child->brk_base = parent->brk_base;
    child->mmap_bump = parent->mmap_bump;
    child->pgid = parent->pgid;
    child->user_rsp = child_stack ? child_stack : cpu_get_user_rsp();

    child->sig_mask = parent->sig_mask;
    memcpy(child->sig_actions, parent->sig_actions, sizeof(parent->sig_actions));
    child->pending_sigs = 0;
    child->fs_base = parent->fs_base;
    child->uid = parent->uid;
    child->euid = parent->euid;
    child->suid = parent->suid;
    child->gid = parent->gid;
    child->egid = parent->egid;
    child->sgid = parent->sgid;
    child->fsuid = parent->fsuid;
    child->fsgid = parent->fsgid;
    child->umask = parent->umask;
    memcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    memcpy(child->exe_path, parent->exe_path, sizeof(child->exe_path));
    child->jail_id = parent->jail_id;
    child->jail_exempt = parent->jail_exempt;
    jail_ref(child->jail_id);

    uint8_t *ksp = child->kstack + KSTACK_SIZE;

    ksp -= sizeof(syscall_frame_t);
    syscall_frame_t *cf = (syscall_frame_t *) ksp;
    *cf = *f;
    cf->rax = 0;

    ksp -= 8;
    *(uint64_t *) ksp = (uint64_t) (uintptr_t) proc_resume_frame;

    ksp -= 6 * 8;
    memset(ksp, 0, 6 * 8);

    child->kstack_rsp = (uint64_t) ksp;
    child->state = PROC_READY;
    proc_set_ready(child);


    return (int64_t) child->pid;

fail_fork:
    vmm_space_free(child->space);
fail_space:
    proc_kstack_free(child);
    kfree(child->fds);
    child->state = PROC_UNUSED;
    proc_clear_used(child);
    return -(int64_t) ENOMEM;
}

static int64_t sys_fork(syscall_frame_t *f) { return sys_fork_at(f, 0); }

#define CLONE_VM 0x00000100
#define CLONE_FILES 0x00000400
#define CLONE_THREAD 0x00010000
#define CLONE_SETTLS 0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID 0x01000000

static int64_t sys_clone(uint64_t flags, uint64_t child_stack, uint32_t *ptid, uint32_t *ctid,
                         uint64_t newtls, syscall_frame_t *f) {
    proc_t *parent = cur();
    if (!parent) return -(int64_t) ENOMEM;

    if (!(flags & CLONE_THREAD)) return sys_fork_at(f, child_stack);
    if ((flags & CLONE_PARENT_SETTID) && (!ptid || !uptr_ok_w(ptid, sizeof(*ptid))))
        return -(int64_t) EFAULT;
    if ((flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) && ctid &&
        !uptr_ok_w(ctid, sizeof(*ctid)))
        return -(int64_t) EFAULT;
    if (!jail_can_fork(parent->jail_id)) return -(int64_t) EAGAIN;

    proc_t *child = proc_alloc(parent->pid);
    if (!child) return -(int64_t) ENOMEM;

    child->space = parent->space; /* shared address space */
    child->is_thread = 1;

    if (flags & CLONE_FILES) {
        kfree(child->fds);
        kfree(child->fds_refcnt);
        child->fds = parent->fds;
        child->fds_refcnt = parent->fds_refcnt;
        if (child->fds_refcnt) (*child->fds_refcnt)++;
    } else {
        vfs_copy_fdtable(child->fds, parent->fds);
        if (child->fds_refcnt) *child->fds_refcnt = 1;
    }

    child->brk = parent->brk;
    child->brk_base = parent->brk_base;
    child->mmap_bump = parent->mmap_bump;
    child->pgid = parent->pgid;
    child->sig_mask = parent->sig_mask;
    memcpy(child->sig_actions, parent->sig_actions, sizeof(parent->sig_actions));
    child->pending_sigs = 0;
    child->fs_base = (flags & CLONE_SETTLS) ? newtls : parent->fs_base;
    child->uid = parent->uid;
    child->euid = parent->euid;
    child->suid = parent->suid;
    child->gid = parent->gid;
    child->egid = parent->egid;
    child->sgid = parent->sgid;
    child->fsuid = parent->fsuid;
    child->fsgid = parent->fsgid;
    child->umask = parent->umask;
    memcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    memcpy(child->exe_path, parent->exe_path, sizeof(child->exe_path));
    child->jail_id = parent->jail_id;
    child->jail_exempt = parent->jail_exempt;
    jail_ref(child->jail_id);

    if ((flags & CLONE_PARENT_SETTID) && ptid) *ptid = child->pid;
    if ((flags & CLONE_CHILD_SETTID) && ctid) *ctid = child->pid;
    if (flags & CLONE_CHILD_CLEARTID) child->cleartid_addr = ctid;

    uint8_t *ksp = child->kstack + KSTACK_SIZE;
    ksp -= sizeof(syscall_frame_t);
    syscall_frame_t *cf = (syscall_frame_t *) ksp;
    *cf = *f;
    cf->rax = 0;

    child->user_rsp = child_stack ? child_stack : parent->user_rsp;

    ksp -= 8;
    *(uint64_t *) ksp = (uint64_t) (uintptr_t) proc_resume_frame;
    ksp -= 6 * 8;
    memset(ksp, 0, 6 * 8);
    child->kstack_rsp = (uint64_t) ksp;
    child->state = PROC_READY;
    proc_set_ready(child);

    log_info("[clone] parent=%u child=%u flags=0x%lx", parent->pid, child->pid, flags);
    return (int64_t) child->pid;
}

static bool copy_user_path(char *out, const char *in) {
    if (!in) return false;
    for (size_t i = 0; i < 511; i++) {
        const char *a = in + i;
        if (i == 0 || ((uint64_t) (uintptr_t) a & 0xFFF) == 0) {
            if (!uptr_ok(a, 1)) return false;
        }
        char c = *a;
        out[i] = c;
        if (!c) return true;
    }
    out[511] = '\0';
    return true;
}

/* resolve a user path to an absolute one in out[512]. returns false
                if the user pointer is invalid; on success out is always non-empty */
static bool path_abs(char *out, const char *in) {
    char tmp[512];
    if (!copy_user_path(tmp, in)) return false;
    const char *root = jail_root_current();
    if (tmp[0] == '/') {
        if (root[0])
            snprintf(out, 512, "%s%s", root, tmp);
        else
            memcpy(out, tmp, sizeof(tmp));
    } else {
        proc_t *p = cur();
        const char *cwd = (p && p->cwd[0]) ? p->cwd : "/";
        size_t cl = strlen(cwd);
        if (cl >= 511) {
            out[0] = '/';
            out[1] = '\0';
        } else {
            memcpy(out, cwd, cl);
            if (out[cl - 1] != '/') out[cl++] = '/';
            strncpy(out + cl, tmp, 511 - cl);
            out[511] = '\0';
        }
    }
    if (root[0]) jail_canon_clamp(out, 512, root);
    return true;
}

#define MAX_EXEC_ARGS 32

static int64_t sys_execve(const char *path, const char **uargv, const char **uenvp) {
    if (!path) return -(int64_t) EFAULT;

    char abs[512];
    if (!path_abs(abs, path)) return -(int64_t) EFAULT;
    const char *exec_path = abs;

    vfs_node_t *node = vfs_lookup(exec_path);
    if (!node || node->type != VFS_TYPE_REG) {
        vfs_node_unref_internal(node);
        return -(int64_t) ENOENT;
    }
    /* lazy-load file data if needed */
    if (!node->data && node->fs_ops && node->fs_ops->read)
        node->fs_ops->read(node, NULL, 0, node->size);
    if (!node->data) {
        vfs_node_unref_internal(node);
        return -(int64_t) ENOENT;
    }
    int exec_perm = vfs_access(exec_path, 1);
    if (exec_perm < 0) {
        vfs_node_unref_internal(node);
        return (int64_t) exec_perm;
    }

    char shebang_interp[512] = { 0 };
    char shebang_arg[256] = { 0 };
    char script_path[512] = { 0 };
    bool is_shebang = false;

    if (node->size >= 2 && node->data[0] == '#' && node->data[1] == '!') {
        char line[256];
        size_t ll = 0;
        while (ll < 254 && 2 + ll < node->size) {
            char ch = (char) node->data[2 + ll];
            if (ch == '\n' || ch == '\r') break;
            line[ll++] = ch;
        }
        line[ll] = '\0';
        char *p2 = line;
        while (*p2 == ' ' || *p2 == '\t') p2++;
        if (!*p2) {
            vfs_node_unref_internal(node);
            return -(int64_t) ENOEXEC;
        }
        char *istart = p2;
        while (*p2 && *p2 != ' ' && *p2 != '\t') p2++;
        size_t ilen = (size_t) (p2 - istart);
        if (!ilen || ilen >= sizeof(shebang_interp)) {
            vfs_node_unref_internal(node);
            return -(int64_t) ENOEXEC;
        }
        memcpy(shebang_interp, istart, ilen);
        while (*p2 == ' ' || *p2 == '\t') p2++;
        if (*p2) {
            strncpy(shebang_arg, p2, sizeof(shebang_arg) - 1);
            char *e = shebang_arg + strlen(shebang_arg) - 1;
            while (e >= shebang_arg && (*e == ' ' || *e == '\t' || *e == '\r')) *e-- = '\0';
        }
        strncpy(script_path, exec_path, sizeof(script_path) - 1);
        vfs_node_unref_internal(node);
        node = vfs_lookup(shebang_interp);
        if (!node || node->type != VFS_TYPE_REG) {
            vfs_node_unref_internal(node);
            return -(int64_t) ENOENT;
        }
        /* lazy-load interpreter if needed */
        if (!node->data && node->fs_ops && node->fs_ops->read)
            node->fs_ops->read(node, NULL, 0, node->size);
        if (!node->data) {
            log_error("execve: interp '%s' has no data", shebang_interp);
            vfs_node_unref_internal(node);
            return -(int64_t) ENOENT;
        }
        exec_path = shebang_interp;
        is_shebang = true;
    }

    char *argv_mem[MAX_EXEC_ARGS] = { 0 };
    char *envp_mem[MAX_EXEC_ARGS] = { 0 };
    const char *kargv[MAX_EXEC_ARGS + 1];
    const char *kenvp[MAX_EXEC_ARGS + 1];
    int argc = 0, envc = 0;

#define FREE_EXEC_STRS()                                                                           \
    do {                                                                                           \
        for (int _i = 0; _i < MAX_EXEC_ARGS; _i++) {                                               \
            if (argv_mem[_i]) {                                                                    \
                kfree(argv_mem[_i]);                                                               \
                argv_mem[_i] = NULL;                                                               \
            }                                                                                      \
            if (envp_mem[_i]) {                                                                    \
                kfree(envp_mem[_i]);                                                               \
                envp_mem[_i] = NULL;                                                               \
            }                                                                                      \
        }                                                                                          \
    } while (0)

    if (is_shebang) {
        kargv[argc++] = shebang_interp;
        if (shebang_arg[0]) kargv[argc++] = shebang_arg;
        kargv[argc++] = script_path;
        int ui = 1;
        while (argc < MAX_EXEC_ARGS && uargv && uargv[ui]) {
            size_t n = strlen(uargv[ui]) + 1;
            argv_mem[argc] = kmalloc(n);
            if (!argv_mem[argc]) {
                vfs_node_unref_internal(node);
                FREE_EXEC_STRS();
                return -(int64_t) ENOMEM;
            }
            memcpy(argv_mem[argc], uargv[ui], n);
            kargv[argc] = argv_mem[argc];
            argc++;
            ui++;
        }
    } else {
        if (uargv) {
            while (argc < MAX_EXEC_ARGS && uargv[argc]) {
                size_t n = strlen(uargv[argc]) + 1;
                argv_mem[argc] = kmalloc(n);
                if (!argv_mem[argc]) {
                    vfs_node_unref_internal(node);
                    FREE_EXEC_STRS();
                    return -(int64_t) ENOMEM;
                }
                memcpy(argv_mem[argc], uargv[argc], n);
                kargv[argc] = argv_mem[argc];
                argc++;
            }
        }
        if (argc == 0) {
            size_t n = strlen(exec_path) + 1;
            argv_mem[0] = kmalloc(n);
            if (!argv_mem[0]) {
                vfs_node_unref_internal(node);
                FREE_EXEC_STRS();
                return -(int64_t) ENOMEM;
            }
            memcpy(argv_mem[0], exec_path, n);
            kargv[0] = argv_mem[0];
            argc = 1;
        }
    }
    kargv[argc] = NULL;

    if (uenvp) {
        while (envc < MAX_EXEC_ARGS && uenvp[envc]) {
            size_t n = strlen(uenvp[envc]) + 1;
            envp_mem[envc] = kmalloc(n);
            if (!envp_mem[envc]) {
                vfs_node_unref_internal(node);
                FREE_EXEC_STRS();
                return -(int64_t) ENOMEM;
            }
            memcpy(envp_mem[envc], uenvp[envc], n);
            kenvp[envc] = envp_mem[envc];
            envc++;
        }
    }
    kenvp[envc] = NULL;

    elf_load_result_t res;
    if (elf_load(node->data, node->size, &res) < 0) {
        vfs_node_unref_internal(node);
        FREE_EXEC_STRS();
        return -(int64_t) ENOEXEC;
    }
    vfs_node_unref_internal(node); /* data is now mapped; release our ref */
    node = NULL;

    if (res.interp[0]) {
        vfs_node_t *inode = vfs_lookup(res.interp);
        if (!inode || inode->type != VFS_TYPE_REG) {
            vfs_node_unref_internal(inode);
            vmm_space_free(res.space);
            FREE_EXEC_STRS();
            return -(int64_t) ENOENT;
        }
        /* lazy-load interpreter data if needed */
        if (!inode->data && inode->fs_ops && inode->fs_ops->read)
            inode->fs_ops->read(inode, NULL, 0, inode->size);
        if (!inode->data) {
            vfs_node_unref_internal(inode);
            vmm_space_free(res.space);
            FREE_EXEC_STRS();
            return -(int64_t) ENOENT;
        }
        elf_load_result_t ires;
        memset(&ires, 0, sizeof(ires));
        if (elf_load_into(res.space, inode->data, inode->size, 0x7f0000000000ULL, &ires) < 0) {
            vfs_node_unref_internal(inode);
            vmm_space_free(res.space);
            FREE_EXEC_STRS();
            return -(int64_t) ENOEXEC;
        }
        vfs_node_unref_internal(inode);
        res.entry = ires.prog_entry;
        res.interp_base = 0x7f0000000000ULL;
    }

    uint64_t rsp = setup_user_stack(res.space, &res, argc, (const char *const *) kargv,
                                    (const char *const *) kenvp);
    FREE_EXEC_STRS();

    if (!rsp) {
        vmm_space_free(res.space);
        return -(int64_t) ENOMEM;
    }

    proc_t *p = cur();
    vmm_space_t *old = p->space;

    p->space = res.space;
    p->brk = PAGE_ALIGN_UP(res.brk);
    p->brk_base = p->brk;
    p->mmap_bump = 0x0000500000000000ULL + ((kern_rand64() & 0x1FFULL) << 21);
    strncpy(p->exe_path, exec_path, sizeof(p->exe_path) - 1);
    g_current_space = p->space;

    vmm_switch(p->space);
    vmm_space_free(old);

    vfs_cloexec_flush();

    for (int i = 0; i < NSIG; i++) {
        if (p->sig_actions[i].sa_handler != SIG_IGN) {
            p->sig_actions[i].sa_handler = SIG_DFL;
            p->sig_actions[i].sa_flags = 0;
            p->sig_actions[i].sa_restorer = 0;
            p->sig_actions[i].sa_mask = 0;
        }
    }

    fpu_init(); /* exec gives the new image a clean, all-masked FPU/SSE state */

    if (g_jail_auto_isolate && !p->jail_exempt) {
        kjail_conf_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.flags = JAILF_ALL; /* empty root => same fs view, fresh pid/ipc namespace */
        int jid = jail_create(p->jail_id, &cfg, p->euid);
        if (jid > 0) jail_enter(p, (uint32_t) jid);
    }

    log_info("[exec] pid=%u entry=0x%lx rsp=0x%lx jail=%u", p->pid, res.entry, rsp, p->jail_id);

    if (p->tracer_pid) {
        syscall_frame_t exec_frame = { 0 };
        exec_frame.rcx = res.entry;
        exec_frame.r11 = 0x202ULL;
        p->user_rsp = rsp;
        proc_ptrace_stop(p, SIGTRAP, 1, &exec_frame, &exec_frame.r11);
        enter_userspace_exec(exec_frame.rcx, p->user_rsp, exec_frame.r11);
    }

    enter_userspace_exec(res.entry, rsp, 0x202ULL);
}

__attribute__((noreturn)) void proc_do_exit(int code) {
    proc_t *p = cur();
    log_info("[pid %u] exit(%d)", p->pid, code);

    /* clear stale pipe waiting pointer before slot is reused */
    if (p->blocked_pipe) {
        pipe_t *bp = (pipe_t *) p->blocked_pipe;
        if (p->blocked_pipe_read) {
            if (bp->waiting_reader == p) bp->waiting_reader = NULL;
        } else {
            if (bp->waiting_writer == p) bp->waiting_writer = NULL;
        }
        p->blocked_pipe = NULL;
    }

    shm_proc_exit(p->pid);
    jail_unref(p->jail_id);
    proc_release_fdtable(p);

    spin_lock(&g_proctable_lock);
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *child = &g_proctable[i];
        if (child->state != PROC_UNUSED && child->ppid == p->pid) child->ppid = 1;
    }
    spin_unlock(&g_proctable_lock);

    if (p->is_thread) {
        if (p->cleartid_addr) {
            *p->cleartid_addr = 0;
            cleartid_wake(p->cleartid_addr);
        }
        p->state = PROC_DYING;
        proc_clear_ready(p);
        proc_defer_thread_reap(p);
        proc_t *nt = sched_claim_next(p);
        if (nt) {
            vfs_set_fdtable(nt->fds);
            g_current_space = nt->space;
            cpu_set_kernel_stack(nt->kstack_top);
            sched_switch(nt);
        }
        /* unreachable: this thread is dead and never scheduled again */
        cpu_halt();
    }

    p->exit_code = code;
    p->state = PROC_ZOMBIE;
    proc_clear_ready(p);

    proc_t *parent = proc_find(p->ppid);
    if (parent && parent->state != PROC_UNUSED) {
        proc_send_signal(parent, SIGCHLD);
        proc_unref(parent);
    }

    /* Switch to this CPU's idle process, which will continue in ap_sched_loop.
       We never sched_switch to the parent because its context may be live
       on another CPU (never saved via sched_switch). */
    uint32_t cpu_id = this_cpu_id();
    proc_t *idle = (proc_t *) g_cpu_local[cpu_id].idle;
    if (idle && idle->state == PROC_READY) {
        idle->state = PROC_RUNNING;
        vfs_set_fdtable(idle->fds);
        g_current_space = idle->space;
        cpu_set_kernel_stack(idle->kstack_top);
        sched_switch(idle);
        /* unreachable */
    }

    /* Fallback: find any READY process that isn't us or the parent */
    for (;;) {
        proc_t *next = sched_claim_next(p);
        if (next) {
            vfs_set_fdtable(next->fds);
            g_current_space = next->space;
            cpu_set_kernel_stack(next->kstack_top);
            sched_switch(next);
        }
        sti();
        hlt();
        cli();
    }
    cpu_halt(); /* unreachable */
}

#define PTRACE_TRACEME 0
#define PTRACE_PEEKTEXT 1
#define PTRACE_PEEKDATA 2
#define PTRACE_POKETEXT 4
#define PTRACE_POKEDATA 5
#define PTRACE_CONT 7
#define PTRACE_KILL 8
#define PTRACE_SINGLESTEP 9
#define PTRACE_GETREGS 12
#define PTRACE_SETREGS 13
#define PTRACE_ATTACH 16
#define PTRACE_DETACH 17
#define PTRACE_SYSCALL 24
#define PTRACE_SETOPTIONS 0x4200

struct ptrace_user_regs {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rax, rcx, rdx, rsi, rdi;
    uint64_t orig_rax, rip, cs, eflags, rsp, ss, fs_base, gs_base, ds, es, fs, gs;
};

static void ptrace_fill_regs(proc_t *t, struct ptrace_user_regs *out) {
    memset(out, 0, sizeof(*out));
    out->cs = GDT_USER_CODE_SEL;
    out->ss = out->ds = out->es = out->fs = out->gs = GDT_USER_DATA_SEL;
    out->fs_base = t->fs_base;
    out->rsp = t->user_rsp;

    if (t->ptrace_frame_kind == 1) {
        syscall_frame_t *f = (syscall_frame_t *) t->ptrace_frame;
        out->r15 = f->r15;
        out->r14 = f->r14;
        out->r13 = f->r13;
        out->r12 = f->r12;
        out->rbp = f->rbp;
        out->rbx = f->rbx;
        out->r11 = f->r11;
        out->r10 = f->r10;
        out->r9 = f->r9;
        out->r8 = f->r8;
        out->rax = f->rax;
        out->rcx = f->rcx;
        out->rdx = f->rdx;
        out->rsi = f->rsi;
        out->rdi = f->rdi;
        out->orig_rax = f->rax;
        out->rip = f->rcx;
        out->eflags = f->r11;
    } else if (t->ptrace_frame_kind == 2) {
        cpu_state_t *s = (cpu_state_t *) t->ptrace_frame;
        out->r15 = s->r15;
        out->r14 = s->r14;
        out->r13 = s->r13;
        out->r12 = s->r12;
        out->rbp = s->rbp;
        out->rbx = s->rbx;
        out->r11 = s->r11;
        out->r10 = s->r10;
        out->r9 = s->r9;
        out->r8 = s->r8;
        out->rax = s->rax;
        out->rcx = s->rcx;
        out->rdx = s->rdx;
        out->rsi = s->rsi;
        out->rdi = s->rdi;
        out->orig_rax = s->rax;
        out->rip = s->rip;
        out->cs = s->cs;
        out->eflags = s->rflags;
        out->rsp = s->rsp;
        out->ss = s->ss;
    }
}

static int ptrace_store_regs(proc_t *t, const struct ptrace_user_regs *in) {
    if (t->ptrace_frame_kind == 1) {
        syscall_frame_t *f = (syscall_frame_t *) t->ptrace_frame;
        f->r15 = in->r15;
        f->r14 = in->r14;
        f->r13 = in->r13;
        f->r12 = in->r12;
        f->rbp = in->rbp;
        f->rbx = in->rbx;
        f->r11 = in->eflags;
        f->r10 = in->r10;
        f->r9 = in->r9;
        f->r8 = in->r8;
        f->rax = in->rax;
        f->rcx = in->rip;
        f->rdx = in->rdx;
        f->rsi = in->rsi;
        f->rdi = in->rdi;
        t->user_rsp = in->rsp;
        return 0;
    }
    if (t->ptrace_frame_kind == 2) {
        cpu_state_t *s = (cpu_state_t *) t->ptrace_frame;
        s->r15 = in->r15;
        s->r14 = in->r14;
        s->r13 = in->r13;
        s->r12 = in->r12;
        s->rbp = in->rbp;
        s->rbx = in->rbx;
        s->r11 = in->r11;
        s->r10 = in->r10;
        s->r9 = in->r9;
        s->r8 = in->r8;
        s->rax = in->rax;
        s->rcx = in->rcx;
        s->rdx = in->rdx;
        s->rsi = in->rsi;
        s->rdi = in->rdi;
        s->rip = in->rip;
        s->rflags = (in->eflags & 0x3F7FD7ULL) | 0x2ULL;
        s->rsp = in->rsp;
        return 0;
    }
    return -1;
}

static int64_t ptrace_rw_mem(proc_t *target, uint64_t addr, void *kbuf, uint64_t len, int write) {
    if (len == 0) return 0;
    if (addr >= USER_LIMIT || len > USER_LIMIT - addr) return -1;

    cli();
    vmm_space_t *saved_space = g_current_space;
    g_current_space = target->space;
    vmm_switch(target->space);

    int ok = write ? uptr_ok_w((void *) (uintptr_t) addr, len) : uptr_ok((void *) (uintptr_t) addr, len);
    if (ok) {
        if (write)
            memcpy((void *) (uintptr_t) addr, kbuf, len);
        else
            memcpy(kbuf, (void *) (uintptr_t) addr, len);
    }

    g_current_space = saved_space;
    vmm_switch(saved_space);
    sti();
    return ok ? 0 : -1;
}

static int64_t sys_ptrace(int64_t request, int64_t pid, uint64_t addr, uint64_t data) {
    proc_t *self = cur();

    if (request == PTRACE_TRACEME) {
        self->tracer_pid = self->ppid;
        return 0;
    }

    proc_t *t = proc_find((uint32_t) pid);
    if (!t || t->state == PROC_UNUSED) { if (t) proc_unref(t); return -(int64_t) ESRCH; }

    int64_t rc = 0;

    if (request == PTRACE_ATTACH) {
        if (t->tracer_pid) { rc = -(int64_t) EPERM; goto out; }
        t->tracer_pid = self->pid;
        proc_send_signal(t, SIGSTOP);
        goto out;
    }

    if (t->tracer_pid != self->pid) { rc = -(int64_t) ESRCH; goto out; }

    switch (request) {
    case PTRACE_PEEKTEXT:
    case PTRACE_PEEKDATA: {
        uint64_t val = 0;
        if (ptrace_rw_mem(t, addr, &val, sizeof(val), 0) < 0) { rc = -(int64_t) EIO; goto out; }
        if (!uptr_ok_w((void *) data, sizeof(val))) { rc = -(int64_t) EFAULT; goto out; }
        *(uint64_t *) data = val;
        break;
    }
    case PTRACE_POKETEXT:
    case PTRACE_POKEDATA: {
        uint64_t val = data;
        if (ptrace_rw_mem(t, addr, &val, sizeof(val), 1) < 0) { rc = -(int64_t) EIO; goto out; }
        break;
    }
    case PTRACE_GETREGS: {
        struct ptrace_user_regs regs;
        ptrace_fill_regs(t, &regs);
        if (!uptr_ok_w((void *) data, sizeof(regs))) { rc = -(int64_t) EFAULT; goto out; }
        memcpy((void *) data, &regs, sizeof(regs));
        break;
    }
    case PTRACE_SETREGS: {
        struct ptrace_user_regs regs;
        if (!uptr_ok((void *) data, sizeof(regs))) { rc = -(int64_t) EFAULT; goto out; }
        memcpy(&regs, (void *) data, sizeof(regs));
        if (ptrace_store_regs(t, &regs) < 0) { rc = -(int64_t) EIO; goto out; }
        break;
    }
    case PTRACE_CONT:
    case PTRACE_SYSCALL:
    case PTRACE_SINGLESTEP:
        if (!t->ptrace_stopped) { rc = -(int64_t) ESRCH; goto out; }
        if ((int) data > 0 && (int) data < NSIG)
            __atomic_fetch_or(&t->pending_sigs, (1ULL << ((int) data - 1)), __ATOMIC_RELAXED);
        t->ptrace_syscall_trace = (request == PTRACE_SYSCALL);
        t->ptrace_step = (request == PTRACE_SINGLESTEP);
        t->ptrace_stopped = 0;
        t->ptrace_reported = 0;
        if (__sync_bool_compare_and_swap(&t->state, PROC_WAITING, PROC_READY))
            proc_set_ready(t);
        break;
    case PTRACE_KILL:
        __atomic_fetch_or(&t->pending_sigs, (1ULL << (SIGKILL - 1)), __ATOMIC_RELAXED);
        t->ptrace_stopped = 0;
        t->ptrace_reported = 0;
        if (__sync_bool_compare_and_swap(&t->state, PROC_WAITING, PROC_READY))
            proc_set_ready(t);
        break;
    case PTRACE_DETACH:
        t->tracer_pid = 0;
        t->ptrace_syscall_trace = 0;
        if (t->ptrace_stopped) {
            t->ptrace_stopped = 0;
            t->ptrace_reported = 0;
            if (__sync_bool_compare_and_swap(&t->state, PROC_WAITING, PROC_READY))
                proc_set_ready(t);
        }
        break;
    case PTRACE_SETOPTIONS:
        break;
    default:
        rc = -(int64_t) EINVAL;
        break;
    }
out:
    proc_unref(t);
    return rc;
}

static int64_t sys_wait4(int pid, int *wstatus, int options, void *rusage) {
    if (wstatus && !uptr_ok_w(wstatus, sizeof(*wstatus))) return -(int64_t) EFAULT;
    if (rusage && !uptr_ok_w(rusage, 144)) return -(int64_t) EFAULT;
    proc_t *parent = cur();

    while (1) {
        bool any_child = false;
        for (int i = 0; i < PROC_MAX; i++) {
            proc_t *c = &g_proctable[i];
            if (c->state == PROC_UNUSED) continue;
            bool is_tracee = c->tracer_pid == parent->pid;
            if (c->ppid != parent->pid && !is_tracee) continue;
            if (!jail_can_see(parent, c)) continue;
            if (pid > 0 && (int) c->pid != pid) continue;
            any_child = true;

            if (c->state == PROC_ZOMBIE) {
                if (wstatus) *wstatus = (c->exit_code & 0xFF) << 8;
                uint32_t cpid = c->pid;
                spin_lock(&g_proctable_lock);
                proc_release_fdtable(c);
                vmm_space_free(c->space);
                proc_kstack_free(c);
                memset(c, 0, sizeof(*c));
                c->state = PROC_UNUSED;
                proc_clear_used(c);
                spin_unlock(&g_proctable_lock);
                return (int64_t) cpid;
            }

            if (is_tracee && c->ptrace_stopped && !c->ptrace_reported) {
                c->ptrace_reported = 1;
                if (wstatus) *wstatus = ((c->ptrace_stop_sig & 0xFF) << 8) | 0x7f;
                return (int64_t) c->pid;
            }
        }

        if (!any_child) return -(int64_t) ECHILD;

        if (options & 1) /* WNOHANG */
            return 0;

        sched_yield_blocking();
    }
}

#define ARCH_SET_FS 0x1002
#define ARCH_SET_GS 0x1001
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

static int64_t sys_arch_prctl(int code, uint64_t addr) {
    switch (code) {
    case ARCH_SET_FS:
        if (addr >= USER_LIMIT) return -(int64_t) EPERM;
        wrmsr(0xC0000100, addr);
        cur()->fs_base = addr;
        return 0;
    case ARCH_SET_GS:
        if (addr >= USER_LIMIT) return -(int64_t) EPERM;
        wrmsr(0xC0000101, addr);
        return 0;
    case ARCH_GET_FS:
        if (!uptr_ok_w((void *) addr, sizeof(uint64_t))) return -(int64_t) EFAULT;
        *(uint64_t *) addr = cur()->fs_base;
        return 0;
    case ARCH_GET_GS:
        if (!uptr_ok_w((void *) addr, sizeof(uint64_t))) return -(int64_t) EFAULT;
        *(uint64_t *) addr = rdmsr(0xC0000101);
        return 0;
    }
    return -(int64_t) EINVAL;
}

struct utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

static int64_t sys_uname(struct utsname *buf) {
    if (!buf) return -(int64_t) EFAULT;
    if (!uptr_ok_w(buf, sizeof(*buf))) return -(int64_t) EFAULT;
    memset(buf, 0, sizeof(*buf));
    memcpy(buf->sysname, "k9", 7);
    memcpy(buf->nodename, "kx", 2);
    memcpy(buf->release, KERNEL_VERSION, sizeof(KERNEL_VERSION));
    memcpy(buf->version, "UP", 6);
    memcpy(buf->machine, "x86_64", 6);
    return 0;
}

static int64_t sys_getcwd(char *buf, uint64_t size) {
    if (!buf || !size) return -(int64_t) EINVAL;
    if (!uptr_ok_w(buf, size)) return -(int64_t) EFAULT;
    proc_t *p = cur();
    char tmp[512];
    strncpy(tmp, p ? p->cwd : g_cwd, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    jail_strip_root(tmp, sizeof(tmp)); /* report jail-relative cwd to the process */
    size_t len = strlen(tmp) + 1;
    if (len > size) return -(int64_t) EINVAL;
    memcpy(buf, tmp, len);
    return (int64_t) (uintptr_t) buf;
}

static int64_t sys_chdir(const char *path) {
    if (!path) return -(int64_t) EFAULT;
    char abs[512];
    if (!path_abs(abs, path)) return -(int64_t) EFAULT;
    const char *rp = abs;
    vfs_node_t *n = vfs_lookup(rp);
    if (!n) return -(int64_t) ENOENT;
    if (n->type != VFS_TYPE_DIR) {
        vfs_node_unref_internal(n);
        return -(int64_t) ENOTDIR;
    }
    proc_t *p = cur();
    if (p) strncpy(p->cwd, rp, sizeof(p->cwd) - 1);
    vfs_node_unref_internal(n);
    return 0;
}

static int64_t sys_getpid(void) { return cur() ? (int64_t) cur()->pid : 1; }
static int64_t sys_getppid(void) { return cur() ? (int64_t) cur()->ppid : 0; }
static int64_t sys_getuid(void) {
    proc_t *p = cur();
    return p ? (int64_t) p->uid : 0;
}
static int64_t sys_getgid(void) {
    proc_t *p = cur();
    return p ? (int64_t) p->gid : 0;
}
static int64_t sys_geteuid(void) {
    proc_t *p = cur();
    return p ? (int64_t) p->euid : 0;
}
static int64_t sys_getegid(void) {
    proc_t *p = cur();
    return p ? (int64_t) p->egid : 0;
}

static bool is_root(void) {
    proc_t *p = cur();
    return p && p->euid == 0;
}

/* host-global privilege: euid 0 AND not confined by a JAILF_PRIV jail */
static bool host_priv(void) { return jail_host_priv(cur()); }

static int64_t sys_setfsuid(uint32_t uid) {
    proc_t *p = cur();
    if (!p) return 0;
    uint32_t old = p->fsuid;
    if (uid == (uint32_t) -1) return old;
    if (p->euid == 0 || uid == p->uid || uid == p->euid || uid == p->suid) p->fsuid = uid;
    return old;
}

static int64_t sys_setfsgid(uint32_t gid) {
    proc_t *p = cur();
    if (!p) return 0;
    uint32_t old = p->fsgid;
    if (gid == (uint32_t) -1) return old;
    if (p->euid == 0 || gid == p->gid || gid == p->egid || gid == p->sgid) p->fsgid = gid;
    return old;
}

static int64_t sys_setuid(uint32_t uid) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EPERM;
    if (p->euid == 0) {
        p->uid = p->euid = p->suid = p->fsuid = uid;
        return 0;
    }
    if (uid != p->uid && uid != p->suid) return -(int64_t) EPERM;
    p->euid = p->fsuid = uid;
    return 0;
}
static int64_t sys_setgid(uint32_t gid) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EPERM;
    if (p->euid == 0) {
        p->gid = p->egid = p->sgid = p->fsgid = gid;
        return 0;
    }
    if (gid != p->gid && gid != p->sgid) return -(int64_t) EPERM;
    p->egid = p->fsgid = gid;
    return 0;
}
static int64_t sys_setreuid(uint32_t ruid, uint32_t euid) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EPERM;
    if (p->euid != 0 && (ruid != (uint32_t) -1 && ruid != p->uid && ruid != p->euid))
        return -(int64_t) EPERM;
    if (p->euid != 0 &&
        (euid != (uint32_t) -1 && euid != p->uid && euid != p->euid && euid != p->suid))
        return -(int64_t) EPERM;
    if (ruid != (uint32_t) -1) p->uid = ruid;
    if (euid != (uint32_t) -1) p->euid = p->fsuid = euid;
    p->suid = p->euid;
    return 0;
}
static int64_t sys_setregid(uint32_t rgid, uint32_t egid) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EPERM;
    if (p->euid != 0 && (rgid != (uint32_t) -1 && rgid != p->gid && rgid != p->egid))
        return -(int64_t) EPERM;
    if (p->euid != 0 &&
        (egid != (uint32_t) -1 && egid != p->gid && egid != p->egid && egid != p->sgid))
        return -(int64_t) EPERM;
    if (rgid != (uint32_t) -1) p->gid = rgid;
    if (egid != (uint32_t) -1) p->egid = p->fsgid = egid;
    p->sgid = p->egid;
    return 0;
}
static int64_t sys_setresuid(uint32_t ruid, uint32_t euid, uint32_t suid) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EPERM;
    if (p->euid != 0) {
        uint32_t cur_set[3] = { p->uid, p->euid, p->suid };
        if (ruid != (uint32_t) -1 && ruid != cur_set[0] && ruid != cur_set[1] && ruid != cur_set[2])
            return -(int64_t) EPERM;
        if (euid != (uint32_t) -1 && euid != cur_set[0] && euid != cur_set[1] && euid != cur_set[2])
            return -(int64_t) EPERM;
        if (suid != (uint32_t) -1 && suid != cur_set[0] && suid != cur_set[1] && suid != cur_set[2])
            return -(int64_t) EPERM;
    }
    if (ruid != (uint32_t) -1) p->uid = ruid;
    if (euid != (uint32_t) -1) p->euid = p->fsuid = euid;
    if (suid != (uint32_t) -1) p->suid = suid;
    return 0;
}
static int64_t sys_setresgid(uint32_t rgid, uint32_t egid, uint32_t sgid) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EPERM;
    if (p->euid != 0) {
        uint32_t cur_set[3] = { p->gid, p->egid, p->sgid };
        if (rgid != (uint32_t) -1 && rgid != cur_set[0] && rgid != cur_set[1] && rgid != cur_set[2])
            return -(int64_t) EPERM;
        if (egid != (uint32_t) -1 && egid != cur_set[0] && egid != cur_set[1] && egid != cur_set[2])
            return -(int64_t) EPERM;
        if (sgid != (uint32_t) -1 && sgid != cur_set[0] && sgid != cur_set[1] && sgid != cur_set[2])
            return -(int64_t) EPERM;
    }
    if (rgid != (uint32_t) -1) p->gid = rgid;
    if (egid != (uint32_t) -1) p->egid = p->fsgid = egid;
    if (sgid != (uint32_t) -1) p->sgid = sgid;
    return 0;
}
static int64_t sys_getresuid(uint32_t *ruid, uint32_t *euid, uint32_t *suid) {
    proc_t *p = cur();
    if (!p || !uptr_ok_w(ruid, 4) || !uptr_ok_w(euid, 4) || !uptr_ok_w(suid, 4))
        return -(int64_t) EFAULT;
    *ruid = p->uid;
    *euid = p->euid;
    *suid = p->suid;
    return 0;
}
static int64_t sys_getresgid(uint32_t *rgid, uint32_t *egid, uint32_t *sgid) {
    proc_t *p = cur();
    if (!p || !uptr_ok_w(rgid, 4) || !uptr_ok_w(egid, 4) || !uptr_ok_w(sgid, 4))
        return -(int64_t) EFAULT;
    *rgid = p->gid;
    *egid = p->egid;
    *sgid = p->sgid;
    return 0;
}
static int64_t sys_getpgrp(void) { return cur() ? (int64_t) cur()->pgid : 1; }
static int64_t sys_getpgid(uint64_t pid) {
    if (pid == 0) return sys_getpgrp();
    proc_t *p = proc_find((uint32_t) pid);
    if (!p || !jail_can_see(cur(), p)) { if (p) proc_unref(p); return -(int64_t) ESRCH; }
    int64_t pgid = (int64_t) p->pgid;
    proc_unref(p);
    return pgid;
}
static int64_t sys_setsid(void) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EPERM;
    p->pgid = p->pid;
    return (int64_t) p->pid;
}
static int64_t sys_setpgid(uint64_t pid, uint64_t pgid) {
    proc_t *p = pid ? proc_find((uint32_t) pid) : cur();
    if (!p) return -(int64_t) ESRCH;
    if (!jail_can_see(cur(), p)) { if (pid) proc_unref(p); return -(int64_t) ESRCH; }
    if (pgid == 0) pgid = p->pid;
    if (pgid > PROC_MAX) { if (pid) proc_unref(p); return -(int64_t) EINVAL; }
    p->pgid = (int) pgid;
    if (pid) proc_unref(p);
    return 0;
}
static bool kill_permitted(const proc_t *sender, const proc_t *target) {
    if (!sender) return true; /* kernel */
    if (sender->euid == 0) return true;
    return sender->uid == target->uid || sender->uid == target->euid ||
           sender->euid == target->uid || sender->euid == target->euid;
}

static int64_t sys_kill(int64_t pid, int sig) {
    if (sig < 0 || sig >= NSIG) return -(int64_t) EINVAL;

    proc_t *self = cur();

    /* sig == 0 is an existence/permission probe: validate the target but deliver nothing. */
    if (pid > 0) {
        proc_t *target = proc_find((uint32_t) pid);
        if (!target) return -(int64_t) ESRCH;
        if (!jail_can_see(self, target)) {
            proc_unref(target);
            return -(int64_t) ESRCH;
        }
        if (!kill_permitted(self, target)) {
            proc_unref(target);
            return -(int64_t) EPERM;
        }
        if (sig) proc_send_signal(target, sig);
        proc_unref(target);
    } else if (pid == -1) {
        for (int i = 0; i < PROC_MAX; i++) {
            if (g_proctable[i].state == PROC_UNUSED) continue;
            if (g_proctable[i].pid == 1) continue;
            if (!jail_can_see(self, &g_proctable[i])) continue;
            if (sig) proc_send_signal(&g_proctable[i], sig);
        }
    } else if (pid < -1) {
        uint32_t pgid = (uint32_t) (-pid);
        bool found = false;
        for (int i = 0; i < PROC_MAX; i++) {
            if (g_proctable[i].state == PROC_UNUSED) continue;
            if (!jail_can_see(self, &g_proctable[i])) continue;
            if (g_proctable[i].pgid == (int) pgid) {
                if (sig) proc_send_signal(&g_proctable[i], sig);
                found = true;
            }
        }
        if (!found) return -(int64_t) ESRCH;
    } else {
        for (int i = 0; i < PROC_MAX; i++) {
            if (g_proctable[i].state == PROC_UNUSED) continue;
            if (!jail_can_see(self, &g_proctable[i])) continue;
            if (self && g_proctable[i].pgid == self->pgid && sig)
                proc_send_signal(&g_proctable[i], sig);
        }
    }
    return 0;
}

static int64_t sys_rt_sigaction(int sig, const k_sigaction_t *act, k_sigaction_t *oldact,
                                uint64_t sigsetsize) {
    (void) sigsetsize;
    if (sig < 1 || sig >= NSIG) return -(int64_t) EINVAL;
    if (sig == SIGKILL || sig == SIGSTOP) return -(int64_t) EINVAL;

    proc_t *p = cur();
    if (!p) return -(int64_t) EFAULT;
    if (oldact && !uptr_ok_w(oldact, sizeof(k_sigaction_t))) return -(int64_t) EFAULT;
    if (act && !uptr_ok(act, sizeof(k_sigaction_t))) return -(int64_t) EFAULT;
    if (oldact) memcpy(oldact, &p->sig_actions[sig - 1], sizeof(k_sigaction_t));
    if (act) memcpy(&p->sig_actions[sig - 1], act, sizeof(k_sigaction_t));
    return 0;
}

static int64_t sys_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset,
                                  uint64_t sigsetsize) {
    (void) sigsetsize;
    proc_t *p = cur();
    if (!p) return -(int64_t) EFAULT;

    if (oldset) {
        if (!uptr_ok_w(oldset, 8)) return -(int64_t) EFAULT;
        *oldset = p->sig_mask;
    }
    if (!set) return 0;
    if (!uptr_ok(set, 8)) return -(int64_t) EFAULT;

    uint64_t bits = *set & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    switch (how) {
    case SIG_BLOCK:
        p->sig_mask |= bits;
        break;
    case SIG_UNBLOCK:
        p->sig_mask &= ~bits;
        break;
    case SIG_SETMASK:
        p->sig_mask = bits;
        break;
    default:
        return -(int64_t) EINVAL;
    }
    return 0;
}

static int64_t sys_rt_sigreturn(syscall_frame_t *f) {
    rt_sigframe_t *frame = (rt_sigframe_t *) (cpu_get_user_rsp() - 8);
    if (!uptr_ok(frame, sizeof(*frame))) return -(int64_t) EFAULT;
    mcontext_t *mc = &frame->uc.uc_mcontext;

    f->r8 = mc->r8;
    f->r9 = mc->r9;
    f->r10 = mc->r10;
    f->r11 = mc->eflags; /* user rflags*/
    f->r12 = mc->r12;
    f->r13 = mc->r13;
    f->r14 = mc->r14;
    f->r15 = mc->r15;
    f->rdi = mc->rdi;
    f->rsi = mc->rsi;
    f->rbp = mc->rbp;
    f->rbx = mc->rbx;
    f->rdx = mc->rdx;
    f->rcx = mc->rip; /* user rip */

    cpu_set_user_rsp(mc->rsp);

    proc_t *p = cur();
    p->sig_mask = frame->uc.uc_sigmask;
    p->sig_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    p->on_sigstack = 0; /* left the alt stack */
    return (int64_t) mc->rax;
}

static int64_t sys_pause(void) {
    proc_t *p = cur();
    if (!p) return 0;
    while (!(p->pending_sigs & ~p->sig_mask)) {
        proc_t *next = NULL;
        for (int i = 0; i < PROC_MAX; i++) {
            if (&g_proctable[i] == p) continue;
            if (g_proctable[i].state == PROC_READY) {
                next = &g_proctable[i];
                break;
            }
        }
        if (!next) {
            cpu_set_kernel_stack(p->kstack_top);
            sti();
            hlt();
            cli();
            continue;
        }
        p->state = PROC_WAITING;
        proc_clear_ready(p);
        next->state = PROC_RUNNING;
        vfs_set_fdtable(next->fds);
        g_current_space = next->space;
        cpu_set_kernel_stack(next->kstack_top);
        /* IF=0 across the switch: keep current-proc/space updates atomic. */
        sched_switch(next);
        p->state = PROC_RUNNING;
        vfs_set_fdtable(p->fds);
        g_current_space = p->space;
        cpu_set_kernel_stack(p->kstack_top);
    }
    return -(int64_t) EINTR;
}

static int64_t sys_rt_sigsuspend(const uint64_t *mask, uint64_t sigsetsize) {
    (void) sigsetsize;
    proc_t *p = cur();
    if (!p) return 0;
    uint64_t saved = p->sig_mask;
    if (mask) {
        if (!uptr_ok(mask, 8)) return -(int64_t) EFAULT;
        p->sig_mask = *mask & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    }
    while (!(p->pending_sigs & ~p->sig_mask)) {
        proc_t *next = NULL;
        for (int i = 0; i < PROC_MAX; i++) {
            if (&g_proctable[i] == p) continue;
            if (g_proctable[i].state == PROC_READY) {
                next = &g_proctable[i];
                break;
            }
        }
        if (!next) {
            cpu_set_kernel_stack(p->kstack_top);
            sti();
            hlt();
            cli();
            continue;
        }
        p->state = PROC_WAITING;
        proc_clear_ready(p);
        next->state = PROC_RUNNING;
        vfs_set_fdtable(next->fds);
        g_current_space = next->space;
        cpu_set_kernel_stack(next->kstack_top);
        /* IF=0 across the switch: keep current-proc/space updates atomic. */
        sched_switch(next);
        p->state = PROC_RUNNING;
        vfs_set_fdtable(p->fds);
        g_current_space = p->space;
        cpu_set_kernel_stack(p->kstack_top);
    }
    p->sig_mask = saved;
    return -(int64_t) EINTR;
}

static int64_t sys_mkdir(const char *path, uint32_t mode) {
    if (!path) return -(int64_t) EINVAL;
    char abs[512];
    if (!path_abs(abs, path)) return -(int64_t) EFAULT;
    return (int64_t) vfs_mkdir(abs, mode);
}

static int64_t sys_rmdir(const char *path) {
    if (!path) return -(int64_t) EINVAL;
    char abs[512];
    if (!path_abs(abs, path)) return -(int64_t) EFAULT;
    return (int64_t) vfs_rmdir(abs);
}

static int64_t sys_unlink(const char *path) {
    if (!path) return -(int64_t) EINVAL;
    char abs[512];
    if (!path_abs(abs, path)) return -(int64_t) EFAULT;
    return (int64_t) vfs_unlink(abs);
}

static int64_t sys_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -(int64_t) EINVAL;
    char abs_old[512], abs_new[512];
    if (!path_abs(abs_old, oldpath) || !path_abs(abs_new, newpath)) return -(int64_t) EFAULT;
    return (int64_t) vfs_rename(abs_old, abs_new);
}

static int64_t sys_set_tid_address(void *p) {
    if (p && !uptr_ok_w(p, sizeof(uint32_t))) return -(int64_t) EFAULT;
    proc_t *proc = cur();
    if (proc) proc->cleartid_addr = (uint32_t *) p;
    return (int64_t) sys_getpid();
}
static int64_t sys_set_robust_list(void *h, uint64_t l) {
    (void) h;
    (void) l;
    return 0;
}

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256

#define FUTEX_MAX_WAITERS PROC_MAX

typedef struct {
    uint32_t *uaddr;
    proc_t *proc;
} futex_entry_t;

static futex_entry_t g_futex_tab[FUTEX_MAX_WAITERS];
static spinlock_t g_futex_lock;

static void cleartid_wake(uint32_t *addr) {
    spin_lock(&g_futex_lock);
    for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
        if (g_futex_tab[i].uaddr == addr && g_futex_tab[i].proc) {
            proc_t *w = g_futex_tab[i].proc;
            if (__sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY))
                proc_set_ready(w);
            g_futex_tab[i].proc = NULL;
        }
    }
    spin_unlock(&g_futex_lock);
}

static int64_t sys_futex(uint32_t *uaddr, int op, uint32_t val, void *timeout, uint32_t *uaddr2,
                         uint32_t val3) {
    if (!uaddr || !uptr_ok(uaddr, sizeof(*uaddr))) return -(int64_t) EFAULT;
    int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    switch (cmd) {
    case FUTEX_WAIT: {
        if (*uaddr != val) return -(int64_t) EAGAIN;
        proc_t *p = cur();
        if (!p) return -(int64_t) EFAULT;
        spin_lock(&g_futex_lock);
        int slot = -1;
        for (int i = 0; i < FUTEX_MAX_WAITERS; i++)
            if (!g_futex_tab[i].proc) {
                slot = i;
                break;
            }
        if (slot < 0) { spin_unlock(&g_futex_lock); return -(int64_t) ENOMEM; }
        g_futex_tab[slot].uaddr = uaddr;
        g_futex_tab[slot].proc = p;
        spin_unlock(&g_futex_lock);
        uint64_t deadline = 0;
        if (timeout) {
            uint64_t ms = ((uint64_t *) timeout)[0] * 1000 + ((uint64_t *) timeout)[1] / 1000000;
            if (ms) deadline = g_ticks + ms;
        }
        p->wakeup_tick = deadline;
        if (deadline) proc_set_timer(p);
        while (g_futex_tab[slot].proc == p) {
            if (deadline && g_ticks >= deadline) break;
            if (proc_next_ready(p))
                sched_yield_blocking();
            else {
                sti();
                hlt();
                cli();
            }
        }
        bool timed_out = deadline && g_ticks >= deadline;
        p->wakeup_tick = 0;
        spin_lock(&g_futex_lock);
        if (g_futex_tab[slot].proc == p) g_futex_tab[slot].proc = NULL;
        spin_unlock(&g_futex_lock);
        return timed_out ? -(int64_t) ETIMEDOUT : 0;
    }
    case FUTEX_WAKE: {
        proc_t *self = cur();
        int woken = 0;
        spin_lock(&g_futex_lock);
        for (int i = 0; i < FUTEX_MAX_WAITERS && woken < (int) val; i++) {
            if (g_futex_tab[i].uaddr == uaddr && g_futex_tab[i].proc &&
                (!self || g_futex_tab[i].proc->jail_id == self->jail_id)) {
                proc_t *w = g_futex_tab[i].proc;
                w->wakeup_tick = 0;
                if (__sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY))
                    proc_set_ready(w);
                g_futex_tab[i].proc = NULL;
                woken++;
            }
        }
        spin_unlock(&g_futex_lock);
        return (int64_t) woken;
    }
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE: {
        if (!uaddr2 || !uptr_ok(uaddr2, sizeof(*uaddr2))) return -(int64_t) EFAULT;
        if (cmd == FUTEX_CMP_REQUEUE && *uaddr != val3) return -(int64_t) EAGAIN;
        uint32_t nr_wake = val;
        uint32_t nr_requeue = (uint32_t) (uint64_t) timeout; /* val2 rides in the timeout slot */
        proc_t *self = cur();
        int woken = 0, requeued = 0;
        spin_lock(&g_futex_lock);
        for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
            if (g_futex_tab[i].uaddr != uaddr || !g_futex_tab[i].proc) continue;
            if (self && g_futex_tab[i].proc->jail_id != self->jail_id) continue;
            if ((uint32_t) woken < nr_wake) {
                proc_t *w = g_futex_tab[i].proc;
                w->wakeup_tick = 0;
                if (__sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY))
                    proc_set_ready(w);
                g_futex_tab[i].proc = NULL;
                woken++;
            } else if ((uint32_t) requeued < nr_requeue) {
                g_futex_tab[i].uaddr = uaddr2; /* move waiter to the new futex */
                requeued++;
            }
        }
        spin_unlock(&g_futex_lock);
        return (int64_t) (woken + requeued);
    }
    default:
        return -(int64_t) ENOSYS;
    }
}

static int64_t sys_getrandom(void *buf, uint64_t len, uint32_t flags) {
    (void) flags;
    if (!buf || !len) return 0;

    chacha20_rng_bytes(&g_chacha20_rng, (uint8_t *) buf, (size_t) len);
    return (int64_t) len;
}

static int64_t sys_sigaltstack(const void *ss, void *oss) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EFAULT;

    /* userspace stack_t: void* ss_sp @0, int ss_flags @8, size_t ss_size @16 */
    if (oss) {
        if (!uptr_ok_w(oss, 24)) return -(int64_t) EFAULT;
        uint8_t *o = (uint8_t *) oss;
        *(uint64_t *) (o + 0) = p->sig_altstack_sp;
        *(int *) (o + 8) = p->on_sigstack ? SS_ONSTACK : (p->sig_altstack_size ? 0 : SS_DISABLE);
        *(uint64_t *) (o + 16) = p->sig_altstack_size;
    }
    if (ss) {
        if (!uptr_ok(ss, 24)) return -(int64_t) EFAULT;
        if (p->on_sigstack) return -(int64_t) EPERM; /* cannot change while running on it */
        const uint8_t *s = (const uint8_t *) ss;
        int flags = *(const int *) (s + 8);
        if (flags & SS_DISABLE) {
            p->sig_altstack_sp = 0;
            p->sig_altstack_size = 0;
        } else {
            p->sig_altstack_sp = *(const uint64_t *) (s + 0);
            p->sig_altstack_size = *(const uint64_t *) (s + 16);
        }
    }
    return 0;
}

static int64_t sys_tgkill(int tgid, int tid, int sig) {
    (void) tgid;
    return sys_kill((int64_t) tid, sig);
}

static int64_t sys_gettid(void) { return sys_getpid(); }

static int64_t sys_prctl(int op, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void) op;
    (void) a2;
    (void) a3;
    (void) a4;
    (void) a5;
    return 0;
}

static int64_t sys_umask(uint64_t mask) {
    proc_t *p = cur();
    if (!p) return 0022;
    uint32_t old = p->umask;
    p->umask = (uint32_t) mask & 0777U;
    return (int64_t) old;
}

static int64_t sys_ftruncate(int fd, uint64_t len) {
    vfs_file_t *file = fd_get_file(fd);
    if (!file) return -(int64_t) EBADF;
    if ((file->flags & O_ACCMODE) == O_RDONLY) return -(int64_t) EBADF;
    vfs_node_t *n = file->node;
    if (!n) return -(int64_t) EBADF;
    if (n->type != VFS_TYPE_REG) return -(int64_t) EINVAL;
    if (len < n->size) n->size = len;
    return 0;
}

static int64_t sys_truncate(const char *path, uint64_t len) {
    if (!path) return -(int64_t) EFAULT;
    char abs[512];
    if (!path_abs(abs, path)) return -(int64_t) EFAULT;
    return (int64_t) vfs_truncate(abs, len);
}

static int64_t sys_symlink(const char *target, const char *linkpath) {
    if (!target || !linkpath) return -(int64_t) EFAULT;
    char abs[512];
    if (!path_abs(abs, linkpath)) return -(int64_t) EFAULT;
    vfs_node_t *n = vfs_create_symlink(abs, target);
    return n ? 0 : -(int64_t) EEXIST;
}

static int64_t sys_statfs(const char *path, void *buf) {
    (void) path;
    if (buf) {
        if (!uptr_ok_w(buf, 120)) return -(int64_t) EFAULT;
        memset(buf, 0, 120);
        uint64_t *w = (uint64_t *) buf; /* Linux struct statfs */
        w[0] = 0x858458f6;              /* f_type = RAMFS_MAGIC */
        w[1] = 4096;                    /* f_bsize  */
        w[2] = 65536;                   /* f_blocks */
        w[3] = 65536;                   /* f_bfree  */
        w[4] = 65536;                   /* f_bavail */
        w[5] = 4096;                    /* f_files  */
        w[6] = 4096;                    /* f_ffree  */
        w[8] = 255;                     /* f_namelen */
        w[9] = 4096;                    /* f_frsize  */
    }
    return 0;
}

static int64_t sys_getgroups(int size, uint32_t *list) {
    if (size < 0) return -(int64_t) EINVAL;
    if (size > 0 && (!list || !uptr_ok_w(list, (uint64_t) size * sizeof(*list))))
        return -(int64_t) EFAULT;
    return 0;
}

static int64_t sys_setgroups(int size, const uint32_t *list) {
    if (!is_root()) return -(int64_t) EPERM;
    if (size < 0) return -(int64_t) EINVAL;
    if (size > 0 && (!list || !uptr_ok(list, (uint64_t) size * sizeof(*list))))
        return -(int64_t) EFAULT;
    return 0;
}

static int64_t sys_madvise(void *addr, uint64_t len, int advice) {
    (void) addr;
    (void) len;
    (void) advice;
    return 0;
}

static int64_t sys_access(const char *p, int m) {
    if (!p) return -(int64_t) EFAULT;
    char abs[512];
    if (!path_abs(abs, p)) return -(int64_t) EFAULT;
    return (int64_t) vfs_access(abs, m);
}

#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2

static int64_t sys_mremap(uint64_t old_addr, uint64_t old_sz, uint64_t new_sz, uint64_t flags,
                          uint64_t new_addr) {
    proc_t *p = cur();
    if (!p || !old_sz || !new_sz) return -(int64_t) EINVAL;
    if (flags & ~(uint64_t) (MREMAP_MAYMOVE | MREMAP_FIXED)) return -(int64_t) EINVAL;
    if ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE)) return -(int64_t) EINVAL;
    if ((old_addr & (PAGE_SIZE - 1)) || ((flags & MREMAP_FIXED) && (new_addr & (PAGE_SIZE - 1))))
        return -(int64_t) EINVAL;
    old_sz = PAGE_ALIGN_UP(old_sz);
    new_sz = PAGE_ALIGN_UP(new_sz);
    if (!user_map_range_ok(old_addr, old_sz) || !vma_range_ok(p->space, old_addr, old_sz))
        return -(int64_t) EINVAL;
    if (new_sz <= old_sz) {
        uint64_t tail = old_sz - new_sz;
        if (tail) {
            unmap_owned_pages(p, old_addr + new_sz, tail);
            int rc = vma_remove(p->space, old_addr + new_sz, tail);
            if (rc < 0) return rc;
        }
        return (int64_t) old_addr;
    }
    if (!(flags & MREMAP_MAYMOVE)) return -(int64_t) ENOMEM;
    uint64_t new_va;
    if (flags & MREMAP_FIXED) {
        new_va = new_addr;
        if (!user_map_range_ok(new_va, new_sz) || vma_conflicts(p->space, new_va, new_sz))
            return -(int64_t) EINVAL;
    } else {
        int pick = mmap_pick_addr(p, 0, new_sz, 0, &new_va);
        if (pick < 0) return pick;
    }

    int rc = vma_add(p->space, new_va, new_sz, PROT_READ | PROT_WRITE, MAP_ANON, true);
    if (rc < 0) return rc;

    for (uint64_t o = 0; o < new_sz; o += PAGE_SIZE) {
        void *ph = pmm_alloc_zeroed();
        if (!ph) {
            rollback_new_mapping(p, new_va, o, new_sz);
            return -(int64_t) ENOMEM;
        }
        if (vmm_map(p->space, new_va + o, (uint64_t) ph, VMM_UDATA) < 0) {
            pmm_free(ph);
            rollback_new_mapping(p, new_va, o, new_sz);
            return -(int64_t) ENOMEM;
        }
        if (o < old_sz) {
            uint64_t src = vmm_virt_to_phys(p->space, old_addr + o);
            if (src) memcpy(phys_to_virt((uint64_t) ph), phys_to_virt(src), PAGE_SIZE);
        }
    }
    unmap_owned_pages(p, old_addr, old_sz);
    rc = vma_remove(p->space, old_addr, old_sz);
    if (rc < 0) return rc;
    return (int64_t) new_va;
}
static int64_t sys_rt_sigtimedwait(const uint64_t *set, void *info, const void *timeout,
                                   uint64_t sigsetsize) {
    (void) sigsetsize;
    (void) info;
    proc_t *p = cur();
    if (!p || !set) return -(int64_t) EFAULT;
    uint64_t mask = *set;
    uint64_t deadline = (uint64_t) -1ULL;
    if (timeout) {
        uint64_t ms =
            ((const uint64_t *) timeout)[0] * 1000 + ((const uint64_t *) timeout)[1] / 1000000;
        if (ms) deadline = g_ticks + ms;
    }
    while (!(p->pending_sigs & mask)) {
        if (deadline != (uint64_t) -1ULL && g_ticks >= deadline) return -(int64_t) ETIMEDOUT;
        if (proc_next_ready(p))
            sched_yield_blocking();
        else {
            sti();
            hlt();
            cli();
        }
    }
    uint64_t bit = p->pending_sigs & mask;
    int sig = 1;
    while (!((bit >> (sig - 1)) & 1)) sig++;
    p->pending_sigs &= ~(1ULL << (sig - 1));
    return (int64_t) sig;
}

static int64_t sys_fchdir(int fd) {
    vfs_node_t *n = fd_get_node(fd);
    if (!n) return -(int64_t) EBADF;
    if (n->type != VFS_TYPE_DIR) return -(int64_t) ENOTDIR;
    proc_t *p = cur();
    if (!p) return 0;
    char buf[512];
    if (vfs_node_abspath(n, buf, sizeof(buf))) strncpy(p->cwd, buf, sizeof(p->cwd) - 1);
    return 0;
}

static int64_t sys_link(const char *old, const char *lnew) {
    if (!old || !lnew) return -(int64_t) EFAULT;
    char abs_old[512], abs_new[512];
    if (!path_abs(abs_old, old) || !path_abs(abs_new, lnew)) return -(int64_t) EFAULT;
    return (int64_t) vfs_link(abs_old, abs_new);
}

static int64_t sys_jail_create(const kjail_conf_t *ucfg) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EFAULT;
    if (!jail_host_priv(p)) return -(int64_t) EPERM; /* jail creation is privileged */
    if (!ucfg || !uptr_ok(ucfg, sizeof(kjail_conf_t))) return -(int64_t) EFAULT;
    kjail_conf_t cfg;
    memcpy(&cfg, ucfg, sizeof(cfg));
    cfg.name[JAIL_NAME_MAX - 1] = '\0';
    cfg.root[JAIL_ROOT_MAX - 1] = '\0';
    int jid = jail_create(p->jail_id, &cfg, p->euid);
    if (jid < 0) return jid;
    if (cfg.attach) jail_enter(p, (uint32_t) jid);
    return jid;
}

static int64_t sys_jail_attach(uint32_t jid) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EFAULT;
    jail_t *j = jail_find(jid);
    if (!j || j->state != JAIL_ACTIVE) return -(int64_t) ESRCH;
    if (!jail_is_descendant(p->jail_id, jid)) return -(int64_t) EPERM; /* no escape upward */
    if (!jail_can_fork(jid)) return -(int64_t) EAGAIN;
    jail_enter(p, jid);
    return 0;
}

static int64_t sys_jail_get(uint32_t jid, kjail_info_t *uout) {
    proc_t *p = cur();
    if (!uout || !uptr_ok_w(uout, sizeof(kjail_info_t))) return -(int64_t) EFAULT;
    jail_t *j = jail_find(jid);
    if (!j) return -(int64_t) ESRCH;
    if (p && !jail_is_descendant(p->jail_id, jid)) return -(int64_t) EPERM;
    kjail_info_t info;
    memset(&info, 0, sizeof(info));
    info.id = j->id;
    info.parent_id = j->parent_id;
    info.flags = j->flags;
    info.nprocs = j->nprocs;
    info.max_procs = j->max_procs;
    info.creator_uid = j->creator_uid;
    memcpy(info.name, j->name, JAIL_NAME_MAX);
    memcpy(info.root, j->root, JAIL_ROOT_MAX);
    memcpy(uout, &info, sizeof(info));
    return 0;
}

static int64_t sys_jail_list(uint32_t *uids, int max) {
    proc_t *p = cur();
    if (max < 0) return -(int64_t) EINVAL;
    if (max > 0 && (!uids || !uptr_ok_w(uids, (uint64_t) max * sizeof(uint32_t))))
        return -(int64_t) EFAULT;
    int n = 0;
    for (int i = 0; i < JAIL_MAX; i++) {
        if (g_jails[i].state == JAIL_UNUSED) continue;
        if (p && !jail_is_descendant(p->jail_id, g_jails[i].id)) continue;
        if (n < max) uids[n] = g_jails[i].id;
        n++;
    }
    return n;
}

static int64_t sys_jail_remove(uint32_t jid) { return jail_remove(jid, cur()); }

static int64_t sys_jail_self(void) {
    proc_t *p = cur();
    return p ? (int64_t) p->jail_id : 0;
}

static int64_t sys_jail_set_auto(int on) {
    if (!host_priv()) return -(int64_t) EPERM;
    g_jail_auto_isolate = on ? 1 : 0;
    return 0;
}

void syscall_dispatch(syscall_frame_t *f) {
    uint64_t nr = f->rax;
    uint64_t a1 = f->rdi, a2 = f->rsi, a3 = f->rdx;
    uint64_t a4 = f->r10, a5 = f->r8, a6 = f->r9;

    proc_t *tp = cur();
    if (tp && tp->ptrace_syscall_trace && nr != 101) {
        tp->ptrace_in_syscall = 1;
        proc_ptrace_stop(tp, SIGTRAP | 0x80, 1, f, &f->r11);
    }

    int64_t ret;
    switch (nr) {
    case 0:
        ret = fd_read((int) a1, (void *) a2, a3);
        break;
    case 1:
        ret = fd_write((int) a1, (const void *) a2, a3);
        break;
    case 2:
        ret = fd_open((const char *) a1, (int) a2, (int) a3);
        break;
    case 3:
        ret = fd_close((int) a1);
        break;
    case 4:
        ret = fd_stat((const char *) a1, (struct linux_stat *) a2);
        break;
    case 5:
        ret = fd_fstat((int) a1, (struct linux_stat *) a2);
        break;
    case 6:
        ret = fd_lstat((const char *) a1, (struct linux_stat *) a2);
        break;
    case 7:
        ret = sys_poll((struct pollfd_s *) a1, a2, (int) a3);
        break;
    case 8:
        ret = fd_lseek((int) a1, (int64_t) a2, (int) a3);
        break;
    case 9:
        ret = sys_mmap(a1, a2, a3, a4, a5, a6);
        break;
    case 10:
        ret = sys_mprotect(a1, a2, a3);
        break;
    case 11:
        ret = sys_munmap(a1, a2);
        break;
    case 12:
        ret = sys_brk(a1);
        break;
    case 25:
        ret = sys_mremap(a1, a2, a3, a4, a5);
        break;
    case 26:
        ret = 0; /* noop ramfs*/
        break;
    case 27:
        if (a3) {
            uint64_t vec_len = (a2 + PAGE_SIZE - 1) / PAGE_SIZE;
            if (!uptr_ok_w((void *) a3, vec_len)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            memset((void *) a3, 1, vec_len);
        }
        ret = 0;
        break;
    case 13:
        ret = sys_rt_sigaction((int) a1, (const k_sigaction_t *) a2, (k_sigaction_t *) a3, a4);
        break;
    case 14:
        ret = sys_rt_sigprocmask((int) a1, (const uint64_t *) a2, (uint64_t *) a3, a4);
        break;
    case 15:
        ret = sys_rt_sigreturn(f);
        break;
    case 16:
        ret = fd_ioctl((int) a1, a2, a3);
        break;
    case 17:
        ret = fd_pread((int) a1, (void *) a2, a3, a4);
        break;
    case 18:
        ret = fd_pwrite((int) a1, (const void *) a2, a3, a4);
        break;
    case 19:
        ret = sys_readv((int) a1, (const struct iovec *) a2, (int) a3);
        break;
    case 20:
        ret = sys_writev((int) a1, (const void *) a2, (int) a3);
        break;
    case 21:
        ret = sys_access((const char *) a1, (int) a2);
        break;
    case 22:
        if (!a1 || !uptr_ok_w((void *) a1, 2 * sizeof(int))) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = fd_pipe((int *) a1);
        break;
    case 24: {
        ret = 0;
        break;
    }
    case 53:
        if (!a4 || !uptr_ok_w((void *) a4, 2 * sizeof(int))) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = fd_socketpair((int *) a4);
        break;
    case 23:
        ret = sys_select((int) a1, (void *) a2, (void *) a3, (void *) a4, (void *) a5);
        break;
    case 28:
        ret = sys_madvise((void *) a1, a2, (int) a3);
        break;
    case 29:
        ret = (int64_t) sys_shmget((int) a1, a2, (int) a3);
        break;
    case 30:
        ret = (int64_t) sys_shmat((int) a1, a2, (int) a3);
        break;
    case 31:
        ret = (int64_t) sys_shmctl((int) a1, (int) a2, (void *) a3);
        break;
    case 40:
        ret = sys_sendfile((int) a1, (int) a2, (uint64_t *) a3, a4);
        break;
    case 41: /* socket(domain, type, proto) */
        ret = (int64_t) fd_socket((int) a1, (int) a2, (int) a3);
        break;
    case 42: /* connect(fd, addr, addrlen) */
        ret = sys_socket_connect((int) a1, (struct sockaddr_un *) a2, a3);
        break;
    case 43: /* accept(fd, addr, addrlen) */
        ret = sys_socket_accept((int) a1, (struct sockaddr_un *) a2, (int *) a3, 0);
        break;
    case 44: { /* sendto(fd, buf, len, flags, addr, addrlen) */
        vfs_file_t *sf = fd_get_file((int) a1);
        if (sf && sf->inet && a5) {
            if (!uptr_ok((void *) a5, 16)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            ret = inet_sendto(sf->inet, (const void *) a2, a3, (const struct sockaddr_in *) a5);
        } else if (sf && sf->inet) {
            ret = inet_fd_write(sf->inet, (const void *) a2, a3);
        } else {
            ret = fd_write((int) a1, (const void *) a2, a3);
        }
        break;
    }
    case 45: { /* recvfrom(fd, buf, len, flags, addr, addrlen_ptr) */
        vfs_file_t *sf = fd_get_file((int) a1);
        if (sf && sf->inet) {
            struct sockaddr_in *sa = a5 ? (struct sockaddr_in *) a5 : NULL;
            if (sa && !uptr_ok_w(sa, sizeof(*sa))) {
                ret = -(int64_t) EFAULT;
                break;
            }
            int rflags = sf->flags | ((a4 & 0x40) ? O_NONBLOCK : 0); /* MSG_DONTWAIT */
            ret = inet_recvfrom(sf->inet, (void *) a2, a3, sa, rflags);
            if (ret >= 0 && sa && a6 && uptr_ok_w((void *) a6, sizeof(int)))
                *(int *) (uintptr_t) a6 = (int) sizeof(*sa);
        } else {
            ret = fd_read((int) a1, (void *) a2, a3);
        }
        break;
    }
    case 46: /* sendmsg(fd, msghdr, flags) */
        ret = sys_socket_sendmsg((int) a1, (const void *) a2, (int) a3);
        break;
    case 47: /* recvmsg(fd, msghdr, flags) */
        ret = sys_socket_recvmsg((int) a1, (void *) a2, (int) a3);
        break;
    case 48: { /* shutdown(fd, how) */
        vfs_file_t *sf = fd_get_file((int) a1);
        if (sf && sf->inet) {
            inet_conn_close(sf->inet);
            sf->inet = NULL;
        }
        ret = 0;
        break;
    }
    case 49: /* bind(fd, addr, addrlen) */
        ret = sys_socket_bind((int) a1, (struct sockaddr_un *) a2, a3);
        break;
    case 50: { /* listen(fd, backlog) */
        vfs_file_t *lf = fd_get_file((int) a1);
        if (lf && lf->inet)
            ret = inet_listen(lf->inet, (int) a2);
        else
            ret = (int64_t) fd_listen_unix((int) a1, (int) a2);
        break;
    }
    case 51: /* getsockname(fd, addr, addrlen) */
        ret = sys_socket_getsockname((int) a1, (struct sockaddr_un *) a2, (int *) a3);
        break;
    case 52: /* getpeername(fd, addr, addrlen) */
        ret = sys_socket_getpeername((int) a1, (struct sockaddr_un *) a2, (int *) a3);
        break;
    case 54:
        ret = sys_socket_setsockopt((int) a1, (int) a2, (int) a3, (void *) a4, (int) a5);
        break;
    case 55: /* getsockopt */
        ret = sys_socket_getsockopt((int) a1, (int) a2, (int) a3, (void *) a4, (int *) a5);
        break;
    case 32:
        ret = fd_dup((int) a1);
        break;
    case 33:
        ret = fd_dup2((int) a1, (int) a2);
        break;
    case 34:
        ret = sys_pause();
        break;
    case 35:
        ret = sys_nanosleep((void *) a1, (void *) a2);
        break;
    case 36:
        ret = sys_getitimer((int) a1, (void *) a2);
        break;
    case 37:
        ret = sys_alarm((uint32_t) a1);
        break;
    case 38:
        ret = sys_setitimer((int) a1, (const void *) a2, (void *) a3);
        break;
    case 39:
        ret = sys_getpid();
        break;
    case 56:
        ret = sys_clone(a1, a2, (uint32_t *) a3, (uint32_t *) a4, a5, f);
        break;
    case 57:
        ret = sys_fork(f);
        break;
    case 58:
        ret = sys_fork(f); /* vfork: treat as fork */
        break;
    case 59:
        ret = sys_execve((const char *) a1, (const char **) a2, (const char **) a3);
        break;
    case 60:
        proc_do_exit((int) a1);
        return;
    case 61:
        ret = sys_wait4((int) a1, (int *) a2, (int) a3, (void *) a4);
        break;
    case 62:
        ret = sys_kill(a1, (int) a2);
        break;
    case 63:
        ret = sys_uname((struct utsname *) a1);
        break;
    case 64:
    case 65:
    case 66:
    case 68:
    case 69:
    case 70:
    case 71:
        ret = -(int64_t) ENOSYS;
        break;
    case 67:
        ret = (int64_t) sys_shmdt(a1);
        break;
    case 72:
        ret = fd_fcntl((int) a1, (int) a2, a3);
        break;
    case 73:
        ret = fd_valid((int) a1) ? 0 : -(int64_t) EBADF;
        break;
    case 74:
    case 75:
        ret = fd_valid((int) a1) ? 0 : -(int64_t) EBADF;
        break;
    case 76:
        ret = sys_truncate((const char *) a1, a2);
        break;
    case 77:
        ret = sys_ftruncate((int) a1, a2);
        break;
    case 78:
        ret = fd_getdents64((int) a1, (void *) a2, a3);
        break;
    case 79:
        ret = sys_getcwd((char *) a1, a2);
        break;
    case 80:
        ret = sys_chdir((const char *) a1);
        break;
    case 81:
        ret = sys_fchdir((int) a1);
        break;
    case 82:
        ret = sys_rename((const char *) a1, (const char *) a2);
        break;
    case 83:
        ret = sys_mkdir((const char *) a1, (uint32_t) a2);
        break;
    case 84:
        ret = sys_rmdir((const char *) a1);
        break;
    case 85:
        ret = fd_open((const char *) a1, O_WRONLY | O_CREAT | O_TRUNC, (int) a2);
        break;
    case 86:
        ret = sys_link((const char *) a1, (const char *) a2);
        break;
    case 87:
        ret = sys_unlink((const char *) a1);
        break;
    case 88:
        ret = sys_symlink((const char *) a1, (const char *) a2);
        break;
    case 89:
        ret = fd_readlink((const char *) a1, (char *) a2, a3);
        break;
    case 90: {
        char abs[512];
        if (!path_abs(abs, (const char *) a1)) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = (int64_t) vfs_chmod(abs, (uint32_t) a2);
        break;
    }
    case 91:
        ret = (int64_t) vfs_fchmod((int) a1, (uint32_t) a2);
        break;
    case 92: {
        char abs[512];
        if (!path_abs(abs, (const char *) a1)) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = (int64_t) vfs_chown(abs, (uint32_t) a2, (uint32_t) a3);
        break;
    }
    case 93:
        ret = (int64_t) vfs_fchown((int) a1, (uint32_t) a2, (uint32_t) a3);
        break;
    case 94: {
        char abs[512];
        if (!path_abs(abs, (const char *) a1)) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = (int64_t) vfs_lchown(abs, (uint32_t) a2, (uint32_t) a3);
        break;
    }
    case 95:
        ret = sys_umask(a1);
        break;
    case 96:
        ret = sys_gettimeofday((void *) a1, (void *) a2);
        break;
    case 97:
        ret = sys_getrlimit(a1, (void *) a2);
        break;
    case 98:
        if (a2) {
            if (!uptr_ok_w((void *) a2, 144)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            memset((void *) a2, 0, 144);
        }
        ret = 0;
        break;
    case 99:
        ret = sys_sysinfo((struct sysinfo_s *) a1);
        break;
    case 100:
        ret = sys_times((void *) a1);
        break;
    case 101:
        ret = sys_ptrace((int64_t) a1, (int64_t) a2, a3, a4);
        break;
    case 103:
        ret = -(int64_t) EPERM; /* syslog */
        break;
    case 102:
        ret = sys_getuid();
        break;
    case 104:
        ret = sys_getgid();
        break;
    case 105:
        ret = sys_setuid((uint32_t) a1);
        break;
    case 106:
        ret = sys_setgid((uint32_t) a1);
        break;
    case 107:
        ret = sys_geteuid();
        break;
    case 108:
        ret = sys_getegid();
        break;
    case 109:
        ret = sys_setpgid(a1, a2);
        break;
    case 110:
        ret = sys_getppid();
        break;
    case 111:
        ret = sys_getpgrp();
        break;
    case 112:
        ret = sys_setsid();
        break;
    case 113:
        ret = sys_setreuid((uint32_t) a1, (uint32_t) a2);
        break;
    case 114:
        ret = sys_setregid((uint32_t) a1, (uint32_t) a2);
        break;
    case 115:
        ret = sys_getgroups((int) a1, (uint32_t *) a2);
        break;
    case 116:
        ret = sys_setgroups((int) a1, (uint32_t *) a2);
        break;
    case 117:
        ret = sys_setresuid((uint32_t) a1, (uint32_t) a2, (uint32_t) a3);
        break;
    case 118:
        ret = sys_getresuid((uint32_t *) a1, (uint32_t *) a2, (uint32_t *) a3);
        break;
    case 119:
        ret = sys_setresgid((uint32_t) a1, (uint32_t) a2, (uint32_t) a3);
        break;
    case 120:
        ret = sys_getresgid((uint32_t *) a1, (uint32_t *) a2, (uint32_t *) a3);
        break;
    case 121:
        ret = sys_getpgid(a1);
        break;
    case 122:
        ret = sys_setfsuid((uint32_t) a1);
        break;
    case 123:
        ret = sys_setfsgid((uint32_t) a1);
        break;
    case 124: {
        proc_t *_p = a1 ? proc_find((uint32_t) a1) : cur();
        ret = _p ? (int64_t) _p->pgid : -(int64_t) ESRCH;
        if (a1 && _p) proc_unref(_p);
        break;
    }
    case 125:
        if (a2) {
            if (!uptr_ok_w((void *) a2, 40)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            memset((void *) a2, 0, 40);
        }
        ret = 0;
        break; /* capget: no caps */
    case 126:
        ret = -(int64_t) EPERM;
        break; /* capset */
    case 127: {
        if (a1) {
            if (!uptr_ok_w((void *) a1, 8)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            *(uint64_t *) a1 = cur() ? (cur()->pending_sigs & cur()->sig_mask) : 0;
        }
        ret = 0;
        break;
    }
    case 128:
        ret = sys_rt_sigtimedwait((const uint64_t *) a1, (void *) a2, (const void *) a3, a4);
        break;
    case 129:
        ret = 0;
        break; /* rt_sigqueueinfo */
    case 132:
        ret = 0;
        break; /* utime: no-op */
    case 130:
        ret = sys_rt_sigsuspend((const uint64_t *) a1, a2);
        break;
    case 131:
        ret = sys_sigaltstack((const void *) a1, (void *) a2);
        break;
    case 133: {
        char abs[512];
        if (!a1 || !path_abs(abs, (const char *) a1)) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = (int64_t) vfs_mknod(abs, (uint32_t) a2, a3);
        break;
    }
    case 135:
        ret = 0;
        break; /* personality */
    case 139:
        ret = -(int64_t) ENOSYS;
        break; /* sysfs */
    case 140:
        ret = 0;
        break; /* getpriority */
    case 141:
        ret = is_root() ? 0 : -(int64_t) EPERM;
        break; /* setpriority */
    case 142:
        ret = is_root() ? 0 : -(int64_t) EPERM;
        break; /* sched_setparam */
    case 143:
        if (a2) {
            if (!uptr_ok_w((void *) a2, 4)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            ((int *) a2)[0] = 0;
        }
        ret = 0;
        break; /* sched_getparam */
    case 144:
        ret = is_root() ? 0 : -(int64_t) EPERM;
        break; /* sched_setscheduler */
    case 145:
        ret = 0;
        break; /* sched_getscheduler: SCHED_OTHER */
    case 146:
        ret = 0;
        break; /* sched_get_priority_max */
    case 147:
        ret = 0;
        break; /* sched_get_priority_min */
    case 148:
        if (a2) {
            if (!uptr_ok_w((void *) a2, 16)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            ((uint64_t *) a2)[0] = 0;
            ((uint64_t *) a2)[1] = 10000000ULL;
        }
        ret = 0;
        break; /* sched_rr_get_interval */
    case 149:
    case 150:
    case 151:
    case 152:
        ret = -(int64_t) ENOSYS;
        break; /* mlock/munlock */
    case 153:
        ret = host_priv() ? 0 : -(int64_t) EPERM;
        break; /* vhangup */
    case 137:
        ret = sys_statfs((const char *) a1, (void *) a2);
        break;
    case 138:
        ret = sys_statfs(NULL, (void *) a2);
        break;
    case 157:
        ret = sys_prctl((int) a1, a2, a3, a4, a5);
        break;
    case 158:
        ret = sys_arch_prctl((int) a1, a2);
        break;
    case 159:
        ret = host_priv() ? 0 : -(int64_t) EPERM;
        break; /* adjtimex */
    case 160:
        ret = sys_getrlimit(a1, (void *) a2); /* setrlimit: accept silently */
        break;
    case 161:
        ret = is_root() ? 0 : -(int64_t) EPERM;
        break; /* chroot: no-op until real roots exist */
    case 162:
        vfs_sync_all();
        ret = 0;
        break;
    case 169:
        if (!host_priv()) {
            ret = -(int64_t) EPERM;
            break;
        }
        vfs_sync_all();
        outw(0x604, 0x2000);
        for (;;) hlt();
        break;
    case 164:
        ret = host_priv() ? 0 : -(int64_t) EPERM;
        break; /* settimeofday */
    case 170:
        ret = host_priv() ? 0 : -(int64_t) EPERM; /* sethostname */
        break;
    case 171:
        ret = host_priv() ? 0 : -(int64_t) EPERM;
        break;  /* setdomainname */
    case 172: { /* iopl(level) - set io privilege level in RFLAGS */
        if (!host_priv()) {
            ret = -(int64_t) EPERM;
            break;
        }
        int level = (int) a1 & 3;
        f->r11 = (f->r11 & ~0x3000ULL) | ((uint64_t) level << 12);
        ret = 0;
        break;
    }
    case 173: { /* ioperm(from, count, turn_on) - we just grant iopl=3 for simplicity hahaha */
        if (!host_priv()) {
            ret = -(int64_t) EPERM;
            break;
        }
        (void) a1;
        (void) a2;
        if (a3) f->r11 |= 0x3000ULL; /* IOPL=3: allow all ports */
        ret = 0;
        break;
    }
    case 175:
    case 176:
        ret = -(int64_t) ENOSYS;
        break; /* init/delete_module */
    case 188:
    case 189:
    case 190:
    case 191:
    case 192:
    case 193:
    case 194:
    case 195:
    case 196:
    case 197:
    case 198:
    case 199:
        ret = -(int64_t) ENOTSUP; /* xattr: not supported */
        break;
    case 186:
        ret = sys_gettid();
        break;
    case 201: {
        uint64_t t = g_epoch_base + g_ticks / 1000;
        if (a1) {
            if (!uptr_ok_w((void *) a1, 8)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            *(uint64_t *) a1 = t;
        }
        ret = (int64_t) t;
        break;
    }
    case 203:
        ret = is_root() ? 0 : -(int64_t) EPERM;
        break; /* sched_setaffinity */
    case 204: {
        uint64_t sz = a2;
        if (a3 && sz > 0) {
            if (!uptr_ok_w((void *) a3, sz)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            memset((void *) a3, 0, sz);
            for (uint32_t i = 0; i < g_cpu_count && i < sz * 8; i++)
                ((uint8_t *) a3)[i / 8] |= (uint8_t) (1 << (i % 8));
        }
        ret = 0;
        break;
    } /* sched_getaffinity */
    case 213:
        ret = sys_epoll_create1(0);
        break; /* epoll_create */
    case 221:
        ret = sys_epoll_wait((int) a1, (struct epoll_event *) a2, (int) a3, -1);
        break; /* epoll_wait (old) */
    case 222:
    case 224:
    case 226:
        ret = 0;
        break; /* timer_create/gettime/delete stubs */
    case 223:
        ret = 0;
        break; /* timer_settime */
    case 225:
        ret = 0;
        break; /* timer_getoverrun */
    case 227:
        ret = host_priv() ? 0 : -(int64_t) EPERM;
        break; /* clock_settime */
    case 232:
        ret = sys_epoll_wait((int) a1, (struct epoll_event *) a2, (int) a3, (int) a4);
        break;
    case 233:
        ret = sys_epoll_ctl((int) a1, (int) a2, (int) a3, (struct epoll_event *) a4);
        break;
    case 200:
        ret = sys_kill((int64_t) a1, (int) a2);
        break; /* tkill */
    case 202:
        ret = sys_futex((uint32_t *) a1, (int) a2, (uint32_t) a3, (void *) a4, (uint32_t *) a5,
                        (uint32_t) a6);
        break;
    case 217:
        ret = fd_getdents64((int) a1, (void *) a2, a3);
        break;
    case 218:
        ret = sys_set_tid_address((void *) a1);
        break;
    case 228:
        ret = sys_clock_gettime(a1, (void *) a2);
        break;
    case 229:
        ret = sys_clock_getres(a1, (void *) a2);
        break;
    case 230:
        ret = sys_clock_nanosleep((int) a1, (int) a2, (const void *) a3, (void *) a4);
        break;
    case 231:
        proc_do_exit((int) a1);
        return;
    case 234:
        ret = sys_tgkill((int) a1, (int) a2, (int) a3);
        break;
    case 235:
        ret = 0; /* utimes */
        break;
    case 236:
        ret = -(int64_t) ENOSYS;
        break; /* vserver */
    case 240:
    case 241:
    case 242:
    case 243:
    case 244:
    case 245:
        ret = -(int64_t) ENOSYS;
        break; /* mqueue */
    case 251:
        ret = is_root() ? 0 : -(int64_t) EPERM;
        break; /* ioprio_set */
    case 252:
        ret = 0;
        break; /* ioprio_get */
    case 253:
    case 254:
    case 255:
        ret = -(int64_t) ENOSYS;
        break; /* inotify */
    case 247:  /* waitid */
        ret = sys_wait4((int) a2, NULL, (int) a4, NULL);
        break;
    case 257:
        ret = fd_openat((int) a1, (const char *) a2, (int) a3, (int) a4);
        break;
    case 258: {
        char abs[512];
        at_resolve((int) a1, (const char *) a2, abs, sizeof(abs));
        ret = (int64_t) vfs_mkdir(abs, (uint32_t) a3);
        break;
    }
    case 260: {
        char abs[512];
        at_resolve((int) a1, (const char *) a2, abs, sizeof(abs));
        ret = (int64_t) vfs_mknod(abs, (uint32_t) a3, a4);
        break;
    } /* mknodat */
    case 261: {
        char abs[512];
        at_resolve((int) a1, (const char *) a2, abs, sizeof(abs));
        int _r = (int) a5 & AT_SYMLINK_NOFOLLOW ?
                     (int) vfs_lchown(abs, (uint32_t) a3, (uint32_t) a4) :
                     (int) vfs_chown(abs, (uint32_t) a3, (uint32_t) a4);
        ret = _r;
        break;
    } /* fchownat */
    case 262:
        ret = fd_fstatat((int) a1, (const char *) a2, (struct linux_stat *) a3, (int) a4);
        break;
    case 263: {
        char abs[512];
        at_resolve((int) a1, (const char *) a2, abs, sizeof(abs));
        ret = ((int) a3 & 0x200) ? (int64_t) vfs_rmdir(abs) : (int64_t) vfs_unlink(abs);
        break;
    }
    case 264: {
        char ao[512], an[512];
        at_resolve((int) a1, (const char *) a2, ao, sizeof(ao));
        at_resolve((int) a3, (const char *) a4, an, sizeof(an));
        ret = (int64_t) vfs_rename(ao, an);
        break;
    }
    case 265: {
        char ao[512], an[512];
        at_resolve((int) a1, (const char *) a2, ao, sizeof(ao));
        at_resolve((int) a3, (const char *) a4, an, sizeof(an));
        ret = (int64_t) vfs_link(ao, an);
        break;
    }
    case 266: {
        char abs[512];
        at_resolve((int) a2, (const char *) a3, abs, sizeof(abs));
        ret = vfs_create_symlink(abs, (const char *) a1) ? 0 : -(int64_t) EEXIST;
        break;
    }
    case 267: {
        char abs[512];
        at_resolve((int) a1, (const char *) a2, abs, sizeof(abs));
        ret = (int64_t) fd_readlink(abs, (char *) a3, a4);
        break;
    }
    case 268: {
        char abs[512];
        at_resolve((int) a1, (const char *) a2, abs, sizeof(abs));
        ret = (int64_t) vfs_chmod(abs, (uint32_t) a3);
        break;
    }
    case 269: {
        char abs[512];
        at_resolve((int) a1, (const char *) a2, abs, sizeof(abs));
        ret = (int64_t) vfs_access(abs, (int) a3);
        break;
    } /* faccessat */
    case 270:
        ret =
            sys_pselect6((int) a1, (void *) a2, (void *) a3, (void *) a4, (void *) a5, (void *) a6);
        break;
    case 271:
        ret = sys_ppoll((struct pollfd_s *) a1, a2, (void *) a3, (const void *) a4, a5);
        break;
    case 272:
        ret = -(int64_t) ENOSYS;
        break; /* unshare */
    case 273:
        ret = sys_set_robust_list((void *) a1, a2);
        break;
    case 274:
    case 275:
    case 276:
    case 277:
        ret = -(int64_t) ENOSYS;
        break; /* splice/tee/sync_file_range/vmsplice */
    case 278:
        ret = -(int64_t) ENOSYS;
        break; /* move_pages */
    case 279:
        ret = 0; /* utimensat */
        break;
    case 280:
        ret = fd_openat((int) a1, (const char *) a2, (int) a3, (int) a4);
        break; /* openat2 */
    case 281:
        ret = sys_epoll_wait((int) a1, (struct epoll_event *) a2, (int) a3, (int) a4);
        break; /* epoll_pwait */
    case 282:
        ret = -(int64_t) ENOSYS;
        break; /* signalfd: not implemented */
    case 283:
        ret = fd_timerfd_create((int) a1, (int) a2);
        break; /* timerfd_create */
    case 284:
        ret = fd_eventfd((uint32_t) a1, (int) a2);
        break; /* eventfd */
    case 285:
        ret = 0;
        break; /* fallocate: no-op */
    case 286:  /* timerfd_settime(fd, flags, new, old) */
        if (!a3 || !uptr_ok((void *) a3, 32)) {
            ret = -(int64_t) EFAULT;
            break;
        }
        if (a4 && !uptr_ok_w((void *) a4, 32)) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = fd_timerfd_settime((int) a1, (int) a2, (const kitimerspec_t *) a3,
                                 (kitimerspec_t *) a4);
        break;
    case 287: /* timerfd_gettime(fd, curr) */
        if (!a2 || !uptr_ok_w((void *) a2, 32)) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = fd_timerfd_gettime((int) a1, (kitimerspec_t *) a2);
        break;
    case 288: /* accept4(fd, addr, addrlen, flags) */
        ret = sys_socket_accept((int) a1, (struct sockaddr_un *) a2, (int *) a3, (int) a4);
        break;
    case 289:
        ret = -(int64_t) ENOSYS;
        break; /* signalfd4: not implemented */
    case 290:
        ret = fd_eventfd((uint32_t) a1, (int) a2);
        break; /* eventfd2 */
    case 291:
        ret = sys_epoll_create1((int) a1);
        break; /* epoll_create1 */
    case 292:
        ret = fd_dup3((int) a1, (int) a2, (int) a3);
        break;
    case 293: /* pipe2(fds, flags) */
        if (!a1 || !uptr_ok_w((void *) a1, 2 * sizeof(int))) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = fd_pipe((int *) a1);
        if (ret == 0 && (a2 & (O_CLOEXEC | O_NONBLOCK))) {
            int *pf = (int *) a1;
            for (int _e = 0; _e < 2; _e++) {
                vfs_file_t *pe = fd_get_file(pf[_e]);
                if (!pe) continue;
                if (a2 & O_CLOEXEC) pe->cloexec = 1;
                if (a2 & O_NONBLOCK) pe->flags |= O_NONBLOCK;
            }
        }
        break;
    case 294:
        ret = -(int64_t) ENOSYS;
        break; /* inotify_init1 */
    case 295:
        ret = sys_preadv((int) a1, (const struct iovec *) a2, (int) a3, a4);
        break; /* preadv */
    case 296:
        ret = sys_pwritev((int) a1, (const struct iovec *) a2, (int) a3, a4);
        break; /* pwritev */
    case 300:
        ret = host_priv() ? 0 : -(int64_t) EPERM;
        break; /* clock_adjtime */
    case 301:
        ret = 0;
        break; /* syncfs: no-op */
    case 302:
        ret = sys_prlimit64(a1, a2, (void *) a3, (void *) a4);
        break;
    case 303:
        ret = -(int64_t) ENOSYS;
        break; /* finit_module */
    case 304:
        ret = is_root() ? 0 : -(int64_t) EPERM;
        break; /* sched_setattr */
    case 305:
        if (a2) {
            if (!uptr_ok_w((void *) a2, 56)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            memset((void *) a2, 0, 56);
        }
        ret = 0;
        break; /* sched_getattr */
    case 306: {
        char ao[512], an[512];
        at_resolve((int) a1, (const char *) a2, ao, sizeof(ao));
        at_resolve((int) a3, (const char *) a4, an, sizeof(an));
        ret = (int64_t) vfs_rename(ao, an);
        break;
    } /* renameat2 */
    case 307:
        ret = -(int64_t) ENOSYS;
        break; /* seccomp not implemented */
    case 318:
        ret = sys_getrandom((void *) a1, a2, (uint32_t) a3);
        break;
    case 319:
        ret = sys_memfd_create((const char *) a1, (uint32_t) a2);
        break;
    case 324:
        ret = 0;
        break;
    case 325:
        ret = -(int64_t) ENOSYS;
        break; /* mlock2 */
    case 326:
        ret = sys_copy_file_range((int) a1, (uint64_t *) a2, (int) a3, (uint64_t *) a4, a5,
                                  (uint32_t) a6);
        break;
    case 327:
        ret = sys_preadv((int) a1, (const struct iovec *) a2, (int) a3, a4);
        break; /* preadv2 */
    case 328:
        ret = sys_pwritev((int) a1, (const struct iovec *) a2, (int) a3, a4);
        break; /* pwritev2 */
    case 332:
        ret = sys_statx((int) a1, (const char *) a2, (int) a3, (uint32_t) a4, (struct statx *) a5);
        break;
    case 334: {
        for (int _fd = (int) a1; _fd <= (int) a2 && _fd < VFS_FD_MAX; _fd++)
            if (fd_valid(_fd)) fd_close(_fd);
        ret = 0;
        break;
    } /* close_range */
    case 439: {
        char abs[512];
        at_resolve((int) a1, (const char *) a2, abs, sizeof(abs));
        ret = (int64_t) vfs_access(abs, (int) a3);
        break;
    } /* faccessat2 */

    case SYS_jail_create:
        ret = sys_jail_create((const kjail_conf_t *) a1);
        break;
    case SYS_jail_attach:
        ret = sys_jail_attach((uint32_t) a1);
        break;
    case SYS_jail_get:
        ret = sys_jail_get((uint32_t) a1, (kjail_info_t *) a2);
        break;
    case SYS_jail_list:
        ret = sys_jail_list((uint32_t *) a1, (int) a2);
        break;
    case SYS_jail_remove:
        ret = sys_jail_remove((uint32_t) a1);
        break;
    case SYS_jail_self:
        ret = sys_jail_self();
        break;
    case SYS_jail_set_auto:
        ret = sys_jail_set_auto((int) a1);
        break;

    default:
        log_debug("[syscall %lu  a1=%lx a2=%lx a3=%lx]", nr, a1, a2, a3);
        ret = -(int64_t) ENOSYS;
        break;
    }
    f->rax = (uint64_t) ret;

    if (tp && tp->ptrace_syscall_trace && nr != 101) {
        tp->ptrace_in_syscall = 0;
        proc_ptrace_stop(tp, SIGTRAP | 0x80, 1, f, &f->r11);
    }

    signal_check(f);
}
