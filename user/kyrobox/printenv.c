#include "common.h"
extern char **environ;
int printenv_main(int argc, char **argv) {
	(void)argc;
	(void)argv;
	kx_prog = "printenv";
    if (argc == 1) {
        for (char **e = environ; *e; e++) puts(*e);
        return 0;
    }
    int rc = 1;
    for (int i = 1; i < argc; i++) {
        char *v = getenv(argv[i]);
        if (v) {
            puts(v);
            rc = 0;
        }
    }
    return rc;
}

#ifndef KX_MONOLITHIC
int main(int argc, char **argv) {

	return printenv_main(argc, argv);
}
#endif
