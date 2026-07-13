#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "pkg.h"

static int has_flag(int argc, char **argv, const char *name) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) return 1;
    }
    return 0;
}

static const char *first_nonflag(int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] != '-') return argv[i];
    }
    return NULL;
}

static void usage(void) {
    fprintf(stderr, "%sUsage:%s\n", ANSI_BOLD, ANSI_RESET);
    fprintf(stderr, "  %shealth%s (-verbose)\n", ANSI_CYAN, ANSI_RESET);
    fprintf(stderr, "  %sset%s [endpoint] (-verbose)\n", ANSI_CYAN, ANSI_RESET);
    fprintf(stderr, "  %sget%s [package] (-verbose)\n", ANSI_CYAN, ANSI_RESET);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *cmd = argv[1];
    verbose_mode = has_flag(argc - 2, argv + 2, "-verbose") || has_flag(argc - 2, argv + 2, "--verbose");

    if (strcmp(cmd, "health") == 0) {
        cmd_health();
        return 0;
    }

    if (strcmp(cmd, "set") == 0) {
        const char *endpoint = first_nonflag(argc - 2, argv + 2);
        if (!endpoint) {
            fprintf(stderr, "%s%s[err]%s endpoint required\n", ANSI_RED, ANSI_BOLD, ANSI_RESET);
            return 1;
        }
        cmd_set(endpoint);
        return 0;
    }

    if (strcmp(cmd, "get") == 0) {
        const char *pkg = first_nonflag(argc - 2, argv + 2);
        if (!pkg) {
            fprintf(stderr, "%s%s[err]%s package name required\n", ANSI_RED, ANSI_BOLD, ANSI_RESET);
            return 1;
        }
        cmd_get(pkg);
        return 0;
    }

    usage();
    return 1;
}
