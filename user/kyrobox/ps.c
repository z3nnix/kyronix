#include "common.h"

int main(int argc, char **argv) {
    kx_prog = "ps";

    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        printf("usage: ps\n");
        return 0;
    }

    FILE *f = fopen("/proc/pids", "r");
    if (!f) {
        kx_warn("/proc/pids");
        return 1;
    }

    printf("%5s %5s %c %5s %8s %s\n", "PID", "PPID", 'S', "UID", "MEM(KB)", "COMMAND");

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        unsigned int pid, ppid, uid;
        unsigned long mem_kb;
        char state;
        char exe[512] = {0};
        if (sscanf(line, "%u %u %c %u %lu %511s", &pid, &ppid, &state, &uid, &mem_kb, exe) >= 5) {
            printf("%5u %5u %c %5u %8lu %s\n", pid, ppid, state, uid, mem_kb, exe);
        }
    }

    fclose(f);
    return 0;
}
