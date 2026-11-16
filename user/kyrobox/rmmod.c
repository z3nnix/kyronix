#define _GNU_SOURCE
#include "common.h"
#include <sys/syscall.h>

int rmmod_main(int argc, char **argv) {
	(void)argc;
	(void)argv;
    kx_prog = "rmmod";
    if (argc != 2) {
        fprintf(stderr, "usage: rmmod MODULE\n");
        return 1;
    }
    if (syscall(SYS_delete_module, argv[1], 0) < 0) {
        kx_warn(argv[1]);
        return 1;
    }
    return 0;
}

#ifndef KX_MONOLITHIC
int main(int argc, char **argv) {

	return rmmod_main(argc, argv);
}
#endif
