#include "common.h"

#include <sys/reboot.h>

static void usage(void) {
    fprintf(stderr, "usage: %s [-p]\n", kx_prog);
    fprintf(stderr, "  reboot   restart the machine\n");
    fprintf(stderr, "  poweroff power the machine off\n");
    fprintf(stderr, "  halt     halt the CPU\n");
    fprintf(stderr, "  -p       power off (for reboot)\n");
}

int main(int argc, char **argv) {
    kx_prog = kx_base(argv[0]);

    /* Command is chosen by applet name, overridable by flags. */
    int cmd;
    if (strcmp(kx_prog, "poweroff") == 0)
        cmd = RB_POWER_OFF;
    else if (strcmp(kx_prog, "halt") == 0)
        cmd = RB_HALT_SYSTEM;
    else
        cmd = RB_AUTOBOOT; /* reboot */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-p") == 0)
            cmd = RB_POWER_OFF;
        else if (strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else {
            usage();
            return 1;
        }
    }

    /* The kernel flushes filesystems before acting on the reboot syscall. */
    if (reboot(cmd) < 0) {
        fprintf(stderr, "%s: %s\n", kx_prog, strerror(errno));
        return 1;
    }
    return 0; /* not reached on success */
}
