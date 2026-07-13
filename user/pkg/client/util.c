#define _POSIX_C_SOURCE 200809L

#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pkg.h"

void log_info(const char *tag, const char *fmt, ...) {
    va_list ap;
    fprintf(stdout, "%s[%s]%s ", ANSI_BLUE, tag, ANSI_RESET);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

void log_ok(const char *fmt, ...) {
    va_list ap;
    fprintf(stdout, "%s%s[ok]%s ", ANSI_GREEN, ANSI_BOLD, ANSI_RESET);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

void dief(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s%s[err]%s ", ANSI_RED, ANSI_BOLD, ANSI_RESET);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

const char *home_dir(void) {
    const char *home = getenv("HOME");
    return (home && *home) ? home : "/tmp";
}

void config_path(char *out, size_t n) {
    snprintf(out, n, "%s/.pkg_endpoint", home_dir());
}

char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (len_out) *len_out = rd;
    return buf;
}

int write_text_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t len = strlen(text);
    int ok = fwrite(text, 1, len, f) == len;
    fclose(f);
    return ok ? 0 : -1;
}

char *read_endpoint(void) {
    char path[512];
    config_path(path, sizeof(path));
    size_t len = 0;
    char *txt = read_file(path, &len);
    if (!txt || len == 0) {
        free(txt);

        size_t def_len = strlen(DEFAULT_ENDPOINT);
        char *copy = (char *)malloc(def_len + 1);
        if (!copy) return NULL;
        memcpy(copy, DEFAULT_ENDPOINT, def_len + 1);
        return copy;
    }
    while (len && isspace((unsigned char)txt[len - 1])) {
        txt[--len] = '\0';
    }
    return txt;
}

void set_endpoint(const char *endpoint) {
    if (!endpoint || !*endpoint) dief("endpoint required");
    char path[512];
    config_path(path, sizeof(path));
    if (write_text_file(path, endpoint) != 0) {
        dief("failed to store endpoint");
    }
    log_ok("endpoint set");
}

int starts_with(const char *s, const char *p) {
    return strncmp(s, p, strlen(p)) == 0;
}

void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = '\0';
}

void ensure_dir(const char *path) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        dief("mkdir failed: %s", path);
    }
}

int run_cmd(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return 0;
    return -1;
}
