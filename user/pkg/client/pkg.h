#ifndef PKG_H
#define PKG_H

#include <stddef.h>
#include <stdint.h>

#define ANSI_RESET   "\x1b[0m"
#define ANSI_RED     "\x1b[31m"
#define ANSI_GREEN   "\x1b[32m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_BLUE    "\x1b[34m"
#define ANSI_MAGENTA "\x1b[35m"
#define ANSI_CYAN    "\x1b[36m"
#define ANSI_BOLD    "\x1b[1m"
#define ANSI_DIM     "\x1b[2m"

#define DEFAULT_ENDPOINT "http://10.0.2.2:8000"
#define USER_AGENT "K9PackageManager/2.0"

#define PKG_VERSION "0.2.0"

typedef struct {
    char name[128];
    char version[64];
    char description[512];
    char arch[32];
    char maintainer[128];
    char license[64];
    char homepage[256];
    int revision;
} PackageInfo;

extern int verbose_mode;

#endif
