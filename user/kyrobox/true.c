#include "common.h"
int true_main(int argc, char **argv) {
	(void)argc;
	(void)argv;
	kx_prog = "true";
    (void) argc;
    (void) argv;
    return 0;
}

#ifndef KX_MONOLITHIC
int main(int argc, char **argv) {

	return true_main(argc, argv);
}
#endif
