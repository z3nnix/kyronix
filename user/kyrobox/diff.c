#include "common.h"

typedef struct {
    char **lines;
    int n;
} filelines_t;

static void read_lines(const char *path, filelines_t *fl) {
    fl->lines = NULL;
    fl->n = 0;
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");
    if (!f) {
        kx_warn(path);
        exit(2);
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int alloc = 0;
    while ((len = getline(&line, &cap, f)) >= 0) {
        if (len > 0 && line[(size_t) len - 1] == '\n') line[--len] = 0;
        if (fl->n >= alloc) {
            alloc = alloc ? alloc * 2 : 64;
            fl->lines = realloc(fl->lines, (size_t) alloc * sizeof(char *));
        }
        fl->lines[fl->n++] = strdup(line);
    }
    free(line);
    if (f != stdin) fclose(f);
}

typedef enum { OP_EQ, OP_DEL, OP_INS } optype_t;

typedef struct {
    optype_t type;
    int ai, bi;
} edit_t;

int main(int argc, char **argv) {
    kx_prog = "diff";
    int first = 1;
    if (first < argc && strcmp(argv[first], "-u") == 0) first++;
    if (argc - first != 2) kx_die("usage: diff [-u] FILE1 FILE2");
    const char *patha = argv[first], *pathb = argv[first + 1];

    filelines_t A, B;
    read_lines(patha, &A);
    read_lines(pathb, &B);
    int n = A.n, m = B.n;

    int *dp = malloc((size_t) (n + 1) * (size_t) (m + 1) * sizeof(int));
#define DP(i, j) dp[(size_t) (i) * (size_t) (m + 1) + (size_t) (j)]
    for (int i = n; i >= 0; i--) {
        for (int j = m; j >= 0; j--) {
            if (i == n || j == m) {
                DP(i, j) = 0;
            } else if (strcmp(A.lines[i], B.lines[j]) == 0) {
                DP(i, j) = DP(i + 1, j + 1) + 1;
            } else {
                int d1 = DP(i + 1, j), d2 = DP(i, j + 1);
                DP(i, j) = d1 > d2 ? d1 : d2;
            }
        }
    }

    edit_t *edits = malloc((size_t) (n + m + 1) * sizeof(edit_t));
    int ne = 0, i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && strcmp(A.lines[i], B.lines[j]) == 0) {
            edits[ne++] = (edit_t){ OP_EQ, i, j };
            i++;
            j++;
        } else if (j < m && (i == n || DP(i, j + 1) > DP(i + 1, j))) {
            edits[ne++] = (edit_t){ OP_INS, i, j };
            j++;
        } else {
            edits[ne++] = (edit_t){ OP_DEL, i, j };
            i++;
        }
    }
    free(dp);

    int changed = 0;
    for (int k = 0; k < ne; k++)
        if (edits[k].type != OP_EQ) changed = 1;
    if (!changed) return 0;

    printf("--- %s\n+++ %s\n", patha, pathb);

    const int ctx = 3;
    int k = 0;
    while (k < ne) {
        if (edits[k].type == OP_EQ) {
            k++;
            continue;
        }

        int hs = k, back = 0;
        while (hs > 0 && back < ctx && edits[hs - 1].type == OP_EQ) {
            hs--;
            back++;
        }

        int he = k;
        for (;;) {
            while (he < ne && edits[he].type != OP_EQ) he++;
            int run = 0, p2 = he;
            while (p2 < ne && edits[p2].type == OP_EQ && run < 2 * ctx) {
                p2++;
                run++;
            }
            if (p2 < ne && edits[p2].type != OP_EQ) {
                he = p2;
                continue;
            }
            break;
        }
        int fwd = 0;
        while (he < ne && fwd < ctx && edits[he].type == OP_EQ) {
            he++;
            fwd++;
        }

        int a_count = 0, b_count = 0;
        for (int q = hs; q < he; q++) {
            if (edits[q].type == OP_EQ) {
                a_count++;
                b_count++;
            } else if (edits[q].type == OP_DEL) {
                a_count++;
            } else {
                b_count++;
            }
        }
        int a_start = a_count > 0 ? edits[hs].ai + 1 : edits[hs].ai;
        int b_start = b_count > 0 ? edits[hs].bi + 1 : edits[hs].bi;

        printf("@@ -%d,%d +%d,%d @@\n", a_start, a_count, b_start, b_count);
        for (int q = hs; q < he; q++) {
            if (edits[q].type == OP_EQ)
                printf(" %s\n", A.lines[edits[q].ai]);
            else if (edits[q].type == OP_DEL)
                printf("-%s\n", A.lines[edits[q].ai]);
            else
                printf("+%s\n", B.lines[edits[q].bi]);
        }
        k = he;
    }

    return 1;
}
