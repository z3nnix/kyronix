#include "common.h"

typedef struct {
    char **lines;
    int n;
    int cap;
} lines_t;

static void lines_push(lines_t *l, const char *s) {
    if (l->n >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->lines = realloc(l->lines, (size_t) l->cap * sizeof(char *));
    }
    l->lines[l->n++] = strdup(s);
}

static void lines_free(lines_t *l) {
    for (int i = 0; i < l->n; i++) free(l->lines[i]);
    free(l->lines);
}

static lines_t read_all_lines(FILE *f) {
    lines_t l = { 0 };
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) >= 0) {
        if (len > 0 && line[(size_t) len - 1] == '\n') line[--len] = 0;
        lines_push(&l, line);
    }
    free(line);
    return l;
}

static const char *strip_p(const char *path, int p) {
    while (p-- > 0) {
        const char *slash = strchr(path, '/');
        if (!slash) break;
        path = slash + 1;
    }
    return path;
}

int main(int argc, char **argv) {
    kx_prog = "patch";
    int p = 0;
    const char *target = NULL;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-p", 2) == 0 && argv[i][2]) {
            p = atoi(argv[i] + 2);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            p = atoi(argv[++i]);
        } else {
            target = argv[i];
        }
    }

    lines_t patch = read_all_lines(stdin);
    int pi = 0;
    int applied_any = 0;

    while (pi < patch.n) {
        while (pi < patch.n && strncmp(patch.lines[pi], "--- ", 4) != 0) pi++;
        if (pi >= patch.n) break;
        pi++;
        if (pi >= patch.n || strncmp(patch.lines[pi], "+++ ", 4) != 0) continue;

        char newhdr[512];
        snprintf(newhdr, sizeof newhdr, "%s", patch.lines[pi] + 4);
        char *tab = strchr(newhdr, '\t');
        if (tab) *tab = 0;
        pi++;

        const char *filename = target ? target : strip_p(newhdr, p);

        FILE *orig = fopen(filename, "r");
        lines_t src = { 0 };
        if (orig) {
            src = read_all_lines(orig);
            fclose(orig);
        }

        lines_t out = { 0 };
        int si = 0;

        while (pi < patch.n && strncmp(patch.lines[pi], "@@ ", 3) == 0) {
            int a_start = 0, a_count = 1, b_start = 0, b_count = 1;
            int got = sscanf(patch.lines[pi], "@@ -%d,%d +%d,%d @@", &a_start, &a_count, &b_start,
                              &b_count);
            if (got < 4) {
                a_count = b_count = 1;
                sscanf(patch.lines[pi], "@@ -%d +%d @@", &a_start, &b_start);
            }
            pi++;

            int target_si = a_count > 0 ? a_start - 1 : a_start;
            if (target_si < 0) target_si = 0;
            while (si < target_si && si < src.n) lines_push(&out, src.lines[si++]);

            while (pi < patch.n &&
                   (patch.lines[pi][0] == ' ' || patch.lines[pi][0] == '-' ||
                    patch.lines[pi][0] == '+')) {
                char c = patch.lines[pi][0];
                const char *content = patch.lines[pi] + 1;
                if (c == ' ') {
                    lines_push(&out, content);
                    si++;
                } else if (c == '-') {
                    si++;
                } else {
                    lines_push(&out, content);
                }
                pi++;
            }
        }

        while (si < src.n) lines_push(&out, src.lines[si++]);

        FILE *w = fopen(filename, "w");
        if (!w) {
            kx_warn(filename);
        } else {
            for (int q = 0; q < out.n; q++) fprintf(w, "%s\n", out.lines[q]);
            fclose(w);
            printf("patching file %s\n", filename);
            applied_any = 1;
        }

        lines_free(&src);
        lines_free(&out);
    }

    lines_free(&patch);
    return applied_any ? 0 : 1;
}
