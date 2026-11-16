#include "common.h"
#include <sys/wait.h>

#define MAX_ARGS 512

static char **g_cmd;
static int g_cmd_n;
static int g_rc;

static void flush(char **words, int nwords) {
    if (!nwords) return;
    char **av = malloc((size_t) (g_cmd_n + nwords + 1) * sizeof(char *));
    if (!av) {
        perror("malloc");
        exit(1);
    }
    for (int j = 0; j < g_cmd_n; j++) av[j] = g_cmd[j];
    for (int j = 0; j < nwords; j++) av[g_cmd_n + j] = words[j];
    av[g_cmd_n + nwords] = NULL;
    pid_t pid = fork();
    if (pid == 0) {
        execvp(av[0], av);
        _exit(127);
    }
    if (pid > 0) {
        int st;
        waitpid(pid, &st, 0);
        if (WEXITSTATUS(st)) g_rc = WEXITSTATUS(st);
    }
    free(av);
}

int xargs_main(int argc, char **argv) {
	(void)argc;
	(void)argv;
    kx_prog = "xargs";
    int i = 1;
    long max_args = MAX_ARGS;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        for (char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
            case 'n':
                if (o[1]) {
                    max_args = strtol(o + 1, NULL, 10);
                    o += strlen(o) - 1;
                } else if (i + 1 < argc)
                    max_args = strtol(argv[++i], NULL, 10);
                else
                    kx_die("-n requires argument");
                break;
            default:
                fprintf(stderr, "%s: invalid option -%c\n", kx_prog, *o);
                exit(1);
            }
        }
    }
    char *echo_cmd[] = { "echo", NULL };
    g_cmd = (i < argc) ? &argv[i] : echo_cmd;
    g_cmd_n = (i < argc) ? (argc - i) : 1;

    char *words[MAX_ARGS];
    int nwords = 0;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, stdin) >= 0) {
        char *p = line;
        while (*p) {
            while (*p && isspace((unsigned char) *p)) p++;
            if (!*p) break;
            char tok[4096];
            int tlen = 0;
            char q = 0;
            while (*p && (q || !isspace((unsigned char) *p))) {
                if (!q && (*p == '\'' || *p == '"')) {
                    q = *p++;
                    continue;
                }
                if (q && *p == q) {
                    q = 0;
                    p++;
                    continue;
                }
                if (tlen < (int) sizeof(tok) - 1) tok[tlen++] = *p;
                p++;
            }
            tok[tlen] = 0;
            if (!tlen) break;
            if (nwords < MAX_ARGS - 1) words[nwords++] = strdup(tok);
            if (nwords >= max_args) {
                flush(words, nwords);
                for (int j = 0; j < nwords; j++) free(words[j]);
                nwords = 0;
            }
        }
    }
    flush(words, nwords);
    for (int j = 0; j < nwords; j++) free(words[j]);
    free(line);
    return g_rc;
}

#ifndef KX_MONOLITHIC
int main(int argc, char **argv) {

	return xargs_main(argc, argv);
}
#endif
