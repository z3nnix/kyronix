#include "common.h"

int lsmod_main(int argc, char **argv) {
	(void)argc;
	(void)argv;
    kx_prog = "lsmod";
    int fd = open("/proc/modules", O_RDONLY);
    if (fd < 0) {
        kx_warn("/proc/modules");
        return 1;
    }
    puts("Module                  Size  Used by");
    int rc = kx_copy_fd(fd, STDOUT_FILENO);
    int saved_errno = errno;
    close(fd);
    if (rc < 0) {
        errno = saved_errno;
        kx_warn("/proc/modules");
        return 1;
    }
    return 0;
}

#ifndef KX_MONOLITHIC
int main(int argc, char **argv) {

	return lsmod_main(argc, argv);
}
#endif
