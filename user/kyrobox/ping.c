#include "common.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#define ICMP_PAYLOAD_SZ 56
#define PING_TIMEOUT_MS 3000

static uint16_t checksum(const void *buf, int len) {
    const uint16_t *p = (const uint16_t *) buf;
    uint32_t sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len) sum += *(const uint8_t *) p;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t) ~sum;
}

static int64_t ms_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void usage(void) {
    fprintf(stderr, "Usage: ping [-c count] <host>\n");
    exit(1);
}

int main(int argc, char **argv) {
    kx_prog = "ping";
    int count = 4;
    const char *host = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)
            count = atoi(argv[++i]);
        else if (argv[i][0] != '-')
            host = argv[i];
        else
            usage();
    }
    if (!host) usage();

    struct in_addr dst;
    struct addrinfo hints, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_RAW;
    hints.ai_protocol = IPPROTO_ICMP;
    int gai = getaddrinfo(host, NULL, &hints, &ai);
    if (gai) {
        fprintf(stderr, "ping: %s: %s\n", host, gai_strerror(gai));
        exit(1);
    }
    dst = ((struct sockaddr_in *) ai->ai_addr)->sin_addr;
    freeaddrinfo(ai);

    char ipstr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &dst, ipstr, sizeof(ipstr));

    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        perror("ping: socket");
        exit(1);
    }

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr = dst;

    printf("PING %s (%s): %d data bytes\n", host, ipstr, ICMP_PAYLOAD_SZ);

    uint8_t pkt[sizeof(struct icmphdr) + ICMP_PAYLOAD_SZ];
    uint8_t rbuf[1500];
    int received = 0;
    int64_t t_start = ms_now();

    for (int seq = 0; seq < count; seq++) {
        // build icmp echo request here
        struct icmphdr *icmp = (struct icmphdr *) pkt;
        memset(icmp, 0, sizeof(*icmp));
        icmp->type = ICMP_ECHO;
        icmp->code = 0;
        icmp->un.echo.id = htons((uint16_t) getpid());
        icmp->un.echo.sequence = htons((uint16_t) (seq + 1));

        uint8_t *payload = pkt + sizeof(struct icmphdr);
        for (int i = 0; i < ICMP_PAYLOAD_SZ; i++) payload[i] = (uint8_t) (i & 0xff);

        icmp->checksum = checksum(pkt, (int) sizeof(pkt));

        int64_t t_send = ms_now();

        ssize_t sent = sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *) &to, sizeof(to));
        if (sent < 0) {
            perror("ping: sendto");
            continue;
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int64_t deadline = t_send + PING_TIMEOUT_MS;

        int got_reply = 0;
        while (!got_reply) {
            int64_t now = ms_now();
            int wait = (int) (deadline - now);
            if (wait <= 0) break;

            int r = poll(&pfd, 1, wait);
            if (r <= 0) break;

            ssize_t n = recv(fd, rbuf, sizeof(rbuf), 0);
            if (n < 0) break;
            if (n < (ssize_t) (sizeof(struct iphdr) + sizeof(struct icmphdr))) continue;

            struct iphdr *iph = (struct iphdr *) rbuf;
            int ip_hlen = iph->ihl * 4;
            if (n < ip_hlen + (ssize_t) sizeof(struct icmphdr)) continue;

            struct icmphdr *rp = (struct icmphdr *) (rbuf + ip_hlen);
            if (rp->type != ICMP_ECHOREPLY) continue;
            if (ntohs(rp->un.echo.id) != (uint16_t) getpid()) continue;
            if (ntohs(rp->un.echo.sequence) != (uint16_t) (seq + 1)) continue;

            int64_t rtt = ms_now() - t_send;
            printf("%d bytes from %s: icmp_seq=%d ttl=%d time=%lld ms\n", (int) (n - ip_hlen), host,
                   ntohs(rp->un.echo.sequence), iph->ttl, (long long) rtt);
            received++;
            got_reply = 1;
        }

        if (!got_reply) printf("Request timeout for icmp_seq %d\n", seq + 1);

        if (seq + 1 < count) sleep(1);
    }

    int64_t t_total = ms_now() - t_start;
    printf("\n--- %s ping statistics ---\n", host);
    printf("%d packets transmitted, %d received, %d%% packet loss, time %lldms\n", count, received,
           count ? (count - received) * 100 / count : 0, (long long) t_total);

    close(fd);
    return (received == count) ? 0 : 1;
}
