#include "common.h"

extern char *realpath(const char *restrict path, char *restrict resolved);

static int print_realpath(const char *path) {
    char buf[PATH_MAX];
    if (!realpath(path, buf)) {
        kx_warn(path);
        return 1;
    }
    puts(buf);
    return 0;
}

int realpath_main(int argc, char **argv) {
	(void)argc;
	(void)argv;
    kx_prog = "realpath";
    int first = 1;
    if (first < argc && strcmp(argv[first], "--") == 0) first++;
    if (first == argc) kx_die("usage: realpath FILE...");
    int rc = 0;
    for (int i = first; i < argc; i++) rc |= print_realpath(argv[i]);
    return rc;
}

#ifndef KX_MONOLITHIC
int main(int argc, char **argv) {

	return realpath_main(argc, argv);
}
#endif
