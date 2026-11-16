#include "common.h"
#include <grp.h>

int id_main(int argc, char **argv) {
	(void)argc;
	(void)argv;
    kx_prog = "id";
    uid_t uid;
    gid_t gid;
    if (argc > 1) {
        struct passwd *pw = getpwnam(argv[1]);
        if (!pw) {
            fprintf(stderr, "%s: %s: no such user\n", kx_prog, argv[1]);
            return 1;
        }
        uid = pw->pw_uid;
        gid = pw->pw_gid;
    } else {
        uid = getuid();
        gid = getgid();
    }
    struct passwd *pw = getpwuid(uid);
    struct group *gr = getgrgid(gid);
    if (pw)
        printf("uid=%u(%s) ", uid, pw->pw_name);
    else
        printf("uid=%u ", uid);
    if (gr)
        printf("gid=%u(%s)", gid, gr->gr_name);
    else
        printf("gid=%u", gid);

    /* supplementary groups */
    gid_t sup[64];
    int nsup = getgroups(64, sup);
    if (nsup > 0) {
        printf(" groups=");
        for (int i = 0; i < nsup; i++) {
            if (i) putchar(',');
            struct group *g = getgrgid(sup[i]);
            if (g)
                printf("%u(%s)", sup[i], g->gr_name);
            else
                printf("%u", sup[i]);
        }
    }
    putchar('\n');
    return 0;
}

#ifndef KX_MONOLITHIC
int main(int argc, char **argv) {

	return id_main(argc, argv);
}
#endif
