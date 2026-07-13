#ifndef PKG_UTIL_H
#define PKG_UTIL_H

#include <stddef.h>

#include "pkg.h"

void log_info(const char *tag, const char *fmt, ...);
void log_ok(const char *fmt, ...);
void dief(const char *fmt, ...);

const char *home_dir(void);
void config_path(char *out, size_t n);
char *read_file(const char *path, size_t *len_out);
int write_text_file(const char *path, const char *text);
char *read_endpoint(void);
void set_endpoint(const char *endpoint);
int starts_with(const char *s, const char *p);
void trim_crlf(char *s);
void ensure_dir(const char *path);
int run_cmd(char *const argv[]);

#endif
