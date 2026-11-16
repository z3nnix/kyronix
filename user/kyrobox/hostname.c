#include "common.h"
int sethostname(const char *name, size_t len);
int hostname_main(int argc, char **argv) {
	(void)argc;
	(void)argv;
    kx_prog = "hostname";
    if (argc > 1) {
        if (sethostname(argv[1], strlen(argv[1])) < 0) {
            kx_warn(argv[1]);
            return 1;
        }
        return 0;
    }
    struct utsname u;
    if (uname(&u) < 0) return 1;
    puts(u.nodename);
    return 0;
}

#ifndef KX_MONOLITHIC
int main(int argc, char **argv) {

	return hostname_main(argc, argv);
}
#endif
