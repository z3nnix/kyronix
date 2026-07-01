#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
} __attribute__((packed));

static int getdents64_raw(int fd, void *buf, size_t count) {
    return syscall(217, fd, buf, count);
}

int main(void) {
    int r;

    /* Test 1: Open /proc/ and read raw directory entries */
    fprintf(stderr, "=== Test 1: readdir /proc/ via opendir ===\n");
    DIR *dp = opendir("/proc");
    if (!dp) {
        fprintf(stderr, "opendir /proc FAILED: %s\n", strerror(errno));
        return 1;
    }
    int nproc = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] >= '1' && de->d_name[0] <= '9') {
            nproc++;
            fprintf(stderr, "  entry: d_type=%d d_ino=%lu name=%s\n",
                    de->d_type, (unsigned long)de->d_ino, de->d_name);
        }
    }
    closedir(dp);
    fprintf(stderr, "  Total numeric entries: %d\n\n", nproc);

    /* Test 2: Open /proc/ with open() and getdents64 syscall */
    fprintf(stderr, "=== Test 2: getdents64(/proc/) ===\n");
    int fd = open("/proc", O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        fprintf(stderr, "open /proc FAILED: %s\n", strerror(errno));
        return 1;
    }
    char buf[4096];
    r = getdents64_raw(fd, buf, sizeof(buf));
    if (r < 0) {
        fprintf(stderr, "getdents64 FAILED: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    fprintf(stderr, "  getdents64 returned %d bytes\n", r);
    int pos = 0;
    nproc = 0;
    while (pos < r) {
        struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + pos);
        fprintf(stderr, "  d_ino=%lu d_off=%ld d_reclen=%u d_type=%u name=%s\n",
                (unsigned long)d->d_ino, (long)d->d_off,
                (unsigned)d->d_reclen, (unsigned)d->d_type, d->d_name);
        if (d->d_reclen == 0) break;
        if (d->d_name[0] >= '1' && d->d_name[0] <= '9') nproc++;
        pos += d->d_reclen;
    }
    fprintf(stderr, "  Total numeric entries: %d\n\n", nproc);
    close(fd);

    /* Test 3: Try to openat /proc/<pid> and /proc/<pid>/stat for each PID */
    fprintf(stderr, "=== Test 3: openat + read stat for each PID ===\n");
    dp = opendir("/proc");
    if (!dp) { fprintf(stderr, "opendir FAILED\n"); return 1; }
    int procFd = open("/proc", O_RDONLY | O_DIRECTORY);
    if (procFd < 0) { fprintf(stderr, "open procFd FAILED\n"); return 1; }

    rewinddir(dp);
    nproc = 0;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        nproc++;

        /* Method A: openat with dirfd */
        int pidFd = openat(procFd, de->d_name, O_RDONLY | O_DIRECTORY);
        if (pidFd < 0) {
            fprintf(stderr, "  PID %s: openat(procFd) FAILED: %s\n", de->d_name, strerror(errno));
            continue;
        }
        int statFd = openat(pidFd, "stat", O_RDONLY);
        if (statFd < 0) {
            fprintf(stderr, "  PID %s: openat(pidFd, \"stat\") FAILED: %s\n", de->d_name, strerror(errno));
            close(pidFd);
            continue;
        }
        char sbuf[512];
        r = (int)read(statFd, sbuf, sizeof(sbuf) - 1);
        if (r <= 0) {
            fprintf(stderr, "  PID %s: read FAILED (%d)\n", de->d_name, r);
        } else {
            sbuf[r] = '\0';
            fprintf(stderr, "  PID %s (openat): %s", de->d_name, sbuf);
        }
        close(statFd);
        close(pidFd);

        /* Method B: open with absolute path */
        char full[1024];
        snprintf(full, sizeof(full), "/proc/%s/stat", de->d_name);
        statFd = open(full, O_RDONLY);
        if (statFd < 0) {
            fprintf(stderr, "  PID %s: open(abs) FAILED: %s\n", de->d_name, strerror(errno));
            continue;
        }
        r = (int)read(statFd, sbuf, sizeof(sbuf) - 1);
        if (r > 0) {
            sbuf[r] = '\0';
            fprintf(stderr, "  PID %s (abs):    %s", de->d_name, sbuf);
        }
        close(statFd);
    }
    closedir(dp);
    close(procFd);
    fprintf(stderr, "\n  TOTAL PIDs seen: %d\n", nproc);

    /* Test 4: Check key /proc files */
    fprintf(stderr, "\n=== Test 4: /proc/stat ===\n");
    fd = open("/proc/stat", O_RDONLY);
    if (fd >= 0) {
        r = (int)read(fd, buf, sizeof(buf) - 1);
        if (r > 0) { buf[r] = '\0'; fprintf(stderr, "%s", buf); }
        close(fd);
    } else {
        fprintf(stderr, "open /proc/stat FAILED\n");
    }

    fprintf(stderr, "\n=== Test 5: /proc/meminfo ===\n");
    fd = open("/proc/meminfo", O_RDONLY);
    if (fd >= 0) {
        r = (int)read(fd, buf, sizeof(buf) - 1);
        if (r > 0) { buf[r] = '\0'; fprintf(stderr, "%s", buf); }
        close(fd);
    } else {
        fprintf(stderr, "open /proc/meminfo FAILED\n");
    }

    return nproc > 0 ? 0 : 1;
}
