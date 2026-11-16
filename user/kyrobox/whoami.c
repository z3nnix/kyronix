#include "common.h"
int whoami_main(int argc, char **argv) {
	(void)argc;
	(void)argv;
	kx_prog = "whoami";
    (void) argc;
    (void) argv;
    struct passwd *pw = getpwuid(geteuid());
    puts(pw ? pw->pw_name : "unknown");
    return 0;
}

#ifndef KX_MONOLITHIC
int main(int argc, char **argv) {

	return whoami_main(argc, argv);
}
#endif
