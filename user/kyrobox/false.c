#include "common.h"
int false_main(int argc, char **argv) {
	(void)argc;
	(void)argv;
	kx_prog = "false";
    (void) argc;
    (void) argv;
    return 1;
}

#ifndef KX_MONOLITHIC
int main(int argc, char **argv) {

	return false_main(argc, argv);
}
#endif
