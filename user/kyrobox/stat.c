#include "common.h"

static const char *ftype_name(mode_t m) {
    if (S_ISREG(m)) return "regular file";
    if (S_ISDIR(m)) return "directory";
    if (S_ISLNK(m)) return "symbolic link";
    if (S_ISCHR(m)) return "character device";
    if (S_ISBLK(m)) return "block device";
    if (S_ISFIFO(m)) return "fifo";
    if (S_ISSOCK(m)) return "socket";
    return "unknown";
}

static void mode_string(mode_t m, char out[12]) {
    out[0] = S_ISDIR(m)  ? 'd' :
             S_ISLNK(m)  ? 'l' :
             S_ISCHR(m)  ? 'c' :
             S_ISBLK(m)  ? 'b' :
             S_ISFIFO(m) ? 'p' :
             S_ISSOCK(m) ? 's' :
                           '-';
    const char *c = "rwx";
    for (int i = 0; i < 9; i++) out[i + 1] = (m & (1 << (8 - i))) ? c[i % 3] : '-';
    out[10] = 0;
}

int stat_main(int argc, char **argv) {
	(void)argc;
	(void)argv;
    kx_prog = "stat";
    bool flag_L = false;
    int first = 1;
    if (first < argc && strcmp(argv[first], "-L") == 0) {
        flag_L = true;
        first++;
    }
    if (first == argc) kx_die("usage: stat [-L] FILE...");
    int rc = 0;
    for (int i = first; i < argc; i++) {
        struct stat st;
        int r = flag_L ? stat(argv[i], &st) : lstat(argv[i], &st);
        if (r < 0) {
            kx_warn(argv[i]);
            rc = 1;
            continue;
        }
        char m[12];
        mode_string(st.st_mode, m);
        struct passwd *pw = getpwuid(st.st_uid);
        char ubuf[16];
        if (pw)
            snprintf(ubuf, sizeof(ubuf), "%s", pw->pw_name);
        else
            snprintf(ubuf, sizeof(ubuf), "%u", st.st_uid);
        printf("  File: %s\n", argv[i]);
        printf("  Size: %-15lld  Blocks: %-8lld  IO Block: %lld  %s\n", (long long) st.st_size,
               (long long) st.st_blocks, (long long) st.st_blksize, ftype_name(st.st_mode));
        printf("Device: %-14llx  Inode: %-12llu  Links: %lu\n", (unsigned long long) st.st_dev,
               (unsigned long long) st.st_ino, (unsigned long) st.st_nlink);
        printf("Access: (%04o/%s)  Uid: (%5u/%8s)  Gid: (%5u)\n", st.st_mode & 07777, m, st.st_uid,
               ubuf, st.st_gid);
        printf("Modify: %lld\n", (long long) st.st_mtime);
    }
    return rc;
}

#ifndef KX_MONOLITHIC
int main(int argc, char **argv) {

	return stat_main(argc, argv);
}
#endif
