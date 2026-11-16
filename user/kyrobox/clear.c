#include "common.h"
int clear_main(int argc, char **argv) {
	(void)argc;
	(void)argv;
	kx_prog = "clear";
	fputs("\033[H\033[2J", stdout);
	fflush(stdout);
	return 0;
}

#ifndef KX_MONOLITHIC
int main(int argc, char **argv) {

	return clear_main(argc, argv);
}
#endif
