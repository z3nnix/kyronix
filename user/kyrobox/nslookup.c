#include "common.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

int main(int argc, char **argv) {
    kx_prog = "nslookup";
    if (argc < 2) {
        fprintf(stderr, "Usage: nslookup <host>\n");
        return 1;
    }
    const char *host = argv[1];

    struct addrinfo hints, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM; // dedupe to one entry per address

    int err = getaddrinfo(host, NULL, &hints, &ai);
    if (err) {
        fprintf(stderr, "nslookup: %s: %s\n", host, gai_strerror(err));
        return 1;
    }

    int found = 0;
    for (struct addrinfo *rp = ai; rp; rp = rp->ai_next) {
        if (rp->ai_family != AF_INET) continue;
        struct sockaddr_in *sin = (struct sockaddr_in *) rp->ai_addr;
        char buf[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) continue;
        printf("Name:    %s\nAddress: %s\n", host, buf);
        found++;
    }
    freeaddrinfo(ai);

    if (!found) {
        fprintf(stderr, "nslookup: %s: no address found\n", host);
        return 1;
    }
    return 0;
}
