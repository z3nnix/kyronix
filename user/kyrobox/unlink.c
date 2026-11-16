#include "common.h"
int unlink_main(int argc, char **argv) {
	(void)argc;
	(void)argv;
    kx_prog = "unlink";
    if (argc != 2) kx_die("usage: unlink FILE");
    if (unlink(argv[1]) < 0) {
        kx_warn(argv[1]);
        return 1;
    }
    return 0;
}

#ifndef KX_MONOLITHIC
int main(int argc, char **argv) {

	return unlink_main(argc, argv);
}
#endif
