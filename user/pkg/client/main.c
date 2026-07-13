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
    fprintf(stderr,
        "%skx-pkg%s - package manager for kyronix. %sv" PKG_VERSION "%s\n\n"
        "%sUsage:%s\n"
        "  %spkg get%s <package>      install a package\n"
        "  %spkg list%s               list installed packages\n"
        "  %spkg remove%s <package>   uninstall a package\n"
        "  %spkg health%s             check registry status\n"
        "  %spkg set%s <url>          set registry endpoint\n"
        "\n",
        ANSI_CYAN, ANSI_RESET,
        ANSI_DIM, ANSI_RESET,
        ANSI_BOLD, ANSI_RESET,
        ANSI_CYAN, ANSI_RESET,
        ANSI_CYAN, ANSI_RESET,
        ANSI_CYAN, ANSI_RESET,
        ANSI_CYAN, ANSI_RESET,
        ANSI_CYAN, ANSI_RESET
    );
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *cmd = argv[1];
    verbose_mode = has_flag(argc - 2, argv + 2, "-verbose") || has_flag(argc - 2, argv + 2, "--verbose");

    if (strcmp(cmd, "health") == 0 || strcmp(cmd, "ping") == 0) {
        cmd_health();
        return 0;
    }

    if (strcmp(cmd, "set") == 0) {
        const char *endpoint = first_nonflag(argc - 2, argv + 2);
        if (!endpoint) {
            fprintf(stderr, "%s%serror:%s registry URL required\n", ANSI_RED, ANSI_BOLD, ANSI_RESET);
            return 1;
        }
        cmd_set(endpoint);
        return 0;
    }

    if (strcmp(cmd, "get") == 0 || strcmp(cmd, "install") == 0) {
        const char *pkg = first_nonflag(argc - 2, argv + 2);
        if (!pkg) {
            fprintf(stderr, "%s%serror:%s package name required\n", ANSI_RED, ANSI_BOLD, ANSI_RESET);
            return 1;
        }
        cmd_get(pkg);
        return 0;
    }

    if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0) {
        cmd_list();
        return 0;
    }

    if (strcmp(cmd, "remove") == 0 || strcmp(cmd, "rm") == 0 || strcmp(cmd, "uninstall") == 0) {
        const char *pkg = first_nonflag(argc - 2, argv + 2);
        if (!pkg) {
            fprintf(stderr, "%s%serror:%s package name required\n", ANSI_RED, ANSI_BOLD, ANSI_RESET);
            return 1;
        }
        cmd_remove(pkg);
        return 0;
    }

    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        fprintf(stdout, "k9-pkg %s\n", PKG_VERSION);
        return 0;
    }

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        usage();
        return 0;
    }

    fprintf(stderr, "%s%serror:%s unknown command '%s'\n", ANSI_RED, ANSI_BOLD, ANSI_RESET, cmd);
    usage();
    return 1;
}
