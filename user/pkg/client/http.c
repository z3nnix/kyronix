#include "http.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

static void parse_http_url(const char *url, char *host, size_t host_n, int *port, char *path, size_t path_n) {
    const char *p = NULL;
    if (starts_with(url, "http://")) {
        p = url + 7;
        *port = 80;
    } else {
        dief("only http:// URLs are supported");
    }

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    if (colon && (!slash || colon < slash)) {
        snprintf(host, host_n, "%.*s", (int)(colon - p), p);
        *port = atoi(colon + 1);
        if (slash) snprintf(path, path_n, "%s", slash);
        else snprintf(path, path_n, "/");
    } else {
        if (slash) {
            snprintf(host, host_n, "%.*s", (int)(slash - p), p);
            snprintf(path, path_n, "%s", slash);
        } else {
            snprintf(host, host_n, "%s", p);
            snprintf(path, path_n, "/");
        }
    }
}

static int connect_tcp(const char *host, int port) {
    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_s, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

unsigned char *http_get_body_raw(const char *url, int *status_code, size_t *body_len) {
    char host[256], path[1024];
    int port = 0;
    parse_http_url(url, host, sizeof(host), &port, path, sizeof(path));

    int fd = connect_tcp(host, port);
    if (fd < 0) dief("network connection failed");

    char request[2048];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, USER_AGENT);

    if (verbose_mode) log_info("net", "GET %s", url);
    if (write(fd, request, strlen(request)) < 0) {
        close(fd);
        dief("network write failed");
    }

    size_t cap = 65536;
    size_t len = 0;
    unsigned char *buf = (unsigned char *)malloc(cap + 1);
    if (!buf) {
        close(fd);
        dief("oom");
    }

    for (;;) {
        if (len == cap) {
            cap *= 2;
            unsigned char *nb = (unsigned char *)realloc(buf, cap + 1);
            if (!nb) {
                free(buf);
                close(fd);
                dief("oom");
            }
            buf = nb;
        }
        ssize_t rd = read(fd, buf + len, cap - len);
        if (rd < 0) {
            free(buf);
            close(fd);
            dief("network read failed");
        }
        if (rd == 0) break;
        len += (size_t)rd;
    }
    close(fd);
    buf[len] = '\0';

    unsigned char *hdr_end = (unsigned char *)strstr((char *)buf, "\r\n\r\n");
    if (!hdr_end) {
        free(buf);
        dief("invalid http response");
    }

    unsigned char *status_line_end = (unsigned char *)strstr((char *)buf, "\r\n");
    if (!status_line_end) {
        free(buf);
        dief("invalid http response");
    }
    char status_line[128];
    snprintf(status_line, sizeof(status_line), "%.*s", (int)(status_line_end - buf), buf);

    int code = 0;
    sscanf(status_line, "HTTP/%*s %d", &code);
    if (status_code) *status_code = code;

    size_t header_len = (size_t)(hdr_end - buf) + 4;
    size_t raw_len = len - header_len;
    unsigned char *body = (unsigned char *)malloc(raw_len + 1);
    if (!body) {
        free(buf);
        dief("oom");
    }
    memcpy(body, buf + header_len, raw_len);
    body[raw_len] = '\0';
    if (body_len) *body_len = raw_len;

    free(buf);
    return body;
}

char *http_get_body(const char *url, int *status_code) {
    size_t body_len = 0;
    unsigned char *raw = http_get_body_raw(url, status_code, &body_len);
    char *text = (char *)malloc(body_len + 1);
    if (!text) {
        free(raw);
        dief("oom");
    }
    memcpy(text, raw, body_len + 1);
    free(raw);
    return text;
}

int http_download(const char *url, const char *dest) {
    int code = 0;
    size_t body_len = 0;
    unsigned char *body = http_get_body_raw(url, &code, &body_len);
    if (code != 200) {
        free(body);
        return -1;
    }
    FILE *f = fopen(dest, "wb");
    if (!f) {
        free(body);
        return -1;
    }
    size_t written = fwrite(body, 1, body_len, f);
    fclose(f);
    free(body);
    return written == body_len ? 0 : -1;
}
